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

    class BotSession : public std::enable_shared_from_this<BotSession>
    {
    public:
        explicit BotSession(ObjectGuid guid);

        ObjectGuid Guid() const { return guid_; }

        // Inbound (httplib → world). Push returns the future; world-thread
        // drain pops with PopInbound.
        std::future<std::string> PushInbound(std::string commandJson);
        bool                     PopInbound(std::shared_ptr<PendingCommand>& out);

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

        // Seize state. While seized, the BridgeActionListener vetoes any
        // action that wasn't tagged agent-originated, suppressing
        // strategy/trigger output. AgentActionInFlight is the tag — set
        // only during the synchronous DoSpecificAction call from the
        // command dispatcher (use ScopedAgentAction).
        void   SetSeized(bool v) { seized_.store(v); }
        bool   IsSeized() const { return seized_.load(); }
        void   SetAgentActionInFlight(bool v) { agentActionInFlight_.store(v); }
        bool   IsAgentActionInFlight() const { return agentActionInFlight_.load(); }

        // Convenience: bounded outbound queue cap. Drop oldest on overflow.
        static constexpr size_t kOutboundCap = 1024;

    private:
        ObjectGuid                                  guid_;

        std::mutex                                  inboundMutex_;
        std::deque<std::shared_ptr<PendingCommand>> inbound_;

        std::mutex                                  outboundMutex_;
        std::condition_variable                     outboundCv_;
        std::deque<OutboundEvent>                   outbound_;

        std::atomic<int64_t>                        lastAliveMs_{0};
        std::atomic<bool>                           isAfk_{false};
        std::atomic<bool>                           sseAttached_{false};
        std::atomic<bool>                           seized_{false};
        std::atomic<bool>                           agentActionInFlight_{false};
    };

    // RAII tag for "this code is running an agent-originated action."
    // The BridgeActionListener checks this on AllowExecution to bypass
    // the seize veto for agent commands. Construct around the
    // DoSpecificAction call; destruction clears the flag even if the
    // call returns early.
    struct ScopedAgentAction
    {
        explicit ScopedAgentAction(BotSession& s) : sess_(s) { sess_.SetAgentActionInFlight(true); }
        ~ScopedAgentAction() { sess_.SetAgentActionInFlight(false); }
        ScopedAgentAction(ScopedAgentAction const&) = delete;
        ScopedAgentAction& operator=(ScopedAgentAction const&) = delete;
    private:
        BotSession& sess_;
    };
}

#endif
