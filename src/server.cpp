#include "server.hpp"
#include "resp.hpp"
#include <iostream>
#include <vector>
#include <cstring>
#include <cctype>

namespace redis {

// ---------------------------------------------------------------------------
// Helper: uppercase a string in-place (used for command-name comparison)
// ---------------------------------------------------------------------------

static std::string str_upper(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

Server::Server(const std::string& host, int port,
               NodeRole role, ReplicationManager* repl_mgr,
               const std::string& rdb_path)
    : host_(host), port_(port),
      listen_fd_(INVALID_SOCKET_VAL), running_(false),
      role_(role), repl_mgr_(repl_mgr),
      rdb_path_(rdb_path)
{}

Server::~Server() {
    stop(); // idempotent — saves DB, sets running_ = false
    if (listen_fd_ != INVALID_SOCKET_VAL) {
        close_socket(listen_fd_);
    }
}

// ---------------------------------------------------------------------------
// init() — Socket setup + database reload
// ---------------------------------------------------------------------------

bool Server::init() {
    // ── 1. Create IPv4 TCP socket ─────────────────────────────────────────
    listen_fd_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd_ == INVALID_SOCKET_VAL) {
        std::cerr << "[Server] ERROR: Failed to create socket: "
                  << get_last_error_string() << std::endl;
        return false;
    }

    // ── 2. Set non-blocking ───────────────────────────────────────────────
    if (!set_nonblocking(listen_fd_)) {
        std::cerr << "[Server] ERROR: Failed to set listening socket non-blocking." << std::endl;
        close_socket(listen_fd_);
        listen_fd_ = INVALID_SOCKET_VAL;
        return false;
    }

    // ── 3. SO_REUSEADDR — avoids "address already in use" on rapid restart ─
    int optval = 1;
#ifdef _WIN32
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&optval), sizeof(optval));
#else
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
#endif

    // ── 4. Bind ───────────────────────────────────────────────────────────
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port_);

#ifdef _WIN32
    addr.sin_addr.s_addr = inet_addr(host_.c_str());
    if (addr.sin_addr.s_addr == INADDR_NONE) {
        std::cerr << "[Server] ERROR: Invalid host address: " << host_ << std::endl;
        close_socket(listen_fd_);
        listen_fd_ = INVALID_SOCKET_VAL;
        return false;
    }
#else
    if (inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) <= 0) {
        std::cerr << "[Server] ERROR: Invalid host address: " << host_ << std::endl;
        close_socket(listen_fd_);
        listen_fd_ = INVALID_SOCKET_VAL;
        return false;
    }
#endif

    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR_VAL) {
        std::cerr << "[Server] ERROR: Failed to bind to "
                  << host_ << ":" << port_ << " — "
                  << get_last_error_string() << std::endl;
        close_socket(listen_fd_);
        listen_fd_ = INVALID_SOCKET_VAL;
        return false;
    }

    // ── 5. Listen ─────────────────────────────────────────────────────────
    if (listen(listen_fd_, SOMAXCONN) == SOCKET_ERROR_VAL) {
        std::cerr << "[Server] ERROR: Failed to listen: "
                  << get_last_error_string() << std::endl;
        close_socket(listen_fd_);
        listen_fd_ = INVALID_SOCKET_VAL;
        return false;
    }

    const char* role_name =
        (role_ == NodeRole::Leader)   ? "LEADER"   :
        (role_ == NodeRole::Replica)  ? "REPLICA"  : "STANDALONE";
    std::cout << "[Server] Listening on " << host_ << ":" << port_
              << "  (role: " << role_name << ")" << std::endl;

    // ── 6. Load persisted database ────────────────────────────────────────
    std::cout << "[Server] Loading database from '" << rdb_path_ << "'..." << std::endl;
    LoadResult result = PersistenceManager::load(db_, rdb_path_);
    if (!result.success) {
        std::cerr << "[Server] WARNING: Database load failed: "
                  << result.error << std::endl;
    }

    return true;
}

// ---------------------------------------------------------------------------
// start() — select() event loop with replication integration
// ---------------------------------------------------------------------------

