/*
 * sbywow Agent Bridge — per-bot session state.
 *
 * One BotSession per attached bot. Holds the inbound command queue
 * (httplib thread → world thread) and the outbound event queue (world
 * thread → SSE writer thread), plus heartbeat timestamp and AFK
 * bookkeeping. The session is owned by BridgeServer via shared_ptr so
 * that an in-flight command or SSE writer can outlive a detach.
 */

#ifndef _SBYWOW_BRIDGE_BOT_SESSION_H
#define _SBYWOW_BRIDGE_BOT_SESSION_H

#include "../AgentEngine/Intent.h"
#include "ObjectGuid.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Sbywow::Bridge
{
    // A single inbound command awaiting world-thread execution. The httplib
    // handler creates one, pushes it to the session, then waits on
    // result.get_future() for the response to send back over HTTP.
    struct PendingCommand
    {
        std::string                    json;       // raw verb payload
        std::promise<std::string>      result;     // JSON response
    };

    // Outbound event JSON, ready to write to the SSE stream.
    struct OutboundEvent
    {
        std::string json;
    };

    // An agent-bonded engine intent awaiting execution. The HTTP
    // handler — having received an intent verb — assigns an intent_id,
    // pushes a PendingIntent onto the bot's queue, and returns
    // {ok, intent_id} to the caller immediately. SbywowAgentEngine
    // pops one per tick and emits SSE events on the `intent` channel
    // (intent_started, intent_completed/failed, intent_cancelled) so
    // the agent harness can correlate completion to the queued id.
    //
    // No promise here: completion travels via SSE, not HTTP. The
    // earlier sync-HTTP model held the connection open until the
    // engine completed, which forced long-running operations
    // through the 3s dispatch budget — this design was the
    // resolution. See decisions.md "Async-via-events" (2026-05-02)
    // for the architectural reasoning.
    struct PendingIntent
    {
        Sbywow::Intent  intent;
        uint64_t        intentId = 0;
        std::string     verb;
    };

    class BotSession : public std::enable_shared_from_this<BotSession>
    {
    public:
        explicit BotSession(ObjectGuid guid);

        ObjectGuid Guid() const { return guid_; }

        // Cached bot name for cheap CLI name→guid lookup. Set on attach
        // when we have the Player*; immutable for the session lifetime.
        std::string const& Name() const { return name_; }
        void SetName(std::string const& n) { name_ = n; }

        // Inbound (httplib → world). Push returns the future; world-thread
        // drain pops with PopInbound.
        std::future<std::string> PushInbound(std::string commandJson);
        bool                     PopInbound(std::shared_ptr<PendingCommand>& out);

        // Intent queue (TickBot dispatcher → SbywowAgentEngine). Bridge
        // verb handlers translate intent verbs (move_to, etc.) into
        // PendingIntent records and push here; engine's DoNextAction
        // pops one per tick and executes. Lifecycle = bridge attach;
        // queue is dropped on detach.
        void   PushIntent(std::shared_ptr<PendingIntent> pending);
        bool   PopIntent(std::shared_ptr<PendingIntent>& out);
        // Peek at the head of the queue WITHOUT removing. Used by
        // DoNextAction's "is the next intent blocking?" check —
        // engine peeks, decides whether the in-flight slot situation
        // permits dispatch, then pops. Safe under the same world-
        // thread invariant that pops live on (peek and pop run on the
        // same thread; HTTP-side pushes append to the back).
        std::shared_ptr<PendingIntent> PeekIntent() const;
        size_t IntentCount() const;

        // Snapshot the queue contents for the context block's
        // active_intents projection. Returns a copy of {intentId, verb,
        // intent kind name} for each queued intent, in queue order.
        // The currently-suspended Wait intent is held in the engine,
        // not the queue, so it's projected separately by the caller.
        struct IntentView
        {
            uint64_t    intentId = 0;
            std::string verb;
            std::string kind;       // "move"|"interact"|"say"|"do_action"|"wait"
        };
        std::vector<IntentView> ProjectIntents() const;

        // Cancel-by-id: remove the matching intent from the queue if
        // present. Returns true on success, false if no queued intent
        // matches. On success, fills outVerb with the cancelled
        // intent's verb (so the caller can record it in the
        // terminal ring). Used by the cancel_intent verb. The
        // companion case — cancelling a Wait intent that has
        // already been popped and is suspending the engine — is
        // handled inside SbywowAgentEngine::CancelWaitIfMatch.
        bool   RemoveIntentById(uint64_t intentId, std::string& outVerb);

        // Recovery ring for the get_intent sync verb. Every terminal
        // intent (completed / failed / cancelled) is recorded here
        // alongside the SSE emit. Bounded ring (kTerminalRingCap)
        // so an agent harness that missed a completion event can
        // ask "what happened to intent N" within the recovery
        // window. See decisions.md "Async intent contract"
        // (2026-05-02) for the design choice — query primitive
        // instead of stream-resume.
        struct TerminalIntent
        {
            uint64_t    intentId   = 0;
            std::string verb;        // original verb (move_to, etc.)
            std::string kind;        // intent_completed | intent_failed | intent_cancelled
            std::string state;       // for cancelled: from_queue | interrupted
            std::string resultJson;  // for completed/failed: serialized result payload
        };
        void RecordTerminal(TerminalIntent record);
        bool LookupTerminal(uint64_t intentId, TerminalIntent& out) const;

        static constexpr size_t kTerminalRingCap = 64;

        // Outbound (world → SSE). Push wakes the SSE writer; the writer
        // calls WaitOutbound which blocks until events arrive or timeout.
        void                     PushOutbound(std::string eventJson);
        bool                     WaitOutbound(OutboundEvent& out, int timeoutMs);

        // Heartbeat / liveness. POST /cmd and explicit ping verb both
        // call MarkAlive. HeartbeatAgeMs is purely informational now —
        // surfaced via inspect.session.heartbeat_ms — and no longer
        // drives any automatic state transition. See decisions.md
        // "Agent mode is an explicit opt-in" for rationale.
        void   MarkAlive();
        int    HeartbeatAgeMs() const;

        // Session uptime, monotonic since BotSession construction
        // (roughly: since first attach for this guid this server-up
        // cycle). steady_clock so we don't inherit getMSTime's
        // uint32 wraparound. Surfaced as session.uptime_ms in the
        // context block.
        int64_t UptimeMs() const;

        // Agent mode. Explicitly opt-in: a freshly-attached bot
        // defaults to agent_mode=false, which means the bot behaves
        // like a normal default merc (SbywowAgentEngine routes ticks
        // to its internal default Engine instance). The agent harness
        // opts in via the `set_agent_mode` bridge verb, or the master
        // can flip it via `.merc agent <name> [on|off]`. When true,
        // the bot's BOT_STATE_NON_COMBAT is owned by the agent's
        // intent queue.
        //
        // Never toggled implicitly — heartbeat lapse, network blips,
        // detach all leave it alone. Reconnects start fresh
        // (default=false), so there's no zombie "agent control"
        // state lingering across summon/dismiss cycles.
        //
        // Queue and wait state on the engine ARE preserved across
        // both transitions so the agent can resume mid-plan on
        // toggle-back. See decisions.md "Agent mode is an explicit
        // opt-in" for the full rationale.
        bool   IsAgentMode() const { return agentMode_.load(); }
        void   SetAgentMode(bool v) { agentMode_.store(v); }

        // Follow mode (idle behavior under agent_mode==true). Default
        // true — when the agent has no queued intent and isn't in a
        // wait suspension, SbywowAgentEngine delegates the tick to the
        // default Engine, which runs the upstream follow / react /
        // autonomic strategies. Bot follows master at idle.
        //
        // Agent disables via `set_follow off` for "anchor here
        // indefinitely" scenarios that don't fit the wait timeout
        // (camping a spot for many minutes, holding rendezvous, etc).
        // Agent intents always preempt follow regardless — having
        // an active intent in the queue or an active Move multi-tick
        // is enough to suppress follow without toggling this flag.
        // See decisions.md "Idle delegation + follow toggle"
        // (2026-05-02) for rationale.
        bool   IsFollowMode() const { return followMode_.load(); }
        void   SetFollowMode(bool v) { followMode_.store(v); }

        // SSE attachment tracking — set when /events handler enters its
        // streaming loop, cleared when the connection drops. The
        // EventEmitter checks this before pushing (no point queuing if
        // nothing is reading), but for v1 we queue unconditionally to
        // avoid losing the first events of a fresh connection.
        void   SetSseAttached(bool v) { sseAttached_.store(v); }
        bool   IsSseAttached() const { return sseAttached_.load(); }

        // Convenience: bounded outbound queue cap. Drop oldest on overflow.
        static constexpr size_t kOutboundCap = 1024;

    private:
        ObjectGuid                                  guid_;
        std::string                                 name_;

        std::mutex                                  inboundMutex_;
        std::deque<std::shared_ptr<PendingCommand>> inbound_;

        mutable std::mutex                          intentMutex_;
        std::deque<std::shared_ptr<PendingIntent>>  intents_;

        mutable std::mutex                          terminalMutex_;
        std::deque<TerminalIntent>                  terminals_;

        std::mutex                                  outboundMutex_;
        std::condition_variable                     outboundCv_;
        std::deque<OutboundEvent>                   outbound_;

        std::atomic<int64_t>                        lastAliveMs_{0};
        std::atomic<bool>                           agentMode_{false};
        std::atomic<bool>                           followMode_{true};
        std::atomic<bool>                           sseAttached_{false};

        // Set in the constructor; read by UptimeMs() to compute
        // session.uptime_ms in the context snapshot. steady_clock
        // avoids the uint32 wrap that getMSTime() suffers from.
        std::chrono::steady_clock::time_point       attachedAt_{
            std::chrono::steady_clock::now()};
    };
}

#endif
