#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <net/ethernet.h>
#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <time.h>

#include "net_arena_memory.h"
#include "net_ring.h"
#include "net_flow.h"
#include "net_engine.h"

#define NET_ENGINE_MAX_EVENTS 64
#define NET_PACKET_ALIGNMENT 64

struct net_engine {
    int epoll_fd;
    int rx_socket_fd;
    bool running;
    struct net_arena *arena;
    struct net_ring *ring;
    struct net_flow_table *flow_table;
    uint64_t total_rx_packets;
    uint64_t total_dropped_packets;
    uint64_t total_arena_exhaustions;
};

static inline uint64_t net_engine_current_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

struct net_engine *net_engine_create(const struct net_engine_config *config) {
    if (!config) {
        errno = EINVAL;
        return NULL;
    }

    struct net_engine *engine = (struct net_engine *)malloc(sizeof(struct net_engine));
    if (!engine) {
        return NULL;
    }

    engine->epoll_fd = -1;
    engine->rx_socket_fd = -1;
    engine->running = false;
    engine->total_rx_packets = 0;
    engine->total_dropped_packets = 0;
    engine->total_arena_exhaustions = 0;

    engine->arena = net_arena_create(config->arena_size);
    if (!engine->arena) {
        free(engine);
        return NULL;
    }

    engine->ring = net_ring_create(config->ring_capacity);
    if (!engine->ring) {
        net_arena_destroy(engine->arena);
        free(engine);
        return NULL;
    }

    engine->flow_table = net_flow_table_create(config->flow_table_size);
    if (!engine->flow_table) {
        net_ring_destroy(engine->ring);
        net_arena_destroy(engine->arena);
        free(engine);
        return NULL;
    }

    engine->rx_socket_fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (engine->rx_socket_fd < 0) {
        net_flow_table_destroy(engine->flow_table);
        net_ring_destroy(engine->ring);
        net_arena_destroy(engine->arena);
        free(engine);
        return NULL;
    }

    engine->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (engine->epoll_fd < 0) {
        close(engine->rx_socket_fd);
        net_flow_table_destroy(engine->flow_table);
        net_ring_destroy(engine->ring);
        net_arena_destroy(engine->arena);
        free(engine);
        return NULL;
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = engine->rx_socket_fd;

    if (epoll_ctl(engine->epoll_fd, EPOLL_CTL_ADD, engine->rx_socket_fd, &ev) < 0) {
        close(engine->epoll_fd);
        close(engine->rx_socket_fd);
        net_flow_table_destroy(engine->flow_table);
        net_ring_destroy(engine->ring);
        net_arena_destroy(engine->arena);
        free(engine);
        return NULL;
    }

    return engine;
}

void net_engine_destroy(struct net_engine *engine) {
    if (!engine) {
        return;
    }

    engine->running = false;

    if (engine->epoll_fd >= 0) {
        close(engine->epoll_fd);
    }
    if (engine->rx_socket_fd >= 0) {
        close(engine->rx_socket_fd);
    }

    if (engine->flow_table) {
        net_flow_table_destroy(engine->flow_table);
    }
    if (engine->ring) {
        net_ring_destroy(engine->ring);
    }
    if (engine->arena) {
        net_arena_destroy(engine->arena);
    }

    free(engine);
}

static bool net_engine_process_packet(struct net_engine *engine, const uint8_t *data, size_t length) {
    engine->total_rx_packets++;

    if (length < sizeof(struct ether_header)) {
        engine->total_dropped_packets++;
        return false;
    }

    if (!net_flow_validate_and_update(engine->flow_table, data, length, net_engine_current_time_ns())) {
        engine->total_dropped_packets++;
        return false;
    }

    void *arena_buf = net_arena_alloc(engine->arena, length, NET_PACKET_ALIGNMENT);
    if (!arena_buf) {
        engine->total_arena_exhaustions++;
        engine->total_dropped_packets++;
        return false;
    }

    memcpy(arena_buf, data, length);

    struct net_packet_descriptor desc;
    desc.data = arena_buf;
    desc.length = length;
    desc.timestamp = net_engine_current_time_ns();

    if (!net_ring_enqueue(engine->ring, &desc)) {
        engine->total_dropped_packets++;
        return false;
    }

    return true;
}

int net_engine_run(struct net_engine *engine) {
    if (!engine) {
        return -EINVAL;
    }

    engine->running = true;
    struct epoll_event events[NET_ENGINE_MAX_EVENTS];
    uint8_t rx_buffer[65536];

    while (atomic_load(&engine->running)) {
        int n = epoll_wait(engine->epoll_fd, events, NET_ENGINE_MAX_EVENTS, 100);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -errno;
        }

        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == engine->rx_socket_fd) {
                while (true) {
                    ssize_t bytes_received = recv(engine->rx_socket_fd, rx_buffer, sizeof(rx_buffer), MSG_DONTWAIT);
                    if (bytes_received < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        if (errno == EINTR) {
                            continue;
                        }
                        break;
                    } else if (bytes_received == 0) {
                        break;
                    }

                    net_engine_process_packet(engine, rx_buffer, (size_t)bytes_received);
                }
            }
        }
    }

    return 0;
}

void net_engine_stop(struct net_engine *engine) {
    if (engine) {
        atomic_store(&engine->running, false);
    }
}

void net_engine_get_stats(const struct net_engine *engine, struct net_engine_stats *stats) {
    if (!engine || !stats) {
        return;
    }

    stats->total_rx_packets = engine->total_rx_packets;
    stats->total_dropped_packets = engine->total_dropped_packets;
    stats->total_arena_exhaustions = engine->total_arena_exhaustions;
    stats->ring_occupancy = net_ring_count(engine->ring);
    stats->arena_allocated_bytes = net_arena_bytes_allocated(engine->arena);
}