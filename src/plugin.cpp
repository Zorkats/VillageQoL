#define NOMINMAX
#include <windows.h>

#include <string>

#include "core.h"
#include "features.h"
#include "host.h"
#include "canopy_api.h"

// =============================================================
// VILLAGEQOL
// =============================================================
// Quality of life features for Village in the Shade, as a Canopy
// mod. Each feature checks its own signatures and creates its own detours, so
// a game update that breaks one leaves the others working. The loader's frame
// callback drives the features that need a per-frame tick on the game thread.

namespace {

using namespace core;

constexpr CanopyText kModName = {"VillageQoL", "VillageQoL"};

// Settings from before the loader lived in VillageQoL.ini next to the game.
// They move to mods\VillageQoL.ini once, before the first setting is read.
void MigrateSettings() {
    const std::string current = ModFilePath("VillageQoL.ini");
    const std::string legacy = ModFilePath("..\\VillageQoL.ini");
    if (GetFileAttributesA(current.c_str()) != INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesA(legacy.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return;
    }
    if (CopyFileA(legacy.c_str(), current.c_str(), TRUE)) {
        Log("Settings copied from the old VillageQoL.ini next to the game.");
    }
}

void Tick(void*) {
    quick_save::Tick();
    settings_hotkey::Tick();
    event_fast_forward::Tick();
    wheel_zoom::Tick();
    key_binding_backup::Tick();
}

void LogFeature(const char* name, bool installed) {
    Log(std::string(name) + (installed ? " installed." : " disabled."));
}

}  // namespace

CANOPY_MOD_MANIFEST("VillageQoL", "VillageQoL", "Zorkats",
                    CANOPY_VERSION(VILLAGEQOL_MAJOR, VILLAGEQOL_MINOR, VILLAGEQOL_PATCH), 3, NULL, 0);

CANOPY_MOD_EXPORT bool CanopyModInit(const CanopyApi* api) {
    if (!api || api->version < CANOPY_API_VERSION || api->size < sizeof(CanopyApi)) {
        return false;
    }
    g_ModuleBase = api->GameBase();
    ResolveAddresses();
    host::Attach(api, CANOPY_NO_MOD);
    MigrateSettings();
    const CanopyMod mod = api->RegisterMod("VillageQoL", &kModName);
    if (mod == CANOPY_NO_MOD) {
        return false;
    }
    host::Attach(api, mod);
    Log(AddressReport());

    // These run prompts, title returns and hotkeys from the tick, and the
    // tick also needs the shared engine services its address list covers.
    const bool ticking = RequirementsMet(retail::kSharedRequirements) && api->OnFrame(mod, &Tick, nullptr);
    const bool bgmInfo = bgm_info::Install();
    const bool quickSave = ticking && quick_save::Install();
    const bool settingsHotkey = ticking && settings_hotkey::Install();
    const bool walkSpeed = ticking && walk_speed::Install();
    const bool eventFastForward = ticking && event_fast_forward::Install();
    const bool wheelZoom = ticking && wheel_zoom::Install();
    const bool keyBindingBackup = ticking && key_binding_backup::Install();
    LogFeature("BGM info", bgmInfo);
    LogFeature("Quick save", quickSave);
    LogFeature("Settings hotkey", settingsHotkey);
    LogFeature("Walk speed", walkSpeed);
    LogFeature("Event fast forward", eventFastForward);
    LogFeature("Wheel zoom", wheelZoom);
    LogFeature("Key binding backup", keyBindingBackup);
    return bgmInfo || quickSave || settingsHotkey || walkSpeed || eventFastForward || wheelZoom || keyBindingBackup;
}

// The loader backs out after a failed hook enable: the quick save's code
// patches are the one change the loader cannot undo for us.
CANOPY_MOD_EXPORT void CanopyModShutdown() {
    quick_save::Uninstall();
}
