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
 * Behaviors per tick:
 *  - Strategy filter (addStrategy) admits only the "default" packet-
 *    handler strategy; cognitive defaults (follow/quest/gather/etc.)
 *    are dropped so PlayerbotAI::ResetStrategies is a no-op against
 *    us by construction.
 *  - DoNextAction drains the per-bot Intent queue (one per tick) and
 *    emits SSE intent_started / intent_completed / intent_failed events
 *    on the bridge's outbound stream. Wait intents arm a steady_clock
 *    suspension; intent_completed fires when the suspension lifts.
 *  - Cancellation lands via CancelWaitIfMatch (queue-side cancel goes
 *    through BotSession::RemoveIntentById in the bridge).
 *  - Reactive autonomic (eat/drink) fires during true idle ticks at
 *    a low cadence; upstream actions self-gate via isUseful().
 */

#ifndef _SBYWOW_AGENT_ENGINE_H
#define _SBYWOW_AGENT_ENGINE_H

#include "Engine.h"
#include "Intent.h"

#include <chrono>
#include <memory>
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

        // Cancellation hook: if the engine is currently suspended on
        // a Wait intent matching this id, clear the suspension. The
        // SSE intent_cancelled event is *not* emitted here — caller
        // (bridge dispatcher) emits it once the cancel call returns
        // true, so all cancel paths converge on a single emit site.
        // Returns true if a wait was cleared, false if the engine
        // wasn't waiting on this id.
        bool CancelWaitIfMatch(uint64_t intentId);

        // ---- Observability accessors -----------------------------
        //
        // Read-only views into engine state for inspect / snapshot.
        // All callers run on the world thread (TickBot dispatch and
        // OnPlayerUpdate snapshot emit), same thread that mutates
        // these fields in DoNextAction / CancelWaitIfMatch — no
        // synchronization needed.
        bool        IsWaiting()           const { return isWaiting_; }
        uint64_t    WaitingIntentId()     const { return waitingIntentId_; }
        std::string const& WaitingIntentVerb() const { return waitingIntentVerb_; }
        // Returns 0 when not waiting; otherwise milliseconds until
        // the suspension expires (clamped at 0 if already past).
        int64_t     WaitingRemainingMs()  const;

        uint64_t    TicksTotal()              const { return ticksTotal_; }
        uint64_t    IntentsDispatchedTotal()  const { return intentsDispatchedTotal_; }
        uint64_t    ReactivesFiredTotal()     const { return reactivesFiredTotal_; }

        // Default-engine accessors. The default engine is the upstream
        // mod-playerbots Engine instance built at construction with
        // the full default non-combat strategy stack. We hold it
        // permanently — strategy ops on us do NOT propagate to it, so
        // its behavior is always "what a normal default merc does."
        // When session->IsAgentMode() is FALSE (the default for fresh
        // attaches), DoNextAction routes here; the agent's queue and
        // wait state on us are preserved untouched so a mid-plan
        // toggle-off-then-on resumes cleanly. See decisions.md
        // "Agent mode is an explicit opt-in" for the design.
        size_t      DefaultEngineStrategiesCount() const;
        uint64_t    DefaultEngineTicksTotal()      const { return defaultEngineTicksTotal_; }

    private:
        std::string ExecuteMove    (Player* bot, Intent const& intent);
        std::string ExecuteInteract(Player* bot, Intent const& intent);
        std::string ExecuteSay     (Player* bot, Intent const& intent);
        std::string ExecuteDoAction(Player* bot, Intent const& intent);
        // Wait is handled in DoNextAction directly (engine state) —
        // it doesn't run synchronously like the other intents; it
        // suspends queue draining for its duration.

        // Reactive autonomic: fire upstream "food"/"drink" actions
        // when the bot is idle (no queued intents, no wait active)
        // at low cadence. The actions self-gate via isUseful() so
        // calls when not-useful are cheap no-ops.
        void TickReactiveAutonomic(Player* bot);

        bool                                   tickedOnce_      = false;
        bool                                   isWaiting_       = false;
        // steady_clock so we avoid getMSTime's uint32 wraparound.
        std::chrono::steady_clock::time_point  waitUntil_;
        // Counter to throttle reactive autonomic checks. Increments
        // each idle tick; reactives fire when it crosses the cadence
        // threshold (kReactiveCadenceTicks below).
        uint32_t                               idleTickCount_   = 0;
        static constexpr uint32_t              kReactiveCadenceTicks = 50;

        // Current Wait suspension's intent_id + verb (always "wait"),
        // retained across the suspension so we can emit
        // intent_completed when isWaiting_ lifts.
        uint64_t                               waitingIntentId_   = 0;
        std::string                            waitingIntentVerb_;

        // Cumulative counters surfaced via inspect for "is the engine
        // ticking? are intents flowing?" sanity. World-thread only;
        // no atomics needed.
        uint64_t                               ticksTotal_                 = 0;
        uint64_t                               intentsDispatchedTotal_     = 0;
        uint64_t                               reactivesFiredTotal_        = 0;
        uint64_t                               defaultEngineTicksTotal_    = 0;

        // Default Engine — built in our constructor via the upstream
        // AiFactory pattern, populated with the full default
        // non-combat strategy stack, fully Init'd. Ticks while
        // session->IsAgentMode() is false (the default for fresh
        // attaches). unique_ptr cleans up on our destruction. Its
        // strategy stack is intentionally immutable post-construction
        // (agent-issued strategy ops on *us* do NOT propagate) so its
        // behavior is predictable: "what a default merc does."
        std::unique_ptr<Engine>                defaultEngine_;
    };
}

#endif
