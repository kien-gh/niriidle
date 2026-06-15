#include "Niriidle.hpp"
#include "../helpers/Log.hpp"
#include "../config/ConfigManager.hpp"
#include "csignal"
#include <sys/wait.h>
#include <sys/poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <thread>
#include <mutex>
#include <cstring>
#include <hyprutils/os/Process.hpp>

CNiriidle::CNiriidle() {
    m_sWaylandState.display = wl_display_connect(nullptr);
    if (!m_sWaylandState.display) {
        Debug::log(CRIT, "Couldn't connect to a wayland compositor");
        exit(1);
    }
}

// ── SIdleListenerGroup ─────────────────────────────────────────────────────

void CNiriidle::SIdleListenerGroup::destroy() {
    for (auto& l : listeners) {
        if (l.notification)
            l.notification->sendDestroy();
    }
    listeners.clear();
    active = false;
}

void CNiriidle::SIdleListenerGroup::create(SP<CCExtIdleNotifierV1> notifier, wl_proxy* seat, bool ignoreWaylandInhibit,
                                            const std::vector<std::pair<uint64_t, std::pair<std::string, std::string>>>& timeouts, bool ignoreInhibit) {
    destroy();
    listeners.resize(timeouts.size());

    for (size_t i = 0; i < timeouts.size(); ++i) {
        auto& l            = listeners[i];
        l.onTimeout        = timeouts[i].second.first;
        l.onRestore        = timeouts[i].second.second;
        l.ignoreInhibit    = ignoreInhibit;
        l.onTimeoutFired   = false;

        if (ignoreWaylandInhibit || ignoreInhibit)
            l.notification = makeShared<CCExtIdleNotificationV1>(
                notifier->sendGetInputIdleNotification(timeouts[i].first * 1000, seat));
        else
            l.notification = makeShared<CCExtIdleNotificationV1>(
                notifier->sendGetIdleNotification(timeouts[i].first * 1000, seat));
    }

    active = true;
}

// ── Parsing helpers ────────────────────────────────────────────────────────

CNiriidle::ELidCloseAction CNiriidle::parseLidCloseAction(const std::string& s) const {
    if (s == "lock_only")
        return ELidCloseAction::LOCK_ONLY;
    if (s == "suspend_only")
        return ELidCloseAction::SUSPEND_ONLY;
    if (s == "nothing")
        return ELidCloseAction::NOTHING;
    return ELidCloseAction::LOCK_SUSPEND;
}

// ── run() ──────────────────────────────────────────────────────────────────

void CNiriidle::run() {
    m_sWaylandState.registry = makeShared<CCWlRegistry>((wl_proxy*)wl_display_get_registry(m_sWaylandState.display));
    m_sWaylandState.registry->setGlobal([this](CCWlRegistry* r, uint32_t name, const char* interface, uint32_t version) {
        const std::string IFACE = interface;
        Debug::log(LOG, "  | got iface: {} v{}", IFACE, version);

        if (IFACE == ext_idle_notifier_v1_interface.name) {
            m_sWaylandIdleState.notifier =
                makeShared<CCExtIdleNotifierV1>((wl_proxy*)wl_registry_bind((wl_registry*)r->resource(), name, &ext_idle_notifier_v1_interface, version));
            Debug::log(LOG, "   > Bound to {} v{}", IFACE, version);
        } else if (IFACE == hyprland_lock_notifier_v1_interface.name) {
            m_sWaylandState.lockNotifier =
                makeShared<CCHyprlandLockNotifierV1>((wl_proxy*)wl_registry_bind((wl_registry*)r->resource(), name, &hyprland_lock_notifier_v1_interface, version));
            Debug::log(LOG, "   > Bound to {} v{}", IFACE, version);
        } else if (IFACE == wl_seat_interface.name) {
            if (m_sWaylandState.seat) {
                Debug::log(WARN, "niriidle does not support multi-seat configurations. Only binding to the first seat.");
                return;
            }

            m_sWaylandState.seat = makeShared<CCWlSeat>((wl_proxy*)wl_registry_bind((wl_registry*)r->resource(), name, &wl_seat_interface, version));
            Debug::log(LOG, "   > Bound to {} v{}", IFACE, version);
        }
    });

    m_sWaylandState.registry->setGlobalRemove([](CCWlRegistry* r, uint32_t name) { Debug::log(LOG, "  | removed iface {}", name); });

    wl_display_roundtrip(m_sWaylandState.display);

    if (!m_sWaylandIdleState.notifier) {
        Debug::log(CRIT, "Couldn't bind to ext-idle-notifier-v1, does your compositor support it?");
        exit(1);
    }

    static const auto IGNOREWAYLANDINHIBIT = g_pConfigManager->getValue<Hyprlang::INT>("general:ignore_wayland_inhibit");

    const auto        RULES = g_pConfigManager->getRules();
    m_sWaylandIdleState.listeners.resize(RULES.size());

    Debug::log(LOG, "found {} rules", RULES.size());

    for (size_t i = 0; i < RULES.size(); ++i) {
        auto&       l   = m_sWaylandIdleState.listeners[i];
        const auto& r   = RULES[i];
        l.onRestore     = r.onResume;
        l.onTimeout     = r.onTimeout;
        l.ignoreInhibit = r.ignoreInhibit;

        if (*IGNOREWAYLANDINHIBIT || r.ignoreInhibit)
            l.notification =
                makeShared<CCExtIdleNotificationV1>(m_sWaylandIdleState.notifier->sendGetInputIdleNotification(r.timeout * 1000 /* ms */, m_sWaylandState.seat->resource()));
        else
            l.notification =
                makeShared<CCExtIdleNotificationV1>(m_sWaylandIdleState.notifier->sendGetIdleNotification(r.timeout * 1000 /* ms */, m_sWaylandState.seat->resource()));

        l.notification->setData(&m_sWaylandIdleState.listeners[i]);

        l.notification->setIdled([this](CCExtIdleNotificationV1* n) { onIdled((CNiriidle::SIdleListener*)n->data()); });
        l.notification->setResumed([this](CCExtIdleNotificationV1* n) { onResumed((CNiriidle::SIdleListener*)n->data()); });
    }

    wl_display_roundtrip(m_sWaylandState.display);

    if (m_sWaylandState.lockNotifier) {
        m_sWaylandState.lockNotification = makeShared<CCHyprlandLockNotificationV1>(m_sWaylandState.lockNotifier->sendGetLockNotification());
        m_sWaylandState.lockNotification->setLocked([this](CCHyprlandLockNotificationV1* n) { onLocked(); });
        m_sWaylandState.lockNotification->setUnlocked([this](CCHyprlandLockNotificationV1* n) { onUnlocked(); });
    }

    Debug::log(LOG, "wayland done, registering dbus");

    try {
        m_sDBUSState.connection = sdbus::createSystemBusConnection();
    } catch (std::exception& e) {
        Debug::log(CRIT, "Couldn't create the dbus connection ({})", e.what());
        exit(1);
    }

    if (!m_sWaylandState.lockNotifier)
        Debug::log(WARN,
                   "Compositor is missing hyprland-lock-notify-v1 (niri/sway/...)\n"
                   "Lock state will be tracked via D-Bus loginctl signals instead.");

    static const auto INHIBIT  = g_pConfigManager->getValue<Hyprlang::INT>("general:inhibit_sleep");
    static const auto SLEEPCMD = g_pConfigManager->getValue<Hyprlang::STRING>("general:before_sleep_cmd");
    static const auto LOCKCMD  = g_pConfigManager->getValue<Hyprlang::STRING>("general:lock_cmd");

    switch (*INHIBIT) {
        case 0: // disabled
            m_inhibitSleepBehavior = SLEEP_INHIBIT_NONE;
            break;
        case 1: // enabled
            m_inhibitSleepBehavior = SLEEP_INHIBIT_NORMAL;
            break;
        case 2: { // auto (enable, but wait until locked if before_sleep_cmd contains hyprlock, or loginctl lock-session and lock_cmd contains hyprlock.)
            if (m_sWaylandState.lockNotifier && std::string{*SLEEPCMD}.contains("hyprlock"))
                m_inhibitSleepBehavior = SLEEP_INHIBIT_LOCK_NOTIFY;
            else if (m_sWaylandState.lockNotifier && std::string{*LOCKCMD}.contains("hyprlock") && std::string{*SLEEPCMD}.contains("lock-session"))
                m_inhibitSleepBehavior = SLEEP_INHIBIT_LOCK_NOTIFY;
            else
                m_inhibitSleepBehavior = SLEEP_INHIBIT_NORMAL;
        } break;
        case 3: // wait until locked
            if (m_sWaylandState.lockNotifier)
                m_inhibitSleepBehavior = SLEEP_INHIBIT_LOCK_NOTIFY;
            break;
        default: Debug::log(ERR, "Invalid inhibit_sleep value: {}", *INHIBIT); break;
    }

    switch (m_inhibitSleepBehavior) {
        case SLEEP_INHIBIT_NONE: Debug::log(LOG, "Sleep inhibition disabled"); break;
        case SLEEP_INHIBIT_NORMAL: Debug::log(LOG, "Sleep inhibition enabled"); break;
        case SLEEP_INHIBIT_LOCK_NOTIFY: Debug::log(LOG, "Sleep inhibition enabled - inhibiting until the wayland session gets locked"); break;
    }

    setupDBUS();
    setupLidDBUS();
    setupPowerDBUS();
    setupMediaDBUS();
    if (m_inhibitSleepBehavior != SLEEP_INHIBIT_NONE)
        inhibitSleep();

    // Initial state machine setup
    updateStateMachine();

    enterEventLoop();
}

