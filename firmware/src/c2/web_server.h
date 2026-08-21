// =============================================================================
// c2/web_server.h — WiFi C2: SoftAP + HTTP dashboard + REST API
// =============================================================================
#ifndef FUNNY_USB_WEB_SERVER_H
#define FUNNY_USB_WEB_SERVER_H

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "config.h"

namespace WebServer {

    // Start the server (creates SoftAP, serves HTTP)
    bool start();
    void stop();
    void tick();  // call every loop
    bool running();

    // Get server status info for the main LCD/LED
    int connectedClients();
    IPAddress apIP();

    // C2 auth token (random per boot, printed to serial). Required on the
    // X-Auth-Token header for mutating routes. Returns an empty string
    // before start() has run.
    const char* authToken();

    // Set the interpreter pointer for API endpoints
    void setInterpreter(class Interpreter* interp);

} // namespace WebServer

#endif