void Server::start() {
    running_   = true;
    loop_tick_ = 0;
    std::cout << "[Server] Event loop started." << std::endl;

    while (running_) {
        // ── Build fd_sets ─────────────────────────────────────────────────
        fd_set read_fds;
        fd_set write_fds;
        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);

        FD_SET(listen_fd_, &read_fds);
        socket_t max_fd = listen_fd_;

        // Add client sockets.
        for (const auto& pair : clients_) {
            socket_t      fd     = pair.first;
            const Client& client = pair.second;
            FD_SET(fd, &read_fds);
            if (client.has_pending_write()) {
                FD_SET(fd, &write_fds);
            }
            if (fd > max_fd) max_fd = fd;
        }

        // Add replication sockets (replica FDs on leader; leader FD on replica).
        if (repl_mgr_ != nullptr) {
            repl_mgr_->register_fds(read_fds, write_fds, max_fd);
        }

        // ── select() with 100ms timeout ───────────────────────────────────
        timeval tv;
        tv.tv_sec  = 0;
        tv.tv_usec = 100000; // 100ms

        int activity = select(static_cast<int>(max_fd) + 1,
                              &read_fds, &write_fds, nullptr, &tv);

        if (activity < 0) {
            if (is_would_block()) continue;
            std::cerr << "[Server] ERROR: select() failed: "
                      << get_last_error_string() << std::endl;
            break;
        }

        // ── Periodic active expiry sweep ──────────────────────────────────
        ++loop_tick_;
        if (loop_tick_ >= kEvictionInterval) {
            db_.evict_expired();
            loop_tick_ = 0;
        }

        if (activity == 0) {
            // Timeout — no socket events.  Still drive reconnect logic.
            if (repl_mgr_ != nullptr && role_ == NodeRole::Replica) {
                repl_mgr_->try_reconnect(db_);
            }
            continue;
        }

        // ── New client connections ────────────────────────────────────────
        if (FD_ISSET(listen_fd_, &read_fds)) {
            handle_new_connection();
        }

        // ── Existing client reads/writes ──────────────────────────────────
        // Snapshot fd list to avoid iterator invalidation when a client is
        // disconnected or promoted to a replica inside the loop.
        std::vector<socket_t> active_fds;
        active_fds.reserve(clients_.size());
        for (const auto& pair : clients_) {
            active_fds.push_back(pair.first);
        }

        for (socket_t fd : active_fds) {
            if (clients_.find(fd) == clients_.end()) continue;

            if (FD_ISSET(fd, &read_fds)) {
                handle_client_read(fd);
                // After handle_client_read(), the client may have been removed
                // (disconnected or promoted to replica).  Re-check before write.
            }

            if (clients_.find(fd) == clients_.end()) continue;

            if (FD_ISSET(fd, &write_fds)) {
                handle_client_write(fd);
            }
        }

        // ── Replication I/O ───────────────────────────────────────────────
        if (repl_mgr_ != nullptr) {

            if (role_ == NodeRole::Leader) {
                // Snapshot replica FDs — flush/read may remove replicas, which
                // would invalidate iterators inside ReplicationManager.
                std::vector<socket_t> rfds = repl_mgr_->replica_fds();

                for (socket_t rfd : rfds) {
                    // Check read first: detect disconnect before attempting write.
                    bool still_alive = true;
                    if (FD_ISSET(rfd, &read_fds)) {
                        still_alive = repl_mgr_->handle_replica_read(rfd);
                        // handle_replica_read() calls remove_replica() internally
                        // if the replica disconnected.
                    }
                    if (still_alive && FD_ISSET(rfd, &write_fds)) {
                        if (!repl_mgr_->flush_replica(rfd)) {
                            repl_mgr_->remove_replica(rfd);
                        }
                    }
                }

            } else if (role_ == NodeRole::Replica) {
                socket_t lfd = repl_mgr_->leader_fd();
                if (lfd != INVALID_SOCKET_VAL && FD_ISSET(lfd, &read_fds)) {
                    // Read and apply leader's replication stream.
                    repl_mgr_->read_from_leader(db_);
                }
                // Attempt reconnect if the leader connection dropped.
                if (!repl_mgr_->is_connected()) {
                    repl_mgr_->try_reconnect(db_);
                }
            }
        }
    }

    std::cout << "[Server] Event loop stopped." << std::endl;
}

// ---------------------------------------------------------------------------
// stop() — signal exit and save database
// ---------------------------------------------------------------------------

void Server::stop() {
    if (!running_.exchange(false)) {
        return; // Already stopped — avoid double-save.
    }

    std::cout << "[Server] Saving database to '" << rdb_path_ << "'..." << std::endl;
    bool ok = PersistenceManager::save(db_, rdb_path_);
    if (!ok) {
        std::cerr << "[Server] ERROR: Failed to save database on shutdown." << std::endl;
    }
}

// ---------------------------------------------------------------------------
// Private: handle_new_connection()
// ---------------------------------------------------------------------------

