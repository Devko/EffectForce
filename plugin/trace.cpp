#include "trace.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>
#include <sys/stat.h>

namespace ef {
namespace {

constexpr long kMaxBytes = 2L << 20;
constexpr long long kCheckMs = 1000;

// One log for every instance (they share the process); UI threads only.
std::mutex mtx;
FILE* logFile = nullptr;
bool on = false;
long long checkedMs = 0;
bool checked = false;

std::string traceDir() {
    const char* d = std::getenv("EF_TRACE_DIR");
    return d && *d ? d : "/tmp";
}

long long steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

bool tracing() {
    std::lock_guard<std::mutex> lk(mtx);
    const long long now = steadyMs();
    if (!checked || now - checkedMs >= kCheckMs) {
        checked = true;
        checkedMs = now;
        struct stat st{};
        on = ::stat((traceDir() + "/effectforce.trace").c_str(), &st) == 0;
        if (!on && logFile) {
            std::fclose(logFile);
            logFile = nullptr;
        }
    }
    return on;
}

void trace(const char* fmt, ...) {
    if (!tracing()) return;
    std::lock_guard<std::mutex> lk(mtx);
    if (!logFile) logFile = std::fopen((traceDir() + "/effectforce.log").c_str(), "a");
    if (!logFile || std::ftell(logFile) > kMaxBytes) return;
    // Wall-clock time, to line up with MPC's own log (journalctl -u acvs).
    const auto t = std::chrono::system_clock::now();
    const std::time_t s = std::chrono::system_clock::to_time_t(t);
    const int ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count() % 1000);
    std::tm tm{};
    localtime_r(&s, &tm);
    std::fprintf(logFile, "%02d:%02d:%02d.%03d ", tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(logFile, fmt, ap);
    va_end(ap);
    std::fputc('\n', logFile);
    std::fflush(logFile);
}

} // namespace ef
