// =============================================================================
// c2/web_server.h — WiFi C2: SoftAP + HTTP dashboard + REST API
// =============================================================================
#ifndef LILY_DUCKY_WEB_SERVER_H
#define LILY_DUCKY_WEB_SERVER_H

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "config.h"

// Named C2Server (not WebServer): the Arduino core's <WebServer.h> already
// declares class WebServer — a namespace of the same name is a hard compile
// error ("redeclared as different kind of entity").

// Forward-declare at global scope: inside the namespace, `class Interpreter`
// would declare a NEW incomplete type C2Server::Interpreter.
class Interpreter;

namespace C2Server {

    // Start the server (creates SoftAP, serves HTTP)
    bool start();
    void stop();
    void tick();  // call every loop
    bool running();

    // Get server status info for the main LCD/LED
    int connectedClients();
    IPAddress apIP();

    // Re-establish the SoftAP radio (softAP + band/channel) without touching
    // routes, the auth token, or running state. Reserved for the deferred
    // Recon ?ap=down variant; deliberately NOT called from the scan-complete
    // path — a full re-init there hung the main loop (see recon.cpp harvestScan,
    // 2026-09-06), which is why 2a uses a light STA-drop recovery instead.
    void restartSoftAp();

    // C2 auth token (random per boot, printed to serial). Required on the
    // X-Auth-Token header for mutating routes. Returns an empty string
    // before start() has run.
    const char* authToken();

    // Set the interpreter pointer for API endpoints
    void setInterpreter(Interpreter* interp);

    // open-questions.md #9 instrumentation: the WiFi driver's per-frame
    // TX-done callback records, per interface, how many frames the MAC
    // reports it transmitted and whether each was ACKed by the peer — the
    // layer *below* lwIP that LWIPSTATS can't see. printTxStats() dumps the
    // counters to serial (TXSTATS console command); resetTxStats() zeroes
    // them (TXRESET) so a single request window can be measured in isolation.
    void printTxStats();
    void resetTxStats();

} // namespace C2Server

#endif
