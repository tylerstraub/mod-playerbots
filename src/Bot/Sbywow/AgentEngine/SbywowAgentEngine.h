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
    namespace Bridge
    {
        class BotSession;
        struct PendingIntent;
    }

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

        // Same shape as CancelWaitIfMatch, but for the in-flight Move
        // intent. Move is now multi-tick: dispatch arms a MovePoint
        // generator and HOLDS the intent across subsequent ticks
        // until arrival or path failure. cancel_intent on an
        // in-flight Move clears the generator and reports cancelled.
        bool CancelInFlightMoveIfMatch(uint64_t intentId);

        // Same shape, for an in-flight Cast intent (cast_spell /
        // use_item / mount). Locates the live Spell* via
        // FindCurrentSpellBySpellId and calls Spell::cancel(),
        // which interrupts the cast and clears m_currentSpells.
        bool CancelInFlightCastIfMatch(uint64_t intentId);

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

        // In-flight Move accessors. While inFlightMove_ is true the
        // engine is holding a Move intent across ticks, polling for
        // arrival / path failure. Surfaced in context.active_intents
        // so the agent can see "I'm currently walking to X."
        bool        IsMoving()             const { return inFlightMove_; }
        uint64_t    MovingIntentId()       const { return inFlightMoveIntentId_; }
        std::string const& MovingIntentVerb() const { return inFlightMoveVerb_; }
        float       MovingTargetX()        const { return inFlightMoveX_; }
        float       MovingTargetY()        const { return inFlightMoveY_; }
        float       MovingTargetZ()        const { return inFlightMoveZ_; }

        // In-flight Cast accessors. Same shape as Move: while
        // inFlightCast_ is true the engine is holding a cast intent
        // (cast_spell / use_item / mount) across ticks, polling
        // m_currentSpells for completion / interruption. Lets the
        // agent observe "I'm currently casting X with N ms left"
        // via context.active_intents.
        bool        IsCasting()            const { return inFlightCast_; }
        uint64_t    CastingIntentId()      const { return inFlightCastIntentId_; }
        std::string const& CastingIntentVerb() const { return inFlightCastVerb_; }
        uint32_t    CastingSpellId()       const { return inFlightCastSpellId_; }

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
        // Per-tick poll helpers — examine in-flight slot state, emit
        // terminal events when the slot resolves, clear the slot.
        // Both fall through (no return) when the slot is still
        // active — non-blocking intents in the queue can drain in
        // parallel with in-flight moves and casts.
        void PollInFlightMove(Player* bot,
                              std::shared_ptr<Sbywow::Bridge::BotSession> const& session);
        void PollInFlightCast(Player* bot,
                              std::shared_ptr<Sbywow::Bridge::BotSession> const& session);

        // Per-kind dispatchers used by DoNextAction's queue drain.
        // DispatchMoveIntent / DispatchCastIntent return true if they
        // dispatched (whether they set the in-flight slot or emitted
        // a terminal inline). DispatchInstantIntent always emits
        // terminal inline.
        bool DispatchMoveIntent(Player* bot,
                                std::shared_ptr<Sbywow::Bridge::BotSession> const& session,
                                std::shared_ptr<Sbywow::Bridge::PendingIntent> const& pending);
        bool DispatchCastIntent(Player* bot,
                                std::shared_ptr<Sbywow::Bridge::BotSession> const& session,
                                std::shared_ptr<Sbywow::Bridge::PendingIntent> const& pending);
        void DispatchInstantIntent(Player* bot,
                                   std::shared_ptr<Sbywow::Bridge::BotSession> const& session,
                                   std::shared_ptr<Sbywow::Bridge::PendingIntent> const& pending);

        // Classifier used at queue-peek time. Move + Wait + cast-time
        // (or channeled) Cast/UseItem/Mount = blocking; everything
        // else (Say, Interact, instant Cast, Buy, Sell, etc.) =
        // non-blocking. Cast classification reads spellInfo's cast
        // time, which means looking up the spell (and for use_item,
        // the item's on-use spell first).
        bool IsBlockingIntent(Player* bot, Intent const& intent);

        std::string ExecuteMove    (Player* bot, Intent const& intent);
        std::string ExecuteInteract(Player* bot, Intent const& intent);
        std::string ExecuteSay     (Player* bot, Intent const& intent);
        std::string ExecuteDoAction(Player* bot, Intent const& intent);
        // Wait is handled in DoNextAction directly (engine state) —
        // it doesn't run synchronously like the other intents; it
        // suspends queue draining for its duration.

        // Phase 4 — vendor / gossip / trade / inventory / world.
        std::string ExecuteBuyItem            (Player* bot, Intent const& intent);
        std::string ExecuteSellItem           (Player* bot, Intent const& intent);
        std::string ExecuteSelectGossipOption (Player* bot, Intent const& intent);
        std::string ExecuteTradeInitiate      (Player* bot, Intent const& intent);
        std::string ExecuteTradeOfferItem     (Player* bot, Intent const& intent);
        std::string ExecuteTradeOfferMoney    (Player* bot, Intent const& intent);
        std::string ExecuteTradeAccept        (Player* bot, Intent const& intent);
        std::string ExecuteTradeCancel        (Player* bot, Intent const& intent);
        std::string ExecuteEquipItem          (Player* bot, Intent const& intent);
        std::string ExecuteUnequipItem        (Player* bot, Intent const& intent);
        std::string ExecuteDestroyItem        (Player* bot, Intent const& intent);
        std::string ExecuteUseItem            (Player* bot, Intent const& intent);
        std::string ExecuteCastSpell          (Player* bot, Intent const& intent);
        std::string ExecuteMount              (Player* bot, Intent const& intent);
        std::string ExecuteDismount           (Player* bot, Intent const& intent);
        std::string ExecuteInteractGameObject (Player* bot, Intent const& intent);
        std::string ExecuteLootTarget         (Player* bot, Intent const& intent);

        // Phase 5 — mail / quest / group.
        std::string ExecuteMailSend           (Player* bot, Intent const& intent);
        std::string ExecuteMailTakeItem       (Player* bot, Intent const& intent);
        std::string ExecuteMailTakeMoney      (Player* bot, Intent const& intent);
        std::string ExecuteQuestAccept        (Player* bot, Intent const& intent);
        std::string ExecuteQuestComplete      (Player* bot, Intent const& intent);
        std::string ExecuteQuestAbandon       (Player* bot, Intent const& intent);
        std::string ExecuteQuestShare         (Player* bot, Intent const& intent);
        std::string ExecuteGroupAcceptInvite  (Player* bot, Intent const& intent);
        std::string ExecuteGroupDeclineInvite (Player* bot, Intent const& intent);
        std::string ExecuteGroupLeave         (Player* bot, Intent const& intent);
        std::string ExecuteGroupPromoteLeader (Player* bot, Intent const& intent);
        std::string ExecuteGroupReadyCheckRespond(Player* bot, Intent const& intent);

        // Batch 6 — perception / movement / trade-flow primitives.
        // (ComeBack is rewritten in the queue drain into a Move using
        // the master's current position, so no Execute fn here.)
        std::string ExecuteTradeAcceptInvite  (Player* bot, Intent const& intent);
        std::string ExecuteFaceTarget         (Player* bot, Intent const& intent);

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

        // Multi-tick Move state. Move is dispatched sub-tick into the
        // MotionMaster, but the *intent* remains active across ticks
        // until the bot arrives or the path fails. While a Move is
        // in-flight, the engine returns false from DoNextAction (no
        // queue drain, no idle delegation), so follow / react /
        // anything-that-could-replace-the-active-MotionMaster does
        // NOT run. This is the mechanism that prevents follow from
        // clobbering agent-issued moves. See decisions.md "Idle
        // delegation + multi-tick Move" (2026-05-02).
        bool                                   inFlightMove_       = false;
        uint64_t                               inFlightMoveIntentId_ = 0;
        std::string                            inFlightMoveVerb_;
        float                                  inFlightMoveX_      = 0.f;
        float                                  inFlightMoveY_      = 0.f;
        float                                  inFlightMoveZ_      = 0.f;
        std::chrono::steady_clock::time_point  inFlightMoveDispatchAt_;
        // Distance threshold (yards, 2D) at which we consider the
        // bot "arrived." MovePoint terminates within ~1 yard in
        // practice; 2.5y is permissive enough for path-snap edge
        // cases without false positives mid-trip.
        static constexpr float                 kMoveArrivalThreshold = 2.5f;
        // Hard timeout for an in-flight Move. Path can refuse to
        // generate (forceDestination=false) and never fire arrival;
        // 60s is generous for any reasonable trip on a single map.
        static constexpr int64_t               kMoveTimeoutMs = 60000;

        // Multi-tick Cast state. Mirror of inFlightMove_. Dispatched
        // by ExecuteCastSpell / ExecuteUseItem / ExecuteMount when the
        // cast goes into m_currentSpells (cast-time, channeled, or
        // mount). Instant casts terminate inline at dispatch and
        // never set this. Polled in DoNextAction: when
        // FindCurrentSpellBySpellId returns null, the cast finished
        // (cooldown started → completed) or was interrupted
        // (no cooldown → failed/interrupted).
        bool                                   inFlightCast_         = false;
        uint64_t                               inFlightCastIntentId_ = 0;
        std::string                            inFlightCastVerb_;
        uint32_t                               inFlightCastSpellId_  = 0;
        std::chrono::steady_clock::time_point  inFlightCastDispatchAt_;
        // Hard timeout — any spell taking more than 30s to resolve
        // is suspicious (longest non-channel cast in WoW 3.3.5 is
        // Hearthstone at 10s; channels can be longer but resolve
        // via natural channel-end, not timeout).
        static constexpr int64_t               kCastTimeoutMs = 30000;

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
