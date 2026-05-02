#include "MercenaryMgr.h"

#include "AccountMgr.h"
#include "CharacterCache.h"
#include "DatabaseEnv.h"
#include "Field.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotMgr.h"
#include "Playerbots.h"  // GET_PLAYERBOT_MGR
#include "QueryResult.h"
#include "RandomPlayerbotFactory.h"
#include "SbywowConstants.h"
#include "SharedDefines.h"
#include "WorldSession.h"

#include <chrono>
#include <thread>
#include <unordered_map>
#include <vector>

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
    // DirectExecute is synchronous — the row is gone before this returns. We
    // need this guarantee for the reaper sweeps (sweep 2 immediately re-queries
    // the table; if RemoveOwnership were async via Execute, sweep 2 would
    // double-catch the same row before the worker thread committed).
    CharacterDatabase.DirectExecute(
        "DELETE FROM mod_sbywow_mercenaries WHERE merc_guid = {}",
        mercGuid.GetCounter());
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

    // If the merc is currently auto-summoned (in-world as a login-bot), route
    // through the owner's PlayerbotMgr::LogoutPlayerBot first. That path saves
    // state, removes from group, tears down the WorldSession, and destroys the
    // Player object — so the subsequent Player::DeleteFromDB doesn't race
    // against a live Player*/session pair (which would crash on next ObjectAccessor sweep).
    if (ObjectAccessor::FindConnectedPlayer(mercGuid))
    {
        QueryResult ownerRow = CharacterDatabase.Query(
            "SELECT owner_guid FROM mod_sbywow_mercenaries WHERE merc_guid = {}",
            mercGuid.GetCounter());
        if (ownerRow)
        {
            ObjectGuid ownerGuid = ObjectGuid::Create<HighGuid::Player>(
                ownerRow->Fetch()[0].Get<uint32>());
            if (Player* owner = ObjectAccessor::FindConnectedPlayer(ownerGuid))
            {
                if (PlayerbotMgr* mgr = GET_PLAYERBOT_MGR(owner))
                    mgr->LogoutPlayerBot(mercGuid);
                else
                    LOG_WARN("server.misc",
                        "Sbywow: merc '{}' (guid={}) online but owner has no PlayerbotMgr — proceeding with raw DeleteFromDB",
                        mercName, mercGuid.GetCounter());
            }
            else
            {
                LOG_WARN("server.misc",
                    "Sbywow: merc '{}' (guid={}) online but owner (guid={}) not connected — proceeding with raw DeleteFromDB",
                    mercName, mercGuid.GetCounter(), ownerGuid.GetCounter());
            }
        }
        else
        {
            LOG_WARN("server.misc",
                "Sbywow: merc '{}' (guid={}) online but no ownership row — proceeding with raw DeleteFromDB",
                mercName, mercGuid.GetCounter());
        }
    }

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

void MercenaryMgr::ReapOrphans()
{
    if (_serviceAccountId == 0)
    {
        LOG_WARN("server.loading",
            "Sbywow: ReapOrphans skipped — service account not bootstrapped");
        return;
    }

    // Sweep 1: ownership rows whose owner_guid no longer exists in characters.
    // The owner was deleted at some point without our OnPlayerDelete cascade
    // running (legacy data from pre-fix runs, or manual SQL deletes). Full
    // dismiss the merc — char row, guild membership, and ownership row.
    {
        QueryResult res = CharacterDatabase.Query(
            "SELECT m.merc_guid FROM mod_sbywow_mercenaries m "
            "LEFT JOIN characters c ON c.guid = m.owner_guid "
            "WHERE c.guid IS NULL");
        uint32 reaped = 0;
        if (res)
        {
            std::vector<ObjectGuid> victims;
            do
            {
                victims.push_back(ObjectGuid::Create<HighGuid::Player>(
                    res->Fetch()[0].Get<uint32>()));
            } while (res->NextRow());

            for (ObjectGuid mercGuid : victims)
            {
                LOG_INFO("server.loading",
                    "Sbywow: reaping orphan merc (dead owner) guid={}",
                    mercGuid.GetCounter());
                DismissMerc(mercGuid);
                ++reaped;
            }
        }
        LOG_INFO("server.loading",
            "Sbywow: orphan reaper sweep 1 (dead owner): {} merc(s) dismissed", reaped);
    }

    // Drain async writes from sweep 1 (DismissMerc → RemoveOwnership uses
    // CharacterDatabase.Execute, async). Without this drain, sweep 2's sync
    // SELECT below would re-find the same rows and double-log them.
    using namespace std::chrono_literals;
    while (CharacterDatabase.QueueSize())
        std::this_thread::sleep_for(50ms);

    // Sweep 2: ownership rows whose merc_guid no longer exists in characters.
    // The character was nuked out from under us (e.g. via .character delete on
    // the service account, or a partial-failed CreateMerc). Just drop the row.
    {
        QueryResult res = CharacterDatabase.Query(
            "SELECT m.merc_guid FROM mod_sbywow_mercenaries m "
            "LEFT JOIN characters c ON c.guid = m.merc_guid "
            "WHERE c.guid IS NULL");
        uint32 reaped = 0;
        if (res)
        {
            do
            {
                uint32 lowGuid = res->Fetch()[0].Get<uint32>();
                ObjectGuid mercGuid = ObjectGuid::Create<HighGuid::Player>(lowGuid);
                LOG_INFO("server.loading",
                    "Sbywow: dropping ownership row for missing merc guid={}",
                    lowGuid);
                RemoveOwnership(mercGuid);
                ++reaped;
            } while (res->NextRow());
        }
        LOG_INFO("server.loading",
            "Sbywow: orphan reaper sweep 2 (missing merc char): {} row(s) dropped", reaped);
    }

    // Sweep 3: warn-only. Service-account characters that aren't tracked in
    // mod_sbywow_mercenaries and aren't the Guildmaster. Could be in-flight
    // hires that crashed mid-CreateMerc, manual GM-created chars, or stale
    // test fixtures. We do NOT auto-delete — character deletion is an explicit
    // action that should pass through .merc admin nuke after human review.
    {
        uint32 gmLow = _guildmasterGuid.GetCounter();
        QueryResult res = CharacterDatabase.Query(
            "SELECT c.guid, c.name FROM characters c "
            "LEFT JOIN mod_sbywow_mercenaries m ON m.merc_guid = c.guid "
            "WHERE c.account = {} AND m.merc_guid IS NULL AND c.guid != {}",
            _serviceAccountId, gmLow);
        uint32 dangling = 0;
        if (res)
        {
            do
            {
                Field* fields = res->Fetch();
                LOG_WARN("server.loading",
                    "Sbywow: untracked service-account character: guid={} name='{}' — review manually (.merc admin nuke <guid> if intended)",
                    fields[0].Get<uint32>(), fields[1].Get<std::string>());
                ++dangling;
            } while (res->NextRow());
        }
        if (dangling > 0)
            LOG_WARN("server.loading",
                "Sbywow: orphan reaper sweep 3 (untracked service chars): {} found — see warnings above", dangling);
        else
            LOG_INFO("server.loading",
                "Sbywow: orphan reaper sweep 3 (untracked service chars): clean");
    }
}
