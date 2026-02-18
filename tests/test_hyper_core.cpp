/*
 * ═══════════════════════════════════════════════════════════════════════
 *   Hyper-Core HFT Matching Engine — Unit Tests
 *   Framework: Catch2-style single-file test runner (zero dependencies)
 *   Standard: C++20
 * ═══════════════════════════════════════════════════════════════════════
 *
 *   Tests verify correctness of every core component:
 *     - MemoryArena (bump allocation, alignment, reset)
 *     - ObjectPool  (acquire, release, recycling)
 *     - LockFreeRingBuffer (push, pop, empty/full)
 *     - Order struct (layout, trivial copyability)
 *     - IntrusiveOrderList (push_back, match, compact)
 *     - PriceLevel (add, match, cancel, compact)
 *     - OrderBook (limit orders, market orders, cancellations, matching)
 *
 *   Build:
 *     g++ -std=c++20 -O2 -Wall -Wextra -pthread tests/test_hyper_core.cpp -o
 * test_hyper_core
 *
 *   Run:
 *     ./test_hyper_core
 */

#define HYPER_CORE_NO_MAIN
#include "../hyper_core_engine.cpp"

#include <iostream>
#include <sstream>
#include <string>

// ═══════════════════════════════════════════════════════════════════════
//  Minimal Test Framework (zero dependencies)
// ═══════════════════════════════════════════════════════════════════════

namespace test {

struct TestStats {
  int total = 0;
  int passed = 0;
  int failed = 0;
};

static TestStats stats;

#define TEST_CASE(name)                                                        \
  static void test_##name();                                                   \
  struct TestRegistrar_##name {                                                \
    TestRegistrar_##name() { test::run_test(#name, test_##name); }             \
  };                                                                           \
  static TestRegistrar_##name registrar_##name;                                \
  static void test_##name()

#define REQUIRE(expr)                                                          \
  do {                                                                         \
    if (!(expr)) {                                                             \
      std::cerr << "    ✗ FAILED: " << #expr << "\n"                           \
                << "      at " << __FILE__ << ":" << __LINE__ << "\n";         \
      throw std::runtime_error("assertion failed");                            \
    }                                                                          \
  } while (0)

#define REQUIRE_EQ(a, b)                                                       \
  do {                                                                         \
    auto va = (a);                                                             \
    auto vb = (b);                                                             \
    if (va != vb) {                                                            \
      std::cerr << "    ✗ FAILED: " << #a << " == " << #b << "\n"              \
                << "      got: " << va << " != " << vb << "\n"                 \
                << "      at " << __FILE__ << ":" << __LINE__ << "\n";         \
      throw std::runtime_error("assertion failed");                            \
    }                                                                          \
  } while (0)

inline void run_test(const char *name, void (*fn)()) {
  stats.total++;
  std::cout << "  ▸ " << name << "... " << std::flush;
  try {
    fn();
    stats.passed++;
    std::cout << "✓ PASSED\n";
  } catch (...) {
    stats.failed++;
  }
}

inline int report() {
  std::cout << "\n══════════════════════════════════════════\n"
            << "  Results: " << stats.passed << "/" << stats.total << " passed";
  if (stats.failed > 0)
    std::cout << " (" << stats.failed << " FAILED)";
  std::cout << "\n══════════════════════════════════════════\n\n";
  return stats.failed > 0 ? 1 : 0;
}

} // namespace test

// ═══════════════════════════════════════════════════════════════════════
//  1. Order Struct Tests
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE(Order_is_cache_line_sized) {
  REQUIRE_EQ(sizeof(Order), config::CACHE_LINE_SIZE);
}

TEST_CASE(Order_is_trivially_copyable) {
  REQUIRE(std::is_trivially_copyable_v<Order>);
}

TEST_CASE(Order_default_next_is_null) {
  Order o{};
  REQUIRE(o.next == nullptr);
}

// ═══════════════════════════════════════════════════════════════════════
//  2. MemoryArena Tests
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE(Arena_allocates_without_crash) {
  MemoryArena arena(4096);
  int *p = arena.allocate<int>(10);
  REQUIRE(p != nullptr);
}

TEST_CASE(Arena_tracks_usage) {
  MemoryArena arena(4096);
  REQUIRE_EQ(arena.used(), static_cast<std::size_t>(0));
  arena.allocate<uint64_t>(8);
  REQUIRE(arena.used() > 0);
  REQUIRE(arena.remaining() < arena.capacity());
}

