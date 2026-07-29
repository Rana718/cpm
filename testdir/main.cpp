#include <csignal>
#include <cstdlib>
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/reactor.hh>
#include <seastar/http/httpd.hh>
#include <seastar/http/routes.hh>
#include <spdlog/spdlog.h>

#include "src/handlers.hpp"

namespace ss = seastar;

static constexpr const char *DEFAULT_DB = "";

int main(int argc, char **argv) {
    init_logger();

    const char *env = std::getenv("DATABASE_URL");
    const std::string conninfo = env && *env ? env : DEFAULT_DB;

    ss::app_template app;

    app.run(argc, argv, [&]() -> ss::future<> {
        auto server = new ss::httpd::http_server_control();
        auto db = ss::make_shared<CountryDB>(conninfo);

        co_await server->start("countries-api");
        co_await server->set_routes([db](ss::httpd::routes &r) {
            r.add(ss::httpd::operation_type::GET, ss::httpd::url("/countries"), new GetCountries(*db));
            r.add(ss::httpd::operation_type::GET, ss::httpd::url("/countries/{id}"), new GetCountryById(*db));
            r.add(ss::httpd::operation_type::GET, ss::httpd::url("/countries/code/{code}"), new GetCountryByCode(*db));
            r.add(ss::httpd::operation_type::POST, ss::httpd::url("/countries"), new PostCountry(*db));
            r.add(ss::httpd::operation_type::POST, ss::httpd::url("/echo"), new PostEcho());
        });
        co_await server->listen(ss::socket_address(ss::ipv4_addr("0.0.0.0", 3000)));

        spdlog::info("Countries API running on http://localhost:3000");
        spdlog::info("GET  /countries            → list all");
        spdlog::info("GET  /countries/{{id}}       → get by id");
        spdlog::info("GET  /countries/code/{{cc}}  → get by ISO code");
        spdlog::info("POST /countries            → create (JSON body)");
        spdlog::info("POST /echo                 → echo body back");
        spdlog::info("Press Ctrl+C to stop");

        auto stop = ss::make_shared<ss::promise<>>();
        seastar::engine().handle_signal(SIGINT, [stop] { stop->set_value(); });
        co_await stop->get_future();

        spdlog::info("Shutting down...");
        co_await server->stop();
    });

    return 0;
}
