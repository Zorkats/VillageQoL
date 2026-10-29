#define NOMINMAX
#include <windows.h>

#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "core.h"
#include "features.h"
#include "host.h"

// =============================================================
// QUICK SAVE DESIGN
// =============================================================
// The quick save hotkey (F5 by default, rebindable on the Mod Settings page)
// runs the game's own AutoSave process, redirected to one extra slot per
// save group (16 + group), so the five rotating autosaves are never evicted
// and the save file format is whatever the engine writes. The rotation and
// .systemsave bookkeeping the AutoSave process does on success is undone for
// the quick slot.
//
// The hotkey first opens the game's own Yes/No popup with a red warning that
// the quick save is deleted once loaded. The save only starts after Yes, once
// the player has control again. A quick save is a suspend save: once it is
// written and the field has control back, every sound player fades out and
// the game returns to the title through the engine's own title routine.
//
// The load menu gets a sixth native row for that slot. The menu loads by
// save list index, and the row carries the quick save's index, so the engine
// loads it natively. The carousel opens on that row, and a group card whose
// group has a quick save loads it too, with the card's mode label marked.
// Once any load of a quick slot succeeded and the player is back in control,
// the game's AutoDelete removes the slot.
//
// A quick load resumes where the save was made: the bed wake-up script the
// engine runs after every normal load is replaced by what it ends with, time
// running again and a fade in from black.
//
// A normal load starts a new morning, so the engine clears every game flag
// the gameflag table marks isDailyOff, among them the flags that keep the
// day's story events from starting twice. A quick load resumes the same day,
// so it skips those clears and the day keeps its flags.
//
// A map event (a crossroads event, a warp, a shop door) starts when a body
// begins to touch its body, and a map event's setup starts it for every body
// already touching it, so a player loaded inside one would set it off. For a
// short window after a quick load, neither counts the player's bodies. The
// player was already inside when the save was made, so, as in the running
// game, they have to step out and back in to start it.
//
// The load takes other branches when the save was made before the story
// starts, late at night, while coming home in the morning, after the player
// collapsed, or with one of the events those branches act on running. Each
// moves the story on instead of resuming, so the quick save is refused there.

