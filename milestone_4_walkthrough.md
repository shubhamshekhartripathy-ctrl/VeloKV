# Walkthrough — Milestone 4: TCP Server

The codebase is a fully operational single-threaded TCP server implementing a Redis-compatible
protocol on `localhost:6379`. This document presents the complete code for every file,
testing instructions, and comprehensive interview Q&A.

---

## Folder Structure

```
d:/redis/
├── Makefile
├── redis.rdb                     ← snapshot persistence file (created at runtime)
└── src/
    ├── main.cpp                  ← entry point, signal handling
    ├── common.hpp / common.cpp   ← cross-platform socket abstractions
    ├── client.hpp / client.cpp   ← per-connection buffers, RAII socket lifetime
    ├── resp.hpp   / resp.cpp     ← RESP + inline parser, response serializers
    ├── command.hpp/ command.cpp  ← command handler registry (SET, GET, TTL, …)
    ├── db.hpp     / db.cpp       ← in-memory store + lazy/active TTL eviction
    ├── persistence.hpp/.cpp      ← atomic snapshot save/load (redis.rdb)
    └── server.hpp / server.cpp   ← select() event loop, accept/recv/send
```

---

## Complete Source Code

### `src/common.hpp`

```cpp
#pragma once

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    using socket_t = SOCKET;
    #define INVALID_SOCKET_VAL INVALID_SOCKET
    #define SOCKET_ERROR_VAL   SOCKET_ERROR
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <arpa/inet.h>
    using socket_t = int;
    #define INVALID_SOCKET_VAL -1
    #define SOCKET_ERROR_VAL   -1
#endif

#include <string>

namespace redis {
    bool        initialize_network();
    void        cleanup_network();
    bool        close_socket(socket_t fd);
    bool        set_nonblocking(socket_t fd);
    bool        is_would_block();
    std::string get_last_error_string();
} // namespace redis
```

---

### `src/common.cpp`

```cpp
#include "common.hpp"

#ifdef _WIN32
    #include <sstream>
#else
    #include <unistd.h>
    #include <fcntl.h>
    #include <cstring>
    #include <cerrno>
#endif

namespace redis {

bool initialize_network() {
#ifdef _WIN32
    WSADATA wd;
    return WSAStartup(MAKEWORD(2, 2), &wd) == 0;
#else
    return true;
#endif
}

void cleanup_network() {
#ifdef _WIN32
    WSACleanup();
#endif
}

bool close_socket(socket_t fd) {
    if (fd == INVALID_SOCKET_VAL) return true;
#ifdef _WIN32
    return closesocket(fd) == 0;
#else
    return close(fd) == 0;
#endif
}

bool set_nonblocking(socket_t fd) {
    if (fd == INVALID_SOCKET_VAL) return false;
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool is_would_block() {
#ifdef _WIN32
    int e = WSAGetLastError();
    return (e == WSAEWOULDBLOCK || e == WSAEINPROGRESS);
#else
    return (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS);
#endif
}

std::string get_last_error_string() {
#ifdef _WIN32
    int err = WSAGetLastError();
    wchar_t* s = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                   reinterpret_cast<LPWSTR>(&s), 0, nullptr);
    if (s) {
        std::wstring ws(s);
        std::string  str(ws.begin(), ws.end());
        LocalFree(s);
        while (!str.empty() && (str.back() == '\n' || str.back() == '\r'))
            str.pop_back();
        return "Socket error " + std::to_string(err) + ": " + str;
    }
    return "Socket error " + std::to_string(err);
#else
    return std::string(strerror(errno));
#endif
}

} // namespace redis
```

---

### `src/client.hpp`

