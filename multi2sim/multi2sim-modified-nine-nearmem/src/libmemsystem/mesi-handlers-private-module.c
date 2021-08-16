#include <libstruct/misc.h>
#include <libstruct/debug.h>
#include <libmhandle/mhandle.h>
#include <libcpuarch/cpuarch.h>

#include "mem-system.h"
#include "message-list.h"
#include "message-manager.h"

#include "private-wb.h"
#include "private-mshr.h"
#include "private-wb-list.h"
#include "private-mshr-list.h"

#include "tlb-cache.h"
#include "private-cache.h"
#include "mesi-handlers-private-module.h"


/*
 * Private functions and variables
 */

static void PrivateModuleMsgWaitForBuffer(PrivateModule *prmod, Msg *msg, ReqWaitType type)
{
        prmod->stats.wait[MsgGetAccessType(msg)][type].total++;
        if (!MsgWasWaitingForBuffer(msg))
        {
                MsgSetWaitingForBuffer(msg);
                prmod->stats.wait[MsgGetAccessType(msg)][type].unique++;
        }
}


static TLBBlock* PrivateModuleVictimizeTLB(PrivateModule *prmod, TLBCache *tlb, Msg *msg)
{
        TLBBlock *tblk = TLBCacheGetReplacementBlock(tlb, MsgGetTag(msg));
        if (TLBBlockIsValid(tblk)) TLBCacheEvictBlock(tlb, tblk);
        return tblk;
}


static inline void PrivateModuleUpdateBlockAccessStat(PrivateModule *prmod, PrivateBlock **pblks)
{
        long long accesses = 0;
        for (PrivateCacheType index = 0; index < NumPrivateCacheTypes; index++)
        {
                if (!pblks || !pblks[index] || !PrivateBlockIsValid(pblks[index])) continue;
                accesses = MAX(PrivateBlockGetAccesses(pblks[index]), accesses);
                PrivateBlockClearAccesses(pblks[index]);
        }
        PrivateModuleUpdateAccessesHist(prmod, accesses);
}


static inline void PrivateModuleUpdateCoherenceStat(PrivateModule *prmod, Msg *request, PrivateBlock **pblks)
{
        PrivateCoherenceStat *stat = prmod->stats.cohrstats[MsgGetType(request)];
        unsigned int presenceindex = 0;

        /* Compute private cache block presence index */
        for (PrivateCacheType index = 0; index < NumPrivateCacheTypes; index++)
        {
                presenceindex <<= 1;
                if (!pblks || !pblks[index] || !PrivateBlockIsValid(pblks[index])) continue;
                presenceindex |= 1;
        }

        /* Check: validity of presence index */
        assert(0 <= presenceindex && presenceindex <= 5);

        /* Update coherence message statistics */
        stat->msgs[presenceindex]++;

        /* Update early coherence race wait statistics */
        if (MsgIsEarlyRaceWait(request))
        {
                stat->early.cycles += MsgGetRaceWaitCycles(request);
                stat->early.msgs++;
        }

        /* Update late coherence race wait statistics */
        if (MsgIsLateRaceWait(request))
        {
                stat->late.cycles += MsgGetRaceWaitCycles(request);
                stat->late.msgs++;
        }
}


static void IntrvReplyHelper(Module *mod, Msg *request, unsigned int present, unsigned int dirty, unsigned int experm, unsigned int latency)
{
        /* Determine intervention reply type */
        MsgType mtype = NumMsgTypes;
        switch (MsgGetType(request))
        {
                case Wb_ShIntrvReq:
                        /* Check: block must be present in private module */
                        assert(present);
                        mtype = dirty ? DirtyData_Wb_ShIntrvRep : CleanData_Wb_ShIntrvRep;
                        break;

                case Wb_ExIntrvReq:
                        /* Check: block must be present in private module */
                        assert(present);
                        mtype = dirty ? DirtyData_Wb_ExIntrvRep : CleanData_Wb_ExIntrvRep;
                        break;

                case Opt_ShIntrvReq:
                        /* Check: block must be present in private module and must not be dirty */
                        assert(present && !dirty);
                        mtype = CleanData_Opt_ShIntrvRep;
                        break;

                case Opt_ExIntrvReq:
                        /* Check: block must be present in private module */
                        assert(present);
                        mtype = dirty ? DirtyData_Opt_ExIntrvRep : CleanData_Opt_ExIntrvRep;
                        break;

                case Norm_ShIntrvReq:
                        /* Check: block must be present in private module */
                        assert(present);
                        mtype = dirty ? DirtyData_Norm_ShIntrvRep : CleanData_Norm_ShIntrvRep;
                        break;

                case Norm_ExIntrvReq:
                        /* Check: block must be present in private module */
                        assert(present);
                        mtype = dirty ? DirtyData_Norm_ExIntrvRep : CleanData_Norm_ExIntrvRep;
                        break;

                case ShCorruptWb_ShIntrvReq:
                        /* Check: block must be present in private module */
                        assert(present);
                        mtype = dirty ? DirtyData_ShCorruptWb_ShIntrvRep : CleanData_ShCorruptWb_ShIntrvRep;
                        break;

                case OwCorruptWb_ExIntrvReq:
                        /* Check: block must be present in private module */
                        assert(present);
                        mtype = dirty ? DirtyData_OwCorruptWb_ExIntrvRep : CleanData_OwCorruptWb_ExIntrvRep;
                        break;

                case ShCorruptWb_ExIntrvReq:
                        /* Check: block must be present in private module */
                        assert(present);
                        mtype = dirty ? DirtyData_ShCorruptWb_ExIntrvRep : CleanData_ShCorruptWb_ExIntrvRep;
                        break;

                case OwCorruptMove_ShIntrvReq:
                        /* Check: block must be present in private module */
                        assert(present);
                        mtype = dirty ? DirtyData_OwCorruptMove_ShIntrvRep : CleanData_OwCorruptMove_ShIntrvRep;
                        break;

                case ShCorruptMove_ShIntrvReq:
                        /* Check: block must be present in private module */
                        assert(present);
                        mtype = dirty ? DirtyData_ShCorruptMove_ShIntrvRep : CleanData_ShCorruptMove_ShIntrvRep;
                        break;

                case OwCorruptMove_ExIntrvReq:
                        /* Check: block must be present in private module */
                        assert(present);
                        mtype = dirty ? DirtyData_OwCorruptMove_ExIntrvRep : CleanData_OwCorruptMove_ExIntrvRep;
                        break;

                case ShCorruptMove_ExIntrvReq:
                        /* Check: block must be present in private module */
                        assert(present);
                        mtype = dirty ? DirtyData_ShCorruptMove_ExIntrvRep : CleanData_ShCorruptMove_ExIntrvRep;
                        break;

                case Brdcst_ShIntrvReq:
                        mtype = present ? (dirty ? DirtyData_Brdcst_ShIntrvRep : CleanData_Brdcst_ShIntrvRep) : NoData_Brdcst_ShIntrvRep;
                        break;

                case Brdcst_ExIntrvReq:
                        mtype = present ? (dirty ? DirtyData_Brdcst_ExIntrvRep : CleanData_Brdcst_ExIntrvRep) : NoData_Brdcst_ExIntrvRep;
                        break;

                default:
                        panic("%s:%d invalid intervention request type", __FILE__, __LINE__);
                        break;
        }

        /* Send intervention reply to requestor after latency cycles */
        Msg *reply = RMsgWithId(MsgGetId(request), mtype, MsgGetTag(request), mod, request->requestor);
        ScheduleMsg(reply, latency);

        /* Set "experm" flag for intervention reply */
        if (experm) MsgSetExPerm(reply);
}


static void FwdReplyHelper(Module *mod, Msg *request, unsigned int latency)
{
        /* Determine type of forward clear and reply messages */
        MsgType htype = NumMsgTypes;
        MsgType rtype = NumMsgTypes;
        switch (MsgGetType(request))
        {
                /* Shared forward requests */
                case Wb_ShFwdReq:
                        htype = CleanData_ShFwdClr;
                        rtype = CleanData_Wb_ShFwdRep;
                        break;

                case Norm_ShFwdReq:
                        htype = NoData_ShFwdClr;
                        rtype = CleanData_Norm_ShFwdRep;
                        break;

                case ShCorruptWb_ShFwdReq:
                        htype = NoData_ShFwdClr;
                        rtype = CleanData_ShCorruptWb_ShFwdRep;
                        break;

                case OwCorruptMove_ShFwdReq:
                        htype = OwPartData_ShFwdClr;
                        rtype = CleanData_OwCorruptMove_ShFwdRep;
                        break;

                case ShCorruptMove_ShFwdReq:
                        htype = ShPartData_ShFwdClr;
                        rtype = CleanData_ShCorruptMove_ShFwdRep;
                        break;

                /* Exclusive forward requests */
                case Wb_ExFwdReq:
                        htype = CleanData_ExFwdClr;
                        rtype = CleanData_Wb_ExFwdRep;
                        break;

                case Norm_ExFwdReq:
                        htype = NoData_ExFwdClr;
                        rtype = CleanData_Norm_ExFwdRep;
                        break;

                case OwCorruptWb_ExFwdReq:
                        htype = NoData_ExFwdClr;
                        rtype = CleanData_OwCorruptWb_ExFwdRep;
                        break;

                case ShCorruptWb_ExFwdReq:
                        htype = NoData_ExFwdClr;
                        rtype = CleanData_ShCorruptWb_ExFwdRep;
                        break;

                case OwCorruptMove_ExFwdReq:
                        htype = OwPartData_ExFwdClr;
                        rtype = CleanData_OwCorruptMove_ExFwdRep;
                        break;

                case ShCorruptMove_ExFwdReq:
                        htype = ShPartData_ExFwdClr;
                        rtype = CleanData_ShCorruptMove_ExFwdRep;
                        break;

                default:
                        panic("%s:%d invalid forward request type", __FILE__, __LINE__);
                        break;
        }

        /* Send forward clear message to home module */
        Msg *hreply = RMsgWithId(MsgGetId(request), htype, MsgGetTag(request), mod, request->sender);
        ScheduleMsg(hreply, latency);

        /* Send forward reply message to requestor module */
        Msg *rreply = RMsgWithId(MsgGetId(request), rtype, MsgGetTag(request), mod, request->requestor);
        MsgSetReplies(rreply, MsgGetReplies(request));
        ScheduleMsg(rreply, latency);
}


/* Victimize block from private module cache. If block can be successfully victimized
 * from cache returns the pointer to block, else returns NULL. */
static PrivateBlock* PrivateModuleVictimizeCache(PrivateModule *prmod, PrivateCache *lcache, Msg *msg, PrivateWBList *wbs, int latency)
{
        PrivateCache *rcache = NULL;
        PrivateBlock *rblk = NULL;

        /* Get cache replacement block. If replacement candidate is an invalid
         * block then victimizing block is not required. */
        PrivateBlock *lblk = PrivateCacheGetReplacementBlock(lcache, MsgGetTag(msg));
        if (!PrivateBlockIsValid(lblk))
                return lblk;

        /* Replacement candidate is a valid block - victimize block */
        long long tag = PrivateBlockGetTag(lblk);

        /* Check: "experm" flag must not be set for shared cache blocks */
        assert(!PrivateBlockIsShared(lblk) || !PrivateBlockIsExPermSet(lblk));

        /* Get remote cache block */
        if (PrivateModuleGetRemoteCacheBlock(prmod, lcache, lblk, &rcache, &rblk))
        {
                /* Set "experm" flag for remote cache block, if flag is set for local cache block */
                if (PrivateBlockIsExPermSet(lblk)) PrivateBlockSetExPerm(rblk);

                /* Set access in remote cache block */
                if (PrivateBlockGetAccesses(rblk) < PrivateBlockGetAccesses(lblk))
                        PrivateBlockSetAccesses(rblk, PrivateBlockGetAccesses(lblk));

                /* Update remote cache block state */
                if (PrivateBlocksAreModified(lblk, rblk)) PrivateBlockSetModified(rblk);
                else if (PrivateBlocksAreExclusive(lblk, rblk)) PrivateBlockSetExclusive(rblk);

                /* Invalidate local cache block */
                PrivateCacheInvalidateBlock(lcache, lblk, PrivateCacheEviction);
		PrivateBlockResetPrefetched(lblk);
                return lblk;
        }

        /* If there is a pending request for the block then writeback is not
         * required and block can directly be invalidated from cache. */
        PrivateMSHR *mshr = PrivateModuleFindMSHR(prmod, tag);
        if (mshr)
        {
                /* Check: block must be in shared state and MSHR must indicate
                 * the presence of block in private module caches. */
                assert(PrivateBlockIsShared(lblk) && PrivateMSHRModuleHasBlock(mshr));

                /* Update accesses hsitogram */
                PrivateModuleUpdateAccessesHist(prmod, PrivateBlockGetAccesses(lblk));

                /* Invalidate local cache block */
                PrivateCacheInvalidateBlock(lcache, lblk, PrivateCacheEviction);
		PrivateBlockResetPrefetched(lblk);
                return lblk;
        }

        /* If silent clean evictions are enabled and block is in shared state, then
         * writeback is not required and block can directly be evicted from cache. */
        if (silent_clean_evictions && PrivateBlockIsShared(lblk))
        {
                /* Notify CPU that block is being evicted from private module.
                 * This is required for enforcing memory consistency. */
                cpu_invalidate(prmod->core, prmod->thread, tag);

                /* Update accesses hsitogram */
                PrivateModuleUpdateAccessesHist(prmod, PrivateBlockGetAccesses(lblk));

                /* Invalidate local cache block */
                PrivateCacheInvalidateBlock(lcache, lblk, PrivateCacheEviction);
		PrivateBlockResetPrefetched(lblk);
                return lblk;
        }

        /* Get a WB for servicing writeback, if a WB cannot be obtained
         * then block cannot be victimized from cache. */
        PrivateWB *wb = PrivateWBListDequeue(wbs);
        if (!wb) return NULL;

        /* Notify CPU that block is being evicted from private module.
         * This is required for enforcing memory consistency. */
        cpu_invalidate(prmod->core, prmod->thread, tag);

        /* Determine type of writeback message */
        MsgType mtype = NumMsgTypes;
        switch (PrivateBlockGetState(lblk))
        {
                case PrivateBlockShared:
                        mtype = NoData_ShWbReq;
                        prmod->stats.writeback.shared++;
                        break;

                case PrivateBlockExclusive:
                        mtype = exwbreq;
                        break;

                case PrivateBlockModified:
                        mtype = DirtyData_ExWbReq;
                        prmod->stats.writeback.dirty++;
                        break;

                default:
                        panic("%s:%d invalid private block state", __FILE__, __LINE__);
                        break;
        }
        prmod->stats.writeback.count++;

        /* Send writeback request to lower module after latency cycles */
        Msg *writeback = RMsg(mtype, tag, asModule(prmod), ModuleGetLowModule(asModule(prmod), tag));
        ScheduleMsg(writeback, latency);

        /* Set "experm" flag for writeback request, if flag is set for cache block */
        if (PrivateBlockIsExPermSet(lblk)) MsgSetExPerm(writeback);

        /* Occupy WB for servicing writeback */
        PrivateWBOccupy(wb, writeback);
        PrivateWBListEnqueue(prmod->occupied_wbs, wb);

        /* Update accesses hsitogram */
        PrivateModuleUpdateAccessesHist(prmod, PrivateBlockGetAccesses(lblk));

        /* Invalidate local cache block */
        PrivateCacheInvalidateBlock(lcache, lblk, PrivateCacheEviction);
	PrivateBlockResetPrefetched(lblk);

        return lblk;
}


