#include "SbywowAgentEngine.h"

#include "../Bridge/BridgeServer.h"
#include "../Bridge/BotSession.h"
#include "../Bridge/deps/json.hpp"

#include "AiFactory.h"
#include "Bag.h"
#include "Corpse.h"
#include "Creature.h"
#include "CreatureData.h"
#include "Event.h"
#include "GameObject.h"
#include "GossipDef.h"
#include "Group.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "ItemPackets.h"
#include "Log.h"
#include "LootMgr.h"
#include "GridTerrainData.h"  // INVALID_HEIGHT
#include "Map.h"
#include "MotionMaster.h"
#include "MovementGenerators/PathGenerator.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "QuestDef.h"
#include "SmartEnum.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TradeData.h"
#include "Playerbots.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

using json = nlohmann::json;

namespace Sbywow
{
    SbywowAgentEngine::SbywowAgentEngine(PlayerbotAI* ai, AiObjectContext* context)
        : Engine(ai, context)
    {
        // v1 strategy state: just "default" (WorldPacketHandlerStrategy).
        // Packet-handler plumbing stays alive (accept loot / taxi /
        // group invite / gossip dispatch). Every cognitive default
        // (follow, quest, gather, chat, emote, loot, duel, rpg, grind,
        // move random) is deliberately omitted — those belong to the
        // agent harness, driven via bridge intents.
        //
        // Class-specific buff/cure/heal-prep strategies (e.g., "dps assist",
        // "cure" for priest) are also omitted in v1; the agent issues
        // do_action for these explicitly when wanted. We can revisit
        // this in v1.x if it turns out to be friction.
        addStrategiesNoInit("default", nullptr);

        // Build the internal default Engine. Mirrors the upstream
        // AiFactory::createNonCombatEngine pattern (new Engine →
        // AddDefaultNonCombatStrategies → Init) but on a separate
        // instance we own. The default state is agent_mode=false so
        // this is what ticks on every fresh attach until the agent
        // harness opts in. Strategy stack is set once here and
        // intentionally never modified afterward — agent-issued
        // strategy ops affect us (and our filter), not the default
        // engine, so its behavior is always "what a default merc
        // does." See decisions.md "Agent mode is an explicit opt-in."
        if (Player* bot = ai ? ai->GetBot() : nullptr)
        {
            defaultEngine_ = std::make_unique<Engine>(ai, context);
            AiFactory::AddDefaultNonCombatStrategies(bot, ai, defaultEngine_.get());
            defaultEngine_->Init();
        }
    }

    void SbywowAgentEngine::addStrategy(std::string const name, bool init)
    {
        // Two channels:
        //   1. "default" packet-handler glue lives on US and is immutable
        //      — that's what keeps the bot ticking at all. PlayerbotAI's
        //      ResetStrategies adds it during login / SetMaster / etc.,
        //      and we accept those.
        //   2. Everything else (cognitive strategies — follow, grind,
        //      rpg, quest, gather, kite, etc.) gets FORWARDED to our
        //      wrapped defaultEngine_. That engine actually iterates
        //      strategies on every tick (when we delegate to it under
        //      agent_mode=off, in-combat, or idle-follow). Without
        //      forwarding, agent-issued `add_strategy grind` would
        //      silently no-op because our DoNextAction doesn't tick
        //      the strategy stack.
        //
        // ResetStrategies-driven re-adds (login, SetMaster, etc.) hit
        // this same path and forward into defaultEngine_, which is
        // already populated with the default merc stack from our
        // constructor. Re-adding an already-present strategy is a
        // no-op in the base Engine, so this is idempotent.
        if (name == "default")
        {
            Engine::addStrategy(name, init);
            return;
        }
        if (defaultEngine_)
        {
            defaultEngine_->addStrategy(name, init);
            LOG_DEBUG("playerbots",
                      "[SbywowAgentEngine] forwarded addStrategy '{}' → defaultEngine_",
                      name.c_str());
            return;
        }
        // No defaultEngine_ — only happens during teardown. Drop quietly.
    }

    bool SbywowAgentEngine::removeStrategy(std::string const name, bool init)
    {
        // "default" on us is immutable — removing it would silence packet
        // handling. Forward all other names to defaultEngine_.
        if (name == "default")
            return false;
        return defaultEngine_ ? defaultEngine_->removeStrategy(name, init) : false;
    }

    bool SbywowAgentEngine::HasStrategy(std::string const name)
    {
        // The "what's actually running" query — reads defaultEngine_'s
        // stack. Our own stack only contains "default", which isn't
        // interesting for the agent's mode-detection use cases.
        return defaultEngine_ ? defaultEngine_->HasStrategy(name) : false;
    }

    std::vector<std::string> SbywowAgentEngine::GetStrategies()
    {
        return defaultEngine_ ? defaultEngine_->GetStrategies() : std::vector<std::string>{};
    }

    void SbywowAgentEngine::ChangeStrategy(std::string const names)
    {
        // Forward the +foo,-bar,~baz comma syntax to defaultEngine_.
        if (defaultEngine_)
            defaultEngine_->ChangeStrategy(names);
    }

    namespace
    {
        // Build an `intent` channel SSE event envelope. The
        // BaseEvent shape (channel/kind/bot_guid/bot_name) mirrors
        // BridgeHooks.cpp; intent_id is wire-stringified to dodge
        // JS-side 2^53 truncation. result is included for terminal
        // states only (completed/failed/cancelled).
        json BuildIntentEvent(Player* bot, std::string const& kind,
                              uint64_t intentId, std::string const& verb)
        {
            return {
                {"channel",   "intent"},
                {"kind",      kind},
                {"bot_guid",  bot->GetGUID().GetRawValue()},
                {"bot_name",  bot->GetName()},
                {"intent_id", std::to_string(intentId)},
                {"verb",      verb}
            };
        }
    }

    bool SbywowAgentEngine::DoNextAction(Unit* target, uint32 depth, bool minimal)
    {
        if (!botAI)
            return false;

        Player* bot = botAI->GetBot();
        if (!bot)
            return false;

        // Count every tick that reached real work — answers
        // "is this engine actually being ticked?" for inspect.
        ++ticksTotal_;

        if (!tickedOnce_)
        {
            tickedOnce_ = true;
            LOG_INFO("playerbots",
                     "[SbywowAgentEngine] first tick for bot guid={} name={}",
                     bot->GetGUID().GetRawValue(), bot->GetName().c_str());
        }

        auto session = Sbywow::Bridge::BridgeServer::Instance().GetSession(bot->GetGUID());

        // Agent-mode routing. Default state is agent_mode=false,
        // meaning the bot behaves like a normal default merc — we
        // delegate the tick to the internal default Engine. The
        // agent harness explicitly opts in via the `set_agent_mode`
        // bridge verb (or master via `.merc agent`), and only then
        // does the agent path below run. Our own queue and wait
        // state are deliberately PRESERVED across both transitions,
        // untouched, so the agent can resume mid-plan on toggle-on
        // even after a silent window. See decisions.md "Agent mode
        // is an explicit opt-in" for the design.
        if (session && !session->IsAgentMode() && defaultEngine_)
        {
            ++defaultEngineTicksTotal_;
            return defaultEngine_->DoNextAction(target, depth, minimal);
        }

        // Combat hand-off. When the bot is in combat we delegate the
        // tick to the default (upstream) non-combat engine. That
        // engine carries the threat-detection + target-acquisition
        // + ChangeEngine(BOT_STATE_COMBAT) logic in its strategy stack
        // (AttackAction / AttackAnythingAction / etc.) — none of
        // which live on us, since SbywowAgentEngine only carries the
        // "default" packet-handler strategy by design.
        //
        // The preservation property still holds: agent's intent queue
        // and wait state are PINNED on us, never touched by this
        // delegation. When combat ends, DropTargetAction swaps the
        // currentEngine back to non_combat (us), and we resume the
        // agent's plan exactly where it left off.
        if (bot->IsInCombat() && defaultEngine_)
        {
            ++defaultEngineTicksTotal_;
            return defaultEngine_->DoNextAction(target, depth, minimal);
        }

        // Poll the multi-tick blocking slots. Each may emit a terminal
        // event (intent_completed or intent_failed) and clear the
        // slot. After polling, if the slot is still set, the intent
        // is still in-flight — but we still fall through to the
        // queue-drain stage so non-blocking intents (instant casts,
        // Say, Interact, Buy, etc.) can run in parallel with the
        // in-flight blocking work. This mirrors real WoW where you
        // can /say or fire instant-cast spells while moving.
        PollInFlightMove(bot, session);
        PollInFlightCast(bot, session);

        // Wait keeps its original universal-pause semantic — when
        // the agent says "wait N ms," the entire plan halts. No
        // intents drain, including non-blocking ones. This is
        // explicit sequencing behavior the agent issued; we don't
        // sneak instant casts through it.
        if (isWaiting_)
        {
            if (std::chrono::steady_clock::now() < waitUntil_)
                return false;  // still waiting — universal pause
            isWaiting_ = false;

            // Wait reached its end naturally — emit intent_completed
            // tagged with the wait's id. The cancel path
            // (CancelWaitIfMatch) clears waitingIntentId_ before
            // returning, so if we got here with id != 0 the
            // suspension expired on its own.
            if (session && waitingIntentId_ != 0)
            {
                json result = json{{"ok", true}, {"verb", "wait"}};
                json ev = BuildIntentEvent(bot, "intent_completed",
                                           waitingIntentId_, waitingIntentVerb_);
                ev["result"] = result;
                session->PushOutbound(ev.dump());

                Sbywow::Bridge::BotSession::TerminalIntent rec;
                rec.intentId   = waitingIntentId_;
                rec.verb       = waitingIntentVerb_;
                rec.kind       = "intent_completed";
                rec.resultJson = result.dump();
                session->RecordTerminal(std::move(rec));
            }
            waitingIntentId_ = 0;
            waitingIntentVerb_.clear();
        }

        if (!session)
            return false;

        // Drain the queue. Non-blocking intents fire inline same tick
        // and we keep draining; a blocking intent (Move, cast-time
        // Cast/UseItem/Mount) takes the head slot for one tick and
        // we stop. While ANY blocking slot is occupied (move or cast
        // in flight), we still drain non-blocking intents.
        bool didWork = false;
        while (true)
        {
            auto pending = session->PeekIntent();
            if (!pending)
                break;

            bool blocking = IsBlockingIntent(bot, pending->intent);
            bool slotsFree = !inFlightMove_ && !inFlightCast_;

            if (blocking && !slotsFree)
                break;  // blocking intent waiting on slot to clear

            // Commit to dispatching this one. Pop atomically — peek
            // and pop run on the same world thread, no other puller.
            std::shared_ptr<Sbywow::Bridge::PendingIntent> popped;
            session->PopIntent(popped);

            // intent_started fires before any executor runs; gives
            // the harness a clean before-dispatch marker.
            {
                json ev = BuildIntentEvent(bot, "intent_started",
                                           popped->intentId, popped->verb);
                session->PushOutbound(ev.dump());
            }
            ++intentsDispatchedTotal_;
            didWork = true;

            // Wait — arm the suspension and stop draining (Wait is
            // universal-pause; no further intents until it lifts).
            if (popped->intent.kind == IntentKind::Wait)
            {
                isWaiting_         = true;
                waitUntil_         = std::chrono::steady_clock::now() +
                                     std::chrono::milliseconds(popped->intent.waitMs);
                waitingIntentId_   = popped->intentId;
                waitingIntentVerb_ = popped->verb;
                return true;
            }

            // Move — sub-tick dispatch + multi-tick hold via
            // inFlightMove_. Same shape as before. Stop draining
            // after setting the slot (one blocking dispatch per tick).
            //
            // ComeBack is rewritten into a Move at dispatch time using
            // the master's *current* position, so the agent's intent
            // doesn't go stale between push and execute. Failure to
            // resolve master emits a terminal failed inline so the
            // queue can keep draining.
            if (popped->intent.kind == IntentKind::ComeBack)
            {
                PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
                Player* master = ai ? ai->GetMaster() : nullptr;
                std::string err;
                if (!master)
                    err = "no master assigned";
                else if (master->GetMapId() != bot->GetMapId())
                    err = "master is on a different map (cross-map TP needed)";
                if (!err.empty())
                {
                    json result = {{"ok", false}, {"verb", "come_back"}, {"error", err}};
                    json ev = BuildIntentEvent(bot, "intent_failed",
                                               popped->intentId, popped->verb);
                    ev["result"] = result;
                    session->PushOutbound(ev.dump());
                    Sbywow::Bridge::BotSession::TerminalIntent rec;
                    rec.intentId   = popped->intentId;
                    rec.verb       = popped->verb;
                    rec.kind       = "intent_failed";
                    rec.resultJson = result.dump();
                    session->RecordTerminal(std::move(rec));
                    continue;
                }
                popped->intent.kind = IntentKind::Move;
                popped->intent.x    = master->GetPositionX();
                popped->intent.y    = master->GetPositionY();
                popped->intent.z    = master->GetPositionZ();
                popped->intent.map  = master->GetMapId();
                // Fall through into the Move dispatch below.
            }
            if (popped->intent.kind == IntentKind::Move)
            {
                if (DispatchMoveIntent(bot, session, popped))
                    return true;  // either set slot or emitted failed
                continue;          // unreachable, defensive
            }

            // Cast verbs — sub-tick dispatch; the executor decides if
            // the cast is in-flight (cast-time/channeled) or
            // terminated inline (instant). DispatchCastIntent
            // handles slot setup OR terminal emit accordingly.
            if (popped->intent.kind == IntentKind::CastSpell ||
                popped->intent.kind == IntentKind::UseItem   ||
                popped->intent.kind == IntentKind::Mount)
            {
                bool inflight = DispatchCastIntent(bot, session, popped);
                if (inflight)
                    return true;  // slot set; stop draining (one blocking per tick)
                // Else terminated inline; continue to drain more.
                continue;
            }

            // All other verbs are non-blocking sub-tick:
            // ExecuteIntent runs synchronously, terminal fires inline.
            DispatchInstantIntent(bot, session, popped);
            // Continue draining — non-blocking intents chain inside
            // a single tick.
        }

        // Queue empty (or only blocking intents waiting on slots).
        // Idle delegation only fires when nothing is in flight either.
        if (!didWork && !inFlightMove_ && !inFlightCast_ && !isWaiting_)
        {
            if (session->IsFollowMode() && defaultEngine_)
            {
                ++defaultEngineTicksTotal_;
                return defaultEngine_->DoNextAction(target, depth, minimal);
            }
            TickReactiveAutonomic(bot);
            return false;
        }

        return didWork;
    }