```cpp
#pragma once

#include "common.hpp"
#include <string>

namespace redis {

// Client encapsulates the state of one TCP connection.
// Owns the socket fd via RAII — closing happens in the destructor.
class Client {
public:
    explicit Client(socket_t fd);
    ~Client();                              // Closes socket

    // Non-copyable (socket is a unique resource)
    Client(const Client&)            = delete;
    Client& operator=(const Client&) = delete;

    // Movable (needed for unordered_map storage)
    Client(Client&&) noexcept;
    Client& operator=(Client&&) noexcept;

    socket_t    fd()                const { return fd_; }
    bool        has_pending_write() const { return !output_buffer_.empty(); }
    std::string& input_buffer()           { return input_buffer_; }

    bool read_from_socket();        // recv() → input_buffer_
    bool write_to_socket();         // output_buffer_ → send()
    void queue_response(const std::string& data);

private:
    socket_t    fd_;
    std::string input_buffer_;     // accumulates partial TCP reads
    std::string output_buffer_;    // queued bytes waiting to be sent
};

} // namespace redis
```

---

### `src/client.cpp`

```cpp
#include "client.hpp"
#include <iostream>

namespace redis {

Client::Client(socket_t fd) : fd_(fd) {}

Client::~Client() {
    if (fd_ != INVALID_SOCKET_VAL) close_socket(fd_);
}

Client::Client(Client&& o) noexcept
    : fd_(o.fd_), input_buffer_(std::move(o.input_buffer_)),
      output_buffer_(std::move(o.output_buffer_)) {
    o.fd_ = INVALID_SOCKET_VAL;
}

Client& Client::operator=(Client&& o) noexcept {
    if (this != &o) {
        if (fd_ != INVALID_SOCKET_VAL) close_socket(fd_);
        fd_            = o.fd_;
        input_buffer_  = std::move(o.input_buffer_);
        output_buffer_ = std::move(o.output_buffer_);
        o.fd_          = INVALID_SOCKET_VAL;
    }
    return *this;
}

bool Client::read_from_socket() {
    char buf[4096];
#ifdef _WIN32
    int n = recv(fd_, buf, sizeof(buf), 0);
#else
    ssize_t n = recv(fd_, buf, sizeof(buf), 0);
#endif
    if (n > 0) { input_buffer_.append(buf, n); return true; }
    if (n == 0) return false;                       // graceful close
    if (is_would_block()) return true;              // no data yet, try later
    std::cerr << "[Client] recv error: " << get_last_error_string() << "\n";
    return false;
}

bool Client::write_to_socket() {
    if (output_buffer_.empty()) return true;
#ifdef _WIN32
    int n = send(fd_, output_buffer_.data(), (int)output_buffer_.size(), 0);
#else
    ssize_t n = send(fd_, output_buffer_.data(), output_buffer_.size(), 0);
#endif
    if (n > 0) { output_buffer_.erase(0, n); return true; }
    if (n == 0) return false;
    if (is_would_block()) return true;              // kernel buffer full, retry later
    std::cerr << "[Client] send error: " << get_last_error_string() << "\n";
    return false;
}

void Client::queue_response(const std::string& data) {
    output_buffer_ += data;
}

} // namespace redis
```

---

### `src/resp.hpp`

```cpp
#pragma once

#include <string>
#include <vector>

namespace redis {

using Command = std::vector<std::string>;

class RESPParser {
public:
    // Parse all complete commands from input_buffer.
    // Consumed bytes are erased from input_buffer.
    // Returns true if at least one command was extracted.
    static bool parse(std::string& input_buffer, std::vector<Command>& out);

    // Response serializers
    static std::string serialize_simple_string(const std::string& s);
    static std::string serialize_error(const std::string& s);
    static std::string serialize_bulk_string(const std::string& s);
    static std::string serialize_null_bulk_string();
    static std::string serialize_integer(long long v);
    static std::string serialize_array(const std::vector<std::string>& elems);
};

} // namespace redis
```

---

### `src/resp.cpp`