static void PrivateModuleExclusiveFill(PrivateModule *prmod, PrivateMSHR *mshr)
{
        PrivateBlock *pblks[NumPrivateCacheTypes] = { NULL };
        PrivateCache **pcaches = prmod->caches;

        Msg *msg = PrivateMSHRGetMissReq(mshr);
        Access *access = MsgGetAccess(msg);
        Module *mod = asModule(prmod);
	AccessType atype = AccessGetType(access);

        long long tag = MsgGetTag(msg);
        unsigned int invalidated = 0;

        unsigned int experm = 0;
        unsigned int dirty = 0;

        /* Check: MSHR must have received exclusive reply */
        assert(PrivateMSHRRecvdExReply(mshr));

        /* Update module access statistics */
        prmod->stats.block.accesses[AccessGetType(access)]++;

        /* Check: MSHR must not have received both intervention and invalidation replies */
        assert(!(PrivateMSHRRecvdIntrvReplies(mshr) && PrivateMSHRRecvdInvalReplies(mshr)));

        /* When optimized interventions are disabled and MSHR received intervention replies */
        if (!intrvs_opt && PrivateMSHRRecvdIntrvReplies(mshr))
        {
                /* Determine type of intervention clear message */
                MsgType mtype = NoData_ExIntrvClr;
                if (PrivateMSHRRecvdWbIntrvReply(mshr))
                        mtype = CleanData_ExIntrvClr;

                /* Sender intervention clear messages to home module */
                Msg *reply = RMsgWithId(MsgGetId(msg), mtype, tag, mod, ModuleGetLowModule(mod, tag));
                ScheduleMsg(reply, 0);

                /* When MSHR received an intervention reply for which "experm" flag
                 * was set, set "experm" flag in intervention clear message. */
                if (PrivateMSHRRecvdExPermIntrvReply(mshr)) MsgSetExPerm(reply);
        }

        /* When optimized invalidations are disabled and MSHR received invalidation replies */
        if (!invals_opt && PrivateMSHRRecvdInvalReplies(mshr))
        {
                /* Send invaldiation clear message to home module immediately */
                Msg *reply = RMsgWithId(MsgGetId(msg), InvalClr, tag, mod, ModuleGetLowModule(mod, tag));
                ScheduleMsg(reply, 0);
        }

        /* Locate block in module caches */
        for (PrivateCacheType index = 0; index < NumPrivateCacheTypes; index++)
        {
                /* Skip if cache is not being modeled or if block is not present in cache */
                if (!pcaches[index]) continue;
                pblks[index] = PrivateCacheFindBlock(pcaches[index], tag);
                if (!pblks[index]) continue;

                /* Check: "experm" flag must not be set for cache
                 * block and it must be in shared state. */
                assert(!PrivateBlockIsExPermSet(pblks[index]));
                assert(PrivateBlockIsShared(pblks[index]));

                /* Check: MSHR must indicate that module contains the block */
                assert(PrivateMSHRModuleHasBlock(mshr));
        }

        /* Update block access histogram, as at each fill a new lifetime of block begins */
        PrivateModuleUpdateBlockAccessStat(prmod, pblks);

        /* Handle pending early invalidation race request */
        if (PrivateMSHRNumInvalRaceReqs(mshr))
        {
                while (PrivateMSHRNumInvalRaceReqs(mshr))
                {
                        /* Get pending early invalidation race request */
                        Msg *erequest = PrivateMSHRGetInvalRaceReq(mshr);
                        assert(erequest);

                        /* When invalidations have to wait for pending miss requests */
                        if (invals_wait_for_pending_miss_requests)
                        {
                                /* Send invalidation reply to requestor module immediately */
                                Msg *reply = RMsgWithId(MsgGetId(erequest), InvalRep, tag, mod, erequest->requestor);
                                ScheduleMsg(reply, 0);
                        }

                        /* Update coherence statistics and free request */
                        PrivateModuleUpdateCoherenceStat(prmod, erequest, pblks);
                        delete(erequest);
                }

                /* Invalidate block from module caches */
                for (PrivateCacheType index = 0; index < NumPrivateCacheTypes; index++)
                {
                        if (pblks[index]) {
				PrivateCacheInvalidateBlock(pcaches[index], pblks[index], CoherenceInvalidation);
				PrivateBlockResetPrefetched(pblks[index]);
			}
                        pblks[index] = NULL;
                }

                /* Update in MSHR that block is not present in module.
                 * Also set the "invalidated" flag. */
                if (PrivateMSHRModuleHasBlock(mshr))
                {
                        PrivateMSHRClearModuleHasBlock(mshr);
                        invalidated = 1;
                }
        }

        /* Invalidate block from remote L1 cache */
        PrivateCacheType rindex = AccessIsInstFetch(access) ? DL1Cache : IL1Cache;
        if (pblks[rindex])
        {
                PrivateCacheInvalidateBlock(pcaches[rindex], pblks[rindex], RemoteCacheInvalidation);
		PrivateBlockResetPrefetched(pblks[rindex]);
                pblks[rindex] = NULL;
        }

        /* When L2 cache is being modeled */
        if (pcaches[L2Cache])
        {
                /* When block is not present in L2 cache, fill block in L2 cache,
                 * esle update L2 cache block to exclusive state. */
                if (!pblks[L2Cache])
                {
                        pblks[L2Cache] = PrivateModuleVictimizeCache(prmod, pcaches[L2Cache], msg, mshr->reserved_wbs, 0);
                        assert(pblks[L2Cache]);
                        PrivateCacheFillBlock(pcaches[L2Cache], pblks[L2Cache], tag, PrivateBlockExclusive, AccessGetEIP(access));
			if ((atype == PrefetchInst) || (atype == PrefetchData)) {
                        	PrivateBlockSetPrefetched(pblks[L2Cache]);
                        }
                }
                else PrivateBlockSetExclusive(pblks[L2Cache]);

                /* Update L2 cache recency stack */
                PrivateCacheAccessBlock(pcaches[L2Cache], pblks[L2Cache], AccessGetEIP(access));
        }

        /* Get index of local L1 cache */
        PrivateCacheType lindex = AccessIsInstFetch(access) ? IL1Cache : DL1Cache;

        /* Fill block in local L1 cache */
        if (!pblks[lindex])
        {
		if ((atype != PrefetchInst) && (atype != PrefetchData)) {
                	pblks[lindex] = PrivateModuleVictimizeCache(prmod, pcaches[lindex], msg, mshr->reserved_wbs, 0);
                	assert(pblks[lindex]);
                	PrivateCacheFillBlock(pcaches[lindex], pblks[lindex], tag, PrivateBlockExclusive, AccessGetEIP(access));
		}
        }

        /* If dirty reply was received or if access is a write access update local L1 cache block
         * to modified state, else update local L1 cache block state to exclusive state. */
        if ((PrivateMSHRRecvdDirtyReply(mshr) || AccessIsDataWrite(access)) && pblks[lindex])
        {
                PrivateBlockSetModified(pblks[lindex]);
                dirty = 1;
        }
        else if (pblks[lindex]) PrivateBlockSetExclusive(pblks[lindex]);

        /* Update local L1 cache recency stack */
        if (pblks[lindex]) PrivateCacheAccessBlock(pcaches[lindex], pblks[lindex], AccessGetEIP(access));

        /* Set "experm" flag for local L1 cache block, if access requires exclusive permissions */
        if (AccessIsExclusive(access) && pblks[lindex])
        {
                PrivateBlockSetExPerm(pblks[lindex]);
                experm = 1;
        }

        /* Update private block accesses */
        if (pblks[lindex]) PrivateBlockUpdateAccesses(pblks[lindex]);

        /* Note: MSHR can receive only one of early shared intervention, exclusive
         * intervention, excluisve back invalidation and shared back invalidation race
         * requests. This is asserted when early race requests are recorded in MSHR. */

        /* Check: MSHR must not have pending early shared back invalidation race request */
        assert(!PrivateMSHRGetShBkInvalRaceReq(mshr));

        /* Check: MSHR must not have pending early forward requests */
        assert(!PrivateMSHRGetFwdRaceReq(mshr));

        /* Handle pending early shared intervention race request */
        Msg *erequest = PrivateMSHRGetShIntrvRaceReq(mshr);
        if (erequest)
        {
                /* When optimized interventions are enabled */
                if (intrvs_opt)
                {
                        /* Check: request must not be broadcast intervention */
                        assert(!MsgIsBrdcstIntrvReq(erequest));

                        /* Determine type of intervention clear message */
                        MsgType mtype = NoData_ShIntrvClr;
                        if (dirty)
                        {
                                /* When message is optimized intervention request */
                                if (MsgIsOptIntrvReq(erequest)) mtype = DirtyNoData_ShIntrvClr;
                                else mtype = DirtyData_ShIntrvClr;
                                dirty = 0;
                        }
                        else if (MsgIsWbIntrvReq(erequest))  mtype = CleanData_ShIntrvClr;
                        else if (MsgIsShCorruptMoveIntrvReq(erequest)) mtype = ShPartData_ShIntrvClr;
                        else if (MsgIsOwCorruptMoveIntrvReq(erequest)) mtype = OwPartData_ShIntrvClr;

                        /* Send intervention clear message to sender module */
                        Msg *reply = RMsgWithId(MsgGetId(erequest), mtype, tag, mod, erequest->sender);
                        ScheduleMsg(reply, 0);

                        /* Set "experm" flag for intervention clear message */
                        if (experm) MsgSetExPerm(reply);
                        experm = 0;
                }

                /* Send intervention reply message */
                IntrvReplyHelper(mod, erequest, 1, dirty, experm, 0);

                /* Update coherence statistics and free request */
                PrivateModuleUpdateCoherenceStat(prmod, erequest, pblks);
                delete(erequest);

                /* Update block in module caches to shared state */
                for (PrivateCacheType index = 0; index < NumPrivateCacheTypes; index++)
                {
                        if (!pblks[index]) continue;
                        PrivateBlockSetShared(pblks[index]);
                        PrivateBlockClearExPerm(pblks[index]);
                }
        }

        /* When there are no pending early race requests which would remove block */
        if (!PrivateMSHRGetExIntrvRaceReq(mshr) && !PrivateMSHRGetExBkInvalRaceReq(mshr))
                goto REQUEST_FINISH;

        /* Handle pending early exclusive intervention race request */
        erequest = PrivateMSHRGetExIntrvRaceReq(mshr);
        if (erequest)
        {
                /* When optimized interventions are enabled */
                if (intrvs_opt)
                {
                        /* Check: request must not be broadcast intervention */
                        assert(!MsgIsBrdcstIntrvReq(erequest));

                        /* Determine type of intervention clear message */
                        MsgType mtype = NoData_ExIntrvClr;
                        if (MsgIsWbIntrvReq(erequest))
                        {
                                mtype = dirty ? DirtyData_ExIntrvClr : CleanData_ExIntrvClr;
                                dirty = 0;
                        }
                        else if (MsgIsCorruptMoveIntrvReq(erequest))
                        {
                                if (dirty) mtype = DirtyData_ExIntrvClr;
                                else if (MsgIsShCorruptMoveIntrvReq(erequest)) mtype = ShPartData_ExIntrvClr;
                                else mtype = OwPartData_ExIntrvClr;
                                dirty = 0;
                        }

                        /* Send intervention clear message to sender module */
                        Msg *reply = RMsgWithId(MsgGetId(erequest), mtype, tag, mod, erequest->sender);
                        ScheduleMsg(reply, 0);

                        /* Set "experm" flag for intervention clear message */
                        if (experm) MsgSetExPerm(reply);
                        experm = 0;
                }

                /* Send intervention reply message */
                IntrvReplyHelper(mod, erequest, 1, dirty, experm, 0);

                /* Update coherence statistics and free request */
                PrivateModuleUpdateCoherenceStat(prmod, erequest, pblks);
                delete(erequest);
        }

        /* Handle pending early exclusive back invalidation race request */
        erequest = PrivateMSHRGetExBkInvalRaceReq(mshr);
        if (erequest)
        {
                /* Determine type of back invalidation reply */
                MsgType mtype = NoData_ExBkInvalRep;
                if (MsgIsCorruptWbBkInvalReq(erequest))
                        mtype = (MsgIsShCorruptWbBkInvalReq(erequest)) ? ShPartData_ExBkInvalRep : OwPartData_ExBkInvalRep;
                if (dirty) mtype = DirtyData_ExBkInvalRep;

                /* Send back invalidation reply message to sender module */
                Msg *reply = RMsgWithId(MsgGetId(erequest), mtype, tag, mod, erequest->sender);
                ScheduleMsg(reply, 0);

                /* Set "experm" flag for back invalidation reply, if "experm"
                 * flag is set for private module cache blocks. */
                if (experm) MsgSetExPerm(reply);

                /* Update coherence statistics and free request */
                PrivateModuleUpdateCoherenceStat(prmod, erequest, pblks);
                delete(erequest);
        }

        /* Update block access histogram */
        PrivateModuleUpdateBlockAccessStat(prmod, pblks);

        /* Invalidate block from module caches and set the "invalidated" flag */
        for (PrivateCacheType index = 0; index < NumPrivateCacheTypes; index++) {
                if (pblks[index]) {
			PrivateCacheInvalidateBlock(pcaches[index], pblks[index], CoherenceInvalidation);
			PrivateBlockResetPrefetched(pblks[index]);
		}
	}
        invalidated = 1;

REQUEST_FINISH:

        /* When block has been invalidated from all private module caches,
         * notify CPU about block being invalidated. This is required for
         * enforcing memory consistency. */
        if (invalidated) cpu_invalidate(prmod->core, prmod->thread, tag);

        /* Schedule CORE_REP message - reuse the parent message */
	if ((atype == PrefetchInst) || (atype == PrefetchData)) msg->type = PrefetchRep;
        else msg->type = CoreRep;
        ScheduleMsg(msg, 0);
}


