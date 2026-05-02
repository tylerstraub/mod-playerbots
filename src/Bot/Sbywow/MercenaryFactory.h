#ifndef _SBYWOW_MERCENARY_FACTORY_H
#define _SBYWOW_MERCENARY_FACTORY_H

#include "ObjectGuid.h"

#include <cstdint>
#include <string>

class MercenaryFactory
{
public:
    // Hire flow: roll race+name for the requested class, build a temporary
    // WorldSession against the service account, Player::Create + SaveToDB,
    // register in sCharacterCache, Guild::AddMember to <Mercenaries>, INSERT
    // ownership row. Owner does not need to be online — only the GUID is
    // recorded. Returns the new merc's character GUID, or empty on error.
    //
    // desiredName: optional override. If non-empty, the merc is renamed
    // in-memory before SaveToDB, and that name is used for the character
    // cache, log line, and auto-summon. Validated for length (2-12) and
    // uniqueness; returns empty guid on collision or invalid name.
    static ObjectGuid CreateMerc(ObjectGuid ownerGuid, uint8 classId,
                                 std::string const& desiredName = "");
};

#endif