```cpp
#include "resp.hpp"
#include <iostream>

namespace redis {

enum class ParseResult { SUCCESS, INCOMPLETE, ERROR };

static ParseResult parse_one(const std::string& buf, size_t& offset, Command& out) {
    if (offset >= buf.size()) return ParseResult::INCOMPLETE;

    if (buf[offset] == '*') {
        // ── RESP Array ────────────────────────────────────────────────────
        size_t crlf = buf.find("\r\n", offset);
        if (crlf == std::string::npos) return ParseResult::INCOMPLETE;

        int count = 0;
        try { count = std::stoi(buf.substr(offset + 1, crlf - offset - 1)); }
        catch (...) { return ParseResult::ERROR; }
        if (count < 0) { offset = crlf + 2; return ParseResult::SUCCESS; }

        size_t pos = crlf + 2;
        Command cmd;
        cmd.reserve(count);

        for (int i = 0; i < count; ++i) {
            if (pos >= buf.size() || buf[pos] != '$') return ParseResult::INCOMPLETE;
            size_t bcrlf = buf.find("\r\n", pos);
            if (bcrlf == std::string::npos) return ParseResult::INCOMPLETE;

            int len = 0;
            try { len = std::stoi(buf.substr(pos + 1, bcrlf - pos - 1)); }
            catch (...) { return ParseResult::ERROR; }

            if (len < 0) { cmd.push_back(""); pos = bcrlf + 2; continue; }

            size_t start = bcrlf + 2;
            if (start + (size_t)len + 2 > buf.size()) return ParseResult::INCOMPLETE;
            cmd.push_back(buf.substr(start, len));
            pos = start + len + 2;
        }
        out    = std::move(cmd);
        offset = pos;
        return ParseResult::SUCCESS;

    } else {
        // ── Inline (netcat / telnet) ──────────────────────────────────────
        size_t lf = buf.find('\n', offset);
        if (lf == std::string::npos) return ParseResult::INCOMPLETE;

        std::string line = buf.substr(offset, lf - offset);
        if (!line.empty() && line.back() == '\r') line.pop_back();

        Command cmd;
        std::string token;
        bool in_quotes = false;
        for (char c : line) {
            if      (c == '"')            { in_quotes = !in_quotes; }
            else if (c == ' ' && !in_quotes) { if (!token.empty()) { cmd.push_back(token); token.clear(); } }
            else                          { token += c; }
        }
        if (!token.empty()) cmd.push_back(token);
        if (!cmd.empty()) out = std::move(cmd);
        offset = lf + 1;
        return ParseResult::SUCCESS;
    }
}

bool RESPParser::parse(std::string& buf, std::vector<Command>& out) {
    size_t offset = 0;
    bool   any    = false;

    while (offset < buf.size()) {
        Command cmd;
        size_t  tmp = offset;
        auto    res = parse_one(buf, tmp, cmd);

        if (res == ParseResult::SUCCESS) {
            if (!cmd.empty()) { out.push_back(std::move(cmd)); any = true; }
            offset = tmp;
        } else if (res == ParseResult::INCOMPLETE) {
            break;
        } else {
            std::cerr << "[RESP] Protocol error — clearing buffer\n";
            buf.clear();
            return any;
        }
    }
    if (offset > 0) buf.erase(0, offset);
    return any;
}

std::string RESPParser::serialize_simple_string(const std::string& s) { return "+" + s + "\r\n"; }
std::string RESPParser::serialize_error(const std::string& s)         { return "-" + s + "\r\n"; }
std::string RESPParser::serialize_null_bulk_string()                   { return "$-1\r\n"; }
std::string RESPParser::serialize_integer(long long v)                 { return ":" + std::to_string(v) + "\r\n"; }
std::string RESPParser::serialize_bulk_string(const std::string& s)   {
    return "$" + std::to_string(s.size()) + "\r\n" + s + "\r\n";
}
std::string RESPParser::serialize_array(const std::vector<std::string>& elems) {
    std::string r = "*" + std::to_string(elems.size()) + "\r\n";
    for (const auto& e : elems) r += serialize_bulk_string(e);
    return r;
}

} // namespace redis
```

