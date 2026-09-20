# VeloKV

[![Language](https://img.shields.io/badge/C%2B%2B-17-blue.svg?style=flat-square&logo=c%2B%2B)](https://en.cppreference.com/w/cpp/17)
[![Category](https://img.shields.io/badge/Networking-TCP-orange.svg?style=flat-square)](https://en.wikipedia.org/wiki/Transmission_Control_Protocol)
[![Category](https://img.shields.io/badge/Domain-Systems%20Programming-green.svg?style=flat-square)](https://en.wikipedia.org/wiki/Systems_programming)
[![Category](https://img.shields.io/badge/Domain-Distributed%20Systems-red.svg?style=flat-square)](https://en.wikipedia.org/wiki/Distributed_computing)

VeloKV is a Redis-inspired, high-performance, in-memory key-value database written in modern C++17. It features a custom TCP server implementation, a custom RESP command parser, list operations, TTL-based key expiration, crash-safe snapshot persistence, and single-leader replication.

Built using native socket APIs, VeloKV is designed to run as a single-threaded event loop utilizing I/O multiplexing (`select`), replicating Redis’s clean, lock-free execution model.

---

## Key Features

- **In-Memory Store:** High-performance, O(1) string and list storage using optimized standard library containers.
- **I/O Multiplexed TCP Server:** Handles multiple concurrent client connections on a single thread using the `select()` system call.
- **Redis-Style List Commands:** Supports double-ended queues for push, pop, and length queries.
- **Key Expiration (TTL):** Evicts expired keys using a hybrid approach—**lazy eviction** (evict on read) combined with an **active sweep** (periodic scanning).
- **Crash-Safe Persistence:** Periodically dumps database state to disk. Uses a safe write-to-temp and atomic rename strategy to prevent file corruption during crashes.
- **Leader–Replica Replication:** One leader process propagates live write commands to multiple read-only replica nodes. Replicas auto-handshake, download the initial database state via full sync, and automatically reconnect upon leader crash/recovery.

---

## System Architecture

VeloKV utilizes a single-threaded event-loop architecture to process network connections, execute transactions, and replicate data downstream without the synchronization overhead and race conditions of multi-threaded databases.

### Request & Propagation Lifecycle

```
                     +----------------------------+
                     |    Client (Telnet/nc/App)  |
                     +--------------+-------------+
                                    |
                             [ TCP Connection ]
                                    |
                                    v
                     +----------------------------+
                     |   TCP Socket Multiplexer   |
                     |         (select)           |
                     +--------------+-------------+
                                    |
                                    v
                     +----------------------------+
                     |   RESP Command Parser      |
                     +--------------+-------------+
                                    |
                                    v
                     +----------------------------+
                     |      Command Processor     |
                     +--------------+-------------+
                                    |
           +------------------------+------------------------+
           | (Read commands)                                 | (Write commands)
           v                                                 v
+--------------------+                             +--------------------+
|  Database Engine   |                             |  Database Engine   |
| (GET / TTL / LLEN) |                             | (SET / DEL / LPUSH)|
+--------------------+                             +----------+---------+
                                                              |
                                                    [ Local Commit & Save ]
                                                              |
                                                              v
                                                   +--------------------+
                                                   | ReplicationManager |
                                                   |  (Leader Node)     |
                                                   +----------+---------+
                                                              |
                                                     [ TCP Replication ]
                                                              |
                                                              v
                                                   +--------------------+
                                                   |  Replica Instance  |
                                                   |   (Read-Only DB)   |
                                                   +--------------------+
```

### Beginner-Friendly Architectural Overview
1. **Network Input:** Clients connect to VeloKV via TCP. The server listens on a designated port.
2. **The Multiplexer (`select`):** Instead of spawning a thread per client (which wastes memory and CPU time), a single thread sits in a loop. The operating system notifies this loop whenever any socket has incoming data to read or is ready to receive data.
3. **Parsing & Execution:** Incoming raw bytes are read into a buffer and parsed via the RESP (REdis Serialization Protocol) engine. The command is routed to the `Database` store.
4. **Data Sync & Replication:** If it is a write command and the database role is set to `leader`, the update is committed locally and appended to the replication buffers of all registered downstream `replica` sockets. If the database role is `replica`, direct write attempts from external clients are rejected.

---

## Supported Commands

### Keys & Strings
- `SET <key> <value>`: Sets the value of a key.
- `GET <key>`: Returns the value of a key, or `nil` if it does not exist.
- `DEL <key>`: Deletes a key from the database.
- `EXISTS <key>`: Returns `1` if the key exists; `0` otherwise.

### Lists
- `LPUSH <key> <value>`: Prepends a value to the head of the list.
- `RPUSH <key> <value>`: Appends a value to the tail of the list.
- `LPOP <key>`: Removes and returns the first element of the list.
- `RPOP <key>`: Removes and returns the last element of the list.
- `LLEN <key>`: Returns the length of the list.

### Key Expirations
- `EXPIRE <key> <seconds>`: Sets a timeout on a key in seconds.
- `TTL <key>`: Returns the remaining time-to-live of a key in seconds (`-1` if no TTL exists, `-2` if key is not found).

### System & Administration
- `PING`: Returns `PONG`.
- `SAVE`: Synchronously dumps the current memory snapshot to disk.

---

## Core Data Structures & Concepts Used

- **`std::unordered_map`:** Used for O(1) average lookup. One map acts as the primary key-value store, and a secondary map indexes key absolute deadlines (epoch-based milliseconds) to handle TTLs.
- **`std::deque`:** Chosen for list-type commands. Offers O(1) insertions and deletions at both the beginning and the end, which aligns perfectly with `LPUSH`/`RPUSH` and `LPOP`/`RPOP` performance demands.
- **TCP Sockets:** Form the network layer. VeloKV configures non-blocking sockets and monitors their readability and writeability states.
- **I/O Multiplexing (Single-Threaded Event Loop):** Uses `select()` to manage all concurrent client connections, leader handshakes, and replica synchronizations on a single thread. This avoids context-switching, thread creation overhead, and race conditions.
- **Crash-Safe Serialization:** Serializes the database snapshot to a temp file (`.rdb.tmp`) and uses an atomic file rename operation (`std::rename`) to swap it with the main database file (`.rdb`).
- **Replication Manager:** Uses socket promotion (fd stealing) via custom reference releases (`Client::release()`) to move replica connections out of the standard client pool into a dedicated downstream replication pipeline.

---

## Project Structure

```
d:/redis/
├── Makefile                       # Platform-aware compilation instructions
├── velokv_server.exe               # Compiled executable (Windows/Winsock2)
└── src/
    ├── main.cpp                   # Bootstrap, CLI flag parsing, server setup
    ├── common.hpp / .cpp          # OS-specific socket abstractions (Windows/Linux)
    ├── client.hpp / .cpp          # Connection buffers and FD ownership releases
    ├── db.hpp / .cpp              # Database storage engine (Strings, Lists, and TTL maps)
    ├── command.hpp / .cpp         # Command router, write command classifications
    ├── resp.hpp / .cpp            # RESP protocol parser and serialization helper
    ├── persistence.hpp / .cpp     # Human-readable atomic file save/load utilities
    ├── server.hpp / .cpp          # Core socket listener and select() multiplexer loop
    └── replication.hpp / .cpp     # Leader connection pools and replica client managers
```

---

## Build & Run Instructions

### Prerequisites
- A C++17 compliant compiler (`g++` or `clang++`).
- Build system (`make` or `mingw32-make` on Windows).

### Compilation
Build the executable from the project root:
```bash
make clean
make
```

### Running VeloKV

#### 1. Standalone Mode
Start the database server on port 6379:
```bash
./velokv_server --port 6379 --rdb redis.rdb
```

#### 2. Replication Mode
Start a leader server and one or more replica nodes pointing to it:
```bash
# Start Leader (Port 6379)
./velokv_server --role leader --port 6379 --rdb leader.rdb

# Start Replica (Port 6380, connects to Leader)
./velokv_server --role replica --port 6380 --leader-host 127.0.0.1 --leader-port 6379 --rdb replica.rdb
```

---

## Example Usage

Connect to the running database instance using standard tools such as `netcat` (`nc`) or `telnet`:

### Working with Strings & TTL
```bash
$ nc 127.0.0.1 6379
SET username dev_user
+OK
GET username
$8
dev_user
EXPIRE username 10
:1
TTL username
:8
```

### Working with Lists
```bash
$ nc 127.0.0.1 6379
RPUSH queue job_1
:1
RPUSH queue job_2
:2
LPUSH queue job_0
:3
LLEN queue
:3
LPOP queue
$5
job_0
```

---

## Key Learnings & Engineering Takeaways

Building VeloKV offered valuable hands-on experience in low-level systems design and networking:

1. **Bare-Metal Socket Programming:** Writing networking code from scratch using `socket`, `bind`, `listen`, `accept`, `send`, and `recv` solidified a practical understanding of the TCP/IP stack.
2. **I/O Multiplexing & Event Loops:** Implementing a custom server using `select()` demystified how high-concurrency servers handle thousands of concurrent file descriptors without running into thread context-switching bottlenecks.
3. **Memory Ownership & Lifetime:** Implementing features like socket FD promotion (`Client::release()`) required careful management of system resources to prevent leaks, double-closes, and dangling descriptors.
4. **Consistency in Replication:** Designing a master-slave replication pipeline exposed the trade-offs of asynchronous replication, synchronization order, and the complexities of network partitions.
5. **Robust File I/O:** Implementing snapshot-based database saving using the temp-and-rename pattern taught the importance of building fail-safe operations in application storage engines.

---

## Future Improvements

- **Binary-Safe Protocol Parsing:** Upgrade the parser to handle full binary RESP payloads containing null bytes.
- **Append-Only File (AOF):** Support log-based write durability to prevent data loss in the write-windows between periodic snapshot saves.
- **Partial Replication (PSYNC):** Implement a circular replication backlog and global transaction offsets on the leader to perform incremental resynchronizations instead of full dumps on reconnection.
- **Probabilistic TTL Sweeping:** Sample subsets of the TTL database rather than executing complete map scans to bound active eviction overhead to O(1) per tick.
