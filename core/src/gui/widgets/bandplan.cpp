#include <gui/widgets/bandplan.h>
#include <fstream>
#include <utils/flog.h>
#include <filesystem>
#include <sstream>
#include <iomanip>

namespace bandplan {
    std::map<std::string, BandPlan_t> bandplans;
    std::vector<std::string> bandplanNames;
    std::string bandplanNameTxt;
    std::map<std::string, BandPlanColor_t> colorTable;

    void generateTxt() {
        bandplanNameTxt = "";
        for (int i = 0; i < bandplanNames.size(); i++) {
            bandplanNameTxt += bandplanNames[i];
            bandplanNameTxt += '\0';
        }
    }

    void to_json(json& j, const Band_t& b) {
        j = json{
            { "name", b.name },
            { "type", b.type },
            { "start", b.start },
            { "end", b.end },
        };
        if(b.raster.width > 0)
            j["raster_width"] = b.raster.width;
        if(b.raster.step > 0)
            j["raster_step"] = b.raster.step;
    }

    void from_json(const json& j, Band_t& b) {
        j.at("name").get_to(b.name);
        j.at("type").get_to(b.type);
        j.at("start").get_to(b.start);
        j.at("end").get_to(b.end);
        if(j.contains("raster_width"))
            j.at("raster_width").get_to(b.raster.width);
        if(j.contains("raster_skip"))
            j.at("raster_step").get_to(b.raster.step);
    }

    void to_json(json& j, const BandPlan_t& b) {
        j = json{
            { "name", b.name },
            { "country_name", b.countryName },
            { "country_code", b.countryCode },
            { "author_name", b.authorName },
            { "author_url", b.authorURL },
            { "bands", b.bands }
        };
    }

    void from_json(const json& j, BandPlan_t& b) {
        j.at("name").get_to(b.name);
        j.at("country_name").get_to(b.countryName);
        j.at("country_code").get_to(b.countryCode);
        j.at("author_name").get_to(b.authorName);
        j.at("author_url").get_to(b.authorURL);
        j.at("bands").get_to(b.bands);
    }

    void to_json(json& j, const BandPlanColor_t& ct) {
        flog::error("ImGui color to JSON not implemented!!!");
    }

    void from_json(const json& j, BandPlanColor_t& ct) {
        std::string col = j.get<std::string>();
        if (col[0] != '#' || !std::all_of(col.begin() + 1, col.end(), ::isxdigit)) {
            return;
        }
        uint8_t r, g, b, a;
        r = std::stoi(col.substr(1, 2), NULL, 16);
        g = std::stoi(col.substr(3, 2), NULL, 16);
        b = std::stoi(col.substr(5, 2), NULL, 16);
        a = std::stoi(col.substr(7, 2), NULL, 16);
        ct.colorValue = IM_COL32(r, g, b, a);
        ct.transColorValue = IM_COL32(r, g, b, 100);
    }

    void loadBandPlan(std::string path) {
        std::ifstream file(path.c_str());
        json data;
        file >> data;
        file.close();

        BandPlan_t plan = data.get<BandPlan_t>();
        if (bandplans.find(plan.name) != bandplans.end()) {
            flog::error("Duplicate band plan name ({0}), not loading.", plan.name);
            return;
        }
        bandplans[plan.name] = plan;
        bandplanNames.push_back(plan.name);
        generateTxt();
    }

    void loadFromDir(std::string path) {
        if (!std::filesystem::exists(path)) {
            flog::error("Band Plan directory does not exist");
            return;
        }
        if (!std::filesystem::is_directory(path)) {
            flog::error("Band Plan directory isn't a directory...");
            return;
        }
        bandplans.clear();
        for (const auto& file : std::filesystem::directory_iterator(path)) {
            std::string path = file.path().generic_string();
            if (file.path().extension().generic_string() != ".json") {
                continue;
            }
            loadBandPlan(path);
        }
    }

    void loadColorTable(json table) {
        colorTable = table.get<std::map<std::string, BandPlanColor_t>>();
    }

    label_t::label_t(const Band_t *b)
        : name(b->name)
    {
        if (bandplan::colorTable.find(b->type.c_str()) != bandplan::colorTable.end()) {
            type = bandplan::colorTable[b->type];
        }
        else {
            type.colorValue = IM_COL32(255, 255, 255, 255);
            type.transColorValue = IM_COL32(255, 255, 255, 100);
        }
    }

    void BandPlan_t::compile_bars() {

        if(!bars.empty())
            return;

        //
        // determine all the edges
        //
        struct edge_t {

            edge_t(bool open, const Band_t &b)
                : open(open),
                  frequency(open ? b.start : b.end),
                  band(&b)
            {
            }

            edge_t(bool open, const Band_t &b, double f)
                : open(open),
                  frequency(f),
                  band(&b)
            {
            }

            bool      open;
            double    frequency;
        const Band_t *band;

            bool
            operator<(const edge_t &e) const
            {
                return frequency < e.frequency;
            }
        };
        std::vector<edge_t> edges;

        for(auto &b : bands) {
            if(b.raster.width > 0.0) {
                double step = (b.raster.step <= 0.0) ? b.raster.width : b.raster.step;
                  
                for(double freq = b.start; freq < b.end; freq += step) {
                    edges.push_back(edge_t( true, b, freq));
                    edges.push_back(edge_t(false, b, freq));
                }
            } else {
                edges.push_back(edge_t( true, b));
                edges.push_back(edge_t(false, b));
            }
        }
        std::sort(edges.begin(), edges.end());

        //
        // between each edge is a stack of rectangles;  this
        // keeps track of which ones are on or off
        //
        std::vector<const Band_t *> active;

        for(size_t k = 0; k < edges.size(); ) {	// note: the ++k is done below
            //
            // update active bars at each distinct edge
            //
            size_t    b = k;

            while(k < edges.size() && edges[b].frequency == edges[k].frequency) {
                if(!edges[k].open) {
                    for(size_t l = 0; l < active.size(); ++l) {
                        if(active[l] == edges[k].band) {
                            active[l] = nullptr;
                            break;
                        }
                    }
                } else {
                    for(size_t l = 0; l <= active.size(); ++l) {
                        if(l == active.size()) {
                            active.push_back(edges[k].band);
                            break;
                        }
                        if(active[l] == nullptr) {
                            active[l] = edges[k].band;
                            break;
                        }
                    }
                }
                ++k;
            }

            //
            // the stack of rectangles is a "bar"
            //
            if(!active.empty() && k < edges.size()) {
                bars.push_back(bar_t(edges[b].frequency, edges[k].frequency));

                for(const auto &a : active)
                    if(a != nullptr)
                        bars.back().labels.push_back(label_t(a));
            }
        }
    }
};
