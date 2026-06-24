#pragma once

#include <string>
#include <vector>

namespace redis {

using Command = std::vector<std::string>;

class RESPParser {
public:
    /**
     * @brief Parse input buffer for complete commands (RESP or inline format).
     *        Consumes successfully parsed commands from the input buffer.
     * @param input_buffer Reference to the client's input buffer.
     * @param out_commands Vector to append parsed commands to.
     * @return true if one or more commands were parsed.
     */
    static bool parse(std::string& input_buffer, std::vector<Command>& out_commands);

    // Serialization utilities for Redis RESP response formats

    /**
     * @brief Serialize a standard simple string. e.g. "+PONG\r\n"
     */
    static std::string serialize_simple_string(const std::string& str);

    /**
     * @brief Serialize an error message. e.g. "-ERR unknown command\r\n"
     */
    static std::string serialize_error(const std::string& err);

    /**
     * @brief Serialize a bulk string. e.g. "$5\r\nhello\r\n"
     */
    static std::string serialize_bulk_string(const std::string& str);

    /**
     * @brief Serialize a null bulk string. e.g. "$-1\r\n"
     */
    static std::string serialize_null_bulk_string();

    /**
     * @brief Serialize a standard integer. e.g. ":1000\r\n"
     */
    static std::string serialize_integer(long long val);

    /**
     * @brief Serialize an array of bulk strings.
     */
    static std::string serialize_array(const std::vector<std::string>& elements);
};

} // namespace redis