void Server::handle_new_connection() {
    while (true) {
        sockaddr_in client_addr;
        socklen_t   client_len = sizeof(client_addr);

        socket_t client_fd = accept(listen_fd_,
                                    reinterpret_cast<sockaddr*>(&client_addr),
                                    &client_len);

        if (client_fd == INVALID_SOCKET_VAL) {
            if (is_would_block()) break;
            std::cerr << "[Server] ERROR: accept() failed: "
                      << get_last_error_string() << std::endl;
            break;
        }

        if (!set_nonblocking(client_fd)) {
            std::cerr << "[Server] ERROR: Failed to set client socket non-blocking. Dropping."
                      << std::endl;
            close_socket(client_fd);
            continue;
        }

        char ip_str[INET_ADDRSTRLEN];
#ifdef _WIN32
        std::strcpy(ip_str, inet_ntoa(client_addr.sin_addr));
#else
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, INET_ADDRSTRLEN);
#endif
        int client_port = ntohs(client_addr.sin_port);
        std::cout << "[Server] New connection from "
                  << ip_str << ":" << client_port
                  << " (fd=" << client_fd << ")" << std::endl;

        clients_.emplace(std::piecewise_construct,
                         std::forward_as_tuple(client_fd),
                         std::forward_as_tuple(client_fd));
    }
}

// ---------------------------------------------------------------------------
// Private: handle_client_read()
// ---------------------------------------------------------------------------

void Server::handle_client_read(socket_t fd) {
    auto it = clients_.find(fd);
    if (it == clients_.end()) return;

    Client& client = it->second;

    if (!client.read_from_socket()) {
        disconnect_client(fd);
        return;
    }

    std::vector<Command> commands;
    if (!RESPParser::parse(client.input_buffer(), commands)) return;

    for (const auto& cmd : commands) {
        if (cmd.empty()) continue;

        std::string cmd_name = str_upper(cmd[0]);

        // ── Detect REPLICAOF handshake (leader only) ──────────────────────
        // A downstream replica's very first message is "REPLICAOF self self\r\n".
        // We promote the connection from a regular client to a replica slot in
        // ReplicationManager, which immediately queues a full-sync payload.
        //
        // PROMOTION SEQUENCE (important: order matters):
        //   1. client.release()   — steals fd_; ~Client() will NOT close socket.
        //   2. clients_.erase(it) — destroys Client safely (fd_ is INVALID).
        //   3. add_replica(rfd)   — ReplicationManager now owns the socket.
        //   4. return             — do NOT use `client` or `it` after erase.
        //
        if (cmd_name == "REPLICAOF" && role_ == NodeRole::Leader && repl_mgr_ != nullptr) {
            std::cout << "[Server] Replica handshake on fd=" << fd
                      << ". Promoting to replica connection." << std::endl;

            socket_t rfd = client.release(); // Step 1: steal the fd.
            clients_.erase(it);              // Step 2: safe destructor (fd_ = INVALID).
            repl_mgr_->add_replica(rfd, db_); // Step 3: ReplicationManager takes over.
            return;                           // Step 4: it / client are dangling — exit.
        }

        // ── READONLY guard (replica rejects client writes) ─────────────────
        if (role_ == NodeRole::Replica && is_write_command(cmd_name)) {
            client.queue_response(RESPParser::serialize_error(
                "READONLY You can't write against a read only replica."));
            continue;
        }

        // ── Execute command ────────────────────────────────────────────────
        std::string response = processor_.execute(cmd, db_);
        if (!response.empty()) {
            client.queue_response(response);
        }

        // ── Propagate write commands to replicas (leader only) ─────────────
        // We propagate AFTER local execution.  This means the leader stores the
        // write before sending it to replicas — so even if a replica is slow,
        // the local state is consistent.
        //
        if (role_ == NodeRole::Leader && repl_mgr_ != nullptr && is_write_command(cmd_name)) {
            repl_mgr_->propagate(cmd);
        }
    }

    // Flush queued responses to the client socket.
    // Re-check that the client is still in the map — the REPLICAOF branch above
    // erases it and returns early, but another command in the same batch might
    // have caused a disconnect.
    if (clients_.find(fd) != clients_.end()) {
        if (!client.write_to_socket()) {
            disconnect_client(fd);
        }
    }
}

// ---------------------------------------------------------------------------
// Private: handle_client_write()
// ---------------------------------------------------------------------------

void Server::handle_client_write(socket_t fd) {
    auto it = clients_.find(fd);
    if (it == clients_.end()) return;

    if (!it->second.write_to_socket()) {
        disconnect_client(fd);
    }
}

// ---------------------------------------------------------------------------
// Private: disconnect_client()
// ---------------------------------------------------------------------------

void Server::disconnect_client(socket_t fd) {
    std::cout << "[Server] Client disconnected (fd=" << fd << ")" << std::endl;
    clients_.erase(fd); // Triggers ~Client(), which closes the socket.
}

} // namespace redis
