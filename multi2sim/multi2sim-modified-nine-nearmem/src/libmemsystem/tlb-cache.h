#ifndef MEM_SYSTEM_TLB_CACHE_H
#define	MEM_SYSTEM_TLB_CACHE_H

#include "cache.h"
#include "tlb-block.h"


/*
 * TLBCacheTypes
 */

extern struct str_map_t TLBCacheTypeMap;

typedef enum
{
        IL1TLB = 0, /* instruction L1 TLB */
        DL1TLB, /* data L1 TLB */
        L2TLB, /* unified L2 TLB */

        NumTLBCacheTypes /* number of TLB types */
} TLBCacheType;

char* MapTLBCacheType(TLBCacheType type);


/*
 * Class 'TLBCache'
 */

CLASS_BEGIN(TLBCache, Cache)

        TLBCacheType type; /* TLB cache type */
        unsigned int latency; /* latency */

        struct
        {
                long long evictions; /* number of evictions */
                long long fills; /* number of fills */
        } stats;

CLASS_END(TLBCache)

void TLBCacheCreate(TLBCache *tcache, TLBCacheType type, CacheArrayType ctype, AddressRange *arange, unsigned int numsets, unsigned int numways, unsigned int blocksize,
                unsigned int numcands, unsigned int latency, CacheBlock **blocks, ReplPolicy *rpolicy);
void TLBCacheDestroy(TLBCache *tcache);

void TLBCacheDumpStats(TLBCache *tcache, FILE *f);

TLBBlock* TLBCacheGetReplacementBlock(TLBCache *tcache, long long addr);
TLBBlock* TLBCacheFindBlock(TLBCache *tcache, long long addr);

void TLBCacheFillBlock(TLBCache *tcache, TLBBlock *tblk, long long addr, TLBBlockState state);
void TLBCacheEvictBlock(TLBCache *tcache, TLBBlock *tblk);


static inline TLBBlock* TLBCacheGetBlock(TLBCache *tcache, unsigned int id)
{
        return asTLBBlock(CacheGetBlock(asCache(tcache), id));
}

static inline void TLBCacheAccessBlock(TLBCache *tcache, TLBBlock *tblk)
{
        CacheAccessBlock(asCache(tcache), asCacheBlock(tblk));
}

static inline TLBCacheType TLBCacheGetType(TLBCache *tcache)
{
        return tcache->type;
}

static inline char* TLBCacheMapType(TLBCache *tcache)
{
        return MapTLBCacheType(TLBCacheGetType(tcache));
}

static inline unsigned int TLBCacheIsIL1TLB(TLBCache *tcache)
{
        return (TLBCacheGetType(tcache) == IL1TLB) ? 1 : 0;
}

static inline unsigned int TLBCacheIsDL1TLB(TLBCache *tcache)
{
        return (TLBCacheGetType(tcache) == DL1TLB) ? 1 : 0;
}

static inline unsigned int TLBCacheIsL2TLB(TLBCache *tcache)
{
        return (TLBCacheGetType(tcache) == L2TLB) ? 1 : 0;
}

static inline unsigned int TLBCacheIsL1TLB(TLBCache *tcache)
{
        return (TLBCacheIsIL1TLB(tcache) || TLBCacheIsDL1TLB(tcache)) ? 1 : 0;
}

static inline unsigned int TLBCacheGetNumBlocks(TLBCache *tcache)
{
        return CacheGetNumBlocks(asCache(tcache));
}

static inline unsigned int TLBCacheGetNumSets(TLBCache *tcache)
{
        return CacheGetNumSets(asCache(tcache));
}

static inline unsigned int TLBCacheGetNumWays(TLBCache *tcache)
{
        return CacheGetNumWays(asCache(tcache));
}

static inline unsigned int TLBCacheGetLatency(TLBCache *tcache)
{
        //return tcache->latency;
        return 0;//leo edit
}

static inline char* TLBCacheGetName(TLBCache *tcache)
{
        return CacheGetName(asCache(tcache));
}


#endif	/* MEM_SYSTEM_TLB_CACHE_H */


