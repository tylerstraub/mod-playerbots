/*
 * sbywow Agent Bridge — script registration.
 *
 * Two scripts:
 *   - SbywowBridgeWorldScript  drives Start/Stop on world startup/shutdown.
 *   - SbywowBridgePlayerScript attaches bot sessions, ticks them every
 *                              update, and fans PlayerScript hooks into
 *                              outbound events for the SSE channel.
 *
 * v1 vertical slice sources events purely from AC's ScriptMgr surface.
 * The action-listener path (engines->AddActionExecutionListener) is
 * deferred until we want behavior telemetry beyond what PlayerScript
 * already gives us; engines[] is protected and adding it would cost a
 * Bucket 2 hook. Stay in pure Bucket 1 for the slice.
 */

#include "BridgeServer.h"
#include "BotSession.h"
#include "deps/json.hpp"

#include "Player.h"
#include "Playerbots.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "ScriptMgr.h"

using json = nlohmann::json;

namespace
{
    using namespace Sbywow::Bridge;

    // Snapshot cadence: emit a periodic state event every Nth tick of
    // OnPlayerUpdate so the agent always has fresh ground-truth context.
    // 50 update calls ≈ ~2.5s at default cadence; tune later.
    constexpr uint32 kSnapshotEveryNUpdates = 50;

    bool IsAttachedBot(Player* player)
    {
        return player && sPlayerbotsMgr.GetPlayerbotAI(player) != nullptr;
    }

    void EmitEvent(Player* bot, json const& payload)
    {
        auto session = BridgeServer::Instance().GetSession(bot->GetGUID());
        if (!session)
            return;
        session->PushOutbound(payload.dump());
    }

    json BaseEvent(Player* bot, std::string const& channel, std::string const& kind)
    {
        return {
            {"channel",  channel},
            {"kind",     kind},
            {"bot_guid", bot->GetGUID().GetRawValue()},
            {"bot_name", bot->GetName()}
        };
    }

    class SbywowBridgeWorldScript : public WorldScript
    {
    public:
        SbywowBridgeWorldScript() : WorldScript("SbywowBridgeWorldScript", {
            WORLDHOOK_ON_STARTUP,
            WORLDHOOK_ON_SHUTDOWN
        }) {}

        void OnStartup() override
        {
            BridgeServer::Instance().Start();
        }

        void OnShutdown() override
        {
            BridgeServer::Instance().Stop();
        }
    };

    class SbywowBridgePlayerScript : public PlayerScript
    {
    public:
        SbywowBridgePlayerScript() : PlayerScript("SbywowBridgePlayerScript", {
            PLAYERHOOK_ON_LOGIN,
            PLAYERHOOK_ON_LOGOUT,
            PLAYERHOOK_ON_UPDATE,
            PLAYERHOOK_ON_PLAYER_JUST_DIED,
            PLAYERHOOK_ON_LEVEL_CHANGED,
            PLAYERHOOK_ON_MAP_CHANGED
        }) {}

        void OnPlayerLogin(Player* /*player*/) override
        {
            // No-op intentionally. Playerbots wires PlayerbotAI via a
            // queued OnBotLogin operation that runs *after*
            // ScriptMgr::OnPlayerLogin fires, so the IsAttachedBot filter
            // would reject every bot login here. Attach is deferred to
            // the first OnPlayerUpdate where the AI is reliably present.
        }

        void OnPlayerLogout(Player* player) override
        {
            // Filter on bridge session, not IsAttachedBot — at logout the
            // PlayerbotAI may already have been torn down.
            if (!BridgeServer::Instance().GetSession(player->GetGUID()))
                return;
            json ev = BaseEvent(player, "lifecycle", "detached");
            EmitEvent(player, ev);

            BridgeServer::Instance().DetachBot(player->GetGUID());
        }

        void OnPlayerUpdate(Player* player, uint32 /*p_time*/) override
        {
            if (!IsAttachedBot(player))
                return;

            // Idempotent attach. Returns true only on first sight; emit
            // the lifecycle "attached" event with bootstrap context.
            if (BridgeServer::Instance().AttachBot(player))
            {
                json ev = BaseEvent(player, "lifecycle", "attached");
                ev["map"]  = player->GetMapId();
                ev["zone"] = player->GetZoneId();
                ev["pos"]  = { player->GetPositionX(), player->GetPositionY(), player->GetPositionZ() };
                EmitEvent(player, ev);
            }

            BridgeServer::Instance().TickBot(player);

            // Periodic state snapshot.
            uint32& tick = updateTickByGuid_[player->GetGUID().GetRawValue()];
            if (++tick % kSnapshotEveryNUpdates != 0)
                return;

            json ev = BaseEvent(player, "snapshot", "state");
            ev["hp_pct"]    = static_cast<int>(player->GetHealthPct());
            ev["map"]       = player->GetMapId();
            ev["zone"]      = player->GetZoneId();
            ev["pos"]       = { player->GetPositionX(), player->GetPositionY(), player->GetPositionZ() };
            ev["in_combat"] = player->IsInCombat();
            ev["alive"]     = player->IsAlive();
            ev["level"]     = player->GetLevel();
            EmitEvent(player, ev);
        }

        void OnPlayerJustDied(Player* player) override
        {
            if (!IsAttachedBot(player))
                return;
            json ev = BaseEvent(player, "combat", "died");
            EmitEvent(player, ev);
        }

        void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override
        {
            if (!IsAttachedBot(player))
                return;
            json ev = BaseEvent(player, "progression", "level_changed");
            ev["old_level"] = static_cast<int>(oldLevel);
            ev["new_level"] = static_cast<int>(player->GetLevel());
            EmitEvent(player, ev);
        }

        void OnPlayerMapChanged(Player* player) override
        {
            if (!IsAttachedBot(player))
                return;
            json ev = BaseEvent(player, "world", "map_changed");
            ev["map"]  = player->GetMapId();
            ev["zone"] = player->GetZoneId();
            EmitEvent(player, ev);
        }

    private:
        // Tracks per-bot update count for the snapshot cadence. Cleared
        // implicitly when we never look up a detached guid again; if the
        // map ever balloons we can clear in OnPlayerLogout, but at our
        // scale it's noise.
        std::unordered_map<uint64, uint32> updateTickByGuid_;
    };
}

void AddSC_SbywowBridgeScripts()
{
    new SbywowBridgeWorldScript();
    new SbywowBridgePlayerScript();
}
