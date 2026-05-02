#include "BridgeServer.h"

#include "BotSession.h"

#include "Config.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Playerbots.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

// Vendored single-header deps. Kept local to the bridge so we don't take
// on a cross-module dependency on mod-playerbots-characters' deps tree.
#include "deps/httplib.h"
#include "deps/json.hpp"

#include <chrono>
#include <regex>
#include <sstream>

using json = nlohmann::json;

namespace Sbywow::Bridge
{
    BridgeServer& BridgeServer::Instance()
    {
        static BridgeServer instance;
        return instance;
    }

    BridgeServer::~BridgeServer() { Stop(); }

    // -----------------------------------------------------------------------
    // Config (read once at Start; reload requires restart of the world)
    // -----------------------------------------------------------------------

    void BridgeServer::LoadConfig()
    {
        config_.enable             = sConfigMgr->GetOption<bool>       ("Sbywow.Bridge.Enable",             true);
        // NOTE: Bind to 0.0.0.0 inside the container, not 127.0.0.1.
        // Podman's port-forward enters the container on a non-loopback
        // interface; binding to 127.0.0.1 inside accepts the connection
        // (the listening socket is reachable) but resets the request.
        // Host-side localhost-only is enforced by `-p 127.0.0.1:8889:8889`
        // in bin/ac. PBC's HTTP server uses the same pattern.
        config_.host               = sConfigMgr->GetOption<std::string>("Sbywow.Bridge.Host",               "0.0.0.0");
        config_.port               = sConfigMgr->GetOption<int32>      ("Sbywow.Bridge.Port",               8889);
        config_.secret             = sConfigMgr->GetOption<std::string>("Sbywow.Bridge.Secret",             "");
        config_.heartbeatTimeoutMs = sConfigMgr->GetOption<int32>      ("Sbywow.Bridge.HeartbeatTimeoutMs", 30000);
    }

    bool BridgeServer::CheckAuth(std::string const& authHeader) const
    {
        if (config_.secret.empty())
            return true;                          // auth disabled

        // Expected format: "Bearer <secret>".
        std::string const prefix = "Bearer ";
        if (authHeader.size() <= prefix.size() ||
            authHeader.compare(0, prefix.size(), prefix) != 0)
            return false;

        return authHeader.substr(prefix.size()) == config_.secret;
    }

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    void BridgeServer::Start()
    {
        if (running_.load())
            return;

        LoadConfig();

        if (!config_.enable)
        {
            LOG_INFO("server.loading", "[SbywowBridge] disabled by config");
            return;
        }

        server_ = std::make_unique<httplib::Server>();
        RegisterRoutes();

        std::string host = config_.host;
        int         port = config_.port;

        serverThread_ = std::thread([this, host, port]()
        {
            LOG_INFO("server.loading", "[SbywowBridge] listening on {}:{}", host, port);
            if (!server_->listen(host, port))
                LOG_ERROR("server.loading", "[SbywowBridge] listen failed on {}:{}", host, port);
        });

        running_.store(true);
    }

    void BridgeServer::Stop()
    {
        if (!running_.load())
            return;

        if (server_)
            server_->stop();

        if (serverThread_.joinable())
            serverThread_.join();

        server_.reset();
        running_.store(false);

        std::lock_guard<std::mutex> lock(sessionsMutex_);
        sessions_.clear();
    }

    // -----------------------------------------------------------------------
    // Session management (called on the world thread)
    // -----------------------------------------------------------------------

    bool BridgeServer::AttachBot(Player* bot)
    {
        if (!bot || !running_.load())
            return false;

        std::lock_guard<std::mutex> lock(sessionsMutex_);
        uint64 key = bot->GetGUID().GetRawValue();
        if (sessions_.find(key) != sessions_.end())
            return false;

        sessions_[key] = std::make_shared<BotSession>(bot->GetGUID());
        LOG_INFO("server.loading", "[SbywowBridge] attached bot guid={} name={}",
                 key, bot->GetName());
        return true;
    }

    void BridgeServer::DetachBot(ObjectGuid guid)
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        auto it = sessions_.find(guid.GetRawValue());
        if (it == sessions_.end())
            return;

