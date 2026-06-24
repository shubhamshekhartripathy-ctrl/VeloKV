#include "db.hpp"
// std::erase_if is C++20; we use the manual two-pass approach (collect + erase)
// for GCC 6.3 / C++17 compatibility throughout this file.

namespace redis {

// ---------------------------------------------------------------------------
// Private static helpers
// ---------------------------------------------------------------------------

int64_t Database::now_ms() noexcept {
    // system_clock is epoch-based (wall clock).  steady_clock is NOT suitable
    // here because it has no defined relationship to the Unix epoch and resets
    // across process restarts — so stored deadlines would become meaningless.
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

bool Database::is_expired(const std::string& key) const {
    auto it = expiry_.find(key);
    if (it == expiry_.end()) {
        return false; // No TTL set for this key.
    }

    if (now_ms() >= it->second) {
        // Deadline has passed — lazy eviction:
        // Erase from whichever store holds this key (possibly neither if the
        // key was already evicted by a previous is_expired call, which is safe).
        string_store_.erase(key);
        list_store_.erase(key);
        expiry_.erase(it);
        // 🚨 CAST to non-const to satisfy the compiler's strictness
        auto& mutable_lru_order = const_cast<std::list<std::string>&>(lru_order_);
        auto& mutable_lru_pos = const_cast<std::unordered_map<std::string, std::list<std::string>::iterator>&>(lru_position_);
        //  SAFE FOR CONST FUNCTIONS: Use .find() instead of []
        auto lru_it = mutable_lru_pos.find(key);
        if (lru_it != mutable_lru_pos.end()) {
            mutable_lru_order.erase(lru_it->second);
            mutable_lru_pos.erase(lru_it);
        }
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// Type inspection
// ---------------------------------------------------------------------------

KeyType Database::type_of(const std::string& key) const {
    // Check expiry first — an expired key must report as None.
    if (is_expired(key)) return KeyType::None;

    if (string_store_.count(key)) return KeyType::String;
    if (list_store_.count(key))   return KeyType::List;
    return KeyType::None;
}

// ---------------------------------------------------------------------------
// String operations
// ---------------------------------------------------------------------------

void Database::set(const std::string& key, const std::string& value) {
    // 1. Remove any existing list/string entries and clear TTL
    list_store_.erase(key);
    expiry_.erase(key);

    // If it's a completely NEW key, handle LRU capacity
    if (string_store_.find(key) == string_store_.end()) {
        // Check if we hit our max memory limit (31)
        while (string_store_.size() + list_store_.size() >= max_capacity_) {
            // Get the least recently used key from the tail of your LRU list
            std::string evicted_key = lru_order_.back();
            
            // Purge the evicted key from EVERY storage map
            string_store_.erase(evicted_key);
            list_store_.erase(evicted_key);
            expiry_.erase(evicted_key);
            lru_position_.erase(evicted_key);
            
            lru_order_.pop_back(); // Remove from tracking list
        }
        
        // Insert new key into the front of the LRU tracking list
        lru_order_.push_front(key);
        lru_position_[key] = lru_order_.begin();
    } else {
        // If the key already existed as a string, just move it to the front (MRU)
        lru_order_.erase(lru_position_[key]);
        lru_order_.push_front(key);
        lru_position_[key] = lru_order_.begin();
    }

    // 2. Perform the actual data write
    string_store_[key] = value;
}

// 1. Remove "const" from the end of the signature
bool Database::get(const std::string& key, std::string& out_value) {
    if (is_expired(key)) return false;

    auto it = string_store_.find(key);
    if (it == string_store_.end()) return false;

    out_value = it->second;

    // 🚨 UPDATE LRU ORDER: Move the accessed key to the front (Most Recently Used)
    if (lru_position_.count(key)) {
        lru_order_.erase(lru_position_[key]);
        lru_order_.push_front(key);
        lru_position_[key] = lru_order_.begin();
    }

    return true;
}

bool Database::del(const std::string& key) {
    expiry_.erase(key);
    // Erase from both stores; at most one will contain the key.
    size_t removed = string_store_.erase(key) + list_store_.erase(key);
    return removed > 0;
}

bool Database::exists(const std::string& key) const {
    if (is_expired(key)) return false;
    // A key exists if it is in either store.
    return string_store_.count(key) > 0 || list_store_.count(key) > 0;
}

// ---------------------------------------------------------------------------
// List operations
// ---------------------------------------------------------------------------

// PRECONDITION (enforced by command handler):
//   The caller has called type_of(key) and confirmed it is NOT KeyType::String.
//   These methods do NOT perform type checking.

int64_t Database::lpush(const std::string& key, const std::string& value) {
    // operator[] on unordered_map inserts a default-constructed (empty) deque
    // if the key is absent, then returns a reference to it. This handles both
    // "key does not exist" (create list) and "key is an existing list" (append).
    list_store_[key].push_front(value);
    return static_cast<int64_t>(list_store_[key].size());
}

int64_t Database::rpush(const std::string& key, const std::string& value) {
    list_store_[key].push_back(value);
    return static_cast<int64_t>(list_store_[key].size());
}

std::pair<bool, std::string> Database::lpop(const std::string& key) {
    if (is_expired(key)) return {false, ""};

    auto it = list_store_.find(key);
    if (it == list_store_.end() || it->second.empty()) {
        return {false, ""};
    }

    // Move the front element out before popping (avoids a copy).
    std::string val = std::move(it->second.front());
    it->second.pop_front();

    // Auto-delete: Redis removes the key when the list becomes empty.
    // INTERVIEW NOTE: "When we push elements into an empty key, a list is
    // automatically created. When we pop the last element, the list disappears."
    if (it->second.empty()) {
        list_store_.erase(it);
        expiry_.erase(key);
    }

    return {true, std::move(val)};
}

std::pair<bool, std::string> Database::rpop(const std::string& key) {
    if (is_expired(key)) return {false, ""};

    auto it = list_store_.find(key);
    if (it == list_store_.end() || it->second.empty()) {
        return {false, ""};
    }

    std::string val = std::move(it->second.back());
    it->second.pop_back();

    if (it->second.empty()) {
        list_store_.erase(it);
        expiry_.erase(key);
    }

    return {true, std::move(val)};
}

int64_t Database::llen(const std::string& key) const {
    if (is_expired(key)) return 0;

    auto it = list_store_.find(key);
    if (it == list_store_.end()) {
        // Key does not exist (or is a String — caller checked type_of already).
        return 0;
    }
    return static_cast<int64_t>(it->second.size());
}

// ---------------------------------------------------------------------------
// TTL / Expiry
// ---------------------------------------------------------------------------

bool Database::expire(const std::string& key, int64_t ttl_ms) {
    if (is_expired(key)) return false;

    // EXPIRE works on any type — check both stores.
    bool found = string_store_.count(key) > 0 || list_store_.count(key) > 0;
    if (!found) return false;

    // Store absolute deadline: now + duration.
    expiry_[key] = now_ms() + ttl_ms;
    return true;
}

void Database::expire_at(const std::string& key, int64_t abs_ms) {
    // Used by PersistenceManager::load() to restore a serialised deadline.
    // We trust the caller has already filtered out already-expired entries.
    bool found = string_store_.count(key) > 0 || list_store_.count(key) > 0;
    if (found) {
        expiry_[key] = abs_ms;
    }
}

int64_t Database::ttl(const std::string& key) const {
    if (is_expired(key)) return -2;

    bool found = string_store_.count(key) > 0 || list_store_.count(key) > 0;
    if (!found) return -2;

    auto it = expiry_.find(key);
    if (it == expiry_.end()) return -1; // Key exists but has no TTL.

    int64_t remaining = it->second - now_ms();
    // Clamp to 0 to handle the tiny race between is_expired returning false
    // and computing the delta.
    return remaining > 0 ? remaining : 0;
}

// ---------------------------------------------------------------------------
// Persistence / Snapshot
// ---------------------------------------------------------------------------

std::vector<SnapshotEntry> Database::snapshot() const {
    std::vector<SnapshotEntry> entries;
    entries.reserve(string_store_.size() + list_store_.size());

    int64_t current_ms = now_ms();

    // ── String entries ────────────────────────────────────────────────────
    for (auto it = string_store_.begin(); it != string_store_.end(); ++it) {
        const std::string& key = it->first;

        auto exp_it = expiry_.find(key);
        int64_t expiry_ms = -1;
        if (exp_it != expiry_.end()) {
            if (current_ms >= exp_it->second) continue; // already expired
            expiry_ms = exp_it->second;
        }

        SnapshotEntry e;
        e.key       = key;
        e.type      = KeyType::String;
        e.expiry_ms = expiry_ms;
        e.str_value = it->second;
        // e.list_value is default-constructed (empty deque) — unused for strings.
        entries.push_back(e);
    }

    // ── List entries ──────────────────────────────────────────────────────
    for (auto it = list_store_.begin(); it != list_store_.end(); ++it) {
        const std::string& key = it->first;

        auto exp_it = expiry_.find(key);
        int64_t expiry_ms = -1;
        if (exp_it != expiry_.end()) {
            if (current_ms >= exp_it->second) continue;
            expiry_ms = exp_it->second;
        }

        // Skip empty lists — they should never exist in list_store_ (auto-delete
        // on lpop/rpop ensures this), but be defensive.
        if (it->second.empty()) continue;

        SnapshotEntry e;
        e.key        = key;
        e.type       = KeyType::List;
        e.expiry_ms  = expiry_ms;
        e.list_value = it->second; // copies the deque
        // e.str_value is default-constructed (empty string) — unused for lists.
        entries.push_back(e);
    }

    return entries;
}

// ---------------------------------------------------------------------------
// Active Eviction
// ---------------------------------------------------------------------------

void Database::evict_expired() {
    // INTERVIEW NOTE: We cannot erase from an unordered_map while iterating it
    // (undefined behaviour in C++). Collect expired keys first, then erase.
    // C++20's std::erase_if() handles this internally, but we target C++17.

    int64_t current_ms = now_ms();
    std::vector<std::string> to_evict;

    for (auto it = expiry_.begin(); it != expiry_.end(); ++it) {
        if (current_ms >= it->second) {
            to_evict.push_back(it->first);
        }
    }

    for (const std::string& key : to_evict) {
        // Erase from whichever store holds this key.
        string_store_.erase(key);
        list_store_.erase(key);
        expiry_.erase(key);
    }
}

} // namespace redis
