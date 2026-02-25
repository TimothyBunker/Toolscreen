#include "notes_overlay.h"
#include "gui.h"
#include "utils.h"

#include "imgui.h"
#include "imgui_stdlib.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <shellapi.h>
#include <GL/gl.h>

#include "stb_image.h"

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE GL_CLAMP
#endif

namespace {

enum class NotesSortMode : int {
    DateNewest = 0,
    DateOldest = 1,
    NameAsc = 2,
    NameDesc = 3,
    NumberAsc = 4,
    NumberDesc = 5
};

struct NotesFileEntry {
    std::filesystem::path path;
    std::string title;
    std::string displayLabel;
    int64_t modifiedEpochSeconds = 0;
    uint64_t numberKey = std::numeric_limits<uint64_t>::max();
    bool pinned = false;
    bool favorite = false;
};

struct NotesOverlayState {
    bool initializedVisibility = false;
    bool visible = false;
    bool refreshRequested = true;
    bool forceTabSelectionNextFrame = false;

    int activeTab = 0; // 0=IGN, 1=General
    bool focusIgnEditorNextFrame = false;
    bool focusGeneralEditorNextFrame = false;
    int ignEditorTab = 0;     // 0=Edit, 1=Preview
    int generalEditorTab = 0; // 0=Edit, 1=Preview
    bool storageDraftInitialized = false;
    std::string markdownDirDraft;
    std::string pdfDirDraft;

    int ignSortMode = static_cast<int>(NotesSortMode::DateNewest);
    int generalSortMode = static_cast<int>(NotesSortMode::DateNewest);

    bool ignEditedSinceOpen = false;
    std::string ignDraft;
    std::filesystem::path ignEditingPath;
    std::vector<NotesFileEntry> ignEntries;
    int selectedIgnEntryIndex = -1;

    std::vector<std::string> generalFolders;
    int selectedGeneralFolderIndex = 0;
    int generalFolderTabOffset = 0;
    std::string newFolderName;
    bool pendingNewGeneralNotePopupOpen = false;
    std::string pendingNewGeneralNoteName;
    std::string generalTitle;
    std::string generalDraft;
    std::filesystem::path generalEditingPath;
    std::vector<NotesFileEntry> generalEntries;
    int selectedGeneralEntryIndex = -1;
    std::set<std::wstring> pinnedPathKeys;
    std::set<std::wstring> favoritePathKeys;

    bool ignDraftDirty = false;
    bool generalDraftDirty = false;
    std::chrono::steady_clock::time_point ignLastEdit = std::chrono::steady_clock::time_point::min();
    std::chrono::steady_clock::time_point generalLastEdit = std::chrono::steady_clock::time_point::min();
    std::chrono::steady_clock::time_point lastAutoRefresh = std::chrono::steady_clock::time_point::min();

    std::string statusText;
    std::chrono::steady_clock::time_point statusUntil = std::chrono::steady_clock::time_point::min();

    std::filesystem::path pendingDeletePath;
    std::string pendingDeleteLabel;
    bool pendingDeleteIsIgn = false;
    bool pendingDeleteOpenPopup = false;

    std::filesystem::path pendingSaveConflictTargetPath;
    std::filesystem::path pendingSaveConflictCurrentPath;
    std::string pendingSaveConflictTitle;
    std::string pendingSaveConflictDraft;
    bool pendingSaveConflictIsPdf = false;
    bool pendingSaveConflictOpenPopup = false;
};

std::mutex s_notesMutex;
NotesOverlayState s_notes;
std::atomic<bool> s_pendingIgnAutoSaveOnClose{ false };

constexpr const char* kGeneralFolderRoot = "";
constexpr const char* kGeneralFolderFavorites = "__favorites__";

std::filesystem::path BuildUniqueFilePath(const std::filesystem::path& folder, const std::string& fileBase, const std::string& extWithDot);
std::string GuessTitleFromPath(const std::filesystem::path& path);

struct NotesIconTexture {
    GLuint textureId = 0;
    int width = 0;
    int height = 0;
    bool attemptedLoad = false;
};

NotesIconTexture s_pinIcon;
NotesIconTexture s_starIcon;

bool WriteUtf8TextFile(const std::filesystem::path& path, const std::string& text);

bool IsInWorldGameState(const std::string& gameState) { return gameState.find("inworld") != std::string::npos; }

bool IsInWorldNow() {
    const std::string gameState = g_gameStateBuffers[g_currentGameStateIndex.load(std::memory_order_acquire)];
    return IsInWorldGameState(gameState);
}

std::string ToLowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::wstring ToLowerWideAscii(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return text;
}

bool PathsEquivalentLoose(const std::filesystem::path& a, const std::filesystem::path& b) {
    try {
        if (std::filesystem::exists(a) && std::filesystem::exists(b)) { return std::filesystem::equivalent(a, b); }
    } catch (...) {}
    return ToLowerWideAscii(a.lexically_normal().wstring()) == ToLowerWideAscii(b.lexically_normal().wstring());
}

bool HasMeaningfulText(const std::string& text) {
    for (unsigned char c : text) {
        if (!std::isspace(c)) return true;
    }
    return false;
}

std::string TrimAscii(const std::string& text) {
    size_t first = 0;
    while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first]))) {
        ++first;
    }
    size_t last = text.size();
    while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1]))) {
        --last;
    }
    return text.substr(first, last - first);
}

std::string SanitizeFileComponent(std::string text) {
    text = TrimAscii(text);
    for (char& c : text) {
        switch (c) {
        case '<':
        case '>':
        case ':':
        case '"':
        case '/':
        case '\\':
        case '|':
        case '?':
        case '*':
            c = '_';
            break;
        default:
            break;
        }
    }

    while (!text.empty() && (text.back() == '.' || text.back() == ' ')) {
        text.pop_back();
    }
    while (!text.empty() && (text.front() == '.' || text.front() == ' ')) {
        text.erase(text.begin());
    }

    if (text.empty()) return "note";
    return text;
}

std::string FormatDateLocal(std::chrono::system_clock::time_point tp, const char* fmt) {
    std::time_t tt = std::chrono::system_clock::to_time_t(tp);
    std::tm localTm{};
#if defined(_WIN32)
    localtime_s(&localTm, &tt);
#else
    localTm = *std::localtime(&tt);
#endif
    std::ostringstream out;
    out << std::put_time(&localTm, fmt);
    return out.str();
}

std::string CurrentDateStamp() { return FormatDateLocal(std::chrono::system_clock::now(), "%Y-%m-%d"); }

std::string CurrentTimeStamp() { return FormatDateLocal(std::chrono::system_clock::now(), "%H%M%S"); }

std::string FormatEpochForList(int64_t epochSeconds) {
    if (epochSeconds <= 0) return "";
    std::time_t tt = static_cast<std::time_t>(epochSeconds);
    std::tm localTm{};
#if defined(_WIN32)
    localtime_s(&localTm, &tt);
#else
    localTm = *std::localtime(&tt);
#endif
    std::ostringstream out;
    out << std::put_time(&localTm, "%Y-%m-%d %H:%M");
    return out.str();
}

int64_t ToEpochSeconds(const std::filesystem::file_time_type& fileTime) {
    using namespace std::chrono;
    const auto sysNow = system_clock::now();
    const auto fileNow = std::filesystem::file_time_type::clock::now();
    const auto sysTp = time_point_cast<system_clock::duration>(fileTime - fileNow + sysNow);
    return duration_cast<seconds>(sysTp.time_since_epoch()).count();
}

uint64_t ExtractFirstNumberKey(const std::string& text) {
    uint64_t value = 0;
    bool found = false;
    for (size_t i = 0; i < text.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) continue;
        found = true;
        size_t j = i;
        while (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j]))) {
            const uint64_t digit = static_cast<uint64_t>(text[j] - '0');
            if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10ull) {
                value = std::numeric_limits<uint64_t>::max();
                break;
            }
            value = value * 10ull + digit;
            ++j;
        }
        break;
    }
    return found ? value : std::numeric_limits<uint64_t>::max();
}

std::filesystem::path GetToolscreenRootPath() {
    if (!g_toolscreenPath.empty()) { return std::filesystem::path(g_toolscreenPath); }
    return std::filesystem::current_path() / "toolscreen";
}

std::filesystem::path ResolveConfiguredPath(const std::string& configuredPathUtf8, const std::filesystem::path& fallbackRelativePath) {
    const std::string trimmed = TrimAscii(configuredPathUtf8);
    const std::filesystem::path basePath = GetToolscreenRootPath();
    if (trimmed.empty()) { return basePath / fallbackRelativePath; }

    std::filesystem::path configuredPath = Utf8ToWide(trimmed);
    if (configuredPath.is_absolute() || configuredPath.has_root_name()) { return configuredPath; }
    return basePath / configuredPath;
}

std::filesystem::path GetMarkdownNotesRootPath() {
    auto cfgSnap = GetConfigSnapshot();
    const std::string configuredPath = cfgSnap ? cfgSnap->notesOverlay.markdownDirectory : "";
    return ResolveConfiguredPath(configuredPath, std::filesystem::path("notes") / "General");
}

std::filesystem::path GetGeneralNotesRootPath() { return GetMarkdownNotesRootPath(); }

std::filesystem::path GetIgnNotesRootPath() { return GetMarkdownNotesRootPath() / L"IGN"; }
std::filesystem::path GetQuickstartNotesRootPath() { return GetMarkdownNotesRootPath() / L"Quickstart"; }

