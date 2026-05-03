/*
 * sbywow Agent Bridge — Trade event emit surface.
 *
 * Bucket 1 entry points called from Bucket 2 splices in upstream
 * TradeHandler.cpp. AC's trade subsystem has no PlayerScript hook
 * surface — the choice is "small splices in TradeHandler" or "no
 * trade events for the agent." We took the splices.
 *
 * Splice points (TradeHandler.cpp), one per function below:
 *   WorldSession::SendTradeStatus       → EmitTradeStatus
 *   WorldSession::HandleSetTradeItem    → EmitTradeItemSet
 *   WorldSession::HandleClearTradeItem  → EmitTradeItemCleared
 *   WorldSession::HandleSetTradeGold    → EmitTradeMoneySet
 *
 * All four are single-line calls. Logic stays here in Bucket 1.
 *
 * Per fork-discipline.md, this is the cohesive-subclass pattern
 * applied to event emission: one stable Bucket 1 surface, tiny
 * Bucket 2 splices that the rebase machinery rarely conflicts on.
 */

#ifndef _SBYWOW_BRIDGE_TRADE_EVENTS_H
#define _SBYWOW_BRIDGE_TRADE_EVENTS_H

#include <cstdint>

class Item;
class Player;
struct TradeStatusInfo;

namespace Sbywow::Bridge
{
    // Populates the function-pointer slots in
    // src/server/game/Sbywow/SbywowBridgeShim.h with the impl
    // functions defined in TradeEvents.cpp. Called once from
    // BridgeServer::Start when the bridge module starts. Until this
    // runs, the game-side splices are no-ops (null pointer guard
    // in the inline shim wrappers).
    void RegisterTradeEmitters();
}

#endif
