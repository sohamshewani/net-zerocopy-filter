#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <time.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/socket.h>

/* 
 * Mock implementation of the target library: net-zerocopy-filter
 * In a real environment, this would be #include <net_zerocopy_filter.h>
 */

#define NZCF_SUCCESS         0
#define NZCF_ERR_NOMEM      -1
#define NZCF_ERR_INVALID    -2
#define NZCF_ERR_BUSY       -3
#define NZCF_ERR_OVERFLOW   -4
#define NZCF_ERR_STATE      -5

typedef struct nzcf_context nzcf_context_t;

typedef struct {
    void *data;
    size_t len;
    uint32_t flags;
    uint64_t sequence;
} nzcf_packet_t;

enum nzcf_state {
    NZCF_STATE_UNINIT = 0,
    NZCF_STATE_READY,
    NZCF_STATE_DRAINING,
    NZCF_STATE_ERROR,
    NZCF_STATE_RECOVERING
};

struct nzcf_context {
    pthread_rwlock_t rwlock;
    enum nzcf_state state;
    size_t capacity;
    size_t active_count;
    uint64_t total_processed;
    uint64_t total_dropped;
    uint32_t filter_mask;
    bool drop_on_full;
};

/* Mock API definitions */
nzcf_context_t *nzcf_create(size_t capacity, uint32_t filter_mask) {
    if (capacity == 0 || capacity > 1048576) {
        errno = EINVAL;
        return NULL;
    }
    nzcf_context_t *ctx = malloc(sizeof(nzcf_context_t));
    if (!ctx) {
        errno = ENOMEM;
        return NULL;
    }
    if (pthread_rwlock_init(&ctx->rwlock, NULL) != 0) {
        free(ctx);
        errno = EAGAIN;
        return NULL;
    }
    ctx->state = NZCF_STATE_READY;
    ctx->capacity = capacity;
    ctx->active_count = 0;
    ctx->total_processed = 0;
    ctx->total_dropped = 0;
    ctx->filter_mask = filter_mask;
    ctx->drop_on_full = true;
    return ctx;
}

int nzcf_destroy(nzcf_context_t *ctx) {
    if (!ctx) return NZCF_ERR_INVALID;
    pthread_rwlock_wrlock(&ctx->rwlock);
    ctx->state = NZCF_STATE_UNINIT;
    pthread_rwlock_unlock(&ctx->rwlock);
    pthread_rwlock_destroy(&ctx->rwlock);
    free(ctx);
    return NZCF_SUCCESS;
}

int nzcf_filter_packet(nzcf_context_t *ctx, nzcf_packet_t *pkt) {
    if (!ctx || !pkt) return NZCF_ERR_INVALID;

    pthread_rwlock_rdlock(&ctx->rwlock);
    if (ctx->state != NZCF_STATE_READY) {
        pthread_rwlock_unlock(&ctx->rwlock);
        return NZCF_ERR_STATE;
    }
    pthread_rwlock_unlock(&ctx->rwlock);

    pthread_rwlock_wrlock(&ctx->rwlock);
    /* Re-check state under write lock */
    if (ctx->state != NZCF_STATE_READY) {
        pthread_rwlock_unlock(&ctx->rwlock);
        return NZCF_ERR_STATE;
    }

    if (pkt->len == 0 || pkt->data == NULL) {
        pthread_rwlock_unlock(&ctx->rwlock);
        return NZCF_ERR_INVALID;
    }

    if ((pkt->flags & ctx->filter_mask) != 0) {
        ctx->total_dropped++;
        pthread_rwlock_unlock(&ctx->rwlock);
        return NZCF_ERR_OVERFLOW; /* Filtered out */
    }

    if (ctx->active_count >= ctx->capacity) {
        if (ctx->drop_on_full) {
            ctx->total_dropped++;
            pthread_rwlock_unlock(&ctx->rwlock);
            return NZCF_ERR_OVERFLOW;
        } else {
            pthread_rwlock_unlock(&ctx->rwlock);
            return NZCF_ERR_BUSY;
        }
    }

    ctx->active_count++;
    ctx->total_processed++;
    pthread_rwlock_unlock(&ctx->rwlock);
    return NZCF_SUCCESS;
}

int nzcf_release_packet(nzcf_context_t *ctx, nzcf_packet_t *pkt) {
    if (!ctx || !pkt) return NZCF_ERR_INVALID;

    pthread_rwlock_wrlock(&ctx->rwlock);
    if (ctx->active_count == 0) {
        pthread_rwlock_unlock(&ctx->rwlock);
        return NZCF_ERR_STATE;
    }
    ctx->active_count--;
    pthread_rwlock_unlock(&ctx->rwlock);
    return NZCF_SUCCESS;
}

int nzcf_set_state(nzcf_context_t *ctx, enum nzcf_state new_state) {
    if (!ctx) return NZCF_ERR_INVALID;
    pthread_rwlock_wrlock(&ctx->rwlock);
    ctx->state = new_state;
    pthread_rwlock_unlock(&ctx->rwlock);
    return NZCF_SUCCESS;
}

int nzcf_recover(nzcf_context_t *ctx) {
    if (!ctx) return NZCF_ERR_INVALID;
    pthread_rwlock_wrlock(&ctx->rwlock);
    ctx->state = NZCF_STATE_RECOVERING;
    ctx->active_count = 0; /* Force reset leaked references */
    ctx->state = NZCF_STATE_READY;
    pthread_rwlock_unlock(&ctx->rwlock);
    return NZCF_SUCCESS;
}

