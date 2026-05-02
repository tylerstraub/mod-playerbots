#ifndef _SBYWOW_MERCENARY_FACTORY_H
#define _SBYWOW_MERCENARY_FACTORY_H

#include "ObjectGuid.h"

#include <cstdint>

class MercenaryFactory
{
public:
    // Hire flow: roll race+name for the requested class, build a temporary
    // WorldSession against the service account, Player::Create + SaveToDB,
    // register in sCharacterCache, Guild::AddMember to <Mercenaries>, INSERT
    // ownership row. Owner does not need to be online — only the GUID is
    // recorded. Returns the new merc's character GUID, or empty on error.
    static ObjectGuid CreateMerc(ObjectGuid ownerGuid, uint8 classId);
};

#endif
