# Findings: Boot screen reloads on deep-sleep wake cycle (ESP32, bb_epaper path)

**Date:** 2026-07-07
**Symptom:** Device on battery enters timer deep sleep; the wake cycle causes the boot
screen (logo + QR) to reload on the e-paper panel.
**Observed:** Serial log shows `=== WOKE FROM DEEP SLEEP ===` on wake, so wake-cause
detection works. Device uses the bb_epaper driver path (not Seeed_GFX).

## The invariant

`writeBootScreenWithQr()` has exactly one reachable call chain on the bb_epaper path:

```
setup() [NORMAL BOOT branch only]  (main.cpp:65)
  -> initDisplay()                 (display_service.cpp:1127)
    -> refreshBootScreenFull()     (display_service.cpp:154)
      -> writeBootScreenWithQr()
```

The deep-sleep wake branch (`minimalSetup()` -> loop -> `fullSetupAfterConnection()`)
never draws the boot screen. Neither do the BLE callbacks, `reloadConfigAfterSave()`,
or any command handler.

**Therefore: boot screen appearing == the chip took a second reset whose wake-cause
reads `ESP_SLEEP_WAKEUP_UNDEFINED`** — a hidden reset (panic, watchdog, brownout, or
`esp_restart()`) somewhere inside the wake cycle. The `WOKE FROM DEEP SLEEP` log line
is the wake itself; the boot screen comes from a later reset in the same cycle.

Note: RTC memory (`deep_sleep_count`, `displayed_etag`, `main.h:286-295`) **survives**
panics, `esp_restart()`, and brownout resets — only true power loss clears it. So
"wake detected correctly + boot screen later" is fully self-consistent with a
mid-cycle crash.

## Reset sources found in the wake cycle (ranked)

### 1. `BLEDevice::deinit(true)` while a client is connected — `main.cpp:296`

Command `0x0052` (`handleDeepSleepCommand`, `device_control.cpp:691-701`) calls
`enterDeepSleep()` **directly from BLE command context with the phone still
connected**, and the non-DFF branch sends *no response first*. `enterDeepSleep()`
stops advertising and calls `BLEDevice::deinit(true)` on a live connection — a
classic Bluedroid panic source (builds use pioarduino / arduino-esp32 3.x, Bluedroid
by default). Panic -> reboot -> `NORMAL BOOT` -> boot screen.

If the companion app sends `0x0052` at the end of each session, this fires **every
cycle** — deterministic, matching the symptom.

### 2. Same deinit racing an incoming reconnection

After an image push, `esp32_restart_ble_advertising()` runs
(`display_service.cpp:1759`); on disconnect the loop then hits `enterDeepSleep()`
(`main.cpp:197-198`). A phone that auto-reconnects the moment advertising reappears
can be mid-connection when deinit runs. Same crash, intermittent flavor. Also applies
when the 10 s advertising timeout (`main.cpp:98-101`) fires just as a connection is
being established.

### 3. Deep sleep entered with WiFi still running — `main.cpp:275-306`

`fullSetupAfterConnection()` calls `initWiFi(false)` -> `WiFi.begin()` on every
wake+connect (`wifi_service.cpp:113`), but `enterDeepSleep()` never calls
`WiFi.disconnect()` / `esp_wifi_stop()` — it only deinits BLE. Sleeping with the WiFi
driver mid-association is undefined/crash-prone, and also wastes sleep current.

### 4. Brownout on battery

Worst-case concurrent load in the wake cycle: BLE connected + WiFi STA associating +
EPD rail up + full refresh (`pwrmgm(true)` at `display_service.cpp:1510`). Brownout
reset -> `NORMAL BOOT` -> boot screen full refresh -> another current spike -> can
repeat. Log signature: `Brownout detector was triggered` on UART.

### 5. App-sent reboot / OTA command

`0x000F` (`communication.cpp:565-568`) or the OTA command (`device_control.cpp:687`)
-> `esp_restart()`. Log signature: `=== Reboot COMMAND (0x000F) ===`.

### 6. `checkResetPin()` false-trigger — runs on every wake

`full_config_init()` -> `checkResetPin()` (`config_parser.cpp:841` ->
`encryption.cpp:797`). If `SECURITY_FLAG_RESET_PIN_ENABLED` is set with neither pull
flag, the pin is read floating 100 ms after a deep-sleep wake (GPIO state differs
from cold boot) -> random `secureEraseConfig()` + `reboot()`. Destructive (erases
config). Log signature: `Reset pin triggered!`.

## Related wake-path defects (not the boot screen, but real)

- **Missing rotation after wake:** `fullSetupAfterConnection()` (`main.cpp:266-271`)
  does `memset(&bbep)` + `bbepSetPanelType` but **omits `bbepSetRotation`**, which
  `initDisplay()` sets (`display_service.cpp:1164`). On rotated-panel configs, every
  post-wake direct write runs with rotation 0 -> misaddressed window -> garbled
  render or BUSY-stall -> refresh timeout (`0x74`) — which an app may "recover" from
  by sending reboot, producing the boot screen via source #5.

## Ruled out

- `waitforrefresh()` and the partial path are watchdog-safe (all polling yields via
  `delay()`).
- `partial_prepare_panel_ram()` white-fills both planes
  (`display_service.cpp:2038-2039`), so the RTC-persisted `displayed_etag` matched
  against power-cycled panel RAM is safe by design.
- `powerDownAXP2101()` deliberately keeps DC1 (MCU rail) enabled
  (`display_service.cpp:978`) — not a rail-collapse source.
- `ble_init_esp32(true)` in minimal mode creates the full GATT server, so connection
  detection after wake works.
- Command queue sizes are consistent (`MAX_COMMAND_SIZE` 512 == queue slot size).

## Decisive diagnostic

The firmware logs wake cause but never **reset reason**. Add at the top of `setup()`:

```c
writeSerial("Reset reason: " + String((int)esp_reset_reason()));
```

The next boot-screen event identifies the culprit directly:

| `esp_reset_reason()`              | Hypothesis confirmed        |
|-----------------------------------|-----------------------------|
| `ESP_RST_PANIC`                   | #1 / #2 / #3 (crash)        |
| `ESP_RST_BROWNOUT`                | #4                          |
| `ESP_RST_SW`                      | #5 / #6 (`esp_restart()`)   |
| `ESP_RST_TASK_WDT` / `ESP_RST_INT_WDT` | watchdog               |
| `ESP_RST_POWERON`                 | latch/rail power drop       |

Also log `deep_sleep_count` continuity: if it keeps climbing across boot-screen
events, RTC survived — confirming a soft reset rather than power loss.

## Recommended fixes (once trigger confirmed)

1. In `handleDeepSleepCommand()`: send the ACK, then **disconnect the client and wait
   for disconnect** before calling `enterDeepSleep()`.
2. In `enterDeepSleep()`: stop WiFi (`WiFi.disconnect(true); WiFi.mode(WIFI_OFF);`)
   before BLE deinit.
3. In `fullSetupAfterConnection()`: add
   `bbepSetRotation(&bbep, globalConfig.displays[0].rotation * 90);`.
4. Add permanent `esp_reset_reason()` logging at boot (diagnostic above).
