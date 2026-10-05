#include "trace.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>

namespace ef {
namespace {

constexpr long kMaxBytes = 4L << 20;

// One log for every instance (they share the process).
std::mutex mtx;
FILE* logFile = nullptr;

} // namespace

std::string traceDir() {
    const char* d = std::getenv("EF_TRACE_DIR");
    return d && *d ? d : "/tmp";
}

void trace(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(mtx);
    const std::string path = traceDir() + "/effectforce.log";
    if (!logFile) logFile = std::fopen(path.c_str(), "a");
    if (logFile && std::ftell(logFile) > kMaxBytes) {
        std::fclose(logFile);
        std::rename(path.c_str(), (path + ".1").c_str());
        logFile = std::fopen(path.c_str(), "a");
    }
    if (!logFile) return;
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
