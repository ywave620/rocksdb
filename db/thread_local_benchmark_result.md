
## conclusion

A thread safe counter is really expensive under high contention. Since there is not built in atomic operation for it, it need to use lock. (OCC style  inc/dec does now work high contention)

Frequent atomic operations across threads like spinlock is really bad for mult threads performance.


This is why thread local ptr beats others
This is why RWMutex beats Mutex

**New findings with realistic work simulation:**

When readers do realistic work (hash table lookups, memory allocation, computation) after acquiring the resource:
- The relative advantage of lock-free approaches diminishes as work amortizes acquisition cost
- ThreadLocal pattern still provides 2.5-3x speedup over Mutex even with medium work
- ThreadLocal provides 5-6x speedup over atomic<shared_ptr> with medium work
- RCU-style remains fastest but the gap narrows significantly

## how to run

for macOS:
```sh
make util/thread_local.o port/port_posix.o util/string_util.o -j4 && g++ -std=c++17 -O3 -I. -Iinclude -pthread \
    -DROCKSDB_PLATFORM_POSIX -DROCKSDB_LIB_IO_POSIX -DOS_MACOSX \
    db/thread_local_resource_bench.cc \
    util/thread_local.o port/port_posix.o util/string_util.o \
    -o thread_local_resource_bench
```

for linux:
```sh
g++ -std=c++17 -O3 -I. -Iinclude -pthread \
    -DROCKSDB_PLATFORM_POSIX -DROCKSDB_LIB_IO_POSIX -DOS_LINUX \
    -c util/thread_local.cc -o util/thread_local.o && g++ -std=c++17 -O3 -I. -Iinclude -pthread \
    -DROCKSDB_PLATFORM_POSIX -DROCKSDB_LIB_IO_POSIX -DOS_LINUX \
    -c port/port_posix.cc -o port/port_posix.o && g++ -std=c++17 -O3 -I. -Iinclude -pthread \
    -DROCKSDB_PLATFORM_POSIX -DROCKSDB_LIB_IO_POSIX -DOS_LINUX \
    -c util/string_util.cc -o util/string_util.o && g++ -std=c++17 -O3 -I. -Iinclude -pthread \
    -DROCKSDB_PLATFORM_POSIX -DROCKSDB_LIB_IO_POSIX -DOS_LINUX \
    db/thread_local_resource_bench.cc \
    util/thread_local.o port/port_posix.o util/string_util.o \
    -o thread_local_resource_bench
```

## Benchmark Results - Pure Resource Acquisition (No Work)

● The benchmark results are very illuminating. Let me summarize the findings:

  Benchmark Results Summary

  ┌────────────────────────┬─────────────────────┬──────────┬───────────────────────┐
  │ Approach               │ 8 Readers (M ops/s) │ vs Mutex │ vs atomic<shared_ptr> │
  ├────────────────────────┼─────────────────────┼──────────┼───────────────────────┤
  │ **RCU-style**          │           4416.72 M │   76.12x │                97.54x │
  ├────────────────────────┼─────────────────────┼──────────┼───────────────────────┤
  │ **ThreadLocal**        │            529.51 M │    9.13x │                11.69x │
  ├────────────────────────┼─────────────────────┼──────────┼───────────────────────┤
  │ **Versioned-TL**       │            538.00 M │    9.27x │                11.88x │
  ├────────────────────────┼─────────────────────┼──────────┼───────────────────────┤
  │ **Mutex**              │             58.02 M │    1.00x │                 1.28x │
  ├────────────────────────┼─────────────────────┼──────────┼───────────────────────┤
  │ **atomic<shared_ptr>** │             45.31 M │    0.78x │                 1.00x │
  ├────────────────────────┼─────────────────────┼──────────┼───────────────────────┤
  │ **RWLock**             │             28.32 M │    0.49x │                 0.63x │
  └────────────────────────┴─────────────────────┴──────────┴───────────────────────┘

## Benchmark Results - With Realistic Work Simulation (8 readers)

