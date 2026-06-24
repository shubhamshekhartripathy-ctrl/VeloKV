#include "common.hpp"

#ifdef _WIN32
    #include <sstream>
#else
    #include <unistd.h>
    #include <fcntl.h>
    #include <cstring>
    #include <cerrno>
#endif

namespace redis {

bool initialize_network() {
#ifdef _WIN32
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        return false;
    }
#endif
    return true;
}

void cleanup_network() {
#ifdef _WIN32
    WSACleanup();
#endif
}

bool close_socket(socket_t fd) {
    if (fd == INVALID_SOCKET_VAL) {
        return true;
    }
#ifdef _WIN32
    int res = closesocket(fd);
    return res == 0;
#else
    int res = close(fd);
    return res == 0;
#endif
}

bool set_nonblocking(socket_t fd) {
    if (fd == INVALID_SOCKET_VAL) {
        return false;
    }
#ifdef _WIN32
    u_long mode = 1;
    int result = ioctlsocket(fd, FIONBIO, &mode);
    return result == 0;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return false;
    }
    int result = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return result == 0;
#endif
}

bool is_would_block() {
#ifdef _WIN32
    int err = WSAGetLastError();
    return (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS);
#else
    return (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS);
#endif
}

std::string get_last_error_string() {
#ifdef _WIN32
    int err = WSAGetLastError();
    wchar_t* s = nullptr;
    FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&s), 0, nullptr
    );
    if (s != nullptr) {
        // Convert wide string to standard string
        std::wstring ws(s);
        std::string str(ws.begin(), ws.end());
        LocalFree(s);
        // Strip trailing newlines
        while (!str.empty() && (str.back() == '\n' || str.back() == '\r')) {
            str.pop_back();
        }
        return "Socket error " + std::to_string(err) + ": " + str;
    }
    return "Socket error " + std::to_string(err);
#else
    return std::string(strerror(errno));
#endif
}

} // namespace redis
