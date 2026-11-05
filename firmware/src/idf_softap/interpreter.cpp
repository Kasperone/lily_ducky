// =============================================================================
// interpreter.cpp - DuckyScript parser & executor (pure ESP-IDF, layer 3b)
// =============================================================================
// Ported from lily_ducky firmware/src/interpreter/interpreter.cpp with the
// Arduino surface removed:
//
//   Serial.println/printf  -> ESP_LOGI / ESP_LOGW
//   millis()               -> esp_timer_get_time() / 1000
//   delay(ms)              -> vTaskDelay() in short slices so stop() is prompt
//   random(0, n)           -> esp_random()
//   Hal::press/release     -> duckhal_press/duckhal_release (no-ops on the C5)
//
// Parsing and execution semantics are deliberately unchanged: the same keycode
// table, the same DEFINE/FUNCTION preprocessing, the same IF/ELSE_IF/ELSE and
// WHILE block machine (including the "return false means _pc already moved"
// convention), the same variable/expression evaluator, the same layouts.
//
// The one structural difference is ownership: the original reserved
// 512 * 256 = 128 KiB of char[][] as a class member, which is fine on an S3
// with PSRAM but not on the C5 (no PSRAM, ~384 KiB SRAM shared with WiFi and
// httpd). Here the line table is a single heap block sized to the payload's
// actual line count and released when the run ends, so a small payload costs a
// few KiB instead of 128 KiB. All the line/len caps are unchanged.
// =============================================================================
#include "interpreter.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "duckcfg.h"
#include "interp_hal.h"

static const char *TAG = "interp";

// Lines a single tick() may execute before yielding. lily used 200; keeping it
// means one tick can never monopolise the CPU for more than a script fragment.
#define TICK_STEP_BUDGET 200

// Heap left untouched after a payload line table is allocated. The C5 has no
// PSRAM, so the table competes with WiFi, lwIP, httpd and the SD driver for
// internal SRAM; refusing a payload that would drop free heap below this
// reserve keeps the SoftAP and C2 API alive. Payloads over the budget are
// rejected outright (runPayload -> false -> C2 404) rather than truncated,
// because a half-parsed script would be subtly wrong.
#define DUCK_MIN_FREE_HEAP_AFTER (64 * 1024)

// Upper bound on a single blocking wait inside the interpreter. Long DELAYs are
// broken into slices of this size so a stop request is honoured within ~20 ms
// instead of after the whole delay (the C5 has no HID to type with, but a
// 30-second payload must still stop when the operator asks it to).
#define SLEEP_SLICE_MS 20

// ── Progress publishing (layer 3c) ──────────────────────────────────────────
// Defined with the C API at the bottom of the file; declared here because
// Interpreter::tick() (above that point) publishes through them. The snapshot
// itself is the only thing the LCD dashboard is allowed to read - it never
// touches the line table.
static void progress_publish(int pc, int lineCount, const char *line);
static void progress_clear(void);

// ─── Keycode table (DuckyScript name -> USB HID scancode) ───────────────────
static const struct { const char *n; uint8_t code; } KEYS[] = {
    {"ENTER",       0x28}, {"RETURN",      0x28},
    {"ESCAPE",      0x29},
    {"BACKSPACE",   0x2a},
    {"DELETE",      0x4c},
    {"INSERT",      0x49},
    {"TAB",         0x2b},
    {"SPACE",       0x2c},
    {"HOME",        0x4a}, {"END",         0x4d},
    {"PAGEUP",      0x4b}, {"PAGEDOWN",    0x4e},
    {"PRINTSCREEN", 0x46},
    {"SCROLLLOCK",  0x47},
    {"PAUSE",       0x48}, {"BREAK",       0x48},
    {"MENU",        0x65}, {"APP",         0x65},
    {"UP",          0x52}, {"UPARROW",     0x52},
    {"DOWN",        0x51}, {"DOWNARROW",   0x51},
    {"LEFT",        0x50}, {"LEFTARROW",   0x50},
    {"RIGHT",       0x4f}, {"RIGHTARROW",  0x4f},
    {"CAPSLOCK",    0x39}, {"NUMLOCK",     0x53},
    {"F1", 0x3a}, {"F2", 0x3b}, {"F3", 0x3c}, {"F4", 0x3d},
    {"F5", 0x3e}, {"F6", 0x3f}, {"F7", 0x40}, {"F8", 0x41},
    {"F9", 0x42}, {"F10", 0x43}, {"F11", 0x44}, {"F12", 0x45},
    {NULL, 0}
};

