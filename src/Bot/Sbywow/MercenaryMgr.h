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

    // Idempotent bootstrap: creates the dedicated service account that owns
    // every mercenary character if missing, then caches its ID. Called once
    // after DB is ready and from .merc admin setup. Mercs spawn guildless;
    // operators that want a shared merc guild can add membership manually
    // via the standard guild admin commands.
    void EnsureServiceState();

    // Hot-path queries used by the patched PlayerbotMgr::AddPlayerBot
    // (allowed-check) and PlayerbotMgr::OnPlayerLogin (autologin union).
    bool                    IsOwnedBy(ObjectGuid mercGuid, ObjectGuid ownerGuid) const;
    std::vector<ObjectGuid> GetMercsForOwner(ObjectGuid ownerGuid) const;
    uint32                  GetMercCount(ObjectGuid ownerGuid) const;

    // Mutations driven by the .merc hire / dismiss flows.
    void AddOwnership(ObjectGuid mercGuid, ObjectGuid ownerGuid, uint8 classId);
    void RemoveOwnership(ObjectGuid mercGuid);

    // Full dismiss: gracefully despawn if online (via owner's PlayerbotMgr),
    // delete character row, drop cache entry, drain queue, remove ownership
    // row.
    void DismissMerc(ObjectGuid mercGuid);

    // Boot-time self-heal: reap orphan rows + log dangling service-account
    // characters. See implementation for the three sweeps.
    void ReapOrphans();

    // Cached after EnsureServiceState. Zero until bootstrap completes.
    uint32     GetServiceAccountId()   const { return _serviceAccountId; }

private:
    MercenaryMgr() = default;

    void ensureServiceAccount();

    uint32     _serviceAccountId   = 0;
};

#define sMercenaryMgr MercenaryMgr::instance()

#endif
