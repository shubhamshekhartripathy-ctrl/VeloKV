#include "replication.hpp"

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <chrono>

// Platform-specific connect/inet includes already pulled in via common.hpp
// (through replication.hpp → common.hpp).

namespace redis {

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

ReplicationManager::ReplicationManager(NodeRole role)
    : role_(role),
      leader_fd_(INVALID_SOCKET_VAL),
      leader_port_(0),
      leader_connected_(false),
      full_sync_in_progress_(false),
      last_reconnect_ms_(0)
{}

ReplicationManager::~ReplicationManager() {
    // Close all replica sockets (leader side).
    // IMPORTANT: replicas_ stores ReplicaConnection objects whose destructors
    // do NOT close the socket (to avoid double-close on move).  We close here.
    for (auto& rc : replicas_) {
        if (rc.fd != INVALID_SOCKET_VAL) {
            close_socket(rc.fd);
            rc.fd = INVALID_SOCKET_VAL;
        }
    }

    // Close the leader connection (replica side).
    if (leader_fd_ != INVALID_SOCKET_VAL) {
        close_socket(leader_fd_);
        leader_fd_ = INVALID_SOCKET_VAL;
    }
}

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

int64_t ReplicationManager::now_ms() noexcept {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string ReplicationManager::encode(const std::string& s) {
    // Percent-encode characters that would break our line-based, space-delimited
    // protocol.  Same scheme as persistence.cpp (Milestone 3).
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

std::string ReplicationManager::decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '%') {
            out += s[i];
            continue;
        }
        // Percent-escape sequence: %XY
        if (i + 2 >= s.size()) {
            throw std::runtime_error(
                "Malformed percent-encoding at position " + std::to_string(i));
        }
        auto hex_val = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        int hi = hex_val(s[i + 1]);
        int lo = hex_val(s[i + 2]);
        if (hi < 0 || lo < 0) {
            throw std::runtime_error(
                "Invalid hex digits in percent sequence at position " + std::to_string(i));
        }
        out += static_cast<char>((hi << 4) | lo);
        i += 2;
    }
    return out;
}

std::string ReplicationManager::encode_command(const Command& cmd) {
    // Format: UPPERCASED_CMD enc_arg1 enc_arg2 ...\r\n
    //
    std::string out;
    for (size_t i = 0; i < cmd.size(); ++i) {
        if (i > 0) out += ' ';
        if (i == 0) {
            // Uppercase the command name token.
            std::string name = cmd[0];
            for (char& c : name) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            out += encode(name);
        } else {
            out += encode(cmd[i]);
        }
    }
    out += "\r\n";
    return out;
}

std::string ReplicationManager::build_full_sync(const Database& db) {
    // Snapshot the database.  db.snapshot() excludes expired keys and returns
    // typed entries (KeyType::String or KeyType::List).
    //
    std::vector<SnapshotEntry> entries = db.snapshot();

    std::string payload;
    payload.reserve(entries.size() * 64 + 32); // Rough estimate.

    payload += "+FULLSYNC\r\n";

    for (const auto& e : entries) {
        if (e.type == KeyType::String) {
            // Format: REPLSET <enc_key> <enc_value> <expiry_ms|-1>
            payload += "REPLSET ";
            payload += encode(e.key);
            payload += ' ';
            payload += encode(e.str_value);
            payload += ' ';
            payload += std::to_string(e.expiry_ms);
            payload += "\r\n";

        } else if (e.type == KeyType::List) {
            // Format: REPLLIST <enc_key> <expiry_ms|-1> <count> <enc_e0> <enc_e1>...
            // Elements are stored head-first (index 0 = front of the deque).
            // Replica uses rpush() to restore them in the same order.
            payload += "REPLLIST ";
            payload += encode(e.key);
            payload += ' ';
            payload += std::to_string(e.expiry_ms);
            payload += ' ';
            payload += std::to_string(e.list_value.size());
            for (const auto& elem : e.list_value) {
                payload += ' ';
                payload += encode(elem);
            }
            payload += "\r\n";
        }
    }

    payload += "+FULLSYNCDONE\r\n";
    return payload;
}

