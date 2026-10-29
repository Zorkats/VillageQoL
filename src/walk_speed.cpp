#include <cstdint>

#include "core.h"
#include "features.h"
#include "host.h"

// =============================================================
// WALK SPEED DESIGN
// =============================================================
// The player's free movement runs in the walk state's update, which asks the
// unit for its move speed and scales it by the stick and its gait. Only that
// call is scaled, while the walk update runs, so the area and status
// slowdowns, cutscene movement and every other user of the move speed stay
// untouched. The update then sets the walk or run animation's speed, which
// is scaled the same way so the steps match the ground covered.

namespace walk_speed {
namespace {

using namespace core;

using WalkUpdateFn = char(__fastcall*)(uint8_t* state, void* frame);
using GetMoveSpeedFn = float(__fastcall*)(uint8_t* unit);

constexpr float kMultipliers[] = {1.0f, 1.25f, 1.5f, 1.75f, 2.0f};
constexpr int32_t kDefaultChoice = 2;

constexpr CanopyText kChoices[] = {
    {"通常", "Normal", "Normale", "Normal", "正常", "보통", "正常"},
    {"1.25倍", "1.25x", "1.25x", "1.25x", "1.25倍", "1.25배", "1.25倍"},
    {"1.5倍", "1.5x", "1.5x", "1.5x", "1.5倍", "1.5배", "1.5倍"},
    {"1.75倍", "1.75x", "1.75x", "1.75x", "1.75倍", "1.75배", "1.75倍"},
    {"2倍", "2x", "2x", "2x", "2倍", "2배", "2倍"},
};
static_assert(sizeof(kChoices) / sizeof(kChoices[0]) == sizeof(kMultipliers) / sizeof(kMultipliers[0]), "choices");

constexpr CanopyText kLabels = {
    "移動速度", "Walking Speed", "Vitesse de marche", "Velocidad al caminar", "移動速度", "이동 속도", "移动速度",
};

constexpr CanopyText kHelp = {
    "フィールドでの歩きと走りの速さです。イベント中の移動は変わりません。",
    "How fast you walk and run in the field. Movement during events is unchanged.",
    "Vitesse de marche et de course sur le terrain. Les déplacements pendant les événements ne changent pas.",
    "Velocidad al caminar y correr en el campo. El movimiento durante los eventos no cambia.",
    "在野外行走與奔跑的速度。事件中的移動不受影響。",
    "필드에서 걷고 달리는 속도입니다. 이벤트 중의 이동은 바뀌지 않습니다.",
    "在野外行走与奔跑的速度。事件中的移动不受影响。",
};

WalkUpdateFn g_OriginalWalkUpdate = nullptr;
GetMoveSpeedFn g_OriginalGetMoveSpeed = nullptr;

int g_Setting = CANOPY_NO_SETTING;
// Set while the walk update runs with a multiplier, and the unit whose move
// speed it asked for.
float g_ActiveMultiplier = 1.0f;
uint8_t* g_WalkingUnit = nullptr;

float Multiplier() {
    const int32_t choice = host::Choice(g_Setting, kDefaultChoice);
    return choice >= 0 && choice < static_cast<int32_t>(sizeof(kMultipliers) / sizeof(kMultipliers[0]))
               ? kMultipliers[choice]
               : 1.0f;
}

float __fastcall Detour_GetMoveSpeed(uint8_t* unit) {
    const float speed = g_OriginalGetMoveSpeed(unit);
    if (g_ActiveMultiplier == 1.0f) {
        return speed;
    }
    g_WalkingUnit = unit;
    return speed * g_ActiveMultiplier;
}

// Matches the animation to the faster steps, as the update itself writes it:
// the gait's speed for a walk or run, 1.0 or the dash's while stopping.
void SyncAnimation(const uint8_t* state, float multiplier) {
    const uint8_t* component = ReadAt<const uint8_t*>(g_WalkingUnit, retail::kUnitAnimationComponent);
    uint8_t* player = component ? ReadAt<uint8_t*>(component, retail::kAnimationComponentPlayer) : nullptr;
    if (!player) {
        return;
    }
    const int32_t gait = ReadAt<int32_t>(state, retail::kWalkGait);
    if (gait == retail::kGaitWalk || gait == retail::kGaitRun) {
        WriteAt<float>(player, retail::kAnimationPlayerSpeed, ReadAt<float>(state, retail::kWalkAnimationSpeed) * multiplier);
    }
}

char __fastcall Detour_WalkUpdate(uint8_t* state, void* frame) {
    // An event may drive the walk state too; only the player's own walking
    // is sped up.
    const float multiplier = PlayerInField() ? Multiplier() : 1.0f;
    g_ActiveMultiplier = multiplier;
    g_WalkingUnit = nullptr;
    const char result = g_OriginalWalkUpdate(state, frame);
    g_ActiveMultiplier = 1.0f;
    if (multiplier != 1.0f && g_WalkingUnit) {
        SyncAnimation(state, multiplier);
    }
    g_WalkingUnit = nullptr;
    return result;
}

}  // namespace

bool Install() {
    if (!RequirementsMet(retail::kWalkSpeedRequirements)) {
        return false;
    }
    DetourSet detours("Walk speed");
    const bool created =
        detours.Add(
            retail::kPlayerWalkUpdate,
            reinterpret_cast<void*>(&Detour_WalkUpdate),
            reinterpret_cast<void**>(&g_OriginalWalkUpdate),
            "playerWalkUpdate"
        ) &&
        detours.Add(
            retail::kPlayerGetMoveSpeed,
            reinterpret_cast<void*>(&Detour_GetMoveSpeed),
            reinterpret_cast<void**>(&g_OriginalGetMoveSpeed),
            "playerGetMoveSpeed"
        );
    if (!created) {
        detours.RemoveAll();
        return false;
    }
    g_Setting = host::AddChoice(
        "WalkSpeed", kLabels, kHelp, kChoices, static_cast<int32_t>(sizeof(kChoices) / sizeof(kChoices[0])),
        kDefaultChoice
    );
    return true;
}

}  // namespace walk_speed
