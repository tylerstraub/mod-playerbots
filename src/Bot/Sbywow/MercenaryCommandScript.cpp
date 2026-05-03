#include "BridgeServer.h"
#include "BotSession.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "Field.h"
#include "Map.h"
#include "MercenaryFactory.h"
#include "MercenaryMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotFactory.h"
#include "PlayerbotMgr.h"
#include "Playerbots.h"  // GET_PLAYERBOT_MGR
#include "QueryResult.h"
#include "SbywowConstants.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"

#include <algorithm>
#include <string>

using namespace Acore::ChatCommands;

namespace
{
    // Returns 0 for unknown class strings.
    uint8 ParseClassArg(std::string arg)
    {
        std::transform(arg.begin(), arg.end(), arg.begin(),
            [](unsigned char c) { return std::tolower(c); });

        if (arg == "warrior")     return CLASS_WARRIOR;
        if (arg == "paladin")     return CLASS_PALADIN;
        if (arg == "hunter")      return CLASS_HUNTER;
        if (arg == "rogue")       return CLASS_ROGUE;
        if (arg == "priest")      return CLASS_PRIEST;
        if (arg == "dk" || arg == "deathknight") return CLASS_DEATH_KNIGHT;
        if (arg == "shaman")      return CLASS_SHAMAN;
        if (arg == "mage")        return CLASS_MAGE;
        if (arg == "warlock")     return CLASS_WARLOCK;
        if (arg == "druid")       return CLASS_DRUID;
        return 0;
    }

    char const* ClassName(uint8 cls)
    {
        switch (cls)
        {
            case CLASS_WARRIOR:      return "Warrior";
            case CLASS_PALADIN:      return "Paladin";
            case CLASS_HUNTER:       return "Hunter";
            case CLASS_ROGUE:        return "Rogue";
            case CLASS_PRIEST:       return "Priest";
            case CLASS_DEATH_KNIGHT: return "Death Knight";
            case CLASS_SHAMAN:       return "Shaman";
            case CLASS_MAGE:         return "Mage";
            case CLASS_WARLOCK:      return "Warlock";
            case CLASS_DRUID:        return "Druid";
            default:                 return "Unknown";
        }
    }

    // Parse optional on/off arg for the .merc agent command.
    // Returns: 1 = "on"/"true"/"1", 0 = "off"/"false"/"0",
    // -1 = invalid, 2 = empty (query mode).
    int ParseOnOff(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
            [](unsigned char c) { return std::tolower(c); });
        if (s.empty())                                return 2;
        if (s == "on"  || s == "true"  || s == "1")   return 1;
        if (s == "off" || s == "false" || s == "0")   return 0;
        return -1;
    }
}

class sbywow_merc_commandscript : public CommandScript
{
public:
    sbywow_merc_commandscript() : CommandScript("sbywow_merc_commandscript") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable mercAdminTable = {
            {"setup",   HandleAdminSetupCommand,   SEC_GAMEMASTER,    Console::Yes},
            {"service", HandleAdminServiceCommand, SEC_GAMEMASTER,    Console::Yes},
            {"reap",    HandleAdminReapCommand,    SEC_GAMEMASTER,    Console::Yes},
            {"hire",    HandleAdminHireCommand,    SEC_ADMINISTRATOR, Console::Yes},
            {"nuke",    HandleAdminNukeCommand,    SEC_ADMINISTRATOR, Console::Yes},
            {"agent",   HandleAdminAgentModeCommand, SEC_GAMEMASTER,  Console::Yes},
        };

