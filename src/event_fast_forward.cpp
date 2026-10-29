#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

#include "core.h"
#include "features.h"
#include "host.h"

// =============================================================
// EVENT FAST FORWARD DESIGN
// =============================================================
// Holding the fast forward key or button speeds up an event the game itself
// marks skippable, the same mark its own skip prompt checks. Events without
// it are left alone, so a first viewing of a scene the game wants watched is
// never rushed.
//
// Speed comes from the time manager's own fast forward, the one Lua
// TimeSetFastforwardFrame drives: each frame it multiplies the frame time by
// a magnification for a count of frames. The tick keeps one frame queued
// while the key is held, so the game resets the magnification itself a frame
// after release. A count the game queued is never shortened.
//
// Talk waits for input whatever the speed, so the tick also queues the talk
// advance command the talk window is waiting for, every other frame, as if
// the player pressed it.
//
// Events are tracked from their setup to their destructor. The skip mark is
// read through the event's status, guarded, since the status is released
// while the event winds down.
//
// =============================================================
// EVENT SKIP DESIGN
// =============================================================
// The game's own O + R2 chord, Cancel with the fast forward button, can run
// the skip the beta build had, as an opt-in toggle. Like fast forward it only
// starts in an event the game marks skippable.
//
// One press runs the event at the skip speed, through the same time manager
// fast forward as holding the button, with talk advanced on its own. The
// event stays on screen: the beta hid it behind a black fade and skipped
// drawing, which this leaves out.
//
// The skip never answers anything for the player. The beta stopped once a
// receiver asked for any command beyond the talk and event ones; so does
// this, so a choice, a popup or the field handing control back stops it and
// the player decides. It also stops when the event ends, when a non-skippable
// event takes over, when the toggle goes off and after a time limit.

