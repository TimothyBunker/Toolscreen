if (ImGui::BeginTabItem("[W] Overlays")) {
    g_currentlyEditingMirror = ""; // Disable image drag mode in other tabs
    g_imageDragMode.store(false);

    // Enable window overlay drag mode when Window Overlays tab is active
    g_windowOverlayDragMode.store(true);

    int windowOverlay_to_remove = -1;
    for (size_t i = 0; i < g_config.windowOverlays.size(); ++i) {
        auto& overlay = g_config.windowOverlays[i];
        ImGui::PushID((int)i);

        // X button on the left
        std::string delete_overlay_label = "X##delete_overlay_" + std::to_string(i);
        if (ImGui::Button(delete_overlay_label.c_str(), ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) {
            std::string overlay_popup_id = "Delete Window Overlay?##" + std::to_string(i);
            ImGui::OpenPopup(overlay_popup_id.c_str());
        }

        std::string overlay_popup_id = "Delete Window Overlay?##" + std::to_string(i);
        if (ImGui::BeginPopupModal(overlay_popup_id.c_str(), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Are you sure you want to delete window overlay '%s'?\nThis cannot be undone.", overlay.name.c_str());
            ImGui::Separator();
            if (ImGui::Button("OK", ImVec2(120, 0))) {
                windowOverlay_to_remove = (int)i;
                g_configIsDirty = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetItemDefaultFocus();
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }

        ImGui::SameLine();

        // Capture old name at frame start, before any ImGui input modifies it
        std::string oldOverlayName = overlay.name;

        bool node_open = ImGui::TreeNodeEx("##overlay_node", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", overlay.name.c_str());

        if (node_open) {

            // Check for duplicate names
            bool hasDuplicate = HasDuplicateWindowOverlayName(overlay.name, i);
            if (hasDuplicate) {
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.6f, 0.2f, 0.2f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.7f, 0.3f, 0.3f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.8f, 0.3f, 0.3f, 1.0f));
            }

            if (ImGui::InputText("Name", &overlay.name)) {
                // Check if the new name is a duplicate
                if (!HasDuplicateWindowOverlayName(overlay.name, i)) {
                    g_configIsDirty = true;
                    // Update all mode references to the renamed window overlay
                    if (oldOverlayName != overlay.name) {
                        for (auto& mode : g_config.modes) {
                            for (auto& overlayId : mode.windowOverlayIds) {
                                if (overlayId == oldOverlayName) { overlayId = overlay.name; }
                            }
                        }
                    }
                } else {
                    // Revert the change if it creates a duplicate
                    overlay.name = oldOverlayName;
                }
            }

            if (hasDuplicate) { ImGui::PopStyleColor(3); }

            if (hasDuplicate) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Name already exists!");
            }

            // Dropdown for selecting from currently open windows
            ImGui::Separator();
            ImGui::Text("Select Window:");

            // Get cached window list from background thread (non-blocking)
            auto s_cachedWindows = GetCachedWindowList();

            ImGui::PushID(("window_dropdown_" + std::to_string(i)).c_str());

            // Show current selection in dropdown button if available
            std::string dropdownPreview = "Choose Window...";
            if (!overlay.windowTitle.empty()) {
                WindowInfo currentInfo;
                currentInfo.title = overlay.windowTitle;
                currentInfo.className = overlay.windowClass;
                currentInfo.executableName = overlay.executableName;
                dropdownPreview = currentInfo.GetDisplayName();
                // Truncate if too long
                if (dropdownPreview.length() > 60) { dropdownPreview = dropdownPreview.substr(0, 57) + "..."; }
            }

            if (ImGui::BeginCombo("##WindowSelector", dropdownPreview.c_str())) {
                if (s_cachedWindows.empty()) {
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No suitable windows found.");
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "WIndow capture thread may not be running.");
                } else {
                    for (const auto& windowInfo : s_cachedWindows) {
                        bool isSelected = (overlay.windowTitle == windowInfo.title && overlay.windowClass == windowInfo.className &&
                                           overlay.executableName == windowInfo.executableName);

                        // Add status indicator
                        std::string displayText = windowInfo.GetDisplayName();
                        if (!IsWindowInfoValid(windowInfo)) {
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
                        } else {
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                        }

                        if (ImGui::Selectable(displayText.c_str(), isSelected)) {
                            if (IsWindowInfoValid(windowInfo)) {
                                overlay.windowTitle = windowInfo.title;
                                overlay.windowClass = windowInfo.className;
                                overlay.executableName = windowInfo.executableName;
                                g_configIsDirty = true;
                                // Queue deferred reload to avoid blocking GUI thread
                                QueueOverlayReload(overlay.name, overlay);
                            }
                        }
                        ImGui::PopStyleColor();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();

            // Window Match Priority dropdown
            ImGui::Text("Window Match Priority");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Window title must match: Only captures windows with exact matching title\n"
                                  "Match title, otherwise find window of same executable: Prefers matching title, falls "
                                  "back to same executable (e.g chrome.exe)");
            }

            const char* priorityOptions[] = { "Window title must match", "Match title, otherwise find window of same executable" };
            const char* priorityValues[] = { "title", "title_executable" };

            int currentPriorityIdx = 0;
            for (int idx = 0; idx < 2; idx++) {
                if (overlay.windowMatchPriority == priorityValues[idx]) {
                    currentPriorityIdx = idx;
                    break;
                }
            }

            ImGui::PushItemWidth(300.0f);
            if (ImGui::Combo("##MatchPriority", &currentPriorityIdx, priorityOptions, 2)) {
                overlay.windowMatchPriority = priorityValues[currentPriorityIdx];
                g_configIsDirty = true;
                // Queue deferred reload to avoid blocking GUI thread
                QueueOverlayReload(overlay.name, overlay);
            }
            ImGui::PopItemWidth();
            ImGui::SeparatorText("Rendering");
            if (ImGui::SliderFloat("Opacity", &overlay.opacity, 0.0f, 1.0f)) g_configIsDirty = true;
            if (ImGui::Checkbox("Pixelated Scaling", &overlay.pixelatedScaling)) g_configIsDirty = true;
            if (ImGui::Checkbox("Only on my screen", &overlay.onlyOnMyScreen)) g_configIsDirty = true;
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("When enabled, this window overlay will only be visible to you and not captured by OBS");
            }

            ImGui::Columns(2, "overlay_render", false);
            ImGui::SetColumnWidth(0, 120);
            ImGui::Text("X");
            ImGui::NextColumn();
            if (Spinner("##overlay_x", &overlay.x)) g_configIsDirty = true;
            ImGui::NextColumn();
            ImGui::Text("Y");
            ImGui::NextColumn();
            if (Spinner("##overlay_y", &overlay.y)) g_configIsDirty = true;
            ImGui::NextColumn();
            ImGui::Text("Scale");
            ImGui::NextColumn();
            float scalePercent = overlay.scale * 100.0f;
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderFloat("##overlay_scale", &scalePercent, 10.0f, 200.0f, "%.0f%%")) {
                overlay.scale = scalePercent / 100.0f;
                g_configIsDirty = true;
            }
            ImGui::NextColumn();
            ImGui::Text("Relative To");
            ImGui::NextColumn();
            const char* current_rel_to = getFriendlyName(overlay.relativeTo, imageRelativeToOptions);
            ImGui::SetNextItemWidth(150);
            if (ImGui::BeginCombo("##overlay_rel_to", current_rel_to)) {
                for (const auto& option : imageRelativeToOptions) {
                    if (ImGui::Selectable(option.second, overlay.relativeTo == option.first)) {
                        overlay.relativeTo = option.first;
                        g_configIsDirty = true;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::Columns(1);

            ImGui::SeparatorText("Cropping (from source window, in pixels)");
            ImGui::Columns(2, "overlay_crop", false);
            ImGui::SetColumnWidth(0, 120);
            ImGui::Text("Crop Top");
            ImGui::NextColumn();
            if (Spinner("##overlay_crop_t", &overlay.crop_top, 1, 0)) g_configIsDirty = true;
            ImGui::NextColumn();
            ImGui::Text("Crop Bottom");
            ImGui::NextColumn();
            if (Spinner("##overlay_crop_b", &overlay.crop_bottom, 1, 0)) g_configIsDirty = true;
            ImGui::NextColumn();
            ImGui::Text("Crop Left");
            ImGui::NextColumn();
            if (Spinner("##overlay_crop_l", &overlay.crop_left, 1, 0)) g_configIsDirty = true;
            ImGui::NextColumn();
            ImGui::Text("Crop Right");
            ImGui::NextColumn();
            if (Spinner("##overlay_crop_r", &overlay.crop_right, 1, 0)) g_configIsDirty = true;
            ImGui::NextColumn();
            ImGui::Columns(1);

            ImGui::SeparatorText("Capture Settings");
            ImGui::Columns(2, "overlay_capture", false);
            ImGui::SetColumnWidth(0, 150);
            ImGui::Text("FPS");
            ImGui::NextColumn();
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderInt("##fps", &overlay.fps, 1, 60, "%d fps")) {
                g_configIsDirty = true;
                // Update FPS in cache immediately
                UpdateWindowOverlayFPS(overlay.name, overlay.fps);
            }
            ImGui::NextColumn();

            ImGui::Text("Search Interval");
            ImGui::NextColumn();
            // Convert milliseconds to seconds for display
            float searchIntervalSeconds = overlay.searchInterval / 1000.0f;
            ImGui::SetNextItemWidth(250);
            if (ImGui::SliderFloat("##searchInterval", &searchIntervalSeconds, 0.5f, 5.0f, "%.1f s")) {
                // Convert seconds back to milliseconds
                overlay.searchInterval = static_cast<int>(searchIntervalSeconds * 1000.0f);
                g_configIsDirty = true;
                // Update search interval in cache immediately (lightweight operation)
                UpdateWindowOverlaySearchInterval(overlay.name, overlay.searchInterval);
            }
            ImGui::NextColumn();
            ImGui::Columns(1);

            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("How often to search for the window if it's not found (in seconds).\nLower values find "
                                  "windows faster but use more CPU.\nRecommended: 1.0s (1 second)");
            }

            ImGui::Text("Capture Method");
            const char* captureMethods[] = { "Windows 10+", "BitBlt" };
            int currentMethodIdx = 0;
            for (int i = 0; i < 2; i++) {
                if (overlay.captureMethod == captureMethods[i]) {
                    currentMethodIdx = i;
                    break;
                }
            }
            ImGui::PushItemWidth(150.0f);
            if (ImGui::Combo("##captureMethod", &currentMethodIdx, captureMethods, 2)) {
                overlay.captureMethod = captureMethods[currentMethodIdx];
                g_configIsDirty = true;
                // Queue deferred reload to avoid blocking GUI thread
                QueueOverlayReload(overlay.name, overlay);
            }
            ImGui::PopItemWidth();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Windows 10+: Captures most windows (recommended)\n"
                                  "  - Similar to the \"Windows 10\" capture mode in OBS\n"
                                  "\n"
                                  "BitBlt: Captures from window device context, less performant\n"
                                  "  - Only recommended if Windows 10+ method doesn't work\n");
            }

            ImGui::SeparatorText("Interaction");
            if (ImGui::Checkbox("Enable Interaction", &overlay.enableInteraction)) g_configIsDirty = true;
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("When enabled, clicking on this overlay while the cursor is visible\n"
                                  "will focus it and forward mouse/keyboard inputs to the real window.\n"
                                  "Click outside the overlay or press Escape to unfocus.");
            }

            ImGui::SeparatorText("Background");
            if (ImGui::Checkbox("Enable Background", &overlay.background.enabled)) g_configIsDirty = true;
            ImGui::BeginDisabled(!overlay.background.enabled);
            if (ImGui::ColorEdit3("BG Color", &overlay.background.color.r)) g_configIsDirty = true;
            if (ImGui::SliderFloat("BG Opacity", &overlay.background.opacity, 0.0f, 1.0f)) g_configIsDirty = true;
            ImGui::EndDisabled();

            ImGui::SeparatorText("Color Keying");
            if (ImGui::Checkbox("Enable Color Key", &overlay.enableColorKey)) g_configIsDirty = true;
            ImGui::BeginDisabled(!overlay.enableColorKey);

            // Multiple color keys UI
            int colorKeyToRemove = -1;
            for (size_t k = 0; k < overlay.colorKeys.size(); k++) {
                ImGui::PushID(static_cast<int>(k));
                auto& ck = overlay.colorKeys[k];

                ImGui::Text("Key %zu:", k + 1);
                ImGui::SameLine();
                ImGui::PushItemWidth(150.0f);
                if (ImGui::ColorEdit3("##color", &ck.color.r, ImGuiColorEditFlags_NoLabel)) g_configIsDirty = true;
                ImGui::PopItemWidth();
                ImGui::SameLine();
                ImGui::PushItemWidth(80.0f);
                if (ImGui::SliderFloat("##sens", &ck.sensitivity, 0.001f, 1.0f, "%.3f")) g_configIsDirty = true;
                ImGui::PopItemWidth();
                ImGui::SameLine();
                if (ImGui::Button("X##remove")) { colorKeyToRemove = static_cast<int>(k); }
                ImGui::PopID();
            }

            // Remove color key if requested
            if (colorKeyToRemove >= 0) {
                overlay.colorKeys.erase(overlay.colorKeys.begin() + colorKeyToRemove);
                g_configIsDirty = true;
            }

            // Add new color key button
            if (ImGui::Button("+ Add Color Key")) {
                ColorKeyConfig newKey;
                newKey.color = { 0.0f, 0.0f, 0.0f };
                newKey.sensitivity = 0.05f;
                overlay.colorKeys.push_back(newKey);
                g_configIsDirty = true;
            }

            ImGui::EndDisabled();

            ImGui::SeparatorText("Border");
            if (ImGui::Checkbox("Enable Border##WindowOverlay", &overlay.border.enabled)) { g_configIsDirty = true; }
            ImGui::SameLine();
            HelpMarker("Draw a border around the window overlay.");

            if (overlay.border.enabled) {
                ImGui::Text("Color:");
                ImVec4 borderCol = ImVec4(overlay.border.color.r, overlay.border.color.g, overlay.border.color.b, 1.0f);
                if (ImGui::ColorEdit3("##BorderColorWindowOverlay", (float*)&borderCol, ImGuiColorEditFlags_NoInputs)) {
                    overlay.border.color = { borderCol.x, borderCol.y, borderCol.z };
                    g_configIsDirty = true;
                }

                ImGui::Text("Width:");
                ImGui::SetNextItemWidth(100);
                if (Spinner("##BorderWidthWindowOverlay", &overlay.border.width, 1, 1, 50)) { g_configIsDirty = true; }
                ImGui::SameLine();
                ImGui::TextDisabled("px");

                ImGui::Text("Corner Radius:");
                ImGui::SetNextItemWidth(100);
                if (Spinner("##BorderRadiusWindowOverlay", &overlay.border.radius, 1, 0, 100)) { g_configIsDirty = true; }
                ImGui::SameLine();
                ImGui::TextDisabled("px");
            }

            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    if (windowOverlay_to_remove >= 0) {
        std::string deletedOverlayName = g_config.windowOverlays[windowOverlay_to_remove].name;
        RemoveWindowOverlayFromCache(deletedOverlayName);
        g_config.windowOverlays.erase(g_config.windowOverlays.begin() + windowOverlay_to_remove);
        // Remove deleted window overlay from all modes
        for (auto& mode : g_config.modes) {
            auto it = std::find(mode.windowOverlayIds.begin(), mode.windowOverlayIds.end(), deletedOverlayName);
            while (it != mode.windowOverlayIds.end()) {
                mode.windowOverlayIds.erase(it);
                it = std::find(mode.windowOverlayIds.begin(), mode.windowOverlayIds.end(), deletedOverlayName);
            }
        }
        g_configIsDirty = true;
    }
    ImGui::Separator();
    if (ImGui::Button("Add New Window Overlay")) {
        WindowOverlayConfig newOverlay;
        newOverlay.name = "New Window Overlay " + std::to_string(g_config.windowOverlays.size() + 1);
        newOverlay.relativeTo = "centerViewport";
        g_config.windowOverlays.push_back(newOverlay);
        g_configIsDirty = true;

        // Automatically add to currently selected mode
        if (!g_currentModeId.empty()) {
            for (auto& mode : g_config.modes) {
                if (mode.id == g_currentModeId) {
                    // Check if not already added
                    if (std::find(mode.windowOverlayIds.begin(), mode.windowOverlayIds.end(), newOverlay.name) ==
                        mode.windowOverlayIds.end()) {
                        mode.windowOverlayIds.push_back(newOverlay.name);
                    }
                    break;
                }
            }
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Reset to Defaults##windowoverlays")) { ImGui::OpenPopup("Reset Window Overlays to Defaults?"); }

    if (ImGui::BeginPopupModal("Reset Window Overlays to Defaults?", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), "WARNING:");
        ImGui::Text("This will delete ALL window overlays.");
        ImGui::Text("This action cannot be undone.");
        ImGui::Separator();
        if (ImGui::Button("Confirm Reset", ImVec2(120, 0))) {
            // Clear window overlay cache before resetting
            for (const auto& overlay : g_config.windowOverlays) { RemoveWindowOverlayFromCache(overlay.name); }
            g_config.windowOverlays = GetDefaultWindowOverlays();
            g_configIsDirty = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    ImGui::EndTabItem();
}
