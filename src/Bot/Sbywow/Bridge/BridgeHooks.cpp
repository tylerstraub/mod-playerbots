/*
 * sbywow Agent Bridge — script registration.
 *
 * Two scripts:
 *   - SbywowBridgeWorldScript  drives Start/Stop on world startup/shutdown.
 *   - SbywowBridgePlayerScript attaches bot sessions, ticks them every
 *                              update, and fans PlayerScript hooks into
 *                              outbound events for the SSE channel.
 *
 * Events are sourced purely from AC's ScriptMgr surface. The
 * action-listener path (engines->AddActionExecutionListener) was
 * removed alongside seize/release in the SbywowAgentEngine pivot —
 * the agent now owns BOT_STATE_NON_COMBAT directly via engine
 * subclass, so the listener-veto pattern is no longer needed.
 */

#include "BridgeServer.h"
#include "BotSession.h"
#include "deps/json.hpp"

#include "Creature.h"
#include "Player.h"
#include "Playerbots.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "ScriptMgr.h"
#include "Spell.h"

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
            PLAYERHOOK_ON_PLAYER_KILLED_BY_CREATURE,
            PLAYERHOOK_ON_PLAYER_PVP_FLAG_CHANGE,
            PLAYERHOOK_ON_MONEY_CHANGED,
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
            PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST
        }) {}

        void OnPlayerLogin(Player* /*player*/) override
        {
            // No-op intentionally. Playerbots wires PlayerbotAI via a
            // queued OnBotLogin operation that runs *after*
            // ScriptMgr::OnPlayerLogin fires, so the IsAttachedBot filter
            // would reject every bot login here. Attach is deferred to
            // the first OnPlayerUpdate where the AI is reliably present.
        }

        void OnPlayerLogout(Player* player) override
        {
            // Filter on bridge session, not IsAttachedBot — at logout the
            // PlayerbotAI may already have been torn down.
            if (!BridgeServer::Instance().GetSession(player->GetGUID()))
                return;
            json ev = BaseEvent(player, "lifecycle", "detached");
            EmitEvent(player, ev);

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

            // Periodic state snapshot.
            uint32& tick = updateTickByGuid_[player->GetGUID().GetRawValue()];
            uint32 cadence = BridgeServer::Instance().Config().snapshotEveryNUpdates;
            if (cadence == 0) cadence = 1;
            if (++tick % cadence != 0)
                return;

            json ev = BaseEvent(player, "snapshot", "state");
            ev["hp_pct"]    = static_cast<int>(player->GetHealthPct());
            ev["map"]       = player->GetMapId();
            ev["zone"]      = player->GetZoneId();
            ev["pos"]       = { player->GetPositionX(), player->GetPositionY(), player->GetPositionZ() };
            ev["in_combat"] = player->IsInCombat();
            ev["alive"]     = player->IsAlive();
            ev["level"]     = player->GetLevel();
            EmitEvent(player, ev);
        }

        void OnPlayerJustDied(Player* player) override
        {
            if (!IsAttachedBot(player))
                return;
            json ev = BaseEvent(player, "combat", "died");
            EmitEvent(player, ev);
        }

        void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override
        {
            if (!IsAttachedBot(player))
                return;
            json ev = BaseEvent(player, "progression", "level_changed");
            ev["old_level"] = static_cast<int>(oldLevel);
            ev["new_level"] = static_cast<int>(player->GetLevel());
            EmitEvent(player, ev);
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
            json ev = BaseEvent(killer, "combat", "pvp_kill");
            ev["killed_guid"] = killed->GetGUID().GetRawValue();
            ev["killed_name"] = killed->GetName();
            EmitEvent(killer, ev);
        }

        void OnPlayerCreatureKill(Player* killer, Creature* killed) override
        {
            if (!IsAttachedBot(killer))
                return;
            json ev = BaseEvent(killer, "combat", "creature_kill");
            ev["entry"] = killed->GetEntry();
            ev["name"]  = killed->GetName();
            EmitEvent(killer, ev);
        }

        void OnPlayerKilledByCreature(Creature* killer, Player* killed) override
        {
            if (!IsAttachedBot(killed))
                return;
            json ev = BaseEvent(killed, "combat", "killed_by_creature");
            if (killer)
            {
                ev["entry"] = killer->GetEntry();
                ev["name"]  = killer->GetName();
            }
            EmitEvent(killed, ev);
        }

        void OnPlayerPVPFlagChange(Player* player, bool state) override
        {
            if (!IsAttachedBot(player))
                return;
            json ev = BaseEvent(player, "combat", "pvp_flag_change");
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
                json ev = BaseEvent(player, "progression", "rep_change");
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
            json ev = BaseEvent(player, "progression", "quest_complete");
            if (quest)
                ev["quest_id"] = quest->GetQuestId();
            EmitEvent(player, ev);
        }

        // ---- social --------------------------------------------------

        void OnPlayerDuelRequest(Player* target, Player* challenger) override
        {
            if (!IsAttachedBot(target))
                return;
            json ev = BaseEvent(target, "social", "duel_request");
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
                json ev = BaseEvent(p, "social", "duel_start");
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
                json ev = BaseEvent(p, "social", "duel_end");
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
            json ev = BaseEvent(player, "social", "emote");
            ev["emote_id"] = emote;
            EmitEvent(player, ev);
        }

        void OnPlayerTextEmote(Player* player, uint32 textEmote, uint32 emoteNum,
                               ObjectGuid guid) override
        {
            if (!IsAttachedBot(player))
                return;
            json ev = BaseEvent(player, "social", "text_emote");
            ev["text_emote"] = textEmote;
            ev["emote_num"]  = emoteNum;
            ev["target_guid"] = guid.GetRawValue();
            EmitEvent(player, ev);
        }

        void OnPlayerSpellCast(Player* player, Spell* spell, bool /*skipCheck*/) override
        {
            if (!IsAttachedBot(player))
                return;
            json ev = BaseEvent(player, "social", "spell_cast");
            if (spell && spell->GetSpellInfo())
                ev["spell_id"] = spell->GetSpellInfo()->Id;
            EmitEvent(player, ev);
        }

        // ---- inventory -----------------------------------------------

        void OnPlayerLootItem(Player* player, Item* item, uint32 count, ObjectGuid lootguid) override
        {
            if (!IsAttachedBot(player))
                return;
            json ev = BaseEvent(player, "inventory", "loot_item");
            if (item)
                ev["item_entry"] = item->GetEntry();
            ev["count"]     = count;
            ev["loot_guid"] = lootguid.GetRawValue();
            EmitEvent(player, ev);
        }

    private:
        // Tracks per-bot update count for the snapshot cadence. Cleared
        // implicitly when we never look up a detached guid again; if the
        // map ever balloons we can clear in OnPlayerLogout, but at our
        // scale it's noise.
        std::unordered_map<uint64, uint32> updateTickByGuid_;
    };
}

void AddSC_SbywowBridgeScripts()
{
    new SbywowBridgeWorldScript();
    new SbywowBridgePlayerScript();
}
