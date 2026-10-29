#pragma once
#include <cstdint>

#include "canopy_api.h"

// VillageQoL's view of Canopy: the API table it was started
// with, its mod handle, and short forms of the calls its features make.
namespace host {

// Set by CanopyModInit before any feature installs; mod may be CANOPY_NO_MOD
// until the mod registers.
void Attach(const CanopyApi* api, CanopyMod mod);
const CanopyApi& Api();
// Null before Attach.
const CanopyApi* ApiOrNull();
CanopyMod Mod();

// Settings of VillageQoL's own mod. See CanopyApi for the contracts.
CanopySetting AddToggle(const char* key, const CanopyText& label, const CanopyText& help, bool defaultOn);
CanopySetting AddChoice(
    const char* key, const CanopyText& label, const CanopyText& help, const CanopyText* choices, int32_t count,
    int32_t defaultIndex
);
CanopySetting AddBinding(const char* key, const CanopyText& label, const CanopyText& help, CanopyDevice device, int32_t defaultPad);

// A toggle's state, on when the setting is missing so a feature keeps its
// default behavior.
bool Enabled(CanopySetting setting);
// A binding's pad, CANOPY_NO_PAD when it is missing.
int32_t Binding(CanopySetting setting);
// A choice's index, fallback when it is missing.
int32_t Choice(CanopySetting setting, int32_t fallback);

// The loader's toast and Yes/No popup, for VillageQoL's own mod. False, with
// the reason in the log, when they cannot show.
bool ShowToast(const char* text);
bool ShowYesNo(const char* text, CanopyAnswerFn callback, void* user);

bool Pressed(int32_t pad);
bool Held(int32_t pad);
int32_t WheelSteps();

}  // namespace host
