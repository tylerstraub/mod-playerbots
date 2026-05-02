#ifndef _SBYWOW_MERCENARY_MGR_H
#define _SBYWOW_MERCENARY_MGR_H

#include "ObjectGuid.h"

#include <cstdint>
#include <vector>

class MercenaryMgr
{
public:
    static MercenaryMgr& instance()
    {
        static MercenaryMgr instance;
        return instance;
    }

    // Idempotent bootstrap: creates the MERCENARIES service account, the
    // immortal Guildmaster service character, and the <Mercenaries> guild if
    // any are missing, then caches their IDs. Called once after DB is ready
    // and from .merc admin setup.
    void EnsureServiceState();

    // Hot-path queries used by the patched PlayerbotMgr::AddPlayerBot
    // (allowed-check) and PlayerbotMgr::OnPlayerLogin (autologin union).
    bool                    IsOwnedBy(ObjectGuid mercGuid, ObjectGuid ownerGuid) const;
    std::vector<ObjectGuid> GetMercsForOwner(ObjectGuid ownerGuid) const;
    uint32                  GetMercCount(ObjectGuid ownerGuid) const;

    // Mutations driven by the .merc hire / dismiss flows.
    void AddOwnership(ObjectGuid mercGuid, ObjectGuid ownerGuid, uint8 classId);
    void RemoveOwnership(ObjectGuid mercGuid);
    void RemoveAllOwnedBy(ObjectGuid ownerGuid);

    // Full dismiss: remove from <Mercenaries> guild, delete character row,
    // drop cache entry, drain queue, remove ownership row.
    void DismissMerc(ObjectGuid mercGuid);

    // Cached after EnsureServiceState. Zero/empty until bootstrap completes.
    uint32     GetServiceAccountId()   const { return _serviceAccountId; }
    uint32     GetMercenariesGuildId() const { return _mercenariesGuildId; }
    ObjectGuid GetGuildmasterGuid()    const { return _guildmasterGuid; }

private:
    MercenaryMgr() = default;

    void ensureServiceAccount();
    void bootstrapMercenariesGuild();

    uint32     _serviceAccountId   = 0;
    uint32     _mercenariesGuildId = 0;
    ObjectGuid _guildmasterGuid;
};

#define sMercenaryMgr MercenaryMgr::instance()

#endif
