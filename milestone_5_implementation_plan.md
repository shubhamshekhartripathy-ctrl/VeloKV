# Milestone 5 — Redis-style Lists

## Overview

Milestone 5 adds five Redis-compatible List commands to the C++17 TCP server built in Milestones 1–4. Lists are an ordered collection of strings accessible from both ends in O(1) time — the foundation for queues, stacks, and pub/sub-style message pipelines in real Redis.

Commands added:
- `LPUSH key value` — insert at the HEAD (left/front)
- `RPUSH key value` — insert at the TAIL (right/back)
- `LPOP  key`       — remove and return HEAD element
- `RPOP  key`       — remove and return TAIL element
- `LLEN  key`       — return list length

---

## Architecture

### Core Design: Dual Store

Before Milestone 5, the Database held a single map:

```
unordered_map<string, string>  store_
```

In Milestone 5 this becomes two separate maps:

```
unordered_map<string, string>    string_store_   // String keys
unordered_map<string, RedisList> list_store_     // List keys
                                                  // (RedisList = deque<string>)
```

**Why two maps instead of `std::variant`?**

| Option | Pros | Cons |
|---|---|---|
| Two separate maps | Simple, explicit, O(1) type check via `count()`, mirrors Redis internals | Slightly more memory per empty map |
| `std::variant<string, deque<string>>` | One map, type-safe | Requires GCC 7+; `std::variant` with non-trivial types has subtle pitfalls; `std::visit` adds boilerplate |
| Tagged union (manual) | Works everywhere | Error-prone, verbose |

Two maps was chosen: simpler code, GCC 6.3 compatible, and mirrors how real Redis internally has separate dictionaries per encoding type.

### Key Invariants

1. A key exists in **at most one** of the two stores.
2. `SET` on any key erases it from `list_store_` before inserting into `string_store_`.
3. When `lpop`/`rpop` empties a list, the key is **automatically deleted** — matching real Redis behaviour ("a list disappears when its last element is popped").
4. `expiry_` is shared: TTLs apply to keys in either store.

### Type Safety: WRONGTYPE Pattern

Every list command handler calls `db.type_of(key)` before any DB mutation:

```
client sends: LPUSH strkey value
         ↓
command handler: type_of("strkey") == KeyType::String
         ↓
return: -WRONGTYPE Operation against a key holding the wrong kind of value
```

The storage layer (`db.cpp`) never produces error strings — that is exclusively the command layer's responsibility.

### Request Lifecycle (updated for lists)

```
TCP Client
    │
    ▼
recv() / RESPParser::parse()
    │  Parses inline command: "LPUSH mylist hello\r\n"
    ▼
CommandProcessor::execute()
    │  Looks up "LPUSH" handler
    ▼
LPUSH handler
    │  1. Check arg count (must be 3)
    │  2. db.type_of(key) — abort with WRONGTYPE if String
    │  3. db.lpush(key, value) — returns new list length
    │  4. RESPParser::serialize_integer(len)
    ▼
send() back to client
```

---

## Data Structure Choice: `std::deque<std::string>`

### Why `std::deque` over alternatives?

| Structure | push_front | push_back | pop_front | pop_back | Notes |
|---|---|---|---|---|---|
| `std::deque<string>` | O(1) | O(1) | O(1) | O(1) | **Chosen** |
| `std::list<string>` | O(1) | O(1) | O(1) | O(1) | Heap node per element → cache hostile |
| `std::vector<string>` | O(N) | O(1) amort. | O(N) | O(1) | Front ops shift all elements |

`std::deque` is chunked: internally an array of fixed-size blocks. This gives:
- O(1) at both ends (LPUSH/RPUSH, LPOP/RPOP)
- O(1) `size()` (LLEN)
- Better cache performance than `std::list` (no per-element heap allocation)

### Return Type: `std::pair<bool, std::string>` instead of `std::optional`

`std::optional<std::string>` was introduced in C++17 but requires GCC 7+. Our toolchain targets GCC 6.3 (MinGW on Windows). We use `std::pair<bool, std::string>` where the `bool` indicates whether a value was available — semantically equivalent, fully portable.

### Real Redis Storage (for context)

Real Redis uses a **quicklist**: a doubly-linked list of **listpacks** (compact byte arrays). For small lists (< 128 elements, each < 64 bytes) the entire list fits in a single listpack. For larger lists, elements spill into additional listpack nodes chained together. Our `std::deque` is the closest C++ standard-library equivalent: internally chunked, O(1) at both ends.

---

## Persistence — v2 Format

The `redis.rdb` file format was bumped from v1 to v2 to accommodate list records.

### File Structure

```
REDIS-CLONE-RDB v2          ← magic header (version sentinel)
S mykey -1 myvalue          ← String record (no TTL)
S withttl 1750000000000 hi  ← String record with TTL (Unix ms deadline)
L mylist -1 3 a b c         ← List record: count=3, elements: a, b, c
L explist 1750000099000 2 x y  ← List record with TTL
EOF                         ← footer sentinel
```

### Record Formats

**String:** `S <percent_encoded_key> <expiry_ms|-1> <percent_encoded_value>`

