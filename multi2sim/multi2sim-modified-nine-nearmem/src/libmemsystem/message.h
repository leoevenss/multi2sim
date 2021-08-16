#ifndef MEM_SYSTEM_MESSAGE_H
#define	MEM_SYSTEM_MESSAGE_H

#include <libesim/esim.h>
#include <libclass/class.h>
#include <libcpuarch/uop.h>

#include "mmu.h"
#include "accesses.h"
#include "message-type.h"


/*
 * Msg Handler
 */

typedef void (*MsgHanlder) (Module *mod, Msg *msg);


/*
 * Class 'Msg'
 */

CLASS_BEGIN(Msg, Object)

        long long id; /* message identifier */
        MsgType type; /* message type */

        long long tag; /* tag of block being requested by message */
        long long pte; /* pte address of block being requested by message */

        Module *sender; /* sender module */
        Module *receiver; /* receiver module */
        Module *requestor; /* requestor module */
        Module *forwarder; /* forwarder module */

        /* Network variables, parameters & statistics - are only for messages
         * going through network. A message only travels through network if the
         * sender and receiver modules are different. */
        int hops; /* network hops */
        long latency; /* network latency in cycles */
        long long wait_latency; /* network wait latency in cycles */

        struct net_msg_t *netpkt; /* network packet */

        /* Data being carried by message */
        struct
        {
                unsigned int pte : 1; /* page table entry */
                unsigned int block : 1; /* instruction/data block */
                unsigned int dirty : 1; /* dirty instruction/data block */
        } data;

        /* This variable is used in CoreReq, ExReq, ShReq, PteReq and DramLoadReq
         * messages. It provides access information for the memory access. */
        Access *access;

        /* This variable is only used for CoreReq, ExReq, ShReq, PteNack and
         * BlkNack messages. If set it indicates the request has been waiting
         * for either buffer allocation or for request being serviced by a
         * buffer to complete. */
        unsigned int buffer_wait : 1;

        /* This variable is valid only for ExReq messages. If set it indicates
         * that the exclusive request is actually an upgrade request. */
        unsigned int upgrade : 1;

        /* This variables is valid for WbReq and intervention reply messages. If set it
         * indicates that sender module required exclusive permission for the block. */
        unsigned int experm : 1;

        /* This variable is valid for ExRep and ShRep messages. It indicates
         * the number of intervention and invalidation replies that must be
         * collected by the requestor module before the completion of
         * miss request. */
        unsigned int replies;

        MsgList *msg_list; /* message list currently containing message */
        Msg *msg_list_prev; /* message list previous pointer */
        Msg *msg_list_next; /* message list next pointer */

        /* Bucket list of accesses in hash table in 'module' */
        Msg *bucket_list_prev;
        Msg *bucket_list_next;

        /* This variable is valid for intervention, invalidation and back invalidation messages.
         * It indiciates the esim clock cycle since when a race messages have been waiting
         * in MSHR and WB for pending requests to complete. */
        struct {
                unsigned int late : 1;
                unsigned int early : 1;
                long long wait_since_cycle;
        } race;

        /* TLB latency hack */
        long long tlb_cycle[2];

CLASS_END(Msg)

void MsgCreate(Msg *msg, long long id, MsgType type, long long address, Module *sender, Module *receiver);
void MsgDestroy(Msg *msg);

static inline unsigned int MsgIsLocal(Msg *msg) { return (msg->sender == msg->receiver) ? 1 : 0; }

static inline long long MsgGetId(Msg *msg) { return msg->id; }
static inline long long MsgGetTag(Msg *msg) { return msg->tag; }
static inline long long MsgGetPTE(Msg *msg) { if (msg->pte < 0) msg->pte = mmu_get_pte_addr(msg->tag); return msg->pte; }

