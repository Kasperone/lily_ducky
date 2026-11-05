// =============================================================================
// interpreter.h - C entry points for the C5 DuckyScript interpreter (layer 3b)
// =============================================================================
// The DuckyScript parser/executor itself lives in interpreter.cpp (C++, because
// the lily_ducky original is a C++ class and a line-for-line port is the safest
// way to preserve payload semantics). main.c is C, so this header is the
// C-callable face of it: initialise, start a payload, stop it, read its state.
//
// Threading contract:
//   * duck_interp_init()  - once, from app_main. Creates the interpreter task.
//   * duck_interp_start() - may be called from any task (the httpd task in
//                           practice). Blocks briefly while a previous payload
//                           winds down, then returns. Execution itself is
//                           asynchronous: a dedicated FreeRTOS task ticks the
//                           interpreter, so the httpd task is never blocked by
//                           DELAY / STRING / a looping WHILE.
//   * duck_interp_state() - lock-free read of the current state.
//   * duck_interp_stop()  - sets STOPPED immediately; the tick task notices
//                           within ~1 FreeRTOS tick and abandons the script.
// =============================================================================
#ifndef C5_DUCK_INTERPRETER_H
#define C5_DUCK_INTERPRETER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Values are identical to lily_ducky's InterpState and to c2_interp_state_t in
// c2_server.h, so the C2 mapping is a straight 1:1 (done explicitly in main.c
// rather than by cast, so a future reorder of either enum is a compile error
// instead of a silent status mix-up).
typedef enum {
    DUCK_INTERP_IDLE     = 0,
    DUCK_INTERP_RUNNING  = 1,
    DUCK_INTERP_COMPLETE = 2,
    DUCK_INTERP_ERROR    = 3,
    DUCK_INTERP_STOPPED  = 4,
} duck_interp_state_t;

// Creates the interpreter task and its synchronisation primitives. Idempotent.
// ESP_OK on success, ESP_ERR_NO_MEM if the task could not be created.
esp_err_t duck_interp_init(void);

// Current interpreter state. Safe to call before duck_interp_init() (reports
// IDLE) and from any task.
duck_interp_state_t duck_interp_state(void);

// Copies `len` bytes of DuckyScript from `buf` into the interpreter and starts
// execution asynchronously. `buf` is not retained: the caller may free it as
// soon as this returns.
//
// Returns false when the script could not be loaded (bad/oversized/empty
// buffer, out of memory) or when a previous payload refused to stop - the C2
// layer turns that into its 404 "Payload not found".
bool duck_interp_start(const char *buf, size_t len);

// Abort the running script (lily Interpreter::stop()). Non-blocking; safe to
// call when nothing is running. Leaves the state at STOPPED, matching lily.
void duck_interp_stop(void);

// ── Progress readout (layer 3c, for the LCD dashboard) ──────────────────────
// These report where the running payload is. The interpreter task owns the line
// table and frees it when a run ends, so exposing a raw pointer into it would
// be a use-after-free hazard for any other task. Instead the tick task keeps a
// small snapshot (pc, line count, a truncated copy of the current line) that
// these read under a spinlock - at most 24 bytes copied, safe from any task.
//
// `pc` is 0-based; `line` is truncated to 23 chars + NUL and empty when no
// payload is loaded.
void duck_interp_progress(int *pc, int *lineCount, char *line, size_t lineLen);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // C5_DUCK_INTERPRETER_H