---

### `src/command.hpp`

```cpp
#pragma once

#include "db.hpp"
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace redis {

using CommandHandler = std::function<std::string(
    const std::vector<std::string>& args, Database& db)>;

class CommandProcessor {
public:
    CommandProcessor();

    std::string execute(const std::vector<std::string>& cmd, Database& db);
    void register_handler(const std::string& name, CommandHandler handler);

private:
    std::unordered_map<std::string, CommandHandler> handlers_;
    void register_builtin_commands();
};

} // namespace redis
```

---

### `src/command.cpp`

```cpp
#include "command.hpp"
#include "persistence.hpp"
#include "resp.hpp"
#include <algorithm>
#include <cctype>

namespace redis {

static std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::toupper(c); });
    return s;
}

CommandProcessor::CommandProcessor() { register_builtin_commands(); }

void CommandProcessor::register_handler(const std::string& name, CommandHandler h) {
    handlers_[to_upper(name)] = std::move(h);
}

std::string CommandProcessor::execute(const std::vector<std::string>& cmd, Database& db) {
    if (cmd.empty()) return "";
    auto it = handlers_.find(to_upper(cmd[0]));
    if (it == handlers_.end())
        return RESPParser::serialize_error("ERR unknown command '" + cmd[0] + "'");
    return it->second(cmd, db);
}

void CommandProcessor::register_builtin_commands() {

    // PING
    register_handler("PING", [](const auto& args, Database&) -> std::string {
        if (args.size() == 1) return RESPParser::serialize_simple_string("PONG");
        if (args.size() == 2) return RESPParser::serialize_bulk_string(args[1]);
        return RESPParser::serialize_error("ERR wrong number of arguments for 'ping' command");
    });

    // SET key value
    register_handler("SET", [](const auto& args, Database& db) -> std::string {
        if (args.size() != 3)
            return RESPParser::serialize_error("ERR wrong number of arguments for 'set' command");
        db.set(args[1], args[2]);
        return RESPParser::serialize_simple_string("OK");
    });

    // GET key
    register_handler("GET", [](const auto& args, Database& db) -> std::string {
        if (args.size() != 2)
            return RESPParser::serialize_error("ERR wrong number of arguments for 'get' command");
        std::string val;
        return db.get(args[1], val) ? RESPParser::serialize_bulk_string(val)
                                    : RESPParser::serialize_null_bulk_string();
    });

    // DEL key
    register_handler("DEL", [](const auto& args, Database& db) -> std::string {
        if (args.size() != 2)
            return RESPParser::serialize_error("ERR wrong number of arguments for 'del' command");
        db.del(args[1]);
        return RESPParser::serialize_simple_string("OK");
    });

    // EXISTS key
    register_handler("EXISTS", [](const auto& args, Database& db) -> std::string {
        if (args.size() != 2)
            return RESPParser::serialize_error("ERR wrong number of arguments for 'exists' command");
        return RESPParser::serialize_integer(db.exists(args[1]) ? 1 : 0);
    });

    // EXPIRE key seconds
    register_handler("EXPIRE", [](const auto& args, Database& db) -> std::string {
        if (args.size() != 3)
            return RESPParser::serialize_error("ERR wrong number of arguments for 'expire' command");
        int64_t s = 0;
        try { s = std::stoll(args[2]); } catch (...) {
            return RESPParser::serialize_error("ERR value is not an integer or out of range");
        }
        if (s < 0)
            return RESPParser::serialize_error("ERR invalid expire time in 'expire' command");
        return RESPParser::serialize_integer(db.expire(args[1], s * 1000LL) ? 1 : 0);
    });

    // TTL key
    register_handler("TTL", [](const auto& args, Database& db) -> std::string {
        if (args.size() != 2)
            return RESPParser::serialize_error("ERR wrong number of arguments for 'ttl' command");
        int64_t ms = db.ttl(args[1]);
        if (ms == -1) return RESPParser::serialize_integer(-1);
        if (ms == -2) return RESPParser::serialize_integer(-2);
        return RESPParser::serialize_integer(ms / 1000LL);
    });

    // SAVE
    static constexpr const char* kDbPath = "redis.rdb";
    register_handler("SAVE", [](const auto& args, Database& db) -> std::string {
        if (args.size() != 1)
            return RESPParser::serialize_error("ERR wrong number of arguments for 'save' command");
        return PersistenceManager::save(db, kDbPath)
               ? RESPParser::serialize_simple_string("OK")
               : RESPParser::serialize_error("ERR failed to save database");
    });
}

} // namespace redis
```

