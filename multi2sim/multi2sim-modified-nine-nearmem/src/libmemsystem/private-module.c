#include <libesim/esim.h>
#include <libstruct/misc.h>
#include <libstruct/debug.h>
#include <libmhandle/mhandle.h>
#include <libcpuarch/cpuarch.h>

#include "mmu.h"
#include "mem-system.h"

#include "message-list.h"
#include "message-manager.h"

#include "private-wb.h"
#include "private-mshr.h"
#include "private-wb-list.h"
#include "private-mshr-list.h"

#include "tlb-cache.h"
#include "private-cache.h"
#include "private-module.h"
#include "prefetcher.h"


static unsigned int private_mod_id = 0;


void PrivateCoherenceStatCreate(PrivateCoherenceStat *prstat, char *name)
{
        /* Initialize coherence stat */
        CoherenceStatCreate(asCoherenceStat(prstat), name);
}


void PrivateCoherenceStatDestroy(PrivateCoherenceStat *prstat)
{
        /* nothing to do */
}


void PrivateCoherenceStatDumpReport(PrivateCoherenceStat *prstat, FILE *f)
{
        char *indexnames[6] = {"Miss", "L2", "DL1", "DL1L2", "IL1", "IL1L2"};
        char *name = PrivateCoherenceStatGetName(prstat);
        unsigned int total = 0;
        unsigned int index = 0;

        /* Compute total number of messages */
        for (index = 0; index < 6; index++)
                total += prstat->msgs[index];

        /* There is nothing to do if total number of messages is zero */
        if (!total) return;

        /* Dump message statistics */
        for (index = 0; index < 6; index++)
                fprintf(f, "%s.%s = %lld\n", name, indexnames[index], prstat->msgs[index]);
        fprintf(f, "\n");

        /* Dump late coherence message statistics */
        fprintf(f, "%s.Late.Total = %lld\n", name, prstat->late.msgs);
        fprintf(f, "%s.Late.Cycles = %lld\n", name, prstat->late.cycles);
        fprintf(f, "\n");

        /* Dump early coherence message statistics */
        fprintf(f, "%s.Early.Total = %lld\n", name, prstat->early.msgs);
        fprintf(f, "%s.Early.Cycles = %lld\n", name, prstat->early.cycles);
        fprintf(f, "\n");
}


void PrivateModuleCreate(PrivateModule *prmod, char *name, AddressRange *arange)
{
        int core, thread;
        Module *mod = asModule(prmod);

        /* Initialize module */
        ModuleCreate(mod, ModuleTypePrivate, name, arange, private_mod_id++);

        /* Virtual functions */
        mod->DumpChunkReport = PrivateModuleDumpChunkReport;
        mod->DumpReport = PrivateModuleDumpReport;

        /* Set compute core identifier */
        core = ModuleGetId(mod) / cpu_threads;
        prmod->core = core;

        /* Set compute thread identifier */
        thread = ModuleGetId(mod) % cpu_threads;
        prmod->thread = thread;

        /* Set private module in compute node */
        THREAD.module = prmod;

        /* Initialize coherence statistics */
        PrivateModuleInitCoherenceStats(prmod);

        /* Create message lists */
        prmod->msgs_waiting_for_pte = new(MsgList);
        prmod->msgs_waiting_for_free_wb = new(MsgList);
        prmod->msgs_waiting_for_free_mshr = new(MsgList);
}


