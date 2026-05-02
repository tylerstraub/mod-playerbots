#include "SbywowAgentEngine.h"

#include "../Bridge/BridgeServer.h"
#include "../Bridge/BotSession.h"
#include "../Bridge/deps/json.hpp"

#include "Creature.h"
#include "Event.h"
#include "GossipDef.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Playerbots.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

using json = nlohmann::json;

namespace Sbywow
{
    SbywowAgentEngine::SbywowAgentEngine(PlayerbotAI* ai, AiObjectContext* context)
        : Engine(ai, context)
    {
        // v1 strategy state: just "default" (WorldPacketHandlerStrategy).
        // Packet-handler plumbing stays alive (accept loot / taxi /
        // group invite / gossip dispatch). Every cognitive default
        // (follow, quest, gather, chat, emote, loot, duel, rpg, grind,
        // move random) is deliberately omitted — those belong to the
        // agent harness, driven via bridge intents.
        //
        // Class-specific buff/cure/heal-prep strategies (e.g., "dps assist",
        // "cure" for priest) are also omitted in v1; the agent issues
        // do_action for these explicitly when wanted. We can revisit
        // this in v1.x if it turns out to be friction.
        addStrategiesNoInit("default", nullptr);
    }

    void SbywowAgentEngine::addStrategy(std::string const name, bool init)
    {
        // Allowlist: only the packet-handler "default" strategy is
        // ever installed in this engine. Class-specific combat-prep
        // strategies (dps assist / cure / tank assist), follow,
        // quest, gather, chat, emote, loot, duel, mount, food, buff,
        // pvp — all rejected. The agent harness drives these via
        // bridge intents (move_to / interact_with / do_action / say).
        //
        // This filter survives PlayerbotAI::ResetStrategies invocations
        // — login, SetMaster, group invite accept, LFG events, talent
        // change, etc. — because all those paths re-enter this method
        // through the virtual dispatch from base ResetStrategies code.
        // The reassertion problem that drove the architectural pivot
        // is closed at the strategy-add level.
        if (name == "default")
        {
            Engine::addStrategy(name, init);
            return;
        }
        // Quietly drop. LOG_DEBUG so we can audit if needed; not a
        // warning because upstream ResetStrategies legitimately tries
        // to add ~15 strategies every time it fires and that's by
        // design — no consumer needs to know it was filtered.
        LOG_DEBUG("playerbots",
                  "[SbywowAgentEngine] filtered strategy add: '{}'", name.c_str());
    }

    namespace
    {
        // Build an `intent` channel SSE event envelope. The
        // BaseEvent shape (channel/kind/bot_guid/bot_name) mirrors
        // BridgeHooks.cpp; intent_id is wire-stringified to dodge
        // JS-side 2^53 truncation. result is included for terminal
        // states only (completed/failed/cancelled).
        json BuildIntentEvent(Player* bot, std::string const& kind,
                              uint64_t intentId, std::string const& verb)
        {
            return {
                {"channel",   "intent"},
                {"kind",      kind},
                {"bot_guid",  bot->GetGUID().GetRawValue()},
                {"bot_name",  bot->GetName()},
                {"intent_id", std::to_string(intentId)},
                {"verb",      verb}
            };
        }
    }

