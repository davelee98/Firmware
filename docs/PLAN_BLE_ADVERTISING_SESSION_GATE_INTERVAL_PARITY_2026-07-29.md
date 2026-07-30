# Plan — shared BLE advertising session gate

**Date:** 2026-07-29  
**Supersedes for implementation:** `PLAN_NRF_NO_ADV_WHILE_CONNECTED_2026-07-29.md`  
**Scope:** BLE advertising session gating and MSD publication correctness on
nRF52840 and ESP32. Advertising interval changes are explicitly deferred.

No wire-protocol, config-schema, client, service UUID, or GATT changes.

## Decision

BLE advertising must be stopped whenever either condition is true:

1. a BLE client is connected; or
2. a LAN client is connected.

Advertising may resume only when both conditions are false and the existing
EPD-refresh safety gate permits it.

This is an advertising policy, not transport exclusivity. If BLE connects first,
a LAN client can still connect afterward because LAN acceptance does not depend
on BLE advertising. Both physical links may therefore overlap. Existing
transport-origin checks continue to protect in-flight transfers; a general
BLE/LAN command-owner lease is separate work.

## Deliberately deferred denial-of-service risk

Stopping advertising at TCP accept allows a LAN peer to suppress BLE discovery
before completing TLS or sending a valid command. A peer can reconnect at the
30-second idle boundary; in plaintext mode it can keep an incomplete frame alive
by periodically sending bytes. A legitimate persistent LAN client has the same
observable effect.

This plan deliberately accepts that behavior to implement the selected policy.
Do not expand this change into LAN authentication, ownership leases, connection
rate limiting, or non-connectable advertising.

Permanent BLE/LAN documentation and release notes must record:

- TCP acceptance currently defines “LAN connected”;
- any accepted LAN socket suppresses BLE advertising;
- this can suppress BLE discovery indefinitely;
- advertising suppression does not prevent BLE-first/LAN-second link overlap;
- redefining LAN ownership and mitigating denial of discovery are deferred.

Follow-up questions:

- Should “LAN connected” later mean TCP accepted, TLS complete, first valid
  authenticated command, or active transfer?
- Is reconnect throttling required?
- Should a persistent LAN session use non-connectable BLE advertising for
  discovery?
- Does the product need an explicit single-command-transport lease?

## Why implement the gate

### nRF52840

The SoftDevice already stops advertising when the sole peripheral connection
slot is occupied, but `setManufacturerData()` still rebuilds and attempts to
restart advertising while connected. That start cannot succeed and can leave
the advertising interval/boost state altered. A gate removes guaranteed-failing
stack work and the associated post-disconnect interval hazard.

### ESP32

NimBLE already stops advertising on BLE connection, and the current
manufacturer-data path avoids restarting it while a BLE client is connected.
Nothing stops advertising when a LAN client connects, and the restart service
does not defer on a live LAN connection. Explicit LAN lifecycle hooks are
required to enforce the chosen policy.

### Shared correctness

`updatemsdata()` currently advances its “published” snapshot and emits
`MSD publish:` before knowing whether the platform accepted the update. A gated
or failed operation can therefore be logged and remembered as successful. The
latest value must remain pending until the BLE stack actually accepts it.

## Implementation boundary

Implement:

1. one shared `bleAdvertisingBlocked()` predicate;
2. BLE- and LAN-aware publication and restart gates;
3. immediate stop on LAN accept;
4. deferred restart on every live-session LAN teardown;
5. idempotent platform stop operations;
6. real success/failure from advertisement publication;
7. retry of the newest deferred MSD;
8. truthful publication logging;
9. one fresh ESP32 restart after session release.

Do not implement:

- advertising interval parity;
- a shared fast/slow/boost phase engine;
- changes to the current nRF interval or boost behavior except preventing the
  connected-time latch;
- changes to ESP32’s current NimBLE default interval;
- transport exclusivity or a command-owner lease;
- LAN authentication/rate-limit changes;
- non-connectable advertising;
- removal of ESP32’s 50 ms/100 ms advertising delays without hardware evidence.

## Shared policy, thin stack adapters

Add a small platform-neutral module:

```text
src/ble_advertising_policy.h
src/ble_advertising_policy.cpp
```

It owns only:

- the blocker predicate;
- pending stop/restart intent;
- the latest complete 16-byte MSD value and dirty flag;
- retry after a gate or stack failure.

It does **not** own advertising intervals, phase deadlines, or platform stack
objects.

NimBLE and Bluefruit implementations retain:

- stack initialization;
- stack-native connect/disconnect behavior;
- interval and boost behavior;
- advertisement-data installation;
- start/stop operations;
- the nRF auto-restart capability difference.

### One LAN predicate on every target

Make `wifiLanClientConnected()` available on all builds:

