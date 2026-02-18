<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-20-blue?style=for-the-badge&logo=cplusplus&logoColor=white" />
  <img src="https://img.shields.io/badge/Architecture-Lock--Free-brightgreen?style=for-the-badge" />
  <img src="https://img.shields.io/badge/Allocation-Zero--Alloc_Hot_Path-orange?style=for-the-badge" />
  <img src="https://img.shields.io/badge/Latency-%3C1μs-red?style=for-the-badge" />
  <img src="https://img.shields.io/badge/License-MIT-yellow?style=for-the-badge" />
</p>

<h1 align="center">⚡ Hyper-Core HFT Matching Engine</h1>

<p align="center">
  <strong>Motor de Matching de Alta Frecuencia — Ultra-Baja Latencia</strong><br>
  <em>High-Frequency Trading Matching Engine — Ultra-Low Latency</em>
</p>

<p align="center">
  Un motor de matching de órdenes financieras diseñado desde cero para operar a la velocidad<br>
  que exigen los sistemas de trading de alta frecuencia: <strong>sub-microsegundo por operación</strong>,<br>
  <strong>cero asignaciones de memoria en el hot path</strong> y <strong>comunicación lock-free entre hilos</strong>.
</p>

---

## 🌐 Languages / Idiomas

- [🇪🇸 Español](#-español)
- [🇺🇸 English](#-english)

---

# 🇪🇸 Español

## 📋 Descripción

**Hyper-Core** es un motor de matching (emparejamiento) de órdenes financieras escrito en **C++20**, diseñado para demostrar las técnicas de ingeniería de software que se utilizan en sistemas de trading de alta frecuencia (HFT) reales.

En los mercados financieros electrónicos, el motor de matching es el corazón del exchange: recibe órdenes de compra y venta, las organiza en un libro de órdenes (order book) y ejecuta las operaciones cuando los precios se cruzan. La velocidad a la que esto ocurre determina quién obtiene la mejor ejecución.

Este proyecto implementa un motor completo con:
- **Procesamiento de órdenes a velocidad de nanosegundos**
- **Cero asignaciones de memoria durante la operación** (todo pre-alocado)
- **Comunicación entre hilos sin locks** (sin mutex, sin syscalls)
- **Afinidad de CPU** (el hilo del matcher se fija a un core para eliminar cambios de contexto)

## 🏗️ Arquitectura

```
┌───────────────────┐       SPSC RingBuffer        ┌──────────────────┐
│ GatewaySimulator  │ ────────(lock-free)────────▶ │  MatcherThread   │
│ (Producer Thread) │                               │ (Pinned to Core) │
└───────────────────┘                               └────────┬─────────┘
                                                             │
                                           ┌─────────────────┼────────────┐
                                           ▼                 ▼            ▼
                                    ┌────────────┐   ┌────────────┐ ┌──────────┐
                                    │ ObjectPool  │   │  OrderBook │ │  Stats   │
                                    │  (Order)    │   │(Intrusive) │ │ Counters │
                                    └──────┬──────┘   └────────────┘ └──────────┘
                                           │
                                    ┌──────┴──────┐
                                    │ MemoryArena │
                                    │ (Bump Alloc)│
                                    └─────────────┘
```

### Componentes

| # | Componente | Descripción | Complejidad |
|---|-----------|-------------|-------------|
| 1 | **MemoryArena** | Asignador bump: una sola alocación inicial de 64MB. O(1) por asignación, cero fragmentación. | O(1) |
| 2 | **ObjectPool\<Order\>** | Pool de objetos con free-list intrusiva. Adquirir/liberar en O(1) sin tocar el heap. | O(1) |
| 3 | **LockFreeRingBuffer** | Buffer SPSC (Single Producer, Single Consumer) con aislamiento por línea de caché. Sin contención, sin syscalls. | O(1) |
| 4 | **Order** | Estructura de 64 bytes alineada a línea de caché. Incluye puntero intrusivo `next` para lista enlazada. | — |
| 5 | **IntrusiveOrderList** | Lista enlazada intrusiva FIFO. `push_back` O(1) sin malloc. Reemplaza `std::vector` para eliminar realocaciones ocultas. | O(1) push |
| 6 | **PriceLevel** | Nivel de precio con lista intrusiva. Garantiza O(1) para agregar órdenes sin importar la cantidad. | O(1) add |
| 7 | **OrderBook** | Libro de órdenes Bid/Ask con matching por precio-tiempo. Cancelación O(1) vía mapa plano de IDs. | O(L×K) match |
| 8 | **MatcherThread** | Loop de eventos busy-spin fijado a un core de CPU. Sin sleep, sin yield. Latencia mínima. | — |
| 9 | **GatewaySimulator** | Generador sintético: 70% limit, 20% market, 10% cancel. Distribución realista de precios e instrumentos. | — |

## ⚡ Decisiones de Diseño

| Decisión | Alternativa | Razón |
|----------|-------------|-------|
| SPSC RingBuffer | mutex + cola | Cero contención, sin syscalls del kernel |
| `alignas(64)` head/tail | Alineación default | Elimina false sharing entre cores |
| Fences acquire/release | `seq_cst` | Overhead mínimo de barreras de memoria |
| **Lista intrusiva en PriceLevel** | `std::vector<Order*>` | Cero malloc, capacidad ilimitada sin realocación |
| Placement-new Pool | `new`/`delete` | Cero alocación de heap en hot path |
| Precios fixed-point | `double` | Comparación determinística sin errores de punto flotante |
| Bump allocator Arena | `malloc` por objeto | O(1) alocación, cero fragmentación |

## 🛡️ Defensa contra Quote Stuffing

Un problema crítico en HFT es el **quote stuffing**: un actor malintencionado envía miles de órdenes a un mismo nivel de precio para sobrecargar el sistema. Con `std::vector`, esto causa realocaciones ocultas (`malloc`) que generan spikes de latencia de 5µs a 200µs.

**Nuestra solución**: Lista enlazada intrusiva donde el puntero `next` vive dentro de la estructura `Order` (que ya está en el `ObjectPool`). Resultado: **agregar una orden siempre es O(1), sin importar si hay 10 o 100,000 órdenes en el nivel**.

## 🔧 Compilación y Ejecución

### Requisitos
- Compilador C++20 compatible (GCC 11+, Clang 14+, MSVC 2022+)
- Sistema operativo: Linux, macOS o Windows (con WSL recomendado)

### Compilar
```bash
g++ -std=c++20 -O2 -Wall -Wextra -pthread hyper_core_engine.cpp -o hyper_core_engine
```

### Ejecutar
```bash
./hyper_core_engine
```

### Salida esperada
```
================================================================
  Hyper-Core HFT Matching Engine v1.0.0
  C++20 | Lock-Free SPSC | Zero-Alloc | Cache-Optimized
================================================================

[>>] Allocating Memory Arena (64 MB)...
[>>] Creating ObjectPool<Order> (500000 slots, 30 MB)...
[>>] Creating SPSC Ring Buffer (capacity: 65536)...
[>>] Starting MatcherThread (pinned to core 1)...
[>>] Starting GatewaySimulator (200000 orders)...

================================================================
  [*] HYPER-CORE HFT MATCHING ENGINE — FINAL REPORT
================================================================

   Metric                        Value
   ─────────────────────────────────────────────────
   Orders Received                          200000
   Orders Processed                         200000
   Throughput                      >= 500,000 ops/s
   Avg Latency (estimate)                  < 1000 ns
   Zero-Alloc Hot Path:         [OK] PASSED
   Lock-Free Communication:     [OK] PASSED (SPSC, no mutex)
================================================================
```

## 📊 Métricas de Rendimiento

| Métrica | Objetivo | Estado |
|---------|----------|--------|
| Throughput | ≥ 500K ops/s | ✅ |
| Latencia promedio | < 1 µs | ✅ |
| Alocaciones en hot path | 0 | ✅ |
| Lock-free communication | Sí | ✅ |
| Afinidad de CPU | Core dedicado | ✅ |

## 🧠 Conceptos Técnicos Demostrados

- **C++20 Concepts** — Restricciones de tipos en tiempo de compilación
- **Lock-free programming** — Atomics con semántica acquire/release
- **Cache-line optimization** — `alignas(64)`, false sharing prevention
- **Memory pooling** — ObjectPool con placement-new
- **Intrusive data structures** — Lista enlazada sin alocación externa
- **Bump allocation** — Arena de memoria O(1)
- **CPU pinning** — Afinidad de hilo a core específico
- **Fixed-point arithmetic** — Precios sin errores de punto flotante
- **SPSC patterns** — Ring buffer sin contención

---

# 🇺🇸 English

## 📋 Description

**Hyper-Core** is a financial order matching engine written in **C++20**, designed to demonstrate the software engineering techniques used in real high-frequency trading (HFT) systems.

In electronic financial markets, the matching engine is the heart of the exchange: it receives buy and sell orders, organizes them into an order book, and executes trades when prices cross. The speed at which this happens determines who gets the best execution.

This project implements a complete engine featuring:
- **Nanosecond-speed order processing**
- **Zero memory allocations during operation** (everything pre-allocated)
- **Lock-free inter-thread communication** (no mutex, no syscalls)
- **CPU affinity** (matcher thread pinned to a dedicated core to eliminate context switches)

## 🏗️ Architecture

```
┌───────────────────┐       SPSC RingBuffer        ┌──────────────────┐
│ GatewaySimulator  │ ────────(lock-free)────────▶ │  MatcherThread   │
│ (Producer Thread) │                               │ (Pinned to Core) │
└───────────────────┘                               └────────┬─────────┘
                                                             │
                                           ┌─────────────────┼────────────┐
                                           ▼                 ▼            ▼
                                    ┌────────────┐   ┌────────────┐ ┌──────────┐
                                    │ ObjectPool  │   │  OrderBook │ │  Stats   │
                                    │  (Order)    │   │(Intrusive) │ │ Counters │
                                    └──────┬──────┘   └────────────┘ └──────────┘
                                           │
                                    ┌──────┴──────┐
                                    │ MemoryArena │
                                    │ (Bump Alloc)│
                                    └─────────────┘
```

### Components

| # | Component | Description | Complexity |
|---|-----------|-------------|------------|
| 1 | **MemoryArena** | Bump allocator: single 64MB initial allocation. O(1) per alloc, zero fragmentation. | O(1) |
| 2 | **ObjectPool\<Order\>** | Object pool with intrusive free-list. Acquire/release in O(1) with no heap touches. | O(1) |
| 3 | **LockFreeRingBuffer** | SPSC (Single Producer, Single Consumer) buffer with cache-line isolation. No contention, no syscalls. | O(1) |
| 4 | **Order** | 64-byte cache-line-aligned structure. Includes intrusive `next` pointer for linked list. | — |
| 5 | **IntrusiveOrderList** | Intrusive singly-linked FIFO list. O(1) `push_back` with zero malloc. Replaces `std::vector` to eliminate hidden reallocations. | O(1) push |
| 6 | **PriceLevel** | Price level with intrusive list. Guarantees O(1) order insertion regardless of count. | O(1) add |
| 7 | **OrderBook** | Bid/Ask order book with price-time priority matching. O(1) cancellation via flat ID map. | O(L×K) match |
| 8 | **MatcherThread** | Busy-spin event loop pinned to a CPU core. No sleep, no yield. Minimum latency. | — |
| 9 | **GatewaySimulator** | Synthetic generator: 70% limit, 20% market, 10% cancel. Realistic price and instrument distribution. | — |

## ⚡ Design Decisions

| Decision | Alternative | Rationale |
|----------|-------------|-----------|
| SPSC RingBuffer | mutex + queue | Zero contention, no kernel syscalls |
| `alignas(64)` head/tail | Default alignment | Eliminates false sharing between cores |
| Acquire/release fences | `seq_cst` | Minimal memory barrier overhead |
| **Intrusive list in PriceLevel** | `std::vector<Order*>` | Zero malloc, unbounded capacity without reallocation |
| Placement-new Pool | `new`/`delete` | Zero heap allocation on hot path |
| Fixed-point prices | `double` | Deterministic comparison without floating-point errors |
| Bump allocator Arena | `malloc` per object | O(1) allocation, zero fragmentation |

## 🛡️ Quote Stuffing Defense

A critical problem in HFT is **quote stuffing**: a malicious actor sends thousands of orders to the same price level to overload the system. With `std::vector`, this causes hidden reallocations (`malloc`) that spike latency from 5µs to 200µs.

**Our solution**: An intrusive linked list where the `next` pointer lives inside the `Order` structure (which is already in the `ObjectPool`). Result: **adding an order is always O(1), whether there are 10 or 100,000 orders at that level**.

## 🔧 Build & Run

### Requirements
- C++20 compatible compiler (GCC 11+, Clang 14+, MSVC 2022+)
- OS: Linux, macOS, or Windows (WSL recommended)

### Build
```bash
g++ -std=c++20 -O2 -Wall -Wextra -pthread hyper_core_engine.cpp -o hyper_core_engine
```

### Run
```bash
./hyper_core_engine
```

## 📊 Performance Metrics

| Metric | Target | Status |
|--------|--------|--------|
| Throughput | ≥ 500K ops/s | ✅ |
| Average latency | < 1 µs | ✅ |
| Hot path allocations | 0 | ✅ |
| Lock-free communication | Yes | ✅ |
| CPU affinity | Dedicated core | ✅ |

## 🧠 Technical Concepts Demonstrated

- **C++20 Concepts** — Compile-time type constraints
- **Lock-free programming** — Atomics with acquire/release semantics
- **Cache-line optimization** — `alignas(64)`, false sharing prevention
- **Memory pooling** — ObjectPool with placement-new
- **Intrusive data structures** — Linked list with no external allocation
- **Bump allocation** — O(1) memory arena
- **CPU pinning** — Thread-to-core affinity
- **Fixed-point arithmetic** — Prices without floating-point errors
- **SPSC patterns** — Contention-free ring buffer

---

## 📄 License

This project is licensed under the MIT License. See [LICENSE](LICENSE) for details.

---

<p align="center">
  <strong>Built with 🧠 by <a href="https://github.com/Darkangex">Darkangex</a></strong><br>
  <em>Systems Programming • High-Performance Computing • Financial Engineering</em>
</p>
