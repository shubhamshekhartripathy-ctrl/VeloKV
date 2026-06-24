# Milestone 5 Walkthrough — Redis-style Lists

## What Was Built

Milestone 5 adds Redis-compatible List commands to the C++17 TCP server:

| Command | Complexity | Description |
|---|---|---|
| `LPUSH key value` | O(1) | Insert at HEAD (left/front) |
| `RPUSH key value` | O(1) | Insert at TAIL (right/back) |
| `LPOP  key`       | O(1) | Remove and return HEAD |
| `RPOP  key`       | O(1) | Remove and return TAIL |
| `LLEN  key`       | O(1) | Return list length |

All 5 commands work over TCP, interoperate with EXPIRE/TTL, persist to disk in a new v2 RDB format, and return WRONGTYPE errors when applied to string keys.

---

## Architecture Explained

### Before Milestone 5

```cpp
// Single store — all values are strings
unordered_map<string, string> store_;
```

### After Milestone 5

```cpp
// Two separate stores — one per value type
unordered_map<string, string>    string_store_;  // String values
unordered_map<string, RedisList> list_store_;    // List values
                                                  // RedisList = deque<string>

// Shared TTL index — applies to keys in either store
unordered_map<string, int64_t> expiry_;
```

**Key invariant:** A key exists in **at most one** of the two stores.

### Why Not `std::variant`?

`std::variant<string, deque<string>>` is the "modern C++" single-map approach. We avoided it because:
1. `std::variant` with non-trivial types has subtle exception safety rules
2. `std::visit` boilerplate for every operation
3. GCC 6.3 (MinGW) has incomplete `<variant>` support
4. Two explicit maps are easier to read, debug, and reason about

This dual-map design directly mirrors how real Redis stores its data: each data type has its own internal dictionary.

---

## Why `std::deque<std::string>`?

Redis lists require O(1) at both ends:

| Operation | Redis Command | Container Requirement |
|---|---|---|
| push front | LPUSH | O(1) push_front |
| push back | RPUSH | O(1) push_back |
| pop front | LPOP | O(1) pop_front |
| pop back | RPOP | O(1) pop_back |
| size | LLEN | O(1) size |

**`std::deque`** satisfies all five requirements. It is internally implemented as an array of fixed-size memory blocks ("chunks"), giving:
- O(1) insertion/deletion at both ends (within a chunk or with a new chunk allocation)
- No per-element heap allocation (unlike `std::list`)
- Better CPU cache performance than `std::list` (memory is chunked, not scattered)
- `size()` is O(1) (unlike some `std::list` implementations)

**`std::vector`** was rejected: `push_front`/`pop_front` are O(N) because all elements must be shifted.

**`std::list`** was rejected: O(1) at both ends, but each element lives in its own heap node → cache-hostile, higher memory overhead.

### Real Redis Internals

Real Redis (version 7.2+) uses:
- **listpack** (formerly ziplist): a compact, cache-friendly byte array for small lists (< 128 elements, each < 64 bytes)
- **quicklist**: a doubly-linked list of listpacks for larger lists

When a listpack grows beyond the threshold, it is split and linked into a quicklist. This gives Redis both the memory efficiency of a packed byte array for small lists and the O(1) insertion/deletion at ends for large lists.

Our `std::deque` is the closest standard-library equivalent: internally chunked, O(1) at both ends.

---

## WRONGTYPE Error Pattern

Real Redis returns this error string when you call a list command on a string key:
```
-WRONGTYPE Operation against a key holding the wrong kind of value
```

Our implementation uses the same pattern in every list command handler:

```cpp
// In command.cpp — LPUSH handler:
if (db.type_of(args[1]) == KeyType::String) {
    return RESPParser::serialize_error(
        "WRONGTYPE Operation against a key holding the wrong kind of value");
}
```

`type_of()` is O(1):
```cpp
KeyType Database::type_of(const std::string& key) const {
    if (is_expired(key)) return KeyType::None;       // lazy eviction
    if (string_store_.count(key)) return KeyType::String;
    if (list_store_.count(key))   return KeyType::List;
    return KeyType::None;
}
```

