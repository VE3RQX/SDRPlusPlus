#include <gui/widgets/volume_meter.h>
#include <algorithm>
#include <gui/style.h>

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui/imgui_internal.h>

namespace ImGui {
    void SNRMeter(float snr, const ImVec2& size_arg = ImVec2(0, 0)) {
        ImGuiWindow* window = GetCurrentWindow();
        ImGuiStyle& style = GImGui->Style;

        ImVec2 min = window->DC.CursorPos;
        ImVec2 size = CalcItemSize(size_arg, CalcItemWidth(), 26);
        ImRect bb(min, min + size);

        ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);

        ItemSize(size, style.FramePadding.y);
        if (!ItemAdd(bb, 0)) {
            return;
        }

        const int snr_limit = 40;

        snr = std::clamp<float>(snr, 0, snr_limit);

        float ratio = size.x / snr_limit;

        window->DrawList->AddRectFilled(min + ImVec2(0, 1),
                                        min + ImVec2(roundf(snr*ratio), 10 * style::uiScale),
                                            IM_COL32(0, 136, 255, 255));

        window->DrawList->AddLine(  min,
                                    min + ImVec2(0, (10.0f * style::uiScale) - 1),
                                        text, style::uiScale);

        window->DrawList->AddLine(  min + ImVec2(0, (10.0f * style::uiScale) - 1),
                                    min + ImVec2(size.x + 1, (10.0f * style::uiScale) - 1),
                                        text, style::uiScale);

        for (int i = 0; i <= snr_limit; i += 5) {
            auto place = roundf(i*ratio);

            window->DrawList->AddLine(  min + ImVec2(place, (10.0f * style::uiScale) - 1),
                                        min + ImVec2(place, (15.0f * style::uiScale) - 1),
                                            text, style::uiScale);

            if((i % 10) != 0)
                continue;

            char buf[32];

            if(i != snr_limit)
                snprintf(buf, sizeof(buf), "%d", i);
            else
                snprintf(buf, sizeof(buf), "%d dB", i);
            ImVec2 sz = ImGui::CalcTextSize(buf);

            window->DrawList->AddText(min + ImVec2(roundf((i*ratio) - (sz.x / 2.0)) + 1, 16.0f * style::uiScale),
                                        text, buf);
        }
    }
}
