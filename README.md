# FactorIA

FactorIA is a native desktop bridge between a locally hosted language model and Factorio. The application talks to llama.cpp through its OpenAI-compatible HTTP API and exposes typed gameplay actions which are executed in Factorio over Source RCON.

## Current capabilities

The initial application shell provides:

- a portable wxWidgets user interface;
- persistent Factorio and llama.cpp connection settings;
- a portable Asio Source RCON client;
- RCON authentication, disconnect, and a controlled Factorio status command;
- llama.cpp health checks and OpenAI-compatible tool-call parsing;
- a bounded observe/decide/execute agent loop with cancellation between calls;
- typed game-state, inventory, nearby-entity, walking, stop, and crafting tools;
- nearest-first spatial scans, coordinate-targeted walking, and real-time entity mining;
- an Agent tab for objectives, execution limits, final output, and full tool tracing;
- a dedicated timestamped application log.

## Requirements

- Visual Studio with the MSVC `v145` toolset
- C++ language preview supporting the C++26 target
- vcpkg integration enabled for Visual Studio
- Factorio started with RCON enabled

Open `FactorIA.sln`, select `Debug|x64` or `Release|x64`, and build. Dependencies are declared in `vcpkg.json`.

Example Factorio arguments:

```text
factorio.exe --rcon-port 27015 --rcon-password mypassword
```

Settings are stored in the platform-specific user configuration directory under `FactorIA/settings.json`.

Start `llama-server` with a tool-capable template and `--jinja`. FactorIA disables parallel tool calls and validates every tool argument before sending a fixed Lua operation through RCON. Raw Lua is not exposed to the model.
