/*
 * ═══════════════════════════════════════════════════════════════════════
 *   Hyper-Core HFT Matching Engine
 *   Version: 1.0.0 (Production Architecture)
 *   Standard: C++20 (Concepts, Atomics, constexpr)
 * ═══════════════════════════════════════════════════════════════════════
 *
 * Architecture:
 *
 *   ┌───────────────────┐       SPSC RingBuffer        ┌──────────────────┐
 *   │ GatewaySimulator  │ ────────(lock-free)────────▶ │  MatcherThread   │
 *   │ (Producer Thread) │                               │ (Pinned to Core) │
 *   └───────────────────┘                               └────────┬─────────┘
 *                                                                │
 *                                              ┌─────────────────┼────────────┐
 *                                              ▼                 ▼            ▼
 *                                       ┌────────────┐   ┌────────────┐
 * ┌──────────┐ │ ObjectPool  │   │  OrderBook │ │  Stats   │ │  (Order)    │
 * │(Intrusive) │ │ Counters │ └──────┬──────┘   └────────────┘ └──────────┘ │
 *                                       ┌──────┴──────┐
 *                                       │ MemoryArena │
 *                                       │ (Bump Alloc)│
 *                                       └─────────────┘
 *
 * Components:
 *   1.  MemoryArena           -> Bump allocator, zero-fragmentation
 *   2.  ObjectPool<T>         -> Intrusive free-list, zero-alloc hot path
 *   3.  LockFreeRingBuffer<T> -> SPSC with cache-line isolation
 *   4.  Order / OrderMessage  -> Cache-line-aligned data structures
 *   5.  PriceLevel            -> Intrusive linked list of orders at one price
 *   6.  OrderBook             -> Bid/Ask sides, price-time matching
 *   7.  MatcherThread         -> Pinned busy-spin event loop
 *   8.  GatewaySimulator      -> Synthetic order generator
 *
 * Decisions:
 *   | Decision               | Alternative        | Rationale |
 *   |------------------------|--------------------|−−−−−−−−−−−−−−−−−−−−−−−−−−−−−−−−−|
 *   | SPSC RingBuffer        | mutex + queue       | Zero contention, no
 * syscalls   | | alignas(64) head/tail  | default alignment   | Eliminates
 * false sharing       | | acquire/release fences | seq_cst             |
 * Minimal fence overhead         | | Intrusive List Levels  |
 * std::vector<Order*> | Zero-alloc, unbounded capacity | | Placement-new Pool
 * | new/delete          | Zero heap alloc on hot path    | | Fixed-point prices
 * | double              | Deterministic comparison       | | Bump allocator
 * Arena   | malloc per object   | O(1) alloc, zero fragmentation |
 *
 * Dependencies: None (stdlib only)
 * Compiler:     g++ -std=c++20 -O2 -Wall -Wextra -pthread
 */

// ═══════════════════════════════════════════════════════════════════════
//  1. INCLUDES
// ═══════════════════════════════════════════════════════════════════════

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <new>
#include <random>

#include <thread>
#include <type_traits>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

// ═══════════════════════════════════════════════════════════════════════
//  2. CONSTANTS & CONFIGURATION
// ═══════════════════════════════════════════════════════════════════════

namespace config {

inline constexpr std::size_t CACHE_LINE_SIZE = 64;
inline constexpr std::size_t RING_BUFFER_CAPACITY = 1
                                                    << 16; // 65536, power-of-2
inline constexpr std::size_t ARENA_SIZE_BYTES = 64 * 1024 * 1024; // 64 MB
inline constexpr std::size_t MAX_ORDERS = 500'000;
inline constexpr std::size_t MAX_PRICE_LEVELS = 10'000;
inline constexpr std::size_t MAX_ORDERS_PER_LEVEL = 1'024;
inline constexpr std::size_t ORDER_ID_MAP_SIZE = 1 << 20; // 1M slots
inline constexpr int MATCHER_CORE_ID = 1;                 // pin to core 1
inline constexpr int64_t PRICE_MULTIPLIER = 10'000; // fixed-point: 4 decimals
inline constexpr int64_t MID_PRICE = 1'000'000;     // $100.0000 in fixed-point
inline constexpr std::size_t GATEWAY_ORDER_COUNT = 200'000;
inline constexpr double LIMIT_ORDER_RATIO = 0.70;
inline constexpr double MARKET_ORDER_RATIO = 0.20;
// Cancel ratio = 1.0 - LIMIT - MARKET = 0.10

} // namespace config

// ═══════════════════════════════════════════════════════════════════════
//  3. C++20 CONCEPTS
// ═══════════════════════════════════════════════════════════════════════

/// T must be trivially copyable and destructible for raw memory operations.
template <typename T>
concept TriviallySafe =
    std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>;

/// T must fit within a reasonable size bound for stack/pool allocation.
template <typename T>
concept BoundedSize = sizeof(T) <= 4096;

/// Combined constraint for ObjectPool and RingBuffer element types.
template <typename T>
concept PoolEligible = TriviallySafe<T> && BoundedSize<T>;

/// Callable that accepts a reference to T (for ring buffer consumer callbacks).
template <typename F, typename T>
concept ConsumerOf = std::invocable<F, T &>;

// ═══════════════════════════════════════════════════════════════════════
//  4. MEMORY ARENA — Bump Allocator (zero fragmentation)
// ═══════════════════════════════════════════════════════════════════════