const char* GetDefaultQuickstartMarkdown() {
    return R"MD(# Minecraft Speedrunning - MCSR Quick Start (1.16.1)

> Practical reference + training checklist for modern MCSR Ranked play.
> Focus: consistency, routing decisions, and execution fundamentals.

---

# Core Philosophy

* Speedrunning is decision speed + execution consistency, not raw mechanics.
* Avoid resets caused by hesitation.
* Play seeds systematically, not reactively.
* Always be progressing toward the next split.

---

# Run Flow Overview

1. Spawn Overworld
2. Loot + Setup
3. Enter Nether (Fast Portal)
4. Bastion First (Gold + Pearls)
5. Fortress (Blaze Rods)
6. Exit Nether (Blind from nether) -> Locate Strong hold (Double eye or Boat eye)
7. Re-Enter Nether and go to Triangulated Coords
8. Portal to Stronghold Entry
9. End Fight + Finish

---

# Overworld (Early Game)

## Goals

- [ ] Food source (7 Haybales, Sheep, Cows, Pigs, Chicken, Cooked Fish, Rotten Flesh, Gapples, Chest Loot)
- [ ] Gather Wood 10-16
- [ ] Bucket
- [ ] Flint and steel or leave for lava ignite
- [ ] Iron tools
- [ ] Sheers for blocks/wool if enough iron
- [ ] Doors if underwater portal / find ravine
- [ ] Make/Complete Portal
- [ ] Light and go to Nether

## Key Techniques

* Sprint-jump routing between structures
* Scan horizon while moving (never stop to look)
* Pre-plan crafting during movement
* You can craft two items at the same time for instance if you place enough materials to craft both items, place them in the right spot, and press right and left mouse button at the same time.
* If you are fighting a golem with a shovel for iron look around for 1 jump (it takes two jumps to crit)
* You need to be falling to crit in minecraft
* Rivers are often between biomes (gravel for flint is often in rivers)

## Reference

* Overworld Guide: [https://www.youtube.com/watch?v=egyiA_8FztM](https://www.youtube.com/watch?v=egyiA_8FztM)
* Village Guide: [https://youtu.be/N3EME1E431U?si=Jx8U7wE2Uljq76HL](https://youtu.be/N3EME1E431U?si=Jx8U7wE2Uljq76HL)

---

# Underwater / Fast Portals

## Goals

* Enter Nether under 4 minutes
* Safe lava access

## Checklist

> The order of these is to be generally followed but change on a run by run basis
- [ ] Food
- [ ] Wood acquired
- [ ] Craft Doors
- [ ] Craft Boat
- [ ] Craft Bucket
- [ ] Iron Tools
- [ ] Find gravel to get flint
- [ ] Flint and Steel
- [ ] Lava pool located
- [ ] Portal built cleanly

## Techniques

* Place water before lava manipulation
* Practice blind portal building muscle memory

## Reference

* Underwater Portals: [https://youtu.be/FD798osoq0o?si=X8vkXed39kj3Fz8G](https://youtu.be/FD798osoq0o?si=X8vkXed39kj3Fz8G)

---

# Nether Overview

> Bastion first unless spawn strongly favors fortress.

## Immediate Actions

* E-Ray for Bastion and Scan immediate area (You can leverage skinny and wide view for this)
  * Side note: Turn down render distance a little so you do not get a far away Bastion (16-20 is usually a good default anyway)
* Get building materials if necesary
* Kill Piggies with lava bucket if absolutely necessary for food (Won't be usually)

## Checklist

- [ ] Gold armor equipped
- [ ] Safe navigation blocks ready
- [ ] Bastion identified

---

# Bastions (Primary Study Section)

## Goals

* Identify Bastion Type
* Start Bastion Route
* Get gold and Piggles (Piglins) in Trade Hole
* Loot Other chests that you have not looted
* Pre-Emptive if necessary
  > Note: You cannot pre-emptive on Treasure until you've broken the Mob Spawner
* Obtain Pearls (At least 1 stack), Obsidian (<=20 blocks), String (At least 1 stack), Fire Resistance Potion (At least 2)
* Craft what you need (Beds, Iron Axe) and sort inventory in-between trades

## Bastion Types

* Housing
* Bridge
* Treasure
* Stables

> In Treasure the aforementioned mob spawner lives at the bottom center of the building across from where you trade with the Piggles. You can go to it via the bridge, turn around, throw a pearl, escape, turn your render distance down to two (This is called pearl hanging, look it up), then use a boat to get down to the spawner to break it so you can pre-emptive, and then finally turn your render distance back up to return to your trades.

## Core Rules

* You need gold armor and gold to distract and travel safely
* You need an iron pick to mine gold blocks
* Get your trades going ASAP
* You need the core items (Pearls, Obsidian, String, Fire Res), but when you have them LEAVE

## Practice Focus

* Bastion Types
* Route memorization
* Piglin aggro control
* Fast inventory management

## Reference

* Bastions: [https://www.youtube.com/watch?v=CRwiJcWWUlY&t=4683s](https://www.youtube.com/watch?v=CRwiJcWWUlY&t=4683s)

---

# Nether Fortress

## Goals

* 6-7 blaze rods minimum

## Strategy

* Locate via terrain scanning + sound cues
* Use safe pillar combat
* Control blaze line-of-sight

## Checklist

- [ ] Blaze rods acquired
- [ ] Blind (That's it)

## Reference

* Nether Fortress: [https://www.youtube.com/watch?v=JsFcAeBXVpk](https://www.youtube.com/watch?v=JsFcAeBXVpk)

---

# Stronghold Location
## Process

1. Craft eyes
2. Get on top of portal / obtain solid line of sight / just use immediate area
3. Boat eye or Double Eye
4. Get Valid Coords (Usually at least 80% Accuracy)
5. Go back to Nether
6. Go to Stronghold Nether Coords
7. Build portal and Blind

## Notes

* Maintain forward momentum between throws
* Avoid over-throwing eyes
* Pop a fire res if you can

---

# Stronghold Nav

## Goals

- [ ] If you are not at starter get there (Follow video tutortial for this it's easy but hard to convey textually)
- [ ] Pie-dar to find End Portal with skinny view, render distance 8, entity distance lowest value, pie-chart root.gameRenderer.entities, look for large blockEntitiy values
- [ ] Get to portal, if you have not made your beds make them and organize inventory/hotbar
- [ ] Fill in portal and go

# The End (Execution Phase)

## Goals

* Defeat that MF LADY DRAGON BOI

## Fight Flow

1. Render distance back up to max if possible and Entity Distance max
2. Turn on Entity Hitboxes (F3 + B)
3. Pearl to Center Island
4. Setup One Cycle
5. Turn Right and Go 70 Blocks
6. Wait for Miss Dragon to SNAP towards the center
7. Pearl in if you have enough health (if not wait closer to center and run in)
8. One Cycle her ahh
9. Build up two blocks to avoid endermen if you are a pansy
10. You WON!!!!!

## Common Errors

> Standing directly center during perch
> Poor bed timing
> Looking at an enderman
> Pearling in too soon / too late
> Moving around too much

## Notes

> If you aggro'd an enderman, do not freak out, place a block, place a boat on the block, let the enderman run into the boat
> When Miss Dragon shoots a missile at you, wait for the missile to explode, then wait a moment, jump up and place a block under you and you're safe
> If you miss your beds, but Miss Dragon is low HP, go to the corner where her head is, sit there (DO NOT JUMP), and smack her head hitbox with your axe
> If you miss a perch the dragon will shoot out in your direction so be ready (if she hits you she launches you upwards)
> Pop a fire res if you have time to be extra safe
> Try to move as little as possible to avoid wasting hp, hunger, and just causing general confusion / anxiety
> Try to look higher or lower than where endermen will be, preferably higher

## References

* End Guide: [https://youtu.be/4It26dOki7g?si=7YOh4XOY-KN6ZEwx](https://youtu.be/4It26dOki7g?si=7YOh4XOY-KN6ZEwx)
* End Mechanics: [https://youtu.be/Gp7Qsab8JNY?si=sc4iCQtWqhJAIDKz](https://youtu.be/Gp7Qsab8JNY?si=sc4iCQtWqhJAIDKz)
)MD";
}

void EnsureQuickstartSeedNote() {
    try {
        const std::filesystem::path folder = GetQuickstartNotesRootPath();
        std::filesystem::create_directories(folder);
        const std::filesystem::path seedMarkerPath = folder / L".toolscreen_quickstart_seed_v2_done";
        if (std::filesystem::exists(seedMarkerPath)) return;

        const std::filesystem::path notePath = folder / L"Minecraft Speedrunning - MCSR Quick Start (1.16.1).md";
        if (!std::filesystem::exists(notePath)) { WriteUtf8TextFile(notePath, GetDefaultQuickstartMarkdown()); }

        // Mark bootstrap as completed so user deletes/replacements are respected.
        WriteUtf8TextFile(seedMarkerPath, "seed_version=2\n");
    } catch (...) {}
}

std::filesystem::path GetPdfExportRootPath() {
    auto cfgSnap = GetConfigSnapshot();
    const std::string configuredPath = cfgSnap ? cfgSnap->notesOverlay.pdfDirectory : "";
    return ResolveConfiguredPath(configuredPath, std::filesystem::path("notes") / "PDF");
}

void EnsureNotesDirectories() {
    try {
        std::filesystem::create_directories(GetGeneralNotesRootPath());
        std::filesystem::create_directories(GetIgnNotesRootPath());
        std::filesystem::create_directories(GetQuickstartNotesRootPath());
        std::filesystem::create_directories(GetPdfExportRootPath());
        EnsureQuickstartSeedNote();
    } catch (...) {}
}

bool WriteUtf8TextFile(const std::filesystem::path& path, const std::string& text) {
    try {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        return out.good();
    } catch (...) {
        return false;
    }
}

bool WriteBinaryFile(const std::filesystem::path& path, const std::string& data) {
    try {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.flush();
        return out.good();
    } catch (...) {
        return false;
    }
}

bool ReadUtf8TextFile(const std::filesystem::path& path, std::string& outText) {
    outText.clear();
    try {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) return false;
        std::ostringstream ss;
        ss << in.rdbuf();
        outText = ss.str();
        return true;
    } catch (...) {
        return false;
    }
}

void SetStatus(NotesOverlayState& st, const std::string& text) {
    st.statusText = text;
    st.statusUntil = std::chrono::steady_clock::now() + std::chrono::seconds(4);
}

void EnsureStorageDraftInitialized(NotesOverlayState& st, const Config& cfg) {
    if (st.storageDraftInitialized) return;
    st.markdownDirDraft = cfg.notesOverlay.markdownDirectory;
    st.pdfDirDraft = cfg.notesOverlay.pdfDirectory;
    st.storageDraftInitialized = true;
}

void ApplyStorageDraft(NotesOverlayState& st) {
    std::string markdownDir = TrimAscii(st.markdownDirDraft);
    std::string pdfDir = TrimAscii(st.pdfDirDraft);
    if (markdownDir.empty()) markdownDir = "notes/General";
    if (pdfDir.empty()) pdfDir = "notes/PDF";

    const bool changed = (g_config.notesOverlay.markdownDirectory != markdownDir) || (g_config.notesOverlay.pdfDirectory != pdfDir);
    st.markdownDirDraft = markdownDir;
    st.pdfDirDraft = pdfDir;
    if (!changed) {
        SetStatus(st, "Storage paths unchanged.");
        return;
    }

    g_config.notesOverlay.markdownDirectory = markdownDir;
    g_config.notesOverlay.pdfDirectory = pdfDir;
    g_configIsDirty = true;
    PublishConfigSnapshot();
    EnsureNotesDirectories();
    st.refreshRequested = true;
    SetStatus(st, "Saved storage paths.");
}

std::string PathForDisplay(const std::filesystem::path& path) {
    try {
        return WideToUtf8(path.lexically_normal().wstring());
    } catch (...) {
        return WideToUtf8(path.wstring());
    }
}

std::filesystem::path GetNotesPinnedMetaPath() { return GetMarkdownNotesRootPath() / L".toolscreen_pins.txt"; }
std::filesystem::path GetNotesFavoritesMetaPath() { return GetMarkdownNotesRootPath() / L".toolscreen_favorites.txt"; }

std::wstring NormalizePathKey(const std::filesystem::path& path) {
    try {
        return ToLowerWideAscii(path.lexically_normal().wstring());
    } catch (...) {
        return ToLowerWideAscii(path.wstring());
    }
}

void LoadPathSetMetadata(const std::filesystem::path& metaPath, std::set<std::wstring>& outSet) {
    outSet.clear();
    std::string raw;
    if (!ReadUtf8TextFile(metaPath, raw)) return;

    std::istringstream in(raw);
    std::string line;
    while (std::getline(in, line)) {
        line = TrimAscii(line);
        if (line.empty()) continue;
        try {
            std::filesystem::path p = Utf8ToWide(line);
            if (p.empty()) continue;
            outSet.insert(NormalizePathKey(p));
        } catch (...) {}
    }
}

void SavePathSetMetadata(const std::filesystem::path& metaPath, const std::set<std::wstring>& keys) {
    std::string out;
    out.reserve(keys.size() * 96);
    for (const auto& key : keys) {
        out += WideToUtf8(key);
        out.push_back('\n');
    }
    WriteUtf8TextFile(metaPath, out);
}

void LoadPinnedMetadata(NotesOverlayState& st) {
    LoadPathSetMetadata(GetNotesPinnedMetaPath(), st.pinnedPathKeys);
}

void LoadFavoriteMetadata(NotesOverlayState& st) {
    LoadPathSetMetadata(GetNotesFavoritesMetaPath(), st.favoritePathKeys);
}

void SavePinnedMetadata(const NotesOverlayState& st) { SavePathSetMetadata(GetNotesPinnedMetaPath(), st.pinnedPathKeys); }
void SaveFavoriteMetadata(const NotesOverlayState& st) { SavePathSetMetadata(GetNotesFavoritesMetaPath(), st.favoritePathKeys); }

bool IsPathPinned(const NotesOverlayState& st, const std::filesystem::path& path) {
    if (path.empty()) return false;
    return st.pinnedPathKeys.find(NormalizePathKey(path)) != st.pinnedPathKeys.end();
}

bool IsPathFavorited(const NotesOverlayState& st, const std::filesystem::path& path) {
    if (path.empty()) return false;
    return st.favoritePathKeys.find(NormalizePathKey(path)) != st.favoritePathKeys.end();
}

bool SetPathPinned(NotesOverlayState& st, const std::filesystem::path& path, bool pinned) {
    if (path.empty()) return false;
    const std::wstring key = NormalizePathKey(path);
    if (key.empty()) return false;
    if (pinned) {
        st.pinnedPathKeys.insert(key);
    } else {
        st.pinnedPathKeys.erase(key);
    }
    SavePinnedMetadata(st);
    st.refreshRequested = true;
    return true;
}

bool SetPathFavorited(NotesOverlayState& st, const std::filesystem::path& path, bool favorited) {
    if (path.empty()) return false;
    const std::wstring key = NormalizePathKey(path);
    if (key.empty()) return false;
    if (favorited) {
        st.favoritePathKeys.insert(key);
    } else {
        st.favoritePathKeys.erase(key);
    }
    SaveFavoriteMetadata(st);
    st.refreshRequested = true;
    return true;
}

bool IsGeneralFavoritesFolderKey(const std::string& key) { return key == kGeneralFolderFavorites; }

std::string GeneralFolderDisplayLabel(const std::string& key) {
    if (key.empty()) return "General";
    if (IsGeneralFavoritesFolderKey(key)) return "Favorites";
    return key;
}

std::filesystem::path FindNotesIconPath(const wchar_t* filename) {
    if (!filename || *filename == 0) return std::filesystem::path();

    std::filesystem::path probe = GetToolscreenRootPath();
    for (int i = 0; i < 6; ++i) {
        const std::filesystem::path candidate = probe / filename;
        if (std::filesystem::exists(candidate)) return candidate;
        if (!probe.has_parent_path()) break;
        probe = probe.parent_path();
    }

    const std::filesystem::path hardcoded = std::filesystem::path(L"C:\\Users\\Tim\\Desktop\\msr") / filename;
    if (std::filesystem::exists(hardcoded)) return hardcoded;
    return std::filesystem::path();
}

bool LoadIconTexture(NotesIconTexture& icon, const std::filesystem::path& path) {
    if (path.empty()) return false;
    try {
        const std::string pathUtf8 = WideToUtf8(path.wstring());
        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_set_flip_vertically_on_load(false);
        unsigned char* data = stbi_load(pathUtf8.c_str(), &width, &height, &channels, 4);
        stbi_set_flip_vertically_on_load(true);
        if (!data || width <= 0 || height <= 0) {
            if (data) stbi_image_free(data);
            return false;
        }

        if (icon.textureId != 0) {
            glDeleteTextures(1, &icon.textureId);
            icon.textureId = 0;
        }

        glGenTextures(1, &icon.textureId);
        glBindTexture(GL_TEXTURE_2D, icon.textureId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
        glBindTexture(GL_TEXTURE_2D, 0);

        stbi_image_free(data);
        icon.width = width;
        icon.height = height;
        return true;
    } catch (...) {
        return false;
    }
}

void EnsureNotesIconTexturesLoaded() {
    if (!s_pinIcon.attemptedLoad) {
        s_pinIcon.attemptedLoad = true;
        LoadIconTexture(s_pinIcon, FindNotesIconPath(L"pin.png"));
    }
    if (!s_starIcon.attemptedLoad) {
        s_starIcon.attemptedLoad = true;
        LoadIconTexture(s_starIcon, FindNotesIconPath(L"star.png"));
    }
}

bool RenderIconToggleButton(const char* id, const NotesIconTexture& icon, bool active, const char* fallbackLabel, const char* hintText) {
    const float size = 18.0f;
    bool pressed = false;

    if (icon.textureId != 0) {
        const ImVec4 tint = active ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f) : ImVec4(0.36f, 0.36f, 0.36f, 0.86f);
        ImTextureRef texRef((void*)(intptr_t)icon.textureId);
        const ImVec4 activeBg = ImVec4(0.72f, 0.62f, 0.20f, 0.34f);
        const ImVec4 idleBg = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, active ? activeBg : idleBg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? ImVec4(0.78f, 0.68f, 0.22f, 0.44f) : ImVec4(1, 1, 1, 0.08f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, active ? ImVec4(0.84f, 0.74f, 0.24f, 0.52f) : ImVec4(1, 1, 1, 0.14f));
        pressed = ImGui::ImageButton(id, texRef, ImVec2(size, size), ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0), tint);
        if (active) {
            if (ImDrawList* dl = ImGui::GetWindowDrawList()) {
                const ImVec2 r0 = ImGui::GetItemRectMin();
                const ImVec2 r1 = ImGui::GetItemRectMax();
                dl->AddRect(r0, r1, IM_COL32(236, 214, 108, 240), 3.0f, 0, 1.4f);
            }
        }
        ImGui::PopStyleColor(3);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, active ? ImVec4(0.95f, 0.95f, 0.95f, 1.0f) : ImVec4(0.48f, 0.48f, 0.48f, 1.0f));
        pressed = ImGui::SmallButton(fallbackLabel);
        ImGui::PopStyleColor();
    }

    if (ImGui::IsItemHovered() && hintText && *hintText) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(hintText);
        ImGui::EndTooltip();
    }
    return pressed;
}

std::string FormatNotesHotkeyLabel(const NotesOverlayConfig& cfg) {
    std::string label;
    if (cfg.hotkeyCtrl) label += "Ctrl+";
    if (cfg.hotkeyShift) label += "Shift+";
    if (cfg.hotkeyAlt) label += "Alt+";

    const int vk = std::clamp(cfg.hotkeyKey, 1, 255);
    std::string keyLabel = VkToString(static_cast<DWORD>(vk));
    if (keyLabel.empty() || keyLabel == "[None]") { keyLabel = "N"; }
    label += keyLabel;
    return label;
}

std::filesystem::path BuildUniqueFilePath(const std::filesystem::path& folder, const std::string& fileBase, const std::string& extWithDot) {
    std::filesystem::path target = folder / Utf8ToWide(fileBase + extWithDot);
    int suffix = 1;
    while (std::filesystem::exists(target) && suffix < 100000) {
        target = folder / Utf8ToWide(fileBase + "_" + std::to_string(suffix) + extWithDot);
        ++suffix;
    }
    return target;
}

bool OpenFolderContainingPath(const std::filesystem::path& path) {
    try {
        std::filesystem::path folder = path.parent_path();
        if (folder.empty()) return false;
        HINSTANCE result = ShellExecuteW(nullptr, L"open", folder.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return reinterpret_cast<intptr_t>(result) > 32;
    } catch (...) {
        return false;
    }
}

std::string GuessTitleFromPath(const std::filesystem::path& path) {
    if (path.empty()) return "";
    try {
        return WideToUtf8(path.stem().wstring());
    } catch (...) {
        return "";
    }
}

std::string ExtractMarkdownTitle(const std::string& markdownText) {
    std::istringstream in(markdownText);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string trimmed = TrimAscii(line);
        if (trimmed.empty()) continue;
        if (trimmed.rfind("# ", 0) == 0) { return TrimAscii(trimmed.substr(2)); }
        break;
    }
    return "";
}

void UpsertMarkdownTitle(std::string& markdownText, const std::string& title) {
    const std::string normalizedTitle = TrimAscii(title);
    const std::string heading = "# " + (normalizedTitle.empty() ? "untitled" : normalizedTitle);

    std::vector<std::string> lines;
    lines.reserve(64);
    std::istringstream in(markdownText);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }

    if (lines.empty()) {
        markdownText = heading + "\n\n";
        return;
    }

    size_t firstMeaningful = std::numeric_limits<size_t>::max();
    for (size_t i = 0; i < lines.size(); ++i) {
        if (HasMeaningfulText(lines[i])) {
            firstMeaningful = i;
            break;
        }
    }

    if (firstMeaningful == std::numeric_limits<size_t>::max()) {
        markdownText = heading + "\n\n";
        return;
    }

    if (TrimAscii(lines[firstMeaningful]).rfind("# ", 0) == 0) {
        lines[firstMeaningful] = heading;
    } else {
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(firstMeaningful), "");
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(firstMeaningful), heading);
    }

    const bool hadTrailingNewline = !markdownText.empty() && markdownText.back() == '\n';
    std::string rebuilt;
    rebuilt.reserve(markdownText.size() + 32);
    for (size_t i = 0; i < lines.size(); ++i) {
        rebuilt += lines[i];
        if (i + 1 < lines.size() || hadTrailingNewline) rebuilt.push_back('\n');
    }
    markdownText.swap(rebuilt);
}

std::string BuildDefaultNewNoteMarkdown(const std::string& title) {
    const std::string normalizedTitle = SanitizeFileComponent(TrimAscii(title));
    const std::string finalTitle = normalizedTitle.empty() ? "untitled" : normalizedTitle;
    return "# " + finalTitle + "\n---\n\n";
}

void EnsureRuleUnderTopHeading(std::string& markdownText) {
    std::vector<std::string> lines;
    lines.reserve(64);
    std::istringstream in(markdownText);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    if (lines.empty()) return;

    size_t firstMeaningful = std::numeric_limits<size_t>::max();
    for (size_t i = 0; i < lines.size(); ++i) {
        if (HasMeaningfulText(lines[i])) {
            firstMeaningful = i;
            break;
        }
    }
    if (firstMeaningful == std::numeric_limits<size_t>::max()) return;
    if (TrimAscii(lines[firstMeaningful]).rfind("# ", 0) != 0) return;

    size_t scan = firstMeaningful + 1;
    while (scan < lines.size() && TrimAscii(lines[scan]).empty()) {
        ++scan;
    }
    if (scan < lines.size() && TrimAscii(lines[scan]) == "---") return;

    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(firstMeaningful + 1), "---");
    if (firstMeaningful + 2 >= lines.size() || HasMeaningfulText(lines[firstMeaningful + 2])) {
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(firstMeaningful + 2), "");
    }

    const bool hadTrailingNewline = !markdownText.empty() && markdownText.back() == '\n';
    std::string rebuilt;
    rebuilt.reserve(markdownText.size() + 12);
    for (size_t i = 0; i < lines.size(); ++i) {
        rebuilt += lines[i];
        if (i + 1 < lines.size() || hadTrailingNewline) rebuilt.push_back('\n');
    }
    markdownText.swap(rebuilt);
}