static void PrivateModuleSharedFill(PrivateModule *prmod, PrivateMSHR *mshr)
{
        PrivateBlock *pblks[NumPrivateCacheTypes] = { NULL };
        PrivateCache **pcaches = prmod->caches;
        Module *mod = asModule(prmod);

        Msg *msg = PrivateMSHRGetMissReq(mshr);
        Access *access = MsgGetAccess(msg);
        long long tag = MsgGetTag(msg);
	AccessType atype = AccessGetType(access);

        /* Check: MSHR must have received shared reply and miss request
         * must be a shared request. */
        assert(PrivateMSHRRecvdShReply(mshr) && AccessIsShared(access));

        /* Update module access statistics */
        prmod->stats.block.accesses[AccessGetType(access)]++;

        /* When optimized interventions are disabled and MSHR received intervention replies */
        if (!intrvs_opt && PrivateMSHRRecvdIntrvReplies(mshr))
        {
                /* Determine type of intervention clear message */
                MsgType mtype = NoData_ShIntrvClr;
                if (PrivateMSHRRecvdDirtyReply(mshr))
                {
                        mtype = DirtyData_ShIntrvClr;
                        PrivateMSHRClearDirtyReply(mshr);
                }
                else if (PrivateMSHRRecvdWbIntrvReply(mshr)) mtype = CleanData_ShIntrvClr;

                /* Send intervention clear message to home module */
                Msg *reply = RMsgWithId(MsgGetId(msg), mtype, tag, mod, ModuleGetLowModule(mod, tag));
                ScheduleMsg(reply, 0);

                /* When MSHR received an intervention reply in which "experm" flag
                 * was set, set "experm" flag in intervention clear message. */
                if (PrivateMSHRRecvdExPermIntrvReply(mshr)) MsgSetExPerm(reply);
        }

        /* Check: must not have received dirty intervention replies or dirty
         * intervention replies must have been processed and cleared by now. */
        assert(!PrivateMSHRRecvdDirtyReply(mshr));

        /* Check: MSHR must not have received invalidation replies */
        assert(!PrivateMSHRRecvdInvalReplies(mshr));

        /* Check: block must not be present in module caches */
        for (PrivateCacheType index = 0; index < NumPrivateCacheTypes; index++)
                assert(!pcaches[index] || !PrivateCacheFindBlock(pcaches[index], tag));

        /* Check: MSHR must indicate that block is not present in module */
        assert(!PrivateMSHRModuleHasBlock(mshr));

        /* Note: MSHR can only receive one of early shared/exclusive forward, invalidation
         * or shared back invalidation race request. */

        /* Check: MSHR must not have pending early intervention race requests */
        assert(!PrivateMSHRGetIntrvRaceReq(mshr));

        /* Check: MSHR must not have pending early exclusive back invalidation race request */
        assert(!PrivateMSHRGetExBkInvalRaceReq(mshr));

        /* When invalidations are the only pending early race requests, the block is not filled in
         * private caches, and the pending early race requests are directly processed. This avoids
         * sending unnecessary writebacks to LLC and creating holes in private caches. */
        if (PrivateMSHRNumInvalRaceReqs(mshr) && !PrivateMSHRGetShBkInvalRaceReq(mshr))
        {
                /* Check: MSHR must not have pending early forward race request */
                assert(!PrivateMSHRGetFwdRaceReq(mshr));

                /* Handle pending early invalidation race request */
                while (PrivateMSHRNumInvalRaceReqs(mshr))
                {
                        /* Get pending early invalidation race request */
                        Msg *erequest = PrivateMSHRGetInvalRaceReq(mshr);
                        assert(erequest);

                        /* When invalidations must wait for pending miss requests */
                        if (invals_wait_for_pending_miss_requests)
                        {
                                /* Send invalidation reply message to requestor module */
                                Msg *reply = RMsgWithId(MsgGetId(erequest), InvalRep, tag, mod, erequest->requestor);
                                ScheduleMsg(reply, 0);
                        }

                        /* Update coherence statistics and free request */
                        PrivateModuleUpdateCoherenceStat(prmod, erequest, pblks);
                        delete(erequest);
                }

                /* When silent clean evictions are not enabled */
                if (!silent_clean_evictions)
                {
                        /* NOTE: A writeback to LLC must be performed to indicate that the private
                         * cache has evicted the block and that it is not longer a sharer of the
                         * block. This is required to maintain accurate sharer representation in
                         * directory for coherence protocols that reply on shared forwarders. */

                        /* Get a WB from the reserved WBs for servicing writeback */
                        PrivateWB *wb = PrivateWBListDequeue(mshr->reserved_wbs);
                        assert(wb); // MSHR must have atleat one reserved WB

                        /* Send writeback request to lower module after latency cycles */
                        Msg *writeback = RMsg(NoData_ShWbReq, tag, asModule(prmod), ModuleGetLowModule(asModule(prmod), tag));
                        ScheduleMsg(writeback, 0);

                        /* Occupy WB for servicing writeback */
                        PrivateWBOccupy(wb, writeback);
                        PrivateWBListEnqueue(prmod->occupied_wbs, wb);

                        /* Update writeback statistics */
                        prmod->stats.writeback.count++;
                        prmod->stats.writeback.shared++;
                }

                /* Notify CPU that block has been invalidated from private module.
                 * This is required for enforcing memory consistency. */
                cpu_invalidate(prmod->core, prmod->thread, tag);

                /* Schedule CORE_REP message - reuse the parent message */
		if ((atype == PrefetchInst) || (atype == PrefetchData)) msg->type = PrefetchRep;
                else msg->type = CoreRep;
                ScheduleMsg(msg, 0);
                return;
        }

        /* When L2 cache is being modeled */
        if (pcaches[L2Cache])
        {
                /* Fill block in L2 cache */
                pblks[L2Cache] = PrivateModuleVictimizeCache(prmod, pcaches[L2Cache], msg, mshr->reserved_wbs, 0);
                assert(pblks[L2Cache]);
                PrivateCacheFillBlock(pcaches[L2Cache], pblks[L2Cache], tag, PrivateBlockShared, AccessGetEIP(access));
		if ((atype == PrefetchInst) || (atype == PrefetchData)) {
                	PrivateBlockSetPrefetched(pblks[L2Cache]);
                }

                /* Update L2 cache recency counters */
                PrivateCacheAccessBlock(pcaches[L2Cache], pblks[L2Cache], AccessGetEIP(access));
        }

        /* Get index of local L1 cache */
        PrivateCacheType lindex = AccessIsInstFetch(access) ? IL1Cache : DL1Cache;

	if ((atype != PrefetchInst) && (atype != PrefetchData)) {
        	/* Fill block in local L1 cache */
        	pblks[lindex] = PrivateModuleVictimizeCache(prmod, pcaches[lindex], msg, mshr->reserved_wbs, 0);
        	assert(pblks[lindex]);
        	PrivateCacheFillBlock(pcaches[lindex], pblks[lindex], tag, PrivateBlockShared, AccessGetEIP(access));

        	/* Update local L1 cache recency counters */
        	PrivateCacheAccessBlock(pcaches[lindex], pblks[lindex], AccessGetEIP(access));

        	/* Update private block accesses */
        	PrivateBlockUpdateAccesses(pblks[lindex]);
	}

        /* Handle pending early shared forward race request */
        Msg *erequest = PrivateMSHRGetShFwdRaceReq(mshr);
        if (erequest)
        {
                /* Send forward reply message */
                FwdReplyHelper(mod, erequest, 0);

                /* Update coherence statistics and free request */
                PrivateModuleUpdateCoherenceStat(prmod, erequest, pblks);
                delete(erequest);
        }

        /* When there are no pending early race requests which would remove block */
        if (!PrivateMSHRGetExFwdRaceReq(mshr) && !PrivateMSHRGetShBkInvalRaceReq(mshr) && !PrivateMSHRNumInvalRaceReqs(mshr))
                goto REQUEST_FINISH;

        /* Handle pending early exclusive forward race request */
        erequest = PrivateMSHRGetExFwdRaceReq(mshr);
        if (erequest)
        {
                /* Send forward reply message */
                FwdReplyHelper(mod, erequest, 0);

                /* Update coherence statistics and free request */
                PrivateModuleUpdateCoherenceStat(prmod, erequest, pblks);
                delete(erequest);
        }

        /* Handle pending early shared back invalidation race request */
        erequest = PrivateMSHRGetShBkInvalRaceReq(mshr);
        if (erequest)
        {
                /* Determine type of back invalidation reply */
                MsgType mtype = NoData_ShBkInvalRep;
                if (erequest->forwarder == mod)
                {
                        /* Check: request must be corrupt writeback request */
                        assert(MsgIsCorruptWbBkInvalReq(erequest));

                        /* Set back invalidation reply type */
                        mtype = (MsgIsShCorruptWbBkInvalReq(erequest)) ? ShPartData_ShBkInvalRep : OwPartData_ShBkInvalRep;
                }

                /* Send back invalidation reply message to sender module */
                Msg *reply = RMsgWithId(MsgGetId(erequest), mtype, tag, mod, erequest->sender);
                ScheduleMsg(reply, 0);

                /* Update coherence statistics and free request */
                PrivateModuleUpdateCoherenceStat(prmod, erequest, pblks);
                delete(erequest);
        }

        /* Handle pending early invalidation race request */
        while (PrivateMSHRNumInvalRaceReqs(mshr))
        {
                /* Get pending early invalidation race request */
                erequest = PrivateMSHRGetInvalRaceReq(mshr);
                assert(erequest);

                /* When invalidations must wait for pending miss requests */
                if (invals_wait_for_pending_miss_requests)
                {
                        /* Send invalidation reply message to requestor module */
                        Msg *reply = RMsgWithId(MsgGetId(erequest), InvalRep, tag, mod, erequest->requestor);
                        ScheduleMsg(reply, 0);
                }

                /* Update coherence statistics and free request */
                PrivateModuleUpdateCoherenceStat(prmod, erequest, pblks);
                delete(erequest);
        }

        /* Update block access histogram */
        PrivateModuleUpdateBlockAccessStat(prmod, pblks);

        /* Invalidate block from module caches and set the "invalidated" flag */
        for (PrivateCacheType index = 0; index < NumPrivateCacheTypes; index++)
                if (pblks[index]) {
			PrivateCacheInvalidateBlock(pcaches[index], pblks[index], CoherenceInvalidation);
			PrivateBlockResetPrefetched(pblks[index]);
		}

        /* Notify CPU that block has been invalidated from private module.
         * This is required for enforcing memory consistency. */
        cpu_invalidate(prmod->core, prmod->thread, tag);

REQUEST_FINISH:

        /* Schedule CORE_REP message - reuse the parent message */
	if ((atype == PrefetchInst) || (atype == PrefetchData)) msg->type = PrefetchRep;
        else msg->type = CoreRep;
        ScheduleMsg(msg, 0);
}


/*
 * Public functions and variables
 */


/* Register message handlers */
void PrivateModuleRegisterHandlers(Module *mod)
{
        ModuleRegisterHandler(mod, TlbReq, TlbReqHandler);
        ModuleRegisterHandler(mod, CoreReq, CoreReqHandler);
	ModuleRegisterHandler(mod, PrefetchReq, PrefetchReqHandler);
        ModuleRegisterHandler(mod, CoreRep, CoreRepHandler);
	ModuleRegisterHandler(mod, PrefetchRep, PrefetchRepHandler);

        ModuleRegisterHandler(mod, PteRep, PteRepHandler);
        ModuleRegisterHandler(mod, PteNack, PteNackHandler);

        /* Block reply handlers */
        ModuleRegisterHandler(mod, ShPutRep, BlkRepHandler);

        ModuleRegisterHandler(mod, NoData_Norm_ExPutRep, BlkRepHandler);
        ModuleRegisterHandler(mod, CleanData_Norm_ExPutRep, BlkRepHandler);

        ModuleRegisterHandler(mod, InvalRep, BlkRepHandler);

        ModuleRegisterHandler(mod, CleanData_Wb_ShFwdRep, BlkRepHandler);
        ModuleRegisterHandler(mod, CleanData_Norm_ShFwdRep, BlkRepHandler);
        ModuleRegisterHandler(mod, CleanData_ShCorruptWb_ShFwdRep, BlkRepHandler);
        ModuleRegisterHandler(mod, CleanData_OwCorruptMove_ShFwdRep, BlkRepHandler);
        ModuleRegisterHandler(mod, CleanData_ShCorruptMove_ShFwdRep, BlkRepHandler);

        ModuleRegisterHandler(mod, CleanData_Wb_ExFwdRep, BlkRepHandler);
        ModuleRegisterHandler(mod, CleanData_Norm_ExFwdRep, BlkRepHandler);
        ModuleRegisterHandler(mod, CleanData_OwCorruptWb_ExFwdRep, BlkRepHandler);
        ModuleRegisterHandler(mod, CleanData_ShCorruptWb_ExFwdRep, BlkRepHandler);
        ModuleRegisterHandler(mod, CleanData_OwCorruptMove_ExFwdRep, BlkRepHandler);
        ModuleRegisterHandler(mod, CleanData_ShCorruptMove_ExFwdRep, BlkRepHandler);

        ModuleRegisterHandler(mod, CleanData_Wb_ShIntrvRep, BlkRepHandler);
        ModuleRegisterHandler(mod, DirtyData_Wb_ShIntrvRep, BlkRepHandler);

        ModuleRegisterHandler(mod, CleanData_Wb_ExIntrvRep, BlkRepHandler);
        ModuleRegisterHandler(mod, DirtyData_Wb_ExIntrvRep, BlkRepHandler);

        ModuleRegisterHandler(mod, CleanData_Opt_ShIntrvRep, BlkRepHandler);

        ModuleRegisterHandler(mod, CleanData_Opt_ExIntrvRep, BlkRepHandler);
        ModuleRegisterHandler(mod, DirtyData_Opt_ExIntrvRep, BlkRepHandler);

        ModuleRegisterHandler(mod, CleanData_Norm_ShIntrvRep, BlkRepHandler);
        ModuleRegisterHandler(mod, DirtyData_Norm_ShIntrvRep, BlkRepHandler);

        ModuleRegisterHandler(mod, CleanData_Norm_ExIntrvRep, BlkRepHandler);
        ModuleRegisterHandler(mod, DirtyData_Norm_ExIntrvRep, BlkRepHandler);

        ModuleRegisterHandler(mod, NoData_Brdcst_ShIntrvRep, BlkRepHandler);
        ModuleRegisterHandler(mod, CleanData_Brdcst_ShIntrvRep, BlkRepHandler);
        ModuleRegisterHandler(mod, DirtyData_Brdcst_ShIntrvRep, BlkRepHandler);

        ModuleRegisterHandler(mod, NoData_Brdcst_ExIntrvRep, BlkRepHandler);
        ModuleRegisterHandler(mod, CleanData_Brdcst_ExIntrvRep, BlkRepHandler);
        ModuleRegisterHandler(mod, DirtyData_Brdcst_ExIntrvRep, BlkRepHandler);

        ModuleRegisterHandler(mod, CleanData_ShCorruptWb_ShIntrvRep, BlkRepHandler);
        ModuleRegisterHandler(mod, DirtyData_ShCorruptWb_ShIntrvRep, BlkRepHandler);

        ModuleRegisterHandler(mod, CleanData_OwCorruptMove_ShIntrvRep, BlkRepHandler);
        ModuleRegisterHandler(mod, DirtyData_OwCorruptMove_ShIntrvRep, BlkRepHandler);

        ModuleRegisterHandler(mod, CleanData_ShCorruptMove_ShIntrvRep, BlkRepHandler);
        ModuleRegisterHandler(mod, DirtyData_ShCorruptMove_ShIntrvRep, BlkRepHandler);

        ModuleRegisterHandler(mod, CleanData_OwCorruptWb_ExIntrvRep, BlkRepHandler);
        ModuleRegisterHandler(mod, DirtyData_OwCorruptWb_ExIntrvRep, BlkRepHandler);

        ModuleRegisterHandler(mod, CleanData_ShCorruptWb_ExIntrvRep, BlkRepHandler);
        ModuleRegisterHandler(mod, DirtyData_ShCorruptWb_ExIntrvRep, BlkRepHandler);

        ModuleRegisterHandler(mod, CleanData_OwCorruptMove_ExIntrvRep, BlkRepHandler);
        ModuleRegisterHandler(mod, DirtyData_OwCorruptMove_ExIntrvRep, BlkRepHandler);

        ModuleRegisterHandler(mod, CleanData_ShCorruptMove_ExIntrvRep, BlkRepHandler);
        ModuleRegisterHandler(mod, DirtyData_ShCorruptMove_ExIntrvRep, BlkRepHandler);

        /* Block request nack handlers */
        ModuleRegisterHandler(mod, BlkNack, BlkNackHandler);

        /* Writeback reply handlers */
        ModuleRegisterHandler(mod, WbNormRep, WbRepHandler);
        ModuleRegisterHandler(mod, WbRaceRep, WbRepHandler);

        ModuleRegisterHandler(mod, Norm_WbNackRep, NormWbNackHandler);
        ModuleRegisterHandler(mod, BlockData_WbNackRep, DataWbNackHandler);
        ModuleRegisterHandler(mod, OwPartData_WbNackRep, DataWbNackHandler);
        ModuleRegisterHandler(mod, ShPartData_WbNackRep, DataWbNackHandler);

        /* Invalidation request handlers */
        ModuleRegisterHandler(mod, Norm_InvalReq, InvalReqHandler);
        ModuleRegisterHandler(mod, ExFwd_InvalReq, InvalReqHandler);
        ModuleRegisterHandler(mod, Brdcst_InvalReq, InvalReqHandler);

        /* Shared intervention request handlers */
        ModuleRegisterHandler(mod, Wb_ShIntrvReq, ShIntrvReqHandler);
        ModuleRegisterHandler(mod, Opt_ShIntrvReq, ShIntrvReqHandler);
        ModuleRegisterHandler(mod, Norm_ShIntrvReq, ShIntrvReqHandler);
        ModuleRegisterHandler(mod, Brdcst_ShIntrvReq, ShIntrvReqHandler);
        ModuleRegisterHandler(mod, ShCorruptWb_ShIntrvReq, ShIntrvReqHandler);
        ModuleRegisterHandler(mod, OwCorruptMove_ShIntrvReq, ShIntrvReqHandler);
        ModuleRegisterHandler(mod, ShCorruptMove_ShIntrvReq, ShIntrvReqHandler);

        /* Exclusive intervention request handlers */
        ModuleRegisterHandler(mod, Wb_ExIntrvReq, ExIntrvReqHandler);
        ModuleRegisterHandler(mod, Opt_ExIntrvReq, ExIntrvReqHandler);
        ModuleRegisterHandler(mod, Norm_ExIntrvReq, ExIntrvReqHandler);
        ModuleRegisterHandler(mod, Brdcst_ExIntrvReq, ExIntrvReqHandler);
        ModuleRegisterHandler(mod, OwCorruptWb_ExIntrvReq, ExIntrvReqHandler);
        ModuleRegisterHandler(mod, ShCorruptWb_ExIntrvReq, ExIntrvReqHandler);
        ModuleRegisterHandler(mod, OwCorruptMove_ExIntrvReq, ExIntrvReqHandler);
        ModuleRegisterHandler(mod, ShCorruptMove_ExIntrvReq, ExIntrvReqHandler);

        /* Shared forward request handlers */
        ModuleRegisterHandler(mod, Wb_ShFwdReq, ShFwdReqHandler);
        ModuleRegisterHandler(mod, Norm_ShFwdReq, ShFwdReqHandler);
        ModuleRegisterHandler(mod, ShCorruptWb_ShFwdReq, ShFwdReqHandler);
        ModuleRegisterHandler(mod, OwCorruptMove_ShFwdReq, ShFwdReqHandler);
        ModuleRegisterHandler(mod, ShCorruptMove_ShFwdReq, ShFwdReqHandler);

        /* Exclusive forward request handlers */
        ModuleRegisterHandler(mod, Wb_ExFwdReq, ExFwdReqHandler);
        ModuleRegisterHandler(mod, Norm_ExFwdReq, ExFwdReqHandler);
        ModuleRegisterHandler(mod, OwCorruptWb_ExFwdReq, ExFwdReqHandler);
        ModuleRegisterHandler(mod, ShCorruptWb_ExFwdReq, ExFwdReqHandler);
        ModuleRegisterHandler(mod, OwCorruptMove_ExFwdReq, ExFwdReqHandler);
        ModuleRegisterHandler(mod, ShCorruptMove_ExFwdReq, ExFwdReqHandler);

        /* Shared back invalidation rquest handlers */
        ModuleRegisterHandler(mod, ShCorruptWb_LLC_ShBkInvalReq, ShBkInvalReqHandler);
        ModuleRegisterHandler(mod, OwCorruptWb_LLC_ShBkInvalReq, ShBkInvalReqHandler);
        ModuleRegisterHandler(mod, Brdcst_LLC_ShBkInvalReq, ShBkInvalReqHandler);
        ModuleRegisterHandler(mod, Norm_LLC_ShBkInvalReq, ShBkInvalReqHandler);

        ModuleRegisterHandler(mod, Norm_SparseDir_ShBkInvalReq, ShBkInvalReqHandler);
        ModuleRegisterHandler(mod, Norm_PrivateDir_ShBkInvalReq, ShBkInvalReqHandler);
        ModuleRegisterHandler(mod, Region_SparseDir_ShBkInvalReq, ShBkInvalReqHandler);
        ModuleRegisterHandler(mod, Brdcst_SparseDir_ShBkInvalReq, ShBkInvalReqHandler);
        ModuleRegisterHandler(mod, Partial_SparseDir_ShBkInvalReq, ShBkInvalReqHandler);

        /* Exclusive back invalidation request handlers */
        ModuleRegisterHandler(mod, ShCorruptWb_LLC_ExBkInvalReq, ExBkInvalReqHandler);
        ModuleRegisterHandler(mod, OwCorruptWb_LLC_ExBkInvalReq, ExBkInvalReqHandler);
        ModuleRegisterHandler(mod, Brdcst_LLC_ExBkInvalReq, ExBkInvalReqHandler);
        ModuleRegisterHandler(mod, Norm_LLC_ExBkInvalReq, ExBkInvalReqHandler);

        ModuleRegisterHandler(mod, Norm_SparseDir_ExBkInvalReq, ExBkInvalReqHandler);
        ModuleRegisterHandler(mod, Norm_PrivateDir_ExBkInvalReq, ExBkInvalReqHandler);
        ModuleRegisterHandler(mod, Region_SparseDir_ExBkInvalReq, ExBkInvalReqHandler);
}