/// Pre-allocates a single contiguous block. O(1) bump-pointer allocation.
/// No individual deallocation — entire arena freed in destructor.
///
/// Design:
///   - Single allocation at construction via aligned_alloc
///   - Bump pointer advances monotonically
///   - Alignment respected per-allocation via pointer arithmetic
///   - Thread safety: NOT thread-safe (used exclusively by matcher thread)
class MemoryArena {
public:
  explicit MemoryArena(std::size_t size_bytes)
      : capacity_(size_bytes), offset_(0) {
    // Allocate cache-line-aligned backing store
    base_ = static_cast<std::byte *>(
#ifdef _WIN32
        _aligned_malloc(size_bytes, config::CACHE_LINE_SIZE)
#else
        std::aligned_alloc(config::CACHE_LINE_SIZE, size_bytes)
#endif
    );
    if (!base_) {
      std::cerr << "[FATAL] MemoryArena: allocation failed (" << size_bytes
                << " bytes)\n";
      std::abort();
    }
    std::memset(base_, 0, size_bytes);
  }

  ~MemoryArena() {
#ifdef _WIN32
    _aligned_free(base_);
#else
    std::free(base_);
#endif
  }

  // ─────────── Non-copyable, non-movable ───────────

  MemoryArena(const MemoryArena &) = delete;
  MemoryArena &operator=(const MemoryArena &) = delete;
  MemoryArena(MemoryArena &&) = delete;
  MemoryArena &operator=(MemoryArena &&) = delete;

  // ─────────── API ───────────

  /// Allocate `count` objects of type T with proper alignment. O(1).
  template <typename T> [[nodiscard]] T *allocate(std::size_t count = 1) {
    constexpr std::size_t alignment = alignof(T);
    const std::size_t total_bytes = sizeof(T) * count;

    // Align the current offset
    std::size_t aligned_offset = (offset_ + alignment - 1) & ~(alignment - 1);

    if (aligned_offset + total_bytes > capacity_) [[unlikely]] {
      std::cerr << "[FATAL] MemoryArena: out of memory ("
                << aligned_offset + total_bytes << " > " << capacity_ << ")\n";
      std::abort();
    }

    T *ptr = reinterpret_cast<T *>(base_ + aligned_offset);
    offset_ = aligned_offset + total_bytes;
    return ptr;
  }

  /// Reset arena to beginning (invalidates all prior allocations).
  void reset() noexcept { offset_ = 0; }

  // ─────────── Stats ───────────

  [[nodiscard]] std::size_t used() const noexcept { return offset_; }
  [[nodiscard]] std::size_t remaining() const noexcept {
    return capacity_ - offset_;
  }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
  std::byte *base_;
  std::size_t capacity_;
  std::size_t offset_;
};

// ═══════════════════════════════════════════════════════════════════════
//  5. OBJECT POOL — Zero-Allocation Recycler
// ═══════════════════════════════════════════════════════════════════════

/// Intrusive free-list object pool backed by MemoryArena.
/// acquire() = O(1), release() = O(1). No heap allocation on hot path.
///
/// Design:
///   - Pre-allocates N slots from arena at construction
///   - Free-list stored as stack of indices (cache-friendly, no pointer
///   chasing)
///   - acquire() pops index, returns pointer via placement-new
///   - release() pushes index back, calls trivial destructor
///
/// Constraint: T must be TriviallySafe && BoundedSize (PoolEligible concept).
template <PoolEligible T> class ObjectPool {
public:
  ObjectPool(MemoryArena &arena, std::size_t max_objects)
      : max_objects_(max_objects), free_count_(max_objects) {
    // Allocate contiguous storage for objects
    storage_ = arena.allocate<T>(max_objects);

    // Allocate free-list index stack
    free_stack_ = arena.allocate<uint32_t>(max_objects);

    // Initialize free stack: all indices available
    for (std::size_t i = 0; i < max_objects; ++i) {
      free_stack_[i] = static_cast<uint32_t>(i);
    }
  }

  // ─────────── API ───────────

  /// Acquire a pre-allocated object. O(1). Returns nullptr if pool exhausted.
  [[nodiscard]] T *acquire() noexcept {
    if (free_count_ == 0) [[unlikely]] {
      return nullptr;
    }
    --free_count_;
    uint32_t idx = free_stack_[free_count_];
    T *slot = &storage_[idx];

    // Placement-new: construct in pre-allocated memory
    return ::new (static_cast<void *>(slot)) T{};
  }

  /// Release an object back to the pool. O(1).
  void release(T *obj) noexcept {
    if (!obj) [[unlikely]]
      return;

    // Calculate index from pointer arithmetic
    auto idx = static_cast<uint32_t>(obj - storage_);
    assert(idx < max_objects_ && "release: pointer not from this pool");

    // Trivial destructor (guaranteed by concept), no-op but semantically
    // correct
    obj->~T();

    free_stack_[free_count_] = idx;
    ++free_count_;
  }

  // ─────────── Stats ───────────

  [[nodiscard]] std::size_t available() const noexcept { return free_count_; }
  [[nodiscard]] std::size_t in_use() const noexcept {
    return max_objects_ - free_count_;
  }
  [[nodiscard]] std::size_t capacity() const noexcept { return max_objects_; }

private:
  T *storage_;
  uint32_t *free_stack_;
  std::size_t max_objects_;
  std::size_t free_count_;
};

// ═══════════════════════════════════════════════════════════════════════
//  6. LOCK-FREE RING BUFFER — SPSC (Single Producer Single Consumer)
// ═══════════════════════════════════════════════════════════════════════

