// =============================================================================
// interpreter.h - DuckyScript parser & executor (Tier 1 + Tier 2)
// =============================================================================
#ifndef LILLY_DUCKY_INTERPRETER_H
#define LILLY_DUCKY_INTERPRETER_H

#include <Arduino.h>
#include "config.h"

enum InterpState {
    INTERP_IDLE       = 0,
    INTERP_RUNNING    = 1,
    INTERP_COMPLETE   = 2,
    INTERP_ERROR      = 3,
    INTERP_STOPPED    = 4,
};

#define MAX_LINES       512
#define MAX_LINE_LEN    256
#define MAX_DEFINES     32
#define MAX_FUNC_NAME   32
#define MAX_VAR_NAME    16
#define MAX_DEFINE_NAME 16

struct DefineEntry { char name[MAX_DEFINE_NAME]; char value[MAX_LINE_LEN]; };
struct FuncEntry   { char name[MAX_FUNC_NAME]; int startLine; int endLine; };
struct VarEntry    { char name[MAX_VAR_NAME]; uint16_t value; };
struct BlockEntry  { int type; // 1=IF, 2=WHILE
                     int startLine;
                     bool active;
                     bool wasTaken; };

class Interpreter {
public:
    Interpreter();
    bool loadBuffer(const char* buf, int len);
    void run();
    void tick();
    void stop();
    InterpState getState();

    // Built-in variable injection (called by main after OS detection)
    void setBuiltinVar(const char* name, uint16_t val);

private:
    char _lines[MAX_LINES][MAX_LINE_LEN];
    int _lineCount;
    int _pc;
    InterpState _state;

    uint16_t _defaultDelay;
    uint16_t _charDelay;
    uint8_t  _jitterMax;

    // Keyboard layout: index into layout tables (0=US, 1=PL, 2=DE)
    uint8_t  _layoutId;

    DefineEntry _defines[MAX_DEFINES];
    int _defCount;
    FuncEntry _funcs[CFG_MAX_FUNCTIONS];
    int _funcCount;
    VarEntry _vars[CFG_MAX_VAR_COUNT];
    int _varCount;
    BlockEntry _blocks[CFG_MAX_NESTED];
    int _blockDepth;
    int _callStack[CFG_MAX_NESTED];
    int _callTop;

    // Cooperative DETECT_OS state — the interpreter pauses on the DETECT_OS
    // line for CFG_OS_DETECT_WINDOW_MS so the C2 WebServer keeps ticking.
    bool     _osDetectStarted;
    uint32_t _osDetectBeganMs;

    void preProcess();
    int resolveDefines(char* buf); // returns bytes written
    bool execLine(int lineno);
    bool handleBlockStart(const char* line);
    bool handleBlockEnd(const char* line);
    bool pushBlock(int type, int startLine, bool taken);
    bool isBlockActive();
    int findEndIf(int startLine, const char* terminator);
    void skipToNext();
    void skipToMatching();
    uint8_t lookupKey(const char* name);
    uint8_t lookupMod(const char* name);
    void execKey(const char* name);
    void execCombo(const char* afterMods, uint8_t mods);
    void execString(const char* text);
    void execStringln(const char* text);
    void execDelay(int ms);
    bool evalCond(const char* expr);
    uint16_t evalExpr(const char* expr);
    uint16_t getVar(const char* name);
    bool setVar(const char* name, uint16_t val);
    char* trim(char* s);
    bool startsWith(const char* str, const char* prefix);
    void jitter();

    // Layout-aware symbol lookup
    uint8_t symbolForChar(char c, uint8_t* mod);
};

// keycodes for single-byte send
#define HK_NONE       0x00
#define HK_MOD_LCTRL  0x01
#define HK_MOD_LSHIFT 0x02
#define HK_MOD_LALT   0x04
#define HK_MOD_LGUI   0x08

#endif
