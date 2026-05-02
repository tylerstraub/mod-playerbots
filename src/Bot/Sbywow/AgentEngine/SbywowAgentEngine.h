/*
 * sbywow Agent Bridge — replacement non-combat engine.
 *
 * For agent-bonded bots (mercs today; PBC-card-bonded bots later),
 * this engine replaces the upstream Engine in BOT_STATE_NON_COMBAT.
 * Combat and dead engines remain default — class spell rotation and
 * death/revive flow stay autonomic. Non-combat decisions (move, talk,
 * interact, choose targets) are agent-driven via the bridge.
 *
 * Architecture: see docs/decisions.md "Architectural pivot:
 * replace the non-combat engine for agent-bonded bots" (2026-05-02).
 * Design: see docs/agent-engine-design.md.
 *
 * v1 scope (this file):
 *   - DoNextAction returns false unconditionally (no work).
 *   - Strategy state held to the single "default" strategy
 *     (WorldPacketHandlerStrategy) so packet-driven plumbing — accept
 *     loot, taxi confirm, group invite, gossip select-option dispatch —
 *     keeps working. None of those drive cognitive decisions.
 *   - One INFO log line on first tick for verification.
 *
 * v2 scope (next phase): drains an Intent queue from BotSession and
 * dispatches per-tick. v3+: reactive eat/drink autonomic.
 */

#ifndef _SBYWOW_AGENT_ENGINE_H
#define _SBYWOW_AGENT_ENGINE_H

#include "Engine.h"

class AiObjectContext;
class PlayerbotAI;
class Unit;

namespace Sbywow
{
    class SbywowAgentEngine : public Engine
    {
    public:
        SbywowAgentEngine(PlayerbotAI* botAI, AiObjectContext* context);
        ~SbywowAgentEngine() override = default;

        bool DoNextAction(Unit* target, uint32 depth = 0, bool minimal = false) override;

        // Filter cognitive strategies that PlayerbotAI::ResetStrategies
        // tries to add via AddDefaultNonCombatStrategies. We accept only
        // packet-handler glue ("default"); everything else (follow,
        // quest, gather, chat, emote, loot, duel, class strategies)
        // gets dropped silently. The agent harness owns those decisions.
        void addStrategy(std::string const name, bool init = true) override;

    private:
        bool tickedOnce_ = false;
    };
}

#endif
