#pragma once
// The probe's log: <dir>/effectforce.log (dir = /tmp, or EF_TRACE_DIR), always on. At 4 MB it
// moves to effectforce.log.1 (replacing an older one) and starts over, so a long session keeps its
// latest 4-8 MB. Lines carry wall-clock time to line up with MPC's own log (journalctl -u acvs).
// File I/O: never from the audio thread (the probe's monitor thread and MPC's UI side only).
#include <string>

namespace ef {

std::string traceDir();
void trace(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

} // namespace ef
