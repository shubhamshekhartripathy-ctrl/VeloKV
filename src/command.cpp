#include "command.hpp"
#include "persistence.hpp"
#include "resp.hpp"
#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace redis {

// ---------------------------------------------------------------------------
// Module-level helper
// ---------------------------------------------------------------------------

// Converts a string to uppercase for case-insensitive command dispatch.
static std::string to_upper(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return str;
}

// ---------------------------------------------------------------------------
// CommandProcessor public API
// ---------------------------------------------------------------------------

CommandProcessor::CommandProcessor() {
    register_builtin_commands();
}

void CommandProcessor::register_handler(const std::string& name, CommandHandler handler) {
    handlers_[to_upper(name)] = std::move(handler);
}

std::string CommandProcessor::execute(const std::vector<std::string>& cmd, Database& db) {
    if (cmd.empty()) {
        return "";
    }

    std::string command_name = to_upper(cmd[0]);
    auto it = handlers_.find(command_name);
    if (it == handlers_.end()) {
        return RESPParser::serialize_error("ERR unknown command '" + cmd[0] + "'");
    }

    return it->second(cmd, db);
}

// ---------------------------------------------------------------------------
// Built-in Command Registrations
// ---------------------------------------------------------------------------

void CommandProcessor::register_builtin_commands() {

    // ── PING ──────────────────────────────────────────────────────────────
    // Standard Redis health-check command.
    // Usage:  PING           → +PONG
    //         PING <message> → <message> (as bulk string)
    register_handler("PING", [](const std::vector<std::string>& args, Database&) -> std::string {
        if (args.size() == 1) {
            return RESPParser::serialize_simple_string("PONG");
        } else if (args.size() == 2) {
            return RESPParser::serialize_bulk_string(args[1]);
        }
        return RESPParser::serialize_error("ERR wrong number of arguments for 'ping' command");
    });

    // ── SET ───────────────────────────────────────────────────────────────
    // Stores a key-value pair. Resets any existing TTL on the key.
    // Usage: SET <key> <value>
    //
    // INTERVIEW NOTE:
    // Real Redis SET accepts optional modifiers like EX, PX, NX, XX, KEEPTTL.
    // For this clone we implement the base form. The Database::set() method
    // already clears any existing TTL, matching Redis semantics.
    register_handler("SET", [](const std::vector<std::string>& args, Database& db) -> std::string {
        if (args.size() != 3) {
            return RESPParser::serialize_error(
                "ERR wrong number of arguments for 'set' command");
        }
        db.set(args[1], args[2]);
        return RESPParser::serialize_simple_string("OK");
    });

    // ── GET ───────────────────────────────────────────────────────────────
    // Retrieves the string value for a key.
    // Returns nil bulk string if the key does not exist.
    // Returns WRONGTYPE error if the key holds a non-string type (e.g. a list).
    // Usage: GET <key>
    register_handler("GET", [](const std::vector<std::string>& args, Database& db) -> std::string {
        if (args.size() != 2) {
            return RESPParser::serialize_error(
                "ERR wrong number of arguments for 'get' command");
        }
        // WRONGTYPE check: GET only works on string keys.
        // INTERVIEW NOTE: Real Redis returns this exact error string (the RESP
        // error prefix '-WRONGTYPE') when a command is applied to a key holding
        // a different data type than expected.
        if (db.type_of(args[1]) == KeyType::List) {
            return RESPParser::serialize_error(
                "WRONGTYPE Operation against a key holding the wrong kind of value");
        }
        std::string value;
        if (db.get(args[1], value)) {
            return RESPParser::serialize_bulk_string(value);
        }
        return RESPParser::serialize_null_bulk_string();
    });

    // ── DEL ───────────────────────────────────────────────────────────────
    // Deletes a key and its TTL. Standard Redis returns the count of deleted
    // keys as an integer; we return +OK per the project specification.
    // Usage: DEL <key>
    register_handler("DEL", [](const std::vector<std::string>& args, Database& db) -> std::string {
        if (args.size() != 2) {
            return RESPParser::serialize_error(
                "ERR wrong number of arguments for 'del' command");
        }
        db.del(args[1]);
        return RESPParser::serialize_simple_string("OK");
    });

    // ── EXISTS ────────────────────────────────────────────────────────────
    // Returns 1 if the key exists and is not expired, 0 otherwise.
    // Usage: EXISTS <key>
    register_handler("EXISTS", [](const std::vector<std::string>& args, Database& db) -> std::string {
        if (args.size() != 2) {
            return RESPParser::serialize_error(
                "ERR wrong number of arguments for 'exists' command");
        }
        return RESPParser::serialize_integer(db.exists(args[1]) ? 1 : 0);
    });

    // ── EXPIRE ────────────────────────────────────────────────────────────
    // Sets a TTL of N seconds on an existing key.
    //
    // INTERVIEW NOTE:
    // Real Redis uses seconds as the unit for EXPIRE and milliseconds for PEXPIRE.
    // We follow the same convention here: EXPIRE <key> <seconds>.
    // Internally we store millisecond-precision deadlines to allow future PEXPIRE.
    //
    // Return value:
    //   :1  — TTL was set (key exists and is live).
    //   :0  — Key does not exist (or is already expired); no TTL was set.
    //
    // Usage: EXPIRE <key> <seconds>
    register_handler("EXPIRE", [](const std::vector<std::string>& args, Database& db) -> std::string {
        if (args.size() != 3) {
            return RESPParser::serialize_error(
                "ERR wrong number of arguments for 'expire' command");
        }

        int64_t seconds = 0;
        try {
            seconds = std::stoll(args[2]);
        } catch (...) {
            return RESPParser::serialize_error(
                "ERR value is not an integer or out of range");
        }

        if (seconds < 0) {
            return RESPParser::serialize_error(
                "ERR invalid expire time in 'expire' command");
        }

        // Convert seconds → milliseconds for internal storage.
        bool ok = db.expire(args[1], seconds * 1000LL);
        return RESPParser::serialize_integer(ok ? 1 : 0);
    });

    // ── TTL ───────────────────────────────────────────────────────────────
    // Returns the remaining TTL for a key, in seconds.
    //
    // INTERVIEW NOTE: Return-value conventions (matching real Redis):
    //   >= 0  — remaining seconds until expiry.
    //   -1    — key exists but has no TTL (persists forever).
    //   -2    — key does not exist or has already expired.
    //
    // We call db.ttl() which returns milliseconds, then round to seconds.
    // Using integer division rounds down, which is the correct behaviour
    // (1900ms remaining → "1 second" TTL, not "2 seconds").
    //
    // Usage: TTL <key>
    register_handler("TTL", [](const std::vector<std::string>& args, Database& db) -> std::string {
        if (args.size() != 2) {
            return RESPParser::serialize_error(
                "ERR wrong number of arguments for 'ttl' command");
        }

        int64_t ttl_ms = db.ttl(args[1]);

        if (ttl_ms == -1) {
            return RESPParser::serialize_integer(-1); // No TTL.
        }
        if (ttl_ms == -2) {
            return RESPParser::serialize_integer(-2); // Not found.
        }

        // Convert milliseconds to whole seconds (floor division).
        int64_t ttl_seconds = ttl_ms / 1000LL;
        return RESPParser::serialize_integer(ttl_seconds);
    });

    // ── SAVE ──────────────────────────────────────────────────────────────
    // Triggers a manual, synchronous save of the database to disk.
    //
    // INTERVIEW NOTE:
    // Real Redis has both SAVE (blocking) and BGSAVE (background, fork-based).
    // We implement the blocking variant — appropriate for a single-threaded server
    // where we can't safely fork or spawn threads without a full threading model.
    //
    // Usage: SAVE
    static constexpr const char* kDbPath = "redis.rdb";
    register_handler("SAVE", [](const std::vector<std::string>& args, Database& db) -> std::string {
        if (args.size() != 1) {
            return RESPParser::serialize_error(
                "ERR wrong number of arguments for 'save' command");
        }
        bool ok = PersistenceManager::save(db, kDbPath);
        if (ok) {
            return RESPParser::serialize_simple_string("OK");
        }
        return RESPParser::serialize_error("ERR failed to save database to disk");
    });

    // =========================================================================
    // Milestone 5 — List commands
    // =========================================================================
    //
    // WRONGTYPE guard pattern used in all list handlers:
    //   db.type_of(key) == KeyType::String  → return WRONGTYPE error
    //   db.type_of(key) == KeyType::None    → key absent, list ops create it
    //   db.type_of(key) == KeyType::List    → proceed normally

    // ── LPUSH ─────────────────────────────────────────────────────────────
    // Inserts value at the HEAD (left/front) of the list stored at key.
    // Creates the list if the key does not exist.
    //
    // INTERVIEW NOTE:
    //   LPUSH mylist c  → [c]         (list created)
    //   LPUSH mylist b  → [b, c]
    //   LPUSH mylist a  → [a, b, c]   ← a is now the HEAD
    // The element pushed last ends up at the front. This is the expected "stack"  
    // behaviour when used with LPOP.
    //
    // Return value: integer — new length of the list.
    // Usage: LPUSH key value
    register_handler("LPUSH", [](const std::vector<std::string>& args, Database& db) -> std::string {
        if (args.size() != 3) {
            return RESPParser::serialize_error(
                "ERR wrong number of arguments for 'lpush' command");
        }
        if (db.type_of(args[1]) == KeyType::String) {
            return RESPParser::serialize_error(
                "WRONGTYPE Operation against a key holding the wrong kind of value");
        }
        return RESPParser::serialize_integer(db.lpush(args[1], args[2]));
    });

    // ── RPUSH ─────────────────────────────────────────────────────────────
    // Inserts value at the TAIL (right/back) of the list stored at key.
    // Creates the list if the key does not exist.
    //
    // INTERVIEW NOTE:
    //   RPUSH mylist a  → [a]
    //   RPUSH mylist b  → [a, b]
    //   RPUSH mylist c  → [a, b, c]   ← c is now the TAIL
    // This is natural queue (FIFO) insertion when used with LPOP.
    //
    // Return value: integer — new length of the list.
    // Usage: RPUSH key value
    register_handler("RPUSH", [](const std::vector<std::string>& args, Database& db) -> std::string {
        if (args.size() != 3) {
            return RESPParser::serialize_error(
                "ERR wrong number of arguments for 'rpush' command");
        }
        if (db.type_of(args[1]) == KeyType::String) {
            return RESPParser::serialize_error(
                "WRONGTYPE Operation against a key holding the wrong kind of value");
        }
        return RESPParser::serialize_integer(db.rpush(args[1], args[2]));
    });

    // ── LPOP ──────────────────────────────────────────────────────────────
    // Removes and returns the HEAD (left/front) element of the list.
    // If the list becomes empty after the pop, the key is automatically deleted.
    //
    // Return value: bulk string (popped element) or nil bulk string ($-1) if
    //   the key does not exist or the list is empty.
    // Usage: LPOP key
    register_handler("LPOP", [](const std::vector<std::string>& args, Database& db) -> std::string {
        if (args.size() != 2) {
            return RESPParser::serialize_error(
                "ERR wrong number of arguments for 'lpop' command");
        }
        if (db.type_of(args[1]) == KeyType::String) {
            return RESPParser::serialize_error(
                "WRONGTYPE Operation against a key holding the wrong kind of value");
        }
        std::pair<bool, std::string> result = db.lpop(args[1]);
        if (result.first) {
            return RESPParser::serialize_bulk_string(result.second);
        }
        return RESPParser::serialize_null_bulk_string();
    });

    // ── RPOP ──────────────────────────────────────────────────────────────
    // Removes and returns the TAIL (right/back) element of the list.
    // If the list becomes empty after the pop, the key is automatically deleted.
    //
    // Return value: bulk string or nil ($-1) if absent/empty.
    // Usage: RPOP key
    register_handler("RPOP", [](const std::vector<std::string>& args, Database& db) -> std::string {
        if (args.size() != 2) {
            return RESPParser::serialize_error(
                "ERR wrong number of arguments for 'rpop' command");
        }
        if (db.type_of(args[1]) == KeyType::String) {
            return RESPParser::serialize_error(
                "WRONGTYPE Operation against a key holding the wrong kind of value");
        }
        std::pair<bool, std::string> result = db.rpop(args[1]);
        if (result.first) {
            return RESPParser::serialize_bulk_string(result.second);
        }
        return RESPParser::serialize_null_bulk_string();
    });

    // ── LLEN ──────────────────────────────────────────────────────────────
    // Returns the number of elements in the list stored at key.
    //
    // INTERVIEW NOTE:
    //   - Returns 0 (not an error) if the key does not exist — matches real Redis.
    //   - WRONGTYPE error if the key exists and holds a string.
    //   - O(1): std::deque::size() is constant time.
    //
    // Return value: integer — list length.
    // Usage: LLEN key
    register_handler("LLEN", [](const std::vector<std::string>& args, Database& db) -> std::string {
        if (args.size() != 2) {
            return RESPParser::serialize_error(
                "ERR wrong number of arguments for 'llen' command");
        }
        if (db.type_of(args[1]) == KeyType::String) {
            return RESPParser::serialize_error(
                "WRONGTYPE Operation against a key holding the wrong kind of value");
        }
        return RESPParser::serialize_integer(db.llen(args[1]));
    });
}

} // namespace redis
