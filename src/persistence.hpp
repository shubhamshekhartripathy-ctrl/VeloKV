#pragma once

#include "db.hpp"
#include <string>

namespace redis {

/**
 * @struct LoadResult
 * @brief Return value of PersistenceManager::load().
 *
 * Using a struct (rather than an out-parameter + bool) gives callers all the
 * information they need in one place and makes error handling explicit without
 * throwing exceptions — which is a pattern preferred in systems code.
 */
struct LoadResult {
    bool        success;      ///< true if the file was found and parsed without errors.
    int         keys_loaded;  ///< Number of non-expired keys that were restored.
    std::string error;        ///< Human-readable error message; empty on success.
};

/**
 * @class PersistenceManager
 * @brief Stateless utility class for saving and loading the Database to/from disk.
 *
 * DESIGN DECISIONS:
 *
 * 1. STATIC-ONLY / STATELESS:
 *    PersistenceManager has no instance state — it is purely a namespace of functions
 *    grouped into a class. This is intentional: persistence logic doesn't need to own
 *    any data itself; it borrows from Database and talks to the filesystem.
 *
 * 2. ATOMIC WRITES (crash safety):
 *    save() writes to a temporary file first, then renames it over the real file.
 *    Rename is an atomic operation on all POSIX systems and on Windows (with certain
 *    flags). This guarantees that a power failure during save never leaves a corrupt
 *    or partially-written .rdb file — the old file remains intact.
 *
 *    INTERVIEW NOTE: This is the same technique used by SQLite's journal mode,
 *    PostgreSQL's WAL, and Redis itself.
 *
 * 3. FILE FORMAT — Version 2 (human-readable text with type prefix):
 *    Each record line is prefixed with a single type character:
 *
 *    ┌────────────────────────────────────────┐
 *    │ REDIS-CLONE-RDB v2                     │  ← magic header
 *    │ S city -1 Rome                         │  ← String key, no TTL
 *    │ S session 1720000000000 token%3A42     │  ← String key with TTL
 *    │ L mylist -1 3 alpha beta gamma         │  ← List, 3 elements
 *    │ L queue 1720000000000 2 first second   │  ← List with TTL
 *    │ EOF                                    │  ← sentinel footer
 *    └────────────────────────────────────────┘
 *
 *    S line: S <enc_key> <expiry_ms_or_-1> <enc_value>
 *    L line: L <enc_key> <expiry_ms_or_-1> <count> <enc_elem0> <enc_elem1> ...
 *
 *    Backward compatibility: v1 files (no type prefix) are detected via the
 *    header line and loaded as all-string records.
 *
 * 4. EXPIRED-KEY FILTERING:
 *    On load, any record whose stored expiry_ms is in the past is silently discarded.
 *    This means the database is always in a consistent state regardless of how long
 *    the server was offline.
 */
class PersistenceManager {
public:
    // Prevent instantiation — this is a pure static utility class.
    PersistenceManager() = delete;

    /**
     * @brief Saves the entire database to disk atomically.
     *
     * Steps:
     *  1. Call db.snapshot() to get a stable list of all live records.
     *  2. Write header + records + footer to <path>.tmp.
     *  3. Atomically rename <path>.tmp → <path>.
     *
     * @param db   The database to snapshot.
     * @param path Destination file path (e.g. "redis.rdb").
     * @return true on success, false if any I/O error occurred.
     */
    static bool save(const Database& db, const std::string& path);

    /**
     * @brief Loads a previously saved database file into memory.
     *
     * Steps:
     *  1. Open the file. If it doesn't exist, return success with 0 keys loaded
     *     (normal first-start scenario).
     *  2. Validate the magic header line.
     *  3. Parse records. For each:
     *       - Decode percent-encoded key and value.
     *       - If expiry_ms != -1 and already expired → skip silently.
     *       - Otherwise: db.set(key, value) + optionally db.expire_at(key, expiry_ms).
     *  4. Validate the EOF sentinel line.
     *
     * @param db   The database to populate.
     * @param path Source file path (e.g. "redis.rdb").
     * @return LoadResult containing success flag, key count, and any error message.
     */
    static LoadResult load(Database& db, const std::string& path);

private:
    // Magic strings that delimit the file format.
    static constexpr const char* kHeader   = "REDIS-CLONE-RDB v2";
    static constexpr const char* kHeaderV1 = "REDIS-CLONE-RDB v1"; ///< backward compat
    static constexpr const char* kFooter   = "EOF";

    /**
     * @brief Encodes a string so it is safe for a single-line, space-separated format.
     *
     * Encoding table:
     *   '%'  → "%25"   (must be first to avoid double-encoding)
     *   ' '  → "%20"
     *   '\r' → "%0D"
     *   '\n' → "%0A"
     *
     * All other bytes pass through unchanged.
     */
    static std::string percent_encode(const std::string& s);

    /**
     * @brief Decodes a percent-encoded string back to its original bytes.
     *
     * @throws std::runtime_error if the input contains a malformed percent sequence.
     */
    static std::string percent_decode(const std::string& s);
};

} // namespace redis
