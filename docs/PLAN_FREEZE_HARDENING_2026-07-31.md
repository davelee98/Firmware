# Freeze-Hardening the OpenDisplay Firmware — 2026-07-31

A fresh five-phase plan for the BLE e-paper firmware, written from the code as it
stands on `fix/nonce-replay-window` (tip `19335e6`, rebased onto the squashed
`main` at `aae5bdf`). It **supersedes** `PLAN_FREEZE_PROOFING_2026-07-26.md`.

This plan does **not** assume that plan's design was correct. Every claim below
was re-verified against the current tree; where the two disagree, this file wins,
and the disagreement is called out. The loop/BLE unification (`#132`) and the
nonce rewrite (this branch) both landed since the old plan was written, and both
changed the ground it stood on.

## Phase map

| # | Phase | Depends on | State today |
|---|---|---|---|
| 1 | Nonce / replay correctness | — | **Shipped** on this branch (`e2e95cd`…`19335e6`) |
| 2 | Connection exclusivity / owner token | Phase 0 seam | Not started |
| 3 | Abort-to-known-state / disconnect hardening | Phase 2 | Partially present (`serviceBleDisconnectCleanup`) |
| 4 | Auth-abuse disconnect | Phase 0 seam | Prototype exists off-branch, not here |
| 5 | Idle timer | Phase 0 seam | Not started |

**Phase 0 (foundational, folded into Phase 2):** a portable
`BleTransport::disconnect(reason)`. Phases 2, 4 and 5 all need to drop a link and
none can today. Building it once, in the transport seam, is the spine of the whole
plan — see Phase 2.

**Two cross-phase deliverables** thread through Phases 2–5 and are specified once
here rather than repeated:

- **`src/session_policy.h`** — every tunable threshold this plan introduces
  (evict-idle age, auth-abuse count, idle-drop timeout) in one header, each
  compile-time and overridable per env, each with a comment naming the *client
  behaviour it assumes*. No threshold is a wire/config field, so none touches the
  hard constraint. The point is that the assumptions are collected and legible, not
  scattered across four handlers as bare numbers.
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
  `s_connHandle` (`:87`), and its writes land in the same RX ring undistinguished.
  This is a live multi-central exposure, not a hypothetical.
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
  past the abstraction into Bluefruit directly.

### No stall detection reaches a hung `loop()`

- nRF has **no watchdog at all** ("every fault handler is `b .`",
  [od_log.h:40](../src/od_log.h)).
- ESP32's TWDT is armed (`CONFIG_FREERTOS_WATCHDOG_TIMEOUT_S=120`) but nothing
  subscribes `loop()` to it, and because `loop()` always yields (`delay()` →
  `vTaskDelay`) the IDLE task feeds it — so a `loop()` blocked inside a
  non-yielding operation (a ~16 s EPD refresh) is **not** caught.
- The only wall-clock teardown is `checkTransferTimeouts()`
  ([display_service.cpp:584-638](../src/display_service.cpp)), and it measures from
  transfer **START**, not from last progress — 15 minutes
  (`TRANSFER_WATCHDOG_MS = 900000`). A transfer dribbling one byte every 14
  minutes is never caught.

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

### State with no disconnect-time reset (Phase 3 surface)

Confirmed missing or open-coded, i.e. what an abort must newly cover:

- `encryptionSession` — **not** cleared on BLE disconnect. Crypto state survives a
  link drop. `clearEncryptionSession()` runs only on session-timeout-at-command or
  a new auth.
- `chunkedWriteState` (config chunked upload,
  [config_parser.h:47](../src/config_parser.h)) — **no reset function**; cleared
  only by open-coded inline assignments in `communication.cpp`, untouched by
  disconnect and by the watchdogs.
- The response TX ring — **no** flush/discard primitive (only `bleRxQueueDiscardTo`
  exists, RX side).
- `directWriteTouchSuspended` — reset only *inside* `cleanupDirectWriteState()`, so
  a teardown routed through the partial path can leave touch suspended.

---

## Phase 1 — Nonce / replay correctness  ✅ SHIPPED

Shipped on this branch. Ground truth is
[`PLAN_PHASE1_NONCE_REPLAY_2026-07-26.md`](PLAN_PHASE1_NONCE_REPLAY_2026-07-26.md),
including its **Reversal of Decision A** (the forward cap was removed in `aef3a6b`;
comparison is numeric, not modular).

