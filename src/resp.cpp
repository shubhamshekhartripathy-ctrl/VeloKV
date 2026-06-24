#include "resp.hpp"
#include <stdexcept>
#include <iostream>

namespace redis {

enum class ParseResult {
    SUCCESS,
    INCOMPLETE,
    PROTOCOL_ERROR
};

// Internal parsing helper
static ParseResult parse_single_command(const std::string& buffer, size_t& offset, Command& out_cmd) {
    if (offset >= buffer.size()) {
        return ParseResult::INCOMPLETE;
    }

    if (buffer[offset] == '*') {
        // RESP Array format (standard Redis client requests)
        size_t next_crlf = buffer.find("\r\n", offset);
        if (next_crlf == std::string::npos) {
            return ParseResult::INCOMPLETE;
        }

        // Parse array size (number of elements)
        std::string len_str = buffer.substr(offset + 1, next_crlf - (offset + 1));
        int num_elements = 0;
        try {
            num_elements = std::stoi(len_str);
        } catch (...) {
            return ParseResult::PROTOCOL_ERROR;
        }

        if (num_elements < 0) {
            // Null array or negative array size.
            offset = next_crlf + 2;
            return ParseResult::SUCCESS;
        }

        size_t current_offset = next_crlf + 2;
        Command cmd;
        cmd.reserve(num_elements);

        for (int i = 0; i < num_elements; ++i) {
            if (current_offset >= buffer.size()) {
                return ParseResult::INCOMPLETE;
            }
            if (buffer[current_offset] != '$') {
                return ParseResult::PROTOCOL_ERROR;
            }

            size_t next_bulk_crlf = buffer.find("\r\n", current_offset);
            if (next_bulk_crlf == std::string::npos) {
                return ParseResult::INCOMPLETE;
            }

            // Parse bulk string length
            std::string bulk_len_str = buffer.substr(current_offset + 1, next_bulk_crlf - (current_offset + 1));
            int bulk_len = 0;
            try {
                bulk_len = std::stoi(bulk_len_str);
            } catch (...) {
                return ParseResult::PROTOCOL_ERROR;
            }

            if (bulk_len < 0) {
                // Null bulk string represented as empty string in our command list
                cmd.push_back("");
                current_offset = next_bulk_crlf + 2;
                continue;
            }

            size_t str_start = next_bulk_crlf + 2;
            if (str_start + bulk_len + 2 > buffer.size()) {
                return ParseResult::INCOMPLETE;
            }

            // Verify trailing CRLF
            if (buffer[str_start + bulk_len] != '\r' || buffer[str_start + bulk_len + 1] != '\n') {
                return ParseResult::PROTOCOL_ERROR;
            }

            cmd.push_back(buffer.substr(str_start, bulk_len));
            current_offset = str_start + bulk_len + 2;
        }

        out_cmd = std::move(cmd);
        offset = current_offset;
        return ParseResult::SUCCESS;

    } else {
        // Inline Command format (fallback for simple netcat/telnet connections)
        size_t next_lf = buffer.find('\n', offset);
        if (next_lf == std::string::npos) {
            return ParseResult::INCOMPLETE;
        }

        std::string line = buffer.substr(offset, next_lf - offset);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        // Parse line into space-delimited arguments, respecting quotes optionally
        Command cmd;
        std::string current_arg;
        bool in_quotes = false;
        for (size_t i = 0; i < line.size(); ++i) {
            char c = line[i];
            if (c == '"') {
                in_quotes = !in_quotes;
            } else if (c == ' ' && !in_quotes) {
                if (!current_arg.empty()) {
                    cmd.push_back(current_arg);
                    current_arg.clear();
                }
            } else {
                current_arg.push_back(c);
            }
        }
        if (!current_arg.empty()) {
            cmd.push_back(current_arg);
        }

        // Only add non-empty commands
        if (!cmd.empty()) {
            out_cmd = std::move(cmd);
        }
        offset = next_lf + 1;
        return ParseResult::SUCCESS;
    }
}

bool RESPParser::parse(std::string& input_buffer, std::vector<Command>& out_commands) {
    size_t offset = 0;
    bool parsed_any = false;

    while (offset < input_buffer.size()) {
        Command cmd;
        size_t temp_offset = offset;
        ParseResult result = parse_single_command(input_buffer, temp_offset, cmd);

        if (result == ParseResult::SUCCESS) {
            if (!cmd.empty()) {
                out_commands.push_back(std::move(cmd));
                parsed_any = true;
            }
            offset = temp_offset;
        } else if (result == ParseResult::INCOMPLETE) {
            // Buffer contains incomplete data, wait for more.
            break;
        } else if (result == ParseResult::PROTOCOL_ERROR) {
            // Protocol error. Discard the remainder of the buffer to prevent lockups.
            std::cerr << "[RESP Error] Protocol violation. Clearing input buffer." << std::endl;
            input_buffer.clear();
            return parsed_any;
        }
    }

    if (offset > 0) {
        // Erase consumed bytes from the input buffer
        input_buffer.erase(0, offset);
    }

    return parsed_any;
}

std::string RESPParser::serialize_simple_string(const std::string& str) {
    return "+" + str + "\r\n";
}

std::string RESPParser::serialize_error(const std::string& err) {
    return "-" + err + "\r\n";
}

std::string RESPParser::serialize_bulk_string(const std::string& str) {
    return "$" + std::to_string(str.size()) + "\r\n" + str + "\r\n";
}

std::string RESPParser::serialize_null_bulk_string() {
    return "$-1\r\n";
}

std::string RESPParser::serialize_integer(long long val) {
    return ":" + std::to_string(val) + "\r\n";
}

std::string RESPParser::serialize_array(const std::vector<std::string>& elements) {
    std::string result = "*" + std::to_string(elements.size()) + "\r\n";
    for (const auto& elem : elements) {
        result += serialize_bulk_string(elem);
    }
    return result;
}

} // namespace redis