// ── Event Loop ─────────────────────────────────────────────────────────────

void CNiriidle::enterEventLoop() {

    int sessionConnFd = m_sDBUSState.sessionConnection ? m_sDBUSState.sessionConnection->getEventLoopPollData().fd : -1;
    int ssConnFd      = m_sDBUSState.screenSaverServiceConnection ? m_sDBUSState.screenSaverServiceConnection->getEventLoopPollData().fd : -1;
    int acpiLidFd     = m_iAcpiLidFd;

    // Fixed slots: [0]=dbus [1]=wl [2]=screensaver [3]=session [4]=acpi-lid
    // poll() ignores entries with fd=-1 (sets revents=0), so always pass all 5.
    constexpr nfds_t pollfdsCount = 5;
    pollfd pollfds[5] = {
        { .fd = m_sDBUSState.connection->getEventLoopPollData().fd, .events = POLLIN },
        { .fd = wl_display_get_fd(m_sWaylandState.display),         .events = POLLIN },
        { .fd = ssConnFd,                                            .events = POLLIN },
        { .fd = sessionConnFd,                                       .events = POLLIN },
        { .fd = acpiLidFd,                                           .events = POLLIN },
    };

    std::thread pollThr([this, &pollfds, ssConnFd, sessionConnFd, acpiLidFd]() {
        while (1) {
            int ret = poll(pollfds, pollfdsCount, 5000 /* 5 seconds, reasonable. It's because we might need to terminate */);
            if (ret < 0) {
                Debug::log(CRIT, "[core] Polling fds failed with {}", errno);
                m_bTerminate = true;
                exit(1);
            }

            for (size_t i = 0; i < pollfdsCount; ++i) {
                if (pollfds[i].revents & POLLHUP) {
                    Debug::log(CRIT, "[core] Disconnected from pollfd id {}", i);
                    m_bTerminate = true;
                    exit(1);
                }
            }

            if (m_bTerminate)
                break;

            if (ret != 0) {
                Debug::log(TRACE, "[core] got poll event");
                std::lock_guard<std::mutex> lg(m_sEventLoopInternals.loopRequestMutex);
                m_sEventLoopInternals.shouldProcess = true;
                m_sEventLoopInternals.loopSignal.notify_all();
            } else {
                // timeout: check hibernate condition periodically
                if (m_sSystemPowerState == ESystemPowerState::SUSPEND && !m_bHibernateChecked)
                    checkHibernateCondition();
            }
        }
    });

    while (1) { // dbus events
        // wait for being awakened
        m_sEventLoopInternals.loopRequestMutex.unlock(); // unlock, we are ready to take events

        std::unique_lock lk(m_sEventLoopInternals.loopMutex);
        if (!m_sEventLoopInternals.shouldProcess) // avoid a lock if a thread managed to request something already since we .unlock()ed
            m_sEventLoopInternals.loopSignal.wait(lk, [this] { return m_sEventLoopInternals.shouldProcess == true; }); // wait for events

        m_sEventLoopInternals.loopRequestMutex.lock(); // lock incoming events

        if (m_bTerminate)
            break;

        m_sEventLoopInternals.shouldProcess = false;

        std::lock_guard<std::mutex> lg(m_sEventLoopInternals.eventLock);

        if (pollfds[0].revents & POLLIN /* dbus */) {
            Debug::log(TRACE, "got dbus event");
            while (m_sDBUSState.connection->processPendingEvent()) {
                ;
            }
        }

        if (pollfds[1].revents & POLLIN /* wl */) {
            Debug::log(TRACE, "got wl event");
            wl_display_flush(m_sWaylandState.display);
            if (wl_display_prepare_read(m_sWaylandState.display) == 0) {
                wl_display_read_events(m_sWaylandState.display);
                wl_display_dispatch_pending(m_sWaylandState.display);
            } else {
                wl_display_dispatch(m_sWaylandState.display);
            }
        }

        if (ssConnFd >= 0 && (pollfds[2].revents & POLLIN) /* dbus2 */) {
            Debug::log(TRACE, "got screensaver dbus event");
            while (m_sDBUSState.screenSaverServiceConnection->processPendingEvent()) {
                ;
            }
        }

        if (sessionConnFd >= 0 && (pollfds[3].revents & POLLIN) /* session dbus (MPRIS) */) {
            Debug::log(TRACE, "got session dbus event");
            while (m_sDBUSState.sessionConnection->processPendingEvent()) {
                ;
            }
        }

        if (acpiLidFd >= 0 && (pollfds[4].revents & POLLIN) /* kernel uevent: ACPI lid + power supply */) {
            Debug::log(TRACE, "got kernel uevent");
            processKernelUevent();
        }

        // finalize wayland dispatching. Dispatch pending on the queue
        int ret = 0;
        do {
            ret = wl_display_dispatch_pending(m_sWaylandState.display);
            wl_display_flush(m_sWaylandState.display);
        } while (ret > 0);
    }

    Debug::log(ERR, "[core] Terminated");
}

// ── spawn ──────────────────────────────────────────────────────────────────

static void spawn(const std::string& args) {
    Debug::log(LOG, "Executing {}", args);

    Hyprutils::OS::CProcess proc("/bin/sh", {"-c", args});
    if (!proc.runAsync()) {
        Debug::log(ERR, "Failed run \"{}\"", args);
        return;
    }

    Debug::log(LOG, "Process Created with pid {}", proc.pid());
}

// ── onIdled / onResumed ────────────────────────────────────────────────────

