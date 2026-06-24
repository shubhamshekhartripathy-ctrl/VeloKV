# Implementation Plan — Milestone 3: Persistence + TTL

## Overview

This milestone adds two interrelated features:

1. **TTL / Expiry** — The `Database` class learns about time. Keys can have an expiry deadline stored alongside their values. Expired keys are evicted lazily on access and proactively via a periodic sweep.
2. **Persistence** — A `PersistenceManager` class saves the entire database to a human-readable file on disk and reloads it on startup. TTL deadlines are saved and restored correctly.

---

## Architecture Explanation

### Why a separate `PersistenceManager` class?

The `Database` class already owns the SRP (Single Responsibility Principle) for in-memory state. Giving it file I/O responsibility would violate SRP and make it harder to unit-test either concern independently. Instead, `PersistenceManager` is a dedicated I/O layer that speaks only to `Database`'s public API (or a dedicated snapshot API). This mirrors how real Redis separates its RDB persistence subsystem from its in-memory data structures.

### File Format Choice: Custom Text Format (`.rdb.txt`)

We use a simple, human-readable line-based text file. Real Redis uses a binary RDB format, which is compact and fast but opaque. For an interview-quality clone, a text format:
- Is trivially debuggable (open in any editor)
- Is easy to parse without third-party libraries
- Still teaches all the same concepts (header, record structure, EOF marker)

**Format grammar:**
```
REDIS-CLONE-RDB v1                  # Magic header
<key> <ttl_unix_ms_or_-1> <value>   # One record per line (space-separated, value is last)
EOF                                  # Sentinel marker
```

`ttl_unix_ms_or_-1`: The absolute expiry time as Unix milliseconds since epoch. `-1` means no expiry. On load, if the stored deadline is already in the past, the key is silently discarded (expired-on-load protection).

The value is the last field so it can contain internal spaces without needing quoting or escaping — but we use URL-percent-encoding (`%20` for space, `%0A` for newline, `%%` for `%`) to keep the format safe for arbitrary binary values.

### Save Flow

```
Server receives SIGINT / SIGTERM
  │
  ▼
Server::stop() is called
  │
  ▼
PersistenceManager::save(const Database& db, const std::string& path)
  │
  ├─ Opens a temporary file: path + ".tmp"
  ├─ Writes the header line: "REDIS-CLONE-RDB v1"
  ├─ Iterates over Database::snapshot() → vector<SnapshotEntry>
  │    Each entry: { key, value, expiry_ms (or -1) }
  │    Skips already-expired entries
  │    Encodes key and value with percent-encoding
  │    Writes: "<encoded_key> <expiry_ms> <encoded_value>\n"
  ├─ Writes the footer: "EOF"
  └─ Atomically renames ".tmp" → actual path (crash-safe write)
```

### Load Flow

```
Server::init() is called
  │
  ▼
PersistenceManager::load(Database& db, const std::string& path)
  │
  ├─ Opens the file; if missing → normal startup (empty database)
  ├─ Validates header: "REDIS-CLONE-RDB v1"
  ├─ Reads records line by line:
  │    Decodes key and value with percent-decoding
  │    Parses expiry_ms
  │    If expiry_ms != -1 AND already expired → skip (do NOT load stale key)
  │    If expiry_ms != -1 AND still valid → db.set(key, value) + db.expire_at(key, expiry_ms)
  │    If expiry_ms == -1 → db.set(key, value)
  ├─ Validates footer: "EOF" (detects truncated files)
  └─ Returns count of loaded keys
```

### Expiry Internals

The `Database` class adds a second map: `std::unordered_map<std::string, int64_t> expiry_` mapping keys to their Unix-millisecond deadlines.

**Lazy eviction**: Every access method (`get`, `exists`, `expire`, `ttl`) first checks `is_expired(key)`. If expired, the key is silently removed from both `store_` and `expiry_` before returning the "not found" result.

**Active expiry sweep**: `Database::evict_expired()` iterates `expiry_` and deletes all entries whose deadline has passed. This is called by the server's event loop every N iterations (configurable). This prevents memory from growing unboundedly with expired-but-never-accessed keys.

---

## Files Changed / Created

### [Storage Engine — TTL + Snapshot API]

