# FactorIA

FactorIA is a native desktop bridge between an AI model and Factorio. The application supports local llama.cpp and hosted OpenRouter models through OpenAI-compatible chat completions, and exposes typed gameplay actions which are executed in Factorio over Source RCON.

## Current capabilities

The initial application shell provides:

- a portable wxWidgets user interface;
- persistent Factorio, llama.cpp, and OpenRouter connection settings;
- a portable Asio Source RCON client;
- RCON authentication, disconnect, and a controlled Factorio status command;
- local and hosted provider connection tests plus OpenAI-compatible tool-call parsing;
- a bounded or non-stop observe/decide/execute agent loop with cancellation between calls;
- typed game-state, inventory, craftability, nearby-entity, walking, stop, crafting, placement, machine-inventory, and container-transfer tools;
- research discovery and queue control, assembler recipe selection, lab science-pack loading, and generic burner fuel handling;
- blueprint-ghost and deconstruction-mark discovery plus bounded, normal-movement tools that service those explicit player requests;
- nearest-first spatial scans up to 8,192 tiles, coordinate-targeted walking, and continuous counted resource mining;
- long-range resource surveys that group ore into patch regions and provide exact walking targets;
- generated-terrain water surveys that return nearby walkable shoreline targets;
- build-aware positioning that pathfinds the character into reach before exact entity placement;
- Factorio-validated placement planning and verification for item, belt, fluid, electric, heat, and wall connections;
- model-visible screenshots through Factorio's `script-output` directory;
- collision-aware `walk_to` routing and tick-synchronized walking and mining through the bundled FactorIA Bridge mod;
- optional persistent AI-owned character control, keeping the human player's character and inventory separate;
- an Agent tab for objectives, execution limits up to 999 rounds, optional non-stop execution, final output, and full tool tracing;
- SQLite-backed agent sessions that persist safe conversation checkpoints, model turns, tool calls/results, and bridge traffic across application restarts;
- a desktop Database tab for inspecting recorded sessions and resuming a saved general-agent context;
- a mobile-friendly local web panel with real-time WebSocket status and model decisions plus run, stop, connect, and disconnect controls;
- a structured, timestamped application log containing redacted model requests, provider responses, decision summaries, tool calls, and tool results.

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

### Headless Linux server

Use the bundled launcher to run a local headless server without pausing when no
human player is connected:

```bash
./scripts/start-factorio-server.sh ~/.factorio/saves/my-save.zip
```

The launcher uses a separate write-data directory under
`~/.local/state/factoria/factorio-server` by default. This keeps the server's
`.lock`, logs, autosaves, and mod state separate from `~/.factorio`, allowing a
graphical Factorio client on the same machine to start and connect normally.
Set `FACTORIO_SERVER_ROOT` or `FACTORIO_EXECUTABLE` to override the default
locations. The RCON listener defaults to `127.0.0.1:27015`; the launcher asks
for its password without echoing it. Set `FACTORIO_RCON_PASSWORD` when starting
the server non-interactively through a service manager.

Settings are stored in the platform-specific user configuration directory under `FactorIA/settings.json`.
The same directory contains `FactorIA/history.sqlite3`, which retains agent checkpoints and a redacted debugging history. Provider keys, RCON passwords, image payloads in the history viewer, and provider reasoning are not exposed there.
Use **Save checkpoint** and **Resume saved context** in the Agent tab to continue a general-agent run after restarting. The Database tab lists sessions, structured model/bridge events, and older event pages for debugging.

### Mobile web control

FactorIA starts an embedded HTTP server on port `8090`. From a phone on the same network, open
`http://<computer-lan-ip>:8090/` to send an objective, choose bounded or non-stop execution, stop the agent,
connect or disconnect Factorio, and watch the compact model-decision feed update over WebSocket in real time.
The desktop log reports whether the listener started successfully.

The panel exposes no provider key, RCON password, raw model request, or tool trace. It currently uses plain HTTP
without authentication and listens on all network interfaces, so expose port `8090` only on a trusted LAN or
restrict it with the host firewall.

## FactorIA Bridge mod

For collision-aware navigation and smooth continuous mining, copy the bundled
`factorio-mod/factoria-bridge_0.1.0` directory into Factorio's `mods` directory and restart Factorio.
`get_game_state` reports `bridge_runtime_available: true` when the current bridge loaded correctly. The updated
mod is required for walking and mining; FactorIA no longer falls back to rapid RCON input pulses.

