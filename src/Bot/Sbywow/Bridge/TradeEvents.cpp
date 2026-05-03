/*
 * sbywow Agent Bridge — Trade event emit implementation.
 *
 * The function-pointer slots declared in
 * src/server/game/Sbywow/SbywowBridgeShim.h are populated here at
 * bridge startup via RegisterTradeEmitters(). Game-side splices
 * (TradeHandler.cpp, tagged `// [sbywow-bridge]`) call the inline
 * shim wrappers, which dispatch to these *_Impl functions when
 * the bridge is loaded.
 *
 * See TradeEvents.h for the design rationale and the
 * decisions.md "Phase 4 trade events" entry for the architecture.
 */

#include "TradeEvents.h"

#include "BridgeServer.h"
#include "BotSession.h"
#include "deps/json.hpp"

#include "Item.h"
#include "ItemTemplate.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Playerbots.h"
#include "PlayerbotMgr.h"
#include "Sbywow/SbywowBridgeShim.h"
#include "SharedDefines.h"
#include "TradeData.h"
#include "WorldSession.h"

using json = nlohmann::json;

namespace
{
    // Forward declarations — money_offered dedup helpers live below
    // EmitTradeStatus_Impl but are called from within it on lifecycle
    // boundaries.
    void ClearMoneyDedup(Player* viewer);

    // Attached-bot predicate, mirrors the BridgeHooks helper.
    bool IsAttachedBot(Player* p)
    {
        return p && sPlayerbotsMgr.GetPlayerbotAI(p) != nullptr;
    }

    json BaseEvent(Player* bot, char const* channel, char const* kind)
    {
        return {
            {"channel",  channel},
            {"kind",     kind},
            {"bot_guid", bot->GetGUID().GetRawValue()},
            {"bot_name", bot->GetName()}
        };
    }

    void Push(Player* bot, json const& payload)
    {
        auto session = Sbywow::Bridge::BridgeServer::Instance().GetSession(bot->GetGUID());
        if (!session)
            return;
        session->PushOutbound(payload.dump());
    }

    // Map TradeStatus enum to event kind. Returns nullptr for statuses
    // that don't carry agent-relevant lifecycle meaning (we still let
    // the original SendTradeStatus packet ship; we just don't emit).
    char const* StatusToKind(uint32 status)
    {
        switch (status)
        {
            case TRADE_STATUS_BEGIN_TRADE:    return "requested";
            case TRADE_STATUS_OPEN_WINDOW:    return "opened";
            case TRADE_STATUS_TRADE_ACCEPT:   return "accepted";
            case TRADE_STATUS_TRADE_COMPLETE: return "completed";
            case TRADE_STATUS_TRADE_CANCELED: return "cancelled";
            case TRADE_STATUS_CLOSE_WINDOW:   return "cancelled";  // close-on-error → cancelled
            // Failures: the agent benefits from learning *why* the
            // request didn't open a window (target dead, too far, etc).
            case TRADE_STATUS_BUSY:
            case TRADE_STATUS_BUSY_2:
            case TRADE_STATUS_NO_TARGET:
            case TRADE_STATUS_TARGET_TO_FAR:
            case TRADE_STATUS_WRONG_FACTION:
            case TRADE_STATUS_TARGET_DEAD:
            case TRADE_STATUS_TARGET_STUNNED:
            case TRADE_STATUS_TARGET_LOGOUT:
            case TRADE_STATUS_YOU_DEAD:
            case TRADE_STATUS_YOU_STUNNED:
            case TRADE_STATUS_YOU_LOGOUT:
            case TRADE_STATUS_TRADE_REJECTED:
            case TRADE_STATUS_IGNORE_YOU:
            case TRADE_STATUS_WRONG_REALM:
            case TRADE_STATUS_TRIAL_ACCOUNT:
            case TRADE_STATUS_NOT_ON_TAPLIST:
                return "failed";
            default:
                return nullptr;
        }
    }

