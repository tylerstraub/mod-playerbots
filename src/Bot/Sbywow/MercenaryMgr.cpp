#include "MercenaryMgr.h"

#include "AccountMgr.h"
#include "CharacterCache.h"
#include "DatabaseEnv.h"
#include "Field.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Log.h"
#include "Player.h"
#include "QueryResult.h"
#include "RandomPlayerbotFactory.h"
#include "SbywowConstants.h"
#include "SharedDefines.h"
#include "WorldSession.h"

#include <chrono>
#include <thread>
#include <unordered_map>

void MercenaryMgr::EnsureServiceState()
{
    if (_serviceAccountId == 0)
        ensureServiceAccount();

    if (_serviceAccountId == 0)
        return;

    if (_mercenariesGuildId != 0)
        return;

    if (Guild* existing = sGuildMgr->GetGuildByName(Sbywow::MERCENARIES_GUILD_NAME))
    {
        _mercenariesGuildId = existing->GetId();
        _guildmasterGuid = existing->GetLeaderGUID();
        LOG_INFO("server.loading",
            "Sbywow: <{}> guild already present (id={}, leader={})",
            Sbywow::MERCENARIES_GUILD_NAME, _mercenariesGuildId,
            _guildmasterGuid.ToString());
        return;
    }

    bootstrapMercenariesGuild();
}

void MercenaryMgr::ensureServiceAccount()
{
    using namespace std::chrono_literals;

    _serviceAccountId = AccountMgr::GetId(Sbywow::SERVICE_ACCOUNT_NAME);
    if (_serviceAccountId != 0)
    {
        LOG_INFO("server.loading",
            "Sbywow: mercenary service account '{}' already present (id={})",
            Sbywow::SERVICE_ACCOUNT_NAME, _serviceAccountId);
        return;
    }

    AccountOpResult result = AccountMgr::CreateAccount(
        Sbywow::SERVICE_ACCOUNT_NAME, Sbywow::SERVICE_ACCOUNT_PASSWORD);

    if (result != AOR_OK)
    {
        LOG_ERROR("server.loading",
            "Sbywow: failed to create service account '{}' (AccountOpResult={})",
            Sbywow::SERVICE_ACCOUNT_NAME, uint32(result));
        return;
    }

    while (LoginDatabase.QueueSize())
        std::this_thread::sleep_for(50ms);

    _serviceAccountId = AccountMgr::GetId(Sbywow::SERVICE_ACCOUNT_NAME);
    LOG_INFO("server.loading",
        "Sbywow: created mercenary service account '{}' (id={})",
        Sbywow::SERVICE_ACCOUNT_NAME, _serviceAccountId);
}

void MercenaryMgr::bootstrapMercenariesGuild()
{
    using namespace std::chrono_literals;

    // Reuse the playerbot factory to create the Guildmaster character on the
    // service account. We accept a random name for v1 — leader name is only
    // visible in guild info, and we can polish later by extracting a
    // CreateNamedCharacter helper that takes an explicit name.
    WorldSession* session = new WorldSession(
        _serviceAccountId, "", 0x0, nullptr, SEC_PLAYER,
        EXPANSION_WRATH_OF_THE_LICH_KING, time_t(0), LOCALE_enUS, 0,
        false, false, 0, true);

    std::unordered_map<RandomPlayerbotFactory::NameRaceAndGender, std::vector<std::string>> emptyCache;
    RandomPlayerbotFactory factory;
    Player* gm = factory.CreateRandomBot(session, CLASS_WARRIOR, emptyCache);

    if (!gm)
    {
        LOG_ERROR("server.loading", "Sbywow: failed to create Guildmaster character");
        delete session;
        return;
    }

    ObjectGuid gmGuid = gm->GetGUID();
    std::string gmName = gm->GetName();

    gm->SaveToDB(true, false);
    sCharacterCache->AddCharacterCacheEntry(
        gmGuid, _serviceAccountId, gmName,
        gm->getGender(), gm->getRace(), gm->getClass(), gm->GetLevel());

    // Wait for async write AND for the SYNC connection to see the row, since
    // Guild::Create internally calls AddMember which uses a SYNC SELECT.
    while (CharacterDatabase.QueueSize())
        std::this_thread::sleep_for(50ms);
    for (int tries = 0; tries < 100; ++tries)
    {
        if (CharacterDatabase.Query(
                "SELECT 1 FROM characters WHERE guid = {}", gmGuid.GetCounter()))
            break;
        std::this_thread::sleep_for(50ms);
    }

    Guild* guild = new Guild();
    if (!guild->Create(gm, Sbywow::MERCENARIES_GUILD_NAME))
    {
        LOG_ERROR("server.loading",
            "Sbywow: Guild::Create failed for <{}>", Sbywow::MERCENARIES_GUILD_NAME);
        delete guild;
        gm->CleanupsBeforeDelete();
        delete gm;
        delete session;
        return;
    }
    sGuildMgr->AddGuild(guild);

    _guildmasterGuid = gmGuid;
    _mercenariesGuildId = guild->GetId();

    gm->CleanupsBeforeDelete();
    delete gm;
    delete session;

    LOG_INFO("server.loading",
        "Sbywow: bootstrapped Guildmaster '{}' (guid={}) and <{}> guild (id={})",
        gmName, gmGuid.GetCounter(), Sbywow::MERCENARIES_GUILD_NAME, _mercenariesGuildId);
}

