#pragma once

// Each feature checks its own signatures, creates its own detours and
// registers its settings under VillageQoL's mod. Install returns false and
// leaves nothing behind when the game build does not match, so one stale
// feature never takes the others down. The loader enables every hook once
// all mods have started.
namespace bgm_info {
bool Install();
}

namespace settings_hotkey {
bool Install();
// Called once per frame on the game thread. Does nothing unless installed.
void Tick();
}

namespace quick_save {
bool Install();
// Called once per frame on the game thread. Does nothing unless installed.
void Tick();
// Removes the detours and code patches Install wrote, if hooks fail to enable.
void Uninstall();
}

namespace walk_speed {
bool Install();
}

namespace event_fast_forward {
bool Install();
// Called once per frame on the game thread. Does nothing unless installed.
void Tick();
}

namespace wheel_zoom {
bool Install();
// Called once per frame on the game thread. Does nothing unless installed.
void Tick();
}

namespace key_binding_backup {
bool Install();
// Called once per frame on the game thread. Does nothing unless installed.
void Tick();
}
