# Implementation Plan - Redis-Inspired Key-Value Database (Milestone 1)

This plan outlines the design and implementation for building a Redis-compatible TCP server from scratch in modern C++. It will support connection management, a single-threaded non-blocking event loop using `select()`, a Redis Serialization Protocol (RESP) parser with an inline fallback, and the `PING` command.

## Architecture & Design Decisions

### 1. Single-Threaded Event Loop (`select`)
Redis is famous for its single-threaded, event-driven architecture. To mimic this in a clean, portable, and production-like manner:
- We will configure client sockets and the listener socket to be **non-blocking**.
- We will use the standard `select()` I/O multiplexing function. While `epoll` or `kqueue` are more scalable on Linux/macOS, `select()` is natively available and performs well on Windows (`winsock2.h`) and POSIX. This ensures maximum portability without relying on external libraries.
- The event loop will continuously query `select()` for readability (to accept connections and read requests) and writability (to flush queued response buffers back to clients).

### 2. Platform Agnostic Socket API
We will write a lightweight socket abstraction layer (`src/common.hpp` / `src/common.cpp`) to handle differences between Windows Winsock and POSIX socket APIs. This includes initializing/cleaning up network resources (`WSAStartup`/`WSACleanup` on Windows) and resolving socket handle types (`SOCKET` on Windows vs `int` on UNIX).

### 3. Dual-Format Parser (RESP + Inline Commands)
Standard Redis clients (like `redis-cli`) communicate using the RESP protocol. To make testing user-friendly, the server will support:
1. **RESP Protocol**: e.g., `*1\r\n$4\r\nPING\r\n` (resolves to `PING`).
2. **Inline Protocol**: e.g., `PING\r\n` or `PING\n` (sent by utilities like `nc` or `telnet`).

### 4. Code Organization
The code will be divided into modular components:
- `src/common.hpp`, `src/common.cpp`: Socket utility wrappers and cross-platform setup.
- `src/resp.hpp`, `src/resp.cpp`: Input buffer parsing, extracting commands, and formatting responses.
- `src/command.hpp`, `src/command.cpp`: Registering command handlers and executing them.
- `src/client.hpp`, `src/client.cpp`: Represents a client connection, containing its socket and state buffers.
- `src/server.hpp`, `src/server.cpp`: Handles binding, listening, and running the single-threaded event loop.
- `src/main.cpp`: Entry point.
- `Makefile`: Build automation using GCC/g++.

---

## Folder Structure

```
d:/redis/
├── Makefile
└── src/
    ├── client.cpp
    ├── client.hpp
    ├── common.cpp
    ├── common.hpp
    ├── command.cpp
    ├── command.hpp
    ├── main.cpp
    ├── resp.cpp
    ├── resp.hpp
    ├── server.cpp
    └── server.hpp
```

---

## Proposed Changes

### [Network Utility]
Provides clean helper functions to handle cross-platform Winsock/POSIX setup.

#### [NEW] [common.hpp](file:///d:/redis/src/common.hpp)
Declares types (`socket_t`), macros for socket errors, and wrapper functions like `initialize_network()`, `cleanup_network()`, `close_socket()`, and `set_nonblocking()`.

#### [NEW] [common.cpp](file:///d:/redis/src/common.cpp)
Implements Winsock startup/cleanup, non-blocking socket configuration, and clean error checking.

---

### [RESP Parser]
Handles decoding client input buffers and encoding server replies.

#### [NEW] [resp.hpp](file:///d:/redis/src/resp.hpp)
Defines representation of parsed commands (`std::vector<std::string>`) and declarations for parsing methods and RESP serialization helpers (e.g. `serialize_simple_string()`, `serialize_bulk_string()`).

#### [NEW] [resp.cpp](file:///d:/redis/src/resp.cpp)
Implements RESP syntax scanning (handling arrays `*`, bulk strings `$`, and fallback raw text commands). Handles partial reads cleanly by maintaining an input stream parse-pointer.

---

### [Command Router]
Manages command registration and executes target actions.

#### [NEW] [command.hpp](file:///d:/redis/src/command.hpp)
Defines the `CommandProcessor` registry class.

#### [NEW] [command.cpp](file:///d:/redis/src/command.cpp)
Implements command matching and executes the registry handler. Provides `PING` logic:
- If no args: return standard RESP simple string `+PONG\r\n`.
- If args: return standard RESP bulk string of the first argument (e.g. `PING hello` -> `$5\r\nhello\r\n`).

---

### [Client Connection]
Manages client-specific connection state.

#### [NEW] [client.hpp](file:///d:/redis/src/client.hpp)
Declares the `Client` class containing socket FD, input buffer, and output response buffer.

#### [NEW] [client.cpp](file:///d:/redis/src/client.cpp)
Implements data appending and sending logic for a single socket.

---

### [TCP Server]
Orchestrates binding, listening, and event multiplexing.

#### [NEW] [server.hpp](file:///d:/redis/src/server.hpp)
Declares the `Server` class, socket set managing containers, and `start()` / `run_event_loop()` methods.

#### [NEW] [server.cpp](file:///d:/redis/src/server.cpp)
Implements binding to port 6379, the main `select()` multiplexing loop, client connection acceptance, reading request segments, triggering command execution, and writing output buffers.

---

### [Application Entry]

#### [NEW] [main.cpp](file:///d:/redis/src/main.cpp)
Initializes networking, sets up the server, listens to Ctrl+C or graceful termination, and runs the event loop.

---

### [Build System]

#### [NEW] [Makefile](file:///d:/redis/Makefile)
Automates compilation of all C++ source files into a single binary (`redis_server.exe` on Windows, `redis_server` on UNIX) using `g++` with flags `-std=c++14 -O3 -Wall -Wextra -lws2_32`.

---

## Verification Plan

### Automated Build Verification
1. Compile using GNU Make:
   ```bash
   mingw32-make
   ```
   Ensure it compiles without warnings or errors.

### Manual Verification
1. **Start the Server**:
   Run the compiled executable:
   ```bash
   ./redis_server
   ```
   Verify console output indicating that it is listening on `127.0.0.1:6379`.

2. **Test Inline Clients (telnet/netcat)**:
   Connect using `PowerShell`'s `Test-NetConnection` or basic TCP socket, or `nc localhost 6379`:
   ```bash
   nc localhost 6379
   PING
   ```
   Verify server responds with `+PONG\r\n`.

3. **Test Standard Redis Client (redis-cli)**:
   If `redis-cli` is installed, run:
   ```bash
   redis-cli PING
   ```
   Verify response is `PONG` (with optional arguments like `redis-cli PING "hello world"` showing `"hello world"`).

4. **Multi-client Multiplexing**:
   Open multiple terminal instances, connect to port 6379 simultaneously, and send `PING` commands to verify that the single-threaded server multiplexes connections concurrently.
