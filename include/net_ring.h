#ifndef NET_RING_H
#define NET_RING_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <assert.h>

#define NET_CACHE_LINE_SIZE 64

typedef struct {
    void **buffer;
    size_int_t capacity;
    size_int_t mask;
    
    _Alignas(NET_CACHE_LINE_SIZE) atomic_size_t head;
    _Alignas(NET_CACHE_LINE_SIZE) atomic_size_t tail;
    
    _Alignas(NET_CACHE_LINE_SIZE) atomic_size_t dropped_packets;
} net_ring_t;

static inline bool net_ring_is_power_of_two(size_t n) {
    return (n != 0) && ((n & (n - 1)) == 0);
}

static inline bool net_ring_init(net_ring_t *ring, void **storage, size_t capacity) {
    if (!ring || !storage || !net_ring_is_power_of_two(capacity)) {
        return false;
    }
    
    ring->buffer = storage;
    ring->capacity = capacity;
    ring->mask = capacity - 1;
    
    atomic_init(&ring->head, 0);
    atomic_init(&ring->tail, 0);
    atomic_init(&ring->dropped_packets, 0);
    
    return true;
}

static inline size_t net_ring_count(const net_ring_t *ring) {
    size_t head = atomic_load_explicit(&ring->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
    if (tail >= head) {
        return tail - head;
    }
    return (ring->capacity - head) + tail;
}

static inline bool net_ring_full(const net_ring_t *ring) {
    return net_ring_count(ring) == ring->capacity;
}

static inline bool net_ring_empty(const net_ring_t *ring) {
    return atomic_load_explicit(&ring->head, memory_order_relaxed) ==
           atomic_load_explicit(&ring->tail, memory_order_relaxed);
}

static inline bool net_ring_mpmc_enqueue(net_ring_t *ring, void *item) {
    if (!ring || !item) {
        return false;
    }

    size_t current_tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
    size_t current_head = atomic_load_explicit(&ring->head, memory_order_acquire);

    while (true) {
        if ((current_tail - current_head) >= ring->capacity) {
            atomic_fetch_add_explicit(&ring->dropped_packets, 1, memory_order_relaxed);
            return false;
        }

        if (atomic_compare_exchange_weak_explicit(&ring->tail,
                                                  &current_tail,
                                                  current_tail + 1,
                                                  memory_order_release,
                                                  memory_order_relaxed)) {
            break;
        }
        current_head = atomic_load_explicit(&ring->head, memory_order_acquire);
    }

    ring->buffer[current_tail & ring->mask] = item;
    return true;
}

static inline bool net_ring_mpmc_dequeue(net_ring_t *ring, void **item) {
    if (!ring || !item) {
        return false;
    }

    size_t current_head = atomic_load_explicit(&ring->head, memory_order_relaxed);
    size_t current_tail = atomic_load_explicit(&ring->tail, memory_order_acquire);

    while (true) {
        if (current_head == current_tail) {
            return false;
        }

        if (atomic_compare_exchange_weak_explicit(&ring->head,
                                                  &current_head,
                                                  current_head + 1,
                                                  memory_order_release,
                                                  memory_order_relaxed)) {
            break;
        }
        current_tail = atomic_load_explicit(&ring->tail, memory_order_acquire);
    }

    *item = ring->buffer[current_head & ring->mask];
    return true;
}

static inline bool net_ring_spsc_enqueue(net_ring_t *ring, void *item) {
    if (!ring || !item) {
        return false;
    }

    size_t head = atomic_load_explicit(&ring->head, memory_order_acquire);
    size_t tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);

    if ((tail - head) >= ring->capacity) {
        atomic_fetch_add_explicit(&ring->dropped_packets, 1, memory_order_relaxed);
        return false;
    }

    ring->buffer[tail & ring->mask] = item;
    atomic_store_explicit(&ring->tail, tail + 1, memory_order_release);
    return true;
}

static inline bool net_ring_spsc_dequeue(net_ring_t *ring, void **item) {
    if (!ring || !item) {
        return false;
    }

    size_t head = atomic_load_explicit(&ring->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&ring->tail, memory_order_acquire);

    if (head == tail) {
        return false;
    }

    *item = ring->buffer[head & ring->mask];
    atomic_store_explicit(&ring->head, head + 1, memory_order_release);
    return true;
}

static inline uint64_t net_ring_get_dropped(const net_ring_t *ring) {
    if (!ring) {
        return 0;
    }
    return atomic_load_explicit(&ring->dropped_packets, memory_order_relaxed);
}

#endif /* NET_RING_H */