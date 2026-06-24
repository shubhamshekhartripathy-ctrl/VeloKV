#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
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


/**
 * @brief Returns true if the command name is a data-modifying write operation.
 *
 * Used by:
 *  1. Server (leader mode): propagate the command to replicas after execution.
 *  2. Server (replica mode): reject the command from clients with READONLY error.
 *
 * The check is case-insensitive. Commands NOT in this list (GET, EXISTS, TTL,
 * LLEN, PING, SAVE) are read-only and safe to execute on replicas.
 *
 * @param name Command name (any case, e.g. "set", "SET", "Set").
 * @return true if the command modifies the database.
 */
bool is_write_command(const std::string& name);

} // namespace redis
