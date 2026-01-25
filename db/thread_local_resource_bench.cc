//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Benchmark comparing approaches for managing shared resources with
// multiple readers and infrequent writers:
//
// 1. Thread-Local Pattern (RocksDB's GetThreadLocalSuperVersion)
//    - Caches resource reference in thread-local storage
//    - Fast path: atomic swap, no locks
//    - Slow path (after write): acquire mutex, get new ref
//
// 2. Atomic shared_ptr (using std::atomic_load/store free functions)
//    - Uses C++11/14 atomic operations on shared_ptr
//    - Internally uses spinlock or lock-free depending on implementation
//
// 3. Hazard Pointer style (simplified)
//    - Uses atomic pointer with deferred reclamation
//
// 4. Mutex baseline
//    - Simple mutex protection
//    - High contention under load
//
// Build:
//   g++ -std=c++17 -O3 -I. -Iinclude -pthread \
//       -DROCKSDB_PLATFORM_POSIX -DROCKSDB_LIB_IO_POSIX -DOS_MACOSX \
//       db/thread_local_resource_bench.cc \
//       util/thread_local.o port/port_posix.o util/string_util.o \
//       -o thread_local_resource_bench

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "util/autovector.h"
#include "util/thread_local.h"

namespace ROCKSDB_NAMESPACE {

// =============================================================================
// CounterResource - A simple resource imitating SuperVersion
// =============================================================================
struct CounterResource {
  std::atomic<uint64_t> value;
  std::atomic<uint32_t> refs;
  uint64_t version;  // Immutable after construction

  static int dummy;
  static void* const kInUse;
  static void* const kObsolete;

  explicit CounterResource(uint64_t v, uint64_t ver = 0)
      : value(v), refs(1), version(ver) {}

  CounterResource* Ref() {
    refs.fetch_add(1, std::memory_order_relaxed);
    return this;
  }

  bool Unref() {
    uint32_t prev = refs.fetch_sub(1, std::memory_order_acq_rel);
    assert(prev > 0);
    return prev == 1;
  }
};

int CounterResource::dummy = 0;
void* const CounterResource::kInUse = &CounterResource::dummy;
void* const CounterResource::kObsolete = nullptr;

// =============================================================================
// WorkSimulator - Simulates realistic work after acquiring a resource
// =============================================================================
// This class simulates what a real reader might do after getting a resource:
// - Access a thread-local hash table (like a block cache lookup)
// - Do some memory allocation (like creating iterators or buffers)
// - Perform some computation
//
// This makes the benchmark more realistic by amortizing the resource
// acquisition cost over actual work.
//
class WorkSimulator {
 public:
  // Amount of work to simulate per read operation
  enum class WorkLevel {
    kNone,      // No work - pure resource acquisition benchmark
    kMinimal,   // ~10 hash lookups
    kLight,     // ~50 hash lookups + small allocation
    kMedium,    // ~100 hash lookups + medium allocation + computation
    kHeavy,     // ~500 hash lookups + large allocation + heavy computation
  };

  WorkSimulator() : rng_(std::random_device{}()) {
    // Pre-populate the hash table with some data
    for (int i = 0; i < 10000; i++) {
      local_cache_[i] = i * 17 + 31;
    }
  }

  // Simulate work that a reader might do after acquiring a resource
  // Returns a value to prevent compiler from optimizing away the work
  uint64_t DoWork(WorkLevel level, uint64_t resource_value) {
    switch (level) {
      case WorkLevel::kNone:
        return resource_value;

      case WorkLevel::kMinimal:
        return DoHashLookups(10, resource_value);

      case WorkLevel::kLight:
        return DoHashLookups(50, resource_value) + DoSmallAllocation();

      case WorkLevel::kMedium:
        return DoHashLookups(100, resource_value) + 
               DoMediumAllocation() + 
               DoComputation(100);

      case WorkLevel::kHeavy:
        return DoHashLookups(500, resource_value) + 
               DoLargeAllocation() + 
               DoComputation(1000);
    }
    return resource_value;
  }

 private:
  // Simulate hash table lookups (like block cache lookups)
  uint64_t DoHashLookups(int count, uint64_t seed) {
    uint64_t result = 0;
    for (int i = 0; i < count; i++) {
      int key = (seed + i * 7) % 10000;
      auto it = local_cache_.find(key);
      if (it != local_cache_.end()) {
        result += it->second;
      }
    }
    return result;
  }

  // Simulate small allocation (like creating a small buffer)
  uint64_t DoSmallAllocation() {
    auto ptr = std::make_unique<char[]>(64);
    ptr[0] = 'x';
    return static_cast<uint64_t>(ptr[0]);
  }

  // Simulate medium allocation (like creating an iterator)
  uint64_t DoMediumAllocation() {
    auto ptr = std::make_unique<char[]>(4096);
    ptr[0] = 'x';
    ptr[4095] = 'y';
    return static_cast<uint64_t>(ptr[0] + ptr[4095]);
  }

