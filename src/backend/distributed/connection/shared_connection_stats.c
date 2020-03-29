/*-------------------------------------------------------------------------
 *
 * shared_connection_stats.c
 *   Keeps track of the number of connections to remote nodes across
 *   backends. The primary goal is to prevent excessive number of
 *   connections (typically > max_connections) to any worker node.
 *
 * Copyright (c) Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "pgstat.h"

#include "libpq-fe.h"

#include "miscadmin.h"

#include "access/hash.h"
#include "access/htup_details.h"
#include "catalog/pg_authid.h"
#include "commands/dbcommands.h"
#include "distributed/cancel_utils.h"
#include "distributed/connection_management.h"
#include "distributed/metadata_cache.h"
#include "distributed/shared_connection_stats.h"
#include "distributed/time_constants.h"
#include "distributed/tuplestore.h"
#include "utils/builtins.h"
#include "utils/hashutils.h"
#include "utils/hsearch.h"
#include "storage/ipc.h"


#define REMOTE_CONNECTION_STATS_COLUMNS 4


/*
 * The data structure used to store data in shared memory. This data structure only
 * used for storing the lock. The actual statistics about the connections are stored
 * in the hashmap, which is allocated separately, as Postgres provides different APIs
 * for allocating hashmaps in the shared memory.
 */
typedef struct ConnectionStatsSharedData
{
	int sharedConnectionHashTrancheId;
	char *sharedConnectionHashTrancheName;

	LWLock sharedConnectionHashLock;

/*	/ * */
/*	 * We prefer mutex over LwLocks for two reasons: */
/*	 *   - The operations we perform while holding the lock is very tiny, and */
/*	 *     performance wise, mutex is encouraged by Postgres for such cases */
/*	 *   - We have to acquire the lock "atexit" callback, and LwLocks requires */
/*	 *     MyProc to be avaliable to acquire the lock. However, "atexit", it is */
/*	 *     not guranteed to have MyProc avaliable. On the other hand, "mutex" is */
/*	 *     independent from MyProc. */
/*	 * / */
/*	slock_t mutex; */
} ConnectionStatsSharedData;

typedef struct SharedConnStatsHashKey
{
	/*
	 * In some cases, Citus allows opening connections to hosts where
	 * there is no notion of "WorkerNode", such as task-tracker daemon.
	 * That's why, we prefer to use "hostname/port" over nodeId.
	 */
	char hostname[MAX_NODE_LENGTH];
	int32 port;

	/*
	 * Given that citus.shared_max_pool_size can be defined per database, we
	 * should keep track of shared connections per database.
	 */
	Oid databaseOid;
} SharedConnStatsHashKey;

/* hash entry for per worker stats */
typedef struct SharedConnStatsHashEntry
{
	SharedConnStatsHashKey key;

	int connectionCount;
} SharedConnStatsHashEntry;


/*
 * Controlled via a GUC, never access directly, use GetMaxSharedPoolSize().
 *  "0" means adjust MaxSharedPoolSize automatically by using MaxConnections.
 * "-1" means do not apply connection throttling
 * Anything else means use that number
 */
int MaxSharedPoolSize = 0;

int ConnectionRetryTimout = 120 * MS_PER_SECOND;

/* the following two structs used for accessing shared memory */
static HTAB *SharedConnStatsHash = NULL;
static ConnectionStatsSharedData *ConnectionStatsSharedState = NULL;


static shmem_startup_hook_type prev_shmem_startup_hook = NULL;


/* local function declarations */
static void StoreAllConnections(Tuplestorestate *tupleStore, TupleDesc tupleDescriptor);
static void LockConnectionSharedMemory(LWLockMode lockMode);
static void UnLockConnectionSharedMemory(void);
static void SharedConnectionStatsShmemInit(void);
static size_t SharedConnectionStatsShmemSize(void);
static int SharedConnectionHashCompare(const void *a, const void *b, Size keysize);
static uint32 SharedConnectionHashHash(const void *key, Size keysize);


PG_FUNCTION_INFO_V1(citus_remote_connection_stats);


/*
 * citus_remote_connection_stats returns all the avaliable information about all
 * the remote connections (a.k.a., connections to remote nodes).
 */
