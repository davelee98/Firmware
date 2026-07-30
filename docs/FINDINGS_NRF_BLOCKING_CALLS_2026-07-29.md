# Blocking-call catalog and fault-path audit — nRF52840

**Date:** 2026-07-29
**Branch:** `fix/loop-hang-watchdog-and-nonblocking-log`
**Target:** `env:nrf52840custom` (nRF52840, Adafruit Bluefruit / SoftDevice S140). ESP32 noted only
where it differs.
**Scope:** every unbounded or long-blocking construct reachable from `loop()`, plus the
exception/allocation/fault handling that decides whether a failure becomes a reset or a permanent
hang. **Unlike [TIMER_AND_WATCHDOG_INVENTORY_2026-07-26.md](TIMER_AND_WATCHDOG_INVENTORY_2026-07-26.md),
this document deliberately includes `.pio/libdeps/**` and `~/.platformio/packages/**`** — that is
where the top two candidates live.

Written while chasing an intermittent freeze that persists after the `od_log` fix
(`a84e512`, `1fc524b`). Every claim below was verified against source or against the linked image
at `.pio/build/nrf52840custom/firmware.elf`.

---

## The headline

**On nRF52840 this firmware has no crash path — only hang paths.** There is no watchdog, and every
fault handler in the linked image is `e7fe  b.n <self>`:

```
0004dd14 <NMI_Handler>:               e7fe   b.n  4dd14
0004dd16 <HardFault_Handler>:         e7fe   b.n  4dd16
0004dd18 <MemoryManagement_Handler>:  e7fe   b.n  4dd18
0004dd1a <BusFault_Handler>:          e7fe   b.n  4dd1a
0004dd1c <UsageFault_Handler>:        e7fe   b.n  4dd1c
0004dd26 <Default_Handler>:           e7fe   b.n  4dd26
```

The Adafruit core *ships* a `HardFault_Handler` that calls `NVIC_SystemReset()`
(`cores/nRF5/utility/debug.cpp:60-64`), but **`debug.cpp` is not linked into this image** — no
`dbgHeap*` / `dbgStack*` symbols are present — so the weak `b .` from
`cores/nRF5/linker/gcc_startup_nrf52840.S` wins. `abort()` (ELF `0x55fc4`) likewise reaches `_exit`
(`0x57c80`), which is also `b .`.

**Therefore a HardFault, a NULL-pointer write, or a stack-corruption jump is indistinguishable from
the `loop()` hang under investigation.** Silent, permanent, no log, no post-mortem, no reset.

---

## Environment facts that frame everything below

- **Task priorities** (`cores/nRF5/rtos.h:57-61`): `loop` = **1 (LOW)**; Bluefruit `BLE` task and
  `SOC` task = **3 (HIGH)** (`bluefruit.cpp:473,480`); TinyUSB `usbd` task = 3; timer/callback task
  = 2. `loop()` is the lowest non-idle task in the system. The stack can starve it, and anything
  `loop()` blocks on is invisible to the rest of the system — the link stays up and advertising
  keeps running, so the tag looks alive while `loop()` is dead. That is the observed freeze
  signature.
- `delay()` on nRF is `TinyUSB_Device_FlushCDC()` + `vTaskDelay()` — a real yield. So any timeout
  built from counted `delay()` iterations bounds **scheduled** time, not wall clock.
- **Stacks:** `loop` **4096 B** (`cores/nRF5/main.cpp:42`), callback task 3072 B, BLE task 5120 B,
  SOC 800 B, usbd 800 B. All task stacks come out of the same newlib heap as `malloc`.
  The **MSP (ISR/SoftDevice) stack is only 2 KB** and butts directly against `__HeapLimit`
  (`__StackTop 0x20040000`, `__StackLimit == __HeapLimit 0x2003f800`; heap = 180 652 B).
- WiFi/LAN is compiled out on nRF (`OPENDISPLAY_HAS_WIFI` requires `TARGET_ESP32 &&
  OPENDISPLAY_ENABLE_WIFI`). All of `wifi_service.cpp` is dead code here.
- **The `[hb]` heartbeat is `od_log_debug`**, so it is compiled out of the stock `nrf52840custom`
  build (`OD_LOG_LEVEL` defaults to INFO). Bracketing a stall requires `nrf52840custom-debug`. The
  stall detector does not exist in the build the freeze is observed on.

---

## §A — Tier 1: genuinely unbounded, reachable from `loop()`

### A1. nRF52 TWIM (I²C) hardware-register spins — highest-probability hang

`~/.platformio/packages/framework-arduinoadafruitnrf52-seeed/libraries/Wire/Wire_nRF52.cpp`