/* TLB latency hack */
static inline void MsgEndTLBAccess(Msg *msg) { if (msg->tlb_cycle[1] < 0) msg->tlb_cycle[1] = esim_cycle; }
static inline void MsgBeginTLBAccess(Msg *msg) { if (msg->tlb_cycle[0] < 0) msg->tlb_cycle[0] = esim_cycle; }
static inline long long MsgGetTLBLatency(Msg *msg) { assert((msg->tlb_cycle[0] >= 0) && (msg->tlb_cycle[1] >= msg->tlb_cycle[0]))return 0;}//return msg->tlb_cycle[1] - msg->tlb_cycle[0]; }// leo edit

/* Msg Buffer Wait */
static inline unsigned int MsgWasWaitingForBuffer(Msg *msg) { return msg->buffer_wait ? 1 : 0; }
static inline void MsgSetWaitingForBuffer(Msg *msg) { msg->buffer_wait = 1; }

/* Msg Upgrade Request */
static inline unsigned int MsgIsUpgrReq(Msg *msg) { return msg->upgrade ? 1 : 0; }
static inline void MsgSetUpgrReq(Msg *msg) { msg->upgrade = 1; }

/*
 * Helper functions for managing 'experm' flag
 */

/* Returns 1 if "experm" flag is set, else return 0. */
static inline unsigned int MsgIsExPermSet(Msg *msg)
{
        return msg->experm ? 1 : 0;
}

/* Set "expermission" flag */
static inline void MsgSetExPerm(Msg *msg)
{
        msg->experm = 1;
}


/* Msg Replies */
static inline void MsgIncrementReplies(Msg *msg) { msg->replies++; }
static inline unsigned int MsgGetReplies(Msg *msg) { return msg->replies; }
static inline void MsgSetReplies(Msg *msg, unsigned int replies) { msg->replies = replies; }


/*
 * Helper function for managing race wait in message
 */

static inline void MsgSetEarlyRaceWait(Msg *msg, unsigned int recordWaitCycles)
{
        /* Check: race wait must not be set for message */
        assert(!msg->race.late || !msg->race.early);

        msg->race.wait_since_cycle = (recordWaitCycles) ? esim_cycle : -1;
        msg->race.early = 1;
}

static inline void MsgSetLateRaceWait(Msg *msg, unsigned int recordWaitCycles)
{
        /* Check: race wait must not be set for message */
        assert(!msg->race.late || !msg->race.early);

        msg->race.wait_since_cycle = (recordWaitCycles) ? esim_cycle : -1;
        msg->race.late = 1;
}

static inline unsigned int MsgIsEarlyRaceWait(Msg *msg)
{
        return (msg->race.early) ? 1 : 0;
}

static inline unsigned int MsgIsLateRaceWait(Msg *msg)
{
        return (msg->race.late) ? 1 : 0;
}

static inline long long MsgGetRaceWaitCycles(Msg *msg)
{
        /* Check: race wait must be set for message */
        assert(msg->race.late || msg->race.early);

        if (msg->race.wait_since_cycle < 0) return 0;

        assert(msg->race.wait_since_cycle <= esim_cycle);
        return esim_cycle - msg->race.wait_since_cycle;
}


/*
 * Helper functions for managing message type
 */

static inline MsgType MsgGetType(Msg *msg) { return msg->type; }
static inline char* MsgMapTyp(Msg *msg) { return MapMsgType(MsgGetType(msg)); }

/* Back invalidation requests and replies */
static inline unsigned int MsgIsBkInvalReq(Msg *msg) { return MsgTypeIsBkInvalReq(MsgGetType(msg)); }
static inline unsigned int MsgIsBkInvalRep(Msg *msg) { return MsgTypeIsBkInvalRep(MsgGetType(msg)); }

static inline unsigned int MsgIsShBkInvalReq(Msg *msg) { return MsgTypeIsShBkInvalReq(MsgGetType(msg)); }
static inline unsigned int MsgIsShBkInvalRep(Msg *msg) { return MsgTypeIsShBkInvalRep(MsgGetType(msg)); }

