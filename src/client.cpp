#include "client.hpp"
#include <iostream>

namespace redis {

Client::Client(socket_t fd) : fd_(fd) {}

Client::~Client() {
    if (fd_ != INVALID_SOCKET_VAL) {
        close_socket(fd_);
    }
}

// Move constructor
Client::Client(Client&& other) noexcept 
    : fd_(other.fd_), 
      input_buffer_(std::move(other.input_buffer_)), 
      output_buffer_(std::move(other.output_buffer_)) {
    other.fd_ = INVALID_SOCKET_VAL;
}

// Move assignment operator
Client& Client::operator=(Client&& other) noexcept {
    if (this != &other) {
        if (fd_ != INVALID_SOCKET_VAL) {
            close_socket(fd_);
        }
        fd_ = other.fd_;
        input_buffer_ = std::move(other.input_buffer_);
        output_buffer_ = std::move(other.output_buffer_);
        other.fd_ = INVALID_SOCKET_VAL;
    }
    return *this;
}

bool Client::read_from_socket() {
    char buf[4096];
    
#ifdef _WIN32
    int bytes_received = recv(fd_, buf, sizeof(buf), 0);
#else
    ssize_t bytes_received = recv(fd_, buf, sizeof(buf), 0);
#endif

    if (bytes_received > 0) {
        input_buffer_.append(buf, bytes_received);
        return true;
    } else if (bytes_received == 0) {
        // Connection closed by client
        return false;
    } else {
        // Socket error
        if (is_would_block()) {
            return true; // No data available right now, not a fatal error
        }
        std::cerr << "[Client Error] recv failed: " << get_last_error_string() << std::endl;
        return false;
    }
}

bool Client::write_to_socket() {
    if (output_buffer_.empty()) {
        return true;
    }

#ifdef _WIN32
    int bytes_sent = send(fd_, output_buffer_.data(), static_cast<int>(output_buffer_.size()), 0);
#else
    ssize_t bytes_sent = send(fd_, output_buffer_.data(), output_buffer_.size(), 0);
#endif

    if (bytes_sent > 0) {
        // Erase sent bytes from output buffer
        output_buffer_.erase(0, bytes_sent);
        return true;
    } else if (bytes_sent == 0) {
        return false;
    } else {
        if (is_would_block()) {
            return true; // Socket buffer is full, try again later
        }
        std::cerr << "[Client Error] send failed: " << get_last_error_string() << std::endl;
        return false;
    }
}

void Client::queue_response(const std::string& response) {
    output_buffer_.append(response);
}

} // namespace redis
