#pragma once

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    using socket_t = SOCKET;
    #define INVALID_SOCKET_VAL INVALID_SOCKET
    #define SOCKET_ERROR_VAL SOCKET_ERROR
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <arpa/inet.h>
    using socket_t = int;
    #define INVALID_SOCKET_VAL -1
    #define SOCKET_ERROR_VAL -1
#endif

#include <string>

namespace redis {

/**
 * @brief Initialize socket libraries (specifically for Windows Winsock).
 * @return true if initialization succeeded, false otherwise.
 */
bool initialize_network();

/**
 * @brief Clean up socket libraries.
 */
void cleanup_network();

/**
 * @brief Close a socket cleanly.
 * @param fd The socket descriptor.
 * @return true if successfully closed, false otherwise.
 */
bool close_socket(socket_t fd);

/**
 * @brief Configure a socket to be non-blocking.
 * @param fd The socket descriptor.
 * @return true if successful, false otherwise.
 */
bool set_nonblocking(socket_t fd);

/**
 * @brief Check if the last socket error indicates that the operation would block.
 * @return true if the error is WOULDBLOCK/EAGAIN.
 */
bool is_would_block();

/**
 * @brief Get a string representation of the last socket error.
 * @return Error message.
 */
std::string get_last_error_string();

} // namespace redis
