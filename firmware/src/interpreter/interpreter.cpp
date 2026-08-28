// =============================================================================
// interpreter.cpp — DuckyScript parser & executor (Tier 1 + Tier 2 + Security)
//
// Tier 1: REM, STRING, STRINGLN, DELAY, DEFAULTDELAY, DEFAULT_CHAR_DELAY,
//          keyboard keys + modifiers + combos
// Tier 2: DEFINE, VAR, IF/ELSE_IF/ELSE/END_IF, WHILE/END_WHILE,
//          FUNCTION/CALL/END_FUNCTION, STRING_MULTI, INJECT_MOD
// Security: JITTER_MAX, LAYOUT, DETECT_OS, EXFIL_START/STOP, ATTACKMODE
// =============================================================================

#include "interpreter.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "hal/hal.h"

// ─── Keycode table (DuckyScript name -> USB HID scancode) ───────────────────
static const struct { const char* n; uint8_t code; } KEYS[] = {
    {"ENTER",         0x28},  {"RETURN",      0x28},
    {"ESCAPE",        0x29},
    {"BACKSPACE",     0x2a},
    {"DELETE",        0x4c},
    {"INSERT",        0x49},
    {"TAB",           0x2b},
    {"SPACE",         0x2c},
    {"HOME",          0x4a},  {"END",         0x4d},
    {"PAGEUP",        0x4b},  {"PAGEDOWN",    0x4e},
    {"PRINTSCREEN",   0x46},
    {"SCROLLLOCK",    0x47},
    {"PAUSE",         0x48},  {"BREAK",       0x48},
    {"MENU",          0x65},  {"APP",         0x65},
    {"UP",            0x52},  {"UPARROW",     0x52},
    {"DOWN",          0x51},  {"DOWNARROW",   0x51},
    {"LEFT",          0x50},  {"LEFTARROW",   0x50},
    {"RIGHT",         0x4f},  {"RIGHTARROW",  0x4f},
    {"CAPSLOCK",      0x39},  {"NUMLOCK",     0x53},
    {"F1", 0x3a}, {"F2", 0x3b}, {"F3", 0x3c}, {"F4", 0x3d},
    {"F5", 0x3e}, {"F6", 0x3f}, {"F7", 0x40}, {"F8", 0x41},
    {"F9", 0x42}, {"F10",0x43}, {"F11",0x44}, {"F12",0x45},
    {NULL, 0}
};

// ─── Modifier table ─────────────────────────────────────────────────────────
static const struct { const char* n; uint8_t m; } MODS[] = {
    {"CTRL",    HK_MOD_LCTRL},  {"CONTROL", HK_MOD_LCTRL},
    {"ALT",     HK_MOD_LALT},   {"SHIFT",   HK_MOD_LSHIFT},
    {"GUI",     HK_MOD_LGUI},   {"WINDOWS", HK_MOD_LGUI},
    {"COMMAND", HK_MOD_LGUI},
    {NULL, 0}
};

// ─── Letter keycodes (a-z = 0x04..0x1d) ─────────────────────────────────────
static uint8_t letterCode(char c)
{
    if (c >= 'a' && c <= 'z') return 0x04 + (c - 'a');
    if (c >= 'A' && c <= 'Z') return 0x04 + (c - 'A');
    return 0;
}

// ─── Number keycodes ────────────────────────────────────────────────────────
// 0=0x27, 1=0x1e..9=0x26
static uint8_t digitCode(char c)
{
    if (c >= '0' && c <= '9') {
        if (c == '0') return 0x27;
        return 0x1e + (c - '1');
    }
    return 0;
}

// ─── Symbol keycodes (US layout — base reference) ───────────────────────────
static uint8_t symbolCodeUS(char c, uint8_t* mod)
{
    *mod = 0;
    switch (c) {
        case '!':  *mod = HK_MOD_LSHIFT; return 0x1e;
        case '@':  *mod = HK_MOD_LSHIFT; return 0x1f;
        case '#':  *mod = HK_MOD_LSHIFT; return 0x20;
        case '$':  *mod = HK_MOD_LSHIFT; return 0x21;
        case '%':  *mod = HK_MOD_LSHIFT; return 0x22;
        case '^':  *mod = HK_MOD_LSHIFT; return 0x23;
        case '&':  *mod = HK_MOD_LSHIFT; return 0x24;
        case '*':  *mod = HK_MOD_LSHIFT; return 0x25;
        case '(':  *mod = HK_MOD_LSHIFT; return 0x26;
        case ')':  *mod = HK_MOD_LSHIFT; return 0x27;
        case '-':  return 0x2d;
        case '_':  *mod = HK_MOD_LSHIFT; return 0x2d;
        case '=':  return 0x2e;
        case '+':  *mod = HK_MOD_LSHIFT; return 0x2e;
        case '[':  return 0x2f;
        case '{':  *mod = HK_MOD_LSHIFT; return 0x2f;
        case ']':  return 0x30;
        case '}':  *mod = HK_MOD_LSHIFT; return 0x30;
        case '\\': return 0x31;
        case '|':  *mod = HK_MOD_LSHIFT; return 0x31;
        case ';':  return 0x33;
        case ':':  *mod = HK_MOD_LSHIFT; return 0x33;
        case '\'': return 0x34;
        case '"':  *mod = HK_MOD_LSHIFT; return 0x34;
        case '`':  return 0x35;
        case '~':  *mod = HK_MOD_LSHIFT; return 0x35;
        case ',':  return 0x36;
        case '<':  *mod = HK_MOD_LSHIFT; return 0x36;
        case '.':  return 0x37;
        case '>':  *mod = HK_MOD_LSHIFT; return 0x37;
        case '/':  return 0x38;
        case '?':  *mod = HK_MOD_LSHIFT; return 0x38;
    }
    return 0;
}

