#include "SbywowAgentEngine.h"

#include "../Bridge/BridgeServer.h"
#include "../Bridge/BotSession.h"
#include "../Bridge/deps/json.hpp"

#include "Log.h"
#include "MotionMaster.h"
#include "Player.h"
#include "Playerbots.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

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

        // Pop one intent per tick from the per-bot session queue,
        // execute it, set the promise so the HTTP-side verb returns.
        // BridgeServer is the canonical owner of sessions; if the
        // bridge is down or the session was detached out from under
        // us, we silently no-op (intent was implicitly cancelled).
        auto session = Sbywow::Bridge::BridgeServer::Instance().GetSession(bot->GetGUID());
        if (!session)
            return false;

        std::shared_ptr<Sbywow::Bridge::PendingIntent> pending;
        if (!session->PopIntent(pending))
            return false;

        std::string out = ExecuteIntent(bot, pending->intent);
        try { pending->result.set_value(std::move(out)); }
        catch (std::future_error const&) { /* receiver gone — drop */ }

        return true;
    }

    std::string SbywowAgentEngine::ExecuteIntent(Player* bot, Intent const& intent)
    {
        switch (intent.kind)
        {
            case IntentKind::Move:
                return ExecuteMove(bot, intent);
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
}