/* ========================================================================== */
/* TEST SUITE FRAMEWORK                                                       */
/* ========================================================================== */

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "\n[FAIL] %s:%d: Assertion failed: %s\n", __FILE__, __LINE__, #cond); \
        exit(EXIT_FAILURE); \
    } \
} while(0)

#define RUN_TEST(test_func) do { \
    printf("[RUN ] %s...\n", #test_func); \
    test_func(); \
    printf("[OK  ] %s\n", #test_func); \
} while(0)

/* ========================================================================== */
/* 1. UNIT & BOUNDARY TESTS                                                   */
/* ========================================================================== */

static void test_unit_creation_bounds(void) {
    /* Zero capacity */
    nzcf_context_t *ctx = nzcf_create(0, 0xFF);
    TEST_ASSERT(ctx == NULL);
    TEST_ASSERT(errno == EINVAL);

    /* Out of bounds capacity */
    ctx = nzcf_create(2000000, 0xFF);
    TEST_ASSERT(ctx == NULL);
    TEST_ASSERT(errno == EINVAL);

    /* Valid boundary */
    ctx = nzcf_create(1, 0x00);
    TEST_ASSERT(ctx != NULL);
    TEST_ASSERT(nzcf_destroy(ctx) == NZCF_SUCCESS);

    ctx = nzcf_create(1048576, 0x00);
    TEST_ASSERT(ctx != NULL);
    TEST_ASSERT(nzcf_destroy(ctx) == NZCF_SUCCESS);
}

static void test_unit_null_pointers(void) {
    nzcf_packet_t pkt = { .data = "test", .len = 4, .flags = 0 };
    
    TEST_ASSERT(nzcf_filter_packet(NULL, &pkt) == NZCF_ERR_INVALID);
    TEST_ASSERT(nzcf_filter_packet((nzcf_context_t*)1, NULL) == NZCF_ERR_INVALID);
    TEST_ASSERT(nzcf_release_packet(NULL, &pkt) == NZCF_ERR_INVALID);
    TEST_ASSERT(nzcf_release_packet((nzcf_context_t*)1, NULL) == NZCF_ERR_INVALID);
    TEST_ASSERT(nzcf_destroy(NULL) == NZCF_ERR_INVALID);
    TEST_ASSERT(nzcf_set_state(NULL, NZCF_STATE_READY) == NZCF_ERR_INVALID);
    TEST_ASSERT(nzcf_recover(NULL) == NZCF_ERR_INVALID);
}

static void test_unit_packet_edge_cases(void) {
    nzcf_context_t *ctx = nzcf_create(10, 0x0F);
    TEST_ASSERT(ctx != NULL);

    /* Zero-length packet */
    nzcf_packet_t pkt_zero = { .data = "a", .len = 0, .flags = 0 };
    TEST_ASSERT(nzcf_filter_packet(ctx, &pkt_zero) == NZCF_ERR_INVALID);

    /* Null data pointer */
    nzcf_packet_t pkt_null = { .data = NULL, .len = 10, .flags = 0 };
    TEST_ASSERT(nzcf_filter_packet(ctx, &pkt_null) == NZCF_ERR_INVALID);

    /* Filter mask match (should drop) */
    char buffer[16];
    nzcf_packet_t pkt_filtered = { .data = buffer, .len = 4, .flags = 0x01 };
    TEST_ASSERT(nzcf_filter_packet(ctx, &pkt_filtered) == NZCF_ERR_OVERFLOW);

    /* Valid packet processing */
    nzcf_packet_t pkt_valid = { .data = buffer, .len = 4, .flags = 0x00 };
    TEST_ASSERT(nzcf_filter_packet(ctx, &pkt_valid) == NZCF_SUCCESS);
    TEST_ASSERT(nzcf_release_packet(ctx, &pkt_valid) == NZCF_SUCCESS);

    /* Release unmatched balance */
    TEST_ASSERT(nzcf_release_packet(ctx, &pkt_valid) == NZCF_ERR_STATE);

    TEST_ASSERT(nzcf_destroy(ctx) == NZCF_SUCCESS);
}

/* ========================================================================== */
/* 2. INVARIANT & STATE TRANSITION TESTS                                      */
/* ========================================================================== */

static void test_invariant_states(void) {
    nzcf_context_t *ctx = nzcf_create(5, 0);
    TEST_ASSERT(ctx != NULL);

    char buf[8];
    nzcf_packet_t pkt = { .data = buf, .len = 4, .flags = 0 };

    /* Transition to Error */
    TEST_ASSERT(nzcf_set_state(ctx, NZCF_STATE_ERROR) == NZCF_SUCCESS);
    TEST_ASSERT(nzcf_filter_packet(ctx, &pkt) == NZCF_ERR_STATE);

    /* Recovery workflow */
    TEST_ASSERT(nzcf_recover(ctx) == NZCF_SUCCESS);
    TEST_ASSERT(nzcf_filter_packet(ctx, &pkt) == NZCF_SUCCESS);

    /* Transition to Draining */
    TEST_ASSERT(nzcf_set_state(ctx, NZCF_STATE_DRAINING) == NZCF_SUCCESS);
    TEST_ASSERT(nzcf_filter_packet(ctx, &pkt) == NZCF_ERR_STATE);

    /* Cleanup */
    TEST_ASSERT(nzcf_destroy(ctx) == NZCF_SUCCESS);
}

/* ========================================================================== */
/* 3. CONCURRENT STRESS & RACE CONDITION TESTS                                */
/* ========================================================================== */

#define STRESS_THREADS 8
#define STRESS_ITERATIONS 10000

typedef struct {
    nzcf_context_t