std::string BuildNextUntitledTitle(const std::filesystem::path& folder) {
    std::string base = "untitled";
    int index = 1;
    while (index < 100000) {
        const std::string candidate = base + "_" + std::to_string(index);
        const std::filesystem::path p = folder / Utf8ToWide(candidate + ".md");
        if (!std::filesystem::exists(p)) return candidate;
        ++index;
    }
    return "untitled_" + CurrentDateStamp() + "_" + CurrentTimeStamp();
}

void MarkIgnDraftDirty(NotesOverlayState& st) {
    st.ignEditedSinceOpen = true;
    st.ignDraftDirty = true;
    st.ignLastEdit = std::chrono::steady_clock::now();
}

void MarkGeneralDraftDirty(NotesOverlayState& st) {
    st.generalDraftDirty = true;
    st.generalLastEdit = std::chrono::steady_clock::now();
}

std::string ResolveIgnPreviewTitle(const NotesOverlayState& st) {
    if (st.selectedIgnEntryIndex >= 0 && st.selectedIgnEntryIndex < static_cast<int>(st.ignEntries.size())) {
        const std::string& candidate = st.ignEntries[static_cast<size_t>(st.selectedIgnEntryIndex)].title;
        if (HasMeaningfulText(candidate)) return TrimAscii(candidate);
    }
    if (!st.ignEditingPath.empty()) {
        const std::string byPath = GuessTitleFromPath(st.ignEditingPath);
        if (HasMeaningfulText(byPath)) return TrimAscii(byPath);
    }
    return "IGN Note";
}

std::string ResolveGeneralPreviewTitle(const NotesOverlayState& st) {
    if (HasMeaningfulText(st.generalTitle)) return TrimAscii(st.generalTitle);
    if (!st.generalEditingPath.empty()) {
        const std::string byPath = GuessTitleFromPath(st.generalEditingPath);
        if (HasMeaningfulText(byPath)) return TrimAscii(byPath);
    }
    return "General Note";
}

enum class MarkdownLineKind : int { Blank = 0, Rule, Heading, Quote, Bullet, Numbered, Task, Code, Body };

struct MarkdownPreviewLine {
    MarkdownLineKind kind = MarkdownLineKind::Body;
    std::string text;
    int headingLevel = 0; // 1..6 when kind == Heading
    std::string listPrefix;
    bool checked = false; // used when kind == Task
    size_t sourceLineIndex = std::numeric_limits<size_t>::max();
};

bool IsMarkdownHorizontalRule(const std::string& line) {
    return line == "---" || line == "***" || line == "___";
}

bool IsMarkdownNumberedListLine(const std::string& trimmedLine, std::string& outPrefix, std::string& outValue) {
    size_t i = 0;
    while (i < trimmedLine.size() && std::isdigit(static_cast<unsigned char>(trimmedLine[i]))) {
        ++i;
    }
    if (i == 0 || i + 1 >= trimmedLine.size()) return false;
    const char sep = trimmedLine[i];
    if (sep != '.' && sep != ')') return false;
    if (trimmedLine[i + 1] != ' ') return false;
    outPrefix = trimmedLine.substr(0, i) + ". ";
    outValue = trimmedLine.substr(i + 2);
    return true;
}

bool IsMarkdownTaskMarker(const std::string& text, bool& outChecked, std::string& outBody) {
    if (text.size() < 4 || text[0] != '[') return false;
    const char mark = static_cast<char>(std::tolower(static_cast<unsigned char>(text[1])));
    if (text[2] != ']' || text[3] != ' ') return false;
    if (mark != 'x' && mark != ' ') return false;
    outChecked = (mark == 'x');
    outBody = text.substr(4);
    return true;
}

size_t CountLeadingIndentColumns(const std::string& line) {
    size_t columns = 0;
    for (char c : line) {
        if (c == ' ') {
            ++columns;
            continue;
        }
        if (c == '\t') {
            columns += 4;
            continue;
        }
        break;
    }
    return columns;
}

std::string TrimLeftAscii(const std::string& text) {
    size_t first = 0;
    while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first]))) {
        ++first;
    }
    return text.substr(first);
}

const char* GetRenderedBulletPrefix() { return "\xE2\x80\xA2 "; }

bool BuildNextMarkdownListPrefix(const std::string& rawLine, std::string& outPrefix) {
    outPrefix.clear();
    if (rawLine.empty()) return false;

    size_t end = rawLine.size();
    while (end > 0) {
        const char c = rawLine[end - 1];
        if (c == ' ' || c == '\t' || c == '\r') {
            --end;
        } else {
            break;
        }
    }
    const std::string line = rawLine.substr(0, end);
    if (line.empty()) return false;

    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
        ++i;
    }
    const std::string indent = line.substr(0, i);
    if (i >= line.size()) return false;

    auto hasBody = [&](size_t start) { return HasMeaningfulText(line.substr(start)); };

    const char marker = line[i];
    if ((marker == '-' || marker == '*' || marker == '+') && i + 1 < line.size() && line[i + 1] == ' ') {
        if (i + 5 < line.size() && line[i + 2] == '[' && line[i + 4] == ']' && line[i + 5] == ' ') {
            const char check = static_cast<char>(std::tolower(static_cast<unsigned char>(line[i + 3])));
            if (check == ' ' || check == 'x') {
                if (!hasBody(i + 6)) return false;
                outPrefix = indent + std::string(1, marker) + " [ ] ";
                return true;
            }
        }
        if (!hasBody(i + 2)) return false;
        outPrefix = indent + std::string(1, marker) + " ";
        return true;
    }

    if (marker == '>' && i + 1 < line.size() && line[i + 1] == ' ') {
        if (!hasBody(i + 2)) return false;
        outPrefix = indent + "> ";
        return true;
    }

    size_t j = i;
    while (j < line.size() && std::isdigit(static_cast<unsigned char>(line[j]))) {
        ++j;
    }
    if (j > i && j + 1 < line.size() && (line[j] == '.' || line[j] == ')') && line[j + 1] == ' ') {
        if (!hasBody(j + 2)) return false;
        unsigned long long value = 0;
        for (size_t k = i; k < j; ++k) {
            const unsigned long long digit = static_cast<unsigned long long>(line[k] - '0');
            if (value > (std::numeric_limits<unsigned long long>::max() - digit) / 10ull) {
                value = std::numeric_limits<unsigned long long>::max();
                break;
            }
            value = value * 10ull + digit;
        }
        if (value < std::numeric_limits<unsigned long long>::max()) {
            ++value;
        }
        outPrefix = indent + std::to_string(value) + ". ";
        return true;
    }

    return false;
}

bool ApplyAutoListContinuation(std::string& text, size_t previousSize) {
    if (text.size() != previousSize + 1 || text.empty() || text.back() != '\n') return false;

    const size_t lineEnd = text.size() - 1;
    size_t lineStart = std::string::npos;
    if (lineEnd > 0) {
        lineStart = text.rfind('\n', lineEnd - 1);
    }
    if (lineStart == std::string::npos) {
        lineStart = 0;
    } else {
        ++lineStart;
    }

    const std::string previousLine = text.substr(lineStart, lineEnd - lineStart);
    std::string continuation;
    if (!BuildNextMarkdownListPrefix(previousLine, continuation) || continuation.empty()) return false;
    text += continuation;
    return true;
}

std::vector<MarkdownPreviewLine> ParseMarkdownPreviewLines(const std::string& markdownText) {
    std::vector<MarkdownPreviewLine> out;
    std::istringstream in(markdownText);
    std::string rawLine;
    bool inCodeFence = false;
    size_t sourceLineIndex = 0;

    auto pushLine = [&](MarkdownLineKind kind, std::string text, int headingLevel, std::string listPrefix, bool checked) {
        MarkdownPreviewLine line;
        line.kind = kind;
        line.text = std::move(text);
        line.headingLevel = headingLevel;
        line.listPrefix = std::move(listPrefix);
        line.checked = checked;
        line.sourceLineIndex = sourceLineIndex;
        out.push_back(std::move(line));
    };

    while (std::getline(in, rawLine)) {
        if (!rawLine.empty() && rawLine.back() == '\r') rawLine.pop_back();
        const std::string trimmed = TrimAscii(rawLine);
        const std::string leadingTrimmed = TrimLeftAscii(rawLine);
        const size_t indentColumns = CountLeadingIndentColumns(rawLine);
        const size_t listIndent = std::min<size_t>(32, (indentColumns / 2) * 2);

        if (trimmed.rfind("```", 0) == 0) {
            inCodeFence = !inCodeFence;
            pushLine(MarkdownLineKind::Rule, "", 0, "", false);
            ++sourceLineIndex;
            continue;
        }

        if (inCodeFence) {
            pushLine(MarkdownLineKind::Code, rawLine, 0, "", false);
            ++sourceLineIndex;
            continue;
        }

        if (trimmed.empty()) {
            pushLine(MarkdownLineKind::Blank, "", 0, "", false);
            ++sourceLineIndex;
            continue;
        }

        if (IsMarkdownHorizontalRule(trimmed)) {
            pushLine(MarkdownLineKind::Rule, "", 0, "", false);
            ++sourceLineIndex;
            continue;
        }

        size_t headingLevel = 0;
        while (headingLevel < trimmed.size() && trimmed[headingLevel] == '#') {
            ++headingLevel;
        }
        if (headingLevel > 0 && headingLevel <= 6 && headingLevel < trimmed.size() && trimmed[headingLevel] == ' ') {
            pushLine(MarkdownLineKind::Heading, trimmed.substr(headingLevel + 1), static_cast<int>(headingLevel), "", false);
            ++sourceLineIndex;
            continue;
        }

        if (leadingTrimmed.rfind("> ", 0) == 0) {
            pushLine(MarkdownLineKind::Quote, leadingTrimmed.substr(2), 0, "", false);
            ++sourceLineIndex;
            continue;
        }

        std::string listValue;
        std::string listPrefix;
        if (leadingTrimmed.rfind("- ", 0) == 0 || leadingTrimmed.rfind("* ", 0) == 0 || leadingTrimmed.rfind("+ ", 0) == 0) {
            listValue = leadingTrimmed.substr(2);
            listPrefix = std::string(listIndent, ' ') + GetRenderedBulletPrefix();
        } else {
            std::string numberedPrefix;
            std::string numberedBody;
            if (IsMarkdownNumberedListLine(leadingTrimmed, numberedPrefix, numberedBody)) {
                const std::string prefix = std::string(listIndent, ' ') + numberedPrefix;
                pushLine(MarkdownLineKind::Numbered, numberedBody, 0, prefix, false);
                ++sourceLineIndex;
                continue;
            }
        }
        if (!listValue.empty()) {
            bool taskChecked = false;
            std::string taskBody;
            if (IsMarkdownTaskMarker(listValue, taskChecked, taskBody)) {
                const std::string taskPrefix = std::string(listIndent, ' ') + (taskChecked ? "[x] " : "[ ] ");
                pushLine(MarkdownLineKind::Task, taskBody, 0, taskPrefix, taskChecked);
            } else {
                pushLine(MarkdownLineKind::Bullet, listValue, 0, listPrefix, false);
            }
            ++sourceLineIndex;
            continue;
        }

        pushLine(MarkdownLineKind::Body, rawLine, 0, "", false);
        ++sourceLineIndex;
    }

    if (out.empty()) {
        MarkdownPreviewLine line;
        line.kind = MarkdownLineKind::Blank;
        line.sourceLineIndex = 0;
        out.push_back(line);
    }
    return out;
}

bool ToggleMarkdownTaskLineByIndex(std::string& markdownText, size_t sourceLineIndex) {
    if (sourceLineIndex == std::numeric_limits<size_t>::max()) return false;
    std::vector<std::string> lines;
    lines.reserve(128);

    std::istringstream in(markdownText);
    std::string rawLine;
    while (std::getline(in, rawLine)) {
        if (!rawLine.empty() && rawLine.back() == '\r') rawLine.pop_back();
        lines.push_back(rawLine);
    }
    if (sourceLineIndex >= lines.size()) return false;

    std::string& line = lines[sourceLineIndex];
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
        ++i;
    }
    if (i + 5 >= line.size()) return false;

    const char marker = line[i];
    if (marker != '-' && marker != '*' && marker != '+') return false;
    if (line[i + 1] != ' ') return false;
    if (line[i + 2] != '[' || line[i + 4] != ']' || line[i + 5] != ' ') return false;

    const char check = static_cast<char>(std::tolower(static_cast<unsigned char>(line[i + 3])));
    if (check == 'x') {
        line[i + 3] = ' ';
    } else if (check == ' ') {
        line[i + 3] = 'x';
    } else {
        return false;
    }

    const bool hadTrailingNewline = !markdownText.empty() && markdownText.back() == '\n';
    std::string rebuilt;
    rebuilt.reserve(markdownText.size() + 8);
    for (size_t idx = 0; idx < lines.size(); ++idx) {
        rebuilt += lines[idx];
        if (idx + 1 < lines.size() || hadTrailingNewline) {
            rebuilt.push_back('\n');
        }
    }
    markdownText.swap(rebuilt);
    return true;
}

struct MarkdownPreviewLink {
    std::string label;
    std::string url;
    size_t start = 0;
    size_t end = 0;
};

bool IsHttpUrl(const std::string& value) {
    return value.rfind("https://", 0) == 0 || value.rfind("http://", 0) == 0;
}

bool IsLikelyVideoUrl(const std::string& url) {
    const std::string lower = ToLowerAscii(url);
    return lower.find("youtube.com/") != std::string::npos || lower.find("youtu.be/") != std::string::npos ||
           lower.find("vimeo.com/") != std::string::npos || lower.find("twitch.tv/") != std::string::npos;
}

bool RangesOverlap(size_t startA, size_t endA, size_t startB, size_t endB) {
    return (startA < endB) && (startB < endA);
}

size_t FindUrlEnd(const std::string& text, size_t start) {
    size_t end = start;
    while (end < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[end]);
        if (std::isspace(c) || c == '<' || c == '>' || c == '"' || c == '\'' || c == '`') break;
        ++end;
    }
    while (end > start) {
        const char tail = text[end - 1];
        if (tail == '.' || tail == ',' || tail == ';' || tail == ':' || tail == ')' || tail == ']') {
            --end;
            continue;
        }
        break;
    }
    return end;
}

