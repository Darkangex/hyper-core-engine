/*
 * ═══════════════════════════════════════════════════════════════════════
 *   Hyper-Core HFT Matching Engine — Latency Benchmark
 *   Self-contained micro-benchmark (zero dependencies)
 *   Standard: C++20
 * ═══════════════════════════════════════════════════════════════════════
 *
 *   Measures precise latency of critical hot-path operations:
 *     1. ObjectPool acquire/release cycle
 *     2. LockFreeRingBuffer push/pop cycle
 *     3. IntrusiveOrderList push_back (the key optimization)
 *     4. PriceLevel add_order + match cycle
 *     5. Full pipeline: add → match → report (end-to-end)
 *
 *   Reports p50, p99, p99.9, min, max, and mean latencies in nanoseconds.
 *
 *   Build:
 *     g++ -std=c++20 -O2 -Wall -Wextra -pthread
 * benchmarks/benchmark_latency.cpp -o benchmark_latency
 *
 *   Run:
 *     ./benchmark_latency
 */

#define HYPER_CORE_NO_MAIN
#include "../hyper_core_engine.cpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numeric>
#include <vector>

// ═══════════════════════════════════════════════════════════════════════
//  Benchmark Harness
// ═══════════════════════════════════════════════════════════════════════

namespace bench {

/// High-resolution timer using steady_clock (nanosecond precision).
struct Timer {
  using clock = std::chrono::steady_clock;
  clock::time_point start;

  void begin() noexcept { start = clock::now(); }

  [[nodiscard]] uint64_t elapsed_ns() const noexcept {
    auto end = clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
            .count());
  }
};

/// Latency statistics computed from a sorted vector of measurements.
struct LatencyReport {
  uint64_t min_ns;
  uint64_t max_ns;
  uint64_t mean_ns;
  uint64_t median_ns; // p50
  uint64_t p99_ns;
  uint64_t p999_ns; // p99.9
  std::size_t sample_count;
};

/// Compute percentile statistics from raw latency samples.
[[nodiscard]] LatencyReport compute_stats(std::vector<uint64_t> &samples) {
  std::sort(samples.begin(), samples.end());

  std::size_t n = samples.size();
  uint64_t sum = 0;
  for (auto s : samples)
    sum += s;

  return LatencyReport{
      .min_ns = samples.front(),
      .max_ns = samples.back(),
      .mean_ns = sum / n,
      .median_ns = samples[n / 2],
      .p99_ns = samples[static_cast<std::size_t>(n * 0.99)],
      .p999_ns = samples[static_cast<std::size_t>(n * 0.999)],
      .sample_count = n,
  };
}

/// Pretty-print a latency report.
void print_report(const char *name, const LatencyReport &r) {
  char line[128];
  std::cout << "\n  ┌─ " << name << " (" << r.sample_count << " samples)\n";

  std::snprintf(line, sizeof(line), "  │  Min:       %8lu ns\n",
                static_cast<unsigned long>(r.min_ns));
  std::cout << line;

  std::snprintf(line, sizeof(line), "  │  p50:       %8lu ns\n",
                static_cast<unsigned long>(r.median_ns));
  std::cout << line;

  std::snprintf(line, sizeof(line), "  │  p99:       %8lu ns\n",
                static_cast<unsigned long>(r.p99_ns));
  std::cout << line;

  std::snprintf(line, sizeof(line), "  │  p99.9:     %8lu ns\n",
                static_cast<unsigned long>(r.p999_ns));
  std::cout << line;

  std::snprintf(line, sizeof(line), "  │  Max:       %8lu ns\n",
                static_cast<unsigned long>(r.max_ns));
  std::cout << line;

  std::snprintf(line, sizeof(line), "  │  Mean:      %8lu ns\n",
                static_cast<unsigned long>(r.mean_ns));
  std::cout << line;

  std::cout << "  └──────────────────────────────\n";
}

} // namespace bench

// ═══════════════════════════════════════════════════════════════════════
//  Benchmark 1: ObjectPool acquire/release
// ═══════════════════════════════════════════════════════════════════════

void bench_object_pool(MemoryArena &arena) {
  constexpr std::size_t N = 100'000;
  ObjectPool<Order> pool(arena, N + 1000);

  std::vector<uint64_t> samples(N);
  bench::Timer timer;

  for (std::size_t i = 0; i < N; ++i) {
    timer.begin();
    Order *o = pool.acquire();
    pool.release(o);
    samples[i] = timer.elapsed_ns();
  }

  auto report = bench::compute_stats(samples);
  bench::print_report("ObjectPool acquire + release", report);
}

// ═══════════════════════════════════════════════════════════════════════
//  Benchmark 2: LockFreeRingBuffer push/pop
// ═══════════════════════════════════════════════════════════════════════

void bench_ring_buffer(MemoryArena &arena) {
  constexpr std::size_t N = 100'000;
  LockFreeRingBuffer<OrderMessage> rb(arena);

  std::vector<uint64_t> samples(N);
  bench::Timer timer;
  OrderMessage msg{};
  OrderMessage out{};

  for (std::size_t i = 0; i < N; ++i) {
    timer.begin();
    rb.push(msg);
    rb.pop(out);
    samples[i] = timer.elapsed_ns();
  }

  auto report = bench::compute_stats(samples);
  bench::print_report("RingBuffer push + pop", report);
}

// ═══════════════════════════════════════════════════════════════════════
//  Benchmark 3: IntrusiveOrderList push_back (THE key operation)
// ═══════════════════════════════════════════════════════════════════════