    // ---- Blocking-intent classifier ----------------------------------
    //
    // The queue drain in DoNextAction asks "can this intent fire while
    // a blocking slot (Move/Cast) is occupied?" Non-blocking intents
    // run inline same tick and don't compete with in-flight work. A
    // cast intent's blocking-ness depends on whether the resolved
    // spell has a cast time > 0 (cast-time / channeled) or 0 (instant).

    bool SbywowAgentEngine::IsBlockingIntent(Player* bot, Intent const& intent)
    {
        switch (intent.kind)
        {
            case IntentKind::Move:
            case IntentKind::Wait:
            case IntentKind::ComeBack:   // wraps Move; same in-flight semantics
                return true;
            case IntentKind::CastSpell:
            {
                if (!intent.spellId)
                    return false;  // will fail at dispatch with clear error
                SpellInfo const* si = sSpellMgr->GetSpellInfo(intent.spellId);
                if (!si) return false;
                return si->CalcCastTime() > 0 || si->IsChanneled();
            }
            case IntentKind::UseItem:
            {
                Item* item = nullptr;
                if (intent.itemGuid)
                    item = bot->GetItemByGuid(ObjectGuid(intent.itemGuid));
                else if (intent.itemEntry)
                    item = bot->GetItemByEntry(intent.itemEntry);
                if (!item) return false;
                ItemTemplate const* tpl = item->GetTemplate();
                if (!tpl) return false;
                for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
                {
                    if (tpl->Spells[i].SpellId != 0 &&
                        tpl->Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
                    {
                        SpellInfo const* si = sSpellMgr->GetSpellInfo(tpl->Spells[i].SpellId);
                        if (!si) return false;
                        return si->CalcCastTime() > 0 || si->IsChanneled();
                    }
                }
                return false;
            }
            case IntentKind::Mount:
            {
                if (intent.spellId)
                {
                    SpellInfo const* si = sSpellMgr->GetSpellInfo(intent.spellId);
                    return si && (si->CalcCastTime() > 0 || si->IsChanneled());
                }
                // Auto-pick path: most mounts are 1.5s cast in WoW
                // 3.3.5 — assume blocking.
                return true;
            }
            default:
                return false;
        }
    }

    // ---- In-flight Move polling --------------------------------------
    //
    // When inFlightMove_ is set, check arrival / path-replaced /
    // timeout each tick and emit terminal when one fires. Falls back
    // (no return) so the queue drain can run non-blocking intents in
    // parallel with the move.

    void SbywowAgentEngine::PollInFlightMove(Player* bot, std::shared_ptr<Sbywow::Bridge::BotSession> const& session)
    {
        if (!inFlightMove_)
            return;
        float dist2d = bot->GetExactDist2d(inFlightMoveX_, inFlightMoveY_);
        bool  arrived = dist2d <= kMoveArrivalThreshold;
        auto  elapsed = std::chrono::steady_clock::now() - inFlightMoveDispatchAt_;
        bool  timedOut = elapsed > std::chrono::milliseconds(kMoveTimeoutMs);

        MotionMaster* mm = bot->GetMotionMaster();
        bool generatorActive = mm &&
            mm->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE;

        if (arrived)
        {
            if (session)
            {
                json result = {
                    {"ok",       true},
                    {"verb",     "move_to"},
                    {"arrived",  true},
                    {"position", {bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()}},
                    {"distance", dist2d}
                };
                json ev = BuildIntentEvent(bot, "intent_completed",
                                           inFlightMoveIntentId_, inFlightMoveVerb_);
                ev["result"] = result;
                session->PushOutbound(ev.dump());

                Sbywow::Bridge::BotSession::TerminalIntent rec;
                rec.intentId   = inFlightMoveIntentId_;
                rec.verb       = inFlightMoveVerb_;
                rec.kind       = "intent_completed";
                rec.resultJson = result.dump();
                session->RecordTerminal(std::move(rec));
            }
            inFlightMove_ = false;
            inFlightMoveIntentId_ = 0;
            inFlightMoveVerb_.clear();
        }
        else if (!generatorActive || timedOut)
        {
            if (session)
            {
                json result = {
                    {"ok",       false},
                    {"verb",     "move_to"},
                    {"error",    timedOut ? "move timeout" :
                                            "move generator replaced or path failed"},
                    {"position", {bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()}},
                    {"target",   {inFlightMoveX_, inFlightMoveY_, inFlightMoveZ_}},
                    {"distance", dist2d}
                };
                json ev = BuildIntentEvent(bot, "intent_failed",
                                           inFlightMoveIntentId_, inFlightMoveVerb_);
                ev["result"] = result;
                session->PushOutbound(ev.dump());

                Sbywow::Bridge::BotSession::TerminalIntent rec;
                rec.intentId   = inFlightMoveIntentId_;
                rec.verb       = inFlightMoveVerb_;
                rec.kind       = "intent_failed";
                rec.resultJson = result.dump();
                session->RecordTerminal(std::move(rec));
            }
            inFlightMove_ = false;
            inFlightMoveIntentId_ = 0;
            inFlightMoveVerb_.clear();
        }
        // else: still moving — fall through, do not return.
    }

    // ---- In-flight Cast polling --------------------------------------
    //
    // Detection signal: FindCurrentSpellBySpellId returns non-null
    // while the spell is in m_currentSpells (CURRENT_GENERIC_SPELL or
    // CURRENT_CHANNELED_SPELL). When it returns null, the cast either
    // completed (cooldown will be set) or was interrupted (no
    // cooldown). Correlate via HasSpellCooldown to disambiguate.
    //
    // Edge case: instant-cast spells with no cooldown can complete
    // without setting either signal. Those terminate inline at
    // dispatch via DispatchCastIntent and never reach this poller.

    void SbywowAgentEngine::PollInFlightCast(Player* bot, std::shared_ptr<Sbywow::Bridge::BotSession> const& session)
    {
        if (!inFlightCast_)
            return;
        Spell* live = bot->FindCurrentSpellBySpellId(inFlightCastSpellId_);
        auto elapsed = std::chrono::steady_clock::now() - inFlightCastDispatchAt_;
        bool timedOut = elapsed > std::chrono::milliseconds(kCastTimeoutMs);

        if (live && !timedOut)
            return;  // still casting

        bool succeeded = bot->HasSpellCooldown(inFlightCastSpellId_);
        SpellInfo const* si = sSpellMgr->GetSpellInfo(inFlightCastSpellId_);
        std::string spellName = (si && si->SpellName[0]) ? si->SpellName[0] : std::string{};

        if (session)
        {
            json result;
            std::string kind;
            if (succeeded)
            {
                result = {
                    {"ok",         true},
                    {"verb",       inFlightCastVerb_},
                    {"spell_id",   inFlightCastSpellId_},
                    {"spell_name", spellName},
                    {"completed",  true}
                };
                kind = "intent_completed";
            }
            else
            {
                result = {
                    {"ok",         false},
                    {"verb",       inFlightCastVerb_},
                    {"spell_id",   inFlightCastSpellId_},
                    {"spell_name", spellName},
                    {"error",      timedOut ? "cast timeout" :
                                              "cast interrupted (movement / damage / cancel / out of range)"}
                };
                kind = "intent_failed";
            }
            json ev = BuildIntentEvent(bot, kind, inFlightCastIntentId_, inFlightCastVerb_);
            ev["result"] = result;
            session->PushOutbound(ev.dump());

            Sbywow::Bridge::BotSession::TerminalIntent rec;
            rec.intentId   = inFlightCastIntentId_;
            rec.verb       = inFlightCastVerb_;
            rec.kind       = kind;
            rec.resultJson = result.dump();
            session->RecordTerminal(std::move(rec));
        }

        inFlightCast_ = false;
        inFlightCastIntentId_ = 0;
        inFlightCastVerb_.clear();
        inFlightCastSpellId_ = 0;
    }

    // ---- Per-kind dispatch helpers (used by DoNextAction) ------------

    bool SbywowAgentEngine::DispatchMoveIntent(Player* bot,
                                               std::shared_ptr<Sbywow::Bridge::BotSession> const& session,
                                               std::shared_ptr<Sbywow::Bridge::PendingIntent> const& pending)
    {
        std::string outStr = ExecuteMove(bot, pending->intent);
        json result;
        try { result = json::parse(outStr); }
        catch (std::exception const&) {
            result = {{"ok", false}, {"error", "engine returned non-JSON"}};
        }
        bool ok = result.value("ok", false);
        if (!ok)
        {
            json ev = BuildIntentEvent(bot, "intent_failed",
                                       pending->intentId, pending->verb);
            ev["result"] = result;
            if (session)
            {
                session->PushOutbound(ev.dump());
                Sbywow::Bridge::BotSession::TerminalIntent rec;
                rec.intentId   = pending->intentId;
                rec.verb       = pending->verb;
                rec.kind       = "intent_failed";
                rec.resultJson = result.dump();
                session->RecordTerminal(std::move(rec));
            }
            return true;  // we did dispatch (and emitted terminal)
        }
        inFlightMove_           = true;
        inFlightMoveIntentId_   = pending->intentId;
        inFlightMoveVerb_       = pending->verb;
        inFlightMoveX_          = pending->intent.x;
        inFlightMoveY_          = pending->intent.y;
        inFlightMoveZ_          = pending->intent.z;
        inFlightMoveDispatchAt_ = std::chrono::steady_clock::now();
        return true;
    }

    bool SbywowAgentEngine::DispatchCastIntent(Player* bot,
                                               std::shared_ptr<Sbywow::Bridge::BotSession> const& session,
                                               std::shared_ptr<Sbywow::Bridge::PendingIntent> const& pending)
    {
        // Each of these executors dispatches the cast and returns
        // JSON. If the spell is now in m_currentSpells (cast-time or
        // channeled), the result has `_inflight: true` AND a
        // populated `spell_id` field — we set the in-flight slot and
        // suppress the terminal emit. Otherwise the result is a
        // normal terminal (instant-completed or pre-dispatch failed).
        std::string outStr = ExecuteIntent(bot, pending->intent);
        json result;
        try { result = json::parse(outStr); }
        catch (std::exception const&) {
            result = {{"ok", false}, {"error", "engine returned non-JSON"}};
        }
        bool inflight = result.value("_inflight", false);
        // _inflight is an internal marker between the executor and the
        // dispatcher; strip it before the result hits SSE / the
        // recovery ring so harness consumers don't see it.
        result.erase("_inflight");

        if (inflight)
        {
            inFlightCast_           = true;
            inFlightCastIntentId_   = pending->intentId;
            inFlightCastVerb_       = pending->verb;
            inFlightCastSpellId_    = result.value("spell_id", 0u);
            inFlightCastDispatchAt_ = std::chrono::steady_clock::now();
            return true;
        }

        // Inline terminal (instant cast or pre-dispatch failure).
        bool ok = result.value("ok", false);
        std::string kind = ok ? "intent_completed" : "intent_failed";
        json ev = BuildIntentEvent(bot, kind, pending->intentId, pending->verb);
        ev["result"] = result;
        if (session)
        {
            session->PushOutbound(ev.dump());
            Sbywow::Bridge::BotSession::TerminalIntent rec;
            rec.intentId   = pending->intentId;
            rec.verb       = pending->verb;
            rec.kind       = std::move(kind);
            rec.resultJson = result.dump();
            session->RecordTerminal(std::move(rec));
        }
        return false;
    }

    void SbywowAgentEngine::DispatchInstantIntent(Player* bot,
                                                  std::shared_ptr<Sbywow::Bridge::BotSession> const& session,
                                                  std::shared_ptr<Sbywow::Bridge::PendingIntent> const& pending)
    {
        std::string outStr = ExecuteIntent(bot, pending->intent);
        json result;
        try { result = json::parse(outStr); }
        catch (std::exception const&) { result = {{"ok", false}, {"error", "engine returned non-JSON"}}; }
        bool ok = result.value("ok", false);
        std::string kind = ok ? "intent_completed" : "intent_failed";
        json ev = BuildIntentEvent(bot, kind, pending->intentId, pending->verb);
        ev["result"] = result;
        if (session)
        {
            session->PushOutbound(ev.dump());
            Sbywow::Bridge::BotSession::TerminalIntent rec;
            rec.intentId   = pending->intentId;
            rec.verb       = pending->verb;
            rec.kind       = std::move(kind);
            rec.resultJson = result.dump();
            session->RecordTerminal(std::move(rec));
        }
    }

    std::string SbywowAgentEngine::ExecuteIntent(Player* bot, Intent const& intent)
    {
        switch (intent.kind)
        {
            case IntentKind::Move:                   return ExecuteMove    (bot, intent);
            case IntentKind::Interact:               return ExecuteInteract(bot, intent);
            case IntentKind::Say:                    return ExecuteSay     (bot, intent);
            case IntentKind::DoAction:               return ExecuteDoAction(bot, intent);
            case IntentKind::Wait:
                // Wait is handled in DoNextAction directly (engine
                // state); should never reach this dispatcher.
                return json{{"ok", false}, {"error", "wait reached ExecuteIntent — bug"}}.dump();
            case IntentKind::BuyItem:                return ExecuteBuyItem            (bot, intent);
            case IntentKind::SellItem:               return ExecuteSellItem           (bot, intent);
            case IntentKind::SelectGossipOption:     return ExecuteSelectGossipOption (bot, intent);
            case IntentKind::TradeInitiate:          return ExecuteTradeInitiate      (bot, intent);
            case IntentKind::TradeOfferItem:         return ExecuteTradeOfferItem     (bot, intent);
            case IntentKind::TradeOfferMoney:        return ExecuteTradeOfferMoney    (bot, intent);
            case IntentKind::TradeAccept:            return ExecuteTradeAccept        (bot, intent);
            case IntentKind::TradeCancel:            return ExecuteTradeCancel        (bot, intent);
            case IntentKind::EquipItem:              return ExecuteEquipItem          (bot, intent);
            case IntentKind::UnequipItem:            return ExecuteUnequipItem        (bot, intent);
            case IntentKind::DestroyItem:            return ExecuteDestroyItem        (bot, intent);
            case IntentKind::UseItem:                return ExecuteUseItem            (bot, intent);
            case IntentKind::CastSpell:              return ExecuteCastSpell          (bot, intent);
            case IntentKind::Mount:                  return ExecuteMount              (bot, intent);
            case IntentKind::Dismount:               return ExecuteDismount           (bot, intent);
            case IntentKind::InteractGameObject:     return ExecuteInteractGameObject (bot, intent);
            case IntentKind::LootTarget:             return ExecuteLootTarget         (bot, intent);
            case IntentKind::MailSend:               return ExecuteMailSend           (bot, intent);
            case IntentKind::MailTakeItem:           return ExecuteMailTakeItem       (bot, intent);
            case IntentKind::MailTakeMoney:          return ExecuteMailTakeMoney      (bot, intent);
            case IntentKind::QuestAccept:            return ExecuteQuestAccept        (bot, intent);
            case IntentKind::QuestComplete:          return ExecuteQuestComplete      (bot, intent);
            case IntentKind::QuestAbandon:           return ExecuteQuestAbandon       (bot, intent);
            case IntentKind::QuestShare:             return ExecuteQuestShare         (bot, intent);
            case IntentKind::GroupAcceptInvite:      return ExecuteGroupAcceptInvite  (bot, intent);
            case IntentKind::GroupDeclineInvite:     return ExecuteGroupDeclineInvite (bot, intent);
            case IntentKind::GroupLeave:             return ExecuteGroupLeave         (bot, intent);
            case IntentKind::GroupPromoteLeader:     return ExecuteGroupPromoteLeader (bot, intent);
            case IntentKind::GroupReadyCheckRespond: return ExecuteGroupReadyCheckRespond(bot, intent);
            case IntentKind::TradeAcceptInvite:      return ExecuteTradeAcceptInvite  (bot, intent);
            case IntentKind::FaceTarget:             return ExecuteFaceTarget         (bot, intent);
            case IntentKind::ComeBack:
                // ComeBack is intercepted in the queue drain and rewritten
                // into a Move using the master's current position before
                // dispatch. Reaching here means a routing bug.
                return json{{"ok", false}, {"error", "come_back must be handled in queue drain"}}.dump();
        }
        return json{{"ok", false}, {"error", "unknown intent kind"}}.dump();
    }

    std::string SbywowAgentEngine::ExecuteMove(Player* bot, Intent const& intent)
    {
        // Optional map id: if set and non-zero, must equal current map.
        // Cross-map TP is a separate verb (future work) with a stricter
        // contract — refuse here rather than teleport quietly.
        if (intent.map != 0 && intent.map != bot->GetMapId())
        {
            return json{
                {"ok", false},
                {"error", "move: map mismatch (cross-map needs a teleport verb)"},
                {"requested_map", intent.map},
                {"current_map",   bot->GetMapId()}
            }.dump();
        }

        PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
        if (!ai)
            return json{{"ok", false}, {"error", "no PlayerbotAI for this bot"}}.dump();

        // Use playerbots' canonical movement-allowed predicate. Covers
        // dead, charmed, polymorphed, in-flight, being teleported.
        if (!ai->CanMove())
            return json{{"ok", false}, {"error", "bot cannot move (dead/CC'd/in-flight)"}}.dump();

        MotionMaster* mm = bot->GetMotionMaster();
        if (!mm)
            return json{{"ok", false}, {"error", "no motion master"}}.dump();

        float fromX = bot->GetPositionX();
        float fromY = bot->GetPositionY();
        float fromZ = bot->GetPositionZ();

        // ---- Defensive validation (Batch H, post-2026-05-03 e2e) ------
        // Agents will routinely supply rough x/y/z coords (LLM-derived,
        // user-typed, etc.). Without validation, bad Z + cliff terrain +
        // sparse mmap coverage produces the "flying through the air"
        // glitch the user reported during the grind setup. Three layers:
        //
        //   1. Snap Z to terrain when wildly off (>5y delta).
        //   2. Pre-flight PathGenerator to verify a navigable path
        //      exists before dispatching MovePoint.
        //   3. Reject paths that would shortcut through geometry or
        //      land far from the intended polygon.
        //
        // --target mode (Batch B) bypasses these issues at the source
        // since target positions are by definition real terrain coords;
        // raw x/y/z from the agent gets the full safety treatment.
        float targetX = intent.x;
        float targetY = intent.y;
        float targetZ = intent.z;
        float zSnapDelta = 0.f;
        bool zWasSnapped = false;
        if (Map* map = bot->GetMap())
        {
            // Probe terrain Z. Search downward from a generous height
            // above the agent's claimed Z (catches "I gave Z=25 but
            // terrain is Z=50" by searching from 75 down).
            float probeZ = targetZ + 50.f;
            float terrainZ = map->GetHeight(targetX, targetY, probeZ,
                                            /*checkVMap=*/ true,
                                            DEFAULT_HEIGHT_SEARCH);
            if (terrainZ > INVALID_HEIGHT)
            {
                float delta = std::abs(targetZ - terrainZ);
                if (delta > 5.0f)
                {
                    // Snap to terrain + 0.5y to avoid clipping into the
                    // floor on splines. Surface the snap so the agent
                    // can learn its coords are off.
                    zSnapDelta = terrainZ - targetZ;
                    targetZ = terrainZ + 0.5f;
                    zWasSnapped = true;
                }
            }
        }

        // Path pre-flight. If the path can't be built, no point asking
        // MovePoint to try — the spline engine's behavior on no-mmap or
        // off-mesh destinations is to fall through to straight-line
        // (looks like flying / clipping). Reject before dispatch.
        PathType pathType = PATHFIND_BLANK;
        {
            PathGenerator path(bot);
            path.CalculatePath(targetX, targetY, targetZ, /*forceDest=*/ false);
            pathType = path.GetPathType();
        }

        auto pathTypeName = [](PathType t) -> char const* {
            if (t & PATHFIND_NORMAL)         return "normal";
            if (t & PATHFIND_SHORTCUT)       return "shortcut";  // bad
            if (t & PATHFIND_INCOMPLETE)     return "incomplete";
            if (t & PATHFIND_NOPATH)         return "no_path";
            if (t & PATHFIND_NOT_USING_PATH) return "no_mmap";
            if (t & PATHFIND_SHORT)          return "short";
            if (t & PATHFIND_FARFROMPOLY)    return "far_from_poly";
            return "blank";
        };

        // Hard rejects — these signal the path would not work properly.
        // SHORTCUT means the spline would cut through geometry; NOPATH
        // means dest is unreachable; NOT_USING_PATH means we're outside
        // mmap coverage and can't navigate safely; FARFROMPOLY means the
        // start or end isn't on the navmesh (would clip).
        if ((pathType & PATHFIND_SHORTCUT) ||
            (pathType & PATHFIND_NOPATH)   ||
            (pathType & PATHFIND_NOT_USING_PATH) ||
            (pathType & PATHFIND_FARFROMPOLY))
        {
            return json{
                {"ok",            false},
                {"reason",        "unsafe_path"},
                {"path_type",     pathTypeName(pathType)},
                {"path_type_raw", static_cast<int>(pathType)},
                {"error",         "destination unreachable / off-mesh / would clip — use --target <guid> for a guaranteed-valid coord"},
                {"to",            {intent.x, intent.y, intent.z}},
                {"z_snapped_to",  zWasSnapped ? targetZ : intent.z}
            }.dump();
        }

        float dist = bot->GetExactDist(targetX, targetY, targetZ);

        // Match mod-playerbots' MovementAction::DoMovePoint shape:
        // stand up if sitting, then Clear() and MovePoint with
        // generatePath=true so mmaps are honored. forceDestination=false
        // lets the spline engine refuse if the target is unreachable
        // rather than teleport into geometry.
        if (bot->IsSitState())
            bot->SetStandState(UNIT_STAND_STATE_STAND);
        mm->Clear();
        mm->MovePoint(/*id*/ 0, targetX, targetY, targetZ, FORCED_MOVEMENT_NONE,
                      /*speed*/ 0.f, /*orientation*/ 0.f,
                      /*generatePath*/ true,
                      /*forceDestination*/ false);

        // Verify a generator was actually pushed. If not, MovePoint
        // silently no-op'd and the bot won't move.
        bool generatorRunning =
            mm->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE;

        json result = {
            {"ok",       generatorRunning},
            {"verb",     "move_to"},
            {"map",      bot->GetMapId()},
            {"from",     {fromX, fromY, fromZ}},
            {"to",       {targetX, targetY, targetZ}},
            {"requested",{intent.x, intent.y, intent.z}},
            {"distance", dist},
            {"path_type", pathTypeName(pathType)}
        };
        if (zWasSnapped)
        {
            result["z_snapped"]       = true;
            result["z_snap_delta"]    = zSnapDelta;
        }
        if (!generatorRunning)
        {
            result["reason"] = "generator_not_started";
            result["error"]  = "MovePoint dispatched but no generator running — likely path-gen failure";
        }
        return result.dump();
    }

    namespace
    {
        // Decode a creature's npc_flags into role tags. Same shape as
        // bridge's find_nearby uses; duplicated here to keep the engine
        // independent of bridge internals. If we ever want one source
        // of truth, lift to a shared helper file.
        json DecodeCreatureFlags(Creature const* c)
        {
            json flags = json::array();
            if (c->IsGossip())          flags.push_back("gossip");
            if (c->IsQuestGiver())      flags.push_back("questgiver");
            if (c->IsTrainer())         flags.push_back("trainer");
            if (c->IsVendor())          flags.push_back("vendor");
            if (c->IsArmorer())         flags.push_back("repair");
            if (c->IsTaxi())            flags.push_back("flightmaster");
            if (c->IsBanker())          flags.push_back("banker");
            if (c->IsInnkeeper())       flags.push_back("innkeeper");
            if (c->IsAuctioner())       flags.push_back("auctioneer");
            if (c->IsBattleMaster())    flags.push_back("battlemaster");
            if (c->IsTabardDesigner())  flags.push_back("tabard");
            if (c->IsSpiritHealer())    flags.push_back("spirithealer");
            if (c->IsSpiritGuide())     flags.push_back("spiritguide");
            if (c->HasNpcFlag(UNIT_NPC_FLAG_STABLEMASTER)) flags.push_back("stablemaster");
            if (c->HasNpcFlag(UNIT_NPC_FLAG_MAILBOX))      flags.push_back("mailbox");
            if (c->HasNpcFlag(UNIT_NPC_FLAG_GUILD_BANKER)) flags.push_back("guild_banker");
            return flags;
        }
    }

    std::string SbywowAgentEngine::ExecuteInteract(Player* bot, Intent const& intent)
    {
        ObjectGuid og(intent.guid);
        if (!og.IsAnyTypeCreature())
        {
            if (og.IsGameObject())
                return json{{"ok", false},
                            {"error", "interact: gameobjects not yet supported"}}.dump();
            return json{{"ok", false},
                        {"error", "interact: guid is not a creature"}}.dump();
        }

        Creature* npc = bot->GetNPCIfCanInteractWith(og, UNIT_NPC_FLAG_NONE);
        if (!npc)
        {
            if (Creature* c = ObjectAccessor::GetCreature(*bot, og))
                return json{
                    {"ok",    false},
                    {"error", "creature out of interact range or not visible"},
                    {"name",  c->GetName()},
                    {"dist",  bot->GetExactDist(c)},
                    {"alive", c->IsAlive()}
                }.dump();
            return json{{"ok", false}, {"error", "creature not found on bot's map"}}.dump();
        }

        json out = {
            {"ok",             true},
            {"verb",           "interact_with"},
            {"guid",           intent.guid},
            {"kind",           "creature"},
            {"entry",          npc->GetEntry()},
            {"name",           npc->GetName()},
            {"flags",          DecodeCreatureFlags(npc)},
            {"gossip_menu_id", npc->GetCreatureTemplate()->GossipMenuId},
            {"dist",           bot->GetExactDist(npc)}
        };

        // Drive HandleGossipHelloOpcode through the bot's session.
        // Mirrors GossipHelloAction's path; safe under same conditions
        // mod-playerbots invokes it routinely.
        bool gossipOpened = false;
        std::string gossipError;
        try
        {
            WorldPacket data;
            data << og;
            bot->GetSession()->HandleGossipHelloOpcode(data);
            gossipOpened = true;
        }
        catch (std::exception const& e)
        {
            gossipError = std::string("gossip_hello threw: ") + e.what();
        }
        out["gossip_opened"] = gossipOpened;
        if (!gossipError.empty())
            out["gossip_error"] = gossipError;

        if (gossipOpened && bot->PlayerTalkClass)
        {
            json gossipOpts = json::array();
            GossipMenu& menu = bot->PlayerTalkClass->GetGossipMenu();
            for (auto const& [idx, item] : menu.GetMenuItems())
            {
                gossipOpts.push_back({
                    {"index", idx},
                    {"icon",  static_cast<int>(item.MenuItemIcon)},
                    {"text",  item.Message}
                });
            }
            out["gossip_options"] = gossipOpts;
            out["gossip_menu_open_id"] = menu.GetMenuId();

            // gossip.menu_opened SSE event so a harness watching the
            // event stream learns that a navigable menu is up — without
            // having to correlate from the interact_with response.
            // Pure vendors that skip the menu (server sends list directly)
            // don't fire this event; the menu stays empty for them.
            if (!menu.Empty())
            {
                if (auto session = Sbywow::Bridge::BridgeServer::Instance().GetSession(bot->GetGUID()))
                {
                    json ev = {
                        {"channel",     "gossip"},
                        {"kind",        "menu_opened"},
                        {"bot_guid",    bot->GetGUID().GetRawValue()},
                        {"bot_name",    bot->GetName()},
                        {"npc_guid",    intent.guid},
                        {"npc_name",    npc->GetName()},
                        {"menu_id",     menu.GetMenuId()},
                        {"sender_guid", menu.GetSenderGUID().GetRawValue()},
                        {"options",     gossipOpts}
                    };
                    session->PushOutbound(ev.dump());
                }
            }
        }

        return out.dump();
    }

    std::string SbywowAgentEngine::ExecuteSay(Player* bot, Intent const& intent)
    {
        PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
        if (!ai)
            return json{{"ok", false}, {"error", "no PlayerbotAI for this bot"}}.dump();

        bool ok = false;
        if      (intent.channel == "say"   || intent.channel.empty()) ok = ai->Say(intent.text);
        else if (intent.channel == "yell")    ok = ai->Yell(intent.text);
        else if (intent.channel == "party")   ok = ai->SayToParty(intent.text);
        else if (intent.channel == "raid")    ok = ai->SayToRaid(intent.text);
        else if (intent.channel == "guild")   ok = ai->SayToGuild(intent.text);
        else if (intent.channel == "world")   ok = ai->SayToWorld(intent.text);
        else if (intent.channel == "master")  ok = ai->TellMaster(intent.text);
        else
        {
            return json{
                {"ok", false},
                {"error", "unknown channel: " + intent.channel +
                          " (say|yell|party|raid|guild|world|master)"}
            }.dump();
        }

        return json{
            {"ok",      ok},
            {"verb",    "say"},
            {"channel", intent.channel.empty() ? "say" : intent.channel},
            {"text",    intent.text}
        }.dump();
    }

    std::string SbywowAgentEngine::ExecuteDoAction(Player* bot, Intent const& intent)
    {
        PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
        if (!ai)
            return json{{"ok", false}, {"error", "no PlayerbotAI for this bot"}}.dump();

        Event ev;
        bool result = ai->DoSpecificAction(intent.actionName, ev,
                                           /*silent=*/true,
                                           intent.actionQualifier);

        return json{
            {"ok",        result},
            {"verb",      "do_action"},
            {"name",      intent.actionName},
            {"qualifier", intent.actionQualifier}
        }.dump();
    }

    namespace
    {
        // Standard "verb not yet wired" stub. All Phase 4 / 5 batches
        // declared their executors up front so the dispatch switch is
        // complete; impls land batch-by-batch. Stubs return a clear
        // error so the harness knows the verb is reserved but not
        // active yet.
        std::string NotImplemented(char const* verb)
        {
            return json{
                {"ok",    false},
                {"verb",  verb},
                {"error", "not yet implemented (stub)"}
            }.dump();
        }
    }

    // ---- Phase 4: vendor verbs ----------------------------------------

    std::string SbywowAgentEngine::ExecuteBuyItem(Player* bot, Intent const& intent)
    {
        if (!intent.vendorGuid || !intent.itemEntry)
            return json{{"ok", false}, {"error", "buy_item requires vendor_guid and item_entry"}}.dump();

        ObjectGuid vGuid(intent.vendorGuid);
        Creature* npc = bot->GetNPCIfCanInteractWith(vGuid, UNIT_NPC_FLAG_VENDOR);
        if (!npc)
            return json{{"ok", false}, {"error", "vendor not found / out of range / not a vendor"}}.dump();

        // Resolve item_entry → vendor slot. The agent reasons in
        // entries; the buy API wants the slot index. Iterate the
        // vendor's item list and match.
        VendorItemData const* vItems = npc->GetVendorItems();
        if (!vItems || vItems->Empty())
            return json{{"ok", false}, {"error", "vendor has no items"}}.dump();

        uint32 slot = 0;
        bool found = false;
        for (uint32 i = 0; i < vItems->GetItemCount(); ++i)
        {
            if (VendorItem const* vi = vItems->GetItem(i))
            {
                if (vi->item == intent.itemEntry)
                {
                    slot = i;
                    found = true;
                    break;
                }
            }
        }
        if (!found)
            return json{
                {"ok", false},
                {"error", "item_entry not in vendor catalog"},
                {"item_entry", intent.itemEntry}
            }.dump();

        // intent.quantity is now ITEMS (not stacks/purchases). Each
        // BuyItemFromVendorSlot call buys 1 "purchase" which yields
        // tpl->BuyCount items (water = 5/purchase, weapons = 1/purchase).
        // Compute purchases = ceil(items / BuyCount) so the agent gets
        // AT LEAST the requested item count. This matches how an agent
        // naturally thinks ("I want 10 water"), not the raw API.
        ItemTemplate const* tpl = sObjectMgr->GetItemTemplate(intent.itemEntry);
        uint32 buyCount = (tpl && tpl->BuyCount > 0) ? tpl->BuyCount : 1u;
        uint32 itemsRequested = intent.quantity ? intent.quantity : 1u;
        uint32 purchasesNeeded = (itemsRequested + buyCount - 1) / buyCount;
        // BuyItemFromVendorSlot uses uint8 for count (number of
        // purchases), so cap at 255. For higher quantities the agent
        // can chain calls.
        uint8 purchases = static_cast<uint8>(std::min<uint32>(purchasesNeeded, 255u));

        // AC's BuyItemFromVendorSlot returns `crItem->maxcount != 0`
        // at the bottom — i.e., false for infinite-stock items
        // (Player.cpp:10826) even on successful purchase. Detect via
        // money + inventory delta instead of trusting the bool.
        uint32 moneyBefore   = bot->GetMoney();
        uint32 itemCountBefore = bot->GetItemCount(intent.itemEntry, /*inBankAlso=*/ false);
        bot->BuyItemFromVendorSlot(vGuid, slot, intent.itemEntry, purchases, NULL_BAG, NULL_SLOT);
        uint32 moneyAfter    = bot->GetMoney();
        uint32 itemCountAfter = bot->GetItemCount(intent.itemEntry, false);

        bool moneyDropped = moneyAfter < moneyBefore;
        bool itemsGained  = itemCountAfter > itemCountBefore;
        bool ok = moneyDropped || itemsGained;
        // (Both true is the normal case; one true alone happens for
        // free items or full inventory edge cases.)

        return json{
            {"ok",              ok},
            {"verb",            "buy_item"},
            {"vendor_guid",     intent.vendorGuid},
            {"vendor_name",     npc->GetName()},
            {"vendor_slot",     slot},
            {"item_entry",      intent.itemEntry},
            {"items_requested", itemsRequested},
            {"buy_count",       buyCount},      // items per vendor "purchase"
            {"purchases",       purchases},     // calls into vendor API
            {"money_before",    moneyBefore},
            {"money_after",     moneyAfter},
            {"copper_spent",    moneyBefore > moneyAfter ? moneyBefore - moneyAfter : 0u},
            {"items_before",    itemCountBefore},
            {"items_after",     itemCountAfter},
            {"items_gained",    itemCountAfter > itemCountBefore ? itemCountAfter - itemCountBefore : 0u}
        }.dump();
    }

    std::string SbywowAgentEngine::ExecuteSellItem(Player* bot, Intent const& intent)
    {
        if (!intent.vendorGuid)
            return json{{"ok", false}, {"error", "sell_item requires vendor_guid"}}.dump();
        if (!intent.itemGuid && !intent.itemEntry)
            return json{{"ok", false}, {"error", "sell_item requires item_guid or item_entry"}}.dump();

        // Resolve item_entry → guid (first matching item in bag) when
        // the agent passed entry-only. Lets the caller reason about
        // entries from snapshot without first looking up the specific
        // physical item's guid.
        Item* item = nullptr;
        if (intent.itemGuid)
            item = bot->GetItemByGuid(ObjectGuid(intent.itemGuid));
        else if (intent.itemEntry)
            item = bot->GetItemByEntry(intent.itemEntry);
        if (!item)
            return json{
                {"ok",         false},
                {"error",      "item not in bot's bag"},
                {"item_guid",  intent.itemGuid},
                {"item_entry", intent.itemEntry}
            }.dump();
        uint64_t resolvedGuid = item->GetGUID().GetRawValue();

        // Construct + populate a SellItem packet, hand to the session.
        // The handler does the validation (vendor proximity, item
        // ownership, refundable check, etc.) and the gold transfer.
        WorldPacket raw(CMSG_SELL_ITEM, 8 + 8 + 4);
        WorldPackets::Item::SellItem packet(std::move(raw));
        packet.VendorGuid = ObjectGuid(intent.vendorGuid);
        packet.ItemGuid   = ObjectGuid(resolvedGuid);
        packet.Count      = intent.quantity;   // 0 = sell whole stack

        // Capture pre-sell info for the response payload.
        std::string itemName;
        uint32      itemEntry = 0;
        if (ItemTemplate const* tpl = item->GetTemplate())
        {
            itemName  = tpl->Name1;
            itemEntry = tpl->ItemId;
        }
        uint32 moneyBefore = bot->GetMoney();

        bot->GetSession()->HandleSellItemOpcode(packet);

        uint32 moneyAfter = bot->GetMoney();
        Item* itemAfter = bot->GetItemByGuid(ObjectGuid(resolvedGuid));
        bool soldFully = (itemAfter == nullptr);

        return json{
            {"ok",            moneyAfter > moneyBefore || soldFully},
            {"verb",          "sell_item"},
            {"vendor_guid",   intent.vendorGuid},
            {"item_guid",     resolvedGuid},
            {"item_entry",    itemEntry},
            {"item_name",     itemName},
            {"requested",     intent.quantity},
            {"money_before",  moneyBefore},
            {"money_after",   moneyAfter},
            {"copper_gained", moneyAfter - moneyBefore},
            {"sold_fully",    soldFully}
        }.dump();
    }

    // ---- Phase 4 / 5 — stubs (impls land in their respective batches) -

    std::string SbywowAgentEngine::ExecuteSelectGossipOption(Player* bot, Intent const& intent)
    {
        if (!bot->PlayerTalkClass)
            return json{{"ok", false}, {"error", "no PlayerTalkClass on bot"}}.dump();
        GossipMenu& menu = bot->PlayerTalkClass->GetGossipMenu();
        if (menu.Empty())
            return json{{"ok", false}, {"error", "no gossip menu currently open"}}.dump();

        uint32 optionIdx = static_cast<uint32>(intent.intParam < 0 ? 0 : intent.intParam);
        if (!menu.GetItem(optionIdx))
            return json{
                {"ok",    false},
                {"error", "option_index not present in current menu"},
                {"option_index", optionIdx},
                {"available_count", menu.GetMenuItemCount()}
            }.dump();

        // Construct + dispatch the same opcode the client would send.
        // HandleGossipSelectOptionOpcode reads `guid >> menuId >>
        // gossipListId` (and an optional code string for IsCoded
        // options); we don't support coded options yet (rare, mostly
        // GM/admin menus).
        ObjectGuid sender = menu.GetSenderGUID();
        uint32     menuId = menu.GetMenuId();
        WorldPacket data(CMSG_GOSSIP_SELECT_OPTION, 8 + 4 + 4);
        data << sender;
        data << menuId;
        data << optionIdx;
        bot->GetSession()->HandleGossipSelectOptionOpcode(data);

        return json{
            {"ok",           true},
            {"verb",         "select_gossip_option"},
            {"option_index", optionIdx},
            {"menu_id",      menuId},
            {"sender_guid",  sender.GetRawValue()}
        }.dump();
    }
    std::string SbywowAgentEngine::ExecuteTradeInitiate(Player* bot, Intent const& intent)
    {
        if (!intent.guid)
            return json{{"ok", false}, {"error", "trade_initiate requires partner_guid"}}.dump();
        ObjectGuid partner(intent.guid);
        WorldPacket data(CMSG_INITIATE_TRADE, 8);
        data << partner;
        bot->GetSession()->HandleInitiateTradeOpcode(data);
        return json{
            {"ok",           true},
            {"verb",         "trade_initiate"},
            {"partner_guid", intent.guid}
        }.dump();
    }

    std::string SbywowAgentEngine::ExecuteTradeOfferItem(Player* bot, Intent const& intent)
    {
        if (!intent.itemGuid && !intent.itemEntry)
            return json{{"ok", false}, {"error", "trade_offer_item requires item_guid or item_entry"}}.dump();
        if (intent.intParam < 0 || intent.intParam >= TRADE_SLOT_COUNT)
            return json{
                {"ok", false},
                {"error", "trade_slot out of range (0-6, where 6 is non-traded)"},
                {"trade_slot", intent.intParam}
            }.dump();
        Item* item = nullptr;
        if (intent.itemGuid)
            item = bot->GetItemByGuid(ObjectGuid(intent.itemGuid));
        else if (intent.itemEntry)
            item = bot->GetItemByEntry(intent.itemEntry);
        if (!item)
            return json{
                {"ok",         false},
                {"error",      "item not in bot's bag"},
                {"item_guid",  intent.itemGuid},
                {"item_entry", intent.itemEntry}
            }.dump();

        // Handler reads uint8 tradeSlot, uint8 bag, uint8 slot.
        WorldPacket data(CMSG_SET_TRADE_ITEM, 3);
        data << uint8(intent.intParam);
        data << uint8(item->GetBagSlot());
        data << uint8(item->GetSlot());
        bot->GetSession()->HandleSetTradeItemOpcode(data);

        return json{
            {"ok",         true},
            {"verb",       "trade_offer_item"},
            {"trade_slot", intent.intParam},
            {"item_guid",  item->GetGUID().GetRawValue()},
            {"item_entry", item->GetTemplate() ? item->GetTemplate()->ItemId : 0}
        }.dump();
    }

    std::string SbywowAgentEngine::ExecuteTradeOfferMoney(Player* bot, Intent const& intent)
    {
        WorldPacket data(CMSG_SET_TRADE_GOLD, 4);
        data << uint32(intent.copper);
        bot->GetSession()->HandleSetTradeGoldOpcode(data);
        return json{
            {"ok",     true},
            {"verb",   "trade_offer_money"},
            {"copper", intent.copper}
        }.dump();
    }

    std::string SbywowAgentEngine::ExecuteTradeAccept(Player* bot, Intent const& /*intent*/)
    {
        WorldPacket data(CMSG_ACCEPT_TRADE, 0);
        bot->GetSession()->HandleAcceptTradeOpcode(data);
        return json{{"ok", true}, {"verb", "trade_accept"}}.dump();
    }

    std::string SbywowAgentEngine::ExecuteTradeCancel(Player* bot, Intent const& /*intent*/)
    {
        WorldPacket data(CMSG_CANCEL_TRADE, 0);
        bot->GetSession()->HandleCancelTradeOpcode(data);
        return json{{"ok", true}, {"verb", "trade_cancel"}}.dump();
    }

    std::string SbywowAgentEngine::ExecuteTradeAcceptInvite(Player* bot, Intent const& /*intent*/)
    {
        // CMSG_BEGIN_TRADE — accepts a pending trade invitation from
        // another player (the "Trade" button on the popup). Without
        // this verb our service-account mercs can't accept invitations
        // because TradeStatusAction.BeginTrade trigger doesn't fire
        // (root cause TBD — see Batch 9 investigation).
        TradeData* td = bot->GetTradeData();
        if (!td)
            return json{
                {"ok",    false},
                {"verb",  "trade_accept_invite"},
                {"error", "no pending trade invitation"}
            }.dump();
        WorldPacket data(CMSG_BEGIN_TRADE, 0);
        bot->GetSession()->HandleBeginTradeOpcode(data);
        Player* partner = td->GetTrader();
        return json{
            {"ok",            true},
            {"verb",          "trade_accept_invite"},
            {"partner_guid",  partner ? partner->GetGUID().GetRawValue() : 0u},
            {"partner_name",  partner ? partner->GetName() : std::string{}}
        }.dump();
    }

    std::string SbywowAgentEngine::ExecuteFaceTarget(Player* bot, Intent const& intent)
    {
        if (!intent.guid)
            return json{{"ok", false}, {"error", "face_target requires target_guid"}}.dump();
        // Try unit first, then gameobject — both have positions and the
        // agent might want to face either (gossip with NPC, mailbox /
        // chest object, etc.).
        ObjectGuid og(intent.guid);
        WorldObject* target = nullptr;
        if (Unit* u = ObjectAccessor::GetUnit(*bot, og))
            target = u;
        else if (og.IsGameObject())
            target = bot->GetMap()->GetGameObject(og);
        if (!target)
            return json{
                {"ok",          false},
                {"verb",        "face_target"},
                {"error",       "target not found / out of visibility"},
                {"target_guid", intent.guid}
            }.dump();
        bot->SetFacingToObject(target);
        return json{
            {"ok",          true},
            {"verb",        "face_target"},
            {"target_guid", intent.guid},
            {"target_name", target->GetName()},
            {"facing",      bot->GetOrientation()}
        }.dump();
    }

    std::string SbywowAgentEngine::ExecuteEquipItem(Player* bot, Intent const& intent)
    {
        if (!intent.itemGuid && !intent.itemEntry)
            return json{{"ok", false}, {"error", "equip_item requires item_guid or item_entry"}}.dump();
        Item* item = nullptr;
        if (intent.itemGuid)
            item = bot->GetItemByGuid(ObjectGuid(intent.itemGuid));
        else if (intent.itemEntry)
            item = bot->GetItemByEntry(intent.itemEntry);
        if (!item)
            return json{
                {"ok",         false},
                {"error",      "item not in bot's bag"},
                {"item_guid",  intent.itemGuid},
                {"item_entry", intent.itemEntry}
            }.dump();
        uint64_t resolvedGuid = item->GetGUID().GetRawValue();

        // intent.intParam: -1 = auto-find slot (the only mode v1
        // supports). Explicit equipment-slot targeting (HandleAutoEquip
        // ItemSlotOpcode) is a follow-up — use the auto path which
        // already picks correctly for the item's class/subclass.
        WorldPacket raw(CMSG_AUTOEQUIP_ITEM, 2);
        WorldPackets::Item::AutoEquipItem packet(std::move(raw));
        packet.SourceBag  = item->GetBagSlot();
        packet.SourceSlot = item->GetSlot();
        bot->GetSession()->HandleAutoEquipItemOpcode(packet);

        // Verify by re-reading the item's slot — if it's now in
        // INVENTORY_SLOT_BAG_0 with slot < EQUIPMENT_SLOT_END the equip
        // succeeded. The handler emits SendEquipError on failure but
        // doesn't return a status here.
        Item* after = bot->GetItemByGuid(ObjectGuid(resolvedGuid));
        bool equipped = false;
        uint8 finalSlot = 0;
        if (after)
        {
            finalSlot = after->GetSlot();
            equipped = (after->GetBagSlot() == INVENTORY_SLOT_BAG_0) &&
                       (finalSlot < EQUIPMENT_SLOT_END);
        }
        return json{
            {"ok",        equipped},
            {"verb",      "equip_item"},
            {"item_guid", resolvedGuid},
            {"final_slot", finalSlot},
            {"final_bag",  after ? after->GetBagSlot() : 0}
        }.dump();
    }

    std::string SbywowAgentEngine::ExecuteUnequipItem(Player* bot, Intent const& intent)
    {
        if (intent.intParam < 0 || intent.intParam >= EQUIPMENT_SLOT_END)
            return json{
                {"ok", false},
                {"error", "equip_slot out of range (0-18)"},
                {"slot", intent.intParam}
            }.dump();
        uint8 srcSlot = static_cast<uint8>(intent.intParam);
        Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, srcSlot);
        if (!item)
            return json{
                {"ok",    false},
                {"error", "no item equipped in that slot"},
                {"slot",  srcSlot}
            }.dump();

        // Find the first free backpack/bag slot to land the item in.
        // Walk backpack first (slots 23..38), then each bag's slots.
        uint8 destBag = INVENTORY_SLOT_BAG_0;
        uint8 destSlot = NULL_SLOT;
        for (uint8 s = INVENTORY_SLOT_ITEM_START; s < INVENTORY_SLOT_ITEM_END; ++s)
        {
            if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, s))
            {
                destSlot = s;
                break;
            }
        }
        if (destSlot == NULL_SLOT)
        {
            for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END && destSlot == NULL_SLOT; ++b)
            {
                Bag* bag = bot->GetBagByPos(b);
                if (!bag) continue;
                for (uint8 s = 0; s < bag->GetBagSize(); ++s)
                {
                    if (!bot->GetItemByPos(b, s))
                    {
                        destBag = b;
                        destSlot = s;
                        break;
                    }
                }
            }
        }
        if (destSlot == NULL_SLOT)
            return json{{"ok", false}, {"error", "no free bag slot to receive item"}}.dump();

        WorldPacket raw(CMSG_SWAP_ITEM, 4);
        WorldPackets::Item::SwapItem packet(std::move(raw));
        packet.DestinationBag  = destBag;
        packet.DestinationSlot = destSlot;
        packet.SourceBag       = INVENTORY_SLOT_BAG_0;
        packet.SourceSlot      = srcSlot;
        bot->GetSession()->HandleSwapItem(packet);

        Item* after = bot->GetItemByPos(destBag, destSlot);
        bool moved = (after != nullptr) && (after->GetGUID() == item->GetGUID());
        return json{
            {"ok",       moved},
            {"verb",     "unequip_item"},
            {"src_slot", srcSlot},
            {"dest_bag", destBag},
            {"dest_slot", destSlot}
        }.dump();
    }

    std::string SbywowAgentEngine::ExecuteDestroyItem(Player* bot, Intent const& intent)
    {
        if (!intent.itemGuid && !intent.itemEntry)
            return json{{"ok", false}, {"error", "destroy_item requires item_guid or item_entry"}}.dump();
        Item* item = nullptr;
        if (intent.itemGuid)
            item = bot->GetItemByGuid(ObjectGuid(intent.itemGuid));
        else if (intent.itemEntry)
            item = bot->GetItemByEntry(intent.itemEntry);
        if (!item)
            return json{
                {"ok",         false},
                {"error",      "item not in bot's bag"},
                {"item_guid",  intent.itemGuid},
                {"item_entry", intent.itemEntry}
            }.dump();
        uint64_t resolvedGuid = item->GetGUID().GetRawValue();

        ItemTemplate const* tpl = item->GetTemplate();
        std::string itemName = tpl ? tpl->Name1 : std::string{};
        uint32 itemEntry = tpl ? tpl->ItemId : 0;
        uint32 startCount = item->GetCount();

        if (intent.quantity == 0 || intent.quantity >= startCount)
        {
            // Destroy whole stack.
            bot->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
            return json{
                {"ok",         true},
                {"verb",       "destroy_item"},
                {"item_guid",  resolvedGuid},
                {"item_entry", itemEntry},
                {"item_name",  itemName},
                {"destroyed",  startCount}
            }.dump();
        }

        // Partial destroy via DestroyItemCount(item*, count, update).
        uint32 toDestroy = intent.quantity;
        bot->DestroyItemCount(item, toDestroy, true);
        return json{
            {"ok",         true},
            {"verb",       "destroy_item"},
            {"item_guid",  resolvedGuid},
            {"item_entry", itemEntry},
            {"item_name",  itemName},
            {"destroyed",  intent.quantity - toDestroy}  // toDestroy is residual
        }.dump();
    }
    std::string SbywowAgentEngine::ExecuteCastSpell(Player* bot, Intent const& intent)
    {
        if (!intent.spellId)
            return json{{"ok", false}, {"error", "cast_spell requires spell_id"}}.dump();
        SpellInfo const* si = sSpellMgr->GetSpellInfo(intent.spellId);
        if (!si)
            return json{
                {"ok",       false},
                {"error",    "unknown spell_id"},
                {"spell_id", intent.spellId}
            }.dump();
        Unit* target = bot;
        if (intent.guid)
        {
            if (Unit* t = ObjectAccessor::GetUnit(*bot, ObjectGuid(intent.guid)))
                target = t;
            else
                return json{{"ok", false}, {"error", "target not visible"}}.dump();
        }
        SpellCastResult res = bot->CastSpell(target, si, TRIGGERED_NONE);
        if (res != SPELL_CAST_OK)
        {
            return json{
                {"ok",               false},
                {"verb",             "cast_spell"},
                {"spell_id",         intent.spellId},
                {"spell_name",       si->SpellName[0] ? si->SpellName[0] : ""},
                {"target_guid",      target->GetGUID().GetRawValue()},
                {"target_name",      target->GetName()},
                {"cast_result",      static_cast<int>(res)},
                {"cast_result_name", EnumUtils::ToConstant(res)},
                {"error",            "cast pre-check failed"}
            }.dump();
        }
        // Cast accepted by CheckCast. If the spell is now in
        // m_currentSpells, it's a cast-time or channeled spell —
        // intent stays in-flight via inFlightCast_ slot. If not,
        // it was instant (already finished); terminal fires inline.
        bool inflight = bot->FindCurrentSpellBySpellId(intent.spellId) != nullptr;
        return json{
            {"ok",          true},
            {"verb",        "cast_spell"},
            {"spell_id",    intent.spellId},
            {"spell_name",  si->SpellName[0] ? si->SpellName[0] : ""},
            {"target_guid", target->GetGUID().GetRawValue()},
            {"target_name", target->GetName()},
            {"_inflight",   inflight}
        }.dump();
    }

    std::string SbywowAgentEngine::ExecuteUseItem(Player* bot, Intent const& intent)
    {
        if (!intent.itemGuid && !intent.itemEntry)
            return json{{"ok", false}, {"error", "use_item requires item_guid or item_entry"}}.dump();
        Item* item = nullptr;
        if (intent.itemGuid)
            item = bot->GetItemByGuid(ObjectGuid(intent.itemGuid));
        else
            item = bot->GetItemByEntry(intent.itemEntry);
        if (!item)
            return json{
                {"ok",         false},
                {"error",      "item not in bot's inventory"},
                {"item_guid",  intent.itemGuid},
                {"item_entry", intent.itemEntry}
            }.dump();
        ItemTemplate const* tpl = item->GetTemplate();
        if (!tpl)
            return json{{"ok", false}, {"error", "item has no template"}}.dump();

        // Pick the first non-null on-use spell from the item's spells.
        // Most usable items put their spell in slot 0 with trigger
        // ITEM_SPELLTRIGGER_ON_USE; some scrolls/equipment use slot 1.
        uint32 spellId = 0;
        for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
        {
            if (tpl->Spells[i].SpellId != 0 &&
                tpl->Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
            {
                spellId = tpl->Spells[i].SpellId;
                break;
            }
        }
        if (!spellId)
            return json{
                {"ok",         false},
                {"error",      "item has no on-use spell"},
                {"item_entry", tpl->ItemId},
                {"item_name",  tpl->Name1}
            }.dump();

        SpellCastTargets targets;
        if (intent.guid)
        {
            if (Unit* t = ObjectAccessor::GetUnit(*bot, ObjectGuid(intent.guid)))
                targets.SetUnitTarget(t);
        }
        else
        {
            targets.SetUnitTarget(bot);
        }

        // CastItemUseSpell returns void — internally constructs a Spell,
        // runs CheckCast(true), and either calls prepare() on success
        // (pushing the spell into m_currentSpells) or deletes the
        // Spell on failure. Detect outcome via post-call inspection
        // rather than a preflight CheckCast — a stack-allocated
        // preflight Spell triggers SPELL_FAILED_SPELL_IN_PROGRESS in
        // the subsequent real CheckCast (Spell.cpp:3465) and silently
        // suppresses the actual cast.
        bot->CastItemUseSpell(item, targets, /*cast_count*/ 0, /*glyphIndex*/ 0);

        // Three-state detection:
        //   - spell now in m_currentSpells → cast-time/channeled, in-flight
        //   - not in m_currentSpells, but cooldown set → instant succeeded
        //   - not in m_currentSpells, no cooldown → pre-check refused
        bool inflight = bot->FindCurrentSpellBySpellId(spellId) != nullptr;
        bool cdSet    = bot->HasSpellCooldown(spellId);
        if (!inflight && !cdSet)
        {
            // CastItemUseSpell is void — no SpellCastResult to surface.
            // Best-effort hint via post-call inspection: which state is
            // most likely to have caused the refusal? Order matches the
            // most-common cast-blocking conditions for item-use spells
            // (hearth, scrolls, bandages, summon-mount items).
            char const* hint = "pre_check_refused";
            SpellInfo const* siUse = sSpellMgr->GetSpellInfo(spellId);
            if (!bot->IsAlive())
                hint = "caster_dead";
            else if (bot->isMoving() && siUse && siUse->CalcCastTime() > 0)
                hint = "moving";
            else if (bot->IsInCombat() && siUse &&
                     siUse->HasAttribute(SPELL_ATTR0_NOT_IN_COMBAT_ONLY_PEACEFUL))
                hint = "in_combat";
            else if (siUse && siUse->IsCooldownStartedOnEvent())
                hint = "cooldown_pending_event";
            return json{
                {"ok",           false},
                {"verb",         "use_item"},
                {"item_entry",   tpl->ItemId},
                {"item_name",    tpl->Name1},
                {"spell_id",     spellId},
                {"failure_hint", hint},
                {"error",        "cast refused — see failure_hint for likely cause"}
            }.dump();
        }
        return json{
            {"ok",         true},
            {"verb",       "use_item"},
            {"item_guid",  intent.itemGuid},
            {"item_entry", tpl->ItemId},
            {"item_name",  tpl->Name1},
            {"spell_id",   spellId},
            {"_inflight",  inflight}
        }.dump();
    }

    std::string SbywowAgentEngine::ExecuteMount(Player* bot, Intent const& intent)
    {
        // Pick a mount spell. If the agent supplied one, use it; else
        // walk the bot's spellbook for a mount-aura spell. AC marks
        // mount spells via Effect[0].ApplyAuraName == SPELL_AURA_MOUNTED.
        uint32 spellId = intent.spellId;
        if (!spellId)
        {
            for (auto const& [sid, _] : bot->GetSpellMap())
            {
                SpellInfo const* si = sSpellMgr->GetSpellInfo(sid);
                if (!si) continue;
                if (si->Effects[0].ApplyAuraName == SPELL_AURA_MOUNTED &&
                    si->IsAbilityLearnedWithProfession() == false)
                {
                    spellId = sid;
                    break;
                }
            }
        }
        if (!spellId)
            return json{{"ok", false}, {"error", "no mount spell found in spellbook"}}.dump();

        SpellInfo const* si = sSpellMgr->GetSpellInfo(spellId);
        if (!si)
            return json{{"ok", false}, {"error", "unknown spell_id"}, {"spell_id", spellId}}.dump();
        SpellCastResult res = bot->CastSpell(bot, si, TRIGGERED_NONE);
        if (res != SPELL_CAST_OK)
        {
            return json{
                {"ok",               false},
                {"verb",             "mount"},
                {"spell_id",         spellId},
                {"spell_name",       si->SpellName[0] ? si->SpellName[0] : ""},
                {"cast_result",      static_cast<int>(res)},
                {"cast_result_name", EnumUtils::ToConstant(res)},
                {"error",            "cast pre-check failed"}
            }.dump();
        }
        bool inflight = bot->FindCurrentSpellBySpellId(spellId) != nullptr;
        return json{
            {"ok",         true},
            {"verb",       "mount"},
            {"spell_id",   spellId},
            {"spell_name", si->SpellName[0] ? si->SpellName[0] : ""},
            {"_inflight",  inflight}
        }.dump();
    }

    std::string SbywowAgentEngine::ExecuteDismount(Player* bot, Intent const& /*intent*/)
    {
        if (!bot->IsMounted())
            return json{{"ok", false}, {"error", "bot is not mounted"}}.dump();
        bot->Dismount();
        bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
        return json{{"ok", true}, {"verb", "dismount"}}.dump();
    }
    std::string SbywowAgentEngine::ExecuteInteractGameObject(Player* bot, Intent const& intent)
    {
        ObjectGuid og(intent.guid);
        if (!og.IsGameObject())
            return json{{"ok", false}, {"error", "interact_gameobject requires a gameobject guid"}}.dump();
        GameObject* go = bot->GetMap()->GetGameObject(og);
        if (!go)
            return json{{"ok", false}, {"error", "gameobject not found on bot's map"}}.dump();

        // INTERACTION_DISTANCE check mirrors the client; some GOs allow
        // longer (fishing nodes), but the canonical check is on the
        // handler — let it enforce.
        WorldPacket data(CMSG_GAMEOBJ_USE, 8);
        data << og;
        bot->GetSession()->HandleGameObjectUseOpcode(data);

        return json{
            {"ok",       true},
            {"verb",     "interact_gameobject"},
            {"guid",     intent.guid},
            {"entry",    go->GetEntry()},
            {"name",     go->GetName()},
            {"go_type",  static_cast<int>(go->GetGoType())},
            {"distance", bot->GetExactDist(go)}
        }.dump();
    }

    std::string SbywowAgentEngine::ExecuteLootTarget(Player* bot, Intent const& intent)
    {
        ObjectGuid lguid(intent.guid);
        // Open loot — drives Player::SendLoot which sets m_lootGuid +
        // sends SMSG_LOOT_RESPONSE to the client. That's also where the
        // server resolves which Loot* applies for this target.
        bot->SendLoot(lguid, LOOT_CORPSE);

        // Resolve the underlying Loot* by guid type. Mirrors the dispatch
        // in HandleAutostoreLootItemOpcode.
        Loot* loot = nullptr;
        if (lguid.IsAnyTypeCreature())
        {
            if (Creature* c = bot->GetMap()->GetCreature(lguid))
                loot = &c->loot;
        }
        else if (lguid.IsGameObject())
        {
            if (GameObject* go = bot->GetMap()->GetGameObject(lguid))
                loot = &go->loot;
        }
        else if (lguid.IsCorpse())
        {
            if (Corpse* c = ObjectAccessor::GetCorpse(*bot, lguid))
                loot = &c->loot;
        }

        if (!loot)
        {
            bot->SendLootRelease(lguid);
            return json{
                {"ok",    false},
                {"error", "no loot for this target (already looted, not lootable, or wrong guid type)"},
                {"guid",  intent.guid}
            }.dump();
        }

        // Auto-take every unsorted item slot. Items requiring a group
        // roll (group_loot / need_before_greed / master_loot) will fail
        // StoreLootItem and stay in the loot — that's correct behavior;
        // the agent can roll separately via group_ready_check_respond
        // (Phase 5) once that wiring lands.
        uint32 itemsTaken = 0;
        json takenList = json::array();
        for (uint8 i = 0; i < loot->items.size(); ++i)
        {
            LootItem const& li = loot->items[i];
            if (li.is_looted)
                continue;
            InventoryResult res = EQUIP_ERR_OK;
            LootItem* taken = bot->StoreLootItem(i, loot, res);
            if (res == EQUIP_ERR_OK && taken)
            {
                ++itemsTaken;
                if (ItemTemplate const* tpl = sObjectMgr->GetItemTemplate(taken->itemid))
                {
                    takenList.push_back({
                        {"entry", taken->itemid},
                        {"name",  tpl->Name1},
                        {"count", static_cast<int>(taken->count)}
                    });
                }
                else
                {
                    takenList.push_back({{"entry", taken->itemid}, {"count", static_cast<int>(taken->count)}});
                }
            }
        }

        // Money: take everything. Mirrors HandleLootMoneyOpcode's
        // single-looter (non-group) path.
        uint32 moneyTaken = loot->gold;
        if (moneyTaken > 0)
        {
            if (Group* group = bot->GetGroup())
            {
                // Group present — split via the same path the handler
                // would. Simplest: just give the bot its share or
                // skip — for now, single-looter gets all the gold.
                // (Group loot money distribution is in
                // HandleLootMoneyOpcode; we skip the split for v1.)
                bot->ModifyMoney(moneyTaken);
                (void)group;
            }
            else
            {
                bot->ModifyMoney(moneyTaken);
            }
            loot->gold = 0;
            loot->NotifyMoneyRemoved();
        }

        bot->SendLootRelease(lguid);

        return json{
            {"ok",            true},
            {"verb",          "loot_target"},
            {"guid",          intent.guid},
            {"items_taken",   itemsTaken},
            {"copper_taken",  moneyTaken},
            {"items",         takenList}
        }.dump();
    }
    namespace
    {
        // Find the nearest mailbox gameobject within INTERACTION_DISTANCE.
        // Mail verbs need this since CanOpenMailBox checks distance to a
        // specific mailbox. Returns ObjectGuid::Empty if none in range.
        ObjectGuid FindNearbyMailboxGuid(Player* bot)
        {
            std::list<GameObject*> gos;
            bot->GetGameObjectListWithEntryInGrid(gos, 0, INTERACTION_DISTANCE);
            for (GameObject* go : gos)
            {
                if (go && go->GetGoType() == GAMEOBJECT_TYPE_MAILBOX)
                    return go->GetGUID();
            }
            return ObjectGuid::Empty;
        }
    }

    std::string SbywowAgentEngine::ExecuteMailSend(Player* bot, Intent const& intent)
    {
        if (intent.strParam1.empty())
            return json{{"ok", false}, {"error", "mail_send requires recipient"}}.dump();

        ObjectGuid mailbox = FindNearbyMailboxGuid(bot);
        if (!mailbox)
            return json{{"ok", false}, {"error", "no mailbox in interact range"}}.dump();

        // Construct the SendMail packet body. The handler reads:
        //   mailbox guid, recipient string, subject, body, unk1, unk2,
        //   item_count, [items...], money, COD, unk3, unk4
        WorldPacket data(CMSG_SEND_MAIL, 64);
        data << mailbox;
        data << intent.strParam1;          // recipient
        data << intent.strParam2;          // subject
        data << intent.strParam3;          // body
        data << uint32(0x00000000);        // unk1 stationery
        data << uint32(0x00000000);        // unk2
        uint8 itemsCount = intent.itemGuid ? 1 : 0;
        data << uint8(itemsCount);
        if (itemsCount > 0)
        {
            data << uint8(0);                  // mail item slot, unused
            data << ObjectGuid(intent.itemGuid);
        }
        data << uint32(intent.copper);     // money
        data << uint32(0);                 // COD
        data << uint64(0);                 // unk3
        data << uint8(0);                  // unk4

        bot->GetSession()->HandleSendMail(data);

        return json{
            {"ok",          true},
            {"verb",        "mail_send"},
            {"recipient",   intent.strParam1},
            {"subject",     intent.strParam2},
            {"item_guid",   intent.itemGuid},
            {"copper",      intent.copper},
            {"mailbox",     mailbox.GetRawValue()}
        }.dump();
    }

    std::string SbywowAgentEngine::ExecuteMailTakeItem(Player* bot, Intent const& intent)
    {
        if (!intent.mailId)
            return json{{"ok", false}, {"error", "mail_take_item requires mail_id"}}.dump();
        if (!intent.itemGuid)
            return json{{"ok", false}, {"error", "mail_take_item requires item_guid"}}.dump();
        ObjectGuid mailbox = FindNearbyMailboxGuid(bot);
        if (!mailbox)
            return json{{"ok", false}, {"error", "no mailbox in interact range"}}.dump();

        WorldPacket data(CMSG_MAIL_TAKE_ITEM, 8 + 4 + 4);
        data << mailbox;
        data << uint32(intent.mailId);
        data << uint32(ObjectGuid(intent.itemGuid).GetCounter());
        bot->GetSession()->HandleMailTakeItem(data);

        return json{
            {"ok",        true},
            {"verb",      "mail_take_item"},
            {"mail_id",   intent.mailId},
            {"item_guid", intent.itemGuid}
        }.dump();
    }

    std::string SbywowAgentEngine::ExecuteMailTakeMoney(Player* bot, Intent const& intent)
    {
        if (!intent.mailId)
            return json{{"ok", false}, {"error", "mail_take_money requires mail_id"}}.dump();
        ObjectGuid mailbox = FindNearbyMailboxGuid(bot);
        if (!mailbox)
            return json{{"ok", false}, {"error", "no mailbox in interact range"}}.dump();

        uint32 moneyBefore = bot->GetMoney();
        WorldPacket data(CMSG_MAIL_TAKE_MONEY, 8 + 4);
        data << mailbox;
        data << uint32(intent.mailId);
        bot->GetSession()->HandleMailTakeMoney(data);

        return json{
            {"ok",            true},
            {"verb",          "mail_take_money"},
            {"mail_id",       intent.mailId},
            {"copper_before", moneyBefore},
            {"copper_after",  bot->GetMoney()},
            {"copper_gained", bot->GetMoney() - moneyBefore}
        }.dump();
    }
    std::string SbywowAgentEngine::ExecuteQuestAccept(Player* bot, Intent const& intent)
    {
        if (!intent.questId)
            return json{{"ok", false}, {"error", "quest_accept requires quest_id"}}.dump();
        Quest const* quest = sObjectMgr->GetQuestTemplate(intent.questId);
        if (!quest)
            return json{{"ok", false}, {"error", "unknown quest_id"}, {"quest_id", intent.questId}}.dump();
        if (!bot->CanTakeQuest(quest, true))
            return json{
                {"ok",       false},
                {"error",    "bot cannot take this quest (level/prereq/repeatable)"},
                {"quest_id", intent.questId}
            }.dump();

        // The questgiver is required for AddQuestAndCheckCompletion's
        // OnQuestAccept hooks, but is allowed to be the bot itself for
        // self-given quests. If the agent supplied an npc_guid, prefer
        // that; else use the bot.
        Object* giver = bot;
        if (intent.guid)
        {
            if (Object* o = ObjectAccessor::GetObjectByTypeMask(*bot, ObjectGuid(intent.guid),
                    TYPEMASK_UNIT | TYPEMASK_GAMEOBJECT | TYPEMASK_ITEM))
                giver = o;
        }
        bot->AddQuestAndCheckCompletion(quest, giver);

        return json{
            {"ok",       true},
            {"verb",     "quest_accept"},
            {"quest_id", intent.questId},
            {"title",    quest->GetTitle()}
        }.dump();
    }

    std::string SbywowAgentEngine::ExecuteQuestComplete(Player* bot, Intent const& intent)
    {
        if (!intent.questId)
            return json{{"ok", false}, {"error", "quest_complete requires quest_id"}}.dump();
        Quest const* quest = sObjectMgr->GetQuestTemplate(intent.questId);
        if (!quest)
            return json{{"ok", false}, {"error", "unknown quest_id"}, {"quest_id", intent.questId}}.dump();
        if (!bot->CanCompleteQuest(intent.questId))
            return json{
                {"ok",       false},
                {"error",    "quest not yet completable (objectives unmet)"},
                {"quest_id", intent.questId}
            }.dump();
        if (!bot->CanRewardQuest(quest, true))
            return json{
                {"ok",       false},
                {"error",    "bot cannot accept reward (bag full / item cap)"},
                {"quest_id", intent.questId}
            }.dump();

        Object* giver = bot;
        if (intent.guid)
        {
            if (Object* o = ObjectAccessor::GetObjectByTypeMask(*bot, ObjectGuid(intent.guid),
                    TYPEMASK_UNIT | TYPEMASK_GAMEOBJECT))
                giver = o;
        }
        // reward index 0 — picks first reward choice; agent harness can
        // explicitly pre-select via a richer reward arg in a future
        // polish pass.
        bot->RewardQuest(quest, /*reward=*/0, giver, /*announce=*/true);

        return json{
            {"ok",       true},
            {"verb",     "quest_complete"},
            {"quest_id", intent.questId},
            {"title",    quest->GetTitle()}
        }.dump();
    }

    std::string SbywowAgentEngine::ExecuteQuestAbandon(Player* bot, Intent const& intent)
    {
        if (!intent.questId)
            return json{{"ok", false}, {"error", "quest_abandon requires quest_id"}}.dump();
        uint16 slot = bot->FindQuestSlot(intent.questId);
        if (slot >= MAX_QUEST_LOG_SIZE)
            return json{
                {"ok",       false},
                {"error",    "quest not in log"},
                {"quest_id", intent.questId}
            }.dump();
        bot->TakeQuestSourceItem(intent.questId, true);
        bot->AbandonQuest(intent.questId);
        bot->RemoveActiveQuest(intent.questId);
        bot->SetQuestSlot(slot, 0);
        return json{
            {"ok",       true},
            {"verb",     "quest_abandon"},
            {"quest_id", intent.questId},
            {"slot",     slot}
        }.dump();
    }

    std::string SbywowAgentEngine::ExecuteQuestShare(Player* bot, Intent const& intent)
    {
        if (!intent.questId)
            return json{{"ok", false}, {"error", "quest_share requires quest_id"}}.dump();
        if (!bot->GetGroup())
            return json{{"ok", false}, {"error", "bot is not in a group"}}.dump();
        // HandlePushQuestToParty reads just questId; construct + dispatch.
        WorldPacket data(CMSG_PUSHQUESTTOPARTY, 4);
        data << uint32(intent.questId);
        bot->GetSession()->HandlePushQuestToParty(data);
        return json{
            {"ok",       true},
            {"verb",     "quest_share"},
            {"quest_id", intent.questId}
        }.dump();
    }
    std::string SbywowAgentEngine::ExecuteGroupAcceptInvite(Player* bot, Intent const& /*intent*/)
    {
        if (!bot->GetGroupInvite())
            return json{{"ok", false}, {"error", "no pending group invite"}}.dump();
        WorldPacket data(CMSG_GROUP_ACCEPT, 4);
        data << uint32(0);
        bot->GetSession()->HandleGroupAcceptOpcode(data);
        return json{{"ok", true}, {"verb", "group_accept_invite"}}.dump();
    }

    std::string SbywowAgentEngine::ExecuteGroupDeclineInvite(Player* bot, Intent const& /*intent*/)
    {
        if (!bot->GetGroupInvite())
            return json{{"ok", false}, {"error", "no pending group invite"}}.dump();
        WorldPacket data(CMSG_GROUP_DECLINE, 0);
        bot->GetSession()->HandleGroupDeclineOpcode(data);
        return json{{"ok", true}, {"verb", "group_decline_invite"}}.dump();
    }

    std::string SbywowAgentEngine::ExecuteGroupLeave(Player* bot, Intent const& /*intent*/)
    {
        if (!bot->GetGroup())
            return json{{"ok", false}, {"error", "bot is not in a group"}}.dump();
        WorldPacket data(CMSG_GROUP_DISBAND, 0);
        bot->GetSession()->HandleGroupDisbandOpcode(data);
        return json{{"ok", true}, {"verb", "group_leave"}}.dump();
    }

    std::string SbywowAgentEngine::ExecuteGroupPromoteLeader(Player* bot, Intent const& intent)
    {
        if (!bot->GetGroup())
            return json{{"ok", false}, {"error", "bot is not in a group"}}.dump();
        if (!intent.guid)
            return json{{"ok", false}, {"error", "group_promote_leader requires target_guid"}}.dump();
        WorldPacket data(CMSG_GROUP_SET_LEADER, 8);
        data << ObjectGuid(intent.guid);
        bot->GetSession()->HandleGroupSetLeaderOpcode(data);
        return json{
            {"ok",          true},
            {"verb",        "group_promote_leader"},
            {"target_guid", intent.guid}
        }.dump();
    }

    std::string SbywowAgentEngine::ExecuteGroupReadyCheckRespond(Player* bot, Intent const& intent)
    {
        if (!bot->GetGroup())
            return json{{"ok", false}, {"error", "bot is not in a group"}}.dump();
        // intParam: 1 = ready, 0 = not ready (default to ready when
        // unset so the verb is a one-shot "yes" by default).
        uint8 state = (intent.intParam == 0) ? 0 : 1;
        WorldPacket data(MSG_RAID_READY_CHECK, 1);
        data << uint8(state);
        bot->GetSession()->HandleRaidReadyCheckOpcode(data);
        return json{
            {"ok",    true},
            {"verb",  "group_ready_check_respond"},
            {"ready", state == 1}
        }.dump();
    }

    bool SbywowAgentEngine::CancelWaitIfMatch(uint64_t intentId)
    {
        // Only match if we're actively suspended on this exact wait
        // — a stale intent_id (e.g., a wait that already expired)
        // shouldn't quietly clear the engine's idle bookkeeping.
        if (!isWaiting_ || waitingIntentId_ != intentId)
            return false;

        isWaiting_ = false;
        waitingIntentId_ = 0;
        waitingIntentVerb_.clear();
        return true;
    }

    bool SbywowAgentEngine::CancelInFlightCastIfMatch(uint64_t intentId)
    {
        // Find the live Spell* for our in-flight cast and call
        // Spell::cancel(). Cancellation interrupts the cast and clears
        // the m_currentSpells slot; the next PollInFlightCast tick
        // will see the slot null + no cooldown → emit intent_failed.
        // We DON'T emit intent_cancelled here — the cancel dispatcher
        // (in BridgeServer) is the one source of truth for the
        // intent_cancelled event, regardless of which path matched.
        // We just clear our slot + return true so the dispatcher
        // knows to emit cancelled instead of letting PollInFlightCast
        // emit failed.
        if (!inFlightCast_ || inFlightCastIntentId_ != intentId)
            return false;

        if (botAI)
        {
            if (Player* bot = botAI->GetBot())
            {
                if (Spell* live = bot->FindCurrentSpellBySpellId(inFlightCastSpellId_))
                    live->cancel();
            }
        }
        inFlightCast_ = false;
        inFlightCastIntentId_ = 0;
        inFlightCastVerb_.clear();
        inFlightCastSpellId_ = 0;
        return true;
    }

    bool SbywowAgentEngine::CancelInFlightMoveIfMatch(uint64_t intentId)
    {
        // Same shape as CancelWaitIfMatch but for an in-flight Move.
        // Clears the active MovePoint generator (so the bot stops
        // moving) and lets the cancel dispatcher emit the
        // intent_cancelled event.
        if (!inFlightMove_ || inFlightMoveIntentId_ != intentId)
            return false;

        if (botAI)
        {
            if (Player* bot = botAI->GetBot())
            {
                if (MotionMaster* mm = bot->GetMotionMaster())
                    mm->Clear();
            }
        }
        inFlightMove_ = false;
        inFlightMoveIntentId_ = 0;
        inFlightMoveVerb_.clear();
        return true;
    }

    void SbywowAgentEngine::TickReactiveAutonomic(Player* bot)
    {
        // Throttle: cadence threshold ticks of idle before we even
        // attempt to fire reactives. Cheap counter increment per
        // idle tick; the work happens once per ~kReactiveCadenceTicks
        // ticks (~2.5s at typical bot tick rate).
        if (++idleTickCount_ < kReactiveCadenceTicks)
            return;
        idleTickCount_ = 0;

        if (!bot->IsAlive())
            return;

        PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
        if (!ai)
            return;

        // Fire upstream EatAction / DrinkAction. They self-gate via
        // isUseful() (high-enough HP/mana, has food/water in bags,
        // not currently eating/drinking) so out-of-context calls
        // are cheap no-ops. Same actions the upstream "food"
        // strategy fires from "low health" / "low mana" triggers;
        // we skip the trigger layer because we don't have the
        // strategy stack — direct DoSpecificAction is enough.
        ++reactivesFiredTotal_;
        Event ev;
        ai->DoSpecificAction("food",  ev, /*silent=*/true);
        ai->DoSpecificAction("drink", ev, /*silent=*/true);
    }

    int64_t SbywowAgentEngine::WaitingRemainingMs() const
    {
        if (!isWaiting_)
            return 0;
        auto now = std::chrono::steady_clock::now();
        if (now >= waitUntil_)
            return 0;
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   waitUntil_ - now).count();
    }

    size_t SbywowAgentEngine::DefaultEngineStrategiesCount() const
    {
        return defaultEngine_ ? defaultEngine_->GetStrategies().size() : 0;
    }
}
