#ifndef __ZSIM_MALLOC_H__
#define __ZSIM_MALLOC_H__

#include "zsim_hooks.h"
#include <malloc.h>
#include <stdio.h>

static void *zsim_malloc_hook (size_t size, const void *caller);
static void *zsim_realloc_hook (void *ptr, size_t size, const void *caller);
static void zsim_free_hook (void *ptr, const void *caller);

static void *(*OldMallocHook) (size_t, const void *);
static void *(*OldReallocHook) (void *, size_t, const void *);
static void (*OldFreeHook) (void *, const void *);

static DataType GlobalType;
static DataValue* GlobalMin;
static DataValue* GlobalMax;

static inline void* malloc_zsim(size_t size, DataType Type)
{
    void* Tmp;
    Tmp = malloc(size);
    DataValue* minValue = (DataValue*) malloc(sizeof(DataValue));
    DataValue* maxValue = (DataValue*) malloc(sizeof(DataValue));
    if (Type == HOOKS_DOUBLE)
    {
        minValue->HOOKS_DOUBLE = -4.1588886540090808;
        maxValue->HOOKS_DOUBLE = 5.0871395561061092;
    }
    else
    {
        minValue->HOOKS_FLOAT = -4.1588886540090808;
        maxValue->HOOKS_FLOAT = 5.0871395561061092;
    }
    zsim_elaborate_allocate_approximate(Tmp, size, Type, minValue, maxValue);
    return Tmp;
}

static inline void* realloc_zsim(void *ptr, size_t size, DataType Type)
{
    void* Tmp;
    zsim_deallocate_approximate(ptr);
    Tmp = realloc(ptr, size);
    DataValue* minValue = (DataValue*) malloc(sizeof(DataValue));
    DataValue* maxValue = (DataValue*) malloc(sizeof(DataValue));
    if (Type == HOOKS_DOUBLE)
    {
        minValue->HOOKS_DOUBLE = -4.1588886540090808;
        maxValue->HOOKS_DOUBLE = 5.0871395561061092;
    }
    else
    {
        minValue->HOOKS_FLOAT = -4.1588886540090808;
        maxValue->HOOKS_FLOAT = 5.0871395561061092;
    }
    zsim_elaborate_allocate_approximate(Tmp, size, Type, minValue, maxValue);
    return Tmp;
}

static inline void* calloc_zsim(size_t size1, size_t size2, DataType Type)
{
    void* Tmp;
    Tmp = calloc(size1, size2);
    DataValue* minValue = (DataValue*) malloc(sizeof(DataValue));
    DataValue* maxValue = (DataValue*) malloc(sizeof(DataValue));
    if (Type == HOOKS_DOUBLE)
    {
        minValue->HOOKS_DOUBLE = -4.1588886540090808;
        maxValue->HOOKS_DOUBLE = 5.0871395561061092;
    }
    else
    {
        minValue->HOOKS_FLOAT = -4.1588886540090808;
        maxValue->HOOKS_FLOAT = 5.0871395561061092;
    }
    zsim_elaborate_allocate_approximate(Tmp, size1*size2, Type, minValue, maxValue);
    return Tmp;
}

static inline void free_zsim(void* ptr)
{
    zsim_deallocate_approximate(ptr);
    free(ptr);
}

static void zsim_mallocs_init(/*DataType Type, DataValue* Min, DataValue* Max*/)
{
    printf("ZSIM Malloc Hooks Active\n");
    GlobalMin = (DataValue*) malloc(sizeof(DataValue));
    GlobalMax = (DataValue*) malloc(sizeof(DataValue));
    // Bwaves
    GlobalMin->HOOKS_DOUBLE = -2000;
    GlobalMax->HOOKS_DOUBLE = 2000;
    // Leslie3d
    GlobalMin->HOOKS_DOUBLE = -450;
    GlobalMax->HOOKS_DOUBLE = 2800;
    // GemsFDTD
    GlobalMin->HOOKS_DOUBLE = -350;
    GlobalMax->HOOKS_DOUBLE = 98139295973;
    // Tonto
    GlobalMin->HOOKS_DOUBLE = -6.2662073581423241e+301;
    GlobalMax->HOOKS_DOUBLE = 5.9323589279593982e+276;
    GlobalType = HOOKS_DOUBLE;
    OldMallocHook = __malloc_hook;
    OldReallocHook = __realloc_hook;
    OldFreeHook = __free_hook;
    __malloc_hook = zsim_malloc_hook;
    __realloc_hook = zsim_realloc_hook;
    __free_hook = zsim_free_hook;
}

static void *zsim_malloc_hook (size_t size, const void *caller)
{
    void *Tmp;
    __malloc_hook = OldMallocHook;
    __realloc_hook = OldReallocHook;
    __free_hook = OldFreeHook;
    printf(" \b");
    Tmp = malloc (size);
    zsim_elaborate_allocate_approximate(Tmp, size, GlobalType, GlobalMin, GlobalMax);
    OldMallocHook = __malloc_hook;
    OldReallocHook = __realloc_hook;
    OldFreeHook = __free_hook;
    __malloc_hook = zsim_malloc_hook;
    __realloc_hook = zsim_realloc_hook;
    __free_hook = zsim_free_hook;
    return Tmp;
}

static void *zsim_realloc_hook (void *ptr, size_t size, const void *caller)
{
    void *Tmp;
    __malloc_hook = OldMallocHook;
    __realloc_hook = OldReallocHook;
    __free_hook = OldFreeHook;
    zsim_deallocate_approximate(ptr);
    Tmp = realloc (ptr, size);
    zsim_elaborate_allocate_approximate(Tmp, size, GlobalType, GlobalMin, GlobalMax);
    // zsim_reallocate_approximate(Tmp, size);
    OldMallocHook = __malloc_hook;
    OldReallocHook = __realloc_hook;
    OldFreeHook = __free_hook;
    __malloc_hook = zsim_malloc_hook;
    __realloc_hook = zsim_realloc_hook;
    __free_hook = zsim_free_hook;
    return Tmp;
}

static void zsim_free_hook (void *ptr, const void *caller)
{
    __malloc_hook = OldMallocHook;
    __realloc_hook = OldReallocHook;
    __free_hook = OldFreeHook;
    zsim_deallocate_approximate(ptr);
    free(ptr);
    OldMallocHook = __malloc_hook;
    OldReallocHook = __realloc_hook;
    OldFreeHook = __free_hook;
    __malloc_hook = zsim_malloc_hook;
    __realloc_hook = zsim_realloc_hook;
    __free_hook = zsim_free_hook;
}

#endif // __ZSIM_MALLOC_H__
