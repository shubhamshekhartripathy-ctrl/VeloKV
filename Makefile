# Compiler settings
# Upgraded to C++17 for structured bindings (auto& [k, v]), std::string_view, etc.
CXX      = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -Wpedantic -Isrc

# Detect OS to handle platform-specific files and cleanup commands
ifeq ($(OS),Windows_NT)
    LIBS      = -lws2_32
    TARGET    = redis_server.exe
    CLEAN_CMD = del /q /f src\*.o $(TARGET) redis.rdb.tmp 2>nul || exit 0
else
    LIBS      =
    TARGET    = redis_server
    CLEAN_CMD = rm -f src/*.o $(TARGET) redis.rdb.tmp
endif

# Source and object files
SRCS = src/common.cpp      \
       src/db.cpp           \
       src/persistence.cpp  \
       src/resp.cpp         \
       src/command.cpp      \
       src/client.cpp       \
       src/replication.cpp  \
       src/server.cpp       \
       src/main.cpp

OBJS = $(SRCS:.cpp=.o)

# Default target
all: $(TARGET)

# Link the executable
$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(OBJS) $(LIBS)

# Compile source files into object files
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Remove all build artifacts (preserves redis.rdb so data survives a clean build)
clean:
	$(CLEAN_CMD)

.PHONY: all clean
