if (ImGui::BeginTabItem("[?] Misc")) {
    g_currentlyEditingMirror = "";
    g_imageDragMode.store(false);
    g_windowOverlayDragMode.store(false);

    ImGui::SeparatorText("Info");

    // Static flag for license popup
    static bool s_showLicensesPopup = false;

    if (ImGui::Button("Licenses")) { s_showLicensesPopup = true; }
    ImGui::SameLine();
    HelpMarker("View license information for open-source libraries used in this project.");

    ImGui::Spacing();
    ImGui::SeparatorText("Toolscreen");
    if (ImGui::Button("Open Config Folder")) {
        if (g_toolscreenPath.empty()) {
            Log("ERROR: Unable to open config folder because toolscreen path is empty.");
        } else {
            std::error_code ec;
            if (!std::filesystem::exists(g_toolscreenPath, ec)) { std::filesystem::create_directories(g_toolscreenPath, ec); }

            HINSTANCE shellResult = ShellExecuteW(NULL, L"open", g_toolscreenPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
            if ((INT_PTR)shellResult <= 32) { Log("ERROR: Failed to open Toolscreen config folder."); }
        }
    }
    ImGui::SameLine();
    HelpMarker("Open the Toolscreen folder that contains config.toml.");

    // License popup modal
    if (s_showLicensesPopup) { ImGui::OpenPopup("Open-Source Licenses"); }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(700, 500), ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("Open-Source Licenses", &s_showLicensesPopup, ImGuiWindowFlags_NoResize)) {
        ImGui::TextWrapped("This software uses the following open-source libraries:");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::BeginChild("LicenseScrollArea", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - 10), true)) {
            // Dear ImGui
            if (ImGui::CollapsingHeader("Dear ImGui", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Indent();
                ImGui::TextWrapped("Copyright (c) 2014-2025 Omar Cornut");
                ImGui::Spacing();
                ImGui::TextWrapped("The MIT License (MIT)");
                ImGui::Spacing();
                ImGui::TextWrapped("Permission is hereby granted, free of charge, to any person obtaining a copy "
                                   "of this software and associated documentation files (the \"Software\"), to deal "
                                   "in the Software without restriction, including without limitation the rights "
                                   "to use, copy, modify, merge, publish, distribute, sublicense, and/or sell "
                                   "copies of the Software, and to permit persons to whom the Software is "
                                   "furnished to do so, subject to the following conditions:");
                ImGui::Spacing();
                ImGui::TextWrapped("The above copyright notice and this permission notice shall be included in all "
                                   "copies or substantial portions of the Software.");
                ImGui::Spacing();
                ImGui::TextWrapped("THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR "
                                   "IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, "
                                   "FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.");
                ImGui::Unindent();
            }

            ImGui::Spacing();

            // nlohmann JSON
            if (ImGui::CollapsingHeader("JSON for Modern C++ (nlohmann/json)")) {
                ImGui::Indent();
                ImGui::TextWrapped("Copyright (c) 2013-2022 Niels Lohmann");
                ImGui::Spacing();
                ImGui::TextWrapped("The MIT License (MIT)");
                ImGui::Spacing();
                ImGui::TextWrapped("Permission is hereby granted, free of charge, to any person obtaining a copy "
                                   "of this software and associated documentation files (the \"Software\"), to deal "
                                   "in the Software without restriction, including without limitation the rights "
                                   "to use, copy, modify, merge, publish, distribute, sublicense, and/or sell "
                                   "copies of the Software, and to permit persons to whom the Software is "
                                   "furnished to do so, subject to the following conditions:");
                ImGui::Spacing();
                ImGui::TextWrapped("The above copyright notice and this permission notice shall be included in all "
                                   "copies or substantial portions of the Software.");
                ImGui::Spacing();
                ImGui::TextWrapped("THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR "
                                   "IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, "
                                   "FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.");
                ImGui::Unindent();
            }

            ImGui::Spacing();

            // stb_image
            if (ImGui::CollapsingHeader("stb_image")) {
                ImGui::Indent();
                ImGui::TextWrapped("By Sean Barrett and contributors");
                ImGui::Spacing();
                ImGui::TextWrapped("Public Domain / MIT License (dual-licensed)");
                ImGui::Spacing();
                ImGui::TextWrapped("This software is available under two licenses -- choose whichever you prefer:");
                ImGui::Spacing();
                ImGui::BulletText("UNLICENSE (Public Domain)");
                ImGui::BulletText("MIT License");
                ImGui::Spacing();
                ImGui::TextWrapped("This is free and unencumbered software released into the public domain. "
                                   "Anyone is free to copy, modify, publish, use, compile, sell, or distribute "
                                   "this software, either in source code form or as a compiled binary, for any "
                                   "purpose, commercial or non-commercial, and by any means.");
                ImGui::Unindent();
            }

            ImGui::Spacing();

            // GLEW
            if (ImGui::CollapsingHeader("GLEW (OpenGL Extension Wrangler Library)")) {
                ImGui::Indent();
                ImGui::TextWrapped("Copyright (c) 2002-2007, Milan Ikits");
                ImGui::TextWrapped("Copyright (c) 2002-2007, Marcelo E. Magallon");
                ImGui::TextWrapped("Copyright (c) 2002, Lev Povalahev");
                ImGui::Spacing();
                ImGui::TextWrapped("Modified BSD License / MIT License");
                ImGui::Spacing();
                ImGui::TextWrapped("Redistribution and use in source and binary forms, with or without modification, "
                                   "are permitted provided that the following conditions are met:");
                ImGui::Spacing();
                ImGui::BulletText("Redistributions of source code must retain the above copyright notice.");
                ImGui::BulletText("Redistributions in binary form must reproduce the above copyright notice.");
                ImGui::BulletText("The name of the author may not be used to endorse products.");
                ImGui::Spacing();
                ImGui::TextWrapped("THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS \"AS IS\" "
                                   "AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE "
                                   "IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE "
                                   "ARE DISCLAIMED.");
                ImGui::Unindent();
            }

            ImGui::Spacing();

            // minhook-detours
            if (ImGui::CollapsingHeader("minhook-detours")) {
                ImGui::Indent();
                ImGui::TextWrapped("https://github.com/m417z/minhook-detours");
                ImGui::TextWrapped("Copyright (c) 2009-2017, Tsuda Kageyu (MinHook)");
                ImGui::TextWrapped("Modified by m417z");
                ImGui::Spacing();
                ImGui::TextWrapped("BSD 2-Clause License");
                ImGui::Spacing();
                ImGui::TextWrapped("Redistribution and use in source and binary forms, with or without modification, "
                                   "are permitted provided that the following conditions are met:");
                ImGui::Spacing();
                ImGui::BulletText("Redistributions of source code must retain the above copyright notice.");
                ImGui::BulletText("Redistributions in binary form must reproduce the above copyright notice.");
                ImGui::Spacing();
                ImGui::TextWrapped("THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS \"AS IS\" "
                                   "AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE "
                                   "IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE "
                                   "ARE DISCLAIMED.");
                ImGui::Unindent();
            }

            ImGui::EndChild();
        }

        ImGui::Spacing();

        // Center the Close button
        float buttonWidth = 120.0f;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - buttonWidth) / 2.0f);
        if (ImGui::Button("Close", ImVec2(buttonWidth, 0))) {
            s_showLicensesPopup = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    ImGui::EndTabItem();
}
