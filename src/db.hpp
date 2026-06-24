#pragma once
#include <list>
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

/**
 * @enum KeyType
 * @brief Identifies the Redis type stored under a key.
 *
 * INTERVIEW NOTE:
 * Real Redis calls this the "encoding" or "type". The TYPE command returns one
 * of: string, list, hash, set, zset, stream. Our clone supports string and list.
 *
 * DESIGN: We expose KeyType so the command layer can perform WRONGTYPE checks
 * (Redis returns `-WRONGTYPE ...` when you call a list command on a string key).
 * The storage layer never returns RESP-encoded errors — that is the command
 * layer's responsibility.
 */
enum class KeyType {
    None,   ///< Key does not exist (or has expired).
    String, ///< Value is a std::string.
    List    ///< Value is a std::deque<std::string>.
};

// ---------------------------------------------------------------------------
// Type aliases
// ---------------------------------------------------------------------------

/**
 * @brief A Redis List: a doubly-ended queue of strings.
 *
 * DESIGN: std::deque<std::string> is chosen over std::list<std::string> because:
 *  - O(1) push_front / push_back  → LPUSH, RPUSH
 *  - O(1) pop_front  / pop_back   → LPOP,  RPOP
 *  - O(1) size()                  → LLEN
 *  - Chunked memory layout: better cache performance than std::list (one heap
 *    node per element), while still allowing O(1) operations at both ends
 *    (which std::vector cannot provide for front operations).
 *
 * Real Redis uses a "quicklist": a doubly-linked list of "listpacks" (compact
 * byte arrays). For small lists (< 128 elements, each < 64 bytes) it stores a
 * single listpack. Our std::deque is the C++ standard-library equivalent:
 * internally chunked, O(1) at both ends.
 */
using RedisList = std::deque<std::string>;

// ---------------------------------------------------------------------------
// Snapshot entry
// ---------------------------------------------------------------------------

/**
 * @struct SnapshotEntry
 * @brief A plain-old-data snapshot of one database record for PersistenceManager.
 *
 * Updated in Milestone 5 to carry a type field and separate value fields for
 * strings and lists. The persistence layer uses type to emit the correct format
 * line (S for string, L for list).
 */
struct SnapshotEntry {
    std::string key;
    KeyType     type;       ///< String or List
    int64_t     expiry_ms;  ///< Absolute Unix deadline in ms, or -1 (no TTL)

    // Exactly one of these is populated (based on type):
    std::string str_value;   ///< Used when type == KeyType::String
    RedisList   list_value;  ///< Used when type == KeyType::List
};

// ---------------------------------------------------------------------------
// Database
// ---------------------------------------------------------------------------

/**
 * @class Database
 * @brief In-memory key-value store supporting string and list value types.
 *
 * ARCHITECTURE — Milestone 5 changes:
 *
 * 1. SPLIT STORES (mirrors real Redis internals):
 *    Previously: one unordered_map<string, string> for everything.
 *    Now:        string_store_ — holds all string-type keys.
 *                list_store_   — holds all list-type keys.
 *    A key can appear in at most one of the two stores. This separation:
 *      - Makes type_of() O(1): check which map contains the key.
 *      - Avoids runtime type-dispatch (no std::variant needed on GCC 6.3).
 *      - Mirrors how real Redis separates its per-type dictionaries internally.
 *
 * 2. TYPE SAFETY VIA TYPE_OF():
 *    Command handlers call type_of(key) first. If the key has the wrong type
 *    for the requested command, the handler returns a WRONGTYPE RESP error
 *    without calling any db method. This keeps the storage layer clean —
 *    it never produces error strings.
 *
 * 3. EMPTY LIST AUTO-DELETE:
 *    When the last element is popped from a list (lpop / rpop), the key is
 *    automatically deleted from list_store_ and expiry_. This is identical to
 *    real Redis behaviour: "A list is automatically created when we push an
 *    element to an empty key, and destroyed when we pop the last element."
 *
 * 4. TTL / EXPIRY:
 *    The expiry_ map is shared between both stores. is_expired() erases from
 *    whichever store holds the key. All TTL semantics (lazy + active eviction,
 *    absolute timestamps) are unchanged from Milestone 3.
 *
 * 5. SET OVERWRITES ANY TYPE:
 *    set(key, value) clears any existing list entry before inserting the string.
 *    This matches real Redis: "SET key value ... any previous type is replaced."
 */
