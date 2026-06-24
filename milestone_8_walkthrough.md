# Walkthrough — Milestone 8: Leader–Replica Replication

> Saved as: `d:\redis\milestone_8_walkthrough.md`

---

## Build Result

The codebase builds cleanly using the updated C++17 configuration:

```
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Isrc -c src/common.cpp -o src/common.o
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Isrc -c src/db.cpp -o src/db.o
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Isrc -c src/persistence.cpp -o src/persistence.o
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Isrc -c src/resp.cpp -o src/resp.o
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Isrc -c src/command.cpp -o src/command.o
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Isrc -c src/client.cpp -o src/client.o
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Isrc -c src/replication.cpp -o src/replication.o
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Isrc -c src/server.cpp -o src/server.o
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Isrc -c src/main.cpp -o src/main.o
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Isrc -o redis_server.exe src/common.o src/db.o src/persistence.o src/resp.o src/command.o src/client.o src/replication.o src/server.o src/main.o -lws2_32
```

Build diagnostics: **Zero warnings. Zero errors.**

---

## Folder Structure

```
d:/redis/
├── Makefile                       # Builds with src/replication.cpp and -lws2_32
├── redis_server.exe               # Compiled RapidKV binary
├── milestone_8_implementation_plan.md
├── milestone_8_walkthrough.md     # [NEW] This document
└── src/
    ├── main.cpp                   # Parses CLI flags (--role, --leader-host, etc.)
    ├── common.hpp / .cpp          # Non-blocking TCP helpers
    ├── client.hpp / .cpp          # Modified: client.release() steals connection
    ├── db.hpp / .cpp              # Storage engine (strings and lists)
    ├── resp.hpp / .cpp            # RESP serialization / deserialization
    ├── command.hpp / .cpp         # Modified: is_write_command() helper
    ├── persistence.hpp / .cpp     # Snapshot save & load
    ├── server.hpp / .cpp          # Modified: select() loop handles leader/replicas
    └── replication.hpp / .cpp     # [NEW] ReplicationManager, ReplicaConnection
```

---

## Architectural Deep-Dive

Replication in RapidKV is fully **event-driven**, running entirely within the single-threaded `select()` loop. This eliminates the need for locks and prevents any race conditions on the database structures.

### Component Relationship

```
                          ┌──────────────────────────┐
                          │    CommandLine Args      │
                          └────────────┬─────────────┘
                                       ▼
                          ┌──────────────────────────┐
                          │        main.cpp          │
                          └────────────┬─────────────┘
                                       ▼
                          ┌──────────────────────────┐
                          │       Server Class       │
                          └─────┬──────────────┬─────┘
                                │              │
     (If Leader Mode)           ▼              ▼         (If Replica Mode)
     ┌────────────────────────────┐          ┌────────────────────────────┐
     │  ReplicationManager        │          │  ReplicationManager        │
     │  (Manages replicas as      │          │  (Maintains connection to  │
     │   downstream connections)  │          │   upstream leader node)    │
     └────────────────────────────┘          └────────────────────────────┘
```

---

## Explanation of Key Socket APIs Used

To build a high-performance network server, standard BSD/Winsock socket APIs are used:

1. **`socket()`**: Creates a socket descriptor. 
   - *Example:* `socket(AF_INET, SOCK_STREAM, 0)` creates an IPv4 TCP stream socket.
2. **`bind()`**: Associates the socket descriptor with a local IP address and port number.
   - *Example:* Binds to `127.0.0.1:6379` so the OS routes incoming traffic on port 6379 to this process.
3. **`listen()`**: Puts the socket into passive mode to listen for incoming connections.
   - *Example:* `listen(server_fd, SOMAXCONN)` allows the OS to queue pending connections.
4. **`accept()`**: Retrieves the first connection request on the queue, creates a new socket for it, and returns the new file descriptor. The server communicates with this specific client using this new socket, while the original listening socket remains open to accept more connections.
5. **`recv()`**: Receives data from a connected socket.
   - *Example:* `recv(client_fd, buffer, length, 0)` reads bytes sent by the client. Returns the number of bytes read, `0` if the connection was closed gracefully, or `-1` on error.
6. **`send()`**: Transmits data over a connected socket.
   - *Example:* `send(client_fd, response, length, 0)` pushes bytes back to the client.

---

## TCP vs. UDP: Why TCP is Used for Replication

Replication systems require a strict protocol with specific transport guarantees. Let's compare TCP and UDP:

