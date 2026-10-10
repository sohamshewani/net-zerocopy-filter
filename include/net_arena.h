#ifndef NET_ARENA_H
#define NET_ARENA_H

#define _GNU_SOURCE
#include <sys/mman.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0x00040000
#endif

#ifndef MAP_HUGE_SHIFT
#define MAP_HUGE_SHIFT 26
#endif

#define NET_ARENA_CACHE_LINE 64
#define NET_ARENA_ALIGNMENT 64

typedef struct {
    uint8_t *base;
    size_t size;
    alignas(NET_ARENA_CACHE_LINE) atomic_size_t offset;
    alignas(NET_ARENA_CACHE_LINE) atomic_size_t active_allocations;
    bool use_hugepages;
} net_arena_t;

static inline size_t net_arena_align_up(size_t val, size_t alignment) {
    return (val + alignment - 1) & ~(alignment - 1);
}

static inline bool net_arena_init(net_arena_t *arena, size_t size, bool use_hugepages) {
    if (!arena || size == 0) {
        errno = EINVAL;
        return false;
    }

    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    size_t aligned_size = (size + page_size - 1) & ~(page_size - 1);

    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    int mmap_hugetlb_flag = use_hugepages ? (MAP_HUGETLB | (21 << MAP_HUGE_SHIFT)) : 0;

    void *addr = mmap(NULL, aligned_size, PROT_READ | PROT_WRITE, flags | mmap_hugetlb_flag, -1, 0);
    if (addr == MAP_FAILED && use_hugepages) {
        addr = mmap(NULL, aligned_size, PROT_READ | PROT_WRITE, flags, -1, 0);
        use_hugepages = false;
    }

    if (addr == MAP_FAILED) {
        return false;
    }

    arena->base = (uint8_t *)addr;
    arena->size = aligned_size;
    atomic_init(&arena->offset, 0);
    atomic_init(&arena->active_allocations, 0);
    arena->use_hugepages = use_hugepages;

    return true;
}

static inline void net_arena_destroy(net_arena_t *arena) {
    if (!arena || !arena->base) {
        return;
    }
    munmap(arena->base, arena->size);
    arena->base = NULL;
    arena->size = 0;
    atomic_store(&arena->offset, 0);
    atomic_store(&arena->active_allocations, 0);
}

static inline void *net_arena_alloc(net_arena_t *arena, size_t size) {
    if (!arena || size == 0) {
        return NULL;
    }

    size_t aligned_size = net_arena_align_up(size, NET_ARENA_ALIGNMENT);
    size_t current_offset = atomic_load_explicit(&arena->offset, memory_order_relaxed);

    while (1) {
        if (current_offset + aligned_size > arena->size) {
            errno = ENOMEM;
            return NULL;
        }

        if (atomic_compare_exchange_weak_explicit(
                &arena->offset,
                &current_offset,
                current_offset + aligned_size,
                memory_order_acq_rel,
                memory_order_relaxed)) {
            
            atomic_fetch_add_explicit(&arena->active_allocations, 1, memory_order_relaxed);
            return (void *)(arena->base + current_offset);
        }
    }
}

static inline void net_arena_reset(net_arena_t *arena) {
    if (!arena) {
        return;
    }
    atomic_store_explicit(&arena->offset, 0, memory_order_release);
    atomic_store_explicit(&arena->active_allocations, 0, memory_order_release);
}

static inline size_t net_arena_available(const net_arena_t *arena) {
    if (!arena) {
        return 0;
    }
    size_t current_offset = atomic_load_explicit(&arena->offset, memory_order_relaxed);
    if (current_offset >= arena->size) {
        return 0;
    }
    return arena->size - current_offset;
}

static inline size_t net_arena_active_count(const net_arena_t *arena) {
    if (!arena) {
        return 0;
    }
    return atomic_load_explicit(&arena->active_allocations, memory_order_relaxed);
}

#endif /* NET_ARENA_H */