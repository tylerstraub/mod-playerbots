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

    struct BridgeConfig
    {
        bool        enable             = true;
        std::string host               = "127.0.0.1";
        int         port               = 8889;
        std::string secret;                    // empty = auth disabled
        int         heartbeatTimeoutMs = 30000;
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
    };
}

#endif
