#include "MercenaryMgr.h"

#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotFactory.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "WorldSession.h"

class SbywowMercenaryPlayerScript : public PlayerScript
{
public:
    SbywowMercenaryPlayerScript() : PlayerScript("SbywowMercenaryPlayerScript", {
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_LOGOUT,
        PLAYERHOOK_ON_LEVEL_CHANGED,
        PLAYERHOOK_ON_DELETE
    }) {}

    void OnPlayerLogin(Player* /*player*/) override
    {
        // The actual auto-summon on login happens inside the patched
        // PlayerbotMgr::OnPlayerLogin (which queries our table); this hook is
        // a placeholder for future sbywow-side login work.
    }

    void OnPlayerLogout(Player* /*player*/) override
    {
        // Login bots auto-despawn when their master logs out via the existing
        // PlayerbotMgr cascade; we may add bookkeeping here later.
    }

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
                continue;  // offline merc — will pick up new level on .merc resync or next hire-time path

            if (merc->GetLevel() != newLevel)
                merc->GiveLevel(newLevel);
            PlayerbotFactory(merc, newLevel, ITEM_QUALITY_EPIC).Randomize(true);
        }
    }

    void OnPlayerDelete(ObjectGuid /*guid*/, uint32 /*accountId*/) override
    {
        // Cascade: dismiss every merc whose owner_guid matches the deleted char.
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
    }
};

void AddSbywowMercenaryHooks()
{
    new SbywowMercenaryPlayerScript();
    new SbywowMercenaryWorldScript();
}
