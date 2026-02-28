if (ImGui::BeginTabItem("[G] General")) {
    g_currentlyEditingMirror = "";
    g_imageDragMode.store(false);
    g_windowOverlayDragMode.store(false);

    // Helper lambda to render inline hotkey binding for a mode
    auto RenderInlineHotkeyBinding = [&](const std::string& targetModeId, const char* label) {
        // Find existing hotkey for this mode (Fullscreen <-> targetMode)
        int hotkeyIdx = -1;
        for (size_t i = 0; i < g_config.hotkeys.size(); ++i) {
            if (EqualsIgnoreCase(g_config.hotkeys[i].mainMode, "Fullscreen") &&
                EqualsIgnoreCase(g_config.hotkeys[i].secondaryMode, targetModeId)) {
                hotkeyIdx = static_cast<int>(i);
                break;
            }
        }

        ImGui::SameLine();
        ImGui::Text("Hotkey:");
        ImGui::SameLine();

        if (hotkeyIdx != -1) {
            std::string keyStr = GetKeyComboString(g_config.hotkeys[hotkeyIdx].keys);
            bool isBinding = (s_mainHotkeyToBind == hotkeyIdx);
            const char* buttonLabel = isBinding ? "[Press Keys...]" : (keyStr.empty() ? "[Click to Bind]" : keyStr.c_str());

            ImGui::PushID(label);
            if (ImGui::Button(buttonLabel, ImVec2(120, 0))) {
                s_mainHotkeyToBind = hotkeyIdx;
                s_altHotkeyToBind = { -1, -1 };
                s_exclusionToBind = { -1, -1 };
            }
            ImGui::PopID();
        } else {
            ImGui::TextDisabled("[No hotkey]");
        }
    };

    // Helper to ensure a mode exists
    auto EnsureModeExists = [&](const std::string& modeId, int width, int height) {
        for (const auto& mode : g_config.modes) {
            if (EqualsIgnoreCase(mode.id, modeId)) return; // Already exists
        }
        // Create the mode
        ModeConfig newMode;
        newMode.id = modeId;
        newMode.width = width;
        newMode.height = height;
        newMode.background.selectedMode = "color";
        newMode.background.color = { 0.0f, 0.0f, 0.0f };
        g_config.modes.push_back(newMode);
        g_configIsDirty = true;
    };

    // Helper to ensure hotkey exists for a mode
    auto EnsureHotkeyForMode = [&](const std::string& targetModeId) {
        // Check if hotkey already exists
        for (const auto& hotkey : g_config.hotkeys) {
            if (EqualsIgnoreCase(hotkey.mainMode, "Fullscreen") && EqualsIgnoreCase(hotkey.secondaryMode, targetModeId)) {
                return; // Already exists
            }
        }
        // Create new hotkey
        HotkeyConfig newHotkey;
        newHotkey.keys = std::vector<DWORD>();
        newHotkey.mainMode = "Fullscreen";
        newHotkey.secondaryMode = targetModeId;
        newHotkey.debounce = 100;
        g_config.hotkeys.push_back(newHotkey);
        ResizeHotkeySecondaryModes(g_config.hotkeys.size());               // Sync runtime state
        SetHotkeySecondaryMode(g_config.hotkeys.size() - 1, targetModeId); // Init new entry
        std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
        RebuildHotkeyMainKeys_Internal();
        g_configIsDirty = true;
    };

    // Helper to remove mode and its hotkey
    auto RemoveModeAndHotkey = [&](const std::string& modeId) {
        // Remove the mode
        for (auto it = g_config.modes.begin(); it != g_config.modes.end(); ++it) {
            if (EqualsIgnoreCase(it->id, modeId)) {
                g_config.modes.erase(it);
                break;
            }
        }
        // Remove any hotkeys that reference this mode as secondary
        g_config.hotkeys.erase(std::remove_if(g_config.hotkeys.begin(), g_config.hotkeys.end(),
                                              [&](const HotkeyConfig& h) { return EqualsIgnoreCase(h.secondaryMode, modeId); }),
                               g_config.hotkeys.end());
        ResetAllHotkeySecondaryModes(); // Sync secondary mode state after hotkey removal
        std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
        RebuildHotkeyMainKeys_Internal();
        g_configIsDirty = true;

        // If currently on this mode, switch to Fullscreen
        if (EqualsIgnoreCase(g_currentModeId, modeId)) {
            std::lock_guard<std::mutex> pendingLock(g_pendingModeSwitchMutex);
            g_pendingModeSwitch.pending = true;
            g_pendingModeSwitch.modeId = "Fullscreen";
            g_pendingModeSwitch.source = "Basic mode disabled";
            g_pendingModeSwitch.forceInstant = true;
        }
    };

    // Helper to check if mode exists
    auto ModeExists = [&](const std::string& modeId) -> bool {
        for (const auto& mode : g_config.modes) {
            if (EqualsIgnoreCase(mode.id, modeId)) return true;
        }
        return false;
    };

    ImGui::SeparatorText("[M] Modes");

    // Helper to check if a mode has a hotkey bound (non-empty keys)
    auto HasHotkeyBound = [&](const std::string& modeId) -> bool {
        for (const auto& hotkey : g_config.hotkeys) {
            if (EqualsIgnoreCase(hotkey.mainMode, "Fullscreen") && EqualsIgnoreCase(hotkey.secondaryMode, modeId)) {
                return !hotkey.keys.empty();
            }
        }
        return false;
    };

    // Helper to render inline hotkey binding
    auto RenderModeHotkeyBinding = [&](const std::string& targetModeId, const char* label) {
        // Find hotkey for this mode (Fullscreen <-> targetMode)
        int hotkeyIdx = -1;
        for (size_t i = 0; i < g_config.hotkeys.size(); ++i) {
            if (EqualsIgnoreCase(g_config.hotkeys[i].mainMode, "Fullscreen") &&
                EqualsIgnoreCase(g_config.hotkeys[i].secondaryMode, targetModeId)) {
                hotkeyIdx = static_cast<int>(i);
                break;
            }
        }

        if (hotkeyIdx == -1) return; // Should never happen since EnsureHotkeyForMode is called first

        std::string keyStr = GetKeyComboString(g_config.hotkeys[hotkeyIdx].keys);
        bool isBinding = (s_mainHotkeyToBind == hotkeyIdx);
        const char* buttonLabel = isBinding ? "[Press Keys...]" : (keyStr.empty() ? "[Click to Bind]" : keyStr.c_str());

        ImGui::PushID(label);
        // Blue button styling
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(40, 60, 100, 180));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(60, 80, 120, 200));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(80, 100, 140, 220));
        float columnWidth = ImGui::GetContentRegionAvail().x;
        if (ImGui::Button(buttonLabel, ImVec2(columnWidth, 0))) {
            s_mainHotkeyToBind = hotkeyIdx;
            s_altHotkeyToBind = { -1, -1 };
            s_exclusionToBind = { -1, -1 };
        }
        ImGui::PopStyleColor(3);
        ImGui::PopID();
    };

    // Helper to get mode config for editing
    auto GetModeConfig = [&](const std::string& modeId) -> ModeConfig* {
        for (auto& mode : g_config.modes) {
            if (EqualsIgnoreCase(mode.id, modeId)) return &mode;
        }
        return nullptr;
    };

    // --- MODE TABLE LAYOUT ---
    // Helper lambda to render a mode row in the table
    auto RenderModeTableRow = [&](const std::string& modeId, const char* label, const char* hotkeyLabel, int defaultWidth,
                                  int defaultHeight, int maxWidth, int maxHeight, bool showZoomOverlay = false) {
        ModeConfig* modeConfig = GetModeConfig(modeId);

        // Ensure hotkey config exists for this mode
        EnsureHotkeyForMode(modeId);

        ImGui::TableNextRow();

        // Column 1: Mode name
        ImGui::TableNextColumn();
        ImGui::Text("%s", label);

        // Column 2: Width spinner
        ImGui::TableNextColumn();
        if (modeConfig) {
            ImGui::PushID((std::string(label) + "_width").c_str());
            if (Spinner("##w", &modeConfig->width, 10, 1, maxWidth, 64, 3)) { g_configIsDirty = true; }
            ImGui::PopID();
        }

        // Column 3: Height spinner
        ImGui::TableNextColumn();
        if (modeConfig) {
            ImGui::PushID((std::string(label) + "_height").c_str());
            if (Spinner("##h", &modeConfig->height, 10, 1, maxHeight, 64, 3)) { g_configIsDirty = true; }
            ImGui::PopID();
        }

        // Column 4: Hotkey binding
        ImGui::TableNextColumn();
        RenderModeHotkeyBinding(modeId, hotkeyLabel);

        // Column 5: Zoom Overlay Pixels (only for EyeZoom)
        ImGui::TableNextColumn();
        if (showZoomOverlay) {
            // Display as half of cloneWidth, set cloneWidth to double the value
            int zoomSize = g_config.eyezoom.cloneWidth / 2;
            int maxZoomSize = 30; // Cap at 30
            ImGui::PushID("eyezoom_zoom_overlay");
            if (Spinner("##zo", &zoomSize, 1, 1, maxZoomSize, 64, 3)) {
                // Set cloneWidth to double the zoom size
                g_config.eyezoom.cloneWidth = zoomSize * 2;
                g_configIsDirty = true;
            }
            ImGui::PopID();
        }
    };

    // Create mode table with headers
    if (ImGui::BeginTable("ModeTable", 5, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Width", ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupColumn("Height", ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupColumn("Hotkey", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("Zoom Overlay Pixels", ImGuiTableColumnFlags_WidthFixed, 200);

        // Custom centered headers
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        const char* headers[] = { "Mode", "Width", "Height", "Hotkey", "Zoom Overlay Pixels" };
        for (int i = 0; i < 5; i++) {
            ImGui::TableSetColumnIndex(i);
            float columnWidth = ImGui::GetColumnWidth();
            float textWidth = ImGui::CalcTextSize(headers[i]).x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (columnWidth - textWidth) * 0.5f);
            ImGui::TableHeader(headers[i]);
        }

        // Get monitor bounds for limits
        int monitorWidth = GetCachedScreenWidth();
        int monitorHeight = GetCachedScreenHeight();

        // Thin row (limited to monitor bounds)
        RenderModeTableRow("Thin", "Thin", "thin_hotkey", 400, monitorHeight, monitorWidth, monitorHeight, false);

        // Wide row (limited to monitor bounds)
        RenderModeTableRow("Wide", "Wide", "wide_hotkey", monitorWidth, 400, monitorWidth, monitorHeight, false);

        // EyeZoom row (special limits: width=monitor, height=16384, with Zoom Overlay Pixels)
        RenderModeTableRow("EyeZoom", "EyeZoom", "eyezoom_hotkey", 384, 16384, monitorWidth, 16384, true);

        ImGui::EndTable();
    }

    // --- SENSITIVITY SECTION ---
    ImGui::SeparatorText("[S] Sensitivity");

    // Global Mouse Sensitivity
    ImGui::Text("Global:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    if (ImGui::SliderFloat("##globalSensBasic", &g_config.mouseSensitivity, 0.1f, 3.0f, "%.2fx")) { g_configIsDirty = true; }

    // EyeZoom Sensitivity Override
    {
        ModeConfig* eyezoomMode = GetModeConfig("EyeZoom");
        if (eyezoomMode) {
            ImGui::Text("EyeZoom:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(200);
            if (ImGui::SliderFloat("##eyezoomSensBasic", &eyezoomMode->modeSensitivity, 0.01f, 3.0f, "%.2fx")) {
                if (eyezoomMode->modeSensitivity < 0.01f) eyezoomMode->modeSensitivity = 0.01f;
                eyezoomMode->sensitivityOverrideEnabled = true;
                g_configIsDirty = true;
            }
        }
    }

    ImGui::SeparatorText("[V] Visual FX");
    ImGui::Text("Startup:");
    ImGui::SameLine();
    if (ImGui::Checkbox("[Auto Apply]", &g_config.boatSetup.autoApplyVisualEffects)) { g_configIsDirty = true; }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%s", "Apply visual effects automatically once each game launch.");
    }

    int distortionPct = std::clamp(g_config.boatSetup.autoDistortionPercent, 0, 100);
    ImGui::Text("Distortion:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    if (ImGui::SliderInt("##distortionPctGeneral", &distortionPct, 0, 100, "%d")) {
        g_config.boatSetup.autoDistortionPercent = distortionPct;
        g_configIsDirty = true;
    }

    int fovPct = std::clamp(g_config.boatSetup.autoFovEffectPercent, 0, 100);
    ImGui::Text("FOV Effects:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    if (ImGui::SliderInt("##fovPctGeneral", &fovPct, 0, 100, "%d")) {
        g_config.boatSetup.autoFovEffectPercent = fovPct;
        g_configIsDirty = true;
    }

    static std::future<BoatSetupScriptRunResult> s_visualFxRunFuture;
    static bool s_visualFxRunActive = false;
    static bool s_visualFxHasRun = false;
    static BoatSetupScriptRunResult s_visualFxLastRun;

    if (s_visualFxRunActive && s_visualFxRunFuture.valid()) {
        const auto ready = s_visualFxRunFuture.wait_for(std::chrono::milliseconds(0));
        if (ready == std::future_status::ready) {
            s_visualFxLastRun = s_visualFxRunFuture.get();
            s_visualFxHasRun = true;
            s_visualFxRunActive = false;
        }
    }

    ImGui::BeginDisabled(s_visualFxRunActive);
    if (ImGui::Button("[Apply] Visual FX")) {
        BoatSetupConfig runCfg = g_config.boatSetup;
        std::wstring toolsPath = g_toolscreenPath;
        s_visualFxRunActive = true;
        s_visualFxRunFuture = std::async(std::launch::async, [runCfg, toolsPath]() { return RunVisualEffectsApplyScript(runCfg, toolsPath, true); });
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("Writes Visual FX to options.txt, standardsettings.json, and config/mcsr/extra-options.json.");

    if (s_visualFxRunActive) {
        ImGui::TextDisabled("Applying visual effects...");
    } else if (s_visualFxHasRun) {
        const bool runOk = s_visualFxLastRun.parsedOk && s_visualFxLastRun.payload.value("ok", false);
        ImGui::TextColored(runOk ? ImVec4(0.45f, 1.0f, 0.55f, 1.0f) : ImVec4(1.0f, 0.45f, 0.45f, 1.0f),
                           "%s", runOk ? "Visual effects applied." : "Visual effects apply failed.");
        if (!s_visualFxLastRun.error.empty()) {
            ImGui::TextWrapped("%s", s_visualFxLastRun.error.c_str());
        } else if (!runOk && s_visualFxLastRun.parsedOk && s_visualFxLastRun.payload.contains("apply") &&
                   s_visualFxLastRun.payload["apply"].is_object()) {
            const std::string msg = s_visualFxLastRun.payload["apply"].value("message", "");
            if (!msg.empty()) ImGui::TextWrapped("%s", msg.c_str());
        }
    }

    ImGui::Separator();
    ImGui::TextDisabled("Overlay and macro settings moved to [O] Stronghold, [N] Notes, and [K] Macros tabs.");

    // --- MIRRORS SECTION ---
    ImGui::SeparatorText("[R] Mirrors");

    // Helper lambda to render mirror assignments for a mode
    auto RenderMirrorAssignments = [&](const std::string& modeId, const char* label) {
        ModeConfig* modeConfig = GetModeConfig(modeId);
        if (!modeConfig) return;

        ImGui::PushID(label);
        if (ImGui::TreeNode(label)) {
            // --- Assigned Mirrors and Mirror Groups ---
            int item_idx_to_remove = -1;
            bool remove_is_group = false;

            // Show individual mirrors with prefix
            for (size_t k = 0; k < modeConfig->mirrorIds.size(); ++k) {
                ImGui::PushID(static_cast<int>(k));
                if (ImGui::Button("X", ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) {
                    item_idx_to_remove = static_cast<int>(k);
                    remove_is_group = false;
                }
                ImGui::SameLine();
                ImGui::TextUnformatted(modeConfig->mirrorIds[k].c_str());
                ImGui::PopID();
            }

            // Show mirror groups with prefix
            for (size_t k = 0; k < modeConfig->mirrorGroupIds.size(); ++k) {
                ImGui::PushID(static_cast<int>(k) + 10000);
                if (ImGui::Button("X", ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) {
                    item_idx_to_remove = static_cast<int>(k);
                    remove_is_group = true;
                }
                ImGui::SameLine();
                ImGui::Text("[Group] %s", modeConfig->mirrorGroupIds[k].c_str());
                ImGui::PopID();
            }

            // Handle removal
            if (item_idx_to_remove != -1) {
                if (remove_is_group) {
                    modeConfig->mirrorGroupIds.erase(modeConfig->mirrorGroupIds.begin() + item_idx_to_remove);
                } else {
                    modeConfig->mirrorIds.erase(modeConfig->mirrorIds.begin() + item_idx_to_remove);
                }
                g_configIsDirty = true;
            }

            // Combined dropdown for mirrors and groups
            if (ImGui::BeginCombo("##AddMirrorOrGroup", "[Add Mirror/Group]")) {
                // Individual mirrors
                for (const auto& mirrorConf : g_config.mirrors) {
                    if (std::find(modeConfig->mirrorIds.begin(), modeConfig->mirrorIds.end(), mirrorConf.name) ==
                        modeConfig->mirrorIds.end()) {
                        if (ImGui::Selectable(mirrorConf.name.c_str())) {
                            modeConfig->mirrorIds.push_back(mirrorConf.name);
                            g_configIsDirty = true;
                        }
                    }
                }
                // Separator if both exist
                if (!g_config.mirrors.empty() && !g_config.mirrorGroups.empty()) { ImGui::Separator(); }
                // Mirror groups with prefix
                for (const auto& groupConf : g_config.mirrorGroups) {
                    if (std::find(modeConfig->mirrorGroupIds.begin(), modeConfig->mirrorGroupIds.end(), groupConf.name) ==
                        modeConfig->mirrorGroupIds.end()) {
                        std::string displayName = "[Group] " + groupConf.name;
                        if (ImGui::Selectable(displayName.c_str())) {
                            modeConfig->mirrorGroupIds.push_back(groupConf.name);
                            g_configIsDirty = true;
                        }
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::TreePop();
        }
        ImGui::PopID();
    };

    // Render for each of the 4 main modes
    RenderMirrorAssignments("Fullscreen", "Fullscreen");
    RenderMirrorAssignments("Thin", "Thin");
    RenderMirrorAssignments("Wide", "Wide");
    RenderMirrorAssignments("EyeZoom", "EyeZoom");

    ImGui::EndTabItem();
}