```cpp
#ifdef OPENDISPLAY_HAS_WIFI
bool wifiLanClientConnected(void);
#else
static inline bool wifiLanClientConnected(void) { return false; }
#endif
```

The shared blocker is:

```cpp
bool bleAdvertisingBlocked() {
    return ble.isConnected() || wifiLanClientConnected();
}
```

Advertising code must not duplicate
`wifiInitialized && wifiServerConnected && wifiClient.connected()`. The LAN
service owns the definition of a live LAN session.

### Lifecycle API

Expose application-level operations similar to:

```cpp
bool bleAdvertisingBlocked();
void bleAdvertisingClientStateChanged();
void bleAdvertisingRequestRestart();
void bleAdvertisingPublishMsd(const uint8_t* data, uint8_t len);
void bleAdvertisingPolicyService();
```

Exact names may change, but the ownership rules may not:

- BLE and LAN lifecycle edges report state changes.
- The shared module decides blocked/deferred/eligible state.
- Platform files execute stack operations only.
- `loop()` services pending work.
- BLE stack callbacks remain copy-and-flag only.

## Session edges

### BLE

When `serviceBleEvents()` consumes a connect event:

- notify the shared advertising policy;
- do not call stack advertising APIs from the callback;
- rely on the BLE stack’s automatic stop at connection establishment.

When it consumes a disconnect event:

- notify the policy;
- request an MSD retry/restart;
- preserve the existing nRF-vs-ESP32 auto-restart distinction.

### LAN

After `handleWiFiServer()` accepts a client and sets
`wifiServerConnected = true`:

- notify the shared policy;
- stop BLE advertising immediately on the loop task.

Client replacement keeps a LAN session continuously active. It must not create a
stop/restart window between the old and new clients.

In `disconnectWiFiServer()`:

- clear LAN connection state first;
- notify the policy;
- request advertising restart;
- leave the request pending while BLE is connected or an EPD refresh is active.

Normal close, TLS failure, invalid frame, Wi-Fi loss, idle timeout, and
configuration-driven server restart must continue to funnel through this path.
`opendisplay_lan_teardown()` is a reboot/power-down path and does not need to
restart advertising because the process does not continue afterward.

## Idempotent stop and safe restart

The shared policy may observe advertising already stopped by a BLE connection.
Platform stops must therefore be idempotent:

- ESP32: retain NimBLE’s already-stopped-safe behavior.
- nRF: check `Bluefruit.Advertising.isRunning()` before calling `stop()`.

Restart requirements:

1. Recheck `bleAdvertisingBlocked()` immediately before starting.
2. Never start during `epdRefreshInProgress`.
3. Do not clear restart intent while either gate remains true.
4. A connect event wins over pending restart.
5. Respect nRF’s `restartOnDisconnect(true)` behavior; do not issue a competing
   application start.
6. ESP32 must start once with the newest cached MSD, not start stale data and
   immediately stop/rebuild/restart it.

## MSD publication contract

Change the platform advertisement-data operation to return real success:

```cpp
bool setManufacturerData(const uint8_t* msd, uint8_t len);
```

Return `false` when:

- advertising is blocked;
- the advertising object is unavailable;
- advertisement data cannot be installed;
- a required start/restart fails.

Return `true` only after the requested stack operation succeeds. Use the actual
Bluefruit/NimBLE boolean results rather than returning true unconditionally.

The shared wrapper always stores the newest complete MSD. On `false`, it keeps
that payload dirty for a later eligible service pass. Multiple blocked updates
replace one pending value; no queue is required.

In `updatemsdata()`:

- continue building `msd_payload` while connected;
- continue updating mDNS TXT during LAN sessions;
- advance the BLE `prev_msd_payload` snapshot only after success;
- emit `MSD publish:` only after success;
- optionally log one rate-limited `MSD deferred:` transition;
- advance `mloopcounter` regardless of BLE publication outcome.

Do not use an early return on BLE deferral that skips mDNS or the loop counter.

### One fresh ESP32 restart

The current restart sequence starts advertising and then calls
`updatemsdata()`, which can stop/rebuild/restart it again. Replace that sequence
with one operation that installs the policy’s newest cached MSD before starting.

The common policy records publication success so `prev_msd_payload` does not
immediately trigger a second restart. Keep this coordination outside the NimBLE
implementation; the adapter should only install data and start.

## Implementation sequence

### Commit 1 — shared blocker and session lifecycle

1. Add the always-available `wifiLanClientConnected()` declaration/stub.
2. Add the small common policy module and blocker predicate.
3. Route advertising restart eligibility through the shared predicate.
4. Notify the policy from loop-side BLE connect/disconnect handling.
5. Stop advertising on LAN accept.
6. Request restart from `disconnectWiFiServer()`.
7. Make platform stop operations idempotent.
8. Remove duplicate platform-local blocker decisions once all callers use the
   shared policy.