The storage layer never produces error strings. Type checking is exclusively the command handler's responsibility.

---

## Empty-List Auto-Delete

When the last element is popped:

```cpp
// In db.cpp — lpop():
it->second.pop_front();

if (it->second.empty()) {
    list_store_.erase(it);   // remove the key entirely
    expiry_.erase(key);      // clear any TTL
}
```

After this: `EXISTS mylist → :0`, `LLEN mylist → :0`, `LPOP mylist → $-1`.

This is identical to real Redis behaviour: "A list is automatically created when we push an element to an empty key, and destroyed when we pop the last element."

---

## Persistence v2 Format

### File Layout

```
REDIS-CLONE-RDB v2
S city -1 Rome
S session 1750000000000 token%3A42
L mylist -1 3 alpha beta gamma
L queue 1750000000000 2 first second
EOF
```

- `S` lines: `S <enc_key> <expiry_ms|-1> <enc_value>`
- `L` lines: `L <enc_key> <expiry_ms|-1> <count> <elem0> <elem1> ...`
- Elements stored head-first (index 0 = front)
- On load, `rpush()` rebuilds order: `rpush(a)→[a]`, `rpush(b)→[a,b]` ✓

### Save Flow

```
1. db.snapshot() → vector<SnapshotEntry>
   (excludes expired keys; includes both string and list entries)

2. Write to redis.rdb.tmp:
   for each SnapshotEntry:
     if String: "S <enc_key> <expiry_ms> <enc_value>\n"
     if List:   "L <enc_key> <expiry_ms> <count> <e0> <e1>...\n"

3. Atomic rename: redis.rdb.tmp → redis.rdb
   (on Windows: remove old file first, then rename)
```

### Load Flow

```
1. Open redis.rdb
   If absent → return {success:true, 0 keys} (normal first start)

2. Read header line
   "REDIS-CLONE-RDB v2" → v2 mode
   "REDIS-CLONE-RDB v1" → v1 backward-compat mode (all records are strings)
   anything else → return error

3. For each record line:
   Parse type char (v2: 'S' or 'L'; v1: always 'S')
   Parse: enc_key, expiry_ms, rest
   If expiry_ms != -1 and current_ms >= expiry_ms → skip (expired offline)
   If String: db.set(key, value); db.expire_at(key, expiry_ms)
   If List:   for each elem: db.rpush(key, elem); db.expire_at(key, expiry_ms)

4. Validate "EOF" sentinel line
   Missing sentinel → log warning, return partial success
```

### Backward Compatibility

Old v1 files (all String records, no `S`/`L` prefix) are detected via the `kHeaderV1` constant and loaded correctly. The first SAVE after upgrade writes v2 format automatically.

---

## Complete Source Code

### `src/db.hpp`

