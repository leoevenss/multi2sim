#include <libstruct/misc.h>
#include <libstruct/debug.h>
#include <libmhandle/mhandle.h>

#include "module.h"
#include "mem-system.h"

#include "message.h"
#include "message-manager.h"


/* Msg counter */
static long long msg_count = 1;


void MsgCreate(Msg *msg, long long id, MsgType type, long long address, Module *sender, Module *receiver)
{
        /* Check: input arguments are valid */
        assert(id > 0);
        assert(address >= 0);
        assert(type < NumMsgTypes);
        assert(sender && receiver);
        assert(sender->net == receiver->net);

        /* Set parameters */
        msg->id = id;
        msg->pte = -1;
        msg->tag = address & ~(block_size - 1);

        msg->type = type;
        msg->sender = sender;
        msg->receiver = receiver;

        msg->tlb_cycle[0] = -1;
        msg->tlb_cycle[1] = -1;
}


void MsgDestroy(Msg *msg)
{
        /* nothing to do */
}


/*
 * Helper functions for creating messages
 */

long long NewMsgId()
{
        return msg_count++;
}


Msg* LMsg(MsgType type, long long address, Module *module)
{
        return new(Msg, NewMsgId(), type, address, module, module);
}


Msg* LMsgWithId(long long id, MsgType type, long long address, Module *module)
{
        return new(Msg, id, type, address, module, module);
}


Msg* RMsg(MsgType type, long long address, Module *sender, Module *receiver)
{
        return new(Msg, NewMsgId(), type, address, sender, receiver);
}


Msg* RMsgWithId(long long id, MsgType type, long long address, Module *sender, Module *receiver)
{
        return new(Msg, id, type, address, sender, receiver);
}
