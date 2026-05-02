/*
 * sbywow Agent Bridge — script registration.
 *
 * Four scripts:
 *   - SbywowBridgeWorldScript   drives Start/Stop on world startup/shutdown.
 *   - SbywowBridgePlayerScript  the main fan-out: attaches bot sessions,
 *                               ticks them, and emits per-player events
 *                               (combat, chat, inventory, master events,
 *                               etc.) into the SSE stream.
 *   - SbywowBridgeUnitScript    fans Unit-level hooks (damage in/out).
 *   - SbywowBridgeGroupScript   fans Group lifecycle (invite/join/leave/
 *                               leader change) into per-bot streams.
 *
 * Events are sourced purely from AC's ScriptMgr surface. The
 * action-listener path (engines->AddActionExecutionListener) was
 * removed alongside seize/release in the SbywowAgentEngine pivot —
 * the agent now owns BOT_STATE_NON_COMBAT directly via engine
 * subclass, so the listener-veto pattern is no longer needed.
 *
 * Event taxonomy is locked: see docs/agent-interface.md "Event taxonomy"
 * for the canonical channel/kind list. Past-tense verbs throughout;
 * one channel per subject area.
 */

#include "BridgeServer.h"
#include "BotSession.h"
#include "deps/json.hpp"

#include "Channel.h"
#include "Creature.h"
#include "Group.h"
#include "GroupScript.h"
#include "Guild.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "LootMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Playerbots.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "UnitScript.h"

#include <unordered_map>
#include <unordered_set>

using json = nlohmann::json;

namespace
{
    using namespace Sbywow::Bridge;

    // Snapshot cadence is sourced from BridgeServer config
    // (Sbywow.Bridge.SnapshotEveryNUpdates, default 500). See
    // BridgeServer.h::BridgeConfig for tuning rationale.

    bool IsAttachedBot(Player* player)
    {
        return player && sPlayerbotsMgr.GetPlayerbotAI(player) != nullptr;
    }

    void EmitEvent(Player* bot, json const& payload)
    {
        auto session = BridgeServer::Instance().GetSession(bot->GetGUID());
        if (!session)
            return;
        session->PushOutbound(payload.dump());
    }

    json BaseEvent(Player* bot, std::string const& channel, std::string const& kind)
    {
        return {
            {"channel",  channel},
            {"kind",     kind},
            {"bot_guid", bot->GetGUID().GetRawValue()},
            {"bot_name", bot->GetName()}
        };
    }

    // Iterate every attached bot whose master == `master`, calling fn(bot).
    // Used by master.* event fan-out: when a master player triggers a
    // notable event (death, combat enter/exit, login/logout, level up),
    // we emit a corresponding event on every attached bot bonded to them.
    template <typename Fn>
    void ForEachBondedBot(Player* master, Fn const& fn)
    {
        if (!master) return;
        PlayerbotMgr* mgr = sPlayerbotsMgr.GetPlayerbotMgr(master);
        if (!mgr) return;
        for (auto it = mgr->GetPlayerBotsBegin(); it != mgr->GetPlayerBotsEnd(); ++it)
        {
            Player* bot = it->second;
            if (!bot || !IsAttachedBot(bot))
                continue;
            PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
            if (!ai || ai->GetMaster() != master)
                continue;
            fn(bot);
        }
    }