static inline unsigned int MsgIsExBkInvalReq(Msg *msg) { return MsgTypeIsExBkInvalReq(MsgGetType(msg)); }
static inline unsigned int MsgIsExBkInvalRep(Msg *msg) { return MsgTypeIsExBkInvalRep(MsgGetType(msg)); }

static inline unsigned int MsgIsCorruptWbBkInvalReq(Msg *msg) { return MsgTypeIsCorruptWbBkInvalReq(MsgGetType(msg)); }
static inline unsigned int MsgIsOwCorruptWbBkInvalReq(Msg *msg) { return MsgTypeIsOwCorruptWbBkInvalReq(MsgGetType(msg)); }
static inline unsigned int MsgIsShCorruptWbBkInvalReq(Msg *msg) { return MsgTypeIsShCorruptWbBkInvalReq(MsgGetType(msg)); }

static inline unsigned int MsgIsNoDataBkInvalRep(Msg *msg) { return MsgTypeIsNoDataBkInvalRep(MsgGetType(msg)); }
static inline unsigned int MsgIsOwPartDataBkInvalRep(Msg *msg) { return MsgTypeIsOwPartDataBkInvalRep(MsgGetType(msg)); }
static inline unsigned int MsgIsShPartDataBkInvalRep(Msg *msg) { return MsgTypeIsShPartDataBkInvalRep(MsgGetType(msg)); }
static inline unsigned int MsgIsDirtyDataBkInvalRep(Msg *msg) { return MsgTypeIsDirtyDataBkInvalRep(MsgGetType(msg)); }

/* Intervention request and replies */
static inline unsigned int MsgIsIntrvReq(Msg *msg) { return MsgTypeIsIntrvReq(MsgGetType(msg)); }
static inline unsigned int MsgIsIntrvRep(Msg *msg) { return MsgTypeIsIntrvRep(MsgGetType(msg)); }

static inline unsigned int MsgIsShIntrvReq(Msg *msg) { return MsgTypeIsShIntrvReq(MsgGetType(msg)); }
static inline unsigned int MsgIsShIntrvRep(Msg *msg) { return MsgTypeIsShIntrvRep(MsgGetType(msg)); }

static inline unsigned int MsgIsExIntrvReq(Msg *msg) { return MsgTypeIsExIntrvReq(MsgGetType(msg)); }
static inline unsigned int MsgIsExIntrvRep(Msg *msg) { return MsgTypeIsExIntrvRep(MsgGetType(msg)); }

static inline unsigned int MsgIsWbIntrvReq(Msg *msg) { return MsgTypeIsWbIntrvReq(MsgGetType(msg)); }
static inline unsigned int MsgIsWbIntrvRep(Msg *msg) { return MsgTypeIsWbIntrvRep(MsgGetType(msg)); }

static inline unsigned int MsgIsBrdcstIntrvReq(Msg *msg) { return MsgTypeIsBrdcstIntrvReq(MsgGetType(msg)); }
static inline unsigned int MsgIsBrdcstIntrvRep(Msg *msg) { return MsgTypeIsBrdcstIntrvRep(MsgGetType(msg)); }

static inline unsigned int MsgIsOptIntrvReq(Msg *msg) { return MsgTypeIsOptIntrvReq(MsgGetType(msg)); }
static inline unsigned int MsgIsOptIntrvRep(Msg *msg) { return MsgTypeIsOptIntrvRep(MsgGetType(msg)); }

static inline unsigned int MsgIsCorruptWbIntrvReq(Msg *msg) { return MsgTypeIsCorruptWbIntrvReq(MsgGetType(msg)); }
static inline unsigned int MsgIsCorruptWbIntrvRep(Msg *msg) { return MsgTypeIsCorruptWbIntrvRep(MsgGetType(msg)); }

static inline unsigned int MsgIsShCorruptWbIntrvReq(Msg *msg) { return MsgTypeIsShCorruptWbIntrvReq(MsgGetType(msg)); }
static inline unsigned int MsgIsShCorruptWbIntrvRep(Msg *msg) { return MsgTypeIsShCorruptWbIntrvRep(MsgGetType(msg)); }

