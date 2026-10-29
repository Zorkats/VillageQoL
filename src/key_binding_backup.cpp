#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "core.h"
#include "features.h"
#include "host.h"

// =============================================================
// KEY BINDING BACKUP DESIGN
// =============================================================
// The game keeps the player's key bindings in its system save data as a
// table of the keys rebound away from their defaults; an empty table means
// every key is at its default. The backup mirrors that table to the
// [KeyBindings] section of VillageQoL.ini, one "key=pad" line per entry.
//
// Mirror: the tick checks the table every half second. A new system data
// object with an empty table is a fresh load, not a reset, so it is never
// written over the backup. A table that empties while the same system data
// is live is the player resetting their keys, and clears the backup.
//
// Restore: when the system save loads, before the game applies the loaded
// table, an empty table is filled from the backup through the table's own
// insert, and the game manager is marked dirty so the game saves it back on
// the next options close. A table with any entry is the player's and is left
// alone.

namespace key_binding_backup {
namespace {

using namespace core;

using LoadSucceededFn = int64_t(__fastcall*)(uint8_t* task);
using MapIndexFn = void*(__fastcall*)(uint8_t* map, uint8_t** outNode, const uint64_t* key);

constexpr const char* kSection = "KeyBindings";
constexpr uint32_t kPollFrames = 30;
// Far above the game's own key count, so a damaged table is never mirrored.
constexpr size_t kMaxEntries = 256;
constexpr uint64_t kMaxKey = 4096;
constexpr uint64_t kMaxPad = 255;
constexpr DWORD kSectionBufferSize = 16 * 1024;

constexpr CanopyText kLabels = {
    "キー設定のバックアップ", "Back Up Key Bindings", "Sauvegarder les touches", "Copia de los controles",
    "備份按鍵設定", "키 설정 백업", "备份按键设置",
};

constexpr CanopyText kHelp = {
    "キー設定をVillageQoL.iniにも保存し、初期状態で読み込まれたときに戻します。",
    "Keeps a copy of your key bindings in VillageQoL.ini and restores it if they load back at their defaults.",
    "Garde une copie de vos touches dans VillageQoL.ini et la rétablit si elles se chargent par défaut.",
    "Guarda una copia de tus controles en VillageQoL.ini y la restaura si se cargan con los valores por defecto.",
    "在VillageQoL.ini中保存按鍵設定的副本，若讀取時回到預設值則加以還原。",
    "키 설정을 VillageQoL.ini에도 저장하고, 기본값으로 불러와지면 되돌립니다.",
    "在VillageQoL.ini中保存按键设置的副本，若读取时回到默认值则加以还原。",
};

using Entry = std::pair<uint64_t, uint64_t>;

struct TableRead {
    Entry entries[kMaxEntries];
    size_t count;
};

LoadSucceededFn g_OriginalLoadSucceeded = nullptr;
MapIndexFn g_MapIndex = nullptr;

bool g_Installed = false;
int g_Setting = CANOPY_NO_SETTING;
uint32_t g_Frames = 0;
const uint8_t* g_LastSystem = nullptr;
std::vector<Entry> g_LastEntries;

// VillageQoL's own settings file, mods\VillageQoL.ini.
const std::string& FilePath() {
    static const std::string path = host::Api().SettingsFile(host::Mod());
    return path;
}

bool Valid(uint64_t key, uint64_t pad) {
    return key != 0 && key < kMaxKey && pad != 0 && pad <= kMaxPad;
}

// Walks the table's node list. Holds no C++ objects, so it runs under
// __try. False when the table is damaged or larger than any real one.
bool ReadTable(const uint8_t* map, TableRead& read) {
    __try {
        read.count = 0;
        const uint64_t size = ReadAt<uint64_t>(map, retail::kMapSize);
        if (size > kMaxEntries) {
            return false;
        }
        const uint8_t* head = ReadAt<const uint8_t*>(map, retail::kMapListHead);
        const uint8_t* node = ReadAt<const uint8_t*>(head, retail::kMapNodeNext);
        while (node != head) {
            if (read.count == size) {
                return false;
            }
            const uint64_t key = ReadAt<uint64_t>(node, retail::kMapNodeKey);
            const uint64_t pad = ReadAt<uint64_t>(node, retail::kMapNodeValue);
            if (!Valid(key, pad)) {
                return false;
            }
            read.entries[read.count++] = {key, pad};
            node = ReadAt<const uint8_t*>(node, retail::kMapNodeNext);
        }
        return read.count == size;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::vector<Entry> LoadBackup() {
    std::vector<char> buffer(kSectionBufferSize);
    const DWORD length = GetPrivateProfileSectionA(kSection, buffer.data(), kSectionBufferSize, FilePath().c_str());
    std::vector<Entry> entries;
    for (const char* line = buffer.data(); line < buffer.data() + length && *line; line += std::strlen(line) + 1) {
        char* end = nullptr;
        const uint64_t key = std::strtoull(line, &end, 10);
        if (*end != '=') {
            continue;
        }
        const uint64_t pad = std::strtoull(end + 1, &end, 10);
        if (*end == '\0' && Valid(key, pad) && entries.size() < kMaxEntries) {
            entries.emplace_back(key, pad);
        }
    }
    return entries;
}

void SaveBackup(const std::vector<Entry>& entries) {
    if (entries.empty()) {
        WritePrivateProfileStringA(kSection, nullptr, nullptr, FilePath().c_str());
        return;
    }
    std::string section;
    for (const Entry& entry : entries) {
        section += std::to_string(entry.first) + "=" + std::to_string(entry.second);
        section.push_back('\0');
    }
    section.push_back('\0');
    WritePrivateProfileSectionA(kSection, section.data(), FilePath().c_str());
}

int64_t __fastcall Detour_LoadSucceeded(uint8_t* task) {
    // The loaded data may reuse the old one's address; it is still a fresh
    // load, never a reset.
    g_LastSystem = nullptr;
    const uint8_t* owner = ReadAt<const uint8_t*>(task, retail::kTaskOwner);
    uint8_t* system = owner ? ReadAt<uint8_t*>(owner, retail::kLoadOwnerSystemData) : nullptr;
    uint8_t* manager = ReadGlobal<uint8_t*>(retail::kGameManager);
    if (g_Installed && host::Enabled(g_Setting) && system && manager) {
        uint8_t* map = system + retail::kSystemKeyTable;
        if (ReadAt<uint64_t>(map, retail::kMapSize) == 0) {
            const std::vector<Entry> backup = LoadBackup();
            for (const Entry& entry : backup) {
                uint8_t* node = nullptr;
                g_MapIndex(map, &node, &entry.first);
                WriteAt<uint64_t>(node, retail::kMapNodeValue, entry.second);
            }
            if (!backup.empty()) {
                WriteAt<uint8_t>(manager, retail::kManagerSystemDirty, 1);
                Log("Key bindings restored from the backup, " + std::to_string(backup.size()) + " keys.");
            }
        }
    }
    return g_OriginalLoadSucceeded(task);
}

void Mirror() {
    const uint8_t* manager = ReadGlobal<const uint8_t*>(retail::kGameManager);
    const uint8_t* system = manager ? ReadAt<const uint8_t*>(manager, retail::kManagerSystemData) : nullptr;
    if (!system) {
        return;
    }
    static TableRead read;
    if (!ReadTable(system + retail::kSystemKeyTable, read)) {
        return;
    }
    std::vector<Entry> entries(read.entries, read.entries + read.count);
    std::sort(entries.begin(), entries.end());
    if (system != g_LastSystem) {
        g_LastSystem = system;
        if (entries.empty()) {
            g_LastEntries.clear();
            return;
        }
    } else if (entries == g_LastEntries) {
        return;
    }
    SaveBackup(entries);
    Log(entries.empty() ? std::string("Key binding backup cleared, the keys were reset.")
                        : "Key binding backup saved, " + std::to_string(entries.size()) + " keys.");
    g_LastEntries = std::move(entries);
}

}  // namespace

bool Install() {
    if (!RequirementsMet(retail::kKeyBindingRequirements)) {
        return false;
    }
    g_MapIndex = GameFunction<MapIndexFn>(retail::kU64MapIndex);
    DetourSet detours("Key binding backup");
    if (!detours.Add(
            retail::kSystemLoadSucceeded,
            reinterpret_cast<void*>(&Detour_LoadSucceeded),
            reinterpret_cast<void**>(&g_OriginalLoadSucceeded),
            "systemLoadSucceeded"
        )) {
        detours.RemoveAll();
        return false;
    }
    g_Setting = host::AddToggle("BackUpKeyBindings", kLabels, kHelp, true);
    g_Installed = true;
    return true;
}

void Tick() {
    if (!g_Installed || ++g_Frames < kPollFrames) {
        return;
    }
    g_Frames = 0;
    if (host::Enabled(g_Setting)) {
        Mirror();
    }
}

}  // namespace key_binding_backup