    // Iterate every attached bot in a Group, optionally skipping a guid.
    // Used by group.* and chat.party_received fan-out: we emit on each
    // attached bot in the group EXCEPT the actor (so the bot doesn't see
    // an event "I joined the group" — which it caused — as if external).
    template <typename Fn>
    void ForEachAttachedBotInGroup(Group* group, ObjectGuid skipGuid, Fn const& fn)
    {
        if (!group) return;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* m = ref->GetSource();
            if (!m || !IsAttachedBot(m)) continue;
            if (m->GetGUID() == skipGuid)  continue;
            fn(m);
        }
    }

    // Map AC chat-language enum to our wire string. Most chat is LANG_UNIVERSAL
    // (cross-faction). Faction-specific is rare for our scenarios.
    char const* ChatLanguageName(uint32 lang)
    {
        switch (lang)
        {
            case LANG_UNIVERSAL: return "universal";
            case LANG_ORCISH:    return "orcish";
            case LANG_COMMON:    return "common";
            case LANG_ADDON:     return "addon";
            default:             return "other";
        }
    }

    class SbywowBridgeWorldScript : public WorldScript
    {
    public:
        SbywowBridgeWorldScript() : WorldScript("SbywowBridgeWorldScript", {
            WORLDHOOK_ON_STARTUP,
            WORLDHOOK_ON_SHUTDOWN
        }) {}

        void OnStartup() override
        {
            BridgeServer::Instance().Start();
        }

        void OnShutdown() override
        {
            BridgeServer::Instance().Stop();
        }
    };

    // ---- Unit-level fan-out --------------------------------------------
    //
    // Emits combat.damage_dealt on the attacker (if attached bot) and
    // combat.damage_taken on the victim (if attached bot). At most two
    // events per OnDamage fire — one per side that's a bot. Schools as
    // raw int for brevity; receiver-side mapping is the harness's job.
    class SbywowBridgeUnitScript : public UnitScript
    {
    public:
        SbywowBridgeUnitScript() : UnitScript("SbywowBridgeUnitScript", true, {
            UNITHOOK_ON_DAMAGE
        }) {}

        void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
        {
            if (!attacker || !victim || damage == 0)
                return;
            char const* attackerKind = attacker->ToCreature() ? "creature"
                                     : attacker->ToPlayer()  ? "player"
                                                              : "unit";
            char const* victimKind   = victim->ToCreature()   ? "creature"
                                     : victim->ToPlayer()    ? "player"
                                                              : "unit";

            if (Player* p = attacker->ToPlayer(); p && IsAttachedBot(p))
            {
                json ev = BaseEvent(p, "combat", "damage_dealt");
                ev["victim_guid"] = victim->GetGUID().GetRawValue();
                ev["victim_name"] = victim->GetName();
                ev["victim_kind"] = victimKind;
                ev["damage"]      = damage;
                ev["victim_hp_pct"]  = static_cast<int>(victim->GetHealthPct());
                EmitEvent(p, ev);
            }
            if (Player* p = victim->ToPlayer(); p && IsAttachedBot(p))
            {
                json ev = BaseEvent(p, "combat", "damage_taken");
                ev["attacker_guid"] = attacker->GetGUID().GetRawValue();
                ev["attacker_name"] = attacker->GetName();
                ev["attacker_kind"] = attackerKind;
                ev["damage"]        = damage;
                ev["self_hp_pct"]   = static_cast<int>(p->GetHealthPct());
                EmitEvent(p, ev);
            }
        }
    };

    // ---- Group-level fan-out -------------------------------------------
    //
    // Group lifecycle events (invite, join, leave, leader change) emit on
    // every attached bot in the group EXCEPT the actor — so a bot whose
    // own join triggered the event doesn't see itself as a third party.
    class SbywowBridgeGroupScript : public GroupScript
    {
    public:
        SbywowBridgeGroupScript() : GroupScript("SbywowBridgeGroupScript", {
            GROUPHOOK_ON_INVITE_MEMBER,
            GROUPHOOK_ON_ADD_MEMBER,
            GROUPHOOK_ON_REMOVE_MEMBER,
            GROUPHOOK_ON_CHANGE_LEADER
        }) {}

        void OnInviteMember(Group* group, ObjectGuid guid) override
        {
            // The invitee is `guid`; if it's an attached bot, surface the
            // invite on its session. Inviter is the current group leader
            // for our purposes (AC's hook doesn't pass the inviter).
            if (!group) return;
            Player* invitee = ObjectAccessor::FindPlayer(guid);
            if (!invitee || !IsAttachedBot(invitee))
                return;
            json ev = BaseEvent(invitee, "group", "invited");
            ev["leader_guid"] = group->GetLeaderGUID().GetRawValue();
            if (char const* ln = group->GetLeaderName())
                ev["leader_name"] = ln;
            EmitEvent(invitee, ev);
        }

        void OnAddMember(Group* group, ObjectGuid guid) override
        {
            Player* joiner = ObjectAccessor::FindPlayer(guid);
            std::string joinerName = joiner ? joiner->GetName() : std::string();
            ForEachAttachedBotInGroup(group, guid, [&](Player* bot)
            {
                json ev = BaseEvent(bot, "group", "member_joined");
                ev["member_guid"] = guid.GetRawValue();
                ev["member_name"] = joinerName;
                EmitEvent(bot, ev);
            });
        }

        void OnRemoveMember(Group* group, ObjectGuid guid, RemoveMethod method,
                            ObjectGuid kicker, char const* reason) override
        {
            Player* leaver = ObjectAccessor::FindPlayer(guid);
            std::string leaverName = leaver ? leaver->GetName() : std::string();
            char const* methodName =
                method == GROUP_REMOVEMETHOD_KICK     ? "kicked"   :
                method == GROUP_REMOVEMETHOD_LEAVE    ? "left"     :
                method == GROUP_REMOVEMETHOD_DEFAULT  ? "default"  :
                method == GROUP_REMOVEMETHOD_KICK_LFG ? "kicked_lfg" :
                                                        "unknown";
            ForEachAttachedBotInGroup(group, guid, [&](Player* bot)
            {
                json ev = BaseEvent(bot, "group", "member_left");
                ev["member_guid"] = guid.GetRawValue();
                ev["member_name"] = leaverName;
                ev["method"]      = methodName;
                ev["kicker_guid"] = kicker.GetRawValue();
                if (reason)
                    ev["reason"] = reason;
                EmitEvent(bot, ev);
            });
        }

        void OnChangeLeader(Group* group, ObjectGuid newLeaderGuid,
                            ObjectGuid oldLeaderGuid) override
        {
            Player* newLeader = ObjectAccessor::FindPlayer(newLeaderGuid);
            Player* oldLeader = ObjectAccessor::FindPlayer(oldLeaderGuid);
            std::string newName = newLeader ? newLeader->GetName() : std::string();
            std::string oldName = oldLeader ? oldLeader->GetName() : std::string();
            ForEachAttachedBotInGroup(group, ObjectGuid::Empty, [&](Player* bot)
            {
                json ev = BaseEvent(bot, "group", "leader_changed");
                ev["new_leader_guid"] = newLeaderGuid.GetRawValue();
                ev["new_leader_name"] = newName;
                ev["old_leader_guid"] = oldLeaderGuid.GetRawValue();
                ev["old_leader_name"] = oldName;
                EmitEvent(bot, ev);
            });
        }
    };

    class SbywowBridgePlayerScript : public PlayerScript
    {
    public:
        SbywowBridgePlayerScript() : PlayerScript("SbywowBridgePlayerScript", {
            PLAYERHOOK_ON_LOGIN,
            PLAYERHOOK_ON_LOGOUT,
            PLAYERHOOK_ON_UPDATE,
            PLAYERHOOK_ON_PLAYER_JUST_DIED,
            PLAYERHOOK_ON_LEVEL_CHANGED,
            PLAYERHOOK_ON_MAP_CHANGED,
            PLAYERHOOK_ON_UPDATE_ZONE,
            PLAYERHOOK_ON_UPDATE_AREA,
            PLAYERHOOK_ON_PVP_KILL,
            PLAYERHOOK_ON_CREATURE_KILL,
            PLAYERHOOK_ON_PLAYER_PVP_FLAG_CHANGE,
            PLAYERHOOK_ON_MONEY_CHANGED,
            PLAYERHOOK_ON_BEFORE_LOOT_MONEY,
            PLAYERHOOK_ON_GIVE_EXP,
            PLAYERHOOK_ON_LEARN_SPELL,
            PLAYERHOOK_ON_FORGOT_SPELL,
            PLAYERHOOK_ON_REPUTATION_CHANGE,
            PLAYERHOOK_ON_DUEL_REQUEST,
            PLAYERHOOK_ON_DUEL_START,
            PLAYERHOOK_ON_DUEL_END,
            PLAYERHOOK_ON_EMOTE,
            PLAYERHOOK_ON_TEXT_EMOTE,
            PLAYERHOOK_ON_SPELL_CAST,
            PLAYERHOOK_ON_LOOT_ITEM,
            PLAYERHOOK_ON_STORE_NEW_ITEM,
            PLAYERHOOK_ON_AFTER_MOVE_ITEM_FROM_INVENTORY,
            PLAYERHOOK_ON_EQUIP,
            PLAYERHOOK_ON_UNEQUIP_ITEM,
            PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST,
            PLAYERHOOK_CAN_PLAYER_USE_CHAT,
            PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT,
            PLAYERHOOK_CAN_PLAYER_USE_GROUP_CHAT,
            PLAYERHOOK_CAN_PLAYER_USE_GUILD_CHAT,
            PLAYERHOOK_CAN_PLAYER_USE_CHANNEL_CHAT,
            PLAYERHOOK_ON_BEFORE_BUY_ITEM_FROM_VENDOR,
            PLAYERHOOK_ON_AFTER_STORE_OR_EQUIP_NEW_ITEM
        }) {}

        void OnPlayerLogin(Player* player) override
        {
            // Bots attach via PlayerbotsMgr's OnBotLogin path, not here —
            // deferred to OnPlayerUpdate where the AI is reliably present.
            // We use this hook only for master.online_changed=true on any
            // already-attached bots whose master just came back online.
            ForEachBondedBot(player, [&](Player* bot) {
                json ev = BaseEvent(bot, "master", "online_changed");
                ev["master_guid"] = player->GetGUID().GetRawValue();
                ev["master_name"] = player->GetName();
                ev["online"]      = true;
                EmitEvent(bot, ev);
            });
        }

        void OnPlayerLogout(Player* player) override
        {
            // Master logout fan-out fires BEFORE bot detach. The bots are
            // still attached at this point — they'll get their own
            // OnPlayerLogout immediately after.
            ForEachBondedBot(player, [&](Player* bot) {
                json ev = BaseEvent(bot, "master", "online_changed");
                ev["master_guid"] = player->GetGUID().GetRawValue();
                ev["master_name"] = player->GetName();
                ev["online"]      = false;
                EmitEvent(bot, ev);
            });

            // Bot self-detach path (filter on bridge session, not
            // IsAttachedBot — at logout the PlayerbotAI may already have
            // been torn down).
            if (!BridgeServer::Instance().GetSession(player->GetGUID()))
                return;
            json ev = BaseEvent(player, "lifecycle", "detached");
            EmitEvent(player, ev);

            // Drop combat state caches for this guid on detach.
            uint64 raw = player->GetGUID().GetRawValue();
            selfInCombatPrev_.erase(raw);
            masterInCombatPrev_.erase(raw);

            BridgeServer::Instance().DetachBot(player->GetGUID());
        }

        void OnPlayerUpdate(Player* player, uint32 /*p_time*/) override
        {
            if (!IsAttachedBot(player))
                return;

            // Idempotent attach. Returns true only on first sight; emit
            // the lifecycle "attached" event with bootstrap context.
            if (BridgeServer::Instance().AttachBot(player))
            {
                json ev = BaseEvent(player, "lifecycle", "attached");
                ev["map"]  = player->GetMapId();
                ev["zone"] = player->GetZoneId();
                ev["pos"]  = { player->GetPositionX(), player->GetPositionY(), player->GetPositionZ() };
                EmitEvent(player, ev);
            }

            BridgeServer::Instance().TickBot(player);

            // Combat-state edge detection on the bot itself + on its
            // master. Both emit on the bot's session: combat.entered/
            // exited for self, master.entered_combat/exited_combat for
            // master. Per-tick delta vs previous-tick state cached by
            // bot guid; cleared on detach.
            uint64 botRaw = player->GetGUID().GetRawValue();
            bool selfNowCombat = player->IsInCombat();
            auto [selfIt, selfNew] = selfInCombatPrev_.try_emplace(botRaw, selfNowCombat);
            if (!selfNew && selfIt->second != selfNowCombat)
            {
                json ev = BaseEvent(player, "combat",
                                    selfNowCombat ? "entered" : "exited");
                EmitEvent(player, ev);
            }
            selfIt->second = selfNowCombat;

            if (PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(player))
            {
                if (Player* master = ai->GetMaster())
                {
                    bool masterNowCombat = master->IsInCombat();
                    auto [mIt, mNew] = masterInCombatPrev_.try_emplace(botRaw, masterNowCombat);
                    if (!mNew && mIt->second != masterNowCombat)
                    {
                        json ev = BaseEvent(player, "master",
                            masterNowCombat ? "entered_combat" : "exited_combat");
                        ev["master_guid"] = master->GetGUID().GetRawValue();
                        ev["master_name"] = master->GetName();
                        EmitEvent(player, ev);
                    }
                    mIt->second = masterNowCombat;
                }
            }

            // Periodic state snapshot. Carries the full structured
            // context block — same shape served by `get_context` and
            // embedded in `inspect`. The agent harness keeps the most
            // recent snapshot warm in memory and reads it on every
            // wake, never having to call a verb to learn its own HP /
            // gold / inventory / master state. See decisions.md
            // "Tool surface is for actions and deep discovery"
            // (2026-05-02) for the architectural reasoning.
            uint32& tick = updateTickByGuid_[player->GetGUID().GetRawValue()];
            uint32 cadence = BridgeServer::Instance().Config().snapshotEveryNUpdates;
            if (cadence == 0) cadence = 1;
            if (++tick % cadence != 0)
                return;

            auto session = BridgeServer::Instance().GetSession(player->GetGUID());
            if (!session)
                return;

            json ev = BaseEvent(player, "snapshot", "state");
            json ctx = Sbywow::Bridge::BuildContextSnapshot(player, *session);
            // Merge the context block fields directly into the event
            // envelope. Top-level shape is {channel, kind, bot_guid,
            // bot_name, self, master, inventory, group, active_intents,
            // session}.
            for (auto it = ctx.begin(); it != ctx.end(); ++it)
                ev[it.key()] = std::move(it.value());
            EmitEvent(player, ev);
        }

        void OnPlayerJustDied(Player* player) override
        {
            if (IsAttachedBot(player))
            {
                json ev = BaseEvent(player, "combat", "died");
                EmitEvent(player, ev);
            }
            // Master.died fan-out: emit on every bonded bot.
            ForEachBondedBot(player, [&](Player* bot) {
                json ev = BaseEvent(bot, "master", "died");
                ev["master_guid"] = player->GetGUID().GetRawValue();
                ev["master_name"] = player->GetName();
                EmitEvent(bot, ev);
            });
        }

        void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override
        {
            if (IsAttachedBot(player))
            {
                json ev = BaseEvent(player, "progression", "level_changed");
                ev["old_level"] = static_cast<int>(oldLevel);
                ev["new_level"] = static_cast<int>(player->GetLevel());
                EmitEvent(player, ev);
            }
            // Master.level_changed fan-out — agent might want to
            // re-evaluate sync-up actions when master dings.
            ForEachBondedBot(player, [&](Player* bot) {
                json ev = BaseEvent(bot, "master", "level_changed");
                ev["master_guid"] = player->GetGUID().GetRawValue();
                ev["master_name"] = player->GetName();
                ev["old_level"]   = static_cast<int>(oldLevel);
                ev["new_level"]   = static_cast<int>(player->GetLevel());
                EmitEvent(bot, ev);
            });
        }

        void OnPlayerMapChanged(Player* player) override
        {
            if (!IsAttachedBot(player))
                return;
            json ev = BaseEvent(player, "world", "map_changed");
            ev["map"]  = player->GetMapId();
            ev["zone"] = player->GetZoneId();
            EmitEvent(player, ev);
        }

        // ---- world ----------------------------------------------------

        void OnPlayerUpdateZone(Player* player, uint32 newZone, uint32 newArea) override
        {
            if (!IsAttachedBot(player))
                return;
            json ev = BaseEvent(player, "world", "zone_changed");
            ev["new_zone"] = newZone;
            ev["new_area"] = newArea;
            EmitEvent(player, ev);
        }

        void OnPlayerUpdateArea(Player* player, uint32 oldArea, uint32 newArea) override
        {
            if (!IsAttachedBot(player))
                return;
            json ev = BaseEvent(player, "world", "area_changed");
            ev["old_area"] = oldArea;
            ev["new_area"] = newArea;
            EmitEvent(player, ev);
        }

        // ---- combat ---------------------------------------------------

        void OnPlayerPVPKill(Player* killer, Player* killed) override
        {
            if (!IsAttachedBot(killer))
                return;
            json ev = BaseEvent(killer, "combat", "killed_player");
            ev["victim_guid"] = killed->GetGUID().GetRawValue();
            ev["victim_name"] = killed->GetName();
            EmitEvent(killer, ev);
        }

        void OnPlayerCreatureKill(Player* killer, Creature* killed) override
        {
            if (!IsAttachedBot(killer))
                return;
            json ev = BaseEvent(killer, "combat", "killed_creature");
            ev["victim_guid"]  = killed->GetGUID().GetRawValue();
            ev["victim_name"]  = killed->GetName();
            ev["victim_entry"] = killed->GetEntry();
            EmitEvent(killer, ev);
        }

        // killed_by_creature removed: redundant with combat.died.
        // Attacker correlation lives on the combat.damage_taken event
        // that fires immediately before the lethal blow.

        void OnPlayerPVPFlagChange(Player* player, bool state) override
        {
            if (!IsAttachedBot(player))
                return;
            json ev = BaseEvent(player, "status", "pvp_flag_changed");
            ev["flagged"] = state;
            EmitEvent(player, ev);
        }

        // ---- progression ---------------------------------------------

        void OnPlayerMoneyChanged(Player* player, int32& amount) override
        {
            if (!IsAttachedBot(player))
                return;
            json ev = BaseEvent(player, "progression", "money_changed");
            ev["delta"] = amount;
            EmitEvent(player, ev);
        }

        void OnPlayerGiveXP(Player* player, uint32& amount, Unit* victim, uint8 xpSource) override
        {
            if (!IsAttachedBot(player))
                return;
            json ev = BaseEvent(player, "progression", "xp_gained");
            ev["amount"] = amount;
            ev["source"] = static_cast<int>(xpSource);
            if (victim)
                ev["victim_name"] = victim->GetName();
            EmitEvent(player, ev);
        }

        void OnPlayerLearnSpell(Player* player, uint32 spellID) override
        {
            if (!IsAttachedBot(player))
                return;
            json ev = BaseEvent(player, "progression", "spell_learned");
            ev["spell_id"] = spellID;
            EmitEvent(player, ev);
        }

        void OnPlayerForgotSpell(Player* player, uint32 spellID) override
        {
            if (!IsAttachedBot(player))
                return;
            json ev = BaseEvent(player, "progression", "spell_forgot");
            ev["spell_id"] = spellID;
            EmitEvent(player, ev);
        }

        bool OnPlayerReputationChange(Player* player, uint32 factionID, int32& standing,
                                      bool incremental) override
        {
            if (IsAttachedBot(player))
            {
                json ev = BaseEvent(player, "progression", "reputation_changed");
                ev["faction_id"]  = factionID;
                ev["standing"]    = standing;
                ev["incremental"] = incremental;
                EmitEvent(player, ev);
            }
            return true;  // do not block the change
        }

        void OnPlayerCompleteQuest(Player* player, Quest const* quest) override
        {
            if (!IsAttachedBot(player))
                return;
            json ev = BaseEvent(player, "quest", "completed");
            if (quest)
                ev["quest_id"] = quest->GetQuestId();
            EmitEvent(player, ev);
        }

        // ---- social --------------------------------------------------

        void OnPlayerDuelRequest(Player* target, Player* challenger) override
        {
            if (!IsAttachedBot(target))
                return;
            json ev = BaseEvent(target, "duel", "requested");
            ev["challenger_guid"] = challenger->GetGUID().GetRawValue();
            ev["challenger_name"] = challenger->GetName();
            EmitEvent(target, ev);
        }

        void OnPlayerDuelStart(Player* p1, Player* p2) override
        {
            // Fire for whichever side is an attached bot. Both could be.
            for (Player* p : { p1, p2 })
            {
                if (!IsAttachedBot(p))
                    continue;
                Player* other = (p == p1) ? p2 : p1;
                json ev = BaseEvent(p, "duel", "started");
                ev["other_guid"] = other->GetGUID().GetRawValue();
                ev["other_name"] = other->GetName();
                EmitEvent(p, ev);
            }
        }

        void OnPlayerDuelEnd(Player* winner, Player* loser, DuelCompleteType type) override
        {
            for (Player* p : { winner, loser })
            {
                if (!IsAttachedBot(p))
                    continue;
                json ev = BaseEvent(p, "duel", "ended");
                ev["winner_guid"] = winner->GetGUID().GetRawValue();
                ev["loser_guid"]  = loser->GetGUID().GetRawValue();
                ev["complete_type"] = static_cast<int>(type);
                EmitEvent(p, ev);
            }
        }

        void OnPlayerEmote(Player* player, uint32 emote) override
        {
            if (!IsAttachedBot(player))
                return;
            json ev = BaseEvent(player, "social", "emoted");
            ev["emote_id"] = emote;
            EmitEvent(player, ev);
        }

        void OnPlayerTextEmote(Player* player, uint32 textEmote, uint32 emoteNum,
                               ObjectGuid guid) override
        {
            if (!IsAttachedBot(player))
                return;
            json ev = BaseEvent(player, "social", "text_emoted");
            ev["text_emote"] = textEmote;
            ev["emote_num"]  = emoteNum;
            ev["target_guid"] = guid.GetRawValue();
            EmitEvent(player, ev);
        }

        void OnPlayerSpellCast(Player* player, Spell* spell, bool /*skipCheck*/) override
        {
            if (!IsAttachedBot(player))
                return;
            json ev = BaseEvent(player, "combat", "spell_cast");
            if (spell && spell->GetSpellInfo())
                ev["spell_id"] = spell->GetSpellInfo()->Id;
            EmitEvent(player, ev);
        }

        // ---- inventory -----------------------------------------------

        void OnPlayerLootItem(Player* player, Item* item, uint32 count, ObjectGuid lootguid) override
        {
            if (!IsAttachedBot(player))
                return;
            json ev = BaseEvent(player, "inventory", "looted_item");
            if (item)
            {
                ev["entry"] = item->GetEntry();
                if (auto const* tpl = item->GetTemplate())
                    ev["name"] = tpl->Name1;
            }
            ev["count"]     = count;
            ev["loot_guid"] = lootguid.GetRawValue();
            EmitEvent(player, ev);
        }

        void OnPlayerStoreNewItem(Player* player, Item* item, uint32 count) override
        {
            if (!IsAttachedBot(player) || !item)
                return;
            json ev = BaseEvent(player, "inventory", "received");
            ev["entry"] = item->GetEntry();
            ev["count"] = count;
            if (auto const* tpl = item->GetTemplate())
            {
                ev["name"]    = tpl->Name1;
                ev["quality"] = static_cast<int>(tpl->Quality);
            }
            EmitEvent(player, ev);
        }

        void OnPlayerAfterMoveItemFromInventory(Player* player, Item* item,
                                                uint8 bag, uint8 slot, bool /*update*/) override
        {
            if (!IsAttachedBot(player) || !item)
                return;
            json ev = BaseEvent(player, "inventory", "lost");
            ev["entry"] = item->GetEntry();
            ev["count"] = item->GetCount();
            ev["bag"]   = bag;
            ev["slot"]  = slot;
            if (auto const* tpl = item->GetTemplate())
                ev["name"] = tpl->Name1;
            EmitEvent(player, ev);
        }

        void OnPlayerEquip(Player* player, Item* item, uint8 /*bag*/, uint8 slot,
                           bool /*update*/) override
        {
            if (!IsAttachedBot(player) || !item)
                return;
            json ev = BaseEvent(player, "inventory", "equipped");
            ev["entry"] = item->GetEntry();
            ev["slot"]  = slot;
            if (auto const* tpl = item->GetTemplate())
            {
                ev["name"]    = tpl->Name1;
                ev["quality"] = static_cast<int>(tpl->Quality);
                ev["ilvl"]    = tpl->ItemLevel;
            }
            EmitEvent(player, ev);
        }

        void OnPlayerUnequip(Player* player, Item* item) override
        {
            if (!IsAttachedBot(player) || !item)
                return;
            json ev = BaseEvent(player, "inventory", "unequipped");
            ev["entry"] = item->GetEntry();
            if (auto const* tpl = item->GetTemplate())
                ev["name"] = tpl->Name1;
            EmitEvent(player, ev);
        }

        void OnPlayerBeforeLootMoney(Player* player, Loot* loot) override
        {
            if (!IsAttachedBot(player) || !loot)
                return;
            json ev = BaseEvent(player, "inventory", "looted_money");
            ev["copper"] = loot->gold;
            EmitEvent(player, ev);
        }

        // ---- vendor (buy path) -------------------------------------------

        void OnPlayerBeforeBuyItemFromVendor(Player* player, ObjectGuid vendorguid,
                                             uint32 vendorslot, uint32& item, uint8 count,
                                             uint8 bag, uint8 slot) override
        {
            if (!IsAttachedBot(player))
                return;
            json ev = BaseEvent(player, "vendor", "purchase_attempted");
            ev["vendor_guid"] = vendorguid.GetRawValue();
            ev["vendor_slot"] = vendorslot;
            ev["item_entry"]  = item;
            ev["count"]       = count;
            ev["dest_bag"]    = bag;
            ev["dest_slot"]   = slot;
            EmitEvent(player, ev);
        }

        void OnPlayerAfterStoreOrEquipNewItem(Player* player, uint32 vendorslot,
                                              Item* item, uint8 count, uint8 bag, uint8 slot,
                                              ItemTemplate const* /*pProto*/, Creature* pVendor,
                                              VendorItem const* /*crItem*/, bool bStore) override
        {
            // Only emit for actual vendor purchases. The same hook fires
            // for some in-store transitions; pVendor distinguishes.
            if (!IsAttachedBot(player) || !pVendor || !item)
                return;
            json ev = BaseEvent(player, "vendor", "purchase_completed");
            ev["vendor_guid"] = pVendor->GetGUID().GetRawValue();
            ev["vendor_name"] = pVendor->GetName();
            ev["vendor_slot"] = vendorslot;
            ev["entry"]       = item->GetEntry();
            ev["count"]       = count;
            ev["dest_bag"]    = bag;
            ev["dest_slot"]   = slot;
            ev["stored"]      = bStore;
            if (auto const* tpl = item->GetTemplate())
                ev["name"] = tpl->Name1;
            EmitEvent(player, ev);
        }

        // ---- chat (inbound + outbound) ----------------------------------
        //
        // AC has no receiver-side chat hook. We hook the *sender* via
        // OnPlayerCanUseChat overloads (basic / private / group / guild
        // / channel) — these are the hooks that *actually* fire when chat
        // is sent (the older PLAYERHOOK_ON_CHAT_* family is unused). For
        // each variant: emit chat.sent on the sender if it's an attached
        // bot, AND emit a chat.<x>_received event on attached recipients.
        // mod-playerbots itself overrides the same hook for command
        // routing; both fire via ScriptMgr without ordering guarantees.
        // Always return true (we're observers).

        bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang,
                                std::string& msg) override
        {
            // Basic — fires from Player::Say / Yell / TextEmote. The
            // bot's outbound say/yell goes through this path. There's
            // no "received" semantic for /say / /yell here because the
            // broadcast happens after, so we only emit chat.sent.
            if (player && IsAttachedBot(player))
            {
                json ev = BaseEvent(player, "chat", "sent");
                ev["chat_type"] = static_cast<int>(type);
                ev["language"]  = ChatLanguageName(lang);
                ev["msg"]       = msg;
                EmitEvent(player, ev);
            }
            return true;
        }

        bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang,
                                std::string& msg, Player* receiver) override
        {
            // Whisper. chat.sent on bot-as-sender; chat.whisper_received
            // on bot-as-receiver. Both can fire if both sides are bots.
            if (player && IsAttachedBot(player))
            {
                json ev = BaseEvent(player, "chat", "sent");
                ev["chat_type"]   = static_cast<int>(type);
                ev["language"]    = ChatLanguageName(lang);
                ev["msg"]         = msg;
                ev["target_guid"] = receiver ? receiver->GetGUID().GetRawValue() : 0ULL;
                ev["target_name"] = receiver ? receiver->GetName() : "";
                EmitEvent(player, ev);
            }
            if (receiver && type == CHAT_MSG_WHISPER && IsAttachedBot(receiver))
            {
                json ev = BaseEvent(receiver, "chat", "whisper_received");
                ev["sender_guid"] = player ? player->GetGUID().GetRawValue() : 0ULL;
                ev["sender_name"] = player ? player->GetName() : "";
                ev["msg"]         = msg;
                ev["language"]    = ChatLanguageName(lang);
                EmitEvent(receiver, ev);
            }
            return true;
        }

        bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang,
                                std::string& msg, Group* group) override
        {
            // Party / raid. chat.sent on bot-as-sender; chat.party_received
            // on every attached bot in the group except the sender.
            if (player && IsAttachedBot(player))
            {
                json ev = BaseEvent(player, "chat", "sent");
                ev["chat_type"] = static_cast<int>(type);
                ev["language"]  = ChatLanguageName(lang);
                ev["msg"]       = msg;
                EmitEvent(player, ev);
            }
            ObjectGuid skipGuid = player ? player->GetGUID() : ObjectGuid::Empty;
            ForEachAttachedBotInGroup(group, skipGuid, [&](Player* bot) {
                json ev = BaseEvent(bot, "chat", "party_received");
                ev["sender_guid"] = player ? player->GetGUID().GetRawValue() : 0ULL;
                ev["sender_name"] = player ? player->GetName() : "";
                ev["chat_type"]   = static_cast<int>(type);
                ev["msg"]         = msg;
                ev["language"]    = ChatLanguageName(lang);
                EmitEvent(bot, ev);
            });
            return true;
        }

        bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang,
                                std::string& msg, Guild* guild) override
        {
            if (player && IsAttachedBot(player))
            {
                json ev = BaseEvent(player, "chat", "sent");
                ev["chat_type"] = static_cast<int>(type);
                ev["language"]  = ChatLanguageName(lang);
                ev["msg"]       = msg;
                EmitEvent(player, ev);
            }
            if (!guild) return true;
            ObjectGuid skipGuid = player ? player->GetGUID() : ObjectGuid::Empty;
            BridgeServer::Instance().ForEachAttachedBot([&](Player* bot) {
                if (bot->GetGUID() == skipGuid)         return;
                if (bot->GetGuildId() != guild->GetId()) return;
                json ev = BaseEvent(bot, "chat", "guild_received");
                ev["sender_guid"] = player ? player->GetGUID().GetRawValue() : 0ULL;
                ev["sender_name"] = player ? player->GetName() : "";
                ev["msg"]         = msg;
                ev["language"]    = ChatLanguageName(lang);
                EmitEvent(bot, ev);
            });
            return true;
        }

        bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang,
                                std::string& msg, Channel* channel) override
        {
            if (player && IsAttachedBot(player))
            {
                json ev = BaseEvent(player, "chat", "sent");
                ev["chat_type"]    = static_cast<int>(type);
                ev["language"]     = ChatLanguageName(lang);
                ev["msg"]          = msg;
                if (channel)
                    ev["channel_name"] = channel->GetName();
                EmitEvent(player, ev);
            }
            if (!channel) return true;
            ObjectGuid skipGuid = player ? player->GetGUID() : ObjectGuid::Empty;
            BridgeServer::Instance().ForEachAttachedBot([&](Player* bot) {
                if (bot->GetGUID() == skipGuid)     return;
                if (!bot->IsInChannel(channel))     return;
                json ev = BaseEvent(bot, "chat", "channel_received");
                ev["sender_guid"]  = player ? player->GetGUID().GetRawValue() : 0ULL;
                ev["sender_name"]  = player ? player->GetName() : "";
                ev["channel_name"] = channel->GetName();
                ev["msg"]          = msg;
                ev["language"]     = ChatLanguageName(lang);
                EmitEvent(bot, ev);
            });
            return true;
        }

    private:
        // Tracks per-bot update count for the snapshot cadence. Cleared
        // implicitly when we never look up a detached guid again; if the
        // map ever balloons we can clear in OnPlayerLogout, but at our
        // scale it's noise.
        std::unordered_map<uint64, uint32> updateTickByGuid_;

        // Combat-state edge detection caches. Per attached-bot guid:
        //   - selfInCombatPrev_:    last seen in_combat for the bot itself
        //   - masterInCombatPrev_:  last seen in_combat for the bot's master
        // Updated every OnPlayerUpdate; emit on transitions; cleared on
        // detach. The first OnPlayerUpdate after attach seeds the cache
        // (no spurious "entered" event on first seen).
        std::unordered_map<uint64, bool> selfInCombatPrev_;
        std::unordered_map<uint64, bool> masterInCombatPrev_;
    };
}

void AddSC_SbywowBridgeScripts()
{
    new SbywowBridgeWorldScript();
    new SbywowBridgePlayerScript();
    new SbywowBridgeUnitScript();
    new SbywowBridgeGroupScript();
}
