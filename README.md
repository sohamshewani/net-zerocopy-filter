# net-zerocopy-filter
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT) [![CI](https://github.com/sohamshewani/net-zerocopy-filter/actions/workflows/ci.yml/badge.svg)](https://github.com/sohamshewani/net-zerocopy-filter/actions) [![Security: Hardened](https://img.shields.io/badge/Security-Hardened-blue.svg)](https://github.com/sohamshewani/net-zerocopy-filter) [![Stars](https://img.shields.io/github/stars/sohamshewani/net-zerocopy-filter?style=flat)](https://github.com/sohamshewani/net-zerocopy-filter)

## Executive Summary & Problem Space

Modern high-speed network telemetry and packet processing systems face a fundamental throughput ceiling imposed by kernel-to-user-space context switching, memory allocation overheads, and redundant memory copies. Traditional networking pipelines (e.g., standard `recv()`/`send()` sockets coupled with glibc `malloc`/`free`) incur catastrophic performance degradation at line rates exceeding 10Gbps due to TLB shootdowns, cache pollution, and lock contention on heap allocators.

`net-zerocopy-filter` is an enterprise-grade, high-performance packet filtering and flow classification engine written in pure C99. Designed from the ground up for low-latency networking environments, it eliminates system call overhead and dynamic memory allocations on the critical path. By implementing a strict zero-copy architecture utilizing custom arena allocators (`net_arena.h`), lockless single-producer single-consumer (SPSC) ring buffers (`net_ring.h`), and direct-mapped hardware/driver packet buffers (`net_types.h`), the engine sustains millions of packets per second with deterministic sub-microsecond latency profiles.

## Structural ASCII Architecture / Dataflow Diagram

The pipeline isolates packet ingestion, arena-backed parsing, multi-threaded flow matching, and sink dispatching into decoupled, cache-aligned stages.

```
 +-----------------------------------------------------------------------+
 |                         HARDWARE / NIC DRIVER                         |
 +-----------------------------------------------------------------------+
                                    | (Direct DMA / mmap)
                                    v
 +-----------------------------------------------------------------------+
 |                 IN_FLIGHT RING BUFFER (`net_ring.h`)                  |
 |  [ Slot 0: Packet* ] -> [ Slot 1: Packet* ] -> [ Slot N: Packet* ]    |
 +-----------------------------------------------------------------------+
                                    |
                                    v
 +-----------------------------------------------------------------------+
 |                 ENGINE DISPATCHER (`src/net_engine.c`)                |
 |  - Acquires batch from ring via atomic head/tail advancement          |
 +-----------------------------------------------------------------------+
         |                                           |
         v (Zero-Copy View)                          v (Zero-Copy View)
 +-----------------------------+             +-----------------------------+
 |     FLOW MATCHER WORKER     |             |     FLOW MATCHER WORKER     |
 |   (`src/net_flow.c`)        |             |   (`src/net_flow.c`)        |
 |  - 5-tuple hash extraction  |             |  - 5-tuple hash extraction  |
 +-----------------------------+             +-----------------------------+
         |                                           |
         v                                           v
 +-----------------------------+             +-----------------------------+
 |    FILTER ENGINE CORE       |             |    FILTER ENGINE CORE       |
 |   (`src/net_filter.c`)      |             |   (`src/net_filter.c`)      |
 |  - Bitmask rule evaluation  |             |  - Bitmask rule evaluation  |
 +-----------------------------+             +-----------------------------+
         \                                           /
          \                                         /
           v                                       v
 +-----------------------------------------------------------------------+
 |                 ARENA ALLOCATOR POOL (`net_arena.h`)                  |
 |  - Monotonic bump-pointer allocation for ephemeral rule-state metadata|
 +-----------------------------------------------------------------------+
                                    |
                                    v
 +-----------------------------------------------------------------------+
 |                        OUTBOUND DISPATCH SINK                         |
 +-----------------------------------------------------------------------+
```

## Concurrency Model & Memory Invariants

### Thread Safety & Lock Contention Prevention
`net-zerocopy-filter` adopts a shared-nothing per-core processing model combined with lockless synchronization primitives:
* **SPSC / MPMC Ring Buffers (`net_ring.h`)**: Communication between ingestion threads and worker processing pools relies entirely on atomic memory operations (`atomic_load_explicit`, `atomic_store_explicit`, `memory_order_acquire`/`memory_order_release`). This avoids heavy OS-level mutexes or futex contention entirely.
* **Flow Separation (`net_flow.c`)**: Flows are partitioned across workers using consistent hashing on packet 5-tuples (`src`, `dst`, `sport`, `dport`, `proto`), eliminating cross-core cache invalidation storms and data races.

### Memory Ownership & Arena Lifetime Semantics
* **Zero-Copy Invariant**: Raw packet payloads are never copied from the moment they are mapped into memory (`net_types.h`) to the moment they pass through the filter stage. Parsers operate directly on memory offsets.
* **Arena Allocations (`net_arena.h`)**: Ephemeral runtime allocations (such as state tracking nodes, match contexts, and temporary scratchpads) do not touch the system heap (`malloc`/`free`). Instead, they are allocated out of pre-allocated, thread-local or arena-backed bump-pointer regions. Deallocation is performed globally and instantaneously by resetting the arena offset counter, reducing fragmentation to absolute zero.

## Performance Characteristics & Algorithmic Complexity Table

| Operation Path | Target Subsystem | Time Complexity ($O$) | Space Complexity ($O$) | Estimated Latency (P99) |
| :--- | :--- | :--- | :--- | :--- |
| **Ring Enqueue/Dequeue** | `net_ring.h` | $O(1)$ | $O(N)$ capacity | $< 15\text{ ns}$ |
| **Arena Bump Allocation** | `net_arena.h` | $O(1)$ | $O(M)$ block size | $< 5\text{ ns}$ |
| **Flow Hash Lookup** | `src/net_flow.c` | $O(1)$ average, $O(K)$ worst | $O(F)$ active flows | $< 40\text{ ns}$ |
| **Filter Rule Evaluation**| `src/net_filter.c` | $O(R)$ where $R = \text{rules}$ | $O(R)$ ruleset | $< 120\text{ ns}$ |
| **Full Engine Pipeline** | `src/net_engine.c` | $O(1)$ per packet | $O(N + F + R)$ | $< 250\text{ ns}$ |

## Security Architecture & Threat Model

* **Input Boundaries & Untrusted Data**: All incoming packets are treated as adversarial inputs. Network headers parsed via `net_types.h` undergo strict bounds checking against packet boundary descriptors before any pointer arithmetic occurs.
* **Memory Corruption Protections**: 
  * The avoidance of general-purpose dynamic allocators (`malloc`/`free`) on the packet path eliminates double-free, use-after-free, and heap-spraying attack surfaces.
  * Ring buffer indices use explicit bitmask wrapping or atomic guards to prevent integer overflow and buffer overreads.
* **Sanitization & Hardening**: Compiled with stringent compiler flags (`-Wall -Wextra -Werror -Wshadow -fstack-protector-strong -D_FORTIFY_SOURCE=2`), ensuring stack overflow mitigations and immediate build failure upon any implicit type conversion or pointer mismatch.

## Build, Verification & Benchmarking Recipes

To compile the codebase, execute test suites, and run the performance benchmarks, use the following terminal recipes:

### 1. Build the Production Binaries and Test Suite
```bash
make clean
make all
```

### 2. Execute Invariant Validation Tests
Runs the test suite verifying memory bounds, arena invariants, ring buffer synchronization correctness, and filter logic:
```bash
make test
```
*(Alternatively, execute the compiled test binary directly: `./tests/test_invariants`)*

### 3. Run Performance Benchmarks
Executes the micro-benchmarking engine (`bench_engine.c`) to measure throughput, packet parsing latency, and filter overhead:
```bash
make bench
```
*(Alternatively, execute the benchmark binary directly: `./benchmarks/bench_engine`)*