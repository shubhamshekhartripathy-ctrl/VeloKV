# Implementation Plan — Milestone 4: TCP Server

This milestone documents converting the in-memory Redis clone into a fully operational TCP
server listening on `localhost:6379`. The networking code is strictly separated from the
database, parser, and command-routing layers.

---

## Background

The server is already implemented across Milestones 1–3. This document explains the **full
architecture end-to-end** — including the network primitives, the request lifecycle, and why
each design decision was made — so the codebase is interview-ready.

---

## Architecture Overview

```
┌──────────────────────────────────────────────────────────────┐
│                         main.cpp                             │
│  • WSAStartup / WSACleanup (Windows socket init)             │
│  • SIGINT / SIGTERM signal handlers → server.stop()          │
│  • Calls server.init() then server.start()                   │
└────────────────────────────┬─────────────────────────────────┘
                             │
                             ▼
┌──────────────────────────────────────────────────────────────┐
│                       server.cpp  (Network Layer)            │
│                                                              │
│  socket() → bind() → listen()   [init()]                     │
│  select() event loop             [start()]                   │
│    ├── accept()  → new Client    [handle_new_connection()]   │
│    ├── recv()    → read data     [handle_client_read()]      │
│    │     └─▶ RESPParser::parse()                             │
│    │         └─▶ CommandProcessor::execute()                 │
│    │             └─▶ Client::queue_response()                │
│    └── send()    → flush data    [handle_client_write()]     │
│                                                              │
│  stop():  save DB → close sockets                            │
└──────────────┬───────────────────┬──────────────────────────┘
               │                   │
               ▼                   ▼
┌─────────────────────┐  ┌────────────────────────────────────┐
│    client.cpp       │  │            resp.cpp                │
│  Client (one per    │  │  RESPParser::parse()               │
│  connected socket)  │  │    • Handles RESP array format     │
│  • input_buffer_    │  │      (*3\r\n$3\r\nSET\r\n...)      │
│  • output_buffer_   │  │    • Handles inline text format    │
│  • read_from_socket │  │      (SET key value\n)             │
│  • write_to_socket  │  │  serialize_*() response helpers    │
└─────────────────────┘  └──────────────┬───────────────────-─┘
                                        │
                                        ▼
                         ┌──────────────────────────────────┐
                         │         command.cpp               │
                         │  CommandProcessor                 │
                         │  • Case-insensitive dispatch      │
                         │  • PING, SET, GET, DEL, EXISTS    │
                         │  • EXPIRE, TTL, SAVE              │
                         └───────────────┬──────────────────┘
                                         │
                         ┌───────────────┴──────────────────┐
                         │   db.cpp          persistence.cpp │
                         │  Database         PersistenceManager│
                         │  • store_  map    • save() / load()│
                         │  • expiry_ map    • redis.rdb      │
                         │  • Lazy eviction  • Atomic rename  │
                         └──────────────────────────────────┘
```

---

## Socket API Primer

### `socket(domain, type, protocol)` — Create an endpoint
```cpp
listen_fd_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
```
- `AF_INET` — IPv4 address family.
- `SOCK_STREAM` — reliable, ordered, connection-based byte stream (TCP).
- `IPPROTO_TCP` — explicitly select TCP (already implied by SOCK_STREAM).
- Returns a **file descriptor** — an integer handle the OS uses to track this socket.
- Returns `INVALID_SOCKET` / `-1` on failure.

### `bind(fd, address, addrlen)` — Assign a local address
```cpp
sockaddr_in addr;
addr.sin_family      = AF_INET;
addr.sin_port        = htons(6379);       // host-to-network byte order
addr.sin_addr.s_addr = inet_addr("127.0.0.1");
bind(listen_fd_, (sockaddr*)&addr, sizeof(addr));
```
- Associates the socket with `127.0.0.1:6379`. Without `bind()`, the OS assigns an
  ephemeral port automatically (fine for clients, not for servers).
- `htons()` converts the port from **host byte order** to **network byte order** (big-endian).
  This is required because different CPU architectures store integers differently.

### `listen(fd, backlog)` — Enter passive listening mode
```cpp
listen(listen_fd_, SOMAXCONN);
```
- Transitions the socket from CLOSED → LISTEN state (TCP state machine).
- `SOMAXCONN` tells the OS to use its maximum allowed connection-queue depth
  (typically 128–4096 depending on the OS).
- The **backlog** is the number of connection requests the kernel will queue up while the
  application is busy processing other events. Connections beyond this limit are rejected.