void CNiriidle::onIdled(SIdleListener* pListener) {
    Debug::log(LOG, "Idled: rule {:x}", (uintptr_t)pListener);
    isIdled = true;
    if (g_pNiriidle->m_iInhibitLocks > 0 && !pListener->ignoreInhibit) {
        Debug::log(LOG, "Ignoring from onIdled(), inhibit locks: {}", g_pNiriidle->m_iInhibitLocks);
        return;
    }

    if (pListener->onTimeout.empty()) {
        Debug::log(LOG, "Ignoring, onTimeout is empty.");
        return;
    }

    Debug::log(LOG, "Running {}", pListener->onTimeout);
    pListener->onTimeoutFired = true;
    spawn(pListener->onTimeout);
}

void CNiriidle::onResumed(SIdleListener* pListener) {
    Debug::log(LOG, "Resumed: rule {:x}", (uintptr_t)pListener);
    isIdled = false;

    // If on-timeout never actually executed (was inhibited), skip on-resume too
    if (!pListener->onTimeoutFired) {
        Debug::log(LOG, "Skipping onResumed: onTimeout was inhibited for rule {:x}", (uintptr_t)pListener);
        return;
    }

    pListener->onTimeoutFired = false;

    if (pListener->onRestore.empty()) {
        Debug::log(LOG, "Ignoring, onRestore is empty.");
        return;
    }

    Debug::log(LOG, "Running {}", pListener->onRestore);
    spawn(pListener->onRestore);
}

// ── onInhibit ──────────────────────────────────────────────────────────────

void CNiriidle::onInhibit(bool lock) {
    m_iInhibitLocks += lock ? 1 : -1;

    if (m_iInhibitLocks < 0) {
        Debug::log(WARN, "BUG THIS: inhibit locks < 0: {}", m_iInhibitLocks);
        m_iInhibitLocks = 0;
    }

    if (m_iInhibitLocks == 0 && isIdled) {
        static const auto IGNOREWAYLANDINHIBIT = g_pConfigManager->getValue<Hyprlang::INT>("general:ignore_wayland_inhibit");

        const auto        RULES = g_pConfigManager->getRules();
        for (size_t i = 0; i < RULES.size(); ++i) {
            auto&       l = m_sWaylandIdleState.listeners[i];
            const auto& r = RULES[i];

            l.notification->sendDestroy();

            if (*IGNOREWAYLANDINHIBIT || r.ignoreInhibit)
                l.notification =
                    makeShared<CCExtIdleNotificationV1>(m_sWaylandIdleState.notifier->sendGetInputIdleNotification(r.timeout * 1000 /* ms */, m_sWaylandState.seat->resource()));
            else
                l.notification =
                    makeShared<CCExtIdleNotificationV1>(m_sWaylandIdleState.notifier->sendGetIdleNotification(r.timeout * 1000 /* ms */, m_sWaylandState.seat->resource()));

            l.notification->setData(&m_sWaylandIdleState.listeners[i]);

            l.notification->setIdled([this](CCExtIdleNotificationV1* n) { onIdled((CNiriidle::SIdleListener*)n->data()); });
            l.notification->setResumed([this](CCExtIdleNotificationV1* n) { onResumed((CNiriidle::SIdleListener*)n->data()); });
        }
    }

    Debug::log(LOG, "Inhibit locks: {}", m_iInhibitLocks);
}

// ── Lock / Unlock ──────────────────────────────────────────────────────────

void CNiriidle::onLocked() {
    if (m_isLocked)
        return;

    Debug::log(LOG, "Wayland session got locked");
    m_isLocked = true;

    static const auto LOCKCMD = g_pConfigManager->getValue<Hyprlang::STRING>("general:on_lock_cmd");
    if (!std::string{*LOCKCMD}.empty())
        spawn(*LOCKCMD);

    if (m_inhibitSleepBehavior == SLEEP_INHIBIT_LOCK_NOTIFY)
        uninhibitSleep();

    updateStateMachine();
}

void CNiriidle::onUnlocked() {
    if (!m_isLocked)
        return;

    Debug::log(LOG, "Wayland session got unlocked");
    m_isLocked = false;

    if (m_inhibitSleepBehavior == SLEEP_INHIBIT_LOCK_NOTIFY)
        inhibitSleep();

    static const auto UNLOCKCMD = g_pConfigManager->getValue<Hyprlang::STRING>("general:on_unlock_cmd");
    if (!std::string{*UNLOCKCMD}.empty())
        spawn(*UNLOCKCMD);

    m_bLastWakeFromLid = false;
    updateStateMachine();
}

// ── Lid Events (Group A) ───────────────────────────────────────────────────

void CNiriidle::onLidEvent(bool closed) {
    static const auto DEBOUNCE_MS = g_pConfigManager->getValue<Hyprlang::INT>("general:debounce_timeout");

    auto now = std::chrono::steady_clock::now();

    // Debounce: ignore events within debounce_timeout of the last lid event
    if (m_bLidDebouncePending) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_tLastLidEvent).count();
        if (elapsed < *DEBOUNCE_MS) {
            Debug::log(LOG, "Lid event debounced ({}ms < {}ms)", elapsed, *DEBOUNCE_MS);
            return;
        }
    }

    m_tLastLidEvent     = now;
    m_bLidDebouncePending = true;

    // Schedule debounce reset
    auto debounceMs = *DEBOUNCE_MS;
    std::thread([this, debounceMs]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(debounceMs));
        m_bLidDebouncePending = false;
    }).detach();

    if (closed) {
        Debug::log(LOG, "Lid closed");
        m_sLidState = ELidState::CLOSED;
        handleLidCloseAction();
    } else {
        Debug::log(LOG, "Lid opened");
        m_sLidState        = ELidState::OPEN;
        m_bLastWakeFromLid = true;

        // Reset idle timers and transition to appropriate state
        isIdled = false;

        static const auto LIDOPEN_RESET = g_pConfigManager->getValue<Hyprlang::INT>("general:lid_open_reset_idle");
        if (*LIDOPEN_RESET) {
            updateStateMachine(); // handles locked state via handleLockedIdle(!m_bLastWakeFromLid)
        } else if (m_isLocked) {
            handleLockedIdle(false); // wakeFromInput=false (lid open) → longer locked_idle_* timeout
        }
    }
}

void CNiriidle::handleLidCloseAction() {
    static const auto LID_CLOSE_ACTION        = g_pConfigManager->getValue<Hyprlang::STRING>("general:lid_close_action");
    static const auto LID_CLOSE_BATTERY_ACTION = g_pConfigManager->getValue<Hyprlang::STRING>("general:lid_close_battery_action");
    static const auto LID_CLOSE_MEDIA_PLAYING  = g_pConfigManager->getValue<Hyprlang::STRING>("general:lid_close_media_playing");
    static const auto LOCKCMD                 = g_pConfigManager->getValue<Hyprlang::STRING>("general:lock_cmd");
    static const auto SLEEPCMD                = g_pConfigManager->getValue<Hyprlang::STRING>("general:before_sleep_cmd");

    // Pick the right action based on power source
    std::string actionStr = m_sPowerSource == EPowerSource::BATTERY ? *LID_CLOSE_BATTERY_ACTION : *LID_CLOSE_ACTION;
    auto        action    = parseLidCloseAction(actionStr);

    // Check media override: if media is playing and config says lock_only
    if (m_sMediaState == EMediaState::PLAYING && std::string{*LID_CLOSE_MEDIA_PLAYING} == "lock_only") {
        Debug::log(LOG, "Media playing, lid close action overridden to lock_only");
        action = ELidCloseAction::LOCK_ONLY;
    }

    Debug::log(LOG, "Lid close action: {}", actionStr);

    switch (action) {
        case ELidCloseAction::LOCK_SUSPEND:
            if (!std::string{*LOCKCMD}.empty())
                spawn(*LOCKCMD);
            if (!std::string{*SLEEPCMD}.empty())
                spawn(*SLEEPCMD);
            break;
        case ELidCloseAction::LOCK_ONLY:
            if (!std::string{*LOCKCMD}.empty())
                spawn(*LOCKCMD);
            break;
        case ELidCloseAction::SUSPEND_ONLY:
            if (!std::string{*SLEEPCMD}.empty())
                spawn(*SLEEPCMD);
            break;
        case ELidCloseAction::NOTHING:
            break;
    }
}