Datum
citus_remote_connection_stats(PG_FUNCTION_ARGS)
{
	TupleDesc tupleDescriptor = NULL;

	CheckCitusVersion(ERROR);
	Tuplestorestate *tupleStore = SetupTuplestore(fcinfo, &tupleDescriptor);

	StoreAllConnections(tupleStore, tupleDescriptor);

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupleStore);

	PG_RETURN_VOID();
}


/*
 * StoreAllConnections gets connections established from the current node
 * and inserts them into the given tuplestore.
 *
 * We don't need to enforce any access privileges as the number of backends
 * on any node is already visible on pg_stat_activity to all users.
 */
static void
StoreAllConnections(Tuplestorestate *tupleStore, TupleDesc tupleDescriptor)
{
	Datum values[REMOTE_CONNECTION_STATS_COLUMNS];
	bool isNulls[REMOTE_CONNECTION_STATS_COLUMNS];

	/*
	 * TODO: We should not do all the iterations/operations while
	 * holding the spinlock.
	 */

	/* we're reading all distributed transactions, prevent new backends */
	LockConnectionSharedMemory(LW_SHARED);

	HASH_SEQ_STATUS status;
	SharedConnStatsHashEntry *connectionEntry = NULL;

	hash_seq_init(&status, SharedConnStatsHash);
	while ((connectionEntry = (SharedConnStatsHashEntry *) hash_seq_search(&status)) != 0)
	{
		/* get ready for the next tuple */
		memset(values, 0, sizeof(values));
		memset(isNulls, false, sizeof(isNulls));

		char *databaseName = get_database_name(connectionEntry->key.databaseOid);
		if (databaseName == NULL)
		{
			/* database might have been dropped */
			continue;
		}

		values[0] = PointerGetDatum(cstring_to_text(connectionEntry->key.hostname));
		values[1] = Int32GetDatum(connectionEntry->key.port);
		values[2] = PointerGetDatum(cstring_to_text(databaseName));
		values[3] = Int32GetDatum(connectionEntry->connectionCount);

		tuplestore_putvalues(tupleStore, tupleDescriptor, values, isNulls);
	}

	UnLockConnectionSharedMemory();
}


/*
 * GetMaxSharedPoolSize is a wrapper around MaxSharedPoolSize which is controlled
 * via a GUC.
 *  "0" means adjust MaxSharedPoolSize automatically by using MaxConnections
 * "-1" means do not apply connection throttling
 * Anything else means use that number
 */
int
GetMaxSharedPoolSize(void)
{
	if (MaxSharedPoolSize == 0)
	{
		return MaxConnections;
	}

	return MaxSharedPoolSize;
}


/*
 * WaitOrErrorForSharedConnection tries to increment the shared connection
 * counter for the given hostname/port and the current database in
 * SharedConnStatsHash.
 *
 * The function implements a retry mechanism. If the function cannot increment
 * the counter withing the specificed amount of the time, it throws an error.
 */
void
WaitOrErrorForSharedConnection(const char *hostname, int port)
{
	int retryCount = 0;

	/*
	 * Sleep this amount before retrying, there is not much value retrying too often
	 * as the remote node is too busy. That's the reason we're retrying.
	 */
	double sleepTimeoutMsec = 1000;

	/* In practice, 0 disables the retry logic */
	int allowedRetryCount = ConnectionRetryTimout / sleepTimeoutMsec;

	while (!TryToIncrementSharedConnectionCounter(hostname, port))
	{
		int latchFlags = WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH;

		CHECK_FOR_INTERRUPTS();

		int rc = WaitLatch(MyLatch, latchFlags, (long) sleepTimeoutMsec,
						   PG_WAIT_EXTENSION);

		/* emergency bailout if postmaster has died */
		if (rc & WL_POSTMASTER_DEATH)
		{
			ereport(ERROR, (errmsg("postmaster was shut down, exiting")));
		}
		else if (rc & WL_LATCH_SET)
		{
			ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();

			if (IsHoldOffCancellationReceived())
			{
				/* in case the interrupts are hold, we still want to cancel */
				ereport(ERROR, (errmsg("canceling statement due to user request")));
			}
		}
		else if (rc & WL_TIMEOUT)
		{
			++retryCount;
			if (allowedRetryCount <= retryCount)
			{
				ereport(ERROR, (errmsg("citus.max_shared_pool_size number of connections "
									   "are already established to the node %s:%d,"
									   "so cannot establish any more connections",
									   hostname, port),
								errhint("consider increasing "
										"citus.max_shared_pool_size or "
										"citus.connection_retry_timeout")));
			}
		}
	}
}


