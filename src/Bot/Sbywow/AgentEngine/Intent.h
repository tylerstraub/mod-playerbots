/*
 * sbywow Agent Bridge — intent record.
 *
 * The agent harness pushes intents through the bridge; the
 * SbywowAgentEngine pops one per tick and executes. This file
 * declares the intent shape — a tagged union of the primitive
 * actions the harness can request.
 *
 * Five kinds today: Move, Interact, Say, DoAction, Wait. The
 * struct is a flat record on purpose: one allocation, trivially
 * copyable, no std::variant indirection. Adding kinds is additive
 * — extend the enum and the struct (or add a per-kind sub-struct
 * if/when fields proliferate).
 */

#ifndef _SBYWOW_AGENT_ENGINE_INTENT_H
#define _SBYWOW_AGENT_ENGINE_INTENT_H

#include <cstdint>
#include <string>

namespace Sbywow
{
    enum class IntentKind : uint8_t
    {
        // Phase 1-3 primitives.
        Move,
        Interact,
        Say,
        DoAction,
        Wait,

        // Phase 4 — vendor / trade / gossip / world-interaction action verbs.
        BuyItem,
        SellItem,
        SelectGossipOption,
        TradeInitiate,
        TradeOfferItem,
        TradeOfferMoney,
        TradeAccept,
        TradeCancel,
        EquipItem,
        UnequipItem,
        DestroyItem,
        UseItem,
        CastSpell,
        Mount,
        Dismount,
        InteractGameObject,
        LootTarget,

        // Phase 5 — mail / quest.
        MailSend,
        MailTakeItem,
        MailTakeMoney,
        QuestAccept,
        QuestComplete,
        QuestAbandon,
        QuestShare,

        // Phase 5 — group control.
        GroupAcceptInvite,
        GroupDeclineInvite,
        GroupLeave,
        GroupPromoteLeader,
        GroupReadyCheckRespond,
    };

    struct Intent
    {
        IntentKind kind = IntentKind::Move;

        // Move:
        float    x    = 0.f;
        float    y    = 0.f;
        float    z    = 0.f;
        uint32_t map  = 0;       // 0 = no constraint

        // Interact / Trade target / GameObject / Loot / Cast target:
        uint64_t guid = 0;       // target object guid (creature/player/go)

        // Say:
        std::string text;
        std::string channel;     // "say"|"yell"|"party"|"raid"|"guild"|"world"|"master"

        // DoAction:
        std::string actionName;
        std::string actionQualifier;

        // Wait:
        uint32_t waitMs = 0;

        // Phase 4 / 5 — generic item / spell / vendor / mail / quest fields.
        uint64_t itemGuid    = 0;     // sell/equip/unequip/destroy/use/trade_offer_item, mail_take_item
        uint32_t itemEntry   = 0;     // buy_item, mail_send (attach by entry)
        uint32_t quantity    = 1;     // buy/destroy stack count, mail_send count
        uint64_t vendorGuid  = 0;     // buy_item / sell_item
        uint32_t spellId     = 0;     // cast_spell, mount-spell
        uint32_t copper      = 0;     // trade_offer_money, mail_send money
        int32_t  intParam    = -1;    // gossip option index, trade slot, equip dest slot, ready_check choice
        uint32_t questId     = 0;     // quest_*
        uint64_t mailId      = 0;     // mail_take_*

        // Mail / chat-style strings (recipient name, subject, body).
        std::string strParam1;        // mail recipient
        std::string strParam2;        // mail subject
        std::string strParam3;        // mail body
    };
}

#endif