class Database {
public:
    Database() = default;
    ~Database() = default;

    // Non-copyable: prevents duplicate state ownership.
    Database(const Database&)            = delete;
    Database& operator=(const Database&) = delete;

    // Movable: Server holds a Database member.
    Database(Database&&) noexcept        = default;
    Database& operator=(Database&&) noexcept = default;

    // -------------------------------------------------------------------------
    // Type inspection
    // -------------------------------------------------------------------------

    /**
     * @brief Returns the type of the value stored under key.
     *
     * Performs lazy expiry before the type lookup. Returns KeyType::None if the
     * key is absent or has expired.
     *
     * INTERVIEW NOTE: This is the equivalent of the Redis TYPE command internally.
     * Command handlers use this to enforce type safety before performing operations.
     */
    KeyType type_of(const std::string& key) const;

    // -------------------------------------------------------------------------
    // String operations
    // -------------------------------------------------------------------------

    /**
     * @brief Stores a string key-value pair.
     *
     * IMPORTANT: If the key previously held a list, the list is deleted. SET is
     * an unconditional overwrite of any existing type. Any previous TTL is also
     * cleared (identical to real Redis SET behaviour).
     *
     * Time Complexity: O(1) average.
     */
    void set(const std::string& key, const std::string& value);

    /**
     * @brief Retrieves the string value for a key.
     *
     * Returns false if the key is absent, expired, or holds a list (wrong type).
     * The command handler is responsible for calling type_of() first and
     * returning a WRONGTYPE error if the key holds a list.
     *
     * Time Complexity: O(1) average.
     */
    bool get(const std::string& key, std::string& out_value);

    /**
     * @brief Deletes a key of any type.
     *
     * @return true if the key existed (in either store), false if absent.
     * Time Complexity: O(1) average.
     */
    bool del(const std::string& key);

    /**
     * @brief Returns true if the key exists and is not expired (any type).
     *
     * Time Complexity: O(1) average.
     */
    bool exists(const std::string& key) const;

    // -------------------------------------------------------------------------
    // List operations
    // -------------------------------------------------------------------------
    // PRECONDITION for all list operations: the caller (command handler) has
    // already verified via type_of() that the key is not a String. These methods
    // do NOT perform type checking — they trust the caller.

    /**
     * @brief Pushes a value to the HEAD (left/front) of a list.
     *
     * Creates a new list if the key does not exist.
     *
     * @return The new length of the list after the push.
     * Time Complexity: O(1) — deque::push_front.
     *
     * INTERVIEW NOTE: Real Redis LPUSH pushes to the head. After:
     *   LPUSH mylist a
     *   LPUSH mylist b
     *   LPUSH mylist c
     * The list is [c, b, a] (c was pushed last → it is at the head/left).
     */
    int64_t lpush(const std::string& key, const std::string& value);

    /**
     * @brief Pushes a value to the TAIL (right/back) of a list.
     *
     * Creates a new list if the key does not exist.
     *
     * @return The new length of the list after the push.
     * Time Complexity: O(1) — deque::push_back.
     */
    int64_t rpush(const std::string& key, const std::string& value);

    /**
     * @brief Pops and returns the HEAD element of a list.
     *
     * If the list becomes empty after the pop, the key is automatically deleted.
     *
     * @return {true, popped_value} if an element was available.
     *         {false, ""} if the key does not exist or the list was empty.
     * Time Complexity: O(1) — deque::pop_front.
     *
     * DESIGN: Returns std::pair<bool, std::string> for GCC 6.3 compatibility
     * (std::optional requires GCC 7+). The bool indicates whether a value was
     * available. An empty return string is ambiguous — the bool disambiguates.
     */
    std::pair<bool, std::string> lpop(const std::string& key);

