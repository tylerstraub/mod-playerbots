#include "SbywowAgentEngine.h"

#include "../Bridge/BridgeServer.h"
#include "../Bridge/BotSession.h"
#include "../Bridge/deps/json.hpp"

#include "AiFactory.h"
#include "Bag.h"
#include "Creature.h"
#include "CreatureData.h"
#include "Event.h"
#include "GossipDef.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "ItemPackets.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
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
        // Allowlist: only the packet-handler "default" strategy is
        // ever installed in this engine. Class-specific combat-prep
        // strategies (dps assist / cure / tank assist), follow,
        // quest, gather, chat, emote, loot, duel, mount, food, buff,
        // pvp — all rejected. The agent harness drives these via
        // bridge intents (move_to / interact_with / do_action / say).
        //
        // This filter survives PlayerbotAI::ResetStrategies invocations
        // — login, SetMaster, group invite accept, LFG events, talent
        // change, etc. — because all those paths re-enter this method
        // through the virtual dispatch from base ResetStrategies code.
        // The reassertion problem that drove the architectural pivot
        // is closed at the strategy-add level.
        if (name == "default")
        {
            Engine::addStrategy(name, init);
            return;
        }
        // Quietly drop. LOG_DEBUG so we can audit if needed; not a
        // warning because upstream ResetStrategies legitimately tries
        // to add ~15 strategies every time it fires and that's by
        // design — no consumer needs to know it was filtered.
        LOG_DEBUG("playerbots",
                  "[SbywowAgentEngine] filtered strategy add: '{}'", name.c_str());
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

        // Multi-tick Move: if a move was dispatched on a prior tick,
        // poll arrival / path failure / timeout. While the move is
        // active we return false outright — no queue drain, no idle
        // delegation. This is the mechanism that prevents the
        // default engine's FollowAction from running and replacing
        // the bot's MotionMaster (which used to clobber agent moves
        // and was the original motivation for the engine pivot).
        if (inFlightMove_)
        {
            float dist2d = bot->GetExactDist2d(inFlightMoveX_, inFlightMoveY_);
            bool  arrived = dist2d <= kMoveArrivalThreshold;
            auto  elapsed = std::chrono::steady_clock::now() - inFlightMoveDispatchAt_;
            bool  timedOut = elapsed > std::chrono::milliseconds(kMoveTimeoutMs);

            // Check if MotionMaster's active generator is still our
            // MovePoint. If something replaced it (FollowAction in a
            // delegated path, charm, knockback, etc.) we treat the
            // move as failed — the agent didn't get where it asked.
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
                // Fall through to wait/queue handling below.
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
                // Fall through.
            }
            else
            {
                // Still moving — preempts queue + idle delegation.
                return false;
            }
        }

        // Wait suspension: while engaged, no intents drain. Wait is
        // explicit agent-issued sequencing — "do A, hold for N ms,
        // then do B" — implemented as a pause on intent dispatch.
        // Chained intents queue behind it until suspension lifts.
        // Uses steady_clock to avoid getMSTime's uint32 wraparound.
        if (isWaiting_)
        {
            if (std::chrono::steady_clock::now() < waitUntil_)
                return false;  // still waiting
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

        // Pop one intent per tick from the per-bot session queue,
        // execute it, emit the corresponding SSE intent_* event.
        // BridgeServer is the canonical owner of sessions; if the
        // bridge is down or the session was detached out from under
        // us, we silently no-op (intent is implicitly cancelled).
        if (!session)
            return false;

        std::shared_ptr<Sbywow::Bridge::PendingIntent> pending;
        if (!session->PopIntent(pending))
        {
            // Idle tick. Two paths:
            //
            // 1. follow_mode ON (default): delegate to the default
            //    engine. Bot follows master at idle, runs the upstream
            //    react / autonomic / eat-drink strategies. Agent
            //    intents preempt this naturally — having an intent in
            //    the queue (or an in-flight Move, or an active Wait)
            //    means we never reach this branch.
            //
            // 2. follow_mode OFF: anchor in place. Agent has
            //    explicitly opted out of follow ("camp here
            //    indefinitely"). We still run our own reactive
            //    eat/drink so a long anchor doesn't leave the bot
            //    starving — autonomic is reflex, not "follow."
            //
            // See decisions.md "Idle delegation + follow toggle"
            // (2026-05-02) for the design.
            if (session->IsFollowMode() && defaultEngine_)
            {
                ++defaultEngineTicksTotal_;
                return defaultEngine_->DoNextAction(target, depth, minimal);
            }
            TickReactiveAutonomic(bot);
            return false;
        }

        // intent_started fires the moment we commit to executing a
        // popped intent. For sub-tick verbs (Move/Interact/Say/
        // DoAction) intent_completed lands in the same tick; for
        // Wait it lands when the suspension expires. Emit before the
        // dispatch so a slow ExecuteIntent (find_nearby-class work)
        // doesn't reorder against completed.
        {
            json ev = BuildIntentEvent(bot, "intent_started",
                                       pending->intentId, pending->verb);
            session->PushOutbound(ev.dump());
        }

        // Count the dispatch — covers all five kinds (Wait included,
        // since arming the suspension is itself a dispatch).
        ++intentsDispatchedTotal_;

        // Wait is special-cased: arm the suspension. Subsequent
        // intents in the queue wait their turn until suspension
        // lifts in a future tick (where intent_completed fires).
        if (pending->intent.kind == IntentKind::Wait)
        {
            isWaiting_         = true;
            waitUntil_         = std::chrono::steady_clock::now() +
                                 std::chrono::milliseconds(pending->intent.waitMs);
            waitingIntentId_   = pending->intentId;
            waitingIntentVerb_ = pending->verb;
            return true;
        }

        // Move is also special-cased: dispatch the MovePoint into
        // the MotionMaster (sub-tick), then HOLD the intent in our
        // in-flight slot. The terminal event (intent_completed on
        // arrival, intent_failed on path-replaced/timeout) fires from
        // the multi-tick poll at the top of DoNextAction. While the
        // move is in-flight, follow / queue-drain / idle delegation
        // are all blocked — that's how the agent's move trajectory
        // gets to complete without FollowAction yanking it back.
        //
        // Pre-dispatch validation (map mismatch, can't move,
        // missing motion master) still fires intent_failed on the
        // same tick like other sub-tick verbs, since no MovePoint
        // was armed.
        if (pending->intent.kind == IntentKind::Move)
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
                session->PushOutbound(ev.dump());

                Sbywow::Bridge::BotSession::TerminalIntent rec;
                rec.intentId   = pending->intentId;
                rec.verb       = pending->verb;
                rec.kind       = "intent_failed";
                rec.resultJson = result.dump();
                session->RecordTerminal(std::move(rec));
                return true;
            }
            // MovePoint armed — hold the intent. Terminal event lands
            // on a future tick from the in-flight poll.
            inFlightMove_           = true;
            inFlightMoveIntentId_   = pending->intentId;
            inFlightMoveVerb_       = pending->verb;
            inFlightMoveX_          = pending->intent.x;
            inFlightMoveY_          = pending->intent.y;
            inFlightMoveZ_          = pending->intent.z;
            inFlightMoveDispatchAt_ = std::chrono::steady_clock::now();
            return true;
        }

        std::string outStr = ExecuteIntent(bot, pending->intent);

        // Translate the verb's `ok` flag into intent_completed vs
        // intent_failed. Embed the full result payload so SSE
        // consumers get the same JSON we used to return synchronously
        // on the HTTP response. Parse defensively — engine outputs
        // valid JSON, but a malformed string shouldn't take down
        // the bridge.
        json result;
        try { result = json::parse(outStr); }
        catch (std::exception const&) { result = {{"ok", false}, {"error", "engine returned non-JSON"}}; }
        bool ok = result.value("ok", false);

        std::string kind = ok ? "intent_completed" : "intent_failed";
        json ev = BuildIntentEvent(bot, kind, pending->intentId, pending->verb);
        ev["result"] = result;
        session->PushOutbound(ev.dump());

        Sbywow::Bridge::BotSession::TerminalIntent rec;
        rec.intentId   = pending->intentId;
        rec.verb       = pending->verb;
        rec.kind       = std::move(kind);
        rec.resultJson = result.dump();
        session->RecordTerminal(std::move(rec));

        return true;
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
        float dist  = bot->GetExactDist(intent.x, intent.y, intent.z);

        // Match mod-playerbots' MovementAction::DoMovePoint shape:
        // stand up if sitting, then Clear() and MovePoint with
        // generatePath=true so mmaps are honored. forceDestination=false
        // lets the spline engine refuse if the target is unreachable
        // rather than teleport into geometry.
        if (bot->IsSitState())
            bot->SetStandState(UNIT_STAND_STATE_STAND);
        mm->Clear();
        mm->MovePoint(/*id*/ 0, intent.x, intent.y, intent.z, FORCED_MOVEMENT_NONE,
                      /*speed*/ 0.f, /*orientation*/ 0.f,
                      /*generatePath*/ true,
                      /*forceDestination*/ false);

        return json{
            {"ok",       true},
            {"verb",     "move_to"},
            {"map",      bot->GetMapId()},
            {"from",     {fromX, fromY, fromZ}},
            {"to",       {intent.x, intent.y, intent.z}},
            {"distance", dist}
        }.dump();
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

        uint8 count = static_cast<uint8>(std::min<uint32>(intent.quantity ? intent.quantity : 1, 255u));
        // bag/slot = NULL_BAG/NULL_SLOT means "auto-find a slot."
        bool ok = bot->BuyItemFromVendorSlot(vGuid, slot, intent.itemEntry, count, NULL_BAG, NULL_SLOT);

        return json{
            {"ok",          ok},
            {"verb",        "buy_item"},
            {"vendor_guid", intent.vendorGuid},
            {"vendor_name", npc->GetName()},
            {"vendor_slot", slot},
            {"item_entry",  intent.itemEntry},
            {"count",       count}
        }.dump();
    }

    std::string SbywowAgentEngine::ExecuteSellItem(Player* bot, Intent const& intent)
    {
        if (!intent.vendorGuid || !intent.itemGuid)
            return json{{"ok", false}, {"error", "sell_item requires vendor_guid and item_guid"}}.dump();

        // Construct + populate a SellItem packet, hand to the session.
        // The handler does the validation (vendor proximity, item
        // ownership, refundable check, etc.) and the gold transfer.
        WorldPacket raw(CMSG_SELL_ITEM, 8 + 8 + 4);
        WorldPackets::Item::SellItem packet(std::move(raw));
        packet.VendorGuid = ObjectGuid(intent.vendorGuid);
        packet.ItemGuid   = ObjectGuid(intent.itemGuid);
        packet.Count      = intent.quantity;   // 0 = sell whole stack

        // Capture pre-sell info for the response payload.
        Item* item = bot->GetItemByGuid(ObjectGuid(intent.itemGuid));
        std::string itemName;
        uint32      itemEntry = 0;
        if (item)
        {
            if (ItemTemplate const* tpl = item->GetTemplate())
            {
                itemName  = tpl->Name1;
                itemEntry = tpl->ItemId;
            }
        }
        uint32 moneyBefore = bot->GetMoney();

        bot->GetSession()->HandleSellItemOpcode(packet);

        uint32 moneyAfter = bot->GetMoney();
        Item* itemAfter = bot->GetItemByGuid(ObjectGuid(intent.itemGuid));
        bool soldFully = (itemAfter == nullptr);

        return json{
            {"ok",            moneyAfter > moneyBefore || soldFully},
            {"verb",          "sell_item"},
            {"vendor_guid",   intent.vendorGuid},
            {"item_guid",     intent.itemGuid},
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

    std::string SbywowAgentEngine::ExecuteSelectGossipOption (Player*, Intent const&) { return NotImplemented("select_gossip_option"); }
    std::string SbywowAgentEngine::ExecuteTradeInitiate      (Player*, Intent const&) { return NotImplemented("trade_initiate"); }
    std::string SbywowAgentEngine::ExecuteTradeOfferItem     (Player*, Intent const&) { return NotImplemented("trade_offer_item"); }
    std::string SbywowAgentEngine::ExecuteTradeOfferMoney    (Player*, Intent const&) { return NotImplemented("trade_offer_money"); }
    std::string SbywowAgentEngine::ExecuteTradeAccept        (Player*, Intent const&) { return NotImplemented("trade_accept"); }
    std::string SbywowAgentEngine::ExecuteTradeCancel        (Player*, Intent const&) { return NotImplemented("trade_cancel"); }
    std::string SbywowAgentEngine::ExecuteEquipItem          (Player*, Intent const&) { return NotImplemented("equip_item"); }
    std::string SbywowAgentEngine::ExecuteUnequipItem        (Player*, Intent const&) { return NotImplemented("unequip_item"); }
    std::string SbywowAgentEngine::ExecuteDestroyItem        (Player*, Intent const&) { return NotImplemented("destroy_item"); }
    std::string SbywowAgentEngine::ExecuteUseItem            (Player*, Intent const&) { return NotImplemented("use_item"); }
    std::string SbywowAgentEngine::ExecuteCastSpell          (Player*, Intent const&) { return NotImplemented("cast_spell"); }
    std::string SbywowAgentEngine::ExecuteMount              (Player*, Intent const&) { return NotImplemented("mount"); }
    std::string SbywowAgentEngine::ExecuteDismount           (Player*, Intent const&) { return NotImplemented("dismount"); }
    std::string SbywowAgentEngine::ExecuteInteractGameObject (Player*, Intent const&) { return NotImplemented("interact_gameobject"); }
    std::string SbywowAgentEngine::ExecuteLootTarget         (Player*, Intent const&) { return NotImplemented("loot_target"); }
    std::string SbywowAgentEngine::ExecuteMailSend           (Player*, Intent const&) { return NotImplemented("mail_send"); }
    std::string SbywowAgentEngine::ExecuteMailTakeItem       (Player*, Intent const&) { return NotImplemented("mail_take_item"); }
    std::string SbywowAgentEngine::ExecuteMailTakeMoney      (Player*, Intent const&) { return NotImplemented("mail_take_money"); }
    std::string SbywowAgentEngine::ExecuteQuestAccept        (Player*, Intent const&) { return NotImplemented("quest_accept"); }
    std::string SbywowAgentEngine::ExecuteQuestComplete      (Player*, Intent const&) { return NotImplemented("quest_complete"); }
    std::string SbywowAgentEngine::ExecuteQuestAbandon       (Player*, Intent const&) { return NotImplemented("quest_abandon"); }
    std::string SbywowAgentEngine::ExecuteQuestShare         (Player*, Intent const&) { return NotImplemented("quest_share"); }
    std::string SbywowAgentEngine::ExecuteGroupAcceptInvite  (Player*, Intent const&) { return NotImplemented("group_accept_invite"); }
    std::string SbywowAgentEngine::ExecuteGroupDeclineInvite (Player*, Intent const&) { return NotImplemented("group_decline_invite"); }
    std::string SbywowAgentEngine::ExecuteGroupLeave         (Player*, Intent const&) { return NotImplemented("group_leave"); }
    std::string SbywowAgentEngine::ExecuteGroupPromoteLeader (Player*, Intent const&) { return NotImplemented("group_promote_leader"); }
    std::string SbywowAgentEngine::ExecuteGroupReadyCheckRespond(Player*, Intent const&) { return NotImplemented("group_ready_check_respond"); }

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