---

### `src/db.hpp` (key declarations)

```cpp
#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <cstdint>

namespace redis {

struct SnapshotEntry {
    std::string key, value;
    int64_t     expiry_ms;   // Unix-ms absolute deadline, or -1
};

class Database {
public:
    Database()  = default;
    ~Database() = default;
    Database(const Database&)            = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&&) noexcept        = default;
    Database& operator=(Database&&) noexcept = default;

    // CRUD
    void    set(const std::string& key, const std::string& value);
    bool    get(const std::string& key, std::string& out)  const;
    bool    del(const std::string& key);
    bool    exists(const std::string& key)                 const;

    // TTL
    bool    expire(const std::string& key, int64_t ttl_ms);
    void    expire_at(const std::string& key, int64_t abs_ms);
    int64_t ttl(const std::string& key)                    const;

    // Persistence
    std::vector<SnapshotEntry> snapshot() const;

    // Active eviction (called from event loop)
    void evict_expired();

private:
    mutable std::unordered_map<std::string, std::string> store_;
    mutable std::unordered_map<std::string, int64_t>     expiry_;

    bool           is_expired(const std::string& key) const;
    static int64_t now_ms() noexcept;
};

} // namespace redis
```

---

### `src/server.hpp`

```cpp
#pragma once

#include "common.hpp"
#include "client.hpp"
#include "command.hpp"
#include "db.hpp"
#include "persistence.hpp"
#include <string>
#include <unordered_map>
#include <atomic>
#include <cstdint>

namespace redis {

class Server {
public:
    Server(const std::string& host, int port);
    ~Server();

    Server(const Server&)            = delete;
    Server& operator=(const Server&) = delete;

    bool init();    // socket() → bind() → listen() → load DB
    void start();   // select() event loop (blocks)
    void stop();    // signal loop to exit, save DB

private:
    std::string       host_;
    int               port_;
    socket_t          listen_fd_;
    std::atomic<bool> running_;

    Database                                  db_;
    CommandProcessor                          processor_;
    std::unordered_map<socket_t, Client>      clients_;

    static constexpr const char*   kDbPath          = "redis.rdb";
    static constexpr uint64_t      kEvictionInterval = 100; // loop ticks
    uint64_t                       loop_tick_        = 0;

    void handle_new_connection();
    void handle_client_read(socket_t fd);
    void handle_client_write(socket_t fd);
    void disconnect_client(socket_t fd);
};

} // namespace redis
```

---

### `src/main.cpp`

```cpp
#include "common.hpp"
#include "server.hpp"
#include <iostream>
#include <signal.h>

namespace { redis::Server* g_server = nullptr; }

void handle_signal(int sig) {
    std::cout << "\n[Main] Signal " << sig << " received — stopping...\n";
    if (g_server) g_server->stop();
}

int main() {
    std::cout << "[Main] Starting Redis clone on 127.0.0.1:6379\n";

    if (!redis::initialize_network()) {
        std::cerr << "[Main] Failed to initialize networking\n";
        return 1;
    }

    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    redis::Server server("127.0.0.1", 6379);
    g_server = &server;

    if (server.init()) {
        server.start();
    } else {
        std::cerr << "[Main] Server initialization failed\n";
    }

    g_server = nullptr;
    redis::cleanup_network();
    std::cout << "[Main] Shutdown complete.\n";
    return 0;
}
```

