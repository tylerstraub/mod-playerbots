#include "BridgeServer.h"

#include "BotSession.h"
#include "TradeEvents.h"

#include "../AgentEngine/IsAgentBonded.h"
#include "../AgentEngine/SbywowAgentEngine.h"
#include "../MercenaryMgr.h"

#include "AiObjectContext.h"
#include "Bag.h"
#include "Cell.h"
#include "CellImpl.h"
#include "Config.h"
#include "Creature.h"
#include "Engine.h"
#include "GameObject.h"
#include "GossipDef.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Playerbots.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Timer.h"
#include "TradeData.h"
#include "Value.h"
#include "WorldPacket.h"
#include "WorldSession.h"

// Vendored single-header deps. Kept local to the bridge so we don't take
// on a cross-module dependency on mod-playerbots-characters' deps tree.
#include "deps/httplib.h"
#include "deps/json.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <regex>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

using json = nlohmann::json;

namespace Sbywow::Bridge
{
    BridgeServer& BridgeServer::Instance()
    {
        static BridgeServer instance;
        return instance;
    }

    BridgeServer::~BridgeServer() { Stop(); }

    // -----------------------------------------------------------------------
    // Config (read once at Start; reload requires restart of the world)
    // -----------------------------------------------------------------------

    void BridgeServer::LoadConfig()
    {
        config_.enable             = sConfigMgr->GetOption<bool>       ("Sbywow.Bridge.Enable",             true);
        // NOTE: Bind to 0.0.0.0 inside the container, not 127.0.0.1.
        // Podman's port-forward enters the container on a non-loopback
        // interface; binding to 127.0.0.1 inside accepts the connection
        // (the listening socket is reachable) but resets the request.
        // Host-side localhost-only is enforced by `-p 127.0.0.1:8889:8889`
        // in bin/ac. PBC's HTTP server uses the same pattern.
        config_.host               = sConfigMgr->GetOption<std::string>("Sbywow.Bridge.Host",               "0.0.0.0");
        config_.port               = sConfigMgr->GetOption<int32>      ("Sbywow.Bridge.Port",               8889);
        config_.secret             = sConfigMgr->GetOption<std::string>("Sbywow.Bridge.Secret",             "");
        config_.heartbeatTimeoutMs = sConfigMgr->GetOption<int32>      ("Sbywow.Bridge.HeartbeatTimeoutMs", 30000);
        config_.snapshotEveryNUpdates =
            static_cast<uint32>(sConfigMgr->GetOption<int32>("Sbywow.Bridge.SnapshotEveryNUpdates", 500));
    }

    bool BridgeServer::CheckAuth(std::string const& authHeader) const
    {
        if (config_.secret.empty())
            return true;                          // auth disabled

        // Expected format: "Bearer <secret>".
        std::string const prefix = "Bearer ";
        if (authHeader.size() <= prefix.size() ||
            authHeader.compare(0, prefix.size(), prefix) != 0)
            return false;

        return authHeader.substr(prefix.size()) == config_.secret;
    }

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    void BridgeServer::Start()
    {
        if (running_.load())
            return;

        LoadConfig();

        if (!config_.enable)
        {
            LOG_INFO("server.loading", "[SbywowBridge] disabled by config");
            return;
        }

        server_ = std::make_unique<httplib::Server>();
        RegisterRoutes();

        // Register fan-out emitters that need to be called from game-side
        // splices (game can't reference module symbols at link time;
        // see src/server/game/Sbywow/SbywowBridgeShim.h).
        RegisterTradeEmitters();

        std::string host = config_.host;
        int         port = config_.port;

        serverThread_ = std::thread([this, host, port]()
        {
            LOG_INFO("server.loading", "[SbywowBridge] listening on {}:{}", host, port);
            if (!server_->listen(host, port))
                LOG_ERROR("server.loading", "[SbywowBridge] listen failed on {}:{}", host, port);
        });

        running_.store(true);
    }

    void BridgeServer::Stop()
    {
        if (!running_.load())
            return;

        if (server_)
            server_->stop();

        if (serverThread_.joinable())
            serverThread_.join();

        server_.reset();
        running_.store(false);

        std::lock_guard<std::mutex> lock(sessionsMutex_);
        sessions_.clear();
    }

    // -----------------------------------------------------------------------
    // Session management (called on the world thread)
    // -----------------------------------------------------------------------

    bool BridgeServer::AttachBot(Player* bot)
    {
        if (!bot || !running_.load())
            return false;

        {
            std::lock_guard<std::mutex> lock(sessionsMutex_);
            uint64 key = bot->GetGUID().GetRawValue();
            if (sessions_.find(key) != sessions_.end())
                return false;

            auto session = std::make_shared<BotSession>(bot->GetGUID());
            session->SetName(bot->GetName());
            sessions_[key] = session;
            LOG_INFO("server.loading", "[SbywowBridge] attached bot guid={} name={}",
                     key, bot->GetName());
        }

        // Default state on attach: agent_mode=false. The bot's WoW
        // AFK marker reflects "agent not driving" so other players
        // see [AFK] from the moment the merc spawns until the agent
        // harness opts in via set_agent_mode true. The session flag
        // already defaults to false; just set the visible side-
        // effect (toggle / autoReplyMsg) — there's no transition
        // event since this is the initial state, not a change.
        if (!bot->isAFK())
        {
            bot->ToggleAFK();
            bot->autoReplyMsg = "Agent not actively driving — running default merc behavior";
        }
        return true;
    }

    void BridgeServer::DetachBot(ObjectGuid guid)
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        auto it = sessions_.find(guid.GetRawValue());
        if (it == sessions_.end())
            return;

