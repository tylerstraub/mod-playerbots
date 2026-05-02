#include "MercenaryFactory.h"

#include "CharacterCache.h"
#include "DatabaseEnv.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Log.h"
#include "MercenaryMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotFactory.h"
#include "PlayerbotMgr.h"
#include "Playerbots.h"  // GET_PLAYERBOT_MGR macro
#include "RandomPlayerbotFactory.h"
#include "SharedDefines.h"
#include "WorldSession.h"

#include <chrono>
#include <thread>
#include <unordered_map>
#include <vector>

ObjectGuid MercenaryFactory::CreateMerc(ObjectGuid ownerGuid, uint8 classId,
                                        std::string const& desiredName)
{
    using namespace std::chrono_literals;

    if (ownerGuid.IsEmpty())
        return ObjectGuid();

    uint32 serviceAccountId = sMercenaryMgr.GetServiceAccountId();
    if (serviceAccountId == 0)
    {
        LOG_ERROR("server.misc",
            "Sbywow: CreateMerc called before service account is bootstrapped");
        return ObjectGuid();
    }

    // Pre-flight name validation. If the caller supplied a name, fail
    // fast on bad-length / collision rather than wasting a CreateRandomBot
    // and discovering it at SaveToDB. Empty string preserves prior
    // behavior (factory generates a random name).
    if (!desiredName.empty())
    {
        if (desiredName.size() < 2 || desiredName.size() > 12)
        {
            LOG_WARN("server.misc",
                "Sbywow: rejecting merc name '{}' — must be 2..12 chars",
                desiredName);
            return ObjectGuid();
        }
        std::string esc = desiredName;
        CharacterDatabase.EscapeString(esc);
        QueryResult collision = CharacterDatabase.Query(
            "SELECT 1 FROM characters WHERE name = '{}'", esc);
        if (collision)
        {
            LOG_WARN("server.misc",
                "Sbywow: rejecting merc name '{}' — already in use",
                desiredName);
            return ObjectGuid();
        }
    }

    // Temporary session against the service account. Lifetime ends with this
    // function — the merc character will be loaded fresh by the playerbot
    // login flow when its owner next logs in.
    WorldSession* session = new WorldSession(
        serviceAccountId, "", 0x0, nullptr, SEC_PLAYER,
        EXPANSION_WRATH_OF_THE_LICH_KING, time_t(0), LOCALE_enUS, 0,
        false, false, 0, true);

    // Reuse the existing factory's race/name/appearance roll. Empty nameCache
    // forces it to fall through to CreateRandomBotName (the playerbots_names
    // table query / conlang generator).
    std::unordered_map<RandomPlayerbotFactory::NameRaceAndGender, std::vector<std::string>> emptyCache;
    RandomPlayerbotFactory factory;
    Player* merc = factory.CreateRandomBot(session, classId, emptyCache);

    if (!merc)
    {
        LOG_ERROR("server.misc",
            "Sbywow: CreateRandomBot failed for class {}", uint32(classId));
        delete session;
        return ObjectGuid();
    }

    ObjectGuid mercGuid = merc->GetGUID();

    // Apply the desired name in-memory before SaveToDB so the rename
    // persists. Object::SetName is a simple m_name = ...; the merc has
    // not been added to any session/guild/cache yet, so no other place
    // is holding the old name.
    if (!desiredName.empty())
        merc->SetName(desiredName);
    std::string mercName = merc->GetName();

    // Level + gear sync at hire if owner is online. Offline-hire mercs stay at
    // the random factory's default level until first .merc resync or the next
    // owner-side level event.
    Player* onlineOwner = ObjectAccessor::FindConnectedPlayer(ownerGuid);
    if (onlineOwner)
    {
        uint8 ownerLevel = onlineOwner->GetLevel();
        if (merc->GetLevel() != ownerLevel)
            merc->GiveLevel(ownerLevel);
        PlayerbotFactory(merc, ownerLevel, ITEM_QUALITY_EPIC).Randomize(false);
    }

    merc->SaveToDB(true, false);
    sCharacterCache->AddCharacterCacheEntry(
        mercGuid, serviceAccountId, mercName,
        merc->getGender(), merc->getRace(),
        merc->getClass(), merc->GetLevel());

    // Wait for the async write to land AND for the SYNC connection (which
    // Guild::AddMember uses for CHAR_SEL_CHAR_DATA_FOR_GUILD) to see it.
    // QueueSize alone isn't enough — async/sync use different connections.
    while (CharacterDatabase.QueueSize())
        std::this_thread::sleep_for(50ms);
    for (int tries = 0; tries < 100; ++tries)
    {
        if (CharacterDatabase.Query(
                "SELECT 1 FROM characters WHERE guid = {}", mercGuid.GetCounter()))
            break;
        std::this_thread::sleep_for(50ms);
    }

    if (uint32 guildId = sMercenaryMgr.GetMercenariesGuildId())
    {
        if (Guild* guild = sGuildMgr->GetGuildById(guildId))
        {
            if (!guild->AddMember(mercGuid))
                LOG_WARN("server.misc",
                    "Sbywow: Guild::AddMember failed for merc guid={}",
                    mercGuid.GetCounter());
        }
    }

    merc->CleanupsBeforeDelete();
    delete merc;
    delete session;

    sMercenaryMgr.AddOwnership(mercGuid, ownerGuid, classId);

    std::string ownerName;
    sCharacterCache->GetCharacterNameByGuid(ownerGuid, ownerName);
    LOG_INFO("server.misc",
        "Sbywow: hired merc '{}' (guid={}, class={}) for owner '{}' (guid={})",
        mercName, mercGuid.GetCounter(), uint32(classId),
        ownerName, ownerGuid.GetCounter());

    // Auto-summon if owner is online — same path the autologin uses, just
    // triggered immediately so the player doesn't have to log out and back in.
    if (onlineOwner)
    {
        if (PlayerbotMgr* mgr = GET_PLAYERBOT_MGR(onlineOwner))
        {
            std::string addCmd = "add " + mercName;
            mgr->HandlePlayerbotCommand(addCmd.c_str(), onlineOwner);
        }
    }

    return mercGuid;
}