std::vector<MarkdownPreviewLink> ExtractMarkdownPreviewLinks(const std::string& text) {
    std::vector<MarkdownPreviewLink> links;
    links.reserve(4);

    // Markdown links: [label](url)
    for (size_t i = 0; i < text.size();) {
        if (text[i] != '[') {
            ++i;
            continue;
        }
        const size_t closeBracket = text.find(']', i + 1);
        if (closeBracket == std::string::npos || closeBracket + 1 >= text.size() || text[closeBracket + 1] != '(') {
            ++i;
            continue;
        }
        const size_t closeParen = text.find(')', closeBracket + 2);
        if (closeParen == std::string::npos) {
            ++i;
            continue;
        }

        const std::string label = TrimAscii(text.substr(i + 1, closeBracket - (i + 1)));
        const std::string url = TrimAscii(text.substr(closeBracket + 2, closeParen - (closeBracket + 2)));
        if (IsHttpUrl(url)) {
            MarkdownPreviewLink link;
            link.label = label;
            link.url = url;
            link.start = i;
            link.end = closeParen + 1;
            links.push_back(std::move(link));
            i = closeParen + 1;
            continue;
        }
        ++i;
    }

    // Plain URLs not already covered by markdown link ranges.
    for (size_t pos = 0; pos < text.size();) {
        const size_t httpPos = text.find("http://", pos);
        const size_t httpsPos = text.find("https://", pos);
        size_t start = std::string::npos;
        if (httpPos == std::string::npos) {
            start = httpsPos;
        } else if (httpsPos == std::string::npos) {
            start = httpPos;
        } else {
            start = std::min(httpPos, httpsPos);
        }
        if (start == std::string::npos) break;

        const size_t end = FindUrlEnd(text, start);
        if (end <= start) {
            pos = start + 1;
            continue;
        }

        bool overlaps = false;
        for (const auto& existing : links) {
            if (RangesOverlap(start, end, existing.start, existing.end)) {
                overlaps = true;
                break;
            }
        }
        if (!overlaps) {
            MarkdownPreviewLink link;
            link.url = text.substr(start, end - start);
            link.label = link.url;
            link.start = start;
            link.end = end;
            if (IsHttpUrl(link.url)) { links.push_back(std::move(link)); }
        }
        pos = end;
    }

    std::sort(links.begin(), links.end(), [](const MarkdownPreviewLink& a, const MarkdownPreviewLink& b) {
        if (a.start != b.start) return a.start < b.start;
        return a.end < b.end;
    });

    std::set<std::string> seenUrls;
    std::vector<MarkdownPreviewLink> deduped;
    deduped.reserve(links.size());
    for (const auto& link : links) {
        if (seenUrls.insert(link.url).second) { deduped.push_back(link); }
    }
    return deduped;
}

bool OpenMarkdownPreviewUrl(const std::string& url) {
    if (!IsHttpUrl(url)) return false;
    const std::wstring wideUrl = Utf8ToWide(url);
    HINSTANCE result = ShellExecuteW(nullptr, L"open", wideUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<intptr_t>(result) > 32;
}

void RenderMarkdownPreviewLinksInline(const MarkdownPreviewLine& line) {
    const std::vector<MarkdownPreviewLink> links = ExtractMarkdownPreviewLinks(line.text);
    if (links.empty()) return;

    for (size_t i = 0; i < links.size(); ++i) {
        const MarkdownPreviewLink& link = links[i];
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::PushID(static_cast<int>(line.sourceLineIndex));
        ImGui::PushID(static_cast<int>(i));
        const char* buttonLabel = IsLikelyVideoUrl(link.url) ? "Video" : "Open";
        if (ImGui::SmallButton(buttonLabel)) { OpenMarkdownPreviewUrl(link.url); }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            if (HasMeaningfulText(link.label) && ToLowerAscii(link.label) != ToLowerAscii(link.url)) {
                ImGui::SetTooltip("%s\n%s", link.label.c_str(), link.url.c_str());
            } else {
                ImGui::SetTooltip("%s", link.url.c_str());
            }
        }
        ImGui::PopID();
        ImGui::PopID();
    }
}

std::vector<std::string> WrapTextToColumns(const std::string& text, size_t maxColumns) {
    std::vector<std::string> wrapped;
    std::string expanded;
    expanded.reserve(text.size() + 8);
    for (char c : text) {
        if (c == '\t') {
            expanded += "    ";
        } else {
            expanded.push_back(c);
        }
    }

    if (expanded.empty()) {
        wrapped.emplace_back();
        return wrapped;
    }

    std::string remaining = expanded;
    while (remaining.size() > maxColumns) {
        size_t cut = remaining.rfind(' ', maxColumns);
        if (cut == std::string::npos || cut < maxColumns / 3) { cut = maxColumns; }
        wrapped.push_back(remaining.substr(0, cut));
        size_t next = cut;
        while (next < remaining.size() && remaining[next] == ' ') {
            ++next;
        }
        remaining = remaining.substr(next);
    }
    wrapped.push_back(remaining);
    return wrapped;
}

std::vector<std::string> WrapTextWithPrefix(const std::string& text, const std::string& firstPrefix, const std::string& nextPrefix,
                                            size_t maxColumns) {
    std::vector<std::string> wrapped = WrapTextToColumns(text, maxColumns > firstPrefix.size() ? maxColumns - firstPrefix.size() : maxColumns);
    for (size_t i = 0; i < wrapped.size(); ++i) {
        wrapped[i] = (i == 0 ? firstPrefix : nextPrefix) + wrapped[i];
    }
    return wrapped;
}

float EstimateIndentPointsFromPrefix(const std::string& prefix) {
    size_t leadingSpaces = 0;
    for (char c : prefix) {
        if (c == ' ') {
            ++leadingSpaces;
            continue;
        }
        if (c == '\t') {
            leadingSpaces += 4;
            continue;
        }
        break;
    }
    return static_cast<float>(leadingSpaces) * 3.4f;
}

std::string EscapePdfLiteralText(const std::string& text) {
    std::string escaped;
    escaped.reserve(text.size() * 2);
    for (unsigned char c : text) {
        if (c == '\\' || c == '(' || c == ')') {
            escaped.push_back('\\');
            escaped.push_back(static_cast<char>(c));
            continue;
        }
        if (c >= 32 && c <= 126) {
            escaped.push_back(static_cast<char>(c));
        } else {
            escaped.push_back('?');
        }
    }
    return escaped;
}

float HeadingPdfFontSize(int headingLevel) {
    static const float kSizes[] = { 20.0f, 18.0f, 16.0f, 14.0f, 12.5f, 11.5f };
    const int idx = std::clamp(headingLevel, 1, 6) - 1;
    return kSizes[idx];
}

size_t HeadingWrapColumns(int headingLevel) {
    static const size_t kColumns[] = { 52, 58, 66, 74, 82, 90 };
    const int idx = std::clamp(headingLevel, 1, 6) - 1;
    return kColumns[idx];
}

bool WriteSimpleMarkdownPdf(const std::filesystem::path& path, const std::string& title, const std::string& markdownText) {
    enum class PdfFont : int {
        Regular = 0,
        Bold = 1,
        Mono = 2
    };

    struct PdfLine {
        std::string text;
        float fontSize = 11.0f;
        bool isBlank = false;
        bool isRule = false;
        PdfFont font = PdfFont::Regular;
        float colorR = 0.07f;
        float colorG = 0.08f;
        float colorB = 0.10f;
        bool drawQuoteBar = false;
        bool drawBulletDot = false;
        bool drawTaskBox = false;
        bool taskChecked = false;
        float xOffset = 0.0f;
        float markerIndent = 0.0f;
    };

    (void)title;
    std::vector<PdfLine> renderLines;

    const std::vector<MarkdownPreviewLine> parsed = ParseMarkdownPreviewLines(markdownText);
    for (const auto& line : parsed) {
        switch (line.kind) {
        case MarkdownLineKind::Blank:
            renderLines.push_back({ "", 0.0f, true, false, PdfFont::Regular, 0.07f, 0.08f, 0.10f });
            break;
        case MarkdownLineKind::Rule:
            renderLines.push_back({ "", 10.0f, false, true, PdfFont::Regular, 0.35f, 0.39f, 0.46f });
            break;
        case MarkdownLineKind::Heading: {
            const float size = HeadingPdfFontSize(line.headingLevel);
            const size_t cols = HeadingWrapColumns(line.headingLevel);
            const auto wrapped = WrapTextToColumns(line.text, cols);
            for (const auto& w : wrapped) {
                renderLines.push_back({ w, size, false, false, PdfFont::Bold, 0.06f, 0.08f, 0.11f });
            }
            break;
        }
        case MarkdownLineKind::Quote: {
            const auto wrapped = WrapTextToColumns(line.text, 86);
            for (const auto& w : wrapped) {
                PdfLine q{};
                q.text = w;
                q.fontSize = 11.0f;
                q.isBlank = false;
                q.isRule = false;
                q.font = PdfFont::Regular;
                q.colorR = 0.32f;
                q.colorG = 0.37f;
                q.colorB = 0.45f;
                q.drawQuoteBar = true;
                q.xOffset = 14.0f;
                q.markerIndent = 0.0f;
                renderLines.push_back(q);
            }
            break;
        }
        case MarkdownLineKind::Bullet: {
            const std::string prefix = line.listPrefix.empty() ? "- " : line.listPrefix;
            const float indentPoints = EstimateIndentPointsFromPrefix(prefix);
            const auto wrapped = WrapTextToColumns(line.text, 84);
            for (size_t i = 0; i < wrapped.size(); ++i) {
                PdfLine b{};
                b.text = wrapped[i];
                b.fontSize = 11.0f;
                b.isBlank = false;
                b.isRule = false;
                b.font = PdfFont::Regular;
                b.colorR = 0.08f;
                b.colorG = 0.09f;
                b.colorB = 0.11f;
                b.drawBulletDot = (i == 0);
                b.xOffset = indentPoints + 12.0f;
                b.markerIndent = indentPoints;
                renderLines.push_back(b);
            }
            break;
        }
        case MarkdownLineKind::Numbered: {
            const std::string prefix = line.listPrefix.empty() ? "1. " : line.listPrefix;
            const std::string indent(prefix.size(), ' ');
            const auto wrapped = WrapTextWithPrefix(line.text, prefix, indent, 92);
            for (const auto& w : wrapped) {
                renderLines.push_back({ w, 11.0f, false, false, PdfFont::Regular, 0.08f, 0.09f, 0.11f });
            }
            break;
        }
        case MarkdownLineKind::Task: {
            const std::string prefix = line.listPrefix.empty() ? (line.checked ? "[x] " : "[ ] ") : line.listPrefix;
            const float r = line.checked ? 0.12f : 0.48f;
            const float g = line.checked ? 0.46f : 0.35f;
            const float b = line.checked ? 0.21f : 0.13f;
            const float indentPoints = EstimateIndentPointsFromPrefix(prefix);
            const auto wrapped = WrapTextToColumns(line.text, 84);
            for (size_t i = 0; i < wrapped.size(); ++i) {
                PdfLine t{};
                t.text = wrapped[i];
                t.fontSize = 11.0f;
                t.isBlank = false;
                t.isRule = false;
                t.font = PdfFont::Regular;
                t.colorR = r;
                t.colorG = g;
                t.colorB = b;
                t.drawTaskBox = (i == 0);
                t.taskChecked = line.checked;
                t.xOffset = indentPoints + 16.0f;
                t.markerIndent = indentPoints;
                renderLines.push_back(t);
            }
            break;
        }
        case MarkdownLineKind::Code: {
            const auto wrapped = WrapTextToColumns(line.text, 96);
            for (const auto& w : wrapped) {
                renderLines.push_back({ w, 10.0f, false, false, PdfFont::Mono, 0.10f, 0.30f, 0.56f });
            }
            break;
        }
        case MarkdownLineKind::Body:
        default: {
            const auto wrapped = WrapTextToColumns(line.text, 96);
            for (const auto& w : wrapped) {
                renderLines.push_back({ w, 11.0f, false, false, PdfFont::Regular, 0.08f, 0.09f, 0.11f });
            }
            break;
        }
        }
    }

    if (renderLines.empty()) renderLines.push_back({ "", 0.0f, true, false, PdfFont::Regular, 0.07f, 0.08f, 0.10f });

    std::vector<std::string> pageStreams;
    std::ostringstream stream;
    constexpr float kLeft = 50.0f;
    constexpr float kTopY = 770.0f;
    constexpr float kBottomY = 50.0f;
    float y = kTopY;

    auto flushPage = [&]() {
        pageStreams.push_back(stream.str());
        stream.str("");
        stream.clear();
        y = kTopY;
    };

    for (const auto& line : renderLines) {
        float lineHeight = 0.0f;
        if (line.isBlank) {
            lineHeight = 7.0f;
        } else if (line.isRule) {
            lineHeight = 8.0f;
        } else if (line.font == PdfFont::Bold && line.fontSize >= 12.0f) {
            lineHeight = line.fontSize + 1.6f;
        } else {
            lineHeight = line.fontSize + 3.0f;
        }
        if (y - lineHeight < kBottomY) { flushPage(); }

        if (!line.isBlank) {
            if (line.isRule) {
                const float yLine = y - 2.0f;
                const float x1 = kLeft;
                const float x2 = 612.0f - kLeft;
                stream << std::fixed << std::setprecision(3) << line.colorR << " " << line.colorG << " " << line.colorB << " RG\n";
                stream << "1 w\n";
                stream << x1 << " " << yLine << " m\n";
                stream << x2 << " " << yLine << " l\n";
                stream << "S\n";
                y -= lineHeight;
                continue;
            }

            if (line.drawQuoteBar) {
                const float xBar = kLeft + line.markerIndent + 4.0f;
                // Align the quote bar to the text glyph bounds around the current baseline.
                const float ascent = std::clamp(line.fontSize * 0.72f, 6.5f, 9.0f);
                const float descent = std::clamp(line.fontSize * 0.30f, 2.2f, 4.4f);
                const float yTop = y + ascent;
                const float yBottom = y - descent;
                stream << std::fixed << std::setprecision(3) << 0.36f << " " << 0.45f << " " << 0.60f << " RG\n";
                stream << "2 w\n";
                stream << xBar << " " << yBottom << " m\n";
                stream << xBar << " " << yTop << " l\n";
                stream << "S\n";
            }

            if (line.drawBulletDot) {
                const float dotRadius = std::clamp(line.fontSize * 0.17f, 1.4f, 2.1f);
                const float dotX = kLeft + line.markerIndent + 6.0f;
                const float dotY = y + std::clamp(line.fontSize * 0.24f, 1.5f, 3.6f);
                const float k = dotRadius * 0.55228475f;
                stream << std::fixed << std::setprecision(3) << line.colorR << " " << line.colorG << " " << line.colorB << " rg\n";
                stream << (dotX + dotRadius) << " " << dotY << " m\n";
                stream << (dotX + dotRadius) << " " << (dotY + k) << " " << (dotX + k) << " " << (dotY + dotRadius) << " " << dotX << " "
                       << (dotY + dotRadius) << " c\n";
                stream << (dotX - k) << " " << (dotY + dotRadius) << " " << (dotX - dotRadius) << " " << (dotY + k) << " " << (dotX - dotRadius)
                       << " " << dotY << " c\n";
                stream << (dotX - dotRadius) << " " << (dotY - k) << " " << (dotX - k) << " " << (dotY - dotRadius) << " " << dotX << " "
                       << (dotY - dotRadius) << " c\n";
                stream << (dotX + k) << " " << (dotY - dotRadius) << " " << (dotX + dotRadius) << " " << (dotY - k) << " "
                       << (dotX + dotRadius) << " " << dotY << " c\n";
                stream << "f\n";
            }

            if (line.drawTaskBox) {
                const float boxSize = std::clamp(line.fontSize * 0.64f, 6.5f, 8.0f);
                const float boxX = kLeft + line.markerIndent + 2.0f;
                const float baselineOffset = std::clamp(line.fontSize * 0.16f, 1.5f, 2.2f);
                const float boxY = y - baselineOffset;
                stream << std::fixed << std::setprecision(3) << line.colorR << " " << line.colorG << " " << line.colorB << " RG\n";
                stream << "1 w\n";
                stream << boxX << " " << boxY << " " << boxSize << " " << boxSize << " re\n";
                stream << "S\n";
                if (line.taskChecked) {
                    const float x1 = boxX + boxSize * 0.22f;
                    const float y1 = boxY + boxSize * 0.48f;
                    const float x2 = boxX + boxSize * 0.43f;
                    const float y2 = boxY + boxSize * 0.22f;
                    const float x3 = boxX + boxSize * 0.80f;
                    const float y3 = boxY + boxSize * 0.86f;
                    stream << x1 << " " << y1 << " m\n";
                    stream << x2 << " " << y2 << " l\n";
                    stream << x3 << " " << y3 << " l\n";
                    stream << "S\n";
                }
            }

            stream << "BT\n";
            const char* fontTag = "/F1";
            if (line.font == PdfFont::Bold) {
                fontTag = "/F2";
            } else if (line.font == PdfFont::Mono) {
                fontTag = "/F3";
            }
            stream << std::fixed << std::setprecision(3) << line.colorR << " " << line.colorG << " " << line.colorB << " rg\n";
            stream << fontTag << " " << std::fixed << std::setprecision(1) << line.fontSize << " Tf\n";
            stream << "1 0 0 1 " << (kLeft + line.xOffset) << " " << y << " Tm\n";
            stream << "(" << EscapePdfLiteralText(line.text) << ") Tj\n";
            stream << "ET\n";
        }
        y -= lineHeight;
    }
    if (pageStreams.empty() || !stream.str().empty()) { flushPage(); }

    std::vector<std::string> objects(6);
    objects[1] = "<< /Type /Catalog /Pages 2 0 R >>";
    objects[3] = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>";
    objects[4] = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold >>";
    objects[5] = "<< /Type /Font /Subtype /Type1 /BaseFont /Courier >>";

    std::vector<int> pageObjectIds;
    pageObjectIds.reserve(pageStreams.size());
    for (const auto& streamData : pageStreams) {
        const int contentObjId = static_cast<int>(objects.size());
        objects.push_back("<< /Length " + std::to_string(streamData.size()) + " >>\nstream\n" + streamData + "endstream");

        const int pageObjId = static_cast<int>(objects.size());
        objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 3 0 R /F2 4 0 R /F3 5 0 R >> >> "
                          "/Contents " +
                          std::to_string(contentObjId) + " 0 R >>");
        pageObjectIds.push_back(pageObjId);
    }

    std::ostringstream kids;
    for (int pageObjId : pageObjectIds) {
        kids << pageObjId << " 0 R ";
    }
    objects[2] = "<< /Type /Pages /Count " + std::to_string(pageObjectIds.size()) + " /Kids [ " + kids.str() + "] >>";

    std::string pdf;
    pdf.reserve(4096 + renderLines.size() * 120);
    pdf += "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";

    std::vector<size_t> offsets(objects.size(), 0);
    for (size_t objId = 1; objId < objects.size(); ++objId) {
        offsets[objId] = pdf.size();
        pdf += std::to_string(objId) + " 0 obj\n";
        pdf += objects[objId];
        pdf += "\nendobj\n";
    }

    const size_t xrefOffset = pdf.size();
    pdf += "xref\n0 " + std::to_string(objects.size()) + "\n";
    pdf += "0000000000 65535 f \n";
    for (size_t objId = 1; objId < objects.size(); ++objId) {
        char lineBuf[32];
        std::snprintf(lineBuf, sizeof(lineBuf), "%010zu 00000 n \n", offsets[objId]);
        pdf += lineBuf;
    }
    pdf += "trailer\n<< /Size " + std::to_string(objects.size()) + " /Root 1 0 R >>\n";
    pdf += "startxref\n" + std::to_string(xrefOffset) + "\n%%EOF\n";

    return WriteBinaryFile(path, pdf);
}