        // Don't erase under callers; let in-flight handlers finish via the
        // shared_ptr they already hold. Erasing from the map is enough.
        sessions_.erase(it);
        LOG_INFO("server.loading", "[SbywowBridge] detached bot guid={}", guid.GetRawValue());
    }

    std::shared_ptr<BotSession> BridgeServer::GetSession(ObjectGuid guid)
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        auto it = sessions_.find(guid.GetRawValue());
        if (it == sessions_.end())
            return nullptr;
        return it->second;
    }

    // -----------------------------------------------------------------------
    // Command dispatch (runs on the world thread, called from TickBot)
    // -----------------------------------------------------------------------

    namespace
    {
        // Push a control-channel event onto the bot's outbound queue so
        // the agent's SSE stream sees state transitions it didn't
        // request directly (e.g., heartbeat-driven force-release).
        void EmitControlEvent(BotSession& sess, std::string const& kind, std::string const& reason)
        {
            json ev = {
                {"channel",  "control"},
                {"kind",     kind},
                {"bot_guid", sess.Guid().GetRawValue()},
                {"reason",   reason}
            };
            sess.PushOutbound(ev.dump());
        }

        // Translate a verb JSON into a result JSON. Runs on the world
        // thread, so any Playerbots API is fair game.
        std::string DispatchCommand(Player* bot, BotSession& sess, std::string const& cmdJson)
        {
            json req;
            try { req = json::parse(cmdJson); }
            catch (std::exception const& e)
            {
                json err = { {"ok", false}, {"error", std::string("bad json: ") + e.what()} };
                return err.dump();
            }

            std::string verb = req.value("verb", "");

            if (verb == "ping")
            {
                json ok = { {"ok", true} };
                return ok.dump();
            }

            if (verb == "seize")
            {
                bool wasSeized = sess.IsSeized();
                sess.SetSeized(true);
                if (!wasSeized)
                    EmitControlEvent(sess, "seize_acquired", "explicit");
                json ok = { {"ok", true}, {"verb", "seize"}, {"seized", true} };
                return ok.dump();
            }

            if (verb == "release")
            {
                bool wasSeized = sess.IsSeized();
                sess.SetSeized(false);
                if (wasSeized)
                    EmitControlEvent(sess, "seize_released", "explicit");
                json ok = { {"ok", true}, {"verb", "release"}, {"seized", false} };
                return ok.dump();
            }

            if (verb == "do_action")
            {
                std::string name      = req.value("name",      "");
                std::string qualifier = req.value("qualifier", "");
                if (name.empty())
                {
                    json err = { {"ok", false}, {"error", "do_action requires name"} };
                    return err.dump();
                }

                PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
                if (!ai)
                {
                    json err = { {"ok", false}, {"error", "no PlayerbotAI for this bot"} };
                    return err.dump();
                }

                // Tag the call as agent-originated so the listener bypass
                // the seize veto. Scope ensures the flag clears even if
                // DoSpecificAction throws (it shouldn't, but cheap safety).
                Event ev;
                bool result;
                {
                    ScopedAgentAction tag(sess);
                    result = ai->DoSpecificAction(name, ev, /*silent=*/true, qualifier);
                }
                json ok = {
                    {"ok",        result},
                    {"verb",      "do_action"},
                    {"name",      name},
                    {"qualifier", qualifier}
                };
                return ok.dump();
            }

            json err = { {"ok", false}, {"error", "unknown verb: " + verb} };
            return err.dump();
        }
    }

    // -----------------------------------------------------------------------
    // World-thread tick: drain inbound, drive AFK
    // -----------------------------------------------------------------------

    void BridgeServer::TickBot(Player* bot)
    {
        if (!bot || !running_.load())
            return;

        auto session = GetSession(bot->GetGUID());
        if (!session)
            return;

        // Drain inbound commands. Each command's promise is set with a
        // result JSON the httplib handler is blocked on.
        std::shared_ptr<PendingCommand> cmd;
        while (session->PopInbound(cmd))
        {
            std::string out = DispatchCommand(bot, *session, cmd->json);
            try { cmd->result.set_value(std::move(out)); }
            catch (std::future_error const&) { /* receiver gone — ignore */ }
        }

        // AFK degradation. Heartbeat-driven for v1.
        bool stale = session->HeartbeatAgeMs() > config_.heartbeatTimeoutMs;
        bool wasAfk = session->IsAfk();
        if (stale && !wasAfk)
        {
            session->SetAfk(true);
            bot->ToggleAFK();
            bot->autoReplyMsg = "Agent unresponsive — autonomic only";
            // Force-release seize on heartbeat loss so a dead harness
            // doesn't pin the bot in a frozen state.
            if (session->IsSeized())
            {
                session->SetSeized(false);
                EmitControlEvent(*session, "seize_released", "heartbeat_timeout");
            }
        }
        else if (!stale && wasAfk)
        {
            session->SetAfk(false);
            if (bot->isAFK())
                bot->ToggleAFK();
            bot->autoReplyMsg = "";
        }
    }

    // -----------------------------------------------------------------------
    // Routes
    // -----------------------------------------------------------------------

    void BridgeServer::RegisterRoutes()
    {
        // POST /bot/<guid>/cmd
        server_->Post(R"(/bot/(\d+)/cmd)",
            [this](httplib::Request const& req, httplib::Response& res)
        {
            if (!CheckAuth(req.get_header_value("Authorization")))
            {
                res.status = 401;
                res.set_content(R"({"ok":false,"error":"unauthorized"})", "application/json");
                return;
            }

            uint64 rawGuid = 0;
            try { rawGuid = std::stoull(req.matches[1].str()); }
            catch (...) { res.status = 400; res.set_content(R"({"ok":false,"error":"bad guid"})", "application/json"); return; }

            auto session = GetSession(ObjectGuid(rawGuid));
            if (!session)
            {
                res.status = 404;
                res.set_content(R"({"ok":false,"error":"bot not attached"})", "application/json");
                return;
            }

            // Push command, wait for world-thread to dispatch and return.
            // Bound the wait so a hung tick doesn't pin the httplib thread
            // forever. 5s is generous; commands run synchronously and
            // should return within one tick (~50ms).
            auto fut = session->PushInbound(req.body);
            if (fut.wait_for(std::chrono::seconds(5)) == std::future_status::timeout)
            {
                res.status = 504;
                res.set_content(R"({"ok":false,"error":"world-thread timeout"})", "application/json");
                return;
            }

            // wait_for returning ready can also mean broken_promise (the
            // session/PendingCommand was destroyed before set_value ran,
            // e.g. bot logged out mid-flight). get() throws future_error
            // in that case; turn it into a clean 503 instead of letting
            // it bubble up as an httplib 500.
            try
            {
                res.status = 200;
                res.set_content(fut.get(), "application/json");
            }
            catch (std::future_error const&)
            {
                res.status = 503;
                res.set_content(R"({"ok":false,"error":"session went away mid-command"})", "application/json");
            }
        });

        // GET /bot/<guid>/events  (SSE stream)
        server_->Get(R"(/bot/(\d+)/events)",
            [this](httplib::Request const& req, httplib::Response& res)
        {
            if (!CheckAuth(req.get_header_value("Authorization")))
            {
                res.status = 401;
                res.set_content(R"({"ok":false,"error":"unauthorized"})", "application/json");
                return;
            }

            uint64 rawGuid = 0;
            try { rawGuid = std::stoull(req.matches[1].str()); }
            catch (...) { res.status = 400; return; }

            auto session = GetSession(ObjectGuid(rawGuid));
            if (!session)
            {
                res.status = 404;
                res.set_content(R"({"ok":false,"error":"bot not attached"})", "application/json");
                return;
            }

            res.set_header("Cache-Control", "no-cache");
            res.set_header("Connection",    "keep-alive");

            // Keep connection alive in a streaming loop. cpp-httplib calls
            // the provider repeatedly; we wait for events with a timeout
            // and emit SSE keepalive comments otherwise.
            session->SetSseAttached(true);
            res.set_chunked_content_provider("text/event-stream",
                [session](size_t /*offset*/, httplib::DataSink& sink) -> bool
            {
                OutboundEvent ev;
                if (session->WaitOutbound(ev, /*timeoutMs=*/15000))
                {
                    std::string frame = "data: " + ev.json + "\n\n";
                    if (!sink.write(frame.data(), frame.size()))
                        return false;          // client gone
                }
                else
                {
                    // SSE keepalive comment line
                    std::string ka = ": keepalive\n\n";
                    if (!sink.write(ka.data(), ka.size()))
                        return false;
                }
                return true;
            },
                [session](bool /*success*/) { session->SetSseAttached(false); });
        });

        // GET /bots — debug listing
        server_->Get("/bots",
            [this](httplib::Request const& req, httplib::Response& res)
        {
            if (!CheckAuth(req.get_header_value("Authorization")))
            {
                res.status = 401;
                res.set_content(R"({"ok":false,"error":"unauthorized"})", "application/json");
                return;
            }

            json arr = json::array();
            {
                std::lock_guard<std::mutex> lock(sessionsMutex_);
                for (auto const& [key, sess] : sessions_)
                {
                    arr.push_back({
                        {"guid",            key},
                        {"heartbeat_ms",    sess->HeartbeatAgeMs()},
                        {"afk",             sess->IsAfk()},
                        {"sse_attached",    sess->IsSseAttached()},
                        {"seized",          sess->IsSeized()}
                    });
                }
            }
            res.set_content(arr.dump(), "application/json");
        });
    }
}
