# VeloKV

[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg?style=flat-square&logo=c%2B%2B)](https://en.cppreference.com/w/cpp/17)
[![Platform](https://img.shields.io/badge/Platform-Windows%20%7C%20Linux-lightgrey.svg?style=flat-square)](https://en.wikipedia.org/wiki/Cross-platform_software)
[![Networking](https://img.shields.io/badge/Networking-Raw%20TCP%20Sockets-orange.svg?style=flat-square)](https://en.wikipedia.org/wiki/Transmission_Control_Protocol)
[![Domain](https://img.shields.io/badge/Domain-Systems%20Programming-green.svg?style=flat-square)](https://en.wikipedia.org/wiki/Systems_programming)

VeloKV is a high-performance, in-memory key-value store built from the ground up in C++17. Inspired by Redis, it implements a fully custom TCP networking stack using raw OS socket APIs, a RESP protocol parser, TTL-based key expiration with LRU eviction, crash-safe persistence, and a single-leader replication pipeline — all running on a single-threaded, non-blocking event loop.

---

## Why VeloKV?

Most developers interact with databases through high-level SDKs that abstract away the networking and storage layers entirely. VeloKV was built to understand what happens *underneath* — how bytes travel from a client socket to an in-memory hash map, how data survives a server crash, and how two independent processes stay in sync over a TCP connection.

---

## Features

| Feature | Details |
|---|---|
| **Event-Driven Server** | Single-threaded `select()`-based event loop handles all client I/O without spawning threads |
| **String & List Storage** | O(1) operations backed by `std::unordered_map` and `std::deque` |
| **LRU Eviction** | Memory-bounded store using a Hash Map + Doubly-Linked List LRU cache |
| **TTL Expiration** | Hybrid expiry: lazy eviction on read + periodic active sweep |
| **Crash-Safe Snapshots** | Atomic temp-file + rename strategy prevents corrupt saves on power loss |
| **Leader-Follower Replication** | Live write propagation to read-only replicas with auto-reconnect |
| **Cross-Platform** | Compiles on both Windows (Winsock2) and POSIX systems (Linux/macOS) |

---

## How It Works

VeloKV avoids the complexity of multi-threaded programming entirely. Instead of spawning one thread per client connection (which incurs memory overhead and context-switching costs), it uses a single-threaded event loop:

```
Client sends a command
        |
        v
[OS kernel buffers bytes on the TCP socket]
        |
        v
[select() wakes up the event loop — "this socket is readable"]
        |
        v
[RESP Parser reads & decodes the raw bytes into a Command]
        |
     +--+--+
     |     |
  (Read)  (Write)
     |     |
     v     v
[Database Engine: std::unordered_map / std::deque]
           |
     [Write command?]
           |
           v
  [ReplicationManager streams command to all replica sockets]
```

When a replica connects to the leader, it first performs a full-sync handshake — the leader serializes its entire in-memory state to disk and streams it over. After the initial sync, all subsequent write commands are propagated live over the same persistent TCP connection.

---

## Supported Commands

### String Operations
```
SET   <key> <value>    — Store a string value
GET   <key>            — Retrieve a value (returns nil if missing or expired)
DEL   <key>            — Remove a key from the store
EXISTS <key>           — Check if a key is present (1 = yes, 0 = no)
```

### List Operations
```
LPUSH <key> <value>    — Prepend a value to a list
RPUSH <key> <value>    — Append a value to a list
LPOP  <key>            — Remove and return the head element
RPOP  <key>            — Remove and return the tail element
LLEN  <key>            — Return the number of elements in a list
```

### Expiration
```
EXPIRE <key> <seconds> — Set a time-to-live on a key
TTL    <key>           — Get remaining lifetime (-1: no TTL, -2: key not found)
```

### Utility
```
PING   — Healthcheck; returns PONG
SAVE   — Trigger an immediate snapshot of the database to disk
```

---

## Project Structure

```
VeloKV/
├── Makefile
└── src/
    ├── main.cpp             — Entry point, CLI argument parsing, signal handling
    ├── common.hpp / .cpp    — Cross-platform socket abstractions (Winsock / POSIX)
    ├── server.hpp / .cpp    — Event loop, select() multiplexer, connection lifecycle
    ├── client.hpp / .cpp    — Per-connection read/write buffers, socket FD ownership
    ├── resp.hpp / .cpp      — RESP protocol parser and response serializer
    ├── command.hpp / .cpp   — Command dispatch table and execution logic
    ├── db.hpp / .cpp        — Core storage engine: strings, lists, TTL, LRU cache
    ├── persistence.hpp/.cpp — Atomic snapshot save/load (temp file + rename)
    └── replication.hpp/.cpp — Leader/replica handshake, sync, and propagation
```

---

## Build & Run

### Requirements
- C++17 compiler: `g++` or `clang++`
- `make` (Linux/macOS) or `mingw32-make` (Windows)

### Compile
```bash
make clean && make
```

### Run: Standalone Mode
```bash
./velokv_server --port 6379 --rdb store.rdb
```

### Run: Replication Mode
```bash
# Terminal 1 — Leader
./velokv_server --role leader --port 6379 --rdb leader.rdb

# Terminal 2 — Replica
./velokv_server --role replica --port 6380 --leader-host 127.0.0.1 --leader-port 6379 --rdb replica.rdb
```

---

## Quick Demo

Connect with `netcat` and issue commands directly:

```bash
$ nc 127.0.0.1 6379

SET session_token abc123
+OK

GET session_token
$6
abc123

EXPIRE session_token 30
:1

TTL session_token
:28

RPUSH tasks "send_email"
:1

RPUSH tasks "generate_report"
:2

LLEN tasks
:2

LPOP tasks
$10
send_email
```

---

## Design Decisions

**Why a single-threaded event loop?**
Threads are expensive — each thread on Linux consumes ~8MB of stack space by default, and the OS spends real CPU time context-switching between them. A single-threaded event loop with non-blocking I/O achieves the same concurrency without any of that overhead. This is the same model used by Redis, Nginx, and Node.js.

**Why `select()` instead of `epoll`?**
`epoll` is more scalable (O(1) vs O(N) per event loop tick) but is Linux-only. `select()` works identically on Windows and POSIX, keeping VeloKV portable across development environments. Migrating the multiplexer to `epoll` on Linux is a natural next step for production deployment.

**Why atomic file rename for persistence?**
If the server crashes halfway through writing a snapshot, a half-written file is worse than no file at all. By writing to a temporary file first and then atomically swapping it in with `rename()`, we guarantee the on-disk state is always either the old valid snapshot or the new one — never a corrupted in-between state.

**Why an LRU eviction policy?**
The store has a configurable memory capacity. When it fills up, the Least Recently Used key is evicted to make room for new data. This is implemented with a doubly-linked list (O(1) move-to-front on access) and a hash map (O(1) position lookup) — the classic O(1) LRU design.

---

## Potential Extensions

- **`epoll` / `kqueue` backend** — Replace `select()` with platform-native multiplexers for O(1) event dispatch
- **Append-Only File (AOF)** — Log every write command to disk for finer-grained durability
- **Partial Resync (PSYNC)** — Maintain a replication backlog on the leader so replicas can catch up without a full re-sync after a brief disconnect
- **Binary-safe parsing** — Handle null bytes inside bulk string payloads
