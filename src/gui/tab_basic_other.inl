if (ImGui::BeginTabItem("[O] Other")) {
    g_currentlyEditingMirror = "";
    g_imageDragMode.store(false);
    g_windowOverlayDragMode.store(false);

    // --- GUI HOTKEY SECTION ---
    ImGui::SeparatorText("[G] GUI");
    ImGui::PushID("basic_gui_hotkey");
    std::string guiKeyStr = GetKeyComboString(g_config.guiHotkey);

    ImGui::Text("Toggle:");
    ImGui::SameLine();

    bool isBindingGui = (s_mainHotkeyToBind == -999);
    const char* guiButtonLabel = isBindingGui ? "[Press Keys...]" : (guiKeyStr.empty() ? "[Click to Bind]" : guiKeyStr.c_str());
    if (ImGui::Button(guiButtonLabel, ImVec2(150, 0))) {
        s_mainHotkeyToBind = -999; // Special ID for GUI hotkey
        s_altHotkeyToBind = { -1, -1 };
        s_exclusionToBind = { -1, -1 };
    }
    ImGui::PopID();

    // --- DISPLAY SETTINGS ---
    ImGui::SeparatorText("[D] Display");

    ImGui::Text("FPS Limit:");
    ImGui::SetNextItemWidth(300);
    int fpsLimitValue = (g_config.fpsLimit == 0) ? 1001 : g_config.fpsLimit;
    if (ImGui::SliderInt("##FpsLimit", &fpsLimitValue, 30, 1001, fpsLimitValue == 1001 ? "Unlimited" : "%d fps")) {
        g_config.fpsLimit = (fpsLimitValue == 1001) ? 0 : fpsLimitValue;
        g_configIsDirty = true;
    }
    ImGui::SameLine();
    HelpMarker("Limits the game's maximum frame rate.\n"
               "Lower FPS can reduce GPU load and power consumption.");

    if (ImGui::Checkbox("Hide In-Game Animations", &g_config.hideAnimationsInGame)) { g_configIsDirty = true; }
    ImGui::SameLine();
    HelpMarker("When enabled, mode transitions appear instant on your screen,\n"
               "but OBS Game Capture will show the animations.");

/*    if (ImGui::Checkbox("Disable Fullscreen Prompt", &g_config.disableFullscreenPrompt)) { g_configIsDirty = true; }
    ImGui::SameLine();
    HelpMarker("Disables the fullscreen toast prompt (toast2).\n"
               "When disabled, toast2 appears in fullscreen and fades out after 3 seconds.");

    if (ImGui::Checkbox("Disable Configure Prompt", &g_config.disableConfigurePrompt)) { g_configIsDirty = true; }
    ImGui::SameLine();
    HelpMarker("Disables the configure toast prompt (toast1) shown in windowed mode.");*/

    // --- FONT SETTINGS ---
    ImGui::SeparatorText("[F] Font");

    ImGui::Text("Path:");
    ImGui::SetNextItemWidth(300);
    if (ImGui::InputText("##FontPath", &g_config.fontPath)) { g_configIsDirty = true; }
    ImGui::SameLine();
    HelpMarker("Path to a .ttf font file for the GUI. Restart required for changes to take effect.");

    // --- LOG TOOLS ---
    ImGui::SeparatorText("[L] Logs");
    static std::string s_logFeedbackText;
    static std::chrono::steady_clock::time_point s_logFeedbackUntil = std::chrono::steady_clock::time_point::min();

    auto setLogFeedback = [&](const std::string& text) {
        s_logFeedbackText = text;
        s_logFeedbackUntil = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    };

    auto resolveLatestLogPath = [&]() -> std::filesystem::path {
        std::vector<std::filesystem::path> candidates;
        auto addCandidate = [&](const std::filesystem::path& p) {
            if (p.empty()) return;
            for (const auto& existing : candidates) {
                if (existing.lexically_normal() == p.lexically_normal()) return;
            }
            candidates.push_back(p.lexically_normal());
        };

        if (!g_toolscreenPath.empty()) {
            addCandidate(std::filesystem::path(g_toolscreenPath) / L"logs" / L"latest.log");
            addCandidate(std::filesystem::path(g_toolscreenPath) / L"latest.log");
        }

        wchar_t cwdBuf[MAX_PATH] = {};
        if (GetCurrentDirectoryW(MAX_PATH, cwdBuf) > 0) {
            const std::filesystem::path cwd(cwdBuf);
            addCandidate(cwd / L"logs" / L"latest.log");
            addCandidate(cwd / L"Toolscreen" / L"logs" / L"latest.log");
            addCandidate(cwd / L"toolscreen" / L"logs" / L"latest.log");
            addCandidate(cwd.parent_path() / L"Toolscreen" / L"logs" / L"latest.log");
            addCandidate(cwd.parent_path() / L"toolscreen" / L"logs" / L"latest.log");
        }

        wchar_t instDirBuf[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"INST_DIR", instDirBuf, MAX_PATH) > 0) {
            const std::filesystem::path instDir(instDirBuf);
            addCandidate(instDir / L"Toolscreen" / L"logs" / L"latest.log");
            addCandidate(instDir / L"toolscreen" / L"logs" / L"latest.log");
        }

        std::error_code ec;
        for (const auto& candidate : candidates) {
            if (std::filesystem::exists(candidate, ec) && !ec && std::filesystem::is_regular_file(candidate, ec) && !ec) { return candidate; }
            ec.clear();
        }
        return {};
    };

    auto readFileTail = [&](const std::filesystem::path& logPath, std::string& outText) -> bool {
        outText.clear();
        std::ifstream in(logPath, std::ios::binary | std::ios::ate);
        if (!in.is_open()) return false;

        const std::streamoff fileSize = in.tellg();
        if (fileSize < 0) return false;
        constexpr std::streamoff kMaxCopyBytes = 512 * 1024;
        const std::streamoff startPos = (fileSize > kMaxCopyBytes) ? (fileSize - kMaxCopyBytes) : 0;
        in.seekg(startPos, std::ios::beg);

        outText.resize(static_cast<size_t>(fileSize - startPos));
        if (!outText.empty()) {
            in.read(&outText[0], static_cast<std::streamsize>(outText.size()));
            const std::streamsize got = in.gcount();
            if (got < 0) return false;
            outText.resize(static_cast<size_t>(got));
        }
        if (!in.eof() && in.fail()) return false;

        if (startPos > 0) { outText = "[Truncated to last 512KB]\n" + outText; }
        return true;
    };

    if (ImGui::Button("[Copy] latest.log")) {
        FlushLogs();
        const std::filesystem::path logPath = resolveLatestLogPath();
        if (logPath.empty()) {
            setLogFeedback("latest.log not found.");
        } else {
            std::string logText;
            if (!readFileTail(logPath, logText)) {
                setLogFeedback("Failed to read latest.log.");
            } else {
                std::string payload;
                payload.reserve(logText.size() + 256);
                payload += "Toolscreen Log Export\n";
                payload += "Path: ";
                payload += WideToUtf8(logPath.wstring());
                payload += "\n\n";
                payload += logText;
                CopyToClipboard(g_minecraftHwnd.load(), payload);
                setLogFeedback("latest.log copied to clipboard.");
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("[Open] Logs Folder")) {
        const std::filesystem::path logPath = resolveLatestLogPath();
        if (logPath.empty()) {
            setLogFeedback("No logs folder found.");
        } else {
            const std::filesystem::path logsDir = logPath.parent_path();
            HINSTANCE shellResult = ShellExecuteW(NULL, L"open", logsDir.c_str(), NULL, NULL, SW_SHOWNORMAL);
            if ((INT_PTR)shellResult <= 32) {
                setLogFeedback("Failed to open logs folder.");
            } else {
                setLogFeedback("Opened logs folder.");
            }
        }
    }

    if (!s_logFeedbackText.empty() && std::chrono::steady_clock::now() < s_logFeedbackUntil) {
        ImGui::TextDisabled("%s", s_logFeedbackText.c_str());
    }

    ImGui::EndTabItem();
}