**Verified:** host suite 47,445 checks under `-Werror`+ASan/UBSan (and 1,635
failures against the pre-change code, proving the tests discriminate);
`nrf52840custom`, `esp32-c3-N16`, `esp32-N4` build.
**Not verified:** the entire hardware matrix, unchanged from the Phase 1 doc.

Nothing in Phase 1 is reopened here. The one carry-forward is that the **auth-abuse
disconnect** the old plan pulled into Phase 1 is **not on this branch** — it lives
on `feat/nonce-replay-and-auth-guard` and is redesigned fresh as Phase 4.

---

## Phase 2 — Connection exclusivity / owner token

**Goal:** at most one owning session at a time, chosen deterministically, on every
transport and target. Close the ESP32 multi-central hole and the BLE/LAN
double-ownership hole.

### Phase 0 seam (do this first) — `BleTransport::disconnect(uint8_t reason)`

The single most reused piece in the plan. Add to the abstraction
([ble_transport.h](../src/ble_transport.h)) and implement per target:

- **ESP32:** `s_server->disconnect(s_connHandle, reason)` when `s_connHandle !=
  BLE_HS_CONN_HANDLE_NONE`. Return the call's bool; log WARN on failure.
- **nRF:** `Bluefruit.disconnect(s_connHandle)` when `s_connHandle !=
  BLE_CONN_HANDLE_INVALID`. Lift the pattern from
  [device_control.cpp:857](../src/device_control.cpp) but keep
  `restartOnDisconnect(true)` (unlike DFU, which disables it).

**Reason code correctness — the trap the old plan flagged and it still applies.**
A host-initiated disconnect must use a reason in the Core Spec's legal
`HCI_Disconnect` allowlist. `BLE_ERR_REM_USER_CONN_TERM` (**0x13**) is legal;
`BLE_ERR_CONN_LIMIT` (0x09) is **not**, and a controller silently rejects it (0x12)
leaving the gatecrasher connected while the code looks like it worked. Neither
constant appears in `src/` today (verified), so this is introduced clean. Use
0x13, check the return, and cover it with a comment naming why 0x09 is wrong.

All disconnect calls are made from the **loop task** (a new `serviceBleLinkDrop`
hook, or inline in the loop-serviced helpers below), never from a stack callback —
the copy-and-flag contract the unified loop relies on
([main.cpp](../src/main.cpp) `serviceBle*` helpers). A callback that severs its own
link mid-dispatch is exactly the class of bug `#132` removed.

### Owner token

A tiny arbiter, one new translation unit (`src/link_owner.h/.cpp`) or folded into
`communication.cpp`:

```
enum LinkOwner { OWNER_NONE, OWNER_BLE, OWNER_LAN };
bool linkClaim(LinkOwner who);   // succeeds iff OWNER_NONE or already `who`
void linkRelease(LinkOwner who); // no-op if `who` is not the holder
LinkOwner linkOwner(void);
```

Single writer (loop task), so a plain `LinkOwner` global suffices — no locking, per
the same argument that lets `g_commandOrigin` be a bare global
([communication.cpp:30-36](../src/communication.cpp)).

### Enforcement

- **ESP32 BLE admission.** On the connect event
  ([main.cpp](../src/main.cpp) `serviceBleEvents` connect branch), if a session is
  already owned by a *different* live link, **refuse** the newcomer:
  `ble.disconnect(0x13)`. Capture the refused handle so the ensuing disconnect
  event does **not** raise `s_disconnectCleanupPending` for it — otherwise refusing
  a stranger tears down the incumbent's transfer (a new remote DoS). This is the
  ESP32 analogue of what nRF gets free from `begin(1,0)`.
- **Evict-idle preference.** Refusing outright strands a returning client when the
  incumbent link lingers after an abrupt loss (supervision timeout 4–32 s). So:
  if the incumbent has `!transferActive()` and last RX older than a threshold
  (~10 s), evict it (`ble.disconnect(0x13)` on the old handle) and accept the new
  one; refuse only when the incumbent is actively transferring. `transferActive()`
  already exists and already excludes a fatally-NACKed pipe
  ([display_service.cpp](../src/display_service.cpp)).
- **LAN.** `handleWiFiServer` accept
  ([wifi_service.cpp:869-895](../src/wifi_service.cpp)) already evicts the previous
  TCP client; gate it on `linkClaim(OWNER_LAN)` and, symmetrically, refuse/evict a
  LAN accept while BLE actively owns a transfer.