```cpp
#pragma once

#include <string>
#include <vector>
#include <deque>
#include <unordered_map>
#include <chrono>
#include <cstdint>
#include <utility>  // std::pair

namespace redis {

// ---------------------------------------------------------------------------
// Value type tag
// ---------------------------------------------------------------------------

enum class KeyType {
    None,   ///< Key does not exist (or has expired).
    String, ///< Value is a std::string.
    List    ///< Value is a std::deque<std::string>.
};

// ---------------------------------------------------------------------------
// Type aliases
// ---------------------------------------------------------------------------

/**
 * A Redis List: a doubly-ended queue of strings.
 *
 * std::deque<string> chosen for O(1) push_front/push_back/pop_front/pop_back
 * and O(1) size(). Better cache performance than std::list (chunked memory,
 * no per-element heap allocation). Supports LPUSH/RPUSH/LPOP/RPOP/LLEN.
 */
using RedisList = std::deque<std::string>;

// ---------------------------------------------------------------------------
// Snapshot entry (used by PersistenceManager)
// ---------------------------------------------------------------------------

struct SnapshotEntry {
    std::string key;
    KeyType     type;        ///< String or List
    int64_t     expiry_ms;   ///< Absolute Unix-ms deadline, or -1 (no TTL)
    std::string str_value;   ///< Used when type == String
    RedisList   list_value;  ///< Used when type == List
};

// ---------------------------------------------------------------------------
// Database
// ---------------------------------------------------------------------------

class Database {
public:
    Database() = default;
    ~Database() = default;
    Database(const Database&)            = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&&) noexcept        = default;
    Database& operator=(Database&&) noexcept = default;

    // Type inspection
    KeyType type_of(const std::string& key) const;

    // String operations
    void set(const std::string& key, const std::string& value);
    bool get(const std::string& key, std::string& out_value) const;
    bool del(const std::string& key);
    bool exists(const std::string& key) const;

    // List operations (caller must verify type_of() != String first)
    int64_t lpush(const std::string& key, const std::string& value);
    int64_t rpush(const std::string& key, const std::string& value);
    std::pair<bool, std::string> lpop(const std::string& key);
    std::pair<bool, std::string> rpop(const std::string& key);
    int64_t llen(const std::string& key) const;

    // TTL / Expiry
    bool    expire(const std::string& key, int64_t ttl_ms);
    void    expire_at(const std::string& key, int64_t abs_ms);
    int64_t ttl(const std::string& key) const;

    // Persistence
    std::vector<SnapshotEntry> snapshot() const;

    // Active eviction
    void evict_expired();

private:
    mutable std::unordered_map<std::string, std::string>  string_store_;
    mutable std::unordered_map<std::string, RedisList>    list_store_;
    mutable std::unordered_map<std::string, int64_t>      expiry_;

    bool is_expired(const std::string& key) const;
    static int64_t now_ms() noexcept;
};

} // namespace redis
```

### `src/db.cpp` — Key Methods

```cpp
// Type inspection — O(1)
KeyType Database::type_of(const std::string& key) const {
    if (is_expired(key)) return KeyType::None;
    if (string_store_.count(key)) return KeyType::String;
    if (list_store_.count(key))   return KeyType::List;
    return KeyType::None;
}

// SET overwrites any existing type
void Database::set(const std::string& key, const std::string& value) {
    list_store_.erase(key);     // remove list if exists
    string_store_[key] = value;
    expiry_.erase(key);         // clear TTL (Redis SET resets TTL)
}

// DEL works on both types
bool Database::del(const std::string& key) {
    expiry_.erase(key);
    size_t removed = string_store_.erase(key) + list_store_.erase(key);
    return removed > 0;
}

// LPUSH — insert at head
int64_t Database::lpush(const std::string& key, const std::string& value) {
    list_store_[key].push_front(value);
    return static_cast<int64_t>(list_store_[key].size());
}

// RPUSH — insert at tail
int64_t Database::rpush(const std::string& key, const std::string& value) {
    list_store_[key].push_back(value);
    return static_cast<int64_t>(list_store_[key].size());
}

// LPOP — remove from head; auto-delete on empty
std::pair<bool, std::string> Database::lpop(const std::string& key) {
    if (is_expired(key)) return {false, ""};
    auto it = list_store_.find(key);
    if (it == list_store_.end() || it->second.empty()) return {false, ""};

    std::string val = std::move(it->second.front());
    it->second.pop_front();

    if (it->second.empty()) {   // Auto-delete: list destroyed on last pop
        list_store_.erase(it);
        expiry_.erase(key);
    }
    return {true, std::move(val)};
}

// RPOP — remove from tail; auto-delete on empty
std::pair<bool, std::string> Database::rpop(const std::string& key) {
    if (is_expired(key)) return {false, ""};
    auto it = list_store_.find(key);
    if (it == list_store_.end() || it->second.empty()) return {false, ""};

    std::string val = std::move(it->second.back());
    it->second.pop_back();

    if (it->second.empty()) {
        list_store_.erase(it);
        expiry_.erase(key);
    }
    return {true, std::move(val)};
}

// LLEN — returns 0 for absent key (not an error)
int64_t Database::llen(const std::string& key) const {
    if (is_expired(key)) return 0;
    auto it = list_store_.find(key);
    if (it == list_store_.end()) return 0;
    return static_cast<int64_t>(it->second.size());
}

// is_expired — shared lazy eviction for both stores
bool Database::is_expired(const std::string& key) const {
    auto it = expiry_.find(key);
    if (it == expiry_.end()) return false;
    if (now_ms() >= it->second) {
        string_store_.erase(key);   // erase from whichever store holds it
        list_store_.erase(key);
        expiry_.erase(it);
        return true;
    }
    return false;
}
```

