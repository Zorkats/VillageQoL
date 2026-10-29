#include <algorithm>
#include <cstdint>

#include "core.h"
#include "features.h"
#include "host.h"

// =============================================================
// WHEEL ZOOM DESIGN
// =============================================================
// The field camera has three zoom levels, which its change camera command
// cycles through. The mouse wheel steps through the same levels instead:
// up zooms in, down zooms out, stopping at either end. The tick collects the
// wheel's notches while the player is in the field, and the camera's own
// update applies them the way its command does: it sets the level and starts
// the same tween toward that level's zoom. The game reads the wheel in the
// field for nothing else.

namespace wheel_zoom {
namespace {

using namespace core;

using CameraUpdateFn = char(__fastcall*)(uint8_t* control, void* frame);
using TweenStartFn = void(__fastcall*)(uint8_t* tween, const float* from, const float* to, const void* param);

// The zoom command's FadeParam: its frames, a zero delay, curve type 27,
// then the curve's range.
struct FadeParam {
    double frames;
    double delay;
    int32_t curve;
    int32_t padding;
    double from;
    double to;
    uint64_t reserved;
};
static_assert(sizeof(FadeParam) == 48, "FadeParam layout");

constexpr int32_t kTweenCurve = 27;
constexpr int32_t kLevelCount = static_cast<int32_t>(sizeof(retail::kCameraZoomLevels) / sizeof(float));
// Notches past either end are dropped, so a long scroll never queues more.
constexpr int32_t kMaxPending = kLevelCount - 1;

constexpr CanopyText kLabels = {
    "ホイールでズーム", "Mouse Wheel Zoom", "Zoom à la molette", "Zoom con la rueda", "滾輪縮放", "휠로 줌",
    "滚轮缩放",
};

constexpr CanopyText kHelp = {
    "フィールドでマウスホイールを回すとカメラが近づいたり離れたりします。",
    "Scroll the mouse wheel in the field to zoom the camera in or out.",
    "Tournez la molette sur le terrain pour rapprocher ou éloigner la caméra.",
    "Gira la rueda del ratón en el campo para acercar o alejar la cámara.",
    "在野外轉動滑鼠滾輪即可拉近或拉遠鏡頭。",
    "필드에서 마우스 휠을 돌리면 카메라가 가까워지거나 멀어집니다.",
    "在野外转动鼠标滚轮即可拉近或拉远镜头。",
};

CameraUpdateFn g_OriginalCameraUpdate = nullptr;
TweenStartFn g_TweenStart = nullptr;

bool g_Installed = false;
int g_Setting = CANOPY_NO_SETTING;
// Wheel notches not yet applied, up positive.
int32_t g_Pending = 0;

void ApplyZoom(uint8_t* control) {
    const int32_t steps = g_Pending;
    g_Pending = 0;
    if (control[retail::kCameraControlEnabled] == 0 || control[retail::kCameraZoomEnabled] == 0 ||
        ReadAt<uintptr_t>(control, retail::kCameraRequestsBegin) !=
            ReadAt<uintptr_t>(control, retail::kCameraRequestsEnd)) {
        return;
    }
    const int32_t level = ReadAt<int32_t>(control, retail::kCameraZoomLevel);
    if (level < 0 || level >= kLevelCount) {
        return;
    }
    const int32_t target = std::clamp(level + steps, 0, kLevelCount - 1);
    if (target == level) {
        return;
    }
    WriteAt<int32_t>(control, retail::kCameraZoomLevel, target);
    const FadeParam param = {retail::kCameraZoomTweenFrames, 0.0, kTweenCurve, 0, 0.0, 1.0, 0};
    g_TweenStart(
        control + retail::kCameraZoomTween,
        reinterpret_cast<const float*>(control + retail::kCameraZoomValue),
        &retail::kCameraZoomLevels[target],
        &param
    );
}

char __fastcall Detour_CameraUpdate(uint8_t* control, void* frame) {
    if (g_Pending != 0) {
        ApplyZoom(control);
    }
    return g_OriginalCameraUpdate(control, frame);
}

}  // namespace

bool Install() {
    if (!RequirementsMet(retail::kWheelZoomRequirements)) {
        return false;
    }
    g_TweenStart = GameFunction<TweenStartFn>(retail::kTweenStart);
    DetourSet detours("Wheel zoom");
    if (!detours.Add(
            retail::kCameraControlUpdate,
            reinterpret_cast<void*>(&Detour_CameraUpdate),
            reinterpret_cast<void**>(&g_OriginalCameraUpdate),
            "cameraControlUpdate"
        )) {
        detours.RemoveAll();
        return false;
    }
    g_Setting = host::AddToggle("WheelZoom", kLabels, kHelp, true);
    g_Installed = true;
    return true;
}

void Tick() {
    if (!g_Installed) {
        return;
    }
    if (!host::Enabled(g_Setting) || !PlayerInField()) {
        g_Pending = 0;
        return;
    }
    g_Pending = std::clamp(g_Pending + host::WheelSteps(), -kMaxPending, kMaxPending);
}

}  // namespace wheel_zoom
