#include "src/db.hpp"
#include <uWebSockets/App.h>
#include <iostream>

// ─── Response helpers ─────────────────────────────────────────

void send_json(auto* res, const std::string& status, const json& body) {
    res->writeStatus(status)
       ->writeHeader("Content-Type", "application/json")
       ->end(body.dump(2));
}

void send_error(auto* res, const std::string& status, const std::string& msg) {
    send_json(res, status, {{"error", msg}});
}

// ─── Main ─────────────────────────────────────────────────────

int main() {
    CountryDB db;

    uWS::App()

        // GET /countries — list all
        .get("/countries", [&db](auto* res, auto*) {
            send_json(res, "200 OK", db.list_all());
        })

        // GET /countries/:id — get by id
        .get("/countries/:id", [&db](auto* res, auto* req) {
            try {
                int id = std::stoi(std::string(req->getParameter(0)));
                auto c = db.get_by_id(id);
                if (c) send_json(res, "200 OK", country_to_json(*c));
                else send_error(res, "404 Not Found", "country not found");
            } catch (...) {
                send_error(res, "400 Bad Request", "invalid id");
            }
        })

        // GET /countries/code/:code — get by ISO code
        .get("/countries/code/:code", [&db](auto* res, auto* req) {
            std::string code(req->getParameter(0));
            auto c = db.get_by_code(code);
            if (c) send_json(res, "200 OK", country_to_json(*c));
            else send_error(res, "404 Not Found", "code '" + code + "' not found");
        })

        // POST /countries — create new
        .post("/countries", [&db](auto* res, auto*) {
            std::string buffer;
            res->onData([res, &db, buffer = std::move(buffer)](std::string_view data, bool last) mutable {
                buffer.append(data);
                if (!last) return;

                json body;
                try { body = json::parse(buffer); }
                catch (...) { send_error(res, "400 Bad Request", "invalid JSON"); return; }

                Country c;
                auto err = validate_country(body, c);
                if (!err.empty()) { send_error(res, "400 Bad Request", err); return; }

                auto [ok, msg] = db.add(std::move(c));
                if (!ok) { send_error(res, "409 Conflict", msg); return; }
                send_json(res, "201 Created", db.last());
            });
            res->onAborted([] {});
        })

        // POST /echo — echo body back
        .post("/echo", [](auto* res, auto*) {
            std::string buffer;
            res->onData([res, buffer = std::move(buffer)](std::string_view data, bool last) mutable {
                buffer.append(data);
                if (last) res->end(buffer);
            });
            res->onAborted([] {});
        })

        .listen(3000, [](auto* token) {
            if (token) {
                std::cout << "\n  🌍 Countries API running on http://localhost:3000\n\n";
                std::cout << "  GET  /countries            → list all\n";
                std::cout << "  GET  /countries/:id        → get by id\n";
                std::cout << "  GET  /countries/code/:cc   → get by ISO code\n";
                std::cout << "  POST /countries            → create (JSON body)\n";
                std::cout << "  POST /echo                 → echo body back\n\n";
            } else {
                std::cerr << "Failed to listen on port 3000\n";
            }
        })
        .run();

    return 0;
}