    char const* StatusToReasonStr(uint32 status)
    {
        switch (status)
        {
            case TRADE_STATUS_BUSY:           return "busy";
            case TRADE_STATUS_BUSY_2:         return "busy";
            case TRADE_STATUS_NO_TARGET:      return "no_target";
            case TRADE_STATUS_TARGET_TO_FAR:  return "target_too_far";
            case TRADE_STATUS_WRONG_FACTION:  return "wrong_faction";
            case TRADE_STATUS_TARGET_DEAD:    return "target_dead";
            case TRADE_STATUS_TARGET_STUNNED: return "target_stunned";
            case TRADE_STATUS_TARGET_LOGOUT:  return "target_logout";
            case TRADE_STATUS_YOU_DEAD:       return "you_dead";
            case TRADE_STATUS_YOU_STUNNED:    return "you_stunned";
            case TRADE_STATUS_YOU_LOGOUT:     return "you_logout";
            case TRADE_STATUS_TRADE_REJECTED: return "rejected";
            case TRADE_STATUS_IGNORE_YOU:     return "ignored";
            case TRADE_STATUS_WRONG_REALM:    return "wrong_realm";
            case TRADE_STATUS_TRIAL_ACCOUNT:  return "trial_account";
            case TRADE_STATUS_NOT_ON_TAPLIST: return "not_on_taplist";
            case TRADE_STATUS_CLOSE_WINDOW:   return "close_error";
            default:                          return "unknown";
        }
    }

    Player* PartnerOf(Player* p)
    {
        if (!p)
            return nullptr;
        TradeData* td = p->GetTradeData();
        return td ? td->GetTrader() : nullptr;
    }

    // ---- Impl functions registered into the game-side shim slots. ----

    void EmitTradeStatus_Impl(Player* viewer, TradeStatusInfo const& info)
    {
        if (!IsAttachedBot(viewer))
            return;
        char const* kind = StatusToKind(info.Status);
        if (!kind)
            return;

        // Reset money_offered dedup at trade lifecycle boundaries —
        // a fresh trade window should always emit the partner's first
        // money_offered (typically 0 from the UI handshake), and after
        // a cancel/complete the next trade is a fresh story.
        std::string kindStr = kind;
        if (kindStr == "opened" || kindStr == "cancelled" ||
            kindStr == "completed" || kindStr == "failed")
            ClearMoneyDedup(viewer);

        json ev = BaseEvent(viewer, "trade", kind);
        ev["status_code"] = static_cast<int>(info.Status);

        Player* partnerPtr = nullptr;
        if (info.Status == TRADE_STATUS_BEGIN_TRADE && info.TraderGuid)
        {
            ev["partner_guid"] = info.TraderGuid.GetRawValue();
            if (Player* partner = ObjectAccessor::FindPlayer(info.TraderGuid))
            {
                ev["partner_name"] = partner->GetName();
                partnerPtr = partner;
            }
        }
        else if (Player* partner = PartnerOf(viewer))
        {
            ev["partner_guid"] = partner->GetGUID().GetRawValue();
            ev["partner_name"] = partner->GetName();
            partnerPtr = partner;
        }

        if (std::string(kind) == "failed")
            ev["reason"] = StatusToReasonStr(info.Status);

        // cancelled doesn't carry a server-side reason — TRADE_STATUS_
        // TRADE_CANCELED is fired by both client UI close and explicit
        // cancel and the auto-faction-rejection path. Surface a
        // cross_faction hint when we can confirm it; the agent's most
        // common "trade keeps cancelling" mystery has historically been
        // the WoW 3.3.5 client UI auto-rejecting opposing-faction trades.
        // Other cancel reasons (movement, range, explicit cancel) need
        // pre-cancel state tracking — deferred.
        if (std::string(kind) == "cancelled" && partnerPtr &&
            viewer->GetTeamId() != partnerPtr->GetTeamId())
        {
            ev["cross_faction"] = true;
            ev["hint"]          = "client_ui_auto_rejected_cross_faction";
        }

        Push(viewer, ev);
    }

