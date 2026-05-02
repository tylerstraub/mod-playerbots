#ifndef _SBYWOW_CONSTANTS_H
#define _SBYWOW_CONSTANTS_H

namespace Sbywow
{
    // Dedicated service account that owns every mercenary character. Never
    // logged into externally; mercs are spawned and summoned via fork code.
    // AC's MAX_ACCOUNT_STR = 17 and MAX_PASS_STR = 16 — keep within those limits.
    constexpr char const* SERVICE_ACCOUNT_NAME     = "MERCENARIES_SVC";
    constexpr char const* SERVICE_ACCOUNT_PASSWORD = "sbywow_x7v_q9k2";

    // Server-side guild that contains every mercenary character, owned by an
    // immortal service character that itself lives on the service account.
    constexpr char const* MERCENARIES_GUILD_NAME = "Mercenaries";
    constexpr char const* GUILDMASTER_NAME       = "Guildmaster";
}

#endif
