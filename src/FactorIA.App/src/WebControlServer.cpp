#include <FactorIA/WebControlServer.h>

#include <FactorIA/AgentController.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <utility>

#include <httplib.h>
#include <nlohmann/json.hpp>

namespace factoria
{
namespace
{
using json = nlohmann::json;

constexpr std::size_t MaximumDecisionHistory = 200;
constexpr std::size_t MaximumInstructionLength = 8 * 1024;
constexpr std::size_t MaximumBroadcastQueue = 512;

constexpr auto EmbeddedPage = R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
  <meta name="theme-color" content="#111827">
  <title>FactorIA Live</title>
  <style>
    :root { color-scheme: dark; font-family: Inter, ui-sans-serif, system-ui, -apple-system, sans-serif; background: #0b1020; color: #eef2ff; }
    * { box-sizing: border-box; }
    body { margin: 0; min-height: 100vh; background: radial-gradient(circle at 100% 0, #1f365d 0, transparent 34rem), #0b1020; }
    header { position: sticky; top: 0; z-index: 2; display: flex; align-items: center; justify-content: space-between; padding: calc(14px + env(safe-area-inset-top)) max(18px, env(safe-area-inset-right)) 14px max(18px, env(safe-area-inset-left)); background: rgba(11,16,32,.88); border-bottom: 1px solid #27334d; backdrop-filter: blur(16px); }
    h1 { margin: 0; font-size: 1.08rem; letter-spacing: .02em; }
    .live { display: flex; align-items: center; gap: 8px; color: #94a3b8; font-size: .82rem; }
    .dot { width: 9px; height: 9px; border-radius: 50%; background: #64748b; box-shadow: 0 0 0 4px rgba(100,116,139,.15); }
    .dot.online { background: #34d399; box-shadow: 0 0 0 4px rgba(52,211,153,.15); }
    main { width: min(780px, 100%); margin: 0 auto; padding: 18px max(14px, env(safe-area-inset-right)) calc(28px + env(safe-area-inset-bottom)) max(14px, env(safe-area-inset-left)); display: grid; gap: 14px; }
    .card { background: rgba(17,24,39,.9); border: 1px solid #293650; border-radius: 16px; padding: 16px; box-shadow: 0 18px 48px rgba(0,0,0,.2); }
    .status-grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 9px; }
    .status { min-width: 0; padding: 11px 12px; border: 1px solid #334155; border-radius: 12px; background: #111b2e; }
    .status span { display: block; color: #8fa2be; font-size: .72rem; text-transform: uppercase; letter-spacing: .08em; }
    .status strong { display: block; overflow: hidden; margin-top: 4px; font-size: .92rem; text-overflow: ellipsis; white-space: nowrap; }
    .ok { color: #6ee7b7; }
    .warn { color: #fbbf24; }
    h2 { margin: 0; font-size: 1rem; }
    label { display: block; margin: 14px 0 7px; color: #b8c4d8; font-size: .84rem; font-weight: 650; }
    textarea, input[type=number] { width: 100%; border: 1px solid #3a4965; border-radius: 11px; background: #0a1324; color: #f8fafc; font: inherit; outline: none; }
    textarea { min-height: 112px; padding: 12px; resize: vertical; line-height: 1.45; }
    input[type=number] { height: 46px; padding: 0 11px; }
    textarea:focus, input:focus { border-color: #60a5fa; box-shadow: 0 0 0 3px rgba(96,165,250,.16); }
    .options { display: grid; grid-template-columns: minmax(120px, 180px) 1fr; align-items: end; gap: 16px; }
    .toggle { display: flex; align-items: center; gap: 10px; min-height: 46px; margin: 0; color: #dbe5f3; }
    .toggle input { width: 20px; height: 20px; accent-color: #3b82f6; }
    .actions { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-top: 15px; }
    .connection-actions { display: flex; gap: 8px; margin-top: 10px; }
    button { min-height: 48px; border: 0; border-radius: 11px; padding: 0 16px; color: #f8fafc; background: #334155; font: inherit; font-weight: 750; cursor: pointer; touch-action: manipulation; }
    button.primary { background: #2563eb; }
    button.danger { background: #b42334; }
    button.secondary { min-height: 42px; flex: 1; background: #26344c; font-size: .87rem; }
    button:disabled { cursor: default; opacity: .4; }
    .feedback { min-height: 20px; margin: 10px 2px 0; color: #93c5fd; font-size: .83rem; }
    .feed-head { display: flex; align-items: baseline; justify-content: space-between; margin-bottom: 10px; }
    .feed-head span { color: #8fa2be; font-size: .78rem; }
    .inventory-grid { display: grid; grid-template-columns: minmax(0, 1fr) auto auto; gap: 1px; overflow: hidden; border: 1px solid #334155; border-radius: 10px; background: #334155; }
    .inventory-grid > div { min-width: 0; padding: 9px 10px; background: #0c1628; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
    .inventory-grid .head { background: #18243a; color: #8fa2be; font-size: .7rem; font-weight: 750; letter-spacing: .07em; text-transform: uppercase; }
    .inventory-grid .count { font-variant-numeric: tabular-nums; text-align: right; }
    .inventory-grid .empty { grid-column: 1 / -1; }
    #decisions { display: grid; max-height: 52vh; overflow-y: auto; gap: 8px; overscroll-behavior: contain; }
    .decision { padding: 11px 12px; border-left: 3px solid #3b82f6; border-radius: 8px; background: #0c1628; line-height: 1.4; overflow-wrap: anywhere; }
    .decision time { display: block; margin-bottom: 4px; color: #7890b0; font-size: .7rem; }
    .empty { padding: 26px 10px; color: #8191a9; text-align: center; }
    @media (max-width: 540px) {
      .status-grid { grid-template-columns: 1fr 1fr; }
      .status:last-child { grid-column: 1 / -1; }
      .card { padding: 14px; border-radius: 14px; }
      .options { grid-template-columns: 1fr 1.15fr; gap: 10px; }
      #decisions { max-height: none; }
    }
  </style>
</head>
<body>
  <header>
    <h1>FactorIA <span style="color:#60a5fa">Live</span></h1>
    <div class="live"><i id="socket-dot" class="dot"></i><span id="socket-label">Connecting</span></div>
  </header>
  <main>
    <section class="card status-grid" aria-label="Current status">
      <div class="status"><span>Factorio</span><strong id="factorio-state">Disconnected</strong></div>
      <div class="status"><span>Agent</span><strong id="agent-state">Idle</strong></div>
      <div class="status"><span>Control</span><strong id="control-state">Waiting for server</strong></div>
    </section>

    <section class="card">
      <h2>Send instruction</h2>
      <label for="instruction">Objective</label>
      <textarea id="instruction" maxlength="8192" placeholder="Tell the agent what to do..."></textarea>
      <div class="options">
        <div>
          <label for="rounds">Maximum AI rounds</label>
          <input id="rounds" type="number" min="1" max="999" value="12" inputmode="numeric">
        </div>
        <label class="toggle"><input id="non-stop" type="checkbox"> Run until stopped</label>
      </div>
      <div class="actions">
        <button id="run" class="primary">Run objective</button>
        <button id="stop" class="danger">Stop</button>
      </div>
      <div class="connection-actions">
        <button id="connect" class="secondary">Connect Factorio</button>
        <button id="disconnect" class="secondary">Disconnect</button>
      </div>
      <p id="feedback" class="feedback" role="status"></p>
    </section>

    <section class="card">
      <div class="feed-head"><h2>Model decisions</h2><span id="decision-count">0 received</span></div>
      <div id="decisions" aria-live="polite"><div class="empty">Decisions will appear here in real time.</div></div>
    </section>

    <section class="card">
      <div class="feed-head"><h2>AI player inventory</h2><span id="inventory-status">Not connected</span></div>
      <div id="inventory" class="inventory-grid" aria-live="polite"></div>
    </section>
  </main>
  <script>
    const byId = id => document.getElementById(id);
    const ui = {
      socketDot: byId('socket-dot'), socketLabel: byId('socket-label'), factorio: byId('factorio-state'),
      agent: byId('agent-state'), control: byId('control-state'), instruction: byId('instruction'),
      rounds: byId('rounds'), nonStop: byId('non-stop'), run: byId('run'), stop: byId('stop'),
      connect: byId('connect'), disconnect: byId('disconnect'), feedback: byId('feedback'),
      decisions: byId('decisions'), count: byId('decision-count'), inventory: byId('inventory'),
      inventoryStatus: byId('inventory-status')
    };
    let state = null;
    let socket;
    let reconnectTimer;
    let heartbeatTimer;
    let instructionDirty = false;
    const rendered = new Set();

    function setSocketStatus(online) {
      ui.socketDot.classList.toggle('online', online);
      ui.socketLabel.textContent = online ? 'Live' : 'Reconnecting';
      if (!state) ui.control.textContent = online ? 'Loading' : 'Offline';
    }

    function applyState(next) {
      state = next;
      ui.factorio.textContent = next.connected ? 'Connected' : 'Disconnected';
      ui.factorio.className = next.connected ? 'ok' : 'warn';
      ui.agent.textContent = next.agent_status;
      ui.agent.className = next.agent_running ? 'ok' : '';
      ui.control.textContent = next.busy ? 'Busy' : 'Ready';
      if (!instructionDirty && document.activeElement !== ui.instruction) ui.instruction.value = next.objective || '';
      if (document.activeElement !== ui.rounds) ui.rounds.value = next.maximum_rounds;
      ui.nonStop.checked = next.non_stop;
      ui.rounds.disabled = next.non_stop || next.busy;
      ui.nonStop.disabled = next.busy;
      ui.instruction.disabled = next.busy;
      ui.run.disabled = !next.connected || next.busy;
      ui.stop.disabled = !next.agent_running;
      ui.connect.disabled = next.connected || next.busy;
      ui.disconnect.disabled = !next.connected || next.busy;
      renderInventory(next.inventory || {});
    }

    function renderInventory(inventory) {
      const items = Array.isArray(inventory.items) ? inventory.items : [];
      ui.inventoryStatus.textContent = inventory.status || 'Unavailable';
      ui.inventory.replaceChildren();
      if (!inventory.available) {
        const empty = document.createElement('div');
        empty.className = 'empty';
        empty.textContent = inventory.status || 'Inventory unavailable';
        ui.inventory.append(empty);
        return;
      }
      for (const title of ['Item', 'Count', 'Quality']) {
        const cell = document.createElement('div');
        cell.className = title === 'Count' ? 'head count' : 'head';
        cell.textContent = title;
        ui.inventory.append(cell);
      }
      if (!items.length) {
        const empty = document.createElement('div');
        empty.className = 'empty';
        empty.textContent = 'The AI player inventory is empty.';
        ui.inventory.append(empty);
        return;
      }
      for (const item of items) {
        const name = document.createElement('div');
        name.textContent = item.name || 'Unknown item';
        const count = document.createElement('div');
        count.className = 'count';
        count.textContent = Number.isFinite(item.count) ? item.count.toLocaleString() : '0';
        const quality = document.createElement('div');
        quality.textContent = item.quality || 'normal';
        ui.inventory.append(name, count, quality);
      }
    }

    function appendDecision(item) {
      if (rendered.has(item.sequence)) return;
      rendered.add(item.sequence);
      const empty = ui.decisions.querySelector('.empty');
      if (empty) empty.remove();
      const nearBottom = ui.decisions.scrollHeight - ui.decisions.scrollTop - ui.decisions.clientHeight < 80;
      const row = document.createElement('article');
      row.className = 'decision';
      const time = document.createElement('time');
      time.dateTime = new Date(item.timestamp_ms).toISOString();
      time.textContent = new Date(item.timestamp_ms).toLocaleTimeString([], {hour: '2-digit', minute: '2-digit', second: '2-digit'});
      const text = document.createElement('div');
      text.textContent = item.text;
      row.append(time, text);
      ui.decisions.append(row);
      ui.count.textContent = `${rendered.size} received`;
      if (nearBottom) ui.decisions.scrollTop = ui.decisions.scrollHeight;
    }

    function connectSocket() {
      clearTimeout(reconnectTimer);
      const protocol = location.protocol === 'https:' ? 'wss:' : 'ws:';
      socket = new WebSocket(`${protocol}//${location.host}/ws`);
      socket.onopen = () => {
        setSocketStatus(true);
        clearInterval(heartbeatTimer);
        heartbeatTimer = setInterval(() => {
          if (socket.readyState === WebSocket.OPEN) socket.send('keepalive');
        }, 2000);
      };
      socket.onmessage = event => {
        const message = JSON.parse(event.data);
        if (message.type === 'snapshot') {
          applyState(message);
          (message.decisions || []).forEach(appendDecision);
        } else if (message.type === 'state') {
          applyState(message);
        } else if (message.type === 'decision') {
          appendDecision(message.decision);
        } else if (message.type === 'server_shutdown') {
          socket.close();
        }
      };
      socket.onclose = () => {
        clearInterval(heartbeatTimer);
        setSocketStatus(false);
        reconnectTimer = setTimeout(connectSocket, 1500);
      };
      socket.onerror = () => socket.close();
    }

    async function command(action, data = {}) {
      ui.feedback.textContent = 'Sending...';
      const response = await fetch('/api/command', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({action, ...data})
      });
      const result = await response.json();
      if (!response.ok) throw new Error(result.error || 'Request failed');
      ui.feedback.textContent = result.message;
    }

    ui.instruction.addEventListener('input', () => { instructionDirty = true; });
    ui.nonStop.addEventListener('change', () => { ui.rounds.disabled = ui.nonStop.checked || (state && state.busy); });
    ui.run.addEventListener('click', async () => {
      try {
        const maximum = Number.parseInt(ui.rounds.value, 10);
        await command('run', {objective: ui.instruction.value, non_stop: ui.nonStop.checked, maximum_rounds: maximum});
        instructionDirty = false;
      } catch (error) { ui.feedback.textContent = error.message; }
    });
    ui.stop.addEventListener('click', () => command('stop').catch(error => { ui.feedback.textContent = error.message; }));
    ui.connect.addEventListener('click', () => command('connect').catch(error => { ui.feedback.textContent = error.message; }));
    ui.disconnect.addEventListener('click', () => command('disconnect').catch(error => { ui.feedback.textContent = error.message; }));
    connectSocket();
  </script>
</body>
</html>)HTML";

struct ApiError final : std::runtime_error
{
    ApiError(int responseStatus, std::string message)
        : std::runtime_error(std::move(message)), status(responseStatus)
    {
    }

    int status;
};

bool IsBlank(const std::string& value)
{
    return value.find_first_not_of(" \t\r\n") == std::string::npos;
}

void SetJsonResponse(httplib::Response& response, int status, const json& value)
{
    response.status = status;
    response.set_header("Cache-Control", "no-store");
    response.set_content(value.dump(), "application/json; charset=utf-8");
}
}

class WebControlServer::Implementation final
{
public:
    explicit Implementation(CommandHandler commandHandler)
        : commandHandler_(std::move(commandHandler))
    {
        server_.set_payload_max_length(MaximumInstructionLength + 1024);
        server_.set_read_timeout(10, 0);
        server_.set_write_timeout(10, 0);
        server_.set_websocket_ping_interval(0);

        server_.Get("/", [](const httplib::Request&, httplib::Response& response) {
            response.set_header("Cache-Control", "no-store");
            response.set_header("Content-Security-Policy",
                "default-src 'self'; connect-src 'self' ws: wss:; img-src 'self'; "
                "style-src 'unsafe-inline'; script-src 'unsafe-inline'; base-uri 'none'; frame-ancestors 'none'");
            response.set_header("Referrer-Policy", "no-referrer");
            response.set_header("X-Content-Type-Options", "nosniff");
            response.set_content(EmbeddedPage, "text/html; charset=utf-8");
        });
        server_.Get("/api/state", [this](const httplib::Request&, httplib::Response& response) {
            SetJsonResponse(response, 200, StateJson("snapshot", true));
        });
        server_.Post("/api/command", [this](const httplib::Request& request, httplib::Response& response) {
            HandleCommand(request, response);
        });
        server_.WebSocket("/ws", [this](const httplib::Request&, httplib::ws::WebSocket& socket) {
            HandleWebSocket(socket);
        });
    }

    ~Implementation()
    {
        Stop();
    }

    bool Start(const std::string& host, std::uint16_t port)
    {
        if (listener_.joinable())
            return true;
        if (!server_.bind_to_port(host, port))
            return false;

        running_.store(true);
        broadcaster_ = std::jthread([this](std::stop_token stopToken) {
            RunBroadcasts(stopToken);
        });
        listener_ = std::jthread([this] {
            server_.listen_after_bind();
            running_.store(false);
        });
        server_.wait_until_ready();
        if (server_.is_running())
            return true;

        running_.store(false);
        broadcaster_.request_stop();
        broadcastChanged_.notify_all();
        broadcaster_.join();
        listener_.join();
        return false;
    }

    void Stop()
    {
        if (!listener_.joinable())
            return;

        running_.store(false);
        server_.stop();
        QueueBroadcast(json{{"type", "server_shutdown"}}.dump());
        broadcaster_.request_stop();
        broadcastChanged_.notify_all();
        broadcaster_.join();
        listener_.join();
    }

    void PublishState(WebControlState state)
    {
        json payload;
        {
            std::scoped_lock lock(stateMutex_);
            state_ = std::move(state);
            payload = StateJsonLocked("state", false);
        }
        QueueBroadcast(payload.dump());
    }

    void PublishDecision(std::string text)
    {
        json decision;
        {
            std::scoped_lock lock(stateMutex_);
            decision = {
                {"sequence", ++decisionSequence_},
                {"timestamp_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count()},
                {"text", std::move(text)},
            };
            decisions_.push_back(decision);
            if (decisions_.size() > MaximumDecisionHistory)
                decisions_.pop_front();
        }
        QueueBroadcast(json{{"type", "decision"}, {"decision", std::move(decision)}}.dump());
    }

private:
    json StateJson(const char* type, bool includeDecisions) const
    {
        std::scoped_lock lock(stateMutex_);
        return StateJsonLocked(type, includeDecisions);
    }

    json StateJsonLocked(const char* type, bool includeDecisions) const
    {
        json inventory = json::array();
        for (const auto& item : state_.inventory)
        {
            inventory.push_back({
                {"name", item.name},
                {"count", item.count},
                {"quality", item.quality},
            });
        }
        json result{
            {"type", type},
            {"connected", state_.connected},
            {"busy", state_.busy},
            {"agent_running", state_.agentRunning},
            {"agent_status", state_.agentStatus},
            {"objective", state_.objective},
            {"maximum_rounds", state_.maximumRounds},
            {"non_stop", state_.nonStop},
            {"inventory", {
                {"available", state_.inventoryAvailable},
                {"status", state_.inventoryStatus},
                {"items", std::move(inventory)},
            }},
        };
        if (includeDecisions)
            result["decisions"] = decisions_;
        return result;
    }

    void HandleCommand(const httplib::Request& request, httplib::Response& response)
    {
        try
        {
            const auto body = json::parse(request.body);
            if (!body.is_object() || !body.contains("action") || !body.at("action").is_string())
                throw ApiError(400, "A string action is required");

            WebControlCommand command;
            const auto action = body.at("action").get<std::string>();
            if (action == "run")
            {
                command.action = WebControlCommand::Action::Run;
                if (!body.contains("objective") || !body.at("objective").is_string())
                    throw ApiError(400, "An objective is required");
                command.objective = body.at("objective").get<std::string>();
                if (command.objective.size() > MaximumInstructionLength)
                    throw ApiError(400, "The objective is too long");
                if (IsBlank(command.objective))
                    throw ApiError(400, "The objective cannot be empty");

                const auto nonStop = body.value("non_stop", false);
                if (!nonStop)
                {
                    if (!body.contains("maximum_rounds") || !body.at("maximum_rounds").is_number_integer())
                        throw ApiError(400, "Maximum rounds must be an integer");
                    const auto maximumRounds = body.at("maximum_rounds").get<int>();
                    if (maximumRounds < AgentController::MinimumRounds ||
                        maximumRounds > AgentController::MaximumRounds)
                    {
                        throw ApiError(400, "Maximum rounds must be between " +
                            std::to_string(AgentController::MinimumRounds) + " and " +
                            std::to_string(AgentController::MaximumRounds));
                    }
                    command.maximumRounds = maximumRounds;
                }
            }
            else if (action == "stop")
                command.action = WebControlCommand::Action::Stop;
            else if (action == "connect")
                command.action = WebControlCommand::Action::Connect;
            else if (action == "disconnect")
                command.action = WebControlCommand::Action::Disconnect;
            else
                throw ApiError(400, "Unknown action");

            const auto stateUpdate = AcceptCommand(command);
            QueueBroadcast(stateUpdate.dump());
            commandHandler_(std::move(command));
            SetJsonResponse(response, 202, {
                {"accepted", true},
                {"message", action == "run" ? "Instruction sent" : "Action sent"},
            });
        }
        catch (const ApiError& error)
        {
            SetJsonResponse(response, error.status, {{"error", error.what()}});
        }
        catch (const json::exception& error)
        {
            SetJsonResponse(response, 400, {{"error", std::string("Invalid JSON: ") + error.what()}});
        }
        catch (const std::exception& error)
        {
            SetJsonResponse(response, 500, {{"error", error.what()}});
        }
    }

    json AcceptCommand(const WebControlCommand& command)
    {
        std::scoped_lock lock(stateMutex_);
        switch (command.action)
        {
        case WebControlCommand::Action::Connect:
            if (state_.busy)
                throw ApiError(409, "Another operation is already running");
            if (state_.connected)
                throw ApiError(409, "Factorio is already connected");
            state_.busy = true;
            break;
        case WebControlCommand::Action::Disconnect:
            if (state_.busy)
                throw ApiError(409, "Stop the active operation before disconnecting");
            if (!state_.connected)
                throw ApiError(409, "Factorio is not connected");
            state_.busy = true;
            break;
        case WebControlCommand::Action::Run:
            if (!state_.connected)
                throw ApiError(409, "Connect to Factorio before running an objective");
            if (state_.busy)
                throw ApiError(409, "Another operation is already running");
            state_.busy = true;
            state_.agentRunning = true;
            state_.agentStatus = "Starting...";
            state_.objective = command.objective;
            state_.nonStop = !command.maximumRounds.has_value();
            if (command.maximumRounds)
                state_.maximumRounds = *command.maximumRounds;
            break;
        case WebControlCommand::Action::Stop:
            if (!state_.agentRunning)
                throw ApiError(409, "The agent is not running");
            state_.agentStatus = "Stopping...";
            break;
        }
        return StateJsonLocked("state", false);
    }

    void HandleWebSocket(httplib::ws::WebSocket& socket)
    {
        {
            std::scoped_lock lock(socketMutex_);
            // Serialize the first snapshot with broadcasts so newer events always follow it.
            if (!socket.send(StateJson("snapshot", true).dump()))
                return;
            sockets_.insert(&socket);
        }

        while (running_.load())
        {
            std::string ignored;
            if (socket.read(ignored) == httplib::ws::ReadResult::Fail)
                break;
        }

        std::scoped_lock lock(socketMutex_);
        sockets_.erase(&socket);
    }

    void QueueBroadcast(std::string payload)
    {
        {
            std::scoped_lock lock(broadcastMutex_);
            if (broadcastQueue_.size() == MaximumBroadcastQueue)
                broadcastQueue_.pop_front();
            broadcastQueue_.push_back(std::move(payload));
        }
        broadcastChanged_.notify_one();
    }

    void RunBroadcasts(std::stop_token stopToken)
    {
        while (true)
        {
            std::string payload;
            {
                std::unique_lock lock(broadcastMutex_);
                broadcastChanged_.wait(lock, stopToken, [this] { return !broadcastQueue_.empty(); });
                if (broadcastQueue_.empty())
                    return;
                payload = std::move(broadcastQueue_.front());
                broadcastQueue_.pop_front();
            }
            Broadcast(payload);
        }
    }

    void Broadcast(const std::string& payload)
    {
        std::scoped_lock lock(socketMutex_);
        for (auto iterator = sockets_.begin(); iterator != sockets_.end();)
        {
            if ((*iterator)->send(payload))
                ++iterator;
            else
                iterator = sockets_.erase(iterator);
        }
    }

    CommandHandler commandHandler_;
    httplib::Server server_;
    std::jthread listener_;
    std::jthread broadcaster_;
    std::atomic_bool running_{};

    std::mutex broadcastMutex_;
    std::condition_variable_any broadcastChanged_;
    std::deque<std::string> broadcastQueue_;

    mutable std::mutex stateMutex_;
    WebControlState state_;
    std::deque<json> decisions_;
    std::uint64_t decisionSequence_{};

    std::mutex socketMutex_;
    std::unordered_set<httplib::ws::WebSocket*> sockets_;
};

WebControlServer::WebControlServer(CommandHandler commandHandler)
    : implementation_(std::make_unique<Implementation>(std::move(commandHandler)))
{
}

WebControlServer::~WebControlServer() = default;

bool WebControlServer::Start(const std::string& host, std::uint16_t port)
{
    return implementation_->Start(host, port);
}

void WebControlServer::Stop()
{
    implementation_->Stop();
}

void WebControlServer::PublishState(WebControlState state)
{
    implementation_->PublishState(std::move(state));
}

void WebControlServer::PublishDecision(std::string decision)
{
    implementation_->PublishDecision(std::move(decision));
}
}