void PrivateModuleDestroy(PrivateModule *prmod)
{
        /* Free TLBs */
        for (TLBCacheType tctype = 0; tctype < NumTLBCacheTypes; tctype++)
        {
                if (!prmod->tlbs[tctype]) continue;
                delete(prmod->tlbs[tctype]);
        }

        /* Free caches */
        for (PrivateCacheType pctype = 0; pctype < NumPrivateCacheTypes; pctype++)
        {
                if (!prmod->caches[pctype]) continue;
                delete(prmod->caches[pctype]);
        }

        /* Free WBS */
        delete(prmod->occupied_wbs);
        delete(prmod->available_wbs);

        /* Free MSHRs */
        delete(prmod->occupied_mshrs);
        delete(prmod->available_mshrs);

        /* Free coherence stats */
        for (MsgType mtype = 0; mtype < NumMsgTypes; mtype++)
        {
                if (!prmod->stats.cohrstats[mtype]) continue;
                delete(prmod->stats.cohrstats[mtype]);
        }

        /* Free message lists */
        delete(prmod->msgs_waiting_for_pte);
        delete(prmod->msgs_waiting_for_free_wb);
        delete(prmod->msgs_waiting_for_free_mshr);

	/* Free stream prefetcher */
        streamPrefetcher_free(prmod->sprefetcher);

        /* Free accesses histogram */
        PrivateModuleDestroyAccessesHist(prmod);

	
}


void PrivateModuleDumpChunkReport(Module *mod, FILE *f, unsigned int chunk)
{
        /* nothing to do */
}


