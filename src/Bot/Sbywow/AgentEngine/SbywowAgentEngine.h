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
 * Phase 1: empty DoNextAction; strategy filter rejects all but
 *          "default" so packet-handler plumbing survives.
 * Phase 2 (this file): DoNextAction drains the per-bot Intent queue
 *          (one per tick), dispatches by IntentKind, sets the
 *          PendingIntent's promise so the HTTP-side verb returns.
 * Phase 3+: reactive autonomic eat/drink, more intent kinds.
 */

#ifndef _SBYWOW_AGENT_ENGINE_H
#define _SBYWOW_AGENT_ENGINE_H

#include "Engine.h"
#include "Intent.h"

#include <chrono>
#include <string>

class AiObjectContext;
class Player;
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

        // Execute a single Intent against the bot. Returns the JSON
        // string the HTTP-side verb will respond with. Public so the
        // bridge can call it for synchronous unit testing if/when we
        // want; primary call site is DoNextAction.
        std::string ExecuteIntent(Player* bot, Intent const& intent);

    private:
        std::string ExecuteMove    (Player* bot, Intent const& intent);
        std::string ExecuteInteract(Player* bot, Intent const& intent);
        std::string ExecuteSay     (Player* bot, Intent const& intent);
        std::string ExecuteDoAction(Player* bot, Intent const& intent);
        // Wait is handled in DoNextAction directly (engine state) —
        // it doesn't run synchronously like the other intents; it
        // suspends queue draining for its duration.

        bool                                   tickedOnce_  = false;
        // Engaged means the engine is in a wait-suspension; no
        // intents are popped until steady_clock::now() >= waitUntil_.
        // steady_clock so we avoid getMSTime's uint32 wraparound.
        bool                                   isWaiting_   = false;
        std::chrono::steady_clock::time_point  waitUntil_;
    };
}

#endif