// ---------------------------------------------------------------------------
// Leader API — implementation
// ---------------------------------------------------------------------------

void ReplicationManager::add_replica(socket_t fd, const Database& db) {
    std::cout << "[Replication] New replica connected (fd=" << fd
              << "). Building full sync..." << std::endl;

    ReplicaConnection rc(fd);

    // Queue the full-sync payload immediately.  The select() loop will drain
    // it asynchronously via flush_replica().
    //
    // KEY DESIGN POINT: Live commands that arrive AFTER this call (via propagate())
    // are appended to output_buffer AFTER the full-sync payload.  TCP delivers
    // the buffer in order, so the replica always sees:
    //   [full-sync data from time T]  →  [live writes from time T onwards]
    // This guarantees the replica converges to the leader's state even during
    // the delivery window of the full sync.
    rc.output_buffer = build_full_sync(db);

    std::cout << "[Replication] Full-sync payload: "
              << rc.output_buffer.size() << " bytes queued." << std::endl;

    replicas_.push_back(std::move(rc));
}

void ReplicationManager::propagate(const Command& cmd) {
    if (replicas_.empty()) return;

    // Encode the command once, append to every replica's buffer.
    //
    // We propagate to ALL replicas, including those whose full-sync payload has
    // not yet been fully delivered (synced == false).  The live command is
    // appended AFTER the full-sync payload in output_buffer; TCP ordering ensures
    // the replica sees them in the correct sequence.
    std::string wire = encode_command(cmd);
    for (auto& rc : replicas_) {
        rc.output_buffer += wire;
    }
}

void ReplicationManager::register_fds(
        fd_set& read_fds, fd_set& write_fds, socket_t& max_fd) const {

    if (role_ == NodeRole::Leader) {
        for (const auto& rc : replicas_) {
            if (rc.fd == INVALID_SOCKET_VAL) continue;
            FD_SET(rc.fd, &read_fds);  // Detect replica disconnection.
            if (!rc.output_buffer.empty()) {
                FD_SET(rc.fd, &write_fds); // Flush pending data.
            }
            if (rc.fd > max_fd) max_fd = rc.fd;
        }
    } else if (role_ == NodeRole::Replica) {
        if (leader_fd_ != INVALID_SOCKET_VAL && leader_connected_) {
            FD_SET(leader_fd_, &read_fds); // Read leader stream.
            if (leader_fd_ > max_fd) max_fd = leader_fd_;
        }
    }
}

bool ReplicationManager::flush_replica(socket_t fd) {
    for (auto& rc : replicas_) {
        if (rc.fd != fd) continue;

        if (rc.output_buffer.empty()) {
            // Nothing to send.  If this is the first time the buffer emptied,
            // log that the replica has caught up (full sync delivered, now live).
            if (!rc.synced) {
                rc.synced = true;
                std::cout << "[Replication] Replica fd=" << fd
                          << " is now live (full sync delivered)." << std::endl;
            }
            return true;
        }

#ifdef _WIN32
        int sent = send(fd,
                        rc.output_buffer.data(),
                        static_cast<int>(rc.output_buffer.size()), 0);
#else
        ssize_t sent = send(fd,
                            rc.output_buffer.data(),
                            rc.output_buffer.size(), 0);
#endif

        if (sent > 0) {
            rc.output_buffer.erase(0, static_cast<size_t>(sent));
            // Check again after drain.
            if (rc.output_buffer.empty() && !rc.synced) {
                rc.synced = true;
                std::cout << "[Replication] Replica fd=" << fd
                          << " is now live (full sync delivered)." << std::endl;
            }
            return true;
        }

        if (sent < 0 && is_would_block()) {
            return true; // Socket buffer temporarily full — retry next iteration.
        }

        // Send error or connection closed.
        std::cerr << "[Replication] Write error to replica fd=" << fd
                  << ": " << get_last_error_string() << std::endl;
        return false; // Caller must call remove_replica(fd).
    }

    return false; // FD not found in replica list.
}

