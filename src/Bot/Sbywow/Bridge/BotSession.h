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
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <string>

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
    // handler — having received an intent verb (move_to today; more
    // in Phase 3) — pushes one of these into the bot's intent queue.
    // SbywowAgentEngine::DoNextAction pops one per tick, executes it,
    // and sets the result promise. The HTTP handler's future is
    // bound to that promise (moved out of the inbound PendingCommand
    // when the verb dispatched), so the agent's HTTP call unblocks
    // when the engine completes the intent — same external timing as
    // synchronous verbs were before, just with the intent queue in
    // the middle.
    struct PendingIntent
    {
        Sbywow::Intent             intent;
        std::promise<std::string>  result;
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
        size_t IntentCount() const;

        // Outbound (world → SSE). Push wakes the SSE writer; the writer
        // calls WaitOutbound which blocks until events arrive or timeout.
        void                     PushOutbound(std::string eventJson);
        bool                     WaitOutbound(OutboundEvent& out, int timeoutMs);

        // Heartbeat / liveness. POST /cmd and explicit ping verb both
        // call MarkAlive. World-thread tick reads HeartbeatAgeMs to drive
        // the AFK degradation flag.
        void   MarkAlive();
        int    HeartbeatAgeMs() const;
        bool   IsAfk() const { return isAfk_.load(); }
        void   SetAfk(bool v) { isAfk_.store(v); }

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

        std::mutex                                  outboundMutex_;
        std::condition_variable                     outboundCv_;
        std::deque<OutboundEvent>                   outbound_;

        std::atomic<int64_t>                        lastAliveMs_{0};
        std::atomic<bool>                           isAfk_{false};
        std::atomic<bool>                           sseAttached_{false};
    };
}

#endif
