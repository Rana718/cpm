#include <seastar/core/app-template.hh>
#include <iostream>

int main(int argc, char** argv) {
    seastar::app_template app;
    app.run(argc, argv, [] {
        std::cout << "Hello from Seastar!" << std::endl;
        return seastar::make_ready_future<>();
    });
    return 0;
}