#### [MODIFY] [db.hpp](file:///d:/redis/src/db.hpp)
- Adds `#include <chrono>` and `#include <cstdint>`
- Adds `expiry_` map: `std::unordered_map<std::string, int64_t>`
- Adds `struct SnapshotEntry { std::string key, value; int64_t expiry_ms; }`
- Adds public methods:
  - `bool expire(const std::string& key, int64_t ttl_ms)` — sets deadline relative to now
  - `void expire_at(const std::string& key, int64_t abs_ms)` — sets absolute deadline (used by persistence load)
  - `int64_t ttl(const std::string& key) const` — returns remaining ms, -1 if no expiry, -2 if not found
  - `std::vector<SnapshotEntry> snapshot() const` — returns all live entries for persistence
  - `void evict_expired()` — active sweep of the expiry map
- Adds private helpers:
  - `bool is_expired(const std::string& key) const`
  - `static int64_t now_ms()` — returns `std::chrono::system_clock` milliseconds since epoch

#### [MODIFY] [db.cpp](file:///d:/redis/src/db.cpp)
- All `get`, `exists`, `del` methods call `is_expired()` first (lazy eviction)
- `set()` clears any existing expiry for the key (same as Redis: SET resets TTL)
- Full implementations of all new methods above

---

### [Persistence Layer — New Files]

#### [NEW] `src/persistence.hpp`
Declares the `PersistenceManager` class:
```cpp
class PersistenceManager {
public:
    static bool save(const Database& db, const std::string& path);
    static LoadResult load(Database& db, const std::string& path);
private:
    static std::string percent_encode(const std::string& s);
    static std::string percent_decode(const std::string& s);
};
struct LoadResult { bool success; int keys_loaded; std::string error; };
```
All methods are `static` — `PersistenceManager` is a stateless utility class (no instance state needed).

#### [NEW] `src/persistence.cpp`
- `save()`: atomic write via `.tmp` temp file + `std::rename()`
- `load()`: line-by-line parse with header/footer validation, expired-key filtering
- `percent_encode()` / `percent_decode()`: encodes `%`, space, `\r`, `\n` for safe line-based serialization

---

### [Command Router — EXPIRE, TTL]

#### [MODIFY] [command.cpp](file:///d:/redis/src/command.cpp)
- `EXPIRE <key> <seconds>` handler: validates args, calls `db.expire(key, seconds * 1000)`, returns `:1` or `:0`
- `TTL <key>` handler: calls `db.ttl(key)`, returns ms converted to seconds (`:N`, `-1`, or `-2`)
- `SAVE` handler (bonus): triggers `PersistenceManager::save()` on demand

#### [MODIFY] [command.hpp](file:///d:/redis/src/command.hpp)
- No signature changes needed (handlers already take `Database&`)

---

### [Server — Startup Load + Shutdown Save + Active Eviction]

#### [MODIFY] [server.hpp](file:///d:/redis/src/server.hpp)
- Adds `#include "persistence.hpp"`
- Adds `static constexpr std::string_view kDbPath = "redis.rdb"` (or a configurable path)
- Adds `uint64_t loop_tick_` counter for eviction scheduling

#### [MODIFY] [server.cpp](file:///d:/redis/src/server.cpp)
- `Server::init()`: after socket setup, calls `PersistenceManager::load(db_, kDbPath)` and logs result
- `Server::stop()`: before returning, calls `PersistenceManager::save(db_, kDbPath)` and logs result
- Event loop: every 100 iterations, calls `db_.evict_expired()`

---

### [Build System]

#### [MODIFY] [Makefile](file:///d:/redis/Makefile)
- Upgrades `CXXFLAGS` from `-std=c++14` to `-std=c++17`
- Adds `src/persistence.cpp` to `SRCS`

---

## Verification Plan

### Build
```
mingw32-make clean && mingw32-make
```
Expected: zero warnings, zero errors.

### Manual Test Session
```powershell
# Terminal 1 — start the server
.\redis_server.exe

# Terminal 2 — write some data
redis-cli SET name Alice
redis-cli EXPIRE name 300
redis-cli SET city "New York"
redis-cli SET temp_key value
redis-cli EXPIRE temp_key 1

# Wait 2 seconds (temp_key expires), then Ctrl+C server
# Restart server

# Terminal 2 — verify persistence
redis-cli GET name         # "Alice"
redis-cli TTL name         # ~300
redis-cli GET city         # "New York"
redis-cli GET temp_key     # (nil) — expired, not loaded
```

### Test Script
A PowerShell test script `test_milestone_3.ps1` will be written to automate all edge cases.
