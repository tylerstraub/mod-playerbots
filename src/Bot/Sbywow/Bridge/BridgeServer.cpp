#include "BridgeServer.h"

#include "BotSession.h"

#include "AiObjectContext.h"
#include "Config.h"
#include "Engine.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Playerbots.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "Value.h"

// Vendored single-header deps. Kept local to the bridge so we don't take
// on a cross-module dependency on mod-playerbots-characters' deps tree.
#include "deps/httplib.h"
#include "deps/json.hpp"

#include <chrono>
#include <functional>
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
        config_.snapshotEveryNUpdates =
            static_cast<uint32>(sConfigMgr->GetOption<int32>("Sbywow.Bridge.SnapshotEveryNUpdates", 500));
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

        auto session = std::make_shared<BotSession>(bot->GetGUID());
        session->SetName(bot->GetName());
        sessions_[key] = session;
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

        // Resolve string state name to BotState, or -1 for "all".
        // Returns -2 on invalid name. Default mapping if absent: non-combat.
        int ParseBotState(std::string const& s)
        {
            if (s.empty() || s == "non-combat" || s == "noncombat") return BOT_STATE_NON_COMBAT;
            if (s == "combat")                                       return BOT_STATE_COMBAT;
            if (s == "dead")                                         return BOT_STATE_DEAD;
            if (s == "all")                                          return -1;
            return -2;
        }

        char const* BotStateName(BotState s)
        {
            switch (s)
            {
                case BOT_STATE_COMBAT:     return "combat";
                case BOT_STATE_NON_COMBAT: return "non-combat";
                case BOT_STATE_DEAD:       return "dead";
                default:                   return "?";
            }
        }

        // For verbs that operate on engines: invoke `fn` on each engine
        // matching `state` (-1 = all). Returns the list of states it ran
        // against for diagnostics.
        json ForEachEngine(PlayerbotAI* ai, int state, std::function<void(Engine*, BotState)> const& fn)
        {
            json states = json::array();
            for (uint8 i = 0; i < BOT_STATE_MAX; ++i)
            {
                if (state >= 0 && i != static_cast<uint8>(state)) continue;
                if (Engine* e = ai->GetEngine(static_cast<BotState>(i)))
                {
                    fn(e, static_cast<BotState>(i));
                    states.push_back(BotStateName(static_cast<BotState>(i)));
                }
            }
            return states;
        }

        // Translate a verb JSON into a result JSON. Runs on the world
        // thread, so any Playerbots API is fair game.
        //
        // Defense: top-level try/catch wraps every verb. A SIGSEGV from
        // a Playerbots impl will still crash the world (incident
        // 2026-05-02), but anything that throws a std::exception turns
        // into a clean 503 instead of bringing down the realm. The
        // narrower per-verb wrappers (Save/Load) cover the highest-risk
        // sites first; this is belt-and-suspenders for the rest.
        std::string DispatchCommandInner(Player* bot, BotSession& sess, std::string const& cmdJson);

        std::string DispatchCommand(Player* bot, BotSession& sess, std::string const& cmdJson)
        {
            try { return DispatchCommandInner(bot, sess, cmdJson); }
            catch (std::exception const& e)
            {
                json err = { {"ok", false}, {"error", std::string("dispatch threw: ") + e.what()} };
                return err.dump();
            }
        }

        std::string DispatchCommandInner(Player* bot, BotSession& sess, std::string const& cmdJson)
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

            // ---- Strategy management (the nudge layer) ----------------

            if (verb == "add_strategy" || verb == "remove_strategy" || verb == "change_strategies")
            {
                PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
                if (!ai)
                {
                    json err = { {"ok", false}, {"error", "no PlayerbotAI for this bot"} };
                    return err.dump();
                }

                std::string stateStr = req.value("state", "non-combat");
                int state = ParseBotState(stateStr);
                if (state == -2)
                {
                    json err = { {"ok", false}, {"error", "bad state: " + stateStr +
                                  " (combat|non-combat|dead|all)"} };
                    return err.dump();
                }

                json applied;
                if (verb == "add_strategy")
                {
                    std::string name = req.value("name", "");
                    if (name.empty())
                    {
                        json err = { {"ok", false}, {"error", "add_strategy requires name"} };
                        return err.dump();
                    }
                    applied = ForEachEngine(ai, state, [&](Engine* e, BotState) { e->addStrategy(name); });
                }
                else if (verb == "remove_strategy")
                {
                    std::string name = req.value("name", "");
                    if (name.empty())
                    {
                        json err = { {"ok", false}, {"error", "remove_strategy requires name"} };
                        return err.dump();
                    }
                    applied = ForEachEngine(ai, state, [&](Engine* e, BotState) { e->removeStrategy(name); });
                }
                else // change_strategies — comma-syntax: "+kite,-aggressive,~focus"
                {
                    std::string ops = req.value("ops", "");
                    if (ops.empty())
                    {
                        json err = { {"ok", false}, {"error", "change_strategies requires ops"} };
                        return err.dump();
                    }
                    applied = ForEachEngine(ai, state, [&](Engine* e, BotState) { e->ChangeStrategy(ops); });
                }

                json ok = { {"ok", true}, {"verb", verb}, {"applied_to", applied} };
                return ok.dump();
            }

            if (verb == "list_strategies")
            {
                PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
                if (!ai)
                {
                    json err = { {"ok", false}, {"error", "no PlayerbotAI for this bot"} };
                    return err.dump();
                }

                std::string stateStr = req.value("state", "all");
                int state = ParseBotState(stateStr);
                if (state == -2)
                {
                    json err = { {"ok", false}, {"error", "bad state: " + stateStr} };
                    return err.dump();
                }

                json out = json::object();
                for (uint8 i = 0; i < BOT_STATE_MAX; ++i)
                {
                    if (state >= 0 && i != static_cast<uint8>(state)) continue;
                    BotState s = static_cast<BotState>(i);
                    if (Engine* e = ai->GetEngine(s))
                        out[BotStateName(s)] = e->GetStrategies();
                }
                return json{ {"ok", true}, {"verb", "list_strategies"}, {"strategies", out} }.dump();
            }

            // ---- Value system (set/get on AiObjectContext) ------------

            if (verb == "set_value" || verb == "get_value")
            {
                PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
                if (!ai)
                {
                    json err = { {"ok", false}, {"error", "no PlayerbotAI for this bot"} };
                    return err.dump();
                }
                std::string key = req.value("key", "");
                if (key.empty())
                {
                    json err = { {"ok", false}, {"error", verb + " requires key"} };
                    return err.dump();
                }

                UntypedValue* uv = ai->GetAiObjectContext()->GetUntypedValue(key);
                if (!uv)
                {
                    json err = { {"ok", false}, {"error", "no such value key: " + key} };
                    return err.dump();
                }

                if (verb == "get_value")
                {
                    // CRITICAL: do NOT call Format(). Many overridden
                    // Format() impls deref pointers (UnitCalculatedValue
                    // calls Calculate()→Unit*, then Unit::GetName()) that
                    // can be stale or null when the value is queried
                    // outside its normal computation context. A SIGSEGV
                    // on the world thread crashes the entire realm and
                    // disconnects every player.
                    //
                    // Save() is much safer: default returns the literal
                    // "?", overrides (RtiValue, PositionValue, Stances,
                    // etc.) return stored primitive data without
                    // dereferences. If Save returns "?", the value isn't
                    // introspectable through this verb — that's the
                    // honest signal to the agent. We additionally wrap
                    // in try/catch for std::exception belt-and-suspenders;
                    // it won't catch a segfault but covers anything that
                    // throws a real C++ exception.
                    //
                    // Incident capture: 2026-05-02 in incidents.md.
                    std::string saved;
                    try { saved = uv->Save(); }
                    catch (std::exception const& e) { saved = std::string("<save threw: ") + e.what() + ">"; }
                    return json{
                        {"ok",   true},
                        {"verb", "get_value"},
                        {"key",  key},
                        {"save", saved}
                    }.dump();
                }

                // set_value — UntypedValue::Load takes a string and
                // returns true on success. Same crash-class risk as
                // Format() (see incident 2026-05-02): a Load impl that
                // dereferences stale pointers can segfault the world
                // thread. Wrap in try/catch for std::exception (won't
                // catch SIGSEGV but covers everything that throws).
                std::string val = req.value("value", "");
                bool loaded = false;
                std::string loadErr;
                try { loaded = uv->Load(val); }
                catch (std::exception const& e) { loadErr = std::string("load threw: ") + e.what(); }
                json out = {
                    {"ok",    loaded},
                    {"verb",  "set_value"},
                    {"key",   key},
                    {"value", val}
                };
                if (!loadErr.empty())
                    out["error"] = loadErr;
                else if (!loaded)
                    out["error"] = "value type does not support Load() (read-only via set_value)";
                return out.dump();
            }

            // ---- Chat / say -------------------------------------------

            if (verb == "say")
            {
                PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
                if (!ai)
                {
                    json err = { {"ok", false}, {"error", "no PlayerbotAI for this bot"} };
                    return err.dump();
                }
                std::string text    = req.value("text",    "");
                std::string channel = req.value("channel", "say");
                if (text.empty())
                {
                    json err = { {"ok", false}, {"error", "say requires text"} };
                    return err.dump();
                }

                bool ok = false;
                if      (channel == "say")     ok = ai->Say(text);
                else if (channel == "yell")    ok = ai->Yell(text);
                else if (channel == "party")   ok = ai->SayToParty(text);
                else if (channel == "raid")    ok = ai->SayToRaid(text);
                else if (channel == "guild")   ok = ai->SayToGuild(text);
                else if (channel == "world")   ok = ai->SayToWorld(text);
                else if (channel == "master")
                {
                    // TellMaster requires a bound master; best-effort.
                    ok = ai->TellMaster(text);
                }
                else
                {
                    json err = { {"ok", false}, {"error", "unknown channel: " + channel +
                                  " (say|yell|party|raid|guild|world|master)"} };
                    return err.dump();
                }
                return json{ {"ok", ok}, {"verb", "say"}, {"channel", channel}, {"text", text} }.dump();
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
                        {"name",            sess->Name()},
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
