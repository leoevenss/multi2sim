#ifndef MEM_SYSTEM_PRIVATE_MODULE_H
#define	MEM_SYSTEM_PRIVATE_MODULE_H

#include "module.h"
#include "tlb-cache.h"
#include "private-cache.h"
#include "prefetcher.h"


/*
 * Class 'PrivateCoherenceStat'
 */

CLASS_BEGIN(PrivateCoherenceStat, CoherenceStat)

        long long msgs[6]; /* messages */

        /* Late and early coherence message statistics */
        struct
        {
                long long msgs; /* number of messages */
                long long cycles; /* cycles messages had to wait */
        } late, early;

CLASS_END(PrivateCoherenceStat)

void PrivateCoherenceStatCreate(PrivateCoherenceStat *prstat, char *name);
void PrivateCoherenceStatDestroy(PrivateCoherenceStat *prstat);

static inline char* PrivateCoherenceStatGetName(PrivateCoherenceStat *prstat) { return CoherenceStatGetName(asCoherenceStat(prstat)); }
void PrivateCoherenceStatDumpReport(PrivateCoherenceStat *prstat, FILE *f);


/*
 * Class 'PrivateModule'
 */

/* Access hash table size */
#define MOD_ACCESS_HASH_TABLE_SIZE  29


CLASS_BEGIN(PrivateModule, Module)

        /* Core and thread identifiers being serviced by module */
        int core;
        int thread;

        /* Ports: There are two/three types of ports depending on module
         * configuration. Index-0 ports are used for processing instruction
         * fetch requests. Index-1 port are used for processing all data
         * requests when dedicated write ports are not modeled. If dedicated
         * write ports are modeled then Index-2 ports are used for processing
         * data write request. */
        struct port_t
        {
                long long cycle;
                unsigned int total;
                unsigned int occupied;
        } ports[3];

        /* Caches: Each private module can model two-levels of private caches.
         * Two different L1 caches are used for instruction and data requests.
         * L2 cache is optional, and can be absent. The L1 and L2 caches are
         * non-inclusive in nature. On a miss in private caches blocks are
         * fetched from LLC caches. */
        PrivateCache *caches[NumPrivateCacheTypes];

        /* TLBs: Each private module can model two-levels of private TLBs. Two
         * different L1-TLBs are used for instruction and data requests. L2 TLB
         * is optional, and can be absent. The L1 and L2 TLBs are inclusive. On
         * a miss in private TLBs the PTEs are fetched from LLC caches. */
        TLBCache *tlbs[NumTLBCacheTypes];

        /* Latency after which nacked requests must be retried */
        int retry_latency;

        /* WBs */
        int num_wbs;
        PrivateWBList *occupied_wbs;
        PrivateWBList *available_wbs;

        /* MSHRs */
        int num_mshrs;
        PrivateMSHRList *occupied_mshrs;
        PrivateMSHRList *available_mshrs;

        /* Pending message lists */
        MsgList *msgs_waiting_for_pte;
        MsgList *msgs_waiting_for_free_wb;
        MsgList *msgs_waiting_for_free_mshr;

        /* Statistics */
        struct
        {
                /* Request wait statistics */
                struct
                {
                        long long total;
                        long long unique;
                } wait[NumAccessTypes][NumPrModReqWaitTypes];

                /* Coherence message statistics - for invalidation, back
                 * invalidation and intervention requests received by
                 * private module. */
                PrivateCoherenceStat *cohrstats[NumMsgTypes];

                /* Writeback statistics */
                struct
                {
                        long long nacks; /* number of nacks */
                        long long retries; /* number of retries */

                        long long count; /* number of writebacks */
                        long long dirty; /* number of dirty writebacks */
                        long long shared; /* number of shared writebacks */
                } writeback;

                /* Block access statistics */
                struct
                {
                        long long nacks[NumAccessTypes]; // number of requests nacked
                        long long l2hits[NumAccessTypes]; // number of L2 cache hits
                        long long accesses[NumAccessTypes]; // number of accesses
                        long long locall1hits[NumAccessTypes]; // number of local L1 cache hits
                        long long remotel1hits[NumAccessTypes]; // number of remote L1 cache hits
                } block;

                /* Page table entry access statistics */
                struct
                {
                        long long nacks[NumAccessTypes]; // number of requests nacked
                        long long l2hits[NumAccessTypes]; // number of L2 TLB hits
                        long long l1hits[NumAccessTypes]; // number of L1 TLB hits
                        long long accesses[NumAccessTypes]; // number of TLB accesses
                } pte;


                /* Histogram for tracking number of accesses to blocks
                 * in private caches. */
                struct
                {
                        unsigned int num_buckets; /* number of buckets */
                        long long *buckets; /* buckets */
                } accesseshist;

        } stats;

        /* Store request which misses in L1 cache */
        Msg* pending_store;

        /* Hash table of core request messages */
        struct access_list_t
        {
                int bucket_list_count;
                int bucket_list_max;
                Msg *bucket_list_head;
                Msg *bucket_list_tail;
        } access_hash_table[MOD_ACCESS_HASH_TABLE_SIZE];

	/* Stream prefetcher */
        struct stream_prefetcher_t *sprefetcher;