// ─── Layout override tables ─────────────────────────────────────────────────
// Each table maps ASCII chars that differ from US layout to {scancode, modifiers}
// Sentinels: ch==0 marks end of table

struct LayoutOverride {
    char    ch;
    uint8_t code;
    uint8_t mod;
};

// Polish Programmer's layout — mostly identical to US
// Diacritical chars (ąćęłńóśźż) require AltGr, but those
// are entered via dead-key sequences, not single scancodes.
static const LayoutOverride LAYOUT_PL_T[] = {
    {0, 0, 0}  // Polish Programmer's = US with no overrides
};

// German QWERTZ layout
static const LayoutOverride LAYOUT_DE_T[] = {
    // Y and Z swapped
    {'z', 0x1d, 0},                          // z → physical y position (scancode 0x1d)
    {'Z', 0x1d, HK_MOD_LSHIFT},
    {'y', 0x1c, 0},                          // y → physical z position (scancode 0x1c)
    {'Y', 0x1c, HK_MOD_LSHIFT},
    // Special characters (ASCII only — the interpreter feeds one byte per
    // char, so multibyte UTF-8 glyphs like ß/€/° can never match a table
    // entry and are left out; see AGENTS.md layout caveat)
    {'@', 0x14, KEY_MOD_RALT},               // AltGr+Q
    {'#', 0x31, 0},                           // on the key left of Enter (US backslash)
    {'\\',0x2d, KEY_MOD_RALT},               // AltGr+ß
    {'[', 0x2f, KEY_MOD_RALT},               // AltGr+8
    {']', 0x30, KEY_MOD_RALT},               // AltGr+9
    {'{', 0x2f, KEY_MOD_RALT},               // AltGr+7 (same as [ on DE, no shift)
    {'}', 0x30, KEY_MOD_RALT},               // AltGr+0
    {'~', 0x30, KEY_MOD_RALT},               // AltGr++ (the key right of ß)
    {'^', 0x35, 0},                           // dead key position (grave on US)
    {'?', 0x2d, HK_MOD_LSHIFT},              // Shift+ß → ?
    {';', 0x36, HK_MOD_LSHIFT},              // Shift+,
    {':', 0x37, HK_MOD_LSHIFT},              // Shift+.
    {'-', 0x38, 0},                           // - on the slash key position
    {'_', 0x38, HK_MOD_LSHIFT},
    {'=', 0x27, HK_MOD_LSHIFT},              // Shift+0
    {'+', 0x30, 0},                           // on ] key
    {'*', 0x30, HK_MOD_LSHIFT},              // Shift+]
    {'\'',0x31, KEY_MOD_RALT},               // AltGr+\
    {'"', 0x1f, HK_MOD_LSHIFT},              // Shift+2
    {'&', 0x23, HK_MOD_LSHIFT},              // Shift+6
    {'/', 0x24, HK_MOD_LSHIFT},              // Shift+7
    {'(', 0x25, HK_MOD_LSHIFT},              // Shift+8
    {')', 0x26, HK_MOD_LSHIFT},              // Shift+9
    {'`', 0x2e, HK_MOD_LSHIFT},              // Shift+= (dead key)
    {0, 0, 0}
};

// ─── Layout-aware symbol resolution ─────────────────────────────────────────
// First check layout overrides, then fall back to US base
static uint8_t lookupLayoutOverride(uint8_t layoutId, char c, uint8_t* mod)
{
    const LayoutOverride* table = NULL;
    switch (layoutId) {
        case LAYOUT_PL: table = LAYOUT_PL_T; break;
        case LAYOUT_DE: table = LAYOUT_DE_T; break;
        default: return 0;  // US or unknown — no overrides
    }
    for (int i = 0; table[i].ch != 0; i++) {
        if (table[i].ch == c) {
            *mod = table[i].mod;
            return table[i].code;
        }
    }
    return 0;  // not in override table
}

// ── Helpers ──────────────────────────────────────────────────────────────────
char* Interpreter::trim(char* s)
{
    while (*s == ' ' || *s == '\t') s++;
    int len = strlen(s);
    while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r' ||
                        s[len-1] == ' ' || s[len-1] == '\t'))
        s[--len] = '\0';
    return s;
}

