# Freeze-Hardening the OpenDisplay Firmware — 2026-07-31

A self-contained four-phase plan for the BLE e-paper firmware, written from the code
as it stands on `fix/nonce-replay-window` (last code commit `9ca1d8f`, rebased onto the
squashed `main` at `aae5bdf`; every commit after it on this branch is docs-only, so the
citations below still describe the tree).

Every claim below was verified by direct reading of the current tree and is cited to
`file:line` so a reviewer can re-check rather than trust. The loop/BLE unification
(PR `#132`) and the nonce rewrite (this branch) both landed recently and changed the
shape of several subsystems, so nothing here is taken on inherited assumption — the
ground truth is re-established from scratch below.

## Phase map

| # | Phase | Depends on | State today |
|---|---|---|---|
| 1 | Nonce / replay correctness | — | **Shipped** on this branch (`e2e95cd`…`19335e6`) |
| 2 | BLE-HAL foundation: link-drop seam, owner token, handle-aware events, RX clock, abort-to-known-state | — | Partially present (`serviceBleDisconnectCleanup`) |
| 3 | Connection-exclusivity **policy** + idle drop | Phase 2 | Not started |
| 4 | Auth-abuse disconnect | Phase 2, Phase 3 | Prototype exists off-branch, not here |

**Phase order note.** Phase 2 is the foundational layer: every transport/HAL
*mechanism* the later phases stand on — the portable `disconnect()`, the connection
owner token, handle-bearing connect events with callback-side write filtering, the
RX-activity clock, and the shared abort routine. Phase 3 is **policy** on top of those
mechanisms (when to refuse and when to drop); Phase 4 is the auth-abuse policy. Phase 2
lands first because 3 and 4 both call into it — building the foundation last (as an
earlier draft did, with exclusivity as Phase 2) created a dependency cycle, since the
idle drop calls the abort routine.

**Two cross-phase deliverables** thread through Phases 2–4 and are specified once
here rather than repeated:

- **Threshold discipline — at the point of use, not in a new header.** Every tunable
  this plan introduces (idle-drop timeout, auth-abuse count and its
  flush deadline) is a compile-time `#ifndef`-guarded `#define` **in the file that
  consumes it**, each carrying a comment naming the *client behaviour it assumes*.
  No threshold is a wire/config field, so none touches the hard constraint.

  This follows the repo's existing convention rather than inventing one. The model is
  [wifi_service.cpp:470-472](../src/wifi_service.cpp):

  ```c
  #ifndef OD_LAN_ROAM_RSSI_THRESHOLD
  #define OD_LAN_ROAM_RSSI_THRESHOLD (-75)   /* dBm; valid range -100..10 */
  #endif
  ```

  and likewise `OD_TINFL_DICT_SIZE`, `OD_CHARGER_FLAG_*`, `OD_LOG_LEVEL`
  ([od_log.h:16-18](../src/od_log.h)); `TRANSFER_WATCHDOG_MS` is a plain `static const`
  in [display_service.cpp:582](../src/display_service.cpp). There is no central
  tunables header in this repo and this plan does not add one.

  *An earlier draft specified a `src/session_policy.h` collecting all four.* It was
  cut. It would have been the only file of its kind, and it groups by **type**
  ("these are all thresholds") rather than by dependency: the four are consumed by two
  unrelated subsystems — the idle drop by the loop-side policy helpers, auth-abuse by
  `communication.cpp` — so the header buys a new include edge shared by two callers
  that need nothing else from each other. The goal behind it was that the assumptions
  be legible rather than bare numbers; that is served by the mandatory
  client-behaviour comment, which reads *better* next to the code that acts on it, and
  by the client-side CI assertions below. If a shared home is ever genuinely needed,
  `structs.h` is the existing common hub.

  **They do not go in the BLE transport headers either.** These are policy, and
  Phase 2 is mechanisms-only by construction. This is settled precedent here, in the
  same direction: [ble_transport.h:89-93](../src/ble_transport.h) records that the
  loop-serviced deferred-work flags were *moved out* of the transport because they
  "encode application policy, not link state, so exporting them from the transport
  seam was backwards." The transport exposes `bleMsSinceLastRx()`; deciding how long
  is too long belongs to the loop-side policy code that Phase 3 adds.