The mod requests paths using the active character's real collision box and collision mask, so water, cliffs,
buildings, and other impassable terrain are considered by Factorio rather than inferred by the language model.
Gameplay behavior lives in the bundled Lua mod: it owns observations, construction, movement, mining, crafting,
placement, inventory transfers, and storage-backed tick jobs. The C++ application validates typed model calls,
forwards JSON requests to the mod's `/factoria-bridge` RCON command, waits or cancels when needed, and returns the
bridge result to the AI provider. The C++ application never constructs or executes Lua; model-generated Lua is
never run, and no gameplay module is uploaded over RCON.

The bridge also records completed typed crafting, building, and mining actions for trigger-based technologies.
This is required for the dedicated AI character because it is not attached to a `LuaPlayer` and therefore does
not emit Factorio's normal player action events. `craft` waits for its requested batch to finish and reports the
trigger progress or unlock directly, preventing repeated trigger-item production when one action is sufficient.

Enable **Use dedicated AI character** in the Connections tab to make the bridge create and persist a separate
blue character on the normal player force. Every observation and action then targets that character, including
inventory, crafting, movement, mining, building, pathfinding, and screenshots. Leave the option disabled to keep
controlling the first connected player's character. The dedicated character starts with an empty inventory and
must gather its own resources.

### In-game agent commands

The bridge mod provides one player-facing console command for inspecting and recovering the dedicated character:

```text
/factoria-agent status
/factoria-agent spawn
/factoria-agent build-ghosts
/factoria-agent remove-markers
/factoria-agent come
/factoria-agent goto
/factoria-agent remove
```

`status` is available to every player and does not spawn the character as a side effect. The other operations
require administrator permission when invoked by a player. `come` moves the agent beside the invoking player and
stops active bridge runtime jobs first; `goto` moves the invoking player beside the agent. `remove` spills the
agent's inventories at its position before removing it. The desktop agent can start another action after `come`
or `remove`, so stop the run in FactorIA first when performing manual recovery.

`build-ghosts` and `remove-markers` queue persistent tasks for the connected desktop application. Each switches the
model to a focused prompt: `build-ghosts` services entity ghosts, while `remove-markers` locates and mines only
entities explicitly marked for deconstruction. Both operate within 64 tiles of the dedicated character and prevent
unrelated factory expansion or rocket work. When no matching requests remain, the agent walks normally back to the
player who issued the command; the return tool tracks that player's current position and rechecks the task before
reporting success. The desktop checks for commands once per second without spending model tokens, interrupts a
general agent run when a task arrives, and resumes a claimed task after a desktop restart. Keep FactorIA connected
to RCON with a working AI provider configuration for queued tasks to run.

## Screenshot tool

Set **Factorio user data directory** on the Connections tab. FactorIA asks non-headless Factorio to write a PNG
under `script-output/FactorIA`, waits for the completed file, and sends it to llama.cpp as an `image_url` data URL.
The screenshot tool therefore requires:

- FactorIA filesystem access to the same Factorio user data directory (or a shared directory for remote Factorio);
- a non-headless Factorio instance;
- a vision-capable llama.cpp model with its multimodal projector configured.

For OpenRouter, FactorIA checks the selected model's advertised input modalities and omits the screenshot tool
from text-only models. This prevents a captured image from causing the next completion request to fail.

The application log summarizes each model request and response without dumping repeated conversation history,
tool schemas, image payloads, or provider reasoning. It retains the selected model, context and tool counts,
token usage, final assistant content, and the agent's observable decision, tool call, and tool result for each step.

## OpenRouter

Select **OpenRouter** on the Connections tab, enter an API key, and choose **Fetch models**. FactorIA loads the
tool-capable models available under the key's provider preferences, privacy settings, and guardrails; select one
from the dropdown and use **Test AI provider**. Enable **Free models only** before fetching to include only models
whose catalog pricing reports zero prompt and completion cost. FactorIA connects to `https://openrouter.ai/api/v1`, authenticates
with a Bearer token, and uses the same bounded tool loop as the local provider.

The API key is stored in the same local `FactorIA/settings.json` file as the existing connection credentials
and is never written to the application log. Choose a model that supports tool calling; FactorIA automatically
exposes screenshot inspection only when the selected OpenRouter model also advertises vision input support.

Start `llama-server` with a tool-capable template and `--jinja`. FactorIA disables parallel tool calls and validates every tool argument before executing it. Raw Lua is not exposed to the model.
