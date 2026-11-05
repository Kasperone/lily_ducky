// =============================================================================
// lily_dashboard.h - LilyDucky C2 dashboard (GET / body), copied verbatim
// from lily_ducky firmware/src/c2/web_server.cpp (DASHBOARD_HTML).
// Inline so the handler has no SPIFFS/LittleFS dependency, same as lily.
// =============================================================================
#ifndef LILY_DASHBOARD_H
#define LILY_DASHBOARD_H

static const char LILY_DASHBOARD_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>LilyDucky</title>
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
<h1>LilyDucky</h1>
<div class="status">
 <div class="status-item"><strong>State</strong><span id="state">?</span></div>
 <div class="status-item"><strong>Clients</strong><span id="clients">0</span></div>
 <div class="status-item"><strong>AP IP</strong><span id="apip">?</span></div>
</div>

<div class="card"><h2>Payloads on SD</h2><ul id="payloadList"><li>Loading...</li></ul></div>

<div class="card">
 <h2>Editor — <span id="editTitle">new.dd</span></h2>
 <textarea id="editor" spellcheck="false"># DuckyScript payload\nREM Edit me or paste your payload here\nDELAY 1000\nSTRING Hello from LilyC2!\nENTER</textarea>
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
    let t = localStorage.getItem('lilyDuckyToken');
    if (!t) {
        t = prompt('Auth token (printed to serial console at boot):') || '';
        if (t) localStorage.setItem('lilyDuckyToken', t);
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
            localStorage.removeItem('lilyDuckyToken');
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

#endif // LILY_DASHBOARD_H