// ── Power Source Events ────────────────────────────────────────────────────

void CNiriidle::onPowerSourceEvent(bool onBattery) {
    Debug::log(LOG, "Power source changed: {}", onBattery ? "BATTERY" : "AC");
    m_sPowerSource = onBattery ? EPowerSource::BATTERY : EPowerSource::AC;

    // Edge Case 3: If idle time already exceeds new timeout, react immediately
    // Recreate listeners with new timeouts; Wayland idle protocol will report
    // idle if the elapsed idle exceeds the new timeout
    updateStateMachine();
}

// ── Media Events ───────────────────────────────────────────────────────────

void CNiriidle::onMediaEvent(bool playing) {
    Debug::log(LOG, "Media state changed: {}", playing ? "PLAYING" : "IDLE");
    m_sMediaState = playing ? EMediaState::PLAYING : EMediaState::IDLE;

    updateStateMachine();
}

// ── Wake Trigger ───────────────────────────────────────────────────────────

void CNiriidle::onWakeTrigger(bool fromLid) {
    Debug::log(LOG, "Wake trigger: {}", fromLid ? "LID" : "INPUT");
    m_bLastWakeFromLid = fromLid;

    if (m_isLocked) {
        handleLockedIdle(!fromLid); // fromLid=true → wakeFromInput=false → longer timeout
    }
}

// ── Locked Idle Handling (Group C) ─────────────────────────────────────────

void CNiriidle::handleLockedIdle(bool wakeFromInput) {
    Debug::log(LOG, "Handling locked idle, wakeFromInput={}", wakeFromInput);

    // When locked, create ultra-short timer group that will:
    // 1. Turn off screen after short timeout
    // 2. Suspend after another short timeout (on battery)

    static const auto LOCKED_IDLE_BATTERY   = g_pConfigManager->getValue<Hyprlang::INT>("general:locked_idle_battery");
    static const auto LOCKED_SUSPEND_BATTERY = g_pConfigManager->getValue<Hyprlang::INT>("general:locked_suspend_battery");
    static const auto LOCKED_IDLE_AC         = g_pConfigManager->getValue<Hyprlang::INT>("general:locked_idle_ac");
    static const auto LOCKED_INPUT_BATTERY   = g_pConfigManager->getValue<Hyprlang::INT>("general:locked_input_battery");
    static const auto LOCKED_INPUT_AC        = g_pConfigManager->getValue<Hyprlang::INT>("general:locked_input_ac");
    static const auto DPMSOFF                = g_pConfigManager->getValue<Hyprlang::STRING>("general:dpms_off_cmd");
    static const auto DPMSON                 = g_pConfigManager->getValue<Hyprlang::STRING>("general:dpms_on_cmd");
    static const auto SLEEPCMD               = g_pConfigManager->getValue<Hyprlang::STRING>("general:before_sleep_cmd");

    if (!m_sWaylandIdleState.notifier || !m_sWaylandState.seat)
        return;

    static const auto IGNOREWAYLANDINHIBIT = g_pConfigManager->getValue<Hyprlang::INT>("general:ignore_wayland_inhibit");

    bool onBattery = m_sPowerSource == EPowerSource::BATTERY;

    std::vector<std::pair<uint64_t, std::pair<std::string, std::string>>> timeouts;

    // Determine screen-off timeout based on wake trigger
    uint64_t screenOffTimeout;
    if (wakeFromInput) {
        screenOffTimeout = onBattery ? (uint64_t)*LOCKED_INPUT_BATTERY : (uint64_t)*LOCKED_INPUT_AC;
    } else {
        screenOffTimeout = onBattery ? (uint64_t)*LOCKED_IDLE_BATTERY : (uint64_t)*LOCKED_IDLE_AC;
    }

    // Screen off timeout
    if (!std::string{*DPMSOFF}.empty())
        timeouts.push_back({screenOffTimeout, {*DPMSOFF, *DPMSON}});

    // Suspend timeout (battery only) — additive on top of screenOffTimeout
    if (onBattery && *LOCKED_SUSPEND_BATTERY > 0 && !std::string{*SLEEPCMD}.empty()) {
        uint64_t suspendTimeout = screenOffTimeout + (uint64_t)*LOCKED_SUSPEND_BATTERY;
        timeouts.push_back({suspendTimeout, {*SLEEPCMD, ""}});
    }

    // ignoreInhibit=true: locked state must turn off screen regardless of media players or inhibitors
    m_sLockedGroup.create(m_sWaylandIdleState.notifier, m_sWaylandState.seat->resource(), *IGNOREWAYLANDINHIBIT, timeouts, true);

    for (auto& l : m_sLockedGroup.listeners) {
        l.notification->setData(&l);
        l.notification->setIdled([this](CCExtIdleNotificationV1* n) { onIdled((CNiriidle::SIdleListener*)n->data()); });
        l.notification->setResumed([this](CCExtIdleNotificationV1* n) { onResumed((CNiriidle::SIdleListener*)n->data()); });
    }
}

// ── Suspend-to-Hibernate (Group D) ─────────────────────────────────────────

void CNiriidle::scheduleHibernateCheck() {
    m_tSuspendStart      = std::chrono::steady_clock::now();
    m_bHibernateChecked  = false;
    m_sSystemPowerState  = ESystemPowerState::SUSPEND;
    Debug::log(LOG, "Scheduled hibernate check");
}

void CNiriidle::checkHibernateCondition() {
    static const auto HIBERNATE_BATTERY_PCT        = g_pConfigManager->getValue<Hyprlang::INT>("general:hibernate_battery_pct");
    static const auto SUSPEND_TO_HIBERNATE_TIMEOUT = g_pConfigManager->getValue<Hyprlang::INT>("general:suspend_to_hibernate_timeout");

    // Edge Case 4: AC connected → skip hibernate
    if (m_sPowerSource == EPowerSource::AC) {
        Debug::log(LOG, "AC connected, skipping hibernate check");
        m_bHibernateChecked = true;
        return;
    }

    auto now             = std::chrono::steady_clock::now();
    auto suspendDuration = std::chrono::duration_cast<std::chrono::seconds>(now - m_tSuspendStart).count();

    // Check if suspend has been too long
    if (suspendDuration >= *SUSPEND_TO_HIBERNATE_TIMEOUT) {
        Debug::log(LOG, "Suspend too long ({}s >= {}s), hibernating", suspendDuration, *SUSPEND_TO_HIBERNATE_TIMEOUT);
        m_sSystemPowerState = ESystemPowerState::HIBERNATING;
        spawn("systemctl hibernate");
        m_bHibernateChecked = true;
        return;
    }

    // Check battery level via UPower
    if (m_sDBUSState.upowerProxy) {
        try {
            sdbus::ObjectPath displayDevicePath;
            m_sDBUSState.upowerProxy->callMethod("GetDisplayDevice").onInterface("org.freedesktop.UPower").storeResultsTo(displayDevicePath);

            auto deviceProxy = sdbus::createProxy(*m_sDBUSState.connection, sdbus::ServiceName{"org.freedesktop.UPower"}, displayDevicePath);
            double percentage = deviceProxy->getProperty("Percentage").onInterface("org.freedesktop.UPower.Device").get<double>();

            if (percentage <= *HIBERNATE_BATTERY_PCT) {
                Debug::log(LOG, "Battery critically low ({:.1f}% <= {}%), hibernating", percentage, *HIBERNATE_BATTERY_PCT);
                m_sSystemPowerState = ESystemPowerState::HIBERNATING;
                spawn("systemctl hibernate");
            }
        } catch (std::exception& e) {
            Debug::log(WARN, "Failed to check battery level: {}", e.what());
        }
    }

    m_bHibernateChecked = true;
}