namespace event_fast_forward {
namespace {

using namespace core;

using EventSetupFn = int64_t(__fastcall*)(uint8_t* event);
using EventDestroyFn = int64_t(__fastcall*)(uint8_t* event);
using PadPushCommandFn = void(__fastcall*)(uint8_t* padManager, int64_t command, const void* vector, int32_t flag);
using PadDatabaseFn = uint8_t*(__fastcall*)(void* registry);
using U64MapFindFn = void*(__fastcall*)(void* map, uint8_t** outNode, const uint64_t* key);
using ResolvePadFn = void*(__fastcall*)(uint8_t* padManager, const int64_t* const** outHolder, void* holder);

// Left Ctrl and RT, the keys the game's own talk fast forward uses.
constexpr int32_t kDefaultKey = 93;
constexpr int32_t kDefaultButton = 16;

constexpr double kSpeeds[] = {2.0, 4.0, 8.0};
constexpr int32_t kDefaultSpeed = 1;

constexpr CanopyText kSpeedChoices[] = {
    {"2倍", "2x", "2x", "2x", "2倍", "2배", "2倍"},
    {"4倍", "4x", "4x", "4x", "4倍", "4배", "4倍"},
    {"8倍", "8x", "8x", "8x", "8倍", "8배", "8倍"},
};
static_assert(sizeof(kSpeedChoices) / sizeof(kSpeedChoices[0]) == sizeof(kSpeeds) / sizeof(kSpeeds[0]), "speeds");

// Each skip frame steps this many frames: four times fast forward's fastest,
// under the beta's 132, which lets moving characters overshoot.
constexpr double kSkipSpeed = 32.0;
constexpr uint64_t kSkipTimeLimitMs = 30000;
// A queue longer than this is not a frame's queries.
constexpr size_t kMaxCommandQueries = 256;

constexpr CanopyText kKeyLabels = {
    "早送りキー", "Fast Forward Key", "Touche d'avance rapide", "Tecla de avance rápido", "快轉鍵", "빨리 감기 키",
    "快进键",
};

constexpr CanopyText kButtonLabels = {
    "早送りボタン", "Fast Forward Button", "Bouton d'avance rapide", "Botón de avance rápido", "快轉按鈕",
    "빨리 감기 버튼", "快进按钮",
};

constexpr CanopyText kBindingHelp = {
    "スキップできるイベント中に押し続けると早送りします。",
    "Hold it during an event that can be skipped to fast forward it.",
    "Maintenez-la pendant un événement passable pour l'accélérer.",
    "Mantenla pulsada durante un evento que se pueda saltar para acelerarlo.",
    "在可跳過的事件中按住即可快轉。",
    "건너뛸 수 있는 이벤트 중에 누르고 있으면 빨리 감기합니다.",
    "在可跳过的事件中按住即可快进。",
};

constexpr CanopyText kSpeedLabels = {
    "早送りの速さ", "Fast Forward Speed", "Vitesse d'avance rapide", "Velocidad de avance rápido", "快轉速度",
    "빨리 감기 속도", "快进速度",
};

constexpr CanopyText kSpeedHelp = {
    "イベントを早送りする速さです。",
    "How fast an event plays while fast forwarding.",
    "Vitesse de lecture d'un événement en avance rapide.",
    "Velocidad de un evento durante el avance rápido.",
    "快轉時事件播放的速度。",
    "빨리 감기 중 이벤트가 재생되는 속도입니다.",
    "快进时事件播放的速度。",
};

constexpr CanopyText kTextLabels = {
    "会話も早送り", "Advance Text", "Faire défiler le texte", "Avanzar el texto", "快轉對話", "대화도 넘기기",
    "快进对话",
};

constexpr CanopyText kTextHelp = {
    "早送り中は会話を自動で送ります。",
    "While fast forwarding, talk advances on its own.",
    "Pendant l'avance rapide, les dialogues défilent seuls.",
    "Durante el avance rápido, los diálogos avanzan solos.",
    "快轉時自動推進對話。",
    "빨리 감기 중에는 대화가 자동으로 넘어갑니다.",
    "快进时自动推进对话。",
};

constexpr CanopyText kSkipLabels = {
    "ベータ版イベントスキップ", "Beta Event Skip", "Passer les événements (bêta)", "Saltar eventos (beta)",
    "測試版事件跳過", "베타 이벤트 건너뛰기", "测试版事件跳过",
};

constexpr CanopyText kSkipHelp = {
    "スキップできるイベント中にキャンセルと早送りボタンを同時に押すと先へ飛ばします。選択肢やポップアップで止まり、操作を戻します。",
    "Press Cancel and the fast forward button together during an event that can be skipped to skip ahead. It stops "
    "at choices and popups and hands control back.",
    "Appuyez ensemble sur Annuler et le bouton d'avance rapide pendant un événement passable pour le passer. "
    "S'arrête aux choix et aux fenêtres et vous rend la main.",
    "Pulsa a la vez Cancelar y el botón de avance rápido durante un evento que se pueda saltar para adelantarlo. "
    "Se detiene en las elecciones y ventanas emergentes y te devuelve el control.",
    "在可跳過的事件中同時按下取消與快轉按鈕即可跳過。遇到選項或彈出視窗時會停下並交還操作。",
    "건너뛸 수 있는 이벤트 중에 취소와 빨리 감기 버튼을 함께 누르면 건너뜁니다. 선택지나 팝업에서는 멈추고 조작을 돌려줍니다.",
    "在可跳过的事件中同时按下取消与快进按钮即可跳过。遇到选项或弹出窗口时会停下并交还操作。",
};

EventSetupFn g_OriginalSetup = nullptr;
EventDestroyFn g_OriginalDestroy = nullptr;
PadPushCommandFn g_PadPushCommand = nullptr;
PadDatabaseFn g_PadDatabase = nullptr;
U64MapFindFn g_U64MapFind = nullptr;
ResolvePadFn g_ResolvePad = nullptr;

bool g_Installed = false;
int g_KeySetting = CANOPY_NO_SETTING;
int g_ButtonSetting = CANOPY_NO_SETTING;
int g_SpeedSetting = CANOPY_NO_SETTING;
int g_TextSetting = CANOPY_NO_SETTING;
int g_SkipSetting = CANOPY_NO_SETTING;

// Live events. Setup and destruction run on the game thread, but the lock
// keeps the list sound if a loader thread ever builds one.
SRWLOCK g_EventsLock = SRWLOCK_INIT;
std::vector<uint8_t*> g_Events;

bool g_FastForwarding = false;
uint32_t g_FastFrames = 0;

bool g_Skipping = false;
bool g_ChordHeld = false;
uint64_t g_SkipStartMs = 0;

int64_t __fastcall Detour_Setup(uint8_t* event) {
    const int64_t result = g_OriginalSetup(event);
    AcquireSRWLockExclusive(&g_EventsLock);
    g_Events.push_back(event);
    ReleaseSRWLockExclusive(&g_EventsLock);
    return result;
}

int64_t __fastcall Detour_Destroy(uint8_t* event) {
    AcquireSRWLockExclusive(&g_EventsLock);
    g_Events.erase(std::remove(g_Events.begin(), g_Events.end(), event), g_Events.end());
    ReleaseSRWLockExclusive(&g_EventsLock);
    return g_OriginalDestroy(event);
}

// Holds no C++ objects, so the read can run under __try.
bool IsSkippable(const uint8_t* event) {
    __try {
        const uint8_t* status = ReadAt<const uint8_t*>(event, retail::kEventStatus);
        const uint8_t* const* slot = status ? ReadAt<const uint8_t* const*>(status, retail::kEventStatusData) : nullptr;
        const uint8_t* data = slot ? *slot : nullptr;
        return data && ReadAt<uint32_t>(data, retail::kEventDataIsSkip) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SkippableEventRunning() {
    AcquireSRWLockShared(&g_EventsLock);
    const bool running = std::any_of(g_Events.begin(), g_Events.end(), IsSkippable);
    ReleaseSRWLockShared(&g_EventsLock);
    return running;
}

double Speed() {
    const int32_t choice = host::Choice(g_SpeedSetting, kDefaultSpeed);
    return kSpeeds[choice >= 0 && choice < static_cast<int32_t>(sizeof(kSpeeds) / sizeof(kSpeeds[0])) ? choice
                                                                                                   : kDefaultSpeed];
}

// Keeps one frame of fast forward queued, unless the game queued more.
void QueueFastFrame(uint8_t* timeManager, double speed) {
    if (ReadAt<int32_t>(timeManager, retail::kTimeFastForwardFrames) <= 1) {
        WriteAt<int32_t>(timeManager, retail::kTimeFastForwardFrames, 1);
        WriteAt<double>(timeManager, retail::kTimeMagnification, speed);
    }
}

// Queues the advance command the talk window waits on, every other frame so
// each one reads as a fresh press.
void AdvanceTalk() {
    uint8_t* padManager = ReadGlobal<uint8_t*>(retail::kPadManager);
    const uint8_t* talkManager = ReadGlobal<const uint8_t*>(retail::kTalkManager);
    if (!padManager || !talkManager || (++g_FastFrames & 1) == 0) {
        return;
    }
    alignas(16) const float zero[4] = {};
    const int64_t command =
        talkManager[retail::kTalkFastNext] != 0 ? retail::kCommandFastNext : retail::kCommandNext2;
    g_PadPushCommand(padManager, command, zero, 0);
}

// The pad the virtual Cancel is bound to, CANOPY_NO_PAD when it cannot be
// resolved. Holds no C++ objects, so the calls can run under __try.
int32_t ResolveCancelRaw() {
    uint8_t* padManager = ReadGlobal<uint8_t*>(retail::kPadManager);
    void* registry = ReadGlobal<void*>(retail::kDatabaseRegistry);
    uint8_t* database = padManager && registry ? g_PadDatabase(registry) : nullptr;
    if (!database) {
        return CANOPY_NO_PAD;
    }
    uint8_t* node = nullptr;
    const uint64_t key = retail::kVirtualPadCancel;
    g_U64MapFind(database + retail::kPadDatabaseTable, &node, &key);
    if (!node || node == ReadAt<uint8_t*>(database, retail::kPadDatabaseTableEnd)) {
        return CANOPY_NO_PAD;
    }
    const int64_t* const* holder = nullptr;
    g_ResolvePad(padManager, &holder, node + retail::kMapNodeValue);
    return holder && *holder ? static_cast<int32_t>(**holder) : CANOPY_NO_PAD;
}

// Controller B, Cancel's default, when the binding cannot be read.
int32_t CancelPad() {
    int32_t pad = CANOPY_NO_PAD;
    __try {
        pad = ResolveCancelRaw();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        pad = CANOPY_NO_PAD;
    }
    return pad != CANOPY_NO_PAD && host::Api().PadName(pad) ? pad : retail::kControllerPadB;
}

bool SkipPasses(int64_t command) {
    return std::find(std::begin(retail::kSkipPassCommands), std::end(retail::kSkipPassCommands), command) !=
           std::end(retail::kSkipPassCommands);
}

// The first command this frame's queries ask for that the skip must not run
// past, 0 when there is none. Holds no C++ objects, so it can run under
// __try.
int64_t BlockingQueryRaw(const uint8_t* padManager) {
    const uint8_t* entry = ReadAt<const uint8_t*>(padManager, retail::kCommandQueriesBegin);
    const uint8_t* end = ReadAt<const uint8_t*>(padManager, retail::kCommandQueriesEnd);
    if (!entry || end < entry || static_cast<size_t>(end - entry) > kMaxCommandQueries * retail::kCommandQuerySize) {
        return 0;
    }
    for (; entry < end; entry += retail::kCommandQuerySize) {
        const uint8_t* const* holder = ReadAt<const uint8_t* const*>(entry, retail::kCommandQueryRecord);
        const uint8_t* record = holder ? *holder : nullptr;
        if (!record || ReadAt<int32_t>(record, retail::kCommandRecordKind) == retail::kCommandKindIgnored) {
            continue;
        }
        const int64_t command = ReadAt<int64_t>(record, 0);
        if (command != 0 && !SkipPasses(command)) {
            return command;
        }
    }
    return 0;
}

// -1 when the queries cannot be read, which stops the skip too.
int64_t BlockingQuery() {
    const uint8_t* padManager = ReadGlobal<const uint8_t*>(retail::kPadManager);
    if (!padManager) {
        return -1;
    }
    __try {
        return BlockingQueryRaw(padManager);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

std::string BlockReason(int64_t command) {
    return command < 0 ? std::string("the command queries could not be read")
                       : "command " + std::to_string(command) + " is waiting for the player";
}

bool SkipEnabled() {
    return g_SkipSetting != CANOPY_NO_SETTING && host::Enabled(g_SkipSetting);
}

// Cancel and the fast forward button, as the game's own chord.
bool ChordHeld() {
    const int32_t button = host::Binding(g_ButtonSetting);
    return button != CANOPY_NO_PAD && host::Held(button) && host::Held(CancelPad());
}

// The time manager resets the speed itself a frame after the last one
// queued.
void StopSkip(const std::string& reason) {
    g_Skipping = false;
    Log("Event skip stopped: " + reason + ".");
}

// Why a running skip must stop now, empty while it may go on.
std::string SkipStopReason(const uint8_t* timeManager) {
    if (!SkipEnabled()) {
        return "the setting was turned off";
    }
    if (!timeManager) {
        return "the time manager is missing";
    }
    if (PlayerInField()) {
        return "the player has control";
    }
    if (!SkippableEventRunning()) {
        return "no skippable event is running";
    }
    if (GetTickCount64() - g_SkipStartMs > kSkipTimeLimitMs) {
        return "time limit reached";
    }
    return {};
}

// Runs the skip. True while it owns the frame, so fast forward stays off.
bool TickSkip(uint8_t* timeManager) {
    const bool chord = SkipEnabled() && ChordHeld();
    const bool pressed = chord && !g_ChordHeld;
    g_ChordHeld = chord;
    if (!g_Skipping) {
        if (!pressed || !timeManager || PlayerInField() || !SkippableEventRunning()) {
            return false;
        }
        if (const int64_t command = BlockingQuery()) {
            Log("Event skip did not start: " + BlockReason(command) + ".");
            return false;
        }
        g_Skipping = true;
        g_SkipStartMs = GetTickCount64();
        Log("Event skip started.");
    }
    const std::string reason = SkipStopReason(timeManager);
    if (!reason.empty()) {
        StopSkip(reason);
        return false;
    }
    if (const int64_t command = BlockingQuery()) {
        StopSkip(BlockReason(command));
        return false;
    }
    QueueFastFrame(timeManager, kSkipSpeed);
    AdvanceTalk();
    return true;
}

}  // namespace

bool Install() {
    if (!RequirementsMet(retail::kEventFastForwardRequirements)) {
        return false;
    }
    g_PadPushCommand = GameFunction<PadPushCommandFn>(retail::kPadPushCommand);
    DetourSet detours("Event fast forward");
    const bool created =
        detours.Add(
            retail::kEventSetup,
            reinterpret_cast<void*>(&Detour_Setup),
            reinterpret_cast<void**>(&g_OriginalSetup),
            "eventSetup"
        ) &&
        detours.Add(
            retail::kEventDestroy,
            reinterpret_cast<void*>(&Detour_Destroy),
            reinterpret_cast<void**>(&g_OriginalDestroy),
            "eventDestroy"
        );
    if (!created) {
        detours.RemoveAll();
        return false;
    }
    g_KeySetting = host::AddBinding(
        "FastForwardKeyboard", kKeyLabels, kBindingHelp, CANOPY_DEVICE_KEYBOARD_MOUSE, kDefaultKey
    );
    g_ButtonSetting = host::AddBinding(
        "FastForwardController", kButtonLabels, kBindingHelp, CANOPY_DEVICE_CONTROLLER, kDefaultButton
    );
    g_SpeedSetting = host::AddChoice(
        "FastForwardSpeed", kSpeedLabels, kSpeedHelp, kSpeedChoices,
        static_cast<int32_t>(sizeof(kSpeedChoices) / sizeof(kSpeedChoices[0])), kDefaultSpeed
    );
    g_TextSetting = host::AddToggle("FastForwardText", kTextLabels, kTextHelp, true);
    // The skip is optional: fast forward works without it.
    if (RequirementsMet(retail::kEventSkipRequirements)) {
        g_PadDatabase = GameFunction<PadDatabaseFn>(retail::kPadDatabase);
        g_U64MapFind = GameFunction<U64MapFindFn>(retail::kU64MapFind);
        g_ResolvePad = GameFunction<ResolvePadFn>(retail::kResolvePad);
        g_SkipSetting = host::AddToggle("EventSkip", kSkipLabels, kSkipHelp, false);
    } else {
        Log("Event skip is off: its game code does not match.");
    }
    g_Installed = true;
    return true;
}

void Tick() {
    if (!g_Installed) {
        return;
    }
    uint8_t* timeManager = ReadGlobal<uint8_t*>(retail::kTimeManager);
    const bool skipping = TickSkip(timeManager);
    const bool held = host::Held(host::Binding(g_KeySetting)) || host::Held(host::Binding(g_ButtonSetting));
    const bool active = !skipping && held && timeManager && !PlayerInField() && SkippableEventRunning();
    if (active != g_FastForwarding) {
        g_FastForwarding = active;
        g_FastFrames = 0;
        Log(active ? "Event fast forward started." : "Event fast forward stopped.");
    }
    if (!active) {
        return;
    }
    QueueFastFrame(timeManager, Speed());
    if (host::Enabled(g_TextSetting)) {
        AdvanceTalk();
    }
}

}  // namespace event_fast_forward
