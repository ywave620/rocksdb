
## conclusion

A thread safe counter is really expensive under high contention. Since there is not built in atomic operation for it, it need to use lock. (OCC style  inc/dec does now work high contention)

Frequent atomic operations across threads like spinlock is really bad for mult threads performance.


This is why thread local ptr beats others
This is why RWMutex beats Mutex


## how to run

g++ -std=c++17 -O3 -I. -Iinclude -pthread \
      -DROCKSDB_PLATFORM_POSIX -DROCKSDB_LIB_IO_POSIX -DOS_MACOSX \
      db/thread_local_resource_bench.cc \
      util/thread_local.o port/port_posix.o util/string_util.o \
      -o thread_local_resource_bench

for linux
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


● The benchmark results are very illuminating. Let me summarize the findings:

  Benchmark Results Summary

  ┌────────────────────────┬─────────────────────┬──────────┬───────────────────────┐
  │ Approach               │ 8 Readers (M ops/s) │ vs Mutex │ vs atomic<shared_ptr> │
  ├────────────────────────┼─────────────────────┼──────────┼───────────────────────┤
  │ **RCU-style**          │           7324.56 M │   61.76x │               132.29x │
  ├────────────────────────┼─────────────────────┼──────────┼───────────────────────┤
  │ **ThreadLocal**        │            403.73 M │    3.25x │                 6.97x │
  ├────────────────────────┼─────────────────────┼──────────┼───────────────────────┤
  │ **Mutex**              │             63.21 M │    1.00x │                 1.21x │
  ├────────────────────────┼─────────────────────┼──────────┼───────────────────────┤
  │ **atomic<shared_ptr>** │             52.24 M │    0.47x │                 1.00x │
  ├────────────────────────┼─────────────────────┼──────────┼───────────────────────┤
  │ **RWLock**             │             26.99 M │    0.30x │                 0.52x │
  └────────────────────────┴─────────────────────┴──────────┴───────────────────────┘

  Key Findings:

  1. RCU-style (atomic pointer) is fastest (~7.3 billion ops/sec with 8 readers)
  because readers only do a single atomic load. However, it has complex memory
  reclamation requirements.
  2. ThreadLocal (RocksDB pattern) is ~7x faster than atomic<shared_ptr> and ~3x faster
   than mutex. The key advantage is that in steady state (no writes), threads don't
  touch any shared state.
  3. atomic<shared_ptr> (using std::atomic_load/store) performs worse than mutex in
  this benchmark! This is because:
    - The C++ standard library implementation uses an internal spinlock
    - Every read increments AND decrements the shared refcount
    - This causes massive cache line bouncing between cores
  4. RWLock (shared_mutex) performs worst due to the overhead of managing reader counts
   atomically.

  Why ThreadLocal Beats atomic<shared_ptr>:

  The ThreadLocal pattern avoids the main problem with atomic<shared_ptr>:
  - Fast path: Reader only touches its own thread-local slot (no shared cache lines)
  - Slow path (after write): Only happens once per writer update, then thread caches
  the new value

  With atomic<shared_ptr>, every single read does:
  1. Atomic load of the shared_ptr (internal spinlock)
  2. Atomic increment of refcount
  3. Read the value
  4. Atomic decrement of refcount

  This causes cache line contention that kills scalability.

● The benchmark results are very informative. Here's a summary:

  Benchmark Results Summary

  ┌──────────────────────────┬─────────────────────┬──────────┬────────────────────────
  ─┐
  │ Approach                 │ 8 Readers (M ops/s) │ vs Mutex │ vs atomic\<shared_ptr\>
   │
  ├──────────────────────────┼─────────────────────┼──────────┼────────────────────────
  ─┤
  │ **RCU-style**            │ 7,415 M             │ 124x     │ 148x
   │
  ├──────────────────────────┼─────────────────────┼──────────┼────────────────────────
  ─┤
  │ **ThreadLocal**          │ 400 M               │ 6.9x     │ 8.3x
   │
  ├──────────────────────────┼─────────────────────┼──────────┼────────────────────────
  ─┤
  │ **Mutex**                │ 63 M                │ 1.0x     │ 1.2x
   │
  ├──────────────────────────┼─────────────────────┼──────────┼────────────────────────
  ─┤
  │ **atomic\<shared_ptr\>** │ 51 M                │ 0.84x    │ 1.0x
   │
  ├──────────────────────────┼─────────────────────┼──────────┼────────────────────────
  ─┤
  │ **RWLock**               │ 27 M                │ 0.41x    │ 0.53x
   │
  └──────────────────────────┴─────────────────────┴──────────┴────────────────────────
  ─┘

  Key Findings

  1. RCU-style (single atomic load) is the fastest by far (~7.4 billion ops/sec with 8
  readers), but it has memory reclamation challenges - the simplified version leaks
  memory.
  2. ThreadLocal pattern (RocksDB's approach) achieves ~400M ops/sec with 8 readers,
  which is 8.25x faster than atomicshared_ptr and 6.9x faster than mutex. It scales
  linearly with readers because the fast path touches no shared state.
  3. atomicshared_ptr using std::atomic_load/store is actually slower than mutex in
  this benchmark! This is because:
    - Each read requires acquiring an internal spinlock (libc++ implementation)
    - Plus atomic refcount increment/decrement
    - Heavy cache line contention on the shared_ptr's control block
  4. RWLock (shared_mutex) performs worst due to its overhead - acquiring even a shared
   lock has significant cost.
  5. Mutex actually performs reasonably for single reader but degrades under
  contention.

  Why ThreadLocal Pattern Wins (for this use case)

  The ThreadLocal pattern's advantage is that in steady state (no writes), each
  thread's read operation only touches thread-local memory:
  - One atomic swap on the thread's own TLS slot
  - Read the cached resource
  - One atomic CAS to return it

  No shared cache lines are bounced between CPUs, making it scale perfectly with the
  number of readers.