        // Don't erase under callers; let in-flight handlers finish via the
        // shared_ptr they already hold. Erasing from the map is enough.
        sessions_.erase(it);
        LOG_INFO("server.loading", "[SbywowBridge] detached bot guid={}", guid.GetRawValue());
    }

    std::shared_ptr<BotSession> BridgeServer::GetSession(ObjectGuid guid)
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        auto it = sessions_.find(guid.GetRawValue());
        if (it == sessions_.end())
            return nullptr;
        return it->second;
    }

    void BridgeServer::ForEachAttachedBot(std::function<void(Player*)> const& fn)
    {
        // Snapshot guids under the lock, then resolve outside — keeps
        // the lock window short and avoids reentering BridgeServer from
        // within a chat-hook callback.
        std::vector<uint64> guids;
        {
            std::lock_guard<std::mutex> lock(sessionsMutex_);
            guids.reserve(sessions_.size());
            for (auto const& [raw, _] : sessions_)
                guids.push_back(raw);
        }
        for (uint64 raw : guids)
        {
            if (Player* bot = ObjectAccessor::FindPlayer(ObjectGuid(raw)))
                fn(bot);
        }
    }

    bool BridgeServer::ApplyAgentModeToggle(Player* bot, bool desired, std::string const& source)
    {
        if (!bot)
            return false;
        auto session = GetSession(bot->GetGUID());
        if (!session)
            return false;

        bool prev = session->IsAgentMode();
        if (prev == desired)
            return prev;  // no-change short-circuit

        session->SetAgentMode(desired);

        // Mirror to the WoW AFK marker for player-visibility. Other
        // players see [AFK] on the bot when agent_mode is false (the
        // default state on attach), so they know the agent isn't
        // driving even if the bot is otherwise active under default
        // behavior. autoReplyMsg conveys the custom semantic to
        // anyone whispering. AFK on agent-bonded bots is owned
        // exclusively by us — see PlayerbotAI.cpp / AcceptInvitation
        // gates against IsAgentBonded.
        if (!desired && !bot->isAFK())
        {
            bot->ToggleAFK();
            bot->autoReplyMsg = "Agent not actively driving — running default merc behavior";
        }
        else if (desired && bot->isAFK())
        {
            bot->ToggleAFK();
            bot->autoReplyMsg = "";
        }

        // Lifecycle SSE: harness consumers can correlate transitions
        // and surface them to the master/operator.
        json ev = {
            {"channel",    "lifecycle"},
            {"kind",       desired ? "agent_mode_entered" : "agent_mode_exited"},
            {"bot_guid",   bot->GetGUID().GetRawValue()},
            {"bot_name",   bot->GetName()},
            {"source",     source},
            {"agent_mode", desired}
        };
        session->PushOutbound(ev.dump());

        LOG_INFO("playerbots",
                 "[SbywowBridge] agent_mode toggle: bot={} mode={} source={}",
                 bot->GetName().c_str(), desired ? "on" : "off", source.c_str());
        return prev;
    }

    // -----------------------------------------------------------------------
    // Context snapshot — single source of truth for the agent harness's
    // wake context. Same payload feeds (a) the periodic `snapshot.state`
    // SSE event, (b) the `get_context` sync verb, (c) the `inspect`
    // verb's embedded `context` field. All callers run on the world
    // thread; the function reads Player*, master Player*, and engine
    // state directly. See decisions.md "Tool surface is for actions
    // and deep discovery" (2026-05-02) for the architectural reasoning.
    // Schema documented in docs/agent-interface.md "Context snapshot
    // block" — keep them in sync.
    // -----------------------------------------------------------------------

    namespace
    {
        // Static name table for the equipped block. Indexed by
        // EQUIPMENT_SLOT_HEAD (0) .. EQUIPMENT_SLOT_TABARD (18).
        char const* kEquipSlotName[EQUIPMENT_SLOT_END] = {
            "head", "neck", "shoulders", "shirt", "chest", "waist",
            "legs", "feet", "wrists", "hands", "finger1", "finger2",
            "trinket1", "trinket2", "back", "main_hand", "off_hand",
            "ranged", "tabard"
        };

        char const* QualityName(uint32 q)
        {
            switch (q)
            {
                case ITEM_QUALITY_POOR:      return "poor";
                case ITEM_QUALITY_NORMAL:    return "normal";
                case ITEM_QUALITY_UNCOMMON:  return "uncommon";
                case ITEM_QUALITY_RARE:      return "rare";
                case ITEM_QUALITY_EPIC:      return "epic";
                case ITEM_QUALITY_LEGENDARY: return "legendary";
                case ITEM_QUALITY_ARTIFACT:  return "artifact";
                case ITEM_QUALITY_HEIRLOOM:  return "heirloom";
                default:                     return "unknown";
            }
        }

        char const* PowerName(Powers p)
        {
            switch (p)
            {
                case POWER_MANA:        return "mana";
                case POWER_RAGE:        return "rage";
                case POWER_FOCUS:       return "focus";
                case POWER_ENERGY:      return "energy";
                case POWER_HAPPINESS:   return "happiness";
                case POWER_RUNE:        return "rune";
                case POWER_RUNIC_POWER: return "runic_power";
                default:                return "unknown";
            }
        }

        // Build a {power_type, power, power_max, power_pct} sub-object for
        // a Unit. Reflects the unit's *current* primary power — for shifted
        // druids this swings (rage in bear form, energy in cat form).
        json BuildPowerBlock(Unit const* u)
        {
            Powers pt = u->getPowerType();
            uint32 cur = u->GetPower(pt);
            uint32 mx  = u->GetMaxPower(pt);
            return {
                {"power_type", PowerName(pt)},
                {"power",      cur},
                {"power_max",  mx},
                {"power_pct",  mx ? static_cast<int>(100ULL * cur / mx) : 0}
            };
        }

        // Quality rank for sorting "other" items by quality desc. Lower
        // index = higher rank.
        int QualityRank(uint32 q)
        {
            switch (q)
            {
                case ITEM_QUALITY_LEGENDARY: return 0;
                case ITEM_QUALITY_EPIC:      return 1;
                case ITEM_QUALITY_RARE:      return 2;
                case ITEM_QUALITY_UNCOMMON:  return 3;
                case ITEM_QUALITY_NORMAL:    return 4;
                case ITEM_QUALITY_POOR:      return 5;
                case ITEM_QUALITY_HEIRLOOM:  return 6;
                case ITEM_QUALITY_ARTIFACT:  return 7;
                default:                     return 8;
            }
        }

        char const* ConsumableBucket(ItemTemplate const* tpl)
        {
            if (!tpl || tpl->Class != ITEM_CLASS_CONSUMABLE) return nullptr;
            switch (tpl->SubClass)
            {
                case ITEM_SUBCLASS_POTION:           return "potion";
                case ITEM_SUBCLASS_ELIXIR:           return "elixir";
                case ITEM_SUBCLASS_FLASK:            return "flask";
                case ITEM_SUBCLASS_SCROLL:           return "scroll";
                case ITEM_SUBCLASS_FOOD:             return "food_drink";
                case ITEM_SUBCLASS_BANDAGE:          return "bandage";
                case ITEM_SUBCLASS_ITEM_ENHANCEMENT: return "item_enhancement";
                default:                             return "consumable_other";
            }
        }

        // Walk every non-equipped, non-bank slot and call fn(Item*).
        // Equipment slots are intentionally NOT included here — those are
        // surfaced separately as `equipped` per-slot. Bank items are
        // never relevant to in-world bot behavior, so they're excluded
        // unconditionally.
        template <typename Fn>
        void ForEachNonEquippedItem(Player* bot, Fn const& fn)
        {
            // Backpack (slots 23..38).
            for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
                if (Item* it = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                    fn(it);

            // Keyring + currency (slots 86..149). Cheap; usually empty.
            for (uint8 i = KEYRING_SLOT_START; i < CURRENCYTOKEN_SLOT_END; ++i)
                if (Item* it = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                    fn(it);

            // Bag slots (19..22) — recurse into each Bag.
            for (uint8 i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
            {
                Bag* pBag = bot->GetBagByPos(i);
                if (!pBag) continue;
                for (uint32 j = 0; j < pBag->GetBagSize(); ++j)
                    if (Item* it = bot->GetItemByPos(i, j))
                        fn(it);
            }
        }

        // Build a {guid, name, kind, hostile, hp_pct, alive} target sub-block
        // for a Unit (the bot or its master). Returns null when the unit
        // has no target or the target can't be resolved.
        json BuildTargetBlock(Unit* viewer)
        {
            if (!viewer)
                return nullptr;
            ObjectGuid tgtGuid = viewer->GetTarget();
            if (!tgtGuid)
                return nullptr;
            Unit* tgt = ObjectAccessor::GetUnit(*viewer, tgtGuid);
            if (!tgt)
                return nullptr;
            char const* kind = "unit";
            if (tgt->ToCreature())    kind = "creature";
            else if (tgt->ToPlayer()) kind = "player";
            return json{
                {"guid",    tgtGuid.GetRawValue()},
                {"name",    tgt->GetName()},
                {"kind",    kind},
                {"hostile", tgt->IsHostileTo(viewer)},
                {"hp_pct",  static_cast<int>(tgt->GetHealthPct())},
                {"alive",   tgt->IsAlive()}
            };
        }

        // Bot's own auras — positive non-passive only, deduped by spell_id
        // (AuraApplicationMap is a multimap keyed per-effect; one Aura with
        // three effects contributes three entries). Capped at kAuraCap to
        // bound payload during heavy buff windows. Permanent buffs report
        // remaining_ms = -1 so the agent can distinguish "always-up" from
        // a 5-minute Fortitude that's about to fall off.
        json BuildAurasBlock(Player* bot)
        {
            constexpr size_t kAuraCap = 20;
            json arr = json::array();
            std::set<uint32> seen;
            for (auto const& [spellId, aurApp] : bot->GetAppliedAuras())
            {
                if (!aurApp || !aurApp->IsPositive())
                    continue;
                Aura* base = aurApp->GetBase();
                if (!base || base->IsPassive())
                    continue;
                if (!seen.insert(spellId).second)
                    continue;
                SpellInfo const* si = base->GetSpellInfo();
                if (!si)
                    continue;
                json one = {
                    {"spell_id",     spellId},
                    {"name",         si->SpellName[0] ? si->SpellName[0] : ""},
                    {"stacks",       static_cast<int>(base->GetStackAmount())},
                    {"remaining_ms", base->IsPermanent() ? -1 : base->GetDuration()}
                };
                arr.push_back(std::move(one));
                if (arr.size() >= kAuraCap)
                    break;
            }
            return arr;
        }

        // Bot's spell cooldowns longer than kCooldownThresholdMs remaining.
        // Filters out the GCD chatter (1.5s rotation cooldowns aren't useful
        // for executive function planning); long CDs (Fade, Power Infusion,
        // Bestial Wrath, etc.) are what the agent times.
        json BuildCooldownsBlock(Player* bot)
        {
            constexpr uint32 kCooldownThresholdMs = 5000;
            constexpr size_t kCooldownCap = 30;
            json arr = json::array();
            uint32 now = getMSTime();
            for (auto const& [spellId, cd] : bot->GetSpellCooldownMap())
            {
                if (cd.end <= now)
                    continue;
                uint32 remaining = cd.end - now;
                if (remaining < kCooldownThresholdMs)
                    continue;
                SpellInfo const* si = sSpellMgr->GetSpellInfo(spellId);
                if (!si)
                    continue;
                arr.push_back(json{
                    {"spell_id",     spellId},
                    {"name",         si->SpellName[0] ? si->SpellName[0] : ""},
                    {"remaining_ms", remaining}
                });
                if (arr.size() >= kCooldownCap)
                    break;
            }
            return arr;
        }

        // Trade window snapshot. Returns null when no trade window is
        // open. Reads bot->GetSession()->GetTradeData() for self/partner
        // offers, accepted flags, partner identity. Cheap — at most 14
        // item lookups (7 slots × 2 sides) when a window is open, zero
        // when not.
        json BuildTradeBlock(Player* bot)
        {
            if (!bot)
                return nullptr;
            TradeData* my = bot->GetTradeData();
            if (!my)
                return nullptr;
            Player* partner = my->GetTrader();
            TradeData* their = my->GetTraderData();

            auto offerJson = [](TradeData const* td) -> json
            {
                json items = json::array();
                if (!td)
                    return json{{"items", items}, {"money", 0}, {"accepted", false}};
                for (uint8 i = 0; i < TRADE_SLOT_COUNT; ++i)
                {
                    Item* it = td->GetItem(TradeSlots(i));
                    if (!it)
                        continue;
                    ItemTemplate const* tpl = it->GetTemplate();
                    if (!tpl)
                        continue;
                    items.push_back({
                        {"trade_slot", static_cast<int>(i)},
                        {"entry",      tpl->ItemId},
                        {"name",       tpl->Name1},
                        {"count",      it->GetCount()},
                        {"item_guid",  it->GetGUID().GetRawValue()}
                    });
                }
                return json{
                    {"items",    items},
                    {"money",    td->GetMoney()},
                    {"accepted", td->IsAccepted()}
                };
            };

            json out = {
                {"partner_guid", partner ? partner->GetGUID().GetRawValue() : 0},
                {"partner_name", partner ? partner->GetName() : std::string{}},
                {"my_offer",     offerJson(my)},
                {"their_offer",  offerJson(their)}
            };
            return out;
        }

        // Gossip menu snapshot. Returns null when no menu is open.
        // Reads bot->PlayerTalkClass->GetGossipMenu() for the option list.
        // Note: PlayerTalkClass is constructed at Player init and survives
        // until destruction, so the pointer is always valid on a live bot;
        // emptiness is detected by GossipMenu::Empty().
        json BuildGossipBlock(Player* bot)
        {
            if (!bot || !bot->PlayerTalkClass)
                return nullptr;
            GossipMenu& menu = bot->PlayerTalkClass->GetGossipMenu();
            if (menu.Empty())
                return nullptr;
            json options = json::array();
            for (auto const& [idx, item] : menu.GetMenuItems())
            {
                options.push_back({
                    {"index",       idx},
                    {"icon",        static_cast<int>(item.MenuItemIcon)},
                    {"option_type", item.OptionType},
                    {"text",        item.Message},
                    {"is_coded",    item.IsCoded},
                    {"box_money",   item.BoxMoney}
                });
            }
            ObjectGuid sender = menu.GetSenderGUID();
            return json{
                {"menu_id",     menu.GetMenuId()},
                {"sender_guid", sender.GetRawValue()},
                {"options",     options}
            };
        }

        // Count used vs. total non-equipped, non-keyring slots:
        // backpack 16 + each bag's GetBagSize.
        void CountInventorySlots(Player* bot, int& used, int& total)
        {
            used = 0;
            total = INVENTORY_SLOT_ITEM_END - INVENTORY_SLOT_ITEM_START;  // 16

            for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
                if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                    ++used;

            for (uint8 i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
            {
                Bag* pBag = bot->GetBagByPos(i);
                if (!pBag) continue;
                int sz = static_cast<int>(pBag->GetBagSize());
                total += sz;
                for (int j = 0; j < sz; ++j)
                    if (bot->GetItemByPos(i, static_cast<uint8>(j)))
                        ++used;
            }
        }
    }

    nlohmann::json BuildContextSnapshot(Player* bot, BotSession& sess)
    {
        if (!bot)
            return json::object();

        PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot);

        // ---- self ----
        uint32 health    = bot->GetHealth();
        uint32 healthMax = bot->GetMaxHealth();
        json power       = BuildPowerBlock(bot);
        bool   resting   = bot->HasRestFlag(REST_FLAG_IN_TAVERN) ||
                           bot->HasRestFlag(REST_FLAG_IN_CITY)   ||
                           bot->HasRestFlag(REST_FLAG_IN_FACTION_AREA);
        uint32 xpCur     = bot->GetUInt32Value(PLAYER_XP);
        uint32 xpNext    = bot->GetUInt32Value(PLAYER_NEXT_LEVEL_XP);
        json self = {
            {"name",       bot->GetName()},
            {"level",      bot->GetLevel()},
            {"class_id",   static_cast<int>(bot->getClass())},
            {"race_id",    static_cast<int>(bot->getRace())},
            {"hp",         health},
            {"hp_max",     healthMax},
            {"hp_pct",     healthMax ? static_cast<int>(100ULL * health / healthMax) : 0},
            {"power_type", power["power_type"]},
            {"power",      power["power"]},
            {"power_max",  power["power_max"]},
            {"power_pct",  power["power_pct"]},
            {"position",   {bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()}},
            {"facing",     bot->GetOrientation()},
            {"map",        bot->GetMapId()},
            {"zone",       bot->GetZoneId()},
            {"area",       bot->GetAreaId()},
            {"alive",      bot->IsAlive()},
            {"in_combat",  bot->IsInCombat()},
            {"mounted",    bot->IsMounted()},
            {"afk",        bot->isAFK()},
            {"resting",    resting},
            {"rest_bonus", bot->GetRestBonus()},
            {"xp",         xpCur},
            {"xp_next",    xpNext},
            {"guid",       bot->GetGUID().GetRawValue()},
            {"target",     BuildTargetBlock(bot)},
            {"auras",      BuildAurasBlock(bot)},
            {"cooldowns",  BuildCooldownsBlock(bot)}
        };

        // ---- master ----
        Player* master = ai ? ai->GetMaster() : nullptr;
        json masterJson = nullptr;
        if (master)
        {
            uint32 mh  = master->GetHealth();
            uint32 mhm = master->GetMaxHealth();
            json mpower = BuildPowerBlock(master);

            masterJson = {
                {"name",       master->GetName()},
                {"guid",       master->GetGUID().GetRawValue()},
                {"level",      master->GetLevel()},
                {"class_id",   static_cast<int>(master->getClass())},
                {"race_id",    static_cast<int>(master->getRace())},
                {"hp",         mh},
                {"hp_max",     mhm},
                {"hp_pct",     mhm ? static_cast<int>(100ULL * mh / mhm) : 0},
                {"power_type", mpower["power_type"]},
                {"power_pct",  mpower["power_pct"]},
                {"position",   {master->GetPositionX(), master->GetPositionY(), master->GetPositionZ()}},
                {"facing",     master->GetOrientation()},
                {"map",        master->GetMapId()},
                {"zone",       master->GetZoneId()},
                {"area",       master->GetAreaId()},
                {"distance",   bot->GetExactDist(master)},
                {"online",     true},   // master pointer non-null implies in-world
                {"alive",      master->IsAlive()},
                {"in_combat",  master->IsInCombat()},
                {"mounted",    master->IsMounted()},
                {"afk",        master->isAFK()},
                {"target",     BuildTargetBlock(master)}
            };
        }

        // ---- inventory ----
        uint32 money = bot->GetMoney();
        json inventory;
        {
            // Equipped per-slot (always inline; bounded at 19 entries).
            json equipped = json::object();
            for (uint8 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
            {
                Item* it = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i);
                if (!it) continue;
                ItemTemplate const* tpl = it->GetTemplate();
                if (!tpl) continue;
                equipped[kEquipSlotName[i]] = {
                    {"entry",     tpl->ItemId},
                    {"name",      tpl->Name1},
                    {"ilvl",      tpl->ItemLevel},
                    {"quality",   QualityName(tpl->Quality)},
                    {"item_guid", it->GetGUID().GetRawValue()}
                };
            }

            // Aggregate non-equipped items by item entry — packed bots
            // hold many stacks of the same trade-good / consumable, and
            // the agent doesn't need separate stack-count entries.
            //
            // Consumables are *partitioned out* into their own block; they
            // do NOT appear in stacked[] / quest_items / notable / other.
            // This avoids the LLM-trap where the same item appears twice
            // (once in the consumables rollup, once in `other`) and the
            // agent double-counts. A consumable's specific entry/name is
            // available in consumables.<bucket>.items.
            // Per-entry aggregates carry the first item_guid we saw,
            // letting the agent issue per-item verbs (sell/destroy/
            // equip/trade_offer) without a separate lookup. Multiple
            // physical stacks of the same entry collapse into one
            // record; the surfaced guid points at the head stack and
            // the entry-fallback pattern in those verbs picks any
            // matching item server-side anyway.
            struct Stack
            {
                ItemTemplate const* tpl       = nullptr;
                uint32              count     = 0;
                uint64_t            firstGuid = 0;
            };
            std::map<uint32, Stack>            stacked;

            // Consumables: per-bucket {total, items}. Items are aggregated
            // by entry across multiple stacks of the same item.
            struct ConsumableEntry
            {
                ItemTemplate const* tpl       = nullptr;
                uint32              count     = 0;
                uint64_t            firstGuid = 0;
            };
            std::map<std::string, std::map<uint32, ConsumableEntry>> consumablesByBucket;
            std::map<std::string, uint32>                            consumableTotals;

            ForEachNonEquippedItem(bot, [&](Item* it) {
                ItemTemplate const* tpl = it->GetTemplate();
                if (!tpl) return;
                uint32 cnt = it->GetCount();
                uint64_t guid = it->GetGUID().GetRawValue();
                if (char const* bucket = ConsumableBucket(tpl))
                {
                    auto& ce = consumablesByBucket[bucket][tpl->ItemId];
                    ce.tpl    = tpl;
                    ce.count += cnt;
                    if (ce.firstGuid == 0) ce.firstGuid = guid;
                    consumableTotals[bucket] += cnt;
                }
                else
                {
                    auto& s = stacked[tpl->ItemId];
                    s.tpl    = tpl;
                    s.count += cnt;
                    if (s.firstGuid == 0) s.firstGuid = guid;
                }
            });

            json questItems = json::array();
            json notable    = json::array();
            json other      = json::array();

            // Sort entries by quality desc / count desc within "other".
            // questItems and notable retain insertion order; the LLM
            // doesn't care, and stable-by-entry is fine for those.
            std::vector<Stack const*> otherSorted;
            otherSorted.reserve(stacked.size());

            for (auto const& [entry, st] : stacked)
            {
                ItemTemplate const* tpl = st.tpl;
                json one = {
                    {"entry",     tpl->ItemId},
                    {"name",      tpl->Name1},
                    {"count",     st.count},
                    {"quality",   QualityName(tpl->Quality)},
                    {"item_guid", st.firstGuid}
                };
                bool isQuest   = (tpl->Class == ITEM_CLASS_QUEST) || tpl->StartQuest != 0;
                bool isNotable = tpl->Quality >= ITEM_QUALITY_RARE;
                if (isQuest)
                    questItems.push_back(std::move(one));
                else if (isNotable)
                    notable.push_back(std::move(one));
                else
                    otherSorted.push_back(&st);
            }

            std::sort(otherSorted.begin(), otherSorted.end(),
                      [](Stack const* a, Stack const* b) {
                          int ra = QualityRank(a->tpl->Quality);
                          int rb = QualityRank(b->tpl->Quality);
                          if (ra != rb) return ra < rb;
                          return a->count > b->count;
                      });

            for (Stack const* st : otherSorted)
            {
                ItemTemplate const* tpl = st->tpl;
                other.push_back({
                    {"entry",     tpl->ItemId},
                    {"name",      tpl->Name1},
                    {"count",     st->count},
                    {"quality",   QualityName(tpl->Quality)},
                    {"item_guid", st->firstGuid}
                });
            }

            // Cap combined unique-entry count at kInventoryCap. quest+notable
            // are always kept; trim "other" only.
            constexpr size_t kInventoryCap = 50;
            bool truncated = false;
            size_t reserved = questItems.size() + notable.size();
            if (reserved >= kInventoryCap)
            {
                if (!other.empty())
                {
                    other = json::array();
                    truncated = true;
                }
            }
            else
            {
                size_t budget = kInventoryCap - reserved;
                if (other.size() > budget)
                {
                    json trimmed = json::array();
                    for (size_t i = 0; i < budget; ++i)
                        trimmed.push_back(std::move(other[i]));
                    other = std::move(trimmed);
                    truncated = true;
                }
            }

            int slotsUsed = 0, slotsTotal = 0;
            CountInventorySlots(bot, slotsUsed, slotsTotal);

            uint32 g = money / 10000;
            uint32 s = (money / 100) % 100;
            uint32 c = money % 100;

            // Render consumables: per-bucket {total, items: [{entry, name, count}]}.
            // Buckets included only if non-empty (sparse object). The
            // `low` flag is set on the regen-critical buckets (food_drink,
            // bandage) when their total drops below kConsumableLowThreshold;
            // it's the agent's "go restock" signal.
            constexpr uint32 kConsumableLowThreshold = 5;
            json consumablesJson = json::object();
            for (auto const& [bucket, total] : consumableTotals)
            {
                json items = json::array();
                auto bit = consumablesByBucket.find(bucket);
                if (bit != consumablesByBucket.end())
                {
                    for (auto const& [entry, ce] : bit->second)
                    {
                        items.push_back({
                            {"entry",     ce.tpl->ItemId},
                            {"name",      ce.tpl->Name1},
                            {"count",     ce.count},
                            {"item_guid", ce.firstGuid}
                        });
                    }
                }
                json bucketJson = {
                    {"total", total},
                    {"items", items}
                };
                bool isRegenBucket = (bucket == "food_drink" || bucket == "bandage");
                if (isRegenBucket && total < kConsumableLowThreshold)
                    bucketJson["low"] = true;
                consumablesJson[bucket] = std::move(bucketJson);
            }
            // Also surface "low: true" for regen buckets that are completely
            // absent — empty bucket = nothing in inventory at all, which is
            // the truly-low state.
            for (char const* regen : {"food_drink", "bandage"})
            {
                if (consumableTotals.find(regen) == consumableTotals.end())
                    consumablesJson[regen] = {{"total", 0}, {"items", json::array()}, {"low", true}};
            }

            inventory = {
                {"money",       {{"gold", g}, {"silver", s}, {"copper", c}, {"raw", money}}},
                {"slot_usage",  {{"used", slotsUsed}, {"total", slotsTotal}}},
                {"equipped",    equipped},
                {"consumables", consumablesJson},
                {"quest_items", questItems},
                {"notable",     notable},
                {"other",       other},
                {"truncated",   truncated}
            };
        }

        // ---- group ----
        json groupJson = nullptr;
        if (Group* group = bot->GetGroup())
        {
            json members = json::array();
            for (auto ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                if (Player* m = ref->GetSource())
                    members.push_back(m->GetName());
            }
            char const* lootMethod = "unknown";
            switch (group->GetLootMethod())
            {
                case FREE_FOR_ALL:      lootMethod = "free_for_all";      break;
                case ROUND_ROBIN:       lootMethod = "round_robin";       break;
                case MASTER_LOOT:       lootMethod = "master_loot";       break;
                case GROUP_LOOT:        lootMethod = "group_loot";        break;
                case NEED_BEFORE_GREED: lootMethod = "need_before_greed"; break;
            }
            std::string leaderName;
            if (char const* ln = group->GetLeaderName())
                leaderName = ln;
            groupJson = {
                {"leader_name", leaderName},
                {"members",     members},
                {"loot_method", lootMethod},
                {"is_raid",     group->isRaidGroup()},
                {"in_dungeon",  bot->GetMap() && bot->GetMap()->IsDungeon()}
            };
        }

        // ---- active_intents ----
        // Order: in-flight Wait suspension (head of execution) first,
        // then in-flight Move (multi-tick), then queued in queue order.
        // Wait + Move are the two intent kinds that persist across
        // ticks — Wait via suspension, Move via in-flight poll. The
        // sub-tick verbs (interact / say / do_action) terminate
        // inside the same tick they're popped, so they're never
        // visible as in-flight.
        json activeIntents = json::array();
        Sbywow::SbywowAgentEngine* agentEng = nullptr;
        if (ai)
            agentEng = dynamic_cast<Sbywow::SbywowAgentEngine*>(ai->GetEngine(BOT_STATE_NON_COMBAT));
        if (agentEng && agentEng->IsWaiting())
        {
            activeIntents.push_back({
                {"intent_id",         std::to_string(agentEng->WaitingIntentId())},
                {"verb",              agentEng->WaitingIntentVerb()},
                {"kind",              "wait"},
                {"status",            "waiting"},
                {"wait_remaining_ms", agentEng->WaitingRemainingMs()}
            });
        }
        if (agentEng && agentEng->IsMoving())
        {
            float dist = bot->GetExactDist2d(agentEng->MovingTargetX(),
                                              agentEng->MovingTargetY());
            activeIntents.push_back({
                {"intent_id",         std::to_string(agentEng->MovingIntentId())},
                {"verb",              agentEng->MovingIntentVerb()},
                {"kind",              "move"},
                {"status",            "in_flight"},
                {"target",            {agentEng->MovingTargetX(),
                                        agentEng->MovingTargetY(),
                                        agentEng->MovingTargetZ()}},
                {"distance_remaining", dist}
            });
        }
        if (agentEng && agentEng->IsCasting())
        {
            uint32 spellId = agentEng->CastingSpellId();
            json one = {
                {"intent_id",   std::to_string(agentEng->CastingIntentId())},
                {"verb",        agentEng->CastingIntentVerb()},
                {"kind",        "cast"},
                {"status",      "in_flight"},
                {"spell_id",    spellId}
            };
            if (Spell* live = bot->FindCurrentSpellBySpellId(spellId))
            {
                one["remaining_ms"] = live->GetCastTimeRemaining();
            }
            if (SpellInfo const* si = sSpellMgr->GetSpellInfo(spellId))
            {
                one["spell_name"] = si->SpellName[0] ? si->SpellName[0] : "";
            }
            activeIntents.push_back(std::move(one));
        }
        for (auto const& iv : sess.ProjectIntents())
        {
            activeIntents.push_back({
                {"intent_id", std::to_string(iv.intentId)},
                {"verb",      iv.verb},
                {"kind",      iv.kind},
                {"status",    "queued"}
            });
        }

        // ---- session ----
        uint64_t intentsTotal   = 0;
        uint64_t ticksTotal     = 0;
        uint64_t reactivesTotal = 0;
        if (agentEng)
        {
            intentsTotal   = agentEng->IntentsDispatchedTotal();
            ticksTotal     = agentEng->TicksTotal();
            reactivesTotal = agentEng->ReactivesFiredTotal();
        }
        // engine_state — what the agent's tick is doing right now.
        // Mirrors the routing tree at the top of
        // SbywowAgentEngine::DoNextAction: agent_mode off, in-combat
        // delegation, wait suspension, in-flight blocking slot, idle
        // follow delegation, anchored idle, or actively dispatching
        // from the queue. Lets the agent reason about "why aren't my
        // intents draining" without reading combat / mode / queue
        // separately and inferring.
        char const* engineState = "agent";
        if (!sess.IsAgentMode())
            engineState = "delegated_default_off";
        else if (bot->IsInCombat())
            engineState = "delegated_combat";
        else if (agentEng && agentEng->IsWaiting())
            engineState = "waiting";
        else if (agentEng && (agentEng->IsMoving() || agentEng->IsCasting()))
            engineState = "in_flight";
        else if (sess.PeekIntent())
            engineState = "agent";
        else if (sess.IsFollowMode())
            engineState = "delegated_idle_follow";
        else
            engineState = "idle_anchored";
        json session = {
            {"agent_mode",               sess.IsAgentMode()},
            {"follow_mode",              sess.IsFollowMode()},
            {"engine_state",             engineState},
            {"uptime_ms",                sess.UptimeMs()},
            {"heartbeat_ms",             sess.HeartbeatAgeMs()},
            {"sse_attached",             sess.IsSseAttached()},
            {"intents_dispatched_total", intentsTotal},
            {"ticks_total",              ticksTotal},
            {"reactives_fired_total",    reactivesTotal}
        };

        // ---- trade / gossip ----
        // Top-level fields populated by their respective wirings (trade
        // in Batch 2 from WorldSession::GetTradeData, gossip in Batch 4
        // from PlayerTalkClass). Null when no window is open. Schema
        // surface lands here up front so consumers can rely on the
        // fields existing.
        json tradeJson  = BuildTradeBlock(bot);
        json gossipJson = BuildGossipBlock(bot);

        return json{
            {"self",           self},
            {"master",         masterJson},
            {"inventory",      inventory},
            {"group",          groupJson},
            {"trade",          tradeJson},
            {"gossip",         gossipJson},
            {"active_intents", activeIntents},
            {"session",        session}
        };
    }

    // -----------------------------------------------------------------------
    // Command dispatch (runs on the world thread, called from TickBot)
    // -----------------------------------------------------------------------

    namespace
    {
        // Resolve string state name to BotState, or -1 for "all".
        // Returns -2 on invalid name. Default mapping if absent: non-combat.
        int ParseBotState(std::string const& s)
        {
            if (s.empty() || s == "non-combat" || s == "noncombat") return BOT_STATE_NON_COMBAT;
            if (s == "combat")                                       return BOT_STATE_COMBAT;
            if (s == "dead")                                         return BOT_STATE_DEAD;
            if (s == "all")                                          return -1;
            return -2;
        }

        char const* BotStateName(BotState s)
        {
            switch (s)
            {
                case BOT_STATE_COMBAT:     return "combat";
                case BOT_STATE_NON_COMBAT: return "non-combat";
                case BOT_STATE_DEAD:       return "dead";
                default:                   return "?";
            }
        }

        // For verbs that operate on engines: invoke `fn` on each engine
        // matching `state` (-1 = all). Returns the list of states it ran
        // against for diagnostics.
        json ForEachEngine(PlayerbotAI* ai, int state, std::function<void(Engine*, BotState)> const& fn)
        {
            json states = json::array();
            for (uint8 i = 0; i < BOT_STATE_MAX; ++i)
            {
                if (state >= 0 && i != static_cast<uint8>(state)) continue;
                if (Engine* e = ai->GetEngine(static_cast<BotState>(i)))
                {
                    fn(e, static_cast<BotState>(i));
                    states.push_back(BotStateName(static_cast<BotState>(i)));
                }
            }
            return states;
        }


        // ---- Diagnostic / debug instrument ------------------------
        //
        // Surfaces the structured context snapshot (same shape as the
        // periodic snapshot.state SSE event and the get_context verb)
        // plus debug-only extras: engine slot dispatch (which engine
        // subclass is installed in each state slot, strategy counts),
        // sbywow agent-bonded predicate, default-engine internals.
        // Use this when bot internals don't match expectations — the
        // operational state lives in `context`, the "why doesn't the
        // engine think what I think" lives in `engines` / `sbywow` /
        // `agent_engine_debug`.
        std::string DoInspect(Player* bot, BotSession& sess)
        {
            PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot);

            // Engine slot inspection — dynamic_cast detects whether
            // our SbywowAgentEngine is installed. Adding new engine
            // subclasses in the future: extend this dispatch.
            auto engineClass = [](Engine* e) -> std::string
            {
                if (!e) return "<null>";
                if (dynamic_cast<Sbywow::SbywowAgentEngine*>(e)) return "SbywowAgentEngine";
                return "Engine";
            };

            json engineInfo;
            json agentEngineDebug;  // populated only when non_combat is SbywowAgentEngine
            if (ai)
            {
                Engine* combat    = ai->GetEngine(BOT_STATE_COMBAT);
                Engine* nonCombat = ai->GetEngine(BOT_STATE_NON_COMBAT);
                Engine* dead      = ai->GetEngine(BOT_STATE_DEAD);

                engineInfo = {
                    {"combat", {
                        {"class",             engineClass(combat)},
                        {"strategies_count",  combat    ? static_cast<int>(combat->GetStrategies().size())    : 0}
                    }},
                    {"non_combat", {
                        {"class",             engineClass(nonCombat)},
                        {"strategies_count",  nonCombat ? static_cast<int>(nonCombat->GetStrategies().size()) : 0}
                    }},
                    {"dead", {
                        {"class",             engineClass(dead)},
                        {"strategies_count",  dead      ? static_cast<int>(dead->GetStrategies().size())      : 0}
                    }}
                };

                // The agent-engine "debug" block carries fields that are
                // NOT in the context snapshot (and shouldn't be — they're
                // internals, not operational state). Wait state and
                // counters are now sourced from context.session +
                // context.active_intents to avoid duplication.
                if (auto* agentEng = dynamic_cast<Sbywow::SbywowAgentEngine*>(nonCombat))
                {
                    agentEngineDebug = {
                        {"default_engine_strategies_count",  static_cast<int>(agentEng->DefaultEngineStrategiesCount())},
                        {"default_engine_ticks_total",       agentEng->DefaultEngineTicksTotal()}
                    };
                }
            }

            json sbywowInfo = {
                {"is_agent_bonded",     Sbywow::IsAgentBonded(bot)},
                {"service_account_id",  sMercenaryMgr.GetServiceAccountId()},
                {"bot_guid",            bot->GetGUID().GetRawValue()},
                {"bot_session_account_id",
                                        bot->GetSession() ? bot->GetSession()->GetAccountId() : 0}
            };

            json out = {
                {"ok",      true},
                {"verb",    "inspect"},
                {"context", BuildContextSnapshot(bot, sess)},
                {"engines", engineInfo},
                {"sbywow",  sbywowInfo}
            };
            if (!agentEngineDebug.is_null())
                out["agent_engine_debug"] = std::move(agentEngineDebug);
            return out.dump();
        }

        // ---- Autonomous-driving primitives ------------------------
        //
        // Three verbs the agent uses to drive a mastered bot when the
        // upstream rpg/grind subsystem is silently no-op'd by the
        // !HasRealPlayerMaster() gates. See decisions.md
        // "Master-bond suppresses roaming" for the architectural why.
        //
        // All five (move/interact/say/do_action/wait) ride existing
        // public Player / WorldSession / MotionMaster surface that
        // AC's own opcode handlers and mod-playerbots' own actions
        // use. The bridge side is a thin queue-pusher; actual
        // execution lives in SbywowAgentEngine on the world thread.

        // Queue an intent for engine execution. Returns the
        // {ok, intent_id, verb} ack immediately — the inbound
        // PendingCommand's promise is set with this string by
        // TickBot, unblocking the HTTP response within microseconds.
        // Engine completion is delivered separately via SSE
        // (intent_completed / intent_failed events keyed by
        // intent_id). See decisions.md "Async-via-events"
        // (2026-05-02) for the protocol shape.
        std::string Defer(Player* bot,
                          BotSession& sess,
                          std::shared_ptr<PendingCommand>& /*cmd*/,
                          std::string const& verb,
                          Sbywow::Intent intent)
        {
            uint64_t id = BridgeServer::Instance().MintIntentId();

            auto pending = std::make_shared<PendingIntent>();
            pending->intent   = std::move(intent);
            pending->intentId = id;
            pending->verb     = verb;

            // Emit intent_queued before pushing to the engine queue,
            // so a fast engine tick can't slip an intent_started in
            // front of us.
            json ev = {
                {"channel",   "intent"},
                {"kind",      "intent_queued"},
                {"bot_guid",  bot->GetGUID().GetRawValue()},
                {"bot_name",  bot->GetName()},
                {"intent_id", std::to_string(id)},
                {"verb",      verb}
            };
            sess.PushOutbound(ev.dump());

            sess.PushIntent(std::move(pending));

            return json{
                {"ok",        true},
                {"verb",      verb},
                {"intent_id", std::to_string(id)},
                {"queued",    true}
            }.dump();
        }

        std::string QueueMoveIntent(Player* bot, BotSession& sess,
                                    std::shared_ptr<PendingCommand>& cmd,
                                    json const& req)
        {
            if (!req.contains("x") || !req.contains("y") || !req.contains("z") ||
                !req["x"].is_number() || !req["y"].is_number() || !req["z"].is_number())
                return json{{"ok", false}, {"error", "move_to requires numeric x, y, z"}}.dump();

            Sbywow::Intent i;
            i.kind = Sbywow::IntentKind::Move;
            i.x = req["x"].get<float>();
            i.y = req["y"].get<float>();
            i.z = req["z"].get<float>();
            if (req.contains("map") && !req["map"].is_null() && req["map"].is_number_unsigned())
                i.map = req["map"].get<uint32_t>();
            return Defer(bot, sess, cmd, "move_to", std::move(i));
        }

        std::string QueueInteractIntent(Player* bot, BotSession& sess,
                                        std::shared_ptr<PendingCommand>& cmd,
                                        json const& req)
        {
            if (!req.contains("guid") || !req["guid"].is_number())
                return json{{"ok", false}, {"error", "interact_with requires numeric guid"}}.dump();

            Sbywow::Intent i;
            i.kind = Sbywow::IntentKind::Interact;
            i.guid = req["guid"].get<uint64_t>();
            return Defer(bot, sess, cmd, "interact_with", std::move(i));
        }

        std::string QueueSayIntent(Player* bot, BotSession& sess,
                                   std::shared_ptr<PendingCommand>& cmd,
                                   json const& req)
        {
            std::string text = req.value("text", "");
            if (text.empty())
                return json{{"ok", false}, {"error", "say requires text"}}.dump();

            Sbywow::Intent i;
            i.kind    = Sbywow::IntentKind::Say;
            i.text    = std::move(text);
            i.channel = req.value("channel", "say");
            return Defer(bot, sess, cmd, "say", std::move(i));
        }

        std::string QueueDoActionIntent(Player* bot, BotSession& sess,
                                        std::shared_ptr<PendingCommand>& cmd,
                                        json const& req)
        {
            std::string name = req.value("name", "");
            if (name.empty())
                return json{{"ok", false}, {"error", "do_action requires name"}}.dump();

            Sbywow::Intent i;
            i.kind            = Sbywow::IntentKind::DoAction;
            i.actionName      = std::move(name);
            i.actionQualifier = req.value("qualifier", "");
            return Defer(bot, sess, cmd, "do_action", std::move(i));
        }

        std::string QueueWaitIntent(Player* bot, BotSession& sess,
                                    std::shared_ptr<PendingCommand>& cmd,
                                    json const& req)
        {
            if (!req.contains("ms") || !req["ms"].is_number_unsigned())
                return json{{"ok", false}, {"error", "wait requires unsigned ms"}}.dump();

            uint32_t ms = req["ms"].get<uint32_t>();
            // Cap at 60s to keep agent harness round-trips bounded;
            // longer pauses can be chained by the harness.
            if (ms > 60000) ms = 60000;

            Sbywow::Intent i;
            i.kind   = Sbywow::IntentKind::Wait;
            i.waitMs = ms;
            return Defer(bot, sess, cmd, "wait", std::move(i));
        }

        // ---- Phase 4 / 5 — intent verb dispatchers ----------------------
        //
        // Each builds an Intent of the appropriate kind, validates required
        // fields, and Defers (returns intent_id ack; engine completes
        // async via SSE). Engine-side executors are in
        // SbywowAgentEngine.cpp; verbs whose executor is still stubbed
        // will return {ok:false, error:"not yet implemented"} via the
        // intent_failed SSE event.

        std::string QueueBuyItemIntent(Player* bot, BotSession& sess,
                                       std::shared_ptr<PendingCommand>& cmd,
                                       json const& req)
        {
            if (!req.contains("vendor_guid") || !req["vendor_guid"].is_number())
                return json{{"ok", false}, {"error", "buy_item requires vendor_guid"}}.dump();
            if (!req.contains("item_entry") || !req["item_entry"].is_number())
                return json{{"ok", false}, {"error", "buy_item requires item_entry"}}.dump();
            Sbywow::Intent i;
            i.kind        = Sbywow::IntentKind::BuyItem;
            i.vendorGuid  = req["vendor_guid"].get<uint64_t>();
            i.itemEntry   = req["item_entry"].get<uint32_t>();
            i.quantity    = req.contains("count") && req["count"].is_number_unsigned()
                            ? req["count"].get<uint32_t>() : 1;
            return Defer(bot, sess, cmd, "buy_item", std::move(i));
        }

        std::string QueueSellItemIntent(Player* bot, BotSession& sess,
                                        std::shared_ptr<PendingCommand>& cmd,
                                        json const& req)
        {
            if (!req.contains("vendor_guid") || !req["vendor_guid"].is_number())
                return json{{"ok", false}, {"error", "sell_item requires vendor_guid"}}.dump();
            if (!req.contains("item_guid") || !req["item_guid"].is_number())
                return json{{"ok", false}, {"error", "sell_item requires item_guid"}}.dump();
            Sbywow::Intent i;
            i.kind       = Sbywow::IntentKind::SellItem;
            i.vendorGuid = req["vendor_guid"].get<uint64_t>();
            i.itemGuid   = req["item_guid"].get<uint64_t>();
            i.quantity   = req.contains("count") && req["count"].is_number_unsigned()
                           ? req["count"].get<uint32_t>() : 0;  // 0 = sell whole stack
            return Defer(bot, sess, cmd, "sell_item", std::move(i));
        }

        std::string QueueSelectGossipOptionIntent(Player* bot, BotSession& sess,
                                                  std::shared_ptr<PendingCommand>& cmd,
                                                  json const& req)
        {
            if (!req.contains("option_index") || !req["option_index"].is_number())
                return json{{"ok", false}, {"error", "select_gossip_option requires option_index"}}.dump();
            Sbywow::Intent i;
            i.kind     = Sbywow::IntentKind::SelectGossipOption;
            i.intParam = req["option_index"].get<int32_t>();
            return Defer(bot, sess, cmd, "select_gossip_option", std::move(i));
        }

        std::string QueueTradeInitiateIntent(Player* bot, BotSession& sess,
                                             std::shared_ptr<PendingCommand>& cmd,
                                             json const& req)
        {
            if (!req.contains("partner_guid") || !req["partner_guid"].is_number())
                return json{{"ok", false}, {"error", "trade_initiate requires partner_guid"}}.dump();
            Sbywow::Intent i;
            i.kind = Sbywow::IntentKind::TradeInitiate;
            i.guid = req["partner_guid"].get<uint64_t>();
            return Defer(bot, sess, cmd, "trade_initiate", std::move(i));
        }

        std::string QueueTradeOfferItemIntent(Player* bot, BotSession& sess,
                                              std::shared_ptr<PendingCommand>& cmd,
                                              json const& req)
        {
            if (!req.contains("trade_slot") || !req["trade_slot"].is_number())
                return json{{"ok", false}, {"error", "trade_offer_item requires trade_slot"}}.dump();
            if (!req.contains("item_guid") || !req["item_guid"].is_number())
                return json{{"ok", false}, {"error", "trade_offer_item requires item_guid"}}.dump();
            Sbywow::Intent i;
            i.kind     = Sbywow::IntentKind::TradeOfferItem;
            i.intParam = req["trade_slot"].get<int32_t>();
            i.itemGuid = req["item_guid"].get<uint64_t>();
            return Defer(bot, sess, cmd, "trade_offer_item", std::move(i));
        }

        std::string QueueTradeOfferMoneyIntent(Player* bot, BotSession& sess,
                                               std::shared_ptr<PendingCommand>& cmd,
                                               json const& req)
        {
            if (!req.contains("copper") || !req["copper"].is_number_unsigned())
                return json{{"ok", false}, {"error", "trade_offer_money requires copper"}}.dump();
            Sbywow::Intent i;
            i.kind   = Sbywow::IntentKind::TradeOfferMoney;
            i.copper = req["copper"].get<uint32_t>();
            return Defer(bot, sess, cmd, "trade_offer_money", std::move(i));
        }

        std::string QueueTradeAcceptIntent(Player* bot, BotSession& sess,
                                           std::shared_ptr<PendingCommand>& cmd,
                                           json const& /*req*/)
        {
            Sbywow::Intent i;
            i.kind = Sbywow::IntentKind::TradeAccept;
            return Defer(bot, sess, cmd, "trade_accept", std::move(i));
        }

        std::string QueueTradeCancelIntent(Player* bot, BotSession& sess,
                                           std::shared_ptr<PendingCommand>& cmd,
                                           json const& /*req*/)
        {
            Sbywow::Intent i;
            i.kind = Sbywow::IntentKind::TradeCancel;
            return Defer(bot, sess, cmd, "trade_cancel", std::move(i));
        }

        std::string QueueEquipItemIntent(Player* bot, BotSession& sess,
                                         std::shared_ptr<PendingCommand>& cmd,
                                         json const& req)
        {
            if (!req.contains("item_guid") || !req["item_guid"].is_number())
                return json{{"ok", false}, {"error", "equip_item requires item_guid"}}.dump();
            Sbywow::Intent i;
            i.kind     = Sbywow::IntentKind::EquipItem;
            i.itemGuid = req["item_guid"].get<uint64_t>();
            // optional dest slot — when -1, engine auto-finds suitable slot
            i.intParam = req.contains("dest_slot") && req["dest_slot"].is_number()
                         ? req["dest_slot"].get<int32_t>() : -1;
            return Defer(bot, sess, cmd, "equip_item", std::move(i));
        }

        std::string QueueUnequipItemIntent(Player* bot, BotSession& sess,
                                           std::shared_ptr<PendingCommand>& cmd,
                                           json const& req)
        {
            if (!req.contains("equip_slot") || !req["equip_slot"].is_number())
                return json{{"ok", false}, {"error", "unequip_item requires equip_slot"}}.dump();
            Sbywow::Intent i;
            i.kind     = Sbywow::IntentKind::UnequipItem;
            i.intParam = req["equip_slot"].get<int32_t>();
            return Defer(bot, sess, cmd, "unequip_item", std::move(i));
        }

        std::string QueueDestroyItemIntent(Player* bot, BotSession& sess,
                                           std::shared_ptr<PendingCommand>& cmd,
                                           json const& req)
        {
            if (!req.contains("item_guid") || !req["item_guid"].is_number())
                return json{{"ok", false}, {"error", "destroy_item requires item_guid"}}.dump();
            Sbywow::Intent i;
            i.kind     = Sbywow::IntentKind::DestroyItem;
            i.itemGuid = req["item_guid"].get<uint64_t>();
            i.quantity = req.contains("count") && req["count"].is_number_unsigned()
                         ? req["count"].get<uint32_t>() : 0;  // 0 = destroy whole stack
            return Defer(bot, sess, cmd, "destroy_item", std::move(i));
        }

        std::string QueueUseItemIntent(Player* bot, BotSession& sess,
                                       std::shared_ptr<PendingCommand>& cmd,
                                       json const& req)
        {
            bool hasGuid  = req.contains("item_guid")  && req["item_guid"].is_number();
            bool hasEntry = req.contains("item_entry") && req["item_entry"].is_number();
            if (!hasGuid && !hasEntry)
                return json{{"ok", false}, {"error", "use_item requires item_guid or item_entry"}}.dump();
            Sbywow::Intent i;
            i.kind      = Sbywow::IntentKind::UseItem;
            i.itemGuid  = hasGuid  ? req["item_guid"].get<uint64_t>()  : 0;
            i.itemEntry = hasEntry ? req["item_entry"].get<uint32_t>() : 0;
            i.guid      = req.contains("target_guid") && req["target_guid"].is_number()
                          ? req["target_guid"].get<uint64_t>() : 0;
            return Defer(bot, sess, cmd, "use_item", std::move(i));
        }

        std::string QueueCastSpellIntent(Player* bot, BotSession& sess,
                                         std::shared_ptr<PendingCommand>& cmd,
                                         json const& req)
        {
            if (!req.contains("spell_id") || !req["spell_id"].is_number_unsigned())
                return json{{"ok", false}, {"error", "cast_spell requires spell_id"}}.dump();
            Sbywow::Intent i;
            i.kind    = Sbywow::IntentKind::CastSpell;
            i.spellId = req["spell_id"].get<uint32_t>();
            i.guid    = req.contains("target_guid") && req["target_guid"].is_number()
                        ? req["target_guid"].get<uint64_t>() : 0;
            return Defer(bot, sess, cmd, "cast_spell", std::move(i));
        }

        std::string QueueMountIntent(Player* bot, BotSession& sess,
                                     std::shared_ptr<PendingCommand>& cmd,
                                     json const& req)
        {
            Sbywow::Intent i;
            i.kind    = Sbywow::IntentKind::Mount;
            // Optional: spell_id picks a specific mount; absent means "any
            // available." Engine-side picks from spellbook.
            i.spellId = req.contains("spell_id") && req["spell_id"].is_number_unsigned()
                        ? req["spell_id"].get<uint32_t>() : 0;
            return Defer(bot, sess, cmd, "mount", std::move(i));
        }

        std::string QueueDismountIntent(Player* bot, BotSession& sess,
                                        std::shared_ptr<PendingCommand>& cmd,
                                        json const& /*req*/)
        {
            Sbywow::Intent i;
            i.kind = Sbywow::IntentKind::Dismount;
            return Defer(bot, sess, cmd, "dismount", std::move(i));
        }

        std::string QueueInteractGameObjectIntent(Player* bot, BotSession& sess,
                                                  std::shared_ptr<PendingCommand>& cmd,
                                                  json const& req)
        {
            if (!req.contains("guid") || !req["guid"].is_number())
                return json{{"ok", false}, {"error", "interact_gameobject requires guid"}}.dump();
            Sbywow::Intent i;
            i.kind = Sbywow::IntentKind::InteractGameObject;
            i.guid = req["guid"].get<uint64_t>();
            return Defer(bot, sess, cmd, "interact_gameobject", std::move(i));
        }

        std::string QueueLootTargetIntent(Player* bot, BotSession& sess,
                                          std::shared_ptr<PendingCommand>& cmd,
                                          json const& req)
        {
            if (!req.contains("guid") || !req["guid"].is_number())
                return json{{"ok", false}, {"error", "loot_target requires guid"}}.dump();
            Sbywow::Intent i;
            i.kind = Sbywow::IntentKind::LootTarget;
            i.guid = req["guid"].get<uint64_t>();
            return Defer(bot, sess, cmd, "loot_target", std::move(i));
        }

        std::string QueueMailSendIntent(Player* bot, BotSession& sess,
                                        std::shared_ptr<PendingCommand>& cmd,
                                        json const& req)
        {
            std::string recipient = req.value("recipient", "");
            if (recipient.empty())
                return json{{"ok", false}, {"error", "mail_send requires recipient"}}.dump();
            Sbywow::Intent i;
            i.kind      = Sbywow::IntentKind::MailSend;
            i.strParam1 = std::move(recipient);
            i.strParam2 = req.value("subject", "");
            i.strParam3 = req.value("body",    "");
            i.itemGuid  = req.contains("item_guid") && req["item_guid"].is_number()
                          ? req["item_guid"].get<uint64_t>() : 0;
            i.copper    = req.contains("copper") && req["copper"].is_number_unsigned()
                          ? req["copper"].get<uint32_t>() : 0;
            return Defer(bot, sess, cmd, "mail_send", std::move(i));
        }

        std::string QueueMailTakeItemIntent(Player* bot, BotSession& sess,
                                            std::shared_ptr<PendingCommand>& cmd,
                                            json const& req)
        {
            if (!req.contains("mail_id") || !req["mail_id"].is_number())
                return json{{"ok", false}, {"error", "mail_take_item requires mail_id"}}.dump();
            if (!req.contains("item_guid") || !req["item_guid"].is_number())
                return json{{"ok", false}, {"error", "mail_take_item requires item_guid"}}.dump();
            Sbywow::Intent i;
            i.kind     = Sbywow::IntentKind::MailTakeItem;
            i.mailId   = req["mail_id"].get<uint64_t>();
            i.itemGuid = req["item_guid"].get<uint64_t>();
            return Defer(bot, sess, cmd, "mail_take_item", std::move(i));
        }

        std::string QueueMailTakeMoneyIntent(Player* bot, BotSession& sess,
                                             std::shared_ptr<PendingCommand>& cmd,
                                             json const& req)
        {
            if (!req.contains("mail_id") || !req["mail_id"].is_number())
                return json{{"ok", false}, {"error", "mail_take_money requires mail_id"}}.dump();
            Sbywow::Intent i;
            i.kind   = Sbywow::IntentKind::MailTakeMoney;
            i.mailId = req["mail_id"].get<uint64_t>();
            return Defer(bot, sess, cmd, "mail_take_money", std::move(i));
        }

        std::string QueueQuestVerbIntent(Player* bot, BotSession& sess,
                                         std::shared_ptr<PendingCommand>& cmd,
                                         json const& req,
                                         Sbywow::IntentKind kind, char const* verb)
        {
            if (!req.contains("quest_id") || !req["quest_id"].is_number_unsigned())
                return json{{"ok", false}, {"error",
                            std::string(verb) + " requires quest_id"}}.dump();
            Sbywow::Intent i;
            i.kind    = kind;
            i.questId = req["quest_id"].get<uint32_t>();
            // accept/complete need the questgiver guid; abandon/share don't
            i.guid    = req.contains("npc_guid") && req["npc_guid"].is_number()
                        ? req["npc_guid"].get<uint64_t>() : 0;
            return Defer(bot, sess, cmd, verb, std::move(i));
        }

        std::string QueueGroupSimpleIntent(Player* bot, BotSession& sess,
                                           std::shared_ptr<PendingCommand>& cmd,
                                           Sbywow::IntentKind kind, char const* verb)
        {
            Sbywow::Intent i;
            i.kind = kind;
            return Defer(bot, sess, cmd, verb, std::move(i));
        }

        std::string QueueGroupPromoteLeaderIntent(Player* bot, BotSession& sess,
                                                  std::shared_ptr<PendingCommand>& cmd,
                                                  json const& req)
        {
            if (!req.contains("target_guid") || !req["target_guid"].is_number())
                return json{{"ok", false}, {"error", "group_promote_leader requires target_guid"}}.dump();
            Sbywow::Intent i;
            i.kind = Sbywow::IntentKind::GroupPromoteLeader;
            i.guid = req["target_guid"].get<uint64_t>();
            return Defer(bot, sess, cmd, "group_promote_leader", std::move(i));
        }

        std::string QueueGroupReadyCheckRespondIntent(Player* bot, BotSession& sess,
                                                      std::shared_ptr<PendingCommand>& cmd,
                                                      json const& req)
        {
            // 1 = ready, 0 = not ready
            int32_t v = 1;
            if (req.contains("ready") && req["ready"].is_boolean())
                v = req["ready"].get<bool>() ? 1 : 0;
            Sbywow::Intent i;
            i.kind     = Sbywow::IntentKind::GroupReadyCheckRespond;
            i.intParam = v;
            return Defer(bot, sess, cmd, "group_ready_check_respond", std::move(i));
        }

        // ---- Debug / test helper: reset_cooldowns -----------------------
        //
        // Wipes spell cooldowns on the bot — pass `spell_id` for a single
        // spell, omit for all. Lets the test runner re-exercise
        // long-cooldown spells (Hearthstone, mounts, etc) without
        // waiting out the natural CD or restarting the server. Sync
        // verb, no intent queue. Bridge auth (GM-only by default)
        // gates abuse.
        std::string DoResetCooldowns(Player* bot, json const& req)
        {
            if (req.contains("spell_id") && req["spell_id"].is_number_unsigned())
            {
                uint32_t spellId = req["spell_id"].get<uint32_t>();
                bot->RemoveSpellCooldown(spellId, /*update=*/ true);
                return json{
                    {"ok",       true},
                    {"verb",     "reset_cooldowns"},
                    {"spell_id", spellId}
                }.dump();
            }
            bot->RemoveAllSpellCooldown();
            return json{
                {"ok",   true},
                {"verb", "reset_cooldowns"},
                {"all",  true}
            }.dump();
        }

        // ---- Sync deep-discovery: vendor_inventory ----------------------
        //
        // Pure read of Creature::GetVendorItems with item template
        // expansion. No packets sent — agent reads catalog without
        // affecting bot state. Range-gated to mirror real proximity.
        std::string DoVendorInventory(Player* bot, json const& req)
        {
            if (!req.contains("vendor_guid") || !req["vendor_guid"].is_number())
                return json{{"ok", false}, {"error", "vendor_inventory requires vendor_guid"}}.dump();
            ObjectGuid vGuid(req["vendor_guid"].get<uint64_t>());

            Creature* npc = ObjectAccessor::GetCreature(*bot, vGuid);
            if (!npc)
                return json{{"ok", false}, {"error", "vendor not found on bot's map"}}.dump();
            if (!npc->IsVendor())
                return json{
                    {"ok", false},
                    {"error", "creature is not a vendor"},
                    {"name", npc->GetName()}
                }.dump();
            float dist = bot->GetExactDist(npc);
            constexpr float kMaxRange = 12.0f;  // matches AC's INTERACTION_DISTANCE roughly
            if (dist > kMaxRange)
                return json{
                    {"ok",       false},
                    {"error",    "vendor out of range"},
                    {"name",     npc->GetName()},
                    {"distance", dist},
                    {"max_range", kMaxRange}
                }.dump();

            VendorItemData const* vItems = npc->GetVendorItems();
            if (!vItems || vItems->Empty())
                return json{
                    {"ok",      true},
                    {"verb",    "vendor_inventory"},
                    {"name",    npc->GetName()},
                    {"items",   json::array()},
                    {"empty",   true}
                }.dump();

            json items = json::array();
            for (uint32 i = 0; i < vItems->GetItemCount(); ++i)
            {
                VendorItem const* vi = vItems->GetItem(i);
                if (!vi) continue;
                ItemTemplate const* tpl = sObjectMgr->GetItemTemplate(vi->item);
                if (!tpl) continue;
                items.push_back({
                    {"slot",            i},
                    {"entry",           vi->item},
                    {"name",            tpl->Name1},
                    {"buy_price",       tpl->BuyPrice},
                    {"sell_price",      tpl->SellPrice},
                    {"required_level",  tpl->RequiredLevel},
                    {"max_stack",       tpl->Stackable},
                    {"stock_max",       vi->maxcount},        // 0 = infinite
                    {"extended_cost",   vi->ExtendedCost},
                    {"quality",         static_cast<int>(tpl->Quality)},
                    {"item_class",      static_cast<int>(tpl->Class)},
                    {"item_subclass",   static_cast<int>(tpl->SubClass)}
                });
            }

            return json{
                {"ok",       true},
                {"verb",     "vendor_inventory"},
                {"name",     npc->GetName()},
                {"distance", dist},
                {"count",    items.size()},
                {"items",    items}
            }.dump();
        }


        // Decode a creature's npc_flags field into a small array of
        // human-readable role tags. The agent uses these to pick which
        // follow-up verb to run (interact_with on a vendor, do_action
        // "buy" on a vendor, etc.) without learning AC's bitfield.
        json DecodeCreatureFlags(Creature const* c)
        {
            json flags = json::array();
            if (c->IsGossip())          flags.push_back("gossip");
            if (c->IsQuestGiver())      flags.push_back("questgiver");
            if (c->IsTrainer())         flags.push_back("trainer");
            if (c->IsVendor())          flags.push_back("vendor");
            if (c->IsArmorer())         flags.push_back("repair");
            if (c->IsTaxi())            flags.push_back("flightmaster");
            if (c->IsBanker())          flags.push_back("banker");
            if (c->IsInnkeeper())       flags.push_back("innkeeper");
            if (c->IsAuctioner())       flags.push_back("auctioneer");
            if (c->IsBattleMaster())    flags.push_back("battlemaster");
            if (c->IsTabardDesigner())  flags.push_back("tabard");
            if (c->IsSpiritHealer())    flags.push_back("spirithealer");
            if (c->IsSpiritGuide())     flags.push_back("spiritguide");
            if (c->HasNpcFlag(UNIT_NPC_FLAG_STABLEMASTER)) flags.push_back("stablemaster");
            if (c->HasNpcFlag(UNIT_NPC_FLAG_MAILBOX))      flags.push_back("mailbox");
            if (c->HasNpcFlag(UNIT_NPC_FLAG_GUILD_BANKER)) flags.push_back("guild_banker");
            return flags;
        }

        std::string DoFindNearby(Player* bot, json const& req)
        {
            // Range default 30 yards, capped at 200 to keep the grid
            // visit bounded. For comparison the player visibility range
            // is ~100 yards on most maps; 200 is generous.
            float range = req.contains("range") && req["range"].is_number()
                          ? req["range"].get<float>() : 30.0f;
            if (range < 0.f)   range = 0.f;
            if (range > 200.f) range = 200.f;

            bool wantCreatures = true, wantPlayers = true, wantGameObjects = true;
            if (req.contains("kinds") && req["kinds"].is_array())
            {
                wantCreatures = wantPlayers = wantGameObjects = false;
                for (auto const& kind : req["kinds"])
                {
                    if (!kind.is_string()) continue;
                    std::string s = kind.get<std::string>();
                    if      (s == "creature")   wantCreatures = true;
                    else if (s == "player")     wantPlayers = true;
                    else if (s == "gameobject") wantGameObjects = true;
                }
            }

            int limit = req.contains("limit") && req["limit"].is_number()
                        ? req["limit"].get<int>() : 50;
            if (limit < 1)   limit = 1;
            if (limit > 200) limit = 200;

            bool aliveOnly = req.contains("alive_only") && req["alive_only"].is_boolean()
                             ? req["alive_only"].get<bool>() : true;

            PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
            Player* master = ai ? ai->GetMaster() : nullptr;

            // Single grid visit collecting WorldObjects in range; we
            // filter by kind below. AllWorldObjectsInRange honors
            // phase, so cross-phase objects don't leak into perception.
            std::list<WorldObject*> objs;
            Acore::AllWorldObjectsInRange check(bot, range);
            Acore::WorldObjectListSearcher<Acore::AllWorldObjectsInRange> searcher(bot, objs, check);
            Cell::VisitObjects(bot, searcher, range);

            // Pair items with their distance so we can sort by it,
            // then truncate to limit before returning.
            std::vector<std::pair<float, json>> scored;
            scored.reserve(objs.size());

            for (WorldObject* o : objs)
            {
                if (!o || o == bot) continue;

                if (Creature* c = o->ToCreature())
                {
                    if (!wantCreatures) continue;
                    if (aliveOnly && !c->IsAlive()) continue;
                    float d = bot->GetExactDist(c);
                    json item = {
                        {"kind",    "creature"},
                        {"guid",    c->GetGUID().GetRawValue()},
                        {"entry",   c->GetEntry()},
                        {"name",    c->GetName()},
                        {"dist",    d},
                        {"x",       c->GetPositionX()},
                        {"y",       c->GetPositionY()},
                        {"z",       c->GetPositionZ()},
                        {"level",   c->GetLevel()},
                        {"hp_pct",  static_cast<int>(c->GetHealthPct())},
                        {"alive",   c->IsAlive()},
                        {"hostile", c->IsHostileTo(bot)},
                        {"flags",   DecodeCreatureFlags(c)}
                    };
                    scored.emplace_back(d, std::move(item));
                }
                else if (Player* p = o->ToPlayer())
                {
                    if (!wantPlayers) continue;
                    if (aliveOnly && !p->IsAlive()) continue;
                    float d = bot->GetExactDist(p);
                    json item = {
                        {"kind",      "player"},
                        {"guid",      p->GetGUID().GetRawValue()},
                        {"name",      p->GetName()},
                        {"dist",      d},
                        {"x",         p->GetPositionX()},
                        {"y",         p->GetPositionY()},
                        {"z",         p->GetPositionZ()},
                        {"level",     p->GetLevel()},
                        {"hp_pct",    static_cast<int>(p->GetHealthPct())},
                        {"is_master", master == p},
                        {"is_bot",    sPlayerbotsMgr.GetPlayerbotAI(p) != nullptr}
                    };
                    scored.emplace_back(d, std::move(item));
                }
                else if (GameObject* go = o->ToGameObject())
                {
                    if (!wantGameObjects) continue;
                    float d = bot->GetExactDist(go);
                    json item = {
                        {"kind",    "gameobject"},
                        {"guid",    go->GetGUID().GetRawValue()},
                        {"entry",   go->GetEntry()},
                        {"name",    go->GetName()},
                        {"dist",    d},
                        {"x",       go->GetPositionX()},
                        {"y",       go->GetPositionY()},
                        {"z",       go->GetPositionZ()},
                        {"go_type", static_cast<int>(go->GetGoType())}
                    };
                    scored.emplace_back(d, std::move(item));
                }
            }

            std::sort(scored.begin(), scored.end(),
                      [](auto const& a, auto const& b) { return a.first < b.first; });

            json arr = json::array();
            int taken = 0;
            for (auto& [d, item] : scored)
            {
                if (taken++ >= limit) break;
                arr.push_back(std::move(item));
            }

            return json{
                {"ok",      true},
                {"verb",    "find_nearby"},
                {"count",   arr.size()},
                {"range",   range},
                {"objects", arr}
            }.dump();
        }


        // Translate a verb JSON into a result JSON. Runs on the world
        // thread, so any Playerbots API is fair game.
        //
        // Defense: top-level try/catch wraps every verb. A SIGSEGV from
        // a Playerbots impl will still crash the world (incident
        // 2026-05-02), but anything that throws a std::exception turns
        // into a clean 503 instead of bringing down the realm. The
        // narrower per-verb wrappers (Save/Load) cover the highest-risk
        // sites first; this is belt-and-suspenders for the rest.
        // Always returns the string the HTTP handler should send back.
        // Intent verbs return {ok, intent_id, queued:true} immediately
        // (engine completion fires later as an SSE event). Sync verbs
        // return their full result inline.
        std::string DispatchCommandInner(Player* bot, BotSession& sess,
                                         std::shared_ptr<PendingCommand>& cmd);

        std::string DispatchCommand(Player* bot, BotSession& sess,
                                    std::shared_ptr<PendingCommand>& cmd)
        {
            try { return DispatchCommandInner(bot, sess, cmd); }
            catch (std::exception const& e)
            {
                json err = { {"ok", false}, {"error", std::string("dispatch threw: ") + e.what()} };
                return err.dump();
            }
        }

        std::string DispatchCommandInner(Player* bot, BotSession& sess,
                                         std::shared_ptr<PendingCommand>& cmd)
        {
            json req;
            try { req = json::parse(cmd->json); }
            catch (std::exception const& e)
            {
                json err = { {"ok", false}, {"error", std::string("bad json: ") + e.what()} };
                return err.dump();
            }

            std::string verb = req.value("verb", "");

            if (verb == "ping")
            {
                json ok = { {"ok", true} };
                return ok.dump();
            }

            // do_action queues a DoAction intent. Engine's
            // ExecuteDoAction calls PlayerbotAI::DoSpecificAction
            // — same as the prior inline body, just relocated.
            if (verb == "do_action")
                return QueueDoActionIntent(bot, sess, cmd, req);

            // ---- Strategy management (the nudge layer) ----------------

            if (verb == "add_strategy" || verb == "remove_strategy" || verb == "change_strategies")
            {
                PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
                if (!ai)
                {
                    json err = { {"ok", false}, {"error", "no PlayerbotAI for this bot"} };
                    return err.dump();
                }

                std::string stateStr = req.value("state", "non-combat");
                int state = ParseBotState(stateStr);
                if (state == -2)
                {
                    json err = { {"ok", false}, {"error", "bad state: " + stateStr +
                                  " (combat|non-combat|dead|all)"} };
                    return err.dump();
                }

                json applied;
                if (verb == "add_strategy")
                {
                    std::string name = req.value("name", "");
                    if (name.empty())
                    {
                        json err = { {"ok", false}, {"error", "add_strategy requires name"} };
                        return err.dump();
                    }
                    applied = ForEachEngine(ai, state, [&](Engine* e, BotState) { e->addStrategy(name); });
                }
                else if (verb == "remove_strategy")
                {
                    std::string name = req.value("name", "");
                    if (name.empty())
                    {
                        json err = { {"ok", false}, {"error", "remove_strategy requires name"} };
                        return err.dump();
                    }
                    applied = ForEachEngine(ai, state, [&](Engine* e, BotState) { e->removeStrategy(name); });
                }
                else // change_strategies — comma-syntax: "+kite,-aggressive,~focus"
                {
                    std::string ops = req.value("ops", "");
                    if (ops.empty())
                    {
                        json err = { {"ok", false}, {"error", "change_strategies requires ops"} };
                        return err.dump();
                    }
                    applied = ForEachEngine(ai, state, [&](Engine* e, BotState) { e->ChangeStrategy(ops); });
                }

                json ok = { {"ok", true}, {"verb", verb}, {"applied_to", applied} };
                return ok.dump();
            }

            if (verb == "list_strategies")
            {
                PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
                if (!ai)
                {
                    json err = { {"ok", false}, {"error", "no PlayerbotAI for this bot"} };
                    return err.dump();
                }

                std::string stateStr = req.value("state", "all");
                int state = ParseBotState(stateStr);
                if (state == -2)
                {
                    json err = { {"ok", false}, {"error", "bad state: " + stateStr} };
                    return err.dump();
                }

                json out = json::object();
                for (uint8 i = 0; i < BOT_STATE_MAX; ++i)
                {
                    if (state >= 0 && i != static_cast<uint8>(state)) continue;
                    BotState s = static_cast<BotState>(i);
                    if (Engine* e = ai->GetEngine(s))
                        out[BotStateName(s)] = e->GetStrategies();
                }
                return json{ {"ok", true}, {"verb", "list_strategies"}, {"strategies", out} }.dump();
            }

            // ---- Value system (set/get on AiObjectContext) ------------

            if (verb == "set_value" || verb == "get_value")
            {
                PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
                if (!ai)
                {
                    json err = { {"ok", false}, {"error", "no PlayerbotAI for this bot"} };
                    return err.dump();
                }
                std::string key = req.value("key", "");
                if (key.empty())
                {
                    json err = { {"ok", false}, {"error", verb + " requires key"} };
                    return err.dump();
                }

                UntypedValue* uv = ai->GetAiObjectContext()->GetUntypedValue(key);
                if (!uv)
                {
                    json err = { {"ok", false}, {"error", "no such value key: " + key} };
                    return err.dump();
                }

                if (verb == "get_value")
                {
                    // CRITICAL: do NOT call Format(). Many overridden
                    // Format() impls deref pointers (UnitCalculatedValue
                    // calls Calculate()→Unit*, then Unit::GetName()) that
                    // can be stale or null when the value is queried
                    // outside its normal computation context. A SIGSEGV
                    // on the world thread crashes the entire realm and
                    // disconnects every player.
                    //
                    // Save() is much safer: default returns the literal
                    // "?", overrides (RtiValue, PositionValue, Stances,
                    // etc.) return stored primitive data without
                    // dereferences. If Save returns "?", the value isn't
                    // introspectable through this verb — that's the
                    // honest signal to the agent. We additionally wrap
                    // in try/catch for std::exception belt-and-suspenders;
                    // it won't catch a segfault but covers anything that
                    // throws a real C++ exception.
                    //
                    // Incident capture: 2026-05-02 in incidents.md.
                    std::string saved;
                    try { saved = uv->Save(); }
                    catch (std::exception const& e) { saved = std::string("<save threw: ") + e.what() + ">"; }
                    return json{
                        {"ok",   true},
                        {"verb", "get_value"},
                        {"key",  key},
                        {"save", saved}
                    }.dump();
                }

                // set_value — UntypedValue::Load takes a string and
                // returns true on success. Same crash-class risk as
                // Format() (see incident 2026-05-02): a Load impl that
                // dereferences stale pointers can segfault the world
                // thread. Wrap in try/catch for std::exception (won't
                // catch SIGSEGV but covers everything that throws).
                std::string val = req.value("value", "");
                bool loaded = false;
                std::string loadErr;
                try { loaded = uv->Load(val); }
                catch (std::exception const& e) { loadErr = std::string("load threw: ") + e.what(); }
                json out = {
                    {"ok",    loaded},
                    {"verb",  "set_value"},
                    {"key",   key},
                    {"value", val}
                };
                if (!loadErr.empty())
                    out["error"] = loadErr;
                else if (!loaded)
                    out["error"] = "value type does not support Load() (read-only via set_value)";
                return out.dump();
            }

            // ---- Diagnostic ------------------------------------------

            if (verb == "inspect")
                return DoInspect(bot, sess);

            // get_context — sync verb returning the same structured
            // payload the agent harness sees on every snapshot.state
            // SSE event. Used for cold-start (harness just connected,
            // no warm snapshot yet) or explicit refresh ("things might
            // have shifted since the last tick"). Bridge-side cost is
            // a single BuildContextSnapshot call. See decisions.md
            // "Tool surface is for actions and deep discovery"
            // (2026-05-02) for the architectural reasoning — verbs
            // are for actions and deep discovery; continuous state
            // is pushed via the snapshot, with this verb as the
            // sync fallback path.
            if (verb == "get_context")
            {
                json ctx = BuildContextSnapshot(bot, sess);
                ctx["ok"]   = true;
                ctx["verb"] = "get_context";
                return ctx.dump();
            }

            // set_agent_mode toggles whether the agent harness is
            // actively driving the bot. Default on attach: false
            // (default Engine ticks; bot acts like a normal merc).
            // Agent opts in by setting true. Body:
            // {"verb":"set_agent_mode","mode":true|false}. Returns
            // {ok, agent_mode, previous_mode, changed}. See
            // decisions.md "Agent mode is an explicit opt-in" for
            // the design; .merc agent chat command is the master-
            // side counterpart.
            if (verb == "set_agent_mode")
            {
                if (!req.contains("mode") || !req["mode"].is_boolean())
                    return json{{"ok", false},
                                {"error", "set_agent_mode requires boolean 'mode'"}}.dump();
                bool desired = req["mode"].get<bool>();
                bool prev = BridgeServer::Instance().ApplyAgentModeToggle(bot, desired, "agent");
                return json{
                    {"ok",            true},
                    {"verb",          "set_agent_mode"},
                    {"agent_mode",    desired},
                    {"previous_mode", prev},
                    {"changed",       prev != desired}
                }.dump();
            }

            // set_follow toggles whether SbywowAgentEngine delegates
            // idle ticks to the default engine (follow + react +
            // autonomic) or anchors in place. Default is true (follow).
            // Body: {"verb":"set_follow","mode":true|false}. Returns
            // {ok, follow_mode, previous_mode, changed}. See
            // decisions.md "Idle delegation + follow toggle"
            // (2026-05-02) for the design.
            if (verb == "set_follow")
            {
                if (!req.contains("mode") || !req["mode"].is_boolean())
                    return json{{"ok", false},
                                {"error", "set_follow requires boolean 'mode'"}}.dump();
                bool desired = req["mode"].get<bool>();
                bool prev = sess.IsFollowMode();
                sess.SetFollowMode(desired);
                if (prev != desired)
                {
                    json ev = {
                        {"channel",      "lifecycle"},
                        {"kind",         desired ? "follow_mode_entered" : "follow_mode_exited"},
                        {"bot_guid",     bot->GetGUID().GetRawValue()},
                        {"bot_name",     bot->GetName()},
                        {"follow_mode",  desired}
                    };
                    sess.PushOutbound(ev.dump());
                }
                return json{
                    {"ok",            true},
                    {"verb",          "set_follow"},
                    {"follow_mode",   desired},
                    {"previous_mode", prev},
                    {"changed",       prev != desired}
                }.dump();
            }

            // ---- Autonomous-driving primitives ------------------------

            if (verb == "move_to")
                return QueueMoveIntent(bot, sess, cmd, req);

            if (verb == "find_nearby")
                return DoFindNearby(bot, req);

            // interact_with queues an Interact intent. The
            // gossip-menu read happens inside the engine's
            // ExecuteInteract on the world thread — same call shape
            // as the prior inline body.
            if (verb == "interact_with")
                return QueueInteractIntent(bot, sess, cmd, req);

            // say queues a Say intent.
            if (verb == "say")
                return QueueSayIntent(bot, sess, cmd, req);

            // wait suspends engine intent dispatch for N ms; useful
            // for chaining "do A, hold, do B" sequences without the
            // harness needing to time-out HTTP round-trips.
            if (verb == "wait")
                return QueueWaitIntent(bot, sess, cmd, req);

            // ---- Phase 4 / 5 — vendor / trade / gossip / inventory /
            //       world / mail / quest / group verbs ------------------
            if (verb == "vendor_inventory")           return DoVendorInventory(bot, req);
            if (verb == "reset_cooldowns")            return DoResetCooldowns(bot, req);
            if (verb == "buy_item")                   return QueueBuyItemIntent(bot, sess, cmd, req);
            if (verb == "sell_item")                  return QueueSellItemIntent(bot, sess, cmd, req);
            if (verb == "select_gossip_option")       return QueueSelectGossipOptionIntent(bot, sess, cmd, req);
            if (verb == "trade_initiate")             return QueueTradeInitiateIntent(bot, sess, cmd, req);
            if (verb == "trade_offer_item")           return QueueTradeOfferItemIntent(bot, sess, cmd, req);
            if (verb == "trade_offer_money")          return QueueTradeOfferMoneyIntent(bot, sess, cmd, req);
            if (verb == "trade_accept")               return QueueTradeAcceptIntent(bot, sess, cmd, req);
            if (verb == "trade_cancel")               return QueueTradeCancelIntent(bot, sess, cmd, req);
            if (verb == "equip_item")                 return QueueEquipItemIntent(bot, sess, cmd, req);
            if (verb == "unequip_item")               return QueueUnequipItemIntent(bot, sess, cmd, req);
            if (verb == "destroy_item")               return QueueDestroyItemIntent(bot, sess, cmd, req);
            if (verb == "use_item")                   return QueueUseItemIntent(bot, sess, cmd, req);
            if (verb == "cast_spell")                 return QueueCastSpellIntent(bot, sess, cmd, req);
            if (verb == "mount")                      return QueueMountIntent(bot, sess, cmd, req);
            if (verb == "dismount")                   return QueueDismountIntent(bot, sess, cmd, req);
            if (verb == "interact_gameobject")        return QueueInteractGameObjectIntent(bot, sess, cmd, req);
            if (verb == "loot_target")                return QueueLootTargetIntent(bot, sess, cmd, req);
            if (verb == "mail_send")                  return QueueMailSendIntent(bot, sess, cmd, req);
            if (verb == "mail_take_item")             return QueueMailTakeItemIntent(bot, sess, cmd, req);
            if (verb == "mail_take_money")            return QueueMailTakeMoneyIntent(bot, sess, cmd, req);
            if (verb == "quest_accept")               return QueueQuestVerbIntent(bot, sess, cmd, req, Sbywow::IntentKind::QuestAccept,   "quest_accept");
            if (verb == "quest_complete")             return QueueQuestVerbIntent(bot, sess, cmd, req, Sbywow::IntentKind::QuestComplete, "quest_complete");
            if (verb == "quest_abandon")              return QueueQuestVerbIntent(bot, sess, cmd, req, Sbywow::IntentKind::QuestAbandon,  "quest_abandon");
            if (verb == "quest_share")                return QueueQuestVerbIntent(bot, sess, cmd, req, Sbywow::IntentKind::QuestShare,    "quest_share");
            if (verb == "group_accept_invite")        return QueueGroupSimpleIntent(bot, sess, cmd, Sbywow::IntentKind::GroupAcceptInvite,  "group_accept_invite");
            if (verb == "group_decline_invite")       return QueueGroupSimpleIntent(bot, sess, cmd, Sbywow::IntentKind::GroupDeclineInvite, "group_decline_invite");
            if (verb == "group_leave")                return QueueGroupSimpleIntent(bot, sess, cmd, Sbywow::IntentKind::GroupLeave,         "group_leave");
            if (verb == "group_promote_leader")       return QueueGroupPromoteLeaderIntent(bot, sess, cmd, req);
            if (verb == "group_ready_check_respond")  return QueueGroupReadyCheckRespondIntent(bot, sess, cmd, req);

            // cancel_intent removes a queued intent or interrupts an
            // in-flight Wait. Sub-tick intents (Move/Interact/Say/
            // DoAction) that have already been popped have already
            // emitted their terminal event by the time cancel sees
            // them — we return ok=false in that case. See decisions.md
            // "Async intent contract: cancellation scope narrowed"
            // (2026-05-02) for the lifetime semantics.
            if (verb == "cancel_intent")
            {
                std::string idStr = req.value("intent_id", "");
                if (idStr.empty())
                    return json{{"ok", false}, {"error", "cancel_intent requires intent_id"}}.dump();

                uint64_t id = 0;
                try { id = std::stoull(idStr); }
                catch (...) { return json{{"ok", false}, {"error", "intent_id must be a uint64 string"}}.dump(); }

                std::string cancelState;
                std::string cancelledVerb;
                if (sess.RemoveIntentById(id, cancelledVerb))
                    cancelState = "from_queue";
                else
                {
                    PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
                    Engine* nc = ai ? ai->GetEngine(BOT_STATE_NON_COMBAT) : nullptr;
                    auto* agentEng = dynamic_cast<Sbywow::SbywowAgentEngine*>(nc);
                    if (agentEng && agentEng->CancelWaitIfMatch(id))
                    {
                        cancelState   = "interrupted";
                        cancelledVerb = "wait";
                    }
                    else if (agentEng && agentEng->CancelInFlightMoveIfMatch(id))
                    {
                        cancelState   = "interrupted";
                        cancelledVerb = "move_to";
                    }
                    else if (agentEng && agentEng->CancelInFlightCastIfMatch(id))
                    {
                        cancelState   = "interrupted";
                        cancelledVerb = agentEng->CastingIntentVerb();
                    }
                }

                if (cancelState.empty())
                {
                    // Last resort: maybe the intent already terminated.
                    // Surface the recovery-ring verdict in the error so
                    // the agent doesn't need a second round-trip.
                    BotSession::TerminalIntent prior;
                    if (sess.LookupTerminal(id, prior))
                        return json{
                            {"ok",          false},
                            {"error",       "intent already terminal"},
                            {"intent_id",   idStr},
                            {"prior_kind",  prior.kind},
                            {"prior_verb",  prior.verb},
                            {"prior_state", prior.state}
                        }.dump();
                    return json{
                        {"ok", false},
                        {"error", "intent_id not found (older than recovery ring or never existed)"},
                        {"intent_id", idStr}
                    }.dump();
                }

                json ev = {
                    {"channel",   "intent"},
                    {"kind",      "intent_cancelled"},
                    {"bot_guid",  bot->GetGUID().GetRawValue()},
                    {"bot_name",  bot->GetName()},
                    {"intent_id", idStr},
                    {"verb",      cancelledVerb},
                    {"state",     cancelState}
                };
                sess.PushOutbound(ev.dump());

                BotSession::TerminalIntent rec;
                rec.intentId = id;
                rec.verb     = cancelledVerb;
                rec.kind     = "intent_cancelled";
                rec.state    = cancelState;
                sess.RecordTerminal(std::move(rec));

                return json{
                    {"ok",        true},
                    {"verb",      "cancel_intent"},
                    {"intent_id", idStr},
                    {"state",     cancelState}
                }.dump();
            }

            // get_intent — recovery primitive. Returns the terminal
            // record for an intent_id that exited the queue/engine
            // recently (within kTerminalRingCap = 64 entries).
            // Useful when an SSE consumer reconnected after
            // disconnection or fell behind the bounded outbound
            // queue. See decisions.md "Async intent contract"
            // (2026-05-02) for the choice of query-primitive
            // over stream-resume.
            if (verb == "get_intent")
            {
                std::string idStr = req.value("intent_id", "");
                if (idStr.empty())
                    return json{{"ok", false}, {"error", "get_intent requires intent_id"}}.dump();

                uint64_t id = 0;
                try { id = std::stoull(idStr); }
                catch (...) { return json{{"ok", false}, {"error", "intent_id must be a uint64 string"}}.dump(); }

                BotSession::TerminalIntent rec;
                if (!sess.LookupTerminal(id, rec))
                    return json{
                        {"ok",        false},
                        {"error",     "intent_id not in recent ring (older than 64 terminals or never existed)"},
                        {"intent_id", idStr}
                    }.dump();

                json out = {
                    {"ok",          true},
                    {"verb",        "get_intent"},
                    {"intent_id",   idStr},
                    {"intent_verb", rec.verb},
                    {"kind",        rec.kind}
                };
                if (!rec.state.empty())
                    out["state"] = rec.state;
                if (!rec.resultJson.empty())
                {
                    try { out["result"] = json::parse(rec.resultJson); }
                    catch (std::exception const&) { out["result"] = rec.resultJson; }
                }
                return out.dump();
            }

            json err = { {"ok", false}, {"error", "unknown verb: " + verb} };
            return err.dump();
        }
    }

    // -----------------------------------------------------------------------
    // World-thread tick: drain inbound commands
    // -----------------------------------------------------------------------

    void BridgeServer::TickBot(Player* bot)
    {
        if (!bot || !running_.load())
            return;

        auto session = GetSession(bot->GetGUID());
        if (!session)
            return;

        // Drain inbound commands. Every dispatch returns the string
        // the HTTP handler is blocked on. Intent verbs return their
        // ack immediately ({ok, intent_id, queued:true}); engine
        // completion is delivered out-of-band via SSE.
        //
        // Agent mode (whether the agent is driving vs. default Engine
        // ticks) is an explicit opt-in managed by `set_agent_mode`
        // (bridge verb) or `.merc agent` (chat command). Default on
        // attach is OFF — see decisions.md "Agent mode is an explicit
        // opt-in." HeartbeatAgeMs is informational only, not coupled
        // to mode.
        std::shared_ptr<PendingCommand> cmd;
        while (session->PopInbound(cmd))
        {
            std::string out = DispatchCommand(bot, *session, cmd);
            try { cmd->result.set_value(std::move(out)); }
            catch (std::future_error const&) { /* receiver gone */ }
        }
    }

    // -----------------------------------------------------------------------
    // Routes
    // -----------------------------------------------------------------------

    void BridgeServer::RegisterRoutes()
    {
        // POST /bot/<guid>/cmd
        server_->Post(R"(/bot/(\d+)/cmd)",
            [this](httplib::Request const& req, httplib::Response& res)
        {
            if (!CheckAuth(req.get_header_value("Authorization")))
            {
                res.status = 401;
                res.set_content(R"({"ok":false,"error":"unauthorized"})", "application/json");
                return;
            }

            uint64 rawGuid = 0;
            try { rawGuid = std::stoull(req.matches[1].str()); }
            catch (...) { res.status = 400; res.set_content(R"({"ok":false,"error":"bad guid"})", "application/json"); return; }

            auto session = GetSession(ObjectGuid(rawGuid));
            if (!session)
            {
                res.status = 404;
                res.set_content(R"({"ok":false,"error":"bot not attached"})", "application/json");
                return;
            }

            // Push command, wait for world-thread to *dispatch* and
            // return. The timeout is a liveness guard against a wedged
            // world thread, not an "execution budget." Sync verbs run
            // entirely on the world thread and finish in <50ms typical
            // (worst case ~500ms for find_nearby with a wide range).
            // Intent verbs return their {ok, intent_id, queued} ack
            // immediately — engine completion lands later as an SSE
            // event (intent_completed/failed/cancelled), so this
            // timeout only covers the dispatch + sync-verb execution
            // path. 3s gives ~60 ticks of headroom for normal
            // world-thread hiccups while surfacing genuine wedges
            // quickly.
            auto fut = session->PushInbound(req.body);
            if (fut.wait_for(std::chrono::seconds(3)) == std::future_status::timeout)
            {
                res.status = 504;
                res.set_content(R"({"ok":false,"error":"world-thread timeout"})", "application/json");
                return;
            }

            // wait_for returning ready can also mean broken_promise (the
            // session/PendingCommand was destroyed before set_value ran,
            // e.g. bot logged out mid-flight). get() throws future_error
            // in that case; turn it into a clean 503 instead of letting
            // it bubble up as an httplib 500.
            try
            {
                res.status = 200;
                res.set_content(fut.get(), "application/json");
            }
            catch (std::future_error const&)
            {
                res.status = 503;
                res.set_content(R"({"ok":false,"error":"session went away mid-command"})", "application/json");
            }
        });

        // GET /bot/<guid>/events  (SSE stream)
        server_->Get(R"(/bot/(\d+)/events)",
            [this](httplib::Request const& req, httplib::Response& res)
        {
            if (!CheckAuth(req.get_header_value("Authorization")))
            {
                res.status = 401;
                res.set_content(R"({"ok":false,"error":"unauthorized"})", "application/json");
                return;
            }

            uint64 rawGuid = 0;
            try { rawGuid = std::stoull(req.matches[1].str()); }
            catch (...) { res.status = 400; return; }

            auto session = GetSession(ObjectGuid(rawGuid));
            if (!session)
            {
                res.status = 404;
                res.set_content(R"({"ok":false,"error":"bot not attached"})", "application/json");
                return;
            }

            res.set_header("Cache-Control", "no-cache");
            res.set_header("Connection",    "keep-alive");

            // Keep connection alive in a streaming loop. cpp-httplib calls
            // the provider repeatedly; we wait for events with a timeout
            // and emit SSE keepalive comments otherwise.
            session->SetSseAttached(true);
            res.set_chunked_content_provider("text/event-stream",
                [session](size_t /*offset*/, httplib::DataSink& sink) -> bool
            {
                OutboundEvent ev;
                if (session->WaitOutbound(ev, /*timeoutMs=*/15000))
                {
                    std::string frame = "data: " + ev.json + "\n\n";
                    if (!sink.write(frame.data(), frame.size()))
                        return false;          // client gone
                }
                else
                {
                    // SSE keepalive comment line
                    std::string ka = ": keepalive\n\n";
                    if (!sink.write(ka.data(), ka.size()))
                        return false;
                }
                return true;
            },
                [session](bool /*success*/) { session->SetSseAttached(false); });
        });

        // GET /bots — debug listing
        server_->Get("/bots",
            [this](httplib::Request const& req, httplib::Response& res)
        {
            if (!CheckAuth(req.get_header_value("Authorization")))
            {
                res.status = 401;
                res.set_content(R"({"ok":false,"error":"unauthorized"})", "application/json");
                return;
            }

            json arr = json::array();
            {
                std::lock_guard<std::mutex> lock(sessionsMutex_);
                for (auto const& [key, sess] : sessions_)
                {
                    arr.push_back({
                        {"guid",            key},
                        {"name",            sess->Name()},
                        {"heartbeat_ms",    sess->HeartbeatAgeMs()},
                        {"agent_mode",      sess->IsAgentMode()},
                        {"sse_attached",    sess->IsSseAttached()},
                        {"intent_count",    static_cast<int>(sess->IntentCount())}
                    });
                }
            }
            res.set_content(arr.dump(), "application/json");
        });
    }
}
