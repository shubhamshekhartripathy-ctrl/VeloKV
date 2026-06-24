#pragma once

#include "common.hpp"
#include "client.hpp"
#include "command.hpp"
#include "db.hpp"
#include "persistence.hpp"
#include <string>
#include <unordered_map>
#include <atomic>
#include <cstdint>

namespace redis {

/**
 * @class Server
 * @brief Single-threaded, select()-based TCP event loop.
 *
 * DESIGN NOTES (Milestone 3 additions):
 *
 * 1. PERSISTENCE LIFECYCLE:
 *    - On startup (init()):  PersistenceManager::load() reloads the last saved state.
 *    - On shutdown (stop()): PersistenceManager::save() flushes state to disk.
 *    - On demand (SAVE cmd): The command handler in command.cpp calls save() directly.
 *
 * 2. ACTIVE EVICTION:
 *    The event loop increments loop_tick_ on every iteration. When it reaches
 *    kEvictionInterval, db_.evict_expired() is called and the counter resets.
 *    This provides a low-overhead periodic sweep without requiring a separate thread.
 *
 * 3. DATABASE FILE:
 *    kDbPath defines the persistence file name. It is created in the working
 *    directory where the server binary is launched.
 */
class Server {
public:
    Server(const std::string& host, int port);
    ~Server();

    // Non-copyable: socket ownership is not shared.
    Server(const Server&) = delete;
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
    std::string       host_;
    int               port_;
    socket_t          listen_fd_;
    std::atomic<bool> running_;

    // Core in-memory storage engine.
    Database db_;

    // Command dispatch registry.
    CommandProcessor processor_;

    // Active client sessions keyed by socket descriptor.
    std::unordered_map<socket_t, Client> clients_;

    // Persistence file path (relative to working directory).
    static constexpr const char* kDbPath = "redis.rdb";

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
