#pragma once

#include "common.hpp"
#include <string>

namespace redis {

class Client {
public:
    explicit Client(socket_t fd);
    ~Client();

    // Disable copy semantics to prevent socket descriptor leaks or double closes
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Enable move semantics
    Client(Client&& other) noexcept;
    Client& operator=(Client&& other) noexcept;

    /**
     * @brief Gets the socket descriptor.
     */
    socket_t fd() const { return fd_; }

    /**
     * @brief Reads data from socket into the input buffer.
     * @return true if successful (including WOULDBLOCK), false if connection closed or error.
     */
    bool read_from_socket();

    /**
     * @brief Flushes data from the output buffer to the socket.
     * @return true if successful (including WOULDBLOCK), false on write error.
     */
    bool write_to_socket();

    /**
     * @brief Checks if there is pending data to write in the queue.
     */
    bool has_pending_write() const { return !output_buffer_.empty(); }

    /**
     * @brief Queues response payload to be sent.
     */
    void queue_response(const std::string& response);

    /**
     * @brief Gets a reference to the input buffer.
     */
    std::string& input_buffer() { return input_buffer_; }

private:
    socket_t fd_;
    std::string input_buffer_;
    std::string output_buffer_;
};

} // namespace redis
