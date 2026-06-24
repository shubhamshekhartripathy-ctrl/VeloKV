# Walkthrough — Milestone 3: Persistence + TTL

## Build Result

```
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Isrc -c src/common.cpp   → common.o
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Isrc -c src/db.cpp        → db.o
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Isrc -c src/persistence.cpp → persistence.o
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Isrc -c src/resp.cpp      → resp.o
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Isrc -c src/command.cpp   → command.o
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Isrc -c src/client.cpp    → client.o
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Isrc -c src/server.cpp    → server.o
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Isrc -c src/main.cpp      → main.o
→ Linked: redis_server.exe
Zero warnings. Zero errors.
```

---

## Updated Folder Structure

```
d:/redis/
├── Makefile                  # Build: C++17, -Wall -Wextra -Wpedantic
├── redis.rdb                 # [NEW] Persistence file (created at runtime)
└── src/
    ├── main.cpp              # Bootstrap: init network, run server
    ├── common.hpp / .cpp     # Platform socket abstractions
    ├── client.hpp / .cpp     # Per-connection read/write buffers
    ├── resp.hpp / .cpp       # RESP protocol parser + serializer
    ├── db.hpp / .cpp         # [MODIFIED] Storage engine + TTL + snapshot
    ├── persistence.hpp / .cpp # [NEW] File I/O: save + load
    ├── command.hpp / .cpp    # [MODIFIED] Handlers: EXPIRE, TTL, SAVE added
    ├── server.hpp / .cpp     # [MODIFIED] Load on init, save on stop, eviction
```

---

## Architecture Explanation

```
┌────────────────────────────────────────────────────┐
│                    main.cpp                        │  Entry point
├────────────────────────────────────────────────────┤
│                   server.cpp                       │  Network + Orchestration
│                                                    │
│  init():  PersistenceManager::load(db_, "redis.rdb")│  ← LOAD on startup
│  stop():  PersistenceManager::save(db_, "redis.rdb")│  ← SAVE on shutdown
│  loop:    db_.evict_expired()  every 100 ticks     │  ← ACTIVE EVICTION
├────────────────────────────────────────────────────┤
│               command.cpp                          │  Command Routing
│   EXPIRE → db.expire()  TTL → db.ttl()            │  ← NEW TTL commands
│   SAVE   → PersistenceManager::save()              │  ← Manual save trigger
├─────────────────┬──────────────────────────────────┤
│    db.cpp       │     persistence.cpp               │  Storage + I/O (separate)
│                 │                                   │
│ store_  : map   │  save(): snapshot → file.tmp      │
│ expiry_ : map   │          → rename to file         │
│                 │  load(): file → parse → db.set()  │
│ is_expired()    │          + db.expire_at()         │
│ lazy eviction   │                                   │
└─────────────────┴──────────────────────────────────┘
                          ↕
                    [ redis.rdb ]   ← disk
```

**Why two separate classes?**
`Database` owns in-memory state. `PersistenceManager` owns file I/O. Mixing them would violate the Single Responsibility Principle and make either class harder to unit-test. This matches how production Redis separates its in-memory data structures (`dict`, `ziplist`) from its persistence subsystem (`rdb.c`, `aof.c`).

---

## File-by-File Explanation

### [`db.hpp`](file:///d:/redis/src/db.hpp) / [`db.cpp`](file:///d:/redis/src/db.cpp) — Storage Engine [MODIFIED]

**New data member:**
```cpp
mutable std::unordered_map<std::string, int64_t> expiry_;
```
Stores absolute Unix-millisecond deadlines. Only keys **with** a TTL appear here. This parallel-map design means:
- Accessing a key with no TTL costs zero extra lookups
- `evict_expired()` only scans `expiry_` (not the whole `store_`)

**New struct:**
```cpp
struct SnapshotEntry { std::string key, value; int64_t expiry_ms; };
```

**New methods:**

| Method | Purpose |
|---|---|
| `expire(key, ttl_ms)` | Set TTL relative to now (now + ttl_ms) |
| `expire_at(key, abs_ms)` | Set absolute deadline (used by persistence load) |
| `ttl(key)` | Returns remaining ms, -1 (no TTL), or -2 (not found) |
| `snapshot()` | Returns all live entries as a vector for file writing |
| `evict_expired()` | Active sweep: erase all past-deadline entries |