| Feature | TCP (Transmission Control Protocol) | UDP (User Datagram Protocol) |
| :--- | :--- | :--- |
| **Connection State** | Connection-oriented (handshake required) | Connectionless |
| **Reliability** | Guaranteed delivery (retransmits lost packets) | Unreliable (packets can be lost) |
| **Ordering** | In-order delivery | Packets can arrive out of order |
| **Flow Control** | Yes (prevents sender from overwhelming receiver) | No |
| **Overhead** | Higher (header size, state machine, ACKs) | Lower (thin header, fire-and-forget) |

### Why TCP is Critical for RapidKV Replication:
1. **Zero Data Loss:** We cannot afford to lose a single write command (`SET`, `DEL`), or the replica database will drift from the leader.
2. **Strict Ordering:** If the leader executes `SET key val1` followed by `SET key val2`, the replica must execute them in that exact order. UDP does not guarantee ordering, which would lead to unpredictable replica states.
3. **Stream-based:** Handshaking and full-sync transmission require a reliable byte stream.

---

## Full Request & Replication Lifecycle

Here is the path a write command takes from the client, through the leader, and down to the replicas:

```
[Client]                   [Leader Server]                 [Replica Server]
   │                             │                                │
   │  SET key value              │                                │
   ├────────────────────────────►│                                │
   │                             │ (Reads socket, parses RESP)    │
   │                             │ (Executes write on local DB)   │
   │                             │                                │
   │                             │ (Encodes write for replication)│
   │                             │ (Queues to Replica output buf) │
   │                             │                                │
   │                             │  SET key value\r\n             │
   │                             ├───────────────────────────────►│
   │                             │                                │ (Reads replication socket)
   │                             │                                │ (Applies SET to local DB)
   │     +OK\r\n                 │                                │
   │◄────────────────────────────┤                                │
```

---

## File-by-File & Class-by-Class Map

### 1. `src/replication.hpp` & `src/replication.cpp`
- **`NodeRole` (Enum):** Defines the role of the current node (`STANDALONE`, `LEADER`, `REPLICA`).
- **`ReplicaConnection` (Class):** Represents a replica socket connection on the leader side. Maintains a write-buffer (`output_buffer`) to queue replication updates asynchronously.
- **`ReplicationManager` (Class):** 
  - **On Leader:** Manages multiple `ReplicaConnection` instances. Encodes database snapshots into `+FULLSYNC` payloads. Serializes write commands and queues them for propagation.
  - **On Replica:** Manages the upstream socket connection to the leader. Handles the handshake (`REPLICAOF`), parses incoming commands (`+FULLSYNC`, `REPLSET`, `REPLLIST`, `+FULLSYNCDONE`), and triggers reconnect attempts with an exponential/5-second backoff when the leader disconnects.

### 2. `src/client.hpp` & `src/client.cpp`
- **`Client::release()`:** Essential for connection promotion. When a client sends a `REPLICAOF` handshake, we steal the file descriptor from the `Client` object so the socket is not closed when the client object is destructed.

### 3. `src/command.hpp` & `src/command.cpp`
- **`is_write_command()`:** Inspects command names and flags write operations (`SET`, `DEL`, `EXPIRE`, `LPUSH`, `RPUSH`, `LPOP`, `RPOP`). Replicas intercept these commands and reject them with a `-READONLY` RESP error.

### 4. `src/server.hpp` & `src/server.cpp`
- **`Server` Select Loop:** Integrates replication sockets into the main multiplexed event loop. Replicas are polled for readability and writeability.
- **Handshake Interception:** Detects raw inline `REPLICAOF` commands sent by replicas, promotes the connection, and registers it with `ReplicationManager`.

### 5. `src/main.cpp`
- Parses node startup configuration flags:
  - `--role <leader|replica>`
  - `--leader-host <IP>`
  - `--leader-port <Port>`
  - `--port <Port>`
  - `--rdb <Filename>`

---

## Test Verification

