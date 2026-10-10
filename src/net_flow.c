#include "net_flow.h"
#include <string.h>
#include <time.h>

#define FLOW_BUCKET_SHIFT 16
#define FLOW_BUCKET_COUNT (1 << FLOW_BUCKET_SHIFT)
#define FLOW_BUCKET_MASK (FLOW_BUCKET_COUNT - 1)

typedef struct flow_bucket {
    net_flow_state_t states[NET_FLOW_MAX_TRACKED];
    atomic_uint_fast32_t count;
} flow_bucket_t;

struct net_flow_table {
    flow_bucket_t buckets[FLOW_BUCKET_COUNT];
    atomic_uint_fast64_t total_dropped;
    atomic_uint_fast64_t total_accepted;
};

static inline uint32_t hash_flow(const net_flow_key_t *key) {
    uint32_t hash = 2166136261U;
    hash ^= key->src_ip;
    hash *= 16777619U;
    hash ^= key->dst_ip;
    hash *= 16777619U;
    hash ^= key->src_port;
    hash *= 16777619U;
    hash ^= key->dst_port;
    hash *= 16777619U;
    hash ^= key->proto;
    hash *= 16777619U;
    return hash;
}

static inline bool keys_equal(const net_flow_key_t *a, const net_flow_key_t *b) {
    return a->src_ip == b->src_ip &&
           a->dst_ip == b->dst_ip &&
           a->src_port == b->src_port &&
           a->dst_port == b->dst_port &&
           a->proto == b->proto;
}

net_flow_table_t *net_flow_table_init(void) {
    net_flow_table_t *table = (net_flow_table_t *)mmap(
        NULL,
        sizeof(net_flow_table_t),
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0
    );
    if (table == MAP_FAILED) {
        return NULL;
    }
    memset(table, 0, sizeof(net_flow_table_t));
    return table;
}

void net_flow_table_destroy(net_flow_table_t *table) {
    if (table != NULL) {
        munmap(table, sizeof(net_flow_table_t));
    }
}

net_flow_result_t net_flow_process(net_flow_table_t *table, const net_flow_key_t *key, uint32_t seq_num) {
    if (table == NULL || key == NULL) {
        return NET_FLOW_DROP;
    }

    uint32_t h = hash_flow(key);
    uint32_t bucket_idx = h & FLOW_BUCKET_MASK;
    flow_bucket_t *bucket = &table->buckets[bucket_idx];

    uint64_t current_time = (uint64_t)time(NULL);

    for (uint32_t i = 0; i < NET_FLOW_MAX_TRACKED; ++i) {
        net_flow_state_t *state = &bucket->states[i];
        
        uint32_t active = atomic_load_explicit(&state->active, memory_order_relaxed);
        if (active) {
            if (keys_equal(&state->key, key)) {
                if (current_time - state->last_seen > 60) {
                    atomic_store_explicit(&state->active, 0, memory_order_release);
                    break;
                }

                atomic_store_explicit(&state->last_seen, current_time, memory_order_relaxed);

                uint32_t base_seq = atomic_load_explicit(&state->window_base, memory_order_relaxed);
                int32_t diff = (int32_t)(seq_num - base_seq);

                if (diff < 0) {
                    atomic_fetch_add_explicit(&table->total_dropped, 1, memory_order_relaxed);
                    return NET_FLOW_DROP;
                }

                if (diff >= NET_FLOW_WINDOW_SIZE) {
                    uint32_t shift = (uint32_t)diff - NET_FLOW_WINDOW_SIZE + 1;
                    if (shift >= NET_FLOW_WINDOW_SIZE) {
                        state->bitmap = 0;
                        atomic_store_explicit(&state->window_base, base_seq + diff - NET_FLOW_WINDOW_SIZE + 1, memory_order_relaxed);
                    } else {
                        state->bitmap >>= shift;
                        atomic_store_explicit(&state->window_base, base_seq + shift, memory_order_relaxed);
                    }
                    diff = (int32_t)(seq_num - atomic_load_explicit(&state->window_base, memory_order_relaxed));
                }

                uint64_t bit = (1ULL << diff);
                if ((state->bitmap & bit) != 0) {
                    atomic_fetch_add_explicit(&table->total_dropped, 1, memory_order_relaxed);
                    return NET_FLOW_DROP;
                }

                state->bitmap |= bit;
                atomic_fetch_add_explicit(&table->total_accepted, 1, memory_order_relaxed);
                return NET_FLOW_ACCEPT;
            }
        }
    }

    for (uint32_t i = 0; i < NET_FLOW_MAX_TRACKED; ++i) {
        net_flow_state_t *state = &bucket->states[i];
        uint32_t active = atomic_load_explicit(&state->active, memory_order_relaxed);
        if (!active) {
            uint32_t expected = 0;
            if (atomic_compare_exchange_strong_explicit(&state->active, &expected, 1, memory_order_acquire, memory_order_relaxed)) {
                memcpy(&state->key, key, sizeof(net_flow_key_t));
                atomic_store_explicit(&state->window_base, seq_num, memory_order_relaxed);
                state->bitmap = 1ULL;
                atomic_store_explicit(&state->last_seen, current_time, memory_order_relaxed);
                
                atomic_fetch_add_explicit(&table->total_accepted, 1, memory_order_relaxed);
                return NET_FLOW_ACCEPT;
            }
        }
    }

    atomic_fetch_add_explicit(&table->total_dropped, 1, memory_order_relaxed);
    return NET_FLOW_DROP;
}

void net_flow_sweep(net_flow_table_t *table) {
    if (table == NULL) {
        return;
    }

    uint64_t current_time = (uint64_t)time(NULL);

    for (uint32_t b = 0; b < FLOW_BUCKET_COUNT; ++b) {
        flow_bucket_t *bucket = &table->buckets[b];
        for (uint32_t i = 0; i < NET_FLOW_MAX_TRACKED; ++i) {
            net_flow_state_t *state = &bucket->states[i];
            if (atomic_load_explicit(&state->active, memory_order_relaxed)) {
                uint64_t last = atomic_load_explicit(&state->last_seen, memory_order_relaxed);
                if (current_time - last > 60) {
                    atomic_store_explicit(&state->active, 0, memory_order_release);
                }
            }
        }
    }
}