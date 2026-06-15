#include "ConfigManager.hpp"
#include "../helpers/Log.hpp"
#include "../helpers/MiscFunctions.hpp"
#include <filesystem>
#include <glob.h>
#include <cstring>
#include <expected>
#include <sstream>

static std::expected<std::string, std::error_code> getMainConfigPath(const std::string& overridePath = "") {
    namespace fs = std::filesystem;

    if (!overridePath.empty()) {
        std::error_code ec;
        if (!fs::exists(overridePath, ec))
            return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
        return overridePath;
    }

    // Search order: $XDG_CONFIG_HOME/niri/, ~/.config/niri/, $XDG_CONFIG_DIRS/niri/, /etc/xdg/niri/
    std::vector<std::string> searchDirs;

    const char* xdgHome = getenv("XDG_CONFIG_HOME");
    if (xdgHome && *xdgHome)
        searchDirs.push_back(std::string(xdgHome) + "/niri");
    else {
        const char* home = getenv("HOME");
        if (home && *home)
            searchDirs.push_back(std::string(home) + "/.config/niri");
    }

    const char* xdgDirs = getenv("XDG_CONFIG_DIRS");
    if (xdgDirs && *xdgDirs) {
        std::istringstream ss(xdgDirs);
        std::string        dir;
        while (std::getline(ss, dir, ':'))
            searchDirs.push_back(dir + "/niri");
    } else {
        searchDirs.push_back("/etc/xdg/niri");
    }

    for (const auto& dir : searchDirs) {
        auto candidate = dir + "/niriidle.conf";
        std::error_code ec;
        if (fs::exists(candidate, ec))
            return candidate;
    }

    return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
}

CConfigManager::CConfigManager(std::string configPath) :
    /* allowMissingConfig = 'true' to avoid library-level exceptions during initialization.
    Existence is guaranteed by the path check below. */
    m_config(getMainConfigPath(configPath).value_or("").c_str(),
             Hyprlang::SConfigOptions{.throwAllErrors = false, .allowMissingConfig = true})
{
    auto pathResult = getMainConfigPath(configPath);

    if (!pathResult)
        return;

    configCurrentPath = *pathResult;
    configHeadPath    = configCurrentPath;

    Debug::log(LOG, "Using config file: {}", configCurrentPath);
}

static Hyprlang::CParseResult handleSource(const char* c, const char* v) {
    const std::string      VALUE   = v;
    const std::string      COMMAND = c;

    const auto             RESULT = g_pConfigManager->handleSource(COMMAND, VALUE);

    Hyprlang::CParseResult result;
    if (RESULT.has_value())
        result.setError(RESULT.value().c_str());
    return result;
}

void CConfigManager::init() {
    m_config.addSpecialCategory("listener", Hyprlang::SSpecialCategoryOptions{.key = nullptr, .anonymousKeyBased = true});
    m_config.addSpecialConfigValue("listener", "timeout", Hyprlang::INT{-1});
    m_config.addSpecialConfigValue("listener", "on-timeout", Hyprlang::STRING{""});
    m_config.addSpecialConfigValue("listener", "on-resume", Hyprlang::STRING{""});
    m_config.addSpecialConfigValue("listener", "ignore_inhibit", Hyprlang::INT{0});

    m_config.addConfigValue("general:lock_cmd", Hyprlang::STRING{""});
    m_config.addConfigValue("general:unlock_cmd", Hyprlang::STRING{""});
    m_config.addConfigValue("general:on_lock_cmd", Hyprlang::STRING{""});
    m_config.addConfigValue("general:on_unlock_cmd", Hyprlang::STRING{""});
    m_config.addConfigValue("general:before_sleep_cmd", Hyprlang::STRING{""});
    m_config.addConfigValue("general:after_sleep_cmd", Hyprlang::STRING{""});
    m_config.addConfigValue("general:ignore_dbus_inhibit", Hyprlang::INT{0});
    m_config.addConfigValue("general:ignore_systemd_inhibit", Hyprlang::INT{0});
    m_config.addConfigValue("general:ignore_wayland_inhibit", Hyprlang::INT{0});
    m_config.addConfigValue("general:inhibit_sleep", Hyprlang::INT{2});
    m_config.addConfigValue("general:dpms_off_cmd", Hyprlang::STRING{"niri msg action power-off-monitors"});
    m_config.addConfigValue("general:dpms_on_cmd", Hyprlang::STRING{"niri msg action power-on-monitors"});

    // Enhanced state machine configs
    m_config.addConfigValue("general:lid_close_action", Hyprlang::STRING{"lock_suspend"});
    m_config.addConfigValue("general:lid_close_battery_action", Hyprlang::STRING{"lock_suspend"});
    m_config.addConfigValue("general:lid_close_media_playing", Hyprlang::STRING{"lock_only"});
    m_config.addConfigValue("general:lid_open_reset_idle", Hyprlang::INT{1});
    m_config.addConfigValue("general:debounce_timeout", Hyprlang::INT{1500});

    m_config.addConfigValue("general:unlocked_dim_battery", Hyprlang::INT{0});
    m_config.addConfigValue("general:unlocked_dim_ac", Hyprlang::INT{0});
    m_config.addConfigValue("general:unlocked_idle_battery", Hyprlang::INT{180});
    m_config.addConfigValue("general:unlocked_idle_ac", Hyprlang::INT{900});
    m_config.addConfigValue("general:unlocked_screen_off_delay", Hyprlang::INT{30});
    m_config.addConfigValue("general:unlocked_suspend_delay", Hyprlang::INT{60});

    m_config.addConfigValue("general:dim_cmd", Hyprlang::STRING{""});
    m_config.addConfigValue("general:dim_resume_cmd", Hyprlang::STRING{""});

    m_config.addConfigValue("general:locked_dim_battery", Hyprlang::INT{0});
    m_config.addConfigValue("general:locked_dim_ac", Hyprlang::INT{0});
    m_config.addConfigValue("general:locked_idle_battery", Hyprlang::INT{15});
    m_config.addConfigValue("general:locked_suspend_battery", Hyprlang::INT{60});
    m_config.addConfigValue("general:locked_idle_ac", Hyprlang::INT{30});
    m_config.addConfigValue("general:locked_input_battery", Hyprlang::INT{10});
    m_config.addConfigValue("general:locked_input_ac", Hyprlang::INT{30});

    m_config.addConfigValue("general:hibernate_battery_pct", Hyprlang::INT{5});
    m_config.addConfigValue("general:suspend_to_hibernate_timeout", Hyprlang::INT{14400});

    // track the file in the circular dependency chain
    alreadyIncludedSourceFiles.insert(std::filesystem::canonical(configHeadPath));

    m_config.registerHandler(&::handleSource, "source", {.allowFlags = false});

    m_config.commence();

    auto result = m_config.parse();

    if (result.error)
        Debug::log(ERR, "Config has errors:\n{}\nProceeding ignoring faulty entries", result.getError());

    result = postParse();

    if (result.error)
        Debug::log(ERR, "Config has errors:\n{}\nProceeding ignoring faulty entries", result.getError());
}