bool Interpreter::startsWith(const char* str, const char* prefix)
{
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

void Interpreter::jitter()
{
    if (_jitterMax > 0) {
        delay(random(0, _jitterMax));
    }
}

// ── Layout-aware character-to-scancode ───────────────────────────────────────
uint8_t Interpreter::symbolForChar(char c, uint8_t* mod)
{
    // Check layout overrides first
    uint8_t code = lookupLayoutOverride(_layoutId, c, mod);
    if (code) return code;
    // Fall back to US base
    return symbolCodeUS(c, mod);
}

// ─── Named constant lookup for evalExpr ─────────────────────────────────────
// Maps OS names and other keywords to numeric values for condition evaluation
static uint16_t namedConstant(const char* name)
{
    if (strcmp(name, "WINDOWS") == 0)   return OS_WINDOWS;
    if (strcmp(name, "MACOS") == 0)     return OS_MACOS;
    if (strcmp(name, "LINUX") == 0)     return OS_LINUX;
    if (strcmp(name, "CHROMEOS") == 0)  return OS_CHROMEOS;
    if (strcmp(name, "UNKNOWN") == 0)   return OS_UNKNOWN;
    if (strcmp(name, "TRUE") == 0)      return 1;
    if (strcmp(name, "FALSE") == 0)     return 0;
    if (strcmp(name, "HID") == 0)       return 1;
    if (strcmp(name, "STORAGE") == 0)   return 2;
    return 0xFFFF;  // sentinel: not a known constant
}

// ── Lookup ──────────────────────────────────────────────────────────────────
uint8_t Interpreter::lookupKey(const char* name)
{
    for (int i = 0; KEYS[i].n; i++)
        if (strcmp(KEYS[i].n, name) == 0) return KEYS[i].code;
    return 0;
}
uint8_t Interpreter::lookupMod(const char* name)
{
    for (int i = 0; MODS[i].n; i++)
        if (strcmp(MODS[i].n, name) == 0) return MODS[i].m;
    return 0;
}

// ── Constructor ─────────────────────────────────────────────────────────────
Interpreter::Interpreter()
{
    memset(this, 0, sizeof(*this));
    _defaultDelay = CFG_DEFAULT_DELAY;
    _charDelay    = 5;  // ms between keystrokes within STRING
    _state        = INTERP_IDLE;
    _layoutId     = LAYOUT_US;
}

// ── Load from buffer ────────────────────────────────────────────────────────
bool Interpreter::loadBuffer(const char* buf, int len)
{
    _lineCount = 0;
    _pc = 0;
    _funcCount = 0;
    _varCount = 0;
    _defCount = 0;
    _blockDepth = 0;
    _callTop = 0;
    _state = INTERP_IDLE;
    _defaultDelay = CFG_DEFAULT_DELAY;
    _charDelay = 5;
    _jitterMax = 0;
    _layoutId = LAYOUT_US;
    _osDetectStarted = false;

    int off = 0;
    while (off < len && _lineCount < MAX_LINES) {
        int eol = off;
        while (eol < len && buf[eol] != '\n' && buf[eol] != '\r') eol++;
        int n = eol - off;
        if (n >= MAX_LINE_LEN) n = MAX_LINE_LEN - 1;
        memcpy(_lines[_lineCount], buf + off, n);
        _lines[_lineCount][n] = '\0';
        _lineCount++;
        // skip newline
        while (eol < len && (buf[eol] == '\n' || buf[eol] == '\r')) eol++;
        off = eol;
    }

    // Pre-process: expand DEFINEs, pre-scan functions
    preProcess();
    return true;
}

// ── Built-in variable injection ─────────────────────────────────────────────
void Interpreter::setBuiltinVar(const char* name, uint16_t val)
{
    setVar(name, val);
}

// ── Pre-process: expand DEFINE, pre-scan FUNCTION/END_FUNCTION ───────────────
void Interpreter::preProcess()
{
    // Phase 1: collect DEFINE and FUNCTION declarations
    for (int i = 0; i < _lineCount; i++) {
        char* line = trim(_lines[i]);

        if (startsWith(line, "DEFINE")) {
            // DEFINE #NAME value   or   DEFINE NAME value
            char* p = line + 6;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '#') p++;  // optional leading #
            char* nameStart = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            int nlen = p - nameStart;
            char* valStart = p;
            while (*valStart == ' ' || *valStart == '\t') valStart++;

            if (_defCount < MAX_DEFINES && nlen > 0 && nlen < MAX_DEFINE_NAME) {
                DefineEntry* de = &_defines[_defCount];
                strncpy(de->name, nameStart, nlen);
                de->name[nlen] = '\0';
                strncpy(de->value, valStart, MAX_LINE_LEN - 1);
                de->value[MAX_LINE_LEN - 1] = '\0';
                _defCount++;
            }
            _lines[i][0] = '\0';  // remove DEFINE line
            continue;
        }

        if (startsWith(line, "FUNCTION")) {
            char* p = line + 8;
            while (*p == ' ') p++;
            char* endParen = strchr(p, '(');
            if (endParen && _funcCount < CFG_MAX_FUNCTIONS) {
                FuncEntry* fe = &_funcs[_funcCount];
                int flen = endParen - p;
                if (flen >= MAX_FUNC_NAME) flen = MAX_FUNC_NAME - 1;
                strncpy(fe->name, p, flen);
                fe->name[flen] = '\0';
                fe->startLine = i + 1;
                fe->endLine = i;  // will fill in on END_FUNCTION scan
                _funcCount++;
            }
            _lines[i][0] = '\0';
        }

        if (strcmp(line, "END_FUNCTION") == 0) {
            if (_funcCount > 0) {
                _funcs[_funcCount - 1].endLine = i;
            }
            _lines[i][0] = '\0';
        }
    }

    // Phase 2: expand DEFINE in all remaining lines (forward references OK).
    // resolveDefines writes the substituted line back into its argument
    // (truncating to MAX_LINE_LEN - 1) and returns the length.
    for (int i = 0; i < _lineCount; i++) {
        if (!_lines[i][0]) continue;
        resolveDefines(_lines[i]);
    }
}

