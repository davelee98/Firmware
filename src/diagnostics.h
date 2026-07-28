#pragma once

#include <stdint.h>

// Runtime health telemetry: heap occupancy at the points where a leak or a
// fragmentation cliff would show up, and a connected-state heartbeat.
//
// Both exist to answer one question the logs could not previously answer: when a
// tag goes silent, did the firmware stop or did it simply have nothing to say?
// While a client is connected loop() never reaches platformIdle(), so the nRF
// MSD cadence never fires and a healthy idle link and a hung loop() produce
// byte-identical output -- nothing at all. The heartbeat removes that ambiguity;
// the heap lines say whether the run was drifting toward exhaustion first.

// Refresh the low-water mark. Cheap (one allocator query); call freely.
// diagLogHeap() calls it, so the sample is current at every logged point.
void diagHeapSample(void);

// One INFO line: "[mem] <tag> total=.. free=.. minfree=.. largest=..".
// INFO rather than DEBUG deliberately -- OD_LOG_LEVEL defaults to INFO, so these
// reach the release envs the field units run, not just the -debug ones.
void diagLogHeap(const char *tag);

// One DEBUG line every 5 s while a client is connected; no-op otherwise.
// `connected` is passed in rather than queried so this stays transport-agnostic
// (BLE central or LAN session, whichever main.cpp counts as connected).
// Call once per loop() pass.
void diagHeartbeat(bool connected);
