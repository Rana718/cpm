#include "src/handlers.hpp"
#include <seastar/core/app-template.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/http/httpd.hh>
#include <seastar/http/routes.hh>
#include <iostream>

namespace ss = seastar;
namespace httpd = seastar::httpd;

int main(int argc, char** argv) {
    ss::app_template app;

    app.run(argc, argv, [&]() -> ss::future<> {
        auto server = new httpd::http_server_control();
        auto db = ss::make_shared<CountryDB>();

        co_await server->start("countries-api");
        co_await server->set_routes([db](httpd::routes& r) {
            r.add(httpd::operation_type::GET,
                  httpd::url("/countries"), new GetCountries(*db));
            r.add(httpd::operation_type::GET,
                  httpd::url("/countries/{id}"), new GetCountryById(*db));
            r.add(httpd::operation_type::GET,
                  httpd::url("/countries/code/{code}"), new GetCountryByCode(*db));
            r.add(httpd::operation_type::POST,
                  httpd::url("/countries"), new PostCountry(*db));
            r.add(httpd::operation_type::POST,
                  httpd::url("/echo"), new PostEcho());
        });
        co_await server->listen(ss::socket_address(ss::ipv4_addr("0.0.0.0", 3000)));

        std::cout << "\n  🌍 Countries API running on http://localhost:3000\n\n";
        std::cout << "  GET  /countries            → list all\n";
        std::cout << "  GET  /countries/{id}       → get by id\n";
        std::cout << "  GET  /countries/code/{cc}  → get by ISO code\n";
        std::cout << "  POST /countries            → create (JSON body)\n";
        std::cout << "  POST /echo                 → echo body back\n\n";

        // Keep server running forever
        co_await ss::sleep(std::chrono::hours(24 * 365));
    });

    return 0;
}
