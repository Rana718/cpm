#include "src/handlers.hpp"
#include <seastar/core/app-template.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/http/httpd.hh>
#include <seastar/http/routes.hh>
#include <iostream>
#include <csignal>

namespace ss = seastar;

int main(int argc, char** argv) {
    ss::app_template app;

    app.run(argc, argv, [&]() -> ss::future<> {
        auto server = new ss::httpd::http_server_control();
        auto db = ss::make_shared<CountryDB>();

        co_await server->start("countries-api");
        co_await server->set_routes([db](ss::httpd::routes& r) {
            r.add(ss::httpd::operation_type::GET,
                  ss::httpd::url("/countries"), new GetCountries(*db));
            r.add(ss::httpd::operation_type::GET,
                  ss::httpd::url("/countries/{id}"), new GetCountryById(*db));
            r.add(ss::httpd::operation_type::GET,
                  ss::httpd::url("/countries/code/{code}"), new GetCountryByCode(*db));
            r.add(ss::httpd::operation_type::POST,
                  ss::httpd::url("/countries"), new PostCountry(*db));
            r.add(ss::httpd::operation_type::POST,
                  ss::httpd::url("/echo"), new PostEcho());
        });
        co_await server->listen(ss::socket_address(ss::ipv4_addr("0.0.0.0", 3000)));

        std::cout << "\n  🌍 Countries API running on http://localhost:3000\n\n";
        std::cout << "  GET  /countries            → list all\n";
        std::cout << "  GET  /countries/{id}       → get by id\n";
        std::cout << "  GET  /countries/code/{cc}  → get by ISO code\n";
        std::cout << "  POST /countries            → create (JSON body)\n";
        std::cout << "  POST /echo                 → echo body back\n\n";
        std::cout << "  Press Ctrl+C to stop.\n\n";

        // Wait for shutdown signal
        auto stop = ss::make_shared<ss::promise<>>();
        seastar::engine().handle_signal(SIGINT, [stop] { stop->set_value(); });
        co_await stop->get_future();

        std::cout << "\n[server] Shutting down...\n";
        co_await server->stop();
    });

    return 0;
}
