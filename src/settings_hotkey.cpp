#define NOMINMAX
#include <windows.h>

#include <cstdint>

#include "core.h"
#include "features.h"
#include "host.h"

// =============================================================
// SETTINGS HOTKEY DESIGN
// =============================================================
// The settings hotkey (F10 by default) opens the main menu on its Option tab
// whenever the player has control. The field's own menu button is ignored
// while game flag 308 forbids the main menu, which the opening hours keep
// set, so the settings are out of reach.
// The hotkey runs the exact sequence the field's menu button runs (alloc,
// ctor, attach to the UI root with mode 2), only with the Option tab as the
// start tab. No game code is patched and flag 308 is never written.

namespace settings_hotkey {
namespace {

using namespace core;

// The hotkey's keyboard or mouse binding, F10 by default, and its controller
// binding, none by default. 132 is F10's framework pad id.
constexpr int32_t kDefaultKey = 132;

constexpr CanopyText kKeyLabels = {
    "設定キー", "Settings Key", "Touche des options", "Tecla de opciones", "設定鍵", "설정 키", "设置键",
};

constexpr CanopyText kButtonLabels = {
    "設定ボタン", "Settings Button", "Bouton des options", "Botón de opciones", "設定按鈕", "설정 버튼", "设置按钮",
};

constexpr CanopyText kSettingHelp = {
    "フィールドで押すとオプションを開きます。",
    "Press it in the field to open the options.",
    "Appuyez dessus sur le terrain pour ouvrir les options.",
    "Púlsalo en el campo para abrir las opciones.",
    "在野外按下即可開啟選項。",
    "필드에서 누르면 옵션을 엽니다.",
    "在野外按下即可打开选项。",
};

using MainMenuCtorFn = uint8_t*(__fastcall*)(uint8_t* task, int32_t startTab);
using PlaySystemSeFn = void(__fastcall*)(uint64_t seId);

MainMenuCtorFn g_MainMenuCtor = nullptr;
PlaySystemSeFn g_PlaySystemSe = nullptr;

bool g_Installed = false;
int g_KeySetting = CANOPY_NO_SETTING;
int g_ButtonSetting = CANOPY_NO_SETTING;
// Our own reference to the menu the hotkey opened, so its refcount dropping
// to ours alone means the engine closed it.
uint8_t* g_MenuTask = nullptr;

void OpenSettings() {
    if (g_MenuTask) {
        Log("Settings hotkey ignored, the menu it opened is still open.");
        return;
    }
    if (!PlayerInField()) {
        Log("Settings hotkey ignored, the player does not have control.");
        return;
    }
    void* root = ReadGlobal<void*>(retail::kUiTaskRoot);
    if (!root) {
        return;
    }
    uint8_t* task = AllocateZeroed(retail::kMainMenuTaskSize);
    if (!task) {
        return;
    }
    g_PlaySystemSe(retail::kMenuOpenSe);
    g_MainMenuCtor(task, retail::kMainMenuOptionTab);
    AddRef(task, retail::kTaskRefCount);
    AttachTask(root, task, retail::kAttachModePushLayer);
    g_MenuTask = task;
    Log("Settings opened from the hotkey.");
}

}  // namespace

bool Install() {
    if (!RequirementsMet(retail::kSettingsHotkeyRequirements)) {
        return false;
    }
    g_MainMenuCtor = GameFunction<MainMenuCtorFn>(retail::kMainMenuCtor);
    g_PlaySystemSe = GameFunction<PlaySystemSeFn>(retail::kPlaySystemSe);
    g_KeySetting = host::AddBinding(
        "SettingsKeyboard", kKeyLabels, kSettingHelp, CANOPY_DEVICE_KEYBOARD_MOUSE, kDefaultKey
    );
    g_ButtonSetting = host::AddBinding(
        "SettingsController", kButtonLabels, kSettingHelp, CANOPY_DEVICE_CONTROLLER, CANOPY_NO_PAD
    );
    g_Installed = true;
    return true;
}

void Tick() {
    if (!g_Installed) {
        return;
    }
    if (g_MenuTask && ReadAt<int32_t>(g_MenuTask, retail::kTaskRefCount) <= 1) {
        Release(g_MenuTask, retail::kTaskRefCount);
        g_MenuTask = nullptr;
    }
    if (host::Pressed(host::Binding(g_KeySetting)) || host::Pressed(host::Binding(g_ButtonSetting))) {
        OpenSettings();
    }
}

}  // namespace settings_hotkey