bool ReplicationManager::handle_replica_read(socket_t fd) {
    // Replicas do not send data after the initial REPLICAOF handshake.
    // A readable event therefore means the replica has sent EOF or a RST.
    char buf[64];

#ifdef _WIN32
    int n = recv(fd, buf, sizeof(buf), 0);
#else
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
#endif

    if (n == 0) {
        std::cout << "[Replication] Replica fd=" << fd
                  << " disconnected gracefully." << std::endl;
    } else if (n < 0 && !is_would_block()) {
        std::cerr << "[Replication] Replica fd=" << fd
                  << " read error: " << get_last_error_string() << std::endl;
    } else {
        // n > 0: unexpected data from replica — ignore and stay connected.
        return true;
    }

    remove_replica(fd);
    return false; // Disconnected.
}

void ReplicationManager::remove_replica(socket_t fd) {
    for (auto it = replicas_.begin(); it != replicas_.end(); ++it) {
        if (it->fd == fd) {
            close_socket(fd);
            it->fd = INVALID_SOCKET_VAL; // Prevent double-close.
            replicas_.erase(it);
            std::cout << "[Replication] Replica removed. Active replicas: "
                      << replicas_.size() << std::endl;
            return;
        }
    }
}

std::vector<socket_t> ReplicationManager::replica_fds() const {
    // Returns a COPY so the caller can safely call remove_replica() while
    // iterating the returned list without invalidating any iterators.
    std::vector<socket_t> fds;
    fds.reserve(replicas_.size());
    for (const auto& rc : replicas_) {
        if (rc.fd != INVALID_SOCKET_VAL) {
            fds.push_back(rc.fd);
        }
    }
    return fds;
}

// ---------------------------------------------------------------------------
// Replica API — implementation
// ---------------------------------------------------------------------------

bool ReplicationManager::connect_to_leader(const std::string& host, int port) {
    leader_host_ = host;
    leader_port_ = port;

    // ── Create TCP socket ─────────────────────────────────────────────────
    socket_t fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCKET_VAL) {
        std::cerr << "[Replication] Failed to create socket: "
                  << get_last_error_string() << std::endl;
        return false;
    }

    // ── Resolve address ───────────────────────────────────────────────────
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(port));

#ifdef _WIN32
    addr.sin_addr.s_addr = inet_addr(host.c_str());
    if (addr.sin_addr.s_addr == INADDR_NONE) {
        std::cerr << "[Replication] Invalid leader address: " << host << std::endl;
        close_socket(fd);
        return false;
    }
#else
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        std::cerr << "[Replication] Invalid leader address: " << host << std::endl;
        close_socket(fd);
        return false;
    }
#endif

    // ── Blocking connect ──────────────────────────────────────────────────
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[Replication] Cannot connect to leader "
                  << host << ":" << port
                  << " — " << get_last_error_string() << std::endl;
        close_socket(fd);
        return false;
    }

    // ── Set non-blocking for the event loop ───────────────────────────────
    if (!set_nonblocking(fd)) {
        std::cerr << "[Replication] Failed to set leader socket non-blocking." << std::endl;
        close_socket(fd);
        return false;
    }

    leader_fd_        = fd;
    leader_connected_ = true;
    full_sync_in_progress_ = false;
    leader_input_buffer_.clear();

    // ── Send handshake ────────────────────────────────────────────────────
    // "REPLICAOF self self\r\n" is our magic keyword.  The leader detects this
    // as the first message and promotes the connection to a replica connection.
    // The "self self" arguments are placeholders that mirror the Redis syntax
    // for "REPLICAOF NO ONE" but here mean "I am connecting as a replica."
    const std::string handshake = "REPLICAOF self self\r\n";
    send(leader_fd_,
         handshake.data(),
         static_cast<int>(handshake.size()), 0);

    std::cout << "[Replication] Connected to leader " << host << ":" << port
              << " (fd=" << fd << "). Awaiting full sync..." << std::endl;
    return true;
}