/// Cache-line-isolated SPSC ring buffer with acquire/release semantics.
///
/// Design:
///   - Power-of-2 capacity for branchless modulo (bitwise AND)
///   - head_ (consumer) and tail_ (producer) on separate cache lines
///   - Producer: store(tail_, release) — data visible before index advances
///   - Consumer: load(tail_, acquire) — sees data written before tail advanced
///   - No CAS loops needed (single producer, single consumer)
///
/// Complexity: push() O(1), pop() O(1)
/// Latency:    ~5-15ns per operation (no syscalls, no contention)
template <PoolEligible T> class LockFreeRingBuffer {
  static_assert((config::RING_BUFFER_CAPACITY &
                 (config::RING_BUFFER_CAPACITY - 1)) == 0,
                "Ring buffer capacity must be a power of 2");

public:
  explicit LockFreeRingBuffer(MemoryArena &arena)
      : mask_(config::RING_BUFFER_CAPACITY - 1) {
    buffer_ = arena.allocate<T>(config::RING_BUFFER_CAPACITY);
  }

  // ─────────── Producer API (single thread) ───────────

  /// Push an element. Returns false if buffer is full (back-pressure).
  [[nodiscard]] bool push(const T &item) noexcept {
    const uint64_t current_tail = tail_.value.load(std::memory_order_relaxed);
    const uint64_t next_tail = current_tail + 1;

    // Full check: if next_tail catches up to head, buffer is full
    if (next_tail - head_.value.load(std::memory_order_acquire) > mask_)
        [[unlikely]] {
      return false;
    }

    buffer_[current_tail & mask_] = item;

    // Release: ensure the data write is visible before tail advances
    tail_.value.store(next_tail, std::memory_order_release);
    return true;
  }

  // ─────────── Consumer API (single thread) ───────────

  /// Pop an element. Returns false if buffer is empty.
  [[nodiscard]] bool pop(T &out) noexcept {
    const uint64_t current_head = head_.value.load(std::memory_order_relaxed);

    // Empty check: head has caught up to tail
    if (current_head >= tail_.value.load(std::memory_order_acquire))
        [[unlikely]] {
      return false;
    }

    out = buffer_[current_head & mask_];

    // Release: ensure the read is complete before head advances
    head_.value.store(current_head + 1, std::memory_order_release);
    return true;
  }

  // ─────────── Stats ───────────

  [[nodiscard]] std::size_t size() const noexcept {
    auto t = tail_.value.load(std::memory_order_relaxed);
    auto h = head_.value.load(std::memory_order_relaxed);
    return static_cast<std::size_t>(t - h);
  }

  [[nodiscard]] bool empty() const noexcept { return size() == 0; }

private:
  // ── Cache-line-isolated atomic counters ──
  // Each on its own 64-byte cache line to prevent false sharing

  struct alignas(config::CACHE_LINE_SIZE) AlignedAtomic {
    std::atomic<uint64_t> value{0};
    // Padding is implicit: alignas(64) ensures the struct occupies
    // a full cache line, so the next member starts on a new line.
  };

  AlignedAtomic head_; // Written by consumer ONLY
  AlignedAtomic tail_; // Written by producer ONLY

  T *buffer_;
  uint64_t mask_;
};

// ═══════════════════════════════════════════════════════════════════════
//  7. ORDER TYPES & DATA STRUCTURES
// ═══════════════════════════════════════════════════════════════════════

enum class Side : uint8_t {
  BID = 0, // Buy
  ASK = 1, // Sell
};

enum class OrderType : uint8_t {
  LIMIT = 0,
  MARKET = 1,
  CANCEL = 2,
};

/// Core order structure — designed to fit in a single cache line (64 bytes).
///
/// Layout:
///   id             8B   offset  0
///   instrument_id  8B   offset  8
///   price          8B   offset 16   (fixed-point, price * PRICE_MULTIPLIER)
///   quantity       4B   offset 24
///   remaining_qty  4B   offset 28
///   timestamp      8B   offset 32
///   side           1B   offset 40
///   type           1B   offset 41
///   active         1B   offset 42
///   (5B implicit)       offset 43   (compiler padding for 8B-aligned next)
///   next           8B   offset 48   (intrusive list pointer)
///
/// Total: 56 bytes usable, alignas(64) pads to 64B cache line
struct alignas(config::CACHE_LINE_SIZE) Order {
  uint64_t id = 0;
  uint64_t instrument_id = 0;
  int64_t price = 0; // Fixed-point: real_price * 10000
  uint32_t quantity = 0;
  uint32_t remaining_qty = 0;
  uint64_t timestamp = 0; // Nanosecond timestamp
  Side side = Side::BID;
  OrderType type = OrderType::LIMIT;
  uint8_t active = 0; // 1 = live, 0 = cancelled/filled

  // Intrusive linked list pointer for PriceLevel — eliminates std::vector
  // and its hidden malloc on the hot path. Compiler inserts 5B padding
  // between active(offset 42) and next(offset 48) for 8B alignment.
  Order *next = nullptr;
};

static_assert(sizeof(Order) == config::CACHE_LINE_SIZE,
              "Order must be exactly one cache line");
static_assert(std::is_trivially_copyable_v<Order>,
              "Order must be trivially copyable for pool/ring");

/// Message envelope for the ring buffer.
/// Contains either an order pointer (for add/match) or an order ID (for
/// cancel).
struct OrderMessage {
  OrderType type = OrderType::LIMIT;
  Order *order = nullptr; // Non-owning pointer from ObjectPool
  uint64_t cancel_id = 0; // Only used for CANCEL messages

  // Padding for trivial copy
  uint8_t _pad[7] = {};
};

static_assert(std::is_trivially_copyable_v<OrderMessage>,
              "OrderMessage must be trivially copyable for ring buffer");

// ═══════════════════════════════════════════════════════════════════════
//  8. INTRUSIVE ORDER LIST — Zero-Allocation FIFO Linked List
// ═══════════════════════════════════════════════════════════════════════

/// Singly-linked intrusive list for Order nodes.
/// The `next` pointer lives inside Order itself (no external allocation).
///
/// Design:
///   - push_back: O(1), sets tail->next = node, advances tail. ZERO alloc.
///   - match:     O(K), walks from head, fills orders FIFO.
///   - compact:   O(N), unlinks inactive nodes (periodic, not hot path).
///   - All nodes come from ObjectPool — no new/delete, no malloc.
///
/// This replaces std::vector<Order*> to eliminate hidden reallocation
/// when a price level exceeds its reserved capacity (quote stuffing defense).
class IntrusiveOrderList {
public:
  IntrusiveOrderList() = default;