void QueueGeneralSaveConflict(NotesOverlayState& st, const std::filesystem::path& target, const std::filesystem::path& currentPath,
                              const std::string& title, const std::string& draft) {
    st.pendingSaveConflictTargetPath = target;
    st.pendingSaveConflictCurrentPath = currentPath;
    st.pendingSaveConflictTitle = title;
    st.pendingSaveConflictDraft = draft;
    st.pendingSaveConflictIsPdf = false;
    st.pendingSaveConflictOpenPopup = true;
    SetStatus(st, "Name conflict: choose overwrite, save-as-new, or cancel.");
}

void QueuePdfSaveConflict(NotesOverlayState& st, const std::filesystem::path& target, const std::string& title, const std::string& markdownText) {
    st.pendingSaveConflictTargetPath = target;
    st.pendingSaveConflictCurrentPath.clear();
    st.pendingSaveConflictTitle = title;
    st.pendingSaveConflictDraft = markdownText;
    st.pendingSaveConflictIsPdf = true;
    st.pendingSaveConflictOpenPopup = true;
    SetStatus(st, "PDF name conflict: choose overwrite, save-as-new, or cancel.");
}

bool FinalizePdfExportSuccess(NotesOverlayState& st, const std::filesystem::path& target) {
    const bool openFolderAfterExport = [&]() {
        auto cfgSnap = GetConfigSnapshot();
        return cfgSnap && cfgSnap->notesOverlay.openPdfFolderAfterExport;
    }();

    if (openFolderAfterExport) {
        const bool opened = OpenFolderContainingPath(target);
        SetStatus(st,
                  "Exported PDF: " + PathForDisplay(target) + (opened ? " (opened folder)" : " (exported; failed to open folder)"));
    } else {
        SetStatus(st, "Exported PDF: " + PathForDisplay(target));
    }
    return true;
}

bool ExportDraftToPdf(NotesOverlayState& st, const std::string& preferredTitle, const std::string& markdownText, const char* sourceLabel) {
    if (!HasMeaningfulText(markdownText)) {
        SetStatus(st, std::string(sourceLabel) + " note is empty; nothing exported.");
        return false;
    }

    EnsureNotesDirectories();
    std::string fileBase = SanitizeFileComponent(preferredTitle);
    if (fileBase.empty() || fileBase == "note") { fileBase = "note_" + CurrentDateStamp() + "_" + CurrentTimeStamp(); }

    const std::filesystem::path folder = GetPdfExportRootPath();
    std::filesystem::create_directories(folder);
    const std::filesystem::path target = folder / Utf8ToWide(fileBase + ".pdf");
    if (std::filesystem::exists(target)) {
        QueuePdfSaveConflict(st, target, fileBase, markdownText);
        return false;
    }

    if (!WriteSimpleMarkdownPdf(target, fileBase, markdownText)) {
        SetStatus(st, "Failed to export PDF.");
        return false;
    }
    return FinalizePdfExportSuccess(st, target);
}

void RenderPreviewTitleBlock(const std::string& noteTitle) {
    const std::string title = HasMeaningfulText(noteTitle) ? TrimAscii(noteTitle) : "Note";
    ImGui::SetWindowFontScale(1.45f);
    ImGui::TextColored(ImVec4(0.92f, 0.95f, 1.0f, 1.0f), "%s", title.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    ImGui::Spacing();
}

bool RenderMarkdownPreview(std::string& markdownText) {
    const std::vector<MarkdownPreviewLine> lines = ParseMarkdownPreviewLines(markdownText);
    size_t pendingToggleLine = std::numeric_limits<size_t>::max();
    for (const auto& line : lines) {
        switch (line.kind) {
        case MarkdownLineKind::Blank:
            ImGui::Spacing();
            break;
        case MarkdownLineKind::Rule:
            ImGui::Separator();
            break;
        case MarkdownLineKind::Heading: {
            const int level = std::clamp(line.headingLevel, 1, 6);
            const ImU32 shades[] = { IM_COL32(236, 243, 255, 255), IM_COL32(220, 235, 255, 255), IM_COL32(200, 224, 255, 255),
                                     IM_COL32(184, 212, 246, 255), IM_COL32(170, 198, 232, 255), IM_COL32(154, 188, 220, 255) };
            const float scales[] = { 1.45f, 1.30f, 1.20f, 1.12f, 1.06f, 1.00f };
            ImGui::SetWindowFontScale(scales[level - 1]);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(shades[level - 1]), "%s", line.text.c_str());
            ImGui::SetWindowFontScale(1.0f);
            RenderMarkdownPreviewLinksInline(line);
            if (level <= 2) ImGui::Spacing();
            break;
        }
        case MarkdownLineKind::Quote:
            ImGui::BeginGroup();
            ImGui::Dummy(ImVec2(10.0f, 0.0f));
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(180, 195, 212, 255));
            ImGui::TextWrapped("%s", line.text.c_str());
            ImGui::PopStyleColor();
            ImGui::EndGroup();
            {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                if (dl) {
                    const ImVec2 qMin = ImGui::GetItemRectMin();
                    const ImVec2 qMax = ImGui::GetItemRectMax();
                    dl->AddLine(ImVec2(qMin.x + 3.0f, qMin.y + 2.0f), ImVec2(qMin.x + 3.0f, qMax.y - 2.0f),
                                IM_COL32(112, 148, 192, 255), 2.0f);
                }
            }
            RenderMarkdownPreviewLinksInline(line);
            break;
        case MarkdownLineKind::Bullet:
            ImGui::TextWrapped("%s%s", line.listPrefix.empty() ? GetRenderedBulletPrefix() : line.listPrefix.c_str(), line.text.c_str());
            RenderMarkdownPreviewLinksInline(line);
            break;
        case MarkdownLineKind::Numbered:
            ImGui::TextWrapped("%s%s", line.listPrefix.empty() ? "1. " : line.listPrefix.c_str(), line.text.c_str());
            RenderMarkdownPreviewLinksInline(line);
            break;
        case MarkdownLineKind::Task: {
            const ImU32 color = line.checked ? IM_COL32(120, 220, 145, 255) : IM_COL32(242, 200, 124, 255);
            const float baseX = ImGui::GetCursorPosX();
            const float spaceW = ImGui::CalcTextSize(" ").x;
            const size_t indentCols = CountLeadingIndentColumns(line.listPrefix);
            const float indentPx = static_cast<float>(indentCols) * spaceW;
            const float boxSize = std::max(9.0f, ImGui::GetFontSize() * 0.78f);
            const float lineH = ImGui::GetTextLineHeight();

            ImGui::SetCursorPosX(baseX + indentPx);
            ImGui::PushID(static_cast<int>(line.sourceLineIndex));
            if (ImGui::InvisibleButton("##task_toggle", ImVec2(boxSize + 2.0f, lineH))) {
                pendingToggleLine = line.sourceLineIndex;
            }
            const bool hovered = ImGui::IsItemHovered();
            const ImVec2 itemMin = ImGui::GetItemRectMin();
            const float boxY = itemMin.y + (lineH - boxSize) * 0.5f;

            if (ImDrawList* dl = ImGui::GetWindowDrawList()) {
                const ImVec2 b0(itemMin.x, boxY);
                const ImVec2 b1(itemMin.x + boxSize, boxY + boxSize);
                dl->AddRect(b0, b1, color, 2.0f, 0, hovered ? 1.8f : 1.2f);
                if (line.checked) {
                    const ImVec2 c1(b0.x + boxSize * 0.20f, b0.y + boxSize * 0.56f);
                    const ImVec2 c2(b0.x + boxSize * 0.44f, b0.y + boxSize * 0.78f);
                    const ImVec2 c3(b0.x + boxSize * 0.82f, b0.y + boxSize * 0.24f);
                    dl->AddLine(c1, c2, color, 1.5f);
                    dl->AddLine(c2, c3, color, 1.5f);
                }
            }

            ImGui::SameLine(0.0f, 6.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::TextWrapped("%s", line.text.c_str());
            ImGui::PopStyleColor();
            ImGui::PopID();
            RenderMarkdownPreviewLinksInline(line);
            break;
        }
        case MarkdownLineKind::Code:
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150, 210, 255, 255));
            ImGui::TextUnformatted(line.text.c_str());
            ImGui::PopStyleColor();
            break;
        case MarkdownLineKind::Body:
        default:
            ImGui::TextWrapped("%s", line.text.c_str());
            RenderMarkdownPreviewLinksInline(line);
            break;
        }
    }
    if (pendingToggleLine != std::numeric_limits<size_t>::max()) {
        return ToggleMarkdownTaskLineByIndex(markdownText, pendingToggleLine);
    }
    return false;
}

void EnsureInitializedLocked(NotesOverlayState& st, const Config& cfg) {
    if (st.initializedVisibility) return;
    st.visible = cfg.notesOverlay.visible;
    st.initializedVisibility = true;
    st.refreshRequested = true;
    if (st.visible) {
        const bool inWorldNow = IsInWorldNow();
        st.activeTab = inWorldNow ? 0 : 1;
        st.forceTabSelectionNextFrame = true;
        st.focusIgnEditorNextFrame = inWorldNow;
        st.focusGeneralEditorNextFrame = !inWorldNow;
    }
}

void SortEntries(std::vector<NotesFileEntry>& entries, NotesSortMode mode) {
    std::sort(entries.begin(), entries.end(), [mode](const NotesFileEntry& a, const NotesFileEntry& b) {
        if (a.pinned != b.pinned) return a.pinned > b.pinned;
        const std::string aName = ToLowerAscii(a.title);
        const std::string bName = ToLowerAscii(b.title);
        switch (mode) {
        case NotesSortMode::DateNewest:
            if (a.modifiedEpochSeconds != b.modifiedEpochSeconds) { return a.modifiedEpochSeconds > b.modifiedEpochSeconds; }
            return aName < bName;
        case NotesSortMode::DateOldest:
            if (a.modifiedEpochSeconds != b.modifiedEpochSeconds) { return a.modifiedEpochSeconds < b.modifiedEpochSeconds; }
            return aName < bName;
        case NotesSortMode::NameAsc:
            return aName < bName;
        case NotesSortMode::NameDesc:
            return aName > bName;
        case NotesSortMode::NumberAsc:
            if (a.numberKey != b.numberKey) { return a.numberKey < b.numberKey; }
            return aName < bName;
        case NotesSortMode::NumberDesc:
            if (a.numberKey != b.numberKey) { return a.numberKey > b.numberKey; }
            return aName > bName;
        default:
            return aName < bName;
        }
    });
}

void ApplyPinnedFlagsAndLabels(std::vector<NotesFileEntry>& entries, const NotesOverlayState& st, bool showRelativeFolder,
                               const std::filesystem::path& relativeRoot) {
    for (auto& item : entries) {
        item.pinned = IsPathPinned(st, item.path);
        item.favorite = IsPathFavorited(st, item.path);
        item.displayLabel = item.title;
        if (showRelativeFolder) {
            try {
                const std::filesystem::path rel = std::filesystem::relative(item.path.parent_path(), relativeRoot);
                const std::wstring relW = rel.lexically_normal().wstring();
                if (!relW.empty() && relW != L"." && relW != L"..") { item.displayLabel += "  [" + WideToUtf8(relW) + "]"; }
            } catch (...) {}
        }
        const std::string stamp = FormatEpochForList(item.modifiedEpochSeconds);
        if (!stamp.empty()) { item.displayLabel += "  [" + stamp + "]"; }
    }
}

bool ShouldSkipGeneralFolder(const std::string& nameLower) {
    if (nameLower.empty()) return true;
    if (nameLower[0] == '.') return true;
    if (nameLower == "ign") return true;
    if (nameLower == "favorites") return true;
    return false;
}