  // Simulate large allocation (like creating a large buffer)
  uint64_t DoLargeAllocation() {
    auto ptr = std::make_unique<char[]>(65536);
    ptr[0] = 'x';
    ptr[65535] = 'y';
    return static_cast<uint64_t>(ptr[0] + ptr[65535]);
  }

  // Simulate some computation
  uint64_t DoComputation(int iterations) {
    uint64_t result = 0;
    for (int i = 0; i < iterations; i++) {
      result = result * 31 + i;
      result ^= (result >> 17);
    }
    return result;
  }

  std::unordered_map<int, uint64_t> local_cache_;
  std::mt19937_64 rng_;
};

// Thread-local work simulator (each thread gets its own)
thread_local WorkSimulator g_work_simulator;

// =============================================================================
// SharedResource - Resource for shared_ptr approaches
// =============================================================================
struct SharedResource {
  uint64_t value;
  explicit SharedResource(uint64_t v) : value(v) {}
};

// Cleanup callback for ThreadLocalPtr
void ResourceUnrefHandle(void* ptr) {
  CounterResource* res = static_cast<CounterResource*>(ptr);
  if (res != nullptr && res != CounterResource::kInUse &&
      res != CounterResource::kObsolete) {
    bool was_last_ref = res->Unref();
    (void)was_last_ref;
    assert(!was_last_ref);
  }
}

// Cleanup callback for ThreadLocalPtr (versioned)
// Note: Unlike the original pattern where scraping happens before resource
// deletion, the versioned pattern may have thread-local slots holding the
// last reference (since we don't scrape). So we must handle deletion here.
void VersionedResourceUnrefHandle(void* ptr) {
  CounterResource* res = static_cast<CounterResource*>(ptr);
  if (res != nullptr && res != CounterResource::kInUse &&
      res != CounterResource::kObsolete) {
    if (res->Unref()) {
      delete res;
    }
  }
}

// =============================================================================
// ResourceManager - Thread-local caching pattern (RocksDB style)
// =============================================================================
// This is the pattern from column_family.cc GetThreadLocalSuperVersion.
//
// Key insight: In steady state (no writes), each thread's read is just:
//   1. Atomic swap on thread-local slot
//   2. Read the value
//   3. Atomic CAS to return it
// No shared state is touched!
//
// Template parameter UnnecessaryVersionCheck: when true, adds an unnecessary
// version comparison in GetThreadLocalResource to measure the overhead of
// the version check alone (for benchmarking purposes).
//
template <bool UnnecessaryVersionCheck = false>
class ResourceManager {
 public:
  ResourceManager()
      : current_resource_(new CounterResource(0)),
        current_version_(0),
        local_resource_(new ThreadLocalPtr(&ResourceUnrefHandle)) {}

  ~ResourceManager() {
    local_resource_.reset();
    if (current_resource_ != nullptr) {
      if (current_resource_->Unref()) {
        delete current_resource_;
      }
    }
  }

  CounterResource* GetThreadLocalResource() {
    void* ptr = local_resource_->Swap(CounterResource::kInUse);
    assert(ptr != CounterResource::kInUse);
    CounterResource* res = static_cast<CounterResource*>(ptr);

    if (res == CounterResource::kObsolete) {
      std::lock_guard<std::mutex> lock(mutex_);
      res = current_resource_->Ref();
    } else if constexpr (UnnecessaryVersionCheck) {
      // Unnecessary version check - the resource is already valid since
      // we use scraping. This is just to measure the overhead of version
      // comparison in the read path.
      uint64_t current_ver = current_version_.load(std::memory_order_acquire);
      if (res->version != current_ver) {
        // This branch should rarely/never be taken in practice since
        // scraping already invalidates stale resources.
        // The point is just to add the atomic load + compare overhead.
      }
    }

    assert(res != nullptr);
    return res;
  }

  bool ReturnThreadLocalResource(CounterResource* res) {
    assert(res != nullptr);
    void* expected = CounterResource::kInUse;
    if (local_resource_->CompareAndSwap(static_cast<void*>(res), expected)) {
      return true;
    }
    assert(expected == CounterResource::kObsolete);
    return false;
  }

  void InstallNewResource(uint64_t new_value) {
    CounterResource* old_res;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      uint64_t new_version = current_version_.load(std::memory_order_relaxed) + 1;
      CounterResource* new_res = new CounterResource(new_value, new_version);
      old_res = current_resource_;
      current_resource_ = new_res;
      current_version_.store(new_version, std::memory_order_release);
      ResetThreadLocalResources();
    }
    if (old_res != nullptr && old_res->Unref()) {
      delete old_res;
    }
  }

 private:
  void ResetThreadLocalResources() {
    autovector<void*> ptrs;
    local_resource_->Scrape(&ptrs, CounterResource::kObsolete);
    for (auto ptr : ptrs) {
      if (ptr == nullptr || ptr == CounterResource::kInUse ||
          ptr == CounterResource::kObsolete) {
        continue;
      }
      auto* res = static_cast<CounterResource*>(ptr);
      bool was_last_ref = res->Unref();
      (void)was_last_ref;
      assert(!was_last_ref);
    }
  }

