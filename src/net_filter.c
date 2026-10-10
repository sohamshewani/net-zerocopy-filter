#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <net/ethernet.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <arpa/inet.h>
#include <stdatomic.h>
#include <time.h>

#include "net_arena_memory.h"
#include "net_ring.h"
#include "net_flow.h"

#define CACHE_LINE_SIZE 64
#define MAX_EVENTS 64
#define RING_SIZE 4096
#define ARENA_SIZE (1024UL * 1024UL * 64UL) // 64MB Hugepage Arena

typedef struct {
    uint64_t packets_received;
    uint64_t packets_filtered;
    uint64_t packets_dropped_ring;
    uint64_t packets_dropped_arena;
    uint64_t packets_dropped_flow;
    uint64_t bytes_processed;
} __attribute__((aligned(CACHE_LINE_SIZE))) filter_telemetry_t;

struct net_filter_context {
    int raw_socket;
    int epoll_fd;
    bool running;
    net_arena_t *arena;
    net_ring_t *ring;
    net_flow_table_t *flow_table;
    filter_telemetry_t telemetry;
};

static atomic_bool g_shutdown_requested = ATOMIC_VAR_INIT(false);

static inline uint32_t calculate_ip_hash(const struct iphdr *iph) {
    uint32_t hash = iph->saddr ^ iph->daddr;
    hash = ((hash >> 16) ^ hash) * 0x45d9f3b;
    hash = ((hash >> 16) ^ hash) * 0x45d9f3b;
    hash = (hash >> 16) ^ hash;
    return hash;
}

static bool validate_packet(const uint8_t *data, size_t len, struct iphdr **out_iph, void **out_payload, size_t *out_payload_len) {
    if (len < sizeof(struct ether_header)) {
        return false;
    }

    const struct ether_header *eth = (const struct ether_header *)data;
    if (ntohs(eth->ether_type) != ETHERTYPE_IP) {
        return false;
    }

    size_t l2_len = sizeof(struct ether_header);
    if (len < l2_len + sizeof(struct iphdr)) {
        return false;
    }

    struct iphdr *iph = (struct iphdr *)(data + l2_len);
    if (iph->version != 4) {
        return false;
    }

    size_t ip_hdr_len = iph->ihl * 4;
    if (ip_hdr_len < sizeof(struct iphdr)) {
        return false;
    }

    if (len < l2_len + ip_hdr_len) {
        return false;
    }

    *out_iph = iph;
    *out_payload = (void *)(data + l2_len + ip_hdr_len);
    *out_payload_len = len - (l2_len + ip_hdr_len);
    return true;
}

net_filter_context_t *net_filter_init(const char *interface_name) {
    net_filter_context_t *ctx = malloc(sizeof(net_filter_context_t));
    if (!ctx) {
        return NULL;
    }
    memset(ctx, 0, sizeof(net_filter_context_t));

    ctx->arena = net_arena_create(ARENA_SIZE);
    if (!ctx->arena) {
        free(ctx);
        return NULL;
    }

    ctx->ring = net_ring_create(RING_SIZE);
    if (!ctx->ring) {
        net_arena_destroy(ctx->arena);
        free(ctx);
        return NULL;
    }

    ctx->flow_table = net_flow_table_create(1024);
    if (!ctx->flow_table) {
        net_ring_destroy(ctx->ring);
        net_arena_destroy(ctx->arena);
        free(ctx);
        return NULL;
    }

    ctx->raw_socket = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (ctx->raw_socket < 0) {
        net_flow_table_destroy(ctx->flow_table);
        net_ring_destroy(ctx->ring);
        net_arena_destroy(ctx->arena);
        free(ctx);
        return NULL;
    }

    int flags = fcntl(ctx->raw_socket, F_GETFL, 0);
    fcntl(ctx->raw_socket, F_SETFL, flags | O_NONBLOCK);

    ctx->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (ctx->epoll_fd < 0) {
        close(ctx->raw_socket);
        net_flow_table_destroy(ctx->flow_table);
        net_ring_destroy(ctx->ring);
        net_arena_destroy(ctx->arena);
        free(ctx);
        return NULL;
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = ctx->raw_socket;
    if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, ctx->raw_socket, &ev) < 0) {
        close(ctx->epoll_fd);
        close(ctx->raw_socket);
        net_flow_table_destroy(ctx->flow_table);
        net_ring_destroy(ctx->ring);
        net_arena_destroy(ctx->arena);
        free(ctx);
        return NULL;
    }

    ctx->running = true;
    return ctx;
}

void net_filter_destroy(net_filter_context_t *ctx) {
    if (!ctx) {
        return;
    }
    ctx->running = false;
    if (ctx->epoll_fd >= 0) {
        close(ctx->epoll_fd);
    }
    if (ctx->raw_socket >= 0) {
        close(ctx->raw_socket);
    }
    if (ctx->flow_table) {
        net_flow_table_destroy(ctx->flow_table);
    }
    if (ctx->ring) {
        net_ring_destroy(ctx->ring);
    }
    if (ctx->arena) {
        net_arena_destroy(ctx->arena);
    }
    free(ctx);
}

bool net_filter_process_batch(net_filter_context_t *ctx) {
    struct epoll_event events[MAX_EVENTS];
    int n = epoll_wait(ctx->epoll_fd, events, MAX_EVENTS, 0);
    if (n < 0) {
        if (errno == EINTR) {
            return true;
        }
        return false;
    }

    for (int i = 0; i < n; i++) {
        if (events[i].events & (EPOLLERR | EPOLLHUP)) {
            return false;
        }

        while (true) {
            uint8_t temp_stack_buf[65535];
            ssize_t packet_len = recv(ctx->raw_socket, temp_stack_buf, sizeof(temp_stack_buf), MSG_DONTWAIT);
            if (packet_len < 0) {
                if (errno == EWOULDBLOCK || errno == EAGAIN) {
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                break;
            }

            ctx->telemetry.packets_received++;
            ctx->telemetry.bytes_processed += packet_len;

            struct iphdr *iph = NULL;
            void *payload = NULL;
            size_t payload_len = 0;

            if (!validate_packet(temp_stack_buf, packet_len, &iph, &payload, &payload_len)) {
                ctx->telemetry.packets_filtered++;
                continue;
            }

            uint32_t flow_hash = calculate_ip_hash(iph);
            if (!net_flow_validate_and_update(ctx->flow_table, flow_hash, iph->id)) {
                ctx->telemetry.packets_dropped_flow++;
                continue;
            }

            void *arena_buf = net_arena_alloc(ctx->arena, packet_len);
            if (!arena_buf) {
                ctx->telemetry.packets_dropped_arena++;
                net_arena_sweep(ctx->arena);
                arena_buf = net_arena_alloc(ctx->arena, packet_len);
                if (!arena_buf) {
                    continue;
                }
            }

            rte_memcpy(arena_buf, temp_stack_buf, packet_len);

            if (!net_ring_enqueue(ctx->ring, arena_buf)) {
                ctx->telemetry.packets_dropped_ring++;
                net_arena_reclaim(ctx->arena, arena_buf, packet_len);
                continue;
            }
        }
    }

    return true;
}

int net_filter_run(net_filter_context_t *ctx) {
    if (!ctx) {
        return -EINVAL;
    }

    while (ctx->running && !atomic_load(&g_shutdown_requested)) {
        if (!net_filter_process_batch(ctx)) {
            usleep(100);
        }
    }

    return 0;
}