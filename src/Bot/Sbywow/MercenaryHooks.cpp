#include "MercenaryMgr.h"

#include "CharacterCache.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotFactory.h"
#include "PlayerbotMgr.h"
#include "Playerbots.h"  // GET_PLAYERBOT_MGR
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "WorldSession.h"

#include <string>

class SbywowMercenaryPlayerScript : public PlayerScript
{
public:
    SbywowMercenaryPlayerScript() : PlayerScript("SbywowMercenaryPlayerScript", {
        PLAYERHOOK_ON_LEVEL_CHANGED,
        PLAYERHOOK_ON_DELETE,
        PLAYERHOOK_ON_DELETE_FROM_DB,
        PLAYERHOOK_ON_MAP_CHANGED
    }) {}

    void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override
    {
        // Filter: real players only. Mercs are bots themselves; if a merc
        // levels (e.g. via XP gained while following), we don't want to
        // recurse into level-syncing them as if they were owners.
        if (!player || !player->GetSession() || player->GetSession()->IsBot())
            return;

        uint8 newLevel = player->GetLevel();
        if (newLevel == oldLevel)
            return;

        for (ObjectGuid mercGuid : sMercenaryMgr.GetMercsForOwner(player->GetGUID()))
        {
            Player* merc = ObjectAccessor::FindConnectedPlayer(mercGuid);
            if (!merc)
                continue;  // offline merc — owner can `.merc summon <name>` then `.merc resync <name>` to pull the level up

            if (merc->GetLevel() != newLevel)
                merc->GiveLevel(newLevel);
            PlayerbotFactory(merc, newLevel, ITEM_QUALITY_EPIC).Randomize(true);
        }
    }

    // Unified merc world-boundary handler. Fires after the owner's TeleportTo
    // completes (cross-zone, continent, instance entry/exit, BG/Arena entry/exit).
    //
    // Decision tree per owned merc:
    //   - Owner now in BG/Arena → despawn online mercs (no PvP-bracket cheese).
    //   - Owner now elsewhere   → re-summon offline mercs (post-BG re-entry,
    //                              or post-instance return) and teleport
    //                              online mercs to owner's new map.
    //
    // Same-map intra-zone movement is a no-op (merc->GetMapId() == map id).
    void OnPlayerMapChanged(Player* player) override
    {
        if (!player || !player->GetSession() || player->GetSession()->IsBot())
            return;

        std::vector<ObjectGuid> mercs = sMercenaryMgr.GetMercsForOwner(player->GetGUID());
        if (mercs.empty())
            return;

        Map* destMap = player->GetMap();
        if (!destMap)
            return;

        PlayerbotMgr* mgr = GET_PLAYERBOT_MGR(player);
        if (!mgr)
            return;  // Owner has no PlayerbotMgr — nothing we can do here.

        bool const isPvP = destMap->IsBattlegroundOrArena();

        for (ObjectGuid mercGuid : mercs)
        {
            Player* merc = ObjectAccessor::FindConnectedPlayer(mercGuid);

            if (isPvP)
            {
                // Despawn online mercs entering PvP. Arena's BGJoinAction
                // teleports group members in for free; this hook tears them
                // back out. BGs typically don't TP group members but we
                // despawn anyway for consistency (no merc presence during PvP).
                if (merc)
                {
                    LOG_INFO("server.misc",
                        "Sbywow: owner '{}' entered PvP map {} — despawning merc '{}'",
                        player->GetName(), destMap->GetId(), merc->GetName());
                    mgr->LogoutPlayerBot(mercGuid);
                }
                continue;
            }

            if (!merc)
            {
                // Merc not in world — re-summon. Two common causes:
                //   (a) Owner just exited BG/Arena, mercs were despawned on entry.
                //   (b) Hire happened while owner was in PvP / autologin missed.
                std::string name;
                sCharacterCache->GetCharacterNameByGuid(mercGuid, name);
                if (name.empty())
                    continue;
                std::string cmd = "add " + name;
                mgr->HandlePlayerbotCommand(cmd.c_str(), player);
                continue;
            }

            // Merc online: teleport if on a different map. (Same-map intra-zone
            // movement is handled by FollowAction; we don't want to spam-TP.)
            if (merc->GetMapId() != player->GetMapId())
            {
                LOG_INFO("server.misc",
                    "Sbywow: teleporting merc '{}' from map {} to owner '{}' on map {}",
                    merc->GetName(), merc->GetMapId(), player->GetName(), player->GetMapId());
                merc->TeleportTo(player->GetMapId(),
                                 player->GetPositionX(),
                                 player->GetPositionY(),
                                 player->GetPositionZ(),
                                 player->GetOrientation());
            }
        }
    }

    void OnPlayerDelete(ObjectGuid guid, uint32 /*accountId*/) override
    {
        // Player char-select delete path. Fires BEFORE Player::DeleteFromDB
        // runs, so it's safe to enumerate owned mercs and dismiss them —
        // DismissMerc cascades each merc through its own DeleteFromDB on the
        // service account. NOTE: this hook does NOT fire for GM `.character
        // erase`; that path triggers OnPlayerDeleteFromDB instead. For GM
        // erase of an owner-with-mercs, the merc characters are orphaned on
        // the service account until orphan reaper sweep 1 catches them
        // (cleanup happens at next restart or via `.merc admin reap`).
        std::vector<ObjectGuid> owned = sMercenaryMgr.GetMercsForOwner(guid);
        for (ObjectGuid mercGuid : owned)
            sMercenaryMgr.DismissMerc(mercGuid);
    }

    void OnPlayerDeleteFromDB(CharacterDatabaseTransaction trans, uint32 guid) override
    {
        // Fires INSIDE Player::DeleteFromDB's transaction for any path that
        // hits the full CHAR_DELETE_REMOVE branch — covers `.character erase`,
        // `.merc admin nuke`, char-select delete, etc. We append the
        // ownership-row removal to the same transaction so the cleanup is
        // atomic with the character deletion.
        //
        // Idempotent for non-merc guids (DELETE WHERE returns zero rows).
        // We deliberately do NOT also delete rows where this guid is the
        // OWNER — that would orphan the merc characters on the service
        // account without dismissing them (DismissMerc would be reentrant
        // inside this transaction). Owner-side cascade lives in
        // OnPlayerDelete + the orphan reaper as the safety net.
        trans->Append("DELETE FROM mod_sbywow_mercenaries WHERE merc_guid = {}", guid);
    }
};

class SbywowMercenaryWorldScript : public WorldScript
{
public:
    SbywowMercenaryWorldScript() : WorldScript("SbywowMercenaryWorldScript", {
        WORLDHOOK_ON_STARTUP
    }) {}

    void OnStartup() override
    {
        // World is fully loaded and DBs are ready — safe to bootstrap state.
        sMercenaryMgr.EnsureServiceState();
        // Self-heal: prune any orphan rows accumulated from prior runs.
        // Common causes: GM `.character erase` of an owner-with-mercs (we
        // can't cascade-dismiss inside the deletion transaction; see
        // OnPlayerDeleteFromDB comment), manual SQL, partial CreateMerc
        // failures (process died between SaveToDB and AddOwnership).
        sMercenaryMgr.ReapOrphans();
    }
};

void AddSbywowMercenaryHooks()
{
    new SbywowMercenaryPlayerScript();
    new SbywowMercenaryWorldScript();
}
