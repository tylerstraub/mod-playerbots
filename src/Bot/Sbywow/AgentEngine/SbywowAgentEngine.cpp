#include "SbywowAgentEngine.h"

#include "Log.h"
#include "Player.h"
#include "PlayerbotAI.h"

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
        if (!tickedOnce_ && botAI)
        {
            tickedOnce_ = true;
            if (Player* bot = botAI->GetBot())
            {
                LOG_INFO("playerbots",
                         "[SbywowAgentEngine] first tick for bot guid={} name={}",
                         bot->GetGUID().GetRawValue(), bot->GetName().c_str());
            }
        }

        // v1: no work. The bot stands still in non-combat unless the
        // agent harness pushes intents. Phase 2 adds intent-queue drain
        // and reactive eat/drink.
        return false;
    }
}