Result: advertising is stopped while either client is connected and resumes only
after both are gone.

### Commit 2 — publication correctness

1. Return actual stack success from manufacturer-data operations.
2. Store the newest pending MSD in the common policy.
3. Advance the published snapshot and log only on success.
4. Preserve mDNS and `mloopcounter` behavior while BLE publication is deferred.
5. Collapse ESP32 session-release publication into one fresh start.
6. Retry after blocked and failed operations.

Result: no false success log, no lost gated update, and no stale-then-fresh
double restart.

### Commit 3 — documentation and measurements

1. Document the shipped gate and deferred denial-of-discovery risk.
2. Record current interval behavior without changing it.
3. Measure the interval-parity hypothesis described below.

Result: policy and risks are permanent, while interval changes remain
evidence-gated.

## Advertising interval investigation — measurement only

Current behavior:

| Target | Fast behavior | Steady behavior | Button boost |
|---|---|---|---|
| nRF52840 | 160 ms, effectively about 1 s | 1000 ms | nominal 20–30 ms for 3 s |
| ESP32 | NimBLE default 30–60 ms | 30–60 ms indefinitely | no-op |

The APIs are not semantically interchangeable:

- Bluefruit `setInterval(fast, slow)` selects sequential fast and slow values.
- NimBLE `setMinInterval(min)` / `setMaxInterval(max)` defines one phase’s range.

Do not copy `256, 1600` directly into NimBLE and call that parity.

### Candidate follow-up policy

If measurements justify parity, evaluate:

| Phase | Candidate interval | Candidate duration |
|---|---:|---:|
| Fast discovery | 160 ms | 10 s |
| Steady discovery | 1000 ms | indefinite |
| Interaction boost | 20 ms | 3 s |

This would preserve nRF’s steady-state power posture and remove ESP32’s
continuous high-rate advertising. Ten seconds reflects the explicit but
currently ineffective nRF `setFastTimeout(10)` intent.

Do not implement this candidate in the gate change. First measure:

- idle current on nRF52840, ESP32-S3, and ESP32-C6;
- discovery and reconnect latency in fast and steady states;
- at least 100 reconnect attempts per target/phase;
- ESP32 Wi-Fi throughput during advertising;
- client presence behavior at a 1000 ms interval.

If the benefit is material and client behavior remains acceptable, write a
separate interval-parity implementation plan. That plan may then justify a
shared phase engine. Until then, retain each stack’s current interval lifecycle.

## Tests

### Gate behavior

1. nRF: connect BLE; button/touch/MSD updates make no advertising restart
   attempt. Disconnect; advertising resumes with the latest MSD.
2. ESP32: connect BLE; advertising remains stopped through MSD updates.
3. ESP32: connect LAN without BLE; advertising stops immediately after accept
   and stays stopped through handshake, commands, touch, and buttons.
4. ESP32: connect BLE first, then LAN; both links may exist, but advertising
   stays stopped.
5. Disconnect either of two live transports; advertising remains stopped until
   the other disconnects.
6. BLE disconnect during LAN leaves restart pending until LAN closes.
7. LAN disconnect during BLE does not restart; the later BLE disconnect does.
8. TLS failure, invalid frame, Wi-Fi loss, idle timeout, normal close, client
   replacement, and LAN server restart leave the correct advertising state.
9. EPD refresh ending after both clients disconnect releases a deferred restart.

### MSD correctness

10. Change MSD repeatedly while blocked; the first eligible advertisement
    contains only the newest value.
11. Force advertisement installation/start failure; the published snapshot does
    not advance, no success log appears, and a later service pass retries.
12. mDNS TXT continues updating during LAN while BLE publication is deferred.
13. ESP32 session release produces one fresh advertising start.
14. nRF post-disconnect advertising retains its existing interval lifecycle and
    no connected-time boost latch remains.

### Build

15. Build all eleven PlatformIO environments.

## Acceptance criteria

- No connectable BLE advertising is observable while either BLE or LAN is
  connected.
- One shared predicate determines that state on every target.
- Stack callbacks remain flag-only.
- Restart intent survives BLE, LAN, and EPD-refresh gates.
- A blocked or failed MSD remains pending and is never logged as published.
- mDNS continues updating during LAN sessions.
- ESP32 session release uses one fresh start.
- Current advertising intervals remain unchanged.
- The denial-of-discovery risk is documented and explicitly deferred.
- All eleven environments build.

## Rollback boundaries

- Commit 1 is the requested advertising gate.
- Commit 2 is independent publication correctness and should remain even if the
  gate policy later changes.
- Commit 3 records behavior, risk, and measurements; retain the risk note as long
  as TCP accept suppresses BLE discovery.
- Any future interval parity change must be a separate measured change with its
  own rollback.