- **A companion HIL test per phase**, under `tests/`, following the existing
  `tests/serial_stall_test.py` pattern (pytest driving a real board through
  `py-opendisplay`). These *are* the Verification sections — versioned with the
  code, not prose. See [Verification model](#verification-model) below.

## Hard constraint — NO wire protocol change

`include/opendisplay_protocol.h` must not change, and no new opcode or response
code may be added. Verified for every phase below: dropping a link, refusing a
connection, and idle teardown are all HCI-level (a disconnect *reason* byte, not
an app-protocol field); `RESP_AUTH_REQUIRED` already exists and is used in its
documented meaning. If any phase turns out to need a wire change it stops and the
change goes through `../opendisplay-protocol` first.

---

## What the current code actually does (ground truth)

Established by direct reading of the tree, 2026-07-31. These are the facts the
phases build on; each is cited so a reviewer can re-check rather than trust.

### Connection model is asymmetric and, on ESP32, unguarded

- **nRF** caps at one central in hardware: `Bluefruit.begin(1, 0)`
  ([ble_transport_nrf.cpp:164](../src/ble_transport_nrf.cpp)). The SoftDevice
  refuses a second central at the link layer. Advertising re-arms itself
  (`restartOnDisconnect(true)`, `:210`).
- **ESP32** allows **three** centrals: `CONFIG_BT_NIMBLE_MAX_CONNECTIONS = 3` is
  baked into the precompiled NimBLE framework and a `-D` override is inert (the
  precompiled `sdkconfig.h` wins). `onConnect`
  ([ble_transport_esp32.cpp:81-93](../src/ble_transport_esp32.cpp)) does **no**
  count check and **no** rejection; a second central's handle simply **overwrites**
  the single scalar `s_connHandle` (`:87`), and its writes land in the same RX ring
  undistinguished. This is a live multi-central exposure, not a hypothetical.
- **LAN** is single-client, last-in-wins: a second TCP accept evicts the first
  ([wifi_service.cpp:871-877](../src/wifi_service.cpp)).
- **BLE and LAN can both be live at once.** There is no connection-level
  arbitration. The only ownership is per-*transfer*: `sessionOrigin`, stamped at
  transfer START ([display_service.cpp:2144-2146](../src/display_service.cpp)),
  enforced per-frame by `frameOwnsSession()` and per-disconnect by
  `serviceBleDisconnectCleanup()`.

### No application code can drop a BLE link through the transport

- `BleTransport` ([ble_transport.h](../src/ble_transport.h)) exposes **no**
  `disconnect()`. `end()` is a full-controller teardown, and a no-op on nRF.
- ESP32 captures the conn handle (`s_connHandle`, `ble_transport_esp32.cpp:87`)
  but **never calls** `NimBLEServer::disconnect()`. The capability is one line
  away and unused.
- nRF has exactly one host-initiated disconnect in the whole firmware —
  `Bluefruit.disconnect(Bluefruit.connHandle())`
  ([device_control.cpp:857](../src/device_control.cpp)), inside DFU entry, reaching
  past the abstraction into Bluefruit directly. **Bluefruit's public `disconnect()`
  always sends reason 0x13 and ignores any argument** — a fact the seam design
  below has to respect.

### No stall detection reaches a hung `loop()`

- nRF has **no watchdog at all** ("every fault handler is `b .`",
  [od_log.h:40](../src/od_log.h)).
- ESP32's `loop()` is **not** subscribed to the task WDT: Arduino leaves
  `loopTaskWDTEnabled = false` and nothing here calls `esp_task_wdt_add()` for the
  loop task, so `loop()` is unsupervised. (Whatever `CONFIG_FREERTOS_WATCHDOG_TIMEOUT_S`
  is set to is immaterial — no framework code arms a loop watchdog from it.)
- The only wall-clock teardown is `checkTransferTimeouts()`
  ([display_service.cpp:584-638](../src/display_service.cpp)), and it measures total
  elapsed from transfer **START** — 15 minutes (`TRANSFER_WATCHDOG_MS = 900000`). It
  is a total-duration bound, **not** a stall/inactivity timeout: a transfer that
  stalls at minute 1 is still not torn down until minute 15, and a slow-but-
  progressing transfer is cut off at 15 minutes regardless of progress.

### An idle connected client is never dropped

- `pollActivity()` stamps `lastActivityMs` whenever `connCount > 0`
  ([main.cpp:366](../src/main.cpp)) — a live link is treated as activity in
  itself. So a client that connects, authenticates, and goes silent holds the
  device out of its idle path **forever**.
- `session_timeout_seconds` ([encryption.cpp:254-265](../src/encryption.cpp))
  measures from session START not last activity, clears the *session* but **not**
  the *link*, and is only evaluated when a command arrives — so it never fires on
  a silent client. It defaults to 0 (disabled).
- There is **no** BLE idle link-drop. LAN has one (`OD_LAN_READ_TIMEOUT_S = 30`,
  [wifi_service.cpp:952](../src/wifi_service.cpp)); BLE has no equivalent.

### State with no disconnect-time reset (Phase 2 surface)

Confirmed missing or open-coded, i.e. what an abort must newly cover:

- `encryptionSession` — **not** cleared on BLE disconnect. Crypto state survives a
  link drop. `clearEncryptionSession()` runs on session-timeout-at-command, a new
  auth, config reload ([communication.cpp:66](../src/communication.cpp)), and LAN
  teardown ([wifi_service.cpp:798,874](../src/wifi_service.cpp)) — but no BLE
  disconnect path is among them.
- `chunkedWriteState` (config chunked upload,
  [config_parser.h:47](../src/config_parser.h)) — **no reset function**; cleared
  only by open-coded inline assignments in `communication.cpp`, untouched by
  disconnect and by the watchdogs.
- The response TX ring — **no** flush/discard primitive (only `bleRxQueueDiscardTo`
  exists, RX side).
- `directWriteTouchSuspended` — reset only *inside* `cleanupDirectWriteState()`, so
  a teardown routed through the partial path can leave touch suspended.
- Buzzer and LED — serviced each loop pass; **no** clean session-teardown stop API
  exists, so Phase 2 must add idempotent `buzzerStop()` / `ledFlashStop()` rather
  than assume a primitive is there.

---

## Phase 1 — Nonce / replay correctness  ✅ SHIPPED

Shipped on this branch (`e2e95cd`…`19335e6`), recorded here for completeness. What
landed:

- The AES-CCM anti-replay state moved from a 512 B ring of raw counter values to a
  32 B sliding bitmap (`src/nonce_window.h`, a dependency-free pure state machine).
- Check split from commit: `nonceCheck()` decides and writes nothing; `nonceCommit()`
  runs only *after* the CCM tag verifies. So packet loss is no longer counted as
  tampering, and an unauthenticated peer cannot advance replay state.
- The forward-distance cap was **removed** and comparison made numeric, not modular:
  a counter ahead of `last_seen` is accepted at any distance (the tag is the gate),
  which fixed a cliff where a forward gap past the cap stranded the session
  unrecoverably. A consumed counter is still never re-accepted (`last_seen` only
  moves up; below it, bitmap-caught or rejected on width).

**Verified:** host suite 47,445 checks under `-Werror`+ASan/UBSan (and 1,635
failures against the pre-change code, proving the tests discriminate);
`nrf52840custom`, `esp32-c3-N16`, `esp32-N4` build.
**Not verified:** the entire hardware matrix.

Nothing in Phase 1 is reopened here. One carry-forward: an **auth-abuse disconnect**
was prototyped alongside the nonce work on a separate branch
(`feat/nonce-replay-and-auth-guard`) but is **not** on this branch, and is redesigned
fresh as Phase 4.

---

## Phase 2 — BLE-HAL foundation (mechanisms)

**Goal:** every transport/HAL *mechanism* the later phases build on — the portable
link-drop seam, the connection owner token, handle-aware connect events with
callback-side write filtering, the RX-activity clock, and the idempotent
abort-to-known-state routine. No *policy* lives here (Phase 3 decides when to refuse,
and when to drop); Phase 2 only makes each action possible and each fact observable.

### The link-drop seam — `BleTransport::disconnect(uint16_t handle)`

Add to the abstraction ([ble_transport.h](../src/ble_transport.h)) and implement per
target. It takes an explicit **handle**, not just "the current connection", because
Phase 3's admission needs to drop a *specific* link; pass the current handle for the
common case.

- **ESP32:** `s_server->disconnect(handle, BLE_ERR_REM_USER_CONN_TERM)`. Return the
  call's bool; log WARN on failure. Note the library already treats "the link is
  gone" as success — `NimBLEServer::disconnect` returns `true` for `BLE_HS_ENOTCONN`,
  `BLE_HS_EALREADY` and `UNK_CONN_ID` (`NimBLEServer.cpp:321-332`), so a WARN here
  means a genuine failure, not a benign race with a client that left first.
- **nRF:** `Bluefruit.disconnect(handle)`. Lift the pattern from
  [device_control.cpp:857](../src/device_control.cpp) but keep
  `restartOnDisconnect(true)` (unlike DFU, which disables it).

**Reason fixed at 0x13, and the seam hard-codes it.** A host-initiated disconnect
must use a Core-Spec-legal `HCI_Disconnect` reason. `BLE_ERR_REM_USER_CONN_TERM`
(**0x13**) is legal; `BLE_ERR_CONN_LIMIT` (0x09) is **not**, and the controller
silently rejects it (0x12) — the gatecrasher stays connected while the code looks
like it worked. The stacks are asymmetric, and both were read rather than assumed:

- NimBLE takes a reason and *defaults it to 0x13* —
  `disconnect(uint16_t connHandle, uint8_t reason = BLE_ERR_REM_USER_CONN_TERM)`
  (`NimBLEServer.h:66`), forwarded to `ble_gap_terminate`.
- Bluefruit takes **only a handle** — `AdafruitBluefruit::disconnect(uint16_t conn_hdl)`
  (`bluefruit.h:171`) delegates to `BLEConnection::disconnect(void)`, which calls
  `sd_ble_gap_disconnect(_conn_hdl, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION)`
  (`BLEConnection.cpp:206`). There is no reason parameter to pass, let alone one to
  honour.

So the seam exposes no `reason` parameter: 0x13 is the only value this plan wants,
the value NimBLE already defaults to, and the only value nRF can send. Both stacks
do take a **handle**, which is what the seam's signature carries.

**Also fix the inbound reason, which currently lies (ESP32).** Not a new feature —
a correctness fix to what is already logged. `s_disconnectReason` is a `uint8_t`
([ble_transport_esp32.cpp:35](../src/ble_transport_esp32.cpp)) assigned from
NimBLE's `int reason` with a truncating cast (`:99`). NimBLE uses two ranges: HCI
reasons wrapped as `BLE_HS_ERR_HCI_BASE + code` (`0x200 + code`), and host-layer
`BLE_HS_E*` codes in `1..31`. The cast keeps only the low byte, so an HCI reason
survives by luck (`0x213 & 0xFF == 0x13`) while `BLE_HS_ENOTCONN` (7) truncates to
`0x07` and reads back as the unrelated HCI "memory capacity exceeded". The log at
[main.cpp:472](../src/main.cpp) then prints it as decimal `%u`, so the two collide
on screen as well as in storage. nRF is unaffected — it stores a raw HCI `uint8_t`
from the SoftDevice with no wrapping ([ble_transport_nrf.cpp:38,135](../src/ble_transport_nrf.cpp)).

Fix: widen `s_disconnectReason` and `takeDisconnectedEvent`'s out-param to
`uint16_t` ([ble_transport.h:81](../src/ble_transport.h), one caller at
[main.cpp:471](../src/main.cpp)), drop the cast, and log `0x%03X` so a wrapped HCI
reason (`0x213`) and a host reason (`0x007`) are visibly distinct. No enum, no
classifier — just stop discarding half the value.

*Deferred, deliberately:* normalizing the inbound reason into an `OdDiscReason`
enum (`SUCCESS / REMOTE / LOCAL / TIMEOUT / MIC_FAILURE / OTHER`). Nothing in
Phases 2–4 branches on *why* a link dropped — the abort runs the same teardown
regardless, and a self-initiated drop is identified by its `*DropPending` flag, not
by reading the reason back. The classifier would feed a log line and nothing else.
The likely first real consumer is MIC-failure handling (0x3D signals encryption
desync); when that lands it is a small header and a `switch`, and the `uint16_t`
raw value preserved here is exactly its input, so nothing above has to be redone.

All disconnect calls are made from the **loop task** (a `serviceBleLinkDrop` hook, or
inline in the loop-serviced helpers), never a stack callback — a callback that severs
its own link mid-dispatch is exactly the class of bug `#132` removed.

### Owner token

A tiny arbiter, one new translation unit (`src/link_owner.h/.cpp`) or folded into
`communication.cpp`:

```
enum LinkOwner { OWNER_NONE, OWNER_BLE, OWNER_LAN };
bool linkClaim(LinkOwner who, uint16_t handle);  // succeeds iff OWNER_NONE or (who,handle) matches
void linkRelease(LinkOwner who);                 // no-op if `who` is not the holder
LinkOwner linkOwner(void);
uint16_t  linkOwnerHandle(void);                 // valid only when linkOwner()==OWNER_BLE
```

The token is **connection-level**: at most one transport-and-link owns the session at
a time. `OWNER_LAN` needs no handle (a single TCP client); `OWNER_BLE` **does** — the
enum alone cannot distinguish two BLE centrals, so the claim carries the conn handle
and the arbiter records it. This recorded handle is the authoritative "who owns the
link", separate from the transport's `s_connHandle` scalar, which the newest connect
always overwrites.

Phase 2 establishes only the *mechanism and the baseline*: the first BLE connect
claims `OWNER_BLE` with its handle; disconnect releases it (wired into `abort` below).
Deciding what to do with a *second* contender is Phase 3 policy (it refuses; see
[the governing decision](#the-governing-decision-admission-never-evicts)). Single writer (loop task), so plain globals suffice — no locking,
per the same argument that lets `g_commandOrigin` be a bare global
([communication.cpp:30-36](../src/communication.cpp)).

### Handle-aware connect events + callback-side write filtering

Two transport changes that the current HAL cannot express, needed before any
admission policy can run:

- **Handle-bearing connect event.** Today `takeConnectedEvent()` returns only a bool
  ([ble_transport_esp32.cpp:341](../src/ble_transport_esp32.cpp)) and connect/disconnect
  events are coalescing booleans, so two connects before loop() lose an event and
  loop() cannot tell which handle arrived. Change the event to carry the connecting
  handle, so loop() (Phase 3) can act on a specific newcomer.
- **Callback-side write filtering.** The ESP32 write callback compares
  `connInfo.getConnHandle()` against `linkOwnerHandle()` and **drops a non-owner's
  write at the callback**, before it enters the shared RX ring. This is a mechanism,
  not policy, and it is required: with the token claimed by the incumbent, a second
  central's writes are ignored *immediately* — even during a ~16 s blocked loop, when
  loop-side refusal has not run yet and the gatecrasher could otherwise inject a full
  transfer's worth of commands. So Phase 2 alone already neutralises the multi-central
  injection hole; Phase 3 only adds the cleaner active disconnect. nRF
  needs no filter (one central, hardware-enforced).

### The RX-activity clock (BLE HAL)

Phase 3's idle drop needs to know how long a link has been *silent*, but this is HAL
infrastructure, so Phase 2 owns it. Today's `lastActivityMs` cannot serve:
`connCount > 0` re-stamps it every pass ([main.cpp:366](../src/main.cpp)), so a
live-but-quiet link never ages.

The clock belongs to the BLE HAL's RX intake: `bleRxQueuePush()`
([command_queue.cpp:50](../src/command_queue.cpp)) is the single point every inbound
frame passes through on both targets. That function lives in `command_queue.cpp`, so
the storage lives there too (a transport file-static cannot be reached from it); the
transport exposes the accessor:

```
uint32_t bleMsSinceLastRx(void);   // 0 when not connected — see the init rule below
```

**Stamp only a frame that was actually queued.** `bleRxQueuePush()` already returns
`false` for an empty / oversized / ring-full frame; stamp `s_lastRxMs` **only on the
`true` path**. This is load-bearing, not tidiness: if malformed or saturating writes
refreshed the clock, an unauthenticated peer could hold the link open forever with
garbage that never authenticates and never reaches the auth-abuse counter (Phase 4).
Stamping only real, queued frames means such a flooder either sends valid commands —
caught by Phase 4 — or sends droppable garbage that never refreshes the clock, so
Phase 3's idle drop fires.

**Written on the BLE callback task.** `bleRxQueuePush()` runs from the write callback
— the NimBLE host task on ESP32, the Bluefruit callback task on nRF (and, under the
`rtos_malloc`-failure inline fallback, the SoftDevice task). Not the loop task. So it
is a genuine cross-task field:

- `static uint32_t s_lastRxMs`, accessed with `__atomic_store_n` / `__atomic_load_n`
  (`__ATOMIC_RELAXED`) — the discipline the RX ring in this same file already uses
  ([command_queue.cpp:62,92](../src/command_queue.cpp)). Plain `volatile` is **not**
  a correct inter-task tool in C++ and is not used here; the deferred/inline-fallback
  callback paths can even run on two different tasks, which relaxed atomics handle
  cleanly (monotonic last-writer-wins, no torn word) and `volatile` does not.
- `millis()` is safe in these callback contexts (ESP32: `esp_timer_get_time`,
  ISR-safe; nRF: `xTaskGetTickCount`, and the fallback runs on the event task, not a
  hardware ISR).

Stamping at intake rather than at loop-side observation (watching `bleRxQueueHead()`
advance) matters: the loop can sit inside a ~16 s EPD refresh, so a loop-side stamp
would record when loop() *drained* the frame, not when it was queued — inflating
"silence" by whatever the loop was blocked on. Intake time is close enough to arrival
(a deferred callback adds sub-ms).

**Idle is measured from the later of connect and last RX** — the init fix. A naive
"`UINT32_MAX` until first RX" would make a freshly connected, still-silent client
instantly past any timeout. So the transport also records connect time (`s_connectMs`,
set on the connect event) and:

```
bleMsSinceLastRx() := millis() - max(s_connectMs, s_lastRxMs)   // 0 when not connected
```

A new client thus gets the full idle window before its first command, and the clock
resets to connect time on every reconnect.

### `abortToKnownState(reason, bool dropLink)`

New `src/session_guard.h/.cpp` (both targets; LAN parts under
`#ifdef OPENDISPLAY_HAS_WIFI`, **not** `TARGET_ESP32` — `esp32-N4` is ESP32 without
WiFi). Ordered teardown:

1. Log first (one line, the reason).
2. Optional client NACK — **skip when `dropLink`** (the link is about to go).
3. `cleanupDirectWriteState(true)` — panel power + touch-resume.
4. `cleanupPartialWriteOnDisconnect()`.
5. `resetPipeWriteState()`.
6. **new** `resetChunkedWriteState()` — a real primitive replacing the open-coded
   inline clears in `communication.cpp`; call it here and from those sites.
7. **new** `touchForceResume()` — asserts the suspend counter reached 0 and clears
   `directWriteTouchSuspended` even when teardown bypassed `cleanupDirectWriteState`.
   A new public idempotent API, not an existing primitive.
8. **new** `buzzerStop()` / `ledFlashStop()` — new public idempotent stop APIs (see
   ground truth: neither exists today).
9. `clearEncryptionSession()` — **new on the disconnect path**; today crypto state
   survives a link drop.
10. **new** response-ring flush primitive (`bleTxQueueReset`), the RX-side analogue
    of `bleRxQueueDiscardTo`.
11. If `dropLink`: `ble.disconnect(currentHandle)` (the seam).
12. `linkRelease(OWNER_BLE)` — the owner token defined above.

**Panel power is NOT force-killed here — deliberately.** An earlier draft added an
`epdSessionForceOff()` step "unless refreshing". That is wrong: `epdSessionForceOff()`
powers off every state except `PWR_OFF`, **including `PWR_WARM`** (the only early
return is `if (pwrmgmState == PWR_OFF) return`,
[display_service.cpp:420-421](../src/display_service.cpp)) — a disconnect during a
refresh is deferred, so by the time abort runs the panel can be WARM with
`epdRefreshInProgress` false, and the step would kill exactly the panel that must
survive. Panel power is handled correctly by steps 3–5: `cleanupDirectWriteState`
forces off only a `PWR_ACTIVE` (mid-transfer) session and no-ops on WARM, matching
the existing "ACTIVE-only teardown" invariant in `serviceBleDisconnectCleanup`. So a
WARM keep-alive panel survives an abort — including an auth-abuse or idle drop of a
client while the panel is warm from a prior push.

Idempotent and loop-task-only: every step is either already a no-op when its state
is inactive, or made one.

### The complete invocation set

Collected here rather than left implicit across three phases, because the value of a
single shared teardown routine depends entirely on every teardown actually reaching
it. Three callers, and one governing invariant: **`dropLink=false` iff the link is
already gone**, which only the first case satisfies.

| Condition | `dropLink` | Phase |
|---|---|---|
| Disconnect event serviced: `s_disconnectCleanupPending && !epdRefreshInProgress && !ownerStillUp` | `false` | 2 |
| `serviceBleIdleTimeout()`: connected `&& !transferActive() && bleMsSinceLastRx() > OD_BLE_IDLE_TIMEOUT_MS` | `true` | 3 |
| Auth-abuse counter reaches its threshold, **after** the bounded TX barrier drains the `FE` or `OD_AUTH_ABUSE_FLUSH_MS` expires | `true` | 4 |

**Explicitly not a caller: refusing a contender.** Admission calls
`ble.disconnect(newHandle)` (or `incoming.stop()` on LAN) and nothing else — no
`abortToKnownState`, no `s_disconnectCleanupPending`, no `linkRelease`. The
incumbent's session must be untouched. This is the case most likely to be got wrong
in implementation, since refusal and teardown sit in the same handler and differ only
in which handle they act on.

**Also not callers, deliberately.** `clearEncryptionSession()` at
[communication.cpp:66](../src/communication.cpp) (config reload) and
[encryption.cpp:261](../src/encryption.cpp) (session timeout) are crypto lifecycle,
not session aborts; they stay as they are.

**Unresolved: `checkTransferTimeouts()`.** The 15-minute watchdog
([display_service.cpp:584-638](../src/display_service.cpp)) still runs its own
open-coded teardown, and this plan cites *that very function* as the reason a single
shared routine is needed. It is not on the list above, and the reason must be stated
rather than left as an omission: the watchdog is **selective** (it kills one transfer
half, not the session) and deliberately does **not** drop the link or clear crypto —
the client is still connected and may legitimately retry, so routing it through an
abort that calls `clearEncryptionSession()` would force re-auth on a healthy link.
That is a behaviour change, not a refactor. Two honest options, to be decided before
Phase 2 code lands: leave it separate and say so here, or factor steps 3–6 (the
transfer-state subset, no crypto, no link) into a shared inner helper that both the
watchdog and `abortToKnownState` call. The second preserves the anti-drift argument;
the first is cheaper. Right now the plan implies the second while arguing for it and
doing neither.

### Wire `serviceBleDisconnectCleanup` through it

`serviceBleDisconnectCleanup` ([main.cpp:388-423](../src/main.cpp)) already defers
correctly and already checks `ownerStillUp`. Phase 2 routes its teardown body through
`abortToKnownState(..., dropLink=false)` (the link is already gone) so the disconnect
path and the abort path can never drift. Keeping two separate teardown paths is
exactly how the direct-write watchdog once tore down a panel while leaving its pipe
session live — a bug this branch already fixed in `checkTransferTimeouts`, and one a
single shared routine prevents from recurring.

**No special nRF deferral is needed for the session clear.** An earlier draft called
for deferring `clearEncryptionSession()` on nRF to avoid a `memset(session_key)`
racing an inline `aes_ccm_decrypt`. That race does not exist in the current
architecture: nRF's write callback only *enqueues*
([ble_transport_nrf.cpp:148-156](../src/ble_transport_nrf.cpp)); all decrypt and
dispatch happen on the loop task in `serviceBleRx()`
([main.cpp:513](../src/main.cpp)), and `serviceBleDisconnectCleanup` is already
loop-task. The abort — session clear included — runs on the loop task, never
concurrently with a decrypt. No `nrfSessionClearPending` machinery.

### Verification

Disconnect mid-direct-write, mid-partial, mid-pipe, mid-chunked-config-write, and
mid-refresh (WARM survives); assert every flagged state is clean afterward, touch is
resumed, buzzer/LED stopped, crypto cleared, TX ring flushed; assert a second
transfer starts clean. On ESP32, a second central's writes are dropped at the callback
while the token is held (mechanism check, before any Phase 3 policy). `bleMsSinceLastRx()`
returns 0 before connect and grows only when queued frames stop arriving. Host-buildable
parts of the token get a unit test on the `linkClaim`/`linkRelease` state machine.
Build all envs.

Two seam-specific bench checks that a build cannot cover. **The drop actually drops:**
call the seam from the loop task on both nRF and ESP32 and confirm the link goes down
on a scanner or the client — the 0x09 trap above is precisely a case where the code
looks like it worked, so "it compiled" proves nothing. **The reason log is honest:** a
real client disconnect logs a sensible HCI reason, and a NimBLE host-layer reason now
logs as `0x0xx` rather than masquerading as an HCI code.

---

## Phase 3 — Connection-exclusivity policy + idle drop

**Goal:** the *policy* on top of Phase 2's mechanisms — refuse any contender while the
slot is held, and reclaim the slot from an incumbent that has gone silent. Phase 2
already makes a second BLE central harmless (its writes are filtered, it cannot own
the token); Phase 3 makes it *clean* (actively disconnected) and closes the idle-link
hole. It consumes the owner token, the handle-bearing connect event, and
`bleMsSinceLastRx()` — all Phase 2 — and adds no new transport state.

### The governing decision: admission never evicts

**A contender is always refused while the slot is held. Reclaiming a slot is the job
of the idle timeout alone, never of the accept path.** These are two independent
mechanisms and this plan deliberately keeps them that way.

An earlier draft made admission a three-way rule (refuse if the incumbent is
transferring or young-idle; *evict* it if idle past a threshold, then admit the
newcomer). That is rejected. What it bought — a faster reclaim when a stale link
lingers — is not worth what it cost:

- **It made an incumbent's fate depend on whether someone else happened to knock.**
  The same idle client is kept or killed for reasons it cannot observe, which is
  hard to reason about and harder to test.
- **It needed a whole extra threshold** (evict-idle age) that this plan's own
  residual-risk list already flagged as the one requiring the most conservative
  tuning, since too aggressive a value refuses a legitimate reconnect.
- **It put a multi-step teardown at a stack-event boundary** — disconnect incumbent,
  `abortToKnownState`, release token, then let the newcomer claim — with the newcomer
  already connected throughout. Pure refusal never touches incumbent state at all.

The cost accepted in exchange is that a returning client waits out the idle timeout
rather than ~10 s. That cost is smaller than it looks, and it differs by transport:

- **BLE: mostly absorbed below us.** The firmware never sets a supervision timeout —
  it takes whatever the central negotiates (commonly ~4–6 s). So an incumbent that is
  genuinely *gone* is reaped by the link layer without firmware involvement, and the
  idle timeout only has to handle a client that is alive and silent. Refusing a
  contender in *that* case is arguably the correct answer anyway.
- **LAN: genuinely dependent on the timeout.** TCP has no supervision timeout; a
  half-open socket persists indefinitely without keepalives. `OD_LAN_READ_TIMEOUT_S`
  (30 s) is the only reclaim path, which is precisely why LAN already has one.

### Enforcement

- **ESP32 admission — refuse, unconditionally.** On a handle-bearing connect event, if
  the token is held by a *different* BLE handle: `ble.disconnect(newHandle)` and stop.
  Do **not** raise `s_disconnectCleanupPending`, do **not** `linkRelease()`, do **not**
  inspect the incumbent's state at all — no `transferActive()` test, no idle-age test.
  The incumbent's session is untouched by construction rather than by a guard that
  could be got wrong. nRF gets the same refusal free from `begin(1,0)`; this bullet is
  the ESP32 analogue.
- **Proactive idle drop — the sole reclaim mechanism.** Since admission never evicts,
  this is the *only* way a held slot is ever released short of the client leaving. A
  loop-serviced `serviceBleIdleTimeout()`: if connected, `!transferActive()`, and
  `bleMsSinceLastRx() > OD_BLE_IDLE_TIMEOUT_MS`, drop via the seam +
  `abortToKnownState`. The BLE equivalent of LAN's `OD_LAN_READ_TIMEOUT_S`. An
  `#ifndef`-guarded define in the file that services it, not a wire/config field. Same
  `!transferActive()` gate; the from-START watchdog remains the backstop for a
  transfer that progresses but never ends.
  - *The value is now load-bearing and is deliberately left unpinned here.* An earlier
    draft defaulted it to 60 s, chosen when evict-idle (~10 s) was the fast reclaim
    path and this was only a backstop. With eviction gone that reasoning no longer
    applies: this timeout alone determines how long a returning client is locked out
    by a stale-but-alive incumbent. 60 s is very likely too generous now, but the
    right number follows from measured `py-opendisplay` behaviour — the longest
    legitimate mid-session silence, which is the floor — not from picking a smaller
    round number here. Pin it against that measurement before Phase 3 code lands, and
    record the measurement in the comment on the define.
  - *Why it cannot live where its LAN cousin does, and what that costs.*
    `OD_LAN_READ_TIMEOUT_S` is **not** a local tunable: it is defined at
    [opendisplay_protocol.h:984](../include/opendisplay_protocol.h) and documented at
    `:84` and `:945` as a client-visible contract ("the server drops a client only
    after `OD_LAN_READ_TIMEOUT_S` with no traffic"). Its home is the wire header
    because the client is entitled to know the number. The hard constraint forbids
    touching that header, so the BLE timeout is forced local — deliberately
    asymmetric with the LAN one, and invisible to clients except through the
    client-side CI assertions below. That is the accepted trade, not an oversight: a
    wrongly-dropped BLE client reconnects, so the cost of the client not knowing the
    exact number is bounded. If the BLE timeout ever needs to be genuinely
    client-visible, that is a wire change and goes through `../opendisplay-protocol`
    first — at which point it belongs in the protocol header beside its LAN cousin,
    not in firmware.
  - *Deep sleep:* leave `lastActivityMs` and the deep-sleep quiet window alone — this
    is a *link* drop, not a sleep decision. After it `connCount` falls to 0,
    `pollActivity` stops re-stamping, and the existing idle/deep-sleep path takes over.
- **LAN, one consistent model — including LAN-vs-LAN.** The token is connection-level,
  so a LAN accept while *any* transport owns the slot is refused (`incoming.stop()`),
  and symmetrically a BLE connect while LAN owns is refused. `handleWiFiServer` accept
  ([wifi_service.cpp:869-877](../src/wifi_service.cpp)) gains the token check and
  `linkClaim(OWNER_LAN, 0)`.

  **This is a behaviour change for LAN, not just a new cross-transport check.** Today
  that path is unconditional last-in-wins: a second TCP accept tears down TLS, clears
  crypto and stops the previous client, with no test of what it was doing. Under the
  rule above it becomes a refusal, which matters more on LAN than on BLE because TLS
  bypasses app-layer auth by design — so today *any* host on the network can kill an
  in-flight display push simply by opening a socket, with no credentials. Refusing
  closes that.

  **A pre-existing bug on the same path, fixed by the same change.** The accept-side
  eviction clears TLS/crypto but never calls `requestTransferSessionCleanup()` — unlike
  `disconnectWiFiServer()`, which does ([wifi_service.cpp:807](../src/wifi_service.cpp)).
  So an evicted client's in-flight direct-write/pipe/partial state stays live, and
  because both clients are `ORIGIN_LAN`, `frameOwnsSession()` does not stop the *new*
  client's frames from landing in the *evicted* one's transfer — the same class of hole
  as the ESP32 multi-central case. Making the path refuse rather than evict removes the
  bug by removing the eviction; nothing is left needing the cleanup call.

  LAN's reclaim path is unchanged and remains `OD_LAN_READ_TIMEOUT_S`
  ([wifi_service.cpp:952](../src/wifi_service.cpp)), which already drops an idle client
  after 30 s. It needs no new clock: `lastLanActivityMs` is a true RX-activity clock,
  stamped at connect, TLS-handshake completion, bytes read and frame dispatch
  ([wifi_service.cpp:886,910,946,972](../src/wifi_service.cpp)) and — unlike BLE's
  `lastActivityMs` — never re-stamped merely for being connected. It is the LAN
  equivalent of what Phase 2 builds for BLE, init rule included.
  - *One weakness to fix while depending on it.* `:946` stamps on `got > 0` — any bytes
    read, not just valid frames. That is exactly the failure Phase 2 is emphatic about
    avoiding for BLE ("if malformed or saturating writes refreshed the clock, an
    unauthenticated peer could hold the link open forever with garbage"). LAN has it
    today: a plain-mode flooder defeats both the 30 s read timeout and any policy built
    on that clock. Now that refusal makes the idle timeout the *only* reclaim path, the
    clock has to be honest — stamp on a successfully parsed frame, not on raw bytes.

### One thing to get right (easy to assume wrong)

The ESP32 central cap **cannot** be forced to 1 with a `-D` build flag — the
`CONFIG_BT_NIMBLE_MAX_CONNECTIONS = 3` in the precompiled `sdkconfig.h` wins, and a
local override is silently inert. Exclusivity must be enforced in the connect handler,
as above, not by config. (`serviceBleDisconnectCleanup`'s `ownerStillUp` guard is
**already** unconditional as of PR `#132`, [main.cpp:404-409](../src/main.cpp) — Phase
3 adds policy, not that restructuring.)

### Verification

Two centrals against one ESP32 (second always refused, whatever the incumbent is doing);
BLE⇄LAN arbitration both directions; a refused stranger's disconnect does not tear
down the incumbent (the `esp32-N4` no-WiFi path specifically). Idle drop: a client
that connects, authenticates, and idles past the timeout is dropped; a fresh client
gets the full window before its first command (the init fix from Phase 2); a streaming
client is not dropped; a keepalive-sending client is not; after a drop the device
returns to advertising/idle.

---

## Phase 4 — Auth-abuse disconnect

**Goal:** drop the link after a bounded run of BLE commands that never authenticate,
so an unauthenticated peer cannot hold the exclusive slot (on ESP32, the *only* slot
the owner token would otherwise hand it) indefinitely.

### Design (fresh — a prototype exists off-branch but is not adopted wholesale)

`feat/nonce-replay-and-auth-guard` carries `fbc7ab2`/`b4fafb5`, which implement this
but (a) drop the link **inline** on nRF — flagged as loop-starving — and (b) place
two `serviceBleAuthAbuseDisconnect()` call sites in the per-target loop arms that
`#132` then merged, so they no longer have a home. Reuse the *counter* logic; drop
the placement.

- **Count only BLE.** A per-session counter of consecutive commands answered with
  `RESP_AUTH_REQUIRED`, incremented **only when `g_commandOrigin == ORIGIN_BLE`**.
  The generic auth gate at [communication.cpp:584,591](../src/communication.cpp) and
  the config-write sites at `:410,472` are also reachable via the LAN-TLS bypass,
  where app-layer auth is intentionally unnecessary; counting those without the
  origin gate would let LAN-TLS traffic increment a counter that disconnects **BLE**.
  Reset to 0 on any authenticated command.
- **Threshold 10** (justify against the client's legitimate handshake, which
  authenticates within one exchange — 10 is generous). Overflow raises
  `s_authAbuseDropPending`; a loop-serviced `serviceBleAuthAbuseDisconnect()` handles
  it. One placement, both targets — the whole reason Phase 2's seam and the unified
  loop exist. Per the threshold discipline above, the count lives `#ifndef`-guarded in
  `communication.cpp` beside the auth gate that increments it, and
  `OD_AUTH_ABUSE_FLUSH_MS` beside the servicer that enforces it — not in a shared
  header.
- **Deliver the final `FE` before dropping — with a real barrier, not one flush.**
  The last `00 xx FE` must reach the client, or it is dropped with no reason. A single
  `serviceBleTx()` then disconnect does **not** guarantee that: TX deliberately
  retains an entry on mbuf backpressure or a missing CCCD
  ([command_queue.cpp:190](../src/command_queue.cpp)), and the final response may not
  even enqueue if the 10-slot ring is full. So the drop is gated on a bounded barrier:
  `serviceBleAuthAbuseDisconnect()` drains TX each loop pass and calls the seam only
  once the TX ring has drained the `FE` **or** a bounded deadline
  (`OD_AUTH_ABUSE_FLUSH_MS`, ~500 ms) elapses — then it drops regardless, so a
  wedged/un-draining client cannot keep the abuser attached. Then
  `abortToKnownState(dropLink=true)` (which releases the token).

### Depends on

Phase 2 (the seam, `abortToKnownState`, the owner token) and Phase 3 (it slots into
the same admission/idle policy layer).

### Verification

A BLE peer sending N unauthenticated commands is dropped at the threshold with the
`FE` delivered first (confirmed on a sniffer, since the barrier is the subtle part);
the drop still happens within the deadline if the client stops reading; a legitimate
client authenticating on its first exchange is never dropped; the counter resets
across a good command; **LAN-TLS traffic never increments it**; on nRF the drop is not
loop-starved.

---

## Verification model

Every phase distinguishes two states, because "the code merged" and "the gap
closed" are not the same claim. Phase 1 is the live example: it is shipped and
host-tested, yet its entire hardware matrix is unrun — landed, not closed.

- **Landed** = builds on all envs + host tests pass. A phase may merge here.
- **Closed** = its companion HIL script has passed on **both** an nRF and an ESP32
  board. The plan tracks a phase as open until then.

The HIL scripts are the executable form of each Verification section, under
`tests/`, pytest driving a real device through `py-opendisplay`/bleak
(`tests/serial_stall_test.py` is the existing template):

| Phase | Script | Asserts |
|---|---|---|
| 1 (retroactive) | `test_nonce_gap.py` | a transfer survives a forced >256 forward counter gap; a nonce-dropped `0x0081` frame is repaired by the client's SACK path and the upload completes |
| 2 | `test_abort_state.py` | disconnect mid-{direct, partial, pipe, chunked-config, refresh}; every flagged state clean afterward, touch resumed, buzzer/LED stopped, crypto cleared, TX flushed, WARM panel survives; a gatecrasher's writes are dropped at the callback while the token is held; `bleMsSinceLastRx()` is 0 pre-connect and ages only on true silence |
| 3 | `test_exclusivity.py` | two centrals against one ESP32 → second always refused, incumbent idle or transferring; a second LAN client is refused, not evicted, and the first's transfer survives; BLE⇄LAN arbitration both directions; refused-stranger disconnect does **not** tear down the incumbent (the `esp32-N4` no-WiFi path) |
| 3 | `test_idle_drop.py` | a fresh silent client survives its first window then is dropped; a streaming client is not; a keepalive-sending client is not; the device returns to advertising after the drop |
| 4 | `test_auth_abuse.py` | N unauthenticated BLE commands → drop at the threshold with `FE` delivered first; drop still occurs within the deadline if the client stops reading; a first-exchange auth is never dropped; the counter resets across a good command; LAN-TLS never increments it; on nRF the drop is not loop-starved |

**Threshold drift is caught in the client's CI, not ours.** These thresholds
assume specific `py-opendisplay` behaviours (handshake authenticates
within one exchange; retransmits carry fresh, higher counters; keepalive cadence).
Add an assertion of each to `py-opendisplay`'s test suite, so a client change that
would invalidate a firmware constant breaks *there* — the same move already used
for the `0x04`-NACK reasoning recorded in `sendPipeNack()`. Every
threshold-triggered drop also logs at WARN with the measured value, so field tuning
has data rather than guesses.

## Cross-cutting: what still has no watchdog

None of Phases 2–4 add a loop-liveness monitor. So a `loop()` genuinely wedged inside
a non-yielding operation is still uncaught on nRF (no watchdog) and on ESP32 (`loop()`
unsubscribed from the TWDT). This is **out of scope** and recorded as residual risk,
not silently dropped. The realistic mitigation — subscribe `loop()` to the ESP32 TWDT
and add an nRF hardware WDT fed from `loop()` — is a separate effort whenever it is
taken up; it is the true "supervisor," and it is none of the four phases here.

## Deliberately not changed

- No wire/protocol/config-schema change (hard constraint).
- No `include/opendisplay_protocol.h` or `include/opendisplay_structs.h` edit.
- The from-START transfer watchdog stays as the backstop; Phase 3's idle drop is
  additive, not a replacement.
- The nonce subsystem (Phase 1) is not reopened.

## Residual risk (honest list)

These are the gaps this plan **cannot** design away, distinct from the ones it now
tracks as work (HIL verification, threshold pinning — those have owners and exit
criteria above, so they are no longer "risk").

- **No loop-liveness watchdog** (see the watchdog footnote above). A `loop()`
  wedged inside a non-yielding operation is still uncaught on nRF, and a true hard
  fault is unrecoverable there. Deliberately left as a separate future effort.
- **Thresholds remain heuristics even when pinned.** The mandatory
  client-behaviour comment on each define, plus the
  client-side assertions, make the assumptions legible and drift-detectable, but the
  numbers are still judgement calls against a client that can change. The auth-abuse
  drop is self-limiting (a wrongly-dropped client reconnects). The one that now
  carries real weight is `OD_BLE_IDLE_TIMEOUT_MS`: with admission refusing rather than
  evicting, it is the sole path by which a held slot is ever reclaimed, so too
  generous a value locks out a returning client for its full duration and too
  aggressive a value drops a client that was legitimately between commands. It is
  pinned against measured client behaviour rather than chosen, and it is the number to
  revisit first if field behaviour disappoints.
- **A wedged transfer holds the slot for up to 15 minutes, and now has no escape
  hatch.** Both the idle drop and (previously) eviction gate on `!transferActive()`, so
  an incumbent that started a transfer and then stopped making progress is neither
  dropped nor displaced until `TRANSFER_WATCHDOG_MS` fires. That is a consequence of
  the from-START watchdog being a total-duration bound rather than a stall timeout —
  identified in the ground truth above and not fixed by any phase here. Removing
  eviction did not create this, but it did remove the one path that could have
  short-circuited it, so it is recorded plainly. The fix is a genuine stall timeout
  gating on *transfer active **and** progressing*, using the same activity clocks
  Phase 2 and LAN already provide; it is a candidate for the next phase after this
  plan, alongside the loop-liveness watchdog.
- **"Closed" depends on hardware nobody has run yet.** The verification model makes
  this explicit rather than papering over it: until the HIL scripts pass on both an
  nRF and an ESP32 board, every phase — including Phase 1 — is landed, not closed.
