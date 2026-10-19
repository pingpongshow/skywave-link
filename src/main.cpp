// Skywave Link - HF Digital Communications Application
// Main entry point

#include "skywave_link/app.hpp"
#include "skywave_link/secret_store.hpp"
#include <iostream>
#include <cstdlib>

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    std::cout << "Skywave Link v" SKYWAVE_LINK_VERSION " - HF Digital Communications\n\n";
    // Account passwords live in the system keychain (tests leave this off).
    skylink::secrets::useSystemStore(true);

    skylink::App app;

    if (!app.initialize()) {
        std::cerr << "Failed to initialize application\n";
        return EXIT_FAILURE;
    }

    app.run();
    app.shutdown();

    return EXIT_SUCCESS;
}
