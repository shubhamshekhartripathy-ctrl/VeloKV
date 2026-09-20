#include "common.hpp"
#include "server.hpp"
#include "replication.hpp"
#include <iostream>
#include <string>
#include <cstdlib>
#include <signal.h>

// ---------------------------------------------------------------------------
// Global server pointer for signal handling
// ---------------------------------------------------------------------------

namespace {
    redis::Server* g_server = nullptr;
}

void handle_signal(int sig) {
    std::cout << "\n[Main] Signal " << sig
              << " received. Stopping server gracefully..." << std::endl;
    if (g_server != nullptr) {
        g_server->stop();
    }
}

// ---------------------------------------------------------------------------
// CLI argument helpers
// ---------------------------------------------------------------------------

/**
 * @brief Finds the value of a named CLI argument.
 *
 * Supports:  --flag value   (two-token form)
 *
 * @param argc   Argument count from main().
 * @param argv   Argument vector from main().
 * @param flag   The flag to search for (e.g. "--role").
 * @param out    Written with the value if found.
 * @return true if the flag was found and a value follows it.
 */
static bool get_arg(int argc, char* argv[],
                    const std::string& flag, std::string& out) {
    for (int i = 1; i < argc - 1; ++i) {
        if (argv[i] == flag) {
            out = argv[i + 1];
            return true;
        }
    }
    return false;
}

static bool has_flag(int argc, char* argv[], const std::string& flag) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == flag) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// main()
// ---------------------------------------------------------------------------

/**
 * @brief Entry point for VeloKV.
 *
 * CLI usage:
 *
 *   Standalone (default — backward compatible):
 *     velokv_server.exe
 *     velokv_server.exe --port 6379 --rdb redis.rdb
 *
 *   Leader mode:
 *     velokv_server.exe --role leader --port 6379 --rdb redis_leader.rdb
 *
 *   Replica mode:
 *     velokv_server.exe --role replica --port 6380 \
 *                      --leader-host 127.0.0.1 --leader-port 6379 \
 *                      --rdb redis_replica.rdb
 *
 * Options:
 *   --role <leader|replica|standalone>   Node role (default: standalone).
 *   --port <N>                           Bind port    (default: 6379).
 *   --host <addr>                        Bind address (default: 127.0.0.1).
 *   --rdb  <path>                        RDB file     (default: redis.rdb).
 *   --leader-host <addr>                 Leader IP    (replica only).
 *   --leader-port <N>                    Leader port  (replica only).
 *   --help                               Print this message and exit.
 *
 */
int main(int argc, char* argv[]) {

    // ── Help ──────────────────────────────────────────────────────────────
    if (has_flag(argc, argv, "--help")) {
        std::cout <<
            "VeloKV — Redis-inspired in-memory key-value store\n\n"
            "Usage:\n"
            "  velokv_server.exe [options]\n\n"
            "Options:\n"
            "  --role <leader|replica|standalone>  Node role (default: standalone)\n"
            "  --port <N>                          Listen port (default: 6379)\n"
            "  --host <addr>                       Bind address (default: 127.0.0.1)\n"
            "  --rdb  <path>                       RDB file path (default: redis.rdb)\n"
            "  --leader-host <addr>                Leader IP   (replica only)\n"
            "  --leader-port <N>                   Leader port (replica only)\n"
            "  --help                              Show this help\n\n"
            "Examples:\n"
            "  # Standalone\n"
            "  velokv_server.exe\n\n"
            "  # Leader on port 6379\n"
            "  velokv_server.exe --role leader --port 6379 --rdb redis_leader.rdb\n\n"
            "  # Replica on port 6380\n"
            "  velokv_server.exe --role replica --port 6380 \\\n"
            "                   --leader-host 127.0.0.1 --leader-port 6379 \\\n"
            "                   --rdb redis_replica.rdb\n";
        return 0;
    }

    // ── Parse arguments ───────────────────────────────────────────────────
    std::string role_str     = "standalone";
    std::string host         = "127.0.0.1";
    std::string port_str     = "6379";
    std::string rdb_path     = "redis.rdb";
    std::string leader_host  = "";
    std::string leader_port_str = "6379";

    get_arg(argc, argv, "--role",        role_str);
    get_arg(argc, argv, "--host",        host);
    get_arg(argc, argv, "--port",        port_str);
    get_arg(argc, argv, "--rdb",         rdb_path);
    get_arg(argc, argv, "--leader-host", leader_host);
    get_arg(argc, argv, "--leader-port", leader_port_str);

    // Determine role
    redis::NodeRole role = redis::NodeRole::Standalone;
    if (role_str == "leader")  role = redis::NodeRole::Leader;
    if (role_str == "replica") role = redis::NodeRole::Replica;

    int port        = std::atoi(port_str.c_str());
    int leader_port = std::atoi(leader_port_str.c_str());

    if (port <= 0 || port > 65535) {
        std::cerr << "[Main] ERROR: Invalid port: " << port_str << std::endl;
        return 1;
    }

    // Replica requires leader-host
    if (role == redis::NodeRole::Replica && leader_host.empty()) {
        std::cerr << "[Main] ERROR: --role replica requires --leader-host <addr>" << std::endl;
        return 1;
    }

    std::cout << "[Main] Starting VeloKV — "
              << role_str << " mode, port=" << port
              << ", rdb=" << rdb_path << std::endl;

    // ── Initialize networking ─────────────────────────────────────────────
    if (!redis::initialize_network()) {
        std::cerr << "[Main] ERROR: Failed to initialize networking subsystem." << std::endl;
        return 1;
    }

    // ── Signal handlers ───────────────────────────────────────────────────
    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    // ── Build ReplicationManager (if needed) ──────────────────────────────
    // Heap-allocate so that Server can hold a non-owning pointer that outlives
    // the Server object (both destroyed at end of main scope in correct order).
    redis::ReplicationManager repl_mgr(role);

    // ── Create and initialize the Server ─────────────────────────────────
    redis::Server server(host, port,
                         role,
                         (role != redis::NodeRole::Standalone) ? &repl_mgr : nullptr,
                         rdb_path);

    g_server = &server;

    if (!server.init()) {
        std::cerr << "[Main] ERROR: Server initialization failed." << std::endl;
        g_server = nullptr;
        redis::cleanup_network();
        return 1;
    }

    // ── Replica: connect to leader BEFORE the event loop ─────────────────
    // We connect here (not inside start()) so that leader_fd_ is registered
    // in the very first select() call.  If the connection fails, the replica
    // starts anyway and retries via try_reconnect() inside the event loop.
    //
    if (role == redis::NodeRole::Replica) {
        if (!repl_mgr.connect_to_leader(leader_host, leader_port)) {
            std::cout << "[Main] WARNING: Could not connect to leader at startup. "
                         "Will retry in the event loop." << std::endl;
        }
    }

    // ── Run the event loop ────────────────────────────────────────────────
    server.start();

    // ── Cleanup ───────────────────────────────────────────────────────────
    g_server = nullptr;
    redis::cleanup_network();

    std::cout << "[Main] VeloKV shutdown complete." << std::endl;
    return 0;
}