static inline unsigned int MsgIsOwCorruptWbIntrvReq(Msg *msg) { return MsgTypeIsOwCorruptWbIntrvReq(MsgGetType(msg)); }
static inline unsigned int MsgIsOwCorruptWbIntrvRep(Msg *msg) { return MsgTypeIsOwCorruptWbIntrvRep(MsgGetType(msg)); }

static inline unsigned int MsgIsCorruptMoveIntrvReq(Msg *msg) { return MsgTypeIsCorruptMoveIntrvReq(MsgGetType(msg)); }
static inline unsigned int MsgIsCorruptMoveIntrvRep(Msg *msg) { return MsgTypeIsCorruptMoveIntrvRep(MsgGetType(msg)); }

static inline unsigned int MsgIsShCorruptMoveIntrvReq(Msg *msg) { return MsgTypeIsShCorruptMoveIntrvReq(MsgGetType(msg)); }
static inline unsigned int MsgIsShCorruptMoveIntrvRep(Msg *msg) { return MsgTypeIsShCorruptMoveIntrvRep(MsgGetType(msg)); }

static inline unsigned int MsgIsOwCorruptMoveIntrvReq(Msg *msg) { return MsgTypeIsOwCorruptMoveIntrvReq(MsgGetType(msg)); }
static inline unsigned int MsgIsOwCorruptMoveIntrvRep(Msg *msg) { return MsgTypeIsOwCorruptMoveIntrvRep(MsgGetType(msg)); }

static inline unsigned int MsgIsNoDataIntrvRep(Msg *msg) { return MsgTypeIsNoDataIntrvRep(MsgGetType(msg)); }
static inline unsigned int MsgIsCleanDataIntrvRep(Msg *msg) { return MsgTypeIsCleanDataIntrvRep(MsgGetType(msg)); }
static inline unsigned int MsgIsDirtyDataIntrvRep(Msg *msg) { return MsgTypeIsDirtyDataIntrvRep(MsgGetType(msg)); }

static inline unsigned int MsgIsDataIntrvClr(Msg *msg) { return MsgTypeIsDataIntrvClr(MsgGetType(msg)); }
static inline unsigned int MsgIsNoDataIntrvClr(Msg *msg) { return MsgTypeIsNoDataIntrvClr(MsgGetType(msg)); }
static inline unsigned int MsgIsDirtyDataIntrvClr(Msg *msg) { return MsgTypeIsDirtyDataIntrvClr(MsgGetType(msg)); }
static inline unsigned int MsgIsDirtyNoDataIntrvClr(Msg *msg) { return MsgTypeIsDirtyNoDataIntrvClr(MsgGetType(msg)); }

/* Invalidation request and replies */
static inline unsigned int MsgIsInvalReq(Msg *msg) { return MsgTypeIsInvalReq(MsgGetType(msg)); }
static inline unsigned int MsgIsInvalRep(Msg *msg) { return MsgTypeIsInvalRep(MsgGetType(msg)); }

/* Forward request and reply messages */
static inline unsigned int MsgIsFwdReq(Msg *msg) { return MsgTypeIsFwdReq(MsgGetType(msg)); }
static inline unsigned int MsgIsFwdRep(Msg *msg) { return MsgTypeIsFwdRep(MsgGetType(msg)); }

static inline unsigned int MsgIsShFwdReq(Msg *msg) { return MsgTypeIsShFwdReq(MsgGetType(msg)); }
static inline unsigned int MsgIsShFwdRep(Msg *msg) { return MsgTypeIsShFwdRep(MsgGetType(msg)); }

static inline unsigned int MsgIsExFwdReq(Msg *msg) { return MsgTypeIsExFwdReq(MsgGetType(msg)); }
static inline unsigned int MsgIsExFwdRep(Msg *msg) { return MsgTypeIsExFwdRep(MsgGetType(msg)); }

