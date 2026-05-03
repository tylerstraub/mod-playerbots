#ifndef _SBYWOW_CONSTANTS_H
#define _SBYWOW_CONSTANTS_H

#include <cstdint>

namespace Sbywow
{
    // Dedicated service account that owns every mercenary character. Never
    // logged into externally; mercs are spawned and summoned via fork code.
    // AC's MAX_ACCOUNT_STR = 17 and MAX_PASS_STR = 16 — keep within those limits.
    constexpr char const* SERVICE_ACCOUNT_NAME     = "MERCENARIES_SVC";
    constexpr char const* SERVICE_ACCOUNT_PASSWORD = "m3rc_svc_8x2vq9";

    // Default cap on mercs-per-owner. Read at runtime via the config key
    // `Mercenaries.MaxPerOwner` so operators can tune without recompiling.
    // Sized to allow a full 5-man (owner + 4 mercs) without filling every
    // party slot, leaving room for one real player to join.
    constexpr uint32 DEFAULT_MAX_MERCS_PER_OWNER = 4;
}

#endif
