#include <libmhandle/mhandle.h>
#include "tlb-block.h"

/*
 * TLBBlockState
 */

struct str_map_t TLBBlockStateMap = {
        NumTLBBlockStates,
        {
                { "I", TLBBlockInvalid},
                { "V", TLBBlockValid}
        }
};


/*
 * Class 'TLBBlock'
 */

static unsigned int TLBCacheBlockIsValid(CacheBlock *cblk)
{
        return TLBBlockIsValid(asTLBBlock(cblk));
}


void TLBBlockCreate(TLBBlock *tblk, unsigned int set, unsigned int way, unsigned int id)
{
        CacheBlock *cblk = asCacheBlock(tblk);

        /* Initialize CacheBlock */
        CacheBlockCreate(cblk, set, way, id);

        /* Virtual functions */
        cblk->IsValid = TLBCacheBlockIsValid;

        /* Initialize TLBBlock */
        tblk->state = TLBBlockInvalid;
}


void TLBBlockDestroy(TLBBlock *tblk)
{
        /* nothing to do */
}


void TLBBlockUpdate(TLBBlock *tblk, long long tag, TLBBlockState state)
{
        CacheBlock *cblk = asCacheBlock(tblk);

        /* Check: new block state must be a valid state */
        assert(state != TLBBlockInvalid);

        /* Check: block must not be in a valid state */
        assert(!TLBBlockIsValid(tblk));

        /* Set state and tag of block */
        TLBBlockSetState(tblk, state);
        CacheBlockSetTag(cblk, tag);
}


/*
 * Helper function for creating TLBBlocks for TLBCache
 */

CacheBlock **CreateTLBBlocks(unsigned int numsets, unsigned int numways)
{
        CacheBlock **blocks = xcalloc(numsets * numways, sizeof (CacheBlock *));
        for (unsigned int id = 0; id < numsets * numways; id++)
                blocks[id] = asCacheBlock(new(TLBBlock, id / numways, id % numways, id));
        return blocks;
}


