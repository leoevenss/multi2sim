#ifndef MEM_SYSTEM_TLB_BLOCK_H
#define	MEM_SYSTEM_TLB_BLOCK_H

#include <libstruct/string.h>
#include "cacheblock.h"


/*
 *  TLBBlockState
 */

extern struct str_map_t TLBBlockStateMap;

typedef enum
{
        TLBBlockInvalid = 0, /* invalid */
        TLBBlockValid, /* valid */

        NumTLBBlockStates /* number of TLB block states */
} TLBBlockState;


static inline char* MapTLBBlockState(TLBBlockState state) {
        return str_map_value(&TLBBlockStateMap, state);
}


/*
 * Class 'TLBBlock'
 */

CLASS_BEGIN(TLBBlock, CacheBlock)

        TLBBlockState state; /* state */

CLASS_END(TLBBlock)

void TLBBlockCreate(TLBBlock *tblk, unsigned int set, unsigned int way, unsigned int id);
void TLBBlockDestroy(TLBBlock *tblk);

void TLBBlockUpdate(TLBBlock *tblk, long long tag, TLBBlockState state);


/*
 * Helper functions for accessing TLBBlock's tag, identifier,
 * set index and way index.
 */

static inline long long TLBBlockGetTag(TLBBlock *tblk)
{
        return CacheBlockGetTag(asCacheBlock(tblk));
}

static inline unsigned int TLBBlockGetId(TLBBlock *tblk)
{
        return CacheBlockGetId(asCacheBlock(tblk));
}

static inline unsigned int TLBBlockGetSet(TLBBlock *tblk)
{
        return CacheBlockGetSet(asCacheBlock(tblk));
}

static inline unsigned int TLBBlockGetWay(TLBBlock *tblk)
{
        return CacheBlockGetWay(asCacheBlock(tblk));
}


/*
 * Helper functions for managing TLBBlock state
 */

static inline void TLBBlockSetState(TLBBlock *tblk, TLBBlockState state)
{
        if (tblk->state == state) return;
        tblk->state = state;
}

static inline TLBBlockState TLBBlockGetState(TLBBlock *tblk)
{
        return tblk->state;
}

static inline unsigned int TLBBlockIsValid(TLBBlock *tblk)
{
        if (tblk->state != TLBBlockInvalid) return 1;
        return 0;
}

static inline char* TLBBlockMapState(TLBBlock *tblk)
{
        return MapTLBBlockState(tblk->state);
}


/*
 * Helper function for creating TLBBlocks for TLBCache
 */

CacheBlock **CreateTLBBlocks(unsigned int numsets, unsigned int numways);


#endif	/* MEM_SYSTEM_TLB_BLOCK_H */
