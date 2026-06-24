#pragma once

#include "common.hpp"
#include "client.hpp"
#include "command.hpp"
#include "db.hpp"
#include "persistence.hpp"
#include "replication.hpp"
#include <string>
#include <unordered_map>
#include <atomic>
#include <cstdint>

namespace redis {

/**
 * @class Server
 * @brief Single-threaded, select()-based TCP event loop.
 *
 * MILESTONE 8 ADDITIONS — Replication integration:
 *
 * 1. NODE ROLE:
 *    The Server can operate in one of three roles:
 *      Standalone — classic single-node mode (no replication, default).
 *      Leader     — accepts all writes, propagates each write to all replicas.
 *      Replica    — read-only; refuses client writes with READONLY error.
 *
 * 2. REPLICATION MANAGER:
 *    An optional ReplicationManager* is stored as repl_mgr_.  When present:
 *      - Its sockets are added to the select() fd_sets each iteration.
 *      - handle_client_read() detects the REPLICAOF handshake and promotes
 *        the connection from a regular client to a replica.
 *      - After executing a write command (leader mode), the command is
 *        propagated to replicas via repl_mgr_->propagate().
 *      - Replica mode: write commands from external clients return READONLY.
 *
 * 3. CLIENT FD PROMOTION PATTERN:
 *    When the REPLICAOF handshake is detected, we must "steal" the socket FD
 *    from the Client object before erasing it (so the Client destructor does
 *    not close the socket).  Client::release() implements this pattern.
 *
 * 4. RDB PATH:
 *    The persistence file path is now a constructor parameter (default
 *    "redis.rdb").  This allows leader and replica to use different files
 *    when running in the same working directory.
 */
class Server {
public:
    /**
     * @param host      Bind address (e.g. "127.0.0.1").
     * @param port      Bind port (e.g. 6379).
     * @param role      Node role; determines replication behaviour.
     * @param repl_mgr  Optional replication manager.  nullptr = standalone.
     * @param rdb_path  Path for the persistence file (default "redis.rdb").
     */
    Server(const std::string& host, int port,
           NodeRole           role     = NodeRole::Standalone,
           ReplicationManager* repl_mgr = nullptr,
           const std::string& rdb_path = "redis.rdb");

    ~Server();

    // Non-copyable: socket ownership is not shared.
    Server(const Server&)            = delete;
    Server& operator=(const Server&) = delete;

    /**
     * @brief Binds the socket, starts listening, and loads the persisted database.
     * @return true if all initialization steps succeeded.
     */
    bool init();

    /**
     * @brief Runs the select()-based event loop. Blocks until stop() is called.
     */
    void start();

    /**
     * @brief Signals the event loop to exit and saves the database to disk.
     */
    void stop();

private:
    std::string        host_;
    int                port_;
    socket_t           listen_fd_;
    std::atomic<bool>  running_;

    // Core in-memory storage engine.
    Database           db_;

    // Command dispatch registry.
    CommandProcessor   processor_;

    // Active client sessions keyed by socket descriptor.
    std::unordered_map<socket_t, Client> clients_;

    // Replication state.
    NodeRole            role_;
    ReplicationManager* repl_mgr_;   ///< Non-owning pointer; lifetime managed by main().

    // Persistence file path.
    std::string rdb_path_;

    // How many event loop iterations between active expiry sweeps.
    // At 100ms select() timeout → 100 iterations ≈ 10 seconds between sweeps.
    static constexpr uint64_t kEvictionInterval = 100;

    // Counts event loop iterations; resets after each eviction sweep.
    uint64_t loop_tick_ = 0;

    void handle_new_connection();
    void handle_client_read(socket_t fd);
    void handle_client_write(socket_t fd);
    void disconnect_client(socket_t fd);
};

} // namespace redis