  CounterResource* current_resource_;
  std::atomic<uint64_t> current_version_;
  std::unique_ptr<ThreadLocalPtr> local_resource_;
  std::mutex mutex_;
};

// =============================================================================
// VersionedResourceManager - Version-based staleness check
// =============================================================================
// This pattern adds a version field to the resource. Instead of marking
// thread-local slots as obsolete, we compare versions. If the cached
// resource's version doesn't match current, we unref it and get a new one.
//
// Advantages:
// - No need to scrape all thread-local slots on write (O(1) vs O(threads))
// - Readers detect staleness themselves via version comparison
//
// Trade-offs:
// - Readers may hold stale references slightly longer
// - Each read does a version comparison (but avoids mutex in common case)
//
class VersionedResourceManager {
 public:
  VersionedResourceManager()
      : current_resource_(new CounterResource(0, 0)),
        current_version_(0),
        local_resource_(new ThreadLocalPtr(&VersionedResourceUnrefHandle)) {}

  ~VersionedResourceManager() {
    local_resource_.reset();
    if (current_resource_ != nullptr) {
      if (current_resource_->Unref()) {
        delete current_resource_;
      }
    }
  }

  CounterResource* GetThreadLocalResource() {
    void* ptr = local_resource_->Swap(CounterResource::kInUse);
    assert(ptr != CounterResource::kInUse);
    CounterResource* res = static_cast<CounterResource*>(ptr);

    if (res == CounterResource::kObsolete) {
      // First access or explicitly invalidated - acquire from current
      std::lock_guard<std::mutex> lock(mutex_);
      res = current_resource_->Ref();
    } else if (res != nullptr) {
      // Have a cached resource - check if version matches current
      uint64_t current_ver = current_version_.load(std::memory_order_acquire);
      if (res->version != current_ver) {
        // Version mismatch - resource is stale, unref and get current
        if (res->Unref()) {
          delete res;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        res = current_resource_->Ref();
      }
      // else: version matches, use cached resource (fast path!)
    }
    assert(res != nullptr);
    return res;
  }

  bool ReturnThreadLocalResource(CounterResource* res) {
    assert(res != nullptr);
    void* expected = CounterResource::kInUse;
    if (local_resource_->CompareAndSwap(static_cast<void*>(res), expected)) {
      return true;
    }
    assert(expected == CounterResource::kObsolete);
    return false;
  }

  void InstallNewResource(uint64_t new_value) {
    CounterResource* old_res;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      uint64_t new_version = current_version_.load(std::memory_order_relaxed) + 1;
      CounterResource* new_res = new CounterResource(new_value, new_version);
      old_res = current_resource_;
      current_resource_ = new_res;
      // Update version AFTER installing new resource
      current_version_.store(new_version, std::memory_order_release);
      // Note: We do NOT scrape thread-local slots here!
      // Readers will detect staleness via version comparison.
    }
    if (old_res != nullptr && old_res->Unref()) {
      delete old_res;
    }
  }

 private:
  CounterResource* current_resource_;
  std::atomic<uint64_t> current_version_;
  std::unique_ptr<ThreadLocalPtr> local_resource_;
  std::mutex mutex_;
};

// =============================================================================
// AtomicSharedPtrManager - Using std::atomic_load/store on shared_ptr
// =============================================================================
// C++11/14 provides atomic operations on shared_ptr via free functions.
// These are typically implemented with a spinlock internally.
//
// Note: C++20's std::atomic<std::shared_ptr<T>> would be cleaner but isn't
// fully supported on all platforms yet.
//
class AtomicSharedPtrManager {
 public:
  AtomicSharedPtrManager() : resource_(std::make_shared<SharedResource>(0)) {}

  // Reader: atomically load the shared_ptr
  std::shared_ptr<SharedResource> GetResource() {
    return std::atomic_load_explicit(&resource_, std::memory_order_acquire);
  }

  // Writer: atomically store new shared_ptr
  void InstallNewResource(uint64_t new_value) {
    auto new_res = std::make_shared<SharedResource>(new_value);
    std::atomic_store_explicit(&resource_, new_res, std::memory_order_release);
  }

 private:
  std::shared_ptr<SharedResource> resource_;
};

// =============================================================================
// RCUStyleManager - Read-Copy-Update inspired approach
// =============================================================================
// Uses atomic pointer with epoch-based reclamation (simplified).
// Readers just do atomic load (very fast), but memory reclamation
// requires tracking when all readers have finished.
//
// This is a simplified version - real RCU is more complex.
//
class RCUStyleManager {
 public:
  RCUStyleManager() : current_(new SharedResource(0)), version_(0) {}

  ~RCUStyleManager() {
    delete current_.load(std::memory_order_relaxed);
  }

  // Reader: just atomic load - super fast
  // Caller must NOT hold the pointer across a long operation
  const SharedResource* GetResource() {
    return current_.load(std::memory_order_acquire);
  }

