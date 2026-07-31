# Connection Policy — OpenDisplay Firmware

**Status:** normative ruleset. This document defines *what must be true*; it does not
schedule the work. Implementation is staged in
[`PLAN_FREEZE_HARDENING_2026-07-31.md`](PLAN_FREEZE_HARDENING_2026-07-31.md), whose
Phase 3 must conform to this document where the two disagree — this one wins.

Written against the tree at `fix/nonce-replay-window`. Every statement about current
behaviour is cited to `file:line` so a reviewer can re-check rather than trust.

No wire-protocol change: every rule here is enforced at the transport/HCI layer or in
firmware-local state. No new opcode, no new response code, no config-schema field.

**Hard constraint — ONE command queue.** There is exactly one RX command ring and one
TX ring, shared by all transports, and this policy must never introduce a per-connection
one. The RX ring is `PIPE_MAX_W + 2` slots ([command_queue.h:63](../src/command_queue.h))
— 18 or 34 depending on `PIPE_SMALL_DRAM_WINDOW` ([structs.h:46-54](../src/structs.h)) —
at `OD_BLE_MAX_FRAME` = 256 B each, so ~4.7 KB or ~8.8 KB. Replicating it across three
NimBLE connection slots would cost 14–26 KB on a device whose zlib window is 512 bytes.
Not a trade worth discussing.

This is not a constraint the policy merely tolerates; it is one the policy *enforces*.
R3 requirement 1 drops a non-owner's write at the callback, before it reaches the ring,
so only the owner's frames ever enter it — there is never a second client's traffic to
separate, and therefore never a reason to partition. Callback-side filtering and the
single queue are the same decision seen from two sides: without the filter you would be
pushed toward per-connection buffering to keep streams apart. It also keeps
`bleRxQueueDiscardTo(rxBoundary)` working unchanged — one ring, one boundary, one flush.

Where this document calls for per-connection *state* (R2's instance identity, R3's
event delivery), that state is **metadata only** — an epoch, a lifecycle state, an RX
boundary; on the order of 8 bytes per slot. Nothing that holds frames is ever
replicated per connection.

> **Revision note.** This document was reviewed against the tree after its first
> draft; that review found four defects that are corrected below and are called out
> where they land, because each is a trap an implementer would otherwise re-enter:
> the generation counter was allocated at the wrong moment (R2), refusal isolation
> needed far more than handle-bearing events (R3), the owner was released before the
> link was actually down (R3a), and the refresh BUSY-wait is *not* bounded on the
> FastEPD path (R5).

---

## Definitions

**Connection instance** — one physical link, from the stack's connect callback to its
matching disconnect. Identified by `(transport, handle, epoch)`; see R2.

**Admitted** — the connection instance currently holding the slot, i.e. the owner.

**Refused** — a connection instance the firmware has decided not to admit. It may be
physically established for a short time while being torn down. It is never the owner.

**Owner state** — `NONE`, `ACTIVE` (admitted and serviceable), or `DROPPING`
(firmware has requested termination; the link may still be physically up). See R3a.

**Inbound command** — a frame from the owner that reaches the dispatcher and is
recognised as a command. Not merely bytes; not merely a queued buffer. See R4.

---

## The rules

### R1 — One admitted client, globally

**At most one *admitted* connection may exist across all transports at any time.** BLE
and LAN are not independent slots. A device serving a BLE client has no LAN capacity,
and the reverse.

**Phrased in terms of admission, not physical links, because the physical form is
unachievable on ESP32.** NimBLE establishes a second central's link *before* it calls
`onConnect` ([ble_transport_esp32.cpp:81-93](../src/ble_transport_esp32.cpp)); there is
no pre-connection filter in the server API. So a transient second *physical* link
necessarily exists while it is being refused. R1 constrains what is *serviceable*; R3
constrains what that transient link can touch, which is nothing.

*Today R1 is false in every direction.* BLE and LAN can both be live simultaneously;
there is no connection-level arbitration, only per-*transfer* ownership (`sessionOrigin`,
stamped at each transfer START, [display_service.cpp:2159,2200,2712](../src/display_service.cpp)).
On ESP32 the single-transport case also fails: `CONFIG_BT_NIMBLE_MAX_CONNECTIONS = 3`
is baked into the precompiled NimBLE framework and cannot be lowered by a `-D`
override, and `onConnect` performs no count check — a second central's handle simply
overwrites the scalar `s_connHandle` (`:87`).

### R2 — Every connection instance carries a unique identity