// ── State Machine ──────────────────────────────────────────────────────────

void CNiriidle::updateStateMachine() {
    Debug::log(LOG, "State Machine Update: Lid={} Power={} Lock={} Media={}",
               m_sLidState == ELidState::OPEN ? "OPEN" : "CLOSED",
               m_sPowerSource == EPowerSource::AC ? "AC" : "BATTERY",
               m_isLocked ? "LOCKED" : "UNLOCKED",
               m_sMediaState == EMediaState::PLAYING ? "PLAYING" : "IDLE");

    // If lid is closed and system is going to sleep, don't set up idle listeners
    if (m_sLidState == ELidState::CLOSED && m_sSystemPowerState != ESystemPowerState::ACTIVE)
        return;

    // Destroy any existing dynamic groups
    m_sUnlockedGroup.destroy();
    m_sLockedGroup.destroy();

    if (!m_sWaylandIdleState.notifier || !m_sWaylandState.seat)
        return;

    static const auto IGNOREWAYLANDINHIBIT = g_pConfigManager->getValue<Hyprlang::INT>("general:ignore_wayland_inhibit");

    if (m_isLocked) {
        // ── Group C: Locked Idle ───────────────────────────────────────────
        handleLockedIdle(!m_bLastWakeFromLid); // lid open → wakeFromInput=false → longer timeout
    } else {
        // ── Group B: Unlocked Idle ─────────────────────────────────────────
        static const auto UNLOCKED_IDLE_BATTERY = g_pConfigManager->getValue<Hyprlang::INT>("general:unlocked_idle_battery");
        static const auto UNLOCKED_IDLE_AC      = g_pConfigManager->getValue<Hyprlang::INT>("general:unlocked_idle_ac");
        static const auto LOCKCMD               = g_pConfigManager->getValue<Hyprlang::STRING>("general:lock_cmd");
        static const auto SLEEPCMD              = g_pConfigManager->getValue<Hyprlang::STRING>("general:before_sleep_cmd");
        static const auto DPMSOFF               = g_pConfigManager->getValue<Hyprlang::STRING>("general:dpms_off_cmd");
        static const auto DPMSON                = g_pConfigManager->getValue<Hyprlang::STRING>("general:dpms_on_cmd");

        bool     onBattery = m_sPowerSource == EPowerSource::BATTERY;
        uint64_t baseTimeout = onBattery ? (uint64_t)*UNLOCKED_IDLE_BATTERY : (uint64_t)*UNLOCKED_IDLE_AC;

        std::vector<std::pair<uint64_t, std::pair<std::string, std::string>>> timeouts;

        if (m_sMediaState == EMediaState::PLAYING) {
            // Media playing: inhibit lock/suspend, only allow screen off after long timeout
            uint64_t mediaTimeout = onBattery ? 1800U : (baseTimeout * 2);
            mediaTimeout          = std::max(mediaTimeout, (uint64_t)900U);
            if (!std::string{*DPMSOFF}.empty())
                timeouts.push_back({mediaTimeout, {*DPMSOFF, *DPMSON}});
        } else {
            // Normal idle: lock → screen off → suspend
            if (!std::string{*LOCKCMD}.empty())
                timeouts.push_back({baseTimeout, {*LOCKCMD, ""}});
            if (!std::string{*DPMSOFF}.empty())
                timeouts.push_back({baseTimeout + 30, {*DPMSOFF, *DPMSON}});
            if (onBattery && !std::string{*SLEEPCMD}.empty())
                timeouts.push_back({baseTimeout + 60, {*SLEEPCMD, ""}});
        }

        m_sUnlockedGroup.create(m_sWaylandIdleState.notifier, m_sWaylandState.seat->resource(), *IGNOREWAYLANDINHIBIT, timeouts, false);

        for (auto& l : m_sUnlockedGroup.listeners) {
            l.notification->setData(&l);
            l.notification->setIdled([this](CCExtIdleNotificationV1* n) { onIdled((CNiriidle::SIdleListener*)n->data()); });
            l.notification->setResumed([this](CCExtIdleNotificationV1* n) { onResumed((CNiriidle::SIdleListener*)n->data()); });
        }
    }
}

void CNiriidle::recreateIdleListeners() {
    // Force recreation of all idle listeners based on current state
    updateStateMachine();
}

// ── D-Bus Helpers ──────────────────────────────────────────────────────────

CNiriidle::SDbusInhibitCookie CNiriidle::getDbusInhibitCookie(uint32_t cookie) {
    for (auto& c : m_sDBUSState.inhibitCookies) {
        if (c.cookie == cookie)
            return c;
    }

    return {};
}

void CNiriidle::registerDbusInhibitCookie(CNiriidle::SDbusInhibitCookie& cookie) {
    m_sDBUSState.inhibitCookies.push_back(cookie);
}

bool CNiriidle::unregisterDbusInhibitCookie(const CNiriidle::SDbusInhibitCookie& cookie) {
    const auto IT = std::ranges::find_if(m_sDBUSState.inhibitCookies, [&cookie](const CNiriidle::SDbusInhibitCookie& item) { return item.cookie == cookie.cookie; });

    if (IT == m_sDBUSState.inhibitCookies.end())
        return false;

    m_sDBUSState.inhibitCookies.erase(IT);
    return true;
}

size_t CNiriidle::unregisterDbusInhibitCookies(const std::string& ownerID) {
    return std::erase_if(m_sDBUSState.inhibitCookies, [&ownerID](const CNiriidle::SDbusInhibitCookie& item) { return item.ownerID == ownerID; });
}

// ── D-Bus Handlers ─────────────────────────────────────────────────────────

static void handleDbusLogin(sdbus::Message msg) {
    // lock & unlock
    static const auto LOCKCMD   = g_pConfigManager->getValue<Hyprlang::STRING>("general:lock_cmd");
    static const auto UNLOCKCMD = g_pConfigManager->getValue<Hyprlang::STRING>("general:unlock_cmd");

    Debug::log(LOG, "Got dbus .Session");

    const std::string MEMBER = msg.getMemberName();
    if (MEMBER == "Lock") {
        Debug::log(LOG, "Got Lock from dbus");

        if (!std::string{*LOCKCMD}.empty()) {
            Debug::log(LOG, "Locking with {}", *LOCKCMD);
            spawn(*LOCKCMD);
        }

        // Fallback for compositors without hyprland-lock-notify-v1 (e.g. niri, sway)
        g_pNiriidle->onLocked();
    } else if (MEMBER == "Unlock") {
        Debug::log(LOG, "Got Unlock from dbus");

        if (!std::string{*UNLOCKCMD}.empty()) {
            Debug::log(LOG, "Unlocking with {}", *UNLOCKCMD);
            spawn(*UNLOCKCMD);
        }

        // Fallback for compositors without hyprland-lock-notify-v1 (e.g. niri, sway)
        g_pNiriidle->onUnlocked();
    }
}