  // Writer: create new resource and swap
  // Old resource leaked in this simplified version (real RCU would defer delete)
  void InstallNewResource(uint64_t new_value) {
    SharedResource* new_res = new SharedResource(new_value);
    SharedResource* old = current_.exchange(new_res, std::memory_order_acq_rel);
    
    // In real RCU, we'd defer this deletion until all readers are done
    // For this benchmark, we'll just leak it (or use a simple delay)
    // This is a simplification - real systems use grace periods
    version_.fetch_add(1, std::memory_order_relaxed);
    
    // Simple grace period - wait a bit then delete
    // In production, use proper epoch-based reclamation
    retired_.push_back(old);
    if (retired_.size() > 1000) {
      // Very crude cleanup - just delete old ones
      for (size_t i = 0; i < retired_.size() - 100; i++) {
        delete retired_[i];
      }
      retired_.erase(retired_.begin(), retired_.begin() + (retired_.size() - 100));
    }
  }

 private:
  std::atomic<SharedResource*> current_;
  std::atomic<uint64_t> version_;
  std::vector<SharedResource*> retired_;  // Leaked for simplicity
  std::mutex cleanup_mutex_;
};

// =============================================================================
// MutexResourceManager - Simple mutex baseline
// =============================================================================
class MutexResourceManager {
 public:
  MutexResourceManager() : value_(0) {}

  uint64_t GetValue() {
    std::lock_guard<std::mutex> lock(mutex_);
    return value_;
  }

  void SetValue(uint64_t v) {
    std::lock_guard<std::mutex> lock(mutex_);
    value_ = v;
  }

 private:
  uint64_t value_;
  std::mutex mutex_;
};

// =============================================================================
// RWLockResourceManager - Using shared_mutex (reader-writer lock)
// =============================================================================
class RWLockResourceManager {
 public:
  RWLockResourceManager() : value_(0) {}

  uint64_t GetValue() {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return value_;
  }

  void SetValue(uint64_t v) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    value_ = v;
  }

 private:
  uint64_t value_;
  std::shared_mutex mutex_;
};

// =============================================================================
// Benchmark runner
// =============================================================================
class Benchmark {
 public:
  struct Config {
    int num_readers = 4;
    int num_writers = 1;
    int duration_seconds = 3;
    int write_interval_us = 1000;
    WorkSimulator::WorkLevel work_level = WorkSimulator::WorkLevel::kNone;
  };

  struct Results {
    uint64_t total_reads = 0;
    uint64_t total_writes = 0;
    double reads_per_second = 0;
    double writes_per_second = 0;
  };

  static const char* WorkLevelName(WorkSimulator::WorkLevel level) {
    switch (level) {
      case WorkSimulator::WorkLevel::kNone: return "None";
      case WorkSimulator::WorkLevel::kMinimal: return "Minimal";
      case WorkSimulator::WorkLevel::kLight: return "Light";
      case WorkSimulator::WorkLevel::kMedium: return "Medium";
      case WorkSimulator::WorkLevel::kHeavy: return "Heavy";
    }
    return "Unknown";
  }

  // Generic benchmark for ResourceManager-like types
  template <typename ManagerType>
  static Results RunResourceManagerBenchmark(const Config& config) {
    ManagerType manager;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> total_reads{0};
    std::atomic<uint64_t> total_writes{0};
    WorkSimulator::WorkLevel work_level = config.work_level;

    std::vector<std::thread> readers;
    for (int i = 0; i < config.num_readers; i++) {
      readers.emplace_back([&, work_level]() {
        uint64_t local_reads = 0;
        volatile uint64_t work_result = 0;
        while (!stop.load(std::memory_order_relaxed)) {
          CounterResource* res = manager.GetThreadLocalResource();
          uint64_t v = res->value.load(std::memory_order_relaxed);
          // Simulate work after acquiring resource
          work_result += g_work_simulator.DoWork(work_level, v);
          if (!manager.ReturnThreadLocalResource(res)) {
            if (res->Unref()) delete res;
          }
          local_reads++;
        }
        (void)work_result;
        total_reads.fetch_add(local_reads, std::memory_order_relaxed);
      });
    }

    std::vector<std::thread> writers;
    for (int i = 0; i < config.num_writers; i++) {
      writers.emplace_back([&]() {
        uint64_t local_writes = 0;
        uint64_t value = 0;
        while (!stop.load(std::memory_order_relaxed)) {
          manager.InstallNewResource(++value);
          local_writes++;
          std::this_thread::sleep_for(
              std::chrono::microseconds(config.write_interval_us));
        }
        total_writes.fetch_add(local_writes, std::memory_order_relaxed);
      });
    }

    std::this_thread::sleep_for(std::chrono::seconds(config.duration_seconds));
    stop.store(true, std::memory_order_relaxed);

    for (auto& t : readers) t.join();
    for (auto& t : writers) t.join();

    Results results;
    results.total_reads = total_reads.load();
    results.total_writes = total_writes.load();
    results.reads_per_second =
        static_cast<double>(results.total_reads) / config.duration_seconds;
    results.writes_per_second =
        static_cast<double>(results.total_writes) / config.duration_seconds;
    return results;
  }