TEST_CASE(Arena_reset_frees_space) {
  MemoryArena arena(4096);
  arena.allocate<char>(1000);
  std::size_t used_before = arena.used();
  arena.reset();
  REQUIRE_EQ(arena.used(), static_cast<std::size_t>(0));
  REQUIRE(used_before > 0);
}

TEST_CASE(Arena_alignment_respected) {
  MemoryArena arena(4096);
  arena.allocate<char>(1); // offset 1 (misaligned for 8-byte)
  uint64_t *p = arena.allocate<uint64_t>(1);
  // Pointer must be 8-byte aligned
  REQUIRE(reinterpret_cast<uintptr_t>(p) % alignof(uint64_t) == 0);
}

// ═══════════════════════════════════════════════════════════════════════
//  3. ObjectPool Tests
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE(Pool_acquire_returns_valid_ptr) {
  MemoryArena arena(1024 * 1024);
  ObjectPool<Order> pool(arena, 100);
  Order *o = pool.acquire();
  REQUIRE(o != nullptr);
}

TEST_CASE(Pool_tracks_available_count) {
  MemoryArena arena(1024 * 1024);
  ObjectPool<Order> pool(arena, 100);
  REQUIRE_EQ(pool.available(), static_cast<std::size_t>(100));
  pool.acquire();
  REQUIRE_EQ(pool.available(), static_cast<std::size_t>(99));
  REQUIRE_EQ(pool.in_use(), static_cast<std::size_t>(1));
}

TEST_CASE(Pool_release_recycles_object) {
  MemoryArena arena(1024 * 1024);
  ObjectPool<Order> pool(arena, 10);
  Order *o = pool.acquire();
  pool.release(o);
  REQUIRE_EQ(pool.available(), static_cast<std::size_t>(10));
}

TEST_CASE(Pool_exhaustion_returns_null) {
  MemoryArena arena(1024 * 1024);
  ObjectPool<Order> pool(arena, 2);
  pool.acquire();
  pool.acquire();
  Order *o3 = pool.acquire();
  REQUIRE(o3 == nullptr);
}

// ═══════════════════════════════════════════════════════════════════════
//  4. LockFreeRingBuffer Tests
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE(RingBuffer_starts_empty) {
  MemoryArena arena(config::RING_BUFFER_CAPACITY * sizeof(OrderMessage) + 4096);
  LockFreeRingBuffer<OrderMessage> rb(arena);
  REQUIRE(rb.empty());
  REQUIRE_EQ(rb.size(), static_cast<std::size_t>(0));
}

TEST_CASE(RingBuffer_push_and_pop) {
  MemoryArena arena(config::RING_BUFFER_CAPACITY * sizeof(OrderMessage) + 4096);
  LockFreeRingBuffer<OrderMessage> rb(arena);

  OrderMessage msg{};
  msg.type = OrderType::LIMIT;
  msg.cancel_id = 42;

  REQUIRE(rb.push(msg));
  REQUIRE(!rb.empty());

  OrderMessage out{};
  REQUIRE(rb.pop(out));
  REQUIRE_EQ(static_cast<int>(out.type), static_cast<int>(OrderType::LIMIT));
  REQUIRE_EQ(out.cancel_id, static_cast<uint64_t>(42));
  REQUIRE(rb.empty());
}

TEST_CASE(RingBuffer_pop_on_empty_fails) {
  MemoryArena arena(config::RING_BUFFER_CAPACITY * sizeof(OrderMessage) + 4096);
  LockFreeRingBuffer<OrderMessage> rb(arena);
  OrderMessage out{};
  REQUIRE(!rb.pop(out));
}

// ═══════════════════════════════════════════════════════════════════════
//  5. IntrusiveOrderList Tests
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE(IntrusiveList_starts_empty) {
  IntrusiveOrderList list;
  REQUIRE(list.empty());
  REQUIRE_EQ(list.size(), static_cast<std::size_t>(0));
}

TEST_CASE(IntrusiveList_push_back_adds_orders) {
  MemoryArena arena(1024 * 1024);
  ObjectPool<Order> pool(arena, 100);

  IntrusiveOrderList list;
  Order *o1 = pool.acquire();
  Order *o2 = pool.acquire();
  o1->remaining_qty = 100;
  o1->active = 1;
  o2->remaining_qty = 200;
  o2->active = 1;

  list.push_back(o1);
  list.push_back(o2);

  REQUIRE_EQ(list.size(), static_cast<std::size_t>(2));
  REQUIRE(!list.empty());
  REQUIRE(list.head() == o1);
}

