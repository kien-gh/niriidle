
#include "config/ConfigManager.hpp"
#include "core/Niriidle.hpp"
#include "helpers/Log.hpp"
#include <memory>

int main(int argc, char** argv, char** envp) {
    std::string configPath;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--verbose" || arg == "-v")
            Debug::verbose = true;

        else if (arg == "--quiet" || arg == "-q")
            Debug::quiet = true;

        else if (arg == "--version" || arg == "-V") {
            Debug::log(NONE, "niriidle v{}", NIRIIDLE_VERSION);
            return 0;
        }

        else if (arg == "--config" || arg == "-c") {
            if (i + 1 >= argc) {
                Debug::log(NONE, "After " + arg + " you should provide a path to a config file.");
                return 1;
            }

            if (!configPath.empty()) {
                Debug::log(NONE, "Multiple config files are provided.");
                return 1;
            }

            configPath = argv[++i];
            if (configPath[0] == '-') {
                Debug::log(NONE, "After " + arg + " you should provide a path to a config file.");
                return 1;
            }
        }

        else if (arg == "--help" || arg == "-h") {
            Debug::log(NONE,
                       "Usage: niriidle [options]\n"
                       "Options:\n"
                       "  -v, --verbose       Enable verbose logging\n"
                       "  -q, --quiet         Suppress all output except errors\n"
                       "  -V, --version       Show version information\n"
                       "  -c, --config <path> Specify a custom config file path\n"
                       "  -h, --help          Show this help message");
            return 0;
        }
    }

    g_pConfigManager = std::make_unique<CConfigManager>(configPath);

    if (g_pConfigManager->configCurrentPath.empty()) {
        if (!configPath.empty()) {
            Debug::log(CRIT, "ConfigManager: Specified file not found: {}\n", configPath);
        } else {
            Debug::log(CRIT, "ConfigManager: No niriidle.conf file found in:");
            Debug::log(NONE, "    $XDG_CONFIG_HOME/niri/, ~/.config/niri/, [XDG_CONFIG_DIRS]/niri/, /etc/xdg/niri/\n");
            Debug::log(NONE, "Create a config or specify one manually:");
            Debug::log(NONE, "    niriidle -c /path/to/conf");
        }
        return 1;
    }

    g_pConfigManager->init();

    g_pNiriidle = std::make_unique<CNiriidle>();
    g_pNiriidle->run();

    return 0;
}
