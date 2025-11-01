# Open-Loop Workload Design for db_bench

## Overview

Implement open-loop workload support in db_bench to control operation arrival rates independently for read and write threads, supporting multiple variance patterns to test RocksDB under realistic and spike traffic conditions.

## Key Design Principles

1. **Open-loop vs Closed-loop**: Currently db_bench uses closed-loop (fixed threads issuing operations as fast as possible). Open-loop controls the *rate* at which operations arrive, avoiding coordinated omission.
2. **Poisson Process**: Use exponential inter-arrival times for realistic request arrival simulation.
3. **Independent Rate Control**: Read and write operations have separate arrival rate controls.
4. **Multiple Variance Patterns**: Support sine wave, random spikes, step function, and Poisson variance.
5. **Extended Metrics**: Add tracking for open-loop specific metrics:   
   - Actual achieved rate vs target rate
   - Drop rate statistics
6. **Backward compatibility**: The implementation should be backward compatible with commands and options of the existing db_bench. This means that given a benchmark command that works before, the new open-loop related command-line flags belowed be able to be applied to it.
---


## New Command-Line Flags

// Open-loop workload flags
DEFINE_double(arrival_rate, 0.0,
             "Target arrival rate in operations per second for open-loop workload. "
             "When set to 0 (default), uses closed-loop mode. "
             "When > 0, enables open-loop mode with Poisson arrival process.");

DEFINE_string(variance_pattern, "none",
             "Variance pattern for arrival rate: "
             "none, sine, random_spikes, step, poisson");

// Variance pattern specific parameters
DEFINE_double(variance_sine_amplitude, 0.5,
             "Amplitude for sine wave variance as fraction of base rate (0.0-1.0). "
             "Rate varies as: base_rate * (1 + amplitude * sin(2*pi*t/period))");

DEFINE_double(variance_sine_period_sec, 60.0,
             "Period of sine wave oscillation in seconds");

DEFINE_double(variance_spike_multiplier, 3.0,
             "Peak rate multiplier for random spike pattern");

DEFINE_double(variance_spike_probability, 0.05,
             "Probability of spike occurring (checked every 100ms)");

DEFINE_double(variance_spike_duration_ms, 100.0,
             "Duration of each spike in milliseconds");

DEFINE_double(variance_step_high_multiplier, 2.0,
             "High rate multiplier for step function pattern");

DEFINE_double(variance_step_low_multiplier, 0.5,
             "Low rate multiplier for step function pattern");

DEFINE_double(variance_step_period_sec, 30.0,
             "Period for step function in seconds (high/low switch time)");

DEFINE_double(variance_poisson_cv, 0.3,
             "Coefficient of variation for Poisson variance (stdev/mean)");

## Example Usage

```bash
# Constant rate: 10K ops/sec
./db_bench --benchmarks=readrandom --arrival_rate=10000 --variance_pattern=none

# Sine wave: 10K ± 50% with 60 second period
./db_bench --benchmarks=readrandom --arrival_rate=10000 \
  --variance_pattern=sine --variance_sine_amplitude=0.5 \
  --variance_sine_period_sec=60

# Random spikes: base 10K with 3x spikes
./db_bench --benchmarks=readrandom --arrival_rate=10000 \
  --variance_pattern=random_spikes --variance_spike_multiplier=3.0 \
  --variance_spike_probability=0.05 --variance_spike_duration_ms=100

# Step function: alternate 20K and 5K every 30 seconds
./db_bench --benchmarks=readrandom --arrival_rate=10000 \
  --variance_pattern=step --variance_step_high_multiplier=2.0 \
  --variance_step_low_multiplier=0.5 --variance_step_period_sec=30

# Poisson variance: random variation around 10K
./db_bench --benchmarks=readrandom --arrival_rate=10000 \
  --variance_pattern=poisson --variance_poisson_cv=0.3
```

---

## Testing Strategy

1. **Unit Tests**: Test ArrivalRateController in isolation

   - Verify exponential distribution of inter-arrival times
   - Test each variance pattern produces expected rate changes
   - Verify thread-safety

2. **Integration Tests**: Test with db_bench

   - Compare closed-loop vs open-loop latency distributions
   - Verify open-loop avoids coordinated omission
   - Test all variance patterns with real workloads

3. **Validation Metrics**:

   - Actual achieved rate vs target rate
   - Queuing delay statistics

---

## Key Implementation Notes

- There should be 2 rate limiters only, one for reads and one for writes.
- Use a producer-consumer pattern to generate and issue operations.
- There should be 2 producers, one for generating operations for reads and one for generating operations for writes. 
- There should be 2 queues, one for reads and one for writes.
- A producer should be a single thread that generates operations and puts them in the queues.
- A consumer thread should be a thread that is doing the reads and writes. There can be multiple consumer threads.
- Existing codes in tools/db_bench_tool.cc can be reused for generating and issuing operations. 
- Use a lock-free queue to avoid contention.
- The producer should use rate limiters to control the rate at which operations are generated.
- The producer should tag a timestamp to an operation when it submits, let's call it t_schedule, the distribution of t_schedule should meet the target rate and the variance pattern set by the rate limiter.
- The queue shoud be bounded, if overflow, it means system can't keep up with rate,drop the oldest operation queued
- The finish/drop time of all operations should be tracked respectively by the consumer and the queue, let's call it t_finish
 

Before starting the implementation, please review the following questions:
Is the implementation notes above reasonable and can it achieve the goals?
Should we shard the queue into multiple ones so that we use queue-per-thread manner?

And the most important question is, how this implementation can be integrated with the existing db_bench? will it be too complex to integrate? Make a plan for the integration.