void ReloadGeneralFolders(NotesOverlayState& st) {
    EnsureNotesDirectories();
    const std::string prevSelection =
        (st.selectedGeneralFolderIndex >= 0 && st.selectedGeneralFolderIndex < static_cast<int>(st.generalFolders.size()))
            ? st.generalFolders[static_cast<size_t>(st.selectedGeneralFolderIndex)]
            : "";

    st.generalFolders.clear();
    st.generalFolders.push_back(kGeneralFolderRoot);
    st.generalFolders.push_back(kGeneralFolderFavorites);

    const std::filesystem::path root = GetGeneralNotesRootPath();
    try {
        for (const auto& entry : std::filesystem::directory_iterator(root)) {
            if (!entry.is_directory()) continue;
            std::filesystem::path rel = entry.path().filename();
            if (rel.empty()) continue;
            const std::string folderName = WideToUtf8(rel.wstring());
            const std::string folderNameLower = ToLowerAscii(folderName);
            if (ShouldSkipGeneralFolder(folderNameLower)) continue;
            st.generalFolders.push_back(folderName);
        }
    } catch (...) {}

    std::sort(st.generalFolders.begin() + 2, st.generalFolders.end(), [](const std::string& a, const std::string& b) {
        return ToLowerAscii(a) < ToLowerAscii(b);
    });

    st.selectedGeneralFolderIndex = 0;
    for (size_t i = 0; i < st.generalFolders.size(); ++i) {
        if (st.generalFolders[i] == prevSelection) {
            st.selectedGeneralFolderIndex = static_cast<int>(i);
            break;
        }
    }
    st.generalFolderTabOffset = std::clamp(st.generalFolderTabOffset, 0, std::max(0, static_cast<int>(st.generalFolders.size()) - 1));
}

std::filesystem::path ResolveGeneralFolderPath(const NotesOverlayState& st) {
    const std::filesystem::path root = GetGeneralNotesRootPath();
    if (st.selectedGeneralFolderIndex < 0 || st.selectedGeneralFolderIndex >= static_cast<int>(st.generalFolders.size())) { return root; }

    const std::string& rel = st.generalFolders[static_cast<size_t>(st.selectedGeneralFolderIndex)];
    if (IsGeneralFavoritesFolderKey(rel)) return root;
    if (rel.empty()) return root;
    return root / Utf8ToWide(rel);
}

bool ShouldSkipNotesFile(const std::filesystem::path& path) {
    const std::string filenameLower = ToLowerAscii(path.filename().string());
    if (filenameLower.empty()) return true;
    if (filenameLower[0] == '.') return true;
    if (filenameLower.rfind(".toolscreen_", 0) == 0) return true;
    return false;
}

std::vector<NotesFileEntry> LoadNotesInDirectory(const std::filesystem::path& folderPath, bool recursive) {
    std::vector<NotesFileEntry> entries;
    auto consume = [&](const std::filesystem::directory_entry& entry) {
        if (!entry.is_regular_file()) return;
        if (ShouldSkipNotesFile(entry.path())) return;
        const std::string ext = ToLowerAscii(entry.path().extension().string());
        if (!ext.empty() && ext != ".md" && ext != ".txt" && ext != ".log") return;

        NotesFileEntry item;
        item.path = entry.path();
        item.title = WideToUtf8(item.path.stem().wstring());
        item.numberKey = ExtractFirstNumberKey(item.title);
        try {
            item.modifiedEpochSeconds = ToEpochSeconds(std::filesystem::last_write_time(item.path));
        } catch (...) {
            item.modifiedEpochSeconds = 0;
        }
        entries.push_back(std::move(item));
    };

    try {
        if (recursive) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(folderPath)) {
                consume(entry);
            }
        } else {
            for (const auto& entry : std::filesystem::directory_iterator(folderPath)) {
                consume(entry);
            }
        }
    } catch (...) {}
    return entries;
}

void ReloadListings(NotesOverlayState& st) {
    std::filesystem::path selectedGeneralPath;
    if (st.selectedGeneralEntryIndex >= 0 && st.selectedGeneralEntryIndex < static_cast<int>(st.generalEntries.size())) {
        selectedGeneralPath = st.generalEntries[static_cast<size_t>(st.selectedGeneralEntryIndex)].path;
    }
    std::filesystem::path selectedIgnPath;
    if (st.selectedIgnEntryIndex >= 0 && st.selectedIgnEntryIndex < static_cast<int>(st.ignEntries.size())) {
        selectedIgnPath = st.ignEntries[static_cast<size_t>(st.selectedIgnEntryIndex)].path;
    }

    ReloadGeneralFolders(st);
    LoadPinnedMetadata(st);
    LoadFavoriteMetadata(st);

    const std::filesystem::path generalFolder = ResolveGeneralFolderPath(st);
    const bool favoritesMode =
        st.selectedGeneralFolderIndex >= 0 && st.selectedGeneralFolderIndex < static_cast<int>(st.generalFolders.size()) &&
        IsGeneralFavoritesFolderKey(st.generalFolders[static_cast<size_t>(st.selectedGeneralFolderIndex)]);
    if (favoritesMode) {
        st.generalEntries = LoadNotesInDirectory(GetGeneralNotesRootPath(), true);
        st.generalEntries.erase(std::remove_if(st.generalEntries.begin(), st.generalEntries.end(),
                                               [&](const NotesFileEntry& item) { return !IsPathFavorited(st, item.path); }),
                                st.generalEntries.end());
    } else {
        st.generalEntries = LoadNotesInDirectory(generalFolder, false);
    }
    ApplyPinnedFlagsAndLabels(st.generalEntries, st, favoritesMode, GetGeneralNotesRootPath());
    SortEntries(st.generalEntries, static_cast<NotesSortMode>(std::clamp(st.generalSortMode, 0, 5)));
    st.selectedGeneralEntryIndex = -1;
    for (int i = 0; i < static_cast<int>(st.generalEntries.size()); ++i) {
        if (PathsEquivalentLoose(st.generalEntries[static_cast<size_t>(i)].path, selectedGeneralPath)) {
            st.selectedGeneralEntryIndex = i;
            break;
        }
    }

    const std::filesystem::path ignRoot = GetIgnNotesRootPath();
    st.ignEntries = LoadNotesInDirectory(ignRoot, true);
    ApplyPinnedFlagsAndLabels(st.ignEntries, st, false, ignRoot);
    SortEntries(st.ignEntries, static_cast<NotesSortMode>(std::clamp(st.ignSortMode, 0, 5)));
    st.selectedIgnEntryIndex = -1;
    for (int i = 0; i < static_cast<int>(st.ignEntries.size()); ++i) {
        if (PathsEquivalentLoose(st.ignEntries[static_cast<size_t>(i)].path, selectedIgnPath)) {
            st.selectedIgnEntryIndex = i;
            break;
        }
    }

    st.refreshRequested = false;
}

bool SaveIgnDraft(NotesOverlayState& st, bool inWorldNow, bool clearDraftAfterSave, bool requireEditedFlag, bool silent) {
    if (requireEditedFlag && !st.ignEditedSinceOpen) return false;
    if (!HasMeaningfulText(st.ignDraft)) {
        if (clearDraftAfterSave) {
            st.ignEditedSinceOpen = false;
            st.ignDraft.clear();
        }
        return false;
    }

    if (!inWorldNow) {
        if (!silent) SetStatus(st, "IGN save blocked (not in game).");
        return false;
    }

    std::filesystem::path target;
    bool updatingExisting = false;
    if (!st.ignEditingPath.empty()) {
        target = st.ignEditingPath;
        updatingExisting = true;
    } else {
        std::string filenameBase = CurrentDateStamp() + "_" + CurrentTimeStamp() + "_ign";
        filenameBase = SanitizeFileComponent(filenameBase);
        std::filesystem::path folder = GetIgnNotesRootPath();
        std::filesystem::create_directories(folder);
        target = BuildUniqueFilePath(folder, filenameBase, ".md");
    }

    if (!WriteUtf8TextFile(target, st.ignDraft)) {
        if (!silent) SetStatus(st, "Failed to save IGN note.");
        return false;
    }

    st.ignEditingPath = target;
    st.refreshRequested = true;
    st.ignEditedSinceOpen = false;
    if (!silent) SetStatus(st, std::string(updatingExisting ? "Updated: " : "Saved: ") + PathForDisplay(target));
    st.ignDraftDirty = false;

    if (clearDraftAfterSave) { st.ignDraft.clear(); }
    return true;
}

bool SaveGeneralToResolvedPath(NotesOverlayState& st, const std::filesystem::path& target, const std::string& title, const std::string& draft,
                               const char* statusVerb, bool silent) {
    if (!WriteUtf8TextFile(target, draft)) {
        if (!silent) SetStatus(st, "Failed to save general note.");
        return false;
    }

    st.generalEditingPath = target;
    st.generalTitle = title;
    st.generalDraft = draft;
    st.selectedGeneralEntryIndex = -1;
    st.refreshRequested = true;
    if (!silent && statusVerb && *statusVerb) { SetStatus(st, std::string(statusVerb) + ": " + PathForDisplay(target)); }
    st.generalDraftDirty = false;
    return true;
}

void ClearPendingSaveConflict(NotesOverlayState& st) {
    st.pendingSaveConflictTargetPath.clear();
    st.pendingSaveConflictCurrentPath.clear();
    st.pendingSaveConflictTitle.clear();
    st.pendingSaveConflictDraft.clear();
    st.pendingSaveConflictIsPdf = false;
    st.pendingSaveConflictOpenPopup = false;
}

void SaveIgnDraftOnCloseIfNeeded(NotesOverlayState& st, bool inWorldNow) {
    if (!st.ignEditedSinceOpen || !HasMeaningfulText(st.ignDraft)) {
        st.ignEditedSinceOpen = false;
        st.ignDraft.clear();
        st.ignEditingPath.clear();
        st.selectedIgnEntryIndex = -1;
        return;
    }

    if (SaveIgnDraft(st, inWorldNow, true, true, false)) {
        st.ignEditingPath.clear();
        st.selectedIgnEntryIndex = -1;
    }
}

bool SaveGeneralDraft(NotesOverlayState& st, bool silent) {
    const bool creatingNewFile = st.generalEditingPath.empty();
    std::string titleFromMarkdown = SanitizeFileComponent(ExtractMarkdownTitle(st.generalDraft));
    std::string titleFromInput = SanitizeFileComponent(st.generalTitle);
    std::string title = HasMeaningfulText(titleFromInput) ? titleFromInput : titleFromMarkdown;

    std::filesystem::path folder = ResolveGeneralFolderPath(st);
    if (st.selectedGeneralFolderIndex >= 0 && st.selectedGeneralFolderIndex < static_cast<int>(st.generalFolders.size()) &&
        IsGeneralFavoritesFolderKey(st.generalFolders[static_cast<size_t>(st.selectedGeneralFolderIndex)])) {
        folder = GetGeneralNotesRootPath();
    }
    if (title.empty() || title == "note") { title = BuildNextUntitledTitle(folder); }
    st.generalTitle = title;
    UpsertMarkdownTitle(st.generalDraft, title);
    if (creatingNewFile) { EnsureRuleUnderTopHeading(st.generalDraft); }

    std::filesystem::path target;
    if (!st.generalEditingPath.empty()) {
        target = st.generalEditingPath.parent_path() / Utf8ToWide(title + ".md");
        if (!PathsEquivalentLoose(st.generalEditingPath, target)) {
            if (std::filesystem::exists(target)) {
                target = BuildUniqueFilePath(target.parent_path(), title, ".md");
            }
            try {
                std::filesystem::create_directories(target.parent_path());
                std::filesystem::rename(st.generalEditingPath, target);
                st.generalEditingPath = target;
            } catch (...) {
                if (!silent) SetStatus(st, "Failed to rename current note.");
                try {
                    if (!WriteUtf8TextFile(target, st.generalDraft)) return false;
                    std::filesystem::remove(st.generalEditingPath);
                    st.generalEditingPath = target;
                } catch (...) { return false; }
            }
        }
    } else {
        std::filesystem::create_directories(folder);
        target = folder / Utf8ToWide(title + ".md");
        if (std::filesystem::exists(target)) {
            target = BuildUniqueFilePath(folder, title, ".md");
        }
        st.generalEditingPath = target;
    }

    return SaveGeneralToResolvedPath(st, target, title, st.generalDraft, "Saved", silent);
}

bool RenameGeneralCurrentNote(NotesOverlayState& st) {
    if (st.generalEditingPath.empty()) {
        SetStatus(st, "No loaded note to rename.");
        return false;
    }
    std::string title = SanitizeFileComponent(st.generalTitle);
    if (title.empty() || title == "note") {
        SetStatus(st, "Enter a note title first.");
        return false;
    }

    const std::filesystem::path target = st.generalEditingPath.parent_path() / Utf8ToWide(title + ".md");
    if (PathsEquivalentLoose(st.generalEditingPath, target)) {
        SetStatus(st, "Name unchanged.");
        return true;
    }
    if (std::filesystem::exists(target)) {
        QueueGeneralSaveConflict(st, target, st.generalEditingPath, title, st.generalDraft);
        return false;
    }

    try {
        std::filesystem::rename(st.generalEditingPath, target);
        st.generalEditingPath = target;
        st.generalTitle = title;
        st.refreshRequested = true;
        SetStatus(st, "Renamed: " + PathForDisplay(target));
        return true;
    } catch (...) {
        SetStatus(st, "Failed to rename note.");
        return false;
    }
}

bool DeleteNoteFile(NotesOverlayState& st, const std::filesystem::path& path, bool isIgn) {
    bool noteMissing = false;
    try {
        if (!std::filesystem::exists(path)) {
            noteMissing = true;
        } else if (!std::filesystem::remove(path)) {
            SetStatus(st, "Failed to delete note.");
            return false;
        }
    } catch (...) {
        SetStatus(st, "Failed to delete note.");
        return false;
    }

    const std::wstring key = NormalizePathKey(path);
    if (!key.empty()) {
        st.pinnedPathKeys.erase(key);
        st.favoritePathKeys.erase(key);
        SavePinnedMetadata(st);
        SaveFavoriteMetadata(st);
    }

    if (isIgn) {
        if (st.selectedIgnEntryIndex >= 0 && st.selectedIgnEntryIndex < static_cast<int>(st.ignEntries.size())) {
            const auto& selected = st.ignEntries[static_cast<size_t>(st.selectedIgnEntryIndex)].path;
            if (PathsEquivalentLoose(selected, path)) {
                st.selectedIgnEntryIndex = -1;
                st.ignDraft.clear();
                st.ignEditedSinceOpen = false;
                st.ignDraftDirty = false;
                st.ignLastEdit = std::chrono::steady_clock::time_point::min();
            }
        }
        if (!st.ignEditingPath.empty() && PathsEquivalentLoose(st.ignEditingPath, path)) {
            st.ignEditingPath.clear();
            st.ignDraft.clear();
            st.ignEditedSinceOpen = false;
            st.ignDraftDirty = false;
            st.ignLastEdit = std::chrono::steady_clock::time_point::min();
        }
    } else {
        if (st.selectedGeneralEntryIndex >= 0 && st.selectedGeneralEntryIndex < static_cast<int>(st.generalEntries.size())) {
            const auto& selected = st.generalEntries[static_cast<size_t>(st.selectedGeneralEntryIndex)].path;
            if (PathsEquivalentLoose(selected, path)) { st.selectedGeneralEntryIndex = -1; }
        }
        if (!st.generalEditingPath.empty() && PathsEquivalentLoose(st.generalEditingPath, path)) {
            st.generalEditingPath.clear();
            st.generalTitle.clear();
            st.generalDraft.clear();
            st.generalDraftDirty = false;
            st.generalLastEdit = std::chrono::steady_clock::time_point::min();
        }
    }

    st.refreshRequested = true;
    SetStatus(st, noteMissing ? "Note already removed." : "Deleted note.");
    return true;
}