static void handleDbusSleep(sdbus::Message msg) {
    const std::string MEMBER = msg.getMemberName();

    if (MEMBER != "PrepareForSleep")
        return;

    bool toSleep = true;
    msg >> toSleep;

    static const auto SLEEPCMD      = g_pConfigManager->getValue<Hyprlang::STRING>("general:before_sleep_cmd");
    static const auto AFTERSLEEPCMD = g_pConfigManager->getValue<Hyprlang::STRING>("general:after_sleep_cmd");

    Debug::log(LOG, "Got PrepareForSleep from dbus with sleep {}", toSleep);

    std::string cmd = toSleep ? *SLEEPCMD : *AFTERSLEEPCMD;

    if (!toSleep) {
        g_pNiriidle->handleInhibitOnDbusSleep(toSleep);
        g_pNiriidle->setSystemPowerState(CNiriidle::ESystemPowerState::ACTIVE);
        // re-establish listeners after wake; lid event may fire late or not at all
        g_pNiriidle->updateStateMachine();
    }

    if (!cmd.empty())
        spawn(cmd);

    if (toSleep) {
        g_pNiriidle->handleInhibitOnDbusSleep(toSleep);
        g_pNiriidle->scheduleHibernateCheck();
    }
}

static void handleDbusBlockInhibits(const std::string& inhibits) {
    static auto inhibited = false;
    // BlockInhibited is a colon separated list of inhibit types. Wrapping in additional colons allows for easier checking if there are active inhibits we are interested in
    auto inhibits_ = ":" + inhibits + ":";
    if (inhibits_.contains(":idle:")) {
        if (!inhibited) {
            inhibited = true;
            Debug::log(LOG, "systemd idle inhibit active");
            g_pNiriidle->onInhibit(true);
        }
    } else if (inhibited) {
        inhibited = false;
        Debug::log(LOG, "systemd idle inhibit inactive");
        g_pNiriidle->onInhibit(false);
    }
}

static void handleDbusBlockInhibitsPropertyChanged(sdbus::Message msg) {
    std::string                           interface;
    std::map<std::string, sdbus::Variant> changedProperties;
    msg >> interface >> changedProperties;
    if (changedProperties.contains("BlockInhibited")) {
        handleDbusBlockInhibits(changedProperties["BlockInhibited"].get<std::string>());
    }
}

static uint32_t handleDbusScreensaver(std::string app, std::string reason, uint32_t cookie, bool inhibit, const char* sender) {
    std::string ownerID = sender;
    bool cookieFound = false;

    if (!inhibit) {
        Debug::log(TRACE, "Read uninhibit cookie: {}", cookie);
        const auto COOKIE = g_pNiriidle->getDbusInhibitCookie(cookie);
        if (COOKIE.cookie == 0) {
            Debug::log(WARN, "No cookie in uninhibit");
        } else {
            app     = COOKIE.app;
            reason  = COOKIE.reason;
            ownerID = COOKIE.ownerID;
            cookieFound = true;

            if (!g_pNiriidle->unregisterDbusInhibitCookie(COOKIE))
                Debug::log(WARN, "BUG THIS: attempted to unregister unknown cookie");
        }
    }

    Debug::log(LOG, "ScreenSaver inhibit: {} dbus message from {} (owner: {}) with content {}", inhibit, app, ownerID, reason);

    if (inhibit)
        g_pNiriidle->onInhibit(true);
    else if (cookieFound)
        g_pNiriidle->onInhibit(false);

    static uint32_t cookieID = 1337;

    if (inhibit) {
        auto cookie = CNiriidle::SDbusInhibitCookie{.cookie = cookieID, .app = app, .reason = reason, .ownerID = ownerID};

        Debug::log(LOG, "Cookie {} sent", cookieID);

        g_pNiriidle->registerDbusInhibitCookie(cookie);

        return cookieID++;
    }

    return 0;
}

static void handleDbusNameOwnerChanged(sdbus::Message msg) {
    std::string name, oldOwner, newOwner;
    msg >> name >> oldOwner >> newOwner;

    if (!newOwner.empty())
        return;

    size_t removed = g_pNiriidle->unregisterDbusInhibitCookies(oldOwner);
    if (removed > 0) {
        Debug::log(LOG, "App with owner {} disconnected", oldOwner);
        for (size_t i = 0; i < removed; i++)
            g_pNiriidle->onInhibit(false);
    }
}

// ── Lid D-Bus Handler ──────────────────────────────────────────────────────

static void handleDbusLidPropertiesChanged(sdbus::Message msg) {
    std::string                           interface;
    std::map<std::string, sdbus::Variant> changedProperties;
    msg >> interface >> changedProperties;

    if (changedProperties.contains("LidClosed")) {
        bool closed = changedProperties["LidClosed"].get<bool>();
        Debug::log(LOG, "Lid state changed: {}", closed ? "CLOSED" : "OPEN");
        g_pNiriidle->onLidEvent(closed);
    }
}

// ── Power D-Bus Handler ────────────────────────────────────────────────────

static void handleDbusPowerPropertiesChanged(sdbus::Message msg) {
    std::string                           interface;
    std::map<std::string, sdbus::Variant> changedProperties;
    msg >> interface >> changedProperties;

    if (changedProperties.contains("OnBattery")) {
        bool onBattery = changedProperties["OnBattery"].get<bool>();
        Debug::log(LOG, "Power source changed via UPower: {}", onBattery ? "BATTERY" : "AC");
        g_pNiriidle->onPowerSourceEvent(onBattery);
    }
}

// ── MPRIS D-Bus Handler ────────────────────────────────────────────────────

static bool isAnyMprisPlaying() {
    // Query all known MPRIS players for their PlaybackStatus via session bus
    try {
        auto sessionBus = sdbus::createSessionBusConnection();
        auto dbusProxy  = sdbus::createProxy(*sessionBus,
                                              sdbus::ServiceName{"org.freedesktop.DBus"},
                                              sdbus::ObjectPath{"/org/freedesktop/DBus"});

        std::vector<std::string> names;
        dbusProxy->callMethod("ListNames").onInterface("org.freedesktop.DBus").storeResultsTo(names);

        bool anyPlaying = false;
        for (const auto& name : names) {
            if (name.find("org.mpris.MediaPlayer2.") != 0)
                continue;

            try {
                auto playerProxy = sdbus::createProxy(*sessionBus, sdbus::ServiceName{name}, sdbus::ObjectPath{"/org/mpris/MediaPlayer2"});
                std::string status = playerProxy->getProperty("PlaybackStatus").onInterface("org.mpris.MediaPlayer2.Player").get<std::string>();
                if (status == "Playing") {
                    anyPlaying = true;
                    break;
                }
            } catch (...) {
                // Player may have disappeared
            }
        }
        return anyPlaying;
    } catch (...) {
        return false;
    }
}



// ── setupDBUS ──────────────────────────────────────────────────────────────

