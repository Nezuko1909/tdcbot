# dcbot

A lightweight D++ Discord bot test code :V in C++.

## Requirements

- CMake 3.16+ 
- A C++17-compatible compiler
- Git for dependencies (FetchContent)

## Configuration

1. Copy `config/.env.example` to `.env`.
2. Set `DISCORD_TOKEN` in `.env`.
3. Optionally set `DISCORD_GUILD_ID` for guild-scoped command registration.

## Build

```bash
mkdir -p build
cd build
cmake ..
cmake --build . --config Release
```

## Run

```bash
./bin/dcbot
```

## Project structure

- `include/`: public headers
- `src/`: implementation files and feature modules
- `src/commands/`: slash command definitions and handlers
- `src/events/`: event registration and dispatch
- `src/handlers/`: command dispatch logic
- `src/config/`: config loading from `.env` or `config/config.json`
- `src/utils/`: helper utilities like logging
- `config/`: example config templates
- `tests/`: optional unit tests

## Notes

- Do not commit `.env` or actual bot tokens.
- `DPP` is loaded via CMake `FetchContent`.
