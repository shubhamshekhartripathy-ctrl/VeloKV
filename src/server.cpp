#include "server.hpp"
#include "resp.hpp"
#include <iostream>
#include <vector>
#include <cstring>

namespace redis {

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

Server::Server(const std::string& host, int port)
    : host_(host), port_(port), listen_fd_(INVALID_SOCKET_VAL), running_(false) {}

Server::~Server() {
    // stop() is idempotent — calling it from both stop() and ~Server() is safe.
    stop();
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
                  << host_ << ":" << port_ << " — " << get_last_error_string() << std::endl;
        close_socket(listen_fd_);
        listen_fd_ = INVALID_SOCKET_VAL;
        return false;
    }

    // ── 5. Listen ─────────────────────────────────────────────────────────
    if (listen(listen_fd_, SOMAXCONN) == SOCKET_ERROR_VAL) {
        std::cerr << "[Server] ERROR: Failed to listen: " << get_last_error_string() << std::endl;
        close_socket(listen_fd_);
        listen_fd_ = INVALID_SOCKET_VAL;
        return false;
    }

    std::cout << "[Server] Listening on " << host_ << ":" << port_ << std::endl;

    // ── 6. Load persisted database ────────────────────────────────────────
    // This is done AFTER the socket is ready so the server is as close to
    // "serving" as possible when data is loaded (minimises the window where
    // the port is open but data is unavailable).
    //
    // INTERVIEW NOTE: Real Redis loads the RDB/AOF file before the socket is
    // opened (so clients can never connect to an empty database). Either
    // ordering is defensible; loading before bind is strictly safer.
    std::cout << "[Server] Loading database from '" << kDbPath << "'..." << std::endl;
    LoadResult result = PersistenceManager::load(db_, kDbPath);

    if (!result.success) {
        // A load failure is non-fatal — we log it and start with an empty DB.
        // In a production system you might choose to abort here.
        std::cerr << "[Server] WARNING: Database load failed: " << result.error << std::endl;
    }

    return true;
}

// ---------------------------------------------------------------------------
// start() — select() event loop with periodic active eviction
// ---------------------------------------------------------------------------

void Server::start() {
    running_ = true;
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

        for (const auto& pair : clients_) {
            socket_t fd     = pair.first;
            const Client& client = pair.second;
            FD_SET(fd, &read_fds);
            if (client.has_pending_write()) {
                FD_SET(fd, &write_fds);
            }
            if (fd > max_fd) {
                max_fd = fd;
            }
        }

        // ── select() with 100ms timeout ───────────────────────────────────
        // The timeout lets us check running_ periodically (for clean SIGINT shutdown)
        // and drive the eviction tick counter without a background thread.
        timeval tv;
        tv.tv_sec  = 0;
        tv.tv_usec = 100000; // 100ms

        int activity = select(static_cast<int>(max_fd) + 1,
                              &read_fds, &write_fds, nullptr, &tv);

        if (activity < 0) {
            if (is_would_block()) continue;
            std::cerr << "[Server] ERROR: select() failed: " << get_last_error_string() << std::endl;
            break;
        }

        // ── Active expiry sweep (periodic) ────────────────────────────────
        // INTERVIEW NOTE:
        // We drive this from the event loop tick rather than a separate timer thread.
        // This avoids any synchronisation overhead (mutexes, condition variables)
        // since the server is single-threaded. The sweep frequency is:
        //   kEvictionInterval × select_timeout = 100 × 100ms = ~10 seconds.
        ++loop_tick_;
        if (loop_tick_ >= kEvictionInterval) {
            db_.evict_expired();
            loop_tick_ = 0;
        }

        if (activity == 0) {
            // select() timed out — no socket events this iteration.
            continue;
        }

        // ── Handle new incoming connections ───────────────────────────────
        if (FD_ISSET(listen_fd_, &read_fds)) {
            handle_new_connection();
        }

        // ── Handle read/write events on existing clients ──────────────────
        // Copy the fd list to avoid iterator invalidation when a client disconnects.
        std::vector<socket_t> active_fds;
        active_fds.reserve(clients_.size());
        for (const auto& pair : clients_) {
            active_fds.push_back(pair.first);
        }

        for (socket_t fd : active_fds) {
            if (clients_.find(fd) == clients_.end()) continue;

            if (FD_ISSET(fd, &read_fds)) {
                handle_client_read(fd);
            }

            if (clients_.find(fd) == clients_.end()) continue;

            if (FD_ISSET(fd, &write_fds)) {
                handle_client_write(fd);
            }
        }
    }

    std::cout << "[Server] Event loop stopped." << std::endl;
}

// ---------------------------------------------------------------------------
// stop() — signals the loop to exit and saves the database
// ---------------------------------------------------------------------------

void Server::stop() {
    if (!running_.exchange(false)) {
        // Already stopped — avoid double-save.
        return;
    }

    // INTERVIEW NOTE:
    // We save here rather than in the destructor to ensure the save happens
    // while the Database object is still fully alive and before any member
    // destructors have run. Saving inside ~Server() would work in this codebase
    // but is an anti-pattern in more complex class hierarchies.
    std::cout << "[Server] Saving database to '" << kDbPath << "'..." << std::endl;
    bool ok = PersistenceManager::save(db_, kDbPath);
    if (!ok) {
        std::cerr << "[Server] ERROR: Failed to save database on shutdown." << std::endl;
    }
}

// ---------------------------------------------------------------------------
// Private connection handlers
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
            std::cerr << "[Server] ERROR: accept() failed: " << get_last_error_string() << std::endl;
            break;
        }

        if (!set_nonblocking(client_fd)) {
            std::cerr << "[Server] ERROR: Failed to set client socket non-blocking. Dropping." << std::endl;
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
        std::cout << "[Server] New connection from " << ip_str << ":" << client_port
                  << " (fd: " << client_fd << ")" << std::endl;

        clients_.emplace(std::piecewise_construct,
                         std::forward_as_tuple(client_fd),
                         std::forward_as_tuple(client_fd));
    }
}

void Server::handle_client_read(socket_t fd) {
    auto it = clients_.find(fd);
    if (it == clients_.end()) return;

    Client& client = it->second;

    if (!client.read_from_socket()) {
        disconnect_client(fd);
        return;
    }

    std::vector<Command> commands;
    if (RESPParser::parse(client.input_buffer(), commands)) {
        for (const auto& cmd : commands) {
            std::string response = processor_.execute(cmd, db_);
            if (!response.empty()) {
                client.queue_response(response);
            }
        }

        // Attempt immediate flush — remainder goes on next writeable select event.
        if (!client.write_to_socket()) {
            disconnect_client(fd);
        }
    }
}

void Server::handle_client_write(socket_t fd) {
    auto it = clients_.find(fd);
    if (it == clients_.end()) return;

    if (!it->second.write_to_socket()) {
        disconnect_client(fd);
    }
}

void Server::disconnect_client(socket_t fd) {
    std::cout << "[Server] Client disconnected (fd: " << fd << ")" << std::endl;
    clients_.erase(fd); // Triggers ~Client(), which closes the socket.
}

} // namespace redis