### `src/command.cpp` — List Handlers

```cpp
// LPUSH handler
register_handler("LPUSH", [](const vector<string>& args, Database& db) -> string {
    if (args.size() != 3)
        return RESPParser::serialize_error("ERR wrong number of arguments for 'lpush'");
    if (db.type_of(args[1]) == KeyType::String)
        return RESPParser::serialize_error(
            "WRONGTYPE Operation against a key holding the wrong kind of value");
    return RESPParser::serialize_integer(db.lpush(args[1], args[2]));
});

// RPUSH handler
register_handler("RPUSH", [](const vector<string>& args, Database& db) -> string {
    if (args.size() != 3)
        return RESPParser::serialize_error("ERR wrong number of arguments for 'rpush'");
    if (db.type_of(args[1]) == KeyType::String)
        return RESPParser::serialize_error(
            "WRONGTYPE Operation against a key holding the wrong kind of value");
    return RESPParser::serialize_integer(db.rpush(args[1], args[2]));
});

// LPOP handler
register_handler("LPOP", [](const vector<string>& args, Database& db) -> string {
    if (args.size() != 2)
        return RESPParser::serialize_error("ERR wrong number of arguments for 'lpop'");
    if (db.type_of(args[1]) == KeyType::String)
        return RESPParser::serialize_error(
            "WRONGTYPE Operation against a key holding the wrong kind of value");
    pair<bool, string> result = db.lpop(args[1]);
    return result.first ? RESPParser::serialize_bulk_string(result.second)
                        : RESPParser::serialize_null_bulk_string();
});

// RPOP, LLEN follow the same pattern (see command.cpp for full code)
```

---

## Test Session (ncat)

Start the server, then connect with ncat:

```bash
# Terminal 1: start server
.\redis_server.exe

# Terminal 2: connect
ncat 127.0.0.1 6379
```

### Stack (LPUSH + LPOP)

```
LPUSH stack c
:1
LPUSH stack b
:2
LPUSH stack a
:3
LLEN stack
:3
LPOP stack
$1
a
LPOP stack
$1
b
LPOP stack
$1
c
LLEN stack
:0
EXISTS stack
:0
```

Stack is built with LPUSH: each push goes to the head, so the last-pushed element is first popped (LIFO).

### Queue (RPUSH + LPOP)

```
RPUSH queue msg1
:1
RPUSH queue msg2
:2
RPUSH queue msg3
:3
LPOP queue
$4
msg1
LPOP queue
$4
msg2
LPOP queue
$4
msg3
LLEN queue
:0
```

Queue uses RPUSH (append to tail) + LPOP (remove from head): first in, first out (FIFO).

### WRONGTYPE Error

```
SET mykey hello
+OK
LPUSH mykey x
-WRONGTYPE Operation against a key holding the wrong kind of value
LLEN mykey
-WRONGTYPE Operation against a key holding the wrong kind of value
```

```
RPUSH mylist a
:1
GET mylist
-WRONGTYPE Operation against a key holding the wrong kind of value
```

### TTL on a List

```
RPUSH jobs task1
:1
EXPIRE jobs 60
:1
TTL jobs
:59
LPOP jobs
$5
task1
EXISTS jobs
:0
TTL jobs
:-2
```

### Persistence

```
RPUSH persist a
:1
RPUSH persist b
:2
RPUSH persist c
:3
SAVE
+OK
```

Restart the server. The list is restored:

```
LLEN persist
:3
LPOP persist
$1
a
LPOP persist
$1
b
```

---

## Test Results (Automated)

