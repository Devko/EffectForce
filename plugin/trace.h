#pragma once
// Device diagnostics (PolyForce's): every value MPC sets and what the plugin made of it, appended
// to <dir>/effectforce.log while <dir>/effectforce.trace exists (dir = /tmp, or EF_TRACE_DIR). Create
// the flag file to start and remove it to stop, with MPC running: it is looked for at most once
// a second. The log stops growing at 2 MB. File I/O: never from the audio thread.

namespace ef {

bool tracing();
void trace(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

} // namespace ef