    void EmitTradeItemSet_Impl(Player* actor, uint8 tradeSlot, Item* item)
    {
        if (!actor || !item)
            return;

        ItemTemplate const* tpl = item->GetTemplate();
        uint32 entry = tpl ? tpl->ItemId : 0;
        std::string name = tpl ? tpl->Name1 : std::string{};
        uint32 count = item->GetCount();

        auto fire = [&](Player* viewer, char const* perspective)
        {
            if (!IsAttachedBot(viewer))
                return;
            json ev = BaseEvent(viewer, "trade", "item_offered");
            ev["source"]     = perspective;
            ev["trade_slot"] = static_cast<int>(tradeSlot);
            ev["entry"]      = entry;
            ev["name"]       = name;
            ev["count"]      = count;
            ev["item_guid"]  = item->GetGUID().GetRawValue();
            Push(viewer, ev);
        };

        fire(actor, "self");
        if (Player* partner = PartnerOf(actor))
            fire(partner, "partner");
    }

    void EmitTradeItemCleared_Impl(Player* actor, uint8 tradeSlot)
    {
        if (!actor)
            return;

        auto fire = [&](Player* viewer, char const* perspective)
        {
            if (!IsAttachedBot(viewer))
                return;
            json ev = BaseEvent(viewer, "trade", "item_cleared");
            ev["source"]     = perspective;
            ev["trade_slot"] = static_cast<int>(tradeSlot);
            Push(viewer, ev);
        };

        fire(actor, "self");
        if (Player* partner = PartnerOf(actor))
            fire(partner, "partner");
    }

    // Per-viewer dedup state for money_offered events. The WoW client
    // UI fires CMSG_SET_TRADE_GOLD as a handshake during trade-window
    // initialization (sometimes 4-6 times in rapid succession with the
    // same value, usually 0), which inflates the SSE rate without
    // adding signal. Suppress consecutive identical (source, copper)
    // events per viewer; reset on trade.opened / trade.cancelled /
    // trade.completed in EmitTradeStatus_Impl. World-thread only — no
    // lock needed.
    struct LastMoney { std::string source; uint32 copper; bool valid = false; };
    std::unordered_map<uint64_t /*viewer raw guid*/, LastMoney> g_lastMoneyOffered;

    void ClearMoneyDedup(Player* viewer)
    {
        if (!viewer) return;
        g_lastMoneyOffered.erase(viewer->GetGUID().GetRawValue());
    }

    void EmitTradeMoneySet_Impl(Player* actor, uint32 copper)
    {
        if (!actor)
            return;

        auto fire = [&](Player* viewer, char const* perspective)
        {
            if (!IsAttachedBot(viewer))
                return;
            // Dedup: identical-to-last (source, copper) is suppressed.
            auto& last = g_lastMoneyOffered[viewer->GetGUID().GetRawValue()];
            if (last.valid && last.source == perspective && last.copper == copper)
                return;
            last.source = perspective;
            last.copper = copper;
            last.valid  = true;
            json ev = BaseEvent(viewer, "trade", "money_offered");
            ev["source"] = perspective;
            ev["copper"] = copper;
            Push(viewer, ev);
        };

        fire(actor, "self");
        if (Player* partner = PartnerOf(actor))
            fire(partner, "partner");
    }
}

namespace Sbywow::Bridge
{
    void RegisterTradeEmitters()
    {
        g_emitTradeStatus      = &EmitTradeStatus_Impl;
        g_emitTradeItemSet     = &EmitTradeItemSet_Impl;
        g_emitTradeItemCleared = &EmitTradeItemCleared_Impl;
        g_emitTradeMoneySet    = &EmitTradeMoneySet_Impl;
    }
}
