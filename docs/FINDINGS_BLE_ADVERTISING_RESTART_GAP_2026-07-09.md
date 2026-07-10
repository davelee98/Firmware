# Findings — BLE advertising can go dark during the post-wake window and never resume

**Date:** 2026-07-09
**Scope:** ESP32 (`TARGET_ESP32`) deep-sleep wake path only.
**Files:** `Firmware/src/main.cpp`, `Firmware/src/ble_init.cpp`, `Firmware/src/esp32_ble_callbacks.h`

## Symptom

After waking from deep sleep, the device advertises for up to `sleep_timeout_ms`
(e.g. 40000 ms) waiting for a client to connect. Serial log shows the full
timeout elapsing (`BLE advertising timeout (40000 ms) - no connection, returning
to deep sleep`) with a clean, uninterrupted time gap — but no client ever showed
up, even though one was in range and briefly connected.

## Root cause

`onDisconnect()` (`esp32_ble_callbacks.h:46-56`) never restarts advertising
itself — it only sets a flag:

```cpp
bleRestartAdvertisingPending = true;
```

The flag is serviced by `loop()`:

```cpp
// main.cpp:185
if (bleRestartAdvertisingPending) { esp32_restart_ble_advertising(); }
```

But during the post-wake wait, `loop()` takes an early-return branch on every
iteration and never reaches line 185:

```cpp
// main.cpp:133-154
if (woke_from_deep_sleep && advertising_timeout_active) {
    if (pServer && pServer->getConnectedCount() > 0) { ... return; }
    uint32_t advertising_duration = millis() - advertising_start_time;
    ...
    if (advertising_duration >= advertising_timeout_ms) { ... return; }
    delay(50);
    return;                      // <-- bleRestartAdvertisingPending never checked
}
```

The underlying BLE library (`framework-arduinoespressif32/libraries/BLE/src/BLEAdvertising.cpp`,
Bluedroid) does **not** auto-resume advertising after a disconnect — the app
must call `start()`/`esp32_restart_ble_advertising()` again. Confirmed via
`ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT` handling (only restarts if a `start()` call
raced with the stop, not on every disconnect).

Net effect: if a client connects and drops within the wake window (e.g. a
phone's BLE scanner doing a quick probe-connect), the radio goes silent for
whatever time remains in `sleep_timeout_ms`. The device does not notice — it
just counts elapsed time and eventually gives up and re-enters deep sleep,
having been non-advertising (and thus unreachable) for part of a window the
log reports as fully active.

## Fix

Service the pending restart from inside the wait branch, reusing the existing
guarded restart function (no new logic — it already no-ops if still connected
or mid-EPD-refresh):

```cpp
// main.cpp, inside the woke_from_deep_sleep && advertising_timeout_active branch,
// after the getConnectedCount() check and before computing advertising_duration:
if (bleRestartAdvertisingPending) {
    esp32_restart_ble_advertising();
}
```

Notes for whoever implements:
- Do **not** reset/extend `advertising_start_time` or the timeout budget — a
  stray connect/drop should just make the radio visible again for whatever
  time is left, not grant a new full window.
- `esp32_restart_ble_advertising()` (`ble_init.cpp:165-183`) already handles:
  connected (no-op), EPD refresh in progress (defers), otherwise calls
  `BLEDevice::startAdvertising()`.
- Verify on hardware: connect from a scanner app, drop the connection
  mid-window, confirm the device is still discoverable/connectable for the
  remainder of `sleep_timeout_ms` instead of going dark.

## Related (already fixed, not part of this task)

`main.cpp` previously logged a hardcoded `"Advertising for 10 seconds..."`
regardless of the configured `sleep_timeout_ms`. Already corrected to log the
actual `sleep_timeout_ms` value and name its source.