**Identity is a triple, allocated per *physical connection*:**

```
(transport, handle, epoch)
```

- `transport` ∈ {`OWNER_BLE`, `OWNER_LAN`}.
- `handle` distinguishes simultaneous links on one transport (BLE conn handle; LAN
  uses 0, being single-socket by construction).
- `epoch` is a monotonically increasing counter making the identity unique *over time*.

> **The epoch is allocated in the connect callback, for every connection instance —
> admitted or not. It is emphatically NOT allocated on successful claim.** The first
> draft of this document said "incremented on every successful claim," which is
> self-defeating: a *refused* contender never claims, so it would carry no epoch, and
> table 7a row 4 — a refused contender that reused the incumbent's handle — could not
> be distinguished from the incumbent at all. Allocation must happen before the
> admission decision, because the identity is what the admission decision is *made
> on*. On admission the owner token copies the instance's already-allocated epoch.

**Why an epoch is needed at all.** BLE connection handles are small integers the stack
reuses: NimBLE allocates from 0 upward, so a client that disconnects and reconnects can
be handed the *same* handle. Any deferred operation carrying a stale handle — a queued
disconnect event, a pending abort, a write filter test — can otherwise match a
different, newer session and act on it. This firmware defers work by design:
`serviceBleDisconnectCleanup` can run tens of seconds late when `loop()` was blocked in
a refresh, a hazard the code already documents at
[main.cpp:398-403](../src/main.cpp). The epoch turns "same handle" into "same
connection instance," which is what every deferred consumer actually needs.

**Required properties:**

- **Comparison is on the full triple.** `handle` alone is never sufficient.
- **ESP32 needs per-live-handle instance state, not a scalar.** While NimBLE permits
  three links, the single `s_connHandle` ([ble_transport_esp32.cpp:87](../src/ble_transport_esp32.cpp))
  cannot represent them. A small fixed array indexed by handle is sufficient.
- **Publication must be atomic.** The triple is written on a stack-callback task and
  read on the loop task. A multi-field `volatile` struct is not an atomic snapshot;
  publish a single word (packed handle+epoch) or guard with the `__atomic_*`
  discipline the RX ring already uses ([command_queue.cpp:62,92](../src/command_queue.cpp)).
- **Scope is one boot.** Uniqueness across reset is not required and is not claimed:
  no deferred RAM state survives a reset.
- **Wrap.** A `uint32_t` epoch wraps after 2^32 connections. The normative invariant
  is that no outstanding event may survive a full counter cycle — trivially true here,
  but stated so a narrower counter is not substituted casually.

### R3 — A contender is always refused, and refusal is inert

**While the slot is held, an incoming connection is refused** — before establishment
where the stack allows it, otherwise by immediate disconnection. Admission never evicts
an incumbent; reclaiming a held slot is exclusively the job of R4.

**Refusal must not perturb device state.** Refusing must not run `abortToKnownState`,
raise `s_disconnectCleanupPending`, call `linkRelease`, touch the encryption session,
touch transfer state, or alter panel power. The incumbent must be unable to observe
that a contender arrived.

> **Handle-bearing events are necessary but nowhere near sufficient.** The first draft
> stopped at "ignore the refused contender's disconnect event." Review of the ESP32
> callbacks showed that a contender perturbs shared state *before any loop-side
> decision runs*. All of the following are live defects in the current tree.

On ESP32, every one of these is global scalar state that any central can move:

| Shared state | Site | What a contender does to the incumbent |
|---|---|---|
| `s_notifySubscribed` | [esp32:81-93](../src/ble_transport_esp32.cpp) (connect), [:129](../src/ble_transport_esp32.cpp) (subscribe, `(void)connInfo`) | Clears/overwrites the incumbent's apparent notify-readiness, stalling its TX |
| RX ring | [:135](../src/ble_transport_esp32.cpp) `onWrite`, `(void)connInfo` | Injects commands into the incumbent's stream; can fill the ring and drop incumbent frames |
| TX / notify | [:269-277](../src/ble_transport_esp32.cpp) | **Leaks incumbent responses to the contender** — see below |
| `s_connHandle` | [:87](../src/ble_transport_esp32.cpp) | Overwritten, so link tuning and any future disconnect target the wrong link |

**The notify leak is the sharpest of these and is present today.** `BleTransport::notify`
calls `s_txCharacteristic->notify(data, len)` — the two-argument overload. NimBLE's
signature is `notify(value, length, connHandle = BLE_HS_CONN_HANDLE_NONE)`, documented
as "or `BLE_HS_CONN_HANDLE_NONE` to send the notification to **all subscribed
clients**." So a second central that connects and subscribes receives every response
the incumbent is sent, including authentication traffic — with no policy decision
having been made, and before `loop()` runs at all.