void PrivateModuleDumpReport(Module *mod, FILE *f)
{
        PrivateModule *prmod = asPrivateModule(mod);

        /* Accumulate access statistics for private cache blocks */
        debug_off(mem_debug_category); // disable memory debugging
        for (PrivateCacheType pctype = 0; pctype < NumPrivateCacheTypes; pctype++)
        {
                /* Skip if cache is not being modeled */
                PrivateCache *lcache = prmod->caches[pctype];
                if (!lcache) continue;

                for (unsigned int id = 0; id < PrivateCacheGetNumBlocks(lcache); id++)
                {
                        /* Skip if block is not in a valid state */
                        PrivateBlock *lblk = PrivateCacheGetBlock(lcache, id);
                        if (!PrivateBlockIsValid(lblk)) continue;

                        PrivateBlock *rblk = NULL;
                        PrivateCache *rcache = NULL;
                        if (PrivateModuleGetRemoteCacheBlock(prmod, lcache, lblk, &rcache, &rblk))
                        {
                                /* Set access in remote cache block */
                                if (PrivateBlockGetAccesses(rblk) < PrivateBlockGetAccesses(lblk))
                                        PrivateBlockSetAccesses(rblk, PrivateBlockGetAccesses(lblk));
                        }
                        else
                        {
                                /* Update accesses histogram */
                                PrivateModuleUpdateAccessesHist(prmod, PrivateBlockGetAccesses(lblk));
                        }

                        /* Invalidate local cache block */
                        PrivateBlockSetState(lblk, PrivateBlockInvalid);
                }
        }
        debug_on(mem_debug_category); // enable memory debugging

        fprintf(f, "[ %s ]\n", ModuleGetName(mod));
        fprintf(f, "InstPorts = %d\n", prmod->ports[0].total);
        fprintf(f, "DataPorts = %d\n", prmod->ports[1].total);
        fprintf(f, "WritePorts = %d\n", prmod->ports[2].total);
        fprintf(f, "\n");

        fprintf(f, "MSHRs = %d\n", prmod->num_mshrs);
        fprintf(f, "WriteBuffers = %d\n", prmod->num_wbs);
        fprintf(f, "\n");

        fprintf(f, "NackRetryLatency = %d\n", prmod->retry_latency);
        fprintf(f, "\n");

        /* Dump cache statistics */
        for (PrivateCacheType pctype = 0; pctype < NumPrivateCacheTypes; pctype++)
        {
                if (!prmod->caches[pctype]) continue;
                PrivateCacheDumpStats(prmod->caches[pctype], f);
        }

        /* Dump TLB statistics */
        for (TLBCacheType tctype = 0; tctype < NumTLBCacheTypes; tctype++)
        {
                if (!prmod->tlbs[tctype]) continue;
                TLBCacheDumpStats(prmod->tlbs[tctype], f);
        }

        /* Dump block access statistics */
        for (AccessType atype = 0; atype < NumAccessTypes; atype++)
        {
                fprintf(f, "Block.%s.Accesses = %lld\n", MapAccessType(atype), prmod->stats.block.accesses[atype]);
                fprintf(f, "Block.%s.LocalL1Hits = %lld\n", MapAccessType(atype), prmod->stats.block.locall1hits[atype]);
                fprintf(f, "Block.%s.RemoteL1Hits = %lld\n", MapAccessType(atype), prmod->stats.block.remotel1hits[atype]);
                fprintf(f, "Block.%s.L2Hits = %lld\n", MapAccessType(atype), prmod->stats.block.l2hits[atype]);
                fprintf(f, "Block.%s.Nacks = %lld\n", MapAccessType(atype), prmod->stats.block.nacks[atype]);
                fprintf(f, "\n");
        }

        /* Dump accesses histogram */
        PrivateModuleDumpAccessesHistStats(prmod, f);

        /* Dump page table entry access statistics */
        for (AccessType atype = 0; atype < NumAccessTypes; atype++)
        {
                fprintf(f, "PTE.%s.Accesses = %lld\n", MapAccessType(atype), prmod->stats.pte.accesses[atype]);
                fprintf(f, "PTE.%s.L1Hits = %lld\n", MapAccessType(atype), prmod->stats.pte.l1hits[atype]);
                fprintf(f, "PTE.%s.L2Hits = %lld\n", MapAccessType(atype), prmod->stats.pte.l2hits[atype]);
                fprintf(f, "PTE.%s.Nacks = %lld\n", MapAccessType(atype), prmod->stats.pte.nacks[atype]);
                fprintf(f, "\n");
        }

        /* Dump request wait statistics */
        for (AccessType atype = 0; atype < NumAccessTypes; atype++)
        {
                for (ReqWaitType rtype = 0; rtype < NumPrModReqWaitTypes; rtype++)
                {
                        fprintf(f, "%s%s = %lld\n", MapAccessType(atype), MapReqWaitType(rtype), prmod->stats.wait[atype][rtype].total);
                        fprintf(f, "%sUnique%s = %lld\n", MapAccessType(atype), MapReqWaitType(rtype), prmod->stats.wait[atype][rtype].unique);
                }
                fprintf(f, "\n");
        }

        /* Dump coherence message statistics */
        for (MsgType mtype = 0; mtype < NumMsgTypes; mtype++)
        {
                if (!prmod->stats.cohrstats[mtype]) continue;
                PrivateCoherenceStatDumpReport(prmod->stats.cohrstats[mtype], f);
        }

        /* Dump writeback statistics */
        fprintf(f, "WriteBack.Count = %lld\n", prmod->stats.writeback.count);
        fprintf(f, "WriteBack.Dirty = %lld\n", prmod->stats.writeback.dirty);
        fprintf(f, "WriteBack.Shared = %lld\n", prmod->stats.writeback.shared);

        fprintf(f, "WriteBack.Nacks = %lld\n", prmod->stats.writeback.nacks);
        fprintf(f, "WriteBack.Retries = %lld\n", prmod->stats.writeback.retries);
        fprintf(f, "\n");
}


void PrivateModuleInitPorts(PrivateModule *prmod, unsigned int inst_ports, unsigned int data_ports, unsigned int write_ports)
{
        /* Check: number of instruction ports must be a positive integer */
        assert(inst_ports);

        /* Check: number of data ports must be a positive integer */
        assert(data_ports);

        prmod->ports[0].total = inst_ports;
        prmod->ports[1].total = data_ports;
        prmod->ports[2].total = write_ports;
}


