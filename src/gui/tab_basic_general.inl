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

    ImGui::Separator();
    ImGui::SeparatorText("[O] Overlays");

    // --- NATIVE STRONGHOLD ARROW OVERLAY ---
    {
        if (ImGui::Checkbox("[HUD] Stronghold", &g_config.strongholdOverlay.enabled)) { g_configIsDirty = true; }

        if (g_config.strongholdOverlay.enabled) {
            const bool mcsrRankedInstance = IsMcsrRankedInstanceDetected();
            auto applyMcsrSafePreset = [&]() {
                g_config.strongholdOverlay.nonMcsrFeaturesEnabled = false;
                g_config.strongholdOverlay.showDirectionArrow = false;
                g_config.strongholdOverlay.showEstimateValues = false;
                g_config.strongholdOverlay.showAlignmentText = false;
            };
            auto applyFullFeaturePreset = [&]() {
                g_config.strongholdOverlay.nonMcsrFeaturesEnabled = true;
                g_config.strongholdOverlay.showDirectionArrow = true;
                g_config.strongholdOverlay.showEstimateValues = true;
                g_config.strongholdOverlay.showAlignmentText = true;
            };
            auto HoverHelp = [](const char* desc) {
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) { ImGui::SetTooltip("%s", desc); }
            };

            bool startHidden = !g_config.strongholdOverlay.visible;
            if (ImGui::Checkbox("[H] Start", &startHidden)) {
                g_config.strongholdOverlay.visible = !startHidden;
                g_configIsDirty = true;
            }
            HoverHelp("Start hidden. Hotkey [H] shows/hides overlay.");
            if (ImGui::Checkbox("[Eye] AutoHide", &g_config.strongholdOverlay.autoHideOnEyeSpy)) { g_configIsDirty = true; }
            HoverHelp("Auto-hide overlay when Eye Spy achievement is detected.");
            const char* strongholdHudLayouts[] = { "Full", "Speedrun" };
            int hudLayoutModeRaw = std::clamp(g_config.strongholdOverlay.hudLayoutMode, 0, 2);
            if (hudLayoutModeRaw == 1) hudLayoutModeRaw = 2; // Compact merged into Speedrun
            int hudLayoutModeUi = (hudLayoutModeRaw == 0) ? 0 : 1;
            if (ImGui::Combo("[L] HUD", &hudLayoutModeUi, strongholdHudLayouts, IM_ARRAYSIZE(strongholdHudLayouts))) {
                g_config.strongholdOverlay.hudLayoutMode = (hudLayoutModeUi == 0) ? 0 : 2;
                g_configIsDirty = true;
            }
            HoverHelp("HUD layout mode.");
            if (ImGui::Checkbox("[N] Default", &g_config.strongholdOverlay.preferNetherCoords)) { g_configIsDirty = true; }
            HoverHelp("Default coordinate mode to Nether.");
            if (ImGui::Checkbox("[Lock] Auto1", &g_config.strongholdOverlay.autoLockOnFirstNether)) {
                g_configIsDirty = true;
            }
            HoverHelp("Auto-lock on first Nether entry.");
            if (ImGui::Checkbox("[C] ChunkCtr", &g_config.strongholdOverlay.useChunkCenterTarget)) { g_configIsDirty = true; }
            HoverHelp("Use chunk center for target conversion.");

            if (mcsrRankedInstance) {
                if (g_config.strongholdOverlay.nonMcsrFeaturesEnabled || g_config.strongholdOverlay.showDirectionArrow ||
                    g_config.strongholdOverlay.showEstimateValues || g_config.strongholdOverlay.showAlignmentText) {
                    applyMcsrSafePreset();
                    g_configIsDirty = true;
                }
                ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.42f, 1.0f), "MCSR enforced");
            } else {
                ImGui::SeparatorText("[P] Presets");
                if (ImGui::Button("MCSR Safe")) {
                    applyMcsrSafePreset();
                    g_configIsDirty = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Full Feature")) {
                    applyFullFeaturePreset();
                    g_configIsDirty = true;
                }

                ImGui::SeparatorText("[N] Non-MCSR");
                if (ImGui::Checkbox("[ON] Non-MCSR", &g_config.strongholdOverlay.nonMcsrFeaturesEnabled)) {
                    g_configIsDirty = true;
                }
                HoverHelp("Enable non-MCSR helper visuals/features.");
                ImGui::BeginDisabled(!g_config.strongholdOverlay.nonMcsrFeaturesEnabled);
                if (ImGui::Checkbox("[Cmp] Compass", &g_config.strongholdOverlay.showDirectionArrow)) { g_configIsDirty = true; }
                HoverHelp("Show large direction compass.");
                if (ImGui::Checkbox("[Est] Values", &g_config.strongholdOverlay.showEstimateValues)) { g_configIsDirty = true; }
                HoverHelp("Show estimated/offset values in HUD.");
                if (ImGui::Checkbox("[A%] Align", &g_config.strongholdOverlay.showAlignmentText)) { g_configIsDirty = true; }
                HoverHelp("Show alignment percentage text.");
                ImGui::EndDisabled();
                if (!g_config.strongholdOverlay.nonMcsrFeaturesEnabled) { ImGui::TextDisabled("[Safe]"); }
            }

            bool forcedStandaloneChanged = false;
            if (!g_config.strongholdOverlay.standaloneClipboardMode) {
                g_config.strongholdOverlay.standaloneClipboardMode = true;
                forcedStandaloneChanged = true;
            }
            if (g_config.strongholdOverlay.manageNinjabrainBotProcess || g_config.strongholdOverlay.autoStartNinjabrainBot ||
                g_config.strongholdOverlay.hideNinjabrainBotWindow) {
                g_config.strongholdOverlay.manageNinjabrainBotProcess = false;
                g_config.strongholdOverlay.autoStartNinjabrainBot = false;
                g_config.strongholdOverlay.hideNinjabrainBotWindow = false;
                forcedStandaloneChanged = true;
            }
            if (forcedStandaloneChanged) { g_configIsDirty = true; }

            ImGui::TextDisabled("[Standalone] F3+C");
            HoverHelp("Standalone parser mode using F3+C clipboard snapshots.");

            bool boatEyeModeEnabled = !g_config.strongholdOverlay.standaloneAllowNonBoatThrows;
            struct StrongholdModeGuiIcon {
                GLuint texture = 0;
                int width = 0;
                int height = 0;
                float u0 = 0.0f;
                float v0 = 0.0f;
                float u1 = 1.0f;
                float v1 = 1.0f;
                int cropWidth = 0;
                int cropHeight = 0;
                bool attempted = false;
            };
            static StrongholdModeGuiIcon s_boatIcon;
            static StrongholdModeGuiIcon s_eyeIcon;

            auto ensureStrongholdModeGuiIcon = [&](int resourceId, StrongholdModeGuiIcon& outIcon, bool pixelated, bool cropToAlpha) {
                if ((outIcon.texture != 0 && outIcon.width > 0 && outIcon.height > 0) || outIcon.attempted) return;
                outIcon.attempted = true;

                stbi_set_flip_vertically_on_load_thread(0);
                HMODULE hModule = NULL;
                GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCWSTR)&RegisterBindingInputEvent, &hModule);
                if (!hModule) return;

                HRSRC hResource = FindResourceW(hModule, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
                if (!hResource) return;
                HGLOBAL hData = LoadResource(hModule, hResource);
                if (!hData) return;

                const DWORD dataSize = SizeofResource(hModule, hResource);
                const unsigned char* rawData = reinterpret_cast<const unsigned char*>(LockResource(hData));
                if (!rawData || dataSize == 0) return;

                int w = 0;
                int h = 0;
                int channels = 0;
                unsigned char* pixels = stbi_load_from_memory(rawData, static_cast<int>(dataSize), &w, &h, &channels, 4);
                if (!pixels || w <= 0 || h <= 0) return;

                int minX = 0;
                int minY = 0;
                int maxX = w - 1;
                int maxY = h - 1;
                if (cropToAlpha) {
                    minX = w;
                    minY = h;
                    maxX = -1;
                    maxY = -1;
                    for (int py = 0; py < h; ++py) {
                        for (int px = 0; px < w; ++px) {
                            const unsigned char a = pixels[(py * w + px) * 4 + 3];
                            if (a <= 8) continue;
                            if (px < minX) minX = px;
                            if (py < minY) minY = py;
                            if (px > maxX) maxX = px;
                            if (py > maxY) maxY = py;
                        }
                    }
                    if (maxX < minX || maxY < minY) {
                        minX = 0;
                        minY = 0;
                        maxX = w - 1;
                        maxY = h - 1;
                    }
                }
                const int cropW = std::max(1, maxX - minX + 1);
                const int cropH = std::max(1, maxY - minY + 1);

                GLuint tex = 0;
                glGenTextures(1, &tex);
                if (tex != 0) {
                    glBindTexture(GL_TEXTURE_2D, tex);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, pixelated ? GL_NEAREST : GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, pixelated ? GL_NEAREST : GL_LINEAR);
                    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
                    glBindTexture(GL_TEXTURE_2D, 0);

                    outIcon.texture = tex;
                    outIcon.width = w;
                    outIcon.height = h;
                    outIcon.u0 = static_cast<float>(minX) / static_cast<float>(w);
                    outIcon.v0 = static_cast<float>(minY) / static_cast<float>(h);
                    outIcon.u1 = static_cast<float>(maxX + 1) / static_cast<float>(w);
                    outIcon.v1 = static_cast<float>(maxY + 1) / static_cast<float>(h);
                    outIcon.cropWidth = cropW;
                    outIcon.cropHeight = cropH;
                }
                stbi_image_free(pixels);
            };

            ensureStrongholdModeGuiIcon(IDR_STRONGHOLD_BOAT_PNG, s_boatIcon, false, true);
            ensureStrongholdModeGuiIcon(IDR_STRONGHOLD_EYE_PNG, s_eyeIcon, false, false);

            auto drawStrongholdModeIconButton = [&](const char* id, const StrongholdModeGuiIcon& icon, bool active,
                                                    const char* fallbackLabel, bool preferWideIcon) {
                const ImVec2 buttonSize = preferWideIcon ? ImVec2(138.0f, 78.0f) : ImVec2(102.0f, 78.0f);
                const bool clicked = ImGui::InvisibleButton(id, buttonSize);
                const bool hovered = ImGui::IsItemHovered();
                ImDrawList* drawList = ImGui::GetWindowDrawList();
                if (!drawList) return clicked;

                const ImVec2 minPt = ImGui::GetItemRectMin();
                const ImVec2 maxPt = ImGui::GetItemRectMax();
                ImU32 bg = active ? IM_COL32(46, 72, 98, 220) : IM_COL32(28, 38, 50, 190);
                if (hovered) bg = active ? IM_COL32(56, 86, 116, 230) : IM_COL32(36, 50, 66, 210);
                const ImU32 border = active ? IM_COL32(122, 180, 232, 255) : IM_COL32(78, 104, 126, 220);
                drawList->AddRectFilled(minPt, maxPt, bg, 7.0f);
                drawList->AddRect(minPt, maxPt, border, 7.0f, 0, active ? 1.8f : 1.2f);

                if (icon.texture != 0 && icon.width > 0 && icon.height > 0) {
                    const float pad = 6.0f;
                    const float availW = buttonSize.x - (2.0f * pad);
                    const float availH = buttonSize.y - (2.0f * pad);
                    const int contentW = (icon.cropWidth > 0) ? icon.cropWidth : icon.width;
                    const int contentH = (icon.cropHeight > 0) ? icon.cropHeight : icon.height;
                    const float scale = std::min(availW / static_cast<float>(contentW), availH / static_cast<float>(contentH));
                    const float drawW = std::max(1.0f, std::floor(static_cast<float>(contentW) * scale));
                    const float drawH = std::max(1.0f, std::floor(static_cast<float>(contentH) * scale));
                    const ImVec2 imgMin(minPt.x + (buttonSize.x - drawW) * 0.5f, minPt.y + (buttonSize.y - drawH) * 0.5f);
                    const ImVec2 imgMax(imgMin.x + drawW, imgMin.y + drawH);
                    const ImU32 tint = IM_COL32(255, 255, 255, 255);
                    drawList->AddImage((ImTextureID)(intptr_t)icon.texture, imgMin, imgMax, ImVec2(icon.u0, icon.v0),
                                       ImVec2(icon.u1, icon.v1), tint);
                } else {
                    const ImVec2 ts = ImGui::CalcTextSize(fallbackLabel);
                    drawList->AddText(ImVec2(minPt.x + (buttonSize.x - ts.x) * 0.5f, minPt.y + (buttonSize.y - ts.y) * 0.5f),
                                      IM_COL32(210, 220, 230, 255), fallbackLabel);
                }

                return clicked;
            };

            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("[Mode]");
            ImGui::SameLine();
            ImGui::PushID("StrongholdModeButtons");
            if (drawStrongholdModeIconButton("Boat", s_boatIcon, boatEyeModeEnabled, "Boat", true)) {
                boatEyeModeEnabled = true;
                g_config.strongholdOverlay.standaloneAllowNonBoatThrows = false;
                g_configIsDirty = true;
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("%s", "Boat mode: only boat-eye workflow captures are used.");
            }
            ImGui::SameLine();
            if (drawStrongholdModeIconButton("Eye", s_eyeIcon, !boatEyeModeEnabled, "Eye", false)) {
                boatEyeModeEnabled = false;
                g_config.strongholdOverlay.standaloneAllowNonBoatThrows = true;
                g_configIsDirty = true;
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("%s", "Eye mode: standard 1-2 eye workflow (non-boat throws).");
            }
            ImGui::PopID();
            ImGui::SameLine();
            ImGui::TextDisabled("%s", boatEyeModeEnabled ? "Boat workflow" : "Eye workflow");
            HoverHelp("Click boat or eye icon to toggle stronghold capture mode.");

            const char* strongholdRenderMonitorModes[] = { "All", "Select" };
            int strongholdRenderMonitorMode = g_config.strongholdOverlay.renderMonitorMode;
            if (strongholdRenderMonitorMode < 0 || strongholdRenderMonitorMode > 1) { strongholdRenderMonitorMode = 0; }
            if (ImGui::Combo("[D] Displays", &strongholdRenderMonitorMode, strongholdRenderMonitorModes,
                             IM_ARRAYSIZE(strongholdRenderMonitorModes))) {
                g_config.strongholdOverlay.renderMonitorMode = strongholdRenderMonitorMode;
                if (g_config.strongholdOverlay.renderMonitorMode == 1 && g_config.strongholdOverlay.renderMonitorMask == 0ull) {
                    g_config.strongholdOverlay.renderMonitorMask = 1ull;
                }
                g_configIsDirty = true;
            }
            HoverHelp("Choose output display routing for companion window.");
            if (g_config.strongholdOverlay.renderMonitorMode == 1) {
                int monitorCount = GetSystemMetrics(SM_CMONITORS);
                if (monitorCount < 1) monitorCount = 1;
                if (monitorCount > 63) monitorCount = 63;

                if (ImGui::Button("[A]##StrongholdMonitors")) {
                    g_config.strongholdOverlay.renderMonitorMask =
                        (monitorCount >= 63) ? (~0ull >> 1) : ((1ull << monitorCount) - 1ull);
                    g_configIsDirty = true;
                }
                HoverHelp("Select all displays.");
                ImGui::SameLine();
                if (ImGui::Button("[0]##StrongholdMonitors")) {
                    g_config.strongholdOverlay.renderMonitorMask = 0ull;
                    g_configIsDirty = true;
                }
                HoverHelp("Clear all display selections.");

                for (int monitorIndex = 0; monitorIndex < monitorCount; ++monitorIndex) {
                    const unsigned long long bit = (1ull << monitorIndex);
                    bool selected = (g_config.strongholdOverlay.renderMonitorMask & bit) != 0ull;
                    std::string label = "[D] " + std::to_string(monitorIndex + 1);
                    if (ImGui::Checkbox(label.c_str(), &selected)) {
                        if (selected) {
                            g_config.strongholdOverlay.renderMonitorMask |= bit;
                        } else {
                            g_config.strongholdOverlay.renderMonitorMask &= ~bit;
                        }
                        g_configIsDirty = true;
                    }
                    HoverHelp("Toggle this display.");
                }

                if (g_config.strongholdOverlay.renderMonitorMask == 0ull) { ImGui::TextDisabled("[!] No display"); }
            }
            if (ImGui::SliderFloat("[O]##StrongholdOverlayOpacityBasic", &g_config.strongholdOverlay.opacity, 0.1f, 1.0f, "%.2f")) {
                g_configIsDirty = true;
            }
            HoverHelp("Overlay text/foreground opacity.");
            if (ImGui::SliderFloat("[BG]##StrongholdBackgroundBasic", &g_config.strongholdOverlay.backgroundOpacity, 0.0f, 1.0f,
                                   "%.2f")) {
                g_configIsDirty = true;
            }
            HoverHelp("Overlay background opacity.");
            if (ImGui::SliderFloat("[S]##StrongholdScaleBasic", &g_config.strongholdOverlay.scale, 0.5f, 2.0f, "%.2fx")) {
                g_configIsDirty = true;
            }
            HoverHelp("Overall overlay scale.");

            ImGui::SeparatorText("[I] Status");
            const std::string gameState = g_gameStateBuffers[g_currentGameStateIndex.load(std::memory_order_acquire)];
            const bool isInWorldState = gameState.find("inworld") != std::string::npos;
            const bool macrosRuntimeEnabled = AreMacrosRuntimeEnabled();
            const bool macroBlockedByState = g_config.keyRebinds.globalOnlyInWorld && !isInWorldState;
            const bool macroGateBlocking = (!macrosRuntimeEnabled) || macroBlockedByState;

            auto extractDisplayNumber = [](const WCHAR* deviceName) -> int {
                if (!deviceName) return -1;
                const WCHAR* p = deviceName;
                while (*p && (*p < L'0' || *p > L'9')) ++p;
                if (!*p) return -1;
                int value = 0;
                while (*p >= L'0' && *p <= L'9') {
                    value = (value * 10) + static_cast<int>(*p - L'0');
                    ++p;
                }
                return (value >= 1 && value <= 63) ? value : -1;
            };

            int gameDisplayNumber = -1;
            HMONITOR gameMonitor = MonitorFromWindow(g_minecraftHwnd.load(), MONITOR_DEFAULTTOPRIMARY);
            if (gameMonitor) {
                MONITORINFOEXW mi{};
                mi.cbSize = sizeof(mi);
                if (GetMonitorInfoW(gameMonitor, reinterpret_cast<MONITORINFO*>(&mi))) {
                    gameDisplayNumber = extractDisplayNumber(mi.szDevice);
                }
            }
            if (gameDisplayNumber < 0) gameDisplayNumber = 1;

            std::string routingLabel;
            if (g_config.strongholdOverlay.renderMonitorMode == 1) {
                routingLabel = "selected mask=0x" + [&]() {
                    std::ostringstream ss;
                    ss << std::hex << g_config.strongholdOverlay.renderMonitorMask;
                    return ss.str();
                }();
            } else {
                routingLabel = "all displays";
            }

            ImGui::Text("[State] %s", gameState.empty() ? "unknown" : gameState.c_str());
            HoverHelp("Detected game/runtime state used for macro gating.");
            ImGui::Text("[Disp] game=%d | %s", gameDisplayNumber, routingLabel.c_str());
            HoverHelp("Current game display and overlay routing target.");
            if (mcsrRankedInstance) {
                ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.42f, 1.0f), "MCSR: ENFORCED");
                HoverHelp("MCSR safety mode is forced for this instance.");
            } else {
                ImGui::TextColored(ImVec4(0.55f, 0.9f, 0.55f, 1.0f), "MCSR: FULL");
                HoverHelp("Full-feature mode available.");
            }

            if (macroGateBlocking) {
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "[M] BLOCKED");
                HoverHelp("Macros blocked by global toggle or state gate.");
            } else {
                ImGui::TextColored(ImVec4(0.45f, 1.0f, 0.55f, 1.0f), "[M] ACTIVE");
                HoverHelp("Macros currently active.");
            }
        }

        ImGui::TextDisabled("[H] [S+H] [C+S+H] [N8/N2] [N4/N6] [N5]");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("%s", "H hide/show | Shift+H lock/unlock | Ctrl+Shift+H reset | Num8/2 adjust | Num4 undo | Num6 redo | Num5 clear");
        }
    }

    // --- F3 MACRO REBIND ---
    {
        ImGui::SeparatorText("[K] Macro / F3");
        auto MacroHoverHelp = [](const char* desc) {
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) { ImGui::SetTooltip("%s", desc); }
        };
        const std::string macroGameState = g_gameStateBuffers[g_currentGameStateIndex.load(std::memory_order_acquire)];
        const bool macroInWorld = macroGameState.find("inworld") != std::string::npos;
        const bool macroRuntimeEnabled = AreMacrosRuntimeEnabled();
        const bool macroBlockedByState = g_config.keyRebinds.globalOnlyInWorld && !macroInWorld;
        const bool macroActiveNow = macroRuntimeEnabled && !macroBlockedByState;
        ImGui::TextUnformatted("[M] Engine");
        MacroHoverHelp("Macro engine runtime status.");
        ImGui::SameLine();
        ImGui::TextColored(macroActiveNow ? ImVec4(0.45f, 1.0f, 0.55f, 1.0f) : ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "[%s]",
                           macroActiveNow ? "ACTIVE" : "BLOCKED");
        MacroHoverHelp("Macro engine ACTIVE/BLOCKED.");
        if (!macroRuntimeEnabled) {
            ImGui::TextDisabled("[M] off (C+S+M)");
            MacroHoverHelp("Global macro toggle hotkey.");
        } else if (macroBlockedByState) {
            ImGui::TextDisabled("[Gate] blocked");
            MacroHoverHelp("Blocked by in-game gate setting.");
        } else {
            ImGui::TextDisabled("[M] C+S+M");
            MacroHoverHelp("Global macro toggle hotkey.");
        }

        if (ImGui::Checkbox("[G] In-Game", &g_config.keyRebinds.globalOnlyInWorld)) { g_configIsDirty = true; }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) { ImGui::SetTooltip("%s", "Enable macros only while in-game states are active."); }
        ImGui::Separator();
        ImGui::TextUnformatted("[F3] Rebind");

        auto isF3TargetRebind = [](const KeyRebind& rebind) {
            return rebind.toKey == VK_F3 && !rebind.useCustomOutput;
        };

        int f3RebindIndex = -1;
        for (int i = 0; i < static_cast<int>(g_config.keyRebinds.rebinds.size()); ++i) {
            if (isF3TargetRebind(g_config.keyRebinds.rebinds[static_cast<size_t>(i)])) {
                f3RebindIndex = i;
                break;
            }
        }

        bool f3MacroEnabled = (f3RebindIndex >= 0);
        if (ImGui::Checkbox("[ON]", &f3MacroEnabled)) {
            if (f3MacroEnabled) {
                if (f3RebindIndex < 0) {
                    KeyRebind rebind{};
                    rebind.fromKey = VK_TAB;
                    rebind.toKey = VK_F3;
                    rebind.enabled = true;
                    rebind.onlyInWorld = true;
                    rebind.useCustomOutput = false;
                    rebind.customOutputVK = 0;
                    rebind.customOutputScanCode = 0;
                    g_config.keyRebinds.rebinds.push_back(rebind);
                } else {
                    g_config.keyRebinds.rebinds[static_cast<size_t>(f3RebindIndex)].enabled = true;
                    g_config.keyRebinds.rebinds[static_cast<size_t>(f3RebindIndex)].onlyInWorld = true;
                }
                g_config.keyRebinds.enabled = true;
            } else {
                g_config.keyRebinds.rebinds.erase(
                    std::remove_if(g_config.keyRebinds.rebinds.begin(), g_config.keyRebinds.rebinds.end(), isF3TargetRebind),
                    g_config.keyRebinds.rebinds.end());
                if (g_config.keyRebinds.rebinds.empty()) { g_config.keyRebinds.enabled = false; }
            }
            g_configIsDirty = true;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) { ImGui::SetTooltip("%s", "Enable F3 macro remap."); }

        // Re-resolve after a toggle so the combo always points at the active entry.
        f3RebindIndex = -1;
        for (int i = 0; i < static_cast<int>(g_config.keyRebinds.rebinds.size()); ++i) {
            if (isF3TargetRebind(g_config.keyRebinds.rebinds[static_cast<size_t>(i)])) {
                f3RebindIndex = i;
                break;
            }
        }

        if (f3RebindIndex >= 0) {
            static const std::vector<std::pair<std::string, DWORD>> macroKeys = []() {
                std::vector<std::pair<std::string, DWORD>> keys;
                keys.reserve(200);
                auto addKey = [&](DWORD vk) {
                    if (vk == 0) return;
                    for (const auto& it : keys) {
                        if (it.second == vk) return;
                    }
                    std::string label = VkToString(vk);
                    if (label.empty() || label == "[None]") return;
                    if (label.size() > 2 && label[0] == '0' && (label[1] == 'x' || label[1] == 'X')) return;
                    keys.emplace_back(label, vk);
                };

                // Prioritize common keys at the top, then append the rest.
                const DWORD preferred[] = { VK_TAB,    VK_CAPITAL, VK_ESCAPE, VK_SPACE,  VK_RETURN, VK_BACK,   VK_LSHIFT, VK_RSHIFT,
                                            VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU, VK_UP,    VK_DOWN,   VK_LEFT,   VK_RIGHT,
                                            VK_INSERT, VK_DELETE,  VK_HOME,   VK_END,    VK_PRIOR, VK_NEXT,   VK_SNAPSHOT, VK_SCROLL,
                                            VK_PAUSE,  VK_NUMLOCK };
                for (DWORD vk : preferred) { addKey(vk); }

                for (DWORD vk = VK_F1; vk <= VK_F24; ++vk) { addKey(vk); }
                for (DWORD vk = 'A'; vk <= 'Z'; ++vk) { addKey(vk); }
                for (DWORD vk = '0'; vk <= '9'; ++vk) { addKey(vk); }
                for (DWORD vk = VK_NUMPAD0; vk <= VK_NUMPAD9; ++vk) { addKey(vk); }
                addKey(VK_MULTIPLY);
                addKey(VK_ADD);
                addKey(VK_SUBTRACT);
                addKey(VK_DECIMAL);
                addKey(VK_DIVIDE);
                addKey(VK_SEPARATOR);

                addKey(VK_OEM_1);
                addKey(VK_OEM_PLUS);
                addKey(VK_OEM_COMMA);
                addKey(VK_OEM_MINUS);
                addKey(VK_OEM_PERIOD);
                addKey(VK_OEM_2);
                addKey(VK_OEM_3);
                addKey(VK_OEM_4);
                addKey(VK_OEM_5);
                addKey(VK_OEM_6);
                addKey(VK_OEM_7);

                // Also include mouse side buttons if wanted.
                addKey(VK_XBUTTON1);
                addKey(VK_XBUTTON2);

                // Sweep remaining keyboard vk range for any additional named keys.
                for (DWORD vk = 1; vk < 256; ++vk) { addKey(vk); }
                return keys;
            }();
            auto& rebind = g_config.keyRebinds.rebinds[static_cast<size_t>(f3RebindIndex)];
            int selectedIndex = -1;
            for (int i = 0; i < static_cast<int>(macroKeys.size()); ++i) {
                if (macroKeys[static_cast<size_t>(i)].second == rebind.fromKey) {
                    selectedIndex = i;
                    break;
                }
            }
            const std::string previewLabel = (selectedIndex >= 0) ? macroKeys[static_cast<size_t>(selectedIndex)].first : VkToString(rebind.fromKey);
            const char* preview = previewLabel.c_str();
            ImGui::SetNextItemWidth(250.0f);
            if (ImGui::BeginCombo("[Key]", preview)) {
                for (int i = 0; i < static_cast<int>(macroKeys.size()); ++i) {
                    bool selected = (i == selectedIndex);
                    const auto& keyOpt = macroKeys[static_cast<size_t>(i)];
                    if (ImGui::Selectable(keyOpt.first.c_str(), selected)) {
                        rebind.fromKey = keyOpt.second;
                        rebind.toKey = VK_F3;
                        rebind.enabled = true;
                        rebind.useCustomOutput = false;
                        rebind.customOutputVK = 0;
                        rebind.customOutputScanCode = 0;
                        g_config.keyRebinds.enabled = true;
                        g_configIsDirty = true;
                    }
                    if (selected) { ImGui::SetItemDefaultFocus(); }
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) { ImGui::SetTooltip("%s", "Select trigger key that sends F3."); }
            ImGui::SameLine();
            if (ImGui::Checkbox("[W]", &rebind.onlyInWorld)) { g_configIsDirty = true; }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("%s", "Only fire trigger while in-world game state is active.");
            }
            ImGui::TextDisabled("[F3] %s", preview);
            MacroHoverHelp("Current trigger key mapped to F3.");
        } else {
            ImGui::TextDisabled("[F3] set [ON] to bind");
            MacroHoverHelp("Enable and choose a trigger key to map to F3.");
        }
    }

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