---

### `Makefile`

```makefile
CXX      = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -Wpedantic -Isrc

ifeq ($(OS),Windows_NT)
    LIBS      = -lws2_32
    TARGET    = redis_server.exe
    CLEAN_CMD = del /q /f src\*.o $(TARGET) redis.rdb.tmp 2>nul || exit 0
else
    LIBS      =
    TARGET    = redis_server
    CLEAN_CMD = rm -f src/*.o $(TARGET) redis.rdb.tmp
endif

SRCS = src/common.cpp     \
       src/db.cpp         \
       src/persistence.cpp \
       src/resp.cpp       \
       src/command.cpp    \
       src/client.cpp     \
       src/server.cpp     \
       src/main.cpp

OBJS = $(SRCS:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(OBJS) $(LIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	$(CLEAN_CMD)

.PHONY: all clean
```

---

## Build Output

```
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Isrc -c src/common.cpp      -o src/common.o
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Isrc -c src/db.cpp          -o src/db.o
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Isrc -c src/persistence.cpp -o src/persistence.o
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Isrc -c src/resp.cpp        -o src/resp.o
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Isrc -c src/command.cpp     -o src/command.o
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Isrc -c src/client.cpp      -o src/client.o
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Isrc -c src/server.cpp      -o src/server.o
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Isrc -c src/main.cpp        -o src/main.o
g++ -std=c++17 -O2 ... -o redis_server.exe ... -lws2_32
Zero warnings. Zero errors.
```

---

## Testing with Netcat / Telnet

### Using `ncat` (netcat)

```bash
# Terminal 1 — start the server
.\redis_server.exe
# Output:
# [Persistence] Loaded 5 key(s) from 'redis.rdb'.
# [Server] Listening on 127.0.0.1:6379
# [Server] Event loop started.

# Terminal 2 — connect (ncat comes with Nmap)
ncat localhost 6379
```

Type these inline commands and press Enter after each:

```
PING
+PONG

SET name Alice
+OK

GET name
$5
Alice

EXPIRE name 30
:1

TTL name
:29

SET counter 0
+OK

EXISTS counter
:1

DEL counter
+OK

EXISTS counter
:0

SAVE
+OK
```

### Using `telnet`

```bash
telnet 127.0.0.1 6379
Trying 127.0.0.1...
Connected to 127.0.0.1.
Escape character is '^]'.

SET city Rome
+OK

GET city
$4
Rome

TTL city
:-1

EXPIRE city 5
:1

TTL city
:4
```

### Using `redis-cli` (RESP protocol, most realistic test)

```bash
redis-cli -h 127.0.0.1 -p 6379 PING
# PONG

redis-cli SET greeting "hello world"
# OK

redis-cli GET greeting
# "hello world"

redis-cli EXPIRE greeting 60
# (integer) 1

redis-cli TTL greeting
# (integer) 59

redis-cli EXISTS greeting
# (integer) 1

redis-cli DEL greeting
# OK

redis-cli EXISTS greeting
# (integer) 0
```

### Persistence Test (server restart)

```bash
# 1. Start server and write data
redis-cli SET session_key "user123"
redis-cli EXPIRE session_key 300
redis-cli SET permanent "no-ttl"

# 2. Stop server (Ctrl+C)
# Output: [Persistence] Saved 2 key(s) to 'redis.rdb'.

# 3. Start server again
.\redis_server.exe
# Output: [Persistence] Loaded 2 key(s) from 'redis.rdb'.

# 4. Verify
redis-cli GET session_key      # "user123" — persisted!
redis-cli TTL session_key      # ~299 — TTL survived restart!
redis-cli GET permanent        # "no-ttl" — persisted!
```

---

## Server Startup Console Log