void PrivateModuleInitBuffers(PrivateModule *prmod, unsigned int num_mshrs, unsigned int num_wbs)
{
        /* Check: number of MSHRs must be a positive integer */
        assert(num_mshrs);

        /* Check: number of WBs must be a positive integer not less than 2 */
        assert(num_wbs >= 2);

        prmod->num_wbs = num_wbs;
        prmod->num_mshrs = num_mshrs;

        prmod->occupied_wbs = new(PrivateWBList);
        prmod->available_wbs = new(PrivateWBList);

        prmod->occupied_mshrs = new(PrivateMSHRList);
        prmod->available_mshrs = new(PrivateMSHRList);

        for (unsigned int num_wb = 0; num_wb < num_wbs; num_wb++)
                PrivateWBListEnqueue(prmod->available_wbs, new(PrivateWB, num_wb));

        for (unsigned int num_mshr = 0; num_mshr < num_mshrs; num_mshr++)
                PrivateMSHRListEnqueue(prmod->available_mshrs, new(PrivateMSHR, num_mshr));
}


void PrivateModuleInitRetryLatency(PrivateModule *prmod, int retry_latency)
{
        /* Check: retry latency must be a positive integer */
        assert(retry_latency > 0);

        prmod->retry_latency = retry_latency;
}


void PrivateModuleInitCoherenceStat(PrivateModule *prmod, MsgType type)
{
        prmod->stats.cohrstats[type] = new(PrivateCoherenceStat, MapMsgType(type));
}


void PrivateModuleInitCoherenceStats(PrivateModule *prmod)
{
        /* Initialize shared back invalidation statistics */
        PrivateModuleInitCoherenceStat(prmod, Partial_SparseDir_ShBkInvalReq);
        PrivateModuleInitCoherenceStat(prmod, Brdcst_SparseDir_ShBkInvalReq);
        PrivateModuleInitCoherenceStat(prmod, Region_SparseDir_ShBkInvalReq);
        PrivateModuleInitCoherenceStat(prmod, Norm_PrivateDir_ShBkInvalReq);
        PrivateModuleInitCoherenceStat(prmod, Norm_SparseDir_ShBkInvalReq);

        PrivateModuleInitCoherenceStat(prmod, ShCorruptWb_LLC_ShBkInvalReq);
        PrivateModuleInitCoherenceStat(prmod, OwCorruptWb_LLC_ShBkInvalReq);
        PrivateModuleInitCoherenceStat(prmod, Brdcst_LLC_ShBkInvalReq);
        PrivateModuleInitCoherenceStat(prmod, Norm_LLC_ShBkInvalReq);

        /* Initialize exclusive back invalidation statistics */
        PrivateModuleInitCoherenceStat(prmod, Region_SparseDir_ExBkInvalReq);
        PrivateModuleInitCoherenceStat(prmod, Norm_PrivateDir_ExBkInvalReq);
        PrivateModuleInitCoherenceStat(prmod, Norm_SparseDir_ExBkInvalReq);

        PrivateModuleInitCoherenceStat(prmod, ShCorruptWb_LLC_ExBkInvalReq);
        PrivateModuleInitCoherenceStat(prmod, OwCorruptWb_LLC_ExBkInvalReq);
        PrivateModuleInitCoherenceStat(prmod, Brdcst_LLC_ExBkInvalReq);
        PrivateModuleInitCoherenceStat(prmod, Norm_LLC_ExBkInvalReq);

        /* Initialize shared forward statistics */
        PrivateModuleInitCoherenceStat(prmod, ShCorruptMove_ShFwdReq);
        PrivateModuleInitCoherenceStat(prmod, OwCorruptMove_ShFwdReq);
        PrivateModuleInitCoherenceStat(prmod, ShCorruptWb_ShFwdReq);
        PrivateModuleInitCoherenceStat(prmod, Norm_ShFwdReq);
        PrivateModuleInitCoherenceStat(prmod, Wb_ShFwdReq);

        /* Initialize exclusive forward statistics */
        PrivateModuleInitCoherenceStat(prmod, ShCorruptMove_ExFwdReq);
        PrivateModuleInitCoherenceStat(prmod, OwCorruptMove_ExFwdReq);
        PrivateModuleInitCoherenceStat(prmod, ShCorruptWb_ExFwdReq);
        PrivateModuleInitCoherenceStat(prmod, OwCorruptWb_ExFwdReq);
        PrivateModuleInitCoherenceStat(prmod, Norm_ExFwdReq);
        PrivateModuleInitCoherenceStat(prmod, Wb_ExFwdReq);

        /* Initialize shared intervention statistics */
        PrivateModuleInitCoherenceStat(prmod, ShCorruptMove_ShIntrvReq);
        PrivateModuleInitCoherenceStat(prmod, OwCorruptMove_ShIntrvReq);
        PrivateModuleInitCoherenceStat(prmod, ShCorruptWb_ShIntrvReq);
        PrivateModuleInitCoherenceStat(prmod, Brdcst_ShIntrvReq);
        PrivateModuleInitCoherenceStat(prmod, Norm_ShIntrvReq);
        PrivateModuleInitCoherenceStat(prmod, Opt_ShIntrvReq);
        PrivateModuleInitCoherenceStat(prmod, Wb_ShIntrvReq);

        /* Initialize exclusive intervention statistics */
        PrivateModuleInitCoherenceStat(prmod, ShCorruptMove_ExIntrvReq);
        PrivateModuleInitCoherenceStat(prmod, OwCorruptMove_ExIntrvReq);
        PrivateModuleInitCoherenceStat(prmod, ShCorruptWb_ExIntrvReq);
        PrivateModuleInitCoherenceStat(prmod, OwCorruptWb_ExIntrvReq);
        PrivateModuleInitCoherenceStat(prmod, Brdcst_ExIntrvReq);
        PrivateModuleInitCoherenceStat(prmod, Norm_ExIntrvReq);
        PrivateModuleInitCoherenceStat(prmod, Opt_ExIntrvReq);
        PrivateModuleInitCoherenceStat(prmod, Wb_ExIntrvReq);

        /* Initialize invalidation statistics */
        PrivateModuleInitCoherenceStat(prmod, Brdcst_InvalReq);
        PrivateModuleInitCoherenceStat(prmod, ExFwd_InvalReq);
        PrivateModuleInitCoherenceStat(prmod, Norm_InvalReq);
}


