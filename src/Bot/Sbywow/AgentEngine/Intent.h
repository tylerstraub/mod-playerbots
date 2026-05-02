/*
 * sbywow Agent Bridge — intent record.
 *
 * The agent harness pushes intents through the bridge; the
 * SbywowAgentEngine pops one per tick and executes. This file
 * declares the intent shape — a tagged union of the primitive
 * actions the harness can request.
 *
 * Phase 2 ships only Move. Phase 3 adds Interact, Say, DoAction,
 * Wait. The struct is a flat record on purpose: one allocation,
 * trivially copyable, no std::variant indirection. Adding kinds is
 * additive — extend the enum and the struct (or add a per-kind
 * sub-struct if/when fields proliferate).
 */

#ifndef _SBYWOW_AGENT_ENGINE_INTENT_H
#define _SBYWOW_AGENT_ENGINE_INTENT_H

#include <cstdint>
#include <string>

namespace Sbywow
{
    enum class IntentKind : uint8_t
    {
        Move,
        // Phase 3 will add: Interact, Say, DoAction, Wait
    };

    struct Intent
    {
        IntentKind kind = IntentKind::Move;

        // Move fields. Map 0 means "no constraint" (use bot's current).
        float  x   = 0.f;
        float  y   = 0.f;
        float  z   = 0.f;
        uint32_t map = 0;
    };
}

#endif
