#include "host.h"

#include <string>

#include "core.h"

namespace host {
namespace {

const CanopyApi* g_Api = nullptr;
CanopyMod g_Mod = CANOPY_NO_MOD;

}  // namespace

void Attach(const CanopyApi* api, CanopyMod mod) {
    g_Api = api;
    g_Mod = mod;
}

const CanopyApi& Api() {
    return *g_Api;
}

const CanopyApi* ApiOrNull() {
    return g_Api;
}

CanopyMod Mod() {
    return g_Mod;
}

CanopySetting AddToggle(const char* key, const CanopyText& label, const CanopyText& help, bool defaultOn) {
    return g_Api->AddToggle(g_Mod, key, &label, &help, defaultOn);
}

CanopySetting AddChoice(
    const char* key, const CanopyText& label, const CanopyText& help, const CanopyText* choices, int32_t count,
    int32_t defaultIndex
) {
    return g_Api->AddChoice(g_Mod, key, &label, &help, choices, count, defaultIndex);
}

CanopySetting AddBinding(const char* key, const CanopyText& label, const CanopyText& help, CanopyDevice device, int32_t defaultPad) {
    return g_Api->AddBinding(g_Mod, key, &label, &help, device, defaultPad);
}

bool Enabled(CanopySetting setting) {
    return g_Api->GetSetting(setting, 1) != 0;
}

int32_t Binding(CanopySetting setting) {
    return g_Api->GetSetting(setting, CANOPY_NO_PAD);
}

int32_t Choice(CanopySetting setting, int32_t fallback) {
    return g_Api->GetSetting(setting, fallback);
}

bool ShowToast(const char* text) {
    return g_Api->ShowToast(g_Mod, text);
}

bool ShowYesNo(const char* text, CanopyAnswerFn callback, void* user) {
    return g_Api->ShowYesNo(g_Mod, text, callback, user);
}

bool Pressed(int32_t pad) {
    return g_Api->Pressed(pad);
}

bool Held(int32_t pad) {
    return g_Api->Held(pad);
}

int32_t WheelSteps() {
    return g_Api->WheelSteps();
}

}  // namespace host

// =============================================================
// CORE SERVICES
// =============================================================
// VillageQoL's side of core.h: its log lines and detours go through the
// loader, which owns the log file and the one MinHook instance.
namespace core {

void Log(const std::string& message) {
    if (const CanopyApi* api = host::ApiOrNull()) {
        api->Log(host::Mod(), message.c_str());
    }
}

bool DetourSet::Add(uintptr_t rva, void* detour, void** original, const char* name) {
    void* target = reinterpret_cast<void*>(g_ModuleBase + rva);
    if (!host::Api().CreateHook(host::Mod(), target, detour, original)) {
        Log(std::string(feature_) + ": the hook for " + name + " was not created.");
        return false;
    }
    targets_.push_back(target);
    return true;
}

void DetourSet::RemoveAll() {
    for (void* target : targets_) {
        host::Api().RemoveHook(host::Mod(), target);
    }
    targets_.clear();
}

}  // namespace core
