#include <uWebSockets/App.h>
#include <iostream>

int main() {
    uWS::App()
        .get("/", [](auto *res, auto *req) {
            res->end("hello");
        })
        .post("/", [](auto *res, auto *req) {
            // Read the body and echo it back
            std::string buffer;
            res->onData([res, buffer = std::move(buffer)](std::string_view data, bool last) mutable {
                buffer.append(data);
                if (last) {
                    res->end(buffer);
                }
            });
            res->onAborted([]() {});
        })
        .listen(3000, [](auto *listen_socket) {
            if (listen_socket) {
                std::cout << "Server running on http://localhost:3000" << std::endl;
            } else {
                std::cerr << "Failed to listen on port 3000" << std::endl;
            }
        })
        .run();

    return 0;
}
