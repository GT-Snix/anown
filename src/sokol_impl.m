// The one translation unit that holds the sokol implementations.
// Must be Objective-C on macOS (see sokol_app.h / sokol_gfx.h).
#define SOKOL_IMPL
#define SOKOL_METAL
// anown is a library: the user's program owns main(), and an_window_run()
// calls sapp_run(). Without this, sokol_app defines main() and expects sokol_main().
#define SOKOL_NO_ENTRY
#include "sokol_log.h"
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
