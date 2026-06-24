# Walkthrough - Redis-Inspired Key-Value Database (Milestone 1)

Milestone 1 is complete! A lightweight, high-performance, single-threaded TCP server has been built from scratch in modern C++ (C++14). It handles multiple simultaneous TCP connections using standard non-blocking `select()` socket multiplexing and parses commands (supporting both the Redis RESP protocol and an inline text fallback for tools like netcat/telnet) to execute the `PING` command.

---

## Folder Structure

Here is the finalized directory layout:
```
d:/redis/
├── Makefile                # GCC build automation
└── src/
    ├── main.cpp            # Server bootstrap & signal setup
    ├── common.hpp          # Cross-platform socket headers
    ├── common.cpp          # Platform-specific socket wrappers (Winsock/POSIX)
    ├── client.hpp          # Client session buffer declarations
    ├── client.cpp          # Socket reading, writing & connection state management
    ├── resp.hpp            # RESP & inline protocol parsing declarations
    ├── resp.cpp            # Parser implementation and data serializers
    ├── command.hpp         # Command router registry definitions
    ├── command.cpp         # Command execution logic & PING handler
    ├── server.hpp          # TCP Listener and select() multiplexer definitions
    └── server.cpp          # Main event loop and connection acceptance
```

---

## File Roles & Responsibilities

### 1. Platform-Independent Network Utility (`src/common.hpp` / `src/common.cpp`)
- **Responsibility**: Provides socket abstractions and wrapper utilities to mask platform differences between Windows (Winsock `ws2_32`) and POSIX (Unix/Linux/macOS) sockets.
- **Key Details**: Defines portable aliases like `socket_t` and wrappers like `initialize_network()` (which calls `WSAStartup` on Windows), `set_nonblocking()` (using `ioctlsocket` on Windows / `fcntl` on POSIX), and error reporting.

### 2. Client Session Manager (`src/client.hpp` / `src/client.cpp`)
- **Responsibility**: Holds connection state (socket file descriptor) and buffer streams for each active client.
- **Key Details**: Uses RAII to ensure client socket closure on destruction. Maintains an incoming `input_buffer` and queued `output_buffer`. Provides non-blocking socket read/write helpers.

### 3. Protocol Parser (`src/resp.hpp` / `src/resp.cpp`)
- **Responsibility**: Decodes byte arrays in the client's input buffer into commands, and encodes server responses back into Redis Serialization Protocol (RESP) format.
- **Key Details**: 
  - Supports standard **RESP Arrays** (e.g. `*1\r\n$4\r\nPING\r\n`) sent by `redis-cli`.
  - Supports **Inline Fallback** (e.g. `PING hello\n`) sent by standard telnet/netcat client tools.
  - Automatically serializes responses to simple strings (`+PONG\r\n`), bulk strings (`$5\r\nhello\r\n`), and error logs (`-ERR ...\r\n`).

### 4. Command Router (`src/command.hpp` / `src/command.cpp`)
- **Responsibility**: Houses a case-insensitive registered map of command handlers.
- **Key Details**: Evaluates syntax and runs `PING`. If no arguments are passed, it responds with simple string `+PONG\r\n`. If a single argument is passed, it returns that value as a bulk string. If more than 1 argument is passed, it returns a RESP syntax error.

### 5. TCP Event Loop Server (`src/server.hpp` / `src/server.cpp`)
- **Responsibility**: Establishes the TCP socket server on port 6379, listens, and runs a single-threaded non-blocking event-driven loop.
- **Key Details**: Uses a standard `select()` select-set registry to multiplex reads (socket accept and client request reads) and writes (client write flushing) concurrently without spawning threads, matching the core architecture of Redis.

### 6. App Entry (`src/main.cpp`)
- **Responsibility**: Initializes platform networking systems, registers system signal handlers (`SIGINT`/`SIGTERM`) for graceful teardown, instantiates the server, and starts the event loop.

---

## Verification & Tests

### 1. Build Verification
The application compiles cleanly using the provided [Makefile](file:///d:/redis/Makefile) and GNU Make (`mingw32-make`):
```bash
mingw32-make
```
Output:
```
g++ -std=c++14 -O3 -Wall -Wextra -Isrc -c src/main.cpp -o src/main.o
g++ -std=c++14 -O3 -Wall -Wextra -Isrc -o redis_server.exe src/common.o src/resp.o src/command.o src/client.o src/server.o src/main.o -lws2_32
```

### 2. Functional Protocols Test
A custom PowerShell verification script ([test.ps1](file:///C:/Users/ASUS/.gemini/antigravity-ide/brain/5bd94325-dea1-44ab-9ec1-5c7e4f8a73cf/scratch/test.ps1)) was executed against the running database instance to validate inline parsing, RESP arrays, multi-argument syntax validation, and unknown command mapping:

```powershell
powershell -File C:\Users\ASUS\.gemini\antigravity-ide\brain\5bd94325-dea1-44ab-9ec1-5c7e4f8a73cf\scratch\test.ps1
```

**Results**:
- **Inline `PING`**: Received `+PONG` (Simple String)
- **Inline `PING hello`**: Received bulk string `$5` followed by `hello`
- **RESP `*1\r\n$4\r\nPING\r\n`**: Received `+PONG` (Simple String)
- **RESP `*2\r\n$4\r\nPING\r\n$5\r\nworld\r\n`**: Received bulk string `$5` followed by `world`
- **Unknown command `UNKNOWN`**: Received `-ERR unknown command 'UNKNOWN'`

### 3. Graceful Shutdown & Client Drop Handling
- The server logs connection and disconnection events nicely:
  ```
  [Main] Starting Redis-inspired key-value server...
  [Server] Listening on 127.0.0.1:6379
  [Server] Event loop started.
  [Server] Accepted new connection from 127.0.0.1:59603 (fd: 356)
  [Server] Client disconnected (fd: 356)
  ```
- If a client disconnects abruptly, the socket error `WSAEWOULDBLOCK` / `10054` is caught, and resources are closed and cleaned up instantly without interrupting the main event loop.
