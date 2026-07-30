# Plan — minimum: don't restart nRF advertising while BLE-connected

**Date:** 2026-07-29 (revised down to minimum, then corrected under review)
**Branch:** `fix/nrf-no-adv-while-connected` (off `fix/loop-hang-3`)
**Scope:** four small changes, no new module, no interval policy, no LAN term.

### Relationship to the parity plan

`PLAN_BLE_ADVERTISING_SESSION_GATE_INTERVAL_PARITY_2026-07-29.md` declares itself
"supersedes for implementation" for this document. **That is inverted by this
revision:** this minimum is what gets implemented now, and the parity plan becomes
the deferred backlog. Its Commit 1 LAN half, Commit 3 (interval parity, shared
policy module) and Commit 4 are out of scope — reasons in
[Deferred](#deferred-and-why).

### Revision history

Draft 1 proposed a one-line nRF guard. Draft 2 expanded it to a shared BLE-or-LAN
gate with a `bool` publish contract and a `prev_msd_payload` fix. Draft 3 cut most
of that. **Draft 4 — this one — fixes five defects found reviewing Draft 3, and
replaces its `sleep_timeout_ms` clamp with a decoupling.**

**The correction that shrank the plan (Draft 3, since re-verified).** Drafts 1–2
argued a gated publish costs *two* attempts because the next call would hit the
`memcmp` early-return at
[display_service.cpp:1824-1828](../src/display_service.cpp#L1824-L1828). Wrong.
`struct MsdAdvertisement` is packed with a `sizeof == 16` assert
([opendisplay_structs.h:1233-1240](../include/opendisplay_structs.h#L1233-L1240)),
`status` is its last byte — `msd_payload[15]` — and `mloopcounter` occupies bits 4-7
of it, masked to `0x0F`. It is folded in at
[display_service.cpp:1807](../src/display_service.cpp#L1807) and copied into
`msd_payload` at [:1817](../src/display_service.cpp#L1817), **before** the `memcmp`,
and it advances on **both** exits ([:1825](../src/display_service.cpp#L1825),
[:1843](../src/display_service.cpp#L1843)). Consecutive payloads therefore always
differ, the skip is unreachable, and `prev_msd_payload` advancing before the push
([:1829](../src/display_service.cpp#L1829)) has **no observable effect** — so
Draft 2's retention/retry machinery fixed a defect that does not manifest.
Consequence recorded as residual 1: the rebuild that `memcmp` guards, including the
I2C/ADC polling at [:1787-1790](../src/display_service.cpp#L1787-L1790), always runs.

**Defects found in Draft 3, all fixed here:**

- *Item 1's diff did not compile.* It inserted `return false` into a function
  declared `void` ([ble_transport.h:48](../src/ble_transport.h#L48)), while claiming
  items 1 and 3 were independent and orderable 1-then-3. Item 1 now returns `void`
  and item 3 converts both targets.
- *Item 2 could lose the publish entirely.* Fixed by gating the consume site — see
  item 2.
- *Item 3 had two uncaught return paths on ESP32.* Both now named explicitly.
- *The conditional-`stop()` rationale was wrong.* Draft 3 claimed the fast-timeout
  residual means advertising can legitimately be stopped after the 1 s window. It
  cannot: `start(0)` leaves `_stop_timeout == 0`, so the `ADV_SET_TERMINATED`
  handler immediately re-arms slow mode via `_start(_slow_interval, 0)`
  (`BLEAdvertising.cpp:435-442`). `isRunning()` is true whenever disconnected except
  the very first call. The change stays — it removes one guaranteed
  `NRF_ERROR_INVALID_STATE` SVC at boot — with honest reasoning.
- *Item 4 was a `sleep_timeout_ms` floor clamped in `loadGlobalConfig()`.*
  **Replaced.** Two independent reasons: the clamp was skipped on the one path that
  actually yields 0 in the field (`loadGlobalConfig()` returns at
  [config_parser.cpp:301-304](../src/config_parser.cpp#L301-L304) when `loadConfig()`
  fails, before the clamp site, leaving `globalConfig` memset), so its central claim
  was false; and a floor is the wrong instrument, because `sleep_timeout_ms` does not
  mean the same thing on both targets. See item 4.

Also refuted in review and recorded because the reasoning was load-bearing:

- *"The boost latches permanently."* No — `setManufacturerData()` itself calls
  `applyAdvInterval()` ([ble_transport_nrf.cpp:254](../src/ble_transport_nrf.cpp#L254)),
  so any later disconnected publish restores the normal interval.
- *`setFastTimeout(10)` has never taken effect.* `startAdvertising()`'s trailing
  `start(0)` ([:214](../src/ble_transport_nrf.cpp#L214)) runs while advertising is
  already up (started by the publish inside `updatemsdata()` at :209), and
  `sd_ble_gap_adv_set_configure` rejects non-NULL params *and* the same data buffers
  while advertising (`ble_gap.h:1888`), silently outside `CFG_DEBUG`
  (`verify.h:72-81`). The fast window is 1 s from boot. Residual 2.

---

## The defect being fixed

`BleTransport::setManufacturerData()` on nRF
([ble_transport_nrf.cpp:249-258](../src/ble_transport_nrf.cpp#L249-L258)) rebuilds
the advertisement and restarts the radio unconditionally. **While BLE-connected the
restart cannot succeed.** `Bluefruit.begin(1, 0)`
([:164](../src/ble_transport_nrf.cpp#L164)) allocates one peripheral role slot;
`BLEAdvertising::_start()` ends in `sd_ble_gap_adv_start(_hdl, CONN_CFG_PERIPHERAL)`
(`BLEAdvertising.cpp:365`), and the S140 7.3.0 header (`ble_gap.h:1933-1936`) is
explicit: `NRF_ERROR_CONN_COUNT` — "connectable advertiser cannot be started."
Documentation-level proof; test 1 confirms on air. (Either that or the preceding
`set_configure` fails on identical buffers; both fail, so item 1 is right either way.)

ESP32 has had this guard since the NimBLE migration
([ble_transport_esp32.cpp:283-288](../src/ble_transport_esp32.cpp#L283-L288)). nRF
never got it.

The guaranteed-failing call is not free. A button press while connected calls
`ble.boostAdvertising()` then `updatemsdata()`
([device_control.cpp:627-628](../src/device_control.cpp#L627-L628), order commented
as load-bearing), so `applyAdvInterval()` sets 20–30 ms
([:50-51](../src/ble_transport_nrf.cpp#L50-L51)) on the `BLEAdvertising` object.
`tick()` cannot restore it: it early-returns on `!isRunning()` — false for the whole
connection — **and clears `was_boosted` / `s_advBoostUntil` on that path**
([:307-311](../src/ble_transport_nrf.cpp#L307-L311)), so the restore is never
retried. On disconnect the SoftDevice re-arms via `BLEAdvertising::_eventHandler`
(`BLEAdvertising.cpp:425`) at the boosted interval: 5–33× the advertising duty until
the next publish.

### Reachability

`updatemsdata()` ends in `ble.setManufacturerData()`
([display_service.cpp:1838](../src/display_service.cpp#L1838)). Reachable while
BLE-connected:

| Call site | Trigger | Frequency |
|---|---|---|
| [main.cpp:638-641](../src/main.cpp#L638-L641) | `s_msdUpdatePending`, raised on the connect event at [:464](../src/main.cpp#L464) | once per connection, by construction |
| [touch_input.cpp:733](../src/touch_input.cpp#L733) | touch state change | per touch/release |
| [device_control.cpp:238](../src/device_control.cpp#L238) | ADC button edge | per press/release |
| [device_control.cpp:628](../src/device_control.cpp#L628) | GPIO button edge (+ boost) | per press/release |

After item 1 all four become no-publish on nRF. The connect-branch flag at
[:464](../src/main.cpp#L464) keeps its remaining purpose: it refreshes
`msd_payload`, which `handleReadMSD()` serves over GATT by reading the global
directly ([communication.cpp:278-289](../src/communication.cpp#L278-L289)). Test 5
asserts that. Residual cost: each gated call still runs the full I2C/ADC poll and
still advances `mloopcounter`, so a host observes counter jumps across a session.

Not reachable while connected: `platformIdle()`'s calls
([main.cpp:610](../src/main.cpp#L610), [:615](../src/main.cpp#L615)) run only when
`!workInFlight`, which includes `ble.isConnected()`
([:695-703](../src/main.cpp#L695-L703)); the setup-path calls run pre-connection;
and `serviceBleAdvertisingRestart()`'s call ([:452](../src/main.cpp#L452)) is
unreachable on nRF via the capability gate at
[:440-443](../src/main.cpp#L440-L443).

---

## The change — four items

### 1. Gate the nRF publish on BLE-connected

[ble_transport_nrf.cpp:249](../src/ble_transport_nrf.cpp#L249). Note the `stop()`
moves **above** the rebuild, and the return stays `void` — item 3 changes the
signature on both targets together:

```diff
 void BleTransport::setManufacturerData(const uint8_t* msd, uint8_t len) {
+    // Bluefruit.begin(1, 0) gives one peripheral role slot, so the start() below
+    // returns NRF_ERROR_CONN_COUNT for the whole duration of a connection --
+    // guaranteed-failing work whose applyAdvInterval() latches a boosted
+    // 20-30 ms interval that tick() cannot restore (it gives up when
+    // !isRunning() and clears its own bookkeeping on the way out).
+    // Deliberately BLE-only: a LAN term would suppress discovery on TCP accept,
+    // pre-auth -- see the deferred list in this commit's plan doc.
+    if (connectedCount() > 0) return;
+    // Stop BEFORE rebuilding, not after. clearData() zeroes _count and addData()
+    // writes in place into _data, which is the very buffer _start() handed the
+    // SoftDevice (BLEAdvertising.cpp:358-362, with a frozen .len -- residual 3).
+    // Rebuilding while the radio is live lets a scanner capture a half-updated
+    // MSD, and ADV_SET_TERMINATED re-enters _start() from the stack task
+    // (BLEAdvertising.cpp:428-442) while the loop task is mid-rebuild.
+    if (Bluefruit.Advertising.isRunning()) Bluefruit.Advertising.stop();
     Bluefruit.Advertising.clearData();
     Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
     Bluefruit.Advertising.addName();
     Bluefruit.Advertising.addData(BLE_GAP_AD_TYPE_MANUFACTURER_SPECIFIC_DATA, msd, len);
     applyAdvInterval();
     Bluefruit.Advertising.setFastTimeout(1);
-    Bluefruit.Advertising.stop();
     Bluefruit.Advertising.start(0);
 }
```

The `isRunning()` test is not about the fast-timeout residual (Draft 3 had that
wrong — revision history). Disconnected, advertising is always running, because
`_stop_timeout == 0` makes the TERMINATED handler re-arm slow mode. The test earns
its place by skipping one guaranteed-`INVALID_STATE` SVC on the very first call, from
`startAdvertising()` at [:209](../src/ble_transport_nrf.cpp#L209) before any
`_start()` has run.

### 2. Publish once, promptly, after every session — and don't lose the flag

Two parts. Raise `s_msdUpdatePending` in the disconnect branch of
`serviceBleEvents()` ([main.cpp:471-504](../src/main.cpp#L471-L504)), mirroring the
connect branch at [:464](../src/main.cpp#L464). **And gate the consume site**
([:638-641](../src/main.cpp#L638-L641)):

```diff
     if (s_msdUpdatePending) {
-        s_msdUpdatePending = false;
         updatemsdata();
+        // Keep the flag raised while the publish cannot land. serviceBleEvents()
+        // takes connect and disconnect in sequential ifs, not else-if, so one pass
+        // can service a disconnect AND a reconnect -- the interleaving the comment
+        // at :481 documents for a ~16 s EPD refresh. Clearing unconditionally would
+        // spend the flag on a gated publish and lose the post-session republish,
+        // including the applyAdvInterval() restore this exists for.
+        s_msdUpdatePending = ble.isConnected();
     }
```

**`updatemsdata()` still runs while connected — do not gate the call itself.** An
earlier version of this item guarded the whole block on `!ble.isConnected()`, which
is wrong: the connect-raised flag's only remaining purpose after item 1 is to refresh
`msd_payload` for `handleReadMSD()`, which reads the global directly
([communication.cpp:283](../src/communication.cpp#L283)). Skipping the call hands a
connected client stale bytes from before its own session and fails test 8. Only the
publish inside `setManufacturerData()` is gated; the sensor poll and payload rebuild
must not be.

This is the whole retry mechanism. No pending-payload buffer and no `bool`-driven
retry state: because every payload differs (revision history), the first
post-disconnect `updatemsdata()` publishes unconditionally. All this does is
guarantee that call happens **next eligible pass** rather than whenever the idle
cadence gets round to it.

Note the flag now persists across a connected pass. That is the point, and it is
bounded: `updatemsdata()` is idempotent and the flag clears on the first
disconnected pass.

### 3. Stop the log claiming a publish that did not happen

`setManufacturerData()` returns `bool` on both targets — `false` when gated, `true`
when the payload reached the stack. Both implementations need every path to return a
value; on ESP32 that is **two** sites, the early `return;` at
[ble_transport_esp32.cpp:285](../src/ble_transport_esp32.cpp#L285) and the
fall-off-the-end at [:302](../src/ble_transport_esp32.cpp#L302). Falling off a
non-void function is a `-Wreturn-type` **warning**, not an error, on this toolchain,
so neither can be left to the compiler.

Update the declaration at [ble_transport.h:48](../src/ble_transport.h#L48). There is
exactly one caller, [display_service.cpp:1838](../src/display_service.cpp#L1838).

In `updatemsdata()`, move the `MSD publish:` block
([:1830-1837](../src/display_service.cpp#L1830-L1837)) to *after* the call and emit
it only on `true`. Leave `opendisplay_mdns_update_msd_txt()`
([:1839-1842](../src/display_service.cpp#L1839-L1842)) and the `mloopcounter`
advance ([:1843-1844](../src/display_service.cpp#L1843-L1844)) unconditional: the
mDNS TXT is the LAN-side channel and must not follow the BLE gate, and the counter
must keep moving or the payload stops changing.

Leave `prev_msd_payload` where it is. It has no observable effect (revision
history), and moving it would be a fix with no defect behind it.

The comment on that log block says it is "the only record of what actually reaches
the air" — which is exactly what it is not today: it fires before the call, and on
ESP32 it has always fired even when the gate dropped the publish.

### 4. Decouple the nRF idle cadence from `sleep_timeout_ms`

`sleep_timeout_ms` is an **ESP32 deep-sleep concept**. Its own field doc calls it
"nominal awake/advertising time before sleep"
([opendisplay_structs.h:490](../include/opendisplay_structs.h#L490)), and
`platformIdle()`'s header says so outright: *"ESP32 owns the deep-sleep decision;
nRF just idles at its configured cadence"*
([main.cpp:581-582](../src/main.cpp#L581-L582)). nRF has no deep sleep — every
`enterDeepSleep()` call site is inside `#ifdef TARGET_ESP32`.

Yet the nRF idle arm binds two unrelated things to it
([main.cpp:612-619](../src/main.cpp#L612-L619)):

```c
if (globalConfig.power_option.sleep_timeout_ms > 0) {
    idleDelay(globalConfig.power_option.sleep_timeout_ms);   // how long to park
    updatemsdata();                                          // how often to publish
} else {
    idleDelay(500);                                          // and neither, when 0
}
```

So a provisioned value simultaneously sets the park duration, the MSD refresh
cadence, and — at 0 — silently disables the refresh entirely. That last case is the
hole Draft 3's clamp was chasing. Decoupling removes it structurally instead.

**ESP32 already has the right shape**, and it is outside the battery/non-battery
branch so it covers both ([main.cpp:607-611](../src/main.cpp#L607-L611)):

```c
static uint32_t lastMsdUpdate = 0;
if (millis() - lastMsdUpdate >= 60000) { lastMsdUpdate = millis(); updatemsdata(); }
```

Give nRF the same structure, and name the shared cadence so the magic 60000 goes
away. Two constants in `main.h`, immediately after the `#ifdef TARGET_ESP32` block
that ends at [:309](../src/main.h#L309) so both targets see them:

```c
// MSD refresh cadence, both targets. Periodic only: touch and button edges publish
// on change from their own handlers, so this covers battery/temperature drift and
// keeps the advertisement's loop counter moving.
#ifndef OD_MSD_REFRESH_MS
#define OD_MSD_REFRESH_MS      60000u
#endif
// How long the nRF idle path parks. NOT power_option.sleep_timeout_ms: that is an
// ESP32 deep-sleep window (see opendisplay_structs.h) and nRF has no deep sleep, so
// binding the park and the MSD cadence to it made a provisioned wake-window value
// govern two unrelated nRF behaviours -- and disabled the MSD refresh outright at 0.
//
// 1000 ms, not the old else-branch's 500: the park no longer carries the MSD
// cadence, so nothing is lost by making it longer, and it is a plain power/latency
// trade with a hard floor on the latency side. idleDelay() chunks at
// CHECK_INTERVAL_MS and returns early on RX or a transport event, so BLE work is
// still picked up within ~100 ms regardless of this value; what it actually bounds
// is how long millis()-polled work waits -- epdSessionTick(), buzzerService(),
// processLedFlash() and checkTransferTimeouts(), none of which needs sub-second
// service. Overridable from platformio.ini for targets that want a different trade.
#ifndef OD_NRF_IDLE_WAIT_MS
#define OD_NRF_IDLE_WAIT_MS    1000u
#endif
```

and the nRF arm becomes:

```diff
 #else
-    if (globalConfig.power_option.sleep_timeout_ms > 0) {
-        idleDelay(globalConfig.power_option.sleep_timeout_ms);
-        updatemsdata();
-    } else {
-        idleDelay(500);
-    }
+    idleDelay(OD_NRF_IDLE_WAIT_MS);
 #endif
+    static uint32_t lastMsdUpdate = 0;
+    if (millis() - lastMsdUpdate >= OD_MSD_REFRESH_MS) {
+        lastMsdUpdate = millis();
+        updatemsdata();
+    }
```

with the ESP32 arm's copy of that block deleted, since it is now shared — which is
the same convergence Phase 4 applied to the rest of `loop()`.

**What changes:**

- **nRF at 0** (factory-fresh, or a failed config load — the case Draft 3's clamp
  missed): the MSD now refreshes every 60 s instead of never, so a boosted interval
  is always restored. This is the actual fix.
- **nRF at a nonzero value**: the park becomes a fixed 1000 ms. Where the
  provisioned value was larger, loop-top work (`serviceBleEvents`,
  `serviceBleDisconnectCleanup`, `checkTransferTimeouts`) is serviced sooner —
  `idleDelay()` breaks early only on `bleRxQueuePending() || ble.eventPending()`
  ([main.cpp:735](../src/main.cpp#L735)), so touch and button edges did **not**
  shorten a long park before. Where it was smaller, marginally later. MSD cadence
  moves to a uniform 60 s either way.
- **The former zero-config case parks 2× longer** (1000 ms vs the old
  else-branch's 500). Nothing regresses: that branch never published, and BLE
  latency is set by the 100 ms chunking, not by this value.
- **Wake rate is unchanged.** `idleDelay()` already chunks at `CHECK_INTERVAL_MS`
  = 100 ms ([main.cpp:733](../src/main.cpp#L733)), so a 2000 ms park was already 20
  × `delay(100)`. Only the full-loop-body rate changes, and only where the
  provisioned value differed from 1000 ms.
- **ESP32: nothing.** It never read `sleep_timeout_ms` for either purpose in this
  arm; its two reads ([:551](../src/main.cpp#L551), [:586](../src/main.cpp#L586))
  and their `== 0` fallbacks are untouched.
- **No config semantics change and no clamp**, so already-provisioned devices keep
  their ESP32 behaviour exactly. This is what makes item 4 strictly narrower than
  Draft 3's floor.

After this, `sleep_timeout_ms` is read only on ESP32. `config_parser.cpp:710` still
prints it on nRF, where it now has no effect — noted as residual 4 rather than
changed, since the dump is target-agnostic by design.

---

## Deferred, and why

Each was in a previous draft or the parity plan. None is refuted; all are out of
scope for a change landing on a branch already carrying loop-hang and blocking-log
work.

1. **LAN gate.** Should not trigger at TCP accept. `lastLanActivityMs` is refreshed
   on *any* received bytes before frame validation
   ([wifi_service.cpp:945-947](../src/wifi_service.cpp#L945-L947)) and the idle
   check only runs when `drainedBytes == 0`, so a peer dripping one byte per tick
   holds the 30 s timer (`OD_LAN_READ_TIMEOUT_S`,
   [opendisplay_protocol.h:984](../include/opendisplay_protocol.h#L984)) open
   indefinitely — suppressing BLE discovery forever, pre-TLS, unauthenticated. The
   protocol explicitly allows persistent LAN clients
   ([opendisplay_protocol.h:943](../include/opendisplay_protocol.h#L943)). The gate
   is also order-dependent rather than exclusive: BLE-then-LAN is permitted,
   LAN-then-BLE is blocked. If a LAN gate is wanted, **active transfer** is the
   right trigger.
2. **Interval parity / the 1000 ms ESP32 steady interval.** ESP32 runs NimBLE's
   30–60 ms default (`BLE_GAP_ADV_FAST_INTERVAL1`; no interval setter anywhere in
   `src/`). Dropping to 1000 ms is ~20× fewer advertising events, and the boost that
   would mitigate it covers only GPIO buttons — not ADC buttons
   ([device_control.cpp:238](../src/device_control.cpp#L238)) or touch
   ([touch_input.cpp:724](../src/touch_input.cpp#L724)). For a `local_push` consumer
   reading transient state out of the MSD that is a delivery-reliability change, not
   power tuning. Needs idle current, discovery latency **and short-tap capture rate**
   against a real HA/ESPHome proxy first.
3. **Shared `ble_advertising_policy` module.** Not justified at this size. Bluefruit
   owns fast/slow phase transitions and auto-restart; NimBLE has one min/max range
   and application-driven restart. A shared phase machine is worth building only once
   measurements prove both targets need the same schedule.
4. **Making `setFastTimeout(10)` real.** It would *add* ~9 s of 160 ms advertising
   after every boot and session release. On a coin cell that is a new cost justified
   so far only by apparent historical intent.
5. **`tick()` hardening** — restore the interval unconditionally on boost expiry
   ([ble_transport_nrf.cpp:307-311](../src/ble_transport_nrf.cpp#L307-L311)).
   Unreachable via this path after item 1, but open to any future
   `applyAdvInterval()` caller.
6. **ESP32 `delay(50)` / `delay(100)`**
   ([ble_transport_esp32.cpp:301](../src/ble_transport_esp32.cpp#L301),
   [:234](../src/ble_transport_esp32.cpp#L234)): unjustified in source or comment,
   and they block `loop()` on every disconnected publish and restart. Directly
   relevant to this branch's stability work — measure, then remove, on its own.
7. **ESP32's stale-start-then-refresh** at [main.cpp:452](../src/main.cpp#L452)
   (`restartAdvertising()` then `updatemsdata()`) does one start with old data then
   another with fresh. Collapsing it is independent of this change.
8. **A `sleep_timeout_ms` floor.** Dropped, not deferred-with-intent: after item 4
   nRF does not read the field, and on ESP32 a floor would raise deliberate sub-10-s
   configs with no defect behind it. If one is ever wanted, note Draft 3's trap —
   `loadGlobalConfig()`'s early returns at
   [config_parser.cpp:301-304](../src/config_parser.cpp#L301-L304) bypass any clamp
   placed before `loaded = true`.

## Proof obligations

| Claim | Evidence |
|---|---|
| nRF restart cannot succeed while BLE-connected | `Bluefruit.begin(1, 0)` ([:164](../src/ble_transport_nrf.cpp#L164)) + `NRF_ERROR_CONN_COUNT`, `ble_gap.h:1933-1936` (docs-level; test 1 verifies on air) |
| `_runnning` false for the whole connection | cleared at `BLEAdvertising.cpp:414` on connect; only `_start()` success sets it (`:368`) |
| The boost cannot self-restore while connected | `tick()` clears `was_boosted`/`s_advBoostUntil` on the `!isRunning()` path ([:307-311](../src/ble_transport_nrf.cpp#L307-L311)); the other restore path is a publish, which item 1 gates |
| One post-session publish suffices | `mloopcounter` is inside the compared bytes (`msd_payload[15]`, bits 4-7) and advances on both exits, so the `memcmp` skip never fires |
| Item 2 cannot drop the flag | consume is gated on `!ble.isConnected()`, so a pass servicing a reconnect leaves it raised |
| Gate adds no new state | `connectedCount()` is `Bluefruit.connected()`, already read every loop pass via `isConnected()` in `workInFlight` |
| Item 4 changes no ESP32 behaviour | ESP32's `sleep_timeout_ms` reads ([:551](../src/main.cpp#L551), [:586](../src/main.cpp#L586)) and their `== 0` fallbacks are untouched; only the shared MSD block is refactored |
| Item 4 needs no config migration | no clamp, no schema change; the field simply stops being read on nRF |
| No LAN behaviour changes | no LAN term added; `serviceBleAdvertisingRestart()` untouched |

## Test

1. **nRF: connect, press a button, disconnect.** Before: post-disconnect advertising
   at ~20–30 ms. After: 160 ms fast / 1000 ms slow. Sniffer or current probe, not a
   log line. Also confirms the `NRF_ERROR_CONN_COUNT` reasoning end-to-end.
2. **nRF, disconnect-inside-EPD-refresh with an immediate reconnect.** The case that
   broke Draft 3: force a ~16 s refresh, drop the client mid-refresh, reconnect
   before the pass completes. Assert the new session's advertising uses the normal
   interval and a fresh MSD once it ends — i.e. the flag survived the connected pass.
3. **nRF with no stored config at all** (factory-fresh / forced `loadConfig()`
   failure, so `sleep_timeout_ms` is 0). Assert the MSD advertisement changes every
   ~60 s and a boosted interval is restored. This is what item 4 fixes and what
   Draft 3's clamp missed.
4. **nRF at `sleep_timeout_ms` = 2000.** Measure the MSD advertisement period and
   loop-pass rate before and after: cadence moves to 60 s, park to 1000 ms, and no
   other behaviour changes. Repeat at 500 (park lengthens) to cover both directions.
5. **Boost carry-over.** Press a button in the last second before disconnecting.
   `s_advBoostUntil` is still live, so the post-disconnect publish applies 20–30 ms
   to the new session; assert `tick()` restores the normal interval within ~3 s
   (advertising is running now, so its `!isRunning()` bail no longer fires). Bounded
   carry-over, not a latch — but nothing tested it before.
6. **MSD freshness.** Change state during a connection, disconnect: the first
   post-disconnect advertisement carries it.
7. **Log honesty, both targets.** `MSD publish:` absent for every connected-time
   update — **and still present** for a normal disconnected publish, so item 3 did
   not silence it unconditionally.
8. **Touch/button while connected** still reach the host over GATT; `handleReadMSD`
   ([communication.cpp:278-289](../src/communication.cpp#L278-L289)) returns fresh
   bytes. Only the advertisement publish is skipped.
9. **nRF idle current**, disconnected, before and after. Expected near-unchanged:
   the 100 ms chunking fixes the wake rate, so only the full-loop-body rate moves,
   and only where the provisioned value differed from 1000 ms.
10. **ESP32 regression only.** No LAN, interval or cadence behaviour changed;
    confirm the 60 s MSD cadence and deep-sleep timing are as before apart from the
    log.
11. **Build matrix**, all 11 environments, with `-Wreturn-type` clean.

## Residuals

1. **The `memcmp` change-detection at
   [display_service.cpp:1824-1828](../src/display_service.cpp#L1824-L1828) is dead**,
   because `mloopcounter` is inside the compared bytes and advances every call. The
   rebuild it guards — including I2C sensor polling and an ADC read
   ([:1787-1790](../src/display_service.cpp#L1787-L1790)) — always runs. Either
   exclude the counter from the comparison or delete the check; both change publish
   cadence, so neither belongs here.
2. **The nRF fast window is 1 s everywhere and `setFastTimeout(10)` has never
   worked** (revision history). The comment at
   [:204-208](../src/ble_transport_nrf.cpp#L204-L208) defends an ordering that
   protects nothing.
3. **`BLEAdvertising::_start()`'s `static ble_gap_adv_data_t gap_adv`**
   (`BLEAdvertising.cpp:358-362`) is a function-local static with a dynamic
   initialiser, so `.len = _count` freezes at the **first** `_start()` call. The
   invariant is therefore not "the length never changes" but "**the first `_start()`
   must be a full-payload one**" — true today because `startAdvertising()`'s
   `updatemsdata()` at [:209](../src/ble_transport_nrf.cpp#L209) builds
   flags+name+16-byte MSD before it. Worth recording precisely, and an upstream note.
4. **`sleep_timeout_ms` is ESP32-only after item 4** but is still printed in the nRF
   config dump ([config_parser.cpp:710](../src/config_parser.cpp#L710)) and still
   writable from `tools/od-device-cli.py` ([:267](../tools/od-device-cli.py#L267))
   with no target awareness. Documenting the field as ESP32-only — in the CLI and in
   `opendisplay_structs.h`'s field doc — is a follow-up.
5. **ESP32's `idleDelay(5)` must stay 5, and its comment says why wrongly.** The
   comment at [main.cpp:599-604](../src/main.cpp#L599-L604) justifies the value on
   BLE responsiveness — "a 2000 ms idle here stalls BLE command/response servicing
   for up to 2 s" — which `4d37d43` made obsolete by returning early from
   `idleDelay()` on `bleRxQueuePending() || ble.eventPending()`. A reader trusting
   that comment would conclude the 5 is vestigial and raise it. It is not vestigial;
   the real reasons are:
   - **ADC ladder debounce.** `ADC_LADDER_POLL_MS = 5` with `ADC_LADDER_DEBOUNCE = 3`
     ([device_control.cpp:106-107](../src/device_control.cpp#L106-L107)) needs 5 ms
     sampling to resolve a change in ~15 ms. The ladder is ESP32-only, which is
     exactly why nRF does not need this cadence and the 5 ms / 100 ms asymmetry is
     correct rather than an oversight.
   - **GPIO tap capture.** The ISR sets `current_state` and bumps `press_count`
     ([:691-706](../src/device_control.cpp#L691-L706)), but `processButtonEvents()`
     re-reads the pin and overwrites `current_state` ([:601-608](../src/device_control.cpp#L601-L608)).
     A press+release inside one poll interval therefore publishes an incremented
     count with state=released — the press is never observed. `lastChangedButtonIndex`
     is also a single slot, so two buttons in one interval report only the last.
   - **Buzzer and LED step timing** are millis-polled from the same path
     ([buzzer_control.cpp:203](../src/buzzer_control.cpp#L203),
     `LED_MIN_STEP_DELAY_MS = 1`), so the poll interval is a floor on step duration.

   And raising it would save nothing: the pinned framework has
   `# CONFIG_PM_ENABLE is not set` and no `CONFIG_FREERTOS_USE_TICKLESS_IDLE` in
   `framework-arduinoespressif32-libs/esp32s3/sdkconfig`, with
   `CONFIG_FREERTOS_HZ=1000`. The 1 kHz tick wakes the CPU regardless of the delay
   length, so `delay(5)` and `delay(20)` have identical wake counts; only loop-body
   execution differs, against a radio with no modem sleep. Contrast nRF, where
   `configUSE_TICKLESS_IDLE=1` means a longer park genuinely reduces wakeups — the
   same code shape with opposite power economics, which is the substantive reason
   the two targets keep different cadences. Fix the comment; leave the value.
6. **`sessionOrigin` is never cleared**, and the `wifiLanSession` /
   `wifiLanClientConnected()` predicate divergence ([main.cpp:673](../src/main.cpp#L673)
   adds `wifiInitialized`) — unify on the accessor someday.

## Risk and rollback

| Risk | Likelihood | Mitigation |
|---|---|---|
| MSD update dropped rather than deferred | Bounded to one call | Item 2's gated consume; tests 2, 6 |
| Boost carries ≤3 s into the new session | Certain when a press precedes disconnect | Bounded by `tick()`; test 5. Clearing `s_advBoostUntil` on disconnect is the alternative |
| nRF loop-body rate changes where the provisioned park differed from 1000 ms | Certain, by design | Wake rate unchanged (100 ms chunking); test 9 measures current, test 4 covers both directions |
| A target wants a different park | n/a | `OD_NRF_IDLE_WAIT_MS` is `#ifndef`-guarded, so `platformio.ini` can override it per env |
| `bool` conversion leaves a path returning nothing | Low | Two named ESP32 sites; test 11 requires `-Wreturn-type` clean |
| Gate wrong about connected state | None | Same predicate `workInFlight` already uses |

Four items. Item 1 is the bug fix; item 2 is two small edits in `main.cpp`; item 3
is diagnostics plus a signature change; item 4 is a `platformIdle()` refactor that
touches no config semantics. **Items 1 and 3 must land together** — item 1's early
return is `void` only until item 3 converts both targets, so splitting them breaks
the build. Items 2 and 4 revert independently.

Item 4 removes the `sleep_timeout_ms == 0` hole that item 2 partly existed to bound,
so with item 4 in place item 2 is a latency improvement (next-pass rather than up to
60 s) plus a genuine correctness fix for the reconnect case. The commit message
should say which.
