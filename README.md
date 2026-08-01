# dcbot

A lightweight, modern, and modular Discord bot written in C++17 powered by the [D++ (DPP)](https://dpp.dev/) library and `libcurl`.

[![C++ Standard](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![CMake](https://img.shields.io/badge/CMake-3.16%2B-green.svg)](https://cmake.org/)
[![D++](https://img.shields.io/badge/D%2B%2B-Discord%20Bot%20Library-5865F2.svg)](https://dpp.dev/)

## Features

- **Slash Commands Support**: Pre-configured slash commands (`/ping`, `/help`, `/sauce`).
- **Context Menu Integration**: Right-click message context menu support (`Apps -> Sauce`) for seamless image source searching.
- **Flexible Configuration Loader**:
  - Automatically loads configuration from OS environment variables, `.env` file (root or `config/.env`), or `config/config.json`.
  - Supports optional guild-scoped command registration (`DISCORD_GUILD_ID`).
- **Custom Logging System**: Formatted console logging with timestamping (`YYYY-MM-DD HH:MM:SS`), severity levels (`INFO`, `WARN`, `ERROR`), and module tags.
- **Modular Code Architecture**: Clean separation between event handlers, command definitions, dispatching logic, configuration loading, and utilities.
- **Unit Testing Setup**: Optional test target configured with [Catch2 v3](https://github.com/catchorg/Catch2).

## Prerequisites

Before building the project, ensure you have the following installed:

- **C++17 Compatible Compiler**: GCC 9+, Clang 10+, or MSVC 2019+.
- **CMake**: Version 3.16 or higher.
- **D++ (libdpp)**: Installed system-wide (or available via `find_package(DPP)`).
- **libcurl**: HTTP library used for external API lookups.
- **Catch2 v3** *(Optional)*: Required only if building unit tests (`BUILD_TESTING=ON`).

### Installing D++ & Dependencies

- **Ubuntu / Debian**:
  ```bash
  sudo apt update
  sudo apt install build-essential cmake libcurl4-openssl-dev
  # Download and install D++ deb package from https://dl.dpp.dev/
  ```
- **Arch Linux**:
  ```bash
  sudo pacman -S cmake curl libdpp
  ```

## Configuration

1. Copy one of the configuration templates:
   ```bash
   cp config/.env.example .env
   # OR
   cp config/config.json.example config/config.json
   ```
2. Configure your environment variables:
   - `DISCORD_TOKEN`: *(Required)* Your Discord Bot Token.
   - `DISCORD_GUILD_ID`: *(Optional)* Your Discord Guild (Server) ID for fast command testing in a specific server.

> **Security Note**: Never commit your `.env` or `config.json` containing sensitive bot tokens to version control.

## Building the Project

### Standard CMake Build

```bash
# Create and navigate to build directory
mkdir -p build && cd build

# Generate build system
cmake ..

# Compile executable
cmake --build . --config Release
```

### Quick Build Script

Alternatively, you can run the included shell script from the repository root:

```bash
chmod +x build.sh
./build.sh
```

The compiled executable will be placed in `build/bin/dcbot`.

### Building Tests

To build unit tests, pass `-DBUILD_TESTING=ON` to CMake:

```bash
cd build
cmake -DBUILD_TESTING=ON ..
cmake --build .
ctest --output-on-failure
```

## Running the Bot

Once built, execute the binary from the root folder:

```bash
./build/bin/dcbot
```

## Commands Overview

| Command | Type | Description |
| :--- | :--- | :--- |
| `/ping` | Slash Command | Replies with "Pong!" to verify bot responsiveness. |
| `/help` | Slash Command | Lists all available bot commands and instructions. |
| `/sauce` | Slash Command | Accepts a Discord message link (`link`), fetches target message attachments, and performs image source lookup. |
| `Sauce` | Message Context Menu | Right-click any message -> **Apps** -> **Sauce** to search for image sources directly. |

## Project Structure

```text
dcbot/
├── CMakeLists.txt          # Main CMake build configuration
├── build.sh                # Helper script for fast compilation
├── config/
│   ├── .env.example        # Environment variable template
│   └── config.json.example # JSON configuration template
├── include/                # Public header files
│   ├── commands/           # Slash & Context command declarations (ping, help, sauce)
│   ├── config/             # Configuration structure & parser interface
│   ├── events/             # D++ event listener declarations
│   ├── handlers/           # Command routing & dispatching logic
│   └── utils/              # Helper utilities (Logging, etc.)
├── src/                    # Implementation files
│   ├── main.cpp            # Application entry point
│   ├── commands/           # Command implementations
│   ├── config/             # Config loader (.env & JSON parser)
│   ├── events/             # Event handler bindings
│   ├── handlers/           # Command dispatchers
│   └── utils/              # Logger implementation
└── tests/                  # Unit tests (Catch2)
    └── test_main.cpp       # Unit test runner
```

## Extending the Bot

To add a new slash command:
1. Create header in `include/commands/my_command.h` and implementation in `src/commands/my_command.cpp`.
2. Define `create_my_command()` and `handle_my_command()`.
3. Register the command in `src/events/handler.cpp` under `bot.on_ready()`.
4. Dispatch the command inside `src/handlers/command_dispatcher.cpp`.

## License

This project is licensed under open-source standards. Feel free to modify and adapt it for your own Discord bot projects.