void ReplicationManager::read_from_leader(Database& db) {
    if (leader_fd_ == INVALID_SOCKET_VAL) return;

    char buf[8192];

#ifdef _WIN32
    int n = recv(leader_fd_, buf, sizeof(buf), 0);
#else
    ssize_t n = recv(leader_fd_, buf, sizeof(buf), 0);
#endif

    if (n == 0) {
        // EOF: leader closed the connection.
        std::cout << "[Replication] Leader disconnected (fd=" << leader_fd_
                  << "). Will retry in " << kReconnectIntervalMs / 1000
                  << "s." << std::endl;
        close_socket(leader_fd_);
        leader_fd_             = INVALID_SOCKET_VAL;
        leader_connected_      = false;
        full_sync_in_progress_ = false;
        last_reconnect_ms_     = now_ms(); // Begin reconnect backoff.
        return;
    }

    if (n < 0) {
        if (is_would_block()) return; // Nothing available right now — normal.
        std::cerr << "[Replication] recv() error from leader: "
                  << get_last_error_string() << std::endl;
        close_socket(leader_fd_);
        leader_fd_         = INVALID_SOCKET_VAL;
        leader_connected_  = false;
        last_reconnect_ms_ = now_ms();
        return;
    }

    leader_input_buffer_.append(buf, static_cast<size_t>(n));

    // ── Parse complete lines ───────────────────────────────────────────────
    // The replication stream is line-oriented: each message ends with \r\n.
    // We accumulate bytes in leader_input_buffer_ and process complete lines.
    size_t pos = 0;
    while (pos < leader_input_buffer_.size()) {
        size_t crlf = leader_input_buffer_.find("\r\n", pos);
        if (crlf == std::string::npos) break; // Incomplete line — wait for more data.

        std::string line = leader_input_buffer_.substr(pos, crlf - pos);
        pos = crlf + 2; // Advance past \r\n.

        if (!line.empty()) {
            apply_leader_line(line, db);
        }
    }

    // Consume processed bytes.
    if (pos > 0) {
        leader_input_buffer_.erase(0, pos);
    }
}