void PrivateModuleMsgWaitForMSHR(PrivateModule *prmod, Msg *msg, PrivateMSHR *mshr)
{
        mem_debug("      msg %lld waiting in %s for mshr-%d to complete\n", MsgGetId(msg), PrivateModuleGetName(prmod), PrivateMSHRGetId(mshr));
        PrivateModuleMsgWaitForBuffer(prmod, msg, ReqWaitForMSHR);
        MsgListEnqueue(mshr->msg_list, msg);
}


void PrivateModuleMsgWaitForWB(PrivateModule *prmod, Msg *msg, PrivateWB *wb)
{
        mem_debug("      msg %lld waiting in %s for wb-%d to complete\n", MsgGetId(msg), PrivateModuleGetName(prmod), PrivateWBGetId(wb));
        PrivateModuleMsgWaitForBuffer(prmod, msg, ReqWaitForWB);
        MsgListEnqueue(wb->msg_list, msg);
}


void PrivateModuleMsgWaitForFreeMSHR(PrivateModule *prmod, Msg *msg)
{
        mem_debug("      msg %lld waiting in %s for free mshr\n", MsgGetId(msg), PrivateModuleGetName(prmod));
        PrivateModuleMsgWaitForBuffer(prmod, msg, ReqWaitForFreeMSHR);
        MsgListEnqueue(prmod->msgs_waiting_for_free_mshr, msg);
}


void PrivateModuleMsgWaitForFreeWB(PrivateModule *prmod, Msg *msg)
{
        mem_debug("      msg %lld waiting in %s for free wb\n", MsgGetId(msg), PrivateModuleGetName(prmod));
        PrivateModuleMsgWaitForBuffer(prmod, msg, ReqWaitForFreeWB);
        MsgListEnqueue(prmod->msgs_waiting_for_free_wb, msg);
}


/* Returns 1 if the block is located in cache with correct permissions for the request,
 * else returns 0. Returns the pointer to block in the last input argument. */
unsigned int PrivateModuleLookup(PrivateModule *prmod, Msg *msg, PrivateCache *cache, PrivateBlock **ppblk)
{
        /* When block is not present in cache */
        PrivateBlock *pblk = PrivateCacheFindBlock(cache, MsgGetTag(msg));
        PTR_ASSIGN(ppblk, pblk);
        if (!pblk) return 0;

        /* When request is for an exclusive access and block is in shared state */
        if (MsgIsExclusiveAccess(msg) && PrivateBlockIsShared(pblk)) return 0;
        return 1;
}


/* TLB request handler */
void TlbReqHandler(Module *mod, Msg *msg)
{
        TLBCache *l1tlb = NULL;
        TLBCache *l2tlb = NULL;
        PrivateModule *prmod = asPrivateModule(mod);

        /* Get TLBs */
        PrivateModuleGetTLBs(prmod, msg, &l1tlb, &l2tlb);

        /* Set TLB begin latency */
        MsgBeginTLBAccess(msg);

        /* Data write requests do not require TLB lookup and TLB lookups are
         * treated as TLB hits. Also when TLBs are not being modeled TLB lookups
         * are treated as TLB hits. */
        if (MsgIsWriteAccess(msg) || !l1tlb)
        {
                /* Schedule CORE_REQ message - reuse the parent message */
                msg->type = CoreReq;
                ExecuteMsg(msg);
                return;
        }

        /* L1 TLB lookup - currently cycle is also included in TLB lookup */
        unsigned int latency =  TLBCacheGetLatency(l1tlb) - 1; // 0; leo edit
        TLBBlock *l1blk = TLBCacheFindBlock(l1tlb, MsgGetTag(msg));
        if (l1blk)
        {
                /* Update L1 TLB recency counters */
                TLBCacheAccessBlock(l1tlb, l1blk);

                /* Update TLB access statistics */
                prmod->stats.pte.accesses[MsgGetAccessType(msg)]++;
                prmod->stats.pte.l1hits[MsgGetAccessType(msg)]++;
                goto PRIV_MOD_TLB_HIT;
        }

        /* Lookup L2 TLB, if it is being modeled */
        if (l2tlb)
        {
                latency +=  TLBCacheGetLatency(l2tlb);//0 leo edit
                TLBBlock *l2blk = TLBCacheFindBlock(l2tlb, MsgGetTag(msg));
                if (l2blk)
                {
                        /* Fill block in L1 TLB */
                        l1blk = PrivateModuleVictimizeTLB(prmod, l1tlb, msg);
                        TLBCacheFillBlock(l1tlb, l1blk, MsgGetTag(msg), TLBBlockValid);

                        /* Update L1 TLB recency counters */
                        TLBCacheAccessBlock(l1tlb, l1blk);

                        /* Update L2 TLB recency counters */
                        TLBCacheAccessBlock(l2tlb, l2blk);

                        /* Update TLB access statistics */
                        prmod->stats.pte.accesses[MsgGetAccessType(msg)]++;
                        prmod->stats.pte.l2hits[MsgGetAccessType(msg)]++;
                        goto PRIV_MOD_TLB_HIT;
                }
        }

        /* Get page table entry address */
        long long pte = MsgGetPTE(msg);

        /* Insert message in TLB miss message list. If message is not the first
         * message in TLB miss message list then wait in TLB miss message list.
         * Another request has already send a PTE_REQ request to LLC and when it
         * completes then the current message would be scheduled. */
        MsgListEnqueue(prmod->msgs_waiting_for_pte, msg);
        if (MsgListFindByPTE(prmod->msgs_waiting_for_pte, pte) != msg)
                return;

        /* Send PTE_REQ message to LLC after latency cycles */
        Msg *request = RMsgWithId(MsgGetId(msg), PteReq, pte, mod, ModuleGetLowModule(mod, pte));
        MsgSetAccess(request, MsgGetAccess(msg));
        ScheduleMsg(request, latency);
        return;

PRIV_MOD_TLB_HIT:

        /* Schedule CORE_REQ message - reuse the parent message */
        msg->type = CoreReq;
        ScheduleMsg(msg, latency);
}


