#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <optional>
#include <algorithm>

using json = nlohmann::json;

struct Country {
    int id = 0;
    std::string name;
    std::string code;
    std::string capital;
    long long population = 0;
    std::string continent;
};

inline json country_to_json(const Country& c) {
    return {
        {"id", c.id}, {"name", c.name}, {"code", c.code},
        {"capital", c.capital}, {"population", c.population},
        {"continent", c.continent}
    };
}

inline std::string validate_country(const json& j, Country& c) {
    if (!j.is_object()) return "body must be a JSON object";
    if (!j.contains("name") || !j["name"].is_string()) return "'name' required";
    if (!j.contains("code") || !j["code"].is_string() || j["code"].get<std::string>().size() != 2)
        return "'code' (2-letter ISO) required";
    if (!j.contains("capital") || !j["capital"].is_string()) return "'capital' required";
    if (!j.contains("population") || !j["population"].is_number_integer()) return "'population' required";
    if (!j.contains("continent") || !j["continent"].is_string()) return "'continent' required";

    c.name = j["name"]; c.code = j["code"]; c.capital = j["capital"];
    c.population = j["population"]; c.continent = j["continent"];
    return "";
}

class CountryDB {
    std::vector<Country> countries_;
    int next_id_ = 1;
public:
    CountryDB() {
        add({0, "India",         "IN", "New Delhi",     1428627663, "Asia"});
        add({0, "United States", "US", "Washington DC", 335893238,  "North America"});
        add({0, "Germany",       "DE", "Berlin",        83794000,   "Europe"});
        add({0, "Brazil",        "BR", "Brasília",      215313498,  "South America"});
        add({0, "Japan",         "JP", "Tokyo",         123294513,  "Asia"});
    }

    json list_all() const {
        json arr = json::array();
        for (const auto& c : countries_) arr.push_back(country_to_json(c));
        return arr;
    }

    std::optional<Country> get_by_id(int id) const {
        auto it = std::find_if(countries_.begin(), countries_.end(),
            [id](const Country& c) { return c.id == id; });
        return it != countries_.end() ? std::optional{*it} : std::nullopt;
    }

    std::optional<Country> get_by_code(const std::string& code) const {
        std::string upper = code;
        std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
        auto it = std::find_if(countries_.begin(), countries_.end(),
            [&upper](const Country& c) { return c.code == upper; });
        return it != countries_.end() ? std::optional{*it} : std::nullopt;
    }

    std::pair<bool, std::string> add(Country c) {
        if (get_by_code(c.code)) return {false, "code '" + c.code + "' exists"};
        c.id = next_id_++;
        countries_.push_back(std::move(c));
        return {true, ""};
    }

    json last() const { return country_to_json(countries_.back()); }
};