void PrivateModuleInitTLB(PrivateModule *prmod, TLBCache *tlb)
{
        prmod->tlbs[TLBCacheGetType(tlb)] = tlb;
}


void PrivateModuleInitCache(PrivateModule *prmod, PrivateCache *pcache)
{
        prmod->caches[PrivateCacheGetType(pcache)] = pcache;
}


void PrivateModuleInitAccessesHist(PrivateModule *prmod, unsigned int num_buckets)
{
        /* Check: number of buckets must be a positive integer */
        assert(num_buckets);

        prmod->stats.accesseshist.num_buckets = num_buckets;
        prmod->stats.accesseshist.buckets = xcalloc(num_buckets + 2, sizeof(long long));
}


void PrivateModuleUpdateAccessesHist(PrivateModule *prmod, long long accesses)
{
        unsigned int num_buckets = prmod->stats.accesseshist.num_buckets;
        long long *buckets = prmod->stats.accesseshist.buckets;

        /* Skip if access histogram is not enabled */
        if (!buckets) return;

        /* When number of accesses is less than number of buckets */
        if (accesses < num_buckets)
        {
                buckets[accesses]++;
                return;
        }

        buckets[num_buckets + 1] += accesses;
        buckets[num_buckets]++;
}


void PrivateModuleDumpAccessesHistStats(PrivateModule *prmod, FILE *f)
{
        unsigned int num_buckets = prmod->stats.accesseshist.num_buckets;
        long long *buckets = prmod->stats.accesseshist.buckets;

        /* Skip if access histogram is not enabled */
        if (!buckets) return;

        for (unsigned int index = 0; index < num_buckets; index++)
        {
                if (!buckets[index]) continue;
                fprintf(f, "Hist.Accesses.%d = %lld\n", index, buckets[index]);
        }

        if (buckets[num_buckets])
        {
                fprintf(f, "Hist.Accesses.%lld", buckets[num_buckets + 1]);
                fprintf(f, " = %lld\n", buckets[num_buckets]);
        }

        fprintf(f, "\n");
}


