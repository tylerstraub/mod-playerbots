/*
 * sbywow Agent Bridge — top-level HTTP+SSE server.
 *
 * Singleton owning the cpp-httplib server, the per-bot session map, and
 * lifecycle (start on WORLDHOOK_ON_STARTUP, stop on shutdown). Threading:
 * the server runs its own thread, httplib spawns a thread per connection.
 * Cross-thread handoff to the world thread happens via per-session queues
 * drained from PlayerScript::OnPlayerUpdate.
 *
 * Architectural rationale lives in docs/decisions.md ("Agent Bridge
 * architecture"). Read that before changing the surface.
 */

#ifndef _SBYWOW_BRIDGE_SERVER_H
#define _SBYWOW_BRIDGE_SERVER_H

#include "ObjectGuid.h"
#include "deps/json.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

class Player;

namespace httplib { class Server; }

namespace Sbywow::Bridge
{
    class BotSession;

    // Build the structured context snapshot for the agent harness's
    // wake context. Same shape feeds three access patterns:
    //   - SSE event `snapshot.state` (periodic, BridgeHooks)
    //   - sync verb `get_context` (cold start / explicit refresh)
    //   - `inspect` verb (debug, embeds the same block as `context`)
    // World-thread only — reads Player* and engine state directly.
    // Schema: see docs/agent-interface.md "Context snapshot block."
    nlohmann::json BuildContextSnapshot(Player* bot, BotSession& sess);

    struct BridgeConfig
    {
        bool        enable               = true;
        std::string host                 = "127.0.0.1";
        int         port                 = 8889;
        std::string secret;                       // empty = auth disabled
        int         heartbeatTimeoutMs   = 30000;
        // Snapshot cadence: emit one `snapshot.state` event per N
        // OnPlayerUpdate calls. Default 500 ≈ 3-4 events/sec under
        // active autonomic load (we measured ~1750 OnPlayerUpdate
        // calls/sec). Pre-tuning default was 50 which produced 35/sec
        // — order of magnitude more than any agent harness needs.
        // Tune via `Sbywow.Bridge.SnapshotEveryNUpdates`.
        uint32      snapshotEveryNUpdates = 500;
    };

    class BridgeServer
    {
    public:
        static BridgeServer& Instance();

        // Lifecycle. Call from WorldScript::OnStartup / OnShutdown.
        void Start();
        void Stop();
        bool IsRunning() const { return running_.load(); }

        // Session management. Called from PlayerScript hooks on the world
        // thread; safe to call from the world thread only. AttachBot
        // returns true if a new session was created (false if the bot was
        // already attached) — callers use this to fire one-shot lifecycle
        // events.
        bool AttachBot(Player* bot);
        void DetachBot(ObjectGuid guid);

        // World-thread tick: drain inbound commands for every attached bot,
        // refresh heartbeat-driven AFK state. Called from
        // PlayerScript::OnPlayerUpdate.
        void TickBot(Player* bot);

        // Lookup. Returns nullptr if the bot is not attached. Safe under
        // sessionsMutex_; callers should hold a strong shared_ptr while in
        // use to keep the session alive across detach.
        std::shared_ptr<BotSession> GetSession(ObjectGuid guid);

        BridgeConfig const& Config() const { return config_; }

        // Mint the next intent_id. Monotonic across the bridge process
        // lifetime; serialized via std::atomic. We expose this as
        // uint64 internally; the wire format wraps in a string so
        // JS-land consumers don't truncate at 2^53. See
        // decisions.md "Async intent contract: four design questions
        // resolved" (2026-05-02) for the format choice.
        uint64_t MintIntentId() { return nextIntentId_.fetch_add(1); }

        // Centralized agent-mode toggle, called by both the
        // `set_agent_mode` bridge verb (source="agent") and the
        // `.merc agent` chat command (source="master"). Updates
        // BotSession::SetAgentMode, side-effect-syncs the bot's WoW
        // AFK marker + autoReplyMsg (AFK is on whenever
        // agent_mode==false to give other players a visible cue),
        // and emits a lifecycle SSE event so attached harnesses see
        // the transition. Returns the *previous* mode so callers
        // can render no-change feedback ("already in agent mode").
        //
        // Queue and wait state on the agent engine are deliberately
        // NOT touched here — preservation across mode flips is the
        // central design property. See decisions.md "Agent mode is
        // an explicit opt-in."
        //
        // World-thread only (called from TickBot dispatch and from
        // the chat command, both world-thread).
        bool ApplyAgentModeToggle(Player* bot, bool desired, std::string const& source);

    private:
        BridgeServer() = default;
        ~BridgeServer();
        BridgeServer(BridgeServer const&) = delete;
        BridgeServer& operator=(BridgeServer const&) = delete;

        void LoadConfig();
        void RegisterRoutes();
        bool CheckAuth(std::string const& authHeader) const;

        BridgeConfig                                         config_;
        std::unique_ptr<httplib::Server>                     server_;
        std::thread                                          serverThread_;
        std::atomic<bool>                                    running_{false};

        std::mutex                                           sessionsMutex_;
        std::unordered_map<uint64, std::shared_ptr<BotSession>> sessions_;

        // Intent ID source. Starts at 1 — id=0 is reserved as
        // "unset" so PendingIntent default-construction is
        // distinguishable from a real id when debugging.
        std::atomic<uint64_t>                                nextIntentId_{1};
    };
}

#endif