/* Core request handler */
void CoreReqHandler(Module *mod, Msg *msg)
{
        PrivateBlock *l2blk = NULL;
        PrivateBlock *ll1blk = NULL;
        PrivateBlock *rl1blk = NULL;

        PrivateCache *l2cache = NULL;
        PrivateCache *ll1cache = NULL;
        PrivateCache *rl1cache = NULL;

        long long tag = MsgGetTag(msg);
        Access *access = MsgGetAccess(msg);
        PrivateModule *prmod = asPrivateModule(mod);

	long long copy_of_tag = tag;
	MsgType type = MsgGetType(msg);

	long long unsigned int pref_tag;
        unsigned char pref_ready;
        Msg *pref_request = NULL;
	int i;

        /* Get caches */
        PrivateModuleGetCaches(prmod, msg, &l2cache, &ll1cache, &rl1cache);

        /* Set TLB end latency */
        MsgEndTLBAccess(msg);

        /* Compute L1 access latency: L1 cache lookups and TLB lookups are modeled
         * to occur in parallel and as TLB latency has already been charged, only
         * the extra latency for L1 cache lookups must be charged. */
        unsigned int latency = MAX(PrivateCacheGetLatency(ll1cache), PrivateCacheGetLatency(rl1cache)) - 1;
        latency = latency - MIN(latency, MsgGetTLBLatency(msg)); //leo edit

        /* Lookup block in local L1 cache */
        if (PrivateModuleLookup(prmod, msg, ll1cache, &ll1blk))
        {
                /* Set "experm" flag for local L1 cache block, if access requires exclusive permission */
                if (AccessIsExclusive(access)) PrivateBlockSetExPerm(ll1blk);

                /* Update local L1 cache block to modified state, if access is a write access */
                if (AccessIsDataWrite(access)) PrivateBlockSetModified(ll1blk);

                /* Update local L1 cache recency counters */
                PrivateCacheAccessBlock(ll1cache, ll1blk, AccessGetEIP(access));

                /* Update private block accesses */
                PrivateBlockUpdateAccesses(ll1blk);

                /* Update local L1 hit and module access statistics */
                prmod->stats.block.locall1hits[AccessGetType(access)]++;
                prmod->stats.block.accesses[AccessGetType(access)]++;

                /* Schedule CORE_REP message - reuse the parent message */
                msg->type = CoreRep;
                ScheduleMsg(msg, latency);
                return;
        }

        /* Update pending stores if request is a write access */
        if (AccessIsDataWrite(access)) PrivateModuleSetPendingStore(prmod, msg);

        /* Set L1 cache miss in request access data */
        AccessSetMissesInL1(access);

        /* Lookup block in remote L1 cache */
        if (PrivateModuleLookup(prmod, msg, rl1cache, &rl1blk))
        {
                /* Check: block must not be present in local L1 cache */
                assert(!ll1blk);

                /* Victimize block from local L1 cache */
                ll1blk = PrivateModuleVictimizeCache(prmod, ll1cache, msg, prmod->available_wbs, latency);
                if (!ll1blk)
                {
                        PrivateModuleMsgWaitForFreeWB(prmod, msg);
                        return;
                }

                /* Fill block in local L1 cache */
                PrivateCacheFillBlock(ll1cache, ll1blk, tag, PrivateBlockGetState(rl1blk), AccessGetEIP(access));

                /* Set block accesses in local L1 cache block from remote L1 cache block */
                PrivateBlockSetAccesses(ll1blk, PrivateBlockGetAccesses(rl1blk));

                /* Set "experm" flag for local L1 cache block, if access requires exclusive
                 * permission or if flag is set for remote L1 cache block. */
                if (AccessIsExclusive(access) || PrivateBlockIsExPermSet(rl1blk))
                        PrivateBlockSetExPerm(ll1blk);

                /* Update local L1 cache block to modified state, if access is a write access */
                if (AccessIsDataWrite(access)) PrivateBlockSetModified(ll1blk);

                /* Invalidate block from remote L1 cache */
                PrivateCacheInvalidateBlock(rl1cache, rl1blk, RemoteCacheInvalidation);
		PrivateBlockResetPrefetched(rl1blk);

                /* Update local L1 cache recency counters */
                PrivateCacheAccessBlock(ll1cache, ll1blk, AccessGetEIP(access));

                /* Update private block accesses */
                PrivateBlockUpdateAccesses(ll1blk);

                /* Update remote L1 hit and module access statistics */
                prmod->stats.block.remotel1hits[AccessGetType(access)]++;
                prmod->stats.block.accesses[AccessGetType(access)]++;

                /* Schedule CORE_REP message - reuse the parent message */
                msg->type = CoreRep;
                ScheduleMsg(msg, latency);
                return;
        }

        /* If L2 cache is not being modeled skip looking for block in L2 cache
         * and treat request as a miss. */
        if (!l2cache) goto PRIV_MOD_MISS;

        /* Lookup block in L2 cache */
        latency += PrivateCacheGetLatency(l2cache);
        if (PrivateModuleLookup(prmod, msg, l2cache, &l2blk))
        {
		if (PrivateBlockIsPrefetched(l2blk)) {
                        PrivateBlockResetPrefetched(l2blk);
                        if (core_stream_prefetcher_enabled && (/*(type == InstFetch) ||*/ (type == DataRead) || (type == DataReadEx))) {
                                for (i=0; i<core_stream_prefetcher_lookahead; i++) {
                                        streamPrefetcher_lookup(prmod->sprefetcher, (long long unsigned int)copy_of_tag, 1, 1, AccessGetEIP(access), &pref_tag, &pref_ready);
                                        if (pref_ready) {
                                                pref_ready_count++;
                                                pref_request = LMsg(PrefetchReq, pref_tag, asModule(prmod));
                                                if (type==InstFetch) {
							MsgSetAccess(pref_request, new(Access, PrefetchInst, pref_tag, AccessGetSize(access), NULL, NULL));
                                                }
                                                else {
							MsgSetAccess(pref_request, new(Access, PrefetchData, pref_tag, AccessGetSize(access), NULL, NULL));
                                                }
                                                /* Execute request message */
                                                ScheduleMsg(pref_request, latency);
                                                //if (logger) fprintf(stderr, "[%llu] %d P C H %#llx %d\n", nextDRAMCycle, mod->id, pref_tag >> core_stream_prefetcher_block_size_log, latency);
                                                copy_of_tag = pref_tag;
                                        }
                                        else break;
                                }
                        }
                }

                /* When block is not present in local L1 cache */
                if (!ll1blk)
                {
                        /* Victimize block from local L1 cache */
                        ll1blk = PrivateModuleVictimizeCache(prmod, ll1cache, msg, prmod->available_wbs, latency);
                        if (!ll1blk)
                        {
                                PrivateModuleMsgWaitForFreeWB(prmod, msg);
                                return;
                        }

                        /* When block is present in remote L1 cache */
                        if (rl1blk)
                        {
                                /* Check: block access in L2 cache block must be less than block access in remote L1 cache block */
                                assert(PrivateBlockGetAccesses(l2blk) < PrivateBlockGetAccesses(rl1blk));

                                /* Set "experm" flag for L2 cache block, if flag is set for remote L1 cache block */
                                if (PrivateBlockIsExPermSet(rl1blk)) PrivateBlockSetExPerm(l2blk);

                                /* Set block accesses in L2 cache block from remote L1 cache block */
                                PrivateBlockSetAccesses(l2blk, PrivateBlockGetAccesses(rl1blk));

                                /* Invalidate block from remote L1 cache */
                                PrivateCacheInvalidateBlock(rl1cache, rl1blk, RemoteCacheInvalidation);
				PrivateBlockResetPrefetched(rl1blk);
                                rl1blk = NULL;
                        }

                        /* Fill block in local L1 cache */
                        PrivateCacheFillBlock(ll1cache, ll1blk, tag, PrivateBlockGetState(l2blk), AccessGetEIP(access));

                        /* Set block accesses in local L1 cache block from L2 cache block */
                        PrivateBlockSetAccesses(ll1blk, PrivateBlockGetAccesses(l2blk));
                }

                /* Check: block must not be present in remote L1 cache */
                assert(!rl1blk);

                /* Check: block access in L2 cache block must not be more than block access in local L1 cache block */
                assert(PrivateBlockGetAccesses(l2blk) <= PrivateBlockGetAccesses(ll1blk));

                /* Update L2 cache recency counters */
                PrivateCacheAccessBlock(l2cache, l2blk, AccessGetEIP(access));

                /* Set "experm" flag for local L1 cache block, if access requires exclusive
                 * permission or if flag is set for L2 cache block. */
                if (AccessIsExclusive(access) || PrivateBlockIsExPermSet(l2blk))
                        PrivateBlockSetExPerm(ll1blk);

                /* Update local L1 cache block to modified state, if access is a write access,
                 * else update local L1 cache block state to L2 cache block state. */
                if (AccessIsDataWrite(access)) PrivateBlockSetModified(ll1blk);
                else PrivateBlockSetState(ll1blk, PrivateBlockGetState(l2blk));

                /* Update local L1 cache recency counters */
                PrivateCacheAccessBlock(ll1cache, ll1blk, AccessGetEIP(access));

                /* Update private block accesses */
                PrivateBlockUpdateAccesses(ll1blk);

                /* Update L2 hit and module access statistics */
                prmod->stats.block.l2hits[AccessGetType(access)]++;
                prmod->stats.block.accesses[AccessGetType(access)]++;

                /* Schedule CORE_REP message - reuse the parent message */
                msg->type = CoreRep;
                ScheduleMsg(msg, latency);
                return;
        }

	copy_of_tag = tag;

	if (l2blk)
        {
                if (PrivateBlockIsPrefetched(l2blk)) {
                        PrivateBlockResetPrefetched(l2blk);
                        if (core_stream_prefetcher_enabled && (/*(type == InstFetch) ||*/ (type == DataRead) || (type == DataReadEx))) {
                                for (i=0; i<core_stream_prefetcher_lookahead; i++) {
                                        streamPrefetcher_lookup(prmod->sprefetcher, (long long unsigned int)copy_of_tag, 1, 1, AccessGetEIP(access), &pref_tag, &pref_ready);
                                        if (pref_ready) {
                                                pref_ready_count++;
                                                pref_request = LMsg(PrefetchReq, pref_tag, asModule(prmod));
                                                if (type==InstFetch) {
                                                        MsgSetAccess(pref_request, new(Access, PrefetchInst, pref_tag, AccessGetSize(access), NULL, NULL));
                                                }
                                                else {
                                                        MsgSetAccess(pref_request, new(Access, PrefetchData, pref_tag, AccessGetSize(access), NULL, NULL));
                                                }
                                                /* Execute request message */
                                                ScheduleMsg(pref_request, latency);
                                                //if (logger) fprintf(stderr, "[%llu] %d P C H %#llx %d\n", nextDRAMCycle, mod->id, pref_tag >> core_stream_prefetcher_block_size_log, latency);
                                                copy_of_tag = pref_tag;
                                        }
                                        else break;
                                }
                        }
                }
	}

/* NOTE: semicolon after at the end of label REQUEST_FINISH is needed - http://stackoverflow.com/q/18496282 */
PRIV_MOD_MISS: ;

        /* If there is a pending writeback for the block then current request
         * must wait for it to complete. */
        PrivateWB *wb = PrivateModuleFindWB(prmod, tag);
        if (wb)
        {
                PrivateModuleMsgWaitForWB(prmod, msg, wb);
                return;
        }

        /* If there is a pending miss request for the block then current request
         * must wait for it to complete. */
        PrivateMSHR *mshr = PrivateModuleFindMSHR(prmod, tag);
        if (mshr)
        {
                PrivateModuleMsgWaitForMSHR(prmod, msg, mshr);
                return;
        }

        /* If MSHRs are not available then request must be retried later when
         * MSHRs are available. */
        if (PrivateMSHRListSize(prmod->available_mshrs) < 1)
        {
                PrivateModuleMsgWaitForFreeMSHR(prmod, msg);
                return;
        }

        /* If WBs are not available then request must be retried later when WBs
         * are available. */
        if (PrivateWBListSize(prmod->available_wbs) < ((prmod->caches[L2Cache]) ? 2 : 1))
        {
                PrivateModuleMsgWaitForFreeWB(prmod, msg);
                return;
        }

        /* Set L2 cache miss in request access data */
        AccessSetMissesInL2(access);

        /* Occupy MSHR for servicing miss request */
        mshr = PrivateMSHRListDequeue(prmod->available_mshrs);
        PrivateMSHROccupy(mshr, msg, (ll1blk || rl1blk || l2blk) ? 1 : 0);

        /* Reserve WBs */
        PrivateMSHRReserveWB(mshr, PrivateWBListDequeue(prmod->available_wbs));
        if (prmod->caches[L2Cache]) PrivateMSHRReserveWB(mshr, PrivateWBListDequeue(prmod->available_wbs));

        /* Add MSHR to occupied MSHR list */
        PrivateMSHRListEnqueue(prmod->occupied_mshrs, mshr);

        /* Create ExReq/ShReq request message to be send to LLC module */
        Msg *request = RMsgWithId(MsgGetId(msg), AccessIsExclusive(access) ? ExGetReq : ShGetReq, tag, mod, ModuleGetLowModule(mod, tag));
        if (PrivateMSHRModuleHasBlock(mshr)) MsgSetUpgrReq(request);
        MsgSetAccess(request, access);

        /* Send ExReq/ShReq request message after latency cycles */
        ScheduleMsg(request, latency);

	copy_of_tag = tag;

        if (core_stream_prefetcher_enabled && (/*(type == InstFetch) ||*/ (type == DataRead) || (type == DataReadEx)) && !ll1blk && !rl1blk && !l2blk) {
        	for (i=0; i<core_stream_prefetcher_lookahead; i++) {
                	streamPrefetcher_lookup(prmod->sprefetcher, (long long unsigned int)copy_of_tag, 1, 1, AccessGetEIP(access), &pref_tag, &pref_ready);
                        if (pref_ready) {
                        	pref_ready_count++;
                                pref_request = LMsg(PrefetchReq, pref_tag, asModule(prmod));
                                if (type==InstFetch) {
                                	MsgSetAccess(pref_request, new(Access, PrefetchInst, pref_tag, AccessGetSize(access), NULL, NULL));
                                }
                                else {
                                	MsgSetAccess(pref_request, new(Access, PrefetchData, pref_tag, AccessGetSize(access), NULL, NULL));
                                }
                                /* Execute request message */
                                ScheduleMsg(pref_request, latency);
                                //if (logger) fprintf(stderr, "[%llu] %d P C H %#llx %d\n", nextDRAMCycle, mod->id, pref_tag >> core_stream_prefetcher_block_size_log, latency);
                                copy_of_tag = pref_tag;
                       	}
                        else break;
                }
        }
}


void PrefetchReqHandler(Module *mod, Msg *msg)
{
	PrivateBlock *l2blk = NULL;
        PrivateBlock *ll1blk = NULL;
        PrivateBlock *rl1blk = NULL;

        PrivateCache *l2cache = NULL;
        PrivateCache *ll1cache = NULL;
        PrivateCache *rl1cache = NULL;

        long long tag = MsgGetTag(msg);
        Access *access = MsgGetAccess(msg);
        PrivateModule *prmod = asPrivateModule(mod);

	AccessType type = AccessGetType(access);
	assert((type == PrefetchInst) || (type == PrefetchData));

        /* Get caches */
        PrivateModuleGetCaches(prmod, msg, &l2cache, &ll1cache, &rl1cache);

        /* Compute L1 access latency: L1 cache lookups and TLB lookups are modeled
         * to occur in parallel and as TLB latency has already been charged, only
         * the extra latency for L1 cache lookups must be charged. */
        unsigned int latency = MAX(PrivateCacheGetLatency(ll1cache), PrivateCacheGetLatency(rl1cache)) - 1;

	/* Lookup block in local L1 cache */
        if (PrivateModuleLookup(prmod, msg, ll1cache, &ll1blk) || ll1blk)
        {
		delete(MsgGetAccess(msg));
                delete(msg);
                return;
        }

        /* Set L1 cache miss in request access data */
        AccessSetMissesInL1(access);

	/* Lookup block in remote L1 cache */
        if (PrivateModuleLookup(prmod, msg, rl1cache, &rl1blk) || rl1blk)
        {
		delete(MsgGetAccess(msg));
                delete(msg);
		return;
	}

	assert(l2cache);

	/* Lookup block in L2 cache */
        latency += PrivateCacheGetLatency(l2cache);
        if (PrivateModuleLookup(prmod, msg, l2cache, &l2blk) || l2blk)
        {
		delete(MsgGetAccess(msg));
                delete(msg);
		return;
	}

	/* If there is a pending miss request for the block then current request
         * must be dropped. */
        PrivateMSHR *mshr = PrivateModuleFindMSHR(prmod, tag);
        if (mshr)
        {
                delete(MsgGetAccess(msg));
                delete(msg);
                return;
        }

	/* If there is a pending writeback for the block then current request
         * must wait for it to complete. */
        PrivateWB *wb = PrivateModuleFindWB(prmod, tag);
        if (wb)
        {
                PrivateModuleMsgWaitForWB(prmod, msg, wb);
                return;
        }

        /* If MSHRs are not available then request must be retried later when
         * MSHRs are available. */
        if (PrivateMSHRListSize(prmod->available_mshrs) < 1)
        {
                PrivateModuleMsgWaitForFreeMSHR(prmod, msg);
                return;
        }

        /* If WBs are not available then request must be retried later when WBs
         * are available. */
        if (PrivateWBListSize(prmod->available_wbs) < ((prmod->caches[L2Cache]) ? 2 : 1))
        {
                PrivateModuleMsgWaitForFreeWB(prmod, msg);
                return;
        }

        /* Set L2 cache miss in request access data */
        AccessSetMissesInL2(access);

	assert(!(ll1blk || rl1blk || l2blk));

        /* Occupy MSHR for servicing miss request */
        mshr = PrivateMSHRListDequeue(prmod->available_mshrs);
        PrivateMSHROccupy(mshr, msg, (ll1blk || rl1blk || l2blk) ? 1 : 0);

	/* Reserve WBs */
        PrivateMSHRReserveWB(mshr, PrivateWBListDequeue(prmod->available_wbs));
        if (prmod->caches[L2Cache]) PrivateMSHRReserveWB(mshr, PrivateWBListDequeue(prmod->available_wbs));

        /* Add MSHR to occupied MSHR list */
        PrivateMSHRListEnqueue(prmod->occupied_mshrs, mshr);

        /* Create ExReq/ShReq request message to be send to LLC module */
        Msg *request = RMsgWithId(MsgGetId(msg), AccessIsExclusive(access) ? ExGetReq : ShGetReq, tag, mod, ModuleGetLowModule(mod, tag));
        if (PrivateMSHRModuleHasBlock(mshr)) MsgSetUpgrReq(request);
        MsgSetAccess(request, access);

        /* Send ExReq/ShReq request message after latency cycles */
        ScheduleMsg(request, latency);
}


void CoreRepHandler(Module *mod, Msg *msg)
{
        PrivateModule *prmod = asPrivateModule(mod);

        /* Remove message from access hash table */
        PrivateModuleAccessListRemoveMsg(prmod, msg);

        /* Clear pending store message from private module */
        PrivateModuleClearPendingStore(prmod, msg);

        /* Free access data */
        delete(MsgGetAccess(msg));

        /* Free message */
        delete(msg);
}


void PrefetchRepHandler(Module *mod, Msg *msg)
{
        /* Free access data */
        delete(MsgGetAccess(msg));

        /* Free message */
        delete(msg);
}


void PteRepHandler(Module *mod, Msg *msg)
{
        TLBCache *l1tlb = NULL;
        TLBCache *l2tlb = NULL;

        long long id = MsgGetId(msg);
        PrivateModule *prmod = asPrivateModule(mod);

        /* Free reply message */
        delete(msg);

        /* Get parent request which initiated PTE request */
        msg = MsgListFindById(prmod->msgs_waiting_for_pte, id);
        assert(msg);

        /* Get TLBs */
        PrivateModuleGetTLBs(prmod, msg, &l1tlb, &l2tlb);

        /* Fill block in L2 TLB */
        if (l2tlb)
        {
                TLBBlock *l2blk = PrivateModuleVictimizeTLB(prmod, l2tlb, msg);
                TLBCacheFillBlock(l2tlb, l2blk, MsgGetTag(msg), TLBBlockValid);
                TLBCacheAccessBlock(l2tlb, l2blk);
        }

        /* Fill block in L1 TLB */
        TLBBlock *l1blk = PrivateModuleVictimizeTLB(prmod, l1tlb, msg);
        TLBCacheFillBlock(l1tlb, l1blk, MsgGetTag(msg), TLBBlockValid);
        TLBCacheAccessBlock(l1tlb, l1blk);

        /* Update TLB access statistics */
        prmod->stats.pte.accesses[MsgGetAccessType(msg)]++;

        /* Remove message from TLB miss message list and wakeup other messages
         * waiting in TLB miss message list which are for matching PTE. */
        MsgListRemove(prmod->msgs_waiting_for_pte, msg);
        MsgListWakeupMsgsByPTE(prmod->msgs_waiting_for_pte, MsgGetPTE(msg));

        /* Schedule CORE_REQ message - reuse the parent message */
        msg->type = CoreReq;
        ScheduleMsg(msg, 0);
}


void PteNackHandler(Module *mod, Msg *msg)
{
        long long id = MsgGetId(msg);
        PrivateModule *prmod = asPrivateModule(mod);

        /* Free reply message */
        delete(msg);

        /* Get parent request which initiated PTE request */
        msg = MsgListFindById(prmod->msgs_waiting_for_pte, id);
        assert(msg);

        /* Update TLB NACK statistics */
        prmod->stats.pte.nacks[MsgGetAccessType(msg)]++;

        /* Get page table entry address */
        long long pte = MsgGetPTE(msg);

        /* Create PteReq request message to be sent to LLC module */
        Msg *request = RMsgWithId(MsgGetId(msg), PteReq, pte, mod, ModuleGetLowModule(mod, pte));
        MsgSetAccess(request, MsgGetAccess(msg));
        MsgSetWaitingForBuffer(request);

        /* Send PteReq request message after retry latency cycles */
        ScheduleMsg(request, prmod->retry_latency);
        return;
}


void BlkRepHandler(Module *mod, Msg *msg)
{
        PrivateModule *prmod = asPrivateModule(mod);

        /* Locate MSHR servicing miss request */
        PrivateMSHR *mshr = PrivateModuleFindMSHR(prmod, MsgGetTag(msg));
        assert(mshr);

        /* Update replies received in MSHR and free reply message */
        PrivateMSHRSetReply(mshr, msg);
        delete(msg);

        /* If more replies are expected then wait for them - do nothing */
        if (PrivateMSHRMoreRepliesExptd(mshr)) return;

        /* Process replies */
        if (PrivateMSHRRecvdShReply(mshr)) PrivateModuleSharedFill(prmod, mshr);
        else PrivateModuleExclusiveFill(prmod, mshr);

        /* Free MSHR */
        PrivateModuleFreeMSHR(prmod, mshr);
}