All 22 integration tests written in [test_milestone_8.ps1](file:///C:/Users/ASUS/.gemini/antigravity-ide/brain/65d61c10-25c4-4379-ac1a-c0888c13b772/scratch/test_milestone_8.ps1) passed successfully.

### Test Results

```
=== Section 1: Basic SET/DEL propagation ===
  [PASS] T01 SET propagated: GET city on replica
  [PASS] T02 SET overwrite propagated
  [PASS] T03 DEL propagated: GET city -> nil
  [PASS] T04 Write to replica -> READONLY
  [PASS] T05 EXISTS on replica after SET on leader
  [PASS] T06 Full-sync: pre-existing key on replica

=== Section 2: READONLY enforcement ===
  [PASS] T07 DEL on replica -> READONLY
  [PASS] T08 EXPIRE on replica -> READONLY
  [PASS] T09 GET on replica still works

=== Section 3: List propagation (LPUSH/RPUSH/LPOP) ===
  [PASS] T10 RPUSH x3 propagated: LLEN=3 on replica
  [PASS] T11 LPUSH propagated: LLEN=4 on replica
  [PASS] T12 LPOP propagated: LLEN=3 on replica
  [PASS] T13 RPOP propagated: LLEN=2 on replica
  [PASS] T14 LPUSH on replica -> READONLY

=== Section 4: TTL propagation ===
  [PASS] T15 EXPIRE propagated: TTL on replica > 0
  [PASS] T16 TTL key exists on replica

=== Section 5: Reconnection ===
  [PASS] T17 Replica synced before leader restart
  [PASS] T18 Replica serves reads when leader is down
  [PASS] T19 Propagation resumes after reconnect

=== Section 6: Standalone backward compat ===
  [PASS] T20 Standalone SET/GET works
  [PASS] T21 Standalone PING
  [PASS] T22 Standalone write not blocked

=======================================
 Results: 22 passed, 0 failed
=======================================
 ALL TESTS PASSED
```

---

## How to Test Manually Using Netcat or Telnet

### 1. Start the Nodes
Open two terminal windows:

*   **Leader on port 6379:**
    ```bash
    .\redis_server.exe --role leader --port 6379 --rdb leader.rdb
    ```
*   **Replica on port 6380 (pointing to leader):**
    ```bash
    .\redis_server.exe --role replica --port 6380 --leader-host 127.0.0.1 --leader-port 6379 --rdb replica.rdb
    ```

### 2. Verify Client Write & Read
Open a third terminal window to connect to the nodes:

*   **Connect to Replica (Port 6380):**
    ```bash
    nc 127.0.0.1 6380
    ```
    *Send command:*
    ```
    GET mykey
    ```
    *Result:* `$-1` (key doesn't exist yet)

*   **Connect to Leader (Port 6379) and Write:**
    ```bash
    nc 127.0.0.1 6379
    ```
    *Send command:*
    ```
    SET mykey "hello-replication"
    ```
    *Result:* `+OK`

*   **Check Replica again:**
    ```
    GET mykey
    ```
    *Result:* `$17\r\nhello-replication` (data successfully propagated! ✅)

---

## Interview Questions & Answers

### Q1: What is event-driven socket multiplexing, and why is `select()` used instead of spawning a thread per connection?
**A:** Spawning a thread per connection incurs high resource overhead (memory for stack space, context-switching overhead). Event multiplexing using `select()` (or `epoll`/`kqueue` on POSIX systems) allows a single thread to monitor multiple file descriptors to see if they are ready for read/write. This keeps the application single-threaded, avoiding race conditions and complex mutex locking logic entirely.

### Q2: Why must replication commands be queued in an output buffer instead of calling `send()` directly in the propagation logic?
**A:** The socket might not be ready to accept data immediately (due to TCP congestion control or slow networks). Calling a blocking `send()` inline would stall the entire server loop, hurting responsiveness for all other clients. By queuing commands to an `output_buffer`, we push the data asynchronously in the background whenever `select()` indicates the replica socket is writable.

### Q3: What is the risk of utilizing absolute deadlines (`system_clock`) vs relative deadlines (`steady_clock`) in replication?
**A:** If we propagate absolute Unix timestamps, clock drift between the leader and replica servers can cause keys to expire prematurely on one node or live too long on another. However, absolute deadlines are stable across restarts. Relative times (propagating `EXPIRE key 10`) don't suffer from clock synchronization drift, but network delay can cause the replica's countdown to start later, keeping the key alive slightly longer. Production Redis uses absolute deadlines (`EXPIREAT`) but relies on NTP to synchronize server clocks.

### Q4: How is a replica's connection promoted on the leader side from a standard client?
**A:** When the replica first connects, it acts like a normal client. When the leader parses its first message and detects `REPLICAOF self self`, it removes the `Client` container from its client map. To prevent the destructor `~Client()` from automatically closing the socket, we call `client.release()`, which sets the object's internal socket descriptor to an invalid state, leaving the OS socket descriptor open. The leader then passes this descriptor to the `ReplicationManager`.

### Q5: What happens if a replica fails to write to its local database during replication?
**A:** In a leader-replica model, the replica should never fail to write unless it runs out of memory or disk space. If a write fails on a replica but succeeded on the leader, the data will become inconsistent. Replicas are designed to crash or halt if their storage engine experiences fatal write errors, forcing manual or automatic failover.
