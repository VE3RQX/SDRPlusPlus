#include <gui/menus/bandplan.h>
#include <gui/widgets/bandplan.h>
#include <gui/gui.h>
#include <core.h>
#include <gui/style.h>
#include <iostream>

namespace bandplanmenu {
    int bandPlanIds[2] = { -1, -1 };
    bool bandPlanEnabled;
    int bandPlanPos = 0;

    const char* bandPlanPosTxt = "Bottom\0Top\0";

    void init() {
        // todo: check if the bandplan wasn't removed
        if (bandplan::bandplanNames.size() == 0) {
            gui::waterfall.hideBandplan();
            return;
        }

        for(int i = 0; i < 2; ++i) {
            bandPlanIds[i] = -1;
            gui::waterfall.bandplan[i] = nullptr;
        }

        auto install_bandplan = [&](int place, std::string name) {

            if (bandplan::bandplans.find(name) == bandplan::bandplans.end())
                return;

            bandPlanIds[place] = std::distance(bandplan::bandplanNames.begin(),
                    std::find(bandplan::bandplanNames.begin(), bandplan::bandplanNames.end(), name));

            gui::waterfall.bandplan[place] = &bandplan::bandplans[name];
        };

        if(core::configManager.conf.contains("bandPlan")) {
            auto j = core::configManager.conf["bandPlan"];

            if(j.is_array()) {
                int place = 0;

                for(const auto &e : j) {
                    install_bandplan(place++, e);
                    if(place == 2)
                        break;
                }
            } else
                install_bandplan(0, j);
        }

        bandPlanEnabled = core::configManager.conf["bandPlanEnabled"];
        bandPlanEnabled ? gui::waterfall.showBandplan() : gui::waterfall.hideBandplan();
        bandPlanPos = core::configManager.conf["bandPlanPos"];
        gui::waterfall.setBandPlanPos(bandPlanPos);
    }

    void draw(void* ctx) {
        float menuColumnWidth = ImGui::GetContentRegionAvail().x;
        ImGui::PushItemWidth(menuColumnWidth);
        if (ImGui::Combo("##_bandplan_name_", &bandPlanIds[0], bandplan::bandplanNameTxt.c_str())) {
            gui::waterfall.bandplan[0] = &bandplan::bandplans[bandplan::bandplanNames[bandPlanIds[0]]];
            core::configManager.acquire();
            auto j = json::array();
            for(int i = 0; i < 2; ++i)
                if(bandPlanIds[i] >= 0)
                    j.push_back(bandplan::bandplanNames[bandPlanIds[i]]);
            if(j.size() != 1)
                core::configManager.conf["bandPlan"] = j;
            else
                core::configManager.conf["bandPlan"] = j[0];
            core::configManager.release(true);
        }
        ImGui::PopItemWidth();

        ImGui::LeftLabel("Position");
        ImGui::SetNextItemWidth(menuColumnWidth - ImGui::GetCursorPosX());
        if (ImGui::Combo("##_bandplan_pos_", &bandPlanPos, bandPlanPosTxt)) {
            gui::waterfall.setBandPlanPos(bandPlanPos);
            core::configManager.acquire();
            core::configManager.conf["bandPlanPos"] = bandPlanPos;
            core::configManager.release(true);
        }

        if (ImGui::Checkbox("Enabled", &bandPlanEnabled)) {
            bandPlanEnabled ? gui::waterfall.showBandplan() : gui::waterfall.hideBandplan();
            core::configManager.acquire();
            core::configManager.conf["bandPlanEnabled"] = bandPlanEnabled;
            core::configManager.release(true);
        }
        bandplan::BandPlan_t plan = bandplan::bandplans[bandplan::bandplanNames[bandPlanIds[0]]];
        ImGui::Text("Country: %s (%s)", plan.countryName.c_str(), plan.countryCode.c_str());
        ImGui::Text("Author: %s", plan.authorName.c_str());
    }
};