void CNiriidle::setupDBUS() {
    static const auto IGNOREDBUSINHIBIT    = g_pConfigManager->getValue<Hyprlang::INT>("general:ignore_dbus_inhibit");
    static const auto IGNORESYSTEMDINHIBIT = g_pConfigManager->getValue<Hyprlang::INT>("general:ignore_systemd_inhibit");

    auto              systemConnection = sdbus::createSystemBusConnection();
    auto              proxy            = sdbus::createProxy(*systemConnection, sdbus::ServiceName{"org.freedesktop.login1"}, sdbus::ObjectPath{"/org/freedesktop/login1"});
    sdbus::ObjectPath path;

    try {
        proxy->callMethod("GetSession").onInterface("org.freedesktop.login1.Manager").withArguments(std::string{"auto"}).storeResultsTo(path);

        m_sDBUSState.connection->addMatch("type='signal',path='" + path + "',interface='org.freedesktop.login1.Session'", ::handleDbusLogin);
        m_sDBUSState.connection->addMatch("type='signal',path='/org/freedesktop/login1',interface='org.freedesktop.login1.Manager'", ::handleDbusSleep);
        m_sDBUSState.login = sdbus::createProxy(*m_sDBUSState.connection, sdbus::ServiceName{"org.freedesktop.login1"}, sdbus::ObjectPath{"/org/freedesktop/login1"});
    } catch (std::exception& e) { Debug::log(WARN, "Couldn't connect to logind service ({})", e.what()); }

    Debug::log(LOG, "Using dbus path {}", path.c_str());

    if (!*IGNORESYSTEMDINHIBIT) {
        m_sDBUSState.connection->addMatch("type='signal',path='/org/freedesktop/login1',interface='org.freedesktop.DBus.Properties'", ::handleDbusBlockInhibitsPropertyChanged);

        try {
            std::string value = (proxy->getProperty("BlockInhibited").onInterface("org.freedesktop.login1.Manager")).get<std::string>();
            handleDbusBlockInhibits(value);
        } catch (std::exception& e) { Debug::log(WARN, "Couldn't retrieve current systemd inhibits ({})", e.what()); }
    }

    if (!*IGNOREDBUSINHIBIT) {
        // attempt to register as ScreenSaver
        std::string paths[] = {
            "/org/freedesktop/ScreenSaver",
            "/ScreenSaver",
        };

        try {
            m_sDBUSState.screenSaverServiceConnection = sdbus::createSessionBusConnection(sdbus::ServiceName{"org.freedesktop.ScreenSaver"});

            for (const std::string& path : paths) {
                try {
                    auto obj = sdbus::createObject(*m_sDBUSState.screenSaverServiceConnection, sdbus::ObjectPath{path});

                    obj->addVTable(sdbus::registerMethod("Inhibit").implementedAs([object = obj.get()](std::string s1, std::string s2) {
                           return handleDbusScreensaver(s1, s2, 0, true, object->getCurrentlyProcessedMessage().getSender());
                       }),
                                    sdbus::registerMethod("UnInhibit").implementedAs([object = obj.get()](uint32_t c) {
                                        handleDbusScreensaver("", "", c, false, object->getCurrentlyProcessedMessage().getSender());
                                    }))
                        .forInterface(sdbus::InterfaceName{"org.freedesktop.ScreenSaver"});

                    m_sDBUSState.screenSaverObjects.push_back(std::move(obj));
                } catch (std::exception& e) { Debug::log(ERR, "Failed registering for {}, perhaps taken?\nerr: {}", path, e.what()); }
            }

            m_sDBUSState.screenSaverServiceConnection->addMatch("type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged'",
                                                                 ::handleDbusNameOwnerChanged);
        } catch (sdbus::Error& e) {
            if (e.getName() == sdbus::Error::Name{"org.freedesktop.DBus.Error.FileExists"}) {
                Debug::log(ERR, "Another service is already providing the org.freedesktop.ScreenSaver interface");
                Debug::log(ERR, "Is niriidle already running?");
            } else
                Debug::log(ERR, "Failed to connect to ScreenSaver service\nerr: {}", e.what());
        }
    }

    systemConnection.reset();
}

void CNiriidle::setupLidDBUS() {
    // Listen for lid events via logind's seat interface
    try {
        auto proxy = sdbus::createProxy(*m_sDBUSState.connection, sdbus::ServiceName{"org.freedesktop.login1"}, sdbus::ObjectPath{"/org/freedesktop/login1"});

        sdbus::ObjectPath seatPath;
        proxy->callMethod("GetSeat").onInterface("org.freedesktop.login1.Manager").withArguments(std::string{"seat0"}).storeResultsTo(seatPath);

        Debug::log(LOG, "Using seat path {}", seatPath.c_str());

        m_sDBUSState.seatProxy = sdbus::createProxy(*m_sDBUSState.connection, sdbus::ServiceName{"org.freedesktop.login1"}, seatPath);

        // Get initial lid state
        try {
            bool lidClosed = m_sDBUSState.seatProxy->getProperty("LidClosed").onInterface("org.freedesktop.login1.Seat").get<bool>();
            Debug::log(LOG, "Initial lid state: {}", lidClosed ? "CLOSED" : "OPEN");
            m_sLidState = lidClosed ? ELidState::CLOSED : ELidState::OPEN;
        } catch (std::exception& e) {
            Debug::log(WARN, "Seat doesn't have LidClosed property (desktop?): {}", e.what());
            setupAcpiLidFallback();
        }

        // Subscribe to property changes
        {
            std::string matchRule = std::string{"type='signal',path='"} + std::string{seatPath.c_str()} + "',interface='org.freedesktop.DBus.Properties'";
            m_sDBUSState.connection->addMatch(matchRule, ::handleDbusLidPropertiesChanged);
        }

    } catch (std::exception& e) {
        Debug::log(WARN, "Couldn't setup lid dbus listener ({})", e.what());
        setupAcpiLidFallback();
    }
}

