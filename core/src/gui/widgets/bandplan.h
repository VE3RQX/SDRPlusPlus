#pragma once
#include <json.hpp>
#include <imgui/imgui.h>
#include <stdint.h>
#include <iostream>

using nlohmann::json;

namespace bandplan {

    struct raster_t {
        double width = {};
        double step = {};

        operator bool() const
        {
            return width > 0.0;
        }
    };

    struct allocation_t {
        std::string name;
        std::string type;
        double start;
        double end;
        raster_t raster;
    };

    struct color_t {
        uint32_t value = IM_COL32(255, 255, 255, 255);
        uint32_t trans = IM_COL32(255, 255, 255, 100);
    };

    struct visible_t {
        bool start = false;
        bool end = false;
    };

    struct label_t {

        label_t(const allocation_t *a);

        std::string name;
        std::string type;
        color_t	color;
        visible_t visible;
    };

    struct band_t {

        band_t(double start, double end)
            : start(start), end(end)
        {
        }

        double start;
        double end;
        std::vector<label_t> labels;
    };

    void to_json(json& j, const allocation_t &b);
    void from_json(const json& j, allocation_t& b);

    struct BandPlan_t {
        std::string name;
        std::string countryName;
        std::string countryCode;
        std::string authorName;
        std::string authorURL;
        std::vector<allocation_t> allocations;

        void compile_bands();

	std::vector<band_t> bands;
    };

    void to_json(json& j, const BandPlan_t& b);
    void from_json(const json& j, BandPlan_t& b);

    void to_json(json& j, const color_t& ct);
    void from_json(const json& j, color_t& ct);

    void loadBandPlan(std::string path);
    void loadFromDir(std::string path);
    void loadColorTable(json table);

    extern std::map<std::string, BandPlan_t> bandplans;
    extern std::vector<std::string> bandplanNames;
    extern std::string bandplanNameTxt;
    extern std::map<std::string, color_t> colorTable;
};
