#ifndef MEM_SYSTEM_MESI_HANDLERS_PRIVATE_MODULE_H
#define	MEM_SYSTEM_MESI_HANDLERS_PRIVATE_MODULE_H

#include "mesi-handlers.h"
#include "private-module.h"


/* Helper functions */
void PrivateModuleRegisterHandlers(Module *mod);

void PrivateModuleMsgWaitForMSHR(PrivateModule *prmod, Msg *msg, PrivateMSHR *mshr);
void PrivateModuleMsgWaitForWB(PrivateModule *prmod, Msg *msg, PrivateWB *wb);

void PrivateModuleMsgWaitForFreeMSHR(PrivateModule *prmod, Msg *msg);
void PrivateModuleMsgWaitForFreeWB(PrivateModule *prmod, Msg *msg);

unsigned int PrivateModuleLookup(PrivateModule *prmod, Msg *msg, PrivateCache *cache, PrivateBlock **ppblk);


/* Msg handlers */
void TlbReqHandler(Module *mod, Msg *msg);
void CoreReqHandler(Module *mod, Msg *msg);
void CoreRepHandler(Module *mod, Msg *msg);

void PteRepHandler(Module *mod, Msg *msg);
void PteNackHandler(Module *mod, Msg *msg);

void BlkRepHandler(Module *mod, Msg *msg);
void BlkNackHandler(Module *mod, Msg *msg);

void WbRepHandler(Module *mod, Msg *reply);
void NormWbNackHandler(Module *mod, Msg *nack);
void DataWbNackHandler(Module *mod, Msg *nack);

void InvalReqHandler(Module *mod, Msg *request);

void ShIntrvReqHandler(Module *mod, Msg *request);
void ExIntrvReqHandler(Module *mod, Msg *request);

void ShFwdReqHandler(Module *mod, Msg *request);
void ExFwdReqHandler(Module *mod, Msg *request);

void ExBkInvalReqHandler(Module *mod, Msg *request);
void ShBkInvalReqHandler(Module *mod, Msg *request);

void PrefetchReqHandler(Module *mod, Msg *msg);
void PrefetchRepHandler(Module *mod, Msg *msg);

#endif	/* MEM_SYSTEM_MESI_HANDLERS_PRIVATE_MODULE_H */