### `accept(fd, addr, addrlen)` — Dequeue one client connection
```cpp
socket_t client_fd = accept(listen_fd_, (sockaddr*)&client_addr, &client_len);
```
- Dequeues the **first** completed TCP handshake from the kernel's accept queue.
- Returns a **brand-new socket descriptor** dedicated to this specific client.
- The listening socket (`listen_fd_`) keeps accepting future clients; it is never used
  directly for data transfer.
- On a non-blocking socket, returns `INVALID_SOCKET` with `WSAEWOULDBLOCK` / `EAGAIN`
  when there are no pending connections — this is not an error.

### `recv(fd, buf, len, flags)` — Read incoming data
```cpp
char buf[4096];
int bytes = recv(client_fd, buf, sizeof(buf), 0);
```
- Copies up to `len` bytes from the kernel's TCP receive buffer into `buf`.
- Returns the **actual number of bytes copied** (may be less than `len` — TCP is a
  stream protocol with no message boundaries).
- Returns `0` if the client closed the connection (graceful FIN).
- Returns `-1` with `EWOULDBLOCK`/`EAGAIN` if no data is available on a non-blocking socket.

### `send(fd, buf, len, flags)` — Write outgoing data
```cpp
int sent = send(client_fd, output_buffer_.data(), output_buffer_.size(), 0);
```
- Copies bytes from our buffer into the kernel's TCP send buffer.
- The kernel is responsible for the actual transmission, retransmission, flow control, etc.
- Returns the number of bytes **accepted by the kernel** (may be less than `len` if the
  kernel's send buffer is full — the remainder must be retried).
- Returns `-1` with `EWOULDBLOCK`/`EAGAIN` when the send buffer is full on a
  non-blocking socket.

---

## TCP vs UDP

| Property | TCP | UDP |
|---|---|---|
| **Connection** | Connection-oriented (3-way handshake) | Connectionless |
| **Delivery** | Guaranteed (retransmit on loss) | Best-effort (packets can be lost) |
| **Ordering** | Guaranteed in-order delivery | No ordering guarantee |
| **Duplicates** | Kernel deduplicates | Application must handle duplicates |
| **Flow control** | Yes (`rwnd` sliding window) | No |
| **Congestion control** | Yes (CUBIC, BBR, etc.) | No |
| **Header overhead** | 20 bytes | 8 bytes |
| **Typical use** | Redis, HTTP, SSH, FTP | DNS, video streaming, QUIC |

**Why Redis uses TCP:**
- A Redis `SET` followed by a `GET` must arrive in that order. UDP has no ordering guarantee.
- If a `GET` response packet is lost, the client must receive the correct data — not silence.
  TCP's retransmission mechanism handles this automatically.
- Redis command pipelining (sending multiple commands before reading replies) relies on the
  stream semantics TCP provides. With UDP, each "message" would need framing, sequencing, and
  acknowledgement logic — essentially re-implementing TCP at the application layer.

---

## Full Request Lifecycle

```
CLIENT                  SERVER (single thread)
  │                          │
  │──── TCP connect ─────────▶  accept() → new Client{fd, buffers}
  │                          │
  │──── "SET x 1\r\n" ───────▶  select() wakes on read event
  │                          │    recv(fd, buf, 4096)
  │                          │    client.input_buffer_ += buf
  │                          │
  │                          │  RESPParser::parse(input_buffer)
  │                          │    • Detects inline format (no '*')
  │                          │    • Splits on whitespace: ["SET","x","1"]
  │                          │    • Erases consumed bytes from input_buffer
  │                          │
  │                          │  CommandProcessor::execute(["SET","x","1"], db)
  │                          │    • to_upper("SET") → finds SET handler
  │                          │    • db.set("x", "1")   O(1)
  │                          │    • db.expiry_.erase("x")  (clear old TTL)
  │                          │    • Returns "+OK\r\n"
  │                          │
  │                          │  client.queue_response("+OK\r\n")
  │                          │  client.write_to_socket()
  │                          │    send(fd, "+OK\r\n", 5)
  │◀──── "+OK\r\n" ──────────│
  │                          │
  │──── "TTL x\r\n" ─────────▶  select() wakes on read event
  │                          │    recv → parse → CommandProcessor
  │                          │    db.ttl("x") → -1 (no TTL)
  │                          │    Returns ":-1\r\n"
  │◀──── ":-1\r\n" ──────────│
  │                          │
  │──── TCP close ────────────▶  recv() returns 0 → disconnect_client(fd)
  │                          │  clients_.erase(fd) → ~Client() → close_socket()
```

---

## File and Class Reference

### `src/common.hpp` / `src/common.cpp` — Platform Abstraction Layer

**Why it exists:** Windows uses `SOCKET` (an unsigned integer) and Winsock2 (`ws2_32.lib`).
POSIX uses `int` file descriptors and POSIX headers. Mixing them causes compile errors.
`common.hpp` defines a single `socket_t` alias and wraps the four platform-specific operations.

| Function | Purpose |
|---|---|
| `initialize_network()` | `WSAStartup(MAKEWORD(2,2))` on Windows; no-op on POSIX |
| `cleanup_network()` | `WSACleanup()` on Windows; no-op on POSIX |
| `close_socket(fd)` | `closesocket(fd)` on Windows; `close(fd)` on POSIX |
| `set_nonblocking(fd)` | `ioctlsocket(fd, FIONBIO, &1)` on Windows; `fcntl(F_SETFL, O_NONBLOCK)` on POSIX |
| `is_would_block()` | Checks `WSAEWOULDBLOCK` / `EAGAIN` / `EWOULDBLOCK` |
| `get_last_error_string()` | `WSAGetLastError()` + `FormatMessage` on Windows; `strerror(errno)` on POSIX |

---

### `src/client.hpp` / `src/client.cpp` — Client Session

```cpp
class Client {
    socket_t fd_;           // OS socket descriptor for this client
    std::string input_buffer_;   // Accumulates partial TCP reads
    std::string output_buffer_;  // Queues unsent response bytes
};
```

**Key design decisions:**

1. **Copy deleted, Move allowed.** A socket descriptor is a unique OS resource —
   copying it would cause double-close bugs. Move semantics are needed because
   `clients_` is an `unordered_map<socket_t, Client>`.

2. **RAII.** `~Client()` always calls `close_socket(fd_)`. The server's `disconnect_client()`
   just calls `clients_.erase(fd)` — the destructor handles cleanup.

3. **`read_from_socket()` partial-read handling.** `recv()` may return fewer bytes
   than available. We call it once per `select()` wakeup and append to `input_buffer_`.
   The parser is then responsible for consuming complete commands and leaving the rest.

4. **`write_to_socket()` partial-send handling.** `send()` may accept fewer bytes than
   we pass (full kernel buffer). We erase only the bytes actually sent. Remaining bytes
   stay in `output_buffer_` and are flushed on the next `select()` write-ready event.

---

### `src/resp.hpp` / `src/resp.cpp` — RESP Parser + Serializer

The **Redis Serialization Protocol (RESP)** is a simple line-based binary-safe protocol.

**RESP data types used in this server:**

| Type | Wire format | Example |
|---|---|---|
| Simple string | `+<text>\r\n` | `+OK\r\n` |
| Error | `-<message>\r\n` | `-ERR unknown command\r\n` |
| Integer | `:<number>\r\n` | `:1\r\n` |
| Bulk string | `$<len>\r\n<data>\r\n` | `$5\r\nhello\r\n` |
| Null bulk string | `$-1\r\n` | (nil) |
| Array | `*<count>\r\n` + N bulk strings | `*3\r\n$3\r\nSET\r\n$1\r\nx\r\n$1\r\n1\r\n` |

**Dual-format parser:** `RESPParser::parse()` inspects the first byte of the buffer:
- `'*'` → RESP array mode (used by `redis-cli`).
- Anything else → inline text mode (used by `telnet`, `netcat`).

The inline parser splits on whitespace and respects double-quoted strings, so
`SET city "New York"` is parsed correctly as three tokens.

**Partial read safety:** The parser returns `INCOMPLETE` when the buffer does not yet
contain a full command. The bytes remain in `input_buffer_` and the next `recv()` will
append more data before the parser tries again.

---

### `src/command.hpp` / `src/command.cpp` — Command Router

```cpp
using CommandHandler = std::function<std::string(
    const std::vector<std::string>& args, Database& db)>;

class CommandProcessor {
    std::unordered_map<std::string, CommandHandler> handlers_;
};
```

**Design decisions:**

1. **`std::function` callbacks.** Handlers are lambdas captured in the constructor.
   Adding a new command is one `register_handler()` call — no switch-case, no enum.

2. **Case-insensitive dispatch.** `to_upper(cmd[0])` before lookup means `SET`, `set`,
   and `SeT` all route to the same handler. Keys are case-sensitive (per Redis spec).

3. **`Database&` by reference.** The processor doesn't own the database; it receives
   a reference. This keeps the two classes independently testable.

**Registered commands:**

| Command | Arity | Returns | Notes |
|---|---|---|---|
| `PING [msg]` | 1–2 | `+PONG` or bulk string | Health check |
| `SET key val` | 3 | `+OK` | Clears existing TTL |
| `GET key` | 2 | Bulk string or `$-1\r\n` | Lazy expiry |
| `DEL key` | 2 | `+OK` | Removes TTL too |
| `EXISTS key` | 2 | `:1` or `:0` | Lazy expiry |
| `EXPIRE key secs` | 3 | `:1` or `:0` | Converts to ms internally |
| `TTL key` | 2 | Seconds remaining, `-1`, or `-2` | Floor division ms→s |
| `SAVE` | 1 | `+OK` | Synchronous snapshot |

---

### `src/db.hpp` / `src/db.cpp` — Storage Engine

```cpp
class Database {
    mutable std::unordered_map<std::string, std::string> store_;  // key → value
    mutable std::unordered_map<std::string, int64_t>     expiry_; // key → Unix-ms deadline
};
```

**Two maps, not one.** Keeping TTL deadlines in a separate `expiry_` map means:
- Keys with no TTL have zero overhead (they simply don't appear in `expiry_`).
- `evict_expired()` only iterates `expiry_` (never the whole `store_`).

**`mutable` for logical const-ness.** `get()`, `exists()`, and `ttl()` are declared `const`
(they don't change observable state). But lazy eviction — erasing an expired key — is a
mutating operation. Marking both maps `mutable` resolves this correctly: the expired key
was already invisible to callers, so erasing it is logically side-effect-free.

**Absolute timestamps.** `expire(key, ttl_ms)` stores `now_ms() + ttl_ms`. The expiry
check is `now_ms() >= stored_deadline` — O(1), always. A relative TTL would require
subtracting elapsed time on every check, which is equivalent.

---

### `src/persistence.hpp` / `src/persistence.cpp` — Snapshot Persistence

**File format:**
```
REDIS-CLONE-RDB v1          ← magic header (format version)
city -1 Rome                ← key=city, no TTL, value=Rome
session 1782229905307 abc   ← key=session, expires at Unix-ms, value=abc
EOF                         ← sentinel (detects truncated files)
```

**Atomic write via temp-file rename:**
1. Write everything to `redis.rdb.tmp`.
2. Flush and close the file.
3. `rename("redis.rdb.tmp", "redis.rdb")` — atomic on POSIX; `remove` + `rename` on Windows.

If the process crashes at step 1, the old `redis.rdb` is intact. If it crashes after
step 3, the new file is complete. There is no window that produces a corrupt file.

**Expired-key filtering on load.** Time keeps ticking while the server is offline.
A key with a 5-minute TTL that was saved 10 minutes ago must be silently discarded
on load, not restored into the database.

---

### `src/server.hpp` / `src/server.cpp` — Event Loop

**`select()` event loop:**
```
loop:
  FD_ZERO / FD_SET all socket fds into read_fds and write_fds
  select(max_fd+1, &read_fds, &write_fds, NULL, &100ms_timeout)
  if listen_fd is readable:  accept() new client
  for each client fd:
    if readable:  recv() → parse → execute → queue response
    if writable:  send() queued response bytes
  every 100 iterations:  db_.evict_expired()
```

**Why `select()` instead of threads?**
- Redis is famous for its single-threaded design. With all data in memory, operations are
  CPU-bound for microseconds — context-switch overhead of threads would dominate.
- Single-threaded avoids all locking on the database (no mutexes, no deadlocks, no races).
- `select()` is portable: it works on both Windows (Winsock) and POSIX without change.
- Limitation: `select()` has an `FD_SETSIZE` cap (typically 1024 on Linux). Production Redis
  uses `epoll` (Linux) or `kqueue` (macOS) for millions of connections.

**Why 100ms `select()` timeout?**
- Lets the loop check `running_` every 100ms for a clean `SIGINT` shutdown.
- Drives the `loop_tick_` counter for active eviction without a separate timer thread.

---

## Verification Plan

### Build
```powershell
cd d:\redis
mingw32-make clean; mingw32-make
```
Expected: zero warnings, zero errors, `redis_server.exe` produced.

### Test with `netcat` (ncat / nc)
```bash
# Terminal 1 — start the server
.\redis_server.exe

# Terminal 2 — connect and send inline commands
ncat localhost 6379
PING
SET city Rome
GET city
EXPIRE city 10
TTL city
DEL city
EXISTS city
SAVE
```

### Test with `telnet`
```bash
telnet localhost 6379
Trying 127.0.0.1...
Connected to localhost.
SET name Alice
+OK
GET name
$5
Alice
```

### Test with `redis-cli` (RESP protocol)
```bash
redis-cli -h 127.0.0.1 -p 6379 PING
redis-cli SET key "hello world"
redis-cli GET key
redis-cli EXPIRE key 5
redis-cli TTL key
```
