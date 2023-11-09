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

    std::map<std::string, color_t> colorTable;

    void generateTxt() {
        bandplanNameTxt = "";
        for(const auto &name : bandplanNames) {
            bandplanNameTxt += name;
            bandplanNameTxt += '\0';
        }
    }

    void to_json(json& j, const raster_t &r) {
        j = json{
            { "width", r.width }
        };
        if(r.step > 0)
            j["step"] = r.step;
    }

    void from_json(const json& j, raster_t &r) {
        j.at("width").get_to(r.width);
        if(j.contains("skip"))
            j.at("step").get_to(r.step);
        else
            r.step = {};
    }

    void to_json(json& j, const allocation_t &a) {
        j = json{
            { "name", a.name },
            { "type", a.type },
            { "start", a.start },
            { "end", a.end },
        };
        if(a.raster)
            to_json(j["raster"], a.raster);
    }

    void from_json(const json& j, allocation_t &a) {
        j.at("name").get_to(a.name);
        j.at("type").get_to(a.type);
        j.at("start").get_to(a.start);
        j.at("end").get_to(a.end);
        if(j.contains("raster"))
            from_json(j.at("raster"), a.raster);
        else
            a.raster = {};
    }

    void to_json(json& j, const BandPlan_t& b) {
        j = json{
            { "name", b.name },
            { "country_name", b.countryName },
            { "country_code", b.countryCode },
            { "author_name", b.authorName },
            { "author_url", b.authorURL },
            { "bands", b.allocations }
        };
    }

    void from_json(const json& j, BandPlan_t& b) {
        j.at("name").get_to(b.name);
        j.at("country_name").get_to(b.countryName);
        j.at("country_code").get_to(b.countryCode);
        j.at("author_name").get_to(b.authorName);
        j.at("author_url").get_to(b.authorURL);
        j.at("bands").get_to(b.allocations);
    }

    void to_json(json& j, const color_t &ct) {
        flog::error("ImGui color to JSON not implemented!!!");
    }

    void from_json(const json& j, color_t &ct) {
        std::string col = j.get<std::string>();

        if (col[0] != '#' || !std::all_of(col.begin() + 1, col.end(), ::isxdigit)) {
            return;
        }
        uint8_t r, g, b, a;
        r = std::stoi(col.substr(1, 2), NULL, 16);
        g = std::stoi(col.substr(3, 2), NULL, 16);
        b = std::stoi(col.substr(5, 2), NULL, 16);
        a = std::stoi(col.substr(7, 2), NULL, 16);
        ct.value = IM_COL32(r, g, b, a);
        ct.trans = IM_COL32(r, g, b, 100);
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

            if (file.path().extension().generic_string() != ".json") {
                continue;
            }

            std::string path = file.path().generic_string();

            if (file.path().filename().generic_string() == "config.json") {
                json config;
                std::ifstream is(path);

                is >> config;
                is.close();

                loadColorTable(config["bandColors"]);
            } else
                loadBandPlan(path);
        }
    }

    void loadColorTable(json table) {
        colorTable = table.get<std::map<std::string, color_t>>();
    }

    label_t::label_t(const allocation_t *a)
        : name(a->name)
    {
        if (colorTable.find(a->type.c_str()) != colorTable.end())
            color = bandplan::colorTable[a->type];
        else
            color = {};
    }

    inline static uint32_t dim(uint32_t c) {
        uint32_t r = 0xff & (c >> IM_COL32_R_SHIFT); r = (3*r)/4;
        uint32_t g = 0xff & (c >> IM_COL32_G_SHIFT); g = (3*g)/4;
        uint32_t b = 0xff & (c >> IM_COL32_B_SHIFT); b = (3*b)/4;
        uint32_t a = 0xff & (a >> IM_COL32_A_SHIFT);

        return IM_COL32(r, g, b, a);
    }

    void BandPlan_t::compile_bands() {

        if(!bands.empty())
            return;

        //
        // determine all the edges
        //
        struct edge_t {

            edge_t(bool open, const allocation_t &a)
                : open(open),
                  frequency(open ? a.start : a.end),
                  allocation(&a)
            {
            }

            bool open;
            double frequency;
        const allocation_t *allocation;

            bool
            operator<(const edge_t &e) const
            {
                return frequency < e.frequency;
            }
        };
        std::vector<edge_t> edges;

        for(auto &a : allocations) {
            edges.push_back(edge_t( true, a));
            edges.push_back(edge_t(false, a));
        }
        std::sort(edges.begin(), edges.end());

        //
        // between each edge is a "band", consisting of a number
        // of allocations.  these are gathered up into a band_t
        //
        std::vector<const allocation_t *> active;
        std::vector<const allocation_t *> opened;
        std::vector<const allocation_t *> closed;

        for(size_t k = 0; k < edges.size(); ) { // note: the ++k is done below
            //
            // gather up the changed allocations into opened and closed
            //
            size_t    b = k;

            opened = {};
            closed = {};
            while(k < edges.size() && edges[b].frequency == edges[k].frequency) {
                if(edges[k].open)
                    opened.push_back(edges[k].allocation);
                else
                    closed.push_back(edges[k].allocation);
                ++k;
            }

            //
            // the active list is in coorespondance to
            // the bands.back().labels;  we can now
            // mark the visible.end flag for any active
            // allocation in the closed list
            //
            if(!bands.empty()) {
                auto &labels = bands.back().labels;

                for(size_t l = 0; l < labels.size(); ++l) {
                    //
                    // recall:  active in correspondance to labels
                    //
                    auto a = active[l];

                    for(auto c : closed) {
                        if(c == a) {
                            labels[l].visible.end = true;

                            //
                            // remove entry without disturbing
                            // the correspondance
                            //
                            active[l] = nullptr;
                            break;
                        }
                    }
                }
            }

            //
            // we now move the opened to active
            //
            size_t w = 0;

            for(auto o : opened) {
                //
                // scan for an open slot
                //
                for(; w < active.size(); ++w)
                    if(active[w] == nullptr)
                        break;

                //
                // drop in, or append
                //
                if(w < active.size())
                    active[w] = o;
                else
                    active.push_back(o);
            }

            //
            // remove unfilled entries in active;  since
            // we know that every entry in active[0:w]
            // is non-nullptr, we can start from there.
            //
            // but we can't be too clever:  if opened
            // is empty, then w is 0, and it may be
            // nullptr, so we have to re-test to be
            // sure:
            //
            for(size_t r = w; r < active.size(); ++r)
                if((active[w] = active[r]) != nullptr)
                    ++w;

            active.erase(active.begin() + w, active.end());

            //
            // and make the next entry in bands -- such that it
            // corresponds to the active list.  we mark all
            // open edges at this point as well.
            //
            if(!active.empty() && k < edges.size()) {
                bands.emplace_back(edges[b].frequency, edges[k].frequency);

                for(auto a : active) {
                    bands.back().labels.emplace_back(a);
                    for(auto o : opened) {
                        if(o == a) {
                            bands.back().labels.back().visible.start = true;
                            break;
                        }
                    }
                }
            }
        }
    }
};