**Therefore R3 requires, at the callback boundary:**

1. **Per-link write filtering.** Drop a non-owner's write in `onWrite` before it
   reaches the RX ring.
2. **Per-link subscribe filtering.** A non-owner's `onSubscribe` must not move the
   owner's notify state; subscription state must be per-instance.
3. **Handle-targeted notification.** `notify()` must pass the owner's conn handle, so
   a subscribed non-owner receives nothing. This is a one-argument change and it
   closes a live leak independent of the rest of this policy.
4. **Identity-bearing disconnect events**, with every consumer ignoring an event whose
   identity does not match the owner. Today `takeDisconnectedEvent`
   ([ble_transport.h:81](../src/ble_transport.h)) carries a reason and an RX boundary
   but no handle, so this is a new transport requirement — *additional* to the
   handle-bearing connect event already planned.
5. **Connection state must survive lost edges — via a table, not a queue.** See below.

#### Requirement 5 in detail: the instance table

Connect and disconnect events are coalescing booleans today
([ble_transport_esp32.cpp:33-34](../src/ble_transport_esp32.cpp),
[:341-354](../src/ble_transport_esp32.cpp)), and the header records the weakness: "a
second same-type event arriving inside the check-then-clear window is lost"
([ble_transport.h:66-71](../src/ble_transport.h)). The side-band data —
`s_disconnectReason`, `s_rxBoundaryAtDisconnect`, `s_connHandle` — is single-slot too,
so each event overwrites the last.

Today that is tolerable because `serviceBleEvents()` decides nothing per-connection: a
connect means "reset `rebootFlag`, update MSD, tune the link," a disconnect means
"flush the RX ring to the boundary, raise the cleanup flag"
([main.cpp:461-500](../src/main.cpp)). Under this policy each event drives an
*admission decision about a specific instance*, so a lost event is a lost decision:

- **Lost connect → an unrefused contender.** Two centrals connect while `loop()` is
  blocked in a refresh; the flag is set twice and read once. One is refused; the other
  is connected, never evaluated, and invisible to the loop.
- **Lost disconnect → the slot held by a ghost.** Owner disconnects, then a contender
  connects and disconnects, all within one refresh block. The flag coalesces and the
  side-band identity is the *last* writer's. The loop sees a disconnect that does not
  match the owner, treats it as inert (7b row 5), and never releases the owner. Every
  new client is refused until the idle timeout reclaims the slot — a device-wide
  outage of one full timeout.