namespace quick_save {
namespace {

using namespace core;

// Quick slots sit after the 15 autosave slots: 16, 17, 18 for groups 0..2.
constexpr int32_t kQuickSlotBase = retail::kGroupCount * retail::kRotationCount + 1;
constexpr int32_t kQuickSlotCapacity = kQuickSlotBase + retail::kGroupCount - 1;
constexpr int kQuickRow = retail::kLoadMenuRetailRowCount;
// A launched task that never shows up as busy is treated as finished after
// this many frames, so a lost task can never block quick saves forever.
constexpr int kLaunchTimeoutFrames = 600;

// "Quick Save" per language index (JA, EN, FR, ES, ZH-TW, KO, ZH-CN), UTF-8.
constexpr const char* kQuickSaveLabels[retail::kLanguageCount] = {
    "\xE3\x82\xAF\xE3\x82\xA4\xE3\x83\x83\xE3\x82\xAF\xE3\x82\xBB\xE3\x83\xBC\xE3\x83\x96",
    "Quick Save",
    "Sauvegarde rapide",
    "Guardado r\xC3\xA1pido",
    "\xE5\xBF\xAB\xE9\x80\x9F\xE5\xAD\x98\xE6\xAA\x94",
    "\xED\x80\xB5 \xEC\x84\xB8\xEC\x9D\xB4\xEB\xB8\x8C",
    "\xE5\xBF\xAB\xE9\x80\x9F\xE5\xAD\x98\xE6\xA1\xA3",
};

// The hotkey's keyboard or mouse binding, F5 by default, and its controller
// binding, none by default. With both unbound the load menu keeps its quick
// save row, so a quick save made earlier can still be loaded. 127 is F5's
// framework pad id.
constexpr int32_t kDefaultKey = 127;

constexpr CanopyText kKeyLabels = {
    "クイックセーブキー", "Quick Save Key", "Touche de sauvegarde rapide", "Tecla de guardado rápido",
    "快速存檔鍵", "빠른 저장 키", "快速存档键",
};

constexpr CanopyText kButtonLabels = {
    "クイックセーブボタン", "Quick Save Button", "Bouton de sauvegarde rapide", "Botón de guardado rápido",
    "快速存檔按鈕", "빠른 저장 버튼", "快速存档按钮",
};

constexpr CanopyText kSettingHelp = {
    "フィールドで押すとクイックセーブしてタイトルに戻ります。",
    "Press it in the field to quicksave and return to the title screen.",
    "Appuyez dessus sur le terrain pour sauvegarder rapidement et revenir à l'écran titre.",
    "Púlsalo en el campo para guardar rápido y volver a la pantalla de título.",
    "在野外按下即可快速存檔並返回標題畫面。",
    "필드에서 누르면 빠른 저장 후 타이틀 화면으로 돌아갑니다.",
    "在野外按下即可快速存档并返回标题画面。",
};

// Shown when the story state does not allow a quick save, UTF-8.
constexpr const char* kRefusedToasts[retail::kLanguageCount] = {
    "今はクイックセーブできません。",
    "Quick save is not available right now.",
    "La sauvegarde rapide n'est pas disponible pour le moment.",
    "El guardado rápido no está disponible ahora.",
    "目前無法快速存檔。",
    "지금은 빠른 저장을 할 수 없습니다.",
    "目前无法快速存档。",
};

// The contact window after a quick load ends once the player has had control
// this many frames, or after the cap.
constexpr int kGuardControlFrames = 30;
constexpr int kGuardMaxFrames = 1800;

// Hotkey prompt per language index, UTF-8: the question, then a red warning line.
constexpr const char* kQuickSavePrompts[retail::kLanguageCount] = {
    "\xE3\x82\xAF\xE3\x82\xA4\xE3\x83\x83\xE3\x82\xAF\xE3\x82\xBB\xE3\x83\xBC\xE3\x83\x96\xE3\x81\x97\xE3\x81"
    "\xA6\xE3\x82\xBF\xE3\x82\xA4\xE3\x83\x88\xE3\x83\xAB\xE3\x81\xAB\xE6\x88\xBB\xE3\x82\x8A\xE3\x81\xBE\xE3"
    "\x81\x99\xE3\x81\x8B\xEF\xBC\x9F<br><c red>\xE3\x82\xAF\xE3\x82\xA4\xE3\x83\x83\xE3\x82\xAF\xE3\x82\xBB"
    "\xE3\x83\xBC\xE3\x83\x96\xE3\x81\xAF\xE8\xAA\xAD\xE3\x81\xBF\xE8\xBE\xBC\xE3\x81\xBF\xE5\xBE\x8C\xE3\x81"
    "\xAB\xE5\x89\x8A\xE9\x99\xA4\xE3\x81\x95\xE3\x82\x8C\xE3\x81\xBE\xE3\x81\x99\xE3\x80\x82</c>",
    "Quicksave and return to the title screen?<br><c red>Quicksaves get deleted after the game loads "
    "them.</c>",
    "Faire une sauvegarde rapide et revenir \xC3\xA0 l'\xC3\xA9"
    "cran titre ?<br><c red>Les sauvegardes rapides sont effac\xC3\xA9"
    "es une fois charg\xC3\xA9"
    "es.</c>",
    "\xC2\xBFGuardar r\xC3\xA1pido y volver a la pantalla de t\xC3\xADtulo?<br><c red>Los guardados r\xC3\xA1"
    "pidos se eliminan al cargarlos.</c>",
    "\xE8\xA6\x81\xE5\xBF\xAB\xE9\x80\x9F\xE5\xAD\x98\xE6\xAA\x94\xE4\xB8\xA6\xE8\xBF\x94\xE5\x9B\x9E\xE6\xA8"
    "\x99\xE9\xA1\x8C\xE7\x95\xAB\xE9\x9D\xA2\xE5\x97\x8E\xEF\xBC\x9F<br><c red>\xE5\xBF\xAB\xE9\x80\x9F\xE5"
    "\xAD\x98\xE6\xAA\x94\xE5\x9C\xA8\xE8\xAE\x80\xE5\x8F\x96\xE5\xBE\x8C\xE6\x9C\x83\xE8\xA2\xAB\xE5\x88\xAA"
    "\xE9\x99\xA4\xE3\x80\x82</c>",
    "\xED\x80\xB5 \xEC\x84\xB8\xEC\x9D\xB4\xEB\xB8\x8C\xED\x95\x98\xEA\xB3\xA0 \xED\x83\x80\xEC\x9D\xB4\xED"
    "\x8B\x80\xEB\xA1\x9C \xEB\x8F\x8C\xEC\x95\x84\xEA\xB0\x80\xEC\x8B\x9C\xEA\xB2\xA0\xEC\x8A\xB5\xEB\x8B"
    "\x88\xEA\xB9\x8C?<br><c red>\xED\x80\xB5 \xEC\x84\xB8\xEC\x9D\xB4\xEB\xB8\x8C\xEB\x8A\x94 \xEB\xB6\x88"
    "\xEB\x9F\xAC\xEC\x98\xA8 \xED\x9B\x84 \xEC\x82\xAD\xEC\xA0\x9C\xEB\x90\xA9\xEB\x8B\x88\xEB\x8B\xA4.</c>",
    "\xE8\xA6\x81\xE5\xBF\xAB\xE9\x80\x9F\xE5\xAD\x98\xE6\xA1\xA3\xE5\xB9\xB6\xE8\xBF\x94\xE5\x9B\x9E\xE6\xA0"
    "\x87\xE9\xA2\x98\xE7\x94\xBB\xE9\x9D\xA2\xE5\x90\x97\xEF\xBC\x9F<br><c red>\xE5\xBF\xAB\xE9\x80\x9F\xE5"
    "\xAD\x98\xE6\xA1\xA3\xE5\x9C\xA8\xE5\x8A\xA0\xE8\xBD\xBD\xE5\x90\x8E\xE4\xBC\x9A\xE8\xA2\xAB\xE5\x88\xA0"
    "\xE9\x99\xA4\xE3\x80\x82</c>",
};

using AutoSaveBuildSettingFn = uint64_t(__fastcall*)(uint8_t** capture, void* data);
using AutoSaveSucceededFn = uint64_t(__fastcall*)(uint8_t** capture);
using SaveUtilAcknowledgeFn = uint8_t(__fastcall*)(uint8_t* util);
using LoadMenuFillRowsFn = uint8_t(__fastcall*)(uint8_t* state, uint8_t* saveList);
using LoadMenuHeaderTextFn = int(__fastcall*)(uint8_t* functor, void* outText);
using LoadCardsListenerFn = uint8_t(__fastcall*)(uint8_t* task, uint8_t* saveList);
using TextProviderFn = void(__fastcall*)(uint8_t* functor, void* outText);
using StateInitializeFn = void(__fastcall*)(uint8_t* state);
using LoadEventStartFn = void(__fastcall*)();

using TaskCtorFn = uint8_t*(__fastcall*)(uint8_t* task, uint8_t** setting);
using SettingBaseCtorFn = uint8_t*(__fastcall*)(uint8_t* setting);
using GetListCursorFn = int32_t(__fastcall*)(void* list);
using SaveInfoCtorFn = uint8_t*(__fastcall*)(uint8_t* info);
using SaveInfoAssignFn = void(__fastcall*)(uint8_t* info, void* source);
using SetGameFlagFn = void(__fastcall*)(int32_t flag, bool value);
using ScreenFadeFn = void(__fastcall*)(
    void* fadeManager, int32_t step, uint32_t color, const void* param, void* onEnd, uint8_t** outTask
);
using CStringAppendFn = void*(__fastcall*)(void* text, const char* data);
using SetListCursorFn = bool(__fastcall*)(void* list, int32_t index, int32_t top);

using FindCollisionComponentFn = uint8_t*(__fastcall*)(uint8_t* entity);
using ClearGameFlagFn = uint64_t(__fastcall*)(uint8_t* saveData, int32_t flag);
using EventConditionsFn = bool(__fastcall*)(
    uint8_t* saveData, void* conditions, uint64_t count, void* flagConditions, uint64_t flagCount
);
using MapEventSetupScriptFn = uint64_t(__fastcall*)(uint8_t* mapEvent);
using MapEventContactFn = uint64_t(__fastcall*)(uint8_t** mapEvent, uint8_t* selfBody, uint8_t* const* otherBody);

// Ngf FadeParam as Lua FadeMake builds it.
struct FadeParam {
    double frames;
    double reserved0;
    int32_t type;
    int32_t padding;
    double from;
    double to;
    uint64_t reserved1;
};
static_assert(sizeof(FadeParam) == 48, "FadeParam layout differs from the engine's");

AutoSaveBuildSettingFn g_OriginalAutoSaveBuildSetting = nullptr;
AutoSaveSucceededFn g_OriginalAutoSaveSucceeded = nullptr;
SaveUtilAcknowledgeFn g_OriginalSaveUtilAcknowledge = nullptr;
LoadMenuFillRowsFn g_OriginalLoadMenuFillRows = nullptr;
LoadMenuHeaderTextFn g_OriginalLoadMenuHeaderText = nullptr;
LoadCardsListenerFn g_OriginalLoadCardsListener = nullptr;
TextProviderFn g_OriginalCardModeText = nullptr;
StateInitializeFn g_OriginalCarouselInitialize = nullptr;
TextProviderFn g_OriginalCarouselPlayTimeText = nullptr;
LoadEventStartFn g_OriginalLoadEventStart = nullptr;
ClearGameFlagFn g_OriginalClearGameFlag = nullptr;
EventConditionsFn g_OriginalEventConditions = nullptr;
MapEventSetupScriptFn g_OriginalMapEventSetupScript = nullptr;
MapEventContactFn g_OriginalMapEventContact = nullptr;

TaskCtorFn g_AutoSaveProcessCtor = nullptr;
TaskCtorFn g_AutoDeleteCtor = nullptr;
SettingBaseCtorFn g_SaveSettingBaseCtor = nullptr;
GetListCursorFn g_GetListCursor = nullptr;
SaveInfoCtorFn g_SaveInfoCtor = nullptr;
SetGameFlagFn g_SetGameFlag = nullptr;
ScreenFadeFn g_ScreenFade = nullptr;
CStringAppendFn g_CStringAppend = nullptr;
SetListCursorFn g_SetListCursor = nullptr;
FindCollisionComponentFn g_FindCollisionComponent = nullptr;

// Set once the load menu patches are in, so the row detours never touch a
// sixth row the engine did not allocate.
bool g_MenuPatched = false;

// Quick save and delete state. Every detour that reads or writes
// these runs on the game thread.
struct TaskWatch {
    bool active = false;
    bool sawBusy = false;
    int frames = 0;
};

uint8_t* g_SaveTask = nullptr;
int32_t g_SaveSlot = 0;
TaskWatch g_SaveWatch;
bool g_SaveWritten = false;
bool g_TitleRequested = false;
int g_TitleRequestFrames = 0;
int32_t g_PendingDeleteSlot = 0;
int32_t g_DeleteSlot = 0;
TaskWatch g_DeleteWatch;
int g_KeySetting = CANOPY_NO_SETTING;
int g_ButtonSetting = CANOPY_NO_SETTING;
// Set by every finished load: true when it read a quick slot. The load's
// daily flag clears read it, and its wake-up script start consumes it.
bool g_ResumeQuickLoad = false;

// The daily flag clears a quick load skipped and how many of those flags
// were set, reported on the next frame.
std::atomic<int> g_SkippedDailyClears{0};
std::atomic<int> g_KeptDailyFlags{0};

// Diagnostics for the log, by id only: the set daily flags a quick load kept,
// and the map events the contact window held back, with the flags their
// conditions check, so the window's close can check them again.
constexpr size_t kMaxLoggedIds = 32;

struct HeldBackEvent {
    bool readable = false;
    uint64_t id = 0;
    const char* source = "";
    int count = 0;
    bool readyThen = false;
    uint64_t requiredCount = 0;
    uint64_t required[retail::kEventRecordFlagSlots] = {};
    uint64_t forbiddenCount = 0;
    uint64_t forbidden[retail::kEventRecordFlagSlots] = {};
};

std::mutex g_DiagnosticsMutex;
std::vector<int32_t> g_KeptFlagIds;
std::vector<HeldBackEvent> g_HeldBackEvents;

// The contact window after a quick load. Map setup may run off the game
// thread, so what the detours touch is atomic. A fault reading the engine's
// bodies turns the window off until the game restarts.
std::atomic<bool> g_GuardActive{false};
std::atomic<bool> g_GuardFaulted{false};
std::atomic<int> g_HeldBackContacts{0};
std::atomic<int> g_UnresolvedContacts{0};
int g_GuardFrames = 0;
int g_GuardControlFrames = 0;

// The walk a map event setup makes over the bodies its body touches, for the
// innermost setup running on this thread.
struct SetupWalk {
    uint8_t* mapEvent = nullptr;
    bool started = false;
    const uint8_t* head = nullptr;
    const uint8_t* node = nullptr;
};
thread_local SetupWalk g_SetupWalk;

// Group cards currently redirected to their quick save, for the card task
// that owns them.
const uint8_t* g_QuickCardTask = nullptr;
bool g_QuickCards[retail::kGroupCount] = {};

// The carousel waiting for its cursor to move to the quick row, and its list
// once one of its text providers has shown it.
const uint8_t* g_CursorState = nullptr;
uint8_t* g_CursorList = nullptr;
int g_CursorFrames = 0;

// Hotkey prompt state: the loader's popup is open until its answer arrives.
bool g_PromptOpen = false;
bool g_SaveRequested = false;
int g_SaveRequestFrames = 0;
// Created by Install and kept so Uninstall can remove them.
DetourSet g_Detours("Quick save");
bool g_Installed = false;

constexpr size_t kLoadMenuPatchCount = sizeof(retail::kLoadMenuPatches) / sizeof(retail::kLoadMenuPatches[0]);

// =============================================================
// UTILS
// =============================================================
// True when every site the build has holds the retail bytes and every site
// that is not optional is there.
bool MatchesPatchSites() {
    for (const retail::CodePatch& patch : retail::kLoadMenuPatches) {
        if (!patch.address->Found()) {
            if (patch.optional) {
                continue;
            }
            Log(std::string("Game address ") + patch.address->Name() + " was not found in this game build.");
            return false;
        }
        const void* code = reinterpret_cast<const void*>(g_ModuleBase + *patch.address);
        if (std::memcmp(code, patch.original, patch.length) != 0) {
            Log(std::string("Patch site ") + patch.address->Name() + " does not hold the expected code.");
            return false;
        }
    }
    return true;
}

// =============================================================
// GAME STATE
// =============================================================
int32_t QuickSlotForGroup(int32_t group) {
    return kQuickSlotBase + group;
}

bool IsQuickSlot(int32_t slot) {
    return slot >= kQuickSlotBase && slot <= kQuickSlotCapacity;
}

int32_t NormalizeRotation(int32_t rotation) {
    // Same normalization the engine applies to the newest rotation index.
    if (rotation > retail::kRotationCount - 1) {
        return rotation % retail::kRotationCount;
    }
    if (rotation < 0) {
        return (rotation + 1) % retail::kRotationCount + retail::kRotationCount - 1;
    }
    return rotation;
}

// The save utility rejects slots above its capacity (16 in retail), so it is
// raised to cover the quick slots of groups 1 and 2 only when one is used.
void EnsureSlotCapacity(int32_t slot) {
    uint8_t* util = ReadGlobal<uint8_t*>(retail::kSaveUtil);
    if (!util) {
        return;
    }
    const int32_t capacity = ReadAt<int32_t>(util, retail::kSaveUtilSlotCapacity);
    if (slot > capacity && slot <= kQuickSlotCapacity) {
        WriteAt<int32_t>(util, retail::kSaveUtilSlotCapacity, kQuickSlotCapacity);
        Log("Raised save slot capacity from " + std::to_string(capacity) + " to " +
            std::to_string(kQuickSlotCapacity) + ".");
    }
}

bool SaveSystemIdle() {
    const uint8_t* work = ReadGlobal<uint8_t*>(retail::kSaveWork);
    const uint8_t* util = ReadGlobal<uint8_t*>(retail::kSaveUtil);
    return work && util && ReadAt<int32_t>(work, retail::kSaveWorkBusy) == 0 &&
           ReadAt<int32_t>(work, retail::kSaveWorkLiveTasks) == 0 && ReadAt<int32_t>(util, retail::kSaveUtilState) == 0;
}

bool GameFlag(const uint8_t* saveData, int32_t flag) {
    const uint64_t word = ReadAt<uint64_t>(saveData, retail::kSaveDataFlags + sizeof(uint64_t) * (flag >> 6));
    return ((word >> (flag & 63)) & 1) != 0;
}

// List index of the valid save list entry for a slot, or -1.
int32_t FindSaveEntry(const uint8_t* saveList, int32_t slot) {
    const uint8_t* begin = ReadAt<const uint8_t*>(saveList, retail::kSaveListBegin);
    const uint8_t* end = ReadAt<const uint8_t*>(saveList, retail::kSaveListEnd);
    const size_t count = begin && end > begin ? static_cast<size_t>(end - begin) / retail::kSaveEntrySize : 0;
    for (size_t index = 0; index < count; ++index) {
        const uint8_t* entry = begin + retail::kSaveEntrySize * index;
        if (ReadAt<int32_t>(entry, retail::kSaveEntrySlot) == slot &&
            ReadAt<int32_t>(entry, retail::kSaveEntryValid) != 0) {
            return static_cast<int32_t>(index);
        }
    }
    return -1;
}

void* SaveEntryInfo(const uint8_t* saveList, int32_t index) {
    const uint8_t* begin = ReadAt<const uint8_t*>(saveList, retail::kSaveListBegin);
    return ReadAt<void*>(begin + retail::kSaveEntrySize * index, retail::kSaveEntryInfo);
}

void AssignSaveInfo(uint8_t* info, void* source) {
    const auto assign = (*reinterpret_cast<SaveInfoAssignFn**>(info))[retail::kSaveInfoAssignSlot];
    assign(info, source);
}

// =============================================================
// STORY STATE
// =============================================================
// Id of the first running event startLoadGame branches on, or 0.
uint64_t RunningLoadBranchEvent(const uint8_t* saveData) {
    const uint8_t* head = ReadAt<const uint8_t*>(saveData, retail::kSaveDataEvents);
    if (!head) {
        return 0;
    }
    for (const uint8_t* node = ReadAt<const uint8_t*>(head, retail::kEventNodeNext); node && node != head;
         node = ReadAt<const uint8_t*>(node, retail::kEventNodeNext)) {
        const uint8_t* status = ReadAt<const uint8_t*>(node, retail::kEventNodeStatus);
        const auto slot = status ? ReadAt<const uint8_t* const*>(status, retail::kEventStatusData) : nullptr;
        const uint8_t* data = slot ? *slot : nullptr;
        if (!data) {
            continue;
        }
        const uint64_t id = ReadAt<uint64_t>(data, 0);
        for (const uint64_t branchEvent : retail::kLoadBranchEvents) {
            if (id == branchEvent) {
                return id;
            }
        }
    }
    return 0;
}

// Why a load of a quick save made now would not resume the same day, or
// empty. Flags and events are logged by id only, their names spoil the story.
std::string QuickSaveBlocker(const uint8_t* saveData) {
    if (!GameFlag(saveData, retail::kFlagStoryStarted)) {
        return "flag " + std::to_string(retail::kFlagStoryStarted) + " is clear";
    }
    for (const int32_t flag : {retail::kFlagLateNight, retail::kFlagComingHomeMorning, retail::kFlagPlayerDead}) {
        if (GameFlag(saveData, flag)) {
            return "flag " + std::to_string(flag) + " is set";
        }
    }
    if (const uint64_t event = RunningLoadBranchEvent(saveData)) {
        return "event " + std::to_string(event) + " is running";
    }
    return {};
}

// =============================================================
// PLAYER BODIES
// =============================================================
// These run inside the engine's map setup and contact handling, where the map
// may still be building, so the reads run under __try. The functions that
// hold __try hold no C++ objects.

// 1 when the body is one of the player's collision bodies, 0 when it is not,
// -1 when the player's bodies are not there yet.
int PlayerBodyRaw(const uint8_t* body) {
    const uint8_t* manager = ReadGlobal<uint8_t*>(retail::kGameManager);
    const uint8_t* owner = manager ? ReadAt<const uint8_t*>(manager, retail::kManagerMapOwner) : nullptr;
    const uint8_t* mapInfo = owner ? ReadAt<const uint8_t*>(owner, retail::kMapOwnerMapInfo) : nullptr;
    const uint8_t* unit = mapInfo ? ReadAt<const uint8_t*>(mapInfo, retail::kMapInfoPlayerUnit) : nullptr;
    uint8_t* entity = unit ? ReadAt<uint8_t*>(unit, retail::kUnitEntity) : nullptr;
    const uint8_t* component = entity ? g_FindCollisionComponent(entity) : nullptr;
    if (!component) {
        return -1;
    }
    const auto begin = ReadAt<const uint8_t* const*>(component, retail::kCollisionBodiesBegin);
    const auto end = ReadAt<const uint8_t* const*>(component, retail::kCollisionBodiesEnd);
    if (!begin || end <= begin) {
        return -1;
    }
    return std::find(begin, end, body) != end ? 1 : 0;
}

// PlayerBodyRaw, or 2 when it faulted.
int GuardedPlayerBody(const uint8_t* body) {
    __try {
        return PlayerBodyRaw(body);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 2;
    }
}

// The body setupScript's walk asks about with this conditions check. The walk
// goes once through its own body's contact list, one check per node, and
// reads each next node after the previous node's script ran, so this does
// too.
const uint8_t* NextSetupWalkBodyRaw(SetupWalk& walk) {
    if (!walk.started) {
        walk.started = true;
        uint8_t* entity = ReadAt<uint8_t*>(walk.mapEvent, retail::kComponentEntity);
        const uint8_t* component = entity ? g_FindCollisionComponent(entity) : nullptr;
        const auto bodies = component ? ReadAt<const uint8_t* const*>(component, retail::kCollisionBodiesBegin) : nullptr;
        const uint8_t* self = bodies ? *bodies : nullptr;
        walk.head = self ? ReadAt<const uint8_t*>(self, retail::kBodyContacts) : nullptr;
        walk.node = walk.head ? ReadAt<const uint8_t*>(walk.head, retail::kContactNodeNext) : nullptr;
    } else if (walk.node && walk.node != walk.head) {
        walk.node = ReadAt<const uint8_t*>(walk.node, retail::kContactNodeNext);
    }
    if (!walk.node || walk.node == walk.head) {
        return nullptr;
    }
    return ReadAt<const uint8_t*>(walk.node, retail::kContactNodeBody);
}

bool GuardedSetupWalkBody(SetupWalk& walk, const uint8_t*& body) {
    __try {
        body = NextSetupWalkBodyRaw(walk);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void TurnGuardOffAfterFault(const char* where) {
    if (!g_GuardFaulted.exchange(true)) {
        g_GuardActive = false;
        Log(std::string("Quick load contact window faulted reading ") + where +
            ", map events are no longer held back until restart.");
    }
}

const uint8_t* CurrentSaveData() {
    const uint8_t* manager = ReadGlobal<const uint8_t*>(retail::kGameManager);
    return manager ? ReadAt<const uint8_t*>(manager, retail::kManagerSaveData) : nullptr;
}

// Copies a map event's id and condition flags. Faults are the caller's.
bool ReadHeldBackEventRaw(const uint8_t* mapEvent, HeldBackEvent& event) {
    const uint8_t* data = mapEvent ? ReadAt<const uint8_t*>(mapEvent, retail::kMapEventData) : nullptr;
    const uint8_t* const* holder = data ? ReadAt<const uint8_t* const*>(data, retail::kMapEventDataRecord) : nullptr;
    const uint8_t* record = holder ? *holder : nullptr;
    if (!record) {
        return false;
    }
    event.id = ReadAt<uint64_t>(record, retail::kEventRecordId);
    event.requiredCount =
        std::min<uint64_t>(ReadAt<uint64_t>(record, retail::kEventRecordRequiredCount), retail::kEventRecordFlagSlots);
    event.forbiddenCount =
        std::min<uint64_t>(ReadAt<uint64_t>(record, retail::kEventRecordForbiddenCount), retail::kEventRecordFlagSlots);
    for (uint64_t index = 0; index < event.requiredCount; ++index) {
        event.required[index] = ReadAt<uint64_t>(record, retail::kEventRecordRequired + sizeof(uint64_t) * index);
    }
    for (uint64_t index = 0; index < event.forbiddenCount; ++index) {
        event.forbidden[index] = ReadAt<uint64_t>(record, retail::kEventRecordForbidden + sizeof(uint64_t) * index);
    }
    return true;
}

bool GuardedHeldBackEvent(const uint8_t* mapEvent, HeldBackEvent& event) {
    __try {
        return ReadHeldBackEventRaw(mapEvent, event);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// What eventConditions answers for the copied flags.
bool EventReadyRaw(const uint8_t* saveData, const HeldBackEvent& event) {
    for (uint64_t index = 0; index < event.requiredCount; ++index) {
        if (event.required[index] != 0 && !GameFlag(saveData, static_cast<int32_t>(event.required[index]))) {
            return false;
        }
    }
    for (uint64_t index = 0; index < event.forbiddenCount; ++index) {
        if (event.forbidden[index] != 0 && GameFlag(saveData, static_cast<int32_t>(event.forbidden[index]))) {
            return false;
        }
    }
    return true;
}

// False also when there is no save or a flag id reads out of bounds.
bool EventReady(const uint8_t* saveData, const HeldBackEvent& event) {
    if (!saveData) {
        return false;
    }
    __try {
        return EventReadyRaw(saveData, event);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Notes a map event the window held back, once per event and source.
void RecordHeldBack(const uint8_t* mapEvent, const char* source) {
    HeldBackEvent event;
    if (GuardedHeldBackEvent(mapEvent, event)) {
        event.readable = true;
        event.readyThen = EventReady(CurrentSaveData(), event);
    } else {
        event = HeldBackEvent{};
    }
    event.source = source;
    std::lock_guard<std::mutex> lock(g_DiagnosticsMutex);
    for (HeldBackEvent& held : g_HeldBackEvents) {
        if (held.readable == event.readable && held.id == event.id && std::strcmp(held.source, source) == 0) {
            ++held.count;
            return;
        }
    }
    if (g_HeldBackEvents.size() < kMaxLoggedIds) {
        event.count = 1;
        g_HeldBackEvents.push_back(event);
    }
}

// Whether a map event must ignore this body's contact: the window is open and
// the body is the player's.
bool HoldBackContact(const uint8_t* body, const uint8_t* mapEvent, const char* source) {
    if (!g_GuardActive || g_GuardFaulted || !body) {
        return false;
    }
    switch (GuardedPlayerBody(body)) {
        case 1:
            ++g_HeldBackContacts;
            RecordHeldBack(mapEvent, source);
            return true;
        case -1:
            ++g_UnresolvedContacts;
            return false;
        case 2:
            TurnGuardOffAfterFault("the player's bodies");
            return false;
        default:
            return false;
    }
}

// =============================================================
// SAVE TASKS
// =============================================================
// Pushes a new top-level task on its own layer of the UI task root, which
// suspends the field until the task ends.
bool AttachToRoot(uint8_t* task) {
    void* root = ReadGlobal<void*>(retail::kUiTaskRoot);
    if (!root) {
        return false;
    }
    AttachTask(root, task, retail::kAttachModePushLayer);
    return true;
}

// Builds a task from a freshly allocated setting. The task copies the setting
// reference, so ours is released once the ctor returns.
uint8_t* CreateTask(TaskCtorFn ctor, size_t taskSize, uint8_t* setting) {
    uint8_t* task = AllocateZeroed(taskSize);
    if (!task) {
        Release(setting, retail::kSettingRefCount);
        return nullptr;
    }
    uint8_t* settingRef = setting;
    task = ctor(task, &settingRef);
    Release(setting, retail::kSettingRefCount);
    return task;
}

void StartWatch(TaskWatch& watch) {
    watch = TaskWatch{true, false, 0};
}

// Advances a launched task's watch. Returns true on the frame it finishes.
bool WatchFinished(TaskWatch& watch, bool idle) {
    if (!watch.active) {
        return false;
    }
    watch.sawBusy = watch.sawBusy || !idle;
    ++watch.frames;
    if ((watch.sawBusy && idle) || (!watch.sawBusy && watch.frames > kLaunchTimeoutFrames)) {
        watch.active = false;
        return true;
    }
    return false;
}

// Save group of the running game when a quick save can start right now, or
// -1 when something else owns the save system or the player.
int32_t QuickSaveGroup() {
    if (g_SaveWatch.active || g_DeleteWatch.active || g_PendingDeleteSlot != 0) {
        Log("Quick save ignored, a quick save or delete is still pending.");
        return -1;
    }
    if (!SaveSystemIdle()) {
        Log("Quick save ignored, the game is already saving or loading.");
        return -1;
    }
    const uint8_t* manager = ReadGlobal<uint8_t*>(retail::kGameManager);
    if (!manager || !PlayerInField()) {
        Log("Quick save ignored, the player does not have control.");
        return -1;
    }
    const int32_t group = ReadAt<int32_t>(manager, retail::kManagerGroup);
    if (group < 0 || group >= retail::kGroupCount) {
        Log("Quick save ignored, unexpected save group " + std::to_string(group) + ".");
        return -1;
    }
    const uint8_t* saveData = ReadAt<const uint8_t*>(manager, retail::kManagerSaveData);
    const std::string blocker = saveData ? QuickSaveBlocker(saveData) : "there is no save data";
    if (!blocker.empty()) {
        Log("Quick save refused, " + blocker + ": a load would not resume this day.");
        host::ShowToast(kRefusedToasts[CurrentLanguage()]);
        return -1;
    }
    return group;
}

void TryQuickSave() {
    const int32_t group = QuickSaveGroup();
    if (group < 0) {
        return;
    }
    const int32_t slot = QuickSlotForGroup(group);
    EnsureSlotCapacity(slot);

    uint8_t* setting = AllocateZeroed(retail::kAutoSaveProcessSettingSize);
    if (!setting) {
        return;
    }
    WriteAt<int32_t>(setting, retail::kSettingRefCount, 0);
    WriteAt<uintptr_t>(setting, 0, g_ModuleBase + retail::kAutoSaveProcessSettingVtable);
    AddRef(setting, retail::kSettingRefCount);

    uint8_t* task = CreateTask(g_AutoSaveProcessCtor, retail::kAutoSaveTaskSize, setting);
    if (!task) {
        Log("Quick save failed, could not create the save task.");
        return;
    }
    g_SaveTask = task;
    g_SaveSlot = slot;
    g_SaveWritten = false;
    StartWatch(g_SaveWatch);
    if (!AttachToRoot(task)) {
        Log("Quick save failed, no UI task root.");
        g_SaveTask = nullptr;
        g_SaveWatch.active = false;
        return;
    }
    Log("Quick save started, slot " + std::to_string(slot) + ".");
}

void LaunchDelete(int32_t slot) {
    uint8_t* setting = AllocateZeroed(retail::kAutoDeleteSettingSize);
    if (!setting) {
        return;
    }
    g_SaveSettingBaseCtor(setting);
    WriteAt<uintptr_t>(setting, 0, g_ModuleBase + retail::kAutoDeleteSettingVtable);
    if (const char* title = LogText(retail::kSaveTitleLogId)) {
        AssignText(setting + retail::kSettingTitle, title);
    }
    WriteAt<int32_t>(setting, retail::kSettingSlot, slot);
    AddRef(setting, retail::kSettingRefCount);

    uint8_t* task = CreateTask(g_AutoDeleteCtor, retail::kAutoDeleteTaskSize, setting);
    if (!task) {
        Log("Quick save delete failed, could not create the delete task.");
        return;
    }
    g_DeleteSlot = slot;
    StartWatch(g_DeleteWatch);
    if (!AttachToRoot(task)) {
        Log("Quick save delete failed, no UI task root.");
        g_DeleteWatch.active = false;
        return;
    }
    Log("Deleting loaded quick save, slot " + std::to_string(slot) + ".");
}

// =============================================================
// HOTKEY PROMPT
// =============================================================
// The loader reports the answer from its frame tick, once the popup is gone.
void OnQuickSaveAnswer(bool yes, void*) {
    g_PromptOpen = false;
    if (yes) {
        g_SaveRequested = true;
        g_SaveRequestFrames = 0;
        Log("Quick save confirmed.");
    }
}

// Opens the loader's Yes/No popup: the engine's own, the one the load menu
// uses to confirm a load, with the game's button labels.
void OpenQuickSavePrompt() {
    Log("Quick save hotkey pressed.");
    if (g_PromptOpen || g_SaveRequested) {
        Log("Quick save ignored, the prompt is already open or a save is queued.");
        return;
    }
    if (!PlayerInField()) {
        Log("Quick save ignored, the player is not in the field.");
        return;
    }
    if (QuickSaveGroup() < 0) {
        return;
    }
    // The loader logs why a popup could not open.
    if (!host::ShowYesNo(kQuickSavePrompts[CurrentLanguage()], &OnQuickSaveAnswer, nullptr)) {
        return;
    }
    g_PromptOpen = true;
    Log("Quick save prompt opened.");
}

// The popup closes over a few frames, so a confirmed save waits until the
// player has control again.
void TickPrompt() {
    if (g_SaveRequested) {
        if (SaveSystemIdle() && PlayerInField()) {
            g_SaveRequested = false;
            TryQuickSave();
        } else if (++g_SaveRequestFrames > kLaunchTimeoutFrames) {
            g_SaveRequested = false;
            Log("Quick save dropped, the player never regained control.");
        }
    }
}

// A written quick save ends the session. The title routine destroys every
// layer of the UI root, so it waits until the save task's layer is gone and
// the field has the player again.
void TickReturnToTitle() {
    if (!g_TitleRequested) {
        return;
    }
    if (PlayerInField()) {
        g_TitleRequested = false;
        Log("Returning to the title screen.");
        ReturnToTitle();
    } else if (++g_TitleRequestFrames > kLaunchTimeoutFrames) {
        g_TitleRequested = false;
        Log("Return to title dropped, the player never regained control.");
    }
}

std::string IdList(const uint64_t* ids, uint64_t count) {
    std::string text;
    for (uint64_t index = 0; index < count; ++index) {
        if (ids[index] != 0) {
            text += (text.empty() ? "" : ", ") + std::to_string(ids[index]);
        }
    }
    return text.empty() ? "none" : text;
}

void LogKeptFlagIds() {
    std::vector<int32_t> kept;
    {
        std::lock_guard<std::mutex> lock(g_DiagnosticsMutex);
        kept.swap(g_KeptFlagIds);
    }
    std::string text;
    for (const int32_t flag : kept) {
        text += (text.empty() ? "" : ", ") + std::to_string(flag);
    }
    if (!text.empty()) {
        Log("Quick load kept these set daily flags: " + text + ".");
    }
}

// Logs each map event the window held back, whether its conditions held then,
// and whether they hold now, when touching it again starts it.
void LogHeldBackEvents() {
    std::vector<HeldBackEvent> events;
    {
        std::lock_guard<std::mutex> lock(g_DiagnosticsMutex);
        events.swap(g_HeldBackEvents);
    }
    const uint8_t* saveData = CurrentSaveData();
    for (const HeldBackEvent& event : events) {
        const std::string times = std::to_string(event.count) + (event.count == 1 ? " time" : " times");
        if (!event.readable) {
            Log(std::string("Held back a map event whose record could not be read, on ") + event.source + ", " +
                times + ".");
            continue;
        }
        const bool readyNow = EventReady(saveData, event);
        Log("Held back map event " + std::to_string(event.id) + " on " + event.source + ", " + times + ". " +
            (event.readyThen ? "Its conditions held then" : "Its conditions did not hold then, so it would not have started") +
            (readyNow ? "; they hold now, so touching it again starts it." : "; they do not hold now.") +
            " Needs set: " + IdList(event.required, event.requiredCount) +
            ". Needs clear: " + IdList(event.forbidden, event.forbiddenCount) + ".");
    }
}

// Reports the daily flags a quick load kept, closes the contact window once
// the player has had control for a moment, then reports what it held back.
void TickQuickLoad() {
    if (const int skipped = g_SkippedDailyClears.exchange(0)) {
        Log("Quick load kept the day: skipped " + std::to_string(skipped) + " daily flag clears, " +
            std::to_string(g_KeptDailyFlags.exchange(0)) + " of those flags were set.");
        LogKeptFlagIds();
    }
    if (g_GuardActive) {
        ++g_GuardFrames;
        g_GuardControlFrames = PlayerInField() ? g_GuardControlFrames + 1 : 0;
        if (g_GuardControlFrames < kGuardControlFrames && g_GuardFrames < kGuardMaxFrames) {
            return;
        }
        g_GuardActive = false;
        std::string text = "Quick load contact window closed after " + std::to_string(g_GuardFrames) + " frames, " +
                           std::to_string(g_HeldBackContacts.exchange(0)) + " player contacts with map events held back";
        if (const int unresolved = g_UnresolvedContacts.exchange(0)) {
            text += ", " + std::to_string(unresolved) + " contacts came before the player's bodies were found";
        }
        Log(text + ".");
    }
    LogHeldBackEvents();
}

void TickQuickSave() {
    TickQuickLoad();
    const bool idle = SaveSystemIdle();
    if (WatchFinished(g_SaveWatch, idle)) {
        g_SaveTask = nullptr;
        if (g_SaveWritten) {
            g_TitleRequested = true;
            g_TitleRequestFrames = 0;
        } else {
            Log("Quick save finished without writing, staying in the game.");
        }
        g_SaveWritten = false;
    }
    WatchFinished(g_DeleteWatch, idle);
    TickReturnToTitle();

    if (g_PendingDeleteSlot != 0 && !g_SaveWatch.active && !g_DeleteWatch.active && idle && PlayerInField()) {
        const int32_t slot = g_PendingDeleteSlot;
        g_PendingDeleteSlot = 0;
        LaunchDelete(slot);
    }

    TickPrompt();

    if (host::Pressed(host::Binding(g_KeySetting)) || host::Pressed(host::Binding(g_ButtonSetting))) {
        OpenQuickSavePrompt();
    }
}

// =============================================================
// DETOURS: SAVE
// =============================================================
uint64_t __fastcall Detour_AutoSaveBuildSetting(uint8_t** capture, void* data) {
    const uint64_t result = g_OriginalAutoSaveBuildSetting(capture, data);
    uint8_t* task = *capture;
    if (task && task == g_SaveTask) {
        // The worker reads the slot when it runs, after this lambda returns.
        if (uint8_t* setting = ReadAt<uint8_t*>(task, retail::kTaskSaveSetting)) {
            WriteAt<int32_t>(setting, retail::kSettingSlot, g_SaveSlot);
        }
    }
    return result;
}

uint64_t __fastcall Detour_AutoSaveSucceeded(uint8_t** capture) {
    uint8_t* manager = ReadGlobal<uint8_t*>(retail::kGameManager);
    if (*capture != g_SaveTask || !manager) {
        return g_OriginalAutoSaveSucceeded(capture);
    }

    // The quick slot is outside the rotation, so the newest autosave, its
    // rotation index and its time must stay as they were.
    uint8_t* system = ReadAt<uint8_t*>(manager, retail::kManagerSystemData);
    const int32_t group = ReadAt<int32_t>(manager, retail::kManagerGroup);
    const int32_t rotation = ReadAt<int32_t>(manager, retail::kManagerRotation);
    const bool trackSystem = system && group >= 0 && group < retail::kGroupCount;
    const size_t newestOffset = retail::kSystemNewestRotation + sizeof(int32_t) * group;
    int32_t systemGroup = 0;
    int32_t systemNewest = 0;
    int32_t systemTime = 0;
    if (trackSystem) {
        systemGroup = ReadAt<int32_t>(system, retail::kSystemGroup);
        systemNewest = ReadAt<int32_t>(system, newestOffset);
        systemTime = ReadAt<int32_t>(system, retail::kSystemSaveTime);
    }
    // The original advances the rotation by one before saving .systemsave;
    // step it back first so the value it writes is the current one.
    if (rotation >= 0 && rotation < retail::kRotationCount) {
        WriteAt<int32_t>(
            manager, retail::kManagerRotation, (rotation + retail::kRotationCount - 1) % retail::kRotationCount
        );
    }

    const uint64_t result = g_OriginalAutoSaveSucceeded(capture);

    WriteAt<int32_t>(manager, retail::kManagerRotation, rotation);
    if (trackSystem) {
        WriteAt<int32_t>(system, retail::kSystemGroup, systemGroup);
        WriteAt<int32_t>(system, newestOffset, systemNewest);
        WriteAt<int32_t>(system, retail::kSystemSaveTime, systemTime);
    }
    g_SaveWritten = true;
    Log("Quick save written, slot " + std::to_string(g_SaveSlot) + ".");
    return result;
}

// =============================================================
// DETOURS: LOAD MENU
// =============================================================
uint8_t* LoadMenuRow(uint8_t* state, int row) {
    return state + retail::kLoadMenuRows + retail::kLoadMenuRowSize * row;
}

void ClearQuickRow(uint8_t* row) {
    WriteAt<int64_t>(row, retail::kRowRotation, -1);
    uint8_t* info = ReadAt<uint8_t*>(row, retail::kRowInfo);
    WriteAt<uint8_t*>(row, retail::kRowInfo, nullptr);
    Release(info, retail::kSaveInfoRefCount);
}

void FillQuickRow(uint8_t* state, uint8_t* saveList) {
    const uint8_t* manager = ReadGlobal<uint8_t*>(retail::kGameManager);
    uint8_t* row = LoadMenuRow(state, kQuickRow);
    const int32_t group = ReadAt<int32_t>(state, retail::kLoadMenuGroup);
    if (!manager || group < 0 || group >= retail::kGroupCount) {
        ClearQuickRow(row);
        return;
    }
    const int32_t index = FindSaveEntry(saveList, QuickSlotForGroup(group));
    if (index < 0) {
        ClearQuickRow(row);
        return;
    }

    // Loading this row must leave the rotation where the newest autosave has
    // it, so the row carries the current newest rotation index.
    const uint8_t* system = ReadAt<const uint8_t*>(manager, retail::kManagerSystemData);
    const int32_t newest =
        system ? ReadAt<int32_t>(system, retail::kSystemNewestRotation + sizeof(int32_t) * group) : 0;
    WriteAt<int32_t>(row, retail::kRowRotation, NormalizeRotation(newest));
    WriteAt<int32_t>(row, retail::kRowListIndex, index);

    uint8_t* info = ReadAt<uint8_t*>(row, retail::kRowInfo);
    if (!info) {
        info = AllocateZeroed(retail::kSaveInfoSize);
        if (!info) {
            ClearQuickRow(row);
            return;
        }
        info = g_SaveInfoCtor(info);
        AddRef(info, retail::kSaveInfoRefCount);
        WriteAt<uint8_t*>(row, retail::kRowInfo, info);
    }
    AssignSaveInfo(info, SaveEntryInfo(saveList, index));
}

// A carousel opens on the quick row when its group has a quick save. This
// runs from the save util tick, outside the list's own refresh, which
// setCursor re-enters. The list and its sixth item only exist a few frames
// after the state does, so it retries until both are there.
constexpr int kCursorTimeoutFrames = 120;

void TryQuickCursor(uint8_t* state) {
    if (state != g_CursorState) {
        return;
    }
    if (++g_CursorFrames > kCursorTimeoutFrames) {
        g_CursorState = nullptr;
        return;
    }
    if (!g_CursorList) {
        return;
    }
    if (!ReadAt<uint8_t*>(LoadMenuRow(state, kQuickRow), retail::kRowInfo)) {
        // The rows are filled before the list is first drawn, so this group
        // has no quick save.
        g_CursorState = nullptr;
        return;
    }
    void* list = ReadAt<void*>(g_CursorList, retail::kListComponent);
    const auto begin = ReadAt<uintptr_t>(list, retail::kListItemsBegin);
    const auto end = ReadAt<uintptr_t>(list, retail::kListItemsEnd);
    if (end <= begin || (end - begin) / sizeof(void*) <= static_cast<size_t>(kQuickRow)) {
        return;
    }
    g_SetListCursor(list, kQuickRow, -1);
    g_CursorState = nullptr;
}

uint8_t __fastcall Detour_LoadMenuFillRows(uint8_t* state, uint8_t* saveList) {
    const uint8_t result = g_OriginalLoadMenuFillRows(state, saveList);
    if (g_MenuPatched) {
        FillQuickRow(state, saveList);
        TryQuickCursor(state);
    }
    return result;
}

void __fastcall Detour_CarouselInitialize(uint8_t* state) {
    g_OriginalCarouselInitialize(state);
    g_CursorState = state;
    g_CursorList = nullptr;
    g_CursorFrames = 0;
}

// The play time text is the provider that knows both the carousel state and
// its list, so the list is picked up there.
void __fastcall Detour_CarouselPlayTimeText(uint8_t* functor, void* outText) {
    if (g_CursorState && ReadAt<const uint8_t*>(functor, retail::kCarouselFunctorState) == g_CursorState) {
        g_CursorList = ReadAt<uint8_t*>(functor, retail::kCarouselFunctorList);
    }
    g_OriginalCarouselPlayTimeText(functor, outText);
}

// A group card loads its list index on Yes and shows its card info. The
// listener rebuilds both from the newest autosave every tick, so a group
// with a quick save gets them pointed at the quick save right after. A group
// with a quick save but no autosave has no card info to redirect; its quick
// save stays reachable from the carousel row.
void RedirectQuickCards(uint8_t* task, const uint8_t* saveList) {
    g_QuickCardTask = task;
    for (int32_t group = 0; group < retail::kGroupCount; ++group) {
        uint8_t* card = task + retail::kCardRows + retail::kCardRowSize * group;
        uint8_t* info = ReadAt<uint8_t*>(card, retail::kCardRowInfo);
        const int32_t index = info ? FindSaveEntry(saveList, QuickSlotForGroup(group)) : -1;
        g_QuickCards[group] = index >= 0;
        if (index >= 0) {
            WriteAt<int32_t>(card, retail::kCardRowListIndex, index);
            AssignSaveInfo(info, SaveEntryInfo(saveList, index));
        }
    }
}

uint8_t __fastcall Detour_LoadCardsListener(uint8_t* task, uint8_t* saveList) {
    const uint8_t done = g_OriginalLoadCardsListener(task, saveList);
    if (done) {
        g_QuickCardTask = nullptr;
    } else if (g_MenuPatched) {
        RedirectQuickCards(task, saveList);
    }
    return done;
}

// The mode label of a redirected card gets the quick save label after it.
void __fastcall Detour_CardModeText(uint8_t* functor, void* outText) {
    g_OriginalCardModeText(functor, outText);
    const int32_t group = ReadAt<int32_t>(functor, retail::kCardFunctorGroup);
    if (ReadAt<const uint8_t*>(functor, retail::kCardFunctorTask) != g_QuickCardTask || group < 0 ||
        group >= retail::kGroupCount || !g_QuickCards[group]) {
        return;
    }
    const char* label = kQuickSaveLabels[CurrentLanguage()];
    if (ReadAt<size_t>(outText, retail::kCStringLength) != 0) {
        g_CStringAppend(outText, " / ");
        g_CStringAppend(outText, label);
    } else {
        AssignText(outText, label);
    }
}

int __fastcall Detour_LoadMenuHeaderText(uint8_t* functor, void* outText) {
    if (g_MenuPatched) {
        const uint8_t* state = ReadAt<const uint8_t*>(functor, retail::kHeaderFunctorState);
        if (g_GetListCursor(ReadAt<void*>(state, retail::kListComponent)) == kQuickRow) {
            AssignText(outText, kQuickSaveLabels[CurrentLanguage()]);
            return 0;
        }
    }
    return g_OriginalLoadMenuHeaderText(functor, outText);
}

// =============================================================
// DETOURS: LOAD & DELETE
// =============================================================
// Slot a finished load read. AutoLoad requests carry it; the load menu
// leaves it at 0 and loads the selected entry of the util's save list.
int32_t LoadedSlot(const uint8_t* util) {
    const int32_t slot = ReadAt<int32_t>(util, retail::kSaveUtilSlot);
    if (slot != 0) {
        return slot;
    }
    const uint8_t* list = util + retail::kSaveUtilList;
    const uint8_t* begin = ReadAt<const uint8_t*>(list, retail::kSaveListBegin);
    const uint8_t* end = ReadAt<const uint8_t*>(list, retail::kSaveListEnd);
    const int32_t selected = ReadAt<int32_t>(list, retail::kSaveListSelected);
    const size_t count = begin && end > begin ? static_cast<size_t>(end - begin) / retail::kSaveEntrySize : 0;
    if (selected < 0 || static_cast<size_t>(selected) >= count) {
        return 0;
    }
    return ReadAt<int32_t>(begin + retail::kSaveEntrySize * selected, retail::kSaveEntrySlot);
}

uint8_t __fastcall Detour_SaveUtilAcknowledge(uint8_t* util) {
    if (ReadAt<int32_t>(util, retail::kSaveUtilState) == retail::kSaveUtilStateDone) {
        const int32_t operation = ReadAt<int32_t>(util, retail::kSaveUtilOperation);
        const int32_t slot = ReadAt<int32_t>(util, retail::kSaveUtilSlot);
        const int32_t status = ReadAt<int32_t>(util, retail::kSaveUtilStatus);
        const int32_t loadedSlot = operation == retail::kSaveUtilOperationLoad ? LoadedSlot(util) : 0;
        if (operation == retail::kSaveUtilOperationLoad) {
            g_ResumeQuickLoad = IsQuickSlot(loadedSlot) && status == retail::kSaveUtilStatusOk;
        }
        if (IsQuickSlot(loadedSlot)) {
            if (status == retail::kSaveUtilStatusOk) {
                // Deleted later, once the loaded game is running and idle.
                g_PendingDeleteSlot = loadedSlot;
                Log("Quick save loaded, slot " + std::to_string(loadedSlot) + " will be deleted.");
            } else {
                Log("Quick save load failed with status " + std::to_string(status) + ", keeping slot " +
                    std::to_string(loadedSlot) + ".");
            }
        } else if (g_DeleteSlot != 0 && slot == g_DeleteSlot && operation != retail::kSaveUtilOperationLoad) {
            Log("Quick save delete finished with status " + std::to_string(status) + ".");
            g_DeleteSlot = 0;
        }
    }
    return g_OriginalSaveUtilAcknowledge(util);
}

// =============================================================
// DETOURS: RESUME AFTER A QUICK LOAD
// =============================================================
// The load task acknowledges the save util, which resets it, before its
// success callback starts the game, so the request is gone by then. The
// acknowledge detour latched what it was.
bool QuickLoadStarting() {
    return g_ResumeQuickLoad;
}

bool CalledFrom(void* returnAddress, uintptr_t callSite) {
    return reinterpret_cast<uintptr_t>(returnAddress) == g_ModuleBase + callSite + retail::kCallLength;
}

// The daily flag clears of a quick load's start are skipped. Only the normal
// branch of startLoadGame reaches them, the one that resumes in the field,
// so the first one also opens the contact window.
uint64_t __fastcall Detour_ClearGameFlag(uint8_t* saveData, int32_t flag) {
    if (!CalledFrom(_ReturnAddress(), retail::kDailyFlagClearCall) || !QuickLoadStarting()) {
        return g_OriginalClearGameFlag(saveData, flag);
    }
    ++g_SkippedDailyClears;
    if (GameFlag(saveData, flag)) {
        ++g_KeptDailyFlags;
        std::lock_guard<std::mutex> lock(g_DiagnosticsMutex);
        if (g_KeptFlagIds.size() < kMaxLoggedIds) {
            g_KeptFlagIds.push_back(flag);
        }
    }
    if (!g_GuardActive && !g_GuardFaulted) {
        g_GuardFrames = 0;
        g_GuardControlFrames = 0;
        g_GuardActive = true;
    }
    return 0;
}

// While the window is open, each map event setup gets a walk of its own.
uint64_t __fastcall Detour_MapEventSetupScript(uint8_t* mapEvent) {
    if (!g_GuardActive) {
        return g_OriginalMapEventSetupScript(mapEvent);
    }
    const SetupWalk outer = g_SetupWalk;
    g_SetupWalk = SetupWalk{mapEvent};
    const uint64_t result = g_OriginalMapEventSetupScript(mapEvent);
    g_SetupWalk = outer;
    return result;
}

// A setup's check for a body that is the player's fails, so the setup does
// not start the event for them.
bool __fastcall Detour_EventConditions(
    uint8_t* saveData, void* conditions, uint64_t count, void* flagConditions, uint64_t flagCount
) {
    if (g_SetupWalk.mapEvent && g_GuardActive && CalledFrom(_ReturnAddress(), retail::kSetupScriptConditionsCall)) {
        const uint8_t* body = nullptr;
        if (!GuardedSetupWalkBody(g_SetupWalk, body)) {
            TurnGuardOffAfterFault("a map event's contacts");
        } else if (HoldBackContact(body, g_SetupWalk.mapEvent, "map entry")) {
            return false;
        }
    }
    return g_OriginalEventConditions(saveData, conditions, count, flagConditions, flagCount);
}

// A player's body that begins to touch a map event in the window does not
// start it. A false return is what failed conditions return.
uint64_t __fastcall Detour_MapEventContact(uint8_t** mapEvent, uint8_t* selfBody, uint8_t* const* otherBody) {
    if (otherBody && HoldBackContact(*otherBody, mapEvent ? *mapEvent : nullptr, "contact")) {
        return 0;
    }
    return g_OriginalMapEventContact(mapEvent, selfBody, otherBody);
}

// The engine spawns the player at the saved position, then this starts the
// wake-up script that moves them to a bed. After a quick load only the end of
// that script runs: game time starts again and the screen fades in. A morning
// that comes home from somewhere keeps its own script.
void __fastcall Detour_LoadEventStart() {
    const bool resume = g_ResumeQuickLoad;
    g_ResumeQuickLoad = false;
    const uint8_t* manager = ReadGlobal<uint8_t*>(retail::kGameManager);
    const uint8_t* saveData = manager ? ReadAt<const uint8_t*>(manager, retail::kManagerSaveData) : nullptr;
    void* root = ReadGlobal<void*>(retail::kUiTaskRoot);
    if (!resume || !saveData || !root || GameFlag(saveData, retail::kFlagComingHomeMorning)) {
        g_OriginalLoadEventStart();
        return;
    }

    g_SetGameFlag(retail::kFlagGameTimeStop, false);
    const FadeParam param{retail::kWakeFadeFrames, 0.0, retail::kFadeParamType, 0, 0.0, 1.0, 0};
    // An empty std::function<void()>: no target pointer in the last slot.
    alignas(16) uint8_t onEnd[64] = {};
    uint8_t* fadeTask = nullptr;
    g_ScreenFade(
        ReadAt<void*>(root, retail::kUiRootFadeManager), retail::kRenderStepFade, retail::kColorClearBlack, &param,
        onEnd, &fadeTask
    );
    Release(fadeTask, retail::kTaskRefCount);
    Log("Quick save resumed at the saved position.");
}

// =============================================================
// HOOK INITIALIZATION
// A site the build lacks, which only an optional patch may be, is skipped.
bool WriteCode(const retail::CodePatch& patch, const uint8_t* bytes) {
    if (!patch.address->Found()) {
        return true;
    }
    void* code = reinterpret_cast<void*>(g_ModuleBase + *patch.address);
    DWORD oldProtect = 0;
    if (!VirtualProtect(code, patch.length, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log(std::string("VirtualProtect failed for ") + patch.address->Name() + ".");
        return false;
    }
    std::memcpy(code, bytes, patch.length);
    VirtualProtect(code, patch.length, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), code, patch.length);
    return true;
}

void RestoreLoadMenuPatches(size_t count) {
    for (size_t index = 0; index < count; ++index) {
        WriteCode(retail::kLoadMenuPatches[index], retail::kLoadMenuPatches[index].original);
    }
}

// The sizes and counts only stay consistent as a set, so a failure part way
// puts every site back to retail.
bool ApplyLoadMenuPatches() {
    constexpr size_t kPatchCount = kLoadMenuPatchCount;
    for (size_t index = 0; index < kPatchCount; ++index) {
        if (!WriteCode(retail::kLoadMenuPatches[index], retail::kLoadMenuPatches[index].patched)) {
            RestoreLoadMenuPatches(index);
            return false;
        }
    }
    return true;
}

void ResolveFunctions() {
    g_AutoSaveProcessCtor = GameFunction<TaskCtorFn>(retail::kAutoSaveProcessCtor);
    g_AutoDeleteCtor = GameFunction<TaskCtorFn>(retail::kAutoDeleteCtor);
    g_SaveSettingBaseCtor = GameFunction<SettingBaseCtorFn>(retail::kSaveSettingBaseCtor);
    g_GetListCursor = GameFunction<GetListCursorFn>(retail::kGetListCursor);
    g_SaveInfoCtor = GameFunction<SaveInfoCtorFn>(retail::kSaveInfoCtor);
    g_SetGameFlag = GameFunction<SetGameFlagFn>(retail::kSetGameFlag);
    g_ScreenFade = GameFunction<ScreenFadeFn>(retail::kScreenFade);
    g_CStringAppend = GameFunction<CStringAppendFn>(retail::kCStringAppend);
    g_SetListCursor = GameFunction<SetListCursorFn>(retail::kSetListCursor);
    g_FindCollisionComponent = GameFunction<FindCollisionComponentFn>(retail::kFindCollisionComponent);
}

}  // namespace

bool Install() {
    // Quick save writes saves, so a build whose addresses were only searched
    // for never gets it: a wrong address there could damage a save.
    if (!CheckedBuild()) {
        Log("Quick save needs a checked game build.");
        return false;
    }
    // Every address and patch site is checked before anything is written,
    // so an updated game leaves this feature fully inert.
    if (!RequirementsMet(retail::kQuickSaveRequirements) || !MatchesPatchSites()) {
        return false;
    }
    ResolveFunctions();

    const bool created =
        g_Detours.Add(
            retail::kAutoSaveBuildSetting,
            reinterpret_cast<void*>(&Detour_AutoSaveBuildSetting),
            reinterpret_cast<void**>(&g_OriginalAutoSaveBuildSetting),
            "autoSaveBuildSetting"
        ) &&
        g_Detours.Add(
            retail::kAutoSaveSucceeded,
            reinterpret_cast<void*>(&Detour_AutoSaveSucceeded),
            reinterpret_cast<void**>(&g_OriginalAutoSaveSucceeded),
            "autoSaveSucceeded"
        ) &&
        g_Detours.Add(
            retail::kSaveUtilAcknowledge,
            reinterpret_cast<void*>(&Detour_SaveUtilAcknowledge),
            reinterpret_cast<void**>(&g_OriginalSaveUtilAcknowledge),
            "saveUtilAcknowledge"
        ) &&
        g_Detours.Add(
            retail::kLoadMenuFillRows,
            reinterpret_cast<void*>(&Detour_LoadMenuFillRows),
            reinterpret_cast<void**>(&g_OriginalLoadMenuFillRows),
            "loadMenuFillRows"
        ) &&
        g_Detours.Add(
            retail::kLoadMenuHeaderText,
            reinterpret_cast<void*>(&Detour_LoadMenuHeaderText),
            reinterpret_cast<void**>(&g_OriginalLoadMenuHeaderText),
            "loadMenuHeaderText"
        ) &&
        g_Detours.Add(
            retail::kLoadCardsListener,
            reinterpret_cast<void*>(&Detour_LoadCardsListener),
            reinterpret_cast<void**>(&g_OriginalLoadCardsListener),
            "loadCardsListener"
        ) &&
        g_Detours.Add(
            retail::kCardModeText,
            reinterpret_cast<void*>(&Detour_CardModeText),
            reinterpret_cast<void**>(&g_OriginalCardModeText),
            "cardModeText"
        ) &&
        g_Detours.Add(
            retail::kCarouselInitialize,
            reinterpret_cast<void*>(&Detour_CarouselInitialize),
            reinterpret_cast<void**>(&g_OriginalCarouselInitialize),
            "carouselInitialize"
        ) &&
        g_Detours.Add(
            retail::kCarouselPlayTimeText,
            reinterpret_cast<void*>(&Detour_CarouselPlayTimeText),
            reinterpret_cast<void**>(&g_OriginalCarouselPlayTimeText),
            "carouselPlayTimeText"
        ) &&
        g_Detours.Add(
            retail::kLoadEventStart,
            reinterpret_cast<void*>(&Detour_LoadEventStart),
            reinterpret_cast<void**>(&g_OriginalLoadEventStart),
            "loadEventStart"
        ) &&
        g_Detours.Add(
            retail::kClearGameFlag,
            reinterpret_cast<void*>(&Detour_ClearGameFlag),
            reinterpret_cast<void**>(&g_OriginalClearGameFlag),
            "clearGameFlag"
        ) &&
        g_Detours.Add(
            retail::kMapEventSetupScript,
            reinterpret_cast<void*>(&Detour_MapEventSetupScript),
            reinterpret_cast<void**>(&g_OriginalMapEventSetupScript),
            "mapEventSetupScript"
        ) &&
        g_Detours.Add(
            retail::kEventConditions,
            reinterpret_cast<void*>(&Detour_EventConditions),
            reinterpret_cast<void**>(&g_OriginalEventConditions),
            "eventConditions"
        ) &&
        g_Detours.Add(
            retail::kMapEventContact,
            reinterpret_cast<void*>(&Detour_MapEventContact),
            reinterpret_cast<void**>(&g_OriginalMapEventContact),
            "mapEventContact"
        );
    if (!created) {
        g_Detours.RemoveAll();
        return false;
    }

    // The load menu does not exist yet this early, so no state object was
    // built with the retail size.
    if (!ApplyLoadMenuPatches()) {
        g_Detours.RemoveAll();
        return false;
    }
    g_MenuPatched = true;
    g_KeySetting = host::AddBinding(
        "QuickSaveKeyboard", kKeyLabels, kSettingHelp, CANOPY_DEVICE_KEYBOARD_MOUSE, kDefaultKey
    );
    g_ButtonSetting = host::AddBinding(
        "QuickSaveController", kButtonLabels, kSettingHelp, CANOPY_DEVICE_CONTROLLER, CANOPY_NO_PAD
    );
    g_Installed = true;
    return true;
}

void Tick() {
    if (g_Installed) {
        TickQuickSave();
    }
}

void Uninstall() {
    if (!g_Installed) {
        return;
    }
    g_Installed = false;
    g_MenuPatched = false;
    RestoreLoadMenuPatches(kLoadMenuPatchCount);
    g_Detours.RemoveAll();
}

}  // namespace quick_save