/*
 * TryToIncrementSharedConnectionCounter tries to increment the shared
 * connection counter for the given nodeId and the current database in
 * SharedConnStatsHash.
 *
 * The function first checks whether the number of connections is less than
 * citus.max_shared_pool_size. If so, the function increments the counter
 * by one and returns true. Else, the function returns false.
 */
bool
TryToIncrementSharedConnectionCounter(const char *hostname, int port)
{
	if (GetMaxSharedPoolSize() == -1)
	{
		/* connection throttling disabled */
		return true;
	}

	bool counterIncremented = false;
	SharedConnStatsHashKey connKey;

	strlcpy(connKey.hostname, hostname, MAX_NODE_LENGTH);
	if (strlen(hostname) > MAX_NODE_LENGTH)
	{
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("hostname exceeds the maximum length of %d",
							   MAX_NODE_LENGTH)));
	}

	connKey.port = port;
	connKey.databaseOid = MyDatabaseId;

	LockConnectionSharedMemory(LW_EXCLUSIVE);

	/*
	 * Note that while holding a spinlock, it would not allowed to use HASH_ENTER_NULL
	 * if the entries in SharedConnStatsHash were allocated via palloc (as palloc
	 * might throw OOM errors). However, in this case we're safe as the hash map is
	 * allocated in shared memory, which doesn't rely on palloc for memory allocation.
	 * This is already asserted in hash_search() by Postgres.
	 */
	bool entryFound = false;
	SharedConnStatsHashEntry *connectionEntry =
		hash_search(SharedConnStatsHash, &connKey, HASH_ENTER_NULL, &entryFound);

	/*
	 * It is possible to throw an error at this point, but that doesn't help us in anyway.
	 * Instead, we try our best, let the connection establishment continue by-passing the
	 * connection throttling.
	 */
	if (!connectionEntry)
	{
		UnLockConnectionSharedMemory();

		return true;
	}

	if (!entryFound)
	{
		/* we successfully allocated the entry for the first time, so initialize it */
		connectionEntry->connectionCount = 1;

		counterIncremented = true;
	}
	else if (connectionEntry->connectionCount + 1 > GetMaxSharedPoolSize())
	{
		/* there is no space left for this connection */
		counterIncremented = false;
	}
	else
	{
		connectionEntry->connectionCount++;
		counterIncremented = true;
	}

	UnLockConnectionSharedMemory();

	return counterIncremented;
}


/*
 * DecrementSharedConnectionCounter decrements the shared counter
 * for the given hostname and port.
 */
void
DecrementSharedConnectionCounter(const char *hostname, int port)
{
	SharedConnStatsHashKey connKey;

	if (GetMaxSharedPoolSize() == -1)
	{
		/* connection throttling disabled */
		return;
	}

	strlcpy(connKey.hostname, hostname, MAX_NODE_LENGTH);
	if (strlen(hostname) > MAX_NODE_LENGTH)
	{
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("hostname exceeds the maximum length of %d",
							   MAX_NODE_LENGTH)));
	}

	connKey.port = port;
	connKey.databaseOid = MyDatabaseId;

	LockConnectionSharedMemory(LW_EXCLUSIVE);

	bool entryFound = false;
	SharedConnStatsHashEntry *connectionEntry =
		hash_search(SharedConnStatsHash, &connKey, HASH_FIND, &entryFound);

	/* we should never decrement for non-existing connections */
	Assert((connectionEntry && entryFound && connectionEntry->connectionCount > 0));

	connectionEntry->connectionCount -= 1;

	UnLockConnectionSharedMemory();
}


/*
 * LockConnectionSharedMemory is a utility function that should be used when
 * accessing to the SharedConnStatsHash, which is in the shared memory.
 */
static void
LockConnectionSharedMemory(LWLockMode lockMode)
{
	LWLockAcquire(&ConnectionStatsSharedState->sharedConnectionHashLock, lockMode);

	/* SpinLockAcquire(&ConnectionStatsSharedState->mutex); */
}


/*
 * UnLockConnectionSharedMemory is a utility function that should be used after
 * LockConnectionSharedMemory().
 */
static void
UnLockConnectionSharedMemory(void)
{
	LWLockRelease(&ConnectionStatsSharedState->sharedConnectionHashLock);

	/* SpinLockRelease(&ConnectionStatsSharedState->mutex); */
}


