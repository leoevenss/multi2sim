#include <libstruct/string.h>
#include <libstruct/debug.h>

#include "tlb-cache.h"
#include "mem-system.h"


/*
 * TLBCacheTypes
 */

struct str_map_t TLBCacheTypeMap = {
        NumTLBCacheTypes,
        {
                { "IL1TLB", IL1TLB},
                { "DL1TLB", DL1TLB},
                { "L2TLB", L2TLB}
        }
};

char* MapTLBCacheType(TLBCacheType type)
{
        return str_map_value(&TLBCacheTypeMap, type);
}


/*
 * Class 'TLBCache'
 */

void TLBCacheCreate(TLBCache *tcache, TLBCacheType type, CacheArrayType ctype, AddressRange *arange, unsigned int numsets, unsigned int numways, unsigned int blocksize,
                unsigned int numcands, unsigned int latency, CacheBlock **blocks, ReplPolicy *rpolicy)
{
        /* Check: validity of TLB cache type */
        assert(0 <= type && type < NumTLBCacheTypes);

        /* Initialize cache */
        CacheCreate(asCache(tcache), MapTLBCacheType(type), ctype, arange, numsets, numways, blocksize, numcands, 1, blocks, rpolicy);
        CacheEnableVirtualIndexing(asCache(tcache));

        /* Initialize TLB cache */
        tcache->type = type;
        tcache->latency =  0;//latency;  leo edit
}


void TLBCacheDestroy(TLBCache *tcache)
{
        /* nothing to do */
}


void TLBCacheDumpStats(TLBCache *tcache, FILE *f)
{
        /* Dump cache statistics */
        CacheDumpStats(asCache(tcache), f);

        /* Dump TLB cache statistics */
        fprintf(f, "%s.Latency = %u\n", TLBCacheGetName(tcache), TLBCacheGetLatency(tcache));
        fprintf(f, "%s.Fills = %lld\n", TLBCacheGetName(tcache), tcache->stats.fills);
        fprintf(f, "%s.Evictions = %lld\n", TLBCacheGetName(tcache), tcache->stats.evictions);
        fprintf(f, "\n");
}


/* Returns pointer to block in TLB cache which can be victimized. */
TLBBlock* TLBCacheGetReplacementBlock(TLBCache *tcache, long long addr)
{
        mem_debug("      victim %s addr=0x%llx: ", TLBCacheGetName(tcache), addr);
        TLBBlock *tblk = asTLBBlock(CacheGetReplacementBlock(asCache(tcache), addr, 0));

        mem_debug("state=%s id=%u", TLBBlockMapState(tblk), TLBBlockGetId(tblk));
        if (TLBBlockIsValid(tblk)) mem_debug(" tag=0x%llx", TLBBlockGetTag(tblk));
        mem_debug("\n");
        return tblk;
}


/* Returns pointer to block in TLB cache with matching address.
 * Returns NULL, if block is not present in TLB cache. */
TLBBlock* TLBCacheFindBlock(TLBCache *tcache, long long addr)
{
        mem_debug("      lookup %s addr=0x%llx: ", TLBCacheGetName(tcache), addr);
        TLBBlock *tblk = asTLBBlock(CacheFindBlock(asCache(tcache), addr, 0));
        if (!tblk)
        {
                mem_debug("state=%s\n", MapTLBBlockState(TLBBlockInvalid));
                return NULL;
        }
        mem_debug("state=%s id=%u tag=0x%llx\n", TLBBlockMapState(tblk), TLBBlockGetId(tblk), TLBBlockGetTag(tblk));
        return tblk;
}


void TLBCacheFillBlock(TLBCache *tcache, TLBBlock *tblk, long long addr, TLBBlockState state)
{
        Cache *cache = asCache(tcache);
        long long tag = CacheGetTag(cache, addr);

        TLBBlockUpdate(tblk, tag, state);
        tcache->stats.fills++;
}


void TLBCacheEvictBlock(TLBCache *tcache, TLBBlock *tblk)
{
        TLBBlockSetState(tblk, TLBBlockInvalid);
        tcache->stats.evictions++;
}