// ─── Modifier table ─────────────────────────────────────────────────────────
static const struct { const char *n; uint8_t m; } MODS[] = {
    {"CTRL",    DUCK_MOD_LCTRL},  {"CONTROL", DUCK_MOD_LCTRL},
    {"ALT",     DUCK_MOD_LALT},   {"SHIFT",   DUCK_MOD_LSHIFT},
    {"GUI",     DUCK_MOD_LGUI},   {"WINDOWS", DUCK_MOD_LGUI},
    {"COMMAND", DUCK_MOD_LGUI},
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
static uint8_t symbolCodeUS(char c, uint8_t *mod)
{
    *mod = 0;
    switch (c) {
        case '!':  *mod = DUCK_MOD_LSHIFT; return 0x1e;
        case '@':  *mod = DUCK_MOD_LSHIFT; return 0x1f;
        case '#':  *mod = DUCK_MOD_LSHIFT; return 0x20;
        case '$':  *mod = DUCK_MOD_LSHIFT; return 0x21;
        case '%':  *mod = DUCK_MOD_LSHIFT; return 0x22;
        case '^':  *mod = DUCK_MOD_LSHIFT; return 0x23;
        case '&':  *mod = DUCK_MOD_LSHIFT; return 0x24;
        case '*':  *mod = DUCK_MOD_LSHIFT; return 0x25;
        case '(':  *mod = DUCK_MOD_LSHIFT; return 0x26;
        case ')':  *mod = DUCK_MOD_LSHIFT; return 0x27;
        case '-':  return 0x2d;
        case '_':  *mod = DUCK_MOD_LSHIFT; return 0x2d;
        case '=':  return 0x2e;
        case '+':  *mod = DUCK_MOD_LSHIFT; return 0x2e;
        case '[':  return 0x2f;
        case '{':  *mod = DUCK_MOD_LSHIFT; return 0x2f;
        case ']':  return 0x30;
        case '}':  *mod = DUCK_MOD_LSHIFT; return 0x30;
        case '\\': return 0x31;
        case '|':  *mod = DUCK_MOD_LSHIFT; return 0x31;
        case ';':  return 0x33;
        case ':':  *mod = DUCK_MOD_LSHIFT; return 0x33;
        case '\'': return 0x34;
        case '"':  *mod = DUCK_MOD_LSHIFT; return 0x34;
        case '`':  return 0x35;
        case '~':  *mod = DUCK_MOD_LSHIFT; return 0x35;
        case ',':  return 0x36;
        case '<':  *mod = DUCK_MOD_LSHIFT; return 0x36;
        case '.':  return 0x37;
        case '>':  *mod = DUCK_MOD_LSHIFT; return 0x37;
        case '/':  return 0x38;
        case '?':  *mod = DUCK_MOD_LSHIFT; return 0x38;
    }
    return 0;
}

// ─── Layout override tables ─────────────────────────────────────────────────
// Each table maps ASCII chars that differ from US layout to {scancode, mods}.
// Sentinels: ch == 0 marks end of table.
struct LayoutOverride {
    char    ch;
    uint8_t code;
    uint8_t mod;
};

// Polish Programmer's layout — mostly identical to US. Diacritical chars
// (ąćęłńóśźż) require AltGr dead-key sequences, so there are no overrides.
static const LayoutOverride LAYOUT_PL_T[] = {
    {0, 0, 0}
};

// German QWERTZ layout
static const LayoutOverride LAYOUT_DE_T[] = {
    // Y and Z swapped
    {'z', 0x1d, 0},                          // z -> physical y position
    {'Z', 0x1d, DUCK_MOD_LSHIFT},
    {'y', 0x1c, 0},                          // y -> physical z position
    {'Y', 0x1c, DUCK_MOD_LSHIFT},
    // Special characters (ASCII only — the interpreter feeds one byte per
    // char, so multibyte UTF-8 glyphs cannot match a table entry)
    {'@', 0x14, DUCK_MOD_RALT},              // AltGr+Q
    {'#', 0x31, 0},                          // key left of Enter
    {'\\',0x2d, DUCK_MOD_RALT},              // AltGr+ß
    {'[', 0x2f, DUCK_MOD_RALT},              // AltGr+8
    {']', 0x30, DUCK_MOD_RALT},              // AltGr+9
    {'{', 0x2f, DUCK_MOD_RALT},              // AltGr+7
    {'}', 0x30, DUCK_MOD_RALT},              // AltGr+0
    {'~', 0x30, DUCK_MOD_RALT},              // AltGr+plus
    {'^', 0x35, 0},                          // dead key position
    {'?', 0x2d, DUCK_MOD_LSHIFT},            // Shift+ß
    {';', 0x36, DUCK_MOD_LSHIFT},            // Shift+,
    {':', 0x37, DUCK_MOD_LSHIFT},            // Shift+.
    {'-', 0x38, 0},
    {'_', 0x38, DUCK_MOD_LSHIFT},
    {'=', 0x27, DUCK_MOD_LSHIFT},            // Shift+0
    {'+', 0x30, 0},
    {'*', 0x30, DUCK_MOD_LSHIFT},
    {'\'',0x31, DUCK_MOD_RALT},              // AltGr+\
    {'"', 0x1f, DUCK_MOD_LSHIFT},            // Shift+2
    {'&', 0x23, DUCK_MOD_LSHIFT},            // Shift+6
    {'/', 0x24, DUCK_MOD_LSHIFT},            // Shift+7
    {'(', 0x25, DUCK_MOD_LSHIFT},            // Shift+8
    {')', 0x26, DUCK_MOD_LSHIFT},            // Shift+9
    {'`', 0x2e, DUCK_MOD_LSHIFT},            // Shift+= (dead key)
    {0, 0, 0}
};

// ─── Layout-aware symbol resolution ─────────────────────────────────────────
static uint8_t lookupLayoutOverride(uint8_t layoutId, char c, uint8_t *mod)
{
    const LayoutOverride *table = NULL;
    switch (layoutId) {
        case DUCK_LAYOUT_PL: table = LAYOUT_PL_T; break;
        case DUCK_LAYOUT_DE: table = LAYOUT_DE_T; break;
        default: return 0;  // US or unknown — no overrides
    }
    for (int i = 0; table[i].ch != 0; i++) {
        if (table[i].ch == c) {
            *mod = table[i].mod;
            return table[i].code;
        }
    }
    return 0;
}

// ─── Named constant lookup for evalExpr ─────────────────────────────────────
static uint16_t namedConstant(const char *name)
{
    if (strcmp(name, "WINDOWS") == 0)  return DUCK_OS_WINDOWS;
    if (strcmp(name, "MACOS") == 0)    return DUCK_OS_MACOS;
    if (strcmp(name, "LINUX") == 0)    return DUCK_OS_LINUX;
    if (strcmp(name, "CHROMEOS") == 0) return DUCK_OS_CHROMEOS;
    if (strcmp(name, "UNKNOWN") == 0)  return DUCK_OS_UNKNOWN;
    if (strcmp(name, "TRUE") == 0)     return 1;
    if (strcmp(name, "FALSE") == 0)    return 0;
    if (strcmp(name, "HID") == 0)      return 1;
    if (strcmp(name, "STORAGE") == 0)  return 2;
    return 0xFFFF;  // sentinel: not a known constant
}

// True when c can be part of an identifier (so it must NOT be a word boundary
// — used to suppress DEFINE substitution inside larger words).
static bool isIdentChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

// ─── Symbol table entry types (unchanged from lily) ─────────────────────────
struct DefineEntry { char name[DUCK_MAX_DEFINE_NAME]; char value[DUCK_MAX_LINE_LEN]; };
struct FuncEntry   { char name[DUCK_MAX_FUNC_NAME]; int startLine; int endLine; };
struct VarEntry    { char name[DUCK_MAX_VAR_NAME]; uint16_t value; };
struct BlockEntry  { int type;  // 1=IF, 2=WHILE
                     int startLine;
                     bool active;
                     bool wasTaken; };

// =============================================================================
// Interpreter
// =============================================================================
class Interpreter {
public:
    Interpreter();

    // Loads a script and leaves it in IDLE. Returns false on allocation
    // failure or an empty/oversized payload. Must only be called while the
    // tick task is parked.
    bool loadBuffer(const char *buf, int len);

    // Executes up to TICK_STEP_BUDGET lines. Returns the number of lines that
    // completed (0 means the PC was moved by a control-flow line, or a wait
    // consumed the tick).
    int  tick();

    // Runs tick() until the script leaves RUNNING. Called on the interpreter
    // task; frees the line table on the way out.
    void runLoop();

    void stop();
    void begin();  // IDLE/terminal -> RUNNING, execution context reset

    duck_interp_state_t getState() const { return _state; }

private:
    // ── Storage ─────────────────────────────────────────────────────────────
    // Single heap block, _lineCount rows of DUCK_MAX_LINE_LEN bytes. NULL when
    // no payload is loaded.
    char (*_lines)[DUCK_MAX_LINE_LEN];
    int _lineCount;
    int _pc;

    // Volatile: written by stop() from the httpd task, read by the tick task.
    volatile duck_interp_state_t _state;

    uint16_t _defaultDelay;
    uint16_t _charDelay;
    uint8_t  _jitterMax;
    uint8_t  _layoutId;

    DefineEntry _defines[DUCK_MAX_DEFINES];
    int _defCount;
    FuncEntry _funcs[DUCK_MAX_FUNCTIONS];
    int _funcCount;
    VarEntry _vars[DUCK_MAX_VAR_COUNT];
    int _varCount;
    BlockEntry _blocks[DUCK_MAX_NESTED];
    int _blockDepth;
    int _callStack[DUCK_MAX_NESTED];
    int _callTop;

    // Cooperative DETECT_OS state — the interpreter pauses on the DETECT_OS
    // line for DUCK_OS_DETECT_WINDOW_MS (0 on the C5, so it never pauses).
    bool     _osDetectStarted;
    uint32_t _osDetectBeganMs;

    // ── Helpers ─────────────────────────────────────────────────────────────
    void freeLines();
    void resetRuntime();

    void preProcess();
    int resolveDefines(char *buf);
    bool execLine(int lineno);

    // Result of asking the block machine about a line.
    enum BlockResult {
        BLOCK_NOT_A_KEYWORD,  // not IF/ELSE/WHILE/END_*: fall through to dispatch
        BLOCK_ADVANCE,        // handled: tick() should advance past this line
        BLOCK_PC_MOVED,       // handled: _pc now points at the line to run next
    };
    BlockResult handleBlockStart(const char *line);
    bool pushBlock(int type, int startLine, bool taken);
    bool isBlockActive();
    void skipToNext();
    void skipToMatching();
    uint8_t lookupKey(const char *name);
    uint8_t lookupMod(const char *name);
    void execString(const char *text);
    void execStringln(const char *text);
    void execDelay(int ms);
    bool evalCond(const char *expr);
    uint16_t evalExpr(const char *expr);
    uint16_t getVar(const char *name);
    bool setVar(const char *name, uint16_t val);
    char *trim(char *s);
    bool startsWith(const char *str, const char *prefix);
    void jitter();
    uint8_t symbolForChar(char c, uint8_t *mod);

    // Interruptible sleeps. Both return false when the script was stopped.
    bool sleepMs(int ms);
    bool stillRunning() const { return _state == DUCK_INTERP_RUNNING; }
};

// ── Constructor ─────────────────────────────────────────────────────────────
// No memset(this): the members are initialised explicitly so the volatile state
// field is written the way the type system expects.
Interpreter::Interpreter()
    : _lines(NULL), _lineCount(0), _pc(0), _state(DUCK_INTERP_IDLE),
      _defaultDelay(DUCK_DEFAULT_DELAY), _charDelay(5), _jitterMax(0),
      _layoutId(DUCK_LAYOUT_US),
      _defCount(0), _funcCount(0), _varCount(0),
      _blockDepth(0), _callTop(0),
      _osDetectStarted(false), _osDetectBeganMs(0)
{
    memset(_defines, 0, sizeof(_defines));
    memset(_funcs, 0, sizeof(_funcs));
    memset(_vars, 0, sizeof(_vars));
    memset(_blocks, 0, sizeof(_blocks));
    memset(_callStack, 0, sizeof(_callStack));
}

// ── Helpers ─────────────────────────────────────────────────────────────────
char *Interpreter::trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    int len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' ||
                       s[len - 1] == ' ' || s[len - 1] == '\t'))
        s[--len] = '\0';
    return s;
}

