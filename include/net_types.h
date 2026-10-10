#ifndef NET_TYPES_H
#define NET_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <sys/types.h>

#define NET_CACHE_LINE_SIZE 64
#define NET_CACHELINE_ALIGN __attribute__((aligned(NET_CACHE_LINE_SIZE)))
#define NET_PACKET_ALIGN __attribute__((aligned(64)))

typedef enum {
    NET_SUCCESS = 0,
    NET_ERR_INVALID_ARG = -1,
    NET_ERR_NO_MEMORY = -2,
    NET_ERR_RING_FULL = -3,
    NET_ERR_RING_EMPTY = -4,
    NET_ERR_OUT_OF_BOUNDS = -5,
    NET_ERR_FLOW_EXPIRED = -6,
    NET_ERR_DROPPED = -7,
    NET_ERR_SYS = -8
} net_status_t;

typedef enum {
    NET_PKT_FLAG_NONE = 0,
    NET_PKT_FLAG_RX = (1 << 0),
    NET_PKT_FLAG_TX = (1 << 1),
    NET_PKT_FLAG_DROPPED = (1 << 2),
    NET_PKT_FLAG_VALIDATED = (1 << 3),
    NET_PKT_FLAG_RECYCLED = (1 << 4)
} net_packet_flags_t;

typedef struct NET_CACHELINE_ALIGN net_packet {
    uint8_t *data;
    uint32_t capacity;
    uint32_t length;
    uint64_t timestamp_ns;
    uint32_t flow_id;
    uint32_t flags;
    uint64_t sequence_num;
    void *arena_owner;
    uint8_t pad[NET_CACHE_LINE_SIZE - (sizeof(uint8_t*) + sizeof(uint32_t) * 4 + sizeof(uint64_t) * 3 + sizeof(void*))];
} net_packet_t;

typedef struct NET_CACHELINE_ALIGN {
    _Atomic uint64_t total_packets;
    _Atomic uint64_t dropped_packets;
    _Atomic uint64_t bytes_processed;
    _Atomic uint64_t arena_allocations;
    _Atomic uint64_t ring_full_events;
} net_telemetry_t;

typedef struct NET_CACHELINE_ALIGN {
    void *base_addr;
    size_t total_size;
    size_t block_size;
    uint32_t total_blocks;
    _Atomic uint32_t allocated_blocks;
    _Atomic size_t bump_offset;
    int hugepage_fd;
} net_arena_t;

typedef struct NET_CACHELINE_ALIGN {
    uint32_t capacity;
    uint32_t bitmask;
    NET_CACHELINE_ALIGN _Atomic uint64_t head;
    NET_CACHELINE_ALIGN _Atomic uint64_t tail;
    void *buffer[];
} net_ring_t;

typedef struct NET_CACHELINE_ALIGN {
    uint32_t flow_id;
    _Atomic uint64_t expected_seq;
    _Atomic uint64_t window_bitmap;
    _Atomic uint64_t last_activity_ns;
    _Atomic bool active;
} net_flow_state_t;

typedef struct NET_CACHELINE_ALIGN {
    uint32_t max_flows;
    uint32_t flow_mask;
    net_flow_state_t *flows;
} net_flow_table_t;

static inline uint32_t net_roundup_pow2(uint32_t v) {
    if (v == 0) return 1;
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v;
}

static inline bool net_is_pow2(uint32_t v) {
    return v && !(v & (v - 1));
}

#endif /* NET_TYPES_H */