```
[Main] Starting Redis clone on 127.0.0.1:6379
[Persistence] Loaded 2 key(s) from 'redis.rdb'.
[Server] Listening on 127.0.0.1:6379
[Server] Event loop started.
[Server] New connection from 127.0.0.1:50234 (fd: 8)
[Server] Client disconnected (fd: 8)
^C
[Main] Signal 2 received — stopping...
[Persistence] Saved 2 key(s) to 'redis.rdb'.
[Server] Event loop stopped.
[Main] Shutdown complete.
```

---

## Interview Questions & Answers

### Q1: What is the difference between TCP and UDP? Why does Redis use TCP?

**A:** TCP is connection-oriented, guarantees ordered delivery, handles retransmission, and
provides flow and congestion control. UDP is connectionless, has no delivery guarantee, and
delivers datagrams out of order or not at all.

Redis uses TCP because:
1. **Ordering** — a `SET` must be processed before the subsequent `GET`.
2. **Reliability** — a dropped `GET` response must be retransmitted; TCP handles this.
3. **Streaming** — clients pipeline many commands without waiting for each reply.
   TCP's stream model handles arbitrary pipelining naturally.

Using UDP would require re-implementing ordering, acknowledgement, and retransmission at the
application layer — essentially building a worse version of TCP.

---

### Q2: Explain the full path of a `SET x 1` command through the server.

**A:**
1. `select()` fires a read event on the client's socket fd.
2. `recv(fd, buf, 4096)` copies bytes from the kernel TCP receive buffer into `input_buffer_`.
3. `RESPParser::parse(input_buffer_)` detects inline format (`S` is not `*`), splits on
   whitespace → `["SET", "x", "1"]`, erases those bytes from the buffer.
4. `CommandProcessor::execute(["SET","x","1"], db_)` uppercases `"SET"`, finds the handler.
5. The handler validates arity (3 args) then calls `db_.set("x", "1")`.
6. `db_.set()` does `store_["x"] = "1"` and `expiry_.erase("x")` (clear any old TTL).
7. The handler returns `"+OK\r\n"`.
8. `client.queue_response("+OK\r\n")` appends to `output_buffer_`.
9. `client.write_to_socket()` calls `send(fd, "+OK\r\n", 5)`, which copies to the kernel
   TCP send buffer. The OS transmits it to the client over the network.

---

### Q3: What is I/O multiplexing and why does `select()` enable a single thread to serve many clients?

**A:** `select()` takes a set of file descriptors and blocks the calling thread until at
least one of them becomes ready for I/O (readable or writable). The key insight is that most
clients are idle most of the time — a Redis client sends a command and waits for the reply
before sending the next one. During that wait the server can handle dozens of other clients.

Without multiplexing you'd need one thread per client. Threads have overhead (stack memory,
context-switch time). `select()` lets one thread interleave service for hundreds of clients
with zero thread overhead, matching Redis's design philosophy.

---

### Q4: Why are all sockets set to non-blocking mode?

**A:** A blocking `recv()` call halts the entire process until data arrives. In a
single-threaded server this would prevent all other clients from being served for the
duration of the wait — even if they have data ready.

Non-blocking mode makes `recv()` and `send()` return immediately with `EWOULDBLOCK`/
`WSAEWOULDBLOCK` when no data is available (recv) or the kernel buffer is full (send).
The server handles this case by trying again on the next `select()` wakeup.
`select()` itself does the blocking — but only until any socket is ready.

---

### Q5: What are partial reads and partial writes? How does the code handle them?

**A:**
- **Partial read:** `recv()` may return fewer bytes than the client sent if the data is
  still in transit. The server appends whatever arrives to `input_buffer_` and calls
  `RESPParser::parse()`. If the buffer doesn't contain a complete command, the parser
  returns `INCOMPLETE` and the bytes stay in the buffer. The next `recv()` appends more.

