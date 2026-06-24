#include "persistence.hpp"

#include <fstream>
#include <sstream>
#include <iostream>
#include <stdexcept>
#include <cstdio>    // std::rename, std::remove
#include <cctype>    // std::isxdigit, std::toupper
#include <chrono>

namespace redis {

// ---------------------------------------------------------------------------
// Percent Encoding / Decoding (unchanged from Milestone 3)
// ---------------------------------------------------------------------------

std::string PersistenceManager::percent_encode(const std::string& s) {
    // Encode only the characters that would break our line-based, space-tokenised
    // format. This is O(N) and minimal — we don't encode every non-ASCII byte.
    //   % → %25   (must be first to prevent double-encoding)
    //   space → %20
    //   \r → %0D
    //   \n → %0A
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        switch (c) {
            case '%':  out += "%25"; break;
            case ' ':  out += "%20"; break;
            case '\r': out += "%0D"; break;
            case '\n': out += "%0A"; break;
            default:   out += static_cast<char>(c); break;
        }
    }
    return out;
}

std::string PersistenceManager::percent_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());

    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%') {
            if (i + 2 >= s.size()) {
                throw std::runtime_error(
                    "PersistenceManager: malformed percent-encoding at position " +
                    std::to_string(i));
            }

            char hi = static_cast<char>(std::toupper(static_cast<unsigned char>(s[i + 1])));
            char lo = static_cast<char>(std::toupper(static_cast<unsigned char>(s[i + 2])));

            if (!std::isxdigit(static_cast<unsigned char>(hi)) ||
                !std::isxdigit(static_cast<unsigned char>(lo))) {
                throw std::runtime_error(
                    "PersistenceManager: non-hex digit in percent sequence at position " +
                    std::to_string(i));
            }

            auto hex_to_val = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return 0;
            };

            out += static_cast<char>((hex_to_val(hi) << 4) | hex_to_val(lo));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Save — writes v2 format with S/L type prefix per record
// ---------------------------------------------------------------------------

bool PersistenceManager::save(const Database& db, const std::string& path) {
    // Step 1: Write to temp file (crash-safe).
    const std::string tmp_path = path + ".tmp";

    std::ofstream ofs(tmp_path, std::ios::out | std::ios::trunc);
    if (!ofs.is_open()) {
        std::cerr << "[Persistence] ERROR: Cannot open temp file for writing: "
                  << tmp_path << std::endl;
        return false;
    }

    // Step 2: Write v2 magic header.
    ofs << kHeader << "\n";

    // Step 3: Snapshot all live records (already filters expired keys).
    std::vector<SnapshotEntry> entries = db.snapshot();
    int written = 0;

    for (const auto& entry : entries) {
        if (entry.type == KeyType::String) {
            // Format: S <enc_key> <expiry_ms> <enc_value>
            ofs << "S "
                << percent_encode(entry.key)       << " "
                << entry.expiry_ms                 << " "
                << percent_encode(entry.str_value) << "\n";

        } else {
            // Format: L <enc_key> <expiry_ms> <count> <enc_elem0> <enc_elem1> ...
            //
            // Elements are stored in list order: index 0 (front/head) first.
            // On reload, we use rpush() to restore them in the same order.
            //
            // INTERVIEW NOTE: The count field is redundant given that the loader
            // reads exactly count space-separated tokens, but it makes the format
            // self-describing and lets a human verify the file without parsing.
            ofs << "L "
                << percent_encode(entry.key) << " "
                << entry.expiry_ms           << " "
                << entry.list_value.size();

            for (const auto& elem : entry.list_value) {
                ofs << " " << percent_encode(elem);
            }
            ofs << "\n";
        }
        ++written;
    }

    // Step 4: Write EOF sentinel.
    ofs << kFooter << "\n";

    if (ofs.fail()) {
        std::cerr << "[Persistence] ERROR: Write failed to temp file: "
                  << tmp_path << std::endl;
        ofs.close();
        std::remove(tmp_path.c_str());
        return false;
    }
    ofs.close();

    // Step 5: Atomic rename.
    // On POSIX, rename(2) is guaranteed atomic. On Windows, std::rename fails
    // if the destination exists — remove it first (not atomic but acceptable).
#ifdef _WIN32
    std::remove(path.c_str());
#endif

    if (std::rename(tmp_path.c_str(), path.c_str()) != 0) {
        std::cerr << "[Persistence] ERROR: Failed to rename temp file to: "
                  << path << std::endl;
        std::remove(tmp_path.c_str());
        return false;
    }

    std::cout << "[Persistence] Saved " << written
              << " key(s) to '" << path << "'." << std::endl;
    return true;
}

// ---------------------------------------------------------------------------
// Load — handles v1 (all strings) and v2 (S/L type prefix)
// ---------------------------------------------------------------------------