/*
 * InitializeSharedConnectionStats requests the necessary shared memory
 * from Postgres and sets up the shared memory startup hook.
 */
void
InitializeSharedConnectionStats(void)
{
	/* allocate shared memory */
	if (!IsUnderPostmaster)
	{
		RequestAddinShmemSpace(SharedConnectionStatsShmemSize());
	}

	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = SharedConnectionStatsShmemInit;
}


/*
 * SharedConnectionStatsShmemSize returns the size that should be allocated
 * on the shared memory for shared connection stats.
 */
static size_t
SharedConnectionStatsShmemSize(void)
{
	Size size = 0;

	size = add_size(size, sizeof(ConnectionStatsSharedData));

	/* size = add_size(size, mul_size(sizeof(LWLock), MaxWorkerNodesTracked)); */

	Size hashSize = hash_estimate_size(MaxWorkerNodesTracked,
									   sizeof(SharedConnStatsHashEntry));

	size = add_size(size, hashSize);

	return size;
}


/*
 * SharedConnectionStatsShmemInit initializes the shared memory used
 * for keeping track of connection stats across backends.
 */
static void
SharedConnectionStatsShmemInit(void)
{
	bool alreadyInitialized = false;
	HASHCTL info;

	/* create (nodeId,database) -> [counter] */
	memset(&info, 0, sizeof(info));
	info.keysize = sizeof(SharedConnStatsHashKey);
	info.entrysize = sizeof(SharedConnStatsHashEntry);
	info.hash = SharedConnectionHashHash;
	info.match = SharedConnectionHashCompare;
	uint32 hashFlags = (HASH_ELEM | HASH_FUNCTION | HASH_COMPARE);

	/*
	 * Currently the lock isn't required because allocation only happens at
	 * startup in postmaster, but it doesn't hurt, and makes things more
	 * consistent with other extensions.
	 */
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	ConnectionStatsSharedState =
		(ConnectionStatsSharedData *) ShmemInitStruct(
			"Shared Connection Stats Data",
			sizeof(ConnectionStatsSharedData),
			&alreadyInitialized);

	if (!alreadyInitialized)
	{
		ConnectionStatsSharedState->sharedConnectionHashTrancheId = LWLockNewTrancheId();
		ConnectionStatsSharedState->sharedConnectionHashTrancheName =
			"Shared Connection Tracking Hash Tranche";
		LWLockRegisterTranche(ConnectionStatsSharedState->sharedConnectionHashTrancheId,
							  ConnectionStatsSharedState->sharedConnectionHashTrancheName);

		LWLockInitialize(&ConnectionStatsSharedState->sharedConnectionHashLock,
						 ConnectionStatsSharedState->sharedConnectionHashTrancheId);

		/* SpinLockInit(&ConnectionStatsSharedState->mutex); */
	}

	/*  allocate hash table */
	SharedConnStatsHash =
		ShmemInitHash("Shared Conn. Stats Hash", MaxWorkerNodesTracked,
					  MaxWorkerNodesTracked, &info, hashFlags);

	LWLockRelease(AddinShmemInitLock);

	Assert(SharedConnStatsHash != NULL);
	Assert(ConnectionStatsSharedState->sharedConnectionHashTrancheId != 0);

	if (prev_shmem_startup_hook != NULL)
	{
		prev_shmem_startup_hook();
	}
}


static uint32
SharedConnectionHashHash(const void *key, Size keysize)
{
	SharedConnStatsHashKey *entry = (SharedConnStatsHashKey *) key;

	uint32 hash = string_hash(entry->hostname, NAMEDATALEN);
	hash = hash_combine(hash, hash_uint32(entry->port));
	hash = hash_combine(hash, hash_uint32(entry->databaseOid));

	return hash;
}


static int
SharedConnectionHashCompare(const void *a, const void *b, Size keysize)
{
	SharedConnStatsHashKey *ca = (SharedConnStatsHashKey *) a;
	SharedConnStatsHashKey *cb = (SharedConnStatsHashKey *) b;

	if (strncmp(ca->hostname, cb->hostname, MAX_NODE_LENGTH) != 0 ||
		ca->port != cb->port ||
		ca->databaseOid != cb->databaseOid)
	{
		return 1;
	}
	else
	{
		return 0;
	}
}