- **Partial write:** `send()` may accept fewer bytes than we passed if the kernel TCP send
  buffer is full (the remote client's receive window is full). We erase only the accepted
  bytes from `output_buffer_`. The remaining bytes stay queued. We register the fd in
  `write_fds` for `select()`, and flush the rest on the next write-ready event.

---

### Q6: What is SO_REUSEADDR and why does the server set it?

**A:** When a server process exits, TCP keeps the port in a `TIME_WAIT` state for ~2 minutes
(2× maximum segment lifetime). If you immediately restart the server, `bind()` fails with
`EADDRINUSE` because the OS thinks the old address is still in use.

`SO_REUSEADDR` tells the kernel to allow binding to a `TIME_WAIT` port, enabling rapid
server restarts during development without waiting 2 minutes. In production this is standard
practice for all TCP servers.

---

### Q7: Why does `select()` have a 100ms timeout instead of blocking forever?

**A:** Two reasons:
1. **Clean shutdown.** After `SIGINT` sets `running_ = false`, the loop must wake up to
   check that flag and exit. Without a timeout, `select()` would block forever waiting for
   socket activity that may never arrive.
2. **Active expiry.** We use the `loop_tick_` counter (incremented every iteration) to
   drive `db_.evict_expired()` every ~10 seconds. This timer is free — it piggybacks on the
   select() timeout without a separate thread or OS timer.

---

### Q8: What is the `FD_SETSIZE` limitation of `select()` and how would you scale beyond it?

**A:** `select()` uses a fixed-size bit array (`fd_set`) of size `FD_SETSIZE` (typically 1024
on Linux). Socket descriptors ≥ 1024 cannot be registered. This caps the server at ~1020
concurrent connections.

For high-concurrency production use:
- **Linux:** Use `epoll` — supports millions of file descriptors with O(1) notification via
  the ready-list model (vs. `select()`'s O(N) scan of the entire fd set).
- **macOS/BSD:** Use `kqueue` — same scalability properties as epoll.
- **Portable:** Use `libuv` or `libevent` which abstract epoll/kqueue/IOCP behind one API.
- **Windows:** Use I/O Completion Ports (IOCP) for the highest throughput on Windows.

---

### Q9: Explain the RESP protocol. How does the parser handle partial data?

**A:** RESP (Redis Serialization Protocol) is a line-based binary-safe protocol. A `SET x 1`
command sent by `redis-cli` looks like:
```
*3\r\n$3\r\nSET\r\n$1\r\nx\r\n$1\r\n1\r\n
```
- `*3` — array of 3 elements.
- `$3\r\nSET` — bulk string of length 3, content `SET`.
- `$1\r\nx` — bulk string of length 1, content `x`.
- `$1\r\n1` — bulk string of length 1, content `1`.

**Partial data:** TCP is a stream — the `*3\r\n$3\r\nSET\r\n` might arrive in the first
`recv()` and `$1\r\nx\r\n$1\r\n1\r\n` in the next. The parser tracks an `offset` into
`input_buffer_`. When it can't find a complete array (e.g., the buffer ends in the middle
of a bulk string), it returns `INCOMPLETE` without advancing the offset. The bytes remain
in the buffer until the next `recv()` appends the rest.

---

### Q10: How would you extend this server to support multiple worker threads?

**A:** The current design is single-threaded, so the `Database` requires no locking.
Adding threads safely requires:

1. **Reader-writer lock** on `Database` — concurrent reads are safe; writes need exclusive
   access. Use `std::shared_mutex` (C++17) with `std::shared_lock` for reads and
   `std::unique_lock` for writes.

2. **Thread-per-connection model** — simple but has O(N) thread overhead for N connections.
   Accept in the main thread, dispatch to a worker thread pool.

3. **io_uring / IOCP model** (advanced) — maintain the single-threaded event loop for
   networking but offload CPU-heavy operations (persistence, blocking I/O) to a thread pool.

Real Redis solved this differently in v6.0: it kept the main command-execution thread
single-threaded but added I/O threads that read from and write to sockets in parallel,
improving throughput without introducing locking on the data store.
