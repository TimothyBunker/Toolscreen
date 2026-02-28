if (ImGui::BeginTabItem("[B] Boat")) {
    g_currentlyEditingMirror = "";
    g_imageDragMode.store(false);
    g_windowOverlayDragMode.store(false);

    auto HoverHelp = [](const char* desc) {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) { ImGui::SetTooltip("%s", desc); }
    };
    auto ToMcPercent = [](double raw) {
        return std::clamp(static_cast<int>(raw * 200.0 + 0.0001), 0, 200);
    };

    if (ImGui::Checkbox("[ON] Setup", &g_config.boatSetup.enabled)) { g_configIsDirty = true; }
    HoverHelp("Enable boat-eye setup helper using pixel-perfect recommendations.");

    ImGui::BeginDisabled(!g_config.boatSetup.enabled);

    if (!g_config.boatSetup.prioritizeLowestPixelSkipping) {
        g_config.boatSetup.prioritizeLowestPixelSkipping = true;
        g_configIsDirty = true;
    }
    if (g_config.boatSetup.autoTrackPreferredStandardSensitivity) {
        // Hidden auto-pref behavior is confusing in the current UX. Keep manual input explicit.
        g_config.boatSetup.autoTrackPreferredStandardSensitivity = false;
        g_configIsDirty = true;
    }
    const bool manualModeActive = g_config.boatSetup.usePreferredStandardSensitivity;

    ImGui::SeparatorText("Current Input");
    if (ImGui::Checkbox("[Mode] Manual Input", &g_config.boatSetup.usePreferredStandardSensitivity)) { g_configIsDirty = true; }
    HoverHelp("ON: use typed current sensitivity/cursor values. OFF: auto-detect current values from config files.");

    int currentDpi = std::max(1, g_config.boatSetup.currentDpi);
    if (ImGui::InputInt("[DPI] Current", &currentDpi, 50, 100)) {
        g_config.boatSetup.currentDpi = std::clamp(currentDpi, 1, 50000);
        g_configIsDirty = true;
    }
    HoverHelp("Current mouse DPI baseline for recommendation math.");

    int preferredStandardPercent = ToMcPercent(g_config.boatSetup.preferredStandardSensitivity);
    int manualCurrentWindowsSpeed = std::clamp(g_config.boatSetup.manualCurrentWindowsSpeed, 1, 20);
    ImGui::BeginDisabled(!manualModeActive);
    if (ImGui::SliderInt("[Win] Current Cursor", &manualCurrentWindowsSpeed, 1, 20, "%d")) {
        g_config.boatSetup.manualCurrentWindowsSpeed = std::clamp(manualCurrentWindowsSpeed, 1, 20);
        g_configIsDirty = true;
    }
    HoverHelp("Manual mode only: your current Windows cursor speed baseline.");
    if (ImGui::SliderInt("[Sens] Current %", &preferredStandardPercent, 0, 200, "%d")) {
        g_config.boatSetup.preferredStandardSensitivity = std::clamp(preferredStandardPercent / 200.0f, 0.0f, 1.0f);
        g_configIsDirty = true;
    }
    HoverHelp("Manual mode only: your current Minecraft sensitivity percent.");
    ImGui::EndDisabled();

    ImGui::SeparatorText("Recommendation");
    if (ImGui::Checkbox("[PP] Pixel-Perfect", &g_config.boatSetup.preferPixelPerfect)) { g_configIsDirty = true; }
    HoverHelp("Use pixel-perfect recommendation engine.");
    if (ImGui::Checkbox("[Choice] #1 Lowest Skip", &g_config.boatSetup.lowestSkipChoiceOne)) {
        if (g_config.boatSetup.lowestSkipChoiceOne) { g_config.boatSetup.recommendationChoice = 1; }
        g_configIsDirty = true;
    }
    HoverHelp("When ON, recommendation stays on strict #1 lowest-skip candidate.");

    int recommendationChoice = std::clamp(g_config.boatSetup.recommendationChoice, 1, 12);
    ImGui::BeginDisabled(g_config.boatSetup.lowestSkipChoiceOne);
    if (ImGui::SliderInt("[Choice] Rank", &recommendationChoice, 1, 12, "%d")) {
        g_config.boatSetup.recommendationChoice = recommendationChoice;
        g_configIsDirty = true;
    }
    ImGui::EndDisabled();
    HoverHelp("Choose ranked alternate candidate when #1 lock is OFF.");
    if (g_config.boatSetup.lowestSkipChoiceOne && g_config.boatSetup.recommendationChoice != 1) {
        g_config.boatSetup.recommendationChoice = 1;
        g_configIsDirty = true;
    }

    int preferredCursorSpeed = std::clamp(g_config.boatSetup.preferredCursorSpeed, 0, 20);
    if (ImGui::SliderInt("[Win] Prefer Cursor", &preferredCursorSpeed, 0, 20, "%d")) {
        g_config.boatSetup.preferredCursorSpeed = preferredCursorSpeed;
        g_configIsDirty = true;
    }
    HoverHelp("Cursor-speed preference for ranking. 0 disables preference.");

    if (ImGui::Checkbox("[Rank] Include Cursor", &g_config.boatSetup.includeCursorInRanking)) { g_configIsDirty = true; }
    HoverHelp("Include cursor-speed distance in ranking score.");
    if (ImGui::Checkbox("[Rank] Prefer Higher DPI", &g_config.boatSetup.preferHigherDpi)) { g_configIsDirty = true; }
    HoverHelp("Bias ranking toward higher-DPI candidates when comparable.");

    float maxSkip = std::clamp(g_config.boatSetup.maxRecommendedPixelSkipping, 0.1f, 5000.0f);
    if (ImGui::SliderFloat("[Skip] Max", &maxSkip, 1.0f, 200.0f, "%.1f")) {
        g_config.boatSetup.maxRecommendedPixelSkipping = std::clamp(maxSkip, 0.1f, 5000.0f);
        g_configIsDirty = true;
    }
    HoverHelp("Filters candidates above this skip threshold.");

    if (!g_config.boatSetup.preferPixelPerfect) {
        int legacyTargetDpi = std::max(1, g_config.boatSetup.legacyTargetDpi);
        if (ImGui::InputInt("[Legacy] Target DPI", &legacyTargetDpi, 50, 100)) {
            g_config.boatSetup.legacyTargetDpi = std::clamp(legacyTargetDpi, 1, 50000);
            g_configIsDirty = true;
        }
        HoverHelp("Legacy target-mapped mode target DPI.");
    }

    ImGui::SeparatorText("Apply");
    if (ImGui::Checkbox("[Raw] Enable", &g_config.boatSetup.enableRawInput)) { g_configIsDirty = true; }
    HoverHelp("Set rawMouseInput:true in options.txt when applying recommendations.");

    if (ImGui::Checkbox("[Accel] Disable", &g_config.boatSetup.disableMouseAccel)) { g_configIsDirty = true; }
    HoverHelp("Disable Windows Enhance Pointer Precision style acceleration settings when applying.");

    static std::future<BoatSetupScriptRunResult> s_boatRunFuture;
    static bool s_boatRunActive = false;
    static bool s_boatHasRun = false;
    static bool s_boatLastApply = false;
    static BoatSetupScriptRunResult s_boatLastRun;
    static std::string s_boatCopyFeedback;
    static std::chrono::steady_clock::time_point s_boatCopyFeedbackUntil{};
    static bool s_showManualDpiPopup = false;
    static std::string s_manualDpiPopupText;

    auto setBoatCopyFeedback = [&](const std::string& msg) {
        s_boatCopyFeedback = msg;
        s_boatCopyFeedbackUntil = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    };

    auto buildBoatValuesSummary = [&]() -> std::string {
        if (!s_boatLastRun.parsedOk || !s_boatLastRun.payload.is_object()) return "No parsed boat calibration payload available.";
        const auto& payload = s_boatLastRun.payload;
        std::ostringstream oss;
        oss.setf(std::ios::fixed);
        oss.precision(8);

        oss << "Boat Calibration Values\n";
        oss << "Status: " << (payload.value("ok", false) ? "ok" : "error") << "\n";
        if (payload.contains("optionsPath") && payload["optionsPath"].is_string()) {
            oss << "optionsPath: " << payload["optionsPath"].get<std::string>() << "\n";
        }
        if (payload.contains("standardSettingsPath") && payload["standardSettingsPath"].is_string()) {
            oss << "standardSettingsPath: " << payload["standardSettingsPath"].get<std::string>() << "\n";
        }
        if (payload.contains("inputMode") && payload["inputMode"].is_string()) {
            oss << "inputMode: " << payload["inputMode"].get<std::string>() << "\n";
        }
        if (payload.contains("current") && payload["current"].is_object()) {
            const auto& current = payload["current"];
            oss << "\n[Current]\n";
            oss << "minecraftSensitivity: " << current.value("minecraftSensitivity", 0.0) << "\n";
            oss << "sensitivitySource: " << current.value("sensitivitySource", "unknown") << "\n";
            if (current.contains("standardSettingsSensitivity") && current["standardSettingsSensitivity"].is_number()) {
                oss << "standardSettingsSensitivity: " << current.value("standardSettingsSensitivity", 0.0) << "\n";
            }
            if (current.contains("optionsSensitivity") && current["optionsSensitivity"].is_number()) {
                oss << "optionsSensitivity: " << current.value("optionsSensitivity", 0.0) << "\n";
            }
            oss << "dpi: " << current.value("dpi", 0) << "\n";
            if (current.contains("currentCursorSpeedForCalc") && current["currentCursorSpeedForCalc"].is_number_integer()) {
                oss << "currentCursorSpeedForCalc: " << current.value("currentCursorSpeedForCalc", 0) << "\n";
            }
            if (current.contains("currentCursorSource") && current["currentCursorSource"].is_string()) {
                oss << "currentCursorSource: " << current.value("currentCursorSource", "unknown") << "\n";
            }
            oss << "windowsPointerSpeed: " << current.value("windowsPointerSpeed", 0) << "\n";
            oss << "rawMouseInput: " << current.value("rawMouseInput", "unknown") << "\n";
        }
        if (payload.contains("recommendations") && payload["recommendations"].is_object()) {
            const auto& recs = payload["recommendations"];
            if (recs.contains("active") && recs["active"].is_object()) {
                const auto& active = recs["active"];
                oss << "\n[Active Recommendation]\n";
                oss << "source: " << active.value("Source", "unknown") << "\n";
                oss << "targetDpi: " << active.value("TargetDpiRounded", 0) << "\n";
                oss << "targetCursor: " << active.value("TargetCursorSpeed", 0) << "\n";
                oss << "selectedSensitivity: " << active.value("SelectedSensitivity", 0.0) << "\n";
                if (active.contains("SecondarySensitivity") && !active["SecondarySensitivity"].is_null()) {
                    oss << "secondarySensitivity: " << active.value("SecondarySensitivity", 0.0) << "\n";
                }
                oss << "pixelSkipping: " << active.value("EstimatedPixelSkipping", 0.0) << "\n";
                if (active.contains("SelectionPolicy") && active["SelectionPolicy"].is_string()) {
                    oss << "selectionPolicy: " << active.value("SelectionPolicy", "closest-feel") << "\n";
                }
                if (active.contains("RecommendationChoice") && active["RecommendationChoice"].is_number_integer()) {
                    const int choice = active.value("RecommendationChoice", 1);
                    const int choiceMax = active.value("RecommendationChoiceMax", choice);
                    oss << "recommendationChoice: " << choice << "/" << choiceMax << "\n";
                }
                if (active.contains("LowestSkipChoiceOne") && active["LowestSkipChoiceOne"].is_boolean()) {
                    oss << "lowestSkipChoiceOne: " << (active.value("LowestSkipChoiceOne", true) ? "true" : "false") << "\n";
                }
                if (active.contains("IncludeCursorInRanking") && active["IncludeCursorInRanking"].is_boolean()) {
                    oss << "includeCursorInRanking: " << (active.value("IncludeCursorInRanking", true) ? "true" : "false") << "\n";
                }
                if (active.contains("PreferHigherDpi") && active["PreferHigherDpi"].is_boolean()) {
                    oss << "preferHigherDpi: " << (active.value("PreferHigherDpi", false) ? "true" : "false") << "\n";
                }
                if (active.contains("MaxRecommendedPixelSkipping") && active["MaxRecommendedPixelSkipping"].is_number()) {
                    oss << "maxRecommendedPixelSkipping: " << active.value("MaxRecommendedPixelSkipping", 0.0) << "\n";
                }
                if (active.contains("SkipFilterIgnored") && active["SkipFilterIgnored"].is_boolean()) {
                    oss << "skipFilterIgnored: " << (active.value("SkipFilterIgnored", false) ? "true" : "false") << "\n";
                }
                if (active.contains("CursorSpeedPreference") && active["CursorSpeedPreference"].is_number_integer()) {
                    oss << "cursorSpeedPreference: " << active.value("CursorSpeedPreference", 0) << "\n";
                }
                if (active.contains("CursorSoftSkipTolerance") && active["CursorSoftSkipTolerance"].is_number()) {
                    oss << "cursorSoftSkipTolerance: " << active.value("CursorSoftSkipTolerance", 0.0) << "\n";
                }
                if (active.contains("ClosestFeelPixelSkipping") && active["ClosestFeelPixelSkipping"].is_number()) {
                    oss << "closestFeelPixelSkipping: " << active.value("ClosestFeelPixelSkipping", 0.0) << "\n";
                }
                if (active.contains("LowestPixelSkipping") && active["LowestPixelSkipping"].is_number()) {
                    oss << "lowestPixelSkipping: " << active.value("LowestPixelSkipping", 0.0) << "\n";
                }
                if (active.contains("PreferredSpeedOverridden") && active["PreferredSpeedOverridden"].is_boolean()) {
                    oss << "preferredSpeedOverridden: " << (active.value("PreferredSpeedOverridden", false) ? "true" : "false") << "\n";
                }
                if (active.contains("PreferredSpeedSkipping") && active["PreferredSpeedSkipping"].is_number()) {
                    oss << "preferredSpeedSkipping: " << active.value("PreferredSpeedSkipping", 0.0) << "\n";
                }
                if (active.contains("AutoPixelSkipping") && active["AutoPixelSkipping"].is_number()) {
                    oss << "autoPixelSkipping: " << active.value("AutoPixelSkipping", 0.0) << "\n";
                }
            }
        }
        if (payload.contains("apply") && payload["apply"].is_object()) {
            const auto& apply = payload["apply"];
            oss << "\n[Apply]\n";
            oss << "requested: " << (apply.value("requested", false) ? "true" : "false") << "\n";
            oss << "applied: " << (apply.value("applied", false) ? "true" : "false") << "\n";
            oss << "canceled: " << (apply.value("canceled", false) ? "true" : "false") << "\n";
            if (apply.contains("message") && apply["message"].is_string()) {
                oss << "message: " << apply["message"].get<std::string>() << "\n";
            }
            if (apply.contains("after") && apply["after"].is_object()) {
                const auto& after = apply["after"];
                oss << "after.minecraftSensitivity: " << after.value("minecraftSensitivity", 0.0) << "\n";
                oss << "after.rawMouseInput: " << after.value("rawMouseInput", "unknown") << "\n";
                oss << "after.windowsPointerSpeed: " << after.value("windowsPointerSpeed", 0) << "\n";
            }
        }
        return oss.str();
    };

    if (s_boatRunActive && s_boatRunFuture.valid()) {
        const auto ready = s_boatRunFuture.wait_for(std::chrono::milliseconds(0));
        if (ready == std::future_status::ready) {
            s_boatLastRun = s_boatRunFuture.get();
            s_boatHasRun = true;
            s_boatRunActive = false;
            const bool runOk = s_boatLastRun.parsedOk && s_boatLastRun.payload.value("ok", false);
            if (s_boatLastApply && runOk && g_config.windowsMouseSpeed != 0) {
                // Avoid game-vs-desktop cursor mismatch from runtime override.
                g_config.windowsMouseSpeed = 0;
                g_configIsDirty = true;
                setBoatCopyFeedback("Disabled runtime Windows cursor override (uses real system speed).");
            }
            if (s_boatLastApply && runOk && s_boatLastRun.payload.contains("apply") && s_boatLastRun.payload["apply"].is_object()) {
                const auto& applyObj = s_boatLastRun.payload["apply"];
                if (applyObj.value("applied", false) && applyObj.contains("after") && applyObj["after"].is_object()) {
                    const auto& after = applyObj["after"];
                    if (after.contains("minecraftSensitivity") && after["minecraftSensitivity"].is_number()) {
                        const float appliedSensitivity =
                            std::clamp(static_cast<float>(after.value("minecraftSensitivity", -1.0)), 0.0f, 1.0f);
                        if (appliedSensitivity >= 0.0f) {
                            g_config.boatSetup.appliedRecommendedSensitivity = appliedSensitivity;
                            g_configIsDirty = true;
                        }
                    }
                }
            }

            if (s_boatLastApply && runOk && s_boatLastRun.payload.contains("current") && s_boatLastRun.payload["current"].is_object() &&
                s_boatLastRun.payload.contains("recommendations") && s_boatLastRun.payload["recommendations"].is_object()) {
                const auto& current = s_boatLastRun.payload["current"];
                const auto& recs = s_boatLastRun.payload["recommendations"];
                if (recs.contains("active") && recs["active"].is_object()) {
                    const auto& active = recs["active"];
                    const int currentDpiDetected = current.value("dpi", 0);
                    const int targetDpi = active.value("TargetDpiRounded", 0);
                    if (currentDpiDetected > 0 && targetDpi > 0 && currentDpiDetected != targetDpi) {
                        std::ostringstream dpiMsg;
                        dpiMsg << "Manual DPI change required.\n\n"
                               << "Current DPI: " << currentDpiDetected << "\n"
                               << "Target DPI:  " << targetDpi << "\n\n"
                               << "This cannot be changed automatically by Toolscreen.\n"
                               << "Open your mouse software and set DPI to the target value.";
                        s_manualDpiPopupText = dpiMsg.str();
                        s_showManualDpiPopup = true;
                    }
                }
            }
        }
    }

    ImGui::Separator();

    ImGui::BeginDisabled(s_boatRunActive);
    if (ImGui::Button("[Recommend]")) {
        BoatSetupConfig runCfg = g_config.boatSetup;
        std::wstring toolsPath = g_toolscreenPath;
        s_boatLastApply = false;
        s_boatRunActive = true;
        s_boatRunFuture = std::async(std::launch::async,
                                     [runCfg, toolsPath]() { return RunBoatSetupCalibrationScript(runCfg, toolsPath, false); });
    }
    HoverHelp("Preview recommendation without applying changes.");

    ImGui::SameLine();
    if (ImGui::Button("[Apply] Recommend")) {
        BoatSetupConfig runCfg = g_config.boatSetup;
        std::wstring toolsPath = g_toolscreenPath;
        s_boatLastApply = true;
        s_boatRunActive = true;
        s_boatRunFuture = std::async(std::launch::async,
                                     [runCfg, toolsPath]() { return RunBoatSetupCalibrationScript(runCfg, toolsPath, true); });
    }
    HoverHelp("Apply the active recommendation to options.txt + standardsettings.json + Windows mouse speed settings.");
    ImGui::SameLine();
    if (ImGui::Button("[Revert] Last Apply")) {
        std::wstring toolsPath = g_toolscreenPath;
        s_boatLastApply = true;
        s_boatRunActive = true;
        s_boatRunFuture = std::async(std::launch::async, [toolsPath]() { return RunBoatSetupRestoreScript(toolsPath); });
    }
    HoverHelp("Restore sensitivity/raw input/windows mouse settings from the latest boat backup.");
    ImGui::EndDisabled();

    if (s_boatRunActive) {
        ImGui::TextDisabled("Running calibration script...");
    }

    if (s_boatHasRun) {
        const bool runOk = s_boatLastRun.parsedOk && s_boatLastRun.payload.value("ok", false);
        const ImVec4 statusColor = runOk ? ImVec4(0.45f, 1.0f, 0.55f, 1.0f) : ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
        ImGui::TextColored(statusColor, "%s", runOk ? (s_boatLastApply ? "Applied" : "Recommendation Ready") : "Calibration Error");
        ImGui::SameLine();
        if (ImGui::Button("[Copy] Log")) {
            CopyToClipboard(g_minecraftHwnd.load(), s_boatLastRun.output.empty() ? "(no output)" : s_boatLastRun.output);
            setBoatCopyFeedback("Boat log copied.");
        }
        ImGui::SameLine();
        if (ImGui::Button("[Copy] Values")) {
            CopyToClipboard(g_minecraftHwnd.load(), buildBoatValuesSummary());
            setBoatCopyFeedback("Boat values copied.");
        }
        if (!s_boatCopyFeedback.empty() && std::chrono::steady_clock::now() < s_boatCopyFeedbackUntil) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", s_boatCopyFeedback.c_str());
        }

        if (!s_boatLastRun.error.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", s_boatLastRun.error.c_str());
        }

        if (!s_boatLastApply) {
            ImGui::TextDisabled("Recommend is preview-only. Use [Apply] Recommend to write changes.");
        }

        if (s_boatLastRun.parsedOk) {
            const auto& payload = s_boatLastRun.payload;
            if (payload.contains("recommendations") && payload["recommendations"].is_object()) {
                const auto& recs = payload["recommendations"];
                if (recs.contains("active") && recs["active"].is_object()) {
                    const auto& active = recs["active"];
                    const std::string source = active.value("Source", "unknown");
                    std::string inputMode = "auto";
                    if (payload.contains("inputMode") && payload["inputMode"].is_string()) {
                        inputMode = payload.value("inputMode", "auto");
                    } else if (payload.contains("current") && payload["current"].is_object() &&
                               payload["current"].contains("inputMode") && payload["current"]["inputMode"].is_string()) {
                        inputMode = payload["current"].value("inputMode", "auto");
                    }
                    const int targetDpi = active.value("TargetDpiRounded", 0);
                    const int targetCursor = active.value("TargetCursorSpeed", 0);
                    const double selectedSensitivity = active.value("SelectedSensitivity", 0.0);
                    const double pixelSkipping = active.value("EstimatedPixelSkipping", 0.0);

                    ImGui::SeparatorText("Active Recommendation");
                    const std::string policy = active.value("SelectionPolicy", "");
                    ImGui::TextColored(ImVec4(0.45f, 1.0f, 0.55f, 1.0f),
                                       "Active recommendation: SELECTED CHOICE");
                    ImGui::Text("Input mode: %s", inputMode == "manual" ? "MANUAL (Typed Current Sens)" : "AUTO (Detect Current Sens)");
                    ImGui::TextDisabled("Source: %s", source.c_str());
                    ImGui::Text("DPI: %d  |  Cursor: %d", targetDpi, targetCursor);
                    ImGui::Text("Sensitivity: %.8f", selectedSensitivity);
                    ImGui::Text("Pixel skipping: %.2f", pixelSkipping);

                    if (active.contains("SecondarySensitivity") && !active["SecondarySensitivity"].is_null()) {
                        ImGui::TextDisabled("Alt sensitivity: %.8f", active.value("SecondarySensitivity", 0.0));
                    }

                    if (!policy.empty()) {
                        ImGui::TextDisabled("Selection policy: %s", policy.c_str());
                    } else {
                        ImGui::TextDisabled("Selection policy: %s",
                                            g_config.boatSetup.prioritizeLowestPixelSkipping ? "lowest-skipping" : "closest-feel");
                    }
                    ImGui::TextDisabled("Ranking: cursor %s | higher-DPI %s | max-skip %.1f",
                                        active.value("IncludeCursorInRanking", true) ? "on" : "off",
                                        active.value("PreferHigherDpi", false) ? "on" : "off",
                                        active.value("MaxRecommendedPixelSkipping", 0.0));
                    ImGui::TextDisabled("#1 mode: %s", active.value("LowestSkipChoiceOne", true) ? "strict lowest-skip" : "balanced");
                    if (active.value("SkipFilterIgnored", false)) {
                        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.35f, 1.0f),
                                           "Skip filter had no candidates and was ignored.");
                    }
                    int selectedChoice = 1;
                    int selectedChoiceMax = 1;
                    if (active.contains("RecommendationChoice") && active["RecommendationChoice"].is_number_integer()) {
                        selectedChoice = active.value("RecommendationChoice", 1);
                        selectedChoiceMax = active.value("RecommendationChoiceMax", selectedChoice);
                        ImGui::TextDisabled("Choice: %d/%d (click a row below or use [Choice] Rank)", selectedChoice, selectedChoiceMax);
                    }
                    if (active.contains("CursorSpeedPreference") && active["CursorSpeedPreference"].is_number_integer()) {
                        const int cursorPreference = active.value("CursorSpeedPreference", 0);
                        if (cursorPreference > 0) {
                            const double skipTol = active.value("CursorSoftSkipTolerance", 0.0);
                            ImGui::TextDisabled("Cursor preference: %d (soft, skip band +%.2f)", cursorPreference, skipTol);
                        }
                    }
                    if (active.contains("CandidateChoices") && active["CandidateChoices"].is_array() && !active["CandidateChoices"].empty()) {
                        if (ImGui::CollapsingHeader("[Choices] Candidate Targets")) {
                            for (const auto& row : active["CandidateChoices"]) {
                                if (!row.is_object()) continue;
                                const int rank = row.value("Rank", 0);
                                const int rowDpi = row.value("TargetDpiRounded", 0);
                                const int rowCursor = row.value("TargetCursorSpeed", 0);
                                const double rowSkip = row.value("EstimatedPixelSkipping", 0.0);
                                const int rowDelta = row.value("SpeedDeltaFromPreference", 0);
                                const double rowSensDeltaPct = row.value("SensitivityDeltaPercent", 0.0);
                                std::ostringstream rowLabel;
                                rowLabel.setf(std::ios::fixed);
                                rowLabel.precision(2);
                                rowLabel << "#" << rank << "  DPI " << rowDpi << "  Cursor " << rowCursor << "  Skip " << rowSkip
                                         << "  dSens " << rowSensDeltaPct << "%  dCursor " << rowDelta;
                                const bool isSelectedRow = (rank == selectedChoice);
                                ImGui::PushID(rank);
                                if (ImGui::Selectable(rowLabel.str().c_str(), isSelectedRow)) {
                                    g_config.boatSetup.recommendationChoice = std::clamp(rank, 1, 12);
                                    g_configIsDirty = true;
                                    setBoatCopyFeedback("Choice selected. Recomputing recommendation...");
                                    if (!s_boatRunActive) {
                                        BoatSetupConfig runCfg = g_config.boatSetup;
                                        std::wstring toolsPath = g_toolscreenPath;
                                        s_boatLastApply = false;
                                        s_boatRunActive = true;
                                        s_boatRunFuture = std::async(std::launch::async,
                                                                     [runCfg, toolsPath]() {
                                                                         return RunBoatSetupCalibrationScript(runCfg, toolsPath, false);
                                                                     });
                                    }
                                }
                                ImGui::PopID();
                            }
                        }
                    }
                    if (payload.contains("current") && payload["current"].is_object()) {
                        const auto& current = payload["current"];
                        ImGui::SeparatorText("Current Detected");
                        const double mcRaw = current.value("minecraftSensitivity", 0.0);
                        const int mcPercent = ToMcPercent(mcRaw);
                        ImGui::Text("Minecraft sens: %d%%", mcPercent);
                        ImGui::TextDisabled("Raw: %.8f", mcRaw);
                        if (current.contains("sensitivitySource") && current["sensitivitySource"].is_string()) {
                            ImGui::Text("Sens source: %s", current.value("sensitivitySource", "unknown").c_str());
                        }
                        if (current.contains("standardSettingsSensitivity") && current["standardSettingsSensitivity"].is_number()) {
                            const double stdRaw = current.value("standardSettingsSensitivity", 0.0);
                            const int stdPercent = ToMcPercent(stdRaw);
                            ImGui::Text("Stdsettings sens: %d%%", stdPercent);
                            ImGui::TextDisabled("Std raw: %.8f", stdRaw);
                        }
                        if (current.contains("optionsSensitivity") && current["optionsSensitivity"].is_number()) {
                            const double optRaw = current.value("optionsSensitivity", 0.0);
                            const int optPercent = ToMcPercent(optRaw);
                            ImGui::Text("options.txt sens: %d%%", optPercent);
                            ImGui::TextDisabled("Opt raw: %.8f", optRaw);
                        }
                        ImGui::Text("Raw input: %s", current.value("rawMouseInput", "unknown").c_str());
                        ImGui::Text("DPI: %d", current.value("dpi", 0));
                        ImGui::Text("Windows pointer speed: %d", current.value("windowsPointerSpeed", 0));
                        ImGui::Text("Windows accel disabled: %s", current.value("windowsAccelDisabled", false) ? "yes" : "no");
                    }

                    ImGui::SeparatorText("Planned Apply");
                    const int selectedPercent = ToMcPercent(selectedSensitivity);
                    ImGui::Text("Minecraft sensitivity -> %d%%", selectedPercent);
                    ImGui::TextDisabled("Raw apply value -> %.8f", selectedSensitivity);
                    ImGui::Text("Raw input -> %s", g_config.boatSetup.enableRawInput ? "true" : "unchanged");
                    ImGui::Text("Windows pointer speed -> %d", targetCursor);
                    ImGui::Text("Disable mouse accel -> %s", g_config.boatSetup.disableMouseAccel ? "yes" : "no");
                    if (g_config.windowsMouseSpeed > 0 && g_config.windowsMouseSpeed != targetCursor) {
                        ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.32f, 1.0f),
                                           "Global windowsMouseSpeed override (%d) can override this target.",
                                           g_config.windowsMouseSpeed);
                        if (ImGui::Button("[Sync] Global Override")) {
                            g_config.windowsMouseSpeed = targetCursor;
                            g_configIsDirty = true;
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("[Disable] Global Override")) {
                            g_config.windowsMouseSpeed = 0;
                            g_configIsDirty = true;
                        }
                    }
                    if (active.contains("SelectionPolicy") && active["SelectionPolicy"].is_string()) {
                        const std::string policy = active.value("SelectionPolicy", "closest-feel");
                        ImGui::Text("Policy -> %s", policy.c_str());
                    }
                    if (active.contains("ClosestFeelPixelSkipping") && active.contains("LowestPixelSkipping") &&
                        active["ClosestFeelPixelSkipping"].is_number() && active["LowestPixelSkipping"].is_number()) {
                        ImGui::Text("Skip (closest vs low) -> %.2f / %.2f", active.value("ClosestFeelPixelSkipping", 0.0),
                                    active.value("LowestPixelSkipping", 0.0));
                    }
                    if (payload.contains("current") && payload["current"].is_object()) {
                        const auto& current = payload["current"];
                        const int currentDpiDetected = current.value("dpi", 0);
                        if (currentDpiDetected > 0 && currentDpiDetected != targetDpi) {
                            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.45f, 1.0f), "DPI manual change required: %d -> %d",
                                               currentDpiDetected, targetDpi);
                        } else if (targetDpi > 0) {
                            ImGui::Text("DPI target: %d (unchanged)", targetDpi);
                        }
                    } else if (targetDpi > 0) {
                        ImGui::Text("DPI target: %d (manual in mouse software)", targetDpi);
                    }
                }
            }

            if (payload.contains("mouse") && payload["mouse"].is_object()) {
                const auto& mouse = payload["mouse"];
                ImGui::SeparatorText("Mouse Software Hints");

                if (mouse.contains("softwareHints") && mouse["softwareHints"].is_array() && !mouse["softwareHints"].empty()) {
                    for (const auto& hint : mouse["softwareHints"]) {
                        if (!hint.is_object()) continue;
                        const std::string vendor = hint.value("Vendor", "Unknown");
                        const std::string software = hint.value("Software", "Vendor utility");
                        const bool installed = hint.value("Installed", false);
                        ImGui::Text("%s: %s", vendor.c_str(), software.c_str());
                        ImGui::SameLine();
                        ImGui::TextColored(installed ? ImVec4(0.45f, 1.0f, 0.55f, 1.0f) : ImVec4(1.0f, 0.58f, 0.42f, 1.0f), "%s",
                                           installed ? "installed" : "missing");
                    }
                } else {
                    ImGui::TextDisabled("No vendor-specific software detected automatically.");
                }

                if (mouse.contains("advice") && mouse["advice"].is_array() && !mouse["advice"].empty()) {
                    for (const auto& advice : mouse["advice"]) {
                        if (!advice.is_string()) continue;
                        const std::string adviceText = advice.get<std::string>();
                        ImGui::TextWrapped("%s", adviceText.c_str());
                    }
                }
            }

            if (payload.contains("apply") && payload["apply"].is_object()) {
                const auto& apply = payload["apply"];
                const std::string msg = apply.value("message", "");
                if (!msg.empty()) {
                    ImGui::Separator();
                    ImGui::TextWrapped("%s", msg.c_str());
                }
            }
        }

        if (ImGui::CollapsingHeader("[Log] Script Output", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::BeginChild("BoatSetupScriptOutput", ImVec2(0.0f, 130.0f), true, ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::TextUnformatted(s_boatLastRun.output.empty() ? "(no output)" : s_boatLastRun.output.c_str());
            ImGui::EndChild();
        }
    } else {
        ImGui::TextDisabled("Run [Recommend] to preview settings and mouse-software guidance.");
    }

    if (s_showManualDpiPopup) {
        ImGui::OpenPopup("Manual DPI Change Required");
        s_showManualDpiPopup = false;
    }
    if (ImGui::BeginPopupModal("Manual DPI Change Required", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "IMPORTANT");
        ImGui::Separator();
        ImGui::TextWrapped("%s", s_manualDpiPopupText.c_str());
        ImGui::Spacing();
        if (ImGui::Button("[Copy] DPI Notice")) {
            CopyToClipboard(g_minecraftHwnd.load(), s_manualDpiPopupText);
            setBoatCopyFeedback("DPI notice copied.");
        }
        ImGui::SameLine();
        if (ImGui::Button("OK", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::EndDisabled();
    ImGui::EndTabItem();
}