void bench_intrusive_list(MemoryArena &arena) {
  constexpr std::size_t N = 100'000;
  ObjectPool<Order> pool(arena, N + 1000);

  IntrusiveOrderList list;
  std::vector<uint64_t> samples(N);
  bench::Timer timer;

  for (std::size_t i = 0; i < N; ++i) {
    Order *o = pool.acquire();
    o->remaining_qty = 100;
    o->active = 1;

    timer.begin();
    list.push_back(o); // THIS must be constant-time regardless of list size
    samples[i] = timer.elapsed_ns();
  }

  auto report = bench::compute_stats(samples);
  bench::print_report("IntrusiveOrderList push_back (100K orders)", report);

  // Verify: push_back at order 1 vs order 100,000 should have similar latency
  auto first_1000 =
      std::vector<uint64_t>(samples.begin(), samples.begin() + 1000);
  auto last_1000 = std::vector<uint64_t>(samples.end() - 1000, samples.end());
  auto report_first = bench::compute_stats(first_1000);
  auto report_last = bench::compute_stats(last_1000);

  std::cout << "  ┌─ Consistency check (first 1K vs last 1K)\n";
  std::cout << "  │  First 1K mean: " << report_first.mean_ns << " ns\n";
  std::cout << "  │  Last 1K mean:  " << report_last.mean_ns << " ns\n";

  double ratio = static_cast<double>(report_last.mean_ns) /
                 static_cast<double>(
                     std::max(report_first.mean_ns, static_cast<uint64_t>(1)));
  std::cout << "  │  Ratio:         " << std::fixed << std::setprecision(2)
            << ratio << "x\n";

  const char *verdict = (ratio < 3.0) ? "✓ CONSTANT TIME" : "✗ DEGRADED";
  std::cout << "  │  Verdict:       " << verdict << "\n";
  std::cout << "  └──────────────────────────────\n";
}

// ═══════════════════════════════════════════════════════════════════════
//  Benchmark 4: PriceLevel add_order + match cycle
// ═══════════════════════════════════════════════════════════════════════

void bench_price_level(MemoryArena &arena) {
  constexpr std::size_t N = 50'000;
  ObjectPool<Order> pool(arena, N + 1000);

  std::vector<uint64_t> add_samples(N);
  std::vector<uint64_t> match_samples(N);
  bench::Timer timer;

  PriceLevel level(1'000'000);

  // Benchmark add_order
  for (std::size_t i = 0; i < N; ++i) {
    Order *o = pool.acquire();
    o->remaining_qty = 10;
    o->active = 1;

    timer.begin();
    level.add_order(o);
    add_samples[i] = timer.elapsed_ns();
  }

  auto add_report = bench::compute_stats(add_samples);
  bench::print_report("PriceLevel add_order", add_report);

  // Benchmark match (partial fills)
  for (std::size_t i = 0; i < N; ++i) {
    timer.begin();
    level.match(1); // Fill 1 unit at a time
    match_samples[i] = timer.elapsed_ns();
  }

  auto match_report = bench::compute_stats(match_samples);
  bench::print_report("PriceLevel match (1 unit)", match_report);
}

// ═══════════════════════════════════════════════════════════════════════
//  Benchmark 5: Full pipeline (end-to-end)
// ═══════════════════════════════════════════════════════════════════════

void bench_full_pipeline(MemoryArena &arena) {
  constexpr std::size_t N = 50'000;
  ObjectPool<Order> pool(arena, N * 2 + 1000);

  OrderBook book;
  std::vector<uint64_t> samples(N);
  bench::Timer timer;

  uint64_t next_id = 1;

  for (std::size_t i = 0; i < N; ++i) {
    // Create a bid and an ask at the same price to force matching
    Order *bid = pool.acquire();
    bid->id = next_id++;
    bid->price = 1'000'000;
    bid->remaining_qty = 10;
    bid->side = Side::BID;
    bid->type = OrderType::LIMIT;
    bid->active = 1;

    Order *ask = pool.acquire();
    ask->id = next_id++;
    ask->price = 1'000'000;
    ask->remaining_qty = 10;
    ask->side = Side::ASK;
    ask->type = OrderType::LIMIT;
    ask->active = 1;

    timer.begin();
    book.add_order(bid);
    book.add_order(ask);
    book.match();
    samples[i] = timer.elapsed_ns();
  }

  auto report = bench::compute_stats(samples);
  bench::print_report("Full pipeline: add(bid) + add(ask) + match", report);
}

// ═══════════════════════════════════════════════════════════════════════
//  Main
// ═══════════════════════════════════════════════════════════════════════

int main() {
  std::cout << "\n"
            << "══════════════════════════════════════════════════\n"
            << "  Hyper-Core HFT Engine — Latency Benchmark\n"
            << "══════════════════════════════════════════════════\n"
            << "  All times in nanoseconds (ns)\n"
            << "  Lower is better\n";

  // Each benchmark gets its own arena to avoid interference
  {
    MemoryArena arena(64 * 1024 * 1024);
    bench_object_pool(arena);
  }
  {
    MemoryArena arena(64 * 1024 * 1024);
    bench_ring_buffer(arena);
  }
  {
    MemoryArena arena(64 * 1024 * 1024);
    bench_intrusive_list(arena);
  }
  {
    MemoryArena arena(64 * 1024 * 1024);
    bench_price_level(arena);
  }
  {
    MemoryArena arena(128 * 1024 * 1024);
    bench_full_pipeline(arena);
  }

  std::cout << "\n══════════════════════════════════════════════════\n"
            << "  Benchmark complete.\n"
            << "══════════════════════════════════════════════════\n\n";

  return 0;
}