void PrivateModuleDestroyAccessesHist(PrivateModule *prmod)
{
        /* Free accesses histogram */
        if (!prmod->stats.accesseshist.buckets) return;
        free(prmod->stats.accesseshist.buckets);
}


/* Returns pointer to message if found message with id in hash table, else returns NULL */
Msg* PrivateModuleAccessListFindMsg(PrivateModule *prmod, long long id)
{
        Msg *msg = NULL;
        struct access_list_t *access_list = NULL;

        /* Check: request id is valid */
        assert(id > 0);

        /* Get access list from access hash table */
        access_list = &prmod->access_hash_table[id % MOD_ACCESS_HASH_TABLE_SIZE];


        /* Look for access in access hash table */
        DOUBLE_LINKED_LIST_FOR_EACH(access_list, bucket, msg)
        {
                if (MsgGetId(msg) == id)
                        return msg;
        }

        return NULL;
}


void PrivateModuleAccessListInsertMsg(PrivateModule *prmod, Msg *msg)
{
        struct access_list_t *access_list = NULL;

        /* Get access list from access hash table */
        access_list = &prmod->access_hash_table[MsgGetId(msg) % MOD_ACCESS_HASH_TABLE_SIZE];

        /* Check: message must not be present in access list */
        assert(!DOUBLE_LINKED_LIST_MEMBER(access_list, bucket, msg));

        /* Insert message in access list */
        DOUBLE_LINKED_LIST_INSERT_TAIL(access_list, bucket, msg);
}


void PrivateModuleAccessListRemoveMsg(PrivateModule *prmod, Msg *msg)
{
        struct access_list_t *access_list = NULL;

        /* Get access list from access hash table */
        access_list = &prmod->access_hash_table[MsgGetId(msg) % MOD_ACCESS_HASH_TABLE_SIZE];

        /* Check: message must be present in access list */
        assert(DOUBLE_LINKED_LIST_MEMBER(access_list, bucket, msg));

        /* Insert message in access list */
        DOUBLE_LINKED_LIST_REMOVE(access_list, bucket, msg);
}


/* Returns local/remote L1 and L2 caches based on the request type */
void PrivateModuleGetCaches(PrivateModule *prmod, Msg *msg, PrivateCache **pl2cache, PrivateCache **pll1cache, PrivateCache **prl1cache)
{
        PTR_ASSIGN(pl2cache, prmod->caches[L2Cache]);
        if (MsgIsInstAccess(msg))
        {
                PTR_ASSIGN(pll1cache, prmod->caches[IL1Cache]);
                PTR_ASSIGN(prl1cache, prmod->caches[DL1Cache]);
        }
        else
        {
                PTR_ASSIGN(pll1cache, prmod->caches[DL1Cache]);
                PTR_ASSIGN(prl1cache, prmod->caches[IL1Cache]);
        }
}


/* Returns L1 and L2 TLBs based on request type */
void PrivateModuleGetTLBs(PrivateModule *prmod, Msg *msg, TLBCache **pl1tlb, TLBCache **pl2tlb)
{
        PTR_ASSIGN(pl2tlb, prmod->tlbs[L2TLB]);
        PTR_ASSIGN(pl1tlb, prmod->tlbs[MsgIsInstAccess(msg) ? IL1TLB : DL1TLB]);
}


