#pragma once

#include "common.hpp"
#include "db.hpp"
#include "resp.hpp"
#include <string>
#include <vector>
#include <cstdint>

namespace redis {

// ---------------------------------------------------------------------------
// NodeRole
// ---------------------------------------------------------------------------

/**
 * @enum NodeRole
 * @brief Determines how this VeloKV instance participates in replication.
 *
 * Standalone: single-node mode (default, backward-compatible).
 * Leader:     accepts all writes; propagates each write to all replicas.
 * Replica:    read-only copy; keeps itself in sync by consuming the leader stream.
 *
 */
enum class NodeRole {
    Standalone,
    Leader,
    Replica
};

// ---------------------------------------------------------------------------
// ReplicaConnection — the leader's view of one connected replica
// ---------------------------------------------------------------------------

/**
 * @struct ReplicaConnection
 * @brief Wraps one TCP socket to a downstream replica (leader-side).
 *
 * DESIGN:
 * Each ReplicaConnection owns a socket (fd) and an output_buffer.
 * The buffer holds, in order:
 *   1. The initial full-sync payload (queued immediately on connect).
 *   2. Live propagated write commands (appended by propagate()).
 *
 * TCP guarantees in-order, lossless delivery, so the replica always applies
 * full-sync data BEFORE any live commands — even if live writes arrive while
 * the full-sync data is still being drained over the wire.
 *
 * The 'synced' flag transitions to true the first time the output_buffer
 * drains to empty. It is used only for logging ("replica is now live").
 *
 */
struct ReplicaConnection {
    socket_t    fd;
    std::string output_buffer;
    bool        synced; ///< Logging flag: true after buffer first fully drains.

    explicit ReplicaConnection(socket_t f) : fd(f), synced(false) {}

    // Non-copyable: conceptually owns the socket lifecycle.
    ReplicaConnection(const ReplicaConnection&)            = delete;
    ReplicaConnection& operator=(const ReplicaConnection&) = delete;

    // Movable so it can live in a std::vector.
    ReplicaConnection(ReplicaConnection&& o) noexcept
        : fd(o.fd), output_buffer(std::move(o.output_buffer)), synced(o.synced) {
        o.fd = INVALID_SOCKET_VAL; // Prevent double-close on move.
    }

    // Move assignment is required by std::vector::erase() to shift elements.
    // We take ownership of `o.fd` and set o.fd to INVALID to prevent double-close.
    ReplicaConnection& operator=(ReplicaConnection&& o) noexcept {
        if (this != &o) {
            fd            = o.fd;
            output_buffer = std::move(o.output_buffer);
            synced        = o.synced;
            o.fd          = INVALID_SOCKET_VAL;
        }
        return *this;
    }
};

// ---------------------------------------------------------------------------
// ReplicationManager
// ---------------------------------------------------------------------------

/**
 * @class ReplicationManager
 * @brief Orchestrates all replication I/O for a VeloKV node.
 *
 * =========================================================================
 * ARCHITECTURE
 * =========================================================================
 *
 * One ReplicationManager instance per Server.  It is constructed BEFORE the
 * event loop begins and wired into the Server's select() call via register_fds().
 *
 * The class is SINGLE-THREADED: it must only be accessed from the event-loop
 * thread that also owns the Database.  No mutexes, no races.
 *
 * =========================================================================
 * REPLICATION PROTOCOL (custom inline text over TCP)
 * =========================================================================
 *
 * Handshake (replica → leader, very first message on connect):
 *   REPLICAOF self self\r\n
 *
 * Full sync (leader → replica, immediately after detecting handshake):
 *   +FULLSYNC\r\n
 *   REPLSET <enc_key> <enc_value> <expiry_ms>\r\n    ← one per string key
 *   REPLLIST <enc_key> <expiry_ms> <n> <e0>...\r\n   ← one per list key
 *   +FULLSYNCDONE\r\n
 *
 * Live propagation (leader → replica, after full sync, per write command):
 *   SET <enc_key> <enc_value>\r\n
 *   DEL <enc_key>\r\n
 *   EXPIRE <enc_key> <seconds>\r\n
 *   LPUSH / RPUSH / LPOP / RPOP  (same pattern)
 *
 * Encoding: all tokens are percent-encoded:
 *   '%' → %25,  ' ' → %20,  '\r' → %0D,  '\n' → %0A
 *
 */
class ReplicationManager {
public:
    explicit ReplicationManager(NodeRole role);
    ~ReplicationManager();

    ReplicationManager(const ReplicationManager&)            = delete;
    ReplicationManager& operator=(const ReplicationManager&) = delete;

    NodeRole role() const { return role_; }

    // =========================================================================
    // Leader API
    // =========================================================================

    /**
     * @brief Promotes a new socket to a replica connection and queues full sync.
     *
     * Called by Server when a REPLICAOF handshake is detected on a client socket.
     * Snapshots the database and queues the full-sync payload into the new
     * replica's output_buffer.  Actual delivery is asynchronous via flush_replica().
     *
     * @param fd  The replica's socket FD (already stolen from the Client object).
     * @param db  Reference to the leader's database for snapshotting.
     */
    void add_replica(socket_t fd, const Database& db);

    /**
     * @brief Queues a write command for delivery to ALL connected replicas.
     *
     * The command is appended to each replica's output_buffer AFTER any
     * full-sync data already queued there.  TCP in-order delivery ensures
     * the replica applies full-sync data before live commands.
     *
     * @param cmd Parsed command tokens (e.g. {"SET", "key", "value"}).
     */
    void propagate(const Command& cmd);