```
=== Milestone 5: Redis-style Lists ===

--- RPUSH / LLEN ---    5/5 PASS
--- LPUSH ---           4/4 PASS
--- LPOP ---            6/6 PASS
--- RPOP ---            5/5 PASS
--- RPUSH/RPOP tail ---  2/2 PASS
--- RPUSH/LPOP FIFO ---  2/2 PASS
--- WRONGTYPE errors --- 6/6 PASS
--- SET overwrites ---   4/4 PASS
--- TTL on list ---      1/1 PASS
--- Persistence ---      3/3 PASS
--- After restart ---    4/4 PASS

PASSED: 42
FAILED: 0
```

---

## Interview Q&A

### Q1: Why use `std::deque` instead of `std::list` for a Redis list?

**A:** Both offer O(1) at both ends, but `std::deque` is faster in practice because it uses chunked memory (an array of fixed-size blocks) while `std::list` allocates one heap node per element. Heap allocations are expensive (malloc overhead, cache misses), and traversing `std::list` nodes causes many cache misses since nodes are scattered in memory. `std::deque` keeps elements grouped in blocks, which is cache-friendly. Also, `std::deque::size()` is guaranteed O(1) in C++11+, which is required for LLEN. Some older `std::list` implementations had O(N) `size()`.

---

### Q2: What is the WRONGTYPE error and when does Redis return it?

**A:** Redis maintains a type for each key. If you call a command that expects a list on a key that holds a string (or vice versa), Redis returns:
```
-WRONGTYPE Operation against a key holding the wrong kind of value
```
This happens before any modification — the database is not touched. In our clone, every list command handler calls `db.type_of(key)` first. If the type is `String`, the handler returns the WRONGTYPE RESP error without calling any DB method.

---

### Q3: What happens when you pop the last element from a list in Redis?

**A:** The key is automatically deleted. Redis documentation states: "A list is automatically created when we push an element to an empty key, and destroyed when we pop the last element." Our `lpop()`/`rpop()` implementations call `list_store_.erase(it)` and `expiry_.erase(key)` when the deque becomes empty. After that: `EXISTS key → 0`, `LLEN key → 0`, `LPOP key → nil`.

---

### Q4: How does `SET key value` behave when the key holds a list?

**A:** `SET` unconditionally replaces whatever the key held before — including lists, hashes, sets, or other types. In our implementation:
```cpp
void Database::set(const std::string& key, const std::string& value) {
    list_store_.erase(key);     // clear any existing list
    string_store_[key] = value; // store as string
    expiry_.erase(key);         // reset TTL
}
```
After `SET key value`, calling `LLEN key` returns WRONGTYPE (because the key now holds a string, not a list), and `GET key` returns the new value.

---

### Q5: How do you implement a queue and a stack using Redis lists?

**A:**

**Stack (LIFO):** Use LPUSH + LPOP.
```
LPUSH stack a   → [a]
LPUSH stack b   → [b, a]
LPUSH stack c   → [c, b, a]
LPOP stack      → c (last pushed, first out)
```

**Queue (FIFO):** Use RPUSH + LPOP.
```
RPUSH queue a   → [a]
RPUSH queue b   → [a, b]
RPUSH queue c   → [a, b, c]
LPOP queue      → a (first pushed, first out)
```

The key insight: LPUSH inserts at the head, RPUSH at the tail, LPOP pops from the head, RPOP from the tail.

---

### Q6: Explain the persistence format design. Why use text and not binary?

**A:** We chose a text format for several reasons:
1. **Human-readable**: You can open `redis.rdb` in a text editor and inspect it without tools.
2. **Debuggable**: Corruption is immediately visible; binary corruption is silent.
3. **Simple to implement**: No endianness issues, no struct packing, no binary parsers.
4. **Extensible**: Adding new record types is trivial (add a new type prefix character).

The trade-off is space efficiency — text is larger than binary. Real Redis's `.rdb` format is binary for this reason, using length-prefixed strings and packed integers. For a production system, binary would be preferred; for a learning clone, text clarity wins.

Crash safety is achieved by writing to a temp file first, then renaming — the same technique used by SQLite, PostgreSQL WAL, and Redis itself.

---

### Q7: Why store `expiry_ms` as an absolute timestamp rather than a relative TTL?