**The mechanism is a fixed per-handle instance table, not an event queue.** Sized by
the connection cap: 3 on every ESP32 target here (`CONFIG_BT_NIMBLE_MAX_CONNECTIONS`
is 3 in the precompiled `sdkconfig.h` for S3/C3/C6, and absent for classic ESP32 so
NimBLE's own `#ifndef` default of 3 applies), 1 on nRF. Each entry holds
`(handle, epoch, state, rxBoundary, reason)` — metadata only, ~8 bytes, never frames
(see the one-command-queue constraint above).

Callbacks write their handle's entry. **The loop does not consume a stream of edges; it
scans the table and compares it against its own notion of the owner.** That inverts the
problem and dissolves the overflow question entirely:

- **It cannot overflow.** State is bounded by the connection cap, not by event rate.
  Contender churn overwrites entries for handles that are already gone. There is no
  eviction policy to specify, because nothing is ever queued.
- **Lost edges stop mattering.** A contender that connects and disconnects wholly
  within a refresh block leaves no entry — correct, since there is nothing left to
  refuse.
- **Owner release is a comparison, not an event.** If the owner's `(handle, epoch)` is
  no longer live in the table, the owner is gone, however many edges were missed. This
  makes 7b rows 6 and 9 (stale epoch, duplicate disconnect) inert for free rather than
  by explicit rule.
- **Ghosts stay visible.** Any live entry that is not the owner is a contender still
  needing refusal, and it remains visible until refused — so a missed refusal
  self-corrects on the next pass instead of leaking a connection slot.

*Search the table by handle rather than indexing by it.* NimBLE allocates handles from
0 upward in practice, so direct indexing usually works, but a 3-entry linear search
costs the same at this size and cannot be broken by a stack change that hands out
sparse handles.

nRF needs only requirement 4 in practice — `Bluefruit.begin(1, 0)` configures the
SoftDevice for a single peripheral link ([ble_transport_nrf.cpp:164](../src/ble_transport_nrf.cpp)),
so cross-central injection is unreachable at the link layer. Its write callback also
discards the handle it is given ([ble_transport_nrf.cpp:148](../src/ble_transport_nrf.cpp)),
which is latent rather than live.

### R3a — Owner lifecycle: a requested drop is not a completed drop

**A firmware-initiated disconnect moves the owner to `DROPPING`, not to `NONE`. The
slot is released only when the matching disconnect event arrives.**

> This corrects a first-draft error: the idle-timeout row released the owner in the
> same step as requesting the disconnect.

A BLE disconnect is asynchronous. `NimBLEServer::disconnect()` returning true means
termination was *requested* — it returns true even for `BLE_HS_ENOTCONN`/`EALREADY`
(`NimBLEServer.cpp:321-332`). Releasing the token at request time would let a new
connection be admitted while the old link is still physically up, so the old client's
writes and its eventual disconnect event would land against the *new* session,
violating both R1 and R3.

While `DROPPING`: refuse all contenders, filter the departing owner's writes as
non-owner, and do not admit. On the matching disconnect event, run R6's abort and go to
`NONE`. **A bounded escape is required**: if no disconnect event arrives within a
stated deadline, force the transition to `NONE` (and, on ESP32, retry the disconnect
once) rather than wedging the slot permanently — otherwise a failed drop is
indistinguishable from a permanently held slot.

### R4 — Each transport enforces an idle timeout, ungated by transfer state

**Idle** is defined as, and only as:

```
idle  :=  no inbound command from the owner on the owning transport
          AND no refresh in progress
```

**The idle timeout is NOT gated on a transfer being in progress.** This is deliberate
and is the rule's whole point: a client that goes silent *during an image upload* is
exactly the case that wedges the device today, and a `!transferActive()` gate would
exempt it. An in-flight transfer confers no protection; only inbound traffic does.

Consequences, stated because they are the cost of the rule:

- A silent client mid-upload **is dropped** and its partial transfer discarded by R6's
  abort. Partial upload state is never preserved across a drop.
- Any client whose legitimate inter-command gap can exceed the timeout will be dropped
  mid-transfer.

**Default: `OD_BLE_IDLE_TIMEOUT_MS = 120000` (120 s).** Set deliberately generous
because this rule made the direction of that error worse: while the drop was gated on
`!transferActive()` a short timeout only killed idle sessions, but with the gate gone
a short timeout kills legitimate *uploads*. The cost of being generous is bounded and
lands on one case only — a returning client waits up to 120 s if a stale-but-*alive*
incumbent holds the slot. A client that is genuinely gone is reaped by the link layer
in ~4–6 s (the firmware sets no supervision timeout, so the central's negotiated value
applies), so the lockout never applies to a crashed or out-of-range peer.

**What counts as activity.** A frame must reach the dispatcher and be **recognised as
a command from the current owner**.

> The first draft said "successfully queued or parsed," and pointed at
> `bleRxQueuePush()`'s success path. That is wrong and self-contradictory: the queue
> accepts any non-empty payload within the size cap
> ([command_queue.cpp:50-93](../src/command_queue.cpp)) — including a one-byte
> malformed frame or an unknown opcode, which the dispatcher only rejects later
> ([communication.cpp:541](../src/communication.cpp)). Stamping on queue success
> leaves a garbage flooder able to hold the slot indefinitely, which is precisely the
> failure the rule exists to prevent.

Two clocks in the tree are unusable as-is and must be fixed rather than reused:

- `pollActivity` stamps `lastActivityMs` whenever `connCount > 0`
  ([main.cpp:366](../src/main.cpp)) — a live-but-silent link never ages.
- LAN stamps `lastLanActivityMs` on `got > 0`, i.e. any bytes read
  ([wifi_service.cpp:946](../src/wifi_service.cpp)) — a flooder holds the slot with
  garbage.

**The clock must not run during a refresh.** `epdRefreshInProgress` brackets a
*blocking* call on the loop task ([display_service.cpp:2446-2467](../src/display_service.cpp),
[:3358-3368](../src/display_service.cpp)): `loop()` does not execute for the refresh's
duration, but wall-clock time passes. A naive `millis() - lastRx` accrues the whole
refresh and can drop an actively engaged client the moment `loop()` resumes.

Implementation requirements for the exclusion:

- **A loop-side edge detector cannot see the edge** — both transitions happen inside
  the blocking handler. The re-stamp must be invoked *at* the transition, via a single
  `endRefresh()` helper that both refresh sites call, not by polling the flag.
- **Re-stamp the current owner's clock only**, and only if the same instance identity
  still owns the slot. ("Both transports" is harmless under a perfect R1 but hides the
  identity requirement, and R1 is exactly what is being built.)
- Re-stamping can only ever *delay* a drop, never cause a spurious one.

**Ordering constraint.** On LAN, inbound bytes may be sitting in the socket when the
deadline is evaluated. The timeout check must run **after** the transport has had its
chance to parse this pass, or an active LAN client is dropped with its command already
in the buffer. BLE avoids this by stamping from callback context on arrival; LAN must
parse first. See R7d.

**Baseline.** The idle window is measured from the later of admission and last inbound
command, so a freshly admitted client gets a full window before its first command. On
LAN, admission is TCP accept, but the baseline starts at **TLS handshake completion**
(see R7a note), since handshake traffic is not a command.

**Per transport.** Each transport enforces its own timer and constant. LAN already has
one (`OD_LAN_READ_TIMEOUT_S` = 30 s, [wifi_service.cpp:952](../src/wifi_service.cpp)),
already ungated by transfer state, so LAN needs only the recognised-command stamping
and the refresh exclusion. BLE has no idle drop at all and needs the whole mechanism.

### R5 — A stuck refresh is a separate problem with a separate watchdog

R4 excludes refresh from idleness, so a refresh that never completes is **not** caught
by the idle timeout. That exposure is handled by a **refresh watchdog**, deliberately
*not* part of this policy but named here so it is tracked rather than assumed away.

Scoping it honestly, from the code:

- On the **`bb_epaper` polling path only**, the BUSY wait is bounded:
  `waitforrefresh(60)` loops `timeout * 100` times at 10 ms and then returns failure
  ([display_service.cpp:803-831](../src/display_service.cpp)).
- **On the FastEPD path there is no bound at all.** `waitforrefresh()` delegates
  immediately to `fastepd_wait_refresh()` (`:805`), which ignores its timeout argument
  outright — `(void)timeout_sec; return !s_init_failed;`
  ([display_fastepd.cpp:277-280](../src/display_fastepd.cpp)). The real blocking lives
  inside `fullUpdate()`/`fastUpdate()`, above that call and unbounded.

  > The first draft claimed the BUSY wait was bounded at 60 s generally. It is not.
  > On FastEPD targets the naive "panel never signals done" case is fully exposed.

- The residual exposure elsewhere is the driver call itself — `bbepRefresh()`,
  `fastepd_direct_refresh()`, `fastepd_partial_refresh()` — plus any SPI-level stall
  inside it, none bounded by the poll loop above it.
- **No loop-serviced watchdog can observe any of this**, because `loop()` is blocked
  for the refresh's entire duration. The watchdog needs an independent timebase: a
  hardware watchdog fed from `loop()`, a timer ISR, or a separate task.
- **The supervisor must recover from a safe context.** A timer ISR can *observe* a
  stuck refresh but must not run panel/SPI teardown from interrupt context; the
  realistic recovery is an MCU reset.
- There is no refresh start timestamp in the tree; the watchdog must add one.

### R6 — Every non-refused disconnect calls `abortToKnownState()`

**Any disconnect of the current owner — on any transport, for any reason, whether
client-initiated, link-layer, or firmware-initiated — runs `abortToKnownState()`**,
leaving the device ready for a new connection.

`abortToKnownState()` must leave, at minimum: no active direct-write, partial, pipe or
chunked-config transfer; touch resumed; encryption session cleared; RX and TX rings
drained of the departed session's traffic; the owner token released.

**Buzzer and LED are explicitly NOT stopped.** They are user-facing *effects*, not
session state. Firing a buzz and immediately dropping the link is a normal pattern —
command, then disconnect to save power — and truncating it defeats the command. A
playing melody cannot corrupt or confuse a later connection the way a half-open pipe
session, a suspended touch input or a live crypto session can, and both are bounded
and self-terminating: the buzzer's `outer` repeat count is a `uint8_t` coerced to at
least 1, with playback stopping at `rep >= outer`
([buzzer_control.cpp:215-217,288-291](../src/buzzer_control.cpp)); the LED runs a
stepped pattern to completion ([device_control.cpp:530-541](../src/device_control.cpp)).
Since this policy fires the abort far more often than a plain disconnect once did,
stopping them would be a correspondingly more visible regression. A WARM (post-refresh keep-alive) panel **survives** — the abort tears down
only a mid-transfer `PWR_ACTIVE` session, preserving the existing ACTIVE-only-teardown
invariant ([main.cpp:411-415](../src/main.cpp)). It runs on the loop task, is
idempotent, and is deferred while `epdRefreshInProgress`
([main.cpp:389](../src/main.cpp)).

**Exceptions — R6 does not apply to:**

1. **A refused contender** (R3). It was never the owner; its disconnect is inert.
2. **Terminal transitions**, where "ready for a new connection" is meaningless because
   the MCU is about to reset, sleep, or lose power. These paths disconnect the link
   and then leave, so no loop pass will ever service the event:
   - nRF DFU: `Bluefruit.disconnect()`, `delay(100)`, `sd_softdevice_disable()`, jump
     to bootloader ([device_control.cpp:847-866](../src/device_control.cpp)).
   - ESP32 DFU/reboot: BLE teardown then immediate restart
     ([device_control.cpp:880](../src/device_control.cpp)).
   - Power-latch off ([device_control.cpp:942](../src/device_control.cpp)) — power can
     be physically removed before any teardown.

   For these, either accept the exception as stated, or call
   `abortToKnownState(dropLink=false)` **synchronously before** the teardown/jump/sleep.
   Choose per path; the exception is the default. What is *not* acceptable is the first
   draft's unqualified "every disconnect," which these paths simply falsify.

### R7 — Permutation tables

Normative. Any combination not listed is a specification gap, not implementer's
discretion.

#### 7a — Admission

"Refuse" means: disconnect/close the contender and change nothing else (R3).

| # | Owner state | Incoming | Action | Owner after | Abort? |
|---|---|---|---|---|---|
| 1 | `NONE` | BLE connect `(h, e)` | Admit; `claim(BLE, h, e)` | `ACTIVE BLE(h,e)` | no |
| 2 | `NONE` | LAN accept | Admit; `claim(LAN, 0, e)` at **TCP accept** | `ACTIVE LAN(0,e)` | no |
| 3 | `ACTIVE BLE(h1,e1)` | BLE connect `(h2, e2)` | **Refuse** `h2` | unchanged | no |
| 4 | `ACTIVE BLE(h1,e1)` | BLE connect `(h1, e2)` — handle reused after a stale link | **Refuse** — epoch differs, so this is a new instance despite the matching handle (R2) | unchanged | no |
| 5 | `ACTIVE BLE(h1,e1)` | LAN accept | **Refuse**: `incoming.stop()` | unchanged | no |
| 6 | `ACTIVE LAN(0,e1)` | BLE connect `(h,e)` | **Refuse** `h` | unchanged | no |
| 7 | `ACTIVE LAN(0,e1)` | LAN accept | **Refuse**: `incoming.stop()` | unchanged | no |
| 8 | `DROPPING` | any | **Refuse** (R3a) | unchanged | no |

Row 7 is a **behaviour change**: LAN accept is unconditional last-in-wins today
([wifi_service.cpp:869-877](../src/wifi_service.cpp)). It matters more than the BLE
rows because LAN-TLS bypasses app-layer auth by design, so today any host on the
network can displace an in-flight push by opening a socket, with no credentials.

**LAN claims at TCP accept, before the TLS handshake.** The handshake is driven
incrementally across later loop passes ([wifi_service.cpp:905](../src/wifi_service.cpp)),
so deferring the claim until it completes would leave the slot free for a BLE connect
or a second socket in the meantime. Consequently:

- A second accept *during* the handshake is refused (rows 5/7 apply).
- **TLS handshake failure is an owner disconnect** — it runs R6's abort and releases.
- Handshake traffic is **not** activity for R4; the idle baseline starts at handshake
  completion, and the handshake itself needs its own bounded deadline.

Rows 3–8 are what make R1 true on ESP32, where the link layer will not. nRF enforces
rows 3–4 at the link layer via `Bluefruit.begin(1, 0)`
([ble_transport_nrf.cpp:164](../src/ble_transport_nrf.cpp)); firmware must still
implement them so behaviour is identical across targets and so rows 5–6 work at all.

#### 7b — Disconnect

**Generic rule, which the rows below instantiate:** *for any owner state, a disconnect
whose full instance identity does not match the owner is inert.*

| # | Owner | Disconnect identity | Action | Owner after | Abort? |
|---|---|---|---|---|---|
| 1 | `ACTIVE BLE(h1,e1)` | matches | Release; `abortToKnownState(dropLink=false)` | `NONE` | **yes** |
| 2 | `ACTIVE LAN(0,e1)` | matches | Release; `abortToKnownState(dropLink=false)` | `NONE` | **yes** |
| 3 | `DROPPING X` | matches | Release; abort (completes R3a) | `NONE` | **yes** |
| 4 | `DROPPING X` | no match | Inert | unchanged | no |
| 5 | any `ACTIVE` | refused contender `(BLE, h2, ·)` | Inert (R3) | unchanged | no |
| 6 | `ACTIVE BLE(h1,e1)` | `(BLE, h1, e0)` — stale epoch | Inert — late event from a prior instance (R2) | unchanged | no |
| 7 | `ACTIVE LAN` | any BLE identity | Inert (cross-transport) | unchanged | no |
| 8 | `ACTIVE BLE` | any LAN identity | Inert (cross-transport) | unchanged | no |
| 9 | any | duplicate of an already-consumed identity | Inert (idempotent) | unchanged | no |
| 10 | `NONE` | any | No-op | `NONE` | no |
| 11 | `DROPPING X` | none arrives before the deadline | Force release; abort; log WARN (R3a) | `NONE` | **yes** |

`dropLink=false` throughout: the link is already gone. Row 6 is the ABA case the epoch
exists to catch, reachable in practice because a disconnect event can be serviced tens
of seconds late when `loop()` was blocked in a refresh, by which time the handle may
have been reissued.

#### 7c — Idle timeout

| # | Owner | Refresh in progress | Inbound silence | Action | Abort? |
|---|---|---|---|---|---|
| 1 | `ACTIVE` | no | `>` timeout | Request drop via the seam; owner → `DROPPING` (R3a) | not yet — on the disconnect event (7b row 3) |
| 2 | `ACTIVE` | no | `≤` timeout | Nothing | no |
| 3 | `ACTIVE` | **yes** | any | Nothing — not idle by definition (R4); `endRefresh()` re-stamps the owner's clock | no |
| 4 | `DROPPING` | any | any | Nothing — a drop is already in flight | no |
| 5 | `NONE` | any | n/a | Nothing — no timer runs without an owner | no |

Row 1 applies **whether or not a transfer is in flight** (R4).

#### 7d — Within-pass ordering

**Normative, because without it two conforming implementations pick different
winners.** The current loop order is `serviceBleEvents()` → BLE RX → deferred
disconnect cleanup → LAN accept/read ([main.cpp:624](../src/main.cpp)), and connect and
disconnect flags are consumed connect-first regardless of actual arrival order
([main.cpp:461-471](../src/main.cpp)) — which is exactly the ambiguity this section
removes.

Within one loop pass, evaluate in this order:

1. **Owner disconnects** (7b) — release and abort first, so a slot freed this pass is
   available to an admission decision in the same pass.
2. **Admissions** (7a), BLE before LAN.
3. **Inbound traffic**, which stamps the activity clock.
4. **Idle timeout** (7c) — last, so traffic parsed in step 3 counts. This is what
   satisfies R4's ordering constraint for LAN.

**The authoritative arbitration point is the earliest transport hook — the BLE connect
callback and the LAN accept — not the loop.** Fixed loop ordering cannot reconstruct
true cross-transport arrival order (a BLE connect during a refresh and a LAN socket
queued in the listen backlog are not comparable by the time `loop()` resumes), and it
must not be relied on for correctness. It resolves *ties within a pass* only; the claim
itself must be atomic at the callback. Where the two disagree, the callback wins.

#### 7e — Terminal transitions

Per R6 exception 2. "Sync abort" = call `abortToKnownState(dropLink=false)`
synchronously before the transition.

| # | Transition | Link handling | R6 abort |
|---|---|---|---|
| 1 | nRF DFU entry | `Bluefruit.disconnect()` + 100 ms, then SoftDevice disable | Exempt (or sync abort) — MCU jumps to bootloader |
| 2 | ESP32 DFU / reboot | BLE teardown, immediate restart | Exempt — MCU resets |
| 3 | Deep sleep (forced or idle) | `ble.end()`, stack down | **Sync abort — required, not exempt.** Deep sleep is not a reset; state persists across it |
| 4 | Power-latch off | Power removed | Exempt — nothing survives |

**Row 3 is resolved: deep sleep calls `abortToKnownState()`.** It is the one terminal
transition that is not a reset — touch-suspend, panel power and the owner token all
survive it — so waking with a half-torn-down session is a real state, not a
theoretical one. Forced deep sleep additionally bypasses the live-link guard
([main.cpp:789](../src/main.cpp)) and does not arbitrate a LAN owner at all, so
without the abort it can sleep straight through an owned slot.

*Interaction with the buzzer/LED carve-out (R6).* The abort deliberately leaves buzzer
and LED running, and deep sleep cuts the clocks they depend on — so at this one
transition the "let the effect finish" rationale cannot hold, because the effect
*cannot* finish. Note also that neither appears in the `workInFlight` gate
([main.cpp:694-699](../src/main.cpp)), so today a melody does not hold off the idle
path at all. Two consistent resolutions, and the choice is **open**: add buzzer/LED
activity to the work gate so sleep waits for them to finish (consistent with the
carve-out's reasoning), or silence them in the deep-sleep path specifically (not in
the abort). Either way it is a *deep-sleep* responsibility, never an abort one. This
is pre-existing behaviour, not a regression introduced here.

---

## What this changes

Relative to the tree:

1. No connection-level arbitration exists; it must be built (R1, R2).
2. ESP32 admits up to three centrals with no check, and a contender can corrupt the
   incumbent's subscribe state, inject into its RX ring, and **receive its
   notifications** (R3).
3. LAN accept evicts rather than refuses (R3, 7a row 7).
4. No BLE idle drop exists; LAN's exists but stamps on raw bytes and runs through
   refreshes (R4).
5. Disconnect events carry no identity, and connect/disconnect events coalesce (R3).
6. **No *BLE* disconnect path clears the encryption session**, and LAN's clearing is
   conditional and divergent — cleared at [wifi_service.cpp:798](../src/wifi_service.cpp)
   only when `wifiClient.connected()`, and again on replacement at `:874`. Several
   teardown paths are open-coded and drift-prone (R6).
7. No refresh start timestamp and no independent timebase exist for R5; the FastEPD
   refresh path has no timeout bound whatsoever.

Relative to `PLAN_FREEZE_HARDENING_2026-07-31.md`, this document **supersedes** its
Phase 3 on four points:

- The plan gates its BLE idle drop on `!transferActive()`. **R4 removes that gate.**
- The plan's owner token is a `(transport, handle)` pair. **R2 requires a per-instance
  epoch.**
- The plan requires handle-bearing *connect* events. **R3 additionally requires
  identity-bearing *disconnect* events, non-coalescing delivery, and callback-side
  subscribe/notify filtering.**
- The plan releases the token when a drop is requested. **R3a requires a `DROPPING`
  state and release only on the disconnect event.**

R5 (refresh watchdog) and R7e (terminal transitions) are new scope the plan does not
cover.

## Open questions

- **Confirm 120 s clears real client behaviour.** The BLE idle timeout is set
  (`OD_BLE_IDLE_TIMEOUT_MS = 120000`, see below), so this is a check rather than a
  choice: verify no legitimate `py-opendisplay` inter-command silence approaches 120 s.
- **The `DROPPING` deadline** (R3a row 11) — long enough to cover a normal supervision
  timeout, short enough that a failed drop does not wedge the slot.
- **Deep sleep vs the buzzer/LED carve-out** (7e row 3) — whether sleep waits for a
  playing effect via the work gate, or silences it in the deep-sleep path. Not an
  abort concern either way.
- **Whether LAN's 30 s satisfies R4 unchanged.** `OD_LAN_READ_TIMEOUT_S` lives in the
  wire header ([opendisplay_protocol.h:984](../include/opendisplay_protocol.h)) and is
  documented as a client-visible contract, so changing its *value* is a wire change and
  out of bounds here. Its *semantics* under R4 are firmware-local and in bounds.

**Resolved since the first draft — event delivery.** The first draft required
"non-coalescing event delivery" and left the queue's overflow behaviour as an open
question. There is no queue: R3 requirement 5 now specifies a fixed per-handle instance
table that the loop scans, so there is nothing to overflow and no eviction policy to
decide. See also the one-command-queue constraint at the top of this document.

**Resolved since the first draft:** `checkTransferTimeouts()` **does** route through
`abortToKnownState(dropLink=true)`. R6 governs disconnects and the watchdog is not one,
so this is an extension of R6's *teardown* to a non-disconnect trigger rather than a
consequence of it: there is one teardown routine and the watchdog uses it. The
rationale, the three behaviour changes it brings, and the one branch deliberately left
out (the orphaned-pipe invariant repair) are recorded in the freeze-hardening plan's
invocation set.