void BlkNackHandler(Module *mod, Msg *nreply)
{
        PrivateModule *prmod = asPrivateModule(mod);
        long long tag = MsgGetTag(nreply);

        PrivateBlock *pblks[NumPrivateCacheTypes] = { NULL };
        PrivateCache **pcaches = prmod->caches;

        /* Locate MSHR servicing miss request */
        PrivateMSHR *mshr = PrivateModuleFindMSHR(prmod, tag);
        assert(mshr);

        /* Get miss request being serviced by MSHR */
        Msg *mrequest = PrivateMSHRGetMissReq(mshr);
        assert(mrequest);

        /* Update NACK statistics and free reply */
        prmod->stats.block.nacks[MsgGetAccessType(mrequest)]++;
        delete(nreply);

        /* Locate block in module caches */
        for (PrivateCacheType index = 0; index < NumPrivateCacheTypes; index++)
        {
                /* Skip if cache is not being modeled or if block is not present in cache */
                if (!pcaches[index]) continue;
                pblks[index] = PrivateCacheFindBlock(pcaches[index], tag);
                if (!pblks[index]) continue;

                /* Check: "experm" flag must not be set for cache
                 * block and it must be in shared state. */
                assert(!PrivateBlockIsExPermSet(pblks[index]));
                assert(PrivateBlockIsShared(pblks[index]));

                /* Check: MSHR must indicate that module contains the block */
                assert(PrivateMSHRModuleHasBlock(mshr));
        }

        /* Handle pending early shared forward race request */
        Msg *erequest = PrivateMSHRGetShFwdRaceReq(mshr);
        if (erequest)
        {
                /* Check: MSHR must contain block */
                assert(PrivateMSHRModuleHasBlock(mshr));

                /* Send forward reply message */
                FwdReplyHelper(mod, erequest, 0);

                /* Update coherence statistics and free request */
                PrivateModuleUpdateCoherenceStat(prmod, erequest, pblks);
                delete(erequest);
        }

        /* When there are no pending early race requests which would remove block */
        if (!PrivateMSHRGetExFwdRaceReq(mshr) && !PrivateMSHRGetShBkInvalRaceReq(mshr) && !PrivateMSHRNumInvalRaceReqs(mshr))
                goto PROCESS_REQUEST;

        /* Handle pending early exclusive forward race request */
        erequest = PrivateMSHRGetExFwdRaceReq(mshr);
        if (erequest)
        {
                /* Check: MSHR must contain block */
                assert(PrivateMSHRModuleHasBlock(mshr));

                /* Send forward reply message */
                FwdReplyHelper(mod, erequest, 0);

                /* Update coherence statistics and free request */
                PrivateModuleUpdateCoherenceStat(prmod, erequest, pblks);
                delete(erequest);
        }

        /* Handle pending early shared back invalidation race request */
        erequest = PrivateMSHRGetShBkInvalRaceReq(mshr);
        if (erequest)
        {
                /* Determine type of back invalidation reply */
                MsgType mtype = NoData_ShBkInvalRep;
                if (erequest->forwarder == mod)
                {
                        /* Check: MSHR must contain block */
                        assert(PrivateMSHRModuleHasBlock(mshr));

                        /* Check: request must be corrupt writeback request */
                        assert(MsgIsCorruptWbBkInvalReq(erequest));

                        /* Set back invalidation reply type */
                        mtype = (MsgIsShCorruptWbBkInvalReq(erequest)) ? ShPartData_ShBkInvalRep : OwPartData_ShBkInvalRep;
                }

                /* Send back invalidation reply message to sender module */
                Msg *reply = RMsgWithId(MsgGetId(erequest), mtype, tag, mod, erequest->sender);
                ScheduleMsg(reply, 0);

                /* Update coherence statistics and free request */
                PrivateModuleUpdateCoherenceStat(prmod, erequest, pblks);
                delete(erequest);
        }

        /* Handle pending early invalidation race request */
        while (PrivateMSHRNumInvalRaceReqs(mshr))
        {
                /* Get pending early invalidation race request */
                erequest = PrivateMSHRGetInvalRaceReq(mshr);
                assert(erequest);

                /* When invalidations must wait for pending miss requests */
                if (invals_wait_for_pending_miss_requests)
                {
                        /* Send invalidation reply message to requestor module */
                        Msg *reply = RMsgWithId(MsgGetId(erequest), InvalRep, tag, mod, erequest->requestor);
                        ScheduleMsg(reply, 0);
                }

                /* Update coherence statistics and free request */
                PrivateModuleUpdateCoherenceStat(prmod, erequest, pblks);
                delete(erequest);
        }

        /* Update block access histogram */
        PrivateModuleUpdateBlockAccessStat(prmod, pblks);

        /* Invalidate block from module caches */
        for (PrivateCacheType index = 0; index < NumPrivateCacheTypes; index++)
                if (pblks[index]) {
			PrivateCacheInvalidateBlock(pcaches[index], pblks[index], CoherenceInvalidation);
			PrivateBlockResetPrefetched(pblks[index]);
		}

        /* Update in MSHR that block is not present in module. Also notify
         * CPU about block being invalidated. This is required for
         * enforcing memory consistency. */
        if (PrivateMSHRModuleHasBlock(mshr))
        {
                PrivateMSHRClearModuleHasBlock(mshr);
                cpu_invalidate(prmod->core, prmod->thread, tag);
        }

PROCESS_REQUEST:

        /* Handle pending early intervention race requests */
        erequest = PrivateMSHRGetIntrvRaceReq(mshr);
        if (erequest)
        {
                /* Check: MSHR must indicate that block is not present in module */
                assert(!PrivateMSHRModuleHasBlock(mshr));

                /* Check: request must be broadcast intervention */
                assert(MsgIsBrdcstIntrvReq(erequest));

                /* Check: optimized interventions must not be enabled */
                assert(!intrvs_opt);

                /* Send intervention reply */
                IntrvReplyHelper(mod, erequest, 0, 0, 0, 0);

                /* Update coherence statistics and free request */
                PrivateModuleUpdateCoherenceStat(prmod, erequest, NULL);
                delete(erequest);
        }

        /* Handle pending early exclusive back invalidation race request */
        erequest = PrivateMSHRGetExBkInvalRaceReq(mshr);
        if (erequest)
        {
                /* Check: MSHR must indicate that block is not present in module */
                assert(!PrivateMSHRModuleHasBlock(mshr));

                /* Check: request must not be corrupt writeback back invalidation */
                assert(!MsgIsCorruptWbBkInvalReq(erequest));

                /* Determine type of back invalidation reply */
                MsgType mtype = NoData_ExBkInvalRep;

                /* Send back invalidation reply message to sender module */
                Msg *reply = RMsgWithId(MsgGetId(erequest), mtype, tag, mod, erequest->sender);
                ScheduleMsg(reply, 0);

                /* Update coherence statistics and free request */
                PrivateModuleUpdateCoherenceStat(prmod, erequest, NULL);
                delete(erequest);
        }

        /* Clear pending race requests from MSHR */
        PrivateMSHRClearRaceReqs(mshr);

        /* Create ExReq/ShReq request message to be send to LLC module */
        Msg *rrequest = RMsgWithId(MsgGetId(mrequest), MsgIsExclusiveAccess(mrequest) ? ExGetReq : ShGetReq, tag, mod, ModuleGetLowModule(mod, tag));
        if (PrivateMSHRModuleHasBlock(mshr)) MsgSetUpgrReq(rrequest);
        MsgSetAccess(rrequest, MsgGetAccess(mrequest));
        MsgSetWaitingForBuffer(rrequest);

        /* Send ExReq/ShReq request message after retry latency cycles */
        ScheduleMsg(rrequest, prmod->retry_latency);
}


void WbRepHandler(Module *mod, Msg *reply)
{
        PrivateModule *prmod = asPrivateModule(mod);
        long long tag = MsgGetTag(reply);

        /* Locate WB servicing writeback request */
        PrivateWB *wb = PrivateModuleFindWB(prmod, tag);
        assert(wb && !PrivateWBWasAcked(wb));

        /* Set in WB that writeback has been acknowledged */
        PrivateWBSetAcked(wb);

        /* Free reply message */
        delete(reply);

        /* Set writeback status flags */
        unsigned int dirty = PrivateWBIsDirtyDataWbReq(wb);
        unsigned int experm = PrivateWBIsExPermSet(wb);

        /* Handle late exclusive intervention race */
        Msg *request = PrivateWBGetExIntrvRaceReq(wb);
        if (request)
        {
                /* Check: WB must not be servicing a shared writeback */
                assert(!PrivateWBIsShWbReq(wb));

                /* Update coherence statistics */
                PrivateModuleUpdateCoherenceStat(prmod, request, NULL);

                /* When interventions must wait for pending writebacks */
                if (intrvs_wait_for_pending_writebacks)
                {
                        /* Check: request must not be corrupt wb or more intervention */
                        assert(!MsgIsCorruptWbIntrvReq(request) && !MsgIsCorruptMoveIntrvReq(request));

                        /* When optimized interventions are enabled */
                        if (intrvs_opt)
                        {
                                /* Check: request must not be broadcast intervention */
                                assert(!MsgIsBrdcstIntrvReq(request));

                                /* Determine type of intervention clear message */
                                MsgType mtype = NoData_ExIntrvClr;
                                if (MsgIsWbIntrvReq(request))
                                {
                                        mtype = dirty ? DirtyData_ExIntrvClr : CleanData_ExIntrvClr;
                                        dirty = 0;
                                }

                                /* Send intervention clear message to sender module */
                                reply = RMsgWithId(MsgGetId(request), mtype, tag, mod, request->sender);
                                ScheduleMsg(reply, 0);

                                /* Set "experm" flag for intervention clear message */
                                if (experm) MsgSetExPerm(reply);
                                experm = 0;
                        }

                        /* Send intervention reply */
                        IntrvReplyHelper(mod, request, 1, dirty, experm, 0);
                }

                /* Free late race request */
                delete(request);
        }

        /* Handle late shared intervention race */
        request = PrivateWBGetShIntrvRaceReq(wb);
        if (request)
        {
                /* Check: WB must not be servicing a shared writeback */
                assert(!PrivateWBIsShWbReq(wb));

                /* Update coherence statistics */
                PrivateModuleUpdateCoherenceStat(prmod, request, NULL);

                /* When interventions must wait for pending writebacks */
                if (intrvs_wait_for_pending_writebacks)
                {
                        /* Note: block has been updated in shared module
                         * as writeback has completed. */

                        /* Check: request must not be corrupt wb or more intervention */
                        assert(!MsgIsCorruptWbIntrvReq(request) && !MsgIsCorruptMoveIntrvReq(request));

                        /* When optimized interventions are enabled */
                        if (intrvs_opt)
                        {
                                /* Check: request must not be broadcast intervention */
                                assert(!MsgIsBrdcstIntrvReq(request));

                                /* Check: request must not be corrupt move intervention */
                                assert(!MsgIsCorruptMoveIntrvReq(request));

                                /* Determine type of intervention clear message */
                                MsgType mtype = NoData_ShIntrvClr;
                                if (MsgIsWbIntrvReq(request)) mtype = CleanData_ShIntrvClr;

                                /* Send intervention clear message to sender module */
                                reply = RMsgWithId(MsgGetId(request), mtype, tag, mod, request->sender);
                                ScheduleMsg(reply, 0);

                                /* Set "experm" flag for intervention clear message */
                                if (experm) MsgSetExPerm(reply);
                                experm = 0;
                        }

                        /* Send intervention reply */
                        IntrvReplyHelper(mod, request, 1, 0, experm, 0);
                }

                /* Free late race request */
                delete(request);
        }

        /* Handle late forward race */
        request = PrivateWBGetFwdRaceReq(wb);
        if (request)
        {
                /* Check: WB must be servicing a shared writeback */
                assert(PrivateWBIsShWbReq(wb));

                /* Update coherence statistics */
                PrivateModuleUpdateCoherenceStat(prmod, request, NULL);

                /* Free late race request */
                delete(request);
        }

        /* Handle late invalidation race */
        request = PrivateWBGetInvalRaceReq(wb);
        if (request)
        {
                /* Check: WB must be servicing a shared writeback */
                assert(PrivateWBIsShWbReq(wb));

                /* Update coherence statistics */
                PrivateModuleUpdateCoherenceStat(prmod, request, NULL);

                /* When invalidations must wait for pending writebacks */
                if (invals_wait_for_pending_writebacks)
                {
                        /* Send invalidation reply message to requestor module */
                        reply = RMsgWithId(MsgGetId(request), InvalRep, tag, mod, request->requestor);
                        ScheduleMsg(reply, 0);
                }

                /* Free late race request */
                delete(request);
        }

        /* Handle late exclusive back invalidation race */
        request = PrivateWBGetExBkInvalRaceReq(wb);
        if (request)
        {
                /* Check: WB must not be servicing a shared writeback */
                assert(!PrivateWBIsShWbReq(wb));

                /* Update coherence statistics */
                PrivateModuleUpdateCoherenceStat(prmod, request, NULL);

                /* When back invalidations must wait for pending writebacks */
                if (bkinvals_wait_for_pending_writebacks)
                {
                        /* Check: request must not be corrupt writeback back invalidation */
                        assert(!MsgIsCorruptWbBkInvalReq(request));

                        /* Send back invalidation reply message to sender module */
                        reply = RMsgWithId(MsgGetId(request), NoData_ExBkInvalRep, tag, mod, request->sender);
                        ScheduleMsg(reply, 0);

                        /* Set "experm" flag for back invalidation reply */
                        if (experm) MsgSetExPerm(reply);
                }

                /* Free late race request */
                delete(request);
        }

        /* Handle late shared back invalidation race */
        request = PrivateWBGetShBkInvalRaceReq(wb);
        if (request)
        {
                /* Check: WB must be servicing a shared writeback */
                assert(PrivateWBIsShWbReq(wb));

                /* Update coherence statistics */
                PrivateModuleUpdateCoherenceStat(prmod, request, NULL);

                /* When back invalidations must wait for pending writebacks */
                if (bkinvals_wait_for_pending_writebacks)
                {
                        /* Check: request must not be corrupt writeback back invalidation */
                        assert(!MsgIsCorruptWbBkInvalReq(request));

                        /* Send back invalidation reply message to sender module */
                        reply = RMsgWithId(MsgGetId(request), NoData_ShBkInvalRep, tag, mod, request->sender);
                        ScheduleMsg(reply, 0);
                }

                /* Free late race request */
                delete(request);
        }

        /* Free WB */
        PrivateModuleFreeWB(prmod, wb);
}