struct port_t* PrivateModuleGetPort(PrivateModule *prmod, AccessType atype)
{
        struct port_t *port = NULL;

        switch (atype)
        {
                case InstFetch:
                        port = &prmod->ports[0];
                        break;

                case DataRead:
                case DataReadEx:
                        port = &prmod->ports[1];
                        break;

                case DataWrite:
                        if (prmod->ports[2].total) port = &prmod->ports[2];
                        else port = &prmod->ports[1];
                        break;

                default:
                        panic("%s: line no %d - should not reach here", __FUNCTION__, __LINE__);
                        return NULL;
        }

        /* Check: port occupancy */
        assert(port->cycle <= esim_cycle);

        return port;
}


void PrivateModuleClearPendingStore(PrivateModule *prmod, Msg *msg)
{
        /* Clear pending store from module if the message is
         * same as the pending store. */
        if (prmod->pending_store == msg)
                prmod->pending_store = NULL;
}


void PrivateModuleSetPendingStore(PrivateModule *prmod, Msg *msg)
{
        /* If message is already the pending store then
         * there is nothing more to do. */
        if (prmod->pending_store == msg) return;

        /* Check: pending store must be empty */
        assert(!prmod->pending_store);

        /* Set pending store */
        prmod->pending_store = msg;
}


/* Free a MSHR which has completed servicing a miss request */
void PrivateModuleFreeMSHR(PrivateModule *prmod, PrivateMSHR *mshr)
{
        PrivateWB *wb = NULL;

        /* Free unused reserved WBs */
        while (PrivateWBListSize(mshr->reserved_wbs))
        {
                /* Get reserved WB */
                wb = PrivateWBListDequeue(mshr->reserved_wbs);

                /* Check: WB must not be occupied */
                assert(!PrivateWBIsOccupied(wb));

                /* Add WB to available WB list */
                PrivateWBListEnqueue(prmod->available_wbs, wb);
        }

        /* Wakeup messages waiting for miss request to complete */
        MsgListWakeupMsgs(mshr->msg_list);

        /* Wakeup messages waiting for a free MSHR */
        MsgListWakeupMsgs(prmod->msgs_waiting_for_free_mshr);

        /* Free MSHR */
        PrivateMSHRFree(mshr);

        /* Remove MSHR from occupied MSHR list */
        PrivateMSHRListRemove(prmod->occupied_mshrs, mshr);

        /* Add MSHR to available MSHR list */
        PrivateMSHRListEnqueue(prmod->available_mshrs, mshr);
}


PrivateMSHR* PrivateModuleFindMSHR(PrivateModule *prmod, long long tag)
{
        /* Locate MSHR with matching tag in occupied MSHR list */
        PrivateMSHR *mshr = PrivateMSHRListFind(prmod->occupied_mshrs, tag);
        if (!mshr) return NULL;

        /* Check: MSHR must be occupied */
        assert(PrivateMSHRIsOccupied(mshr));
        return mshr;
}


/* Free an WB which has completed servicing a writeback */
void PrivateModuleFreeWB(PrivateModule *prmod, PrivateWB *wb)
{
        /* Wakeup messages waiting for writeback to complete */
        MsgListWakeupMsgs(wb->msg_list);

        /* Wakeup messages waiting for a free WB */
        MsgListWakeupMsgs(prmod->msgs_waiting_for_free_wb);

        /* Free WB */
        PrivateWBFree(wb);

        /* Remove WB from occupied WB list */
        PrivateWBListRemove(prmod->occupied_wbs, wb);

        /* Add WB to available WB list */
        PrivateWBListEnqueue(prmod->available_wbs, wb);
}


PrivateWB* PrivateModuleFindWB(PrivateModule *prmod, long long tag)
{
        /* Locate WB with matching tag in occupied WB list */
        PrivateWB *wb = PrivateWBListFind(prmod->occupied_wbs, tag);
        if (!wb) return NULL;

        /* Check: WB must be occupied */
        assert(PrivateWBIsOccupied(wb));
        return wb;
}


