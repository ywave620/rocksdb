//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Open-loop workload implementation for db_bench

#include "tools/open_loop_workload.h"

#include <cmath>
#include "util/mutexlock.h"

namespace ROCKSDB_NAMESPACE {

// ============================================================================
// ArrivalRateController Base Class
// ============================================================================

ArrivalRateController::ArrivalRateController(double base_rate_ops_per_sec, 
                                             SystemClock* clock)
    : base_rate_(base_rate_ops_per_sec),
      clock_(clock),
      start_time_(clock->NowMicros()) {}

double ArrivalRateController::GetCurrentRateOpsPerSec() {
  return base_rate_;
}

uint64_t ArrivalRateController::ExponentialInterArrival(double rate_ops_per_sec) {
  if (rate_ops_per_sec <= 0) {
    return 1000000;  // 1 second default if rate is invalid
  }
  
  // Mean inter-arrival time in microseconds
  double mean_micros = 1000000.0 / rate_ops_per_sec;
  
  // Generate exponentially distributed value using inverse transform
  static thread_local std::mt19937_64 gen(std::random_device{}());
  std::uniform_real_distribution<double> uniform(0.0, 1.0);
  double u = uniform(gen);
  
  // Avoid log(0)
  if (u < 1e-10) {
    u = 1e-10;
  }
  
  return static_cast<uint64_t>(-mean_micros * std::log(u));
}

// ============================================================================
// ConstantRateController
// ============================================================================

ConstantRateController::ConstantRateController(double base_rate_ops_per_sec, 
                                               SystemClock* clock)
    : ArrivalRateController(base_rate_ops_per_sec, clock) {}

uint64_t ConstantRateController::GetNextInterArrivalMicros() {
  return ExponentialInterArrival(base_rate_);
}

// ============================================================================
// SineVarianceController
// ============================================================================

SineVarianceController::SineVarianceController(double base_rate_ops_per_sec, 
                                               SystemClock* clock,
                                               double amplitude, 
                                               double period_sec)
    : ArrivalRateController(base_rate_ops_per_sec, clock),
      amplitude_(amplitude),
      period_micros_(static_cast<uint64_t>(period_sec * 1000000)) {
  if (amplitude_ < 0) amplitude_ = 0;
  if (amplitude_ > 1) amplitude_ = 1;
}

uint64_t SineVarianceController::GetNextInterArrivalMicros() {
  double rate = GetCurrentRateOpsPerSec();
  return ExponentialInterArrival(rate);
}

double SineVarianceController::GetCurrentRateOpsPerSec() {
  uint64_t elapsed = clock_->NowMicros() - start_time_;
  double t = static_cast<double>(elapsed) / static_cast<double>(period_micros_);
  double multiplier = 1.0 + amplitude_ * std::sin(2.0 * M_PI * t);
  return base_rate_ * multiplier;
}

// ============================================================================
// RandomSpikeController
// ============================================================================

RandomSpikeController::RandomSpikeController(double base_rate_ops_per_sec, 
                                             SystemClock* clock,
                                             double spike_multiplier, 
                                             double spike_probability,
                                             double spike_duration_ms)
    : ArrivalRateController(base_rate_ops_per_sec, clock),
      spike_multiplier_(spike_multiplier),
      spike_probability_(spike_probability),
      spike_duration_micros_(static_cast<uint64_t>(spike_duration_ms * 1000)),
      in_spike_(false),
      spike_end_time_(0),
      last_check_time_(clock->NowMicros()),
      gen_(std::random_device{}()) {}

uint64_t RandomSpikeController::GetNextInterArrivalMicros() {
  UpdateSpikeState();
  double rate = GetCurrentRateOpsPerSec();
  return ExponentialInterArrival(rate);
}

double RandomSpikeController::GetCurrentRateOpsPerSec() {
  return in_spike_ ? (base_rate_ * spike_multiplier_) : base_rate_;
}

void RandomSpikeController::UpdateSpikeState() {
  uint64_t now = clock_->NowMicros();
  
  // Check if current spike has ended
  if (in_spike_ && now >= spike_end_time_) {
    in_spike_ = false;
  }
  
  // Check for new spike every 100ms
  if (!in_spike_ && (now - last_check_time_) >= 100000) {
    last_check_time_ = now;
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    if (uniform(gen_) < spike_probability_) {
      in_spike_ = true;
      spike_end_time_ = now + spike_duration_micros_;
    }
  }
}

// ============================================================================
// StepFunctionController
// ============================================================================

StepFunctionController::StepFunctionController(double base_rate_ops_per_sec, 
                                               SystemClock* clock,
                                               double high_multiplier, 
                                               double low_multiplier,
                                               double period_sec)
    : ArrivalRateController(base_rate_ops_per_sec, clock),
      high_multiplier_(high_multiplier),
      low_multiplier_(low_multiplier),
      period_micros_(static_cast<uint64_t>(period_sec * 1000000)) {}

uint64_t StepFunctionController::GetNextInterArrivalMicros() {
  double rate = GetCurrentRateOpsPerSec();
  return ExponentialInterArrival(rate);
}

double StepFunctionController::GetCurrentRateOpsPerSec() {
  uint64_t elapsed = clock_->NowMicros() - start_time_;
  uint64_t phase = elapsed / period_micros_;
  bool is_high = (phase % 2 == 0);
  double multiplier = is_high ? high_multiplier_ : low_multiplier_;
  return base_rate_ * multiplier;
}

// ============================================================================
// PoissonVarianceController
// ============================================================================

PoissonVarianceController::PoissonVarianceController(double base_rate_ops_per_sec, 
                                                     SystemClock* clock,
                                                     double cv)
    : ArrivalRateController(base_rate_ops_per_sec, clock),
      cv_(cv),
      gen_(std::random_device{}()),
      rate_update_interval_micros_(1000000),  // Update rate every second
      last_rate_update_(clock->NowMicros()),
      current_rate_(base_rate_ops_per_sec) {
  UpdateRate();
}

uint64_t PoissonVarianceController::GetNextInterArrivalMicros() {
  uint64_t now = clock_->NowMicros();
  if (now - last_rate_update_ >= rate_update_interval_micros_) {
    UpdateRate();
    last_rate_update_ = now;
  }
  return ExponentialInterArrival(current_rate_);
}

double PoissonVarianceController::GetCurrentRateOpsPerSec() {
  return current_rate_;
}

void PoissonVarianceController::UpdateRate() {
  // Generate rate from normal distribution with specified CV
  double stddev = base_rate_ * cv_;
  std::normal_distribution<double> normal(base_rate_, stddev);
  current_rate_ = normal(gen_);
  
  // Ensure positive rate
  if (current_rate_ < base_rate_ * 0.1) {
    current_rate_ = base_rate_ * 0.1;
  }
}

// ============================================================================
// OperationQueue
// ============================================================================

OperationQueue::OperationQueue(int capacity)
    : capacity_(capacity),
      cv_(&mutex_),
      stopped_(false),
      enqueue_count_(0),
      dequeue_count_(0),
      drop_count_(0) {}

bool OperationQueue::Enqueue(const Operation& op) {
  MutexLock lock(&mutex_);
  if (stopped_) {
    return false;
  }
  
  if (static_cast<int>(queue_.size()) >= capacity_) {
    // Drop the oldest operation
    queue_.pop();
    drop_count_++;
  }
  
  queue_.push(op);
  enqueue_count_++;
  cv_.Signal();
  return true;
}

bool OperationQueue::Dequeue(Operation* op) {
  MutexLock lock(&mutex_);
  while (queue_.empty() && !stopped_) {
    cv_.Wait();
  }
  
  if (queue_.empty()) {
    return false;
  }
  
  *op = queue_.front();
  queue_.pop();
  dequeue_count_++;
  return true;
}

void OperationQueue::Stop() {
  MutexLock lock(&mutex_);
  stopped_ = true;
  cv_.SignalAll();
}

int OperationQueue::GetDepth() {
  MutexLock lock(&mutex_);
  return static_cast<int>(queue_.size());
}

uint64_t OperationQueue::GetEnqueueCount() const {
  return enqueue_count_.load();
}

uint64_t OperationQueue::GetDequeueCount() const {
  return dequeue_count_.load();
}

uint64_t OperationQueue::GetDropCount() const {
  return drop_count_.load();
}

}  // namespace ROCKSDB_NAMESPACE