    /**
     * @brief Pops and returns the TAIL element of a list.
     *
     * If the list becomes empty after the pop, the key is automatically deleted.
     *
     * @return {true, popped_value} or {false, ""} (see lpop).
     * Time Complexity: O(1) — deque::pop_back.
     */
    std::pair<bool, std::string> rpop(const std::string& key);

    /**
     * @brief Returns the number of elements in a list.
     *
     * @return List length, or 0 if the key does not exist.
     *         (Returns 0 rather than an error for a non-existent key — matches Redis.)
     * Time Complexity: O(1) — deque::size.
     */
    int64_t llen(const std::string& key) const;

    // -------------------------------------------------------------------------
    // TTL / Expiry (unchanged from Milestone 3 — now works for both types)
    // -------------------------------------------------------------------------

    /**
     * @brief Sets a TTL relative to now (in milliseconds).
     *
     * Works on any key type (string or list). Internally stores an absolute deadline.
     *
     * @return true if the key existed and the TTL was set, false if key not found.
     */
    bool expire(const std::string& key, int64_t ttl_ms);

    /**
     * @brief Sets a TTL as an absolute Unix-millisecond timestamp.
     *
     * Used exclusively by PersistenceManager::load() to restore saved deadlines.
     */
    void expire_at(const std::string& key, int64_t abs_ms);

    /**
     * @brief Returns remaining TTL for a key.
     *
     * @return Remaining ms (>= 0), -1 (live, no TTL), or -2 (not found/expired).
     */
    int64_t ttl(const std::string& key) const;

    // -------------------------------------------------------------------------
    // Persistence / Snapshot
    // -------------------------------------------------------------------------

    /**
     * @brief Captures a consistent snapshot of all live key-value records.
     *
     * Returns entries for both string and list keys. Expired keys are excluded.
     * The PersistenceManager uses this to write the .rdb file.
     */
    std::vector<SnapshotEntry> snapshot() const;

    // -------------------------------------------------------------------------
    // Active Eviction
    // -------------------------------------------------------------------------

    /**
     * @brief Scans the expiry map and removes all expired keys (any type).
     *
     * Called periodically by the server event loop. O(E) where E is the number
     * of keys with an expiry set.
     */
    void evict_expired();

private:
    // ── Two separate stores — one per value type ───────────────────────────
    // INTERVIEW NOTE: This mirrors Redis's internal architecture where each
    // data type has its own dictionary/encoding. A key exists in exactly one
    // of these maps (never both).
    mutable std::unordered_map<std::string, std::string> string_store_;
    mutable std::unordered_map<std::string, RedisList>   list_store_;

    // ── Shared expiry index ────────────────────────────────────────────────
    // Stores absolute Unix-ms deadlines. Applies to keys in either store.
    // A key only appears here if it has an explicit TTL.
    mutable std::unordered_map<std::string, int64_t> expiry_;

    // THESE THREE LINES FOR LRU EVICTION CAPACITY POLICY:
    size_t max_capacity_ = 31; 
    std::list<std::string> lru_order_;
    std::unordered_map<std::string, std::list<std::string>::iterator> lru_position_;
    /**
     * @brief Checks whether a key's TTL has elapsed; evicts if expired.
     *
     * Looks up expiry_, compares against now_ms(). If expired, erases the key
     * from whichever of string_store_ or list_store_ holds it, then from expiry_.
     *
     * Declared const with mutable stores to support lazy eviction in const methods
     * (get, exists, ttl, type_of). Logical const-ness is preserved — an expired
     * key was already unobservable to callers.
     */
    bool is_expired(const std::string& key) const;

    /**
     * @brief Returns current wall-clock time as milliseconds since Unix epoch.
     *
     * Uses system_clock (not steady_clock) because we need epoch-based timestamps
     * that survive process restarts and are serialised to the .rdb file.
     */
    static int64_t now_ms() noexcept;
};

} // namespace redis