| Line | Call | Bound | Hangs forever when |
|---|---|---|---|
| 166 | `while(!EVENTS_RXSTARTED && !EVENTS_ERROR);` | none | slave holds SCL low |
| 169 | `while(!EVENTS_LASTRX && !EVENTS_ERROR);` | none | clock-stretch forever / SDA short |
| **175** | `while(!EVENTS_STOPPED);` | none — **no `EVENTS_ERROR` escape at all** | classic TWIM lock-up: bus wedged after a NACK or glitch, STOPPED never fires |
| **181** | `while(!EVENTS_SUSPENDED);` | none — **no error escape** | same |
| 230 | `while(!EVENTS_TXSTARTED && !EVENTS_ERROR);` | none | " |
| 234 | `while(!EVENTS_LASTTX && !EVENTS_ERROR);` | none | " |
| **241** | `while(!EVENTS_STOPPED);` | none — **no error escape** | " |
| **247** | `while(!EVENTS_SUSPENDED);` | none — **no error escape** | " |

Tight CPU spins, no `yield()`, no deadline, at priority 1. Four of the eight do not even break on
`EVENTS_ERROR`. Reachable from `loop()` on essentially every pass:

- [`src/main.cpp:716`](../src/main.cpp) `processTouchInput()` → [`src/touch_input.cpp:80,83`](../src/touch_input.cpp)
  `Wire.endTransmission()` / `Wire.requestFrom()`. Polls at ≥100 ms
  (`TOUCH_PROCESS_MIN_INTERVAL_MS`), and `gt911_read_reg` retries **6× per read**
  ([`src/touch_input.cpp:200-213`](../src/touch_input.cpp)) — six chances to wedge per poll.
- [`src/main.cpp:621,657`](../src/main.cpp) `updatemsdata()` → `pollSht40SensorsForMsd()` /
  `pollBq27220ForMsd()` ([`src/sensor_sht40.cpp:64,80`](../src/sensor_sht40.cpp),
  [`src/sensor_bq27220.cpp:59,62`](../src/sensor_bq27220.cpp)). 30 s TTL each.
- [`src/main.cpp:644`](../src/main.cpp) `epdSessionTick()` and every panel acquire → `pwrmgm()` →
  `Wire.begin()` ([`src/display_service.cpp:903`](../src/display_service.cpp)) / `Wire.end()`
  ([`src/main.cpp:897`](../src/main.cpp)).

The firmware's own retry and back-off logic
([`src/touch_input.cpp:36,644`](../src/touch_input.cpp) — `TOUCH_I2C_FAIL_BACKOFF_MS`,
`TOUCH_I2C_FAIL_DISABLE_THRESHOLD`) is sound, but it only fires on a **returned error**. It never
fires if the driver never returns.

`gt911_drain_wire()` at [`src/touch_input.cpp:66`](../src/touch_input.cpp) is safe — it drains a RAM
ring, not hardware.

### A2. nRF InternalFS / SoftDevice flash — `portMAX_DELAY` plus unbounded busy-retry

`~/.platformio/packages/framework-arduinoadafruitnrf52-seeed/libraries/InternalFileSytem/src/flash/flash_nrf5x.c`

| Line | Call | Bound |
|---|---|---|
| 116-119 | `while (NRF_ERROR_BUSY == sd_flash_page_erase(...)) { delay(1); }` | **none** |
| **126, 149, 156, 163** | `xSemaphoreTake(_sem, portMAX_DELAY)` — four per page program | **INFINITE** |
| 143, 151, 158 | `while (NRF_ERROR_BUSY == sd_flash_write(...)) { delay(1); }` | **none** |

`_sem` is given only from `flash_nrf5x_event_cb` (`:48-51`), invoked on the SOC task
(`bluefruit.cpp:711`) on `NRF_EVT_FLASH_OPERATION_SUCCESS` **and** `_ERROR`. It is a counting
semaphore (max 10), so a lost or duplicated SoC event permanently desyncs take/give counts and a
later write blocks forever.

Reachable from `loop()` → `serviceBleRx()` → `imageDataWritten()`:

- `CMD_CONFIG_WRITE` (0x0041), `CMD_CONFIG_CHUNK` (0x0042), `CMD_CONFIG_CLEAR` (0x0045) →
  `saveConfig()` ([`src/config_parser.cpp:113-139`](../src/config_parser.cpp)) — `InternalFS.remove`
  (erase), two `file.write`, `file.close`.
- `secureEraseConfig()` ([`src/encryption.cpp:805-826`](../src/encryption.cpp)) rewrites the whole
  file in 512-byte chunks — many erase+program cycles back to back, four `portMAX_DELAY` waits each.

Two aggravating details in firmware code:

- `secureEraseConfig()` is called at [`src/communication.cpp:414`](../src/communication.cpp) and
  [`:476`](../src/communication.cpp) on the **unauthenticated** arm — reached when encryption is on,
  the peer has not authenticated, and `securityConfig.flags` bit 0 (rewrite-allowed) is set. That
  flag is the only thing standing between an unauthenticated peer and this flash path, so on a
  device that sets it the erase-and-rewrite runs before any credential is checked.