bool ReplicationManager::apply_leader_line(const std::string& line, Database& db) {
    if (line.empty()) return true;

    // ── Control messages (simple string format: "+TOKEN") ─────────────────

    if (line == "+FULLSYNC") {
        std::cout << "[Replication] Full sync started from leader." << std::endl;
        full_sync_in_progress_ = true;
        // NOTE: In a production implementation we would flush the local database
        // here (db.clear()) to ensure we start from a clean slate.  In this
        // educational clone we apply the leader's records on top of the existing
        // state.  Since REPLSET uses db.set() (overwrite semantics), any key in
        // the leader's snapshot will be correctly reflected.  Stale keys that
        // exist locally but not on the leader would remain — a known limitation.
        return true;
    }

    if (line == "+FULLSYNCDONE") {
        full_sync_in_progress_ = false;
        std::cout << "[Replication] Full sync complete. Replica is live." << std::endl;
        return true;
    }

    // ── Tokenise the line ─────────────────────────────────────────────────
    std::vector<std::string> tokens;
    {
        std::istringstream iss(line);
        std::string tok;
        while (iss >> tok) {
            tokens.push_back(tok);
        }
    }
    if (tokens.empty()) return true;

    // Uppercase the command/record-type token for case-insensitive matching.
    std::string type = tokens[0];
    for (char& c : type) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }

    // ── Full-sync records ─────────────────────────────────────────────────

    if (type == "REPLSET") {
        // Format: REPLSET <enc_key> <enc_value> <expiry_ms>
        if (tokens.size() < 4) {
            std::cerr << "[Replication] Malformed REPLSET: " << line << std::endl;
            return false;
        }
        try {
            std::string key       = decode(tokens[1]);
            std::string value     = decode(tokens[2]);
            int64_t     expiry_ms = std::stoll(tokens[3]);

            db.set(key, value);
            if (expiry_ms != -1) {
                db.expire_at(key, expiry_ms);
            }
        } catch (const std::exception& e) {
            std::cerr << "[Replication] REPLSET error: " << e.what() << std::endl;
        }
        return true;
    }

    if (type == "REPLLIST") {
        // Format: REPLLIST <enc_key> <expiry_ms> <count> <enc_e0> <enc_e1>...
        if (tokens.size() < 4) {
            std::cerr << "[Replication] Malformed REPLLIST: " << line << std::endl;
            return false;
        }
        try {
            std::string key       = decode(tokens[1]);
            int64_t     expiry_ms = std::stoll(tokens[2]);
            int         count     = std::stoi(tokens[3]);

            db.del(key); // Clear any existing key before restoring the list.
            for (int i = 0; i < count && (4 + i) < static_cast<int>(tokens.size()); ++i) {
                db.rpush(key, decode(tokens[4 + i]));
            }
            if (expiry_ms != -1) {
                db.expire_at(key, expiry_ms);
            }
        } catch (const std::exception& e) {
            std::cerr << "[Replication] REPLLIST error: " << e.what() << std::endl;
        }
        return true;
    }

    // ── Live propagated write commands ────────────────────────────────────
    // These arrive after +FULLSYNCDONE.  We decode each token and apply the
    // command directly to the local database — bypassing CommandProcessor to
    // avoid triggering READONLY checks or re-propagation.
    //
    // Decode all tokens (keys/values are percent-encoded in the stream).
    Command decoded;
    decoded.reserve(tokens.size());
    for (const auto& tok : tokens) {
        try {
            decoded.push_back(decode(tok));
        } catch (...) {
            decoded.push_back(tok); // Best-effort: pass through on decode error.
        }
    }
    if (decoded.empty()) return false;

    // type is already the uppercased command name.

    if (type == "SET" && decoded.size() >= 3) {
        db.set(decoded[1], decoded[2]);
        std::cout << "[Replication] Applied SET " << decoded[1] << std::endl;

    } else if (type == "DEL" && decoded.size() >= 2) {
        db.del(decoded[1]);
        std::cout << "[Replication] Applied DEL " << decoded[1] << std::endl;

    } else if (type == "EXPIRE" && decoded.size() >= 3) {
        // The leader propagated "EXPIRE key seconds" (relative TTL).
        // We re-apply from the replica's current time.
        //
        // LIMITATION: Due to network latency, the replica's effective TTL will
        // be slightly shorter than the leader's.  Real Redis propagates EXPIREAT
        // (absolute Unix-ms timestamp) to avoid this drift.
        try {
            int64_t secs = std::stoll(decoded[2]);
            db.expire(decoded[1], secs * 1000LL);
        } catch (...) {}
        std::cout << "[Replication] Applied EXPIRE " << decoded[1] << std::endl;

    } else if (type == "LPUSH" && decoded.size() >= 3) {
        db.lpush(decoded[1], decoded[2]);
        std::cout << "[Replication] Applied LPUSH " << decoded[1] << std::endl;

    } else if (type == "RPUSH" && decoded.size() >= 3) {
        db.rpush(decoded[1], decoded[2]);
        std::cout << "[Replication] Applied RPUSH " << decoded[1] << std::endl;

    } else if (type == "LPOP" && decoded.size() >= 2) {
        db.lpop(decoded[1]);
        std::cout << "[Replication] Applied LPOP " << decoded[1] << std::endl;

    } else if (type == "RPOP" && decoded.size() >= 2) {
        db.rpop(decoded[1]);
        std::cout << "[Replication] Applied RPOP " << decoded[1] << std::endl;

    } else {
        // Unknown command from leader — log and ignore.
        std::cerr << "[Replication] Unknown propagated command: '"
                  << type << "' — ignoring." << std::endl;
        return false;
    }

    return true;
}

void ReplicationManager::try_reconnect(Database& db) {
    if (leader_connected_)   return; // Already connected.
    if (leader_host_.empty()) return; // No leader configured.

    int64_t now = now_ms();
    if (now - last_reconnect_ms_ < kReconnectIntervalMs) return; // Backoff.

    std::cout << "[Replication] Attempting reconnect to leader "
              << leader_host_ << ":" << leader_port_ << "..." << std::endl;

    last_reconnect_ms_ = now;
    connect_to_leader(leader_host_, leader_port_);
    (void)db; // db not used here; kept for API symmetry.
}

} // namespace redis