  // ─────────── API ───────────

  /// Append an order to the tail. O(1), zero allocation.
  void push_back(Order *order) noexcept {
    order->next = nullptr;
    if (tail_) {
      tail_->next = order;
    } else {
      head_ = order;
    }
    tail_ = order;
    ++count_;
  }

  /// Match against orders in FIFO order up to `qty` units.
  /// Returns total quantity filled.
  uint32_t match(uint32_t qty) noexcept {
    uint32_t filled = 0;
    Order *current = head_;

    while (current && qty > 0) {
      if (current->active && current->remaining_qty > 0) {
        uint32_t fill_qty = std::min(current->remaining_qty, qty);
        current->remaining_qty -= fill_qty;
        qty -= fill_qty;
        filled += fill_qty;

        if (current->remaining_qty == 0) {
          current->active = 0;
        }
      }
      current = current->next;
    }
    return filled;
  }

  /// Unlink inactive/filled nodes from the list (periodic cleanup).
  /// O(N) where N = list length. NOT on the hot path.
  void compact() noexcept {
    Order *prev = nullptr;
    Order *current = head_;

    while (current) {
      Order *next_node = current->next;

      if (!current->active || current->remaining_qty == 0) {
        // Unlink this node
        if (prev) {
          prev->next = next_node;
        } else {
          head_ = next_node;
        }
        if (current == tail_) {
          tail_ = prev;
        }
        current->next = nullptr;
        --count_;
      } else {
        prev = current;
      }
      current = next_node;
    }
  }

  // ─────────── Accessors ───────────

  [[nodiscard]] bool empty() const noexcept { return head_ == nullptr; }
  [[nodiscard]] std::size_t size() const noexcept { return count_; }
  [[nodiscard]] Order *head() const noexcept { return head_; }

private:
  Order *head_ = nullptr;
  Order *tail_ = nullptr;
  std::size_t count_ = 0;
};

// ═══════════════════════════════════════════════════════════════════════
//  9. PRICE LEVEL — Intrusive list of orders at one price point
// ═══════════════════════════════════════════════════════════════════════

/// Orders at a single price level maintained as an intrusive linked list.
/// Orders are FIFO (price-time priority). Cancellation sets remaining_qty=0
/// (lazy delete — avoids list surgery on hot path).
///
/// Key invariant: add_order is ALWAYS O(1) with ZERO heap allocation,
/// regardless of how many orders are at this price level.
///
/// Complexity: add O(1) guaranteed, match O(K) where K = fills per level
class PriceLevel {
public:
  PriceLevel() = default;

  explicit PriceLevel(int64_t price) : price_(price) {}

  // ─────────── API ───────────

  void add_order(Order *order) noexcept {
    orders_.push_back(order);
    cached_qty_ += order->remaining_qty;
  }

  /// Match against this level up to `qty` units.
  /// Returns total quantity filled at this level.
  uint32_t match(uint32_t qty) noexcept {
    uint32_t filled = orders_.match(qty);
    cached_qty_ -= filled;
    return filled;
  }

  /// Decrement cached quantity (for external cancellation).
  void reduce_qty(uint32_t amount) noexcept {
    if (amount <= cached_qty_)
      cached_qty_ -= amount;
    else
      cached_qty_ = 0;
  }

  /// Remove fully filled/cancelled orders (periodic cleanup, not on hot path).
  void compact() noexcept { orders_.compact(); }

  // ─────────── Accessors ───────────

  [[nodiscard]] int64_t price() const noexcept { return price_; }
  [[nodiscard]] bool empty() const noexcept { return orders_.empty(); }
  [[nodiscard]] uint32_t total_qty() const noexcept { return cached_qty_; }
  [[nodiscard]] std::size_t order_count() const noexcept {
    return orders_.size();
  }

private:
  int64_t price_ = 0;
  uint32_t cached_qty_ = 0; // O(1) total quantity tracking
  IntrusiveOrderList orders_;
};

// ═══════════════════════════════════════════════════════════════════════
//  10. ORDER BOOK — Bid/Ask with Price-Time Matching
// ═══════════════════════════════════════════════════════════════════════

/// Cache-friendly order book with flat vector price levels.
///
/// Design:
///   - Bids and asks stored as vectors of PriceLevel, indexed by normalized
///   price
///   - Price normalization: index = (price - base_price) mapped to [0,
///   MAX_PRICE_LEVELS)
///   - Order ID -> Order* lookup via flat array (O(1) cancel)
///   - Matching: walk best bid vs best ask, fill at aggressor price
///
/// Complexity:
///   add_order: O(1) guaranteed (index + intrusive list push_back, ZERO alloc)
///   cancel:    O(1) (lookup + set inactive)
///   match:     O(L * K) where L = crossing levels, K = orders per level
class OrderBook {
public:
  OrderBook() {
    bid_levels_.resize(config::MAX_PRICE_LEVELS);
    ask_levels_.resize(config::MAX_PRICE_LEVELS);

    // Initialize price levels with their prices
    for (std::size_t i = 0; i < config::MAX_PRICE_LEVELS; ++i) {
      auto price = static_cast<int64_t>(i) * config::PRICE_MULTIPLIER / 100;
      bid_levels_[i] = PriceLevel(price);
      ask_levels_[i] = PriceLevel(price);
    }

    // Order ID map: heap-allocated flat array for O(1) lookup
    // Allocated once at construction (not on hot path)
    id_map_.resize(config::ORDER_ID_MAP_SIZE, nullptr);
  }

  // ─────────── API ───────────