bool AddGeneralFolder(NotesOverlayState& st) {
    const std::string raw = TrimAscii(st.newFolderName);
    if (raw.empty()) {
        SetStatus(st, "Folder name is empty.");
        return false;
    }
    std::string folderName = SanitizeFileComponent(raw);
    if (ShouldSkipGeneralFolder(ToLowerAscii(folderName)) || IsGeneralFavoritesFolderKey(folderName)) {
        SetStatus(st, "Folder name is reserved.");
        return false;
    }

    const std::filesystem::path folderPath = GetGeneralNotesRootPath() / Utf8ToWide(folderName);
    try {
        std::filesystem::create_directories(folderPath);
    } catch (...) {
        SetStatus(st, "Failed to create folder.");
        return false;
    }

    st.newFolderName.clear();
    st.refreshRequested = true;
    SetStatus(st, "Created folder " + folderName);
    return true;
}

bool RenderSortCombo(const char* label, int& modeValue) {
    static const char* kSortLabels[] = { "Date newest", "Date oldest", "Name A-Z", "Name Z-A", "Number asc", "Number desc" };
    int clamped = std::clamp(modeValue, 0, 5);
    bool changed = false;
    if (ImGui::BeginCombo(label, kSortLabels[clamped])) {
        for (int i = 0; i < 6; ++i) {
            const bool selected = (clamped == i);
            if (ImGui::Selectable(kSortLabels[i], selected)) {
                modeValue = i;
                changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

void RenderIgnTab(NotesOverlayState& st, bool inWorldNow) {
    if (RenderSortCombo("Sort##ign", st.ignSortMode)) { st.refreshRequested = true; }
    ImGui::SameLine();
    if (ImGui::Button("Clear Draft##ign")) {
        st.ignDraft.clear();
        st.ignEditedSinceOpen = false;
        st.ignDraftDirty = false;
        st.selectedIgnEntryIndex = -1;
        st.ignEditingPath.clear();
    }
    ImGui::SameLine();
    const bool canDeleteIgn = st.selectedIgnEntryIndex >= 0 && st.selectedIgnEntryIndex < static_cast<int>(st.ignEntries.size());
    ImGui::BeginDisabled(!canDeleteIgn);
    if (ImGui::Button("Delete##ign")) {
        st.pendingDeleteIsIgn = true;
        st.pendingDeletePath = st.ignEntries[static_cast<size_t>(st.selectedIgnEntryIndex)].path;
        st.pendingDeleteLabel = st.ignEntries[static_cast<size_t>(st.selectedIgnEntryIndex)].title;
        st.pendingDeleteOpenPopup = true;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    const bool canExportIgnPdf = HasMeaningfulText(st.ignDraft);
    ImGui::BeginDisabled(!canExportIgnPdf);
    if (ImGui::Button("Export PDF##ign")) {
        std::string exportTitle =
            (st.selectedIgnEntryIndex >= 0 && st.selectedIgnEntryIndex < static_cast<int>(st.ignEntries.size()))
                ? st.ignEntries[static_cast<size_t>(st.selectedIgnEntryIndex)].title
                : ("ign_" + CurrentDateStamp() + "_" + CurrentTimeStamp());
        ExportDraftToPdf(st, exportTitle, st.ignDraft, "IGN");
    }
    ImGui::EndDisabled();

    ImGui::TextDisabled(inWorldNow ? "IGN autosave enabled." : "IGN autosave paused (not in game).");

    const float listWidth = std::max(220.0f, ImGui::GetContentRegionAvail().x * 0.34f);
    ImGui::BeginChild("##ign_notes_list", ImVec2(listWidth, 0.0f), true);
    if (ImGui::BeginTable("##ign_note_table", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("P", ImGuiTableColumnFlags_WidthFixed, 24.0f);
        ImGui::TableSetupColumn("F", ImGuiTableColumnFlags_WidthFixed, 24.0f);
        ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthStretch);
        for (int i = 0; i < static_cast<int>(st.ignEntries.size()); ++i) {
            NotesFileEntry& entry = st.ignEntries[static_cast<size_t>(i)];
            const bool selected = (st.selectedIgnEntryIndex == i);

            ImGui::TableNextRow();
            ImGui::PushID(i);
            ImGui::TableSetColumnIndex(0);
            if (RenderIconToggleButton("##pin", s_pinIcon, entry.pinned, "P", entry.pinned ? "Unpin" : "Pin")) {
                if (SetPathPinned(st, entry.path, !entry.pinned)) {
                    entry.pinned = !entry.pinned;
                    SetStatus(st, std::string(entry.pinned ? "Pinned: " : "Unpinned: ") + entry.title);
                }
            }
            ImGui::TableSetColumnIndex(1);
            if (RenderIconToggleButton("##fav", s_starIcon, entry.favorite, "*", entry.favorite ? "Unfavorite" : "Favorite")) {
                if (SetPathFavorited(st, entry.path, !entry.favorite)) {
                    entry.favorite = !entry.favorite;
                    SetStatus(st, std::string(entry.favorite ? "Favorited: " : "Unfavorited: ") + entry.title);
                }
            }
            ImGui::TableSetColumnIndex(2);
            if (selected) {
                ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(70, 105, 146, 220));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(75, 114, 159, 240));
                ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(80, 120, 164, 255));
            }
            if (ImGui::Selectable(entry.displayLabel.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
                std::string loaded;
                if (ReadUtf8TextFile(entry.path, loaded)) {
                    st.ignDraft = loaded;
                    st.ignEditedSinceOpen = false;
                    st.ignDraftDirty = false;
                    st.selectedIgnEntryIndex = i;
                    st.ignEditingPath = entry.path;
                    SetStatus(st, "Loaded IGN note.");
                    st.focusIgnEditorNextFrame = true;
                } else {
                    SetStatus(st, "Failed to read IGN note.");
                }
            }
            if (selected) { ImGui::PopStyleColor(3); }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##ign_editor", ImVec2(0.0f, 0.0f), true);
    if (ImGui::BeginTabBar("##ign_editor_tabs")) {
        if (ImGui::BeginTabItem("Edit##ign")) {
            st.ignEditorTab = 0;
            if (st.focusIgnEditorNextFrame) {
                ImGui::SetKeyboardFocusHere();
                st.focusIgnEditorNextFrame = false;
            }
            const size_t ignSizeBeforeEdit = st.ignDraft.size();
            const bool ignChanged =
                ImGui::InputTextMultiline("##ign_draft", &st.ignDraft, ImVec2(-FLT_MIN, -ImGui::GetFrameHeightWithSpacing() * 2.2f),
                                          ImGuiInputTextFlags_AllowTabInput);
            if (ignChanged) {
                ApplyAutoListContinuation(st.ignDraft, ignSizeBeforeEdit);
                if (HasMeaningfulText(st.ignDraft)) { MarkIgnDraftDirty(st); }
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Preview##ign")) {
            st.ignEditorTab = 1;
            ImGui::BeginChild("##ign_preview", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing() * 2.2f), false,
                              ImGuiWindowFlags_HorizontalScrollbar);
            if (RenderMarkdownPreview(st.ignDraft)) { MarkIgnDraftDirty(st); }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::TextDisabled("Markdown preview: headings/lists/tasks/quotes/code + link open. Export preserves markdown formatting.");
    ImGui::EndChild();
}

void RenderGeneralTab(NotesOverlayState& st, float panelScale) {
    const int folderCount = static_cast<int>(st.generalFolders.size());
    if (folderCount > 0) {
        st.selectedGeneralFolderIndex = std::clamp(st.selectedGeneralFolderIndex, 0, folderCount - 1);
        st.generalFolderTabOffset = std::clamp(st.generalFolderTabOffset, 0, std::max(0, folderCount - 1));
    } else {
        st.selectedGeneralFolderIndex = 0;
        st.generalFolderTabOffset = 0;
    }

    const float folderArrowW = 26.0f * panelScale;
    const float folderInputW = 160.0f * panelScale;
    const float folderAvailW = std::max(180.0f, ImGui::GetContentRegionAvail().x - (folderArrowW * 2.0f) - folderInputW - 130.0f);
    int maxVisibleTabs = std::clamp(static_cast<int>(folderAvailW / (120.0f * panelScale)), 1, 8);
    if (folderCount < maxVisibleTabs) maxVisibleTabs = folderCount;
    if (st.selectedGeneralFolderIndex < st.generalFolderTabOffset) { st.generalFolderTabOffset = st.selectedGeneralFolderIndex; }
    if (maxVisibleTabs > 0 && st.selectedGeneralFolderIndex >= st.generalFolderTabOffset + maxVisibleTabs) {
        st.generalFolderTabOffset = st.selectedGeneralFolderIndex - maxVisibleTabs + 1;
    }
    st.generalFolderTabOffset = std::clamp(st.generalFolderTabOffset, 0, std::max(0, folderCount - maxVisibleTabs));

    const bool allowFolderArrowKeys =
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive() && !ImGui::GetIO().WantTextInput;
    if (allowFolderArrowKeys && folderCount > 0) {
        int nextFolderIndex = st.selectedGeneralFolderIndex;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false)) {
            nextFolderIndex = std::max(0, st.selectedGeneralFolderIndex - 1);
        } else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) {
            nextFolderIndex = std::min(folderCount - 1, st.selectedGeneralFolderIndex + 1);
        }
        if (nextFolderIndex != st.selectedGeneralFolderIndex) {
            st.selectedGeneralFolderIndex = nextFolderIndex;
            st.selectedGeneralEntryIndex = -1;
            st.generalEditingPath.clear();
            st.refreshRequested = true;
        }
    }

    ImGui::SetNextItemWidth(folderInputW);
    ImGui::InputTextWithHint("##new_general_folder", "new folder", &st.newFolderName);
    ImGui::SameLine();
    if (ImGui::Button("+ Folder")) { AddGeneralFolder(st); }
    ImGui::SameLine();
    ImGui::BeginDisabled(st.generalFolderTabOffset <= 0 || folderCount <= 1);
    if (ImGui::Button("<##GeneralFolderLeft", ImVec2(folderArrowW, 0.0f))) { st.generalFolderTabOffset = std::max(0, st.generalFolderTabOffset - 1); }
    ImGui::EndDisabled();
    ImGui::SameLine();

    if (folderCount > 0) {
        const int tabStart = st.generalFolderTabOffset;
        const int tabEnd = std::min(folderCount, tabStart + std::max(1, maxVisibleTabs));
        for (int i = tabStart; i < tabEnd; ++i) {
            if (i > tabStart) ImGui::SameLine();
            const bool selected = (st.selectedGeneralFolderIndex == i);
            const std::string tabLabel = GeneralFolderDisplayLabel(st.generalFolders[static_cast<size_t>(i)]) + "##Folder" + std::to_string(i);
            if (selected) {
                ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(62, 94, 128, 230));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(72, 108, 146, 240));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(80, 118, 158, 255));
            }
            if (ImGui::Button(tabLabel.c_str())) {
                st.selectedGeneralFolderIndex = i;
                st.selectedGeneralEntryIndex = -1;
                st.generalEditingPath.clear();
                st.refreshRequested = true;
            }
            if (selected) { ImGui::PopStyleColor(3); }
        }
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(folderCount <= 1 || (st.generalFolderTabOffset + std::max(1, maxVisibleTabs)) >= folderCount);
    if (ImGui::Button(">##GeneralFolderRight", ImVec2(folderArrowW, 0.0f))) {
        st.generalFolderTabOffset = std::min(std::max(0, folderCount - std::max(1, maxVisibleTabs)), st.generalFolderTabOffset + 1);
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::SetNextItemWidth(165.0f * panelScale);
    if (RenderSortCombo("Sort##general", st.generalSortMode)) { st.refreshRequested = true; }
    ImGui::SameLine();
    if (ImGui::Button("New##general")) {
        std::filesystem::path folder = ResolveGeneralFolderPath(st);
        if (st.selectedGeneralFolderIndex >= 0 && st.selectedGeneralFolderIndex < static_cast<int>(st.generalFolders.size()) &&
            IsGeneralFavoritesFolderKey(st.generalFolders[static_cast<size_t>(st.selectedGeneralFolderIndex)])) {
            folder = GetGeneralNotesRootPath();
        }
        st.pendingNewGeneralNoteName = BuildNextUntitledTitle(folder);
        st.pendingNewGeneralNotePopupOpen = true;
    }
    ImGui::SameLine();
    const bool canDeleteGeneral = st.selectedGeneralEntryIndex >= 0 && st.selectedGeneralEntryIndex < static_cast<int>(st.generalEntries.size());
    ImGui::BeginDisabled(!canDeleteGeneral);
    if (ImGui::Button("Delete##general")) {
        st.pendingDeleteIsIgn = false;
        st.pendingDeletePath = st.generalEntries[static_cast<size_t>(st.selectedGeneralEntryIndex)].path;
        st.pendingDeleteLabel = st.generalEntries[static_cast<size_t>(st.selectedGeneralEntryIndex)].title;
        st.pendingDeleteOpenPopup = true;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    const bool canExportGeneralPdf = HasMeaningfulText(st.generalDraft);
    ImGui::BeginDisabled(!canExportGeneralPdf);
    if (ImGui::Button("Export PDF##general")) {
        std::string exportTitle = SanitizeFileComponent(st.generalTitle);
        if (!HasMeaningfulText(exportTitle)) { exportTitle = GuessTitleFromPath(st.generalEditingPath); }
        if (!HasMeaningfulText(exportTitle)) { exportTitle = "note_" + CurrentDateStamp() + "_" + CurrentTimeStamp(); }
        ExportDraftToPdf(st, exportTitle, st.generalDraft, "General");
    }
    ImGui::EndDisabled();

    if (st.pendingNewGeneralNotePopupOpen) {
        ImGui::OpenPopup("New Note");
        st.pendingNewGeneralNotePopupOpen = false;
    }
    if (ImGui::BeginPopupModal("New Note", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Name:");
        ImGui::SetNextItemWidth(280.0f);
        const bool submit = ImGui::InputTextWithHint("##new_note_name_popup", "untitled_1", &st.pendingNewGeneralNoteName,
                                                      ImGuiInputTextFlags_EnterReturnsTrue);

        std::filesystem::path folder = ResolveGeneralFolderPath(st);
        if (st.selectedGeneralFolderIndex >= 0 && st.selectedGeneralFolderIndex < static_cast<int>(st.generalFolders.size()) &&
            IsGeneralFavoritesFolderKey(st.generalFolders[static_cast<size_t>(st.selectedGeneralFolderIndex)])) {
            folder = GetGeneralNotesRootPath();
        }

        auto createFromPopup = [&]() {
            std::string title = SanitizeFileComponent(TrimAscii(st.pendingNewGeneralNoteName));
            if (!HasMeaningfulText(title)) title = BuildNextUntitledTitle(folder);
            st.generalTitle = title;
            st.generalDraft = BuildDefaultNewNoteMarkdown(st.generalTitle);
            st.generalEditingPath.clear();
            st.selectedGeneralEntryIndex = -1;
            st.focusGeneralEditorNextFrame = true;
            MarkGeneralDraftDirty(st);
            ImGui::CloseCurrentPopup();
        };

        if (submit || ImGui::Button("Create", ImVec2(120.0f, 0.0f))) { createFromPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f))) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    const float listWidth = std::max(220.0f, ImGui::GetContentRegionAvail().x * 0.34f);
    ImGui::BeginChild("##general_notes_list", ImVec2(listWidth, 0.0f), true);
    if (ImGui::BeginTable("##general_note_table", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("P", ImGuiTableColumnFlags_WidthFixed, 24.0f);
        ImGui::TableSetupColumn("F", ImGuiTableColumnFlags_WidthFixed, 24.0f);
        ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthStretch);
        for (int i = 0; i < static_cast<int>(st.generalEntries.size()); ++i) {
            NotesFileEntry& entry = st.generalEntries[static_cast<size_t>(i)];
            const bool selected = (st.selectedGeneralEntryIndex == i);

            ImGui::TableNextRow();
            ImGui::PushID(i);
            ImGui::TableSetColumnIndex(0);
            if (RenderIconToggleButton("##pin", s_pinIcon, entry.pinned, "P", entry.pinned ? "Unpin" : "Pin")) {
                if (SetPathPinned(st, entry.path, !entry.pinned)) {
                    entry.pinned = !entry.pinned;
                    SetStatus(st, std::string(entry.pinned ? "Pinned: " : "Unpinned: ") + entry.title);
                }
            }
            ImGui::TableSetColumnIndex(1);
            if (RenderIconToggleButton("##fav", s_starIcon, entry.favorite, "*", entry.favorite ? "Unfavorite" : "Favorite")) {
                if (SetPathFavorited(st, entry.path, !entry.favorite)) {
                    entry.favorite = !entry.favorite;
                    SetStatus(st, std::string(entry.favorite ? "Favorited: " : "Unfavorited: ") + entry.title);
                }
            }
            ImGui::TableSetColumnIndex(2);
            if (selected) {
                ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(70, 105, 146, 220));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(75, 114, 159, 240));
                ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(80, 120, 164, 255));
            }
            if (ImGui::Selectable(entry.displayLabel.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
                std::string loaded;
                if (ReadUtf8TextFile(entry.path, loaded)) {
                    st.generalDraft = loaded;
                    st.generalTitle = ExtractMarkdownTitle(st.generalDraft);
                    if (!HasMeaningfulText(st.generalTitle)) st.generalTitle = entry.title;
                    st.generalEditingPath = entry.path;
                    st.selectedGeneralEntryIndex = i;
                    st.generalDraftDirty = false;
                    SetStatus(st, "Loaded note.");
                    st.focusGeneralEditorNextFrame = true;
                } else {
                    SetStatus(st, "Failed to read note.");
                }
            }
            if (selected) { ImGui::PopStyleColor(3); }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##general_editor", ImVec2(0.0f, 0.0f), true);
    if (ImGui::BeginTabBar("##general_editor_tabs")) {
        if (ImGui::BeginTabItem("Edit##general")) {
            st.generalEditorTab = 0;
            if (st.focusGeneralEditorNextFrame) {
                ImGui::SetKeyboardFocusHere();
                st.focusGeneralEditorNextFrame = false;
            }
            const size_t generalSizeBeforeEdit = st.generalDraft.size();
            if (ImGui::InputTextMultiline("##general_draft", &st.generalDraft, ImVec2(-FLT_MIN, -ImGui::GetFrameHeightWithSpacing() * 2.2f),
                                          ImGuiInputTextFlags_AllowTabInput)) {
                ApplyAutoListContinuation(st.generalDraft, generalSizeBeforeEdit);
                const std::string titleFromMarkdown = ExtractMarkdownTitle(st.generalDraft);
                if (HasMeaningfulText(titleFromMarkdown)) { st.generalTitle = titleFromMarkdown; }
                MarkGeneralDraftDirty(st);
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Preview##general")) {
            st.generalEditorTab = 1;
            ImGui::BeginChild("##general_preview", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing() * 2.2f), false,
                              ImGuiWindowFlags_HorizontalScrollbar);
            if (RenderMarkdownPreview(st.generalDraft)) {
                const std::string titleFromMarkdown = ExtractMarkdownTitle(st.generalDraft);
                if (HasMeaningfulText(titleFromMarkdown)) { st.generalTitle = titleFromMarkdown; }
                MarkGeneralDraftDirty(st);
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::TextDisabled("Autosaves while editing.");
    ImGui::EndChild();
}

void RunNotesAutoSaveTick(NotesOverlayState& st, bool inWorldNow) {
    const auto now = std::chrono::steady_clock::now();
    constexpr auto kAutosaveDelay = std::chrono::milliseconds(450);

    if (st.generalDraftDirty && st.generalLastEdit != std::chrono::steady_clock::time_point::min() &&
        (now - st.generalLastEdit) >= kAutosaveDelay) {
        SaveGeneralDraft(st, true);
    }

    if (st.ignDraftDirty && st.ignLastEdit != std::chrono::steady_clock::time_point::min() && (now - st.ignLastEdit) >= kAutosaveDelay) {
        SaveIgnDraft(st, inWorldNow, false, false, true);
    }
}

void RenderDeletePopup(NotesOverlayState& st) {
    if (st.pendingDeleteOpenPopup) {
        ImGui::OpenPopup("Delete Note?");
        st.pendingDeleteOpenPopup = false;
    }

    if (!ImGui::BeginPopupModal("Delete Note?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::Text("Delete note:");
    ImGui::TextWrapped("%s", st.pendingDeleteLabel.empty() ? "(unnamed)" : st.pendingDeleteLabel.c_str());
    ImGui::Separator();
    if (ImGui::Button("Delete", ImVec2(120.0f, 0.0f))) {
        DeleteNoteFile(st, st.pendingDeletePath, st.pendingDeleteIsIgn);
        st.pendingDeletePath.clear();
        st.pendingDeleteLabel.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f))) {
        st.pendingDeletePath.clear();
        st.pendingDeleteLabel.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void RenderSaveConflictPopup(NotesOverlayState& st) {
    if (st.pendingSaveConflictOpenPopup) {
        ImGui::OpenPopup("File Already Exists");
        st.pendingSaveConflictOpenPopup = false;
    }

    if (!ImGui::BeginPopupModal("File Already Exists", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::TextUnformatted("A file with this name already exists.");
    ImGui::TextWrapped("%s", PathForDisplay(st.pendingSaveConflictTargetPath).c_str());
    if (!st.pendingSaveConflictIsPdf && !st.pendingSaveConflictCurrentPath.empty() &&
        !PathsEquivalentLoose(st.pendingSaveConflictCurrentPath, st.pendingSaveConflictTargetPath)) {
        ImGui::TextDisabled("Current note: %s", PathForDisplay(st.pendingSaveConflictCurrentPath).c_str());
    }
    ImGui::Separator();

    if (ImGui::Button("Overwrite", ImVec2(130.0f, 0.0f))) {
        bool ok = false;
        if (st.pendingSaveConflictIsPdf) {
            ok = WriteSimpleMarkdownPdf(st.pendingSaveConflictTargetPath, st.pendingSaveConflictTitle, st.pendingSaveConflictDraft);
            if (ok) {
                FinalizePdfExportSuccess(st, st.pendingSaveConflictTargetPath);
            } else {
                SetStatus(st, "Failed to export PDF.");
            }
        } else {
            ok = SaveGeneralToResolvedPath(st, st.pendingSaveConflictTargetPath, st.pendingSaveConflictTitle, st.pendingSaveConflictDraft,
                                           "Overwrote", false);
        }
        if (ok) {
            ClearPendingSaveConflict(st);
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Save As New (+1)", ImVec2(150.0f, 0.0f))) {
        std::string baseName = "note";
        try {
            baseName = WideToUtf8(st.pendingSaveConflictTargetPath.stem().wstring());
        } catch (...) {}
        baseName = SanitizeFileComponent(baseName);
        std::string ext = st.pendingSaveConflictIsPdf ? ".pdf" : ".md";
        try {
            const std::string rawExt = st.pendingSaveConflictTargetPath.extension().string();
            if (!rawExt.empty()) ext = rawExt;
        } catch (...) {}
        const std::filesystem::path copyPath =
            BuildUniqueFilePath(st.pendingSaveConflictTargetPath.parent_path(), baseName, ext);
        bool ok = false;
        if (st.pendingSaveConflictIsPdf) {
            ok = WriteSimpleMarkdownPdf(copyPath, st.pendingSaveConflictTitle, st.pendingSaveConflictDraft);
            if (ok) {
                FinalizePdfExportSuccess(st, copyPath);
            } else {
                SetStatus(st, "Failed to export PDF.");
            }
        } else {
            const std::string copyTitle = GuessTitleFromPath(copyPath);
            ok = SaveGeneralToResolvedPath(st, copyPath, copyTitle, st.pendingSaveConflictDraft, "Saved", false);
        }
        if (ok) {
            ClearPendingSaveConflict(st);
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(110.0f, 0.0f))) {
        SetStatus(st, "Save cancelled.");
        ClearPendingSaveConflict(st);
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

} // namespace

bool HandleNotesOverlayToggleHotkey(unsigned int keyVk, bool ctrlDown, bool shiftDown, bool altDown) {
    auto cfgSnap = GetConfigSnapshot();
    if (!cfgSnap || !cfgSnap->notesOverlay.enabled) return false;
    const int configuredVk = std::clamp(cfgSnap->notesOverlay.hotkeyKey, 1, 255);
    if (static_cast<int>(keyVk) != configuredVk) return false;
    if (ctrlDown != cfgSnap->notesOverlay.hotkeyCtrl) return false;
    if (shiftDown != cfgSnap->notesOverlay.hotkeyShift) return false;
    if (altDown != cfgSnap->notesOverlay.hotkeyAlt) return false;

    std::lock_guard<std::mutex> lock(s_notesMutex);
    EnsureInitializedLocked(s_notes, *cfgSnap);
    EnsureStorageDraftInitialized(s_notes, *cfgSnap);
    const bool inWorldNow = IsInWorldNow();

    const bool wasVisible = s_notes.visible;
    s_notes.visible = !s_notes.visible;
    if (s_notes.visible) {
        s_notes.refreshRequested = true;
        s_notes.lastAutoRefresh = std::chrono::steady_clock::time_point::min();
        s_notes.storageDraftInitialized = false;
        s_notes.activeTab = inWorldNow ? 0 : 1;
        s_notes.forceTabSelectionNextFrame = true;
        s_notes.ignEditorTab = 0;
        s_notes.generalEditorTab = 0;
        s_notes.focusIgnEditorNextFrame = inWorldNow;
        s_notes.focusGeneralEditorNextFrame = !inWorldNow;
    } else if (wasVisible) {
        s_pendingIgnAutoSaveOnClose.store(true, std::memory_order_release);
    }
    return true;
}

bool IsNotesOverlayVisible() {
    auto cfgSnap = GetConfigSnapshot();
    if (!cfgSnap || !cfgSnap->notesOverlay.enabled) return false;

    std::lock_guard<std::mutex> lock(s_notesMutex);
    EnsureInitializedLocked(s_notes, *cfgSnap);
    return s_notes.visible;
}

bool IsNotesOverlayInputCaptureActive() { return IsNotesOverlayVisible(); }

bool HasNotesOverlayPendingWork() {
    if (s_pendingIgnAutoSaveOnClose.load(std::memory_order_acquire)) return true;
    return IsNotesOverlayVisible();
}

void RenderNotesOverlayImGui() {
    auto cfgSnap = GetConfigSnapshot();
    if (!cfgSnap) return;
    const bool inWorldNow = IsInWorldNow();

    std::lock_guard<std::mutex> lock(s_notesMutex);
    EnsureInitializedLocked(s_notes, *cfgSnap);

    if (!cfgSnap->notesOverlay.enabled) {
        s_notes.visible = false;
        s_pendingIgnAutoSaveOnClose.store(false, std::memory_order_release);
        return;
    }

    if (s_pendingIgnAutoSaveOnClose.exchange(false, std::memory_order_acq_rel)) {
        SaveIgnDraftOnCloseIfNeeded(s_notes, inWorldNow);
    }

    if (!s_notes.visible) return;

    const auto now = std::chrono::steady_clock::now();
    if (s_notes.lastAutoRefresh == std::chrono::steady_clock::time_point::min() ||
        (now - s_notes.lastAutoRefresh) >= std::chrono::milliseconds(1000)) {
        s_notes.refreshRequested = true;
        s_notes.lastAutoRefresh = now;
    }

    RunNotesAutoSaveTick(s_notes, inWorldNow);
    if (s_notes.refreshRequested) { ReloadListings(s_notes); }

    EnsureNotesIconTexturesLoaded();

    const float bgAlpha = std::clamp(cfgSnap->notesOverlay.backgroundOpacity, 0.0f, 1.0f);
    const float panelScale = std::clamp(cfgSnap->notesOverlay.panelScale, 0.75f, 1.5f);

    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(displaySize, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(bgAlpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    const ImGuiWindowFlags backdropFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                           ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::Begin("##notes_overlay_backdrop", nullptr, backdropFlags)) {
        const float panelW = std::clamp(displaySize.x * 0.78f * panelScale, 700.0f, std::max(700.0f, displaySize.x - 32.0f));
        const float panelH = std::clamp(displaySize.y * 0.82f * panelScale, 520.0f, std::max(520.0f, displaySize.y - 28.0f));
        const ImVec2 panelPos((displaySize.x - panelW) * 0.5f, (displaySize.y - panelH) * 0.5f);

        ImGui::SetCursorPos(panelPos);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
        if (ImGui::BeginChild("##notes_overlay_panel", ImVec2(panelW, panelH), true, ImGuiWindowFlags_None)) {
            ImGui::TextUnformatted("Notes");
            ImGui::SameLine();
            ImGui::TextDisabled("%s close", FormatNotesHotkeyLabel(cfgSnap->notesOverlay).c_str());

            if (ImGui::CollapsingHeader("Storage & Export")) {
                ImGui::SetNextItemWidth(std::max(320.0f, panelW * 0.38f));
                ImGui::InputTextWithHint("MD Dir", "notes/General", &s_notes.markdownDirDraft);
                ImGui::SetNextItemWidth(std::max(320.0f, panelW * 0.38f));
                ImGui::InputTextWithHint("PDF Dir", "notes/PDF", &s_notes.pdfDirDraft);

                if (ImGui::Button("Apply Dirs")) { ApplyStorageDraft(s_notes); }
                ImGui::SameLine();
                if (ImGui::Button("Reset Dirs")) {
                    s_notes.markdownDirDraft = "notes/General";
                    s_notes.pdfDirDraft = "notes/PDF";
                    ApplyStorageDraft(s_notes);
                }

                bool openFolderAfterExport = g_config.notesOverlay.openPdfFolderAfterExport;
                if (ImGui::Checkbox("Open PDF folder after export", &openFolderAfterExport)) {
                    g_config.notesOverlay.openPdfFolderAfterExport = openFolderAfterExport;
                    g_configIsDirty = true;
                    PublishConfigSnapshot();
                    SetStatus(s_notes, openFolderAfterExport ? "Will open PDF folder after export."
                                                             : "Will not open PDF folder after export.");
                }
                ImGui::TextDisabled("Folders are auto-created when notes are saved/exported.");
            }

            if (!inWorldNow && s_notes.activeTab == 0) {
                s_notes.activeTab = 1;
                s_notes.forceTabSelectionNextFrame = true;
                s_notes.focusGeneralEditorNextFrame = true;
            }

            if (ImGui::BeginTabBar("##notes_tabs")) {
                const bool selectIgn = s_notes.forceTabSelectionNextFrame && s_notes.activeTab == 0;
                const bool selectGeneral = s_notes.forceTabSelectionNextFrame && s_notes.activeTab == 1;

                ImGui::BeginDisabled(!inWorldNow);
                const ImGuiTabItemFlags ignFlags = selectIgn ? ImGuiTabItemFlags_SetSelected : 0;
                if (ImGui::BeginTabItem("IGN", nullptr, ignFlags)) {
                    s_notes.activeTab = 0;
                    RenderIgnTab(s_notes, inWorldNow);
                    ImGui::EndTabItem();
                }
                ImGui::EndDisabled();

                const ImGuiTabItemFlags generalFlags = selectGeneral ? ImGuiTabItemFlags_SetSelected : 0;
                if (ImGui::BeginTabItem("General", nullptr, generalFlags)) {
                    s_notes.activeTab = 1;
                    RenderGeneralTab(s_notes, panelScale);
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
                s_notes.forceTabSelectionNextFrame = false;
            }

            RenderDeletePopup(s_notes);
            RenderSaveConflictPopup(s_notes);

            const auto now = std::chrono::steady_clock::now();
            if (!s_notes.statusText.empty() && now <= s_notes.statusUntil) {
                ImGui::Separator();
                ImGui::TextColored(ImVec4(0.72f, 0.92f, 0.75f, 1.0f), "%s", s_notes.statusText.c_str());
            }
            ImGui::EndChild();
        }
        ImGui::PopStyleVar();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}
