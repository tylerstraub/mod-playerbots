#include "MercenaryFactory.h"

#include "CharacterCache.h"
#include "DatabaseEnv.h"
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

namespace
{
    // Map race id → team id. Mirrors AC's Player::TeamForRace; copied
    // here so we don't need to call into Player static helpers from a
    // factory context. Must stay in sync with the race enum.
    bool RaceIsAlliance(uint8 race)
    {
        switch (race)
        {
            case RACE_HUMAN:
            case RACE_DWARF:
            case RACE_NIGHTELF:
            case RACE_GNOME:
            case RACE_DRAENEI:
                return true;
            case RACE_ORC:
            case RACE_UNDEAD_PLAYER:
            case RACE_TAUREN:
            case RACE_TROLL:
            case RACE_BLOODELF:
                return false;
            default:
                return true;  // unknown → assume alliance (won't trigger reroll)
        }
    }

    bool RaceMatchesRequirement(uint8 race, FactionRequirement req)
    {
        switch (req)
        {
            case FactionRequirement::Any:      return true;
            case FactionRequirement::Alliance: return RaceIsAlliance(race);
            case FactionRequirement::Horde:    return !RaceIsAlliance(race);
        }
        return true;
    }
}

ObjectGuid MercenaryFactory::CreateMerc(ObjectGuid ownerGuid, uint8 classId,
                                        std::string const& desiredName,
                                        FactionRequirement factionReq)
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
    //
    // Faction filter: upstream CreateRandomBot does its own 50/50 faction
    // roll then picks a race within that faction. To force a specific
    // faction we re-roll until the candidate matches. Each rejected
    // candidate is destroyed in-memory (no DB write — that happens later
    // at SaveToDB) and consumes a player-guid from the generator.
    // 8 retries gives ~99.6% success rate against a 50/50 distribution.
    std::unordered_map<RandomPlayerbotFactory::NameRaceAndGender, std::vector<std::string>> emptyCache;
    RandomPlayerbotFactory factory;
    constexpr int kMaxFactionRetries = 8;
    Player* merc = nullptr;
    int rolls = 0;
    for (int attempt = 0; attempt < kMaxFactionRetries; ++attempt)
    {
        ++rolls;
        Player* candidate = factory.CreateRandomBot(session, classId, emptyCache);
        if (!candidate)
            continue;
        if (RaceMatchesRequirement(candidate->getRace(), factionReq))
        {
            merc = candidate;
            break;
        }
        // Faction mismatch — clean up the in-memory Player. CleanupsBeforeDelete
        // is safe on a freshly-created Player that's never entered the world
        // or hit SaveToDB. Wastes the player-guid the generator handed out.
        candidate->CleanupsBeforeDelete();
        delete candidate;
    }

    if (!merc)
    {
        LOG_ERROR("server.misc",
            "Sbywow: CreateRandomBot failed for class {} after {} attempts "
            "(faction_req={})",
            uint32(classId), rolls, static_cast<int>(factionReq));
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

    // Drain the async write so the merc row is durable before we hand off
    // to PlayerbotMgr for auto-summon (which loads from DB on the sync
    // connection). We wait on both the queue and a sync SELECT against the
    // SYNC connection because async/sync use different connections.
    while (CharacterDatabase.QueueSize())
        std::this_thread::sleep_for(50ms);
    for (int tries = 0; tries < 100; ++tries)
    {
        if (CharacterDatabase.Query(
                "SELECT 1 FROM characters WHERE guid = {}", mercGuid.GetCounter()))
            break;
        std::this_thread::sleep_for(50ms);
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
