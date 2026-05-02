/*
 * sbywow Agent Bridge — agent-bonded predicate.
 *
 * The single point of truth for "should this bot get the
 * SbywowAgentEngine instead of upstream Engine for BOT_STATE_NON_COMBAT?"
 * Today's answer is "yes if the bot lives on the MERCENARIES service
 * account" — every merc shares one account, so account-id equality
 * with MercenaryMgr's cached service-account id is the cheapest test.
 *
 * Future: extend to PBC-card-bonded bots (any character with a
 * card.txt that declares agent_bonded). Adding that case is purely
 * additive — flip the predicate to OR over both checks.
 */

#ifndef _SBYWOW_IS_AGENT_BONDED_H
#define _SBYWOW_IS_AGENT_BONDED_H

class Player;

namespace Sbywow
{
    bool IsAgentBonded(Player* player);
}

#endif
