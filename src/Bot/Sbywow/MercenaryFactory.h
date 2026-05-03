#ifndef _SBYWOW_MERCENARY_FACTORY_H
#define _SBYWOW_MERCENARY_FACTORY_H

#include "ObjectGuid.h"

#include <cstdint>
#include <string>

// Faction filter for merc race-roll. The upstream
// RandomPlayerbotFactory rolls 50/50 across factions, then picks any
// race valid for the class within that faction. For "I want a
// guaranteed same-faction trade partner" use cases (which is most of
// them post-FFA-rollback), the agent or master needs control. The
// factory re-rolls until a candidate matches the requirement, with a
// bounded retry cap.
enum class FactionRequirement
{
    Any,        // upstream behavior: 50/50 alliance/horde
    Alliance,   // any Alliance race valid for the class
    Horde,      // any Horde race valid for the class
};

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
    //
    // factionReq: optional faction filter for the race-roll. Default
    // Any preserves upstream 50/50 behavior. Alliance / Horde re-rolls
    // up to ~8 times to land a same-faction race. Each rejected
    // candidate consumes a player-guid from the generator (small
    // wastage; cleanup is in-memory only — no DB write).
    static ObjectGuid CreateMerc(ObjectGuid ownerGuid, uint8 classId,
                                 std::string const& desiredName = "",
                                 FactionRequirement factionReq = FactionRequirement::Any);
};

#endif