- **Release** on the owning transport's disconnect (BLE disconnect cleanup; LAN
  `disconnectWiFiServer`).

### Divergences from the old plan

- The old plan's Phase 4 assumed the ESP32 cap could be forced to 1 via a build
  flag; it can't (precompiled `sdkconfig.h`). Exclusivity must be enforced in the
  connect handler, as above.
- `serviceBleDisconnectCleanup`'s `ownerStillUp` guard is **already** unconditional
  (not inside `#ifdef OPENDISPLAY_HAS_WIFI`) — the old plan listed making it so as
  work; `#132` already did it ([main.cpp:404-409](../src/main.cpp)). Phase 2 only
  adds the token, not that restructuring.

### Verification

Two centrals against one ESP32 (second refused or incumbent evicted per rule);
BLE-then-LAN and LAN-then-BLE both arbitrated; refused-stranger disconnect does not
tear down the incumbent (the `esp32-N4` no-WiFi path specifically). Host-buildable
parts of the arbiter get a unit test on the `linkClaim`/`linkRelease` state
machine.

---

## Phase 3 — Abort-to-known-state / disconnect hardening

**Goal:** one idempotent routine that returns the device to a known-idle state from
*any* session, and close the reset-surface gaps found above.

### `abortToKnownState(reason, bool dropLink)`

New `src/session_guard.h/.cpp` (both targets; LAN parts under
`#ifdef OPENDISPLAY_HAS_WIFI`, **not** `TARGET_ESP32` — `esp32-N4` is ESP32 without
WiFi). Ordered teardown, each step already having a primitive except where noted:

1. Log first (one line, the reason).
2. Optional client NACK — **skip when `dropLink`** (the link is about to go).
3. `cleanupDirectWriteState(true)` — panel power + touch-resume.
4. `cleanupPartialWriteOnDisconnect()`.
5. `resetPipeWriteState()`.
6. **new** `resetChunkedWriteState()` — a real primitive replacing the open-coded
   inline clears in `communication.cpp`; call it here and from those sites.
7. **new** touch force-resume that asserts the suspend counter reached 0 and clears
   `directWriteTouchSuspended` even when teardown bypassed
   `cleanupDirectWriteState`.
8. Buzzer/LED stop.
9. `epdSessionForceOff()` **only if** `!epdRefreshInProgress` (a WARM post-refresh
   panel survives, unchanged).
10. `clearEncryptionSession()` — **new on the disconnect path**; today crypto state
    survives a link drop.
11. **new** response-ring flush primitive (`bleTxQueueReset`), the RX-side analogue
    of `bleRxQueueDiscardTo`.
12. If `dropLink`: `ble.disconnect(0x13)` (Phase 0 seam).
13. `linkRelease()` (Phase 2).

Idempotent and loop-task-only: every step is either already a no-op when its state
is inactive, or made one.

### Wire `serviceBleDisconnectCleanup` through it

`serviceBleDisconnectCleanup` ([main.cpp:388-423](../src/main.cpp)) already defers
correctly and already checks `ownerStillUp`. Phase 3 routes its teardown body
through `abortToKnownState(..., dropLink=false)` (the link is already gone) so the
disconnect path and the abort path can never drift — the old plan's split between
them is exactly how the direct-write watchdog once tore down a panel while leaving
its pipe session live (a bug this branch already fixed in `checkTransferTimeouts`).

### nRF deferred session clear

The `clearEncryptionSession()` added at step 10 must be **deferred** on nRF, not run
in the disconnect callback: a `memset(session_key)` landing mid-`aes_ccm_decrypt` on
Bluefruit's inline-fallback path (callback runs on the BLE task under heap pressure)
is a real race. Set `nrfSessionClearPending`, service from `loop()` when the
in-flight depth is 0 — the same copy-and-flag discipline as every other
loop-serviced BLE helper.

### Verification

Disconnect mid-direct-write, mid-partial, mid-pipe, mid-chunked-config-write, and
mid-refresh (WARM survives); assert every flagged state is clean afterward and touch
is resumed; assert a second transfer starts clean. Build all envs.

---

## Phase 4 — Auth-abuse disconnect

**Goal:** drop the link after a bounded run of commands that never authenticate, so
an unauthenticated peer cannot hold a slot (and, on ESP32, the *only* slot the owner
token would otherwise hand it) indefinitely.

### Design (fresh — a prototype exists off-branch but is not adopted wholesale)

