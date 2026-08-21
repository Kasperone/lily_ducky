// =============================================================================
// c2/web_server.cpp — WiFi C2: SoftAP + HTTP dashboard + REST API
// =============================================================================
// Endpoints:
//   GET  /                   → Dashboard (SPA)
//   GET  /api/status         → JSON: {state, clients, ap_ip}
//   GET  /api/payloads       → JSON: [{name, size}, ...]
//   GET  /api/payload/<name> → Serve payload text
//   PUT  /api/payload/<name> → Save payload body                 [auth]
//   POST /api/run/<name>     → Trigger payload execution         [auth]
//   POST /api/stop           → Stop running payload              [auth]
// =============================================================================

#include "web_server.h"
#include <esp_system.h>  // esp_random() for token seed
#include "../storage/storage.h"
#include "../interpreter/interpreter.h"
#include "../hal/hal.h"
#include "config.h"

static WebServer _server(CFG_HTTP_PORT);
static bool _running = false;
static Interpreter* _interp = NULL;  // set from main on start

// ── Auth token ──────────────────────────────────────────────────────────────
// Generated at start(), printed to USB serial. Required for mutating routes
// (/api/run/*, /api/payload/* PUT, /api/stop). The dashboard prompts for it
// once and stores it in localStorage.
static char _authToken[17] = {0};

static void generateToken()
{
    // Seed Arduino's PRNG from the ESP32 HW RNG so tokens differ across boots
    // even if the auto-seed didn't run (or got the same entropy twice).
    randomSeed(esp_random());

    static const char alphabet[] =
        "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";  // no 0/O/1/I to ease retyping
    for (int i = 0; i < 16; i++) {
        _authToken[i] = alphabet[random(0, sizeof(alphabet) - 1)];
    }
    _authToken[16] = '\0';
}

static bool authOk()
{
    if (_server.hasHeader("X-Auth-Token") &&
        _server.header("X-Auth-Token") == _authToken) return true;
    if (_server.hasArg("token") &&
        _server.arg("token") == _authToken) return true;
    _server.send(401, "text/plain", "Unauthorized — supply X-Auth-Token (see serial console)");
    return false;
}

