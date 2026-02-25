if (ImGui::BeginTabItem("[O] Stronghold")) {
    g_currentlyEditingMirror = "";
    g_imageDragMode.store(false);
    g_windowOverlayDragMode.store(false);

    if (ImGui::Checkbox("[HUD] Stronghold", &g_config.strongholdOverlay.enabled)) { g_configIsDirty = true; }

    auto HoverHelp = [](const char* desc) {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) { ImGui::SetTooltip("%s", desc); }
    };

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

        bool startHidden = !g_config.strongholdOverlay.visible;
        if (ImGui::Checkbox("[H] Start", &startHidden)) {
            g_config.strongholdOverlay.visible = !startHidden;
            g_configIsDirty = true;
        }
        HoverHelp("Start hidden. Hotkey [H] shows/hides overlay.");

        if (ImGui::Checkbox("[G] GameHUD", &g_config.strongholdOverlay.renderInGameOverlay)) { g_configIsDirty = true; }
        HoverHelp("Render the stronghold HUD on the game view.");

        if (ImGui::Checkbox("[W] Companion", &g_config.strongholdOverlay.renderCompanionOverlay)) { g_configIsDirty = true; }
        HoverHelp("Render the detached companion window on non-game monitors.");

        if (ImGui::Checkbox("[Eye] AutoHide", &g_config.strongholdOverlay.autoHideOnEyeSpy)) { g_configIsDirty = true; }
        HoverHelp("Auto-hide overlay when Eye Spy achievement is detected.");

        const char* strongholdHudLayouts[] = { "Full", "Speedrun" };
        int hudLayoutModeRaw = std::clamp(g_config.strongholdOverlay.hudLayoutMode, 0, 2);
        if (hudLayoutModeRaw == 1) hudLayoutModeRaw = 2;
        int hudLayoutModeUi = (hudLayoutModeRaw == 0) ? 0 : 1;
        if (ImGui::Combo("[L] HUD", &hudLayoutModeUi, strongholdHudLayouts, IM_ARRAYSIZE(strongholdHudLayouts))) {
            g_config.strongholdOverlay.hudLayoutMode = (hudLayoutModeUi == 0) ? 0 : 2;
            g_configIsDirty = true;
        }
        HoverHelp("HUD layout mode.");

        if (ImGui::Checkbox("[N] Default", &g_config.strongholdOverlay.preferNetherCoords)) { g_configIsDirty = true; }
        HoverHelp("Default coordinate mode to Nether.");

        if (ImGui::Checkbox("[Lock] Auto1", &g_config.strongholdOverlay.autoLockOnFirstNether)) { g_configIsDirty = true; }
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
            if (ImGui::Checkbox("[ON] Non-MCSR", &g_config.strongholdOverlay.nonMcsrFeaturesEnabled)) { g_configIsDirty = true; }
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

        auto drawStrongholdModeIconButton = [&](const char* id, const StrongholdModeGuiIcon& icon, bool active, const char* fallbackLabel,
                                                bool preferWideIcon, bool drawDoubleEye) {
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
                if (drawDoubleEye) {
                    const ImVec2 backMin(imgMin.x - drawW * 0.14f, imgMin.y + drawH * 0.11f);
                    const ImVec2 backMax(backMin.x + drawW * 0.88f, backMin.y + drawH * 0.88f);
                    const ImVec2 frontMin(imgMin.x + drawW * 0.12f, imgMin.y - drawH * 0.09f);
                    const ImVec2 frontMax(frontMin.x + drawW, frontMin.y + drawH);
                    drawList->AddImage((ImTextureID)(intptr_t)icon.texture, backMin, backMax, ImVec2(icon.u0, icon.v0),
                                       ImVec2(icon.u1, icon.v1), IM_COL32(210, 225, 240, 215));
                    drawList->AddImage((ImTextureID)(intptr_t)icon.texture, frontMin, frontMax, ImVec2(icon.u0, icon.v0),
                                       ImVec2(icon.u1, icon.v1), IM_COL32(255, 255, 255, 255));
                } else {
                    drawList->AddImage((ImTextureID)(intptr_t)icon.texture, imgMin, imgMax, ImVec2(icon.u0, icon.v0),
                                       ImVec2(icon.u1, icon.v1), IM_COL32(255, 255, 255, 255));
                }
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
        if (drawStrongholdModeIconButton("Boat", s_boatIcon, boatEyeModeEnabled, "Boat", true, false)) {
            g_config.strongholdOverlay.standaloneAllowNonBoatThrows = false;
            g_configIsDirty = true;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("%s", "Boat mode: only boat-eye workflow captures are used.");
        }
        ImGui::SameLine();
        if (drawStrongholdModeIconButton("DoubleEye", s_eyeIcon, !boatEyeModeEnabled, "2x Eye", false, true)) {
            g_config.strongholdOverlay.standaloneAllowNonBoatThrows = true;
            g_configIsDirty = true;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("%s", "Double Eye mode: standard 1-2 eye workflow (non-boat throws).");
        }
        ImGui::PopID();

        ImGui::BeginDisabled(!g_config.strongholdOverlay.renderCompanionOverlay);
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
                g_config.strongholdOverlay.renderMonitorMask = (monitorCount >= 63) ? (~0ull >> 1) : ((1ull << monitorCount) - 1ull);
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
            }

            if (g_config.strongholdOverlay.renderMonitorMask == 0ull) { ImGui::TextDisabled("[!] No display"); }
        }
        ImGui::EndDisabled();

        if (ImGui::SliderFloat("[O]##StrongholdOverlayOpacityBasic", &g_config.strongholdOverlay.opacity, 0.1f, 1.0f, "%.2f")) {
            g_configIsDirty = true;
        }
        if (ImGui::SliderFloat("[BG]##StrongholdBackgroundBasic", &g_config.strongholdOverlay.backgroundOpacity, 0.0f, 1.0f, "%.2f")) {
            g_configIsDirty = true;
        }
        if (ImGui::SliderFloat("[S]##StrongholdScaleBasic", &g_config.strongholdOverlay.scale, 0.5f, 2.0f, "%.2fx")) {
            g_configIsDirty = true;
        }

        ImGui::SeparatorText("[Status]");
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
        if (!g_config.strongholdOverlay.renderCompanionOverlay) {
            routingLabel = "companion off";
        } else if (g_config.strongholdOverlay.renderMonitorMode == 1) {
            routingLabel = "selected mask=0x" + [&]() {
                std::ostringstream ss;
                ss << std::hex << g_config.strongholdOverlay.renderMonitorMask;
                return ss.str();
            }();
        } else {
            routingLabel = "all displays";
        }

        ImGui::Text("[State] %s", gameState.empty() ? "unknown" : gameState.c_str());
        ImGui::Text("[Disp] game=%d | %s", gameDisplayNumber, routingLabel.c_str());
        ImGui::Text("[View] gameHUD=%s | companion=%s", g_config.strongholdOverlay.renderInGameOverlay ? "on" : "off",
                    g_config.strongholdOverlay.renderCompanionOverlay ? "on" : "off");
        if (mcsrRankedInstance) {
            ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.42f, 1.0f), "MCSR: ENFORCED");
        } else {
            ImGui::TextColored(ImVec4(0.55f, 0.9f, 0.55f, 1.0f), "MCSR: FULL");
        }
        ImGui::TextColored(macroGateBlocking ? ImVec4(1.0f, 0.45f, 0.45f, 1.0f) : ImVec4(0.45f, 1.0f, 0.55f, 1.0f), "[M] %s",
                           macroGateBlocking ? "BLOCKED" : "ACTIVE");
    }

    ImGui::Separator();
    ImGui::TextDisabled("[Hotkeys] H | Shift+H | Ctrl+Shift+H | Num8/2 | Num4/6 | Num5");
    HoverHelp("H hide/show | Shift+H lock/unlock | Ctrl+Shift+H reset | Num8/2 adjust | Num4 undo | Num6 redo | Num5 clear");

    ImGui::EndTabItem();
}