    bool SbywowAgentEngine::DoNextAction(Unit* /*target*/, uint32 /*depth*/, bool /*minimal*/)
    {
        if (!botAI)
            return false;

        Player* bot = botAI->GetBot();
        if (!bot)
            return false;

        if (!tickedOnce_)
        {
            tickedOnce_ = true;
            LOG_INFO("playerbots",
                     "[SbywowAgentEngine] first tick for bot guid={} name={}",
                     bot->GetGUID().GetRawValue(), bot->GetName().c_str());
        }

        auto session = Sbywow::Bridge::BridgeServer::Instance().GetSession(bot->GetGUID());

        // Wait suspension: while engaged, no intents drain. Wait is
        // explicit agent-issued sequencing — "do A, hold for N ms,
        // then do B" — implemented as a pause on intent dispatch.
        // The Wait intent's promise is set at *start* of suspension
        // (so the agent's HTTP call returns immediately with "queued
        // for N ms"), and chained intents queue behind it until
        // suspension lifts. Uses steady_clock to avoid getMSTime's
        // uint32 wraparound — small bug but free to avoid.
        if (isWaiting_)
        {
            if (std::chrono::steady_clock::now() < waitUntil_)
                return false;  // still waiting
            isWaiting_ = false;

            // Wait reached its end naturally — emit intent_completed
            // tagged with the wait's id. Phase 4 cancellation will
            // also clear isWaiting_ but emits intent_cancelled
            // there instead, so we're safe to claim "completed" here.
            if (session && waitingIntentId_ != 0)
            {
                json ev = BuildIntentEvent(bot, "intent_completed",
                                           waitingIntentId_, waitingIntentVerb_);
                ev["result"] = json{{"ok", true}, {"verb", "wait"}};
                session->PushOutbound(ev.dump());
            }
            waitingIntentId_ = 0;
            waitingIntentVerb_.clear();
        }

        // Pop one intent per tick from the per-bot session queue,
        // execute it, set the promise so the HTTP-side verb returns.
        // BridgeServer is the canonical owner of sessions; if the
        // bridge is down or the session was detached out from under
        // us, we silently no-op (intent was implicitly cancelled).
        if (!session)
            return false;

        std::shared_ptr<Sbywow::Bridge::PendingIntent> pending;
        if (!session->PopIntent(pending))
        {
            // Idle tick — run reactive autonomic at low cadence.
            // The agent doesn't have to think about food/drink;
            // it's a reflex like combat reactions.
            TickReactiveAutonomic(bot);
            return false;
        }

        // intent_started fires the moment we commit to executing a
        // popped intent. For sub-tick verbs (Move/Interact/Say/
        // DoAction) intent_completed lands in the same tick; for
        // Wait it lands when the suspension expires. Emit before the
        // dispatch so a slow ExecuteIntent (find_nearby-class work)
        // doesn't reorder against completed.
        {
            json ev = BuildIntentEvent(bot, "intent_started",
                                       pending->intentId, pending->verb);
            session->PushOutbound(ev.dump());
        }

        // Wait is special-cased: arm the suspension and respond
        // immediately. Subsequent intents in the queue wait their
        // turn until suspension lifts in a future tick.
        if (pending->intent.kind == IntentKind::Wait)
        {
            isWaiting_         = true;
            waitUntil_         = std::chrono::steady_clock::now() +
                                 std::chrono::milliseconds(pending->intent.waitMs);
            waitingIntentId_   = pending->intentId;
            waitingIntentVerb_ = pending->verb;
            json out = {
                {"ok",       true},
                {"verb",     "wait"},
                {"wait_ms",  pending->intent.waitMs}
            };
            try { pending->result.set_value(out.dump()); }
            catch (std::future_error const&) {}
            return true;
        }

        std::string outStr = ExecuteIntent(bot, pending->intent);

        // Translate the verb's `ok` flag into intent_completed vs
        // intent_failed. Embed the full result payload so consumers
        // get the same JSON they'd see on the (Phase-1) sync HTTP
        // response. Parse defensively — engine outputs valid JSON,
        // but a malformed string shouldn't take down the bridge.
        json result;
        try { result = json::parse(outStr); }
        catch (std::exception const&) { result = {{"ok", false}, {"error", "engine returned non-JSON"}}; }
        bool ok = result.value("ok", false);

        json ev = BuildIntentEvent(bot, ok ? "intent_completed" : "intent_failed",
                                   pending->intentId, pending->verb);
        ev["result"] = result;
        session->PushOutbound(ev.dump());

        try { pending->result.set_value(std::move(outStr)); }
        catch (std::future_error const&) { /* receiver gone — drop */ }

        return true;
    }

    std::string SbywowAgentEngine::ExecuteIntent(Player* bot, Intent const& intent)
    {
        switch (intent.kind)
        {
            case IntentKind::Move:     return ExecuteMove    (bot, intent);
            case IntentKind::Interact: return ExecuteInteract(bot, intent);
            case IntentKind::Say:      return ExecuteSay     (bot, intent);
            case IntentKind::DoAction: return ExecuteDoAction(bot, intent);
            case IntentKind::Wait:
                // Wait is handled in DoNextAction directly (engine
                // state); should never reach this dispatcher.
                return json{{"ok", false}, {"error", "wait reached ExecuteIntent — bug"}}.dump();
        }
        return json{{"ok", false}, {"error", "unknown intent kind"}}.dump();
    }