**List:** `L <percent_encoded_key> <expiry_ms|-1> <count> <elem0> <elem1> ...`
- Elements are stored front-to-back (index 0 = head)
- On load, `rpush()` is called for each element, preserving original order
- The `count` field is redundant but makes the format human-readable

### Encoding

Keys, values, and list elements are percent-encoded:
- `%` → `%25` (always first to prevent double-encoding)
- space → `%20`
- `\r` → `%0D`
- `\n` → `%0A`

### Backward Compatibility

The loader detects `REDIS-CLONE-RDB v1` (old format) and treats all records as String records (no `S`/`L` prefix). This allows seamless upgrade from Milestone 3 databases.

### Crash Safety

Save writes to `redis.rdb.tmp` first, then renames to `redis.rdb`. On Windows, `std::rename` fails if the destination exists, so we `remove()` it first (not atomic but adequate for this prototype).

---

## Files Changed

### `src/db.hpp` — MODIFIED

- Added `enum class KeyType { None, String, List }`
- Added `using RedisList = std::deque<std::string>`
- Updated `SnapshotEntry` with `type`, `str_value`, `list_value` fields
- Split `store_` into `string_store_` + `list_store_`
- Added declarations: `type_of()`, `lpush()`, `rpush()`, `lpop()`, `rpop()`, `llen()`
- Extensive Doxygen comments on every method

### `src/db.cpp` — MODIFIED

- `set()`: erases from `list_store_` before inserting into `string_store_`
- `get()`: unchanged logic, searches only `string_store_`
- `del()`: erases from both stores
- `exists()`: checks both stores
- `expire()` / `expire_at()` / `ttl()`: check both stores
- `is_expired()`: erases from both stores on expiry
- `snapshot()`: iterates both stores, produces typed `SnapshotEntry` records
- `evict_expired()`: erases from both stores
- New: `type_of()`, `lpush()`, `rpush()`, `lpop()`, `rpop()`, `llen()`

### `src/command.cpp` — MODIFIED

- `GET` handler: added WRONGTYPE guard for list keys
- Added 5 new handlers: `LPUSH`, `RPUSH`, `LPOP`, `RPOP`, `LLEN`

### `src/persistence.hpp` — MODIFIED

- `kHeader` constant bumped to `"REDIS-CLONE-RDB v2"`
- Added `kHeaderV1 = "REDIS-CLONE-RDB v1"` for backward-compat detection

### `src/persistence.cpp` — MODIFIED

- `save()`: emits `S`/`L` type prefix per record; lists use count + element format
- `load()`: detects v1/v2 header; parses `S` and `L` records; calls `rpush()` to restore list order; backward-compatible v1 path unchanged

### Files NOT changed

- `src/server.cpp`, `src/client.cpp`, `src/resp.cpp`, `src/common.cpp` — networking layer is unchanged
- `src/command.hpp`, `src/resp.hpp`, `src/client.hpp`, `src/server.hpp` — headers unchanged
- `Makefile` — unchanged (all .cpp files are already in the compile rule)

---

## Complexity Summary

| Command | Time | Space | Notes |
|---|---|---|---|
| LPUSH | O(1) | O(1) | `deque::push_front` |
| RPUSH | O(1) | O(1) | `deque::push_back` |
| LPOP | O(1) | O(1) | `deque::pop_front` |
| RPOP | O(1) | O(1) | `deque::pop_back` |
| LLEN | O(1) | O(1) | `deque::size` |
| EXPIRE (list) | O(1) | O(1) | shared expiry map |
| Snapshot (save) | O(K) | O(K) | K = total keys |
| Load | O(K + E) | O(K + E) | E = total list elements |

---

## Verification Plan

### Build
```
mingw32-make clean
mingw32-make
```
Expected: zero warnings with `-Wall -Wextra -Wpedantic`.

### Automated Test Script
```powershell
cd d:\redis
powershell -ExecutionPolicy Bypass -File test_milestone_5.ps1
```
42 tests covering: RPUSH/LLEN, LPUSH, LPOP (stack), RPOP, RPUSH+LPOP FIFO queue, WRONGTYPE on all 5 commands + GET, SET overwrite, TTL on list, persistence save/load.

### Manual Verification (ncat)
```
ncat 127.0.0.1 6379
RPUSH mylist a
RPUSH mylist b
RPUSH mylist c
LLEN mylist
LPOP mylist
RPOP mylist
SAVE
```

---

## Open Questions / Design Decisions

| Decision | Choice Made | Rationale |
|---|---|---|
| Variant vs dual map | Dual map | GCC 6.3 compat; simpler; mirrors Redis |
| `optional` vs `pair<bool,T>` | `pair<bool,T>` | GCC 6.3 compat (`optional` needs GCC 7+) |
| `deque` vs `list` | `deque` | Better cache perf; same O(1) at ends |
| LLEN on absent key | Returns 0, not error | Matches real Redis behaviour |
| Empty list after pop | Auto-delete key | Matches real Redis behaviour |
| LRANGE, LINDEX | Not implemented | Out of scope for Milestone 5 |
