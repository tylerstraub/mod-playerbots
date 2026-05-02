#include "BridgeServer.h"

#include "BotSession.h"

#include "../AgentEngine/IsAgentBonded.h"
#include "../AgentEngine/SbywowAgentEngine.h"
#include "../MercenaryMgr.h"

#include "AiObjectContext.h"
#include "Cell.h"
#include "CellImpl.h"
#include "Config.h"
#include "Creature.h"
#include "Engine.h"
#include "GameObject.h"
#include "GossipDef.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Playerbots.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "Value.h"
#include "WorldPacket.h"
#include "WorldSession.h"

// Vendored single-header deps. Kept local to the bridge so we don't take
// on a cross-module dependency on mod-playerbots-characters' deps tree.
#include "deps/httplib.h"
#include "deps/json.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <regex>
#include <sstream>
#include <utility>
#include <vector>

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

        {
            std::lock_guard<std::mutex> lock(sessionsMutex_);
            uint64 key = bot->GetGUID().GetRawValue();
            if (sessions_.find(key) != sessions_.end())
                return false;

            auto session = std::make_shared<BotSession>(bot->GetGUID());
            session->SetName(bot->GetName());
            sessions_[key] = session;
            LOG_INFO("server.loading", "[SbywowBridge] attached bot guid={} name={}",
                     key, bot->GetName());
        }

        // Default state on attach: agent_mode=false. The bot's WoW
        // AFK marker reflects "agent not driving" so other players
        // see [AFK] from the moment the merc spawns until the agent
        // harness opts in via set_agent_mode true. The session flag
        // already defaults to false; just set the visible side-
        // effect (toggle / autoReplyMsg) — there's no transition
        // event since this is the initial state, not a change.
        if (!bot->isAFK())
        {
            bot->ToggleAFK();
            bot->autoReplyMsg = "Agent not actively driving — running default merc behavior";
        }
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

    bool BridgeServer::ApplyAgentModeToggle(Player* bot, bool desired, std::string const& source)
    {
        if (!bot)
            return false;
        auto session = GetSession(bot->GetGUID());
        if (!session)
            return false;

        bool prev = session->IsAgentMode();
        if (prev == desired)
            return prev;  // no-change short-circuit

        session->SetAgentMode(desired);

        // Mirror to the WoW AFK marker for player-visibility. Other
        // players see [AFK] on the bot when agent_mode is false (the
        // default state on attach), so they know the agent isn't
        // driving even if the bot is otherwise active under default
        // behavior. autoReplyMsg conveys the custom semantic to
        // anyone whispering. AFK on agent-bonded bots is owned
        // exclusively by us — see PlayerbotAI.cpp / AcceptInvitation
        // gates against IsAgentBonded.
        if (!desired && !bot->isAFK())
        {
            bot->ToggleAFK();
            bot->autoReplyMsg = "Agent not actively driving — running default merc behavior";
        }
        else if (desired && bot->isAFK())
        {
            bot->ToggleAFK();
            bot->autoReplyMsg = "";
        }

        // Lifecycle SSE: harness consumers can correlate transitions
        // and surface them to the master/operator.
        json ev = {
            {"channel",    "lifecycle"},
            {"kind",       desired ? "agent_mode_entered" : "agent_mode_exited"},
            {"bot_guid",   bot->GetGUID().GetRawValue()},
            {"bot_name",   bot->GetName()},
            {"source",     source},
            {"agent_mode", desired}
        };
        session->PushOutbound(ev.dump());

        LOG_INFO("playerbots",
                 "[SbywowBridge] agent_mode toggle: bot={} mode={} source={}",
                 bot->GetName().c_str(), desired ? "on" : "off", source.c_str());
        return prev;
    }

    // -----------------------------------------------------------------------
    // Command dispatch (runs on the world thread, called from TickBot)
    // -----------------------------------------------------------------------

    namespace
    {
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


        // ---- Diagnostic / debug instrument ------------------------
        //
        // Surfaces everything we want visible during engine-replacement
        // work: which engine subclass is installed in each state slot,
        // bot session account-id vs cached service-account-id, master
        // pointer state, current engine, last action. Adding this as a
        // first-class verb (not a one-off log) so future engine work
        // has a real instrument instead of LOG_INFO archaeology.
        std::string DoInspect(Player* bot, BotSession& sess)
        {
            PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot);

            json botInfo = {
                {"guid",               bot->GetGUID().GetRawValue()},
                {"name",               bot->GetName()},
                {"session_account_id", bot->GetSession() ? bot->GetSession()->GetAccountId() : 0},
                {"level",              bot->GetLevel()},
                {"class_id",           static_cast<int>(bot->getClass())},
                {"race_id",            static_cast<int>(bot->getRace())},
                {"map",                bot->GetMapId()},
                {"zone",               bot->GetZoneId()},
                {"area",               bot->GetAreaId()},
                {"pos",                {bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()}},
                {"in_combat",          bot->IsInCombat()},
                {"alive",              bot->IsAlive()},
                {"is_afk",             bot->isAFK()}
            };

            // Engine slot inspection — dynamic_cast detects whether
            // our SbywowAgentEngine is installed. Adding new engine
            // subclasses in the future: extend this dispatch.
            auto engineClass = [](Engine* e) -> std::string
            {
                if (!e) return "<null>";
                if (dynamic_cast<Sbywow::SbywowAgentEngine*>(e)) return "SbywowAgentEngine";
                return "Engine";
            };

            json engineInfo;
            json agentEngineState;  // populated only when non_combat is SbywowAgentEngine
            if (ai)
            {
                Engine* combat    = ai->GetEngine(BOT_STATE_COMBAT);
                Engine* nonCombat = ai->GetEngine(BOT_STATE_NON_COMBAT);
                Engine* dead      = ai->GetEngine(BOT_STATE_DEAD);

                engineInfo = {
                    {"combat", {
                        {"class",             engineClass(combat)},
                        {"strategies_count",  combat    ? static_cast<int>(combat->GetStrategies().size())    : 0}
                    }},
                    {"non_combat", {
                        {"class",             engineClass(nonCombat)},
                        {"strategies_count",  nonCombat ? static_cast<int>(nonCombat->GetStrategies().size()) : 0}
                    }},
                    {"dead", {
                        {"class",             engineClass(dead)},
                        {"strategies_count",  dead      ? static_cast<int>(dead->GetStrategies().size())      : 0}
                    }}
                };

                // When non_combat is our SbywowAgentEngine, surface its
                // wait-suspension state plus tick counters. Closes the
                // "why isn't my move_to executing?" question without an
                // SSE-scrub round trip. waitingIntentId stringified for
                // wire consistency with the rest of the intent contract.
                if (auto* agentEng = dynamic_cast<Sbywow::SbywowAgentEngine*>(nonCombat))
                {
                    agentEngineState = {
                        {"agent_mode",                       sess.IsAgentMode()},
                        {"default_engine_strategies_count",  static_cast<int>(agentEng->DefaultEngineStrategiesCount())},
                        {"default_engine_ticks_total",       agentEng->DefaultEngineTicksTotal()},
                        {"is_waiting",                       agentEng->IsWaiting()},
                        {"waiting_intent_id",                agentEng->WaitingIntentId() != 0
                                                             ? json(std::to_string(agentEng->WaitingIntentId()))
                                                             : json(nullptr)},
                        {"waiting_intent_verb",              agentEng->WaitingIntentVerb()},
                        {"waiting_remaining_ms",             agentEng->WaitingRemainingMs()},
                        {"ticks_total",                      agentEng->TicksTotal()},
                        {"intents_dispatched_total",         agentEng->IntentsDispatchedTotal()},
                        {"reactives_fired_total",            agentEng->ReactivesFiredTotal()}
                    };
                }

                Player* master = ai->GetMaster();
                botInfo["has_master"]  = (master != nullptr);
                botInfo["master_name"] = master ? master->GetName() : "";
                botInfo["master_guid"] = master ? master->GetGUID().GetRawValue() : 0ULL;
            }

            json sbywowInfo = {
                {"is_agent_bonded",     Sbywow::IsAgentBonded(bot)},
                {"service_account_id",  sMercenaryMgr.GetServiceAccountId()}
            };

            json sessionInfo = {
                {"heartbeat_ms",   sess.HeartbeatAgeMs()},
                {"agent_mode",     sess.IsAgentMode()},
                {"sse_attached",   sess.IsSseAttached()},
                {"intent_count",   static_cast<int>(sess.IntentCount())}
            };

            json out = {
                {"ok",       true},
                {"verb",     "inspect"},
                {"bot",      botInfo},
                {"engines",  engineInfo},
                {"sbywow",   sbywowInfo},
                {"session",  sessionInfo}
            };
            if (!agentEngineState.is_null())
                out["agent_engine"] = std::move(agentEngineState);
            return out.dump();
        }

        // ---- Autonomous-driving primitives ------------------------
        //
        // Three verbs the agent uses to drive a mastered bot when the
        // upstream rpg/grind subsystem is silently no-op'd by the
        // !HasRealPlayerMaster() gates. See decisions.md
        // "Master-bond suppresses roaming" for the architectural why.
        //
        // All five (move/interact/say/do_action/wait) ride existing
        // public Player / WorldSession / MotionMaster surface that
        // AC's own opcode handlers and mod-playerbots' own actions
        // use. The bridge side is a thin queue-pusher; actual
        // execution lives in SbywowAgentEngine on the world thread.

        // Queue an intent for engine execution. Returns the
        // {ok, intent_id, verb} ack immediately — the inbound
        // PendingCommand's promise is set with this string by
        // TickBot, unblocking the HTTP response within microseconds.
        // Engine completion is delivered separately via SSE
        // (intent_completed / intent_failed events keyed by
        // intent_id). See decisions.md "Async-via-events"
        // (2026-05-02) for the protocol shape.
        std::string Defer(Player* bot,
                          BotSession& sess,
                          std::shared_ptr<PendingCommand>& /*cmd*/,
                          std::string const& verb,
                          Sbywow::Intent intent)
        {
            uint64_t id = BridgeServer::Instance().MintIntentId();

            auto pending = std::make_shared<PendingIntent>();
            pending->intent   = std::move(intent);
            pending->intentId = id;
            pending->verb     = verb;

            // Emit intent_queued before pushing to the engine queue,
            // so a fast engine tick can't slip an intent_started in
            // front of us.
            json ev = {
                {"channel",   "intent"},
                {"kind",      "intent_queued"},
                {"bot_guid",  bot->GetGUID().GetRawValue()},
                {"bot_name",  bot->GetName()},
                {"intent_id", std::to_string(id)},
                {"verb",      verb}
            };
            sess.PushOutbound(ev.dump());

            sess.PushIntent(std::move(pending));

            return json{
                {"ok",        true},
                {"verb",      verb},
                {"intent_id", std::to_string(id)},
                {"queued",    true}
            }.dump();
        }

        std::string QueueMoveIntent(Player* bot, BotSession& sess,
                                    std::shared_ptr<PendingCommand>& cmd,
                                    json const& req)
        {
            if (!req.contains("x") || !req.contains("y") || !req.contains("z") ||
                !req["x"].is_number() || !req["y"].is_number() || !req["z"].is_number())
                return json{{"ok", false}, {"error", "move_to requires numeric x, y, z"}}.dump();

            Sbywow::Intent i;
            i.kind = Sbywow::IntentKind::Move;
            i.x = req["x"].get<float>();
            i.y = req["y"].get<float>();
            i.z = req["z"].get<float>();
            if (req.contains("map") && !req["map"].is_null() && req["map"].is_number_unsigned())
                i.map = req["map"].get<uint32_t>();
            return Defer(bot, sess, cmd, "move_to", std::move(i));
        }

        std::string QueueInteractIntent(Player* bot, BotSession& sess,
                                        std::shared_ptr<PendingCommand>& cmd,
                                        json const& req)
        {
            if (!req.contains("guid") || !req["guid"].is_number())
                return json{{"ok", false}, {"error", "interact_with requires numeric guid"}}.dump();

            Sbywow::Intent i;
            i.kind = Sbywow::IntentKind::Interact;
            i.guid = req["guid"].get<uint64_t>();
            return Defer(bot, sess, cmd, "interact_with", std::move(i));
        }

        std::string QueueSayIntent(Player* bot, BotSession& sess,
                                   std::shared_ptr<PendingCommand>& cmd,
                                   json const& req)
        {
            std::string text = req.value("text", "");
            if (text.empty())
                return json{{"ok", false}, {"error", "say requires text"}}.dump();

            Sbywow::Intent i;
            i.kind    = Sbywow::IntentKind::Say;
            i.text    = std::move(text);
            i.channel = req.value("channel", "say");
            return Defer(bot, sess, cmd, "say", std::move(i));
        }

        std::string QueueDoActionIntent(Player* bot, BotSession& sess,
                                        std::shared_ptr<PendingCommand>& cmd,
                                        json const& req)
        {
            std::string name = req.value("name", "");
            if (name.empty())
                return json{{"ok", false}, {"error", "do_action requires name"}}.dump();

            Sbywow::Intent i;
            i.kind            = Sbywow::IntentKind::DoAction;
            i.actionName      = std::move(name);
            i.actionQualifier = req.value("qualifier", "");
            return Defer(bot, sess, cmd, "do_action", std::move(i));
        }

        std::string QueueWaitIntent(Player* bot, BotSession& sess,
                                    std::shared_ptr<PendingCommand>& cmd,
                                    json const& req)
        {
            if (!req.contains("ms") || !req["ms"].is_number_unsigned())
                return json{{"ok", false}, {"error", "wait requires unsigned ms"}}.dump();

            uint32_t ms = req["ms"].get<uint32_t>();
            // Cap at 60s to keep agent harness round-trips bounded;
            // longer pauses can be chained by the harness.
            if (ms > 60000) ms = 60000;

            Sbywow::Intent i;
            i.kind   = Sbywow::IntentKind::Wait;
            i.waitMs = ms;
            return Defer(bot, sess, cmd, "wait", std::move(i));
        }


        // Decode a creature's npc_flags field into a small array of
        // human-readable role tags. The agent uses these to pick which
        // follow-up verb to run (interact_with on a vendor, do_action
        // "buy" on a vendor, etc.) without learning AC's bitfield.
        json DecodeCreatureFlags(Creature const* c)
        {
            json flags = json::array();
            if (c->IsGossip())          flags.push_back("gossip");
            if (c->IsQuestGiver())      flags.push_back("questgiver");
            if (c->IsTrainer())         flags.push_back("trainer");
            if (c->IsVendor())          flags.push_back("vendor");
            if (c->IsArmorer())         flags.push_back("repair");
            if (c->IsTaxi())            flags.push_back("flightmaster");
            if (c->IsBanker())          flags.push_back("banker");
            if (c->IsInnkeeper())       flags.push_back("innkeeper");
            if (c->IsAuctioner())       flags.push_back("auctioneer");
            if (c->IsBattleMaster())    flags.push_back("battlemaster");
            if (c->IsTabardDesigner())  flags.push_back("tabard");
            if (c->IsSpiritHealer())    flags.push_back("spirithealer");
            if (c->IsSpiritGuide())     flags.push_back("spiritguide");
            if (c->HasNpcFlag(UNIT_NPC_FLAG_STABLEMASTER)) flags.push_back("stablemaster");
            if (c->HasNpcFlag(UNIT_NPC_FLAG_MAILBOX))      flags.push_back("mailbox");
            if (c->HasNpcFlag(UNIT_NPC_FLAG_GUILD_BANKER)) flags.push_back("guild_banker");
            return flags;
        }

        std::string DoFindNearby(Player* bot, json const& req)
        {
            // Range default 30 yards, capped at 200 to keep the grid
            // visit bounded. For comparison the player visibility range
            // is ~100 yards on most maps; 200 is generous.
            float range = req.contains("range") && req["range"].is_number()
                          ? req["range"].get<float>() : 30.0f;
            if (range < 0.f)   range = 0.f;
            if (range > 200.f) range = 200.f;

            bool wantCreatures = true, wantPlayers = true, wantGameObjects = true;
            if (req.contains("kinds") && req["kinds"].is_array())
            {
                wantCreatures = wantPlayers = wantGameObjects = false;
                for (auto const& kind : req["kinds"])
                {
                    if (!kind.is_string()) continue;
                    std::string s = kind.get<std::string>();
                    if      (s == "creature")   wantCreatures = true;
                    else if (s == "player")     wantPlayers = true;
                    else if (s == "gameobject") wantGameObjects = true;
                }
            }

            int limit = req.contains("limit") && req["limit"].is_number()
                        ? req["limit"].get<int>() : 50;
            if (limit < 1)   limit = 1;
            if (limit > 200) limit = 200;

            bool aliveOnly = req.contains("alive_only") && req["alive_only"].is_boolean()
                             ? req["alive_only"].get<bool>() : true;

            PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
            Player* master = ai ? ai->GetMaster() : nullptr;

            // Single grid visit collecting WorldObjects in range; we
            // filter by kind below. AllWorldObjectsInRange honors
            // phase, so cross-phase objects don't leak into perception.
            std::list<WorldObject*> objs;
            Acore::AllWorldObjectsInRange check(bot, range);
            Acore::WorldObjectListSearcher<Acore::AllWorldObjectsInRange> searcher(bot, objs, check);
            Cell::VisitObjects(bot, searcher, range);

            // Pair items with their distance so we can sort by it,
            // then truncate to limit before returning.
            std::vector<std::pair<float, json>> scored;
            scored.reserve(objs.size());

            for (WorldObject* o : objs)
            {
                if (!o || o == bot) continue;

                if (Creature* c = o->ToCreature())
                {
                    if (!wantCreatures) continue;
                    if (aliveOnly && !c->IsAlive()) continue;
                    float d = bot->GetExactDist(c);
                    json item = {
                        {"kind",    "creature"},
                        {"guid",    c->GetGUID().GetRawValue()},
                        {"entry",   c->GetEntry()},
                        {"name",    c->GetName()},
                        {"dist",    d},
                        {"x",       c->GetPositionX()},
                        {"y",       c->GetPositionY()},
                        {"z",       c->GetPositionZ()},
                        {"level",   c->GetLevel()},
                        {"hp_pct",  static_cast<int>(c->GetHealthPct())},
                        {"alive",   c->IsAlive()},
                        {"hostile", c->IsHostileTo(bot)},
                        {"flags",   DecodeCreatureFlags(c)}
                    };
                    scored.emplace_back(d, std::move(item));
                }
                else if (Player* p = o->ToPlayer())
                {
                    if (!wantPlayers) continue;
                    if (aliveOnly && !p->IsAlive()) continue;
                    float d = bot->GetExactDist(p);
                    json item = {
                        {"kind",      "player"},
                        {"guid",      p->GetGUID().GetRawValue()},
                        {"name",      p->GetName()},
                        {"dist",      d},
                        {"x",         p->GetPositionX()},
                        {"y",         p->GetPositionY()},
                        {"z",         p->GetPositionZ()},
                        {"level",     p->GetLevel()},
                        {"hp_pct",    static_cast<int>(p->GetHealthPct())},
                        {"is_master", master == p},
                        {"is_bot",    sPlayerbotsMgr.GetPlayerbotAI(p) != nullptr}
                    };
                    scored.emplace_back(d, std::move(item));
                }
                else if (GameObject* go = o->ToGameObject())
                {
                    if (!wantGameObjects) continue;
                    float d = bot->GetExactDist(go);
                    json item = {
                        {"kind",    "gameobject"},
                        {"guid",    go->GetGUID().GetRawValue()},
                        {"entry",   go->GetEntry()},
                        {"name",    go->GetName()},
                        {"dist",    d},
                        {"x",       go->GetPositionX()},
                        {"y",       go->GetPositionY()},
                        {"z",       go->GetPositionZ()},
                        {"go_type", static_cast<int>(go->GetGoType())}
                    };
                    scored.emplace_back(d, std::move(item));
                }
            }

            std::sort(scored.begin(), scored.end(),
                      [](auto const& a, auto const& b) { return a.first < b.first; });

            json arr = json::array();
            int taken = 0;
            for (auto& [d, item] : scored)
            {
                if (taken++ >= limit) break;
                arr.push_back(std::move(item));
            }

            return json{
                {"ok",      true},
                {"verb",    "find_nearby"},
                {"count",   arr.size()},
                {"range",   range},
                {"objects", arr}
            }.dump();
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
        // Always returns the string the HTTP handler should send back.
        // Intent verbs return {ok, intent_id, queued:true} immediately
        // (engine completion fires later as an SSE event). Sync verbs
        // return their full result inline.
        std::string DispatchCommandInner(Player* bot, BotSession& sess,
                                         std::shared_ptr<PendingCommand>& cmd);

        std::string DispatchCommand(Player* bot, BotSession& sess,
                                    std::shared_ptr<PendingCommand>& cmd)
        {
            try { return DispatchCommandInner(bot, sess, cmd); }
            catch (std::exception const& e)
            {
                json err = { {"ok", false}, {"error", std::string("dispatch threw: ") + e.what()} };
                return err.dump();
            }
        }

        std::string DispatchCommandInner(Player* bot, BotSession& sess,
                                         std::shared_ptr<PendingCommand>& cmd)
        {
            json req;
            try { req = json::parse(cmd->json); }
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

            // do_action queues a DoAction intent. Engine's
            // ExecuteDoAction calls PlayerbotAI::DoSpecificAction
            // — same as the prior inline body, just relocated.
            if (verb == "do_action")
                return QueueDoActionIntent(bot, sess, cmd, req);

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

            // ---- Diagnostic ------------------------------------------

            if (verb == "inspect")
                return DoInspect(bot, sess);

            // set_agent_mode toggles whether the agent harness is
            // actively driving the bot. Default on attach: false
            // (default Engine ticks; bot acts like a normal merc).
            // Agent opts in by setting true. Body:
            // {"verb":"set_agent_mode","mode":true|false}. Returns
            // {ok, agent_mode, previous_mode, changed}. See
            // decisions.md "Agent mode is an explicit opt-in" for
            // the design; .merc agent chat command is the master-
            // side counterpart.
            if (verb == "set_agent_mode")
            {
                if (!req.contains("mode") || !req["mode"].is_boolean())
                    return json{{"ok", false},
                                {"error", "set_agent_mode requires boolean 'mode'"}}.dump();
                bool desired = req["mode"].get<bool>();
                bool prev = BridgeServer::Instance().ApplyAgentModeToggle(bot, desired, "agent");
                return json{
                    {"ok",            true},
                    {"verb",          "set_agent_mode"},
                    {"agent_mode",    desired},
                    {"previous_mode", prev},
                    {"changed",       prev != desired}
                }.dump();
            }

            // ---- Autonomous-driving primitives ------------------------

            if (verb == "move_to")
                return QueueMoveIntent(bot, sess, cmd, req);

            if (verb == "find_nearby")
                return DoFindNearby(bot, req);

            // interact_with queues an Interact intent. The
            // gossip-menu read happens inside the engine's
            // ExecuteInteract on the world thread — same call shape
            // as the prior inline body.
            if (verb == "interact_with")
                return QueueInteractIntent(bot, sess, cmd, req);

            // say queues a Say intent.
            if (verb == "say")
                return QueueSayIntent(bot, sess, cmd, req);

            // wait suspends engine intent dispatch for N ms; useful
            // for chaining "do A, hold, do B" sequences without the
            // harness needing to time-out HTTP round-trips.
            if (verb == "wait")
                return QueueWaitIntent(bot, sess, cmd, req);

            // cancel_intent removes a queued intent or interrupts an
            // in-flight Wait. Sub-tick intents (Move/Interact/Say/
            // DoAction) that have already been popped have already
            // emitted their terminal event by the time cancel sees
            // them — we return ok=false in that case. See decisions.md
            // "Async intent contract: cancellation scope narrowed"
            // (2026-05-02) for the lifetime semantics.
            if (verb == "cancel_intent")
            {
                std::string idStr = req.value("intent_id", "");
                if (idStr.empty())
                    return json{{"ok", false}, {"error", "cancel_intent requires intent_id"}}.dump();

                uint64_t id = 0;
                try { id = std::stoull(idStr); }
                catch (...) { return json{{"ok", false}, {"error", "intent_id must be a uint64 string"}}.dump(); }

                std::string cancelState;
                std::string cancelledVerb;
                if (sess.RemoveIntentById(id, cancelledVerb))
                    cancelState = "from_queue";
                else
                {
                    PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
                    Engine* nc = ai ? ai->GetEngine(BOT_STATE_NON_COMBAT) : nullptr;
                    auto* agentEng = dynamic_cast<Sbywow::SbywowAgentEngine*>(nc);
                    if (agentEng && agentEng->CancelWaitIfMatch(id))
                    {
                        cancelState   = "interrupted";
                        cancelledVerb = "wait";  // only Wait can be in this state
                    }
                }

                if (cancelState.empty())
                {
                    // Last resort: maybe the intent already terminated.
                    // Surface the recovery-ring verdict in the error so
                    // the agent doesn't need a second round-trip.
                    BotSession::TerminalIntent prior;
                    if (sess.LookupTerminal(id, prior))
                        return json{
                            {"ok",          false},
                            {"error",       "intent already terminal"},
                            {"intent_id",   idStr},
                            {"prior_kind",  prior.kind},
                            {"prior_verb",  prior.verb},
                            {"prior_state", prior.state}
                        }.dump();
                    return json{
                        {"ok", false},
                        {"error", "intent_id not found (older than recovery ring or never existed)"},
                        {"intent_id", idStr}
                    }.dump();
                }

                json ev = {
                    {"channel",   "intent"},
                    {"kind",      "intent_cancelled"},
                    {"bot_guid",  bot->GetGUID().GetRawValue()},
                    {"bot_name",  bot->GetName()},
                    {"intent_id", idStr},
                    {"verb",      cancelledVerb},
                    {"state",     cancelState}
                };
                sess.PushOutbound(ev.dump());

                BotSession::TerminalIntent rec;
                rec.intentId = id;
                rec.verb     = cancelledVerb;
                rec.kind     = "intent_cancelled";
                rec.state    = cancelState;
                sess.RecordTerminal(std::move(rec));

                return json{
                    {"ok",        true},
                    {"verb",      "cancel_intent"},
                    {"intent_id", idStr},
                    {"state",     cancelState}
                }.dump();
            }

            // get_intent — recovery primitive. Returns the terminal
            // record for an intent_id that exited the queue/engine
            // recently (within kTerminalRingCap = 64 entries).
            // Useful when an SSE consumer reconnected after
            // disconnection or fell behind the bounded outbound
            // queue. See decisions.md "Async intent contract"
            // (2026-05-02) for the choice of query-primitive
            // over stream-resume.
            if (verb == "get_intent")
            {
                std::string idStr = req.value("intent_id", "");
                if (idStr.empty())
                    return json{{"ok", false}, {"error", "get_intent requires intent_id"}}.dump();

                uint64_t id = 0;
                try { id = std::stoull(idStr); }
                catch (...) { return json{{"ok", false}, {"error", "intent_id must be a uint64 string"}}.dump(); }

                BotSession::TerminalIntent rec;
                if (!sess.LookupTerminal(id, rec))
                    return json{
                        {"ok",        false},
                        {"error",     "intent_id not in recent ring (older than 64 terminals or never existed)"},
                        {"intent_id", idStr}
                    }.dump();

                json out = {
                    {"ok",          true},
                    {"verb",        "get_intent"},
                    {"intent_id",   idStr},
                    {"intent_verb", rec.verb},
                    {"kind",        rec.kind}
                };
                if (!rec.state.empty())
                    out["state"] = rec.state;
                if (!rec.resultJson.empty())
                {
                    try { out["result"] = json::parse(rec.resultJson); }
                    catch (std::exception const&) { out["result"] = rec.resultJson; }
                }
                return out.dump();
            }

            json err = { {"ok", false}, {"error", "unknown verb: " + verb} };
            return err.dump();
        }
    }

    // -----------------------------------------------------------------------
    // World-thread tick: drain inbound commands
    // -----------------------------------------------------------------------

    void BridgeServer::TickBot(Player* bot)
    {
        if (!bot || !running_.load())
            return;

        auto session = GetSession(bot->GetGUID());
        if (!session)
            return;

        // Drain inbound commands. Every dispatch returns the string
        // the HTTP handler is blocked on. Intent verbs return their
        // ack immediately ({ok, intent_id, queued:true}); engine
        // completion is delivered out-of-band via SSE.
        //
        // Agent mode (whether the agent is driving vs. default Engine
        // ticks) is an explicit opt-in managed by `set_agent_mode`
        // (bridge verb) or `.merc agent` (chat command). Default on
        // attach is OFF — see decisions.md "Agent mode is an explicit
        // opt-in." HeartbeatAgeMs is informational only, not coupled
        // to mode.
        std::shared_ptr<PendingCommand> cmd;
        while (session->PopInbound(cmd))
        {
            std::string out = DispatchCommand(bot, *session, cmd);
            try { cmd->result.set_value(std::move(out)); }
            catch (std::future_error const&) { /* receiver gone */ }
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

            // Push command, wait for world-thread to *dispatch* and
            // return. The timeout is a liveness guard against a wedged
            // world thread, not an "execution budget." Sync verbs run
            // entirely on the world thread and finish in <50ms typical
            // (worst case ~500ms for find_nearby with a wide range).
            // Intent verbs return their {ok, intent_id, queued} ack
            // immediately — engine completion lands later as an SSE
            // event (intent_completed/failed/cancelled), so this
            // timeout only covers the dispatch + sync-verb execution
            // path. 3s gives ~60 ticks of headroom for normal
            // world-thread hiccups while surfacing genuine wedges
            // quickly.
            auto fut = session->PushInbound(req.body);
            if (fut.wait_for(std::chrono::seconds(3)) == std::future_status::timeout)
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
                        {"agent_mode",      sess->IsAgentMode()},
                        {"sse_attached",    sess->IsSseAttached()},
                        {"intent_count",    static_cast<int>(sess->IntentCount())}
                    });
                }
            }
            res.set_content(arr.dump(), "application/json");
        });
    }
}