// True when c can be part of an identifier (so it must NOT be a word boundary
// — used to suppress DEFINE substitution inside larger words).
static bool isIdentChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

int Interpreter::resolveDefines(char* src)
{
    char buf[MAX_LINE_LEN * 2] = {0};
    int wpos = 0;
    const char* p = src;
    const char* origin = src;

    while (*p && wpos < (int)sizeof(buf) - 1) {
        bool matched = false;
        // Only attempt a substitution at a word boundary: start of line, or
        // preceded by a non-identifier character. Prevents FOO inside FOOLISH
        // from being rewritten.
        bool atBoundary = (p == origin) || !isIdentChar(*(p - 1));
        if (atBoundary) {
            for (int d = 0; d < _defCount; d++) {
                int dlen = strlen(_defines[d].name);
                if (dlen == 0) continue;
                if (strncmp(p, _defines[d].name, dlen) == 0 &&
                    !isIdentChar(p[dlen])) {
                    const char* val = _defines[d].value;
                    while (*val && wpos < (int)sizeof(buf) - 1) {
                        buf[wpos++] = *val++;
                    }
                    p += dlen;
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) {
            buf[wpos++] = *p++;
        }
    }

    int copy = wpos;
    if (copy > MAX_LINE_LEN - 1) copy = MAX_LINE_LEN - 1;
    memcpy(src, buf, copy);
    src[copy] = '\0';
    return copy;
}

// ── Run / tick / stop / getState ────────────────────────────────────────────
void Interpreter::run()
{
    if (_state == INTERP_IDLE || _state == INTERP_COMPLETE ||
        _state == INTERP_STOPPED || _state == INTERP_ERROR) {
        _pc = 0;
        _blockDepth = 0;
        _callTop = 0;
        _state = INTERP_RUNNING;
        Serial.println("[INT] Run");
    }
}

void Interpreter::tick()
{
    if (_state != INTERP_RUNNING) return;

    int steps = 0;
    while (_state == INTERP_RUNNING && _pc < _lineCount && steps < 200) {
        if (!_lines[_pc][0]) { _pc++; continue; }
        bool advanced = execLine(_pc);
        if (advanced) {
            _pc++;
        } else {
            // execLine kept _pc where it set it. Three cases land here:
            //   - CALL / RESTART_PAYLOAD jumped; target is at _pc.
            //   - IF/ELSE_IF/ELSE skipped to a new branch line; that line
            //     is at _pc and should execute on the next tick.
            //   - Cooperative pause (e.g. DETECT_OS) is waiting on a timer.
            // Break so the outer loop() pumps C2Server/Display/etc. before
            // we re-enter.
            break;
        }
        steps++;
    }

    if (_pc >= _lineCount && _state == INTERP_RUNNING) {
        _state = INTERP_COMPLETE;
        Serial.println("[INT] Done");
    }
}

void Interpreter::stop()
{
    _state = INTERP_STOPPED;
    Hal::release();
    Serial.println("[INT] Stopped");
}

InterpState Interpreter::getState() { return _state; }

// ── executeLine ──────────────────────────────────────────────────────────────
bool Interpreter::execLine(int lineno)
{
    char* raw = _lines[lineno];
    char* text = trim(raw);

    // Skip empty / comment
    if (!text[0]) return true;
    if (startsWith(text, "REM") &&
        (text[3] == '\0' || text[3] == ' ' || text[3] == '\t')) return true;

    // ── Block handling ───────────────────────────────────────────────────
    if (handleBlockStart(text)) {
        return true;
    }

    // IF not inside an active block, skip
    if (!isBlockActive()) {
        return true;
    }

    // ── Tier 1: DEFAULTDELAY ─────────────────────────────────────────────
    if (startsWith(text, "DEFAULTDELAY ") || startsWith(text, "DEFAULT_DELAY ")) {
        _defaultDelay = atoi(text + (startsWith(text, "DEFAULT_DELAY ") ? 14 : 13));
        return true;
    }

    // ── Tier 1: DEFAULT_CHAR_DELAY ───────────────────────────────────────
    if (startsWith(text, "DEFAULT_CHAR_DELAY ")) {
        _charDelay = atoi(text + 19);
        return true;
    }

    // ── Security: JITTER_MAX — random inter-keystroke delay ──────────────
    if (startsWith(text, "JITTER_MAX ") || startsWith(text, "JITTER ")) {
        const char* p = startsWith(text, "JITTER_MAX ") ? text + 11 : text + 7;
        while (*p == ' ') p++;
        _jitterMax = atoi(p);
        Serial.printf("[INT] Jitter: 0-%d ms\n", _jitterMax);
        return true;
    }

    // ── Security: LAYOUT — keyboard layout selection ─────────────────────
    if (startsWith(text, "LAYOUT ")) {
        const char* p = text + 7;
        while (*p == ' ') p++;
        if (strcmp(p, "US") == 0)      _layoutId = LAYOUT_US;
        else if (strcmp(p, "PL") == 0)  _layoutId = LAYOUT_PL;
        else if (strcmp(p, "DE") == 0)  _layoutId = LAYOUT_DE;
        else Serial.printf("[INT] Unknown layout: %s (staying US)\n", p);
        Serial.printf("[INT] Layout set: %s (id=%d)\n", p, _layoutId);
        return true;
    }

    // ── Security: DETECT_OS — trigger OS detection, set $_OS ─────────────
    // Runs cooperatively so the C2 server keeps ticking. The interpreter
    // pauses on this line: we return false (don't advance PC) until the
    // detection window closes, then commit the result and advance.
    if (strcmp(text, "DETECT_OS") == 0) {
        if (!_osDetectStarted) {
            Serial.println("[INT] Running OS detection...");
            Hal::osDetectStart();
            _osDetectStarted = true;
            _osDetectBeganMs = millis();
            return false;  // re-enter on next tick
        }
        Hal::osDetectTick();
        if (millis() - _osDetectBeganMs < CFG_OS_DETECT_WINDOW_MS) {
            return false;  // still detecting; keep PC where it is
        }
        uint8_t osId  = Hal::osDetectResult();
        uint8_t count = Hal::osDetectCount();
        setVar("_OS", osId);
        setVar("_HOST_CONFIGURATION_REQUEST_COUNT", count);
        Serial.printf("[INT] $_OS=%d, $_HOST_CONFIGURATION_REQUEST_COUNT=%d\n",
                      osId, count);
        _osDetectStarted = false;
        return true;
    }

    // ── Security: EXFIL_START — begin keystroke capture ──────────────────
    if (strcmp(text, "EXFIL_START") == 0 || strcmp(text, "EXFILSTART") == 0) {
        Hal::captureBegin();
        return true;
    }

    // ── Security: EXFIL_STOP — end capture, flush to SD ──────────────────
    if (strcmp(text, "EXFIL_STOP") == 0 || strcmp(text, "EXFILSTOP") == 0) {
        Hal::captureEnd();
        return true;
    }

    // ── Security: ATTACKMODE — switch USB descriptor mode ────────────────
    if (startsWith(text, "ATTACKMODE ") || startsWith(text, "ATTACKMODE")) {
        const char* p = text + 10;  // strlen("ATTACKMODE")
        while (*p == ' ') p++;

        bool wantHID     = (strstr(p, "HID") != NULL);
        bool wantStorage = (strstr(p, "STORAGE") != NULL);

        if (wantHID) {
            // HID is always active (we started with keyboardBegin)
            Serial.println("[INT] ATTACKMODE: HID active");
        }
        if (wantStorage) {
            if (Hal::enableMSC()) {
                Serial.println("[INT] ATTACKMODE: STORAGE active");
            } else {
                Serial.println("[INT] ATTACKMODE: STORAGE failed (SD unavailable or MSC not plumbed)");
            }
        }
        if (!wantHID && !wantStorage) {
            Serial.println("[INT] ATTACKMODE: unknown mode, ignoring");
        }
        return true;
    }

    // ── Tier 2: VAR ──────────────────────────────────────────────────────
    if (startsWith(text, "VAR ")) {
        // VAR $x = 42  — parse and store
        char* p = text + 4;
        while (*p == ' ') p++;
        if (*p == '$') p++;
        char vname[MAX_VAR_NAME];
        int vn = 0;
        while (*p && *p != '=' && vn < MAX_VAR_NAME-1 && *p != ' ')
            vname[vn++] = *p++;
        vname[vn] = '\0';
        while (*p != '=' && *p) p++;  // skip to =
        if (*p == '=') {
            p++;                      // skip past '='
            while (*p == ' ') p++;    // skip spaces after '='
            uint16_t val = evalExpr(p);
            setVar(vname, val);
        }
        return true;
    }

    // ── Tier 1: DELAY ────────────────────────────────────────────────────
    if (startsWith(text, "DELAY ")) {
        execDelay(atoi(text + 6));
        return true;
    }

    // ── Tier 1: STRINGLN ─────────────────────────────────────────────────
    if (startsWith(text, "STRINGLN ")) {
        execStringln(text + 9);
        return true;
    }
    // STRINGLN as alias
    if (startsWith(text, "STRINGLINE ")) {
        execStringln(text + 11);
        return true;
    }

    // ── Tier 1: STRING ───────────────────────────────────────────────────
    if (startsWith(text, "STRING ")) {
        execString(text + 7);
        return true;
    }

    // ── Tier 1: INJECT_MOD ───────────────────────────────────────────────
    if (startsWith(text, "INJECTMOD ")) {
        uint8_t m = lookupMod(text + 10);
        if (m) Hal::press(m, HK_NONE);
        Hal::release();
        return true;
    }

    // ── Tier 2: CALL ─────────────────────────────────────────────────────
    if (startsWith(text, "CALL ")) {
        const char* fn = text + 5;
        while (*fn == ' ') fn++;
        for (int i = 0; i < _funcCount; i++) {
            if (strcmp(_funcs[i].name, fn) == 0 && _callTop < CFG_MAX_NESTED) {
                _callStack[_callTop++] = _pc;  // save return
                _pc = _funcs[i].startLine;     // jump
                return false;  // manual advance
            }
        }
        return true;  // unknown function, skip
    }

    // ── Tier 2: RESTART_PAYLOAD ──────────────────────────────────────────
    if (startsWith(text, "RESTART_PAYLOAD")) {
        // Restart from the top with a fresh execution context. Symbol tables
        // (_defines, _funcs) and parsed lines stay; runtime state resets.
        // Return false so tick() doesn't auto-increment off our reset PC.
        _pc = 0;
        _blockDepth = 0;
        _callTop    = 0;
        _varCount   = 0;
        _jitterMax  = 0;
        _layoutId   = LAYOUT_US;
        _defaultDelay = CFG_DEFAULT_DELAY;
        _charDelay    = 5;
        _osDetectStarted = false;
        return false;
    }

    // ── Tier 2: STOP_PAYLOAD ─────────────────────────────────────────────
    if (startsWith(text, "STOP_PAYLOAD")) {
        _state = INTERP_STOPPED;
        return true;
    }

    // ── KEY command: bare keyword like ENTER, TAB, DELAY (no args) ───────
    uint8_t key = lookupKey(text);
    if (key) {
        Hal::press(HK_NONE, key);
        Hal::release();
        return true;
    }

    // ── Modifier combo: CTRL ALT DELETE, GUI r, etc. ─────────────────────
    uint8_t mods = 0;
    const char* afterMods = text;
    {
        const char* p = text;
        bool found = true;
        while (found && *p) {
            found = false;
            for (int i = 0; MODS[i].n; i++) {
                int n = strlen(MODS[i].n);
                if (strncmp(p, MODS[i].n, n) == 0 &&
                    (p[n] == ' ' || p[n] == '\0')) {
                    mods |= MODS[i].m;
                    p += n;
                    while (*p == ' ') p++;
                    afterMods = p;
                    found = true;
                    break;
                }
            }
        }
    }

    if (mods && afterMods[0]) {
        key = lookupKey(afterMods);
        if (key) {
            Hal::press(mods, key);
            Hal::release();
            return true;
        }
        // Single char: e.g., "GUI r" → r is lowercase, treat as letter
        char c = afterMods[0];
        uint8_t cm = 0;
        uint8_t cc = symbolForChar(c, &cm);
        if (!cc) {
            cc = letterCode(c);
            if (cc && c >= 'A' && c <= 'Z') cm |= HK_MOD_LSHIFT;
        }
        if (!cc) cc = digitCode(c);
        if (cc) {
            Hal::press(mods | cm, cc);
            Hal::release();
            return true;
        }
    }

    // If it's JUST a modifier with nothing after, press and release it alone
    if (mods && !afterMods[0]) {
        Hal::press(mods, HK_NONE);
        Hal::release();
        return true;
    }

    Serial.printf("[INT] Unknown: %s\n", text);
    return true;  // skip unknown
}

// ── Key / String execution ──────────────────────────────────────────────────
void Interpreter::execKey(const char* name)
{
    uint8_t key = lookupKey(name);
    if (key) { Hal::press(HK_NONE, key); Hal::release(); }
}

void Interpreter::execCombo(const char* afterMods, uint8_t mods)
{
    uint8_t key = lookupKey(afterMods);
    if (key) { Hal::press(mods, key); Hal::release(); return; }
    char c = afterMods[0];
    uint8_t cm = 0;
    uint8_t cc = symbolForChar(c, &cm);
    if (!cc) cc = letterCode(c);
    if (!cc) cc = digitCode(c);
    if (cc) { Hal::press(mods | cm, cc); Hal::release(); }
}

void Interpreter::execString(const char* text)
{
    uint8_t mod;
    uint8_t code;
    for (int i = 0; text[i]; i++) {
        char c = text[i];
        mod = 0;
        code = symbolForChar(c, &mod);
        if (!code) {
            code = letterCode(c);
            // letterCode() returns the same scancode for 'a' and 'A'; OR-in
            // SHIFT for ASCII uppercase so STRING types case-correctly.
            if (code && c >= 'A' && c <= 'Z') mod |= HK_MOD_LSHIFT;
        }
        if (!code) code = digitCode(c);
        if (code) Hal::press(mod, code);
        Hal::release();
        jitter();
        if (_charDelay) delay(_charDelay);
    }
}

void Interpreter::execStringln(const char* text)
{
    execString(text);
    Hal::press(HK_NONE, 0x28);  // ENTER
    Hal::release();
}

void Interpreter::execDelay(int ms)
{
    if (ms < CFG_MIN_DELAY) ms = CFG_MIN_DELAY;
    delay(ms);
}

// ── Block handling ──────────────────────────────────────────────────────────
// Pushes a new IF/WHILE block, or sets _state = INTERP_ERROR on overflow so
// the matching END_* can't silently mis-pair against an unrelated outer block.
bool Interpreter::pushBlock(int type, int startLine, bool taken)
{
    if (_blockDepth >= CFG_MAX_NESTED) {
        Serial.printf("[INT] Block nesting overflow at line %d (max %d)\n",
                      startLine, CFG_MAX_NESTED);
        _state = INTERP_ERROR;
        return false;
    }
    _blocks[_blockDepth].type      = type;
    _blocks[_blockDepth].startLine = startLine;
    _blocks[_blockDepth].wasTaken  = taken;
    _blocks[_blockDepth].active    = taken;
    _blockDepth++;
    return true;
}

// Return convention: true  → tick() advances past the current line
//                    false → tick() leaves _pc where we set it so the line
//                            we just landed on (after skipToNext /
//                            skipToMatching / rewind / function-return) is
//                            executed on the next tick.
bool Interpreter::handleBlockStart(const char* line)
{
    // IF ... THEN / IF (...) — push, and on false skip to the next branch
    // (ELSE_IF / ELSE / END_IF) so chained conditions are honoured.
    if (startsWith(line, "IF ")) {
        const char* p = line + 3;
        while (*p == ' ') p++;
        bool taken = evalCond(p);
        pushBlock(1, _pc, taken);
        if (!taken) {
            skipToNext();
            return false;  // execute the branch / END_IF we landed on
        }
        return true;       // step into body

    // ELSE_IF — same chain logic; if an earlier branch was taken, jump past
    // the whole IF block (lands ON END_IF so the pop fires).
    } else if (startsWith(line, "ELSE_IF ")) {
        if (_blockDepth > 0) {
            if (_blocks[_blockDepth-1].wasTaken) {
                skipToMatching();
                return false;
            }
            const char* p = line + 8;
            while (*p == ' ') p++;
            bool taken = evalCond(p);
            if (taken) {
                _blocks[_blockDepth-1].wasTaken = true;
                _blocks[_blockDepth-1].active = true;
                return true;   // step into branch body
            }
            skipToNext();
            return false;      // try the next branch
        }
        return true;

    // ELSE — runs iff no earlier branch was taken
    } else if (strcmp(line, "ELSE") == 0) {
        if (_blockDepth > 0) {
            if (_blocks[_blockDepth-1].wasTaken) {
                skipToMatching();
                return false;
            }
            _blocks[_blockDepth-1].wasTaken = true;
            _blocks[_blockDepth-1].active = true;
        }
        return true;

    // WHILE ... — skipToMatching lands ON END_WHILE so the pop fires there
    } else if (startsWith(line, "WHILE ")) {
        const char* p = line + 6;
        while (*p == ' ') p++;
        bool cond = evalCond(p);
        pushBlock(2, _pc, cond);
        if (!cond) {
            skipToMatching();
            return false;
        }
        return true;

    // END_IF / END_WHILE / END_FUNCTION — pop the matching block, and for
    // WHILE re-evaluate the condition; if still true, re-push and rewind
    // so the body re-runs.
    } else if (strcmp(line, "END_IF") == 0 || strcmp(line, "END_WHILE") == 0 ||
               strcmp(line, "END_FUNCTION") == 0) {
        if (_blockDepth > 0) {
            BlockEntry& b = _blocks[_blockDepth-1];
            bool wasWhile = (b.type == 2);
            int  whileStart = b.startLine;
            _blockDepth--;

            if (wasWhile && (_blockDepth == 0 || _blocks[_blockDepth-1].active)) {
                char* wl = _lines[whileStart];
                const char* p = wl + 6;  // skip "WHILE "
                while (*p == ' ') p++;
                bool cond = evalCond(p);
                if (cond) {
                    pushBlock(2, whileStart, true);
                    _pc = whileStart;       // tick advances to body
                    return true;
                }
            }
        }
        if (strcmp(line, "END_FUNCTION") == 0 && _callTop > 0) {
            _pc = _callStack[--_callTop];   // tick advances past the CALL
            return true;
        }
        return true;
    }

    return false;
}

bool Interpreter::isBlockActive()
{
    for (int i = 0; i < _blockDepth; i++)
        if (!_blocks[i].active) return false;
    return true;
}

// Skip past the rest of the current IF/WHILE body, landing ON the matching
// END_*. Tracks nested IF/WHILE so inner blocks don't fool the depth count.
// Called for: WHILE-not-taken, ELSE_IF-after-already-taken, ELSE-after-taken.
void Interpreter::skipToMatching()
{
    int depth = 0;  // count nested IF/WHILE we step over
    while (_pc + 1 < _lineCount) {
        _pc++;
        char* l = trim(_lines[_pc]);
        if (startsWith(l, "IF ") || startsWith(l, "WHILE ")) {
            depth++;
        } else if (startsWith(l, "END_")) {
            if (depth == 0) return;  // matching END_*, land here so pop fires
            depth--;
        }
    }
    _pc = _lineCount;
}

// Skip forward inside an IF block to the next branch point (ELSE_IF / ELSE /
// END_IF) at the *current* nesting depth. Lands ON that line so the handler
// runs. Called when IF or ELSE_IF evaluated false and we want to try the
// next branch. Without this, `skipToMatching` would jump past END_IF and
// skip all alternative branches.
void Interpreter::skipToNext()
{
    int depth = 0;
    while (_pc + 1 < _lineCount) {
        _pc++;
        char* l = trim(_lines[_pc]);
        if (startsWith(l, "IF ") || startsWith(l, "WHILE ")) {
            depth++;
        } else if (startsWith(l, "END_")) {
            if (depth == 0) return;  // matching END_IF
            depth--;
        } else if (depth == 0 &&
                   (startsWith(l, "ELSE_IF ") || strcmp(l, "ELSE") == 0)) {
            return;
        }
    }
    _pc = _lineCount;
}

// ── Variables ───────────────────────────────────────────────────────────────
uint16_t Interpreter::getVar(const char* name)
{
    for (int i = 0; i < _varCount; i++)
        if (strcmp(_vars[i].name, name) == 0) return _vars[i].value;
    return 0;  // undefined vars = 0
}

bool Interpreter::setVar(const char* name, uint16_t val)
{
    if (val > CFG_MAX_VAR) val = CFG_MAX_VAR;
    for (int i = 0; i < _varCount; i++) {
        if (strcmp(_vars[i].name, name) == 0) { _vars[i].value = val; return true; }
    }
    if (_varCount < CFG_MAX_VAR_COUNT) {
        strncpy(_vars[_varCount].name, name, MAX_VAR_NAME-1);
        _vars[_varCount].name[MAX_VAR_NAME-1] = '\0';
        _vars[_varCount].value = val;
        _varCount++;
        return true;
    }
    return false;
}

// ── Expression evaluator (simplified: $x + 5, $y * 2, $a && $b) ─────────
uint16_t Interpreter::evalExpr(const char* expr)
{
    const char* p = expr;
    while (*p == ' ') p++;
    
    if (*p == '$') {
        p++;
        char name[MAX_VAR_NAME];
        int n = 0;
        while (*p && *p != ' ' && *p != '\0' && n < MAX_VAR_NAME-1)
            name[n++] = *p++;
        name[n] = '\0';
        return getVar(name);
    }

    // Check for named constants (WINDOWS, MACOS, LINUX, TRUE, FALSE)
    char word[32];
    int wn = 0;
    const char* ws = p;
    while (*ws && *ws != ' ' && *ws != '\0' && wn < 31)
        word[wn++] = *ws++;
    word[wn] = '\0';
    uint16_t nc = namedConstant(word);
    if (nc != 0xFFFF) return nc;

    return atoi(p);
}

// ── Condition evaluator ───────────────────────────────────────────────────
bool Interpreter::evalCond(const char* expr)
{
    // Supports: $x == 5, $x != 0, $x > 3, $x < 10, $x >= 5, $x <= 3
    // Also: $_OS == WINDOWS, $_OS != MACOS (named constants resolved in evalExpr)
    char left[64]  = {0};
    char right[64] = {0};
    char op[4]     = {0};

    // Trim
    while (*expr == ' ') expr++;
    char tmp[128];
    strncpy(tmp, expr, 127);
    tmp[127] = '\0';

    // Strip trailing THEN/DO
    char* thenPos = strstr(tmp, " THEN");
    if (!thenPos) thenPos = strstr(tmp, " DO");
    if (thenPos) *thenPos = '\0';
    // Re-trim
    int len = strlen(tmp);
    while (len > 0 && (tmp[len-1] == ' ' || tmp[len-1] == '\t' ||
                        tmp[len-1] == ')' || tmp[len-1] == '\n'))
        tmp[--len] = '\0';
    // Strip leading parenthesis
    const char* exprStart = tmp;
    while (*exprStart == ' ' || *exprStart == '(') exprStart++;
    strncpy(tmp, exprStart, 127);
    tmp[127] = '\0';

    // Find operator (longest first, so >= beats >)
    const char* ops[] = {">=", "<=", "!=", "==", ">", "<"};
    int opLen[] =      { 2,   2,   2,   2,   1,   1 };

    for (int i = 0; i < 6; i++) {
        const char* found = strstr(tmp, ops[i]);
        if (found) {
            int llen = found - tmp;
            while (llen > 0 && tmp[llen-1] == ' ') llen--;
            // Clamp to fit so over-long operands degrade to truncation
            // instead of being silently skipped (which would leave `left`
            // as zero-init and evalExpr() would return 0).
            if (llen > (int)sizeof(left) - 1) llen = sizeof(left) - 1;
            memcpy(left, tmp, llen);
            left[llen] = '\0';

            strncpy(op, ops[i], 3);

            const char* rp = found + opLen[i];
            while (*rp == ' ') rp++;
            int rlen = strlen(rp);
            while (rlen > 0 && rp[rlen-1] == ' ') rlen--;
            if (rlen > (int)sizeof(right) - 1) rlen = sizeof(right) - 1;
            memcpy(right, rp, rlen);
            right[rlen] = '\0';
            break;
        }
    }

    if (!op[0]) {
        // Bare truthy: feed the whole (trimmed) expression to evalExpr.
        return evalExpr(tmp) != 0;
    }

    uint16_t l = evalExpr(left);
    uint16_t r = evalExpr(right);

    if (strcmp(op, "==") == 0) return l == r;
    if (strcmp(op, "!=") == 0) return l != r;
    if (strcmp(op, ">")  == 0) return l > r;
    if (strcmp(op, "<")  == 0) return l < r;
    if (strcmp(op, ">=") == 0) return l >= r;
    if (strcmp(op, "<=") == 0) return l <= r;
    return false;
}