unsigned int PrivateModuleGetRemoteCacheBlock(PrivateModule *prmod, PrivateCache *lcache, PrivateBlock *lblk, PrivateCache **prcache, PrivateBlock **prblk)
{
        /* Iterate over private caches */
        for (PrivateCacheType ptype = 0; ptype < NumPrivateCacheTypes; ptype++)
        {
                /* Skip if private cache is not being modeled or if
                 * private cache is same as the local cache. */
                PrivateCache *pcache = prmod->caches[ptype];
                if (!pcache || (pcache == lcache)) continue;

                /* Skip if block is not present in private cache */
                PrivateBlock *pblk = PrivateCacheFindBlock(pcache, PrivateBlockGetTag(lblk));
                if (!pblk) continue;

                /* Set remote cache and block return pointers */
                PTR_ASSIGN(prcache, pcache);
                PTR_ASSIGN(prblk, pblk);
                return 1;
        }
        return 0;
}


long long PrivateModuleAccess(PrivateModule *prmod, AccessType atype, long long address, unsigned int size, struct linked_list_t *list, struct uop_t *uop)
{
        /* Construct access */
        Access *access = new(Access, atype, address, size, list, uop);

        /* Check: a data write access can only be processed if there
         * are no pending store requests */
        assert(!AccessIsDataWrite(access) || !prmod->pending_store);

        /* Get port */
        struct port_t *port = PrivateModuleGetPort(prmod, AccessGetType(access));

        /* Reset port occupancy if port was last occupied in an older
         * cycle than the current cycle. */
        if (port->cycle != esim_cycle)
                port->occupied = 0;

        /* Check: number of occupied ports must be less than total
         * number of port. */
        assert(port->occupied < port->total);

        /* Update port occupancy */
        port->cycle = esim_cycle;
        port->occupied++;

        /* Create request message */
        Msg *request = LMsg(TlbReq, AccessGetAddress(access), asModule(prmod));
        MsgSetAccess(request, access);

        /* Insert request message in access hash table */
        PrivateModuleAccessListInsertMsg(prmod, request);

        /* Execute request message */
        ExecuteMsg(request);

        /* Return request message id */
        return MsgGetId(request);
}


unsigned int PrivateModuleCanAccess(PrivateModule *prmod, AccessType atype)
{
        /* Check: access type validity */
        assert(0 <= atype && atype < NumAccessTypes);

        /* A data write access can only be performed if there are
         * no pending store requests. */
        if ((atype == DataWrite) && prmod->pending_store)
                return 0;

        /* Get port */
        struct port_t *port = PrivateModuleGetPort(prmod, atype);

        /* If the last cycle in which port was occupied is same as current cycle
         * and the number of occupied ports is greater than or equal to total
         * number of ports available, then access cannot be performed. */
        if (port->cycle == esim_cycle && port->occupied >= port->total)
                return 0;

        return 1;
}


void PrivateModuleInvalidateAccess(PrivateModule *prmod, long long id)
{
        Msg *msg = PrivateModuleAccessListFindMsg(prmod, id);
        if (!msg) return;

        /* Clear access data for message */
        AccessClear(MsgGetAccess(msg));
}


unsigned int PrivateModuleInFlightAccess(PrivateModule *prmod, long long id)
{
        Msg *msg = PrivateModuleAccessListFindMsg(prmod, id);
        if (!msg) return 0;
        return 1;
}


struct uop_t* PrivateModuleGetPendingStoreUop(PrivateModule *prmod)
{
        Access *access = NULL;

        /* Return NULL, if there is no pending store request at private module */
        if (!prmod->pending_store) return NULL;

        /* Return NULL, if there is no pending store at */
        access = MsgGetAccess(prmod->pending_store);
        if (!access) return NULL;

        return access->uop;
}
