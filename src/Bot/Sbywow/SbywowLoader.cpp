// Single registration entry point for everything under src/Bot/Sbywow/.
// Called from src/Script/Playerbots.cpp::AddPlayerbotsScripts.

void AddSbywowMercenaryHooks();
void AddSbywowMercCommands();

void AddSC_SbywowMercenaryScripts()
{
    AddSbywowMercenaryHooks();
    AddSbywowMercCommands();

    // EnsureServiceState() needs the DB ready. It will be wired into a
    // WorldScript bootstrap hook in a later step rather than called here.
}