void CNiriidle::setupAcpiLidFallback() {
    // Try common ACPI lid state paths
    static const std::vector<std::string> candidates = {
        "/proc/acpi/button/lid/LID0/state",
        "/proc/acpi/button/lid/LID/state",
        "/proc/acpi/button/lid/LID1/state",
    };

    for (const auto& path : candidates) {
        std::ifstream f(path);
        if (!f.is_open())
            continue;

        std::string line;
        if (!std::getline(f, line))
            continue;

        m_sAcpiLidStatePath = path;
        bool closed = line.find("closed") != std::string::npos;
        m_sLidState = closed ? ELidState::CLOSED : ELidState::OPEN;
        Debug::log(LOG, "ACPI lid fallback: initial state {} ({})", closed ? "CLOSED" : "OPEN", path);
        break;
    }

    if (m_sAcpiLidStatePath.empty()) {
        Debug::log(WARN, "ACPI lid fallback: no /proc/acpi/button/lid/*/state found, lid events disabled");
        return;
    }

    // Open netlink uevent socket to receive kernel ACPI notifications
    m_iAcpiLidFd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
    if (m_iAcpiLidFd < 0) {
        Debug::log(WARN, "ACPI lid fallback: socket() failed ({}), lid events disabled", strerror(errno));
        return;
    }

    sockaddr_nl addr{};
    addr.nl_family = AF_NETLINK;
    addr.nl_pid    = static_cast<uint32_t>(getpid());
    addr.nl_groups = 1; // kernel uevents

    if (bind(m_iAcpiLidFd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        Debug::log(WARN, "ACPI lid fallback: bind() failed ({}), lid events disabled", strerror(errno));
        close(m_iAcpiLidFd);
        m_iAcpiLidFd = -1;
        return;
    }

    Debug::log(LOG, "ACPI lid fallback active via uevent socket");
}

void CNiriidle::setupPowerFallback() {
    namespace fs = std::filesystem;
    std::error_code ec;

    for (const auto& entry : fs::directory_iterator("/sys/class/power_supply", ec)) {
        std::ifstream typeFile(entry.path() / "type");
        std::string   type;
        if (!std::getline(typeFile, type) || type != "Mains")
            continue;

        auto          onlinePath = entry.path() / "online";
        std::ifstream onlineFile(onlinePath);
        std::string   online;
        if (!std::getline(onlineFile, online))
            continue;

        m_sSysPowerOnlinePath = onlinePath.string();
        bool onBattery        = (online == "0");
        m_sPowerSource        = onBattery ? EPowerSource::BATTERY : EPowerSource::AC;
        Debug::log(LOG, "Power fallback: {} ({})", onBattery ? "BATTERY" : "AC", m_sSysPowerOnlinePath);
        return;
    }

    if (ec)
        Debug::log(WARN, "Power fallback: /sys/class/power_supply scan failed ({})", ec.message());
    else
        Debug::log(WARN, "Power fallback: no AC adapter found, assuming AC");
}

void CNiriidle::processKernelUevent() {
    char    buf[4096];
    ssize_t len = recv(m_iAcpiLidFd, buf, sizeof(buf) - 1, MSG_DONTWAIT);
    if (len <= 0)
        return;
    buf[len] = '\0';

    // Uevents are null-separated "KEY=VALUE" strings — extract SUBSYSTEM first.
    std::string_view subsystem;
    for (const char* p = buf; p < buf + len; p += strlen(p) + 1) {
        std::string_view kv(p);
        if (kv.starts_with("SUBSYSTEM=")) {
            subsystem = kv.substr(10);
            break;
        }
    }

    if (subsystem == "acpi" && !m_sAcpiLidStatePath.empty()) {
        std::ifstream f(m_sAcpiLidStatePath);
        std::string   line;
        if (!std::getline(f, line))
            return;
        bool      closed   = line.find("closed") != std::string::npos;
        ELidState newState = closed ? ELidState::CLOSED : ELidState::OPEN;
        if (newState != m_sLidState) {
            Debug::log(LOG, "ACPI lid uevent: {}", closed ? "CLOSED" : "OPEN");
            onLidEvent(closed);
        }

    } else if (subsystem == "power_supply" && !m_sSysPowerOnlinePath.empty()) {
        std::ifstream f(m_sSysPowerOnlinePath);
        std::string   line;
        if (!std::getline(f, line))
            return;
        bool         onBattery = (line == "0");
        EPowerSource newSrc    = onBattery ? EPowerSource::BATTERY : EPowerSource::AC;
        if (newSrc != m_sPowerSource) {
            Debug::log(LOG, "Power source uevent: {}", onBattery ? "BATTERY" : "AC");
            onPowerSourceEvent(onBattery);
        }
    }
}

void CNiriidle::setupPowerDBUS() {
    // Listen for power source changes via UPower
    try {
        m_sDBUSState.upowerProxy = sdbus::createProxy(*m_sDBUSState.connection,
                                                       sdbus::ServiceName{"org.freedesktop.UPower"},
                                                       sdbus::ObjectPath{"/org/freedesktop/UPower"});

        // Get initial power state
        try {
            bool onBattery = m_sDBUSState.upowerProxy->getProperty("OnBattery").onInterface("org.freedesktop.UPower").get<bool>();
            Debug::log(LOG, "Initial power source: {}", onBattery ? "BATTERY" : "AC");
            m_sPowerSource = onBattery ? EPowerSource::BATTERY : EPowerSource::AC;
        } catch (std::exception& e) {
            Debug::log(WARN, "UPower OnBattery property not available: {}", e.what());
            setupPowerFallback(); // UPower not running — use sysfs + uevent instead
            return;
        }

        // Subscribe to property changes
        m_sDBUSState.connection->addMatch(
            "type='signal',path='/org/freedesktop/UPower',interface='org.freedesktop.DBus.Properties'",
            ::handleDbusPowerPropertiesChanged);

    } catch (std::exception& e) {
        Debug::log(WARN, "Couldn't connect to UPower ({})", e.what());
        setupPowerFallback();
    }
}

void CNiriidle::setupMediaDBUS() {
    // Listen for MPRIS media player state changes on the session bus
    try {
        m_sDBUSState.sessionConnection = sdbus::createSessionBusConnection();

        // Monitor MPRIS name ownership changes (player start/stop)
        m_sDBUSState.sessionConnection->addMatch(
            "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged'",
            [](sdbus::Message msg) {
                std::string name, oldOwner, newOwner;
                msg >> name >> oldOwner >> newOwner;
                if (name.find("org.mpris.MediaPlayer2.") != 0)
                    return;
                Debug::log(LOG, "MPRIS player {} changed owner: {} -> {}", name, oldOwner, newOwner);
                bool playing = isAnyMprisPlaying();
                g_pNiriidle->onMediaEvent(playing);
            });

        // Monitor MPRIS PlaybackStatus changes (play/pause within an existing player)
        m_sDBUSState.sessionConnection->addMatch(
            "type='signal',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged'",
            [](sdbus::Message msg) {
                std::string                           interface;
                std::map<std::string, sdbus::Variant> changedProperties;
                msg >> interface >> changedProperties;
                if (interface != "org.mpris.MediaPlayer2.Player" || !changedProperties.contains("PlaybackStatus"))
                    return;
                Debug::log(LOG, "MPRIS PlaybackStatus changed");
                bool playing = isAnyMprisPlaying();
                g_pNiriidle->onMediaEvent(playing);
            });

        // Get initial media state
        bool playing = isAnyMprisPlaying();
        Debug::log(LOG, "Initial media state: {}", playing ? "PLAYING" : "IDLE");
        m_sMediaState = playing ? EMediaState::PLAYING : EMediaState::IDLE;

    } catch (std::exception& e) {
        Debug::log(WARN, "Couldn't setup MPRIS listener ({})", e.what());
        Debug::log(WARN, "Media playing state will not be tracked");
    }
}

// ── Sleep Inhibition ───────────────────────────────────────────────────────

void CNiriidle::handleInhibitOnDbusSleep(bool toSleep) {
    if (m_inhibitSleepBehavior == SLEEP_INHIBIT_NONE ||     //
        m_inhibitSleepBehavior == SLEEP_INHIBIT_LOCK_NOTIFY // Sleep inhibition handled via onLocked/onUnlocked
    )
        return;

    if (!toSleep)
        inhibitSleep();
    else
        uninhibitSleep();
}

void CNiriidle::inhibitSleep() {
    if (!m_sDBUSState.login) {
        Debug::log(WARN, "Can't inhibit sleep. Dbus logind interface is not available.");
        return;
    }

    if (m_sDBUSState.sleepInhibitFd.isValid()) {
        Debug::log(WARN, "Called inhibitSleep, but previous sleep inhibitor is still active!");
        m_sDBUSState.sleepInhibitFd.reset();
    }

    auto method = m_sDBUSState.login->createMethodCall(sdbus::InterfaceName{"org.freedesktop.login1.Manager"}, sdbus::MethodName{"Inhibit"});
    method << "sleep";
    method << "niriidle";
    method << "niriidle wants to delay sleep until its before_sleep handling is done.";
    method << "delay";

    try {
        auto reply = m_sDBUSState.login->callMethod(method);

        if (!reply || !reply.isValid()) {
            Debug::log(ERR, "Failed to inhibit sleep");
            return;
        }

        if (reply.isEmpty()) {
            Debug::log(ERR, "Failed to inhibit sleep, empty reply");
            return;
        }

        sdbus::UnixFd fd;
        // This calls dup on the fd, no F_DUPFD_CLOEXEC :(
        // There seems to be no way to get the file descriptor out of the reply other than that.
        reply >> fd;

        // Setting the O_CLOEXEC flag does not work for some reason. Instead we make our own dupe and close the one from UnixFd.
        auto immidiateFD            = Hyprutils::OS::CFileDescriptor(fd.release());
        m_sDBUSState.sleepInhibitFd = immidiateFD.duplicate(F_DUPFD_CLOEXEC);
        immidiateFD.reset(); // close the fd that was opened with dup

        Debug::log(LOG, "Inhibited sleep with fd {}", m_sDBUSState.sleepInhibitFd.get());
    } catch (const std::exception& e) { Debug::log(ERR, "Failed to inhibit sleep ({})", e.what()); }
}

void CNiriidle::uninhibitSleep() {
    if (!m_sDBUSState.sleepInhibitFd.isValid()) {
        Debug::log(ERR, "No sleep inhibitor fd to release");
        return;
    }

    Debug::log(LOG, "Releasing the sleep inhibitor!");
    m_sDBUSState.sleepInhibitFd.reset();
}