  // Benchmark: Thread-Local Pattern
  template <bool UnnecessaryVersionCheck = false>
  static Results RunThreadLocalBenchmark(const Config& config) {
    return RunResourceManagerBenchmark<ResourceManager<UnnecessaryVersionCheck>>(config);
  }

  // Benchmark: Versioned Thread-Local Pattern
  static Results RunVersionedThreadLocalBenchmark(const Config& config) {
    return RunResourceManagerBenchmark<VersionedResourceManager>(config);
  }

  // Benchmark: atomic shared_ptr
  static Results RunAtomicSharedPtrBenchmark(const Config& config) {
    AtomicSharedPtrManager manager;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> total_reads{0};
    std::atomic<uint64_t> total_writes{0};
    WorkSimulator::WorkLevel work_level = config.work_level;

    std::vector<std::thread> readers;
    for (int i = 0; i < config.num_readers; i++) {
      readers.emplace_back([&, work_level]() {
        uint64_t local_reads = 0;
        volatile uint64_t work_result = 0;
        while (!stop.load(std::memory_order_relaxed)) {
          auto res = manager.GetResource();
          uint64_t v = res->value;
          // Simulate work after acquiring resource
          work_result += g_work_simulator.DoWork(work_level, v);
          local_reads++;
        }
        (void)work_result;
        total_reads.fetch_add(local_reads, std::memory_order_relaxed);
      });
    }

    std::vector<std::thread> writers;
    for (int i = 0; i < config.num_writers; i++) {
      writers.emplace_back([&]() {
        uint64_t local_writes = 0;
        uint64_t value = 0;
        while (!stop.load(std::memory_order_relaxed)) {
          manager.InstallNewResource(++value);
          local_writes++;
          std::this_thread::sleep_for(
              std::chrono::microseconds(config.write_interval_us));
        }
        total_writes.fetch_add(local_writes, std::memory_order_relaxed);
      });
    }

    std::this_thread::sleep_for(std::chrono::seconds(config.duration_seconds));
    stop.store(true, std::memory_order_relaxed);

    for (auto& t : readers) t.join();
    for (auto& t : writers) t.join();

    Results results;
    results.total_reads = total_reads.load();
    results.total_writes = total_writes.load();
    results.reads_per_second =
        static_cast<double>(results.total_reads) / config.duration_seconds;
    results.writes_per_second =
        static_cast<double>(results.total_writes) / config.duration_seconds;
    return results;
  }

  // Benchmark: RCU-style (atomic pointer)
  static Results RunRCUStyleBenchmark(const Config& config) {
    RCUStyleManager manager;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> total_reads{0};
    std::atomic<uint64_t> total_writes{0};
    WorkSimulator::WorkLevel work_level = config.work_level;

    std::vector<std::thread> readers;
    for (int i = 0; i < config.num_readers; i++) {
      readers.emplace_back([&, work_level]() {
        uint64_t local_reads = 0;
        volatile uint64_t work_result = 0;
        while (!stop.load(std::memory_order_relaxed)) {
          const SharedResource* res = manager.GetResource();
          uint64_t v = res->value;
          // Simulate work after acquiring resource
          work_result += g_work_simulator.DoWork(work_level, v);
          local_reads++;
        }
        (void)work_result;
        total_reads.fetch_add(local_reads, std::memory_order_relaxed);
      });
    }

    std::vector<std::thread> writers;
    for (int i = 0; i < config.num_writers; i++) {
      writers.emplace_back([&]() {
        uint64_t local_writes = 0;
        uint64_t value = 0;
        while (!stop.load(std::memory_order_relaxed)) {
          manager.InstallNewResource(++value);
          local_writes++;
          std::this_thread::sleep_for(
              std::chrono::microseconds(config.write_interval_us));
        }
        total_writes.fetch_add(local_writes, std::memory_order_relaxed);
      });
    }

    std::this_thread::sleep_for(std::chrono::seconds(config.duration_seconds));
    stop.store(true, std::memory_order_relaxed);

    for (auto& t : readers) t.join();
    for (auto& t : writers) t.join();

    Results results;
    results.total_reads = total_reads.load();
    results.total_writes = total_writes.load();
    results.reads_per_second =
        static_cast<double>(results.total_reads) / config.duration_seconds;
    results.writes_per_second =
        static_cast<double>(results.total_writes) / config.duration_seconds;
    return results;
  }