CLASS_END(PrivateModule)

void PrivateModuleCreate(PrivateModule *prmod, char *name, AddressRange *arange);
void PrivateModuleDestroy(PrivateModule *prmod);

void PrivateModuleDumpChunkReport(Module *mod, FILE *f, unsigned int chunk);
void PrivateModuleDumpReport(Module *mod, FILE *f);

void PrivateModuleInitPorts(PrivateModule *prmod, unsigned int inst_ports, unsigned int data_ports, unsigned int write_ports);
void PrivateModuleInitBuffers(PrivateModule *prmod, unsigned int num_mshrs, unsigned int num_wbs);
void PrivateModuleInitRetryLatency(PrivateModule *prmod, int retry_latency);
void PrivateModuleInitCoherenceStat(PrivateModule *prmod, MsgType type);
void PrivateModuleInitCoherenceStats(PrivateModule *prmod);

void PrivateModuleInitTLB(PrivateModule *prmod, TLBCache *tcache);
void PrivateModuleInitCache(PrivateModule *prmod, PrivateCache *pcache);

void PrivateModuleInitAccessesHist(PrivateModule *prmod, unsigned int num_buckets);
void PrivateModuleUpdateAccessesHist(PrivateModule *prmod, long long accesses);
void PrivateModuleDumpAccessesHistStats(PrivateModule *prmod, FILE *f);
void PrivateModuleDestroyAccessesHist(PrivateModule *prmod);

Msg* PrivateModuleAccessListFindMsg(PrivateModule *prmod, long long id);
void PrivateModuleAccessListInsertMsg(PrivateModule *prmod, Msg *msg);
void PrivateModuleAccessListRemoveMsg(PrivateModule *prmod, Msg *msg);

void PrivateModuleGetCaches(PrivateModule *prmod, Msg *msg, PrivateCache **pl2cache, PrivateCache **pll1cache, PrivateCache **prl1cache);
void PrivateModuleGetTLBs(PrivateModule *prmod, Msg *msg, TLBCache **pl1tlb, TLBCache **pl2tlb);
struct port_t* PrivateModuleGetPort(PrivateModule *prmod, AccessType atype);

void PrivateModuleClearPendingStore(PrivateModule *prmod, Msg *msg);
void PrivateModuleSetPendingStore(PrivateModule *prmod, Msg *msg);

PrivateMSHR* PrivateModuleFindMSHR(PrivateModule *prmod, long long tag);
void PrivateModuleFreeMSHR(PrivateModule *prmod, PrivateMSHR *mshr);

PrivateWB* PrivateModuleFindWB(PrivateModule *prmod, long long tag);
void PrivateModuleFreeWB(PrivateModule *prmod, PrivateWB *wb);

static inline char* PrivateModuleGetName(PrivateModule *prmod)
{
        return ModuleGetName(asModule(prmod));
}

unsigned int PrivateModuleGetRemoteCacheBlock(PrivateModule *prmod, PrivateCache *lcache, PrivateBlock *lblk, PrivateCache **prcache, PrivateBlock **prblk);

/* Core access functions */
long long PrivateModuleAccess(PrivateModule *prmod, AccessType atype, long long address, unsigned int size, struct linked_list_t *list, struct uop_t *uop);
unsigned int PrivateModuleCanAccess(PrivateModule *prmod, AccessType atype);

unsigned int PrivateModuleInFlightAccess(PrivateModule *prmod, long long id);
void PrivateModuleInvalidateAccess(PrivateModule *prmod, long long id);
struct uop_t* PrivateModuleGetPendingStoreUop(PrivateModule *prmod);


#endif	/* MEM_SYSTEM_PRIVATE_MODULE_H */