    std::string SbywowAgentEngine::ExecuteMove(Player* bot, Intent const& intent)
    {
        // Optional map id: if set and non-zero, must equal current map.
        // Cross-map TP is a separate verb (future work) with a stricter
        // contract — refuse here rather than teleport quietly.
        if (intent.map != 0 && intent.map != bot->GetMapId())
        {
            return json{
                {"ok", false},
                {"error", "move: map mismatch (cross-map needs a teleport verb)"},
                {"requested_map", intent.map},
                {"current_map",   bot->GetMapId()}
            }.dump();
        }

        PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
        if (!ai)
            return json{{"ok", false}, {"error", "no PlayerbotAI for this bot"}}.dump();

        // Use playerbots' canonical movement-allowed predicate. Covers
        // dead, charmed, polymorphed, in-flight, being teleported.
        if (!ai->CanMove())
            return json{{"ok", false}, {"error", "bot cannot move (dead/CC'd/in-flight)"}}.dump();

        MotionMaster* mm = bot->GetMotionMaster();
        if (!mm)
            return json{{"ok", false}, {"error", "no motion master"}}.dump();

        float fromX = bot->GetPositionX();
        float fromY = bot->GetPositionY();
        float fromZ = bot->GetPositionZ();
        float dist  = bot->GetExactDist(intent.x, intent.y, intent.z);

        // Match mod-playerbots' MovementAction::DoMovePoint shape:
        // stand up if sitting, then Clear() and MovePoint with
        // generatePath=true so mmaps are honored. forceDestination=false
        // lets the spline engine refuse if the target is unreachable
        // rather than teleport into geometry.
        if (bot->IsSitState())
            bot->SetStandState(UNIT_STAND_STATE_STAND);
        mm->Clear();
        mm->MovePoint(/*id*/ 0, intent.x, intent.y, intent.z, FORCED_MOVEMENT_NONE,
                      /*speed*/ 0.f, /*orientation*/ 0.f,
                      /*generatePath*/ true,
                      /*forceDestination*/ false);

        return json{
            {"ok",       true},
            {"verb",     "move_to"},
            {"map",      bot->GetMapId()},
            {"from",     {fromX, fromY, fromZ}},
            {"to",       {intent.x, intent.y, intent.z}},
            {"distance", dist}
        }.dump();
    }

    namespace
    {
        // Decode a creature's npc_flags into role tags. Same shape as
        // bridge's find_nearby uses; duplicated here to keep the engine
        // independent of bridge internals. If we ever want one source
        // of truth, lift to a shared helper file.
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
    }

    std::string SbywowAgentEngine::ExecuteInteract(Player* bot, Intent const& intent)
    {
        ObjectGuid og(intent.guid);
        if (!og.IsAnyTypeCreature())
        {
            if (og.IsGameObject())
                return json{{"ok", false},
                            {"error", "interact: gameobjects not yet supported"}}.dump();
            return json{{"ok", false},
                        {"error", "interact: guid is not a creature"}}.dump();
        }

        Creature* npc = bot->GetNPCIfCanInteractWith(og, UNIT_NPC_FLAG_NONE);
        if (!npc)
        {
            if (Creature* c = ObjectAccessor::GetCreature(*bot, og))
                return json{
                    {"ok",    false},
                    {"error", "creature out of interact range or not visible"},
                    {"name",  c->GetName()},
                    {"dist",  bot->GetExactDist(c)},
                    {"alive", c->IsAlive()}
                }.dump();
            return json{{"ok", false}, {"error", "creature not found on bot's map"}}.dump();
        }

        json out = {
            {"ok",             true},
            {"verb",           "interact_with"},
            {"guid",           intent.guid},
            {"kind",           "creature"},
            {"entry",          npc->GetEntry()},
            {"name",           npc->GetName()},
            {"flags",          DecodeCreatureFlags(npc)},
            {"gossip_menu_id", npc->GetCreatureTemplate()->GossipMenuId},
            {"dist",           bot->GetExactDist(npc)}
        };

        // Drive HandleGossipHelloOpcode through the bot's session.
        // Mirrors GossipHelloAction's path; safe under same conditions
        // mod-playerbots invokes it routinely.
        bool gossipOpened = false;
        std::string gossipError;
        try
        {
            WorldPacket data;
            data << og;
            bot->GetSession()->HandleGossipHelloOpcode(data);
            gossipOpened = true;
        }
        catch (std::exception const& e)
        {
            gossipError = std::string("gossip_hello threw: ") + e.what();
        }
        out["gossip_opened"] = gossipOpened;
        if (!gossipError.empty())
            out["gossip_error"] = gossipError;

        if (gossipOpened && bot->PlayerTalkClass)
        {
            json gossipOpts = json::array();
            GossipMenu& menu = bot->PlayerTalkClass->GetGossipMenu();
            for (auto const& [idx, item] : menu.GetMenuItems())
            {
                gossipOpts.push_back({
                    {"index", idx},
                    {"icon",  static_cast<int>(item.MenuItemIcon)},
                    {"text",  item.Message}
                });
            }
            out["gossip_options"] = gossipOpts;
            out["gossip_menu_open_id"] = menu.GetMenuId();
        }

        return out.dump();
    }