  // Benchmark: Mutex
  static Results RunMutexBenchmark(const Config& config) {
    MutexResourceManager manager;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> total_reads{0};
    std::atomic<uint64_t> total_writes{0};
    WorkSimulator::WorkLevel work_level = config.work_level;

    std::vector<std::thread> readers;
    for (int i = 0; i < config.num_readers; i++) {
      readers.emplace_back([&, work_level]() {
        uint64_t local_reads = 0;
        volatile uint64_t work_result = 0;
        while (!stop.load(std::memory_order_relaxed)) {
          uint64_t v = manager.GetValue();
          // Simulate work after acquiring resource
          work_result += g_work_simulator.DoWork(work_level, v);
          local_reads++;
        }
        (void)work_result;
        total_reads.fetch_add(local_reads, std::memory_order_relaxed);
      });
    }

    std::vector<std::thread> writers;
    for (int i = 0; i < config.num_writers; i++) {
      writers.emplace_back([&]() {
        uint64_t local_writes = 0;
        uint64_t value = 0;
        while (!stop.load(std::memory_order_relaxed)) {
          manager.SetValue(++value);
          local_writes++;
          std::this_thread::sleep_for(
              std::chrono::microseconds(config.write_interval_us));
        }
        total_writes.fetch_add(local_writes, std::memory_order_relaxed);
      });
    }

    std::this_thread::sleep_for(std::chrono::seconds(config.duration_seconds));
    stop.store(true, std::memory_order_relaxed);

    for (auto& t : readers) t.join();
    for (auto& t : writers) t.join();

    Results results;
    results.total_reads = total_reads.load();
    results.total_writes = total_writes.load();
    results.reads_per_second =
        static_cast<double>(results.total_reads) / config.duration_seconds;
    results.writes_per_second =
        static_cast<double>(results.total_writes) / config.duration_seconds;
    return results;
  }

