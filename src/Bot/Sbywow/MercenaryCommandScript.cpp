#include "CharacterCache.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "Field.h"
#include "MercenaryFactory.h"
#include "MercenaryMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotFactory.h"
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
            {"hire",    HandleAdminHireCommand,    SEC_ADMINISTRATOR, Console::Yes},
            {"nuke",    HandleAdminNukeCommand,    SEC_ADMINISTRATOR, Console::Yes},
        };

        static ChatCommandTable mercTable = {
            {"hire",       HandleHireCommand,       SEC_PLAYER,     Console::No},
            {"list",       HandleListCommand,       SEC_PLAYER,     Console::No},
            {"dismiss",    HandleDismissCommand,    SEC_PLAYER,     Console::No},
            {"dismissall", HandleDismissAllCommand, SEC_PLAYER,     Console::No},
            {"resync",    HandleResyncCommand,      SEC_GAMEMASTER, Console::No},
            {"admin",     mercAdminTable},
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
            handler->SendSysMessage("Usage: .merc hire <warrior|paladin|hunter|rogue|priest|dk|shaman|mage|warlock|druid>");
            return true;
        }

        uint32 currentCount = sMercenaryMgr.GetMercCount(player->GetGUID());
        if (currentCount >= Sbywow::MAX_MERCS_PER_OWNER)
        {
            handler->PSendSysMessage("You already have {} mercenaries (cap: {}). Dismiss one before hiring another.",
                currentCount, Sbywow::MAX_MERCS_PER_OWNER);
            return true;
        }

        uint8 cls = ParseClassArg(arg);
        if (cls == 0)
        {
            handler->PSendSysMessage("Unknown class: '{}'. Valid: warrior, paladin, hunter, rogue, priest, dk, shaman, mage, warlock, druid.", arg);
            return true;
        }

        ObjectGuid mercGuid = MercenaryFactory::CreateMerc(player->GetGUID(), cls);
        if (mercGuid.IsEmpty())
        {
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

    static bool HandleDismissCommand(ChatHandler* handler, char const* args)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        std::string raw = args ? args : "";
        if (raw.empty())
        {
            handler->SendSysMessage("Usage: .merc dismiss <name> [force]");
            return true;
        }

        // Parse optional trailing 'force' keyword used to override the
        // pending-mail guard. Mercs never auto-process mail (they always have
        // a master, so CheckMailAction skips), and DismissMerc cascades through
        // Player::DeleteFromDB which silently nukes the mail + items.
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
                handler->PSendSysMessage("'{}' has {} pending mail item(s) — they will be lost on dismiss.", name, mailCount);
                handler->PSendSysMessage("Use '.merc dismiss {} force' to confirm.", name);
                return true;
            }
        }

        sMercenaryMgr.DismissMerc(mercGuid);
        handler->PSendSysMessage("Dismissed mercenary '{}'.", name);
        return true;
    }

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

        for (ObjectGuid mercGuid : mercs)
            sMercenaryMgr.DismissMerc(mercGuid);

        handler->PSendSysMessage("Dismissed {} mercenary(s).", mercs.size());
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
            handler->SendSysMessage("Merc must be online to resync. Re-hire path will sync next summon.");
            return true;
        }

        uint8 level = player->GetLevel();
        if (merc->GetLevel() != level)
            merc->GiveLevel(level);
        PlayerbotFactory(merc, level, ITEM_QUALITY_EPIC).Randomize(true);

        handler->PSendSysMessage("Resynced '{}' to level {} with fresh gear.", name, uint32(level));
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

    // .merc admin hire <ownerLowGuid> <class> — same as .merc hire but with
    // explicit owner GUID so it can be exercised from SOAP/console for testing.
    static bool HandleAdminHireCommand(ChatHandler* handler, char const* args)
    {
        std::string a = args ? args : "";
        if (a.empty())
        {
            handler->SendSysMessage("Usage: .merc admin hire <ownerLowGuid> <class>");
            return true;
        }

        std::size_t space = a.find(' ');
        if (space == std::string::npos)
        {
            handler->SendSysMessage("Usage: .merc admin hire <ownerLowGuid> <class>");
            return true;
        }

        uint32 ownerLow = std::strtoul(a.substr(0, space).c_str(), nullptr, 10);
        std::string classArg = a.substr(space + 1);

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
        ObjectGuid mercGuid = MercenaryFactory::CreateMerc(ownerGuid, cls);
        if (mercGuid.IsEmpty())
        {
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
    // account using Player::DeleteFromDB (handles all 30+ related tables). Use
    // this to clean up orphans left by failed CreateMerc / partial dismiss.
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
};

void AddSbywowMercCommands()
{
    new sbywow_merc_commandscript();
}