LoadResult PersistenceManager::load(Database& db, const std::string& path) {
    std::ifstream ifs(path);

    if (!ifs.is_open()) {
        std::cout << "[Persistence] No existing database file found at '"
                  << path << "'. Starting with empty database." << std::endl;
        return {true, 0, ""};
    }

    std::string line;

    // Step 1: Read and validate header.
    if (!std::getline(ifs, line)) {
        return {false, 0, "File is empty or unreadable."};
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();

    bool is_v2 = (line == kHeader);    // "REDIS-CLONE-RDB v2"
    bool is_v1 = (line == kHeaderV1);  // "REDIS-CLONE-RDB v1" (backward compat)

    if (!is_v2 && !is_v1) {
        return {false, 0,
                "Unknown file format. Expected '" + std::string(kHeader) +
                "' or '" + std::string(kHeaderV1) + "', got: '" + line + "'"};
    }

    // Step 2: Snapshot time for expiry filtering.
    // We capture now() once so all keys in this load cycle are compared against
    // the same reference point — avoids a subtle clock drift bug.
    using namespace std::chrono;
    int64_t current_ms =
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();

    int  keys_loaded  = 0;
    bool found_footer = false;

    // Step 3: Parse records.
    while (std::getline(ifs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line == kFooter) { found_footer = true; break; }
        if (line.empty())    continue;

        // ── Determine type character and the payload ───────────────────────
        // v2: lines begin with "S " or "L "
        // v1: all lines are string records with no prefix
        char        type_ch = 'S';
        std::string record  = line;

        if (is_v2) {
            if (line.size() < 3 || line[1] != ' ') {
                std::cerr << "[Persistence] WARNING: Skipping malformed v2 record: "
                          << line << std::endl;
                continue;
            }
            type_ch = line[0];
            record  = line.substr(2); // strip "S " or "L "
        }
        // For v1, record == line and type_ch == 'S' (all records are strings).

        // ── Parse shared fields: <enc_key> <expiry_ms> <rest...> ──────────
        std::size_t s1 = record.find(' ');
        if (s1 == std::string::npos) {
            std::cerr << "[Persistence] WARNING: Skipping malformed record (no space after key): "
                      << record << std::endl;
            continue;
        }

        std::size_t s2 = record.find(' ', s1 + 1);
        if (s2 == std::string::npos) {
            std::cerr << "[Persistence] WARNING: Skipping malformed record (no expiry field): "
                      << record << std::endl;
            continue;
        }

        std::string enc_key    = record.substr(0, s1);
        std::string expiry_str = record.substr(s1 + 1, s2 - s1 - 1);
        std::string rest       = record.substr(s2 + 1); // value OR "count elem0 elem1..."

        // Decode key.
        std::string key;
        try {
            key = percent_decode(enc_key);
        } catch (const std::runtime_error& e) {
            std::cerr << "[Persistence] WARNING: Skipping record with bad key encoding: "
                      << e.what() << std::endl;
            continue;
        }

        // Parse expiry.
        int64_t expiry_ms = -1;
        try {
            expiry_ms = std::stoll(expiry_str);
        } catch (...) {
            std::cerr << "[Persistence] WARNING: Skipping record with invalid expiry '"
                      << expiry_str << "'" << std::endl;
            continue;
        }

        // Expired-key filtering: the server was offline, but time kept ticking.
        if (expiry_ms != -1 && current_ms >= expiry_ms) {
            continue; // Key expired while server was down — do not load.
        }

        // ── Type-specific deserialization ──────────────────────────────────

        if (type_ch == 'S') {
            // String record: rest is the percent-encoded value.
            std::string value;
            try {
                value = percent_decode(rest);
            } catch (const std::runtime_error& e) {
                std::cerr << "[Persistence] WARNING: Skipping string record with bad value: "
                          << e.what() << std::endl;
                continue;
            }
            db.set(key, value);
            if (expiry_ms != -1) db.expire_at(key, expiry_ms);
            ++keys_loaded;

        } else if (type_ch == 'L') {
            // List record: rest = "<count> <enc_elem0> <enc_elem1> ..."
            //
            // Elements were saved front-to-back (index 0 first).
            // We restore them using rpush() so the order is preserved:
            //   Saved:  [a, b, c]  → file: "3 a b c"
            //   Loaded: rpush(a) → [a], rpush(b) → [a,b], rpush(c) → [a,b,c] ✓

            std::size_t count_sep = rest.find(' ');
            std::string count_str = (count_sep == std::string::npos)
                                        ? rest
                                        : rest.substr(0, count_sep);
            std::string elems_str = (count_sep == std::string::npos)
                                        ? std::string()
                                        : rest.substr(count_sep + 1);

            int count = 0;
            try {
                count = std::stoi(count_str);
            } catch (...) {
                std::cerr << "[Persistence] WARNING: Skipping list record with bad count '"
                          << count_str << "'" << std::endl;
                continue;
            }

            if (count <= 0) continue; // Empty list — should not exist (auto-delete).

            // Parse and push each element.
            std::string remaining = elems_str;
            bool decode_error = false;

            for (int i = 0; i < count; ++i) {
                std::size_t sep = remaining.find(' ');
                std::string enc_elem = (sep == std::string::npos)
                                           ? remaining
                                           : remaining.substr(0, sep);
                if (sep != std::string::npos) {
                    remaining = remaining.substr(sep + 1);
                } else {
                    remaining.clear();
                }

                try {
                    db.rpush(key, percent_decode(enc_elem));
                } catch (const std::runtime_error& e) {
                    std::cerr << "[Persistence] WARNING: Skipping list element with bad encoding: "
                              << e.what() << std::endl;
                    decode_error = true;
                    break;
                }
            }

            if (decode_error) {
                // Partial list was pushed — remove it to avoid inconsistent state.
                db.del(key);
                continue;
            }

            if (expiry_ms != -1) db.expire_at(key, expiry_ms);
            ++keys_loaded;

        } else {
            std::cerr << "[Persistence] WARNING: Unknown type prefix '"
                      << type_ch << "' — skipping." << std::endl;
        }
    }

    // Step 4: Validate EOF sentinel.
    if (!found_footer) {
        std::cerr << "[Persistence] WARNING: EOF sentinel missing. "
                     "File may be truncated. Loaded "
                  << keys_loaded << " key(s)." << std::endl;
        return {true, keys_loaded,
                "File truncated: EOF marker not found. Partial load completed."};
    }

    std::cout << "[Persistence] Loaded " << keys_loaded
              << " key(s) from '" << path << "'." << std::endl;
    return {true, keys_loaded, ""};
}

} // namespace redis
