# FactorIA

FactorIA is a native desktop bridge between an AI model and Factorio. The application supports local llama.cpp and hosted OpenRouter models through OpenAI-compatible chat completions, and exposes typed gameplay actions which are executed in Factorio over Source RCON.

## Current capabilities

The initial application shell provides:

- a portable wxWidgets user interface;
- persistent Factorio, llama.cpp, and OpenRouter connection settings;
- a portable Asio Source RCON client;
- RCON authentication, disconnect, and a controlled Factorio status command;
- local and hosted provider connection tests plus OpenAI-compatible tool-call parsing;
- a bounded observe/decide/execute agent loop with cancellation between calls;
- typed game-state, inventory, craftability, nearby-entity, walking, stop, crafting, placement, machine-inventory, and container-transfer tools;
- nearest-first spatial scans, coordinate-targeted walking, and continuous counted resource mining;
- long-range resource surveys that group ore into patch regions and provide exact walking targets;
- model-visible screenshots through Factorio's `script-output` directory;
- collision-aware `walk_to` routing and tick-synchronized walking and mining through the bundled FactorIA Bridge mod;
- an Agent tab for objectives, execution limits, final output, and full tool tracing;
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

Settings are stored in the platform-specific user configuration directory under `FactorIA/settings.json`.

## FactorIA Bridge mod

For collision-aware navigation and smooth continuous mining, copy the bundled
`factorio-mod/factoria-bridge_0.1.0` directory into Factorio's `mods` directory and restart Factorio.
`get_game_state` reports `bridge_runtime_available: true` when the current bridge loaded correctly. The updated
mod is required for walking and mining; FactorIA no longer falls back to rapid RCON input pulses.

The mod requests paths using the active character's real collision box and collision mask, so water, cliffs,
buildings, and other impassable terrain are considered by Factorio rather than inferred by the language model.
FactorIA uploads its trusted player-control action module over RCON in small chunks and the mod executes active
actions on every game tick. Uploaded functions are session-local and are automatically uploaded again after a
save load or mod reload. Only typed, validated tools are exposed to the model; model-generated Lua is never run.

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
from the dropdown and use **Test AI provider**. FactorIA connects to `https://openrouter.ai/api/v1`, authenticates
with a Bearer token, and uses the same bounded tool loop as the local provider.

The API key is stored in the same local `FactorIA/settings.json` file as the existing connection credentials
and is never written to the application log. Choose a model that supports tool calling; FactorIA automatically
exposes screenshot inspection only when the selected OpenRouter model also advertises vision input support.

Start `llama-server` with a tool-capable template and `--jinja`. FactorIA disables parallel tool calls and validates every tool argument before executing it. Raw Lua is not exposed to the model.
