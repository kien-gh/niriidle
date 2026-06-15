#pragma once

#include <memory>
#include <vector>
#include <string>
#include <sdbus-c++/sdbus-c++.h>
#include <hyprutils/os/FileDescriptor.hpp>
#include <condition_variable>
#include <chrono>
#include <unordered_set>

#include "wayland.hpp"
#include "ext-idle-notify-v1.hpp"
#include "hyprland-lock-notify-v1.hpp"

#include "../defines.hpp"

class CNiriidle {
  public:
    CNiriidle();

    // ── State Enums ──────────────────────────────────────────────────────
    enum class ELidState { OPEN = 0, CLOSED };
    enum class EPowerSource { AC = 0, BATTERY };
    enum class EMediaState { IDLE = 0, PLAYING };
    enum class ESystemPowerState { ACTIVE = 0, SUSPEND, HIBERNATING };
    enum class ELidCloseAction { LOCK_SUSPEND = 0, LOCK_ONLY, SUSPEND_ONLY, NOTHING };

    // ── Listeners ─────────────────────────────────────────────────────────
    struct SIdleListener {
        SP<CCExtIdleNotificationV1> notification   = nullptr;
        std::string                 onTimeout      = "";
        std::string                 onRestore      = "";
        bool                        ignoreInhibit  = false;
        bool                        onTimeoutFired = false;
    };

    struct SIdleListenerGroup {
        std::vector<SIdleListener> listeners;
        bool                       active = false;

        void destroy();
        void create(SP<CCExtIdleNotifierV1> notifier, wl_proxy* seat, bool ignoreWaylandInhibit,
                    const std::vector<std::pair<uint64_t, std::pair<std::string, std::string>>>& timeouts, bool ignoreInhibit);
    };

    // ── D-Bus Inhibit Cookies ────────────────────────────────────────────
    struct SDbusInhibitCookie {
        uint32_t    cookie = 0;
        std::string app, reason, ownerID;
    };

    // ── Public API ────────────────────────────────────────────────────────
    void               run();

    void               onGlobal(void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version);
    void               onGlobalRemoved(void* data, struct wl_registry* registry, uint32_t name);

    void               onIdled(SIdleListener*);
    void               onResumed(SIdleListener*);

    void               onInhibit(bool lock);

    void               onLocked();
    void               onUnlocked();

    SDbusInhibitCookie getDbusInhibitCookie(uint32_t cookie);
    void               registerDbusInhibitCookie(SDbusInhibitCookie& cookie);
    bool               unregisterDbusInhibitCookie(const SDbusInhibitCookie& cookie);
    size_t             unregisterDbusInhibitCookies(const std::string& ownerID);

    void               handleInhibitOnDbusSleep(bool toSleep);
    void               inhibitSleep();
    void               uninhibitSleep();

    // ── New State Machine ────────────────────────────────────────────────
    void onLidEvent(bool closed);
    void onPowerSourceEvent(bool onBattery);
    void onMediaEvent(bool playing);
    void onWakeTrigger(bool fromLid);

    void updateStateMachine();
    void recreateIdleListeners();
    void handleLockedIdle(bool wakeFromInput);
    void handleLidCloseAction();
    void handleSuspendToHibernate();

    ELidCloseAction   parseLidCloseAction(const std::string& s) const;
    void              scheduleHibernateCheck();
    void              checkHibernateCondition();
    void              setSystemPowerState(ESystemPowerState s) { m_sSystemPowerState = s; }

  private:
    void    setupDBUS();
    void    setupLidDBUS();
    void    setupAcpiLidFallback();
    void    setupPowerFallback();
    void    processKernelUevent();
    void    setupPowerDBUS();
    void    setupMediaDBUS();
    void    enterEventLoop();

    // ── State ─────────────────────────────────────────────────────────────
    bool              m_bTerminate    = false;
    bool              isIdled         = false;
    bool              m_isLocked      = false;
    int64_t           m_iInhibitLocks = 0;

    // Enhanced state tracking
    ELidState         m_sLidState            = ELidState::OPEN;
    EPowerSource      m_sPowerSource         = EPowerSource::AC;
    EMediaState       m_sMediaState          = EMediaState::IDLE;
    ESystemPowerState m_sSystemPowerState    = ESystemPowerState::ACTIVE;
    bool              m_bLastWakeFromLid     = true;

    // Debounce
    std::chrono::steady_clock::time_point m_tLastLidEvent;
    bool                                  m_bLidDebouncePending = false;

    // Hibernate check scheduling
    std::chrono::steady_clock::time_point m_tSuspendStart;
    bool                                  m_bHibernateChecked = false;

    // Kernel uevent fallback (ACPI lid + power supply)
    int         m_iAcpiLidFd       = -1;
    std::string m_sAcpiLidStatePath;
    std::string m_sSysPowerOnlinePath; // sysfs AC adapter "online" file

    // Inhibit sleep behavior
    enum {
        SLEEP_INHIBIT_NONE,
        SLEEP_INHIBIT_NORMAL,
        SLEEP_INHIBIT_LOCK_NOTIFY,
    } m_inhibitSleepBehavior;

    // ── Wayland Objects ───────────────────────────────────────────────────
    struct {
        wl_display*                      display          = nullptr;
        SP<CCWlRegistry>                 registry         = nullptr;
        SP<CCWlSeat>                     seat             = nullptr;
        SP<CCHyprlandLockNotifierV1>     lockNotifier     = nullptr;
        SP<CCHyprlandLockNotificationV1> lockNotification = nullptr;
    } m_sWaylandState;

    struct {
        SP<CCExtIdleNotifierV1>    notifier = nullptr;
        std::vector<SIdleListener> listeners; // legacy static listeners
    } m_sWaylandIdleState;

    // Enhanced listener groups
    SIdleListenerGroup m_sUnlockedGroup; // Group B: Unlocked idle timers
    SIdleListenerGroup m_sLockedGroup;   // Group C: Locked idle timers

    // ── D-Bus State ───────────────────────────────────────────────────────
    struct {
        std::unique_ptr<sdbus::IConnection>          connection;
        std::unique_ptr<sdbus::IConnection>          screenSaverServiceConnection;
        std::unique_ptr<sdbus::IProxy>               login;
        std::unique_ptr<sdbus::IProxy>               seatProxy;
        std::unique_ptr<sdbus::IProxy>               upowerProxy;
        std::vector<std::unique_ptr<sdbus::IObject>> screenSaverObjects;
        std::vector<SDbusInhibitCookie>              inhibitCookies;
        Hyprutils::OS::CFileDescriptor               sleepInhibitFd;

        // MPRIS tracking
        std::unique_ptr<sdbus::IConnection>          sessionConnection;
        std::unordered_set<std::string>              mprisPlayers;
        bool                                         mprisMonitored = false;
    } m_sDBUSState;

    // ── Event Loop ────────────────────────────────────────────────────────
    struct {
        std::condition_variable loopSignal;
        std::mutex              loopMutex;
        std::atomic<bool>       shouldProcess = false;
        std::mutex              loopRequestMutex;
        std::mutex              eventLock;
    } m_sEventLoopInternals;
};

inline std::unique_ptr<CNiriidle> g_pNiriidle;
