// =============================================================================
// c2/c2_selftest.cpp — on-device end-to-end test of the WiFi C2 REST API
// =============================================================================
// See c2_selftest.h for WHY. Compiled in only under -DCFG_C2_SELFTEST=1.
// =============================================================================
#include "config.h"

#if CFG_C2_SELFTEST

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "c2_selftest.h"
#include "web_server.h"   // C2Server::authToken()

namespace {

// Base URL of the C2 server as reached over loopback. Chosen at runtime by
// pickBase() — normally the SoftAP's own IP, with 127.0.0.1 as a fallback in
// case this core's lwIP doesn't loop own-AP-IP traffic back.
String g_base;

int g_pass = 0;
int g_fail = 0;

// DuckyScript fixtures the test uploads to the SD card and runs. HELLO has no
// cooperative pause, so it runs to completion in one tick. DETECT holds the
// interpreter in RUNNING for CFG_OS_DETECT_WINDOW_MS *without* blocking the
// main loop (DETECT_OS yields every tick), which is exactly what the mid-run
// stop test and the "C2 responsive during detection" test both need.
const char* HELLO =
    "REM LilyDucky C2 loopback self-test payload\n"
    "DELAY 100\n"
    "STRING hello from loopback self-test\n"
    "ENTER\n";

const char* DETECT =
    "REM DETECT_OS self-test — cooperative 3s pause, C2 must stay responsive\n"
    "DETECT_OS\n"
    "STRINGLN os detection done\n";

void check(const char* name, bool ok)
{
    if (ok) { g_pass++; Serial.printf("  [PASS] %s\n", name); }
    else    { g_fail++; Serial.printf("  [FAIL] %s\n", name); }
}

// One HTTP request over loopback. Returns true if the transport succeeded (a
// real HTTP status came back); `code` holds the status (or a negative
// HTTPClient error), `out` the response body.
bool req(const char* method, const String& path, const String& body,
         bool withToken, int& code, String& out)
{
    WiFiClient client;
    HTTPClient http;
    if (!http.begin(client, g_base + path)) { code = -1000; out = ""; return false; }
    http.setConnectTimeout(4000);
    http.setTimeout(5000);
    if (withToken) http.addHeader("X-Auth-Token", C2Server::authToken());

    if (strcmp(method, "GET") == 0) {
        code = http.GET();
    } else if (strcmp(method, "PUT") == 0) {
        http.addHeader("Content-Type", "text/plain");
        code = http.PUT((uint8_t*)body.c_str(), body.length());
    } else { // POST
        code = http.POST(body);
    }
    out = (code > 0) ? http.getString() : String("");
    http.end();
    return code > 0;
}

// Pull the "state" field out of a /api/status JSON body.
String extractState(const String& body)
{
    int i = body.indexOf("\"state\":\"");
    if (i < 0) return "";
    i += 9;
    int j = body.indexOf('"', i);
    if (j < 0) return "";
    return body.substring(i, j);
}

bool terminal(const String& s)
{
    return s == "complete" || s == "error" || s == "stopped" || s == "idle";
}

// Poll /api/status until the interpreter reaches a terminal state (or timeout).
// Returns the last state seen.
String pollUntilTerminal(uint32_t timeoutMs)
{
    uint32_t start = millis();
    String st = "";
    while (millis() - start < timeoutMs) {
        delay(400);
        int c; String b;
        if (req("GET", "/api/status", "", false, c, b)) {
            st = extractState(b);
            if (terminal(st)) break;
        }
    }
    return st;
}

// Find a reachable base URL for the C2 server. Prefer the SoftAP IP; fall back
// to 127.0.0.1 if own-AP-IP loopback isn't routed on this build.
bool pickBase()
{
    String candidates[2] = { "http://" + WiFi.softAPIP().toString(),
                             "http://127.0.0.1" };
    for (int k = 0; k < 2; k++) {
        g_base = candidates[k];
        int c; String b;
        if (req("GET", "/api/status", "", false, c, b) && c == 200) {
            Serial.printf("[SELFTEST] reaching C2 via %s\n", g_base.c_str());
            return true;
        }
        Serial.printf("[SELFTEST] %s not reachable (code %d), trying next\n",
                      g_base.c_str(), c);
    }
    return false;
}

void runChecks()
{
    int c; String b;

    // ── 1. status endpoint ──────────────────────────────────────────────
    Serial.println("[1] GET /api/status");
    req("GET", "/api/status", "", false, c, b);
    Serial.printf("    %d %s\n", c, b.c_str());
    check("status returns JSON with state + ap_ip 192.168.4.1",
          c == 200 && b.indexOf("\"state\"") >= 0 &&
          b.indexOf("\"ap_ip\":\"192.168.4.1\"") >= 0);

    // ── 2. upload payload (auth via header) ─────────────────────────────
    Serial.println("[2] PUT /api/payload/st_hello.dd");
    req("PUT", "/api/payload/st_hello.dd", HELLO, true, c, b);
    check("authenticated PUT returns 200 OK", c == 200 && b == "OK");

    // ── 3. round-trip read-back ─────────────────────────────────────────
    Serial.println("[3] GET /api/payload/st_hello.dd round-trip");
    req("GET", "/api/payload/st_hello.dd", "", false, c, b);
    check("downloaded payload identical to source", c == 200 && b == String(HELLO));

    // ── 4. payload listing ──────────────────────────────────────────────
    Serial.println("[4] GET /api/payloads");
    req("GET", "/api/payloads", "", false, c, b);
    Serial.printf("    %s\n", b.c_str());
    check("payload list contains st_hello.dd", c == 200 && b.indexOf("st_hello.dd") >= 0);

    // ── 5. negative auth: PUT without token must be 401 ─────────────────
    Serial.println("[5] PUT without token");
    req("PUT", "/api/payload/st_evil.dd", "EVIL", false, c, b);
    Serial.printf("    HTTP %d\n", c);
    check("unauthenticated PUT rejected with 401", c == 401);

    // ── 6. run to completion ────────────────────────────────────────────
    Serial.println("[6] POST /api/run/st_hello.dd -> poll until complete");
    req("POST", "/api/run/st_hello.dd", "", true, c, b);
    check("run returns 200 Running", c == 200 && b == "Running");
    String st = pollUntilTerminal(10000);
    Serial.printf("    final state: %s\n", st.c_str());
    check("st_hello.dd ran to completion (typing is a no-op on C5)", st == "complete");

    // ── 7. mid-run stop (DETECT_OS holds RUNNING cooperatively ~3s) ─────
    Serial.println("[7] PUT st_detect.dd, run, stop within ~300 ms");
    req("PUT", "/api/payload/st_detect.dd", DETECT, true, c, b);
    check("PUT st_detect.dd returns 200 OK", c == 200 && b == "OK");
    req("POST", "/api/run/st_detect.dd", "", true, c, b);
    bool started = (c == 200 && b == "Running");
    delay(300);
    req("POST", "/api/stop", "", true, c, b);
    check("stop returns 200 Stopped", started && c == 200 && b == "Stopped");
    delay(400);
    req("GET", "/api/status", "", false, c, b);
    st = extractState(b);
    Serial.printf("    state after stop: %s\n", st.c_str());
    check("status reports stopped", st == "stopped");

    // ── 8. DETECT_OS: C2 responsive during the detection window ─────────
    Serial.println("[8] run st_detect.dd; C2 must stay responsive during window");
    req("POST", "/api/run/st_detect.dd", "", true, c, b);
    int slow = 0;
    for (int i = 1; i <= 4; i++) {
        uint32_t t0 = millis();
        bool ok = req("GET", "/api/status", "", false, c, b);
        uint32_t dt = millis() - t0;
        Serial.printf("    poll %d: /api/status answered in %lu ms\n", i, (unsigned long)dt);
        if (!ok || dt >= 1000) slow++;
        delay(600);
    }
    check("C2 responsive during detection window (all answers < 1 s)", slow == 0);
    st = pollUntilTerminal(6000);
    Serial.printf("    final state: %s\n", st.c_str());
    check("st_detect.dd ran to completion (serial should show $_OS=0)", st == "complete");
}

void task(void*)
{
    delay(1500);  // let the SoftAP + HTTP server settle after boot
    Serial.println("\n[SELFTEST] ==== C2 loopback self-test start ====");
    Serial.println("[SELFTEST] loopback covers HTTP+handler+interpreter+SD; NOT the OTA radio path.");
    Serial.println("[SELFTEST] a true over-the-air pass still needs scripts/c2_api_test.sh from a WiFi client.");

    g_pass = 0;
    g_fail = 0;

    if (!pickBase()) {
        Serial.println("[SELFTEST] [FATAL] C2 server unreachable via loopback — cannot run");
        Serial.println("[SELFTEST] ==== C2 loopback self-test end: UNABLE TO RUN ====");
        vTaskDelete(NULL);
        return;
    }

    runChecks();

    Serial.printf("[SELFTEST] ==== RESULT: %d passed, %d failed ====\n", g_pass, g_fail);
    Serial.println(g_fail == 0 ? "[SELFTEST] ALL PASS" : "[SELFTEST] FAILURES PRESENT");
    Serial.println("[SELFTEST] ==== C2 loopback self-test end ====");
    vTaskDelete(NULL);
}

} // namespace

void C2SelfTest::begin()
{
    // 8 KB stack: HTTPClient + WiFiClient + String churn. Priority 1 matches
    // the Arduino loopTask so the two round-robin; the test's blocking socket
    // waits yield to loopTask, which is what actually services the requests.
    xTaskCreate(task, "c2selftest", 8192, NULL, 1, NULL);
}

#endif // CFG_C2_SELFTEST
