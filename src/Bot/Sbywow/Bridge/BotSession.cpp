#include "BotSession.h"

#include "Timer.h"

namespace Sbywow::Bridge
{
    BotSession::BotSession(ObjectGuid guid)
        : guid_(guid)
    {
        MarkAlive();
    }

    std::future<std::string> BotSession::PushInbound(std::string commandJson)
    {
        auto pending  = std::make_shared<PendingCommand>();
        pending->json = std::move(commandJson);
        auto fut      = pending->result.get_future();

        {
            std::lock_guard<std::mutex> lock(inboundMutex_);
            inbound_.push_back(pending);
        }

        MarkAlive();
        return fut;
    }

    bool BotSession::PopInbound(std::shared_ptr<PendingCommand>& out)
    {
        std::lock_guard<std::mutex> lock(inboundMutex_);
        if (inbound_.empty())
            return false;
        out = inbound_.front();
        inbound_.pop_front();
        return true;
    }

    void BotSession::PushOutbound(std::string eventJson)
    {
        {
            std::lock_guard<std::mutex> lock(outboundMutex_);
            outbound_.push_back(OutboundEvent{std::move(eventJson)});
            // Bounded queue: drop oldest if we overflow. Slow consumer
            // (or no consumer) shouldn't grow memory unbounded.
            while (outbound_.size() > kOutboundCap)
                outbound_.pop_front();
        }
        outboundCv_.notify_one();
    }

    bool BotSession::WaitOutbound(OutboundEvent& out, int timeoutMs)
    {
        std::unique_lock<std::mutex> lock(outboundMutex_);
        if (outbound_.empty())
        {
            outboundCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                 [&] { return !outbound_.empty(); });
        }
        if (outbound_.empty())
            return false;
        out = std::move(outbound_.front());
        outbound_.pop_front();
        return true;
    }

    void BotSession::MarkAlive()
    {
        lastAliveMs_.store(static_cast<int64_t>(getMSTime()));
    }

    int BotSession::HeartbeatAgeMs() const
    {
        int64_t last = lastAliveMs_.load();
        int64_t now  = static_cast<int64_t>(getMSTime());
        return static_cast<int>(now - last);
    }
}