static inline unsigned int MsgIsWbFwdReq(Msg *msg) { return MsgTypeIsWbFwdReq(MsgGetType(msg)); }
static inline unsigned int MsgIsWbFwdRep(Msg *msg) { return MsgTypeIsWbFwdRep(MsgGetType(msg)); }

static inline unsigned int MsgIsNormFwdReq(Msg *msg) { return MsgTypeIsNormFwdReq(MsgGetType(msg)); }
static inline unsigned int MsgIsNormFwdRep(Msg *msg) { return MsgTypeIsNormFwdRep(MsgGetType(msg)); }

static inline unsigned int MsgIsCorruptWbFwdReq(Msg *msg) { return MsgTypeIsCorruptWbFwdReq(MsgGetType(msg)); }
static inline unsigned int MsgIsCorruptWbFwdRep(Msg *msg) { return MsgTypeIsCorruptWbFwdRep(MsgGetType(msg)); }

static inline unsigned int MsgIsShCorruptWbFwdReq(Msg *msg) { return MsgTypeIsShCorruptWbFwdReq(MsgGetType(msg)); }
static inline unsigned int MsgIsShCorruptWbFwdRep(Msg *msg) { return MsgTypeIsShCorruptWbFwdRep(MsgGetType(msg)); }

static inline unsigned int MsgIsCorruptMoveFwdReq(Msg *msg) { return MsgTypeIsCorruptMoveFwdReq(MsgGetType(msg)); }
static inline unsigned int MsgIsCorruptMoveFwdRep(Msg *msg) { return MsgTypeIsCorruptMoveFwdRep(MsgGetType(msg)); }

static inline unsigned int MsgIsOwCorruptMoveFwdReq(Msg *msg) { return MsgTypeIsOwCorruptMoveFwdReq(MsgGetType(msg)); }
static inline unsigned int MsgIsOwCorruptMoveFwdRep(Msg *msg) { return MsgTypeIsOwCorruptMoveFwdRep(MsgGetType(msg)); }

static inline unsigned int MsgIsShCorruptMoveFwdReq(Msg *msg) { return MsgTypeIsShCorruptMoveFwdReq(MsgGetType(msg)); }
static inline unsigned int MsgIsShCorruptMoveFwdRep(Msg *msg) { return MsgTypeIsShCorruptMoveFwdRep(MsgGetType(msg)); }

static inline unsigned int MsgIsNoDataFwdClr(Msg *msg) { return MsgTypeIsNoDataFwdClr(MsgGetType(msg)); }
static inline unsigned int MsgIsCleanDataFwdClr(Msg *msg) { return MsgTypeIsCleanDataFwdClr(MsgGetType(msg)); }
static inline unsigned int MsgIsOwPartDataFwdClr(Msg *msg) { return MsgTypeIsOwPartDataFwdClr(MsgGetType(msg)); }
static inline unsigned int MsgIsShPartDataFwdClr(Msg *msg) { return MsgTypeIsShPartDataFwdClr(MsgGetType(msg)); }


/* Block get request and put reply messages */
static inline unsigned int MsgIsGetReq(Msg *msg) { return MsgTypeIsGetReq(MsgGetType(msg)); }
static inline unsigned int MsgIsPutRep(Msg *msg) { return MsgTypeIsPutRep(MsgGetType(msg)); }

static inline unsigned int MsgIsShGetReq(Msg *msg) { return MsgTypeIsShGetReq(MsgGetType(msg)); }
static inline unsigned int MsgIsShPutRep(Msg *msg) { return MsgTypeIsShPutRep(MsgGetType(msg)); }

static inline unsigned int MsgIsExGetReq(Msg *msg) { return MsgTypeIsExGetReq(MsgGetType(msg)); }
static inline unsigned int MsgIsExPutRep(Msg *msg) { return MsgTypeIsExPutRep(MsgGetType(msg)); }

/* Page table entry request and reply messages */
static inline unsigned int MsgIsPteReq(Msg *msg) { return MsgTypeIsPteReq(MsgGetType(msg)); }
static inline unsigned int MsgIsPteRep(Msg *msg) { return MsgTypeIsPteRep(MsgGetType(msg)); }