Work levels tested:
- **Minimal**: ~10 hash lookups
- **Light**: ~50 hash lookups + small allocation (64B)
- **Medium**: ~100 hash lookups + medium allocation (4KB) + computation
- **Heavy**: ~500 hash lookups + large allocation (64KB) + heavy computation

  ┌────────────────────────┬─────────┬─────────┬─────────┬─────────┬─────────┐
  │ Approach               │ None    │ Minimal │ Light   │ Medium  │ Heavy   │
  ├────────────────────────┼─────────┼─────────┼─────────┼─────────┼─────────┤
  │ **RCU-style**          │ 4331.95 │ 403.39  │  88.15  │  47.77  │   2.74  │
  ├────────────────────────┼─────────┼─────────┼─────────┼─────────┼─────────┤
  │ **ThreadLocal**        │  396.44 │ 239.27  │  64.67  │  36.81  │   2.51  │
  ├────────────────────────┼─────────┼─────────┼─────────┼─────────┼─────────┤
  │ **Versioned-TL**       │  385.19 │ 134.11  │  72.96  │  42.73  │   2.52  │
  ├────────────────────────┼─────────┼─────────┼─────────┼─────────┼─────────┤
  │ **Mutex**              │   58.68 │  19.61  │  10.86  │  14.90  │   1.84  │
  ├────────────────────────┼─────────┼─────────┼─────────┼─────────┼─────────┤
  │ **atomic<shared_ptr>** │   46.46 │  17.56  │   8.06  │   7.14  │   1.59  │
  ├────────────────────────┼─────────┼─────────┼─────────┼─────────┼─────────┤
  │ **RWLock**             │   29.35 │  14.22  │   5.55  │   4.37  │   1.50  │
  └────────────────────────┴─────────┴─────────┴─────────┴─────────┴─────────┘
  (All values in M ops/sec)

## Impact of Work on Relative Performance (8 readers)

  ┌──────────────────────┬─────────────┬─────────────┐
  │ Metric               │ No Work     │ Medium Work │
  ├──────────────────────┼─────────────┼─────────────┤
  │ TL vs Mutex          │      6.69x  │      2.66x  │
  ├──────────────────────┼─────────────┼─────────────┤
  │ TL vs atomic<sp>     │      8.77x  │      5.37x  │
  ├──────────────────────┼─────────────┼─────────────┤
  │ atomic<sp> vs Mutex  │      0.76x  │      0.49x  │
  └──────────────────────┴─────────────┴─────────────┘

## Key Findings

1. **RCU-style (single atomic load)** is the fastest by far (~4.4 billion ops/sec with 8
readers with no work), but it has memory reclamation challenges - the simplified version leaks
memory.

2. **ThreadLocal pattern (RocksDB's approach)** achieves ~530M ops/sec with 8 readers (no work),
which is ~11.7x faster than atomic<shared_ptr> and ~9x faster than mutex. It scales
well with readers because the fast path touches no shared state.

3. **atomic<shared_ptr>** using std::atomic_load/store is actually slower than mutex in
this benchmark! This is because:
  - Each read requires acquiring an internal spinlock (libc++ implementation)
  - Plus atomic refcount increment/decrement
  - Heavy cache line contention on the shared_ptr's control block

4. **RWLock (shared_mutex)** performs worst due to its overhead - acquiring even a shared
lock has significant cost.

5. **With realistic work simulation**:
  - ThreadLocal still provides 2.5-3x speedup over Mutex
  - ThreadLocal provides 5-6x speedup over atomic<shared_ptr>
  - The relative advantages diminish as work dominates total time
  - But ThreadLocal remains the best choice for this pattern

## Why ThreadLocal Pattern Wins (for this use case)

The ThreadLocal pattern's advantage is that in steady state (no writes), each
thread's read operation only touches thread-local memory:
- One atomic swap on the thread's own TLS slot
- Read the cached resource
- One atomic CAS to return it

No shared cache lines are bounced between CPUs, making it scale perfectly with the
number of readers.

**Even with realistic work**, the pattern maintains significant advantages because:
1. It avoids cache line bouncing between cores
2. The fast path is truly lock-free (just thread-local operations)  
3. No shared state is touched in the common case
4. Lower contention means threads spend more time doing actual work
