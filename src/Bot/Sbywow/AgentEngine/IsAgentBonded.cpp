#include "IsAgentBonded.h"

#include "../MercenaryMgr.h"

#include "Player.h"
#include "WorldSession.h"

namespace Sbywow
{
    bool IsAgentBonded(Player* player)
    {
        if (!player)
            return false;

        WorldSession* session = player->GetSession();
        if (!session)
            return false;

        // All mercs share the MERCENARIES service account. If
        // EnsureServiceState hasn't run yet (very-early-boot), the
        // cached id is 0 — we conservatively return false so we don't
        // accidentally agent-bond a real player whose account id
        // happens to be unset somehow. Once bootstrap is complete (well
        // before any merc could ever attach), this is a single integer
        // compare per createNonCombatEngine call.
        uint32 svcAccountId = sMercenaryMgr.GetServiceAccountId();
        if (svcAccountId == 0)
            return false;

        return session->GetAccountId() == svcAccountId;
    }
}