void NormWbNackHandler(Module *mod, Msg *nack)
{
        PrivateModule *prmod = asPrivateModule(mod);
        long long tag = MsgGetTag(nack);
        long long id = MsgGetId(nack);

        /* Locate WB servicing writeback request */
        PrivateWB *wb = PrivateModuleFindWB(prmod, tag);
        assert(wb && !PrivateWBWasAcked(wb));

        /* Free reply message */
        delete(nack);

        /* Update writeback NACK statistics */
        prmod->stats.writeback.nacks++;

        /* Get writeback status */
        unsigned int dirty = PrivateWBIsDirtyDataWbReq(wb);
        unsigned int experm = PrivateWBIsExPermSet(wb);

        /* Handle late exclusive intervention race */
        if (PrivateWBGetExIntrvRaceReq(wb))
        {
                /* Check: WB must not be servicing a shared writeback */
                assert(!PrivateWBIsShWbReq(wb));

                /* Get late exclusive intervention race */
                Msg *request = PrivateWBGetExIntrvRaceReq(wb);

                /* Update coherence statistics */
                PrivateModuleUpdateCoherenceStat(prmod, request, NULL);

                /* When interventions must wait for pending writebacks */
                if (intrvs_wait_for_pending_writebacks)
                {
                        /* Check: request must not be corrupt wb or more intervention */
                        assert(!MsgIsCorruptWbIntrvReq(request) && !MsgIsCorruptMoveIntrvReq(request));

                        /* When optimized interventions are enabled */
                        if (intrvs_opt)
                        {
                                /* Check: request must not be broadcast intervention */
                                assert(!MsgIsBrdcstIntrvReq(request));

                                /* Determine type of intervention clear message */
                                MsgType mtype = NoData_ExIntrvClr;
                                if (MsgIsWbIntrvReq(request))
                                {
                                        mtype = dirty ? DirtyData_ExIntrvClr : CleanData_ExIntrvClr;
                                        dirty = 0;
                                }

                                /* Send intervention clear message to sender module */
                                Msg *reply = RMsgWithId(MsgGetId(request), mtype, tag, mod, request->sender);
                                ScheduleMsg(reply, 0);

                                /* Set "experm" flag for intervention clear message */
                                if (experm) MsgSetExPerm(reply);
                                experm = 0;
                        }

                        /* Send intervention reply */
                        IntrvReplyHelper(mod, request, 1, dirty, experm, 0);
                }

                /* Free WB and late request */
                PrivateModuleFreeWB(prmod, wb);
                delete(request);
                return;
        }

        /* Handle late exclusive forward race */
        if (PrivateWBGetExFwdRaceReq(wb))
        {
                /* Check: WB must be servicing a shared writeback */
                assert(PrivateWBIsShWbReq(wb));

                /* Get laste exclusive forward race */
                Msg *request = PrivateWBGetExFwdRaceReq(wb);

                /* Update coherence statistics */
                PrivateModuleUpdateCoherenceStat(prmod, request, NULL);

                /* Free WB and late request */
                PrivateModuleFreeWB(prmod, wb);
                delete(request);
                return;
        }

        /* Handle late invalidation race */
        if (PrivateWBGetInvalRaceReq(wb))
        {
                /* Check: WB must be servicing a shared writeback */
                assert(PrivateWBIsShWbReq(wb));

                /* Get late invalidation race */
                Msg *request = PrivateWBGetInvalRaceReq(wb);

                /* Update coherence statistics */
                PrivateModuleUpdateCoherenceStat(prmod, request, NULL);

                /* When invalidations must wait for pending writebacks */
                if (invals_wait_for_pending_writebacks)
                {
                        /* Send invalidation reply message to requestor module */
                        Msg *reply = RMsgWithId(MsgGetId(request), InvalRep, tag, mod, request->requestor);
                        ScheduleMsg(reply, 0);
                }

                /* Free WB and late request */
                PrivateModuleFreeWB(prmod, wb);
                delete(request);
                return;
        }

        /* Handle late exclusive back invalidation race */
        if (PrivateWBGetExBkInvalRaceReq(wb))
        {
                /* Check: WB must not be servicing a shared writeback */
                assert(!PrivateWBIsShWbReq(wb));

                /* Get last exclusive back invalidation race */
                Msg *request = PrivateWBGetExBkInvalRaceReq(wb);

                /* Update coherence statistics */
                PrivateModuleUpdateCoherenceStat(prmod, request, NULL);

                /* When back invalidations must wait for pending writebacks */
                if (bkinvals_wait_for_pending_writebacks)
                {
                        /* Check: request must not be corrupt writeback back invalidation */
                        assert(!MsgIsCorruptWbBkInvalReq(request));

                        /* Determine type of back invalidation reply */
                        MsgType rtype = (dirty) ? DirtyData_ExBkInvalRep : NoData_ExBkInvalRep;

                        /* Send back invalidation reply message to sender module */
                        Msg *reply = RMsgWithId(MsgGetId(request), rtype, tag, mod, request->sender);
                        ScheduleMsg(reply, 0);

                        /* Set "experm" flag for back invalidation reply */
                        if (experm) MsgSetExPerm(reply);
                }

                /* Free WB and late request */
                PrivateModuleFreeWB(prmod, wb);
                delete(request);
                return;
        }

        /* Handle late shared back invalidation race */
        if (PrivateWBGetShBkInvalRaceReq(wb))
        {
                /* Check: WB must be servicing a shared writeback */
                assert(PrivateWBIsShWbReq(wb));

                /* Get late shared back invalidation race */
                Msg *request = PrivateWBGetShBkInvalRaceReq(wb);

                /* Update coherence statistics */
                PrivateModuleUpdateCoherenceStat(prmod, request, NULL);

                /* When back invalidations must wait for pending writebacks */
                if (bkinvals_wait_for_pending_writebacks)
                {
                        /* Check: request must not be corrupt writeback back invalidation */
                        assert(!MsgIsCorruptWbBkInvalReq(request));

                        /* Determine type of back invalidation reply */
                        MsgType rtype = NoData_ShBkInvalRep;

                        /* Send back invalidation reply message to sender module */
                        Msg *reply = RMsgWithId(MsgGetId(request), rtype, tag, mod, request->sender);
                        ScheduleMsg(reply, 0);
                }

                /* Free WB and late request */
                PrivateModuleFreeWB(prmod, wb);
                delete(request);
                return;
        }

        /* Determine type of writeback retry request */
        MsgType rtype = PrivateWBGetWbReq(wb);

        /* Send writeback reqeust to lower module after latency cycles */
        Msg *retry = RMsgWithId(id, rtype, tag, mod, ModuleGetLowModule(mod, tag));
        ScheduleMsg(retry, prmod->retry_latency);

        /* Set "experm" flag for writeback request, when flag is set for WB. */
        if (PrivateWBIsExPermSet(wb)) MsgSetExPerm(retry);

        /* Update writeback retry statistics */
        prmod->stats.writeback.retries++;
}


void DataWbNackHandler(Module *mod, Msg *nack)
{
        PrivateModule *prmod = asPrivateModule(mod);
        long long tag = MsgGetTag(nack);
        long long id = MsgGetId(nack);

        /* Locate WB servicing writeback request */
        PrivateWB *wb = PrivateModuleFindWB(prmod, tag);
        assert(wb && !PrivateWBWasAcked(wb));

        /* Update writeback NACK statistics */
        prmod->stats.writeback.nacks++;

        /* Determine type of writeback retry request */
        MsgType rtype = PrivateWBGetWbReq(wb);
        switch (PrivateWBGetWbReq(wb))
        {
                case ShPartData_ShWbReq:
                        /* Check: message must be block data writeback nack reply */
                        assert(MsgIsBlockDataWbNackRep(nack));

                case OwPartData_ShWbReq:
                        /* Check: message must either be block data or shared part data writeback nack reply */
                        assert(MsgIsBlockDataWbNackRep(nack) || MsgIsShPartDataWbNackRep(nack));

                case NoData_ShWbReq:
                        /* When messgae is block data writeback nack reply */
                        if (MsgIsBlockDataWbNackRep(nack))
                        {
                                rtype = CleanData_ShWbReq;
                                break;
                        }

                        /* When message is shared part data writeback nack reply */
                        if (MsgIsShPartDataWbNackRep(nack))
                        {
                                rtype = ShPartData_ShWbReq;
                                break;
                        }

                        /* Check: message must be owned part data writeback nack reply */
                        assert(MsgIsOwPartDataWbNackRep(nack));
                        rtype = OwPartData_ShWbReq;
                        break;

                case ShPartData_ExWbReq:
                        /* Check: message must be block data writeback nack reply */
                        assert(MsgIsBlockDataWbNackRep(nack));

                case OwPartData_ExWbReq:
                        /* Check: message must either be block data or shared part data writeback nack reply */
                        assert(MsgIsBlockDataWbNackRep(nack) || MsgIsShPartDataWbNackRep(nack));

                case NoData_ExWbReq:
                        /* When messgae is block data writeback nack reply */
                        if (MsgIsBlockDataWbNackRep(nack))
                        {
                                rtype = CleanData_ExWbReq;
                                break;
                        }

                        /* When message is shared part data writeback nack reply */
                        if (MsgIsShPartDataWbNackRep(nack))
                        {
                                rtype = ShPartData_ExWbReq;
                                break;
                        }

                        /* Check: message must be owned part data writeback nack reply */
                        assert(MsgIsOwPartDataWbNackRep(nack));
                        rtype = OwPartData_ExWbReq;
                        break;

                default:
                        panic("%s:%d invalid writeback request type", __FILE__, __LINE__);
                        break;
        }

        /*  Send writeback retry request message to home module */
        Msg *retry = RMsgWithId(id, rtype, tag, mod, ModuleGetLowModule(mod, tag));
        ScheduleMsg(retry, 0);

        /* Set "experm" flag for writeback request, when flag is set for WB. */
        if (PrivateWBIsExPermSet(wb)) MsgSetExPerm(retry);

        /* Update writeback retry statistics */
        prmod->stats.writeback.retries++;

        /* Free nack reply */
        delete(nack);
}


void InvalReqHandler(Module *mod, Msg *request)
{
        PrivateModule *prmod = asPrivateModule(mod);
        long long tag = MsgGetTag(request);
        unsigned int latency = 0;

        /* Check: requestor must be valid */
        assert(request->requestor);

	coherenceMessageCount[MsgGetType(request)]++;

        /* Miss request - early invalidation race */
        PrivateMSHR *mshr = PrivateModuleFindMSHR(prmod, tag);
        if (mshr)
        {
		coherenceMessageHits[MsgGetType(request)]++;
                /* Set invalidation race message in MSHR */
                PrivateMSHRSetRaceReq(mshr, request);

                /* When invalidations must wait for pending miss requests */
                if (invals_wait_for_pending_miss_requests)
                        return;

                /* Send invalidation reply to requestor immediately */
                Msg *reply = RMsgWithId(MsgGetId(request), InvalRep, tag, mod, request->requestor);
                ScheduleMsg(reply, 0);
                return;
        }

        /* Writeback - late invalidation race */
        PrivateWB *wb = PrivateModuleFindWB(prmod, tag);
        if (wb)
        {
                /* Check: WB must be servicing a shared writeback */
                assert(PrivateWBIsShWbReq(wb));

                /* Check: WB must not have received an acknowledgment */
                assert(!PrivateWBWasAcked(wb));

                /* Set invalidation race message in WB */
                PrivateWBSetRaceReq(wb, request);

                /* When invalidations must wait for pending writebacks */
                if (invals_wait_for_pending_writebacks)
                        return;
        }
        else
        {
                PrivateBlock *pblks[NumPrivateCacheTypes] = { NULL };
                PrivateCache **pcaches = prmod->caches;
                unsigned int present = 0;

                /* Locate block in module caches */
                for (PrivateCacheType index = 0; index < NumPrivateCacheTypes; index++)
                {
                        /* Skip if cache is not being modeled or if block is not present in cache */
                        if (!pcaches[index]) continue;
                        latency = MAX(latency, PrivateCacheGetLatency(pcaches[index]));
                        pblks[index] = PrivateCacheFindBlock(pcaches[index], tag);
                        if (!pblks[index]) continue;

                        /* Check: "experm" flag must not be set for cache
                         * block and it must be in shared state. */
                        assert(!PrivateBlockIsExPermSet(pblks[index]));
                        assert(PrivateBlockIsShared(pblks[index]));

                        /* Record the state of cache block */
                        present = 1;
                }

                /* Update coherence statistics */
                PrivateModuleUpdateCoherenceStat(prmod, request, pblks);

                /* Update block access histogram */
                PrivateModuleUpdateBlockAccessStat(prmod, pblks);

                /* Invalidate block from module caches */
                for (PrivateCacheType index = 0; index < NumPrivateCacheTypes; index++)
                        if (pblks[index]) {
				PrivateCacheInvalidateBlock(pcaches[index], pblks[index], CoherenceInvalidation);
				PrivateBlockResetPrefetched(pblks[index]);
			}

                /* Notify CPU about block being invalidated from module.
                 * This is required for enforcing memory consistency. */
                if (present) {
			cpu_invalidate(prmod->core, prmod->thread, tag);
			coherenceMessageHits[MsgGetType(request)]++;
		}
        }

        /* Send invalidation reply message to requestor module */
        Msg *reply = RMsgWithId(MsgGetId(request), InvalRep, tag, mod, request->requestor);
        ScheduleMsg(reply, latency);

        /* Free request */
        if (!wb) delete(request);
}


void ShIntrvReqHandler(Module *mod, Msg *request)
{
        PrivateModule *prmod = asPrivateModule(mod);
        long long tag = MsgGetTag(request);
        unsigned int latency = 0;

        unsigned int present = 0;
        unsigned int experm = 0;
        unsigned int dirty = 0;

        /* Check: requestor must be valid */
        assert(request->requestor);

        /* Miss request - early intervention race */
        PrivateMSHR *mshr = PrivateModuleFindMSHR(prmod, tag);
        if (mshr)
        {
                /* Set intervention race message in MSHR */
                PrivateMSHRSetRaceReq(mshr, request);
                return;
        }

        /* Writeback - late intervention race */
        PrivateWB *wb = PrivateModuleFindWB(prmod, tag);
        if (wb)
        {
                /* Check: WB must not be servicing a shared writeback */
                assert(!PrivateWBIsShWbReq(wb));

                /* Set intervention race message in WB */
                PrivateWBSetRaceReq(wb, request);

                /* When interventions must wait for pending writebacks */
                if (intrvs_wait_for_pending_writebacks)
                        return;

                /* Set writeback status flags */
                dirty = PrivateWBIsDirtyDataWbReq(wb);
                experm = PrivateWBIsExPermSet(wb);
                present = 1;
        }
        else
        {
                PrivateBlock *pblks[NumPrivateCacheTypes] = { NULL };
                PrivateCache **pcaches = prmod->caches;

                /* Locate block in module caches */
                for (PrivateCacheType index = 0; index < NumPrivateCacheTypes; index++)
                {
                        /* Skip if cache is not being modeled or if block is not present in cache */
                        if (!pcaches[index]) continue;
                        latency = MAX(latency, PrivateCacheGetLatency(pcaches[index]));
                        pblks[index] = PrivateCacheFindBlock(pcaches[index], tag);
                        if (!pblks[index]) continue;

                        /* Check: block must not be in shared state */
                        assert(!PrivateBlockIsShared(pblks[index]));

                        /* Record the state of cache block */
                        if (PrivateBlockIsExPermSet(pblks[index])) experm = 1;
                        if (PrivateBlockIsModified(pblks[index])) dirty = 1;
                        present = 1;
                }

                /* Update coherence statistics */
                PrivateModuleUpdateCoherenceStat(prmod, request, pblks);

                /* Update cache blocks to shared state */
                for (PrivateCacheType index = 0; index < NumPrivateCacheTypes; index++)
                {
                        if (!pblks[index]) continue;
                        PrivateBlockSetShared(pblks[index]);
                        PrivateBlockClearExPerm(pblks[index]);
                }
        }

        /* Check: block must be present in module caches
         * for non broadcast intervention requests. */
        assert(MsgIsBrdcstIntrvReq(request) || present);

        /* When optimized interventions are enabled */
        if (intrvs_opt)
        {
                /* Check: request must not be broadcast intervention */
                assert(!MsgIsBrdcstIntrvReq(request));

                /* Determine type of intervention clear message */
                MsgType mtype = NoData_ShIntrvClr;
                if (dirty)
                {
                        /* When message is optimized intervention request */
                        if (MsgIsOptIntrvReq(request)) mtype = DirtyNoData_ShIntrvClr;
                        else mtype = DirtyData_ShIntrvClr;
                        dirty = 0;
                }
                else if (MsgIsWbIntrvReq(request))  mtype = CleanData_ShIntrvClr;
                else if (MsgIsShCorruptMoveIntrvReq(request)) mtype = ShPartData_ShIntrvClr;
                else if (MsgIsOwCorruptMoveIntrvReq(request)) mtype = OwPartData_ShIntrvClr;

                /* Send intervention clear message after latency cycles */
                Msg *reply = RMsgWithId(MsgGetId(request), mtype, tag, mod, request->sender);
                ScheduleMsg(reply, latency);

                /* Set "experm" flag for intervention clear message */
                if (experm) MsgSetExPerm(reply);
                experm = 0;
        }

        /* Send intervention reply message */
        IntrvReplyHelper(mod, request, present, dirty, experm, latency);

        /* Free request */
        if (!wb) delete(request);
}