  // Benchmark: RWLock (shared_mutex)
  static Results RunRWLockBenchmark(const Config& config) {
    RWLockResourceManager manager;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> total_reads{0};
    std::atomic<uint64_t> total_writes{0};
    WorkSimulator::WorkLevel work_level = config.work_level;

    std::vector<std::thread> readers;
    for (int i = 0; i < config.num_readers; i++) {
      readers.emplace_back([&, work_level]() {
        uint64_t local_reads = 0;
        volatile uint64_t work_result = 0;
        while (!stop.load(std::memory_order_relaxed)) {
          uint64_t v = manager.GetValue();
          // Simulate work after acquiring resource
          work_result += g_work_simulator.DoWork(work_level, v);
          local_reads++;
        }
        (void)work_result;
        total_reads.fetch_add(local_reads, std::memory_order_relaxed);
      });
    }

    std::vector<std::thread> writers;
    for (int i = 0; i < config.num_writers; i++) {
      writers.emplace_back([&]() {
        uint64_t local_writes = 0;
        uint64_t value = 0;
        while (!stop.load(std::memory_order_relaxed)) {
          manager.SetValue(++value);
          local_writes++;
          std::this_thread::sleep_for(
              std::chrono::microseconds(config.write_interval_us));
        }
        total_writes.fetch_add(local_writes, std::memory_order_relaxed);
      });
    }

    std::this_thread::sleep_for(std::chrono::seconds(config.duration_seconds));
    stop.store(true, std::memory_order_relaxed);

    for (auto& t : readers) t.join();
    for (auto& t : writers) t.join();

    Results results;
    results.total_reads = total_reads.load();
    results.total_writes = total_writes.load();
    results.reads_per_second =
        static_cast<double>(results.total_reads) / config.duration_seconds;
    results.writes_per_second =
        static_cast<double>(results.total_writes) / config.duration_seconds;
    return results;
  }
};

}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char** argv) {
  using namespace ROCKSDB_NAMESPACE;

  printf("=============================================================================\n");
  printf("  Benchmark: Concurrent Read-Heavy Resource Access Patterns\n");
  printf("=============================================================================\n\n");

  Benchmark::Config config;
  config.duration_seconds = 3;
  config.write_interval_us = 1000;  // 1ms between writes

  printf("Configuration:\n");
  printf("  Duration: %d seconds per test\n", config.duration_seconds);
  printf("  Write interval: %d us (writers sleep between updates)\n",
         config.write_interval_us);
  printf("  Scenario: Read-heavy workload with infrequent updates\n\n");

  // =========================================================================
  // Part 1: No work (pure acquisition benchmark) - original behavior
  // =========================================================================
  printf("=============================================================================\n");
  printf("  Part 1: No Work (Pure Resource Acquisition)\n");
  printf("=============================================================================\n\n");

  config.work_level = WorkSimulator::WorkLevel::kNone;

  // Header
  printf("%-6s | %-14s | %-14s | %-14s | %-14s | %-14s | %-14s | %-14s\n",
         "Rdrs", "ThreadLocal", "TL+VerChk", "Versioned-TL", "atomic<sp>", "RCU-style", "RWLock", "Mutex");
  printf("-------|----------------|----------------|----------------|----------------|----------------|----------------|----------------\n");

  std::vector<int> reader_counts = {1, 2, 4, 8, 16};

  for (int num_readers : reader_counts) {
    config.num_readers = num_readers;

    auto tl = Benchmark::RunThreadLocalBenchmark<false>(config);
    auto tlvc = Benchmark::RunThreadLocalBenchmark<true>(config);
    auto vtl = Benchmark::RunVersionedThreadLocalBenchmark(config);
    auto asp = Benchmark::RunAtomicSharedPtrBenchmark(config);
    auto rcu = Benchmark::RunRCUStyleBenchmark(config);
    auto rwl = Benchmark::RunRWLockBenchmark(config);
    auto mtx = Benchmark::RunMutexBenchmark(config);

    printf("%-6d | %11.2f M | %11.2f M | %11.2f M | %11.2f M | %11.2f M | %11.2f M | %11.2f M\n",
           num_readers,
           tl.reads_per_second / 1e6,
           tlvc.reads_per_second / 1e6,
           vtl.reads_per_second / 1e6,
           asp.reads_per_second / 1e6,
           rcu.reads_per_second / 1e6,
           rwl.reads_per_second / 1e6,
           mtx.reads_per_second / 1e6);
  }

  // =========================================================================
  // Part 2: With realistic work simulation
  // =========================================================================
  printf("\n=============================================================================\n");
  printf("  Part 2: With Work Simulation (8 readers)\n");
  printf("=============================================================================\n\n");

  printf("Work levels:\n");
  printf("  - Minimal: ~10 hash lookups\n");
  printf("  - Light:   ~50 hash lookups + small allocation (64B)\n");
  printf("  - Medium:  ~100 hash lookups + medium allocation (4KB) + computation\n");
  printf("  - Heavy:   ~500 hash lookups + large allocation (64KB) + heavy computation\n\n");

  config.num_readers = 8;

  // Header
  printf("%-10s | %-12s | %-12s | %-12s | %-12s | %-12s | %-12s | %-12s\n",
         "Work", "ThreadLocal", "TL+VerChk", "Versioned-TL", "atomic<sp>", "RCU-style", "RWLock", "Mutex");
  printf("-----------|--------------|--------------|--------------|--------------|--------------|--------------|-------------\n");

  std::vector<WorkSimulator::WorkLevel> work_levels = {
      WorkSimulator::WorkLevel::kNone,
      WorkSimulator::WorkLevel::kMinimal,
      WorkSimulator::WorkLevel::kLight,
      WorkSimulator::WorkLevel::kMedium,
      WorkSimulator::WorkLevel::kHeavy,
  };

  for (auto work_level : work_levels) {
    config.work_level = work_level;

    auto tl = Benchmark::RunThreadLocalBenchmark<false>(config);
    auto tlvc = Benchmark::RunThreadLocalBenchmark<true>(config);
    auto vtl = Benchmark::RunVersionedThreadLocalBenchmark(config);
    auto asp = Benchmark::RunAtomicSharedPtrBenchmark(config);
    auto rcu = Benchmark::RunRCUStyleBenchmark(config);
    auto rwl = Benchmark::RunRWLockBenchmark(config);
    auto mtx = Benchmark::RunMutexBenchmark(config);

    printf("%-10s | %9.2f M | %9.2f M | %9.2f M | %9.2f M | %9.2f M | %9.2f M | %9.2f M\n",
           Benchmark::WorkLevelName(work_level),
           tl.reads_per_second / 1e6,
           tlvc.reads_per_second / 1e6,
           vtl.reads_per_second / 1e6,
           asp.reads_per_second / 1e6,
           rcu.reads_per_second / 1e6,
           rwl.reads_per_second / 1e6,
           mtx.reads_per_second / 1e6);
  }

  // =========================================================================
  // Part 3: Speedup analysis with Medium work (realistic case)
  // =========================================================================
  printf("\n=============================================================================\n");
  printf("  Part 3: Speedup Analysis (8 readers, Medium work)\n");
  printf("=============================================================================\n\n");

  config.num_readers = 8;
  config.work_level = WorkSimulator::WorkLevel::kMedium;

  auto tl = Benchmark::RunThreadLocalBenchmark<false>(config);
  auto tlvc = Benchmark::RunThreadLocalBenchmark<true>(config);
  auto vtl = Benchmark::RunVersionedThreadLocalBenchmark(config);
  auto asp = Benchmark::RunAtomicSharedPtrBenchmark(config);
  auto rcu = Benchmark::RunRCUStyleBenchmark(config);
  auto rwl = Benchmark::RunRWLockBenchmark(config);
  auto mtx = Benchmark::RunMutexBenchmark(config);

  printf("=== Speedup vs Mutex (8 readers, Medium work) ===\n");
  printf("ThreadLocal:     %.2fx\n", tl.reads_per_second / mtx.reads_per_second);
  printf("TL+VerChk:       %.2fx\n", tlvc.reads_per_second / mtx.reads_per_second);
  printf("Versioned-TL:    %.2fx\n", vtl.reads_per_second / mtx.reads_per_second);
  printf("atomic<sp>:      %.2fx\n", asp.reads_per_second / mtx.reads_per_second);
  printf("RCU-style:       %.2fx\n", rcu.reads_per_second / mtx.reads_per_second);
  printf("RWLock:          %.2fx\n", rwl.reads_per_second / mtx.reads_per_second);

  printf("\n=== Speedup vs atomic<shared_ptr> (8 readers, Medium work) ===\n");
  printf("ThreadLocal:     %.2fx\n", tl.reads_per_second / asp.reads_per_second);
  printf("TL+VerChk:       %.2fx\n", tlvc.reads_per_second / asp.reads_per_second);
  printf("Versioned-TL:    %.2fx\n", vtl.reads_per_second / asp.reads_per_second);
  printf("RCU-style:       %.2fx\n", rcu.reads_per_second / asp.reads_per_second);

  printf("\n=== Version check overhead (8 readers, Medium work) ===\n");
  printf("TL+VerChk vs TL: %.2fx\n", tlvc.reads_per_second / tl.reads_per_second);
  printf("Versioned-TL vs TL: %.2fx\n", vtl.reads_per_second / tl.reads_per_second);

  // =========================================================================
  // Part 4: Comparison - No work vs Medium work
  // =========================================================================
  printf("\n=============================================================================\n");
  printf("  Part 4: Impact of Work on Relative Performance (8 readers)\n");
  printf("=============================================================================\n\n");

  config.work_level = WorkSimulator::WorkLevel::kNone;
  auto tl_nowork = Benchmark::RunThreadLocalBenchmark<false>(config);
  auto asp_nowork = Benchmark::RunAtomicSharedPtrBenchmark(config);
  auto mtx_nowork = Benchmark::RunMutexBenchmark(config);

  config.work_level = WorkSimulator::WorkLevel::kMedium;
  auto tl_work = Benchmark::RunThreadLocalBenchmark<false>(config);
  auto asp_work = Benchmark::RunAtomicSharedPtrBenchmark(config);
  auto mtx_work = Benchmark::RunMutexBenchmark(config);

  printf("%-20s | %-15s | %-15s\n", "Metric", "No Work", "Medium Work");
  printf("---------------------|-----------------|----------------\n");
  printf("%-20s | %12.2fx | %12.2fx\n", "TL vs Mutex",
         tl_nowork.reads_per_second / mtx_nowork.reads_per_second,
         tl_work.reads_per_second / mtx_work.reads_per_second);
  printf("%-20s | %12.2fx | %12.2fx\n", "TL vs atomic<sp>",
         tl_nowork.reads_per_second / asp_nowork.reads_per_second,
         tl_work.reads_per_second / asp_work.reads_per_second);
  printf("%-20s | %12.2fx | %12.2fx\n", "atomic<sp> vs Mutex",
         asp_nowork.reads_per_second / mtx_nowork.reads_per_second,
         asp_work.reads_per_second / mtx_work.reads_per_second);

  printf("\n=== Analysis ===\n\n");

  printf("Key Insight: When readers do real work after acquiring the resource,\n");
  printf("the relative advantage of lock-free approaches diminishes because:\n");
  printf("1. The work dominates the total time, amortizing acquisition cost\n");
  printf("2. Contention is reduced as threads spend more time doing work\n");
  printf("3. Lock-based approaches become more competitive\n\n");

  printf("However, the ThreadLocal pattern still wins because:\n");
  printf("1. It avoids cache line bouncing between cores\n");
  printf("2. The fast path is truly lock-free (just thread-local operations)\n");
  printf("3. No shared state is touched in the common case\n\n");

  printf("1. ThreadLocal (RocksDB pattern):\n");
  printf("   - Best for: High-frequency reads, infrequent writes\n");
  printf("   - Mechanism: Per-thread cached reference, scrape on write\n");
  printf("   - Fast path: atomic swap (no shared state touched)\n");
  printf("   - Trade-off: Complex implementation, O(threads) write cost\n\n");

  printf("2. Versioned ThreadLocal:\n");
  printf("   - Best for: High-frequency reads, infrequent writes, many threads\n");
  printf("   - Mechanism: Per-thread cached reference with version check\n");
  printf("   - Fast path: atomic swap + version compare (no scrape needed)\n");
  printf("   - Trade-off: Extra version comparison, but O(1) write cost\n\n");

  printf("3. atomic<shared_ptr> (std::atomic_load/store):\n");
  printf("   - Best for: Simple code, moderate read frequency\n");
  printf("   - Mechanism: Atomic load/store with internal spinlock\n");
  printf("   - Trade-off: Every read touches shared refcount\n\n");

  printf("4. RCU-style (atomic pointer):\n");
  printf("   - Best for: Extreme read performance, can leak/defer cleanup\n");
  printf("   - Mechanism: Single atomic load, deferred reclamation\n");
  printf("   - Trade-off: Memory reclamation complexity, memory usage\n\n");

  printf("5. RWLock (shared_mutex):\n");
  printf("   - Best for: Simple code, mixed read/write workloads\n");
  printf("   - Mechanism: Reader-writer lock\n");
  printf("   - Trade-off: Still has contention, writer starvation possible\n\n");

  printf("6. Mutex:\n");
  printf("   - Best for: Simple code, low contention\n");
  printf("   - Trade-off: Serializes all access\n");

  return 0;
}