`feat/nonce-replay-and-auth-guard` carries `fbc7ab2`/`b4fafb5`, which implement this
but (a) drop the link **inline** on nRF — flagged as loop-starving — and (b) place
two `serviceBleAuthAbuseDisconnect()` call sites in the per-target loop arms that
`#132` then merged, so they no longer have a home. Reuse the *counter* logic; drop
the placement.

- A per-session counter of consecutive commands answered with `RESP_AUTH_REQUIRED`
  (the two sites at [communication.cpp:584,591](../src/communication.cpp), plus the
  config-write sites at `:410,472`). Reset to 0 on any authenticated command.
- At a threshold (start at **10**; justify against the client's legitimate
  handshake, which authenticates within one exchange — 10 is generous), request a
  link drop **through the Phase 0 seam from loop()**, not inline.
- Ordering: the guard's final `00 xx FE` must already be in the response ring
  *before* the drop is requested, or the client is left with no reason. So: raise
  `s_authAbuseDropPending`, and a loop-serviced `serviceBleAuthAbuseDisconnect()`
  (placed after `serviceBleTx()` in the shared loop body,
  [main.cpp](../src/main.cpp)) drains TX, then `ble.disconnect(0x13)`, then
  `abortToKnownState`/`linkRelease`. One placement, both targets — the whole reason
  Phase 0 and the unified loop exist.

### Depends on

Phase 0 seam (the drop) and Phase 2 (release the token on drop). Independent of
Phase 3 except that it should call `abortToKnownState` for the teardown rather than
re-implementing it.

### Verification

A peer sending N unauthenticated commands is dropped at the threshold with the
`FE` delivered first; a legitimate client authenticating on its first exchange is
never dropped; the counter resets across a good command. On nRF confirm the drop is
not starved (the inline version's failure mode).

---

## Phase 5 — Idle timer

**Goal:** drop a BLE client that connects and then goes silent. Give BLE the
idle-link-drop that LAN already has.

### Design

The blocker is that `connCount > 0` currently counts as activity
([main.cpp:366](../src/main.cpp)), so `lastActivityMs` can never age out under a
live link. Introduce a **separate** last-RX timestamp that a *live link alone* does
NOT refresh — only actual inbound frames do — so silence is distinguishable from
connection.

- `lastBleRxMs`, stamped only when `bleRxQueueHead()` advances (real command
  arrival), never by link liveness.
- A loop-serviced `serviceBleIdleTimeout()`: if connected, `!transferActive()`, and
  `millis() - lastBleRxMs > OD_BLE_IDLE_TIMEOUT_MS`, request a link drop via the
  Phase 0 seam and `abortToKnownState`. Default the timeout generously (start at 60
  s; the LAN analogue is 30 s but LAN sockets are cheaper to re-establish) and make
  it a compile-time constant, overridable per env, **not** a wire/config field
  (keeps the hard constraint).
- Gate on `!transferActive()` so a long but legitimately-progressing transfer is
  never dropped — pair it with the from-START watchdog, which remains the backstop
  for a transfer that progresses but never ends.

### Interaction with deep sleep

Leave `lastActivityMs` and the deep-sleep quiet window alone; this is a *link* drop,
not a sleep decision. After the drop, `connCount` falls to 0, `pollActivity` stops
re-stamping, and the existing idle/deep-sleep path takes over naturally.

### Depends on

Phase 0 seam. Independent of Phases 2–4, but shares `serviceBle*` placement and
`abortToKnownState`, so it lands cleanest after them.

### Verification

A client that connects, authenticates, and idles past the timeout is dropped; a
client streaming a transfer past the timeout is **not**; a client sending periodic
keepalive commands is not; after the drop the device returns to advertising/idle.

---

## Verification model

Every phase distinguishes two states, because "the code merged" and "the gap
closed" are not the same claim and the old plan conflated them (it marked Phase 1
shipped while its entire hardware matrix was — and remains — unrun).

- **Landed** = builds on all envs + host tests pass. A phase may merge here.
- **Closed** = its companion HIL script has passed on **both** an nRF and an ESP32
  board. The plan tracks a phase as open until then.

The HIL scripts are the executable form of each Verification section, under
`tests/`, pytest driving a real device through `py-opendisplay`/bleak
(`tests/serial_stall_test.py` is the existing template):