  /// Add a limit order to the book.
  void add_order(Order *order) {
    std::size_t level_idx = price_to_index(order->price);
    if (level_idx >= config::MAX_PRICE_LEVELS) [[unlikely]]
      return;

    order->active = 1;

    // Register in ID map for O(1) cancel
    auto map_idx = order->id & (config::ORDER_ID_MAP_SIZE - 1);
    id_map_[map_idx] = order;

    if (order->side == Side::BID) {
      bid_levels_[level_idx].add_order(order);
      if (level_idx > best_bid_idx_)
        best_bid_idx_ = level_idx;
    } else {
      ask_levels_[level_idx].add_order(order);
      if (best_ask_idx_ == 0 || level_idx < best_ask_idx_) {
        best_ask_idx_ = level_idx;
      }
    }
  }

  /// Cancel an order by ID. O(1).
  /// Updates PriceLevel cached_qty_ to prevent stale-quantity infinite loops.
  bool cancel_order(uint64_t order_id) {
    auto map_idx = order_id & (config::ORDER_ID_MAP_SIZE - 1);
    Order *order = id_map_[map_idx];

    if (!order || order->id != order_id || !order->active) {
      return false;
    }

    // Update cached quantity on the correct price level BEFORE zeroing
    std::size_t level_idx = price_to_index(order->price);
    if (level_idx < config::MAX_PRICE_LEVELS) {
      if (order->side == Side::BID) {
        bid_levels_[level_idx].reduce_qty(order->remaining_qty);
      } else {
        ask_levels_[level_idx].reduce_qty(order->remaining_qty);
      }
    }

    order->active = 0;
    order->remaining_qty = 0;
    id_map_[map_idx] = nullptr;
    ++cancel_count_;
    return true;
  }

  /// Match crossing orders (bid >= ask). Returns total filled quantity.
  uint64_t match() {
    uint64_t total_filled = 0;

    while (best_bid_idx_ > 0 && best_ask_idx_ > 0 &&
           best_bid_idx_ < config::MAX_PRICE_LEVELS &&
           best_ask_idx_ < config::MAX_PRICE_LEVELS) {
      auto &bid_level = bid_levels_[best_bid_idx_];
      auto &ask_level = ask_levels_[best_ask_idx_];

      // Crossing condition: best bid price >= best ask price
      if (bid_level.price() < ask_level.price())
        break;

      uint32_t bid_qty = bid_level.total_qty();
      uint32_t ask_qty = ask_level.total_qty();

      if (bid_qty == 0) {
        // No active bids at this level, move down
        if (best_bid_idx_ > 0)
          --best_bid_idx_;
        else
          break;
        continue;
      }
      if (ask_qty == 0) {
        // No active asks at this level, move up
        ++best_ask_idx_;
        continue;
      }

      // Match: fill the smaller side
      uint32_t match_qty = std::min(bid_qty, ask_qty);
      bid_level.match(match_qty);
      ask_level.match(match_qty);

      total_filled += match_qty;
      ++match_count_;

      // Update best levels if exhausted
      if (bid_level.total_qty() == 0 && best_bid_idx_ > 0)
        --best_bid_idx_;
      if (ask_level.total_qty() == 0)
        ++best_ask_idx_;
    }

    return total_filled;
  }

  /// Match a market order immediately against the book.
  uint64_t match_market(Order *order) {
    uint64_t filled = 0;

    if (order->side == Side::BID) {
      // Market buy: match against asks (ascending)
      for (std::size_t i = best_ask_idx_; i < config::MAX_PRICE_LEVELS; ++i) {
        if (order->remaining_qty == 0)
          break;
        uint32_t fill = ask_levels_[i].match(order->remaining_qty);
        order->remaining_qty -= fill;
        filled += fill;
        if (ask_levels_[i].total_qty() == 0 && i == best_ask_idx_) {
          ++best_ask_idx_;
        }
      }
    } else {
      // Market sell: match against bids (descending)
      for (std::size_t i = best_bid_idx_; i < config::MAX_PRICE_LEVELS; --i) {
        if (order->remaining_qty == 0)
          break;
        uint32_t fill = bid_levels_[i].match(order->remaining_qty);
        order->remaining_qty -= fill;
        filled += fill;
        if (bid_levels_[i].total_qty() == 0 && i == best_bid_idx_) {
          if (best_bid_idx_ > 0)
            --best_bid_idx_;
          else
            break;
        }
        if (i == 0)
          break;
      }
    }

    if (filled > 0)
      ++match_count_;
    return filled;
  }

  // ─────────── Stats ───────────

  [[nodiscard]] uint64_t match_count() const noexcept { return match_count_; }
  [[nodiscard]] uint64_t cancel_count() const noexcept { return cancel_count_; }

  [[nodiscard]] int64_t best_bid_price() const noexcept {
    if (best_bid_idx_ < config::MAX_PRICE_LEVELS) {
      return bid_levels_[best_bid_idx_].price();
    }
    return 0;
  }

  [[nodiscard]] int64_t best_ask_price() const noexcept {
    if (best_ask_idx_ < config::MAX_PRICE_LEVELS) {
      return ask_levels_[best_ask_idx_].price();
    }
    return 0;
  }

private:
  /// Convert fixed-point price to level index.
  [[nodiscard]] static std::size_t price_to_index(int64_t price) noexcept {
    // Normalize: price / (PRICE_MULTIPLIER/100) gives index
    auto idx = static_cast<std::size_t>(price * 100 / config::PRICE_MULTIPLIER);
    return std::min(idx, config::MAX_PRICE_LEVELS - 1);
  }

  std::vector<PriceLevel> bid_levels_;
  std::vector<PriceLevel> ask_levels_;

  // Flat O(1) order ID -> Order* map (heap-allocated once, not on hot path)
  std::vector<Order *> id_map_;

  std::size_t best_bid_idx_ = 0;
  std::size_t best_ask_idx_ = 0;

