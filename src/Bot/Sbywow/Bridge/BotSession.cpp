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

    void BotSession::PushIntent(std::shared_ptr<PendingIntent> pending)
    {
        std::lock_guard<std::mutex> lock(intentMutex_);
        intents_.push_back(std::move(pending));
    }

    bool BotSession::PopIntent(std::shared_ptr<PendingIntent>& out)
    {
        std::lock_guard<std::mutex> lock(intentMutex_);
        if (intents_.empty())
            return false;
        out = intents_.front();
        intents_.pop_front();
        return true;
    }

    std::shared_ptr<PendingIntent> BotSession::PeekIntent() const
    {
        std::lock_guard<std::mutex> lock(intentMutex_);
        if (intents_.empty())
            return nullptr;
        return intents_.front();
    }

    size_t BotSession::IntentCount() const
    {
        std::lock_guard<std::mutex> lock(intentMutex_);
        return intents_.size();
    }

    std::vector<BotSession::IntentView> BotSession::ProjectIntents() const
    {
        std::vector<IntentView> out;
        std::lock_guard<std::mutex> lock(intentMutex_);
        out.reserve(intents_.size());
        for (auto const& p : intents_)
        {
            if (!p) continue;
            char const* kindName = "unknown";
            switch (p->intent.kind)
            {
                case Sbywow::IntentKind::Move:                   kindName = "move";       break;
                case Sbywow::IntentKind::Interact:               kindName = "interact";   break;
                case Sbywow::IntentKind::Say:                    kindName = "say";        break;
                case Sbywow::IntentKind::DoAction:               kindName = "do_action";  break;
                case Sbywow::IntentKind::Wait:                   kindName = "wait";       break;
                case Sbywow::IntentKind::BuyItem:                kindName = "buy_item"; break;
                case Sbywow::IntentKind::SellItem:               kindName = "sell_item"; break;
                case Sbywow::IntentKind::SelectGossipOption:     kindName = "select_gossip_option"; break;
                case Sbywow::IntentKind::TradeInitiate:          kindName = "trade_initiate"; break;
                case Sbywow::IntentKind::TradeOfferItem:         kindName = "trade_offer_item"; break;
                case Sbywow::IntentKind::TradeOfferMoney:        kindName = "trade_offer_money"; break;
                case Sbywow::IntentKind::TradeAccept:            kindName = "trade_accept"; break;
                case Sbywow::IntentKind::TradeCancel:            kindName = "trade_cancel"; break;
                case Sbywow::IntentKind::EquipItem:              kindName = "equip_item"; break;
                case Sbywow::IntentKind::UnequipItem:            kindName = "unequip_item"; break;
                case Sbywow::IntentKind::DestroyItem:            kindName = "destroy_item"; break;
                case Sbywow::IntentKind::UseItem:                kindName = "use_item"; break;
                case Sbywow::IntentKind::CastSpell:              kindName = "cast_spell"; break;
                case Sbywow::IntentKind::Mount:                  kindName = "mount"; break;
                case Sbywow::IntentKind::Dismount:               kindName = "dismount"; break;
                case Sbywow::IntentKind::InteractGameObject:     kindName = "interact_gameobject"; break;
                case Sbywow::IntentKind::LootTarget:             kindName = "loot_target"; break;
                case Sbywow::IntentKind::MailSend:               kindName = "mail_send"; break;
                case Sbywow::IntentKind::MailTakeItem:           kindName = "mail_take_item"; break;
                case Sbywow::IntentKind::MailTakeMoney:          kindName = "mail_take_money"; break;
                case Sbywow::IntentKind::QuestAccept:            kindName = "quest_accept"; break;
                case Sbywow::IntentKind::QuestComplete:          kindName = "quest_complete"; break;
                case Sbywow::IntentKind::QuestAbandon:           kindName = "quest_abandon"; break;
                case Sbywow::IntentKind::QuestShare:             kindName = "quest_share"; break;
                case Sbywow::IntentKind::GroupAcceptInvite:      kindName = "group_accept_invite"; break;
                case Sbywow::IntentKind::GroupDeclineInvite:     kindName = "group_decline_invite"; break;
                case Sbywow::IntentKind::GroupLeave:             kindName = "group_leave"; break;
                case Sbywow::IntentKind::GroupPromoteLeader:     kindName = "group_promote_leader"; break;
                case Sbywow::IntentKind::GroupReadyCheckRespond: kindName = "group_ready_check_respond"; break;
            }
            out.push_back(IntentView{ p->intentId, p->verb, kindName });
        }
        return out;
    }

    bool BotSession::RemoveIntentById(uint64_t intentId, std::string& outVerb)
    {
        std::lock_guard<std::mutex> lock(intentMutex_);
        for (auto it = intents_.begin(); it != intents_.end(); ++it)
        {
            if (*it && (*it)->intentId == intentId)
            {
                outVerb = (*it)->verb;
                intents_.erase(it);
                return true;
            }
        }
        return false;
    }

    void BotSession::RecordTerminal(TerminalIntent record)
    {
        std::lock_guard<std::mutex> lock(terminalMutex_);
        terminals_.push_back(std::move(record));
        while (terminals_.size() > kTerminalRingCap)
            terminals_.pop_front();
    }

    bool BotSession::LookupTerminal(uint64_t intentId, TerminalIntent& out) const
    {
        std::lock_guard<std::mutex> lock(terminalMutex_);
        // Scan from newest backward — common case is "I just missed
        // a completion event seconds ago."
        for (auto it = terminals_.rbegin(); it != terminals_.rend(); ++it)
        {
            if (it->intentId == intentId)
            {
                out = *it;
                return true;
            }
        }
        return false;
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

    int64_t BotSession::UptimeMs() const
    {
        auto delta = std::chrono::steady_clock::now() - attachedAt_;
        return std::chrono::duration_cast<std::chrono::milliseconds>(delta).count();
    }
}
