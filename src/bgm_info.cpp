#include <bitset>
#include <cstdint>
#include <string>

#include "core.h"
#include "features.h"
#include "host.h"
#include "ost_tracklist.h"

// =============================================================
// BGM INFO DESIGN
// =============================================================
// The first time a song plays, its localized title from bgm.dat is shown as
// a loader toast, in the game's own popup log. The text is built when the
// song starts, in the language the game shows then.

namespace bgm_info {
namespace {

using namespace core;

// The highest BGM id tracked. bgm.dat ids are far below it.
constexpr uint64_t kMaxBgmId = 0xFFFF;
// U+266A EIGHTH NOTE followed by a space.
constexpr char kNotePrefix[] = "\xE2\x99\xAA ";
// U+2022 BULLET followed by a space. English, French and Spanish text uses
// Lora, which has no U+266A, and the engine does no glyph fallback.
constexpr char kLatinPrefix[] = "\xE2\x80\xA2 ";

constexpr CanopyText kSettingLabels = {
    "BGM情報を表示",
    "Show BGM Information",
    "Afficher les infos BGM",
    "Mostrar información de BGM",
    "顯示BGM資訊",
    "BGM 정보 표시",
    "显示BGM信息",
};

constexpr CanopyText kSettingHelp = {
    "曲が初めて流れたときに曲名を表示します。",
    "Shows each song's title the first time it plays.",
    "Affiche le titre de chaque morceau la première fois qu'il est joué.",
    "Muestra el título de cada canción la primera vez que suena.",
    "樂曲第一次播放時顯示曲名。",
    "곡이 처음 재생될 때 곡명을 표시합니다.",
    "乐曲第一次播放时显示曲名。",
};

using MakeBgmParamFn = void*(__fastcall*)(void* soundManager, void* outParam, uint64_t bgmId);
using GetBgmTableFn = uint8_t*(__fastcall*)(void* databaseRegistry);

MakeBgmParamFn g_OriginalMakeBgmParam = nullptr;
GetBgmTableFn g_GetBgmTable = nullptr;
int g_Setting = CANOPY_NO_SETTING;
// Each BGM id is announced at most once per game session. Only touched from
// makeBgmParam, which always runs on the game thread.
std::bitset<kMaxBgmId + 1> g_AnnouncedBgmIds;

// =============================================================
// BGM TITLES
// =============================================================
const char* FindBgmTitle(uint64_t bgmId) {
    void* registry = ReadGlobal<void*>(retail::kDatabaseRegistry);
    if (!registry) {
        return nullptr;
    }
    const uint8_t* table = g_GetBgmTable(registry);
    if (!table) {
        return nullptr;
    }

    const int32_t language = CurrentLanguage();
    auto slot = ReadAt<const uint8_t* const* const*>(table, retail::kBgmTableSlotsBegin);
    const auto end = ReadAt<const uint8_t* const* const*>(table, retail::kBgmTableSlotsEnd);
    for (; slot != end; ++slot) {
        const uint8_t* row = **slot;
        if (!row || ReadAt<uint64_t>(row, retail::kBgmRowId) != bgmId) {
            continue;
        }
        const int fallbacks[] = {language, retail::kLanguageEnglish, retail::kLanguageJapanese};
        for (int candidate : fallbacks) {
            const size_t offset = retail::kBgmRowTitles + sizeof(const char*) * candidate;
            const char* title = ReadAt<const char*>(row, offset);
            if (title && title[0] != '\0') {
                return title;
            }
        }
        return nullptr;
    }
    return nullptr;
}

const char* TitlePrefix() {
    switch (ReadGlobal<int32_t>(retail::kLanguage)) {
        case retail::kLanguageEnglish:
        case retail::kLanguageFrench:
        case retail::kLanguageSpanish:
            return kLatinPrefix;
        default:
            return kNotePrefix;
    }
}

// The title is never logged: the log should not spoil songs.
void AnnounceBgm(uint64_t bgmId) {
    // A song that plays while the popup is off stays unannounced, so it gets
    // its popup next time if the setting is back on.
    if (!host::Enabled(g_Setting) || bgmId == 0 || bgmId > kMaxBgmId || g_AnnouncedBgmIds.test(bgmId)) {
        return;
    }
    const char* title = FindBgmTitle(bgmId);
    if (!title) {
        Log("No title for BGM id " + std::to_string(bgmId) + ", skipping.");
        return;
    }
    std::string text = std::string(TitlePrefix()) + title;
    if (const ost::TrackPosition* position = ost::FindTrack(bgmId)) {
        text += " (Disc " + std::to_string(position->disc) + ", Track " + std::to_string(position->track) + ")";
    }
    // The loader logs why a toast failed; the song is tried again next time.
    if (!host::ShowToast(text.c_str())) {
        return;
    }
    g_AnnouncedBgmIds.set(bgmId);
    Log("Announced BGM id " + std::to_string(bgmId) + ".");
}

// =============================================================
// DETOURS
// =============================================================
void* __fastcall Detour_MakeBgmParam(void* soundManager, void* outParam, uint64_t bgmId) {
    void* result = g_OriginalMakeBgmParam(soundManager, outParam, bgmId);
    // The engine may remap the requested id; the param holds the id that plays.
    AnnounceBgm(ReadAt<uint64_t>(outParam, retail::kBgmParamIdOffset));
    return result;
}

}  // namespace

bool Install() {
    if (!RequirementsMet(retail::kBgmInfoRequirements)) {
        return false;
    }
    g_GetBgmTable = GameFunction<GetBgmTableFn>(retail::kGetBgmTable);

    DetourSet detours("BGM info");
    if (!detours.Add(
            retail::kMakeBgmParam,
            reinterpret_cast<void*>(&Detour_MakeBgmParam),
            reinterpret_cast<void**>(&g_OriginalMakeBgmParam),
            "makeBgmParam"
        )) {
        return false;
    }
    g_Setting = host::AddToggle("ShowBgmInfo", kSettingLabels, kSettingHelp, true);
    return true;
}

}  // namespace bgm_info
