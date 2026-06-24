#include "common.hpp"
#include "server.hpp"
#include <iostream>
#include <signal.h>

namespace {
    // Global pointer to the server instance to allow signal handlers to stop it gracefully
    redis::Server* g_server = nullptr;
}

// Signal handler callback
void handle_signal(int sig) {
    std::cout << "\n[Main] Signal " << sig << " received. Stopping server gracefully..." << std::endl;
    if (g_server != nullptr) {
        g_server->stop();
    }
}

int main() {
    std::cout << "[Main] Starting Redis-inspired key-value server..." << std::endl;

    // 1. Initialize platform socket libraries (specifically Winsock on Windows)
    if (!redis::initialize_network()) {
        std::cerr << "[Main Error] Failed to initialize networking subsystem." << std::endl;
        return 1;
    }

    // 2. Set up signal handlers for Ctrl+C (SIGINT) and termination (SIGTERM)
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    // 3. Create and initialize the server on localhost port 6379
    redis::Server server("127.0.0.1", 6379);
    g_server = &server;

    if (server.init()) {
        // 4. Run the event loop (blocks until a signal is received or an error stops it)
        server.start();
    } else {
        std::cerr << "[Main Error] Server initialization failed." << std::endl;
    }

    // Reset global pointer and clean up socket library
    g_server = nullptr;
    redis::cleanup_network();

    std::cout << "[Main] Server shutdown complete. Goodbye!" << std::endl;
    return 0;
}