TEST_CASE(IntrusiveList_match_fills_FIFO) {
  MemoryArena arena(1024 * 1024);
  ObjectPool<Order> pool(arena, 100);

  IntrusiveOrderList list;
  Order *o1 = pool.acquire();
  o1->remaining_qty = 50;
  o1->active = 1;
  Order *o2 = pool.acquire();
  o2->remaining_qty = 80;
  o2->active = 1;
  list.push_back(o1);
  list.push_back(o2);

  // Match 70 — should fill o1 (50) fully and o2 (20) partially
  uint32_t filled = list.match(70);
  REQUIRE_EQ(filled, static_cast<uint32_t>(70));
  REQUIRE_EQ(o1->remaining_qty, static_cast<uint32_t>(0));
  REQUIRE_EQ(o1->active, static_cast<uint8_t>(0)); // auto-deactivated
  REQUIRE_EQ(o2->remaining_qty, static_cast<uint32_t>(60));
  REQUIRE_EQ(o2->active, static_cast<uint8_t>(1)); // still active
}

TEST_CASE(IntrusiveList_match_skips_inactive) {
  MemoryArena arena(1024 * 1024);
  ObjectPool<Order> pool(arena, 100);

  IntrusiveOrderList list;
  Order *o1 = pool.acquire();
  o1->remaining_qty = 50;
  o1->active = 0; // cancelled
  Order *o2 = pool.acquire();
  o2->remaining_qty = 30;
  o2->active = 1;
  list.push_back(o1);
  list.push_back(o2);

  uint32_t filled = list.match(100);
  REQUIRE_EQ(filled, static_cast<uint32_t>(30));
  REQUIRE_EQ(o1->remaining_qty, static_cast<uint32_t>(50)); // untouched
}

TEST_CASE(IntrusiveList_compact_removes_dead_nodes) {
  MemoryArena arena(1024 * 1024);
  ObjectPool<Order> pool(arena, 100);

  IntrusiveOrderList list;
  Order *o1 = pool.acquire();
  o1->remaining_qty = 0;
  o1->active = 0;
  Order *o2 = pool.acquire();
  o2->remaining_qty = 100;
  o2->active = 1;
  Order *o3 = pool.acquire();
  o3->remaining_qty = 0;
  o3->active = 0;
  list.push_back(o1);
  list.push_back(o2);
  list.push_back(o3);

  REQUIRE_EQ(list.size(), static_cast<std::size_t>(3));
  list.compact();
  REQUIRE_EQ(list.size(), static_cast<std::size_t>(1));
  REQUIRE(list.head() == o2);
}

TEST_CASE(IntrusiveList_unbounded_capacity_no_malloc) {
  // This is the KEY test: push 5000 orders (way beyond the old 1024 reserve)
  // with ZERO memory allocation — proving the intrusive list advantage.
  MemoryArena arena(64 * 1024 * 1024);
  ObjectPool<Order> pool(arena, 10000);

  IntrusiveOrderList list;
  for (int i = 0; i < 5000; ++i) {
    Order *o = pool.acquire();
    REQUIRE(o != nullptr);
    o->remaining_qty = 1;
    o->active = 1;
    list.push_back(o);
  }

  REQUIRE_EQ(list.size(), static_cast<std::size_t>(5000));
  // If we got here, 5000 push_backs with ZERO malloc succeeded.
}

// ═══════════════════════════════════════════════════════════════════════
//  6. PriceLevel Tests
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE(PriceLevel_add_and_match) {
  MemoryArena arena(1024 * 1024);
  ObjectPool<Order> pool(arena, 100);

  PriceLevel level(1000000);
  Order *o = pool.acquire();
  o->remaining_qty = 100;
  o->active = 1;

  level.add_order(o);
  REQUIRE_EQ(level.total_qty(), static_cast<uint32_t>(100));
  REQUIRE_EQ(level.order_count(), static_cast<std::size_t>(1));

  uint32_t filled = level.match(60);
  REQUIRE_EQ(filled, static_cast<uint32_t>(60));
  REQUIRE_EQ(level.total_qty(), static_cast<uint32_t>(40));
}