// Validate a payload filename: non-empty, no traversal, no path separators,
// no control chars or leading dot, under CFG_MAX_PAYLOAD_FN.
static bool validName(const String& name)
{
    int n = name.length();
    if (n == 0 || n >= CFG_MAX_PAYLOAD_FN) return false;
    if (name[0] == '.') return false;
    if (name.indexOf('/') >= 0 || name.indexOf('\\') >= 0 ||
        name.indexOf("..") >= 0) return false;
    for (int i = 0; i < n; i++) {
        char c = name[i];
        if ((unsigned char)c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

// ── HTML Dashboard (inline to avoid SPIFFS dependency) ─────────────────────
static const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>DuckC2</title>
<style>
 body{font-family:monospace;background:#1a1a2e;color:#e0e0e0;margin:0;padding:16px}
 h1{color:#0ff;border-bottom:1px solid #333;padding-bottom:8px}
 .card{background:#16213e;border:1px solid #0f3460;border-radius:6px;padding:16px;margin:12px 0}
 .card h2{margin-top:0;color:#e94560;font-size:14px;text-transform:uppercase}
 textarea{width:100%;height:200px;background:#0f3460;color:#eee;border:1px solid #333;
          border-radius:4px;padding:8px;font-family:monospace;font-size:13px;resize:vertical}
 button{background:#e94560;color:#fff;border:none;padding:8px 20px;border-radius:4px;
        cursor:pointer;font-family:monospace;font-size:13px;margin:4px}
 button:hover{background:#ff6b81}
 .btn-run{background:#00b894} .btn-run:hover{background:#00d2a0}
 .btn-stop{background:#d63031} .btn-stop:hover{background:#ff7675}
 button:disabled{opacity:0.4;cursor:not-allowed}
 .status{display:flex;gap:16px;flex-wrap:wrap}
 .status-item{background:#0f3460;padding:8px 16px;border-radius:4px;min-width:120px}
 .status-item strong{color:#fdcb6e;display:block;font-size:11px}
 #payloadList{list-style:none;padding:0}
 #payloadList li{padding:8px;background:#0f3460;margin:4px 0;border-radius:4px;
                display:flex;justify-content:space-between;align-items:center}
 #payloadList li button{margin:0;padding:4px 12px;font-size:11px}
 #editTitle{color:#fdcb6e;font-size:16px;font-weight:bold}
 #output{background:#000;color:#0f0;padding:8px;border-radius:4px;min-height:40px;
         font-size:12px;white-space:pre-wrap;overflow:auto}
 label{color:#aaa;font-size:12px}
</style>
</head>
<body>
<h1>DuckC2</h1>
<div class="status">
 <div class="status-item"><strong>State</strong><span id="state">?</span></div>
 <div class="status-item"><strong>Clients</strong><span id="clients">0</span></div>
 <div class="status-item"><strong>AP IP</strong><span id="apip">?</span></div>
</div>

<div class="card"><h2>Payloads on SD</h2><ul id="payloadList"><li>Loading...</li></ul></div>

<div class="card">
 <h2>Editor — <span id="editTitle">new.dd</span></h2>
 <textarea id="editor" spellcheck="false"># DuckyScript payload\nREM Edit me or paste your payload here\nDELAY 1000\nSTRING Hello from DuckC2!\nENTER</textarea>
 <div><button onclick="savePayload()">Save</button>
      <button onclick="loadEditor()" class="btn-run">Load</button>
      <button onclick="runPayload()" class="btn-run">Run Now</button></div>
</div>

<div class="card">
 <h2>Output</h2>
 <div id="output"></div>
 <button onclick="runPayload()" class="btn-run" style="margin-top:8px">Run Current</button>
 <button onclick="stopPayload()" class="btn-stop">Stop</button>
</div>

<script>
let currentName = 'new.dd';

function getToken() {
    let t = localStorage.getItem('duckc2_token');
    if (!t) {
        t = prompt('Auth token (printed to serial console at boot):') || '';
        if (t) localStorage.setItem('duckc2_token', t);
    }
    return t;
}

// Wrap fetch so mutating requests carry X-Auth-Token automatically. GETs are
// unauthenticated (read-only status/list/get-payload routes).
function authFetch(path, opts) {
    opts = opts || {};
    opts.headers = opts.headers || {};
    opts.headers['X-Auth-Token'] = getToken();
    return fetch(path, opts).then(r => {
        if (r.status === 401) {
            localStorage.removeItem('duckc2_token');
            log('401: token rejected, will re-prompt on next action');
        }
        return r;
    });
}

function log(msg) {
    document.getElementById('output').textContent += msg + '\\n';
    document.getElementById('output').scrollTop = 999999;
}

async function refresh() {
    let s; try { s = await fetch('/api/status').then(r=>r.json()); } catch(e) { return; }
    document.getElementById('state').textContent = s.state || '?';
    document.getElementById('clients').textContent = s.clients || 0;
    document.getElementById('apip').textContent = s.ap_ip || '?';
}

async function listPayloads() {
    let p; try { p = await fetch('/api/payloads').then(r=>r.json()); } catch(e) { return; }
    let ul = document.getElementById('payloadList');
    ul.innerHTML = '';
    if (!p.length) { ul.innerHTML = '<li>No payloads on SD</li>'; return; }
    p.forEach(x => {
        const li = document.createElement('li');
        const span = document.createElement('span');
        span.textContent = x.name;  // XSS-safe — filename never interpreted as HTML
        const div = document.createElement('div');
        const loadBtn = document.createElement('button');
        loadBtn.textContent = 'Load';
        loadBtn.addEventListener('click', () => load(x.name));
        const runBtn = document.createElement('button');
        runBtn.textContent = 'Run';
        runBtn.className = 'btn-run';
        runBtn.addEventListener('click', () => run(x.name));
        div.appendChild(loadBtn);
        div.appendChild(runBtn);
        li.appendChild(span);
        li.appendChild(div);
        ul.appendChild(li);
    });
}

async function load(name) {
    currentName = name;
    document.getElementById('editTitle').textContent = name;
    const text = await fetch('/api/payload/'+encodeURIComponent(name)).then(r=>r.text());
    document.getElementById('editor').value = text;
}

async function loadEditor() {
    await load(currentName);
    log('Loaded '+currentName);
}

async function savePayload() {
    let body = document.getElementById('editor').value;
    try {
        await authFetch('/api/payload/'+encodeURIComponent(currentName), {
            method: 'PUT', body: body,
            headers: {'Content-Type': 'text/plain'}
        });
        log('Saved '+currentName);
        await listPayloads();
    } catch(e) { log('Save failed: '+e); }
}

async function run(name) {
    log('Running '+name+'...');
    try {
        await authFetch('/api/run/'+encodeURIComponent(name), {method: 'POST'});
        pollStatus();
    } catch(e) { log('Run failed: '+e); }
}

async function runPayload() {
    // Save current editor as 'current.dd', then run it
    await saveAs('current.dd');
    await run('current.dd');
}

async function saveAs(name) {
    currentName = name;
    document.getElementById('editTitle').textContent = name;
    let body = document.getElementById('editor').value;
    await authFetch('/api/payload/'+encodeURIComponent(name), {
        method: 'PUT', body: body,
        headers: {'Content-Type': 'text/plain'}
    });
    await listPayloads();
}

async function stopPayload() {
    log('Stopping...');
    await authFetch('/api/stop', {method: 'POST'});
    log('Stopped');
}

function pollStatus() {
    let i = setInterval(async () => {
        let s = await fetch('/api/status').then(r=>r.json());
        document.getElementById('state').textContent = s.state || '?';
        if (s.state === 'idle' || s.state === 'stopped' || s.state === 'complete') {
            clearInterval(i);
        }
    }, 500);
}

document.addEventListener('DOMContentLoaded', () => {
    refresh();
    listPayloads();
    setInterval(refresh, 3000);
});
</script>
</body>
</html>
)rawliteral";

// ── Handlers ─────────────────────────────────────────────────────────────
static void handleRoot()
{
    _server.send(200, "text/html", DASHBOARD_HTML);
}

static void handleStatus()
{
    // Format: {"state":"running","clients":N,"ap_ip":"192.168.4.1"}
    String j = "{";
    j += "\"state\":\"";
    if (!_interp) j += "no_interp";
    else {
        InterpState s = _interp->getState();
        switch (s) {
            case INTERP_IDLE: j += "idle"; break;
            case INTERP_RUNNING: j += "running"; break;
            case INTERP_COMPLETE: j += "complete"; break;
            case INTERP_ERROR: j += "error"; break;
            case INTERP_STOPPED: j += "stopped"; break;
            default: j += "unknown";
        }
    }
    j += "\",\"clients\":" + String(WiFi.softAPgetStationNum());
    j += ",\"ap_ip\":\"" + WiFi.softAPIP().toString() + "\"}";
    _server.send(200, "application/json", j);
}

static void handleListPayloads()
{
    // [{"name":"hello.dd","size":123},...]
    String j = "[";
    File dir = Storage::fs().open(SD_PAYLOAD_DIR);
    if (dir) {
        File entry = dir.openNextFile();
        bool first = true;
        while (entry) {
            if (!entry.isDirectory()) {
                if (!first) j += ",";
                j += "{\"name\":\"";
                j += entry.name();
                j += "\",\"size\":";
                j += String(entry.size());
                j += "}";
                first = false;
            }
            entry = dir.openNextFile();
        }
        dir.close();
    }
    j += "]";
    _server.send(200, "application/json", j);
}

static void handleGetPayload()
{
    String name = _server.pathArg(0);
    if (!validName(name)) {
        _server.send(400, "text/plain", "Invalid name");
        return;
    }
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", SD_PAYLOAD_DIR, name.c_str());
    File f = Storage::fs().open(path);
    if (!f) {
        _server.send(404, "text/plain", "Not found");
        return;
    }
    _server.streamFile(f, "text/plain");
    f.close();
}

static void handlePutPayload()
{
    if (!authOk()) return;
    String name = _server.pathArg(0);
    if (!validName(name)) {
        _server.send(400, "text/plain", "Invalid name");
        return;
    }
    String body = _server.arg("plain");
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", SD_PAYLOAD_DIR, name.c_str());

    if (!Storage::dirExists(SD_PAYLOAD_DIR)) {
        Storage::createDir(SD_PAYLOAD_DIR);
    }

    File f = Storage::fs().open(path, FILE_WRITE);
    if (!f) {
        _server.send(500, "text/plain", "Cannot write");
        return;
    }
    f.print(body);
    f.close();
    _server.send(200, "text/plain", "OK");
}

static void handleRunPayload()
{
    if (!authOk()) return;
    String name = _server.pathArg(0);
    if (!validName(name)) {
        _server.send(400, "text/plain", "Invalid name");
        return;
    }
    if (_interp) {
        char* buf = NULL;
        int len = 0;
        if (!Storage::loadPayload(name.c_str(), &buf, &len)) {
            _server.send(404, "text/plain", "Payload not found");
            return;
        }
        _interp->loadBuffer(buf, len);
        free(buf);
        _interp->run();
    }
    _server.send(200, "text/plain", "Running");
    Hal::ledBlink(0, 255, 0, 2, 100);
}

static void handleStop()
{
    if (!authOk()) return;
    if (_interp) _interp->stop();
    _server.send(200, "text/plain", "Stopped");
}

static void handleNotFound()
{
    _server.send(404, "text/plain", "Not found");
}

// ── Public: start / stop / tick ─────────────────────────────────────────────
bool WebServer::start()
{
    Serial.print("[C2] Starting SoftAP...");
    WiFi.softAP(CFG_WIFI_SSID, CFG_WIFI_PASS, CFG_WIFI_CHANNEL, 0, 1);
    IPAddress ip = WiFi.softAPIP();
    Serial.printf(" OK — %s\n", ip.toString().c_str());

    generateToken();
    Serial.printf("[C2] Auth token: %s\n", _authToken);
    Serial.println("[C2] (paste this when the dashboard prompts; mutating routes require it)");

    // Routes
    _server.on("/", HTTP_GET, handleRoot);
    _server.on("/api/status", HTTP_GET, handleStatus);
    _server.on("/api/payloads", HTTP_GET, handleListPayloads);
    _server.on("/api/payload/(.*)", HTTP_GET, handleGetPayload);
    _server.on("/api/payload/(.*)", HTTP_PUT, handlePutPayload);
    _server.on("/api/run/(.*)", HTTP_POST, handleRunPayload);
    _server.on("/api/stop", HTTP_POST, handleStop);
    _server.onNotFound(handleNotFound);

    // Without this the `WebServer` class drops non-standard request headers,
    // so authOk() can't see X-Auth-Token.
    const char* tracked[] = { "X-Auth-Token" };
    _server.collectHeaders(tracked, 1);

    _server.begin();
    _running = true;
    Serial.println("[C2] HTTP server started");
    return true;
}

void WebServer::stop()
{
    _server.stop();
    WiFi.softAPdisconnect(true);
    _running = false;
    Serial.println("[C2] Server stopped");
}

void WebServer::tick()
{
    if (_running) _server.handleClient();
}

bool WebServer::running() { return _running; }

int WebServer::connectedClients()
{
    // WiFi.softAPgetStationNum() — number of connected stations
    return WiFi.softAPgetStationNum();
}

IPAddress WebServer::apIP() { return WiFi.softAPIP(); }

const char* WebServer::authToken() { return _authToken; }

// Setter for the interpreter pointer (call from main AFTER constructing interp)
void WebServer::setInterpreter(Interpreter* interp)
{
    _interp = interp;
}