    std::string SbywowAgentEngine::ExecuteSay(Player* bot, Intent const& intent)
    {
        PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
        if (!ai)
            return json{{"ok", false}, {"error", "no PlayerbotAI for this bot"}}.dump();

        bool ok = false;
        if      (intent.channel == "say"   || intent.channel.empty()) ok = ai->Say(intent.text);
        else if (intent.channel == "yell")    ok = ai->Yell(intent.text);
        else if (intent.channel == "party")   ok = ai->SayToParty(intent.text);
        else if (intent.channel == "raid")    ok = ai->SayToRaid(intent.text);
        else if (intent.channel == "guild")   ok = ai->SayToGuild(intent.text);
        else if (intent.channel == "world")   ok = ai->SayToWorld(intent.text);
        else if (intent.channel == "master")  ok = ai->TellMaster(intent.text);
        else
        {
            return json{
                {"ok", false},
                {"error", "unknown channel: " + intent.channel +
                          " (say|yell|party|raid|guild|world|master)"}
            }.dump();
        }

        return json{
            {"ok",      ok},
            {"verb",    "say"},
            {"channel", intent.channel.empty() ? "say" : intent.channel},
            {"text",    intent.text}
        }.dump();
    }

    std::string SbywowAgentEngine::ExecuteDoAction(Player* bot, Intent const& intent)
    {
        PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
        if (!ai)
            return json{{"ok", false}, {"error", "no PlayerbotAI for this bot"}}.dump();

        Event ev;
        bool result = ai->DoSpecificAction(intent.actionName, ev,
                                           /*silent=*/true,
                                           intent.actionQualifier);

        return json{
            {"ok",        result},
            {"verb",      "do_action"},
            {"name",      intent.actionName},
            {"qualifier", intent.actionQualifier}
        }.dump();
    }

    void SbywowAgentEngine::TickReactiveAutonomic(Player* bot)
    {
        // Throttle: cadence threshold ticks of idle before we even
        // attempt to fire reactives. Cheap counter increment per
        // idle tick; the work happens once per ~kReactiveCadenceTicks
        // ticks (~2.5s at typical bot tick rate).
        if (++idleTickCount_ < kReactiveCadenceTicks)
            return;
        idleTickCount_ = 0;

        if (!bot->IsAlive())
            return;

        PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
        if (!ai)
            return;

        // Fire upstream EatAction / DrinkAction. They self-gate via
        // isUseful() (high-enough HP/mana, has food/water in bags,
        // not currently eating/drinking) so out-of-context calls
        // are cheap no-ops. Same actions the upstream "food"
        // strategy fires from "low health" / "low mana" triggers;
        // we skip the trigger layer because we don't have the
        // strategy stack — direct DoSpecificAction is enough.
        Event ev;
        ai->DoSpecificAction("food",  ev, /*silent=*/true);
        ai->DoSpecificAction("drink", ev, /*silent=*/true);
    }
}
