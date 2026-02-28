if (ImGui::BeginTabItem("[P] Practice")) {
    g_currentlyEditingMirror = "";
    g_imageDragMode.store(false);
    g_windowOverlayDragMode.store(false);

    struct PracticeCatalogEntry {
        std::string label;
        std::string creator;
        std::string url;
        std::string storageName;
    };
    struct PracticeMapEntry {
        std::string mapName;
        std::filesystem::path rootPath;
        std::filesystem::path basePath;
        std::filesystem::path instancePath;
        bool hasBase = false;
        bool hasInstance = false;
        bool hasSaveInInstance = false;
    };

    static std::vector<PracticeCatalogEntry> s_catalog;
    static std::vector<PracticeMapEntry> s_maps;
    static int s_selectedCatalog = -1;
    static int s_selectedMap = -1;
    static bool s_initialized = false;
    static bool s_installRunning = false;
    static std::future<BoatSetupScriptRunResult> s_installFuture;
    static BoatSetupScriptRunResult s_lastInstallResult;
    static bool s_hasInstallResult = false;
    static std::string s_status;
    static std::chrono::steady_clock::time_point s_statusUntil{};
    static std::string s_catalogPathDisplay = "(not found)";
    static std::filesystem::path s_libraryOverrideRoot;
    static std::filesystem::path s_instanceOverrideSavesRoot;
    static bool s_pathOverridesLoaded = false;
    struct PracticeMapCoverEntry {
        GLuint textureId = 0;
        int width = 0;
        int height = 0;
        std::filesystem::path iconPath;
        std::filesystem::file_time_type iconWriteTime{};
        bool iconWriteTimeValid = false;
    };
    static std::map<std::wstring, PracticeMapCoverEntry> s_mapCoverCache;

    auto setStatus = [&](const std::string& msg, float seconds = 4.0f) {
        s_status = msg;
        s_statusUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int>(seconds * 1000.0f));
    };

    auto sanitizeWorldName = [](const std::string& input) -> std::string {
        std::string out;
        out.reserve(input.size());
        for (char ch : input) {
            const bool invalid = (ch == '<' || ch == '>' || ch == ':' || ch == '"' || ch == '/' || ch == '\\' || ch == '|' || ch == '?' ||
                                  ch == '*');
            out.push_back(invalid ? '_' : ch);
        }
        while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
        while (!out.empty() && (out.front() == ' ' || out.front() == '.')) out.erase(out.begin());
        if (out.empty()) out = "Practice_Map";
        return out;
    };

    auto worldFolderValid = [](const std::filesystem::path& worldPath) -> bool {
        std::error_code ec;
        if (!std::filesystem::exists(worldPath, ec) || ec || !std::filesystem::is_directory(worldPath, ec) || ec) return false;
        return std::filesystem::exists(worldPath / L"level.dat", ec) && !ec;
    };

    auto getMapCacheKey = [](const std::filesystem::path& p) {
        std::filesystem::path n = p.lexically_normal();
        std::wstring key = n.wstring();
        std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return key;
    };

    auto releaseMapCoverEntry = [](PracticeMapCoverEntry& entry) {
        if (entry.textureId != 0) {
            glDeleteTextures(1, &entry.textureId);
            entry.textureId = 0;
        }
        entry.width = 0;
        entry.height = 0;
        entry.iconPath.clear();
        entry.iconWriteTimeValid = false;
    };

    auto findMapIconPath = [&](const PracticeMapEntry& map) -> std::filesystem::path {
        const std::filesystem::path candidates[] = { map.instancePath / L"icon.png", map.basePath / L"icon.png", map.rootPath / L"icon.png" };
        std::error_code ec;
        for (const auto& path : candidates) {
            ec.clear();
            if (std::filesystem::exists(path, ec) && !ec && std::filesystem::is_regular_file(path, ec) && !ec) return path;
        }
        return {};
    };

    auto getOrLoadMapCoverTexture = [&](const PracticeMapEntry& map) -> GLuint {
        const std::wstring cacheKey = getMapCacheKey(map.rootPath);
        auto& entry = s_mapCoverCache[cacheKey];
        const std::filesystem::path iconPath = findMapIconPath(map);
        if (iconPath.empty()) {
            releaseMapCoverEntry(entry);
            return 0;
        }

        std::error_code ec;
        const auto writeTime = std::filesystem::last_write_time(iconPath, ec);
        const bool writeTimeValid = !ec;

        if (entry.textureId != 0 && entry.iconPath == iconPath && entry.iconWriteTimeValid == writeTimeValid &&
            (!writeTimeValid || entry.iconWriteTime == writeTime)) {
            return entry.textureId;
        }

        releaseMapCoverEntry(entry);

        int width = 0, height = 0, channels = 0;
        stbi_set_flip_vertically_on_load(false);
        const std::string iconUtf8 = WideToUtf8(iconPath.wstring());
        unsigned char* rgbaData = stbi_load(iconUtf8.c_str(), &width, &height, &channels, 4);
        if (!rgbaData || width <= 0 || height <= 0) {
            if (rgbaData) stbi_image_free(rgbaData);
            entry.iconPath = iconPath;
            entry.iconWriteTimeValid = writeTimeValid;
            if (writeTimeValid) entry.iconWriteTime = writeTime;
            return 0;
        }

        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgbaData);
        stbi_image_free(rgbaData);

        entry.textureId = tex;
        entry.width = width;
        entry.height = height;
        entry.iconPath = iconPath;
        entry.iconWriteTimeValid = writeTimeValid;
        if (writeTimeValid) entry.iconWriteTime = writeTime;
        return entry.textureId;
    };

    auto getPersistentToolscreenRoot = [&]() -> std::filesystem::path {
        // Keep practice worlds in the user profile config area so they survive
        // launcher updates and instance save replacement.
        wchar_t userProfileBuf[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"USERPROFILE", userProfileBuf, MAX_PATH) > 0) {
            const std::filesystem::path preferredRoot = std::filesystem::path(userProfileBuf) / L".config" / L"toolscreen";
            std::error_code ec;
            std::filesystem::create_directories(preferredRoot, ec);
            if (!ec) return preferredRoot;
        }

        if (!g_toolscreenPath.empty()) {
            const std::filesystem::path configuredRoot = std::filesystem::path(g_toolscreenPath);
            std::error_code ec;
            std::filesystem::create_directories(configuredRoot, ec);
            if (!ec) return configuredRoot;
        }

        wchar_t cwdBuf[MAX_PATH] = {};
        if (GetCurrentDirectoryW(MAX_PATH, cwdBuf) > 0) {
            const std::filesystem::path fallbackRoot = std::filesystem::path(cwdBuf) / L"toolscreen";
            std::error_code ec;
            std::filesystem::create_directories(fallbackRoot, ec);
            if (!ec) return fallbackRoot;
        }
        return std::filesystem::path(L".");
    };

    auto getPathsConfigPath = [&]() -> std::filesystem::path {
        return getPersistentToolscreenRoot() / L"practice_maps" / L"paths.json";
    };

    auto repairLegacyDotConfigPath = [&](const std::filesystem::path& input) -> std::filesystem::path {
        if (input.empty()) return input;
        std::wstring raw = input.lexically_normal().wstring();

        const std::wstring marker = L"\\Users\\";
        const size_t usersPos = raw.find(marker);
        if (usersPos == std::wstring::npos) return input;

        const size_t usernameStart = usersPos + marker.size();
        const size_t usernameEnd = raw.find(L'\\', usernameStart);
        if (usernameEnd == std::wstring::npos) return input;

        const std::wstring usernameComponent = raw.substr(usernameStart, usernameEnd - usernameStart);
        const std::wstring suffix = L".config";
        if (usernameComponent.size() <= suffix.size()) return input;
        if (usernameComponent.rfind(suffix) != usernameComponent.size() - suffix.size()) return input;

        const std::wstring fixedUsername = usernameComponent.substr(0, usernameComponent.size() - suffix.size());
        if (fixedUsername.empty()) return input;

        std::wstring repaired = raw.substr(0, usernameStart);
        repaired += fixedUsername;
        repaired += L"\\.config";
        repaired += raw.substr(usernameEnd);
        return std::filesystem::path(repaired).lexically_normal();
    };

    auto loadPathOverrides = [&]() {
        if (s_pathOverridesLoaded) return;
        s_pathOverridesLoaded = true;
        s_libraryOverrideRoot.clear();
        s_instanceOverrideSavesRoot.clear();

        const auto cfgPath = getPathsConfigPath();
        std::error_code ec;
        if (!std::filesystem::exists(cfgPath, ec) || ec) return;

        try {
            std::ifstream in(cfgPath, std::ios::binary);
            if (!in.is_open()) return;
            nlohmann::json j;
            in >> j;
            if (j.contains("mapsRoot") && j["mapsRoot"].is_string()) {
                const auto p = Utf8ToWide(j["mapsRoot"].get<std::string>());
                if (!p.empty()) s_libraryOverrideRoot = repairLegacyDotConfigPath(std::filesystem::path(p));
            }
            if (j.contains("instanceSavesRoot") && j["instanceSavesRoot"].is_string()) {
                const auto p = Utf8ToWide(j["instanceSavesRoot"].get<std::string>());
                if (!p.empty()) s_instanceOverrideSavesRoot = repairLegacyDotConfigPath(std::filesystem::path(p));
            }
        } catch (...) {
        }
    };

    auto savePathOverrides = [&]() {
        std::error_code ec;
        const auto cfgPath = getPathsConfigPath();
        std::filesystem::create_directories(cfgPath.parent_path(), ec);
        if (ec) return;

        nlohmann::json j;
        if (!s_libraryOverrideRoot.empty()) j["mapsRoot"] = WideToUtf8(s_libraryOverrideRoot.wstring());
        if (!s_instanceOverrideSavesRoot.empty()) j["instanceSavesRoot"] = WideToUtf8(s_instanceOverrideSavesRoot.wstring());

        std::ofstream out(cfgPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return;
        out << j.dump(2);
    };

    auto getLibraryRoot = [&]() -> std::filesystem::path {
        if (!s_libraryOverrideRoot.empty()) return s_libraryOverrideRoot;
        return getPersistentToolscreenRoot() / L"practice_maps" / L"library";
    };

    auto normalizeInstanceSelectionToSavesDir = [&](const std::filesystem::path& selected) -> std::filesystem::path {
        if (selected.empty()) return {};
        const std::filesystem::path p = selected.lexically_normal();
        auto lower = [](const std::wstring& s) {
            std::wstring out = s;
            std::transform(out.begin(), out.end(), out.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            return out;
        };
        const std::wstring filenameLower = lower(p.filename().wstring());
        if (filenameLower == L"saves") return p;
        if (filenameLower == L".minecraft") return p / L"saves";

        std::error_code ec;
        if (std::filesystem::exists(p / L"options.txt", ec) && !ec) return p / L"saves";
        ec.clear();
        if (std::filesystem::exists(p / L"saves", ec) && !ec) return p / L"saves";
        ec.clear();
        if (std::filesystem::exists(p / L".minecraft" / L"options.txt", ec) && !ec) return p / L".minecraft" / L"saves";
        ec.clear();
        if (std::filesystem::exists(p / L".minecraft" / L"saves", ec) && !ec) return p / L".minecraft" / L"saves";

        // If uncertain, treat user selection as the saves root directly.
        return p;
    };

    auto discoverCatalogPath = [&]() -> std::filesystem::path {
        std::vector<std::filesystem::path> starts;
        wchar_t cwdBuf[MAX_PATH] = {};
        if (GetCurrentDirectoryW(MAX_PATH, cwdBuf) > 0) starts.push_back(std::filesystem::path(cwdBuf));
        if (!g_toolscreenPath.empty()) starts.push_back(std::filesystem::path(g_toolscreenPath));

        for (const auto& start : starts) {
            std::filesystem::path current = start;
            for (int depth = 0; depth <= 8 && !current.empty(); ++depth) {
                const auto candidate = current / L"maps.json";
                std::error_code ec;
                if (std::filesystem::exists(candidate, ec) && !ec && std::filesystem::is_regular_file(candidate, ec) && !ec) return candidate;
                const auto parent = current.parent_path();
                if (parent == current) break;
                current = parent;
            }
        }

        wchar_t userProfileBuf[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"USERPROFILE", userProfileBuf, MAX_PATH) > 0) {
            const auto fallback = std::filesystem::path(userProfileBuf) / L"Desktop" / L"msr" / L"maps.json";
            std::error_code ec;
            if (std::filesystem::exists(fallback, ec) && !ec && std::filesystem::is_regular_file(fallback, ec) && !ec) return fallback;
        }
        return {};
    };

    auto resolveInstanceSavesDir = [&](std::filesystem::path& outSavesPath, std::string& outErr) -> bool {
        outErr.clear();
        std::vector<std::filesystem::path> candidates;
        std::vector<std::wstring> seen;
        auto addUnique = [&](const std::filesystem::path& p) {
            if (p.empty()) return;
            std::filesystem::path n = p.lexically_normal();
            std::wstring key = n.wstring();
            std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            if (std::find(seen.begin(), seen.end(), key) != seen.end()) return;
            seen.push_back(key);
            candidates.push_back(n);
        };

        std::wstring optionsPath;
        std::wstring standardSettingsPath;
        if (TryResolveActiveMinecraftConfigPaths(optionsPath, standardSettingsPath) && !optionsPath.empty()) {
            addUnique(std::filesystem::path(optionsPath).parent_path() / L"saves");
        }

        wchar_t instMcDirBuf[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"INST_MC_DIR", instMcDirBuf, MAX_PATH) > 0) {
            addUnique(std::filesystem::path(instMcDirBuf) / L"saves");
        }
        wchar_t instDirBuf[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"INST_DIR", instDirBuf, MAX_PATH) > 0) {
            addUnique(std::filesystem::path(instDirBuf) / L".minecraft" / L"saves");
            addUnique(std::filesystem::path(instDirBuf) / L"saves");
        }

        wchar_t cwdBuf[MAX_PATH] = {};
        if (GetCurrentDirectoryW(MAX_PATH, cwdBuf) > 0) {
            const std::filesystem::path cwd(cwdBuf);
            addUnique(cwd / L"saves");
            addUnique(cwd / L".minecraft" / L"saves");
        }

        // Keep manual override available, but prefer active instance discovery first.
        if (!s_instanceOverrideSavesRoot.empty()) addUnique(normalizeInstanceSelectionToSavesDir(s_instanceOverrideSavesRoot));

        wchar_t userProfileBuf[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"USERPROFILE", userProfileBuf, MAX_PATH) > 0) {
            const std::filesystem::path userRoot(userProfileBuf);
            addUnique(userRoot / L"Desktop" / L"msr" / L"MultiMC" / L"instances" / L"MCSRRanked-Windows-1.16.1-All" / L".minecraft" / L"saves");
            addUnique(userRoot / L"AppData" / L"Roaming" / L".minecraft" / L"saves");
        }

        // Guaranteed final fallback.
        addUnique(getPersistentToolscreenRoot() / L"practice_maps" / L"instance_saves");

        std::error_code ec;
        for (const auto& cand : candidates) {
            ec.clear();
            std::filesystem::create_directories(cand, ec);
            if (!ec) {
                outSavesPath = cand;
                return true;
            }
        }

        outErr = "Could not resolve or create any instance saves path.";
        return false;
    };

    auto copyWorldDirectoryReplace = [&](const std::filesystem::path& src, const std::filesystem::path& dst, std::string& outErr) -> bool {
        outErr.clear();
        std::error_code ec;
        if (!worldFolderValid(src)) {
            outErr = "Source world invalid: " + WideToUtf8(src.wstring());
            return false;
        }

        std::filesystem::create_directories(dst.parent_path(), ec);
        if (ec) {
            outErr = "Failed to create destination parent: " + WideToUtf8(dst.parent_path().wstring());
            return false;
        }

        // Fast path: incremental mirror copy on Windows (usually much faster than
        // remove-all + full recursive copy for repeat launches).
        {
            std::wstringstream mirrorCmd;
            mirrorCmd.imbue(std::locale::classic());
            mirrorCmd << L"cmd.exe /C robocopy " << QuoteCommandArg(src.wstring()) << L" " << QuoteCommandArg(dst.wstring())
                      << L" /MIR /R:1 /W:1 /NFL /NDL /NJH /NJS /NC /NS /NP";
            std::string mirrorOut;
            int mirrorExit = -1;
            std::string mirrorErr;
            if (RunHiddenProcessCapture(mirrorCmd.str(), mirrorOut, mirrorExit, mirrorErr)) {
                if (mirrorExit >= 0 && mirrorExit <= 7) return true;
            }
        }

        if (std::filesystem::exists(dst, ec) && !ec) {
            std::filesystem::remove_all(dst, ec);
            if (ec) {
                // Handle stale junction/symlink and path edge-cases with command fallback.
                ec.clear();
                std::filesystem::remove(dst, ec);
                if (ec) {
                    std::wstringstream rmCmd;
                    rmCmd.imbue(std::locale::classic());
                    rmCmd << L"cmd.exe /C rmdir /S /Q " << QuoteCommandArg(dst.wstring());
                    std::string rmOut;
                    int rmExit = -1;
                    std::string rmErr;
                    if (!RunHiddenProcessCapture(rmCmd.str(), rmOut, rmExit, rmErr) || rmExit != 0) {
                        if (!rmErr.empty()) {
                            outErr = "Failed to clear destination world: " + WideToUtf8(dst.wstring()) + " | rmdir: " + rmErr;
                        } else if (!rmOut.empty()) {
                            outErr =
                                "Failed to clear destination world: " + WideToUtf8(dst.wstring()) + " | rmdir exit " + std::to_string(rmExit);
                        } else {
                            outErr = "Failed to clear destination world: " + WideToUtf8(dst.wstring());
                        }
                        return false;
                    }
                }
            }
        }
        std::filesystem::copy(src, dst, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
        if (!ec) return true;

        // Fallback for Windows path edge-cases where std::filesystem::copy can fail
        // on some map archives (deep paths / special entries).
        std::wstringstream cmd;
        cmd.imbue(std::locale::classic());
        cmd << L"cmd.exe /C robocopy " << QuoteCommandArg(src.wstring()) << L" " << QuoteCommandArg(dst.wstring())
            << L" /MIR /R:1 /W:1 /NFL /NDL /NJH /NJS /NC /NS /NP";
        std::string cmdOutput;
        int cmdExit = -1;
        std::string cmdErr;
        if (RunHiddenProcessCapture(cmd.str(), cmdOutput, cmdExit, cmdErr)) {
            // Robocopy success codes are 0..7; >=8 indicates failure.
            if (cmdExit >= 0 && cmdExit <= 7) return true;
            if (!cmdErr.empty()) {
                outErr = "Failed to copy world: " + ec.message() + " | robocopy: " + cmdErr;
            } else if (!cmdOutput.empty()) {
                outErr = "Failed to copy world: " + ec.message() + " | robocopy exit " + std::to_string(cmdExit);
            } else {
                outErr = "Failed to copy world: " + ec.message() + " | robocopy exit " + std::to_string(cmdExit);
            }
            return false;
        }
        outErr = "Failed to copy world: " + ec.message();
        return false;
    };

    auto copyInstanceIntoSaves = [&](const std::filesystem::path& instancePath, const std::filesystem::path& saveWorld, std::string& outErr) -> bool {
        outErr.clear();
        if (!worldFolderValid(instancePath)) {
            outErr = "Persistent instance missing level.dat: " + WideToUtf8(instancePath.wstring());
            return false;
        }
        return copyWorldDirectoryReplace(instancePath, saveWorld, outErr);
    };

    auto findCurrentProcessWindow = []() -> HWND {
        HWND foreground = GetForegroundWindow();
        if (foreground != NULL) {
            DWORD pid = 0;
            GetWindowThreadProcessId(foreground, &pid);
            if (pid == GetCurrentProcessId()) return foreground;
        }

        struct EnumContext {
            DWORD pid;
            HWND hwnd;
        } ctx{ GetCurrentProcessId(), NULL };

        EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
            auto* context = reinterpret_cast<EnumContext*>(lp);
            if (!context) return TRUE;
            if (!IsWindowVisible(hwnd)) return TRUE;
            if (GetWindow(hwnd, GW_OWNER) != NULL) return TRUE;

            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != context->pid) return TRUE;

            wchar_t className[64] = {};
            GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
            std::wstring classNameWs(className);
            if (classNameWs.find(L"GLFW") != std::wstring::npos || classNameWs.find(L"LWJGL") != std::wstring::npos) {
                context->hwnd = hwnd;
                return FALSE;
            }

            if (context->hwnd == NULL) context->hwnd = hwnd;
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));

        return ctx.hwnd;
    };

    auto focusGameWindow = [](HWND hwnd) -> bool {
        if (hwnd == NULL) return false;
        if (!IsWindow(hwnd)) return false;

        if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);

        const HWND fg = GetForegroundWindow();
        const DWORD thisThread = GetCurrentThreadId();
        const DWORD targetThread = GetWindowThreadProcessId(hwnd, NULL);
        const DWORD fgThread = (fg != NULL) ? GetWindowThreadProcessId(fg, NULL) : 0;

        if (fgThread != 0 && fgThread != thisThread) AttachThreadInput(thisThread, fgThread, TRUE);
        if (targetThread != 0 && targetThread != thisThread) AttachThreadInput(thisThread, targetThread, TRUE);

        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
        SetActiveWindow(hwnd);
        SetFocus(hwnd);

        if (targetThread != 0 && targetThread != thisThread) AttachThreadInput(thisThread, targetThread, FALSE);
        if (fgThread != 0 && fgThread != thisThread) AttachThreadInput(thisThread, fgThread, FALSE);
        return true;
    };

    auto sendVirtualKeyToWindow = [focusGameWindow](HWND hwnd, WORD vk) -> bool {
        if (hwnd == NULL) return false;
        if (!focusGameWindow(hwnd)) return false;

        INPUT inputs[2] = {};
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wVk = vk;
        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wVk = vk;
        inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
        const UINT sent = SendInput(2, inputs, sizeof(INPUT));
        if (sent == 2) return true;

        PostMessage(hwnd, WM_KEYDOWN, vk, 1);
        PostMessage(hwnd, WM_KEYUP, vk, (1 << 30) | (1 << 31));
        return true;
    };

    auto queueAutoLaunch = [&](const std::string& gameState) -> bool {
        if (gameState.find("inworld") != std::string::npos) return false;

        g_showGui.store(false);
        std::thread([findCurrentProcessWindow, sendVirtualKeyToWindow, focusGameWindow, gameState]() {
            auto sendClientClickRaw = [focusGameWindow](HWND hwnd, LONG x, LONG y, bool doubleClick) {
                if (hwnd == NULL) return;
                if (!focusGameWindow(hwnd)) return;

                POINT pt{ x, y };
                if (!ClientToScreen(hwnd, &pt)) return;

                const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
                const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
                const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
                const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
                if (vw <= 0 || vh <= 0) return;

                auto toAbsoluteCoord = [](int pos, int origin, int span) -> LONG {
                    const double normalized = (static_cast<double>(pos - origin) * 65535.0) / static_cast<double>(std::max(1, span - 1));
                    const long long rounded = static_cast<long long>(std::llround(normalized));
                    return static_cast<LONG>(std::clamp<long long>(rounded, 0LL, 65535LL));
                };

                const LONG absX = toAbsoluteCoord(pt.x, vx, vw);
                const LONG absY = toAbsoluteCoord(pt.y, vy, vh);

                auto clickOnce = [&]() {
                    SetCursorPos(pt.x, pt.y);

                    INPUT move{};
                    move.type = INPUT_MOUSE;
                    move.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
                    move.mi.dx = absX;
                    move.mi.dy = absY;
                    SendInput(1, &move, sizeof(INPUT));

                    INPUT down{};
                    down.type = INPUT_MOUSE;
                    down.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
                    SendInput(1, &down, sizeof(INPUT));

                    INPUT up{};
                    up.type = INPUT_MOUSE;
                    up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
                    SendInput(1, &up, sizeof(INPUT));

                    const LPARAM lp = MAKELPARAM(x, y);
                    PostMessage(hwnd, WM_MOUSEMOVE, 0, lp);
                    PostMessage(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp);
                    PostMessage(hwnd, WM_LBUTTONUP, 0, lp);
                };

                clickOnce();
                if (doubleClick) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(95));
                    clickOnce();
                }
            };
            auto sendModeClick = [sendClientClickRaw](HWND hwnd, const ModeViewportInfo& geo, LONG modeX, LONG modeY, bool doubleClick) {
                LONG clientX = modeX;
                LONG clientY = modeY;
                if (geo.valid && geo.width > 0 && geo.height > 0 && geo.stretchWidth > 0 && geo.stretchHeight > 0) {
                    const double nx = static_cast<double>(modeX) / static_cast<double>(geo.width);
                    const double ny = static_cast<double>(modeY) / static_cast<double>(geo.height);
                    clientX = static_cast<LONG>(std::lround(static_cast<double>(geo.stretchX) + nx * static_cast<double>(geo.stretchWidth)));
                    clientY = static_cast<LONG>(std::lround(static_cast<double>(geo.stretchY) + ny * static_cast<double>(geo.stretchHeight)));
                }
                sendClientClickRaw(hwnd, clientX, clientY, doubleClick);
            };

            auto inWorldOrGenerating = []() -> bool {
                const std::string nowState = g_gameStateBuffers[g_currentGameStateIndex.load(std::memory_order_acquire)];
                return nowState.find("inworld") != std::string::npos || nowState == "generating";
            };
            auto isMenuLikeState = [](const std::string& state) -> bool {
                return state.empty() || isWallTitleOrWaiting(state);
            };

            std::this_thread::sleep_for(std::chrono::milliseconds(130));

            HWND hwnd = findCurrentProcessWindow();
            if (hwnd == NULL) return;
            focusGameWindow(hwnd);
            Log("[Practice] Auto-launch queued from state: " + gameState);

            auto readGameState = []() -> std::string {
                return g_gameStateBuffers[g_currentGameStateIndex.load(std::memory_order_acquire)];
            };
            for (int attempt = 0; attempt < 16; ++attempt) {
                if (inWorldOrGenerating()) return;

                if (hwnd == NULL || !IsWindow(hwnd)) {
                    hwnd = findCurrentProcessWindow();
                    if (hwnd == NULL) {
                        Log("[Practice] Auto-launch could not reacquire game window.");
                        std::this_thread::sleep_for(std::chrono::milliseconds(120));
                        continue;
                    }
                }
                focusGameWindow(hwnd);

                RECT rc{};
                if (!GetClientRect(hwnd, &rc)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(120));
                    continue;
                }
                const LONG width = rc.right - rc.left;
                const LONG height = rc.bottom - rc.top;
                if (width <= 0 || height <= 0) {
                    Log("[Practice] Auto-launch got zero-size client rect; reacquiring window.");
                    hwnd = findCurrentProcessWindow();
                    std::this_thread::sleep_for(std::chrono::milliseconds(120));
                    continue;
                }

                const std::string nowState = readGameState();
                ModeViewportInfo geo = GetCurrentModeViewport();
                const LONG uiW = (geo.valid && geo.width > 0) ? static_cast<LONG>(geo.width) : width;
                const LONG uiH = (geo.valid && geo.height > 0) ? static_cast<LONG>(geo.height) : height;
                Log("[Practice] Auto-launch attempt " + std::to_string(attempt + 1) + " state=" + nowState + " wnd=" + std::to_string(width) +
                    "x" + std::to_string(height) + " ui=" + std::to_string(uiW) + "x" + std::to_string(uiH));
                const bool menuLike = isMenuLikeState(nowState);
                if (menuLike) {
                    // Enter Singleplayer from title/wall/waiting with a small
                    // vertical sweep so GUI-scale/layout differences still land.
                    const LONG singleX = uiW / 2;
                    const LONG singleYs[] = { uiH / 4 + 48, uiH / 4 + 58, uiH / 4 + 72 };
                    for (LONG y : singleYs) {
                        sendModeClick(hwnd, geo, singleX, y, false);
                        std::this_thread::sleep_for(std::chrono::milliseconds(90));
                        sendVirtualKeyToWindow(hwnd, VK_RETURN);
                        std::this_thread::sleep_for(std::chrono::milliseconds(110));
                        if (inWorldOrGenerating()) return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(160));
                    if (inWorldOrGenerating()) return;
                }

                // Try to open top world from the world list. This also acts as fallback
                // when state output reports "waiting"/unknown while the UI is menu-like.
                const LONG firstRowX = uiW / 2;
                const LONG playButtonX = uiW / 2 - 79;
                const LONG firstRowYs[] = { 46, 52, 68, 86 };
                for (LONG rowY : firstRowYs) {
                    sendModeClick(hwnd, geo, firstRowX, rowY, true);
                    std::this_thread::sleep_for(std::chrono::milliseconds(120));
                    sendVirtualKeyToWindow(hwnd, VK_RETURN);
                    std::this_thread::sleep_for(std::chrono::milliseconds(130));
                    if (inWorldOrGenerating()) return;
                }

                // "Play Selected World" is near the bottom-left of center row in
                // world list. Sweep Y to tolerate UI scale differences.
                const LONG playButtonYs[] = { uiH - 18, uiH - 24, uiH - 32, uiH - 42 };
                for (LONG playY : playButtonYs) {
                    sendModeClick(hwnd, geo, playButtonX, playY, false);
                    std::this_thread::sleep_for(std::chrono::milliseconds(120));
                    sendVirtualKeyToWindow(hwnd, VK_RETURN);
                    std::this_thread::sleep_for(std::chrono::milliseconds(180));
                    if (inWorldOrGenerating()) return;
                }

            }
            Log("[Practice] Auto-launch attempts exhausted without world-enter.");
        }).detach();
        return true;
    };

    auto pickFolderDialog = [](const wchar_t* title, std::filesystem::path& outPath) -> bool {
        BROWSEINFOW bi{};
        bi.lpszTitle = title;
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;
        PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
        if (pidl == NULL) return false;
        wchar_t folder[MAX_PATH] = {};
        const BOOL ok = SHGetPathFromIDListW(pidl, folder);
        CoTaskMemFree(pidl);
        if (!ok || folder[0] == L'\0') return false;
        outPath = std::filesystem::path(folder);
        return true;
    };

    auto addDefaultCatalogEntries = [&]() {
        s_catalog.clear();
        auto add = [&](const char* label, const char* creator, const char* url) {
            PracticeCatalogEntry entry;
            entry.label = label ? label : "";
            entry.creator = creator ? creator : "";
            entry.url = url ? url : "";
            const std::string baseName = entry.creator.empty() ? entry.label : (entry.label + " - " + entry.creator);
            entry.storageName = sanitizeWorldName(baseName);
            if (!entry.label.empty() && !entry.url.empty()) s_catalog.push_back(std::move(entry));
        };

        add("Bastion practice", "Llama", "https://github.com/LlamaPag/bastion/releases/download/3.14.0/LBP_3.14.0.zip");
        add("Zero Cycle", "Mescht", "https://github.com/Mescht/Zero-Practice/releases/download/v1.2.1/Zero.Practice.v1.2.1.zip");
        add("End Practice", "Ryguy2k4",
            "https://github.com/ryguy2k4/ryguy2k4endpractice/releases/download/v3.4.0/_Ryguy2k4_End_Practice_v3.4.0-1.16.1.zip");
        add("Blaze Practice", "Semperzz", "https://github.com/Semperzz/Blaze-Practice/releases/download/v1.3/Blaze.Practice.zip");
        add("End Portal Fill", "cylorun", "https://github.com/cylorun/End-Portal-Fill/releases/download/Minecraft/EndPortal.v2.zip");
        add("Portal Practice v2", "Semperzz", "https://github.com/Semperzz/Portal-Practice/releases/download/v2.8/Portal.Practice.v2.zip");
        add("Crafting v2", "Semperzz", "https://github.com/Semperzz/Crafting-Practice-v2/releases/download/v2.1/Crafting.Practice.v2.zip");
        add("Overworld practice", "7rowl", "https://github.com/7rowl/OWPractice/releases/download/v2.0/OW.Practice.v2.0.zip");
        add("Zero Sorting", "Semperzz", "https://github.com/Semperzz/Zero-Sorting-Practice/releases/download/v1.5/Zero.Sorting.zip");
        add("BT Practice", "Mescht", "https://github.com/Mescht/BTPractice/releases/download/v1.3/BTPractice-Map_v1.3.zip");
        add("Crafting practice & more", "romu", "https://github.com/romuuuuu/crafingworld/releases/download/d/_craftingworld.zip");
    };

    auto refreshCatalog = [&]() {
        s_catalog.clear();
        s_selectedCatalog = -1;
        s_catalogPathDisplay = "(not found)";

        const auto catalogPath = discoverCatalogPath();
        if (catalogPath.empty()) {
            addDefaultCatalogEntries();
            s_catalogPathDisplay = "(embedded default catalog)";
            return;
        }
        s_catalogPathDisplay = WideToUtf8(catalogPath.wstring());

        try {
            std::ifstream in(catalogPath, std::ios::binary);
            if (!in.is_open()) {
                addDefaultCatalogEntries();
                s_catalogPathDisplay = "(embedded default catalog)";
                return;
            }
            nlohmann::json j;
            in >> j;
            if (!j.is_array()) {
                addDefaultCatalogEntries();
                s_catalogPathDisplay = "(embedded default catalog)";
                return;
            }
            for (const auto& item : j) {
                if (!item.is_object()) continue;
                PracticeCatalogEntry entry;
                entry.label = item.value("label", "");
                entry.creator = item.value("creator", "");
                entry.url = item.value("url", "");
                if (entry.label.empty() || entry.url.empty()) continue;
                const std::string baseName = entry.creator.empty() ? entry.label : (entry.label + " - " + entry.creator);
                entry.storageName = sanitizeWorldName(baseName);
                s_catalog.push_back(std::move(entry));
            }
            if (s_catalog.empty()) {
                addDefaultCatalogEntries();
                s_catalogPathDisplay = "(embedded default catalog)";
            }
        } catch (...) {
            addDefaultCatalogEntries();
            s_catalogPathDisplay = "(embedded default catalog)";
            setStatus("Failed to parse maps.json catalog. Using embedded defaults.");
        }
    };

    auto refreshMaps = [&]() {
        const auto libRoot = getLibraryRoot();
        std::string previousSelectionName;
        if (s_selectedMap >= 0 && s_selectedMap < static_cast<int>(s_maps.size())) previousSelectionName = s_maps[s_selectedMap].mapName;
        s_maps.clear();
        s_selectedMap = -1;

        std::filesystem::path savesPath;
        std::string savesErr;
        const bool hasSavesPath = resolveInstanceSavesDir(savesPath, savesErr);
        (void)hasSavesPath;

        std::error_code ec;
        std::filesystem::create_directories(libRoot, ec);
        if (ec) return;

        std::vector<PracticeMapEntry> maps;
        for (const auto& entry : std::filesystem::directory_iterator(libRoot, ec)) {
            if (ec) break;
            if (!entry.is_directory(ec) || ec) continue;

            PracticeMapEntry map;
            map.mapName = WideToUtf8(entry.path().filename().wstring());
            map.rootPath = entry.path();
            map.instancePath = map.rootPath / L"instance";
            // New format: map/base (base world). Legacy format: map root itself is base world.
            const auto candidateBase = map.rootPath / L"base";
            map.basePath = worldFolderValid(candidateBase) ? candidateBase : map.rootPath;
            map.hasBase = worldFolderValid(map.basePath);
            map.hasInstance = worldFolderValid(map.instancePath);
            if (hasSavesPath) map.hasSaveInInstance = worldFolderValid(savesPath / Utf8ToWide(map.mapName));
            maps.push_back(std::move(map));
        }
        std::sort(maps.begin(), maps.end(), [](const PracticeMapEntry& a, const PracticeMapEntry& b) { return a.mapName < b.mapName; });
        s_maps = std::move(maps);

        if (!previousSelectionName.empty()) {
            for (int i = 0; i < static_cast<int>(s_maps.size()); ++i) {
                if (s_maps[i].mapName == previousSelectionName) {
                    s_selectedMap = i;
                    break;
                }
            }
        }

        // Clean up cached map covers for maps that no longer exist.
        for (auto it = s_mapCoverCache.begin(); it != s_mapCoverCache.end();) {
            bool stillPresent = false;
            for (const auto& map : s_maps) {
                if (getMapCacheKey(map.rootPath) == it->first) {
                    stillPresent = true;
                    break;
                }
            }
            if (!stillPresent) {
                releaseMapCoverEntry(it->second);
                it = s_mapCoverCache.erase(it);
            } else {
                ++it;
            }
        }
    };

    auto psQuoteSingle = [](const std::wstring& value) -> std::wstring {
        std::wstring out;
        out.reserve(value.size() + 2);
        out.push_back(L'\'');
        for (wchar_t ch : value) {
            if (ch == L'\'') out.append(L"''");
            else out.push_back(ch);
        }
        out.push_back(L'\'');
        return out;
    };

    auto beginInstallCatalogMap = [&](const PracticeCatalogEntry& entry) {
        const std::filesystem::path libRoot = getLibraryRoot();
        std::error_code ec;
        std::filesystem::create_directories(libRoot, ec);
        if (ec) {
            setStatus("Failed to create practice library directory.");
            return;
        }

        std::wstringstream script;
        script.imbue(std::locale::classic());
        script << L"$ErrorActionPreference='Stop';";
        script << L"$url=" << psQuoteSingle(Utf8ToWide(entry.url)) << L";";
        script << L"$name=" << psQuoteSingle(Utf8ToWide(entry.storageName)) << L";";
        script << L"$lib=" << psQuoteSingle(libRoot.wstring()) << L";";
        script << L"New-Item -ItemType Directory -Path $lib -Force | Out-Null;";
        script << L"$tmpZip=Join-Path $env:TEMP ('toolscreen_map_' + [Guid]::NewGuid().ToString('N') + '.zip');";
        script << L"$tmpDir=Join-Path $env:TEMP ('toolscreen_map_' + [Guid]::NewGuid().ToString('N'));";
        script << L"Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $tmpZip -TimeoutSec 180;";
        script << L"Expand-Archive -LiteralPath $tmpZip -DestinationPath $tmpDir -Force;";
        script << L"$world=Get-ChildItem -LiteralPath $tmpDir -Directory -Recurse | "
                  L"Where-Object { Test-Path (Join-Path $_.FullName 'level.dat') } | Select-Object -First 1;";
        script << L"if(-not $world){ throw 'No world folder with level.dat found in downloaded map archive.' };";
        script << L"$root=Join-Path $lib $name;";
        script << L"$base=Join-Path $root 'base';";
        script << L"if(Test-Path -LiteralPath $root){ Remove-Item -LiteralPath $root -Recurse -Force };";
        script << L"New-Item -ItemType Directory -Path $root -Force | Out-Null;";
        script << L"Copy-Item -LiteralPath $world.FullName -Destination $base -Recurse -Force;";
        script << L"Remove-Item -LiteralPath $tmpZip -Force -ErrorAction SilentlyContinue;";
        script << L"Remove-Item -LiteralPath $tmpDir -Recurse -Force -ErrorAction SilentlyContinue;";
        script << L"Write-Output ('installed:' + $root);";

        std::wstringstream cmd;
        cmd.imbue(std::locale::classic());
        cmd << L"powershell.exe -NoLogo -NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -Command "
            << QuoteCommandArg(script.str());

        s_installRunning = true;
        s_hasInstallResult = false;
        s_installFuture = std::async(std::launch::async, [command = cmd.str()]() {
            BoatSetupScriptRunResult result;
            int exitCode = -1;
            std::string procErr;
            if (!RunHiddenProcessCapture(command, result.output, exitCode, procErr)) {
                result.launched = false;
                result.exitCode = exitCode;
                result.error = procErr.empty() ? "Failed to start map install process." : procErr;
                return result;
            }
            result.launched = true;
            result.exitCode = exitCode;
            if (exitCode != 0) {
                result.error = result.output.empty() ? ("Map install failed with exit code " + std::to_string(exitCode)) : result.output;
            }
            return result;
        });
        setStatus("Installing map: " + entry.label, 8.0f);
    };

    auto runStartForSelected = [&]() {
        if (s_selectedMap < 0 || s_selectedMap >= static_cast<int>(s_maps.size())) return;
        const std::string gameStateNow = g_gameStateBuffers[g_currentGameStateIndex.load(std::memory_order_acquire)];
        if (gameStateNow.find("inworld") != std::string::npos) {
            setStatus("Finish or leave the current world before starting a practice map.");
            return;
        }
        auto map = s_maps[s_selectedMap];
        std::filesystem::path savesPath;
        std::string err;
        if (!resolveInstanceSavesDir(savesPath, err)) {
            setStatus(err);
            return;
        }

        const auto saveWorld = savesPath / Utf8ToWide(map.mapName);
        const bool hasSaveWorld = worldFolderValid(saveWorld);
        const bool hasInstanceWorld = worldFolderValid(map.instancePath);
        const bool hasBaseWorld = worldFolderValid(map.basePath);
        if (!hasInstanceWorld && !hasBaseWorld && !hasSaveWorld) {
            setStatus("Map has no valid base or instance world.");
            return;
        }

        bool needsCopyToSave = true;

        // Resume precedence:
        // 1) Existing persistent instance
        // 2) Existing save world promoted to instance
        // 3) Base world cloned to instance
        if (hasInstanceWorld) {
            if (hasSaveWorld) {
                // If current save is newer than persistent instance, keep progress by promoting save -> instance.
                std::error_code ec1, ec2;
                const auto saveTime = std::filesystem::last_write_time(saveWorld / L"level.dat", ec1);
                const auto instTime = std::filesystem::last_write_time(map.instancePath / L"level.dat", ec2);
                if (!ec1 && !ec2 && saveTime > instTime) {
                    if (!copyWorldDirectoryReplace(saveWorld, map.instancePath, err)) {
                        setStatus("Failed to sync save to persistent instance: " + err);
                        return;
                    }
                    // Save was authoritative and was promoted to instance; no need to
                    // immediately mirror instance back to save.
                    needsCopyToSave = false;
                } else if (!ec1 && !ec2 && saveTime == instTime) {
                    // Already in sync by timestamp.
                    needsCopyToSave = false;
                }
            }
        } else if (hasSaveWorld) {
            if (!copyWorldDirectoryReplace(saveWorld, map.instancePath, err)) {
                setStatus("Failed to promote save to persistent instance: " + err);
                return;
            }
            needsCopyToSave = false;
        } else {
            if (!copyWorldDirectoryReplace(map.basePath, map.instancePath, err)) {
                setStatus("Failed to create persistent instance from base: " + err);
                return;
            }
        }

        if (needsCopyToSave) {
            if (!copyInstanceIntoSaves(map.instancePath, saveWorld, err)) {
                setStatus("Failed to load map into instance saves: " + err);
                return;
            }
        }
        Log("[Practice] Start map='" + map.mapName + "' saves='" + WideToUtf8(savesPath.wstring()) + "'");

        // Hide GUI and immediately return focus/cursor control to Minecraft so
        // practice start does not leave an unfocused/free cursor state.
        g_showGui.store(false, std::memory_order_release);
        {
            HWND hwnd = g_minecraftHwnd.load(std::memory_order_relaxed);
            if (hwnd == NULL) hwnd = findCurrentProcessWindow();
            if (hwnd != NULL) {
                focusGameWindow(hwnd);
                RECT fullScreenRect;
                fullScreenRect.left = 0;
                fullScreenRect.top = 0;
                fullScreenRect.right = GetCachedScreenWidth();
                fullScreenRect.bottom = GetCachedScreenHeight();
                ClipCursor(&fullScreenRect);
                SetCursor(NULL);
            }
        }

        std::string queueErr;
        const bool queuedDirectLaunch = QueuePracticeWorldLaunchRequest(map.mapName, &queueErr);
        bool postedLaunchMessage = false;
        if (queuedDirectLaunch) {
            HWND hwnd = g_minecraftHwnd.load();
            if (hwnd != NULL) { postedLaunchMessage = (PostMessage(hwnd, GetPracticeWorldLaunchMessageId(), 0, 0) != FALSE); }
        }
        // Always keep a world-enter recapture armed after a practice start so
        // cursor/focus is restored even if immediate capture was interrupted.
        g_captureCursorOnWorldEnter.store(true, std::memory_order_release);

        if (queuedDirectLaunch && postedLaunchMessage) {
            setStatus("Started map '" + map.mapName + "' and requested direct world load.");
        } else if (queuedDirectLaunch) {
            setStatus("Started map '" + map.mapName + "'. Direct load queued; if it doesn't open, re-open title once.");
        } else {
            setStatus("Started map '" + map.mapName + "'. Direct load queue failed: " + queueErr);
        }
        refreshMaps();
    };

    auto runResetForSelected = [&]() {
        if (s_selectedMap < 0 || s_selectedMap >= static_cast<int>(s_maps.size())) return;
        auto map = s_maps[s_selectedMap];
        if (!worldFolderValid(map.basePath)) {
            setStatus("Reset failed: base world is missing.");
            return;
        }
        std::filesystem::path savesPath;
        std::string err;
        if (!resolveInstanceSavesDir(savesPath, err)) {
            setStatus(err);
            return;
        }
        const auto saveWorld = savesPath / Utf8ToWide(map.mapName);
        if (!copyWorldDirectoryReplace(map.basePath, map.instancePath, err)) {
            setStatus("Reset failed (base -> instance): " + err);
            return;
        }
        if (!copyInstanceIntoSaves(map.instancePath, saveWorld, err)) {
            setStatus("Reset failed (instance -> saves): " + err);
            return;
        }
        setStatus("Reset map '" + map.mapName + "' to base state.");
        refreshMaps();
    };

    auto runRemoveForSelected = [&]() {
        if (s_selectedMap < 0 || s_selectedMap >= static_cast<int>(s_maps.size())) return;
        const auto map = s_maps[s_selectedMap];
        std::error_code ec;
        std::filesystem::remove_all(map.rootPath, ec);
        if (ec) {
            setStatus("Remove failed (library): " + ec.message());
            return;
        }
        std::filesystem::path savesPath;
        std::string err;
        if (resolveInstanceSavesDir(savesPath, err)) {
            std::filesystem::remove_all(savesPath / Utf8ToWide(map.mapName), ec);
        }
        setStatus("Removed map '" + map.mapName + "' (base + instance + save).");
        refreshMaps();
    };

    if (s_installRunning && s_installFuture.valid()) {
        const auto ready = s_installFuture.wait_for(std::chrono::milliseconds(0));
        if (ready == std::future_status::ready) {
            s_lastInstallResult = s_installFuture.get();
            s_hasInstallResult = true;
            s_installRunning = false;
            if (s_lastInstallResult.exitCode == 0) {
                setStatus("Map installed to practice library.");
                refreshMaps();
            } else {
                setStatus("Map install failed. Check details below.");
            }
        }
    }

    if (!s_initialized) {
        loadPathOverrides();
        refreshCatalog();
        refreshMaps();
        s_initialized = true;
    }

    if (!s_status.empty() && std::chrono::steady_clock::now() <= s_statusUntil) {
        ImGui::TextWrapped("%s", s_status.c_str());
    }

    ImGui::SeparatorText("Install Maps");
    if (ImGui::Button("[Refresh]")) {
        refreshCatalog();
        refreshMaps();
    }
    ImGui::SameLine();
    if (ImGui::Button("[Open] Library Folder")) {
        const auto libRoot = getLibraryRoot();
        ShellExecuteW(NULL, L"open", libRoot.wstring().c_str(), NULL, NULL, SW_SHOWNORMAL);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(s_selectedCatalog < 0 || s_installRunning);
    if (ImGui::Button("[Install]")) beginInstallCatalogMap(s_catalog[s_selectedCatalog]);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("[Maps]")) {
        std::filesystem::path chosen;
        if (pickFolderDialog(L"Select maps directory", chosen)) {
            s_libraryOverrideRoot = chosen;
            savePathOverrides();
            refreshMaps();
            setStatus("Maps directory updated.");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("[Instances]")) {
        std::filesystem::path chosen;
        if (pickFolderDialog(L"Select instance saves directory", chosen)) {
            s_instanceOverrideSavesRoot = normalizeInstanceSelectionToSavesDir(chosen);
            savePathOverrides();
            refreshMaps();
            setStatus("Instances directory updated.");
        }
    }

    ImGui::BeginChild("PracticeCatalogList", ImVec2(0, 150), true);
    if (s_catalog.empty()) {
        ImGui::TextDisabled("No catalog entries found.");
    } else {
        for (int i = 0; i < static_cast<int>(s_catalog.size()); ++i) {
            const auto& e = s_catalog[i];
            const bool selected = (s_selectedCatalog == i);
            const std::string line = e.label + (e.creator.empty() ? "" : (" - " + e.creator));
            if (ImGui::Selectable(line.c_str(), selected)) s_selectedCatalog = i;
        }
    }
    ImGui::EndChild();

    if (s_installRunning) {
        ImGui::TextDisabled("Installing...");
    }
    if (s_hasInstallResult && s_lastInstallResult.exitCode != 0) {
        ImGui::TextWrapped("%s", s_lastInstallResult.error.empty() ? s_lastInstallResult.output.c_str() : s_lastInstallResult.error.c_str());
    }

    ImGui::SeparatorText("Installed Practice Maps");

    bool queuedDoubleStart = false;
    ImGui::BeginChild("PracticeMapList", ImVec2(0, 210), true);
    if (s_maps.empty()) {
        ImGui::TextDisabled("No installed practice maps.");
    } else {
        const float cardSize = 74.0f;
        const float cardSpacing = 8.0f;
        const float regionW = std::max(1.0f, ImGui::GetContentRegionAvail().x);
        const int columns = std::max(1, static_cast<int>((regionW + cardSpacing) / (cardSize + cardSpacing)));
        int col = 0;
        for (int i = 0; i < static_cast<int>(s_maps.size()); ++i) {
            const auto& map = s_maps[i];
            ImGui::PushID(i);
            const bool selected = (s_selectedMap == i);
            const GLuint coverTex = getOrLoadMapCoverTexture(map);
            bool clicked = false;
            if (coverTex != 0) {
                const ImVec4 bg = selected ? ImVec4(0.22f, 0.38f, 0.62f, 0.70f) : ImVec4(0.10f, 0.12f, 0.16f, 0.65f);
                ImTextureRef texRef((void*)(intptr_t)coverTex);
                clicked = ImGui::ImageButton("##MapCover", texRef, ImVec2(cardSize, cardSize), ImVec2(0, 0), ImVec2(1, 1), bg,
                                             ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
            } else {
                clicked = ImGui::Button("##MapCoverFallback", ImVec2(cardSize, cardSize));
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 mn = ImGui::GetItemRectMin();
                const ImVec2 mx = ImGui::GetItemRectMax();
                dl->AddRectFilled(mn, mx, IM_COL32(34, 41, 56, 180), 4.0f);
                dl->AddRect(mn, mx, selected ? IM_COL32(92, 148, 218, 220) : IM_COL32(70, 86, 110, 160), 4.0f, 0,
                            selected ? 2.2f : 1.0f);
                dl->AddText(ImVec2(mn.x + 8.0f, mn.y + 8.0f), IM_COL32(220, 230, 245, 220), "MAP");
            }

            if (clicked) s_selectedMap = i;
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                s_selectedMap = i;
                queuedDoubleStart = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(map.mapName.c_str());
                ImGui::EndTooltip();
            }
            if (selected) {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 mn = ImGui::GetItemRectMin();
                const ImVec2 mx = ImGui::GetItemRectMax();
                dl->AddRect(mn, mx, IM_COL32(120, 176, 246, 255), 4.0f, 0, 2.0f);
            }
            ImGui::PopID();

            ++col;
            if (col < columns && i + 1 < static_cast<int>(s_maps.size())) {
                ImGui::SameLine(0.0f, cardSpacing);
            } else {
                col = 0;
            }
        }
    }
    ImGui::EndChild();

    const std::string gameStateNowForUi = g_gameStateBuffers[g_currentGameStateIndex.load(std::memory_order_acquire)];
    const bool isInWorldForUi = (gameStateNowForUi.find("inworld") != std::string::npos);
    ImGui::BeginDisabled(s_selectedMap < 0 || isInWorldForUi);
    if (ImGui::Button("[Start]")) runStartForSelected();
    ImGui::SameLine();
    if (ImGui::Button("[Reset]")) runResetForSelected();
    ImGui::SameLine();
    if (ImGui::Button("[Remove] Map")) runRemoveForSelected();
    ImGui::EndDisabled();
    if (isInWorldForUi) {
        ImGui::SameLine();
        ImGui::TextDisabled("(Exit current world to use practice start/reset/remove)");
    }

    if (queuedDoubleStart) runStartForSelected();

    ImGui::EndTabItem();
}
