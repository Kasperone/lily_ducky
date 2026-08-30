// =============================================================================
// c2/c2_selftest.h — on-device end-to-end test of the WiFi C2 REST API
// =============================================================================
// Why this exists: the C2 REST API can only be exercised over-the-air from a
// machine JOINED to the SoftAP (see scripts/c2_api_test.sh). The build/flash
// host is a headless VM with NO WiFi radio (only wired ethernet), so it can
// never join `LilyC2` — leaving the API's happy path unverified on hardware.
//
// This module closes that gap WITHOUT an external client: it runs an HTTP
// client, on the dongle itself, against the C2 server's own SoftAP IP
// (loopback through lwIP), and drives the same 8 checks scripts/c2_api_test.sh
// does — status, payload PUT/GET round-trip, listing, 401 on missing token,
// run-to-completion, mid-run stop, and DETECT_OS with a C2-responsiveness
// probe during the detection window. Every result is printed to the USB
// serial console as [PASS]/[FAIL], so a headless capture (probe_reset.py /
// serial_monitor.py) is the whole verification.
//
// SCOPE / HONESTY: loopback exercises the full TCP -> WebServer -> handler ->
// interpreter -> SD path, but it does NOT exercise the real radio association
// / DHCP path. A true over-the-air pass still needs scripts/c2_api_test.sh
// from a WiFi client. This is the strongest check the headless build host can
// run unaided; it is a complement to, not a replacement for, the OTA test.
//
// Compiled in only under -DCFG_C2_SELFTEST=1 (env: T-Dongle-C5-selftest); the
// default firmware build has CFG_C2_SELFTEST=0 and this file is empty, so the
// production boot path is unchanged.
// =============================================================================
#ifndef LILY_DUCKY_C2_SELFTEST_H
#define LILY_DUCKY_C2_SELFTEST_H

namespace C2SelfTest {

    // Spawn the self-test as its own FreeRTOS task and return immediately.
    // MUST be called after C2Server::start() + setInterpreter(): the test
    // task makes HTTP requests that only the main loop's C2Server::tick()
    // can service, so the caller's loop() must keep running concurrently.
    void begin();

} // namespace C2SelfTest

#endif // LILY_DUCKY_C2_SELFTEST_H