Hyprlang::CParseResult CConfigManager::postParse() {
    const auto             KEYS = m_config.listKeysForSpecialCategory("listener");

    Hyprlang::CParseResult result;
    if (KEYS.empty()) {
        result.setError("No rules configured");
        return result;
    }

    for (auto& k : KEYS) {
        STimeoutRule  rule;

        Hyprlang::INT timeout = std::any_cast<Hyprlang::INT>(m_config.getSpecialConfigValue("listener", "timeout", k.c_str()));

        rule.timeout   = timeout;
        rule.onTimeout = std::any_cast<Hyprlang::STRING>(m_config.getSpecialConfigValue("listener", "on-timeout", k.c_str()));
        rule.onResume  = std::any_cast<Hyprlang::STRING>(m_config.getSpecialConfigValue("listener", "on-resume", k.c_str()));

        rule.ignoreInhibit = std::any_cast<Hyprlang::INT>(m_config.getSpecialConfigValue("listener", "ignore_inhibit", k.c_str()));

        if (timeout == -1) {
            result.setError("Category has a missing timeout setting");
            continue;
        }

        m_vRules.emplace_back(rule);
    }

    for (auto& r : m_vRules) {
        Debug::log(LOG, "Registered timeout rule for {}s:\n      on-timeout: {}\n      on-resume: {}\n      ignore_inhibit: {}", r.timeout, r.onTimeout, r.onResume,
                   r.ignoreInhibit);
    }

    return result;
}

std::vector<CConfigManager::STimeoutRule> CConfigManager::getRules() {
    return m_vRules;
}

std::optional<std::string> CConfigManager::handleSource(const std::string& command, const std::string& rawpath) {
    if (rawpath.length() < 2)
        return "source path " + rawpath + " bogus!";

    std::unique_ptr<glob_t, void (*)(glob_t*)> glob_buf{new glob_t, [](glob_t* g) { globfree(g); }};
    memset(glob_buf.get(), 0, sizeof(glob_t));

    const auto CURRENTDIR = std::filesystem::path(configCurrentPath).parent_path().string();

    if (auto r = glob(absolutePath(rawpath, CURRENTDIR).c_str(), GLOB_TILDE, nullptr, glob_buf.get()); r != 0) {
        std::string err = std::format("source= globbing error: {}", r == GLOB_NOMATCH ? "found no match" : GLOB_ABORTED ? "read error" : "out of memory");
        Debug::log(ERR, "{}", err);
        return err;
    }

    for (size_t i = 0; i < glob_buf->gl_pathc; i++) {
        const auto PATH = absolutePath(glob_buf->gl_pathv[i], CURRENTDIR);

        if (PATH.empty() || PATH == configCurrentPath) {
            Debug::log(WARN, "source= skipping invalid path");
            continue;
        }

        if (std::find(alreadyIncludedSourceFiles.begin(), alreadyIncludedSourceFiles.end(), PATH) != alreadyIncludedSourceFiles.end()) {
            Debug::log(WARN, "source= skipping already included source file {} to prevent circular dependency", PATH);
            continue;
        }

        if (!std::filesystem::is_regular_file(PATH)) {
            if (std::filesystem::exists(PATH)) {
                Debug::log(WARN, "source= skipping non-file {}", PATH);
                continue;
            }

            Debug::log(ERR, "source= file doesnt exist");
            return "source file " + PATH + " doesn't exist!";
        }

        // track the file in the circular dependency chain
        alreadyIncludedSourceFiles.insert(PATH);

        // allow for nested config parsing
        auto backupConfigPath = configCurrentPath;
        configCurrentPath     = PATH;

        m_config.parseFile(PATH.c_str());

        configCurrentPath = backupConfigPath;
    }

    return {};
}
