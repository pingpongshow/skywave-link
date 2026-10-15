// Skywave Link - HF Digital Communications Application
// Main entry point

#include "skywave_link/app.hpp"
#include <iostream>
#include <cstdlib>

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    std::cout << "Skywave Link v1.0.0 - HF Digital Communications\n";
    std::cout << "==========================================\n\n";

    skylink::App app;

    if (!app.initialize()) {
        std::cerr << "Failed to initialize application\n";
        return EXIT_FAILURE;
    }

    app.run();
    app.shutdown();

    return EXIT_SUCCESS;
}
