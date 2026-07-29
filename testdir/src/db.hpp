#pragma once

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

inline void init_logger() {
    auto logger = spdlog::stdout_color_mt("api");
    logger->set_pattern("[%H:%M:%S] [%^%l%$] %v");
    spdlog::set_default_logger(logger);
    spdlog::set_level(spdlog::level::info);
}

#include <algorithm>
#include <libpq-fe.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>

using json = nlohmann::json;

struct Country {
    int id = 0;
    std::string name;
    std::string code;
    std::string capital;
    long long population = 0;
    std::string continent;
};

inline json country_to_json(const Country &c) { return {{"id", c.id}, {"name", c.name}, {"code", c.code}, {"capital", c.capital}, {"population", c.population}, {"continent", c.continent}}; }

inline std::string validate_country(const json &j, Country &c) {
    if (!j.is_object()) return "body must be a JSON object";
    if (!j.contains("name") || !j["name"].is_string()) return "'name' required";
    if (!j.contains("code") || !j["code"].is_string() || j["code"].get<std::string>().size() != 2) return "'code' (2-letter ISO) required";
    if (!j.contains("capital") || !j["capital"].is_string()) return "'capital' required";
    if (!j.contains("population") || !j["population"].is_number_integer()) return "'population' required";
    if (!j.contains("continent") || !j["continent"].is_string()) return "'continent' required";
    c.name = j["name"];
    c.code = j["code"];
    c.capital = j["capital"];
    c.population = j["population"];
    c.continent = j["continent"];
    return "";
}

class CountryDB {
    PGconn *conn_ = nullptr;

    void check(PGresult *r, ExecStatusType expected = PGRES_COMMAND_OK) {
        if (PQresultStatus(r) != expected) {
            const std::string msg = PQerrorMessage(conn_);
            PQclear(r);
            throw std::runtime_error("DB error: " + msg);
        }
    }

    Country row_to_country(PGresult *r, int row) const {
        return {
            std::stoi(PQgetvalue(r, row, 0)),
            PQgetvalue(r, row, 1),
            PQgetvalue(r, row, 2),
            PQgetvalue(r, row, 3),
            std::stoll(PQgetvalue(r, row, 4)),
            PQgetvalue(r, row, 5),
        };
    }

  public:
    explicit CountryDB(const std::string &conninfo) {
        conn_ = PQconnectdb(conninfo.c_str());
        if (PQstatus(conn_) != CONNECTION_OK) {
            const std::string err = PQerrorMessage(conn_);
            PQfinish(conn_);
            conn_ = nullptr;
            throw std::runtime_error("PostgreSQL connection failed: " + err);
        }
        spdlog::info("Connected to PostgreSQL");
    }

    ~CountryDB() {
        if (conn_) PQfinish(conn_);
    }
    // ── Queries ───────────────────────────────────────────────────────────────
    json list_all() const {
        PGresult *r = PQexec(conn_, "SELECT id,name,code,capital,population,continent FROM countries ORDER BY id");
        if (PQresultStatus(r) != PGRES_TUPLES_OK) {
            PQclear(r);
            return json::array();
        }
        json arr = json::array();
        for (int i = 0; i < PQntuples(r); ++i) arr.push_back(country_to_json(row_to_country(r, i)));
        PQclear(r);
        return arr;
    }

    std::optional<Country> get_by_id(int id) const {
        const std::string sid = std::to_string(id);
        const char *params[] = {sid.c_str()};
        PGresult *r = PQexecParams(conn_, "SELECT id,name,code,capital,population,continent FROM countries WHERE id=$1", 1, nullptr, params, nullptr, nullptr, 0);
        std::optional<Country> result;
        if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) == 1) result = row_to_country(r, 0);
        PQclear(r);
        return result;
    }

    std::optional<Country> get_by_code(const std::string &code) const {
        std::string upper = code;
        std::ranges::transform(upper, upper.begin(), ::toupper);
        const char *params[] = {upper.c_str()};
        PGresult *r = PQexecParams(conn_, "SELECT id,name,code,capital,population,continent FROM countries WHERE code=$1", 1, nullptr, params, nullptr, nullptr, 0);
        std::optional<Country> result;
        if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) == 1) result = row_to_country(r, 0);
        PQclear(r);
        return result;
    }

    std::pair<bool, std::string> add(Country c) {
        std::ranges::transform(c.code, c.code.begin(), ::toupper);
        const std::string spop = std::to_string(c.population);
        const char *params[] = {c.name.c_str(), c.code.c_str(), c.capital.c_str(), spop.c_str(), c.continent.c_str()};
        PGresult *r = PQexecParams(conn_,
            "INSERT INTO countries(name,code,capital,population,continent)"
            " VALUES($1,$2,$3,$4,$5) ON CONFLICT(code) DO NOTHING RETURNING id",
            5, nullptr, params, nullptr, nullptr, 0);
        const bool ok = PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) == 1;
        PQclear(r);
        return ok ? std::make_pair(true, std::string{}) : std::make_pair(false, "code '" + c.code + "' already exists");
    }

    json last() const {
        PGresult *r = PQexec(conn_, "SELECT id,name,code,capital,population,continent FROM countries ORDER BY id DESC LIMIT 1");
        json result;
        if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) == 1) result = country_to_json(row_to_country(r, 0));
        PQclear(r);
        return result;
    }
};