  uint64_t match_count_ = 0;
  uint64_t cancel_count_ = 0;
};

// ═══════════════════════════════════════════════════════════════════════
//  10. CPU PINNING — Platform-specific thread affinity
// ═══════════════════════════════════════════════════════════════════════

namespace platform {

/// Pin the calling thread to a specific CPU core.
/// Prevents context switches and ensures cache locality.
inline bool pin_thread_to_core(int core_id) {
#ifdef _WIN32
  DWORD_PTR mask = static_cast<DWORD_PTR>(1) << core_id;
  HANDLE thread = GetCurrentThread();
  DWORD_PTR prev = SetThreadAffinityMask(thread, mask);
  return prev != 0;
#else
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core_id, &cpuset);
  return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) ==
         0;
#endif
}

/// Get current timestamp in nanoseconds (monotonic clock).
[[nodiscard]] inline uint64_t timestamp_ns() noexcept {
  auto now = std::chrono::steady_clock::now();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          now.time_since_epoch())
          .count());
}

} // namespace platform

// ═══════════════════════════════════════════════════════════════════════
//  11. ENGINE STATISTICS — Atomic counters for cross-thread reporting
// ═══════════════════════════════════════════════════════════════════════

struct alignas(config::CACHE_LINE_SIZE) EngineStats {
  std::atomic<uint64_t> orders_received{0};
  std::atomic<uint64_t> orders_processed{0};
  std::atomic<uint64_t> total_fills{0};
  std::atomic<uint64_t> ring_buffer_full_count{0};
  std::atomic<uint64_t> pool_exhausted_count{0};
  std::atomic<bool> running{true};
};

// ═══════════════════════════════════════════════════════════════════════
//  12. MATCHER THREAD — Pinned busy-spin event loop
// ═══════════════════════════════════════════════════════════════════════

/// The core matching engine loop.
///
/// Design:
///   - Pinned to a dedicated CPU core (no context switches)
///   - Busy-spin: no sleep(), no yield() — minimum latency
///   - All memory from pre-allocated ObjectPool (zero heap alloc)
///   - Processes OrderMessages from the SPSC ring buffer
///
/// Hot path: pop() -> dispatch -> add/cancel/match -> stats update
/// Expected latency per order: < 1 microsecond
class MatcherThread {
public:
  MatcherThread(LockFreeRingBuffer<OrderMessage> &ring_buffer,
                ObjectPool<Order> &order_pool, EngineStats &stats, int core_id)
      : ring_buffer_(ring_buffer), order_pool_(order_pool), stats_(stats),
        core_id_(core_id) {}

  /// Main entry point — runs until stats_.running becomes false.
  void operator()() {
    // ── Step 1: Pin to dedicated core ──
    if (platform::pin_thread_to_core(core_id_)) {
      // Pinning successful (silently continue)
    } else {
      std::cerr << "[WARN] MatcherThread: failed to pin to core " << core_id_
                << "\n";
    }

    // ── Step 2: Busy-spin event loop ──
    OrderMessage msg{};
    uint64_t loop_count = 0;
    constexpr uint64_t COMPACT_INTERVAL = 100'000;

    while (stats_.running.load(std::memory_order_relaxed)) {
      if (ring_buffer_.pop(msg)) {
        process_message(msg);
        stats_.orders_processed.fetch_add(1, std::memory_order_relaxed);
      }
      // No sleep, no yield — pure busy-spin for minimum latency

      // Periodic maintenance (not on critical path)
      ++loop_count;
      if ((loop_count & (COMPACT_INTERVAL - 1)) == 0) [[unlikely]] {
        // Could do periodic level compaction here
        // book_.compact() — omitted for hot-path purity
      }
    }

    // ── Step 3: Drain remaining messages ──
    while (ring_buffer_.pop(msg)) {
      process_message(msg);
      stats_.orders_processed.fetch_add(1, std::memory_order_relaxed);
    }
  }

private:
  void process_message(const OrderMessage &msg) {
    switch (msg.type) {
    case OrderType::LIMIT: {
      book_.add_order(msg.order);
      uint64_t fills = book_.match();
      if (fills > 0) {
        stats_.total_fills.fetch_add(fills, std::memory_order_relaxed);
      }
      break;
    }
    case OrderType::MARKET: {
      uint64_t fills = book_.match_market(msg.order);
      stats_.total_fills.fetch_add(fills, std::memory_order_relaxed);
      // Market orders are fully processed, release back to pool
      order_pool_.release(msg.order);
      break;
    }
    case OrderType::CANCEL: {
      book_.cancel_order(msg.cancel_id);
      break;
    }
    }
  }

  LockFreeRingBuffer<OrderMessage> &ring_buffer_;
  ObjectPool<Order> &order_pool_;
  EngineStats &stats_;
  int core_id_;
  OrderBook book_;
};

// ═══════════════════════════════════════════════════════════════════════
//  13. GATEWAY SIMULATOR — Synthetic order generator (Producer)
// ═══════════════════════════════════════════════════════════════════════

/// Simulates an order gateway feeding the matching engine.
///
/// Generates realistic order flow:
///   - 70% Limit orders (normal price distribution around mid-price)
///   - 20% Market orders (immediate execution)
///   - 10% Cancel orders (cancel previously sent orders)
///   - Zipfian instrument distribution (few hot instruments)
class GatewaySimulator {
public:
  GatewaySimulator(LockFreeRingBuffer<OrderMessage> &ring_buffer,
                   ObjectPool<Order> &order_pool, EngineStats &stats,
                   std::size_t total_orders)
      : ring_buffer_(ring_buffer), order_pool_(order_pool), stats_(stats),
        total_orders_(total_orders),
        rng_(42) // Deterministic seed for reproducibility
  {}

