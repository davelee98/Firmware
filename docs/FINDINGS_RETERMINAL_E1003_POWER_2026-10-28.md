# reTerminal E1003 power-saving investigation (2026-10-28)

This report looks at why a Seeed reTerminal E1003 running OpenDisplay drains
its battery, which is upstream issue
[OpenDisplay/Firmware#168](https://github.com/OpenDisplay/Firmware/issues/168).
It covers:

- what Seeed's firmware, TRMNL, Tesserae and the ESPHome reference project do
  about the same problem;
- which pins float during sleep;
- how to put the touch controller, the SD card and the other peripherals to
  sleep;
- what can be saved in both operating modes.

The two operating modes are:

- **Deep sleep**: on battery with `deep_sleep_time_seconds > 0`. The device
  wakes on a timer or a button, advertises over BLE, and goes back to sleep.
- **No sleep**: always on. This is either mains power (`power_mode != 1`), or
  battery with `deep_sleep_time_seconds == 0`. Note that the shipped
  `battery-3000` preset has `deep_sleep_time_seconds == 0`.

Code references are to **upstream `OpenDisplay/Firmware` at `6d05832`**
(2026-10-13). This fork's `main` (`3716925`) is **5 commits behind upstream**.
It does **not** yet contain the GT911 reset fix (#172) discussed below.

**No currents were measured on a bench in this investigation.** Figures are
quoted from the cited sources, and most of those were inferred from battery
percentage or voltage, not from an ammeter. Statements about the schematic
come second-hand from the Tesserae firmware's code comments, because the Seeed
schematic PDFs could not be downloaded from this environment. Every claim that
depends on something unverified says so.

---

## 1. Executive summary

1. **The dominant drain in deep sleep was the GT911 touch controller scanning
   all night (~5–8 mA).** On the E1003 the digitiser rail is always powered,
   and the reset line (TP_RST, GPIO48) has an external pull-up. Unless firmware
   intervenes, the chip comes out of reset and scans in Normal or Green mode
   through every sleep. Three groups independently traced the same ~5 mA to
   it: trmnl-firmware#572, Tesserae 1.40 and the ESPHome reference. Upstream
   OpenDisplay **#172** now holds GPIO48 low through deep sleep. **This fork
   does not have #172 yet, so the first action is to merge upstream `main`.**
2. **Every other switchable rail on the E1003 is left unmanaged by
   OpenDisplay.** These are EPD bias (11), IT8951 core (21), SD (39), mic
   (38), header (46) and the VBAT divider (40). Per Tesserae's reading of the
   schematic, each has a 100 kΩ pull-down, so each should fall to "off" once
   the pads isolate. The ESPHome reference and TRMNL PR #715 nonetheless drive
   them LOW and hold them, and both report sub-mA sleep. Holding them LOW is
   cheap insurance: a few dozen bytes of code, no runtime cost, about 0 µA.
   Whether OpenDisplay *needs* it is an open measurement question (§11).
3. **The holds must be done right.** A pad hold also latches its internal
   pull-up. A pin held LOW with its pull-up still on wastes **~70 µA per pin**
   (Tesserae measured this). Set the pull to floating before calling
   `gpio_hold_en()`. Separately, #172's `gpio_deep_sleep_hold_en()` has a side
   effect: from now on, **every** un-held digital pad is isolated (no output,
   no input, no pulls) during deep sleep. That is fine for the E1003, but it is
   a behaviour change for every board that has a touch reset pin configured.
4. **Even after the sleep fix, the next-biggest cost is the awake window.**
   - Every timer wake advertises for `sleep_timeout_ms`, which the shipped
     battery preset sets to 40 s, at 240 MHz with no power management.
   - The GT911 is reset back into Normal mode (~8 mA) on every wake, even when
     nobody can touch it.
   - With an hourly wake on 3000 mAh, cutting 40 s to 10 s and keeping the
     GT911 in reset on timer wakes is worth roughly **5–10× battery life on top
     of the sleep fix** (see §9).
5. **No-sleep mode has no power saving at all today.**
   - The CPU stays at 240 MHz and the loop ticks every 5 ms. There is no DFS
     and no light sleep, and the BLE TX power setting is ignored on ESP32.
   - The GT911 scans continuously.
   - After a refresh, the IT8951 bus stays **driven HIGH into a powered-off
     IT8951**, which back-powers the chip.
   - Expect ~35–45 mA, which is about 3 days on 3000 mAh. A realistic target
     with the measures in §8.2 is single-digit mA.
6. **Issue #168 asked to change the preset's `pullups` to `0x0`.** The E1003
   preset's I2C `dataBus.pullups = 0x3` turns on redundant internal pull-ups on
   GPIO19/20; the button `pullups` is already `0x0`. The battery preset's
   40 s awake window is the source of the "40 s → 10 s" request. Both are
   opendisplay.org preset changes, not firmware changes (§10, P0-5).

---

## 2. Issue #168 recap

**Original report.** MartinPo opened the issue on 2026-09-29. Setup: E1003,
60 min deep sleep, BLE, no encryption. Home Assistant pushes images only when
someone is home. The battery still lasts only **~2 weeks**; on 3000 mAh that
is about **8.9 mA average**. The reporter suspected the GT911 touch controller
and the SD card, citing trmnl-firmware#572.

GT911 power states quoted in the issue:

| GT911 state | Current | How it is entered |
|---|---|---|
| Normal (scanning) | ~8 mA | after reset release, or on a touch |
| Green (slow scan) | ~3.3 mA | automatically after idle |
| Doze | ~0.78 mA | — |
| Sleep | 70–120 µA | host writes `0x05` to register `0x8040`; to wake, host drives INT high for 2–5 ms (a touch cannot wake it) |

The key point: unless the host commands Sleep or holds RST low, the GT911
parks in Green mode at ~3.3 mA indefinitely. That includes the time the
ESP32-S3 spends in deep sleep.

**Follow-ups:**

- **yieldhog (10-18):** addressed in trmnl-firmware PR #715 (§5.1).
- **Upstream #172 (merged 2026-10-13):** GPIO48 is held low through deep
  sleep. If touch is configured as `NONE`, GPIO48 is also driven low while
  awake.
- **MartinArt10 (10-19, 10-28)** reported results from a local branch:
  - SD changes gave **~30 % longer** battery life.
  - "Sleeping 2 s after displaying an image" brought drain to **~2 %/day**,
    about 60 mAh/day or 2.5 mA average.

  MartinArt10 also asked for:
  1. the E1003 preset `pullups` set to `0x0`;
  2. the web-config battery idle timeout cut from 40 s to 10 s;
  3. sleep for the SD card, buzzer and mic, via GPIO 39 (SD_EN), 38 (PDM_EN),
     11 (EPD_Drive_EN), 21 (ITE_VCC_EN), 45 (BUZZER) and 40 (VBAT_EN);
  4. battery voltage read directly on the ESP32-S3 for better accuracy.

2.5 mA average is still roughly **25× worse** than the <100 µA the ESPHome
reference reaches on the same board (§5.3), so much more can be saved.

---

## 3. E1003 hardware and pin map

Sources:

- Seeed SenseCraft HMI `boards/reterminal_e1003/config.h:112-150`
- Seeed `GxEPD2_reTerminal_E1003.ino`
- Tesserae `boards/seeed_reterminal_e1003.h`
- TRMNL `include/config.h:200-209` and PR #715
- OpenDisplay preset `reterminal-e1003` (opendisplay.org
  `simple-config-presets.json`)

"Board pull" values come from Tesserae's code comments citing the Seeed
schematic. They are **not independently verified**.

| GPIO | Signal | Active | Board pull | Switches | OD preset uses it? |
|---|---|---|---|---|---|
| 7 / 9 / 8 | SPI SCLK / MOSI / MISO (IT8951 **and** microSD) | — | — | — | yes (`clk`/`data`/`dc`=MISO) |
| 10 | IT8951 CS | low | — | — | yes |
| 12 | IT8951 RST | low | — | — | yes |
| 13 | IT8951 HRDY/BUSY (**HIGH = ready**) | — | — | — | yes |
| 11 | **EPD_Drive_EN**: TPS65185 panel bias | high | 100 k pull-down (R44) | panel HV rail | FastEPD default (`pwr_pin_2`=0 → 11) |
| 21 | **ITE_VCC_EN**: IT8951 core | high | 100 k pull-down (R15) | IT8951 3V3 | FastEPD default (`pwr_pin_3`=0 → 21) |
| 39 | **SD_EN**: microSD slot power | high | 100 k pull-down (claimed) | SD VDD | **no** |
| 14 | SD CS | low | ? | — | no |
| 15 | SD DET (low = card present) | low | ? | — | no |
| 38 | **PDM_EN**: mic (TPS22916 load switch) | high | 100 k pull-down (claimed) | mic VDD | **no** |
| 41 / 42 | PDM DATA / CLK | — | — | — | no |
| 46 | **HEADER_EN**: expansion header rail (strap pin) | high | 100 k pull-down (claimed) + strap pull-down | header VDD | **no** |
| 40 | **VBAT_EN**: battery divider gate | high | ? (unverified) | divider | yes (`battery_sense_enable_pin`) |
| 1 | VBAT ADC (ADC1_CH0, 2:1 divider) | — | — | — | yes |
| 19 / 20 | I2C SDA / SCL: SHT4x 0x44, PCF8563 0x51, SY6974 charger 0x6B, GT911 0x5D/0x14 | — | external pull-ups (assumed) | — | yes (`dataBus`, **internal pull-ups 0x3**) |
| 2 | GT911 TP_INT (normal mode: active low; gesture mode: active high) | — | **none** on main board | — | yes (`int_pin`) |
| 48 | GT911 TP_RST | low | **external pull-up** | — | yes (`rst_pin` 0x30) |
| 45 | Buzzer, passive piezo, LEDC (strap pin) | — | strap pull-down | — | yes |
| 16 | User LED | **low** | none | — | yes (`led_2_g` 0x10) |
| 3 / 4 / 5 | Buttons: refresh-green / right / left | low | external pull-ups | wake | yes (`invert` 0x7, `pullups` 0x0) |
| 43 / 44 | UART console | — | — | — | `-extuart` env log |

**The digitiser rail is not switchable on the E1003.** There is no TOUCH_EN
pin, so the GT911's quiescent current can only be controlled two ways:

- **reset:** hold TP_RST low;
- **I2C sleep command:** write `0x8040 ← 0x05`, then keep INT from drifting
  high.

The SY6974 charger, the PCF8563 RTC and the SHT4x also sit on always-on
rails. Their quiescent current (SHT4x ~1 µA idle, PCF8563 ~0.25 µA, charger
datasheet-dependent) is the floor no firmware can remove.

**E1003 is the odd one out in the family.** On the E1001, E1002 and E1004 the
same functions sit on different pins: SD_EN is GPIO16, VBAT_EN is GPIO21, the
LED is GPIO6 (GPIO48 on E1004), and the charger I2C is on 39/40 (E1001/E1002).
§10 lists two possible preset bugs this exposes.

---

## 4. What OpenDisplay does today

### 4.1 Deep-sleep path (`enterDeepSleep()`, `src/main.cpp:1158-1255`)

The sleep path runs these steps in order:

1. `abortToKnownState`
2. `epdSessionForceOff()`
3. `buzzerStopForSleep()` and `ledStopForSleep()`
4. `ble.end()`
5. timer wake
6. `armButtonWakeSources()`
7. `powerLatchHoldForSleep()`
8. **`touchPrepareForDeepSleep()`** (#172)
9. `nm420HoldForSleep()`
10. `esp_deep_sleep_start()`

What each E1003 line ends up as **during deep sleep**, on upstream `6d05832`:

| GPIO | Function | Driven awake by OD? | Held for sleep? | State in sleep |
|---|---|---|---|---|
| 48 | TOUCH_RES | yes (touch init) | **yes, LOW** (#172) | GT911 in reset ✅ |
| 2 | TOUCH_INT | INPUT_PULLUP | no | isolated; GT911 in reset, so harmless |
| 11 | EPD_Drive_EN | LOW after `IT8951IODeInit()` | **no** | isolated; off only via the board pull-down |
| 21 | ITE_VCC_EN | LOW after `IT8951IODeInit()` | **no** | isolated; off only via the board pull-down |
| 39 | SD_EN | **never** | **no** | isolated; off only via the board pull-down |
| 38 | PDM_EN | **never** | **no** | isolated; off only via the board pull-down |
| 46 | HEADER_EN | **never** | **no** | isolated; off via board and strap pull-downs |
| 40 | VBAT_EN | HIGH ~30 ms per read, then LOW | **no** | isolated; board pull unknown ⚠️ |
| 45 | Buzzer | LOW after `buzzerStopForSleep()` | no | strap pull-down, so OK |
| 16 | LED (active low) | config | no | floats; off with no pull, so OK |
| 19/20 | I2C | Wire, internal pull-ups on | no | isolated |
| 3/4/5 | Buttons | input | wake pads (IDF holds them) | RTC ext0/ext1 wake |
| 7/8/9/10/12/13 | IT8951 SPI, RST, BUSY | see §4.3 | no | isolated |

**#172 changed how every other pin behaves in sleep.**
`touchPrepareForDeepSleep()` calls `gpio_deep_sleep_hold_en()`. In ESP-IDF
5.5.4, `esp_deep_sleep_start()` → `esp_sleep_start_safe()` →
`esp_sleep_isolate_digital_gpio()` (`sleep_modes.c:885-887`,
`sleep_gpio.c:131-181`). Once the global deep-sleep hold is enabled, that call
**isolates every digital pad that is not individually held**: output off, input
off, pull-up off, pull-down off. Pads used by the MSPI and the default console
UART are exempt.

So after #172, GPIO 11/21/38/39/40/46 are guaranteed to be high-Z with no
internal pull, and the board's own resistors decide whether each rail is on.
Before #172 the hold was never enabled on the E1003, and these pads simply lost
their driven level when sleep began.

Either way, **touch is the only E1003 rail actively held off in deep sleep
today**. Everything else depends on 100 kΩ pull-downs that have not been
verified first-hand. If one is missing (VBAT_EN is the most uncertain), that
rail floats.

Two side notes on `esp_sleep_isolate_digital_gpio()`:

- **Internal-RAM stack.** It asserts that the calling task's stack is in
  internal RAM. `enterDeepSleep()` runs on the Arduino loop task, whose stack
  is internal, and must stay there: if the loop task's stack ever moves to
  PSRAM, every deep sleep would hit the assert while the global hold is on.
- **Touch wake.** Tesserae found that latching TP_RST with
  `gpio_deep_sleep_hold_en()` broke **ext1 touch wake** on E1003 hardware
  (`touch_gt911.c:581-593`). This does not matter for OpenDisplay today
  because touch is not a wake source, but it matters if touch-to-wake is ever
  added (§7.1).

### 4.2 Wake path

**Display.** On a deep-sleep wake, `setup()` skips `initDisplay()`
(`main.cpp:244-252`), so the IT8951 is not powered until a transfer arrives.
That is good.

**Unmanaged rails.** Nothing drives 11/21/38/39/40/46 to a defined level
either. For the whole awake window they sit at the ESP32-S3 reset default
(input, not driven) and depend on the board pulls.

**Touch.** `initTouchInput()` releases the GT911 hold. Because the E1003
preset configures a GT911, it then **runs a hardware reset that puts the chip
into Normal mode (~8 mA)**. This happens on every wake, including a timer wake
when nobody will touch the screen.

### 4.3 After an IT8951 power-off, the bus still drives the unpowered chip (awake)

Teardown runs `epdSessionForceOff()` → `fastepd_direct_sleep()` → FastEPD
`IT8951IODeInit()`. That sends `TCON_SLEEP` and then drives EN (11) and ITE_EN
(21) LOW (`FastEPD.inl:791-797`). `pwrmgm(false)` runs next, but on the E1003
it skips two steps:

- `SPI.end()`, because `fastepd_driver_spi` is set;
- `configureDisplayPinsLowPower()`, because the preset has `pwr_pin = 0xFF`.

The result while awake: **CS (10) and RST (12) stay driven HIGH, and the SPI
pins stay attached to the peripheral**, pointing at an IT8951 whose 3V3 rail
has just been switched off. That back-powers the controller through its I/O
protection diodes, a classic phantom-power leak. The size is unmeasured; this
kind of leak is typically anywhere from 100 µA to a few mA. In deep sleep the
pads are isolated (§4.1), so the leak stops. In **no-sleep mode it is
permanent between refreshes.**

### 4.4 Awake and no-sleep behaviour

- **CPU:** 240 MHz, no `esp_pm_configure()`, no DFS, no automatic light sleep.
  `CONFIG_PM_ENABLE` is not set in the Arduino prebuilt sdkconfig.
- **Main loop cadence:** `platformIdle()` → `idleDelay(5)`
  (`main.cpp:911-935`). The CPU wakes about 200 times a second, which would
  defeat automatic light sleep even if power management were enabled.
- **BLE:**
  - Advertising interval 160–1000 ms (`ble_transport_esp32.cpp:49-52`); 20–30
    ms boost after a button press.
  - `power_option.tx_power` is applied **only on nRF**
    (`ble_transport_nrf.cpp:227`). ESP32 uses the controller default.
- **WiFi:** the firmware deliberately never calls `esp_wifi_set_ps()`
  (`wifi_service.h:111`), so when WiFi is on the default `WIFI_PS_MIN_MODEM`
  applies. WiFi is deferred on a wake, which is good.
- **Touch:** the GT911 is polled over I2C every 100 ms (preset
  `poll_interval_ms 0x64`). It runs in Normal or Green mode and is never
  commanded to Sleep.
- **Battery read:** VBAT_EN goes HIGH for about 30 ms per read, and the result
  is cached for 30 s. That is fine. The read uses raw `analogRead()` × a
  scaling factor (`display_service.cpp:1797-1822`) rather than the
  eFuse-calibrated `analogReadMilliVolts()`. That is the accuracy point raised
  in #168.
- **Awake window per wake (deep-sleep mode):** the `battery-3000` preset sets
  `sleep_timeout_ms = 40000`, so every timer wake spends 40 s advertising over
  BLE at full CPU clock.
- **Preset default:** `battery-3000` ships `deep_sleep_time_seconds = 0`. An
  E1003 configured from presets therefore **never deep-sleeps on battery**
  unless the user changes this value.

---

## 5. Other firmwares

### 5.1 TRMNL (`usetrmnl/trmnl-firmware`)

**Stock TRMNL** does **nothing** reTerminal-specific before deep sleep: no GPIO
hold, no RTC pull setup, no `esp_sleep_pd_config`, and no SD or touch power-down
(`src/sleep_session.cpp:36-106`). The `config_gpio_for_lp()` parking at
149-204 runs only for `BOARD_TRMNL_X`. WiFi is turned off before sleep. The
E1003 is driven through FastEPD with `bbep.einkPower(0); bbep.deInit();`
(`src/display.cpp:2752-2760`), exactly like OpenDisplay, so it has the same
unheld 11/21.

**Issue #572**, "reTerminal E1003 draws ~5 mA in deep sleep (GPIO39/GPIO48 not
held)", reports ~3.3 %/day on 3000 mAh, almost independent of the refresh
interval. That means the cost is sleep current, not wake time.

**PR #715** (open; `src/power/e1003_sleep.cpp`; E1003 only):

```c
static const gpio_num_t kSleepLowPins[] = {GPIO_NUM_11, GPIO_NUM_21, GPIO_NUM_38, GPIO_NUM_39, GPIO_NUM_48};
static void hold_low(gpio_num_t pin) {
  gpio_hold_dis(pin); pinMode(pin, OUTPUT);
  gpio_set_pull_mode(pin, GPIO_FLOATING); // a hold latches the pull-up too: ~70 uA/pin
  digitalWrite(pin, LOW); gpio_hold_en(pin);
}
void e1003_park_pins_for_sleep(void) {
  gpio_set_direction(GPIO_NUM_19, GPIO_MODE_DISABLE);   // I2C
  gpio_set_direction(GPIO_NUM_20, GPIO_MODE_DISABLE);
  for (...) hold_low(kSleepLowPins[i]);
  gpio_deep_sleep_hold_en();                            // needed for non-RTC 38/39/48
}
void e1003_enable_button_wakeup(int pin) {
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
  rtc_gpio_pullup_en(pin); rtc_gpio_pulldown_dis(pin);
  esp_sleep_enable_ext1_wakeup(1ULL << pin, ESP_EXT1_WAKEUP_ANY_LOW);
}
void e1003_release_sleep_holds(void) {   // first thing in setup()
  for (...) gpio_hold_dis(...); gpio_deep_sleep_hold_dis();
  digitalWrite(48, LOW);  // GT911 kept in reset while awake too
  digitalWrite(39, LOW);  // SD kept off while awake
}
```

The PR deliberately does **not** hold three pins:

- GPIO16, the active-low LED (holding it LOW would light it);
- GPIO40, VBAT_EN, left to the battery code;
- GPIO45, a strap pin that is already pulled down.

The GT911 is only held in reset; no I2C sleep command is sent.

Reported results. These are battery-gauge estimates, not ammeter readings.

| Who | Before | After |
|---|---|---|
| PR author, 2.5 h refresh | −2.9 %/day, ~34 days | −0.4 to −0.8 %/day, ~125 days |
| sebastfr, 30 min refresh, 3000 mAh | ~7 mA sleep, ~13 days | ~0.5 mA sleep, ~3.5 months |

**Awake-side techniques in TRMNL:**

- `display_sleep()` uses `esp_light_sleep_start()` instead of `delay()`. This
  is disabled on the E1003 via `DO_NOT_LIGHT_SLEEP`.
- bb_epaper's `setLightSleep()` light-sleeps while waiting on BUSY. This
  applies to SPI panels only and is skipped while WiFi is up.
- CPU clock: 160 MHz on the E1002, 240 MHz on the E1003.
- `CONFIG_PM_ENABLE` is not set.

TRMNL's strategy is short awake windows with WiFi off, not modem power-save.
For reference, the TRMNL OG (ESP32-C3) draws ~4 µA in deep sleep with timer-only
wake and ~100 µA with GPIO wake armed (`README.md:111-123`).

### 5.2 Seeed firmware and examples (`Seeed-Projects/OSHW-reTerminal-Series-E-D`)

- **`examples/base/LowPower_DeepSleep.ino:85-95`.** It arms ext1 ANY_LOW on
  GPIO3, enables the RTC pull-up, and calls `esp_deep_sleep_start()`. It
  claims **~14 µA**, but for a bare sketch: no `gpio_hold`, no rail shutdown,
  no touch sleep. On a real E1003 with the GT911 un-reset, that figure is not
  reachable.
- **SenseCraft HMI (`APP/app_device_info.cpp:690-764`).**
  - It skips sleep while charging.
  - It arms timer wake, and puts the GT911 into **gesture-wake mode**
    (`Gt911Touch::enableGestureWakeup`, `boards/common/gt911_touch.cpp:71-99`):
    write `0x814B ← 0`, `0x8046 ← 0x08`, then `0x8040 ← 0x08`, with an ext0
    high-level wake on INT. Button ext1 ANY_LOW is set in `hal.cpp:247-260`.
  - **No GPIO holds.** It drives mic EN (GPIO38) **HIGH at boot** and never
    turns it off. POWER_EN (VBAT divider) is also left high.
- **Seeed's TRMNL build (`TRMNL/src/bl.cpp:3224-3295`).** It turns WiFi off
  and arms ext0 on GPIO3, with no rail parking. On the E1003 it has a bug:
  `PIN_BATTERY` falls through to 3, the button pin, and the VBAT switch is
  never driven (`config.h:171-177`).
- **Gesture-mode current** (Tesserae's measurement of Seeed's approach): about
  1.1 mA idle, with ~11 mA bursts lasting ~2.5 s roughly every 17 s. That is
  far too much for a battery device that sleeps for hours.

**Takeaway:** Seeed's own firmware is *not* a good power reference. It leaves
rails on and keeps the GT911 active.

### 5.3 Tesserae (`dmellok/tesserae-device-firmware`, ESP-IDF) and the ESPHome reference

Tesserae's sleep path is `sleep_forever_or_until_timer()`
(`src/main.c:739-848`). In order:

1. **Buzzer:** `buzzer_idle()` sets duty 0, calls `ledc_stop`, and drives
   GPIO45 LOW. It is not held.
2. **SD:** `sdcard_prepare_sleep()` unmounts the card and parks CS HIGH and
   SD_EN LOW, sets the pulls to floating, and holds them. It also re-holds
   11/21 LOW, because an SD mount re-raises them; the SD card shares the SPI
   bus.
3. **Unused rails:** `sleep_park_rails()` handles
   `BOARD_SLEEP_DRIVE_LOW_MASK` = GPIO38 | GPIO46. For each pin it disables any
   hold, sets the pin as an output, sets the pulls to floating, drives it LOW,
   enables the hold, and calls `gpio_deep_sleep_hold_en()`.
4. **Panel:** after the paint, `it8951_sleep()` sends the IT8951 SLEEP
   command, drives 11/21 LOW, sets the pulls to floating and holds them. This
   is opt-in per board via `BOARD_EPD_HOLD_RAILS_IN_SLEEP`.
5. **Touch OFF** (`touch_park_idle()`, `touch_gt911.c:689-783`):
   - init the GT911;
   - turn off the GPIO and RTC pulls on INT, then drive INT LOW as an output;
   - write `0x8040 ← 0x05` (Sleep);
   - `gpio_hold_en(INT)` plus `gpio_deep_sleep_hold_en()`.

   The reason: a sleeping GT911 wakes when INT goes HIGH, so a released INT
   line can drift up and undo the park. **TP_RST is deliberately left HIGH**
   on its external pull-up.

   The cost is a ~120 ms GT911 reset on the next wake, because a sleeping
   GT911 stops answering its product-ID probe.
6. **Touch ON, tap mode:** the GT911 keeps scanning, and INT is folded into
   the button ext1 ANY_LOW mask. Before sleeping, the code waits up to 100 ms
   for INT to read idle-high; if it does not, touch wake is skipped for that
   sleep. Help text: costs "a few mA".
7. **Touch ON, gesture mode:** the Seeed sequence above, with an ext0
   high-level wake.
8. **Wake sources:** ext1 ANY_LOW on the buttons (`rtc_gpio_pullup_en`), and
   a drift-corrected timer.
9. **Wake stub:** `touch_wakestub.c` bit-bangs the GT911 from RTC memory about
   1 ms after wake, so a quick tap is captured before boot finishes.
10. **After wake:** each driver releases its own holds.

**Lessons Tesserae learned the hard way:**

- **Pull-up latching:** v1.40 holds drew **~70 µA per pin**, because the hold
  latched the pin's internal pull-up. v1.42 sets the pull to floating before
  every hold (CHANGELOG 515-525).
- **TP_RST and touch wake:** holding TP_RST through sleep broke ext1 touch wake.
- **IT8951 wedge:** a wedged IT8951 kept the SoC at active current for minutes
  while every HRDY wait ran to its 3 s timeout. After the first timeout,
  Tesserae now skips the remaining waits.
- **Light sleep during BUSY:** Tesserae light-sleeps during panel BUSY/HRDY
  waits (`busy_sleep.c:60-125`): it wakes on the HRDY GPIO level, with a
  ≥200 ms timer as backstop. This happens **only while WiFi and BLE are down
  and no USB host is attached**. It must wrap the call in
  `esp_sleep_enable_gpio_switch(false)`; otherwise IDF's pad workaround floats
  CS/RST/EN mid-refresh.
- **Other awake-side measures:**
  - WiFi stops before the paint, and reconnects use a cached BSSID and channel
    to skip the 1–2 s scan.
  - The IT8951 image load runs at 10 MHz: ~1 s, against 2.6 s at 4 MHz.
  - The battery is read once per wake, before the radio comes up.
  - `CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP=y` (copied from Seeed's
    TRMNL E1003 sdkconfig).
  - CPU fixed at 240 MHz, `CONFIG_PM_ENABLE=n`.
- **Unmeasured:** Tesserae states that its own rail-latch and touch-park savings
  are **unmeasured** on the bench (CHANGELOG 625-630).

**ESPHome reference** (`ar0v3r/reTerminal-E1003-ESPHome`, cited by #572 and
#715):

- drives 11, 21, 38, 39, 45 and 48 LOW and holds them;
- disables I2C on 19/20;
- turns VBAT_EN (40) off;
- leaves the LED (16) unheld;
- wakes on GPIO4 via ext1 ANY_LOW.

It reports **~4.95 mA dropping to below 100 µA**. This is the best published
figure for this board.

### 5.4 Comparison

| Measure | Seeed | TRMNL stock | TRMNL #715 | Tesserae ≥1.42 | ESPHome ref | **OpenDisplay `6d05832`** |
|---|---|---|---|---|---|---|
| GT911 parked in sleep | gesture (~1 mA) | ✗ | RST held LOW | I2C Sleep + INT held LOW | RST held LOW | **RST held LOW** ✅ |
| GT911 off while awake (unused) | ✗ | ✗ | ✓ (RST LOW) | ✓ | ? | only if `touch_ic_type = NONE` |
| 11 / 21 held LOW | ✗ | ✗ | ✓ | ✓ (opt-in) | ✓ | ✗ (board pull-down only) |
| SD_EN held LOW | ✗ | ✗ | ✓ | ✓ | ✓ | ✗ |
| Mic EN held LOW | ✗ (drives it HIGH!) | ✗ | ✓ | ✓ | ✓ | ✗ |
| Header EN held LOW | ✗ | ✗ | ✗ | ✓ | ✗ | ✗ |
| VBAT_EN | left HIGH | not driven (bug) | battery code | LOW, not held | off | LOW after read, not held |
| Pulls floating before hold | n/a | n/a | ✓ | ✓ (since 1.42) | ? | ✓ (`pinMode(OUTPUT)` clears pulls) |
| I2C pads disabled in sleep | ✗ | ✗ | ✓ | — | ✓ | isolated by IDF (via #172 global hold) |
| Button wake | ext1 + RTC pull-up | ext0 | ext1 ANY_LOW | ext1 ANY_LOW | ext1 ANY_LOW | ext0/ext1 (`armButtonWakeSources`) ✅ |
| Light sleep during BUSY | ✗ | SPI panels only | — | ✓ (radio down) | — | ✗ |
| Skip bootloader validate on wake | ✓ (TRMNL sdkconfig) | ? | — | ✓ | ? | ✗ (Arduino prebuilt bootloader) |
| Awake window | — | short (WiFi) | — | short | — | 40 s preset default |

---

## 6. Floating-pin analysis

On the ESP32-S3, a level set with `digitalWrite()` does **not** survive deep
sleep. The digital GPIO matrix loses power. There are two possible outcomes:

- **No global hold** (`gpio_deep_sleep_hold_en()` never called): pads drop to
  their reset or sleep default and the driven level is lost.
- **Global hold enabled** (the case on OpenDisplay since #172, and in every
  firmware that holds anything): IDF isolates every un-held pad (input off,
  output off, pulls off). The line then sits wherever the external circuit
  puts it.

So for every pin, the questions are: what is on the other side, and does it
have a defined pull?

| Pin(s) | Risk if floating in sleep | Verdict for OpenDisplay |
|---|---|---|
| 48 TP_RST | The external pull-up takes it HIGH, so the GT911 runs at ~3.3–8 mA. | **Must hold LOW** (done in #172), or use the I2C Sleep alternative (§7.1). |
| 2 TP_INT | No board pull. Matters only when the GT911 is in *I2C Sleep* (a rising INT wakes it). Irrelevant while RST is held LOW. | Hold LOW only if the I2C-Sleep approach is chosen. |
| 11, 21, 39, 38, 46 | A 100 k pull-down (claimed) keeps the rail off. If a pull-down is missing, the rail floats and may half-turn-on a load switch. | **Hold LOW** with pulls floating: cheap insurance (§7). |
| 40 VBAT_EN | Board pull unknown. If it floats high, the divider (2:1, total resistance unknown) draws continuously: e.g. 2 × 100 k means ~20 µA; 2 × 10 k means ~200 µA. | **Hold LOW** (generic for every board with `battery_sense_enable_pin`). |
| 14 SD CS, 15 SD DET | Harmless with SD_EN off. | Leave isolated. |
| 19/20 I2C | External pull-ups to an always-on 3V3. Isolated pads draw nothing. If a pad were left driving LOW, it would sink through the external pull-up. | Isolated by IDF; nothing to do. Optional `gpio_reset_pin()` before sleep. |
| 7/8/9/10/12/13 IT8951 bus | In sleep: isolated, and IT8951 VCC is off. **While awake after power-off: CS/RST/SPI are actively driven into the unpowered IT8951** (§4.3). | Fix for awake/no-sleep: after `deInit()`, release them (§7.3). |
| 45 Buzzer (strap) | Strap pull-down, so LOW. | Do **not** hold. Leave it to the strap pull-down. |
| 46 Header EN (strap) | Strap pull-down plus the claimed board pull-down. | Hold LOW optional. Do not drive HIGH at boot: it is a strap pin. |
| 16 LED (active low) | Floating means off. | Do **not** hold LOW (that would light it). |
| 3/4/5 Buttons | External pull-ups. As wake pads, IDF holds them. | Keep preset `pullups 0x0`. Internal RTC pulls are redundant. |

**Awake** (no-sleep mode, or during a wake window), pins that OpenDisplay never
drives float at the reset default. On the E1003 these are 38, 39 and 46. They
are off **only if** the claimed pull-downs exist. Driving them LOW once at boot
costs nothing and removes the dependency.

**Pull-up-under-hold trap.** A pad hold latches the internal pull-up and
pull-down as well as the level. OpenDisplay's existing holds
(`touchPrepareForDeepSleep()`, `nm420HoldForSleep()`,
`powerLatchHoldForSleep()`) all go through `pinMode(pin, OUTPUT)` first. In
arduino-esp32 3.3.x that sets `pull_up_en = pull_down_en = 0`, so they are safe
today. Any new hold code should still call
`gpio_set_pull_mode(pin, GPIO_FLOATING)` explicitly, so it does not depend on
that Arduino detail.

---

## 7. Peripheral-by-peripheral sleep strategy

### 7.1 GT911 touch

There are three ways to quiesce the GT911. The TP_RST pull-up value is
unknown, so the best choice needs one measurement.

| Approach | GT911 current | Extra current | Pros | Cons |
|---|---|---|---|---|
| **A. Hold TP_RST LOW** (OD #172, TRMNL #715, ESPHome) | in reset; datasheet doesn't say, presumably µA-class | 3.3 V / R(pull-up) through the external pull-up: 10 k ⇒ 330 µA, 100 k ⇒ 33 µA | no I2C needed; robust; can't be undone by INT drift; measured <100 µA total by ESPHome | ~60 ms re-init on next use; makes touch-wake impossible |
| **B. I2C Sleep** (`0x8040 ← 0x05`), INT driven LOW and held, RST left HIGH (Tesserae) | 70–120 µA | none | no resistor current | needs a working I2C transaction before sleep; INT must be held; ~120 ms reset on next wake |
| **C. Leave it scanning, use INT as ext1 wake** (Tesserae tap mode) | ~3.3 mA (Green) | — | touch wakes the device | 30–50× the sleep budget; not for battery |
| **D. Gesture mode** (Seeed) | ~1.1 mA + ~11 mA bursts | — | double-tap wakes | worse than C on average |

**Recommendation:**

- **Keep A** (already upstream). ESPHome's <100 µA total with A implies the
  pull-up is ≥ ~47 kΩ.
- **Measure** A against B once on a board. If B wins by more than ~30 µA,
  switch.
- **Never offer C or D on battery by default.** If touch-to-wake is wanted,
  make it an explicit opt-in config flag with a warning about the cost.

**While awake:**

- **Timer wake:** do not release the GT911 from reset. Bring it up lazily, on
  a button wake, on a BLE connect, or when an image has just been drawn and
  the user can interact. Today it costs ~8 mA for the whole awake window (40
  s by default) on every timer wake.
- **No-sleep mode:**
  - The GT911 drops to Green (~3.3 mA) by itself.
  - Optionally add an "idle touch off" timeout: after N minutes with no touch,
    issue the I2C Sleep command, and wake the GT911 (drive INT HIGH for 5 ms)
    when a button is pressed.
  - Touch cannot wake a sleeping GT911, so this trades responsiveness for
    ~3 mA. Make it a config option, not a default.
  - Consider interrupt-driven reads (an INT falling-edge ISR is already
    implemented in `attach_touch_int`) instead of a 100 ms poll, so the loop
    can sleep longer (§8.2).

### 7.2 microSD

OpenDisplay does not use the SD card on any reTerminal. On the E1003:

- **Boot:** drive SD_EN (39) LOW.
- **Sleep:** hold it LOW with the pulls floating.
- Leave SD CS (14) alone. With VDD off, a driven-HIGH CS back-powers the card.
  If CS must be driven at all, drive it only when the rail is on.

An idle microSD card draws anywhere from ~100 µA to a few mA depending on the
card, so this is worth doing even though the claimed pull-down probably keeps
it off already. This is likely where MartinArt10's "SD changes ⇒ +30 %" came
from, although battery-percentage measurements cannot separate it from the
GT911 change.

The reTerminal **Sticky** already parks SD (`display_service.cpp:1688-1698`:
SD_CS HIGH, SD_EN LOW), using a pin-signature heuristic. The E1003 should
follow the same idea, but via config (§10).

### 7.3 IT8951 + TPS65185 (EPD)

- **After every refresh**, the existing `fastepd_direct_sleep()` sequence is
  correct: `TCON_SLEEP`, then EN and ITE_EN LOW.
- **Add:** after `deInit()`, release the bus so it stops back-powering the
  unpowered IT8951 (§4.3). Call `SPI.end()`, then set CS (10), RST (12), SCLK
  (7) and MOSI (9) to `INPUT` or `LOW`. Leave MISO (8) and BUSY (13) as inputs.
  - FastEPD's `initIT8951()` re-asserts all of these, so the next push is
    unaffected.
  - CPU and RAM cost: ~0.
  - This matters most in **no-sleep mode**, where it is the only fix that
    applies.
- **Before deep sleep:** hold 11/21 LOW with the pulls floating.
  - The wake path must release those holds before `initIT8951()`, which
    power-cycles them. Otherwise the first push after a wake silently fails,
    because a held pad ignores writes.
- **Keep-alive window:** `epdSessionTick` / PWR_WARM keeps the IT8951 in
  `STANDBY`, not `SLEEP`, with ITE_VCC on. On battery the warm window is
  already capped by the idle hold. In no-sleep mode, keep the configured
  keep-alive short on the E1003. The IT8951 draws noticeably more in standby
  than with its rail off.
- **Optional:** light-sleep the CPU while waiting on HRDY (Tesserae). For
  OpenDisplay this applies only when the BLE link is down during the refresh,
  which is rare, because the host usually stays connected. Low priority.

### 7.4 Microphone (PDM)

OpenDisplay never uses it. Drive PDM_EN (38) LOW at boot and hold it LOW in
sleep. Never copy Seeed's "drive HIGH at boot".

### 7.5 Expansion header rail

GPIO46 is HEADER_EN and also a strap pin. Driving it LOW at boot is safe:
LOW is the strap's default. Hold it LOW in sleep, optionally. Only drive it
HIGH if a header peripheral is configured.

### 7.6 Buzzer

`buzzerStopForSleep()` already silences the buzzer and drives GPIO45 LOW. Do
not hold it: GPIO45 is a strap pin with a default pull-down, and a held LEDC pad
would need care on the next boot. No change.

### 7.7 Battery divider (VBAT_EN)

- **Reads:** the current approach is right. Gate HIGH, sample, gate LOW, and
  cache for 30 s.
- **Add:** hold LOW in sleep. This is generic for every board that has a
  `battery_sense_enable_pin` (E1001/E1002/E1004 use GPIO21, which is also an
  RTC pad).
- **Accuracy:**
  - Use `analogReadMilliVolts()` with the board's divider ratio instead of raw
    `analogRead() × scaling`. It applies the eFuse ADC calibration and fixes
    the nonlinearity at 11 dB attenuation.
  - Read once per wake, before BLE starts. TX bursts load the cell and depress
    the reading.
  - Both changes would answer the "more accurate levels" ask in #168.

### 7.8 I2C bus, sensors, RTC, charger

- **Preset pull-ups:** the preset enables internal pull-ups on 19/20
  (`dataBus.pullups = 0x3`), on top of the board's external pull-ups. They
  cost nothing at idle-high, but add ~70 µA per line whenever a line is low
  (transfers, or a stuck bus) and are redundant. **Set `pullups` to `0x0`** in
  the E1003 preset. That is the most plausible reading of MartinArt10's
  request.
- **GPIO19/20 are the ESP32-S3's USB D−/D+ pins.** The `-extuart` env still
  builds with `ARDUINO_USB_MODE=1` and `ARDUINO_USB_CDC_ON_BOOT=1`. The
  USB-Serial-JTAG PHY's D+ pull-up on GPIO20 is enabled at reset unless it is
  disabled. When I2C pulls SCL low, that adds a few hundred µA to ~2 mA of
  extra current, but only for the duration of each transfer. Worth verifying
  with a scope or ammeter. If confirmed, disable the USB-JTAG pad for the
  E1003 (for example, build the `-extuart` env without USB CDC).
- **SHT4x:** ~1 µA idle; no action. **PCF8563:** ~0.25 µA. It could later
  provide a calendar-aligned wake via its INT pin, if that pin is wired to an
  RTC GPIO (unknown).
- **SY6974 charger:** its battery-only quiescent current is the dominant
  floor no firmware can remove. It also has a **ship mode** (BATFET off),
  which could implement a true "power off" button with ~0 µA drain.
  Future work.

---

## 8. Mode-specific recommendations

### 8.1 Deep-sleep mode (battery, `deep_sleep_time_seconds > 0`)

In priority order:

1. **Sleep current to <100 µA.**
   - GT911 RST held LOW: #172, already upstream. Merge it.
   - 11, 21, 38, 39 and 40 (+ 46) held LOW with pulls floating.
   - Holds released at boot, before any driver touches the pins.
2. **Shorter awake window.** Today every timer wake advertises for 40 s.
   - Change the `sleep_timeout_ms` preset default to **10 s**; MartinArt10
     validated this.
   - Better still, have the host send `CMD_DEEP_SLEEP` (0x0053) as soon as it
     has finished. HA and py-opendisplay know when the upload is done, so the
     device can sleep within a second of the refresh instead of idling out.
     This is the "sleep 2 s after displaying an image" that got MartinArt10 to
     ~2 %/day.
   - Firmware-side alternative: a short "post-refresh idle hold" (e.g. 2 s)
     that applies after a completed refresh followed by a disconnect.
3. **No GT911 on timer wakes** (§7.1). Saves ~8 mA × awake window.
4. **BLE TX power on ESP32.** Apply `power_option.tx_power` through
   `NimBLEDevice::setPower()`. ESP32 currently ignores the configured value
   and uses the controller default. A lower TX power reduces the peak current
   of each advertising event. Small average gain, but it is a correctness fix.
5. **CPU at 80 MHz during the advertising-only window.** Raise it to 240 MHz
   when a connection or transfer starts (§8.2-2).
6. **Bootloader skip-validate on deep-sleep wake**
   (`CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP`). Saves the app-image hash
   on every wake, tens to hundreds of ms at full current. It needs a custom
   bootloader build (pioarduino `custom_sdkconfig` / hybrid compile), so it
   should be weighed against build complexity. Low priority.

### 8.2 No-sleep mode (mains, or battery with `deep_sleep_time_seconds == 0`)

On mains, power hardly matters. On **battery with no sleep**, which the
shipped preset default produces, the device lasts about 3 days. In order of
value per effort:

1. **Stop back-powering the IT8951** after each refresh (§7.3). Zero cost.
2. **CPU frequency scaling.** Call `setCpuFrequencyMhz(80)` while nothing is
   in flight, and switch to 240 MHz for decode, decompression and refresh.
   - The ESP32-S3 APB stays at 80 MHz for any CPU clock ≥ 80 MHz, so
     SPI/I2C/UART timing is unaffected. BLE works at 80 MHz.
   - Expect ~10 mA saved in idle (datasheet ballpark: ~32 mA at 240 MHz idle
     against ~20 mA at 80 MHz). Measure on the board.
   - Risk: WiFi/LAN throughput and TLS handshakes are slower at 80 MHz. Raise
     the clock while WiFi is up.
3. **Longer idle tick.** `platformIdle()` → `idleDelay(5)` wakes the CPU about
   200 times a second.
   - When nothing is in flight, a 50–100 ms tick (or blocking on a FreeRTOS
     notification that the BLE callbacks already have the context to send)
     costs no latency, since RX and events already break out of
     `idleDelay()`.
   - This is a prerequisite for item 4.
   - The comment at `main.cpp:925-931` explains why 2000 ms was too long. 50
     ms is a different trade-off. Verify that first-exchange latency stays
     acceptable.
4. **Automatic light sleep + BLE modem sleep** (`esp_pm_configure()` with
   `light_sleep_enable = true`).
   - This is the biggest no-sleep win. It is what gets ESP32-S3 BLE peripherals
     to ~1–3 mA average while advertising at 1 s intervals.
   - It requires `CONFIG_PM_ENABLE`, `CONFIG_FREERTOS_USE_TICKLESS_IDLE` and
     BLE controller modem sleep with a low-power clock. None of these is in
     the Arduino prebuilt sdkconfig, so it needs pioarduino `custom_sdkconfig`
     (hybrid compile).
   - Costs and risks:
     - much slower CI builds;
     - a new interaction with the pinned IDF 5.5.4 / NimBLE-Arduino 2.5.0;
     - light sleep is blocked while USB-Serial-JTAG is attached;
     - pads that must stay driven (EN lines during a refresh) need
       `esp_sleep_enable_gpio_switch(false)` handling, as Tesserae found.
   - Treat it as a separate project with its own findings doc.
5. **GT911 idle-off option** (§7.1). ~3.3 mA, at the cost of touch-to-wake.
6. **BLE TX power** (as in §8.1).
7. **Preset default:** decide whether the E1003 battery preset should ship
   with a non-zero `deep_sleep_time_seconds`. Today's default (0) means "always
   on" on battery, which surprises users.

---

## 9. Power budget estimates

These are **rough estimates**, for ranking measures only. Assumptions:

- 3000 mAh cell, 1 refresh/hour (≈ 24/day), hourly timer wake.
- ESP32-S3 awake with BLE advertising at 240 MHz: ~35 mA (datasheet
  ballpark).
- GT911 Normal ~8 mA, Green ~3.3 mA.
- IT8951 + TPS65185 full refresh: ~0.2 mAh each. This is unmeasured and could
  be off by 2×.
- Self-discharge, the charger quiescent current and cell ageing are ignored,
  so real figures will be lower at the long end.

### Deep-sleep mode

| Scenario | Sleep current | Awake | mAh/day | Battery life |
|---|---|---|---|---|
| Reporter (#168), pre-#172 | ~5–8 mA | 40 s × (35 + 8) mA | ~130–200 | **~2–3 weeks** (matches report) |
| + #172 only (GT911 RST held) | ~0.1–0.5 mA (rails depend on pull-downs) | 40 s × 43 mA | ~14–24 | ~4–7 months |
| + all rails held, 10 s window, no GT911 on timer wake | ≤0.1 mA | 10 s × 35 mA | ~2.4 + 2.3 + 4.8 (refresh) ≈ **10** | **~10 months** theoretical |
| + host `CMD_DEEP_SLEEP` right after upload (~3 s awake) | ≤0.1 mA | 3 s × 35 mA | ≈ **8** | ~1 year theoretical; self-discharge dominates |

**Once the sleep current is fixed, the refresh energy dominates.** After that,
the remaining lever is to **refresh less often**: skip a push when the image
hash is unchanged, which the ETag path already supports for HA.

### No-sleep mode (battery, `deep_sleep_time_seconds = 0`)

| Scenario | Average | Battery life |
|---|---|---|
| Today: 240 MHz, 5 ms tick, GT911 Green, IT8951 back-powered | ~40 mA | **~3 days** |
| + IT8951 bus released, 80 MHz idle | ~25 mA | ~5 days |
| + GT911 idle-off | ~22 mA | ~6 days |
| + auto light sleep + BLE modem sleep (needs a custom sdkconfig) | ~2–5 mA | **~4–8 weeks** |

**Battery no-sleep is only viable with power management.** Without it, the
preset default should be deep sleep.

---

## 10. Implementation plan for OpenDisplay

The CLAUDE.md rules apply: prefer config over new firmware targets, avoid
heap, and consider performance. Every item below is static, needs no heap,
and costs at most a few microseconds per boot or per sleep.

### P0: deep-sleep current (small, low risk)

1. **Merge upstream `main` into this fork.** This brings in #172 (GT911 RST
   hold) plus #165/#166/#167/#171.
2. **Hold the FastEPD IT8951 enables in sleep.** No protocol change: the pins
   are already known (`s_en`, `s_ite_en`, from `pwr_pin_2`/`pwr_pin_3`, with
   defaults 11/21).
   - Add `fastepd_prepare_deep_sleep()`: drive LOW, set pulls floating,
     `gpio_hold_en()`, and `gpio_deep_sleep_hold_en()` (skipped on C6, like
     `padHold()`).
   - Call it from `enterDeepSleep()` next to `touchPrepareForDeepSleep()`.
   - Add a matching `gpio_hold_dis()` at the top of
     `fastepd_epaper_begin()`, before `initIT8951()`.
   - In the same file, release the SPI/CS/RST pins after `deInit()` (§7.3).
3. **Hold the battery-sense gate LOW in sleep, generically.** If
   `battery_sense_enable_pin != 0xFF`, drive it LOW, set pulls floating, and
   hold it. Release it in the read path before driving it. This benefits
   every reTerminal (GPIO21 on E1001/E1002/E1004).
4. **Park the unused E1003 rails** (SD_EN 39, PDM_EN 38, HEADER_EN 46).
   These pins appear in no config block today: the wire format has no "spare
   output" field. That is the same situation the NM-EPD-420 hit
   (`nm420_init()` / `nm420HoldForSleep()`, `main.cpp:1480-1520`). Two
   options:
   - **(a) Short-term:** a `DEVICE_FLAG_RETERMINAL_E1003_INIT` device flag that
     mirrors the NM420 pattern. It needs a new flag bit, added in
     `../opendisplay-protocol` and propagated with
     `sync_protocol_header.py --push`, never hand-edited here. Then set the
     flag in the E1003 preset. It works, but it hard-codes board knowledge in
     firmware.
   - **(b) Preferred, medium-term:** a generic **"parked GPIO" config block**
     in the protocol: up to N `{pin, awake_level, sleep_level, flags}` entries,
     applied at boot and held across deep sleep. The NM420 list, the Sticky's
     SD parking heuristic (`display_service.cpp:1688-1698`) and the E1003
     rails would all move into presets, and new boards would need no firmware
     change. This matches the CLAUDE.md guidance to prefer configs over
     firmware changes for new devices. It needs a protocol change plus
     `tools/od-device-cli.py` `BLOCKS` and opendisplay.org toolbox support.
5. **Preset changes** (opendisplay.org `simple-config-presets.json`; no
   firmware needed):
   - `reterminal-e1003.dataBus.pullups`: `0x3` → `0x0` (board has external
     I2C pull-ups).
   - battery presets `sleep_timeout_ms`: `40000` → `10000`. If this is too
     aggressive for other boards, create an E1003-specific power preset.
   - `reterminal-e1003.powerDefaults.deep_sleep_current_ua`: fill in once
     measured, so the capacity estimator is right.
   - Consider shipping a non-zero default `deep_sleep_time_seconds` for the
     E1003 battery preset (§8.2-7).
   - **Check (other boards):** the E1001/E1002/E1004 presets put an LED on
     **GPIO16 (`led_3_b`)**, which is **SD_EN** on those boards per the Seeed
     and Tesserae pin maps. Driving that "LED" would switch the SD rail on.
     E1001/E1002 also set `pwr_pin = 11`, the same pin as `dc = 11`. Both need
     verification against the schematics.

### P1: awake window

6. **Don't release the GT911 from reset on a timer wake.** Initialise it on a
   button wake, on a BLE connect, or on first use (§7.1). The I2C bus is
   already initialised separately. `initTouchInput()` just needs a "deferred"
   path.
7. **Sleep promptly after a completed refresh.** Add host-side
   `CMD_DEEP_SLEEP` (0x0053) after an upload, in py-opendisplay / the HA
   integration, and/or a firmware "post-refresh idle hold" (§8.1-2).
8. **Apply `tx_power` on ESP32** via `NimBLEDevice::setPower()`.
9. **Battery accuracy:** use `analogReadMilliVolts()` × divider ratio, and
   read once per wake, before BLE comes up.

### P2: no-sleep mode

10. CPU at 80 MHz when idle, 240 MHz on demand.
11. A longer idle tick, or event-driven waits.
12. A GT911 idle-off option.
13. Automatic light sleep + BLE modem sleep, via `custom_sdkconfig`. This is
    its own project and findings doc.
14. Light sleep during IT8951 HRDY waits, only while the radio is idle.
15. Bootloader skip-validate on deep-sleep wake.

**Verification for P0/P1:**

- Build all 11 CI targets, including `esp32-wrover-e-N4R8` and `esp32-c6-N4`.
  `gpio_deep_sleep_hold_en()` does not exist on C6, so guard it as
  `padHold()` already does.
- On hardware, measure with a µCurrent or PPK2-class meter in series with the
  battery:
  1. deep-sleep floor, before and after each P0 item;
  2. awake-window average;
  3. charge per full refresh;
  4. no-sleep idle at 240 versus 80 MHz.
- Confirm that the button wake (ext0/ext1 on 3/4/5) still works, and that the
  first push after a timer wake still refreshes. This guards against holds that
  were never released.

---

## 11. Open questions / things to measure

1. **The actual deep-sleep floor of the E1003 on upstream `6d05832`.** Is
   anything still drawing current, or do the board pull-downs already keep
   every rail off? This decides whether P0-2/3/4 are savings or only
   insurance.
2. **The TP_RST pull-up value** (schematic) and GT911 current in reset. This
   decides approach A versus B in §7.1.
3. **The VBAT_EN board pull and divider resistance** (schematic).
4. **The real ESP32-S3 + BLE advertising current on this board** at 240 and
   80 MHz, and with `tx_power` applied.
5. **The charge per IT8951 full refresh** (GC16 vs DU), to firm up §9.
6. **Whether the USB-Serial-JTAG D+ pull-up on GPIO20 is active** on the
   `-extuart` env, and whether it matters.
7. **The SY6974 charger's quiescent current** in battery-only mode, and
   whether ship mode can implement a real off switch.
8. **The schematic PDFs** could not be fetched from this environment
   (`files.seeedstudio.com` blocked). Every "board pull" claim above is
   second-hand from Tesserae, and should be confirmed against
   `202004522_reTerminal_E1003_V1.0_SCH_251231.pdf`.

---

## Sources

- OpenDisplay/Firmware#168: https://github.com/OpenDisplay/Firmware/issues/168
- OpenDisplay/Firmware #172 (`6d05832`): "hold the GT911 reset line low
  instead of leaving it floating"
- FastEPD 2.x `src/FastEPD.inl` (`IT8951EinkPower`, `IT8951IODeInit`,
  `bbepInitIT8951`): https://github.com/bitbank2/FastEPD
- ESP-IDF v5.5.4 `components/esp_hw_support/sleep_modes.c`, `sleep_gpio.c`
  (`esp_sleep_isolate_digital_gpio`)
- usetrmnl/trmnl-firmware issue #572 and PR #715:
  https://github.com/usetrmnl/trmnl-firmware/issues/572,
  https://github.com/usetrmnl/trmnl-firmware/pull/715
- Seeed-Projects/OSHW-reTerminal-Series-E-D (LowPower_DeepSleep, SenseCraft
  HMI, TRMNL build): https://github.com/Seeed-Projects/OSHW-reTerminal-Series-E-D
- Seeed wiki sources: https://github.com/Seeed-Studio/wiki-documents
- dmellok/tesserae-device-firmware (CHANGELOG 1.40–1.42, `touch_gt911.c`,
  `sdcard.c`, `it8951_gray.c`, `busy_sleep.c`, `main.c`):
  https://github.com/dmellok/tesserae-device-firmware
- ar0v3r/reTerminal-E1003-ESPHome (via #572 / #715)
- E1003 schematic (not fetched):
  https://files.seeedstudio.com/wiki/reterminal_e10xx/img/e1003/202004522_reTerminal_E1003_V1.0_SCH_251231.pdf