        static ChatCommandTable mercTable = {
            {"hire",       HandleHireCommand,       SEC_PLAYER, Console::No},
            {"list",       HandleListCommand,       SEC_PLAYER, Console::No},
            {"info",       HandleInfoCommand,       SEC_PLAYER, Console::No},
            {"summon",     HandleSummonCommand,     SEC_PLAYER, Console::No},
            // .merc dismiss / unsummon both run the gentle path —
            // graceful despawn, character + gear preserved, recallable
            // via .merc summon. Two names because "dismiss" matches
            // English expectation ("send away") and "unsummon" is
            // explicit muscle memory. Permanent termination is its own
            // verb: .merc release.
            {"dismiss",    HandleDismissCommand,    SEC_PLAYER, Console::No},
            {"unsummon",   HandleDismissCommand,    SEC_PLAYER, Console::No},
            {"dismissall", HandleDismissAllCommand, SEC_PLAYER, Console::No},
            // .merc release <name> — permanent termination. Cascades
            // to character delete + drops all bag/equipped items.
            // Requires confirmation; mail items require explicit
            // 'force' keyword. The destructive verb that used to be
            // ".merc dismiss" — renamed because friends kept reading
            // "dismiss" as the gentle send-away semantic, leading to
            // accidental gear loss.
            {"release",    HandleReleaseCommand,    SEC_PLAYER, Console::No},
            {"resync",     HandleResyncCommand,     SEC_PLAYER, Console::No},
            {"agent",      HandleAgentModeCommand,  SEC_PLAYER, Console::No},
            {"admin",      mercAdminTable},
        };

        static ChatCommandTable commandTable = {
            {"merc", mercTable},
        };

