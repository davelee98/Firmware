# Plan: Configurable EPD Keep-Alive via PowerOption.screen_timeout_seconds

> Plan of record, 2026-07-13, branch `feat/less-latency`. Companion implementation
> summary: `IMPLEMENTATION_EPD_KEEPALIVE_CONFIG_2026-07-13.md`.

## Context

The `feat/less-latency` branch introduced an EPD panel power session state machine
(`PWR_OFF` / `PWR_WARM` / `PWR_ACTIVE`) with a keep-alive window: after a successful
refresh the panel rail + controller stay powered for a hardcoded `EPD_KEEPALIVE_MS`
(30 s), so a follow-up push skips the ~900 ms cold bring-up. This feature is **new on
this branch — `main` does not have it**; on `main` every refresh powers the panel
straight off.

Goal: replace the hardcoded 30 s with a per-device config value:

- New field **`uint8_t screen_timeout_seconds`** in `PowerOption` (0x04 config packet)
  — seconds the panel stays powered (PWR_WARM) after a refresh before shutdown.
- **Hard maximum 30 s**: effective window = `min(30, configured value)`.
- **0 → panel shuts down immediately** after refresh (this is also the default: old
  persisted config blobs and factory defaults have zeros in the reserved bytes, so
  existing devices match `main`'s shipped immediate-off behavior).
- This supersedes the earlier sleep_flags-bit-1 + sleep_timeout_ms idea: no flag bit is
  needed — 0 vs non-zero encodes disable/enable in one field, and the window is tunable
  independently of `sleep_timeout_ms` (which stays triple-duty: nRF advertising
  timeout, nRF loop interval, ESP32 idle-hold).
- The AXP2101 "force window to 0" sensor-scan safety override is **kept**: on AXP2101
  PMIC boards (warm idle draw unmeasured) the window is forced to 0 regardless of the
  configured value, and the override announces itself via `writeSerial` whenever it
  actually suppresses a non-zero configured value.

## Validation of existing keep-alive logic (pre-work, done)

The nRF-path keep-alive logic is **sound**:
- Timer is a wrap-safe `millis()` deadline poll (`(int32_t)(millis() - deadline) >= 0`,
  `display_service.cpp:299`), ticked from `loop()` top (`main.cpp:263`) and inside
  `idleDelay()`'s wait loop (`main.cpp:433`) — so nRF, which idles in
  `idleDelay(sleep_timeout_ms)`, expires on time.
- The nRF cross-task race (Bluefruit write-callback task runs Acquire/Release; loop
  task runs the tick) is guarded by `pwrmgmLock`; the tick try-locks and skips if held,
  and the take-spin yields via `delay(1)` (avoids the f8d683e priority-inversion
  livelock).
- Config-read race during reload is safe-direction: a Release racing
  `loadGlobalConfig()`'s memset reads `screen_timeout_seconds == 0` → immediate off.
  Single-byte read; no tearing. Same pattern as existing `globalConfig` reads in
  Acquire/Release.
- Minor pre-existing nit fixed by this change: `display_service.h:16` claims a
  "hard cap ~60 s" that was never enforced — the min(30 s, value) clamp makes the cap
  real (at 30 s).

## Analysis: handle `screen_timeout_seconds == 0` explicitly or via loop()/timers?

**Explicitly — and the explicit path already exists.** `epdSessionRelease()`
(`display_service.cpp:276`) already branches `if (window == 0 || !refreshSuccess) →
epdSessionForceOffLocked()`: synchronous power-down under `pwrmgmLock`, controller
properly slept (`bbepSleep` + 50 ms settle) before the rail cut. Value 0 needs **zero
new shutdown code** — `epdKeepAliveWindowMs()` returning 0 routes into it.

The timer alternative (arm `pwrmgmOffDeadlineMs = millis() + 0`, let `epdSessionTick()`
collect it) is strictly worse:
- Power-off latency becomes tick-cadence-dependent. Usually one loop()/idleDelay pass
  (ms), but unbounded in principle: on nRF the higher-priority Bluefruit callback task
  can keep the loop task off-CPU through back-to-back commands, and the tick's
  try-lock deliberately skips passes while a transfer holds the lock.
- The panel would transit PWR_WARM with a dead deadline — state churn, a misleading
  "panel warm-idle, off in 0 ms" log, and `epdSessionIsWarm()` briefly true for no
  benefit.
- The `!refreshSuccess` path already proves the synchronous branch is the intended
  "no keep-alive" route.

## Changes

### 1. `src/structs.h` — carve the field from reserved (line 61)
```c
    uint16_t min_wake_time_seconds; // Min awake window after first boot or button wake; 0 = default 120 s
    uint8_t screen_timeout_seconds; // EPD keep-alive: seconds panel stays powered (WARM) after a
                                    // refresh before shutdown. Clamped to 30 max; 0 = power off
                                    // immediately after refresh (default; matches pre-session behavior)
    uint8_t reserved[4];
```
Same total struct size (packed layout preserved) — the fixed-size 0x04 `memcpy` in
`config_parser.cpp:313-316` needs no change; old blobs yield 0. Same carve-out pattern
as `min_wake_time_seconds`.

### 2. `src/display_service.h:16` — cap constant replaces the default
```c
#define EPD_KEEPALIVE_MAX_S 30   // hard cap on power_option.screen_timeout_seconds (clamped, not rejected)
```
Remove `EPD_KEEPALIVE_MS` (no longer any default window — 0 means off). Update the
surrounding comment block.

### 3. `src/display_service.cpp` — `epdKeepAliveWindowMs()` (~lines 182-189)
Keep the AXP2101 safety override (first, before the config lookup) and make it
announce itself when it actually overrides a configured value; then source the window
from config:
```c
// Keep-alive window from config: screen_timeout_seconds, clamped to EPD_KEEPALIVE_MAX_S;
// 0 (also the old-blob/factory default) -> Release powers the panel straight down.
// Forced to 0 on AXP2101 boards regardless of config (PMIC warm idle draw unmeasured) —
// announced on the log whenever the override suppresses a non-zero configured value.
static uint32_t epdKeepAliveWindowMs(void) {
    uint8_t s = globalConfig.power_option.screen_timeout_seconds;
    for (uint8_t i = 0; i < globalConfig.sensor_count; i++) {
        if (globalConfig.sensors[i].sensor_type == SENSOR_TYPE_AXP2101) {
            if (s != 0) {
                writeSerial("[EPD session] AXP2101 present - keep-alive forced off (screen_timeout_seconds ignored)", true);
            }
            return 0;
        }
    }
    if (s > EPD_KEEPALIVE_MAX_S) s = EPD_KEEPALIVE_MAX_S;
    return (uint32_t)s * 1000;
}
```
The `s != 0` guard keeps the log quiet in the common case (AXP2101 board with the
field left at its 0 default — no override is actually happening). With a non-zero
config it logs once per Release (i.e. once per image push), which is informative
without being spammy. (`structs.h` already visible in this TU; no include changes.)

### 4. `src/communication.cpp` — `reloadConfigAfterSave()` (~line 26) hardening
After a successful reload, if keep-alive is now disabled and the panel is warm, power
it off so disabling takes effect immediately instead of after the stale deadline
(worst case 30 s):
```c
if (globalConfig.power_option.screen_timeout_seconds == 0 && epdSessionIsWarm()) {
    epdSessionForceOff();
}
```
`display_service.h` is already included (line 7); `epdSessionForceOff()` /
`epdSessionIsWarm()` are public, idempotent, and safe on the nRF callback task
(disconnect cleanup already calls ForceOff there). Do NOT force off unconditionally —
that would kill the warm panel on every config save. (A *shortened* non-zero value
applies from the next Release; residual ≤ 30 s, not worth re-clamping live.)

### 5. `src/config_parser.cpp` diagnostics (~lines 680-685)
Next to the existing "Sleep Flags" / "Button Wake" prints, add a decoded line, e.g.
`Screen Timeout: <n> s (EPD keep-alive; 0 = off immediately after refresh)` — mirrors
the bit-0 precedent used for log-based verification.

### 6. Stale "30 s" comments (code)
- `src/main.cpp:263` — "power the panel down 30 s after last release" → config-driven
  (`screen_timeout_seconds`).
- `src/main.cpp:488-494` — `enterDeepSleep` comment block ("expires it at 30 s",
  "effective keep-alive = min(30 s, idle-hold)") → min(window, idle-hold).
- `src/main.h:192` — pointer comment mentioning `EPD_KEEPALIVE_MS` → rename to
  `EPD_KEEPALIVE_MAX_S` / reword.

### 7. Docs
- **`docs/epd-panel-power-session.md`**: §4 keep-alive timer (lines ~226-260) — window
  now sourced from `screen_timeout_seconds` (clamped to 30 s, 0 = off/default);
  note that the AXP2101 override is retained and now logs when it suppresses a
  configured value. Update the hardcoded "30 s" mentions at lines 52, 238-239, 287-288, 307, 335
  (e.g. "disconnect within the keep-alive window reconnects onto a warm panel").
- **`docs/architecture-deep-sleep-power-buttons.md`**: add `screen_timeout_seconds` to
  the PowerOption/timer documentation (timer table ~line 93): purpose, clamp, default,
  and the ESP32-battery note that the effective warm time is min(window, idle-hold)
  because `enterDeepSleep` force-offs the panel.
- **`docs/PLAN_EPD_KEEPALIVE_CONFIG_2026-07-13.md`** (this file): plan copy in docs.

### Out of scope / follow-up
- **Toolbox**: the companion `opendisplay.org` toolbox `config.yaml` must expose the
  new `screen_timeout_seconds` byte in the 0x04 power_option packet (same as was done
  for `min_wake_time_seconds` / sleep_flags bit 0), or nobody can enable the feature —
  since the default is 0/off, the branch's warm-reconnect latency win ships disabled
  until the toolbox exposes it. Different repo; flagged for follow-up.

## Semantics summary

| screen_timeout_seconds | Effective keep-alive window |
|---|---|
| 0 (default; old blobs/factory) | none — panel powers off immediately after refresh (matches main) |
| 1–30 | value × 1000 ms |
| 31–255 | clamped to 30 000 ms |
| any, on an AXP2101 board | forced to 0 (safety override); logged when a non-zero value is suppressed |

ESP32 battery note: `enterDeepSleep` always calls `epdSessionForceOff()`, so the
effective window remains min(window, idle-hold); ForceOff is idempotent, so either
timer firing first is safe. nRF has no deep-sleep path — the loop/idleDelay tick is
the sole expiry mechanism there, and it is sound (see validation above).

## Verification

1. **Build both targets**: `pio run` for the nRF and ESP32 envs in `platformio.ini`
   (at minimum the default env and one `TARGET_ESP32` env) — must compile clean;
   `sizeof(struct PowerOption)` unchanged (packed carve-out).
2. **Default / 0**: flash nRF board with existing config (field absent → 0), push an
   image; serial log must show `[EPD session] release: keep-alive disabled, powering
   off` immediately after refresh (no `panel warm-idle` line); second push shows
   `acquire: COLD bring-up`.
3. **Enabled (e.g. 15)**: write config via 0x0041/0x0042 with screen_timeout_seconds =
   15; boot diagnostic prints `Screen Timeout: 15 s`; push an image → log shows
   `release: panel warm-idle, off in 15000 ms`; second push within 15 s shows
   `acquire: WARM re-acquire` (fast path); idle >15 s shows
   `keep-alive expired — powering panel off`.
4. **Clamp**: set 120 → log shows `off in 30000 ms`.
5. **Live disable**: while panel is warm, write a config with the field = 0 → panel
   forced off during `reloadConfigAfterSave` (log `[EPD session] force off`).
6. **ESP32 battery sanity**: with a non-zero value on a battery-mode ESP32, confirm
   deep sleep still enters on idle-hold and the panel is off before
   `esp_deep_sleep_start()`.
7. **AXP2101 override (if hardware available)**: on an AXP2101 board with a non-zero
   `screen_timeout_seconds`, push an image → log shows `[EPD session] AXP2101 present -
   keep-alive forced off (screen_timeout_seconds ignored)` followed by the immediate
   power-off release line.
