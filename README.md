# niriidle

An idle management daemon forked from [hypridle](https://github.com/hyprwm/hypridle), built specifically for [niri](https://github.com/YaLTeR/niri).

## What's different from hypridle

hypridle is designed for Hyprland and relies on several Hyprland-specific interfaces that don't exist on niri. niriidle replaces all of them with standard fallbacks:

| Feature | hypridle | niriidle |
|---|---|---|
| Lock detection | `hyprland-lock-notify-v1` | D-Bus `loginctl` Lock/Unlock signals |
| Lid detection | logind `LidClosed` property | ACPI `/proc/acpi/button/lid/*/state` + kernel uevent |
| Power source | UPower D-Bus | `/sys/class/power_supply/*/online` + kernel uevent |
| DPMS default | `hyprctl dispatch dpms off/on` | `niri msg action power-off/on-monitors` |
| Config path | `~/.config/hypr/hypridle.conf` | `~/.config/niri/niriidle.conf` |

## State machine

niriidle adds a state machine on top of the static listener model. Instead of manually chaining timeouts, you declare intent and the daemon handles the logic based on lid state, power source, lock state, and media playback.

### Group A — Lid events

| Event | Action |
|---|---|
| Lid close (AC) | lock + suspend (configurable) |
| Lid close (battery) | lock + suspend (separate config) |
| Lid close (media playing) | lock only, keep audio alive |
| Lid open | wake screen, reset idle timer |

### Group B — Unlocked idle

| State | Timeout chain |
|---|---|
| Battery | [`unlocked_dim_battery` → dim →] `unlocked_idle_battery` → lock → +`unlocked_screen_off_delay` → screen off → +`unlocked_suspend_delay` → suspend |
| AC | [`unlocked_dim_ac` → dim →] `unlocked_idle_ac` → lock → +`unlocked_screen_off_delay` → screen off (no suspend) |
| Media playing | long timeout → screen off only, no lock/suspend |

The dim step is optional — only active when `dim_cmd` is set and `unlocked_dim_*` > 0.

### Group C — Locked idle

Once locked, shorter timers take over independently of Group B.

| State | Timeout chain |
|---|---|
| Lid open + battery | [`locked_dim_battery` → dim →] `locked_idle_battery` → screen off → +`locked_suspend_battery` → suspend |
| Lid open + AC | [`locked_dim_ac` → dim →] `locked_idle_ac` → screen off |
| Input tap + battery | [`locked_dim_battery` → dim →] `locked_input_battery` → screen off → +`locked_suspend_battery` → suspend |
| Input tap + AC | [`locked_dim_ac` → dim →] `locked_input_ac` → screen off |

The dim step is optional — only active when `dim_cmd` is set and `locked_dim_*` > 0.

### Group D — Suspend → hibernate

| Condition | Action |
|---|---|
| Battery < `hibernate_battery_pct` while suspended | hibernate |
| Suspended longer than `suspend_to_hibernate_timeout` | hibernate |
| AC connected while suspended | stay suspended |

## Configuration

Config file: `~/.config/niri/niriidle.conf`

```ini
general {
    # ── Commands ──────────────────────────────────────────────────────────────
    # On niri: chain loginctl unlock-session so niriidle detects unlock.
    # On Hyprland: harmless duplicate (onUnlocked has idempotency guard).
    lock_cmd             = pidof hyprlock || (hyprlock; loginctl unlock-session)
    unlock_cmd           =
    before_sleep_cmd     = loginctl lock-session
    after_sleep_cmd      = niri msg action power-on-monitors
    on_lock_cmd          = niri msg action power-on-monitors
    on_unlock_cmd        =
    dpms_off_cmd         = niri msg action power-off-monitors
    dpms_on_cmd          = niri msg action power-on-monitors
    dim_cmd              = brightnessctl -s set 10
    dim_resume_cmd       = brightnessctl -r

    # ── Inhibit ───────────────────────────────────────────────────────────────
    inhibit_sleep            = 2    # 0=off 1=always 2=auto 3=lock-notify only
    ignore_dbus_inhibit      = 0
    ignore_systemd_inhibit   = 0
    ignore_wayland_inhibit   = 0

    # ── Group A: Lid ──────────────────────────────────────────────────────────
    lid_close_action         = lock_suspend   # lock_suspend | lock_only | suspend_only | nothing
    lid_close_battery_action = lock_suspend
    lid_close_media_playing  = lock_only
    lid_open_reset_idle      = true
    debounce_timeout         = 1500           # ms

    # ── Group B: Unlocked idle (seconds) ──────────────────────────────────────
    unlocked_dim_battery        = 150   # dim before lock (0 = disabled)
    unlocked_dim_ac             = 600
    unlocked_idle_battery       = 180
    unlocked_idle_ac            = 900
    unlocked_screen_off_delay   = 30    # screen off = base + this
    unlocked_suspend_delay      = 60    # suspend = base + screen_off_delay + this (battery only)

    # ── Group C: Locked idle (seconds) ────────────────────────────────────────
    locked_dim_battery       = 10   # dim before screen-off when locked (0 = disabled)
    locked_dim_ac            = 20
    locked_idle_battery      = 15   # screen off after lid-open wake
    locked_suspend_battery   = 60   # added on top of screen-off timeout before suspend
    locked_idle_ac           = 30
    locked_input_battery     = 10   # screen off after accidental input
    locked_input_ac          = 30

    # ── Group D: Suspend → hibernate ──────────────────────────────────────────
    hibernate_battery_pct        = 5
    suspend_to_hibernate_timeout = 14400     # seconds (4 hours)
}

# Static listeners: only for things not handled by the state machine.
# Dim is now handled via dim_cmd in general{} — use static listeners only
# for extras like keyboard backlight.
# Do NOT add lock/screen-off/suspend here — they will double-fire.

listener {
    timeout    = 150
    on-timeout = brightnessctl -sd rgb:kbd_backlight set 0
    on-resume  = brightnessctl -rd rgb:kbd_backlight
}
```

## Dependencies

- wayland
- wayland-protocols
- hyprland-protocols (for `ext-idle-notify-v1` and `hyprland-lock-notify-v1`)
- hyprlang >= 0.6.0
- hyprutils >= 0.2.0
- sdbus-c++ >= 0.2.0
- hyprwayland-scanner >= 0.4.4

## Building

```sh
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -S . -B build
cmake --build build -j$(nproc)
sudo cmake --install build
```

## Usage

Launch after login. With niri, add to your `config.kdl`:

```kdl
spawn-at-startup "niriidle"
```

Or via systemd:

```sh
systemctl --user enable --now niriidle.service
```

## Flags

```
-c <path>, --config <path>   path to config file
-v, --verbose
-q, --quiet
-V, --version
```