bool MercenaryMgr::IsOwnedBy(ObjectGuid mercGuid, ObjectGuid ownerGuid) const
{
    QueryResult result = CharacterDatabase.Query(
        "SELECT 1 FROM mod_sbywow_mercenaries WHERE merc_guid = {} AND owner_guid = {}",
        mercGuid.GetCounter(), ownerGuid.GetCounter());
    return result != nullptr;
}

std::vector<ObjectGuid> MercenaryMgr::GetMercsForOwner(ObjectGuid ownerGuid) const
{
    std::vector<ObjectGuid> mercs;
    QueryResult result = CharacterDatabase.Query(
        "SELECT merc_guid FROM mod_sbywow_mercenaries WHERE owner_guid = {}",
        ownerGuid.GetCounter());

    if (!result)
        return mercs;

    do
    {
        Field* fields = result->Fetch();
        mercs.push_back(ObjectGuid::Create<HighGuid::Player>(fields[0].Get<uint32>()));
    } while (result->NextRow());

    return mercs;
}

uint32 MercenaryMgr::GetMercCount(ObjectGuid ownerGuid) const
{
    QueryResult result = CharacterDatabase.Query(
        "SELECT COUNT(*) FROM mod_sbywow_mercenaries WHERE owner_guid = {}",
        ownerGuid.GetCounter());
    if (!result)
        return 0;
    return result->Fetch()[0].Get<uint32>();
}

void MercenaryMgr::AddOwnership(ObjectGuid mercGuid, ObjectGuid ownerGuid, uint8 classId)
{
    CharacterDatabase.Execute(
        "INSERT INTO mod_sbywow_mercenaries (merc_guid, owner_guid, class_id) "
        "VALUES ({}, {}, {})",
        mercGuid.GetCounter(), ownerGuid.GetCounter(), uint32(classId));
}

void MercenaryMgr::RemoveOwnership(ObjectGuid mercGuid)
{
    CharacterDatabase.Execute(
        "DELETE FROM mod_sbywow_mercenaries WHERE merc_guid = {}",
        mercGuid.GetCounter());
}

void MercenaryMgr::RemoveAllOwnedBy(ObjectGuid ownerGuid)
{
    CharacterDatabase.Execute(
        "DELETE FROM mod_sbywow_mercenaries WHERE owner_guid = {}",
        ownerGuid.GetCounter());
}

void MercenaryMgr::DismissMerc(ObjectGuid mercGuid)
{
    using namespace std::chrono_literals;

    if (_serviceAccountId == 0)
    {
        LOG_ERROR("server.misc",
            "Sbywow: DismissMerc called before service account is bootstrapped");
        return;
    }

    std::string mercName;
    sCharacterCache->GetCharacterNameByGuid(mercGuid, mercName);

    if (_mercenariesGuildId)
    {
        if (Guild* guild = sGuildMgr->GetGuildById(_mercenariesGuildId))
            guild->DeleteMember(mercGuid, false, true, false);
    }

    Player::DeleteFromDB(mercGuid.GetCounter(), _serviceAccountId, true, true);
    sCharacterCache->DeleteCharacterCacheEntry(mercGuid, mercName);

    while (CharacterDatabase.QueueSize())
        std::this_thread::sleep_for(50ms);

    RemoveOwnership(mercGuid);

    LOG_INFO("server.misc",
        "Sbywow: dismissed merc '{}' (guid={})", mercName, mercGuid.GetCounter());
}
