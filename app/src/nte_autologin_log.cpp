#include "nte_autologin_log.hpp"

#include "stream_diagnostics.hpp"

#ifdef __SWITCH__
#include <sys/stat.h>
#endif

#include <chrono>
#include <fstream>
#include <mutex>

namespace opennow
{
namespace
{

constexpr const char* kNteLogPath =
    "sdmc:/switch/SwitchNOW/nte/nte_autologin.log";
std::mutex g_nte_log_mutex;
std::chrono::steady_clock::time_point g_nte_log_started;

void EnsureNteLogDirectory()
{
#ifdef __SWITCH__
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/SwitchNOW", 0777);
    mkdir("sdmc:/switch/SwitchNOW/nte", 0777);
#endif
}

} // namespace

void ResetNteAutoLoginLog(const std::string& session_context)
{
    if (!StreamDiagnosticsEnabled())
        return;
    std::lock_guard<std::mutex> lock(g_nte_log_mutex);
    EnsureNteLogDirectory();
    g_nte_log_started = std::chrono::steady_clock::now();
    std::ofstream stream(kNteLogPath, std::ios::binary | std::ios::trunc);
    if (!stream.is_open())
        return;
    stream << "SwitchNOW NTE auto-login trace\n";
    stream << "Credentials and key codes are intentionally redacted.\n";
    stream << "session=" << session_context << "\n";
    stream << "========================================\n";
}

void AppendNteAutoLoginLog(const std::string& line)
{
    if (!StreamDiagnosticsEnabled())
        return;
    std::lock_guard<std::mutex> lock(g_nte_log_mutex);
    EnsureNteLogDirectory();
    if (g_nte_log_started.time_since_epoch().count() == 0)
        g_nte_log_started = std::chrono::steady_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - g_nte_log_started).count();
    std::ofstream stream(kNteLogPath, std::ios::binary | std::ios::app);
    if (stream.is_open())
        stream << "[+" << elapsed_ms << "ms] " << line << '\n';
}

} // namespace opennow
