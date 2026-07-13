# Implementation Summary: Configurable EPD Keep-Alive (screen_timeout_seconds)

> 2026-07-13, branch `feat/less-latency`. Plan of record:
> `PLAN_EPD_KEEPALIVE_CONFIG_2026-07-13.md`. Status: **implemented as planned**,
> both target builds verified.

## What was implemented

The EPD panel keep-alive window (how long the panel stays powered in `PWR_WARM`
after a successful refresh) changed from a hardcoded 30 s (`EPD_KEEPALIVE_MS`) to a
per-device config value:

| screen_timeout_seconds | Effective keep-alive window |
|---|---|
| 0 (default; old blobs/factory) | none — panel powers off immediately after refresh (matches `main`) |
| 1–30 | value × 1000 ms |
| 31–255 | clamped to 30 000 ms (`EPD_KEEPALIVE_MAX_S`) |
| any, on an AXP2101 board | forced to 0 (safety override); logged when a non-zero value is suppressed |

The `screen_timeout_seconds == 0` case is handled **explicitly** (not via the tick
timer): `epdKeepAliveWindowMs()` returns 0 and `epdSessionRelease()`'s pre-existing
`window == 0` branch powers the panel down synchronously under `pwrmgmLock`, with
the controller slept before the rail cut. No new shutdown code was needed; the panel
never transits a zero-length `PWR_WARM`.

## Code changes (all in `src/`)

1. **`structs.h:61-64`** — carved `uint8_t screen_timeout_seconds` out of
   `PowerOption.reserved[5]` → field + `reserved[4]`. Packed struct size unchanged,
   so the fixed-size 0x04 `memcpy` in `config_parser.cpp` needs no change and old
   persisted blobs read 0 (feature off). Same carve-out pattern as
   `min_wake_time_seconds`.
2. **`display_service.h:16`** — `#define EPD_KEEPALIVE_MS 30000` replaced by
   `#define EPD_KEEPALIVE_MAX_S 30` (hard cap, clamped not rejected). The previous
   comment's "hard cap ~60 s" claim was never enforced; the clamp now makes the cap
   real, at 30 s.
3. **`display_service.cpp:182-197`** — `epdKeepAliveWindowMs()` rewritten: AXP2101
   sensor scan first (override retained; PMIC warm idle draw unmeasured), returning 0
   and logging
   `[EPD session] AXP2101 present - keep-alive forced off (screen_timeout_seconds ignored)`
   only when it suppresses a non-zero configured value (quiet in the default-0 case;
   at most once per release otherwise). Non-AXP2101 path:
   `min(screen_timeout_seconds, EPD_KEEPALIVE_MAX_S) * 1000`.
4. **`communication.cpp:32-38`** — live-disable hardening in
   `reloadConfigAfterSave()`: after a successful config reload, if
   `screen_timeout_seconds == 0 && epdSessionIsWarm()` → `epdSessionForceOff()`, so
   disabling takes effect immediately instead of after the stale ≤30 s deadline.
   Conditional on purpose — a normal config save never tears down a warm panel.
5. **`config_parser.cpp:686`** — boot diagnostic added next to the sleep-flags
   prints: `Screen Timeout: <n> s (EPD keep-alive; 0 = off immediately after refresh)`
   (enables log-based verification, mirroring the sleep_flags bit-0 precedent).
6. **Stale "30 s" comments fixed** (comments only): `main.cpp:263` (tick call site),
   `main.cpp:488-495` (`enterDeepSleep` block — now "min(configured window,
   idle-hold)"), `main.h:191` and `display_service.cpp:45` (`EPD_KEEPALIVE_MS` →
   `EPD_KEEPALIVE_MAX_S`), `device_control.cpp:102-103` ("reconnect < 30 s" →
   "reconnect within the window").

## Docs changes

- **`epd-panel-power-session.md`** — §2.1 constant updated; §4 rewritten around the
  config-sourced window (source/clamp/0-default semantics, AXP2101 override + exact
  log line, actual `epdKeepAliveWindowMs()` listing); new §4 subsection
  "Live-disable hardening (config reload)"; §5 deep-sleep permutation table, §6
  results, and §7 residual-behavior notes reworded from "30 s" to "the configured
  keep-alive window (≤30 s)".
- **`architecture-deep-sleep-power-buttons.md`** — timer table gained an
  "EPD keep-alive window" row (`structs.h:61`, uint8 s, default 0 = off, 30 s clamp,
  AXP2101 forced-0), plus a prose note that on battery ESP32 the effective warm time
  is `min(keep-alive window, idle-hold)` because `enterDeepSleep()` always calls the
  idempotent `epdSessionForceOff()`.
- **`PLAN_EPD_KEEPALIVE_CONFIG_2026-07-13.md`** — plan of record saved into docs.

## Deviations from the plan

- `extern struct GlobalConfig globalConfig;` in `communication.cpp` was declared
  *below* `reloadConfigAfterSave()`; the declaration was moved above the function
  (compilation necessity, no behavioral change).
- Two additional stale keep-alive comments found by grep and fixed beyond the three
  the plan listed (`display_service.cpp:45`, `device_control.cpp:103`).

Everything else matches the plan verbatim, including the exact log/diagnostic
strings.

## Build verification

`pio run` — both environments compile clean (verified twice: by the implementing
agent and independently afterward):

| Environment | Result |
|---|---|
| `nrf52840custom` (nRF52) | SUCCESS (only pre-existing `-Wmaybe-uninitialized` warning in `boot_screen.cpp`, unrelated) |
| `esp32-s3-N16R8` (ESP32-S3) | SUCCESS |

## On-hardware verification (pending)

Steps 2–7 of the plan's Verification section require hardware: default/0 immediate
off, enabled window (`off in <n> ms` → `WARM re-acquire` → `keep-alive expired`),
clamp at 120 → 30 000 ms, live disable via config write, ESP32 battery deep-sleep
sanity, and the AXP2101 override log.

## Follow-up (out of scope)

- **Toolbox (`opendisplay.org` repo)**: `config.yaml` must expose the new
  `screen_timeout_seconds` byte in the 0x04 power_option packet (as was done for
  `min_wake_time_seconds` / sleep_flags bit 0). Until then the default is 0/off and
  the warm-reconnect latency win cannot be enabled from the toolbox.