bool Interpreter::startsWith(const char *str, const char *prefix)
{
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

// Interruptible delay: sleeps in SLEEP_SLICE_MS chunks and gives up as soon as
// the script stops, so POST /api/stop is honoured mid-DELAY.
bool Interpreter::sleepMs(int ms)
{
    if (ms <= 0) {
        vTaskDelay(1);  // always yield, even for 0 ms
        return stillRunning();
    }
    while (ms > 0) {
        if (!stillRunning()) return false;
        int slice = (ms > SLEEP_SLICE_MS) ? SLEEP_SLICE_MS : ms;
        vTaskDelay(pdMS_TO_TICKS(slice) == 0 ? 1 : pdMS_TO_TICKS(slice));
        ms -= slice;
    }
    return stillRunning();
}

void Interpreter::jitter()
{
    if (_jitterMax > 0) {
        sleepMs((int)(esp_random() % _jitterMax));
    }
}

// Layout-aware character-to-scancode
uint8_t Interpreter::symbolForChar(char c, uint8_t *mod)
{
    uint8_t code = lookupLayoutOverride(_layoutId, c, mod);
    if (code) return code;
    return symbolCodeUS(c, mod);
}

uint8_t Interpreter::lookupKey(const char *name)
{
    for (int i = 0; KEYS[i].n; i++)
        if (strcmp(KEYS[i].n, name) == 0) return KEYS[i].code;
    return 0;
}

uint8_t Interpreter::lookupMod(const char *name)
{
    for (int i = 0; MODS[i].n; i++)
        if (strcmp(MODS[i].n, name) == 0) return MODS[i].m;
    return 0;
}

// ── Line table lifetime ─────────────────────────────────────────────────────
void Interpreter::freeLines()
{
    free(_lines);
    _lines = NULL;
    _lineCount = 0;
    _pc = 0;
}

// Reset everything that describes a *run* (PC, blocks, vars, timing knobs).
// The parsed line table and the DEFINE/FUNCTION tables survive, which is what
// RESTART_PAYLOAD relies on.
void Interpreter::resetRuntime()
{
    _pc = 0;
    _blockDepth = 0;
    _callTop = 0;
    _varCount = 0;
    _jitterMax = 0;
    _layoutId = DUCK_LAYOUT_US;
    _defaultDelay = DUCK_DEFAULT_DELAY;
    _charDelay = 5;
    _osDetectStarted = false;
}

// ── Load from buffer ────────────────────────────────────────────────────────
bool Interpreter::loadBuffer(const char *buf, int len)
{
    freeLines();
    // Any previously published progress refers to the table just freed.
    progress_clear();
    memset(_defines, 0, sizeof(_defines));
    memset(_funcs, 0, sizeof(_funcs));
    memset(_vars, 0, sizeof(_vars));
    memset(_blocks, 0, sizeof(_blocks));
    memset(_callStack, 0, sizeof(_callStack));
    _defCount = 0;
    _funcCount = 0;
    resetRuntime();
    _state = DUCK_INTERP_IDLE;

    if (!buf || len < 0) return false;

    // Pass 1: how many lines does this payload have (capped like lily)?
    int count = 0;
    for (int off = 0; off < len && count < DUCK_MAX_LINES; ) {
        count++;
        while (off < len && buf[off] != '\n' && buf[off] != '\r') off++;
        while (off < len && (buf[off] == '\n' || buf[off] == '\r')) off++;
    }
    if (count == 0) {
        // Empty payload: lily loads it and the run completes immediately.
        _lines = NULL;
        _lineCount = 0;
        _state = DUCK_INTERP_IDLE;
        return true;
    }
    if (count == DUCK_MAX_LINES) {
        ESP_LOGW(TAG, "payload exceeds %d lines, truncating", DUCK_MAX_LINES);
    }

    // One allocation for the whole table: _lineCount * 256 bytes. Deliberately
    // not 512 * 256 (lily's fixed array) — see the file header.
    size_t bytes = (size_t)count * DUCK_MAX_LINE_LEN;

    // The C5 has no PSRAM, so this comes out of the same internal SRAM that
    // WiFi, lwIP, httpd and the SD driver run from. Refuse a payload whose
    // table would leave less than DUCK_MIN_FREE_HEAP_AFTER free, so a huge
    // script cannot starve the SoftAP and take the C2 API down with it.
    size_t freeHeap = esp_get_free_heap_size();
    if (freeHeap < bytes + DUCK_MIN_FREE_HEAP_AFTER) {
        ESP_LOGE(TAG, "payload too large: %u lines need %u bytes, "
                      "only %u bytes of heap are free (need %u spare)",
                 (unsigned)count, (unsigned)bytes, (unsigned)freeHeap,
                 (unsigned)DUCK_MIN_FREE_HEAP_AFTER);
        _lineCount = 0;
        return false;
    }

    _lines = (char (*)[DUCK_MAX_LINE_LEN])malloc(bytes);
    if (!_lines) {
        ESP_LOGE(TAG, "no memory for %u payload lines (%u bytes)",
                 (unsigned)count, (unsigned)bytes);
        _lineCount = 0;
        return false;
    }

    // Pass 2: fill the table, splitting on CR/LF like lily.
    int off = 0;
    while (off < len && _lineCount < count) {
        int eol = off;
        while (eol < len && buf[eol] != '\n' && buf[eol] != '\r') eol++;
        int n = eol - off;
        if (n >= DUCK_MAX_LINE_LEN) n = DUCK_MAX_LINE_LEN - 1;
        memcpy(_lines[_lineCount], buf + off, n);
        _lines[_lineCount][n] = '\0';
        _lineCount++;
        while (eol < len && (buf[eol] == '\n' || buf[eol] == '\r')) eol++;
        off = eol;
    }

    preProcess();
    ESP_LOGI(TAG, "loaded %d lines (%u bytes of line table)",
             _lineCount, (unsigned)bytes);
    return true;
}

// ── Pre-process: expand DEFINE, pre-scan FUNCTION/END_FUNCTION ───────────────
void Interpreter::preProcess()
{
    // Phase 1: collect DEFINE and FUNCTION declarations.
    for (int i = 0; i < _lineCount; i++) {
        char *line = trim(_lines[i]);

        if (startsWith(line, "DEFINE")) {
            // DEFINE #NAME value   or   DEFINE NAME value
            char *p = line + 6;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '#') p++;  // optional leading #
            char *nameStart = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            int nlen = p - nameStart;
            char *valStart = p;
            while (*valStart == ' ' || *valStart == '\t') valStart++;

            if (_defCount < DUCK_MAX_DEFINES && nlen > 0 &&
                nlen < DUCK_MAX_DEFINE_NAME) {
                DefineEntry *de = &_defines[_defCount];
                strncpy(de->name, nameStart, nlen);
                de->name[nlen] = '\0';
                strncpy(de->value, valStart, DUCK_MAX_LINE_LEN - 1);
                de->value[DUCK_MAX_LINE_LEN - 1] = '\0';
                _defCount++;
            }
            _lines[i][0] = '\0';  // remove DEFINE line
            continue;
        }

        if (startsWith(line, "FUNCTION")) {
            char *p = line + 8;
            while (*p == ' ') p++;
            char *endParen = strchr(p, '(');
            if (endParen && _funcCount < DUCK_MAX_FUNCTIONS) {
                FuncEntry *fe = &_funcs[_funcCount];
                int flen = endParen - p;
                if (flen >= DUCK_MAX_FUNC_NAME) flen = DUCK_MAX_FUNC_NAME - 1;
                strncpy(fe->name, p, flen);
                fe->name[flen] = '\0';
                fe->startLine = i + 1;
                fe->endLine = i;  // filled in on END_FUNCTION scan
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
    for (int i = 0; i < _lineCount; i++) {
        if (!_lines[i][0]) continue;
        resolveDefines(_lines[i]);
    }
}

int Interpreter::resolveDefines(char *src)
{
    char buf[DUCK_MAX_LINE_LEN * 2] = {0};
    int wpos = 0;
    const char *p = src;
    const char *origin = src;

    while (*p && wpos < (int)sizeof(buf) - 1) {
        bool matched = false;
        // Only substitute at a word boundary: start of line or preceded by a
        // non-identifier character. Prevents FOO inside FOOLISH being
        // rewritten.
        bool atBoundary = (p == origin) || !isIdentChar(*(p - 1));
        if (atBoundary) {
            for (int d = 0; d < _defCount; d++) {
                int dlen = strlen(_defines[d].name);
                if (dlen == 0) continue;
                if (strncmp(p, _defines[d].name, dlen) == 0 &&
                    !isIdentChar(p[dlen])) {
                    const char *val = _defines[d].value;
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
    if (copy > DUCK_MAX_LINE_LEN - 1) copy = DUCK_MAX_LINE_LEN - 1;
    memcpy(src, buf, copy);
    src[copy] = '\0';
    return copy;
}

// ── Run / tick / stop / state ───────────────────────────────────────────────
void Interpreter::begin()
{
    resetRuntime();
    _state = DUCK_INTERP_RUNNING;
    ESP_LOGI(TAG, "run (%d lines)", _lineCount);
}

void Interpreter::stop()
{
    // Idempotent: also safe when nothing is running, and called from the httpd
    // task. The tick task notices either at the next line boundary or inside
    // sleepMs()/execString(), whichever comes first.
    bool wasRunning = (_state == DUCK_INTERP_RUNNING);
    _state = DUCK_INTERP_STOPPED;
    duckhal_release();
    if (wasRunning) ESP_LOGI(TAG, "stopped at line %d", _pc);
}

int Interpreter::tick()
{
    if (_state != DUCK_INTERP_RUNNING) return 0;

    int steps = 0;
    while (_state == DUCK_INTERP_RUNNING && _pc < _lineCount &&
           steps < TICK_STEP_BUDGET) {
        if (!_lines[_pc][0]) { _pc++; steps++; continue; }
        bool advanced = execLine(_pc);
        if (advanced) {
            _pc++;
        } else {
            // execLine kept _pc where it set it. Three cases land here:
            //   - CALL / RESTART_PAYLOAD jumped; target is at _pc.
            //   - IF/ELSE_IF/ELSE skipped to a new branch line; that line is
            //     at _pc and should execute on the next tick.
            //   - Cooperative pause (e.g. DETECT_OS) is waiting on a timer.
            break;
        }
        steps++;
    }

    // Publish progress for the LCD (layer 3c). Done here, on the task that owns
    // the line table, so no other task ever dereferences _lines.
    progress_publish(_pc, _lineCount,
                     (_pc >= 0 && _pc < _lineCount) ? _lines[_pc] : "");

    if (_pc >= _lineCount && _state == DUCK_INTERP_RUNNING) {
        _state = DUCK_INTERP_COMPLETE;
        ESP_LOGI(TAG, "done");
    }
    return steps;
}

void Interpreter::runLoop()
{
    while (_state == DUCK_INTERP_RUNNING) {
        int steps = tick();
        if (_state != DUCK_INTERP_RUNNING) break;
        // Yield between ticks. A tick that exhausted its budget, or one that
        // made no progress (block jump / pause), gets a real tick of sleep so
        // a delay-free WHILE can never monopolise the CPU that WiFi needs.
        if (steps == 0 || steps >= TICK_STEP_BUDGET) {
            vTaskDelay(1);
        } else {
            taskYIELD();
        }
    }
    ESP_LOGI(TAG, "run loop exit, state=%d", (int)_state);
    freeLines();  // reclaim the line table as soon as the payload is over
    progress_clear();  // the dashboard must stop referring to freed lines
}

// ── executeLine ─────────────────────────────────────────────────────────────
bool Interpreter::execLine(int lineno)
{
    char *raw = _lines[lineno];
    char *text = trim(raw);

    // Skip empty / comment
    if (!text[0]) return true;
    if (startsWith(text, "REM") &&
        (text[3] == '\0' || text[3] == ' ' || text[3] == '\t')) return true;

    // ── Block handling ───────────────────────────────────────────────────
    // The block machine has three outcomes, and conflating the last two is
    // exactly the bug this port fixes in the reference (see the note on
    // handleBlockStart). Only ADvance and "not a keyword" may fall through.
    switch (handleBlockStart(text)) {
        case BLOCK_ADVANCE:      return true;
        case BLOCK_PC_MOVED:     return false;
        case BLOCK_NOT_A_KEYWORD: break;
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
        const char *p = startsWith(text, "JITTER_MAX ") ? text + 11 : text + 7;
        while (*p == ' ') p++;
        _jitterMax = atoi(p);
        ESP_LOGI(TAG, "jitter 0-%d ms", _jitterMax);
        return true;
    }

    // ── Security: LAYOUT — keyboard layout selection ─────────────────────
    if (startsWith(text, "LAYOUT ")) {
        const char *p = text + 7;
        while (*p == ' ') p++;
        if (strcmp(p, "US") == 0)      _layoutId = DUCK_LAYOUT_US;
        else if (strcmp(p, "PL") == 0) _layoutId = DUCK_LAYOUT_PL;
        else if (strcmp(p, "DE") == 0) _layoutId = DUCK_LAYOUT_DE;
        else ESP_LOGW(TAG, "unknown layout '%s' (staying US)", p);
        ESP_LOGI(TAG, "layout set: %s (id=%d)", p, _layoutId);
        return true;
    }

    // ── Security: DETECT_OS — set $_OS ───────────────────────────────────
    // Same cooperative pause as lily, driven by a millisecond clock rather than
    // millis(). On the C5 DUCK_OS_DETECT_WINDOW_MS is 0 because there is no
    // USB-OTG to observe, so the line resolves to OS_UNKNOWN in the same tick.
    if (strcmp(text, "DETECT_OS") == 0) {
        if (!_osDetectStarted) {
            ESP_LOGI(TAG, "OS detection (no HID on C5: will report unknown)");
            _osDetectStarted = true;
            _osDetectBeganMs = (uint32_t)(esp_timer_get_time() / 1000);
        }
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        // A zero window means "resolve now": no HID exists to observe, so
        // there is nothing to wait for. (The reference's equivalent test would
        // be elapsed < 0, i.e. never true, which is why the check is spelled
        // out instead of copied.)
        if (DUCK_OS_DETECT_WINDOW_MS > 0 &&
            (uint32_t)(now - _osDetectBeganMs) < DUCK_OS_DETECT_WINDOW_MS) {
            return false;  // still detecting; keep PC where it is
        }
        uint8_t osId  = duckhal_os_detect_result();
        uint8_t count = duckhal_os_detect_count();
        setVar("_OS", osId);
        setVar("_HOST_CONFIGURATION_REQUEST_COUNT", count);
        ESP_LOGI(TAG, "$_OS=%d, $_HOST_CONFIGURATION_REQUEST_COUNT=%d",
                 osId, count);
        _osDetectStarted = false;
        return true;
    }

    // ── Security: EXFIL_START — begin keystroke capture ──────────────────
    if (strcmp(text, "EXFIL_START") == 0 || strcmp(text, "EXFILSTART") == 0) {
        duckhal_capture_begin();
        return true;
    }

    // ── Security: EXFIL_STOP — end capture ───────────────────────────────
    if (strcmp(text, "EXFIL_STOP") == 0 || strcmp(text, "EXFILSTOP") == 0) {
        duckhal_capture_end();
        return true;
    }

    // ── Security: ATTACKMODE — no USB on this board ──────────────────────
    // HID cannot be offered and MSC is not plumbed (the SD card stays a normal
    // FATFS mount), so every mode is ignored after logging it. Never fails the
    // script: an ATTACKMODE payload still types nothing but still completes.
    // Matches lily, which also accepts the bare keyword with no arguments.
    if (startsWith(text, "ATTACKMODE")) {
        const char *p = text + 10;
        while (*p == ' ') p++;
        bool wantHID     = (strstr(p, "HID") != NULL);
        bool wantStorage = (strstr(p, "STORAGE") != NULL);
        if (wantHID) {
            ESP_LOGW(TAG, "ATTACKMODE HID: unavailable on C5 (no USB-OTG)");
        }
        if (wantStorage && !duckhal_msc_enable()) {
            ESP_LOGW(TAG, "ATTACKMODE STORAGE: unavailable on C5 (no USB)");
        }
        if (!wantHID && !wantStorage) {
            ESP_LOGW(TAG, "ATTACKMODE '%s': unknown mode, ignoring", p);
        }
        return true;
    }

    // ── Tier 2: VAR ──────────────────────────────────────────────────────
    if (startsWith(text, "VAR ")) {
        // VAR $x = 42
        char *p = text + 4;
        while (*p == ' ') p++;
        if (*p == '$') p++;
        char vname[DUCK_MAX_VAR_NAME];
        int vn = 0;
        while (*p && *p != '=' && vn < DUCK_MAX_VAR_NAME - 1 && *p != ' ')
            vname[vn++] = *p++;
        vname[vn] = '\0';
        while (*p != '=' && *p) p++;  // skip to =
        if (*p == '=') {
            p++;
            while (*p == ' ') p++;
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
        if (m) duckhal_press(m, DUCK_HK_NONE);
        duckhal_release();
        return true;
    }

    // ── Tier 2: CALL ─────────────────────────────────────────────────────
    if (startsWith(text, "CALL ")) {
        const char *fn = text + 5;
        while (*fn == ' ') fn++;
        for (int i = 0; i < _funcCount; i++) {
            if (strcmp(_funcs[i].name, fn) == 0 && _callTop < DUCK_MAX_NESTED) {
                _callStack[_callTop++] = _pc;  // save return
                _pc = _funcs[i].startLine;     // jump
                return false;                  // manual advance
            }
        }
        return true;  // unknown function, skip
    }

    // ── Tier 2: RESTART_PAYLOAD ──────────────────────────────────────────
    if (startsWith(text, "RESTART_PAYLOAD")) {
        // Restart with a fresh execution context. Symbol tables (_defines,
        // _funcs) and parsed lines stay; runtime state resets.
        resetRuntime();
        _state = DUCK_INTERP_RUNNING;  // a RESTART must not leave a stale terminal
        return false;
    }

    // ── Tier 2: STOP_PAYLOAD ─────────────────────────────────────────────
    if (startsWith(text, "STOP_PAYLOAD")) {
        _state = DUCK_INTERP_STOPPED;
        return true;
    }

    // ── KEY command: bare keyword like ENTER, TAB (no args) ───────────────
    uint8_t key = lookupKey(text);
    if (key) {
        duckhal_press(DUCK_HK_NONE, key);
        duckhal_release();
        return true;
    }

    // ── Modifier combo: CTRL ALT DELETE, GUI r, etc. ─────────────────────
    uint8_t mods = 0;
    const char *afterMods = text;
    {
        const char *p = text;
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
            duckhal_press(mods, key);
            duckhal_release();
            return true;
        }
        // Single char: e.g. "GUI r"
        char c = afterMods[0];
        uint8_t cm = 0;
        uint8_t cc = symbolForChar(c, &cm);
        if (!cc) {
            cc = letterCode(c);
            if (cc && c >= 'A' && c <= 'Z') cm |= DUCK_MOD_LSHIFT;
        }
        if (!cc) cc = digitCode(c);
        if (cc) {
            duckhal_press(mods | cm, cc);
            duckhal_release();
            return true;
        }
    }

    // JUST a modifier with nothing after: press and release it alone
    if (mods && !afterMods[0]) {
        duckhal_press(mods, DUCK_HK_NONE);
        duckhal_release();
        return true;
    }

    ESP_LOGW(TAG, "unknown command: %s", text);
    return true;  // skip unknown
}

// ── Key / String execution ──────────────────────────────────────────────────
void Interpreter::execString(const char *text)
{
    uint8_t mod;
    uint8_t code;
    for (int i = 0; text[i]; i++) {
        // Honour a stop request between characters: a long STRING must not
        // keep the interpreter busy after POST /api/stop.
        if (!stillRunning()) return;
        char c = text[i];
        mod = 0;
        code = symbolForChar(c, &mod);
        if (!code) {
            code = letterCode(c);
            // letterCode() returns the same scancode for 'a' and 'A'; OR-in
            // SHIFT for ASCII uppercase so STRING types case-correctly.
            if (code && c >= 'A' && c <= 'Z') mod |= DUCK_MOD_LSHIFT;
        }
        if (!code) code = digitCode(c);
        if (code) duckhal_press(mod, code);
        duckhal_release();
        jitter();
        if (_charDelay) sleepMs(_charDelay);
    }
}

void Interpreter::execStringln(const char *text)
{
    execString(text);
    duckhal_press(DUCK_HK_NONE, 0x28);  // ENTER
    duckhal_release();
}

void Interpreter::execDelay(int ms)
{
    if (ms < DUCK_MIN_DELAY) ms = DUCK_MIN_DELAY;
    sleepMs(ms);
}

// ── Block handling ──────────────────────────────────────────────────────────
// Pushes a new IF/WHILE block, or sets ERROR on overflow so the matching END_*
// cannot silently mis-pair against an unrelated outer block.
bool Interpreter::pushBlock(int type, int startLine, bool taken)
{
    if (_blockDepth >= DUCK_MAX_NESTED) {
        ESP_LOGE(TAG, "block nesting overflow at line %d (max %d)",
                 startLine, DUCK_MAX_NESTED);
        _state = DUCK_INTERP_ERROR;
        return false;
    }
    _blocks[_blockDepth].type      = type;
    _blocks[_blockDepth].startLine = startLine;
    _blocks[_blockDepth].wasTaken  = taken;
    _blocks[_blockDepth].active    = taken;
    _blockDepth++;
    return true;
}

// Block keywords move the PC in ways tick() must not second-guess, so the
// outcome is explicit rather than a bool:
//
//   BLOCK_NOT_A_KEYWORD - this line is a normal command; go run it.
//   BLOCK_ADVANCE       - handled; tick() should advance past this line.
//   BLOCK_PC_MOVED      - handled; _pc already points at the line that must
//                         execute next (it has NOT run yet).
//
// The distinction is load-bearing. lily_ducky collapses it into a bool where
// "false" means both "not a block keyword" and "PC moved", and execLine() then
// treats a moved PC as a normal command line. On a real payload that means:
//   * IF <false> skips its ELSE_IF line without evaluating it, so the first
//     ELSE_IF branch can never be taken;
//   * the landed-on END_IF is likewise never dispatched, so a taken IF whose
//     ELSE was skipped leaks its block (depth grows, nesting eventually
//     overflows);
//   * WHILE <false> advances past END_WHILE without popping the block, leaving
//     an inactive block on the stack that suppresses the REST OF THE SCRIPT.
// All three are reproducible against the reference (see the port notes). This
// port dispatches the landed-on line, which is what the reference clearly
// intended (its skipToMatching() comment says "land here so pop fires").
Interpreter::BlockResult Interpreter::handleBlockStart(const char *line)
{
    if (startsWith(line, "IF ")) {
        const char *p = line + 3;
        while (*p == ' ') p++;
        bool taken = evalCond(p);
        pushBlock(1, _pc, taken);
        if (!taken) {
            skipToNext();
            return BLOCK_PC_MOVED;  // dispatch the branch / END_IF we landed on
        }
        return BLOCK_ADVANCE;       // step into body

    } else if (startsWith(line, "ELSE_IF ")) {
        if (_blockDepth <= 0) return BLOCK_ADVANCE;  // stray ELSE_IF: ignore
        if (_blocks[_blockDepth - 1].wasTaken) {
            skipToMatching();
            return BLOCK_PC_MOVED;  // land on END_IF so it pops the block
        }
        const char *p = line + 8;
        while (*p == ' ') p++;
        if (evalCond(p)) {
            _blocks[_blockDepth - 1].wasTaken = true;
            _blocks[_blockDepth - 1].active = true;
            return BLOCK_ADVANCE;   // step into this branch's body
        }
        skipToNext();
        return BLOCK_PC_MOVED;      // try the next branch

    } else if (strcmp(line, "ELSE") == 0) {
        if (_blockDepth <= 0) return BLOCK_ADVANCE;  // stray ELSE: ignore
        if (_blocks[_blockDepth - 1].wasTaken) {
            skipToMatching();
            return BLOCK_PC_MOVED;  // land on END_IF so it pops the block
        }
        _blocks[_blockDepth - 1].wasTaken = true;
        _blocks[_blockDepth - 1].active = true;
        return BLOCK_ADVANCE;

    } else if (startsWith(line, "WHILE ")) {
        const char *p = line + 6;
        while (*p == ' ') p++;
        bool cond = evalCond(p);
        pushBlock(2, _pc, cond);
        if (!cond) {
            skipToMatching();
            return BLOCK_PC_MOVED;  // land on END_WHILE so it pops the block
        }
        return BLOCK_ADVANCE;

    } else if (strcmp(line, "END_IF") == 0 || strcmp(line, "END_WHILE") == 0 ||
               strcmp(line, "END_FUNCTION") == 0) {
        bool isEndIf    = (strcmp(line, "END_IF") == 0);
        bool isEndWhile = (strcmp(line, "END_WHILE") == 0);

        // Pop only a block of the matching kind. A mismatched END_* in a
        // malformed payload then fails loudly instead of tearing down an
        // unrelated outer block.
        if (_blockDepth > 0) {
            BlockEntry &b = _blocks[_blockDepth - 1];
            bool matches = isEndIf    ? (b.type == 1)
                         : isEndWhile ? (b.type == 2)
                         : true;  // END_FUNCTION closes whatever is open
            if (matches) {
                bool wasWhile = (b.type == 2);
                int  whileStart = b.startLine;
                _blockDepth--;

                if (wasWhile &&
                    (_blockDepth == 0 || _blocks[_blockDepth - 1].active)) {
                    char *wl = _lines[whileStart];
                    const char *p = wl + 6;  // skip "WHILE "
                    while (*p == ' ') p++;
                    if (evalCond(p)) {
                        pushBlock(2, whileStart, true);
                        _pc = whileStart;  // tick advances into the body
                        return BLOCK_ADVANCE;
                    }
                }
            } else {
                ESP_LOGW(TAG, "%s does not match the open block (type %d)",
                         line, b.type);
            }
        }

        if (strcmp(line, "END_FUNCTION") == 0 && _callTop > 0) {
            _pc = _callStack[--_callTop];  // tick advances past the CALL
            return BLOCK_ADVANCE;
        }
        return BLOCK_ADVANCE;
    }

    return BLOCK_NOT_A_KEYWORD;
}

bool Interpreter::isBlockActive()
{
    for (int i = 0; i < _blockDepth; i++)
        if (!_blocks[i].active) return false;
    return true;
}

// Skip past the rest of the current IF/WHILE body, landing ON the matching
// END_*. Tracks nested IF/WHILE so inner blocks don't fool the depth count.
void Interpreter::skipToMatching()
{
    int depth = 0;
    while (_pc + 1 < _lineCount) {
        _pc++;
        char *l = trim(_lines[_pc]);
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
// END_IF) at the current nesting depth. Lands ON that line so the handler runs.
void Interpreter::skipToNext()
{
    int depth = 0;
    while (_pc + 1 < _lineCount) {
        _pc++;
        char *l = trim(_lines[_pc]);
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
uint16_t Interpreter::getVar(const char *name)
{
    for (int i = 0; i < _varCount; i++)
        if (strcmp(_vars[i].name, name) == 0) return _vars[i].value;
    return 0;  // undefined vars = 0
}

bool Interpreter::setVar(const char *name, uint16_t val)
{
    if (val > DUCK_MAX_VAR) val = DUCK_MAX_VAR;
    for (int i = 0; i < _varCount; i++) {
        if (strcmp(_vars[i].name, name) == 0) { _vars[i].value = val; return true; }
    }
    if (_varCount < DUCK_MAX_VAR_COUNT) {
        strncpy(_vars[_varCount].name, name, DUCK_MAX_VAR_NAME - 1);
        _vars[_varCount].name[DUCK_MAX_VAR_NAME - 1] = '\0';
        _vars[_varCount].value = val;
        _varCount++;
        return true;
    }
    return false;
}

// ── Expression evaluator (simplified: $x + 5, $y * 2 — as in lily) ─────────
uint16_t Interpreter::evalExpr(const char *expr)
{
    const char *p = expr;
    while (*p == ' ') p++;

    if (*p == '$') {
        p++;
        char name[DUCK_MAX_VAR_NAME];
        int n = 0;
        while (*p && *p != ' ' && *p != '\0' && n < DUCK_MAX_VAR_NAME - 1)
            name[n++] = *p++;
        name[n] = '\0';
        return getVar(name);
    }

    // Named constants (WINDOWS, MACOS, LINUX, TRUE, FALSE, ...)
    char word[32];
    int wn = 0;
    const char *ws = p;
    while (*ws && *ws != ' ' && *ws != '\0' && wn < 31)
        word[wn++] = *ws++;
    word[wn] = '\0';
    uint16_t nc = namedConstant(word);
    if (nc != 0xFFFF) return nc;

    return atoi(p);
}

// ── Condition evaluator ────────────────────────────────────────────────────
bool Interpreter::evalCond(const char *expr)
{
    // Supports: $x == 5, $x != 0, $x > 3, $x < 10, $x >= 5, $x <= 3
    // Also: $_OS == WINDOWS (named constants resolved in evalExpr)
    char left[64]  = {0};
    char right[64] = {0};
    char op[4]     = {0};

    while (*expr == ' ') expr++;
    char tmp[128];
    strncpy(tmp, expr, 127);
    tmp[127] = '\0';

    // Strip trailing THEN/DO
    char *thenPos = strstr(tmp, " THEN");
    if (!thenPos) thenPos = strstr(tmp, " DO");
    if (thenPos) *thenPos = '\0';
    // Re-trim
    int len = strlen(tmp);
    while (len > 0 && (tmp[len - 1] == ' ' || tmp[len - 1] == '\t' ||
                       tmp[len - 1] == ')' || tmp[len - 1] == '\n'))
        tmp[--len] = '\0';
    // Strip leading parenthesis
    const char *exprStart = tmp;
    while (*exprStart == ' ' || *exprStart == '(') exprStart++;
    strncpy(tmp, exprStart, 127);
    tmp[127] = '\0';

    // Find operator (longest first, so >= beats >)
    const char *ops[] = {">=", "<=", "!=", "==", ">", "<"};
    int opLen[] =      { 2,   2,   2,   2,   1,   1 };

    for (int i = 0; i < 6; i++) {
        const char *found = strstr(tmp, ops[i]);
        if (found) {
            int llen = found - tmp;
            while (llen > 0 && tmp[llen - 1] == ' ') llen--;
            // Clamp so over-long operands degrade to truncation instead of
            // being silently skipped.
            if (llen > (int)sizeof(left) - 1) llen = sizeof(left) - 1;
            memcpy(left, tmp, llen);
            left[llen] = '\0';

            strncpy(op, ops[i], 3);

            const char *rp = found + opLen[i];
            while (*rp == ' ') rp++;
            int rlen = strlen(rp);
            while (rlen > 0 && rp[rlen - 1] == ' ') rlen--;
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

// =============================================================================
// C API — one interpreter instance, one tick task
// =============================================================================
// ── Progress snapshot (layer 3c) ────────────────────────────────────────────
// The dashboard needs (pc, lineCount, current line) but must never touch the
// line table, which the interpreter task frees when a run ends. The tick task
// publishes a copy here; readers copy it out under a spinlock. Both critical
// sections are a couple of word/byte copies, so a portMUX is the right tool
// (no scheduler involvement, safe from the LCD caller's task).
#define DUCK_PROGRESS_LINE 24

static portMUX_TYPE s_progMux = portMUX_INITIALIZER_UNLOCKED;
static int  s_progPc;
static int  s_progLines;
static char s_progLine[DUCK_PROGRESS_LINE];

// Called from the interpreter task only (see tick()).
static void progress_publish(int pc, int lineCount, const char *line)
{
    taskENTER_CRITICAL(&s_progMux);
    s_progPc = pc;
    s_progLines = lineCount;
    if (line) {
        strncpy(s_progLine, line, DUCK_PROGRESS_LINE - 1);
        s_progLine[DUCK_PROGRESS_LINE - 1] = '\0';
    } else {
        s_progLine[0] = '\0';
    }
    taskEXIT_CRITICAL(&s_progMux);
}

// Called from the interpreter task when the payload ends and the table is freed.
static void progress_clear(void)
{
    taskENTER_CRITICAL(&s_progMux);
    s_progPc = 0;
    s_progLines = 0;
    s_progLine[0] = '\0';
    taskEXIT_CRITICAL(&s_progMux);
}

void duck_interp_progress(int *pc, int *lineCount, char *line, size_t lineLen)
{
    taskENTER_CRITICAL(&s_progMux);
    int p = s_progPc;
    int n = s_progLines;
    char buf[DUCK_PROGRESS_LINE];
    memcpy(buf, s_progLine, sizeof(buf));
    taskEXIT_CRITICAL(&s_progMux);

    if (pc) *pc = p;
    if (lineCount) *lineCount = n;
    if (line && lineLen) {
        size_t c = lineLen - 1;
        if (c >= sizeof(buf)) c = sizeof(buf) - 1;
        memcpy(line, buf, c);
        line[c] = '\0';
    }
}

static Interpreter s_interp;

static SemaphoreHandle_t s_runSem;      // start request -> task
static SemaphoreHandle_t s_parkedSem;   // task -> start (task is idle)
static SemaphoreHandle_t s_loadedSem;   // task -> start (parse finished)

// Serialises duck_interp_start()/duck_interp_stop() against each other. httpd
// is single-task by default, but the hooks are a public surface and a second
// client (or a future multi-task httpd) must not be able to interleave two
// starts over the same line table.
static SemaphoreHandle_t s_apiMutex;

// True from the moment a payload is handed to the task until runLoop() has
// returned. duck_interp_start() must wait on this rather than on the reported
// state: the state turns terminal inside tick(), which is a moment *before*
// runLoop() frees the line table, and loading a new payload in that window
// would race the free.
static volatile bool s_taskBusy;

// Payload bytes handed from duck_interp_start() to the interpreter task. The
// script is parsed on that task rather than in the caller, because parsing is
// the deepest call chain (loadBuffer -> preProcess -> resolveDefines, ~800 B)
// and duck_interp_start() runs on the httpd task, whose stack is only 4 KiB.
// Keeping parse + run on one task also means the line table is only ever
// touched by one thread.
static const char *s_pendingBuf;
static size_t s_pendingLen;
static volatile bool s_loadOk;

// Called by the task once it has finished a run and released the line table.
static void interp_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (xSemaphoreTake(s_runSem, portMAX_DELAY) != pdTRUE) continue;

        // Parse, then run, then reclaim - all on this task's stack.
        bool loaded = s_interp.loadBuffer(s_pendingBuf, (int)s_pendingLen);
        s_loadOk = loaded;
        if (loaded) s_interp.begin();
        xSemaphoreGive(s_loadedSem);   // caller waits for the parse result

        if (loaded) s_interp.runLoop();

        // Clear "busy" and signal "parked" only after runLoop() has dropped
        // the line table, so duck_interp_start() can never hand us a new
        // payload mid-run.
        s_taskBusy = false;
        xSemaphoreGive(s_parkedSem);
    }
}

esp_err_t duck_interp_init(void)
{
    static bool s_ready = false;
    if (s_ready) return ESP_OK;

    s_runSem = xSemaphoreCreateBinary();
    s_parkedSem = xSemaphoreCreateBinary();
    s_loadedSem = xSemaphoreCreateBinary();
    s_apiMutex = xSemaphoreCreateMutex();
    if (!s_runSem || !s_parkedSem || !s_loadedSem || !s_apiMutex)
        return ESP_ERR_NO_MEM;

    // 6 KiB: the deepest path is loadBuffer -> preProcess -> resolveDefines
    // (~800 bytes) plus ESP_LOG formatting. No C++ exceptions/RTTI.
    BaseType_t ok = xTaskCreate(interp_task, "duckinterp", 6144, NULL, 5, NULL);
    if (ok != pdPASS) {
        vSemaphoreDelete(s_runSem);
        vSemaphoreDelete(s_parkedSem);
        vSemaphoreDelete(s_loadedSem);
        vSemaphoreDelete(s_apiMutex);
        s_runSem = s_parkedSem = s_loadedSem = s_apiMutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_ready = true;
    ESP_LOGI(TAG, "interpreter task started (HID: none on C5, keys are no-ops)");
    return ESP_OK;
}

duck_interp_state_t duck_interp_state(void)
{
    if (!s_runSem) return DUCK_INTERP_IDLE;
    // Lock-free by design: this is polled by GET /api/status on every request,
    // and a torn read is impossible for a single aligned enum/bool.
    return (duck_interp_state_t)s_interp.getState();
}

bool duck_interp_start(const char *buf, size_t len)
{
    if (!s_runSem) {
        ESP_LOGE(TAG, "start refused: interpreter not initialised");
        return false;
    }
    if (!buf) return false;

    bool started = false;
    if (xSemaphoreTake(s_apiMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGW(TAG, "start refused: another start/stop is in progress");
        return false;
    }

    // A new run pre-empts the old one, like lily's Interpreter::run() which
    // restarts from the top when the state is not RUNNING. If a script is
    // still being executed, ask it to stop and wait for the task to park; a
    // payload that ignores the stop for >1 s is reported as a failure rather
    // than racing two writers over the line table.
    bool previousStopped = true;
    if (s_taskBusy) {
        s_interp.stop();
        previousStopped =
            (xSemaphoreTake(s_parkedSem, pdMS_TO_TICKS(1000)) == pdTRUE);
        if (!previousStopped) {
            ESP_LOGW(TAG, "previous payload did not stop in time, start refused");
        }
    }
    // Drain a stale "parked" token left by an earlier run.
    while (xSemaphoreTake(s_parkedSem, 0) == pdTRUE) { }

    if (previousStopped) {
        // Hand the bytes to the task and wait for its parse verdict. The
        // script has been copied into the line table by then, so the caller's
        // buffer is free to go.
        //
        // The wait is unbounded on purpose. loadBuffer() does no I/O: it is a
        // pair of bounded passes over at most DUCK_MAX_LINES * DUCK_MAX_LINE_LEN
        // bytes (128 KiB worst case, microseconds on the C5), so "forever" is
        // a few milliseconds in practice. A timeout here would be worse than
        // useless: returning while the task is still memcpy'ing from *buf
        // would let the caller free memory the interpreter is reading.
        s_pendingBuf = buf;
        s_pendingLen = len;
        s_loadOk = false;
        s_taskBusy = true;
        xSemaphoreGive(s_runSem);
        (void)xSemaphoreTake(s_loadedSem, portMAX_DELAY);
        started = s_loadOk;
        if (!started) ESP_LOGW(TAG, "payload rejected by loader");
    }

    xSemaphoreGive(s_apiMutex);
    return started;
}

void duck_interp_stop(void)
{
    if (!s_runSem) return;
    // Deliberately no mutex here. stop() only writes the state word, which the
    // tick task polls, so it stays callable (and immediate) even while a start
    // is waiting out its 1 s park timeout.
    s_interp.stop();
}
