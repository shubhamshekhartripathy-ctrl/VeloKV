# Walkthrough - Redis-Inspired Key-Value Database (Milestone 2)

Milestone 2 is complete! We have decoupled the network loop and command parser layers from the data storage layer and successfully implemented the core database commands `SET`, `GET`, `DEL`, and `EXISTS`.

---

## Folder Structure

Here is the updated project structure containing the new `db` component:
```
d:/redis/
├── Makefile                # GCC compiler configuration (includes src/db.cpp)
└── src/
    ├── main.cpp            # Server bootstrap
    ├── common.hpp          # Socket definitions
    ├── common.cpp          # Platform-specific socket wrappers
    ├── client.hpp          # Client buffers
    ├── client.cpp          # Non-blocking socket I/O
    ├── resp.hpp            # Decoders & RESP encoders
    ├── resp.cpp            # String stream processing
    ├── command.hpp         # Command processor registry (updated for DB ref)
    ├── command.cpp         # Custom command handlers (PING, GET, SET, DEL, EXISTS)
    ├── db.hpp              # [NEW] Storage engine class declaration
    ├── db.cpp              # [NEW] Storage engine methods (std::unordered_map wrapper)
    ├── server.hpp          # TCP Event Loop (updated to hold Database member)
    └── server.cpp          # select() event router
```

---

## File Roles & Responsibilities

### 1. Storage Engine Core (`src/db.hpp` / `src/db.cpp`) [NEW]
- **Responsibility**: Manages in-memory data storage, isolated from the network loop and raw protocols.
- **Key Details**: Wraps a `std::unordered_map<std::string, std::string>`. Access methods (`set`, `get`, `del`, `exists`) are written in standard C++ types. Average lookup time is $O(1)$. Detailed comments were added explaining time complexities and hash collisions to prepare for technical interviews.

### 2. Command Processor (`src/command.hpp` / `src/command.cpp`) [MODIFIED]
- **Responsibility**: Routes, validates, and processes incoming commands against the storage engine.
- **Key Details**: 
  - Handlers were updated to accept a `Database&` context reference.
  - Case-insensitive matching is applied to command words (e.g. `set` matches `SET`).
  - Added key-value commands:
    - **`SET <key> <value>`**: Associates key with value. Returns simple string `+OK\r\n`.
    - **`GET <key>`**: Looks up key. Returns its value as a bulk string (`$<len>\r\n<value>\r\n`), or returns a null bulk string (`$-1\r\n`) if missing.
    - **`DEL <key>`**: Deletes key. Returns simple string `+OK\r\n`.
    - **`EXISTS <key>`**: Checks key. Returns integer `:1\r\n` (exists) or `:0\r\n` (missing).

### 3. Server Coordinator (`src/server.hpp` / `src/server.cpp`) [MODIFIED]
- **Responsibility**: Runs the single-threaded event loop and passes its `Database` instance to the command execution processor.
- **Key Details**: Added a `Database db_;` member variable and updated the execution thread:
  ```cpp
  std::string response = processor_.execute(cmd, db_);
  ```

---

## Verification & Tests

### 1. Compilation Verification
The compilation compiles cleanly under `mingw32-make` on Windows:
```
del /q /f src\*.o redis_server.exe 2>nul || exit 0
g++ -std=c++14 -O3 -Wall -Wextra -Isrc -c src/common.cpp -o src/common.o
g++ -std=c++14 -O3 -Wall -Wextra -Isrc -c src/db.cpp -o src/db.o
g++ -std=c++14 -O3 -Wall -Wextra -Isrc -c src/resp.cpp -o src/resp.o
g++ -std=c++14 -O3 -Wall -Wextra -Isrc -c src/command.cpp -o src/command.o
g++ -std=c++14 -O3 -Wall -Wextra -Isrc -c src/client.cpp -o src/client.o
g++ -std=c++14 -O3 -Wall -Wextra -Isrc -c src/server.cpp -o src/server.o
g++ -std=c++14 -O3 -Wall -Wextra -Isrc -c src/main.cpp -o src/main.o
g++ -std=c++14 -O3 -Wall -Wextra -Isrc -o redis_server.exe src/common.o src/db.o src/resp.o src/command.o src/client.o src/server.o src/main.o -lws2_32
```

### 2. Functional Tests
A verification test script ([test_milestone_2.ps1](file:///C:/Users/ASUS/.gemini/antigravity-ide/brain/5bd94325-dea1-44ab-9ec1-5c7e4f8a73cf/scratch/test_milestone_2.ps1)) was run to check the network protocols:

```powershell
powershell -File C:\Users\ASUS\.gemini\antigravity-ide\brain\5bd94325-dea1-44ab-9ec1-5c7e4f8a73cf/scratch/test_milestone_2.ps1
```

**Results**:
- **`SET name krish`**: Received simple string `+OK`
- **`GET name`**: Received length and value `$5 krish`
- **`EXISTS name`**: Received integer `:1`
- **`DEL name`**: Received simple string `+OK`
- **`GET name` (after deletion)**: Received null bulk string `$-1`
- **`EXISTS name` (after deletion)**: Received integer `:0`
- **Command Case-Insensitivity (`set name second_value` / `get name`)**: Routed correctly, returned `+OK` and `$12 second_value`.
- **Key Case-Sensitivity (`EXISTS Name`)**: Correctly returned `:0` (proving keys are case-sensitive).
- **Syntax check (`SET name`)**: Returned syntax error `-ERR wrong number of arguments for 'set' command`.