/* Writeback request and reply messages */
static inline unsigned int MsgIsWbReq(Msg *msg) { return MsgTypeIsWbReq(MsgGetType(msg)); }

static inline unsigned int MsgIsShWbReq(Msg *msg) { return MsgTypeIsShWbReq(MsgGetType(msg)); }
static inline unsigned int MsgIsExWbReq(Msg *msg) { return MsgTypeIsExWbReq(MsgGetType(msg)); }

static inline unsigned int MsgIsDataWbReq(Msg *msg) { return MsgTypeIsDataWbReq(MsgGetType(msg)); }
static inline unsigned int MsgIsNoDataWbReq(Msg *msg) { return MsgTypeIsNoDataWbReq(MsgGetType(msg)); }
static inline unsigned int MsgIsCleanDataWbReq(Msg *msg) { return MsgTypeIsCleanDataWbReq(MsgGetType(msg)); }
static inline unsigned int MsgIsDirtyDataWbReq(Msg *msg) { return MsgTypeIsDirtyDataWbReq(MsgGetType(msg)); }
static inline unsigned int MsgIsOwPartDataWbReq(Msg *msg) { return MsgTypeIsOwPartDataWbReq(MsgGetType(msg)); }
static inline unsigned int MsgIsShPartDataWbReq(Msg *msg) { return MsgTypeIsShPartDataWbReq(MsgGetType(msg)); }

static inline unsigned int MsgIsWbNackRep(Msg *msg) { return MsgTypeIsWbNackRep(MsgGetType(msg)); }
static inline unsigned int MsgIsNormWbNackRep(Msg *msg) { return MsgTypeIsNormWbNackRep(MsgGetType(msg)); }
static inline unsigned int MsgIsBlockDataWbNackRep(Msg *msg) { return MsgTypeIsBlockDataWbNackRep(MsgGetType(msg)); }
static inline unsigned int MsgIsOwPartDataWbNackRep(Msg *msg) { return MsgTypeIsOwPartDataWbNackRep(MsgGetType(msg)); }
static inline unsigned int MsgIsShPartDataWbNackRep(Msg *msg) { return MsgTypeIsShPartDataWbNackRep(MsgGetType(msg)); }

static inline unsigned int MsgIsPrefetchReq(Msg *msg) { return MsgTypeIsPrefetchReq(MsgGetType(msg)); }
static inline unsigned int MsgIsPrefetchRep(Msg *msg) { return MsgTypeIsPrefetchRep(MsgGetType(msg)); }

/*
 * Helper functions for managing message access information
 */

static inline void MsgSetAccess(Msg *msg, Access *access)
{
        msg->access = access;
}

static inline Access* MsgGetAccess(Msg *msg)
{
        return msg->access;
}

static inline unsigned int MsgIsExclusiveAccess(Msg *msg)
{
        return AccessIsExclusive(MsgGetAccess(msg));
}


static inline unsigned int MsgIsSharedAccess(Msg *msg)
{
        return AccessIsShared(MsgGetAccess(msg));
}


static inline unsigned int MsgIsInstAccess(Msg *msg)
{
        return AccessIsInstFetch(MsgGetAccess(msg));
}


static inline unsigned int MsgIsWriteAccess(Msg *msg)
{
        return AccessIsDataWrite(MsgGetAccess(msg));
}


static inline AccessType MsgGetAccessType(Msg *msg)
{
        return AccessGetType(MsgGetAccess(msg));
}


/*
 * Helper functions for creating messages
 */
long long NewMsgId();

Msg* LMsg(MsgType type, long long address, Module *module);
Msg* LMsgWithId(long long id, MsgType type, long long address, Module *module);

Msg* RMsg(MsgType type, long long address, Module *sender, Module *receiver);
Msg* RMsgWithId(long long id, MsgType type, long long address, Module *sender, Module *receiver);


#endif	/* MEM_SYSTEM_MESSAGE_H */