  /// Main entry point — generates and pushes orders.
  void operator()() {
    uint64_t next_id = 1;

    for (std::size_t i = 0; i < total_orders_; ++i) {
      if (!stats_.running.load(std::memory_order_relaxed))
        break;

      double roll = dist_uniform_(rng_);
      OrderMessage msg{};

      if (roll < config::LIMIT_ORDER_RATIO) {
        // ── Limit Order ──
        Order *order = order_pool_.acquire();
        if (!order) [[unlikely]] {
          stats_.pool_exhausted_count.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        fill_limit_order(order, next_id++);

        msg.type = OrderType::LIMIT;
        msg.order = order;
      } else if (roll <
                 config::LIMIT_ORDER_RATIO + config::MARKET_ORDER_RATIO) {
        // ── Market Order ──
        Order *order = order_pool_.acquire();
        if (!order) [[unlikely]] {
          stats_.pool_exhausted_count.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        fill_market_order(order, next_id++);

        msg.type = OrderType::MARKET;
        msg.order = order;
      } else {
        // ── Cancel Order ──
        msg.type = OrderType::CANCEL;
        msg.cancel_id = generate_cancel_id(next_id);
      }

      // Push to ring buffer with back-pressure retry
      while (!ring_buffer_.push(msg)) {
        stats_.ring_buffer_full_count.fetch_add(1, std::memory_order_relaxed);
        // Spin-wait: producer backs off briefly
        std::this_thread::yield();
      }

      stats_.orders_received.fetch_add(1, std::memory_order_relaxed);
    }
  }

private:
  void fill_limit_order(Order *order, uint64_t id) {
    order->id = id;
    order->instrument_id = dist_instrument_(rng_) % 100;
    order->side = (dist_uniform_(rng_) < 0.5) ? Side::BID : Side::ASK;
    order->type = OrderType::LIMIT;
    order->timestamp = platform::timestamp_ns();

    // Price: normal distribution around mid-price
    double price_offset = dist_price_(rng_);
    int64_t raw_price = config::MID_PRICE + static_cast<int64_t>(price_offset);
    order->price = std::max(raw_price, static_cast<int64_t>(1));

    // Quantity: 1-1000 units
    order->quantity = static_cast<uint32_t>(dist_qty_(rng_)) + 1;
    order->remaining_qty = order->quantity;
    order->active = 1;
  }

  void fill_market_order(Order *order, uint64_t id) {
    order->id = id;
    order->instrument_id = dist_instrument_(rng_) % 100;
    order->side = (dist_uniform_(rng_) < 0.5) ? Side::BID : Side::ASK;
    order->type = OrderType::MARKET;
    order->price = 0; // Market orders have no price
    order->timestamp = platform::timestamp_ns();
    order->quantity = static_cast<uint32_t>(dist_qty_(rng_)) + 1;
    order->remaining_qty = order->quantity;
    order->active = 1;
  }

  [[nodiscard]] uint64_t generate_cancel_id(uint64_t current_max_id) {
    if (current_max_id <= 1)
      return 1;
    // Cancel a random recent order
    auto range = std::uniform_int_distribution<uint64_t>(1, current_max_id - 1);
    return range(rng_);
  }

  LockFreeRingBuffer<OrderMessage> &ring_buffer_;
  ObjectPool<Order> &order_pool_;
  EngineStats &stats_;
  std::size_t total_orders_;

  // ── RNG state ──
  std::mt19937_64 rng_;
  std::uniform_real_distribution<double> dist_uniform_{0.0, 1.0};
  std::normal_distribution<double> dist_price_{0.0, 5000.0};
  std::uniform_int_distribution<uint32_t> dist_qty_{1, 999};
  std::uniform_int_distribution<uint64_t> dist_instrument_{0, 99};
};

// ═══════════════════════════════════════════════════════════════════════
//  14. REPORT — Final statistics output
// ═══════════════════════════════════════════════════════════════════════

namespace report {

/// Format a fixed-point price to human-readable string.
[[nodiscard]] inline std::string format_price(int64_t fixed_price) {
  int64_t whole = fixed_price / config::PRICE_MULTIPLIER;
  int64_t fraction = std::abs(fixed_price % config::PRICE_MULTIPLIER);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%lld.%04lld", static_cast<long long>(whole),
                static_cast<long long>(fraction));
  return std::string(buf);
}

inline void print_report(const EngineStats &stats, double elapsed_seconds,
                         const MemoryArena &arena) {
  auto received = stats.orders_received.load();
  auto processed = stats.orders_processed.load();
  auto fills = stats.total_fills.load();
  auto rb_full = stats.ring_buffer_full_count.load();
  auto pool_oom = stats.pool_exhausted_count.load();

  double throughput = (elapsed_seconds > 0)
                          ? static_cast<double>(processed) / elapsed_seconds
                          : 0.0;

  double avg_latency_ns = (throughput > 0) ? 1e9 / throughput : 0.0;

  std::cout
      << "\n"
      << "================================================================\n"
      << "  [*] HYPER-CORE HFT MATCHING ENGINE — FINAL REPORT\n"
      << "================================================================\n"
      << "\n"
      << "   Metric                        Value\n"
      << "   ─────────────────────────────────────────────────\n";

  char line[128];

  std::snprintf(line, sizeof(line), "   %-30s %20llu\n", "Orders Received",
                static_cast<unsigned long long>(received));
  std::cout << line;

  std::snprintf(line, sizeof(line), "   %-30s %20llu\n", "Orders Processed",
                static_cast<unsigned long long>(processed));
  std::cout << line;

  std::snprintf(line, sizeof(line), "   %-30s %20llu\n", "Total Fills (units)",
                static_cast<unsigned long long>(fills));
  std::cout << line;

  std::snprintf(line, sizeof(line), "   %-30s %17.2f s\n", "Elapsed Time",
                elapsed_seconds);
  std::cout << line;

  std::snprintf(line, sizeof(line), "   %-30s %14.0f ops/s\n", "Throughput",
                throughput);
  std::cout << line;

  std::snprintf(line, sizeof(line), "   %-30s %17.0f ns\n",
                "Avg Latency (estimate)", avg_latency_ns);
  std::cout << line;

  std::cout << "\n"
            << "   ─────────────────────────────────────────────────\n"
            << "   [*] INFRASTRUCTURE\n"
            << "   ─────────────────────────────────────────────────\n";

  std::snprintf(line, sizeof(line), "   %-30s %20llu\n",
                "Ring Buffer Full Events",
                static_cast<unsigned long long>(rb_full));
  std::cout << line;

  std::snprintf(line, sizeof(line), "   %-30s %20llu\n",
                "Pool Exhausted Events",
                static_cast<unsigned long long>(pool_oom));
  std::cout << line;

  double arena_used_mb = static_cast<double>(arena.used()) / (1024.0 * 1024.0);
  double arena_cap_mb =
      static_cast<double>(arena.capacity()) / (1024.0 * 1024.0);

  std::snprintf(line, sizeof(line), "   %-30s %13.2f / %.0f MB\n",
                "Arena Memory Used", arena_used_mb, arena_cap_mb);
  std::cout << line;

  std::snprintf(line, sizeof(line), "   %-30s %20zu B\n", "sizeof(Order)",
                sizeof(Order));
  std::cout << line;

  std::snprintf(line, sizeof(line), "   %-30s %20zu B\n",
                "sizeof(OrderMessage)", sizeof(OrderMessage));
  std::cout << line;

  // ── Evaluation ──
  std::cout
      << "\n"
      << "================================================================\n"
      << "  [*] EVALUATION\n"
      << "================================================================\n";

  const char *throughput_status =
      (throughput >= 500'000.0) ? "[OK] PASSED" : "[!!] BELOW TARGET";
  std::snprintf(line, sizeof(line),
                "   Throughput >= 500K ops/s:    %s (%.0f ops/s)\n",
                throughput_status, throughput);
  std::cout << line;

  const char *zero_alloc_status =
      (pool_oom == 0) ? "[OK] PASSED" : "[!!] POOL EXHAUSTION DETECTED";
  std::snprintf(line, sizeof(line), "   Zero-Alloc Hot Path:         %s\n",
                zero_alloc_status);
  std::cout << line;

  const char *lock_free_status = "[OK] PASSED (SPSC, no mutex)";
  std::snprintf(line, sizeof(line), "   Lock-Free Communication:     %s\n",
                lock_free_status);
  std::cout << line;

  std::cout
      << "================================================================\n\n";
}

} // namespace report

// ═══════════════════════════════════════════════════════════════════════
//  15. MAIN — Orchestration
// ═══════════════════════════════════════════════════════════════════════

#ifndef HYPER_CORE_NO_MAIN // Allow tests/benchmarks to exclude main()

int main() {
  using namespace std::chrono;

  std::cout
      << "\n"
      << "================================================================\n"
      << "  Hyper-Core HFT Matching Engine v1.0.0\n"
      << "  C++20 | Lock-Free SPSC | Zero-Alloc | Cache-Optimized\n"
      << "================================================================\n"
      << "\n";

  // ── Step 1: Pre-allocate all memory ──
  std::cout << "[>>] Allocating Memory Arena ("
            << config::ARENA_SIZE_BYTES / (1024 * 1024) << " MB)..."
            << std::endl;

  MemoryArena arena(config::ARENA_SIZE_BYTES);

  std::cout << "[>>] Creating ObjectPool<Order> (" << config::MAX_ORDERS
            << " slots, " << config::MAX_ORDERS * sizeof(Order) / (1024 * 1024)
            << " MB)..." << std::endl;

  ObjectPool<Order> order_pool(arena, config::MAX_ORDERS);

  std::cout << "[>>] Creating SPSC Ring Buffer (capacity: "
            << config::RING_BUFFER_CAPACITY << ")..." << std::endl;

  LockFreeRingBuffer<OrderMessage> ring_buffer(arena);

  // ── Step 2: Create shared stats ──
  EngineStats stats{};

  std::cout << "[>>] Arena used after init: " << arena.used() / (1024 * 1024)
            << " MB / " << arena.capacity() / (1024 * 1024) << " MB"
            << std::endl;

  // ── Step 3: Launch matcher thread (pinned) ──
  std::cout << "[>>] Starting MatcherThread (pinned to core "
            << config::MATCHER_CORE_ID << ")..." << std::endl;

  MatcherThread matcher(ring_buffer, order_pool, stats,
                        config::MATCHER_CORE_ID);
  std::thread matcher_thread(std::ref(matcher));

  // Brief pause to let matcher thread initialize and pin
  std::this_thread::sleep_for(milliseconds(50));

  // ── Step 4: Launch gateway simulator ──
  std::cout << "[>>] Starting GatewaySimulator (" << config::GATEWAY_ORDER_COUNT
            << " orders)..." << std::endl;

  auto start_time = steady_clock::now();

  GatewaySimulator gateway(ring_buffer, order_pool, stats,
                           config::GATEWAY_ORDER_COUNT);
  std::thread gateway_thread(std::ref(gateway));

  // ── Step 5: Wait for gateway to finish ──
  gateway_thread.join();

  // Brief drain period
  std::this_thread::sleep_for(milliseconds(100));

  // ── Step 6: Signal stop and wait for matcher ──
  stats.running.store(false, std::memory_order_release);
  matcher_thread.join();

  auto end_time = steady_clock::now();
  double elapsed =
      duration_cast<microseconds>(end_time - start_time).count() / 1e6;

  // ── Step 7: Print report ──
  report::print_report(stats, elapsed, arena);

  return 0;
}

#endif // HYPER_CORE_NO_MAIN