- Its `while (written < fileSize)` loop advances `written` by the *requested* size and ignores
  `file.write()`'s return, so a failing filesystem reports success having written nothing. (Same
  shape in the ESP32 arm at [`:834-839`](../src/encryption.cpp).)

**This is the candidate that best matches "hangs during or after a config push while connected"** —
sustained radio activity is exactly when the SoftDevice withholds flash slots.

### A3. `pwrmgmLockTake()` — unbounded spin, no deadline

[`src/display_service.cpp:410`](../src/display_service.cpp):

```c
while (__atomic_exchange_n(&pwrmgmLock, 1, __ATOMIC_ACQUIRE)) { delay(1); }
```

Take/give are balanced at all five sites ([`:442`/`:491`](../src/display_service.cpp),
[`:499`/`:500`/`:512`](../src/display_service.cpp), [`:516`/`:518`](../src/display_service.cpp)),
`epdSessionTick` uses the try-variant ([`:523`](../src/display_service.cpp)), and every caller is on
the loop task since Phase 3 — so it cannot spin today. The comment at
[`:403-409`](../src/display_service.cpp) says exactly this.

It remains a **self-deadlock trap**: the lock is non-recursive with no owner field, so one missed
give on a future early return, or one caller arriving from the SoftDevice/timer task, is an instant
permanent lock-up.

### A4. `powerOff()` stuck-button spin

[`src/power_latch.cpp:170-172`](../src/power_latch.cpp):

```c
while (digitalRead(buttonPin()) == LOW) { delay(20); }
```

Unbounded. A stuck-low or shorted button pin, or a missing pull-up, holds the device here forever
with the latch still engaged. Two call paths, both from `processButtonEvents()`:

- [`src/device_control.cpp:593`](../src/device_control.cpp) `powerButtonPoll()` →
  [`src/power_latch.cpp:216`](../src/power_latch.cpp) `powerOff()` (hold-to-power-off).
- [`src/device_control.cpp:948`](../src/device_control.cpp) `powerLatchPowerOff()` →
  [`src/power_latch.cpp:252`](../src/power_latch.cpp) `powerOff()`.

