# Full Code Audit — ESP32 paths (bb_epaper, Seeed GFX) and deep-sleep wake

**Date:** 2026-07-09
**Tree:** branch `reset-reason-logging`, HEAD `b6b9752`
**Scope:** the whole `src/` tree with emphasis on (a) the ESP32 display path *without* Seeed GFX
(bb_epaper / `BBEPDISP`), (b) the ESP32 display path *with* `OPENDISPLAY_SEEED_GFX`, and (c) the
deep-sleep wake branch. nRF findings appear only where they reveal a cross-target inconsistency.
**Type:** report only. No code was modified.

**Method.** Six parallel line-by-line reads of the source (boot/power, bb_epaper display pipeline,
Seeed GFX path, BLE/WiFi/communication, config/encryption/peripherals, plus a re-verification pass
over the root `FINDINGS.md`), followed by first-hand confirmation of every Critical and High finding
against the current source. Where two readers disagreed, the disagreement was resolved by reading the
code (see [Ruled out](#ruled-out-checked-and-found-not-to-be-defects)). PlatformIO is not installed in
this environment, so **nothing here was compile- or hardware-verified**; findings marked
*needs-verification* additionally depend on facts not determinable from the source alone.

> **ID namespace.** `C#`/`H#`/`M#`/`L#` below are **independent of** the root `FINDINGS.md`
> numbering. Where a finding restates a still-open item from that document, it is cross-referenced
> as `FINDINGS.md H5` etc. Section 6 gives the full status of all 61 prior findings.

---

## 1. Summary

| Severity | Count | Character |
|---|---|---|
| Critical | 1 | Remotely reachable NULL dereference / panic |
| High | 12 | Display corruption, silent protocol failure, crypto, cross-task hardware races, battery |
| Medium | 24 | Correctness gaps, build-config defects, divergence between the two ESP32 display paths |
| Low / Info | 30 | Dead code, stale comments, micro-inefficiencies |

The three highest-value themes:

1. **The partial-write (0x76) path is the least-guarded code in the firmware.** Its sibling
   `handleDirectWriteStart` checks `seeed_driver_used()`; `handlePartialWriteStart` checks nothing —
   not the driver, not `display_count`, not panel validity, not `partial_update_support`. This yields
   one Critical (C1) and three separate High/Medium bugs (H12, M17, M19).
2. **The Seeed GFX path cannot report failure.** `waitforrefresh()` on Seeed is `delay(300); return
   true`. Every refresh is reported to the client as success (0x73) and commits `displayed_etag`,
   whatever actually happened on the wire. Combined with a once-per-boot init flag (H8) and an
   un-awaited 4-gray refresh (H11), a device can silently serve stale images indefinitely.
3. **`updatemsdata()`'s change-detection is dead code**, so the ESP32 tears down and rebuilds
   advertising — with a `delay(50)` — on every button press, touch coordinate change, BLE connect and
   60-second idle tick. This is a steady-state battery and radio-availability cost on the most
   power-sensitive product path, and it was introduced by an optimization intended to prevent exactly
   that.

---

## 2. Critical

### C1. Partial write (0x76) on a Seeed panel drives bb_epaper against a zeroed `bbep` → NULL dereference

**Category:** bug (remote crash) · **Path:** Seeed GFX (ESP32-S3) · *Verified.*

`handlePartialWriteStart()` ([display_service.cpp:1643-1731](../src/display_service.cpp#L1643-L1731))
has no `seeed_driver_used()` guard, no `display_count` check, and no panel-validity check. Its only
device gate is:

```cpp
if (getBitsPerPixel() != 1) { send_direct_write_nack(0x76, ERR_PARTIAL_UNSUPPORTED, false); return; }
```
— [display_service.cpp:1673](../src/display_service.cpp#L1673)

`getBitsPerPixel()` special-cases only the **4-gray** Seeed panel `3001`
([display_service.cpp:1212-1223](../src/display_service.cpp#L1212-L1223)). The **1bpp** Seeed panel
`PANEL_IC_SEEED_ED103TC2_1872X1404 = 3000` ([structs.h:71](../src/structs.h#L71)) with
`color_scheme = MONO` falls through and returns `1`, so the gate **passes**.

On the Seeed path `bbep` is never initialized: `initDisplay()`'s Seeed branch
([display_service.cpp:1138](../src/display_service.cpp#L1138)) skips `bbepSetPanelType`, and
`fullSetupAfterConnection()` returns early for Seeed ([main.cpp:303-309](../src/main.cpp#L303-L309)).
`mapEpd(3000)` hits `default: return EP_PANEL_UNDEFINED`
([display_service.cpp:303](../src/display_service.cpp#L303)). So `bbep.pInitFull` and
`bbep.pInitPart` are both `NULL` and geometry is zero.

`partial_prepare_panel_ram()` then runs unconditionally
([display_service.cpp:2140-2151](../src/display_service.cpp#L2140-L2151)):

```cpp
bbepInitIO(&bbep, ...dc_pin, reset_pin, busy_pin, cs_pin, data_pin, clk_pin, 8000000);
bbepWakeUp(&bbep);
const uint8_t* initSeq = bbep.pInitPart ? bbep.pInitPart : bbep.pInitFull;  // == NULL
bbepSendCMDSequence(&bbep, initSeq);                                        // NULL deref
bbepFill(&bbep, BBEP_WHITE, PLANE_1);                                       // pColorLookup NULL deref
```

**Reachability.** The etag precondition at
[display_service.cpp:1666](../src/display_service.cpp#L1666) (`oldEtag == displayed_etag`) is
satisfiable because `handleDirectWriteEnd()` commits `displayed_etag = newEtag` on the shared code
path *after* the Seeed branch ([display_service.cpp:1878](../src/display_service.cpp#L1878)) — and on
Seeed `waitforrefresh()` always returns true (H7), so the commit always happens. A client that does a
normal direct write, learns the etag, then sends a matching 0x76, crashes the device.

Additionally, on Seeed the `dc_pin` field is repurposed as **MISO**
([display_seeed_gfx.cpp:57](../src/display_seeed_gfx.cpp#L57)), so even before the dereference,
`bbepInitIO` bit-bangs onto the IT8951's MISO line.

**Recommendation.** In `handlePartialWriteStart()`, NACK `ERR_PARTIAL_UNSUPPORTED` when
`seeed_driver_used()`, when `globalConfig.display_count == 0`, or when
`mapEpd(displays[0].panel_ic_type) == EP_PANEL_UNDEFINED`. Consider also honoring
`displays[0].partial_update_support`, which the handler currently ignores entirely (M33).

---

## 3. High

### H1. Stale RTC `displayed_etag` survives a hidden reset; partial writes then diff against the boot screen

**Category:** bug (display corruption) · **Path:** both · *Verified.*

`displayed_etag` is `RTC_DATA_ATTR` ([main.h:292](../src/main.h#L292)), so it survives panic, task/int
watchdog, brownout, `esp_restart()` and the 0x000F reboot command — `main.cpp`'s own comment at
[main.cpp:79-81](../src/main.cpp#L79-L81) says exactly this. On a NORMAL BOOT the firmware calls
`initDisplay()`, which **redraws the boot screen over the panel**, but nothing clears
`displayed_etag`. Grep confirms the only writes are in the direct-write and partial-write completion
paths ([display_service.cpp:1800](../src/display_service.cpp#L1800),
[:1804](../src/display_service.cpp#L1804), [:1878-1883](../src/display_service.cpp#L1878-L1883),
[:2186](../src/display_service.cpp#L2186)) — never on the boot path, and never in `setup()`.

**Failure scenario.** Host displays image with etag `X` → device takes a mid-cycle panic/WDT reset (the
exact scenario `docs/FINDINGS_DEEP_SLEEP_WAKE_BOOT_SCREEN_2026-07-07.md` documents as *observed in the
field*) → boot screen is drawn → host sends a partial update with `oldEtag = X` →
[display_service.cpp:1666](../src/display_service.cpp#L1666) accepts it → the rect is diffed against a
panel that now shows the logo and QR code. Corrupted display, and the host believes it is in sync.

Power-on is safe (RTC RAM is cleared, `displayed_etag == 0`); deep-sleep wake is *intentionally* safe
(the panel image genuinely persists). Only the hidden-reset case is wrong — and that is precisely the
case the RTC comment was added to reason about.

**Recommendation.** `displayed_etag = 0;` in the `!is_deep_sleep_wake` branch of `setup()`, or
immediately after the boot-screen refresh.

### H2. BLE config read silently truncates any config larger than ~850 bytes

**Category:** bug (protocol) · **Path:** ESP32, BLE only · *Verified.*

`handleReadConfig()` ([communication.cpp:330-372](../src/communication.cpp#L330-L372)) bulk-sends up
to `maxChunks = (MAX_CONFIG_SIZE + 93) / 94 = 44` chunks in a tight `while` loop, calling
`sendResponse()` for each. On ESP32 `sendResponse()` does **not** transmit — it calls
`esp32_queue_ble_notify_copy()` ([communication.cpp:212](../src/communication.cpp#L212)), which pushes
into `responseQueue[RESPONSE_QUEUE_SIZE = 10]`. That queue is drained only by `loop()`
([main.cpp:168-189](../src/main.cpp#L168-L189)) — which cannot run until `handleReadConfig()` returns.

After 9 queued chunks the ring is full and every subsequent chunk is dropped:

```cpp
uint8_t nextHead = (responseQueueHead + 1) % RESPONSE_QUEUE_SIZE_LOCAL;
if (nextHead == responseQueueTail) { writeSerial("ERROR: Response queue full, dropping response", true); return; }
```
— [communication.cpp:92-95](../src/communication.cpp#L92-L95)

Chunk 0 carries 94 payload bytes (it also carries the 2-byte total-length header), later chunks 96, so
the cliff is at roughly **94 + 8×96 ≈ 862 bytes**. Below it everything works, which is why this has
survived. The `delay(1)` at [communication.cpp:365](../src/communication.cpp#L365) does not drain the
queue.

This is an **unintended ESP32/BLE-only divergence**: the nRF path notifies synchronously
([communication.cpp:228](../src/communication.cpp#L228)) and the ESP32 LAN path
(`send_wifi_lan_frame`) writes synchronously, so both deliver the whole config.

**Recommendation.** Drain the response queue between chunks, send config-read chunks synchronously, or
size `responseQueue` for the worst case (44).

### H3. `reloadConfigAfterSave()` calls blocking `initWiFi()` — up to ~34 s of unresponsive BLE after every config write

**Category:** bug · **Path:** ESP32 · *Verified.*

```cpp
#ifdef TARGET_ESP32
    initWiFi();          // <-- no argument
#endif
```
— [communication.cpp:35](../src/communication.cpp#L35)

`initWiFi` is declared `void initWiFi(bool waitForConnection = true)`
([wifi_service.h:6](../src/wifi_service.h#L6)). Every other caller passes `false`
([main.cpp:110](../src/main.cpp#L110), [main.cpp:302](../src/main.cpp#L302)); this one takes the
default and enters the blocking retry loop at
[wifi_service.cpp:119-146](../src/wifi_service.cpp#L119-L146): 3 retries × 10 s poll + 2 × 2 s backoff
≈ **34 s worst case**.

During that stall `loop()` is blocked, so: the config-write ACK (queued only *after*
`reloadConfigAfterSave` at [communication.cpp:413-415](../src/communication.cpp#L413-L415)) is not
sent, the 5-slot `commandQueue` overflows and silently drops writes (M10), and the response queue
cannot drain. Triggers whenever WiFi is configured and the host writes a config.

**Recommendation.** `initWiFi(false);` — `handleWiFiServer()` already handles late association
asynchronously. (This is the same class of bug as `FINDINGS.md` M11, which was fixed in `setup()` but
missed here.)

### H4. Replay of the most-recent authenticated command is always accepted

**Category:** security (crypto) · *Verified.* · Restates **`FINDINGS.md` H5 — still open.**

```cpp
if (nonce_counter <= encryptionSession.last_seen_counter && counter_diff != 0) {
    /* ...scan replay_window[64]... */
}
if (nonce_counter > encryptionSession.last_seen_counter) { encryptionSession.last_seen_counter = nonce_counter; }
```
— [encryption.cpp:136-150](../src/encryption.cpp#L136-L150)

The `&& counter_diff != 0` guard means a packet whose counter *equals* `last_seen_counter` skips the
already-seen scan entirely and returns `true` at
[encryption.cpp:155](../src/encryption.cpp#L155). Since `last_seen_counter` advances only on a strict
increase, the newest legitimate command — and the initial counter-0 command — can be captured off the
air and replayed indefinitely. The 16-byte nonce (`session_id ‖ counter`) is transmitted in cleartext
with every command, so a passive eavesdropper has everything needed.

Replayable commands include `0x000F` reboot, `0x0052` deep-sleep/power-off, `0x0045` clear-config, and
any config-write chunk.

**Recommendation.** Drop the `&& counter_diff != 0` guard; treat an exact match as a replay unless it
is a genuine new maximum.

### H5. AES-CCM nonce reuse across command and response directions

**Category:** security (crypto) · Restates **`FINDINGS.md` H4 — still open.**

The nonce is `session_id[8] ‖ counter[8]` with no direction/domain-separation bit
([encryption.cpp:158-170](../src/encryption.cpp#L158-L170)). On a new session both the response
counter and the inbound command counter start at 0 ([:194](../src/encryption.cpp#L194),
[:604](../src/encryption.cpp#L604)). Command #0 and response #0 therefore use an identical
`(key, nonce)` pair under the same session key. CCM nonce reuse leaks the keystream XOR of the two
messages and breaks tag unforgeability.

**Recommendation.** Reserve one nonce bit as a direction flag (set for device→host); require it clear
on inbound.

### H6. Replay state is advanced *before* the auth tag is verified

**Category:** security (crypto DoS) · **New.**

`decryptCommand()` calls `verifyNonceReplay()` ([encryption.cpp:640](../src/encryption.cpp#L640)) —
which mutates `last_seen_counter` and writes `replay_window[]`
([:149-154](../src/encryption.cpp#L149-L154)) — **before** `aes_ccm_decrypt()` at
[:663](../src/encryption.cpp#L663). Because `session_id` is observable on the wire, an attacker can
forge frames that pass the `session_id` compare at [:118](../src/encryption.cpp#L118) with any
in-window counter. Each forged frame pollutes the replay ring and, on the inevitable decrypt failure,
increments `integrity_failures`; three of them clear the session
([:641-646](../src/encryption.cpp#L641-L646), [:678-682](../src/encryption.cpp#L678-L682)). A cheap
unauthenticated DoS that forces re-authentication at will. (Compounds `FINDINGS.md` L4, still open.)

**Recommendation.** Verify the CCM tag first; update replay state only on success.

### H7. On the Seeed path `waitforrefresh()` always returns true — refresh failures are reported as success and commit an etag

**Category:** bug · **Path:** Seeed GFX · *Verified.*

```cpp
bool waitforrefresh(int timeout){
#if defined(TARGET_ESP32) && defined(OPENDISPLAY_SEEED_GFX)
    if (seeed_driver_used()) return seeed_gfx_wait_refresh(timeout);
#endif
    /* ...real busy-poll... */
}
```
— [display_service.cpp:320-323](../src/display_service.cpp#L320-L323)

```cpp
bool seeed_gfx_wait_refresh(int timeout_sec) { (void)timeout_sec; delay(300); return true; }
```
— [display_seeed_gfx.cpp:123-127](../src/display_seeed_gfx.cpp#L123-L127)

The timeout argument is discarded and the panel is never consulted. Consequences: the 0x74
"refresh timeout" response is **unreachable** on Seeed; `handleDirectWriteEnd()` always takes the
`refreshSuccess` branch and commits `displayed_etag = newEtag`
([display_service.cpp:1878](../src/display_service.cpp#L1878)); and the TCON busy-timeout flag
(`opnd_seeed_tcon_busy_timed_out`) is never consulted here, so a wedged TCON reports success forever.
This is the enabler for C1's etag precondition and masks H8 and H11.

**Recommendation.** Have `seeed_gfx_wait_refresh()` return `!opnd_seeed_tcon_busy_timeout_occurred()`
at minimum, and poll the IT8951 `LUTAFSR` register with a real deadline.

### H8. Seeed re-init is gated on a once-per-boot flag that a panel power-cycle never clears

**Category:** bug · **Path:** Seeed GFX · *Verified* (symptom scope depends on `pwr_pin` being set —
*needs-verification* on hardware).

`seeed_gfx_hw_initialized` is set `true` in `seeed_gfx_epaper_begin()`
([display_seeed_gfx.cpp:116](../src/display_seeed_gfx.cpp#L116)) and — grep-confirmed — is **never set
back to false**. `seeed_gfx_direct_write_reset()` only does a full `begin()` when the flag is false;
otherwise it takes the `wake()` branch
([display_seeed_gfx.cpp:145-164](../src/display_seeed_gfx.cpp#L145-L164)), whose own comment states
that `wake()` alone "would leave the TCON unconfigured (garbled/blank first refresh or a busy stall)"
after a power cycle.

But the rail *is* cycled within a boot session. After the boot screen and after every direct write,
`pwrmgm(false)` runs ([display_service.cpp:1159](../src/display_service.cpp#L1159),
[:1550-1551](../src/display_service.cpp#L1550-L1551)). With `pwr_pin != 0xFF` that calls
`configureDisplayPinsLowPower()` ([main.cpp:354-381](../src/main.cpp#L354-L381)), which drives
`pwr_pin_2` and `pwr_pin_3` LOW — and on Seeed those two are exactly `TFT_ENABLE` and `ITE_ENABLE`,
the panel and TCON power enables ([display_seeed_gfx.cpp:64-66](../src/display_seeed_gfx.cpp#L64-L66)).
The subsequent `pwrmgm(true)` Seeed branch raises only `pwr_pin` and `reset_pin`
([main.cpp:452-458](../src/main.cpp#L452-L458)) — never `TFT_ENABLE`/`ITE_ENABLE`, which are raised
only inside the library's `init()`/`initFromSleep()`.

**Failure scenario.** The second image push of a boot session talks to an unpowered TCON over
detached SPI pins, `tconWaitForReady` stalls for its 15 s timeout (blocking the BLE command loop),
latches the busy flag, and then — because of H7 — reports 0x73 success and commits the etag.

Contrast the bb_epaper path, which re-runs `bbepInitIO + bbepWakeUp + pInitFull` on **every**
direct-write start ([display_service.cpp:1620-1624](../src/display_service.cpp#L1620-L1624)).

**Recommendation.** Clear `seeed_gfx_hw_initialized` in `pwrmgm(false)`, or key re-init off
`displayPowerState` rather than a boot-scoped flag. Also leave the flag false when `begin()` times out
(currently set unconditionally at [:115-116](../src/display_seeed_gfx.cpp#L115-L116), so no retry ever
happens).

### H9. `updatemsdata()`'s change-detection can never fire; advertising is torn down and rebuilt on every event

**Category:** bug (dead code) + inefficiency (battery/radio) · **Path:** both · *Verified.*

`statusByte` embeds the rolling sequence nibble and is written into `msd_payload[15]`:

```cpp
uint8_t statusByte = ... | ((mloopcounter & 0x0F) << 4);
...
msd_payload[15] = statusByte;
```
— [display_service.cpp:1301-1311](../src/display_service.cpp#L1301-L1311)

The unchanged-payload early-out then compares that same buffer against the previous one:

```cpp
static uint8_t prev_msd_payload[16] = {0xFF};
if (memcmp(prev_msd_payload, msd_payload, 16) == 0) { mloopcounter++; mloopcounter &= 0x0F; return; }
```
— [display_service.cpp:1331-1336](../src/display_service.cpp#L1331-L1336) (nRF equivalent at
[:1313-1318](../src/display_service.cpp#L1313-L1318))

`mloopcounter` increments once per call ([:1369-1370](../src/display_service.cpp#L1369-L1370)), so
byte 15 always differs from the stored copy by one nibble. **The `memcmp` can never be zero**, the
early-return branch is unreachable, and its internal increment is dead.

Every `updatemsdata()` therefore takes the full path — `pAdvertising->stop()`, rebuild
`BLEAdvertisementData`, `delay(50)`, `pAdvertising->start()`
([:1350-1363](../src/display_service.cpp#L1350-L1363)) — on every button press, every touch coordinate
change (up to 10 Hz), every BLE connect callback, the redundant `setup()` call
([main.cpp:112](../src/main.cpp#L112)), and every 60 s idle tick
([main.cpp:258-262](../src/main.cpp#L258-L262)). The radio goes dark ≥50 ms each time. The same nibble
also defeats the 400 ms mDNS throttle's `memcmp` ([wifi_service.cpp:59-61](../src/wifi_service.cpp#L59-L61)).

Note the nRF-side `memcmp` was *added* as an optimization (`FINDINGS.md` M9, commit `daf5e83`) and the
RTC persistence of `mloopcounter` was added separately (commit `062cba3`); together they cancel out.

**Recommendation.** Compare with byte 15's high nibble masked out, and increment the sequence nibble
only when the rest of the payload actually changed.

### H10. Heavy hardware work runs in the Bluedroid callback task, racing `loop()`

**Category:** bug (cross-task race) · **Path:** ESP32 · Restates **`FINDINGS.md` M2 — still open**
(raised to High: independently re-derived by two readers, with the SPI-teardown-mid-write mechanism
now traced).

`onConnect()` calls `updatemsdata()` directly ([esp32_ble_callbacks.h:48](../src/esp32_ble_callbacks.h#L48)),
which performs I2C sensor reads, an ADC sample, an advertising rebuild and an mDNS update — all of
which also run from `loop()` with no mutex.

`onDisconnect()` calls `cleanupDirectWriteState(true)` when `directWriteActive && !epdRefreshInProgress`
([esp32_ble_callbacks.h:53-58](../src/esp32_ble_callbacks.h#L53-L58)). That path runs `bbepSleep()`,
`delay(200)`, and `pwrmgm(false)` → `SPI.end()` / `Wire.end()` / GPIO reconfiguration
([main.cpp:460-473](../src/main.cpp#L460-L473)) **from the BT host task**, while `loop()` may still be
draining queued 0x71 chunks and calling `bbepWriteData()` on the same SPI bus
([main.cpp:160-167](../src/main.cpp#L160-L167)). The `epdRefreshInProgress` guard covers only the
refresh phase, not the upload phase. A link drop mid-upload can tear the SPI peripheral out from under
an in-flight write.

**Recommendation.** Callbacks should set a `*_pending` flag and let `loop()` do the work — the pattern
`bleRestartAdvertisingPending` already establishes.

### H11. Seeed 4-gray refresh is never awaited; the panel is slept (and the rail cut) mid-refresh

**Category:** bug · **Path:** Seeed GFX, 4-gray (`3001`) · *needs-verification on hardware; the code
asymmetry is objective.*

For 1bpp, `EPaper::update()` blocks until the refresh completes (`EPD_UPDATE` →
`tconDisplayArea1bpp` → `tconWaitForDisplayReady`). For gray mode, `EPD_UPDATE_GRAY` →
`tconDisplayArea` **returns without waiting**, and `update()` immediately issues `sleep()`/`tconSleep`
(`lib/Seeed_GFX/Extensions/EPaper.cpp:67`).

The firmware's only wait is `seeed_gfx_wait_refresh()`'s fixed `delay(300)` (H7), after which
`handleDirectWriteEnd()` reports success and `cleanupDirectWriteState()` calls `pwrmgm(false)`
([display_service.cpp:1854-1868](../src/display_service.cpp#L1854-L1868),
[:1550-1551](../src/display_service.cpp#L1550-L1551)). A GC16 refresh on this panel takes 1–2 s.

Additionally, `seeed_gfx_direct_refresh()`'s second full-mode `update()`
([display_seeed_gfx.cpp:179-184](../src/display_seeed_gfx.cpp#L179-L184)) re-loads image data while the
first GC16 pass may still be running; the IT8951 requires a `LUTAFSR` wait before the next load.

**Recommendation.** Poll `LUTAFSR` with a bounded deadline after gray updates before sleeping or
reporting success.

### H12. Partial write is accepted on BWR/BWY panels and paints the old image into the accent plane

**Category:** bug (display corruption) · **Path:** bb_epaper.

The gate is `getBitsPerPixel() != 1`, but BWR and BWY return `1` (only `BWGBRY`, `BWRY` and `GRAY4`
return >1 — [display_service.cpp:1219-1221](../src/display_service.cpp#L1219-L1221)). On a 3-color
panel, `partial_write_stream_bytes` streams the "old image" rect into `PLANE_1`
([display_service.cpp:2100-2121](../src/display_service.cpp#L2100-L2121)), which on BWR/BWY hardware is
the **red/yellow plane**. The old-image bits are rendered as accent color. Separately, `bbepRefresh`
forces `REFRESH_FAST` for 3-color SSD panels, so "partial" is fictional there anyway.

**Recommendation.** Exclude `COLOR_SCHEME_BWR` / `COLOR_SCHEME_BWY` from partial support explicitly
(the same guard block that fixes C1).

---

## 4. Medium

### Deep-sleep wake branch

**M1. Dead-end state when `enterDeepSleep()` declines after the wake window times out.**
[main.cpp:151-155](../src/main.cpp#L151-L155) clears `advertising_timeout_active` *before* calling
`enterDeepSleep()`. If that function returns early because `power_mode != 1` or
`deep_sleep_time_seconds == 0` ([main.cpp:321-330](../src/main.cpp#L321-L330) — reachable if the stored
config changed since the sleep was armed), the device drops into the normal loop with
`woke_from_deep_sleep` still true. `fullSetupAfterConnection()` is then permanently unreachable (its
only call site requires `advertising_timeout_active`, [main.cpp:133-137](../src/main.cpp#L133-L137)), so
`bbep` is never given a panel type — the wake path skipped `initDisplay()` — and WiFi is never
initialized. A later direct write runs `bbepSendCMDSequence(&bbep, bbep.pInitFull)` with a NULL
sequence. *Recommendation:* on decline, re-arm the window or run the full-setup path.

**M2. The post-wake advertising window never services buttons, touch, or power-off polling.**
The branch at [main.cpp:133-159](../src/main.cpp#L133-L159) ends in `delay(50); return;` and never calls
`processButtonEvents()` / `processTouchInput()` — yet `setup()` deliberately initializes both on the
wake path ([main.cpp:113-116](../src/main.cpp#L113-L116)). Consequences: button presses during the
window update ISR state but are lost at `enterDeepSleep()` (not RTC-backed), and long-press power-off
(`powerButtonPoll`, `pollConfiguredPowerOffButtons`) is inoperative for the whole window (up to ~65 s).
Only `processLedFlash()` runs. *Recommendation:* call both before the `delay(50)`.

**M3. Idle→deep-sleep entry is not atomic with the idle check.** `bleActive` is computed at
[main.cpp:224-229](../src/main.cpp#L224-L229), then `enterDeepSleep()` runs at
[:253](../src/main.cpp#L253). A BLE connect, or a command enqueued by the BT task, in that window is
silently destroyed by `BLEDevice::deinit(true)` ([:341](../src/main.cpp#L341)). `enterDeepSleep()` never
re-checks `getConnectedCount()` or queue emptiness.

**M4. `enterDeepSleep()` performs no peripheral shutdown besides BLE.** It never drives the GT911
`enable_pin` low, never stops WiFi on the cold-boot-then-sleep path (started at
[main.cpp:110](../src/main.cpp#L110)), and leaves `checkResetPin` pullups configured. *needs-verification:*
measure deep-sleep current with touch configured vs not.

**M5. Disconnect cleans up direct write but not partial write.** `onDisconnect`
([esp32_ble_callbacks.h:53-58](../src/esp32_ble_callbacks.h#L53-L58)) checks only `directWriteActive`. A
disconnect mid-partial leaves the panel rail powered (`partial_prepare_panel_ram` did `pwrmgm(true)`)
until `checkPartialWriteTimeout()` fires 15 minutes later. On nRF there is no timeout at all (see M6).

**M6. Direct-write and partial-write timeouts are ESP32-only.** Both checks sit inside the
`#ifdef TARGET_ESP32` block ([main.cpp:193-200](../src/main.cpp#L193-L200)); the nRF loop is
[main.cpp:266-277](../src/main.cpp#L266-L277). On nRF a stalled stream leaves `directWriteActive` /
`partialCtx.active` set and the panel rail on indefinitely. *Recommendation:* move both to common loop
code.

**M7. `powerLatchBegin()` never asserts the latch pin in `DEVICE_FLAG_BATTERY_LATCH` mode.**
[power_latch.cpp:110-122](../src/power_latch.cpp#L110-L122) calls `gpio_hold_dis(latchPin())` and then
configures only the button pin — it never drives `pwr_pin_2` HIGH. The DFF path re-engages immediately
by contrast ([:52-61](../src/power_latch.cpp#L52-L61)). *needs-verification:* if the hardware is truly
self-holding this is benign and the sleep-hold is anti-float insurance; otherwise every wake browns out
at config load. The asymmetry is unexplained either way.

**M8. `powerOff()` spins forever waiting for button release.**
[power_latch.cpp:85-90](../src/power_latch.cpp#L85-L90): `while (digitalRead(buttonPin()) == LOW) delay(20);`
with no deadline and no watchdog feed. A stuck or shorted button hangs the device instead of powering
it off.

**M9. `printConfigSummary()` runs on every deep-sleep wake.** `full_config_init()` →
`loadGlobalConfig()` → `printConfigSummary()` emits ~150 `String`-concatenated UART lines
([config_parser.cpp:625+](../src/config_parser.cpp#L625)) during the power-sensitive wake bring-up. At
115200 baud this is meaningful awake-time energy per cycle.

**M10. Boot LED flash runs on every ESP32 wake but is VBUS-gated only on nRF.** `initio()` runs
unconditionally on the wake path ([main.cpp:88](../src/main.cpp#L88)); the 4-color `flashLed(…,15)`
sequence busy-waits ~10 ms each. The `#ifdef TARGET_NRF if (nrfVbusPresent())` gate at
[display_service.cpp:545-548](../src/display_service.cpp#L545-L548) shows the battery-saving intent
exists but was not applied to ESP32.

**M11. Redundant `updatemsdata()` in `setup()`.** `ble_init_esp32(true)` already calls it and starts
advertising; `setup()` calls it again at [main.cpp:112](../src/main.cpp#L112). Because of H9 this stops
advertising, rebuilds, `delay(50)`, restarts — on every boot *and every wake* — plus a duplicate round
of sensor polling.

### Display pipeline (bb_epaper)

**M12. `waitforrefresh()` false-fails on panels with no BUSY pin and on sub-100 ms refreshes.**
[display_service.cpp:324-341](../src/display_service.cpp#L324-L341). `bbepIsBusy` returns `false`
immediately when `busy_pin == 0xFF`, so `i == 0` triggers "Epaper not busy after refresh command" →
`return false` → the client receives 0x74 and `displayed_etag` is cleared, even though the refresh
proceeds blind. Same false negative for a fast partial refresh. (Restates `FINDINGS.md` L6, still open,
with the `busy_pin == 0xFF` case added.)

**M13. `bbepSetPanelType()`'s return value is ignored.** [display_service.cpp:1170](../src/display_service.cpp#L1170),
[main.cpp:314](../src/main.cpp#L314). `mapEpd` returns `EP_PANEL_UNDEFINED` for `0x0000`, `0x0041`,
`0x0042`, all Seeed ids and any unknown value; `bbepSetPanelType` then leaves `bbep` zeroed. Later calls
silently no-op or crash (C1).

**M14. Single-plane uncompressed uploads can refresh stale RAM and commit an etag.** The completeness
check `directWriteBytesWritten == directWriteTotalBytes` at
[display_service.cpp:1819-1830](../src/display_service.cpp#L1819-L1830) applies only to two-plane
streams. A 1bpp MONO client that sends 0x70 then an early 0x72 (with etag) refreshes whatever is in
panel RAM and commits `displayed_etag = newEtag` — exactly the "stale etag base" the comment at
[:1873-1877](../src/display_service.cpp#L1873-L1877) tries to prevent.

**M15. On ESP32 the "early" 0x72 ACK is not delivered until the refresh completes.** `sendResponse`
only queues BLE notifies; the queue is drained by `loop()`, which is the very thread blocked inside
`handleDirectWriteEnd` / `partial_write_to_panel` for up to ~60 s of `waitforrefresh`. BLE clients see
the 0x72 ACK and the 0x73/0x74 result back-to-back after the refresh, while LAN and nRF clients get the
ACK first. Any BLE client with an ACK timeout shorter than the refresh will falsely time out. (Same
root cause as H2.)

**M16. `commandQueue` overflow is a silent drop with no NACK.**
[esp32_ble_callbacks.h:91-100](../src/esp32_ble_callbacks.h#L91-L100). The characteristic advertises
`WRITE_NR`, so a client streaming write-without-response gets no indication of loss. During a blocking
60 s refresh the BT task fills all 5 slots and then drops — corrupting an image stream.

**M17. No `display_count` check in `handleDirectWriteStart` / `handlePartialWriteStart`.** With no
display configured, `displays[0]` is all zeros: width/height 0 → `directWriteTotalBytes == 0` → the
first 0x71 instantly auto-triggers `handleDirectWriteEnd` and a refresh on a zeroed `bbep`.

**M18. Partial write never suspends touch; direct write does.** `handleDirectWriteStart` calls
`touchSuspendForEpdRefresh()` ([display_service.cpp:1564](../src/display_service.cpp#L1564)); the
partial path refreshes with GT911 polling live. Whatever bus/EMI reason justified the direct-write
suspend applies equally.

**M19. `partial_update_support` is never checked.** It is parsed and shown on the boot screen
([boot_screen.cpp:783](../src/boot_screen.cpp#L783)) but no handler consults it — the protocol relies
on clients honoring the advertisement. This also lets C1 trigger on configs that declare no partial
support.

**M20. The panel init sequence is sent twice per session.** `bbepInitIO()` internally ends with
`bbepSendCMDSequence(pInitFull)`; the firmware then calls `bbepWakeUp()` (hardware reset, discarding
it) and re-sends the sequence ([display_service.cpp:1620-1622](../src/display_service.cpp#L1620-L1622),
[:2145-2148](../src/display_service.cpp#L2145-L2148)). Every direct write, partial write and boot
session pays one wasted full init sequence including BUSY waits.

### Seeed GFX path

**M21. TCON busy-timeout flag is latched for the rest of the boot and consulted only at boot-screen
init.** `opnd_seeed_tcon_busy_timed_out` makes all subsequent `tconWaitForReady()` calls no-ops. It is
reset only in `seeed_gfx_epaper_begin()` ([display_seeed_gfx.cpp:106](../src/display_seeed_gfx.cpp#L106))
and read only in `initDisplay` ([display_service.cpp:1144-1147](../src/display_service.cpp#L1144-L1147)).
Not checked after `seeed_gfx_direct_write_reset()`'s first-begin on the wake path, nor after
`seeed_gfx_direct_refresh()`, nor in `seeed_gfx_wait_refresh()` (H7).

**M22. `tconWaitForDisplayReady()` has no timeout and no yield** (`lib/Seeed_GFX/Extensions/Tcon.cpp:521-523`,
`while (tconReadReg(LUTAFSR));`). The OpenDisplay busy-timeout patch covers `tconWaitForReady` only. On
the 1bpp Seeed panel every `update()` funnels through it; a wedged TCON returning nonzero on MISO hangs
`loopTask` forever. *needs-verification:* depends on MISO idle level with a dead TCON.

**M23. Boot-screen render failure is ignored on the Seeed branch.** `initDisplay`'s Seeed branch
discards `writeBootScreenWithQr()`'s return ([display_service.cpp:1152](../src/display_service.cpp#L1152))
and refreshes anyway; the bb branch checks it and skips the refresh, and has a battery retry the Seeed
branch lacks. Failure is realistic — `boot_screen.cpp:892-894` returns false when
`rowBytesNeeded > sizeof(staticRowBuffer)`.

**M24. "Full refresh" means two different things on the two Seeed call sites.** Boot screen full
refresh = one `update()` ([display_seeed_gfx.cpp:119-121](../src/display_seeed_gfx.cpp#L119-L121));
client `REFRESH_FULL` direct write = two `update()`s
([:179-184](../src/display_seeed_gfx.cpp#L179-L184)) — a double push of a 1.3 MB frame and a double
GC16. Either the boot screen misses the anti-ghosting pass or the direct write pays 2× time and energy.

### Build configuration

**M25. `-DCONFIG_FREERTOS_WATCHDOG_TIMEOUT_S=120` is inert.** Set on six ESP32 envs
([platformio.ini:47](../platformio.ini#L47), 71, 95, 121, 146, 163, 183, 204). It is an sdkconfig
option baked into the precompiled ESP-IDF libraries shipped with the Arduino framework; a `-D` flag
cannot change it, and grep confirms no project code reads it. The intended 120 s watchdog extension does
not happen. Given the blocking 60 s `waitforrefresh` and 15 s TCON stalls, this matters.

**M26. C3/C6 envs use the registry `espressif32` platform; every S3 env uses pioarduino.**
[platformio.ini:138](../platformio.ini#L138), [:155](../platformio.ini#L155), [:175](../platformio.ini#L175)
vs [:38](../platformio.ini#L38) etc. The registry platform's Arduino core does not support ESP32-C6, so
`esp32-c6-N4` is very likely unbuildable; and the two platform families ship different Arduino core
majors, meaning different BLE stacks and different GPIO/deep-sleep APIs across envs of the same
firmware. *needs-verification:* PlatformIO is not installed here, so this was not build-tested.
*Recommendation:* standardize on pioarduino.

**M27. `pinMode(7, LOW)` in `ws_pp_init()`.** [main.cpp:521](../src/main.cpp#L521). `LOW` is `0`, not a
pin mode; the output driver stays disabled, so the following `digitalWrite(7, LOW)` does not drive the
pin — GPIO7 floats. Every other driven pin in the function correctly uses `OUTPUT` (e.g.
[:518-519](../src/main.cpp#L518-L519)). Affects boards with `DEVICE_FLAG_WS_PP_INIT`.

### Communication / config / peripherals

**M28. `SECURITY_FLAG_REWRITE_ALLOWED` is non-functional; its enforcement branches are dead.** The
dispatcher rejects **all** commands with status `0xFE` when encryption is enabled and the peer is not
authenticated, except `0x0050` and `0x0043`
([communication.cpp:509-515](../src/communication.cpp#L509-L515)) — and both config-write handlers are
reached only from the switch at [:562](../src/communication.cpp#L562), *after* that gate. Therefore the
`rewriteAllowed` checks at [:376-384](../src/communication.cpp#L376-L384) and
[:438-447](../src/communication.cpp#L438-L447) are unreachable and their `secureEraseConfig()` never
runs. The flag documented in [structs.h:316-330](../src/structs.h#L316-L330) has no effect. This is
**fail-secure** — see [Ruled out](#ruled-out-checked-and-found-not-to-be-defects) — but the feature is
broken and the code is dead. *Recommendation:* honor the flag in the dispatcher gate, or remove flag and
branches.

**M29. A single global `encryptionSession` is shared by the BLE and LAN transports.**
`wifi_service.cpp:163,191,204` clears the one global session on LAN connect/replace/disconnect, and LAN
frames feed the same `imageDataWritten`. A LAN connection can tear down a session a BLE central
authenticated, and authenticated state on one transport authorizes commands on the other. Nothing
enforces one-transport-at-a-time.

**M30. `send_wifi_lan_frame()` uses a blocking `wifiClient.write()` in `loop()`.**
[communication.cpp:73-81](../src/communication.cpp#L73-L81), called from `sendResponse` for every
response. `WiFiClient::write` blocks up to the 30 s socket timeout if the TCP send buffer is full,
stalling all BLE processing.

**M31. Negotiated ATT MTU is never verified.** `BLEDevice::setMTU(512)`
([ble_init.cpp:198](../src/ble_init.cpp#L198)) is a request. Notifications are sent at full length
([main.cpp:174-175](../src/main.cpp#L174-L175)); a client negotiating the 23-byte default receives
silently truncated notifications. Config-read chunk sizing implicitly assumes MTU ≥ ~132.

**M32. The TLV parser ignores the per-packet wire length field.** [config_parser.cpp:290](../src/config_parser.cpp#L290)
does `offset++` discarding the length byte and advances by hardcoded `sizeof(struct X)`; an unknown
packet id sets `offset = configLen - 2` and drops the rest of the config
([:605-608](../src/config_parser.cpp#L605-L608)); the trailing CRC-16 check
([:611-620](../src/config_parser.cpp#L611-L620)) is advisory and still sets `loaded = true`. Schema drift
between toolbox and firmware silently desyncs the parse. All reads stay in bounds, so this is corruption
of *meaning*, not memory. (Restates `FINDINGS.md` M6 — partially fixed: `ff3656f` added the CRC but made
it non-fatal.) Only `SensorData`, `TouchController` and `PassiveBuzzerConfig` have size `static_assert`s.

**M33. `FlashConfig` (packet 0x2B) is parsed and stored on ESP32 but never used.** Its only consumer,
`powerDownExternalFlashFromConfig()`, is an empty stub on ESP32
([main.cpp:594](../src/main.cpp#L594)) and its call site is nRF-gated. Fields `mode`, `bus_instance`,
`flash_ic_type`, `power_pin`, `power_active`, `power_on_delay_ms`, `power_off_delay_ms` are referenced on
*no* target.

**M34. `checkResetPin` has no validity guard on `securityConfig.reset_pin`.**
[encryption.cpp:802,811,825](../src/encryption.cpp#L802). With the reset-pin flag set and
`reset_pin == 0xFF` or an out-of-range GPIO (both BLE-supplied), it calls `pinMode(255, …)` /
`digitalRead(255)` on every boot, plus an unconditional `delay(100)`.

**M35. `handleLedActivate` uses stale `led->reserved` when `len < 13`.**
[device_control.cpp:376-379](../src/device_control.cpp#L376-L379). `mode` is then derived from whatever
was previously in `reserved[0]`. Reject short activate payloads.

**M36. The `EPaper` global constructor allocates a ~329 KB 1bpp PSRAM sprite at static-init in every
`OPENDISPLAY_SEEED_GFX` build**, including on S3 devices actually configured for bb_epaper panels; it is
then freed and reallocated at 4bpp (~1.31 MB) for 4-gray panels.

---

## 5. Low / Info

### Dead code

- `renderChar_4BPP` / `_2BPP` / `_1BPP` — static, no callers
  ([display_service.cpp:1059-1132](../src/display_service.cpp#L1059-L1132)).
- `writeTextAndFill` — declared ([display_service.h:31](../src/display_service.h#L31)), never defined.
- `writelineFont` — extern'd ([main.h:383](../src/main.h#L383)), no definition, no use.
- `staticWhiteRow[680]` and `staticLineBuffer[256]` — 936 B of `.bss`, reachable only by dead externs
  ([main.h:145,147](../src/main.h#L145)).
- `bleResponseBuffer[94]` — defined, never referenced anywhere ([main.h:124](../src/main.h#L124)).
- `directWriteDataKind` — zero references; its comment ("display_service.cpp tracks full vs partial")
  is false. `directWriteRefreshMode` — only ever reset to 0, never read ([main.h:177-178](../src/main.h#L177-L178)).
- `connectionRequested` — never written; MSD status bit 2 is permanently 0 ([main.h:133](../src/main.h#L133)).
- `seeed_gfx_boot_skip_planes()` — empty no-op ([display_seeed_gfx.cpp:142-143](../src/display_seeed_gfx.cpp#L142-L143)).
- `src/driver.h` — an empty header (include guard only).
- `if(false)` block in `powerDownExternalFlash` ([main.cpp:653-671](../src/main.cpp#L653-L671)).
- `powerDownExternalFlash` always returns `false`; its sole caller ignores the result.
- `bleDrain < 16` can never bind — `RESPONSE_QUEUE_SIZE` is 10 ([main.cpp:171](../src/main.cpp#L171)).
- `case 0x0043` in the dispatch switch ([communication.cpp:586-589](../src/communication.cpp#L586-L589))
  — handled by the early return at [:502-506](../src/communication.cpp#L502-L506).
- `bootLinesFull` / `bootLinesReduced` are identical arrays; `useZoneLayout ? 5u : 5u`
  ([boot_screen.cpp:725-732](../src/boot_screen.cpp#L725-L732)).
- `uint32_t pixels` computed and never used ([display_service.cpp:1577](../src/display_service.cpp#L1577)).
- `esp32BleNotifySubscribed` is write-only in the Bluedroid build (`onSubscribe` compiles only under
  `CONFIG_NIMBLE_ENABLED`).
- `pRxCharacteristic` is only ever assigned `= pTxCharacteristic` and never read.
- `DISABLE_USB_SERIAL` is referenced in three places ([main.cpp:47,480,493](../src/main.cpp#L47)) but
  defined in **no** environment — so the `writeSerial` compile-out never happens and the `String`
  arguments are always built and always printed.
- Inner `#ifdef TARGET_ESP32 / #else` for `wifiLanSession` sits inside the outer ESP32-only block; the
  `#else` arm is dead ([main.cpp:219-223](../src/main.cpp#L219-L223)).
- `expectedLogicalSize == 0` is unreachable ([display_service.cpp:1696-1700](../src/display_service.cpp#L1696-L1700))
  and sends a bare `{0xFF,0x76}` rather than the structured NACK used everywhere else.
- `mapEpd` cases `0x0041` / `0x0042` map to `EP_PANEL_UNDEFINED`; `0x0042` has an explanatory comment,
  `0x0041` is a silent unsupported id.
- `TRANSMISSION_MODE_*` ([structs.h:91](../src/structs.h#L91)) are never enforced — `handleDirectWriteStart`
  accepts 0x70 regardless of the display's declared `transmission_modes`.
- Fallback `#define TRANSMISSION_MODE_DIRECT_WRITE` in
  [display_seeed_gfx.cpp:18-20](../src/display_seeed_gfx.cpp#L18-L20) — `structs.h` always defines it and
  the macro is unused in that file.
- `nrfVbusPresent()` is stubbed to `false` on nRF (real check commented out) and `true` elsewhere, so the
  `initDisplay` battery-retry is dead on ESP32 ([display_service.cpp:114-122](../src/display_service.cpp#L114-L122)).
- `woke_from_deep_sleep = true;` in `enterDeepSleep()` ([main.cpp:332](../src/main.cpp#L332)) is a dead
  store — `setup()` unconditionally rewrites it from the live wake cause, and `esp_deep_sleep_start()`
  never returns. Its `RTC_DATA_ATTR` is pointless for the same reason.
- `shaStr != "\"\""` can never be true after the quote-stripping above it
  ([main.cpp:58](../src/main.cpp#L58)); `!= ""` duplicates `length() > 0`.
- `replay_window_index` is a function-static never reset on a new session
  ([encryption.cpp:152](../src/encryption.cpp#L152)).

### Structural / code health

- **Globals are *defined* (not declared) in `main.h`** — `bbep`, `globalConfig`, `chunkedWriteState`,
  the RTC variables, both BLE queues, `pServer`… ([main.h:108,123-183,281-311,363-380](../src/main.h#L108)).
  This links only because exactly one TU includes the header. Every other TU re-declares these with
  ad-hoc local `extern`s — which is precisely how the `ButtonState` ODR violation (`FINDINGS.md` H2,
  since fixed) happened. `chunked_write_state_t` ([communication.cpp:44-52](../src/communication.cpp#L44-L52))
  and `ResponseQueueItem` ([:60-64](../src/communication.cpp#L60-L64)) are *still* re-typed with
  hardcoded `4096` / `512` / `10` literals. **This is the single highest-leverage structural fix in the
  tree.**
- Duplicate `#include <WiFi.h>` in two `#ifdef TARGET_ESP32` blocks ([main.h:94,154](../src/main.h#L94)).
- Duplicate `extern` of `advertising_timeout_active` / `advertising_start_time`
  ([main.h:104-105](../src/main.h#L104-L105) and [:197-198](../src/main.h#L197-L198)); the comment calls
  them "RTC memory variables … declared in main.cpp" — they are neither.
- `extern chunked_write_state_t chunkedWriteState;` immediately followed by its own definition
  ([main.h:280-281](../src/main.h#L280-L281)).
- `BUILD_VERSION` / `SHA` / `XSTRINGIFY` macro block duplicated verbatim in
  [main.h:12-21](../src/main.h#L12-L21) and [communication.cpp:110-118](../src/communication.cpp#L110-L118).
- The AXP2101 register map is defined twice ([main.h:314-342](../src/main.h#L314-L342),
  [display_service.cpp:188-210](../src/display_service.cpp#L188-L210)); `FONT_*` twice
  ([main.h:39-41](../src/main.h#L39-L41), [display_service.cpp:211-213](../src/display_service.cpp#L211-L213));
  `DEVICE_FLAG_*` / `COMM_MODE_*` twice ([main.h:61-71](../src/main.h#L61-L71),
  [config_parser.cpp:20-31](../src/config_parser.cpp#L20-L31)).
- Board-specific GPIOs hardcoded in generic, config-driven code: `initAXP2101` toggles GPIO21
  ([display_service.cpp:641-645](../src/display_service.cpp#L641-L645)); `pwrmgm` drives GPIO47/48 HIGH on
  power-down ([main.cpp:408-411](../src/main.cpp#L408-L411)).
- `sleep_timeout_ms` ([structs.h:47](../src/structs.h#L47)) means the post-wake advertising window on
  ESP32 but the per-iteration idle delay on nRF. One config field, two semantics.
- `resetReasonName()` lacks the newer IDF codes (USB, JTAG, EFUSE, PWR_GLITCH, CPU_LOCKUP) → logged as
  "UNKNOWN", though the numeric value is also printed.
- `rebootFlag = 1` on every wake (globals re-init), so MSD bit 1 reads as "rebooted" after every sleep
  cycle until a connect. *needs-verification* of host semantics.
- Stale comment: `writeGray4PlaneRow` says "well within the 680B buffer"
  ([boot_screen.cpp:497-502](../src/boot_screen.cpp#L497-L502)) but `staticRowBuffer` is
  `BOOT_ROW_BUFFER_SIZE = 960`; 680 refers to the unrelated dead `staticWhiteRow`.
- `main.h:176`'s comment for `directWriteTotalBytes` ("per plane (for bitplanes)") contradicts the code,
  which stores the two-plane total ([display_service.cpp:1578](../src/display_service.cpp#L1578)).
- Mixed endianness inside one protocol family: the 0x70 decompressed-size field is read little-endian via
  `memcpy` ([display_service.cpp:1597](../src/display_service.cpp#L1597)) while 0x76 etags/rect and the
  0x72 etag use `parse_be_u32`.
- `directWriteCompressed = (len >= 4)` ([display_service.cpp:1574](../src/display_service.cpp#L1574)) is a
  transport heuristic, not a flag. Any future extra start field misparses.
- `platformio.local.ini` is referenced by `[platformio] extra_configs`
  ([platformio.ini:2-3](../platformio.ini#L2-L3)) but is gitignored and absent. *needs-verification* of
  whether PlatformIO errors or silently skips a missing `extra_configs` entry.
- `esp32-s3-N16R8-extuart` sets `OPENDISPLAY_LOG_UART` *and* `ARDUINO_USB_CDC_ON_BOOT=1`; `Serial` is
  never begun, so USB CDC enumerates but is unused. `esp32-s3-N32R8-extuart` correctly sets both USB flags
  to 0.

### Behavioral

- **Zlib window: the comment contradicts the configuration.** `platformio.ini:22-24` says "Existing
  targets pin 32 KB zlib windows for backwards compatibility with legacy clients", but
  `-DOPENDISPLAY_ZLIB_WINDOW_BITS=15` is commented out in **every** env, so all builds use the default of
  9 ([uzlib.h:21-29](../lib/uzlib/src/uzlib.h#L21-L29)) and `od_zlib_stream.c:641-644` hard-rejects any
  zlib header declaring a window > 2^9. A client compressing with stock `zlib.compress()` (wbits=15) has
  **every** compressed direct/partial write NACKed at the first chunk. *needs-verification* against the
  host tooling — no compression code exists in `tools/`, so the wbits used by `py-opendisplay` could not
  be checked here. Either the comment is stale or the flag needs restoring.
- `send_direct_write_nack` unconditionally zeroes `displayed_etag`
  ([display_service.cpp:2185-2186](../src/display_service.cpp#L2185-L2186)), including for pure
  pre-validation rejects that never touched the panel — forcing a needless full re-upload. (`FINDINGS.md`
  L5, still open.)
- Uncompressed auto-complete makes the client's 0x72 a silent no-op: `handleDirectWriteEnd(nullptr, 0)`
  forces `REFRESH_FULL` and `displayed_etag = 0`, and a subsequent real 0x72 returns at
  [display_service.cpp:1811](../src/display_service.cpp#L1811) with **no response at all**. Compressed
  streams keep 0x72 semantics — the two transports diverge.
- A 0x71 with an empty payload returns silently with no ACK/NACK
  ([display_service.cpp:1734-1746](../src/display_service.cpp#L1734-L1746)), stalling a client waiting on
  the ack.
- Heap-window OOM at `od_zlib_stream_reset` is not checked at start; the 0x70 is still ACKed and the
  failure surfaces as a generic NACK on the first 0x71.
- `isAuthenticated()` has a session-clearing side effect (it calls `checkEncryptionSessionTimeout()`) and
  is called repeatedly per command — a session can expire between the gate check and `sendResponse`.
- nRF `disconnect_callback` calls `cleanupDirectWriteState(true)` unconditionally
  ([device_control.cpp:89-95](../src/device_control.cpp#L89-L95)) — every disconnect runs `pwrmgm(false)`
  → `SPI.end()` / `Wire.end()` even with no transfer active, and it lacks the ESP32
  `epdRefreshInProgress` deferral guard.
- The 0x0052 command sends no response on the non-DFF path
  ([device_control.cpp:691-705](../src/device_control.cpp#L691-L705)); if `enterDeepSleep()` declines, the
  handler returns with no response at all.
- `esp32_restart_ble_advertising()` calls `startAdvertising()` then `updatemsdata()`, which (when not
  connected) does `stop()` + `start()` again — net start→stop→start, plus an unconditional `delay(100)`
  ([ble_init.cpp:179-181](../src/ble_init.cpp#L179-L181)). Inside the wake window a disconnect costs
  ~150 ms of the window.
- `mloopcounter` is a non-atomic read-modify-write reached from both `loop()` and the BT `onConnect`;
  `handleReadMSD` can read a half-written `msd_payload`.
- The `esp32-s3` `bleDrain` loop, `pwrmgm`, and `updatemsdata` all run I2C/SPI from `loop()` while
  callbacks may do the same from the BT task (see H10).
- `EP397` partial `DISP_CTRL2` value diverges from the bb_epaper library's
  ([display_service.cpp:2127](../src/display_service.cpp#L2127): `0xff`, library: `0xfc`).
  *needs-verification* — if deliberate, comment it.
- Seeed 1bpp buffer-size formulas disagree: `fb_byte_size()` uses flat `(w*h+7)/8`, `handleDirectWriteStart`
  uses row-padded `ceil(w/8)*h`, `TFT_eSprite` uses truncating `(w>>3)*h`. Identical for 1872 px; any
  future width not a multiple of 8 breaks. Same latent class as the already-fixed `FINDINGS.md` H3.
- Seeed waveform temperature is pinned at the library default 16 °C; `readChipTemperature()` exists but
  `setTemp()` is never called.
- `esp_sleep_config_gpio_isolate()` in `dffLatchRelease` ([power_latch.cpp:74](../src/power_latch.cpp#L74))
  is not followed by any sleep; if USB keeps the device alive, that isolation config persists into a later
  timer deep sleep.
- `esp_deep_sleep_enable_gpio_wakeup()`'s return is unchecked
  ([power_latch.cpp:99-104](../src/power_latch.cpp#L99-L104)); on C3/C6 only deep-sleep-domain GPIOs are
  valid and an out-of-range configured pin fails silently.
- Duplicate power-off pollers: `powerButtonPoll` (fixed 3000 ms) and `pollConfiguredPowerOffButtons`
  (configurable `power_off_hold_ms`) can both track the same physical button.
- `button_id = (instance*8 + pinIdx) % 8 == pinIdx`, so `instance_number` is a no-op in the ID
  ([device_control.cpp:578-579](../src/device_control.cpp#L578-L579)). (`FINDINGS.md` L25.)
- WiFi LAN frames up to 4096 B feed `imageDataWritten`, whose BLE callers cap at 512 B
  ([wifi_service.cpp:17,239](../src/wifi_service.cpp#L17)). Framing math is correct; downstream tolerance
  remains *unverified*. (`FINDINGS.md` L28.)

---

## 6. Status of the prior audit (`FINDINGS.md`, commit `4f4b503`, 46 commits back)

All 61 findings were re-checked against the current source. **30 fixed, 8 partially fixed, 23 still
open, 0 obsolete.**

**Fixed:** H1 (dangling `errorResponse`), H2 (`ButtonState` ODR), H3 (4bpp row-buffer overflow), H6
(wake busy-spin), H7 (partial-write rail timeout), H8 (touch-suspend counter), H9 (unbounded buzzer),
H10 (`led_run_step` hang), H11 (per-event sensor sweep), M1 (volatile queue indices), M3
(`epdRefreshInProgress` on partial), M4 (BWR/BWY second bitplane), M5 (`static configLen`), M8 (2 KB
decompression chunk), M9 (nRF MSD early-out), M11 (blocking `initWiFi` in `setup`), M12 (WiFi on wake),
M15 (`sendResponse` length width), M16 (double config load), M19 (`flashLed` 0xFF pins), M21 (touch
flag RMW), L1, L3, L7, L16.

**Partially fixed:** M6 (CRC added but advisory; per-packet lengths still ignored → M32 above), M13
(hot paths quieted; no compile-out `LOG` macro, and `DISABLE_USB_SERIAL` is defined nowhere), M18
(race fixed; single event slot and the ISR `break` remain), M20 (light probe added; 200 ms blocking
settle remains), M22 (BWR/BWY color pass skips rows for rot 0/2 only; gray4 second pass still re-runs
the full pipeline), L9 (bus now configurable; GPIO21 still hardcoded), L23 (`directWritePlane2` now
functional; the rest of the dead render code remains), L27 (min/max preferred fixed; redundant
re-application remains).

**Still open — highest value:**

| ID | What | Where now |
|---|---|---|
| H4 | AES-CCM nonce reuse across directions | [encryption.cpp:158-170](../src/encryption.cpp#L158-L170) → **H5** above |
| H5 | Exact-counter replay always accepted | [encryption.cpp:136](../src/encryption.cpp#L136) → **H4** above |
| M2 | BLE-task teardown races `loop()` | [esp32_ble_callbacks.h:48,53-58](../src/esp32_ble_callbacks.h#L48) → **H10** above |
| M7 | Byte-at-a-time zlib dispatch + per-byte adler modulo | [od_zlib_stream.c:482,313-320](../lib/uzlib/src/od_zlib_stream.c#L482) |
| M10 | nRF advertising boost never engages | [device_control.cpp:438-440](../src/device_control.cpp#L438-L440) |
| M14 | Three-buffer copy chain per encrypted packet | [encryption.cpp:662-673](../src/encryption.cpp#L662-L673) |
| M17 | ~16.4 KB pinned in static config buffers | [config_parser.cpp:85,159,211,273](../src/config_parser.cpp#L85) |

Also still open: L2, L4, L5, L6, L8, L10–L15, L17–L22, L24–L26, L28. Most are restated with current
line numbers in §5.

**M7 remains the single largest steady-state CPU win** in the image path and is untouched:
`process_block_data` still returns after every literal byte, forcing a re-dispatch through two `switch`
statements per output byte, and `adler_update_byte` still performs a divide per byte.

---

## 7. Ruled out (checked and found *not* to be defects)

These were raised during the audit and disproved by reading the code. Recording them so they are not
re-reported.

- **"Unauthenticated config rewrite escape hatch" via `SECURITY_FLAG_REWRITE_ALLOWED`.** *Not
  reachable.* The dispatcher rejects all unauthenticated commands before dispatch
  ([communication.cpp:509-515](../src/communication.cpp#L509-L515)) and both config-write handlers are
  called only from the switch at [:562](../src/communication.cpp#L562). The behavior is fail-secure; the
  branches are merely dead (M28).
- **`commandQueueHead` / `commandQueueTail` missing `volatile`.** Fixed since the prior audit
  ([main.h:370-371](../src/main.h#L370-L371)); single-writer-per-index SPSC, TOCTOU-safe. (Formal memory
  ordering on dual-core S3 is still `volatile`-only, not release/acquire — theoretical, noted in §5.)
- **`tcpReceiveBuffer[8192]` overflow.** Bounds are correct
  ([wifi_service.cpp:216-246](../src/wifi_service.cpp#L216-L246)): the read is clamped to remaining
  space, `flen` is validated in `(0, 4096]`, and the remainder is `memmove`d.
- **Config TLV parser memory safety.** Every `offset + sizeof(...) <= configLen - 2` guard is sound
  (`configLen >= 3` enforced at [config_parser.cpp:279](../src/config_parser.cpp#L279); no unsigned
  underflow), and all instance counts are clamped to array bounds. The M32 problem is semantic, not
  memory-unsafe.
- **`decryptCommand` / `encryptResponse` length handling.** Rejects `encrypted_len > 512`, validates the
  inner `payload_length`, refuses response payloads > 255; the AD binds the plaintext command header, so
  header tampering fails the tag.
- **`imageDataWritten`'s `static` scratch buffers.** Single-task on ESP32 (BLE `onWrite` only enqueues;
  both the queue drain and the LAN path call it from `loop()`), so no reentrancy despite being `static`.
- **`millis()` overflow.** All ESP32-path duration checks use the wraparound-safe unsigned-subtraction
  idiom. The one exception is the nRF advertising boost (`FINDINGS.md` L2).
- **MAC/tag/session-id comparisons** use `constantTimeCompare` — timing-safe.
- **ESP32 `bbepWriteData`** uses `SPI.transferBytes` block writes — no per-byte SPI inefficiency on this
  path (nRF builds use a per-byte loop at the library level).
- **The uzlib calling contract** in `display_service.cpp` is correct: `od_zlib_stream_reset` per stream,
  push→poll-until-`NEEDS_INPUT`/`DONE` loops satisfy the "previous input fully consumed" precondition,
  and final-flush / `expected_output` clamping / adler + length verification all match the state machine.
- **The advertising-restart gap** documented in `FINDINGS_BLE_ADVERTISING_RESTART_GAP_2026-07-09.md` is
  **fixed** at HEAD ([main.cpp:143-145](../src/main.cpp#L143-L145), commit `b6b9752`).
- **The BWR/BWY two-plane direct-write bug** documented in `FINDINGS_BWR_BWY_2026-07-05.md` is **fixed**
  at HEAD ([display_service.cpp:1504-1522,1578,1761](../src/display_service.cpp#L1504-L1522)).
- **The missing-rotation-on-wake bug** from `FINDINGS_SETUP_VS_MINIMALSETUP_ESP32_2026-07-08.md` §6.2 is
  **fixed** ([main.cpp:315](../src/main.cpp#L315)).
- **`#if defined(TARGET_ESP32) && defined(OPENDISPLAY_SEEED_GFX)` vs bare `#ifdef`.** All 14 `src/` sites
  use the compound form; `platformio.ini` never defines `OPENDISPLAY_SEEED_GFX` without `TARGET_ESP32`,
  and `seeed_driver_used()` hard-returns false on non-Seeed builds. Defensively redundant, never
  meaning-changing.

---

## 8. Recommended priority

1. **C1** — guard `handlePartialWriteStart` (driver, `display_count`, panel validity, `partial_update_support`,
   BWR/BWY). One block of code closes C1, H12, M17 and M19.
2. **H1** — clear `displayed_etag` on the NORMAL BOOT branch. One line; prevents silent display corruption
   after any panic/WDT/reboot, which the existing docs confirm happens in the field.
3. **H4, H5, H6** — the three crypto defects. Fix before any security claim is made about the product.
4. **H7, H8, H11** — the Seeed path's inability to detect or report failure. H7 first: it masks the other
   two and manufactures the etag that reaches C1.
5. **H2, H3** — the two silent protocol failures (config read truncation, 34 s config-write stall). Both
   are small, local fixes.
6. **H9** — mask the sequence nibble out of the MSD `memcmp`. One line; removes an advertising
   stop/`delay(50)`/start from every button, touch and idle tick.
7. **H10, M3, M5, M6** — cross-task races and unbounded state. Adopt the `*_pending` flag pattern
   uniformly.
8. **M25, M26** — the build configuration is not doing what it says (inert watchdog flag; a C6 env that
   likely cannot build). Cheap to verify, cheap to fix, and M25 interacts with every blocking wait above.
9. **`FINDINGS.md` M7** — the zlib hot path. The largest remaining steady-state CPU/battery win.
10. **§5 "Globals defined in `main.h`"** — the structural fix that makes the whole duplicate-declaration
    class of bug (of which the tree has already suffered one memory-corrupting instance) impossible.