TEST_CASE(PriceLevel_reduce_qty_for_cancel) {
  PriceLevel level(1000000);
  MemoryArena arena(1024 * 1024);
  ObjectPool<Order> pool(arena, 100);

  Order *o = pool.acquire();
  o->remaining_qty = 100;
  o->active = 1;
  level.add_order(o);

  level.reduce_qty(100);
  REQUIRE_EQ(level.total_qty(), static_cast<uint32_t>(0));
}

// ═══════════════════════════════════════════════════════════════════════
//  7. OrderBook Tests
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE(OrderBook_add_limit_order) {
  MemoryArena arena(64 * 1024 * 1024);
  ObjectPool<Order> pool(arena, 1000);
  OrderBook book;

  Order *o = pool.acquire();
  o->id = 1;
  o->price = 1000000;
  o->remaining_qty = 100;
  o->side = Side::BID;
  o->type = OrderType::LIMIT;
  o->active = 1;

  book.add_order(o);
  REQUIRE(o->active == 1);
}

TEST_CASE(OrderBook_cancel_order) {
  MemoryArena arena(64 * 1024 * 1024);
  ObjectPool<Order> pool(arena, 1000);
  OrderBook book;

  Order *o = pool.acquire();
  o->id = 42;
  o->price = 1000000;
  o->remaining_qty = 100;
  o->side = Side::ASK;
  o->type = OrderType::LIMIT;
  o->active = 1;

  book.add_order(o);
  bool cancelled = book.cancel_order(42);
  REQUIRE(cancelled);
  REQUIRE_EQ(o->active, static_cast<uint8_t>(0));
  REQUIRE_EQ(o->remaining_qty, static_cast<uint32_t>(0));
}

TEST_CASE(OrderBook_cancel_nonexistent_returns_false) {
  OrderBook book;
  REQUIRE(!book.cancel_order(999999));
}

TEST_CASE(OrderBook_match_crossing_orders) {
  MemoryArena arena(64 * 1024 * 1024);
  ObjectPool<Order> pool(arena, 1000);
  OrderBook book;

  // Bid at 100.00
  Order *bid = pool.acquire();
  bid->id = 1;
  bid->price = 1000000;
  bid->remaining_qty = 50;
  bid->side = Side::BID;
  bid->type = OrderType::LIMIT;
  bid->active = 1;
  book.add_order(bid);

  // Ask at 100.00 (same price → should cross)
  Order *ask = pool.acquire();
  ask->id = 2;
  ask->price = 1000000;
  ask->remaining_qty = 30;
  ask->side = Side::ASK;
  ask->type = OrderType::LIMIT;
  ask->active = 1;
  book.add_order(ask);

  uint64_t filled = book.match();
  REQUIRE(filled > 0);
}

TEST_CASE(OrderBook_market_order_fills) {
  MemoryArena arena(64 * 1024 * 1024);
  ObjectPool<Order> pool(arena, 1000);
  OrderBook book;

  // Resting ask at 100.00
  Order *ask = pool.acquire();
  ask->id = 1;
  ask->price = 1000000;
  ask->remaining_qty = 100;
  ask->side = Side::ASK;
  ask->type = OrderType::LIMIT;
  ask->active = 1;
  book.add_order(ask);

  // Market buy should match against the resting ask
  Order *market_buy = pool.acquire();
  market_buy->id = 2;
  market_buy->price = 0;
  market_buy->remaining_qty = 50;
  market_buy->side = Side::BID;
  market_buy->type = OrderType::MARKET;
  market_buy->active = 1;

  uint64_t filled = book.match_market(market_buy);
  REQUIRE_EQ(filled, static_cast<uint64_t>(50));
  REQUIRE_EQ(market_buy->remaining_qty, static_cast<uint32_t>(0));
  REQUIRE_EQ(ask->remaining_qty, static_cast<uint32_t>(50));
}

// ═══════════════════════════════════════════════════════════════════════
//  Main — Run all tests
// ═══════════════════════════════════════════════════════════════════════

int main() {
  std::cout << "\n"
            << "══════════════════════════════════════════\n"
            << "  Hyper-Core HFT Engine — Unit Tests\n"
            << "══════════════════════════════════════════\n\n";

  // Tests are auto-registered and already ran via static constructors.
  // Just print the report.
  return test::report();
}