| Phase | Script | Asserts |
|---|---|---|
| 1 (retroactive) | `test_nonce_gap.py` | a transfer survives a forced >256 forward counter gap; the old "Test 2b" (a nonce-dropped `0x0081` is repaired by SACK, upload completes) |
| 2 | `test_exclusivity.py` | two centrals against one ESP32 → second refused, or idle incumbent evicted; BLE⇄LAN arbitration both directions; a refused stranger's disconnect does **not** tear down the incumbent (the `esp32-N4` no-WiFi path) |
| 3 | `test_abort_state.py` | disconnect mid-{direct, partial, pipe, chunked-config, refresh}; every flagged state clean afterward, touch resumed, WARM panel survives, a second transfer starts clean |
| 4 | `test_auth_abuse.py` | N unauthenticated commands → drop at the threshold with `FE` delivered first; a client authenticating on its first exchange is never dropped; the counter resets across a good command; on nRF the drop is not loop-starved |
| 5 | `test_idle_drop.py` | a silent connected client is dropped past the timeout; a streaming client is not; a keepalive-sending client is not; the device returns to advertising after the drop |

**Threshold drift is caught in the client's CI, not ours.** `session_policy.h`'s
constants assume specific `py-opendisplay` behaviours (handshake authenticates
within one exchange; retransmits carry fresh, higher counters; keepalive cadence).
Add an assertion of each to `py-opendisplay`'s test suite, so a client change that
would invalidate a firmware constant breaks *there* — the same move already used
for the `0x04`-NACK reasoning recorded in `sendPipeNack()`. Every
threshold-triggered drop also logs at WARN with the measured value, so field tuning
has data rather than guesses.

## Cross-repo: the sibling firmwares carry a worse form of Phase 1

Not porting work inside this plan, but a **named deliverable**, not a footnote.
`Firmware_NRF54/src/opendisplay_pipe.c` and `Firmware_NRF/encryption.c` run the
same client and the same PIPE protocol, and both carry the nonce bug in a *worse*
form than this repo had: they mutate `last_seen_counter` / the replay window
**before** the CCM tag is verified (the D2 bug this repo already fixed), and use a
±32 window that has the same forward-gap cliff Phase 1 removed.

Deliverable: a `FINDINGS_*` doc in `opendisplay-protocol/agents/` mapping the defect
in each, plus a tracked issue per repo. Minimum parity set, mirroring Phase 1:
check/commit split (commit only post-tag), numeric ordering, no forward cap. Owner
of each firmware schedules the port; this plan does not block on it, but records
that the client-facing behaviour is only consistent once all four converge.

## Cross-cutting: what still has no watchdog

None of Phases 2–5 add a loop-liveness monitor, and the old plan's Phase 2 scope
cut removed the one it had. So a `loop()` genuinely wedged inside a non-yielding
operation is still uncaught on nRF (no watchdog) and on ESP32 (TWDT fed by the
yielding IDLE task). This is **out of scope** for this plan and recorded as residual
risk, not silently dropped. The realistic mitigation — arm the ESP32 TWDT over
`loop()` and add an nRF hardware WDT fed from `loop()` — is a separate phase whenever
it is taken up; it is the true "supervisor," and it is not any of the five here.

## Deliberately not changed

- No wire/protocol/config-schema change (hard constraint).
- No `include/opendisplay_protocol.h` or `include/opendisplay_structs.h` edit.
- The from-START transfer watchdog stays as the backstop; Phase 5's idle drop is
  additive, not a replacement.
- The nonce subsystem (Phase 1) is not reopened.

## Residual risk (honest list)

These are the gaps this plan **cannot** design away, distinct from the ones it now
tracks as work (HIL verification, threshold pinning, sibling-firmware parity —
those have owners and exit criteria above, so they are no longer "risk").

- **No loop-liveness watchdog** (see the watchdog footnote above). A `loop()`
  wedged inside a non-yielding operation is still uncaught on nRF, and a true hard
  fault is unrecoverable there. Deliberately left as a separate future effort.
- **Thresholds remain heuristics even when pinned.** `session_policy.h` and the
  client-side assertions make the assumptions legible and drift-detectable, but the
  numbers are still judgement calls against a client that can change. The Phase 4/5
  drops are self-limiting (a wrongly-dropped client reconnects); the one to tune
  conservatively is Phase 2's evict-idle, where too aggressive a value could refuse
  a legitimate reconnect.
- **"Closed" depends on hardware nobody has run yet.** The verification model makes
  this explicit rather than papering over it: until the HIL scripts pass on both a
  nRF and an ESP32 board, every phase — including Phase 1 — is landed, not closed.