void ExIntrvReqHandler(Module *mod, Msg *request)
{
        PrivateModule *prmod = asPrivateModule(mod);
        long long tag = MsgGetTag(request);
        unsigned int latency = 0;

        unsigned int present = 0;
        unsigned int experm = 0;
        unsigned int dirty = 0;

        /* Check: requestor must be valid */
        assert(request->requestor);

        /* Miss request - early intervention race */
        PrivateMSHR *mshr = PrivateModuleFindMSHR(prmod, tag);
        if (mshr)
        {
                /* Set intervention race message in MSHR */
                PrivateMSHRSetRaceReq(mshr, request);
                return;
        }

        /* Writeback - late intervention race */
        PrivateWB *wb = PrivateModuleFindWB(prmod, tag);
        if (wb)
        {
                /* Check: WB must not be servicing a shared writeback */
                assert(!PrivateWBIsShWbReq(wb));

                /* Set intervention race message in WB */
                PrivateWBSetRaceReq(wb, request);

                /* When interventions must wait for pending writebacks */
                if (intrvs_wait_for_pending_writebacks)
                        return;

                /* Set writeback status flags */
                dirty = PrivateWBIsDirtyDataWbReq(wb);
                experm = PrivateWBIsExPermSet(wb);
                present = 1;
        }
        else
        {
                PrivateBlock *pblks[NumPrivateCacheTypes] = { NULL };
                PrivateCache **pcaches = prmod->caches;

                /* Locate block in module caches */
                for (PrivateCacheType index = 0; index < NumPrivateCacheTypes; index++)
                {
                        /* Skip if cache is not being modeled or if block is not present in cache */
                        if (!pcaches[index]) continue;
                        latency = MAX(latency, PrivateCacheGetLatency(pcaches[index]));
                        pblks[index] = PrivateCacheFindBlock(pcaches[index], tag);
                        if (!pblks[index]) continue;

                        /* Check: block must not be in shared state */
                        assert(!PrivateBlockIsShared(pblks[index]));

                        /* Record the state of cache block */
                        if (PrivateBlockIsExPermSet(pblks[index])) experm = 1;
                        if (PrivateBlockIsModified(pblks[index])) dirty = 1;
                        present = 1;
                }

                /* Update coherence statistics */
                PrivateModuleUpdateCoherenceStat(prmod, request, pblks);

                /* Update block access histogram */
                PrivateModuleUpdateBlockAccessStat(prmod, pblks);

                /* Invalidate block from module caches */
                for (PrivateCacheType index = 0; index < NumPrivateCacheTypes; index++)
                        if (pblks[index]) {
				PrivateCacheInvalidateBlock(pcaches[index], pblks[index], CoherenceInvalidation);
				PrivateBlockResetPrefetched(pblks[index]);
			}

                /* Notify CPU about block being invalidated from module.
                 * This is required for enforcing memory consistency. */
                if (present) cpu_invalidate(prmod->core, prmod->thread, tag);
        }

        /* Check: block must be present in module caches
         * for non broadcast intervention requests. */
        assert(MsgIsBrdcstIntrvReq(request) || present);

        /* When optimized interventions are enabled */
        if (intrvs_opt)
        {
                /* Check: request must not be broadcast intervention */
                assert(!MsgIsBrdcstIntrvReq(request));

                /* Determine type of intervention clear message */
                MsgType mtype = NoData_ExIntrvClr;
                if (MsgIsWbIntrvReq(request))
                {
                        mtype = dirty ? DirtyData_ExIntrvClr : CleanData_ExIntrvClr;
                        dirty = 0;
                }
                else if (MsgIsCorruptMoveIntrvReq(request))
                {
                        if (dirty) mtype = DirtyData_ExIntrvClr;
                        else if (MsgIsShCorruptMoveIntrvReq(request)) mtype = ShPartData_ExIntrvClr;
                        else mtype = OwPartData_ExIntrvClr;
                        dirty = 0;
                }

                /* Send intervention clear message after latency cycles */
                Msg *reply = RMsgWithId(MsgGetId(request), mtype, tag, mod, request->sender);
                ScheduleMsg(reply, latency);

                /* Set "experm" flag for intervention clear message */
                if (experm) MsgSetExPerm(reply);
                experm = 0;
        }

        /* Send intervention reply message */
        IntrvReplyHelper(mod, request, present, dirty, experm, latency);

        /* Free request */
        if (!wb) delete(request);
}


void ShFwdReqHandler(Module *mod, Msg *request)
{
        PrivateModule *prmod = asPrivateModule(mod);
        long long tag = MsgGetTag(request);
        unsigned int latency = 0;

        /* Check: requestor must be valid */
        assert(request->requestor);

        /* Miss request - early forward race */
        PrivateMSHR *mshr = PrivateModuleFindMSHR(prmod, tag);
        if (mshr)
        {
                /* Set forward race message in MSHR */
                PrivateMSHRSetRaceReq(mshr, request);
                return;
        }

        /* Writeback - late forward race */
        PrivateWB *wb = PrivateModuleFindWB(prmod, tag);
        if (wb)
        {
                /* Check: WB must be servicing a shared writeback */
                assert(PrivateWBIsShWbReq(wb));

                /* Record late race request in WB */
                PrivateWBSetRaceReq(wb, request);
        }
        else
        {
                PrivateBlock *pblks[NumPrivateCacheTypes] = { NULL };
                PrivateCache **pcaches = prmod->caches;
                unsigned int present = 0;

                /* Locate block in private module caches */
                for (PrivateCacheType index = 0; index < NumPrivateCacheTypes; index++)
                {
                        /* Skip if cache is not being modeled or if block is not present in cache */
                        if (!pcaches[index]) continue;
                        latency = MAX(latency, PrivateCacheGetLatency(pcaches[index]));
                        pblks[index] = PrivateCacheFindBlock(pcaches[index], tag);
                        if (!pblks[index]) continue;

                        /* Check: "experm" flag must not be set for cache
                         * block and it must be in shared state. */
                        assert(!PrivateBlockIsExPermSet(pblks[index]));
                        assert(PrivateBlockIsShared(pblks[index]));

                        /* Set the "present" flag */
                        present = 1;
                }

                /* Check: block must be present in private caches */
                assert(present);

                /* Update coherence statistics */
                PrivateModuleUpdateCoherenceStat(prmod, request, pblks);
        }

        /* Send forward reply message */
        FwdReplyHelper(mod, request, latency);

        /* Free request */
        if (!wb) delete(request);
}


void ExFwdReqHandler(Module *mod, Msg *request)
{
        PrivateModule *prmod = asPrivateModule(mod);
        long long tag = MsgGetTag(request);
        unsigned int latency = 0;

        /* Check: requestor must be valid */
        assert(request->requestor);

        /* Miss request - early forward race */
        PrivateMSHR *mshr = PrivateModuleFindMSHR(prmod, tag);
        if (mshr)
        {
                /* Set forward race message in MSHR */
                PrivateMSHRSetRaceReq(mshr, request);
                return;
        }

        /* Writeback - late forward request race */
        PrivateWB *wb = PrivateModuleFindWB(prmod, tag);
        if (wb)
        {
                /* Check: WB must be servicing a shared writeback */
                assert(PrivateWBIsShWbReq(wb));

                /* Record late race request in WB */
                PrivateWBSetRaceReq(wb, request);
        }
        else
        {
                PrivateBlock *pblks[NumPrivateCacheTypes] = { NULL };
                PrivateCache **pcaches = prmod->caches;
                unsigned int present = 0;

                /* Locate block in private module caches */
                for (PrivateCacheType index = 0; index < NumPrivateCacheTypes; index++)
                {
                        /* Skip if cache is not being modeled or if block is not present in cache */
                        if (!pcaches[index]) continue;
                        latency = MAX(latency, PrivateCacheGetLatency(pcaches[index]));
                        pblks[index] = PrivateCacheFindBlock(pcaches[index], tag);
                        if (!pblks[index]) continue;

                        /* Check: "experm" flag must not be set for cache
                         * block and it must be in shared state. */
                        assert(!PrivateBlockIsExPermSet(pblks[index]));
                        assert(PrivateBlockIsShared(pblks[index]));

                        /* Set the "present" flag */
                        present = 1;
                }

                /* Check: block must be present in private caches */
                assert(present);

                /* Update coherence statistics */
                PrivateModuleUpdateCoherenceStat(prmod, request, pblks);

                /* Update block access histogram */
                PrivateModuleUpdateBlockAccessStat(prmod, pblks);

                /* Invalidate block from module caches */
                for (PrivateCacheType index = 0; index < NumPrivateCacheTypes; index++)
                        if (pblks[index]) {
				PrivateCacheInvalidateBlock(pcaches[index], pblks[index], CoherenceInvalidation);
				PrivateBlockResetPrefetched(pblks[index]);
			}

                /* Notify CPU about block being invalidated from module.
                 * This is required for enforcing memory consistency. */
                cpu_invalidate(prmod->core, prmod->thread, tag);
        }

        /* Send forward reply message */
        FwdReplyHelper(mod, request, latency);

        /* Free request */
        if (!wb) delete(request);
}


void ExBkInvalReqHandler(Module *mod, Msg *request)
{
        PrivateModule *prmod = asPrivateModule(mod);
        long long tag = MsgGetTag(request);
        unsigned int latency = 0;

        unsigned int present = 0;
        unsigned int experm = 0;
        unsigned int dirty = 0;

	coherenceMessageCount[MsgGetType(request)]++;

        /* Miss request - early back invalidation race */
        PrivateMSHR *mshr = PrivateModuleFindMSHR(prmod, tag);
        if (mshr)
        {
		coherenceMessageHits[MsgGetType(request)]++;
                /* Set back invalidation race message in MSHR */
                PrivateMSHRSetRaceReq(mshr, request);
                return;
        }

        /* Writeback - late back invalidation race */
        PrivateWB *wb = PrivateModuleFindWB(prmod, tag);
        if (wb)
        {
                /* Check: WB must not be servicing shared writeback */
                assert(!PrivateWBIsShWbReq(wb));

                /* Set back invalidation race message in WB */
                PrivateWBSetRaceReq(wb, request);

                /* When back invalidations must wait for pending writebacks */
                if (bkinvals_wait_for_pending_writebacks)
                        return;

                /* Set writeback status flags */
                dirty = PrivateWBIsDirtyDataWbReq(wb);
                experm = PrivateWBIsExPermSet(wb);
                present = 1;
        }
        else
        {
                PrivateBlock *pblks[NumPrivateCacheTypes] = { NULL };
                PrivateCache **pcaches = prmod->caches;

                /* Locate block in module caches */
                for (PrivateCacheType index = 0; index < NumPrivateCacheTypes; index++)
                {
                        /* Skip if cache is not being modeled or if block is not present in cache */
                        if (!pcaches[index]) continue;
                        latency = MAX(latency, PrivateCacheGetLatency(pcaches[index]));
                        pblks[index] = PrivateCacheFindBlock(pcaches[index], tag);
                        if (!pblks[index]) continue;

                        /* Check: block must not be in shared state */
                        assert(!PrivateBlockIsShared(pblks[index]));

                        /* Record the state of cache block */
                        if (PrivateBlockIsExPermSet(pblks[index])) experm = 1;
                        if (PrivateBlockIsModified(pblks[index])) dirty = 1;
                        present = 1;
                }

                /* Update coherence statistics */
                PrivateModuleUpdateCoherenceStat(prmod, request, pblks);

                /* Update block access histogram */
                PrivateModuleUpdateBlockAccessStat(prmod, pblks);

                /* Invalidate block from module caches */
                for (PrivateCacheType index = 0; index < NumPrivateCacheTypes; index++)
                        if (pblks[index]) {
				PrivateCacheInvalidateBlock(pcaches[index], pblks[index], CoherenceInvalidation);
				PrivateBlockResetPrefetched(pblks[index]);
			}

                /* Notify CPU about block being invalidated from module.
                 * This is required for enforcing memory consistency. */
                if (present) {
			cpu_invalidate(prmod->core, prmod->thread, tag);
			coherenceMessageHits[MsgGetType(request)]++;
		}
        }

        /* Determine type of back invalidation reply */
        MsgType mtype = NoData_ExBkInvalRep;
        if (MsgIsCorruptWbBkInvalReq(request))
        {
                /* Check: block must be present in private module */
                assert(present);
                mtype = (MsgIsShCorruptWbBkInvalReq(request)) ? ShPartData_ExBkInvalRep : OwPartData_ExBkInvalRep;
        }
        if (dirty) mtype = DirtyData_ExBkInvalRep;

        /* Send back invalidation reply message to sender module */
        Msg *reply = RMsgWithId(MsgGetId(request), mtype, tag, mod, request->sender);
        ScheduleMsg(reply, latency);

        /* Set "experm" flag for back invalidation reply */
        if (experm) MsgSetExPerm(reply);

        /* Free request */
        if (!wb) delete(request);
}


void ShBkInvalReqHandler(Module *mod, Msg *request)
{
        PrivateModule *prmod = asPrivateModule(mod);
        long long tag = MsgGetTag(request);
        unsigned int latency = 0;
        unsigned int present = 0;

        /* Check: forwarder module must be valid, when request is corrupt writeback back invalidation */
        assert(!MsgIsCorruptWbBkInvalReq(request) || request->forwarder);

	coherenceMessageCount[MsgGetType(request)]++;

        /* Miss request - early back invalidation race */
        PrivateMSHR *mshr = PrivateModuleFindMSHR(prmod, tag);
        if (mshr)
        {
		coherenceMessageHits[MsgGetType(request)]++;
                /* Set back invalidation race message in MSHR */
                PrivateMSHRSetRaceReq(mshr, request);
                return;
        }

        /* Writeback - late back invalidation race */
        PrivateWB *wb = PrivateModuleFindWB(prmod, tag);
        if (wb)
        {
                /* Check: WB must be servicing shared writeback */
                assert(PrivateWBIsShWbReq(wb));

                /* Set back invalidation race message in WB */
                PrivateWBSetRaceReq(wb, request);

                /* When back invalidations must wait for pending writebacks */
                if (bkinvals_wait_for_pending_writebacks)
                        return;

                /* Set writeback status flags */
                present = 1;
        }
        else
        {
                PrivateBlock *pblks[NumPrivateCacheTypes] = { NULL };
                PrivateCache **pcaches = prmod->caches;

                /* Locate block in module caches */
                for (PrivateCacheType index = 0; index < NumPrivateCacheTypes; index++)
                {
                        /* Skip if cache is not being modeled or if block is not present in cache */
                        if (!pcaches[index]) continue;
                        latency = MAX(latency, PrivateCacheGetLatency(pcaches[index]));
                        pblks[index] = PrivateCacheFindBlock(pcaches[index], tag);
                        if (!pblks[index]) continue;

                        /* Check: "experm" flag must not be set for cache
                         * block and it must be in shared state. */
                        assert(!PrivateBlockIsExPermSet(pblks[index]));
                        assert(PrivateBlockIsShared(pblks[index]));

                        /* Record the state of cache block */
                        present = 1;
                }

                /* Update coherence statistics */
                PrivateModuleUpdateCoherenceStat(prmod, request, pblks);

                /* Update block access histogram */
                PrivateModuleUpdateBlockAccessStat(prmod, pblks);

                /* Invalidate block from module caches */
                for (PrivateCacheType index = 0; index < NumPrivateCacheTypes; index++)
                        if (pblks[index]) {
				PrivateCacheInvalidateBlock(pcaches[index], pblks[index], CoherenceInvalidation);
				PrivateBlockResetPrefetched(pblks[index]);
			}

                /* Notify CPU about block being invalidated from module.
                 * This is required for enforcing memory consistency. */
                if (present) {
			cpu_invalidate(prmod->core, prmod->thread, tag);
			coherenceMessageHits[MsgGetType(request)]++;
		}
        }

        /* Determine type of back invalidation reply */
        MsgType mtype = NoData_ShBkInvalRep;
        if (request->forwarder == mod)
        {
                /* Check: block must be present in private module */
                assert(present);

                /* Check: request must be corrupt writeback request */
                assert(MsgIsCorruptWbBkInvalReq(request));

                /* Set back invalidation reply type */
                mtype = (MsgIsShCorruptWbBkInvalReq(request)) ? ShPartData_ShBkInvalRep : OwPartData_ShBkInvalRep;
        }

        /* Send back invalidation reply message to sender module */
        Msg *reply = RMsgWithId(MsgGetId(request), mtype, tag, mod, request->sender);
        ScheduleMsg(reply, latency);

        /* Free request */
        if (!wb) delete(request);
}