**A:** If we stored a relative TTL (e.g., "expires in 30 seconds"), the server would have to record when the key was created and recompute the remaining TTL on every check. More importantly, persistence would break: saving "30 seconds remaining" is meaningless if the server is down for 45 seconds. By storing an absolute Unix-millisecond timestamp, the expiry check is always a single comparison: `now_ms() >= expiry_ms`. When loading from disk, keys whose deadline has passed are silently discarded — the database is always consistent on startup.

---

### Q8: What is the difference between lazy and active expiry? Which does our server use?

**A:**

- **Lazy expiry**: Check if a key is expired when it is accessed. Only expired keys that are actually accessed get cleaned up. Our `is_expired()` is called at the start of every read/write method.

- **Active expiry**: A background process periodically scans all keys with TTLs and evicts expired ones. Our `evict_expired()` is called by the server's timer loop (every second).

Our server uses **both**: lazy eviction handles most cases with zero overhead (we only check keys being accessed), while active eviction ensures expired keys eventually get cleaned up even if they are never accessed again. This is the same strategy Redis uses — without active eviction, expired-but-never-accessed keys would accumulate and leak memory.

---

### Q9: Why is `is_expired()` declared `const` but the stores are `mutable`?

**A:** `is_expired()` is called from `const` methods like `get()`, `exists()`, `ttl()`, and `type_of()`. These methods are logically read-only from the caller's perspective — they don't modify the observable database state. However, expiring a key is a side effect that must happen even during a read. In C++, `mutable` allows `const` methods to modify members while maintaining the logical const contract: an expired key was already unobservable, so deleting it during a read doesn't change the externally visible state. This pattern is sometimes called "lazy propagation of hidden state."

---

### Q10: How does real Redis store lists differently from our implementation?

**A:**

Real Redis (7.2+) uses a **quicklist** encoding:

1. For **small lists** (< 128 elements, each < 64 bytes, configurable via `list-max-listpack-size`): a single **listpack** (formerly ziplist). A listpack is a contiguous byte array encoding each element with a length prefix. Extremely cache-friendly and memory-efficient.

2. For **large lists**: a doubly-linked list of listpacks (**quicklist**). Each listpack node holds up to `fill` elements. This gives O(1) at both ends via the linked structure, with cache-friendly element storage within each listpack.

Our `std::deque` is conceptually similar: internally chunked (array of fixed-size blocks), O(1) at both ends. The main difference is that real Redis's encoding is tunable, compact (variable-length integers, no C++ string overhead), and avoids heap allocations for small lists entirely.

---

## Files Changed in Milestone 5

| File | Status | Change |
|---|---|---|
| [src/db.hpp](file:///d:/redis/src/db.hpp) | MODIFIED | KeyType enum, RedisList alias, SnapshotEntry updated, dual stores, 5 new declarations |
| [src/db.cpp](file:///d:/redis/src/db.cpp) | MODIFIED | Dual-store implementation, lpush/rpush/lpop/rpop/llen, updated set/del/exists/expire/snapshot/evict |
| [src/command.cpp](file:///d:/redis/src/command.cpp) | MODIFIED | WRONGTYPE guard on GET, 5 new command handlers |
| [src/persistence.hpp](file:///d:/redis/src/persistence.hpp) | MODIFIED | kHeader → v2, added kHeaderV1 |
| [src/persistence.cpp](file:///d:/redis/src/persistence.cpp) | MODIFIED | save() v2 format with S/L prefix, load() v1/v2 detection and list loading |
| src/server.cpp | UNCHANGED | Networking layer needs no changes |
| src/client.cpp | UNCHANGED | Client handling needs no changes |
| src/resp.cpp | UNCHANGED | RESP serialization needs no changes |
| Makefile | UNCHANGED | All .cpp files already in compile rule |

---

## Build & Run

```powershell
# Build
cd d:\redis
mingw32-make clean
mingw32-make

# Start server
.\redis_server.exe

# Run test suite
powershell -ExecutionPolicy Bypass -File "C:\Users\ASUS\.gemini\antigravity-ide\brain\65d61c10-25c4-4379-ac1a-c0888c13b772\scratch\test_milestone_5.ps1"
```

Expected output: `PASSED: 42 / FAILED: 0`
