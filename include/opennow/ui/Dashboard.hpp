#pragma once

#include <string>
#include <vector>

#include "opennow/core/Log.hpp"
#include "opennow/gfn/Auth.hpp"
#include "opennow/gfn/GfnTypes.hpp"

namespace opennow::ui {

enum class StatusTone {
    Ok,
    Pending,
    Warning,
    Blocked,
};

struct StatusRow {
    std::string label;
    std::string value;
    StatusTone tone = StatusTone::Pending;
};

struct AccountPanel {
    bool signedIn = false;
    AuthUser user;
    std::string provider = "NVIDIA";
    std::string deviceName = "OpenNOW Switch";
    std::string authState = "Ready for off-device login";
    gfn::DeviceAuthorizationInfo deviceAuthorization;
};

struct GameCategoryPanel {
    std::string title;
    std::string subtitle;
    std::uint32_t count = 0;
    StatusTone tone = StatusTone::Pending;
};

struct LogLine {
    LogLevel level = LogLevel::Info;
    LogCategory category = LogCategory::App;
    std::string message;
};

struct DashboardModel {
    AccountPanel account;
    std::vector<GameCategoryPanel> games;
    std::vector<StatusRow> status;
    std::vector<LogLine> logs;
};

class DashboardRenderer {
public:
    explicit DashboardRenderer(std::size_t width = 78);

    std::string render(const DashboardModel& model) const;

private:
    std::string border(char left, char fill, char right) const;
    std::string row(std::string_view text) const;
    std::string keyValue(std::string_view key, std::string_view value) const;
    std::string sectionTitle(std::string_view title) const;
    std::vector<std::string> wrap(std::string_view text, std::size_t width) const;
    std::string statusGlyph(StatusTone tone) const;
    std::string trimTo(std::string value, std::size_t width) const;

    std::size_t width_;
};

DashboardModel buildStartupDashboard();

} // namespace opennow::ui