        return commandTable;
    }

    static bool HandleHireCommand(ChatHandler* handler, char const* args)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        std::string arg = args ? args : "";
        if (arg.empty())
        {
            handler->SendSysMessage("Usage: .merc hire <class> [name]   (class: warrior|paladin|hunter|rogue|priest|dk|shaman|mage|warlock|druid)");
            return true;
        }

        uint32 currentCount = sMercenaryMgr.GetMercCount(player->GetGUID());
        if (currentCount >= Sbywow::MAX_MERCS_PER_OWNER)
        {
            handler->PSendSysMessage("You already have {} mercenaries (cap: {}). Dismiss one before hiring another.",
                currentCount, Sbywow::MAX_MERCS_PER_OWNER);
            return true;
        }

        // Split into class [name].
        std::string classArg, desiredName;
        std::size_t space = arg.find(' ');
        if (space == std::string::npos)
        {
            classArg = arg;
        }
        else
        {
            classArg    = arg.substr(0, space);
            desiredName = arg.substr(space + 1);
            // Strip trailing whitespace from name.
            while (!desiredName.empty() && desiredName.back() == ' ')
                desiredName.pop_back();
        }

        uint8 cls = ParseClassArg(classArg);
        if (cls == 0)
        {
            handler->PSendSysMessage("Unknown class: '{}'. Valid: warrior, paladin, hunter, rogue, priest, dk, shaman, mage, warlock, druid.", classArg);
            return true;
        }

        ObjectGuid mercGuid = MercenaryFactory::CreateMerc(player->GetGUID(), cls, desiredName);
        if (mercGuid.IsEmpty())
        {
            if (!desiredName.empty())
                handler->PSendSysMessage("Hire failed — name '{}' may be invalid (must be 2..12 chars and unique). Check server log.", desiredName);
            else
                handler->SendSysMessage("Hire failed. Check server log for details.");
            return true;
        }

        std::string mercName;
        sCharacterCache->GetCharacterNameByGuid(mercGuid, mercName);
        handler->PSendSysMessage("Hired {} mercenary '{}' (guid={}). Auto-summoned and joined your party.",
            ClassName(cls), mercName, mercGuid.GetCounter());
        return true;
    }

    static bool HandleListCommand(ChatHandler* handler, char const* /*args*/)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        std::vector<ObjectGuid> mercs = sMercenaryMgr.GetMercsForOwner(player->GetGUID());
        if (mercs.empty())
        {
            handler->SendSysMessage("You have no hired mercenaries.");
            return true;
        }

        handler->PSendSysMessage("You have {} mercenary(s):", mercs.size());
        for (ObjectGuid mercGuid : mercs)
        {
            CharacterCacheEntry const* entry = sCharacterCache->GetCharacterCacheByGuid(mercGuid);
            if (!entry)
            {
                handler->PSendSysMessage("  guid={} (cache miss)", mercGuid.GetCounter());
                continue;
            }
            handler->PSendSysMessage("  '{}' — {} (level {}, guid={})",
                entry->Name, ClassName(entry->Class), uint32(entry->Level), mercGuid.GetCounter());
        }
        return true;
    }

    // .merc info <name> — detailed state for one merc. Online: live position
    // and HP from the Player object. Offline: last-saved snapshot from the
    // characters table.
    static bool HandleInfoCommand(ChatHandler* handler, char const* args)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        std::string name = args ? args : "";
        if (name.empty())
        {
            handler->SendSysMessage("Usage: .merc info <name>");
            return true;
        }

        ObjectGuid mercGuid = sCharacterCache->GetCharacterGuidByName(name);
        if (mercGuid.IsEmpty() || !sMercenaryMgr.IsOwnedBy(mercGuid, player->GetGUID()))
        {
            handler->PSendSysMessage("'{}' is not one of your mercenaries.", name);
            return true;
        }

        CharacterCacheEntry const* entry = sCharacterCache->GetCharacterCacheByGuid(mercGuid);
        if (!entry)
        {
            handler->PSendSysMessage("'{}' has no cache entry — try .merc admin reap.", name);
            return true;
        }

        handler->PSendSysMessage("Mercenary '{}' (guid={}):", entry->Name, mercGuid.GetCounter());
        handler->PSendSysMessage("  Class: {}, Level: {}", ClassName(entry->Class), uint32(entry->Level));

        Player* merc = ObjectAccessor::FindConnectedPlayer(mercGuid);
        if (merc)
        {
            uint32 hpPct = merc->GetMaxHealth() > 0
                ? uint32((100ull * merc->GetHealth()) / merc->GetMaxHealth())
                : 0;
            char const* deadTag = merc->IsAlive() ? "" : " [DEAD]";
            handler->PSendSysMessage("  State: ONLINE — map={}, zone={}, pos=({:.1f}, {:.1f}, {:.1f})",
                merc->GetMapId(), merc->GetZoneId(),
                merc->GetPositionX(), merc->GetPositionY(), merc->GetPositionZ());
            handler->PSendSysMessage("  HP: {}/{} ({}%){}", merc->GetHealth(), merc->GetMaxHealth(), hpPct, deadTag);
            if (Group* grp = merc->GetGroup())
            {
                ObjectGuid leaderGuid = grp->GetLeaderGUID();
                std::string leaderName;
                sCharacterCache->GetCharacterNameByGuid(leaderGuid, leaderName);
                handler->PSendSysMessage("  Group leader: '{}' (guid={})",
                    leaderName.empty() ? "?" : leaderName.c_str(), leaderGuid.GetCounter());
            }
            else
                handler->SendSysMessage("  Group: solo");
        }
        else
        {
            QueryResult res = CharacterDatabase.Query(
                "SELECT map, zone, position_x, position_y, position_z, health, logout_time "
                "FROM characters WHERE guid = {}", mercGuid.GetCounter());
            if (!res)
            {
                handler->PSendSysMessage("  State: OFFLINE — character row missing (orphan candidate; will be reaped on next restart)");
                return true;
            }
            Field* f = res->Fetch();
            handler->PSendSysMessage("  State: OFFLINE — last-saved map={}, zone={}, pos=({:.1f}, {:.1f}, {:.1f})",
                f[0].Get<uint16>(), f[1].Get<uint16>(),
                f[2].Get<float>(), f[3].Get<float>(), f[4].Get<float>());
            handler->PSendSysMessage("  Last-saved HP: {}, logout_time epoch: {}",
                f[5].Get<uint32>(), f[6].Get<uint32>());
        }
        return true;
    }

    // .merc summon <name> — manual force-summon. Use when autosummon failed,
    // or when you want to call a previously-unsummoned merc back without
    // waiting for the next zone change. No-op if already in world.
    static bool HandleSummonCommand(ChatHandler* handler, char const* args)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        std::string name = args ? args : "";
        if (name.empty())
        {
            handler->SendSysMessage("Usage: .merc summon <name>");
            return true;
        }

        ObjectGuid mercGuid = sCharacterCache->GetCharacterGuidByName(name);
        if (mercGuid.IsEmpty() || !sMercenaryMgr.IsOwnedBy(mercGuid, player->GetGUID()))
        {
            handler->PSendSysMessage("'{}' is not one of your mercenaries.", name);
            return true;
        }

        if (ObjectAccessor::FindConnectedPlayer(mercGuid))
        {
            handler->PSendSysMessage("'{}' is already summoned.", name);
            return true;
        }

        if (Map* m = player->GetMap(); m && m->IsBattlegroundOrArena())
        {
            handler->SendSysMessage("Cannot summon mercenaries while in a battleground or arena.");
            return true;
        }

        PlayerbotMgr* mgr = GET_PLAYERBOT_MGR(player);
        if (!mgr)
        {
            handler->SendSysMessage("PlayerbotMgr not available — cannot summon.");
            return true;
        }

        std::string cmd = "add " + name;
        mgr->HandlePlayerbotCommand(cmd.c_str(), player);
        handler->PSendSysMessage("Summoning '{}'...", name);
        return true;
    }

    // .merc dismiss <name> (and the .merc unsummon alias) — graceful
    // despawn that PRESERVES the merc. State is saved via
    // PlayerbotMgr::LogoutPlayerBot so the next .merc summon picks up
    // where they left off; bag and equipped items are untouched.
    //
    // For PERMANENT termination (cascade-delete the character row +
    // drop all items), use `.merc release <name>` instead — that's the
    // destructive verb. We split them because friends naturally read
    // "dismiss" as English "send away," and the previous semantic
    // (where dismiss == delete) caused accidental gear loss. See
    // decisions.md "Dismiss is the gentle send-away" (2026-05-02).
    static bool HandleDismissCommand(ChatHandler* handler, char const* args)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        std::string name = args ? args : "";
        if (name.empty())
        {
            handler->SendSysMessage("Usage: .merc dismiss <name>   (recallable via .merc summon)");
            handler->SendSysMessage("       .merc release <name>   (permanent — drops gear)");
            return true;
        }

        ObjectGuid mercGuid = sCharacterCache->GetCharacterGuidByName(name);
        if (mercGuid.IsEmpty() || !sMercenaryMgr.IsOwnedBy(mercGuid, player->GetGUID()))
        {
            handler->PSendSysMessage("'{}' is not one of your mercenaries.", name);
            return true;
        }

        if (!ObjectAccessor::FindConnectedPlayer(mercGuid))
        {
            handler->PSendSysMessage("'{}' is already offline.", name);
            return true;
        }

        PlayerbotMgr* mgr = GET_PLAYERBOT_MGR(player);
        if (!mgr)
        {
            handler->SendSysMessage("PlayerbotMgr not available — cannot dismiss.");
            return true;
        }

        mgr->LogoutPlayerBot(mercGuid);
        handler->PSendSysMessage("'{}' dismissed (recallable via .merc summon {}).", name, name);
        return true;
    }

    // .merc dismissall — graceful despawn of every merc the player owns.
    // Same gentle semantic as .merc dismiss; mercs stay in the roster
    // and can be re-summoned individually.
    static bool HandleDismissAllCommand(ChatHandler* handler, char const* /*args*/)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        std::vector<ObjectGuid> mercs = sMercenaryMgr.GetMercsForOwner(player->GetGUID());
        if (mercs.empty())
        {
            handler->SendSysMessage("You have no mercenaries to dismiss.");
            return true;
        }

        PlayerbotMgr* mgr = GET_PLAYERBOT_MGR(player);
        if (!mgr)
        {
            handler->SendSysMessage("PlayerbotMgr not available — cannot dismiss.");
            return true;
        }

        uint32 count = 0;
        for (ObjectGuid mercGuid : mercs)
        {
            if (!ObjectAccessor::FindConnectedPlayer(mercGuid))
                continue;
            mgr->LogoutPlayerBot(mercGuid);
            ++count;
        }

        handler->PSendSysMessage("Dismissed {} mercenary(s) (all recallable via .merc summon).", count);
        return true;
    }

    // .merc release <name> [force] — permanent termination. Cascade-
    // deletes the character row + drops all bag/equipped items. Mail
    // items require explicit `force` confirmation. This is the
    // destructive verb that used to be ".merc dismiss"; renamed
    // because friends kept reading "dismiss" as the gentle send-away
    // and accidentally losing gear. See decisions.md
    // "Dismiss is the gentle send-away" (2026-05-02).
    static bool HandleReleaseCommand(ChatHandler* handler, char const* args)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        std::string raw = args ? args : "";
        if (raw.empty())
        {
            handler->SendSysMessage("Usage: .merc release <name> [force]");
            handler->SendSysMessage("WARNING: permanently deletes the merc + all gear.");
            handler->SendSysMessage("To send a merc home recoverably, use '.merc dismiss <name>'.");
            return true;
        }

        // Parse optional trailing 'force' keyword used to override the
        // pending-mail guard. Mercs never auto-process mail (they always
        // have a master, so CheckMailAction skips), and DismissMerc
        // cascades through Player::DeleteFromDB which silently nukes
        // mail + items.
        bool force = false;
        std::string name = raw;
        std::size_t space = raw.rfind(' ');
        if (space != std::string::npos)
        {
            std::string tail = raw.substr(space + 1);
            std::string tailLower = tail;
            std::transform(tailLower.begin(), tailLower.end(), tailLower.begin(),
                [](unsigned char c) { return std::tolower(c); });
            if (tailLower == "force")
            {
                force = true;
                name = raw.substr(0, space);
            }
        }

        ObjectGuid mercGuid = sCharacterCache->GetCharacterGuidByName(name);
        if (mercGuid.IsEmpty())
        {
            handler->PSendSysMessage("No character named '{}' exists.", name);
            return true;
        }

        if (!sMercenaryMgr.IsOwnedBy(mercGuid, player->GetGUID()))
        {
            handler->PSendSysMessage("'{}' is not one of your mercenaries.", name);
            return true;
        }

        if (!force)
        {
            QueryResult mailRes = CharacterDatabase.Query(
                "SELECT COUNT(*) FROM mail WHERE receiver = {}", mercGuid.GetCounter());
            uint32 mailCount = mailRes ? mailRes->Fetch()[0].Get<uint32>() : 0;
            if (mailCount > 0)
            {
                handler->PSendSysMessage("'{}' has {} pending mail item(s) — they will be lost on release.", name, mailCount);
                handler->PSendSysMessage("Use '.merc release {} force' to confirm.", name);
                return true;
            }
        }

        sMercenaryMgr.DismissMerc(mercGuid);
        handler->PSendSysMessage("Released mercenary '{}' (character + items deleted).", name);
        return true;
    }

    static bool HandleResyncCommand(ChatHandler* handler, char const* args)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        std::string name = args ? args : "";
        if (name.empty())
        {
            handler->SendSysMessage("Usage: .merc resync <name>");
            return true;
        }

        ObjectGuid mercGuid = sCharacterCache->GetCharacterGuidByName(name);
        if (mercGuid.IsEmpty() || !sMercenaryMgr.IsOwnedBy(mercGuid, player->GetGUID()))
        {
            handler->PSendSysMessage("'{}' is not one of your mercenaries.", name);
            return true;
        }

        Player* merc = ObjectAccessor::FindConnectedPlayer(mercGuid);
        if (!merc)
        {
            handler->PSendSysMessage("'{}' must be online to resync. Use '.merc summon {}' first.", name, name);
            return true;
        }

        uint8 level = player->GetLevel();
        if (merc->GetLevel() != level)
            merc->GiveLevel(level);
        PlayerbotFactory(merc, level, ITEM_QUALITY_EPIC).Randomize(true);

        handler->PSendSysMessage("Resynced '{}' to level {} with fresh gear.", name, uint32(level));
        return true;
    }

    // .merc agent <name> [on|off] — toggle whether the LLM agent is
    // actively driving this merc. No arg = query current state.
    // Default on summon is `off` (the merc runs default behavior
    // until the agent or you flip this on). Use `on` to hand control
    // to the agent harness; `off` to take it back / run defaults.
    //
    // The agent's queued intents and any active wait suspension are
    // preserved across both transitions — the merc resumes mid-task
    // when toggled back on. See decisions.md "Agent mode is an
    // explicit opt-in."
    static bool HandleAgentModeCommand(ChatHandler* handler, char const* args)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        std::string arg = args ? args : "";
        std::string name, onOff;
        if (std::size_t space = arg.find(' '); space == std::string::npos)
            name = arg;
        else
        {
            name  = arg.substr(0, space);
            onOff = arg.substr(space + 1);
            while (!onOff.empty() && onOff.back() == ' ')
                onOff.pop_back();
        }

        if (name.empty())
        {
            handler->SendSysMessage("Usage: .merc agent <name> [on|off]");
            return true;
        }

        ObjectGuid mercGuid = sCharacterCache->GetCharacterGuidByName(name);
        if (mercGuid.IsEmpty() || !sMercenaryMgr.IsOwnedBy(mercGuid, player->GetGUID()))
        {
            handler->PSendSysMessage("'{}' is not one of your mercenaries.", name);
            return true;
        }

        Player* merc = ObjectAccessor::FindConnectedPlayer(mercGuid);
        if (!merc)
        {
            handler->PSendSysMessage("'{}' is offline. Summon them first (.merc summon {}) — "
                                     "agent mode requires an active bridge session.",
                                     name, name);
            return true;
        }

        int parsed = ParseOnOff(onOff);
        if (parsed == -1)
        {
            handler->PSendSysMessage("Bad arg: '{}'. Use 'on' or 'off'.", onOff);
            return true;
        }

        // Query mode — no arg, just report current state.
        if (parsed == 2)
        {
            auto session = Sbywow::Bridge::BridgeServer::Instance().GetSession(mercGuid);
            bool agentMode = session && session->IsAgentMode();
            handler->PSendSysMessage("'{}' agent mode: {}.", name, agentMode ? "on (LLM driving)" : "off (default merc)");
            return true;
        }

        bool desired = (parsed == 1);
        bool prev = Sbywow::Bridge::BridgeServer::Instance().ApplyAgentModeToggle(merc, desired, "master");
        if (prev == desired)
            handler->PSendSysMessage("'{}' agent mode was already {} (no change).",
                                     name, desired ? "on" : "off");
        else
            handler->PSendSysMessage("'{}' agent mode: {} → {}.",
                                     name,
                                     prev    ? "on" : "off",
                                     desired ? "on" : "off");
        return true;
    }

    static bool HandleAdminSetupCommand(ChatHandler* handler, char const* /*args*/)
    {
        handler->SendSysMessage("Sbywow: running EnsureServiceState (idempotent)...");
        sMercenaryMgr.EnsureServiceState();
        handler->PSendSysMessage("Service account id: {}", sMercenaryMgr.GetServiceAccountId());
        return true;
    }

    static bool HandleAdminServiceCommand(ChatHandler* handler, char const* /*args*/)
    {
        QueryResult res = CharacterDatabase.Query("SELECT COUNT(*) FROM mod_sbywow_mercenaries");
        uint32 totalMercs = res ? res->Fetch()[0].Get<uint32>() : 0;

        handler->SendSysMessage("Mercenary service status:");
        handler->PSendSysMessage("  Service account: '{}' (id={})",
            Sbywow::SERVICE_ACCOUNT_NAME, sMercenaryMgr.GetServiceAccountId());
        handler->PSendSysMessage("  Mercenaries guild id: {} (0 = not yet bootstrapped)",
            sMercenaryMgr.GetMercenariesGuildId());
        handler->PSendSysMessage("  Guildmaster guid: {}",
            sMercenaryMgr.GetGuildmasterGuid().ToString());
        handler->PSendSysMessage("  Total mercs in DB: {}", totalMercs);
        return true;
    }

    // .merc admin reap — trigger the orphan reaper without restarting. Three
    // sweeps log to Server.log (server.loading channel); this command just
    // returns a one-line ack since the detail goes to log.
    static bool HandleAdminReapCommand(ChatHandler* handler, char const* /*args*/)
    {
        handler->SendSysMessage("Sbywow: running ReapOrphans — see Server.log for sweep details.");
        sMercenaryMgr.ReapOrphans();
        handler->SendSysMessage("Sbywow: ReapOrphans complete.");
        return true;
    }

    // .merc admin hire <ownerLowGuid> <class> — same as .merc hire but with
    // explicit owner GUID so it can be exercised from SOAP/console for testing.
    static bool HandleAdminHireCommand(ChatHandler* handler, char const* args)
    {
        std::string a = args ? args : "";
        if (a.empty())
        {
            handler->SendSysMessage("Usage: .merc admin hire <ownerLowGuid> <class> [name]");
            return true;
        }

        // Parse: ownerLowGuid <class> [name with possible spaces stripped]
        std::size_t s1 = a.find(' ');
        if (s1 == std::string::npos)
        {
            handler->SendSysMessage("Usage: .merc admin hire <ownerLowGuid> <class> [name]");
            return true;
        }

        uint32 ownerLow = std::strtoul(a.substr(0, s1).c_str(), nullptr, 10);
        std::string rest = a.substr(s1 + 1);

        std::string classArg, desiredName;
        std::size_t s2 = rest.find(' ');
        if (s2 == std::string::npos)
        {
            classArg = rest;
        }
        else
        {
            classArg    = rest.substr(0, s2);
            desiredName = rest.substr(s2 + 1);
            while (!desiredName.empty() && desiredName.back() == ' ')
                desiredName.pop_back();
        }

        if (ownerLow == 0)
        {
            handler->SendSysMessage("Bad ownerLowGuid.");
            return true;
        }

        uint8 cls = ParseClassArg(classArg);
        if (cls == 0)
        {
            handler->PSendSysMessage("Unknown class: '{}'.", classArg);
            return true;
        }

        ObjectGuid ownerGuid = ObjectGuid::Create<HighGuid::Player>(ownerLow);
        ObjectGuid mercGuid = MercenaryFactory::CreateMerc(ownerGuid, cls, desiredName);
        if (mercGuid.IsEmpty())
        {
            if (!desiredName.empty())
                handler->PSendSysMessage("Hire failed — name '{}' may be invalid (must be 2..12 chars and unique). Check server log.", desiredName);
            else
                handler->SendSysMessage("Hire failed. Check server log for details.");
            return true;
        }

        std::string mercName;
        sCharacterCache->GetCharacterNameByGuid(mercGuid, mercName);
        handler->PSendSysMessage("Hired {} '{}' (guid={}) for owner guid={}.",
            ClassName(cls), mercName, mercGuid.GetCounter(), ownerLow);
        return true;
    }

    // .merc admin nuke <guid> — full cascade-delete a character on the service
    // account using Player::DeleteFromDB (handles all 30+ related tables).
    // The OnPlayerDeleteFromDB hook also cleans the ownership row atomically;
    // the explicit RemoveOwnership below is belt-and-suspenders. Primary use:
    // clean up dangling service-account characters flagged by orphan reaper
    // sweep 3 (untracked chars; not in mod_sbywow_mercenaries).
    static bool HandleAdminNukeCommand(ChatHandler* handler, char const* args)
    {
        std::string a = args ? args : "";
        if (a.empty())
        {
            handler->SendSysMessage("Usage: .merc admin nuke <characterLowGuid>");
            return true;
        }

        uint32 lowGuid = std::strtoul(a.c_str(), nullptr, 10);
        if (lowGuid == 0)
        {
            handler->SendSysMessage("Bad characterLowGuid.");
            return true;
        }

        uint32 serviceAccount = sMercenaryMgr.GetServiceAccountId();
        if (serviceAccount == 0)
        {
            handler->SendSysMessage("Service account not bootstrapped.");
            return true;
        }

        ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(lowGuid);
        std::string name;
        sCharacterCache->GetCharacterNameByGuid(guid, name);

        Player::DeleteFromDB(lowGuid, serviceAccount, true, true);
        sCharacterCache->DeleteCharacterCacheEntry(guid, name);
        sMercenaryMgr.RemoveOwnership(guid);

        handler->PSendSysMessage("Nuked character guid={} (name='{}'). Cascade complete.",
            lowGuid, name);
        return true;
    }

    // .merc admin agent <name> [on|off] — same as .merc agent but
    // without the ownership check, for GMs assisting any player.
    // SOAP-accessible (Console::Yes) so the agent harness operator
    // can toggle remotely if needed.
    static bool HandleAdminAgentModeCommand(ChatHandler* handler, char const* args)
    {
        std::string arg = args ? args : "";
        std::string name, onOff;
        if (std::size_t space = arg.find(' '); space == std::string::npos)
            name = arg;
        else
        {
            name  = arg.substr(0, space);
            onOff = arg.substr(space + 1);
            while (!onOff.empty() && onOff.back() == ' ')
                onOff.pop_back();
        }

        if (name.empty())
        {
            handler->SendSysMessage("Usage: .merc admin agent <name> [on|off]");
            return true;
        }

        ObjectGuid mercGuid = sCharacterCache->GetCharacterGuidByName(name);
        if (mercGuid.IsEmpty())
        {
            handler->PSendSysMessage("No character found with name '{}'.", name);
            return true;
        }

        Player* merc = ObjectAccessor::FindConnectedPlayer(mercGuid);
        if (!merc)
        {
            handler->PSendSysMessage("'{}' is offline.", name);
            return true;
        }

        int parsed = ParseOnOff(onOff);
        if (parsed == -1)
        {
            handler->PSendSysMessage("Bad arg: '{}'. Use 'on' or 'off'.", onOff);
            return true;
        }

        if (parsed == 2)
        {
            auto session = Sbywow::Bridge::BridgeServer::Instance().GetSession(mercGuid);
            bool agentMode = session && session->IsAgentMode();
            handler->PSendSysMessage("'{}' agent mode: {}.", name, agentMode ? "on (LLM driving)" : "off (default merc)");
            return true;
        }

        bool desired = (parsed == 1);
        bool prev = Sbywow::Bridge::BridgeServer::Instance().ApplyAgentModeToggle(merc, desired, "master");
        if (prev == desired)
            handler->PSendSysMessage("'{}' agent mode was already {} (no change).",
                                     name, desired ? "on" : "off");
        else
            handler->PSendSysMessage("'{}' agent mode: {} → {}.",
                                     name,
                                     prev    ? "on" : "off",
                                     desired ? "on" : "off");
        return true;
    }
};

void AddSbywowMercCommands()
{
    new sbywow_merc_commandscript();
}