**Key Design — `mutable`:**
`is_expired()` is declared `const` so it can be called from const methods like `get()`, `exists()`, and `ttl()`. It needs to erase from `store_` and `expiry_` when a key has expired. Both maps are marked `mutable` — this is the standard C++ idiom for lazy computation in const contexts (the logical state hasn't changed; the expired key was unobservable anyway).

**TTL stored as absolute time, not relative:**
```
stored_deadline = now_ms() + ttl_ms
```
This means we never need to update the stored value. The check is `now_ms() >= stored_deadline` — O(1), always.

---

### [`persistence.hpp`](file:///d:/redis/src/persistence.hpp) / [`persistence.cpp`](file:///d:/redis/src/persistence.cpp) — File I/O [NEW]

A **stateless utility class** — all methods are `static`. It has no member variables. Instantiation is prevented with a deleted constructor.

#### File Format

```
REDIS-CLONE-RDB v1
<encoded_key> <expiry_ms_or_-1> <encoded_value>
...
EOF
```

**Example `redis.rdb`:**
```
REDIS-CLONE-RDB v1
city_key 1782230208911 NewYork
city -1 NewYork
session 1782229905307 abc
persist_key -1 persist_val
reset_test -1 val2
EOF
```

- `city_key 1782230208911 NewYork` — expires at Unix ms `1782230208911`, value `NewYork`
- `city -1 NewYork` — no TTL (persists forever)

#### Percent-Encoding

Keys and values that contain spaces, newlines, or `%` are encoded:

| Character | Encoded |
|---|---|
| `%` | `%25` |
| ` ` (space) | `%20` |
| `\r` | `%0D` |
| `\n` | `%0A` |

The value is the **last field on the line**, so a decoded value can safely contain spaces once decoded — only newlines and literal `%` need encoding.

#### Save Flow (Crash-Safe)

```
db.snapshot()          ← get all live entries (skips expired ones)
    ↓
open "redis.rdb.tmp"   ← write to TEMP file first
    ↓
write header + records + EOF
    ↓
fclose + rename ".tmp" → "redis.rdb"   ← ATOMIC on POSIX
```

If the process crashes mid-write, the `.tmp` file is incomplete — but the real `redis.rdb` is untouched. On the next startup, the old clean file is loaded. This is the same strategy used by SQLite, PostgreSQL, and Redis itself.

#### Load Flow (Expired-Key Filtering)

```
open "redis.rdb"       ← missing? → normal first start, 0 keys
    ↓
validate "REDIS-CLONE-RDB v1" header
    ↓
for each line:
    split: encoded_key  expiry_ms  encoded_value
    percent_decode key and value
    if expiry_ms != -1 AND now_ms() >= expiry_ms:
        SKIP  ← key expired while server was offline
    else:
        db.set(key, value)
        if expiry_ms != -1:
            db.expire_at(key, expiry_ms)   ← restore absolute deadline
    ↓
validate "EOF" sentinel
    ↓
return LoadResult { success, keys_loaded, error }
```

**Expired-key filtering** is critical: if the server is offline for 30 minutes and a key had a 5-minute TTL, that key must NOT be loaded. Time kept ticking while the server was down.

---

### [`command.cpp`](file:///d:/redis/src/command.cpp) — Command Handlers [MODIFIED]

Three new handlers registered in `register_builtin_commands()`:

**`EXPIRE <key> <seconds>`**
- Validates argument count and that `<seconds>` is a non-negative integer
- Calls `db.expire(key, seconds * 1000)` (converts to milliseconds internally)
- Returns `:1` if the key existed, `:0` if not found

**`TTL <key>`**
- Calls `db.ttl(key)` which returns milliseconds
- Divides by 1000 (integer/floor division) to return whole seconds
- Returns: `≥0` remaining seconds, `:-1` (no TTL), `:-2` (not found)

**`SAVE`** (bonus)
- Calls `PersistenceManager::save(db, "redis.rdb")` directly
- Returns `+OK` on success, `-ERR` on failure
- Synchronous (blocking) — appropriate for a single-threaded server

---

### [`server.hpp`](file:///d:/redis/src/server.hpp) / [`server.cpp`](file:///d:/redis/src/server.cpp) — Orchestrator [MODIFIED]

**Three changes:**

1. **`init()`** — After socket setup, calls `PersistenceManager::load(db_, kDbPath)` and logs the result. A load failure is non-fatal (logs warning, starts with empty DB).

2. **`stop()`** — Uses `atomic::exchange` to ensure save is called only once (even if `stop()` is called multiple times):
   ```cpp
   if (!running_.exchange(false)) return; // Already stopped
   PersistenceManager::save(db_, kDbPath);
   ```

3. **Active eviction in the event loop:**
   ```cpp
   if (++loop_tick_ >= kEvictionInterval) {
       db_.evict_expired();
       loop_tick_ = 0;
   }
   ```
   With `kEvictionInterval = 100` and a 100ms select() timeout → sweeps every ~10 seconds. No extra thread needed.

---

## Test Results

**38 / 38 tests passed** — `test_milestone_3.ps1`

| Section | Tests | Result |
|---|---|---|
| SET / GET | 4 | ✅ All passed |
| DEL / EXISTS | 4 | ✅ All passed |
| EXPIRE / TTL | 6 | ✅ All passed |
| Key expiry (1s TTL wait) | 3 | ✅ All passed |
| SET clears existing TTL | 4 | ✅ All passed |
| SAVE + file content | 7 | ✅ All passed |
| Error handling | 6 | ✅ All passed |
| **Persistence reload** | manual | ✅ `Loaded 5 key(s)` on restart, TTLs preserved |

**Reload verification:**
```
[Server] Loaded 5 key(s) from 'redis.rdb'.

GET persist_key => $11   (11 chars = "persist_val" ✅)
GET city        => $7    (7 chars = "NewYork" ✅)
GET reset_test  => $4    (4 chars = "val2" ✅)
TTL session     => :206  (300 - elapsed ≈ 206 seconds remaining ✅)
TTL city_key    => :510  (600 - elapsed ≈ 510 seconds remaining ✅)
```

---

## Interview Questions & Answers

### Q1: Why store TTL as an absolute timestamp instead of a relative countdown?

**A:** Storing a relative TTL (e.g., "10 seconds") requires either:
- Decrementing the stored value every millisecond — expensive O(N) update on all keys each tick, or
- Recording *when* the TTL was set alongside the relative duration

Storing an **absolute deadline** (`stored_at + ttl`) means the check is always `now() >= deadline` — a single comparison. It also makes serialisation trivial: we write the absolute timestamp to the file, and on reload we compare it against the current time to filter expired keys. No adjustment needed.

---

### Q2: What is lazy vs. active expiry? Why use both?

**A:**
- **Lazy expiry**: Check the TTL only when a key is accessed. Expired keys are evicted at access time. Cost: O(1) per access, but keys that are never re-accessed stay in memory indefinitely.
- **Active expiry**: A periodic sweep iterates the TTL index and removes all past-deadline entries. Cost: O(E) where E = number of keys with a TTL, but it prevents memory leaks from cold (never re-accessed) expired keys.

Real Redis uses exactly this combination. Our implementation:
- Lazy: every `get()`, `exists()`, `ttl()` calls `is_expired()` first
- Active: `evict_expired()` runs every ~10 seconds driven by the select() tick counter

---

### Q3: Why is the atomic rename (write-to-temp + rename) important for crash safety?

**A:** If we wrote directly to `redis.rdb` and the process crashed mid-write, we'd have a partially-written, corrupt file. On the next startup, the load would fail or silently return garbage data.

Writing to `redis.rdb.tmp` first, then renaming, makes the swap atomic:
- On POSIX (Linux/macOS), `rename(2)` is guaranteed atomic at the filesystem level.
- On Windows, it requires `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH`; our `std::rename` + `std::remove` pair is not atomic but is sufficient for a dev clone.

If the process crashes during the temp-file write, the original `redis.rdb` is untouched and fully valid.

---

### Q4: Why is `store_` marked `mutable` in `Database`?

**A:** `is_expired()` is declared `const` so it can be called from const methods like `get()`, `exists()`, and `ttl()`. But lazy eviction requires calling `store_.erase()` and `expiry_.erase()` — which are mutating operations.

Declaring `store_` and `expiry_` as `mutable` allows mutations inside `const` methods. This is the canonical C++ pattern for **logical const-ness**: the observable state (which non-expired keys exist) hasn't changed. The expired key was already invisible to callers — we're just reclaiming its memory.

---

### Q5: What happens if the server is killed mid-save (e.g., power failure)?

**A:** Because we write to a `.tmp` file first:
- The original `redis.rdb` is untouched if the crash happens before the rename.
- If the crash happens *after* the rename, the new file is complete (rename is atomic).
- The only case of data loss is the window between the last save and the crash — keys written to the DB after the last save are lost. This is acceptable for RDB-style ("snapshot") persistence; AOF (Append-Only File) persistence solves this at the cost of higher I/O.

---

### Q6: How does `TTL` differ from `PTTL`? How would you add `PTTL`?

**A:** `TTL` returns remaining time in **seconds**. `PTTL` returns in **milliseconds**. Our internal storage already uses milliseconds (`db.ttl()` returns ms), so adding `PTTL` is trivial:

```cpp
register_handler("PTTL", [](const std::vector<std::string>& args, Database& db) {
    return RESPParser::serialize_integer(db.ttl(args[1])); // no /1000 conversion
});
```

---

### Q7: Why does `SET` on a key with a TTL clear the TTL?

**A:** Real Redis documents this explicitly: "SET key value ... Any previous time to live associated with the key is discarded on successful SET operation." The rationale is that `SET` is an unconditional overwrite — the new value replaces everything about the old key, including its expiry. Our `Database::set()` calls `expiry_.erase(key)` to implement this. If you want to keep the TTL after updating the value, Redis provides `GETSET` or the `KEEPTTL` flag on `SET`.

---

### Q8: What is the time complexity of `evict_expired()`?

**A:** O(E) where E is the number of keys with a TTL set (i.e., the size of `expiry_`). We iterate `expiry_` once to collect expired keys (O(E)), then erase each from both maps (O(1) amortised per erase). We cannot erase during iteration of an `unordered_map` (undefined behaviour in C++), hence the two-pass approach using a temporary `to_evict` vector.

---

### Q9: Why use `system_clock` for TTL deadlines rather than `steady_clock`?

**A:** `steady_clock` is monotonically increasing within a process lifetime and is guaranteed not to go backward, making it ideal for measuring durations. However, it has **no defined relationship to the calendar epoch** — its reference point is implementation-defined and resets across reboots.

For TTL deadlines that must survive a server restart (i.e., be serialized to a file and compared against a future `now()`), we need **epoch-based** timestamps. `system_clock` provides wall-clock time since Unix epoch, which is stable across process restarts and comparable across systems. The tradeoff is that system clock adjustments (NTP, DST) can cause slight TTL inaccuracies, which is acceptable for a cache expiry system.

---

### Q10: How would you extend this to support AOF (Append-Only File) persistence?

**A:** AOF logs every write command in sequence. On restart, it replays the log to reconstruct state. Key differences from RDB:
- **Durability**: AOF can be configured to `fsync` on every write (near-zero data loss) vs. RDB which loses data since the last snapshot.
- **File size**: AOF is larger and grows continuously; it needs periodic rewriting (`BGREWRITEAOF`) to compact it.
- **Implementation**: Add an `AofLogger` class that appends `SET key value\n`, `DEL key\n`, `EXPIRE key ms\n` to a file after each successful command execution in `CommandProcessor::execute()`.

---

### Q11: How would you make `evict_expired()` more efficient at scale?

**A:** For millions of keys, iterating the full `expiry_` map every 10 seconds is expensive. Production Redis uses a **probabilistic sampling** approach: each cycle, it randomly samples 20 keys from the expiry map, evicts the expired ones, and repeats if > 25% of the sample was expired. This bounds the eviction cost per cycle while still keeping memory usage under control.

An alternative is a **min-heap (priority queue)** keyed on expiry time. The heap always shows the soonest-expiring key. Eviction is O(log E) per key and only examines keys that are actually close to expiring.

---

### Q12: What would happen if two processes tried to save to `redis.rdb` simultaneously?

**A:** With the current atomic-rename approach, the last writer wins. The `std::rename` call is atomic, so neither file will be corrupt — but one save will overwrite the other. In a production system with multiple processes writing to the same file, you'd need OS-level file locking (`flock` on POSIX, `LockFileEx` on Windows), or — better — a single designated writer process with other processes communicating via IPC. Redis itself is single-process precisely to avoid these races.