    /**
     * @brief Adds all replica FDs to the select() fd_sets.
     *
     * Read set:  detect disconnection (replicas don't send data post-handshake).
     * Write set: only if output_buffer is non-empty.
     */
    void register_fds(fd_set& read_fds, fd_set& write_fds, socket_t& max_fd) const;

    /**
     * @brief Flushes pending bytes to one replica. Called when select() says writable.
     *
     * When output_buffer drains to empty for the first time, sets synced=true
     * and logs that the replica has caught up.
     *
     * @return true if still alive; false if the replica should be removed
     *         (caller must call remove_replica).
     */
    bool flush_replica(socket_t fd);

    /**
     * @brief Called when a replica FD is readable. Detects disconnection.
     *
     * Replicas do not send data after the initial handshake.  Any readable
     * event means the replica has sent EOF (disconnected).
     *
     * @return true if still alive; false if disconnected (already removed internally).
     */
    bool handle_replica_read(socket_t fd);

    /**
     * @brief Closes and removes a replica. Safe to call even if already removed.
     */
    void remove_replica(socket_t fd);

    /**
     * @brief Returns a snapshot of all active replica socket descriptors.
     *
     * Returning a copy (not references) makes it safe to call remove_replica()
     * while iterating the returned list.
     */
    std::vector<socket_t> replica_fds() const;

    /** @return Number of currently connected replicas. */
    size_t replica_count() const { return replicas_.size(); }

    // =========================================================================
    // Replica API
    // =========================================================================

    /**
     * @brief Connects to the leader via TCP and sends the REPLICAOF handshake.
     *
     * Uses a blocking connect.  This is acceptable at startup; if the leader is
     * not reachable, it fails quickly (ECONNREFUSED) or after the OS timeout.
     * After a successful connect the socket is set non-blocking for the event loop.
     *
     * LIMITATION: If the leader host is unreachable (not ECONNREFUSED), this
     * blocks the calling thread for the OS TCP connect timeout (~20s).  In
     * production you would use non-blocking connect + a state machine.
     *
     * @param host Leader IP address string (e.g. "127.0.0.1").
     * @param port Leader port.
     * @return true if connected and handshake sent.
     */
    bool connect_to_leader(const std::string& host, int port);

    /**
     * @brief Reads and applies all available data from the leader socket.
     *
     * Parses lines from the leader stream:
     *   +FULLSYNC       → sets full_sync_in_progress_ = true
     *   REPLSET ...     → applies string key (during full sync)
     *   REPLLIST ...    → applies list key (during full sync)
     *   +FULLSYNCDONE   → clears full_sync_in_progress_; live mode begins
     *   SET/DEL/EXPIRE  → applies live write command directly to db
     *
     * On leader disconnect: closes socket, begins reconnect backoff.
     *
     * @param db Reference to the replica's local database.
     */
    void read_from_leader(Database& db);

    /** @return The leader socket FD, or INVALID_SOCKET_VAL if not connected. */
    socket_t leader_fd() const { return leader_fd_; }

    /** @return true if the replica is currently connected to a leader. */
    bool is_connected() const { return leader_connected_; }

    /**
     * @brief Retries the leader connection if disconnected and backoff elapsed.
     *
     * Called every select() iteration.  Reconnects at most once per
     * kReconnectIntervalMs milliseconds.  On success, a new full sync begins.
     *
     * @param db Passed to connect_to_leader; not used directly here.
     */
    void try_reconnect(Database& db);

private:
    NodeRole role_;

    // ── Leader state ──────────────────────────────────────────────────────
    std::vector<ReplicaConnection> replicas_;

    // ── Replica state ─────────────────────────────────────────────────────
    socket_t    leader_fd_;
    std::string leader_host_;
    int         leader_port_;
    std::string leader_input_buffer_;    ///< Accumulates bytes from leader socket.
    bool        leader_connected_;
    bool        full_sync_in_progress_;  ///< Between +FULLSYNC and +FULLSYNCDONE.
    int64_t     last_reconnect_ms_;      ///< Timestamp of last connect attempt.

    static constexpr int64_t kReconnectIntervalMs = 5000; ///< 5s reconnect backoff.

    // ── Internal helpers ──────────────────────────────────────────────────

    /**
     * @brief Encodes a Command as a percent-encoded inline line for the stream.
     *
     * Format: UPPERCASE_CMD enc_arg1 enc_arg2 ...\r\n
     *
     */
    static std::string encode_command(const Command& cmd);

    /**
     * @brief Builds the full-sync payload for a new replica.
     *
     * Calls db.snapshot() to get a consistent list of all live keys, then
     * serialises each one as a REPLSET or REPLLIST line between the
     * +FULLSYNC and +FULLSYNCDONE markers.
     */
    static std::string build_full_sync(const Database& db);

    /**
     * @brief Parses and applies one line from the leader's replication stream.
     * @return true if the line was a recognised protocol message.
     */
    bool apply_leader_line(const std::string& line, Database& db);

    /**
     * @brief Percent-encodes special characters in a string.
     * Encodes: '%' → %25, ' ' → %20, '\r' → %0D, '\n' → %0A.
     */
    static std::string encode(const std::string& s);

    /**
     * @brief Decodes a percent-encoded string.
     * @throws std::runtime_error on malformed input.
     */
    static std::string decode(const std::string& s);

    /** @brief Milliseconds since Unix epoch (system_clock). */
    static int64_t now_ms() noexcept;
};

} // namespace redis
