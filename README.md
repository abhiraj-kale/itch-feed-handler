# Nasdaq ITCH 5.0 Feed Handler

A C++20 market-data feed handler for Nasdaq TotalView-ITCH 5.0:

- a zero-copy parser for all 23 message types
- MoldUDP64 sequencing with gap detection and retransmission recovery
- per-symbol order books rebuilt from every add, execute, cancel, delete, and replace
- top-of-book updates handed to a strategy thread over a lock-free SPSC ring

It comes with a UDP replay server that can drop packets on purpose, a synthetic session generator, benchmarks, and tests that check the books against a `std::map` reference.

Header-only core, no dependencies beyond the standard library and the OS socket API.

**Live demo: [itch-feed-handler.vercel.app](https://itch-feed-handler.vercel.app)**

**Interactive demo:** the same handler compiled to WebAssembly, running behind a simulated network you can break (packet loss, duplicates, reordering). It shows gap requests and retransmits on an animated wire, live books for the most active symbols, a byte-level message inspector, and an in-browser benchmark. Source in [`demo/`](demo).

## Results

Measured on a laptop (Intel Core i7-8565U, Windows 11, g++ 15.2 `-O3 -march=native`). The core is pinned, and the run uses a 20.4M message synthetic session (500 symbols, Nasdaq-like message mix) held in memory.

| Stage | Throughput | Per message |
|---|---|---|
| Framing + dispatch (walk length prefixes, switch on type) | **264 M msgs/s** (8.2 GB/s) | **3.8 ns** |
| Full field decode of every order message | 104 M msgs/s | 9.6 ns |
| Parse + rebuild every symbol's book | **15.9 M msgs/s** | 63 ns |

Per-message latency of parse + book update, timed individually with `rdtsc`/`rdtscp` (timer overhead subtracted):

| p50 | p90 | p99 | p99.9 |
|---|---|---|---|
| 59 ns | 95 ns | 159 ns | 350 ns |

End to end over real UDP on the same machine, replay server to feed handler, with packets dropped on purpose:

| Transport | Loss injected | Result |
|---|---|---|
| Multicast 239.1.1.1 | 0.5% of packets (617 dropped) | 633 gaps detected and recovered, **book matches the file** |
| Unicast loopback | 0.5% of packets | 2.1M msgs/s end to end (sender-bound), **book matches the file** |

"Book matches" means the feed handler compares a checksum of every level of every symbol, plus the live order count, against a book built straight from the capture file.

**Caveats, stated plainly:**

- These numbers come from synthetic data. A real full-day Nasdaq file has more symbols (about 8,000) and far more live orders, so the order table spills out of cache more and the book rebuild will be slower. The parser numbers should carry over.
- On Windows, loopback multicast sends cap the replay at about 0.5M msgs/s, so the end-to-end runs measure the network path, not the handler.

## How it works

```
 itch_replay                                   feed_handler
 ┌──────────────────────┐   MoldUDP64/UDP     ┌───────────────────────────────────────────────┐
 │ capture -> packets   │ ──────────────────▶ │ network thread (pinned)                       │
 │ (drops on purpose)   │                     │   MoldSession: sequence check, park, request  │
 │                      │ ◀── gap request ─── │   parse_stream: zero-copy views               │
 │ retransmit server    │ ── retransmits ───▶ │   BookBuilder: order table + per-symbol books │
 └──────────────────────┘                     │        │ top-of-book changes                  │
                                              │        ▼ SpscQueue<TopOfBook> (never blocks)  │
                                              │ strategy thread (pinned)                      │
                                              └───────────────────────────────────────────────┘
```

**Zero-copy parsing.** Every ITCH message is a fixed-size, big-endian record with an 11-byte common header. The parser walks the 2-byte length prefixes in place and hands the handler a view (`AddOrder{ptr}`, `OrderReplace{ptr}`, ...). A field is decoded only when it's read, with `memcpy` plus a byte swap, which compiles to one load and a `bswap`. Handlers are templates, so callbacks inline and unused ones compile away.

**MoldUDP64 gap recovery.** Each packet carries the sequence number of its first message. `MoldSession` handles four cases:

- An in-order packet is delivered.
- A duplicate or partially overlapping packet is trimmed message by message.
- A packet from the future is parked, and the missing range `[expected, first parked)` is requested from the retransmission server.
- Heartbeats and end-of-session packets reveal trailing gaps.

Parked packets are released as soon as the hole fills. If the retransmit itself is lost, a timeout asks again. The logic has no socket code, so it's tested deterministically.

**Order table.** Live orders sit inline (24 bytes) in an open-addressing table keyed by order reference. It uses linear probing, Fibonacci hashing, and backward-shift deletion, so the constant add and delete churn of a real feed never leaves tombstones. It starts small and doubles: sizing it far above the live order count turned every lookup into a cache miss.

**Prefetching.** The order table is the one structure that doesn't fit in cache. The parser looks 8 messages ahead and calls the handler's `prefetch()`. For executes, cancels, deletes, and replaces, `BookBuilder` prefetches the hash slot that message will probe.

**Price levels.** Each side of each book is a vector sorted worst to best, so the best price is at the back and most updates touch the last few entries (scanned linearly, then binary search). A level that empties below the touch stays in place and gets reused when the price returns, so the vector rarely shifts. Empty levels are only popped off the back.

**Strategy hand-off.** When a symbol's best bid or ask changes, a 32-byte `TopOfBook` goes onto a lock-free SPSC ring to the strategy thread. If the ring is ever full, the update is counted and dropped rather than blocking the feed.

## Correctness

`itch_tests` has 10 tests:

- message sizes for all 23 types match the spec, and nothing else has a size
- an Add Order built byte by byte from the spec decodes every field correctly
- partial messages at the end of a buffer are carried over, and corrupt type or length bytes are skipped
- file parsing gives identical books with 37-byte read chunks (almost every message straddles a boundary)
- the order table matches `std::unordered_map` across 400k operations and several resizes
- hand-written scenarios for every order message, including top-of-book updates firing only on real changes
- unknown order references are counted, not fatal
- **Differential:** 3 sessions of about 300k messages through `BookBuilder` and a `std::map` reference book. Full depth of every symbol must match, there must be zero unknown references, and books must never cross.
- **MoldUDP64 under abuse:** 60k messages with 3% loss, 3% delayed delivery, 3% duplicates, and a retransmission server that loses 20% of its answers. Every message must be delivered exactly once, in order, with a matching content hash.
- a heartbeat after the last packet reveals a trailing gap and triggers the right request

CI builds with GCC and Clang on Ubuntu, runs the tests, then replays a 1M message session over loopback UDP with 0.5% packet loss. That run must end with a matching book.

## Build and run

Requires CMake 3.20+ and a C++20 compiler (GCC 11+ or Clang 14+). x86-64 for the TSC benchmarks.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/itch_tests

# benchmark on a synthetic session, or on a real capture
./build/bench_parse --gen 20000000 500
./build/bench_parse 01302019.NASDAQ_ITCH50        # gunzip the public sample first

# end to end: generate, then replay with 0.5% packet loss into the handler
./build/itch_gen data/session.itch 5000000 500
./build/feed_handler --listen 239.1.1.1:31000 --request 127.0.0.1:31001 --verify data/session.itch &
./build/itch_replay data/session.itch --dest 239.1.1.1:31000 --req-port 31001 --drop 0.005
```

Public full-day captures are on Nasdaq's site at `emi.nasdaq.com/ITCH/Nasdaq ITCH/` (about 4.8 GB compressed each). The parser reads them directly; they use the same length-prefixed framing as MoldUDP64 payloads.

On Windows this builds with MSYS2 MinGW-w64 and links statically. Rebuild the WebAssembly demo with:

```bash
docker run --rm -v "$PWD":/src -w /src emscripten/emsdk:3.1.74 sh demo/build.sh
```

## Layout

```
include/itch/
  endian.hpp       big-endian loads/stores (memcpy + bswap)
  messages.hpp     ITCH 5.0 message views and size table
  parser.hpp       dispatch + length-prefixed stream walker with lookahead prefetch
  order_map.hpp    open-addressing order table, backward-shift delete, growth
  book.hpp         BookBuilder: per-symbol books, top-of-book publishing, checksum
  moldudp64.hpp    MoldSession (sequencing, gaps, recovery) + PacketBuilder
  generator.hpp    synthetic, spec-conformant ITCH sessions (uncrossed books)
  file.hpp         BinaryFILE readers (whole file or streamed in chunks)
  udp.hpp          small cross-platform UDP socket with multicast
  spsc_queue.hpp   lock-free single-producer single-consumer ring
  platform.hpp     thread pinning, TSC timing, percentiles
src/feed_handler.cpp     network thread + strategy thread
tools/itch_replay.cpp    MoldUDP64 replay + retransmission server, packet loss injection
tools/itch_gen.cpp       writes synthetic sessions
bench/bench_parse.cpp    parser and book benchmarks
tests/test_main.cpp      unit, differential, and recovery tests
demo/                    WebAssembly build and the interactive site
```

## Next steps

- Benchmark on a real full-day capture and publish those numbers next to the synthetic ones.
- Busy-poll receive and kernel-bypass options (recvmmsg batching, then AF_XDP) on Linux.
- Feed top-of-book updates into the [matching engine](https://github.com/abhiraj-kale/low-latency-order-book) to drive it with market-shaped flow.