This is the still-unfixed finding from
[`AUDIT_FIRMWARE_2026-07-13.md:268-289`](AUDIT_FIRMWARE_2026-07-13.md) ("the only recovery is the
hardware/interrupt watchdog") — on nRF that recovery does not exist.

Gated on `DEVICE_FLAG_BATTERY_LATCH` plus a valid `pwr_pin_2`
([`src/power_latch.cpp:118-121`](../src/power_latch.cpp)), so latent on today's nRF boards rather
than live.

[`src/power_latch.cpp:102-104`](../src/power_latch.cpp) `for(;;) { delay(1000); }` is the deliberate
`[[noreturn]]` park after the rail cut. Intentional — but on nRF it is permanent if the latch
hardware does not actually drop the rail.

### A5. `enterDFUMode()` terminal spin

[`src/device_control.cpp:877`](../src/device_control.cpp) `while (1) {}`, after
`sd_softdevice_disable()` and `bootloader_util_app_start()`. Intentional and normally unreachable —
but with the SoftDevice down and all NVIC interrupts cleared
([`:866-871`](../src/device_control.cpp)), a failed bootloader jump is unrecoverable. Reached via
`CMD_ENTER_DFU` (0x0051).

---

## §B — Tier 2: bounded, but long enough to read as a freeze

### B1. Panel refresh wait — the longest window in the firmware

[`src/display_service.cpp:778-791`](../src/display_service.cpp):

```c
for (size_t i = 0; i < (size_t)(timeout * 100); i++){
    delay(10);
    if(i % 50 == 0) od_log_raw(".");
    if(!bbepIsBusy(&bbep)){ ... }
}
```

`timeout = 60` at every call site ([`:545`](../src/display_service.cpp),
[`:1611`](../src/display_service.cpp), [`:2417`](../src/display_service.cpp),
[`:2426`](../src/display_service.cpp), [`:3239`](../src/display_service.cpp),
[`:3242`](../src/display_service.cpp)).

**The bound is not 60 s.** `bbepIsBusy()` itself does `delay(10) + delay(1)` per call
(`bb_ep.inl:3984,3986`), so each iteration costs ~21 ms, not 10. 6000 iterations ≈ **126 s of dead
`loop()`**. The comment at [`:774-777`](../src/display_service.cpp) — "timeout\*100 iterations of
10 ms" — has the arithmetic wrong. And because `delay()` is `vTaskDelay`, the real elapsed time is
longer still under load.

For the whole window there is no BLE RX drain, no TX drain, no heartbeat, no button or touch
polling. [`DESIGN_COOPERATIVE_REFRESH_WAIT_2026-07-27.md`](DESIGN_COOPERATIVE_REFRESH_WAIT_2026-07-27.md)
measures 16 s on a Spectra 6 panel and documents the reconnect corruption that fell out of it.

`5f3e74c` on `debug/freeze-fix-phase2` already replaces this with a `millis()`-deadline
`waitForPanelIdle()`. That branch is **not merged**.

Separately, `bbepWaitBusy()` (`bb_ep.inl:3957-3976`) is bounded at 5 s (B/W) or **30 s**
(3/4/7-colour), and is invoked once per `BUSY_WAIT` token in every init sequence — dozens per panel
bring-up.

### B2. BLE notify semaphore chain

`BLECharacteristic::notify()` → `BLEConnection::getHvnPacket()` →
`xSemaphoreTake(_hvn_sem, ms2tick(BLE_GENERIC_TIMEOUT))`, `BLE_GENERIC_TIMEOUT = 100`
(`bluefruit_common.h:47`). Bounded at 100 ms per packet. But it compounds:

- `serviceBleTx()` drains up to 16 ([`src/command_queue.cpp:191`](../src/command_queue.cpp)) →
  up to 1.6 s.
- `serviceBleRx()` drains up to `COMMAND_QUEUE_SIZE` = 34 commands and calls `serviceBleTx()` after
  **each** ([`src/main.cpp:529-539`](../src/main.cpp)) → worst case ≈ **54 s of semaphore waiting in
  one `loop()` pass**, before any refresh time.

`BLEConnection.cpp:294 xSemaphoreTake(_hvc_sem, portMAX_DELAY)` (`waitForIndicateConfirm`) is
infinite but **not reachable**: the characteristic is declared `BLEWrite | BLEWriteWithoutResponse
| BLENotify` ([`src/ble_transport_nrf.cpp:31-32`](../src/ble_transport_nrf.cpp)), never
`BLEIndicate`. Worth an assertion so a future edit cannot open it.

No bonding/pairing path is used on nRF, so `Bluefruit.Security._authenticate` is unreachable.

### B3. Blocking `delay()` sequences inside `loop()`

| Site | Cost |
|---|---|
| `pwrmgm(true)` rail bring-up [`src/main.cpp:914,942`](../src/main.cpp) | `delay(800)` + `delay(100)` ≈ **900 ms** per cold acquire, unconditional |
| `epdSessionForceOffLocked` [`src/display_service.cpp:433`](../src/display_service.cpp) | `delay(50)` after `bbepSleep`, fires from `epdSessionTick()` in `loop()` |
| Panel init [`src/display_service.cpp:341,349`](../src/display_service.cpp), [`:1648`](../src/display_service.cpp) | `delay(200)` each |
| `prepareEpdRailForBoot` [`src/display_service.cpp:173,175`](../src/display_service.cpp) | `delay(50)` ×2 |
| GT911 reset [`src/touch_input.cpp:315-321`](../src/touch_input.cpp) | 300 ms + 200 ms per attempt, up to 3 attempts |
| `bbepWriteCmd` (`bb_ep.inl` / `arduino_io.inl`) | `delay(1)` **per command byte** — a 40-command init sequence costs 40 ms plus every `BUSY_WAIT` |
| `od_log_flush()` [`src/od_log.cpp:290`](../src/od_log.cpp) | `delay(5)`, 16 call sites (~80 ms across a boot) |

### B4. `idleDelay()`

[`src/main.cpp:725-751`](../src/main.cpp). Cooperative and well-behaved: chunked at 100 ms, services
`ble.tick`, buttons, touch, LED, EPD tick, buzzer and `serviceBleTx`, and early-returns on
`bleRxQueuePending()`. On nRF `platformIdle()` passes `sleep_timeout_ms`, a `uint16`, so the
argument can be up to **65.5 s**. Note each 100 ms chunk re-enters `processTouchInput()`, i.e. the
§A1 spins.

### B5. The only application-level watchdogs

Direct-write timeout 900 000 ms (15 min, [`src/main.cpp:664-670`](../src/main.cpp)) and
`checkPartialWriteTimeout()` 900 000 ms
([`src/display_service.cpp:580-589`](../src/display_service.cpp)). Both run *inside* `loop()`, so
they are the first casualty of a stall rather than a defence against one.

---

## §C — Tier 3: checked and found safe

Recorded so they are not re-investigated.

- **`od_log` / USB CDC.** `Adafruit_USBD_CDC::write()` (`Adafruit_USBD_CDC.cpp:218-236`) is a
  genuine unbounded `while (remain && tud_cdc_n_connected(...)) { ...; yield(); }` — the classic
  nRF CDC freeze, and the one this branch was opened for. It is **correctly fenced off**:
  [`src/od_log.cpp:111-123`](../src/od_log.cpp) is a `millis()`-deadline wait, the lock is a 20 ms
  `xSemaphoreTake` ([`:144`](../src/od_log.cpp)), text is truncated to 232 B against the 256 B
  FIFO, and the drop-notice path re-measures room before the second `println`
  ([`:188-200`](../src/od_log.cpp)). Worst case ≈ 40 ms per line.
- **No shipping-build code writes to `Serial` outside `od_log`.** The `Serial.println`/`flush` at
  [`src/main.cpp:90-199`](../src/main.cpp) and the unbounded `while (!Serial)` at
  [`:81-88`](../src/main.cpp) are all `OPENDISPLAY_BOOT_DIAG`, i.e. `env:nrf52840-bootdiag` only,
  which is excluded from `default_envs`.
- **`od_log_flush()`** — `Adafruit_USBD_CDC::flush()` is just `tud_cdc_n_write_flush()`,
  non-blocking. The `delay(5)` is the only cost.
- **nRF SPI** — `SPIClass::transfer` chunks into ≤64 KB descriptors and `nrfx_spim_xfer` in
  blocking mode is master-clocked; always terminates.
- **zlib / inflate** — the `for(;;)` loops at
  [`src/display_service.cpp:3134,3180`](../src/display_service.cpp) always terminate on
  `NEEDS_INPUT`, with output capped at `directWriteDecompressedTotal` / `expected_stream_size`
  ([`:3154`](../src/display_service.cpp), [`:3207`](../src/display_service.cpp)).
- Bounded by construction: the `serviceBleRx` drain (34,
  [`src/main.cpp:529`](../src/main.cpp)); the pipe reorder drain
  ([`src/display_service.cpp:2829`](../src/display_service.cpp), bounded by `PIPE_REORDER_SLOTS`);
  [`src/buzzer_control.cpp:163`](../src/buzzer_control.cpp);
  [`src/device_control.cpp:418`](../src/device_control.cpp); and every string/buffer loop in
  `boot_screen.cpp`, `communication.cpp`, `config_parser.cpp`, `encryption.cpp:783,795`.
- **No `portMAX_DELAY`, `vTaskDelay`, `taskYIELD`, `__WFE` or `sd_app_evt_wait` anywhere in
  `src/`.** Every infinite wait in the reachable graph lives in vendor code.

---

## §D — Task starvation: a freeze no timeout fixes

`loop_task` runs at priority 1; the BLE and SOC tasks at 3; the callback task at 2. A sustained
inbound BLE flood can therefore starve `loop()` indefinitely, and no amount of bounding calls
*inside* `loop()` addresses it.

This is not hypothetical. Commit `23ecaed` on `debug/freeze-fix-phase1`:

> The guard tripped but never dropped anything on nRF while a client flooded gated frames inside a
> direct write. Confirmed on hardware: the threshold's "drop pending" line printed, and then
> NEITHER the drop nor its skip diagnostic appeared until the flood stopped. loop() was not being
> scheduled at all.

In `-debug` builds this is aggravated: `bleRxQueuePush()` emits a hex line
([`src/command_queue.cpp:79-84`](../src/command_queue.cpp)) on the **callback task** (priority 2),
where `od_emit` can spend 20 ms on the lock plus 20 ms waiting for FIFO room — 40 ms per frame,
above `loop()`. The cost is documented and deliberate
([`src/command_queue.cpp:36-41`](../src/command_queue.cpp)); noting it because it makes the debug
build behave differently from the build being debugged.

---

## §E — Exceptions, allocation, and fault handling

### E1. Exceptions are disabled; `new` fails silently

`-fno-rtti -fno-exceptions` come from the **platform builder**, not `platformio.ini`
(`~/.platformio/platforms/nordicnrf52/builder/frameworks/arduino/adafruit.py:94-99`; also
`_bare.py:41-42`, `nrf5.py:50-51`). Resolved `build_flags` for `nrf52840custom` via
`pio project config --json-output` are only `-DTARGET_NRF -DOPENDISPLAY_ZLIB_USE_HEAP_WINDOW=0`.

Confirmed against the ELF: **zero** `__cxa_throw` / `_Unwind_` symbols. There are no `throw` /
`try` / `catch` sites and no `std::vector` / `string` / `map` / `function` anywhere in `src/`.

In the linked image `operator new` is a **bare tail-call to malloc with no failure handling**:

```
0004ddf8 <_Znwj>:   b.w  4b650 <__wrap_malloc>
0004ddfc <_ZdlPv>:  b.w  4b684 <__wrap_free>
```

So an allocation failure neither throws nor aborts — it returns `NULL` and the caller dereferences
it. On nRF52840 a read of `*NULL` succeeds (address 0 is the MBR, so you get garbage); a **write**
faults → `HardFault_Handler` → `b .` → dead. That is strictly worse than an abort for diagnosis.

### E2. FreeRTOS failure hooks are compiled to nothing

`cores/nRF5/rtos.cpp:84-94`:

```c
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {
  LOG_LV1("RTOS", "Task %s: stack Overflow !!!", pcTaskName);
  while(CFG_DEBUG) yield();
}
void vApplicationMallocFailedHook(void) {
  LOG_LV1("RTOS", "Task %s: failed to Malloc", ...);
  while(CFG_DEBUG) yield();
}
```

`CFG_DEBUG` is forced to **0** (`adafruit.py:252-253`), so both **return and let execution
continue** — a detected stack overflow proceeds on a corrupted stack, and a malloc failure returns
`NULL`. (With `CFG_DEBUG >= 1` they become a permanent hang instead. Neither outcome resets.)

`configCHECK_FOR_STACK_OVERFLOW 1` (`FreeRTOSConfig.h:78`) is method 1 — stack-pointer bound at
context switch only, so a transient deep excursion is missed entirely. `configASSERT`
(`FreeRTOSConfig.h:101-105`) is gated behind `DEBUG_NRF` / `DEBUG_NRF_USER`, neither of which is
defined, so every FreeRTOS internal sanity check is compiled out.

### E3. Allocation sites — project code is clean; the vendor BLE path is not

No `new` / `malloc` / `calloc` / `realloc` / `strdup` in any nRF-compiled source. The few
allocations that exist are checked or not compiled here:

| Site | Checked? |
|---|---|
| [`src/display_fastepd.cpp:149`](../src/display_fastepd.cpp) `malloc(pitch)` | yes ([`:150`](../src/display_fastepd.cpp)) — ESP32/FastEPD only |
| `lib/uzlib/src/od_zlib_stream.c:566` `malloc(WINDOW_SIZE)` | yes — and compiled out on nRF (`OPENDISPLAY_ZLIB_USE_HEAP_WINDOW=0`) |
| [`src/od_log.cpp:84-86`](../src/od_log.cpp) `xSemaphoreCreateMutex()` | yes ([`:143`](../src/od_log.cpp)); NULL degrades to unserialised writes, documented |
| [`src/config_parser.cpp:88`](../src/config_parser.cpp) `configScratch[4096]` | static, not stack — see the comment at [`src/communication.cpp:353-358`](../src/communication.cpp) |

Arduino `String` ([`src/main.cpp:121,191`](../src/main.cpp)) is `realloc`-based and fails soft.

The real churn is invisible to `src/`: [`src/ble_transport_nrf.cpp:169`](../src/ble_transport_nrf.cpp)
registers the write callback with Bluefruit's default `useAdaCallback = true`, so **every inbound
BLE frame does two `rtos_malloc`s** (`cores/nRF5/utility/AdaCallback.c:102-121`). Failure is handled
— `BLECharacteristic.cpp:536-541` falls back to calling the callback inline on the BLE task — so it
is a latency change rather than a crash. But a PIPE_WRITE burst is ~2 mallocs × ~200 frames/s
against the same heap that backs every task stack.

`_sbrk` (ELF `0x4de00`) does bound-check against `__HeapLimit` and fails cleanly.
[`src/diagnostics.cpp:41-49`](../src/diagnostics.cpp) computes free heap as
`__HeapLimit - __HeapBase - mallinfo().uordblks`, which correctly counts task stacks as used, and
reports `largest=n/a` because newlib offers no largest-block query
([`:73-76`](../src/diagnostics.cpp)).

### E4. Stack pressure on a 4 KB loop task

Notable frames on the 4096-byte loop stack:

- `char addrList[700]` + `foundDevices[128]` in `scanI2CDevices()`
  ([`src/display_service.cpp:1039,1021`](../src/display_service.cpp)) — called at boot from `initio()`.
- `url[128]` + `qrBuf[256]` + `payloadB64[64]` + `QRCode` under `refreshBootScreenFull()`
  ([`src/boot_screen.cpp:682,687`](../src/boot_screen.cpp)).
- `char buf[256]` on **every** log call ([`src/od_log.cpp:222,246`](../src/od_log.cpp)), at whatever
  depth.
- `uint8_t melody[256]` ([`src/buzzer_control.cpp:132`](../src/buzzer_control.cpp)).
- `char line[192]` + `label[48]` ([`src/command_queue.cpp:82`](../src/command_queue.cpp)) — on the
  3 KB callback task.

Worth measuring with `uxTaskGetStackHighWaterMark()`; the `[hb]` line is the natural place to
report it.

### E5. BLE- and config-driven indexing — no unchecked path found

Every attacker-reachable `memcpy` and index is bounds-checked. Verified:

- [`src/communication.cpp:544-549`](../src/communication.cpp) `len < 2` floor before `data[0]/[1]`.
- [`:589-604`](../src/communication.cpp) full envelope-length check before the nonce/tag copies.
- [`:624-628`](../src/communication.cpp) `memcpy(decrypted_data + 2, ...)` into a 512-byte buffer —
  safe because [`src/encryption.cpp:718-724`](../src/encryption.cpp) caps `payload_length` at a
  `uint8_t` and at `encrypted_len - 1`.
- [`:416-434`](../src/communication.cpp), [`:478-485`](../src/communication.cpp) chunked config
  write — every copy clamped by `CONFIG_CHUNK_SIZE` and `receivedSize + len > MAX_CONFIG_SIZE`,
  chunk count capped at `MAX_CONFIG_CHUNKS`.
- [`src/config_parser.cpp:200-222`](../src/config_parser.cpp) header length + CRC;
  [`:313-318`](../src/config_parser.cpp) per-iteration `offset` bound;
  [`:350-392`](../src/config_parser.cpp) each repeated-packet append double-gated on count and offset.
- [`src/display_service.cpp:2814`](../src/display_service.cpp)
  `if (plen > PIPE_REORDER_SLOT_SIZE) { sendPipeNack(0x03); return; }` — load-bearing: max reachable
  `plen` is 253 against a 248-byte slot.
- [`src/command_queue.cpp:47-56`](../src/command_queue.cpp) rejects `len == 0` and
  `len > MAX_COMMAND_SIZE` before the ring copy.

Only cosmetic nit: [`src/communication.cpp:179`](../src/communication.cpp) reads `frame[0]` in the
`len < 2` branch of `logTxFrame`; no current caller passes `len == 0`.

### E6. No watchdog, no reset-reason telemetry

- No `NRF_WDT` / `nrf_wdt` / `nrfx_wdt` / `sd_wdt_*` reference anywhere in `src/`, `include/`,
  `lib/`, `variants/` or `platformio.ini`. The only framework hits are unused MDK register
  definitions. The nine ESP32 envs get `-DCONFIG_FREERTOS_WATCHDOG_TIMEOUT_S=120`; the nRF env has
  no equivalent.
- `NRF_POWER->RESETREAS` is **never read**. The reset-reason block at
  [`src/main.cpp:133-155`](../src/main.cpp) is `#ifdef TARGET_ESP32`. nRF cannot distinguish
  power-on from brownout from `NVIC_SystemReset` from a pin reset — and since USB CDC re-enumerates
  on reset, an operator reads a silent reboot as "the tag froze".
- No GPREGRET crash breadcrumb either; `sd_power_gpregret_set(0, 0xB1)`
  ([`src/device_control.cpp:861-862`](../src/device_control.cpp)) is only the DFU magic.
- `NVIC_SystemReset()` is reached **only** from an explicit host `CMD_REBOOT`
  ([`src/device_control.cpp:277`](../src/device_control.cpp)). Nothing reboots the nRF on a fault.

---

## Ranked summary

| # | Item | § | Bound | Likelihood given a BLE-connected freeze |
|---|---|---|---|---|
| 1 | TWIM `EVENTS_STOPPED` / `EVENTS_SUSPENDED` spins, no error escape | A1 | none | **High** — up to 6× per 100 ms from `processTouchInput()` |
| 2 | SoftDevice flash `portMAX_DELAY` ×4 per page program | A2 | none | **High** — every config write/chunk/clear, and the radio is the contender |
| 3 | SoftDevice flash `NRF_ERROR_BUSY` retry loops | A2 | none | High — same trigger |
| 4 | HardFault / abort / pure-virtual → `b .` | headline, E1 | none | Turns any memory bug into this same freeze |
| 5 | `waitforrefresh` ≈126 s, not 60 s | B1 | 126 s | Reads as a freeze; fixed on an unmerged branch |
| 6 | `serviceBleRx` × `serviceBleTx` notify chain ≈54 s/pass | B2 | 54 s | Reads as a freeze under burst load |
| 7 | `loop()` starvation at priority 1 | D | n/a | Observed on hardware (`23ecaed`) |
| 8 | `pwrmgmLockTake()` no deadline | A3 | none | Latent — safe by single-task invariant only |
| 9 | `powerOff()` release wait, DFU `while(1)` | A4, A5 | none | Latent — config/command gated |

---

## Recommended remediation (decisions taken 2026-07-29)

**The vendor drivers stay as-is.** No `nrfx_twim` wrapper, no bounded flash path, no framework
patching. §A1 and §A2 are accepted as hangs and the watchdog is the recovery for both — the tag
reboots mid-transfer rather than recovering in place.

Recording the trade explicitly so it is not mistaken for an oversight: a config write colliding
with radio traffic (§A2) will cost a reset, and a wedged I²C bus (§A1) will **reset-loop** for as
long as the bus stays wedged, because `initio()` re-probes it on every boot. Reset-reason telemetry
(item 3 below) is what makes that loop legible rather than mysterious, which is why it is not
optional.

### Stage 0 — prerequisite

0. Cherry-pick `5f3e74c` from `debug/freeze-fix-phase2` (`waitForPanelIdle()`, `millis()`
   deadline). It fixes the §B1 arithmetic and gives Stage 1 a single place to feed the watchdog
   during a refresh. Doing the watchdog first guarantees a conflict in that loop body.

### Stage 1 — make hangs recoverable and diagnosable

1. **Arm `NRF_WDT`** in `setup()` (~30–60 s), fed once per `loop()` pass at
   [`src/main.cpp:636`](../src/main.cpp). Converts every Tier-1 item, and the fault-handler spins,
   into a reset. The feed must also reach inside the refresh wait and `idleDelay()`, or a
   legitimate worst-case refresh trips it — the §B1 and §B4 bounds set the timeout floor.
2. **Override the fault handlers.** `HardFault_Handler` is `weak`
   (`gcc_startup_nrf52840.S:310`), so a definition in `src/` wins with no framework patch: capture
   the stacked frame (PC, LR, PSR, CFSR/HFSR) into a `NO_INIT` RAM block or GPREGRET, then
   `NVIC_SystemReset()`. Same for `MemoryManagement_` / `BusFault_` / `UsageFault_`.
3. **Read, log and clear `NRF_POWER->RESETREAS`** at boot, alongside the retained crash block from
   (2). Extend the `#ifdef TARGET_ESP32` block at [`src/main.cpp:133-155`](../src/main.cpp) with an
   nRF arm.
4. **Define `vApplicationStackOverflowHook` / `vApplicationMallocFailedHook` in `src/`** so they
   record and reset instead of returning into corrupted state (§E2).
5. **Promote the heartbeat to INFO on nRF** (or add `-DOD_HEARTBEAT_AT_INFO`). As it stands the
   stall detector does not exist in the build the freeze is observed on.
6. Report `uxTaskGetStackHighWaterMark()` for the loop and callback tasks in the `[hb]` line (§E4).

### Stage 2 — cheap in-`src/` hardening (optional follow-on, no vendor code)

7. Give `pwrmgmLockTake()` ([`src/display_service.cpp:410`](../src/display_service.cpp)) a deadline
   and an owner field (§A3).
8. Bound `powerOff()`'s release wait ([`src/power_latch.cpp:170`](../src/power_latch.cpp)) (§A4).
9. Fix `secureEraseConfig()` ([`src/encryption.cpp:817`](../src/encryption.cpp)) to check
   `file.write()`'s return. Separately, review whether the rewrite-allowed flag should really let
   an unauthenticated peer reach the flash path at
   [`src/communication.cpp:414`](../src/communication.cpp) / [`:476`](../src/communication.cpp)
   (§A2) — that is a protocol decision, not a bug fix.
10. Add a compile-time assertion that the characteristic is never declared `BLEIndicate`, so the
    unreachable `portMAX_DELAY` at `BLEConnection.cpp:294` cannot be opened by a future edit (§B2).

### Verification (for whenever the stages are implemented)

- `pio run` — all eleven CI envs must stay green.
- `tests/serial_stall_test.py` still passes: hold DTR, stop reading, tag keeps servicing BLE.
- Bench repro for §A1: short SDA to GND mid-session. Pre-Stage-1 → permanent freeze; post → reset
  with `RESETREAS` naming the watchdog. Expect a reset loop while the short is held — that is the
  accepted behaviour, and the repro should confirm the log makes it obvious.
- Bench repro for §A2: push a config chunk sequence during a sustained PIPE_WRITE.
- Confirm the watchdog does **not** fire across a legitimate worst-case refresh on a Spectra 6
  panel, nor across `idleDelay(65535)`.

---

## Corrections to existing docs

- [`DESIGN_COOPERATIVE_REFRESH_WAIT_2026-07-27.md`](DESIGN_COOPERATIVE_REFRESH_WAIT_2026-07-27.md)
  and the comment at [`src/display_service.cpp:774-777`](../src/display_service.cpp) both describe
  the refresh bound as "timeout \* 100 iterations of 10 ms" = 60 s. The real figure is ~126 s,
  because `bbepIsBusy()` adds `delay(10) + delay(1)` per iteration.
- [`TIMER_AND_WATCHDOG_INVENTORY_2026-07-26.md`](TIMER_AND_WATCHDOG_INVENTORY_2026-07-26.md) §7.1
  records "Hung I²C (GT911 holding SDA low)" as bounded by the firmware's 5-failure disable
  threshold "but only if the transaction *returns*". That caveat is the whole story: the
  transaction does not return (§A1). The same table's "CPU/peripheral hard hang → accepted per the
  plan's software-only decision" understates the case — the fault handlers themselves hang
  (headline), which is fixable in `src/` with no hardware change.
- That inventory also describes the nRF architecture as "commands run inline on the Bluefruit
  Callback task, no queues". Phase 3 changed this; on HEAD both targets dispatch from `loop()`.
- Root `FINDINGS.md` is pinned to commit `4f4b503`; its three High-severity items (H1 dangling
  `errorResponse`, H2 `ButtonState` ODR, H3 `staticRowBuffer` overflow) are all fixed on HEAD. The
  only surviving audit item relevant here is the unbounded `powerOff()` release wait
  ([`AUDIT_FIRMWARE_2026-07-13.md:268-289`](AUDIT_FIRMWARE_2026-07-13.md) → §A4).
