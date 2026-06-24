#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include "db.hpp"

namespace redis {

// Callback function type for command handlers
// Takes a vector of command arguments (including the command name at index 0)
// and a reference to the Database storage instance.
// Returns a serialized RESP response.
using CommandHandler = std::function<std::string(const std::vector<std::string>&, Database&)>;

class CommandProcessor {
public:
    CommandProcessor();

    /**
     * @brief Executes a parsed command by routing it to its registered handler.
     * @param cmd The parsed command arguments. cmd[0] is the command name.
     * @param db Reference to the storage engine.
     * @return Serialized RESP string response.
     */
    std::string execute(const std::vector<std::string>& cmd, Database& db);

    /**
     * @brief Registers a command handler. Command names are registered/matched case-insensitively.
     * @param name The name of the command.
     * @param handler The callback handler logic.
     */
    void register_handler(const std::string& name, CommandHandler handler);

private:
    std::unordered_map<std::string, CommandHandler> handlers_;

    // Registers all standard commands (e.g., PING)
    void register_builtin_commands();
};

} // namespace redis
