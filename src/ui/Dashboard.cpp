#include "opennow/ui/Dashboard.hpp"

#include <algorithm>
#include <sstream>

namespace opennow::ui {

namespace {

std::string repeat(char c, std::size_t count) {
    return std::string(count, c);
}

}

DashboardRenderer::DashboardRenderer(std::size_t width)
    : width_(std::max<std::size_t>(48, width)) {}

std::string DashboardRenderer::render(const DashboardModel& model) const {
    std::ostringstream out;
    out << border('+', '=', '+') << '\n';
    out << row("OpenNOW Switch") << '\n';
    out << row("GeForce NOW native Horizon prototype") << '\n';
    out << border('+', '=', '+') << '\n';

    out << sectionTitle("Account") << '\n';
    out << keyValue("State", model.account.signedIn ? "Signed in" : model.account.authState) << '\n';
    out << keyValue("Provider", model.account.provider) << '\n';
    out << keyValue("Profile", model.account.signedIn ? model.account.user.displayName : "Not signed in") << '\n';
    out << keyValue("Plan", model.account.signedIn ? model.account.user.membershipTier : "Unknown until login") << '\n';
    out << keyValue("Device", model.account.deviceName) << '\n';

    const auto& device = model.account.deviceAuthorization;
    out << sectionTitle("Off-Device Login") << '\n';
    out << keyValue("Flow", "Steam Deck-style device authorization") << '\n';
    out << keyValue("URL", device.verificationUri.empty() ? "Waiting for /device/authorize" : device.verificationUri) << '\n';
    out << keyValue("Code", device.userCode.empty() ? "---- ----" : device.userCode) << '\n';
    out << keyValue("QR target", device.verificationUriComplete.empty() ? "Available after device authorization starts" : device.verificationUriComplete) << '\n';
    out << row("The QR page will encode the complete URL returned by NVIDIA device authorization.") << '\n';

    out << sectionTitle("Games") << '\n';
    for (const auto& game : model.games) {
        std::ostringstream line;
        line << statusGlyph(game.tone) << ' ' << game.title;
        if (game.count > 0) {
            line << " (" << game.count << ')';
        }
        line << " - " << game.subtitle;
        out << row(line.str()) << '\n';
    }

    out << sectionTitle("Runtime") << '\n';
    for (const auto& status : model.status) {
        std::ostringstream line;
        line << statusGlyph(status.tone) << ' ' << status.label << ": " << status.value;
        out << row(line.str()) << '\n';
    }

    out << sectionTitle("Logs") << '\n';
    const auto begin = model.logs.size() > 8 ? model.logs.end() - 8 : model.logs.begin();
    for (auto it = begin; it != model.logs.end(); ++it) {
        out << row(formatLogLine(it->level, it->category, it->message)) << '\n';
    }

    out << border('+', '-', '+') << '\n';
    out << row("Controls: PLUS exit | L/R switch pages later | A start login later") << '\n';
    out << border('+', '-', '+') << '\n';
    return out.str();
}

std::string DashboardRenderer::border(char left, char fill, char right) const {
    return std::string(1, left) + repeat(fill, width_ - 2) + std::string(1, right);
}

std::string DashboardRenderer::row(std::string_view text) const {
    std::ostringstream out;
    const auto inner = width_ - 4;
    const auto wrapped = wrap(text, inner);
    for (std::size_t i = 0; i < wrapped.size(); ++i) {
        if (i != 0) {
            out << '\n';
        }
        const auto line = trimTo(wrapped[i], inner);
        out << "| " << line << repeat(' ', inner - line.size()) << " |";
    }
    return out.str();
}

std::string DashboardRenderer::keyValue(std::string_view key, std::string_view value) const {
    std::ostringstream out;
    out << key << ": " << value;
    return row(out.str());
}

std::string DashboardRenderer::sectionTitle(std::string_view title) const {
    const auto filler = width_ > title.size() + 4 ? width_ - title.size() - 4 : 1;
    std::ostringstream line;
    line << "| " << title << ' ' << repeat('-', filler) << " |";
    return line.str();
}

std::vector<std::string> DashboardRenderer::wrap(std::string_view text, std::size_t width) const {
    std::vector<std::string> lines;
    std::string current;
    std::istringstream words{std::string(text)};
    std::string word;
    while (words >> word) {
        if (word.size() > width) {
            if (!current.empty()) {
                lines.push_back(current);
                current.clear();
            }
            for (std::size_t i = 0; i < word.size(); i += width) {
                lines.push_back(word.substr(i, width));
            }
            continue;
        }
        if (current.empty()) {
            current = word;
        } else if (current.size() + 1 + word.size() <= width) {
            current += ' ';
            current += word;
        } else {
            lines.push_back(current);
            current = word;
        }
    }
    if (!current.empty()) {
        lines.push_back(current);
    }
    if (lines.empty()) {
        lines.push_back("");
    }
    return lines;
}

std::string DashboardRenderer::statusGlyph(StatusTone tone) const {
    switch (tone) {
    case StatusTone::Ok: return "[OK]";
    case StatusTone::Pending: return "[..]";
    case StatusTone::Warning: return "[!!]";
    case StatusTone::Blocked: return "[XX]";
    }
    return "[..]";
}

std::string DashboardRenderer::trimTo(std::string value, std::size_t width) const {
    if (value.size() <= width) {
        return value;
    }
    if (width <= 3) {
        return value.substr(0, width);
    }
    value.resize(width - 3);
    value += "...";
    return value;
}

DashboardModel buildStartupDashboard() {
    DashboardModel model;
    model.account.deviceAuthorization.verificationUri = "https://login.nvidia.com/activate";
    model.account.deviceAuthorization.userCode = "---- ----";

    model.games = {
        {"My Library", "requires account login and catalog fetch", 0, StatusTone::Pending},
        {"Free-to-Play", "catalog endpoint pending", 0, StatusTone::Pending},
        {"Recent", "session history pending", 0, StatusTone::Pending},
        {"Linked Stores", "Steam/Epic/Xbox account info pending", 0, StatusTone::Pending},
    };

    model.status = {
        {"NRO", "built and validated", StatusTone::Ok},
        {"Auth protocol", "OAuth PKCE and device authorization request builders ready", StatusTone::Ok},
        {"Switch login UI", "off-device QR/code screen scaffolded", StatusTone::Pending},
        {"CloudMatch", "create/poll/stop/claim builders ready", StatusTone::Ok},
        {"WSS signaling", "native mbedTLS socket linked", StatusTone::Ok},
#if defined(OPENNOW_HAS_LIBPEER)
        {"Media transport", "libpeer ICE/DTLS-SRTP/RTP linked; decode/render/audio sinks pending", StatusTone::Warning},
#else
        {"Media transport", "WebRTC/ICE/RTP library not linked", StatusTone::Blocked},
#endif
    };

    model.logs = {
        {LogLevel::Info, LogCategory::Build, "NRO package validation passed."},
        {LogLevel::Info, LogCategory::Auth, "Steam Deck device authorization flow identified in research JS."},
        {LogLevel::Info, LogCategory::Session, "CloudMatch protocol builders available."},
        {LogLevel::Info, LogCategory::Signaling, "Native WSS client linked with GFN peer_info, offer, answer, ICE, ack, and heartbeat messages."},
#if defined(OPENNOW_HAS_LIBPEER)
        {LogLevel::Info, LogCategory::Media, "Native libpeer WebRTC transport linked for Switch; decoder/render/audio sinks are next."},
#else
        {LogLevel::Warning, LogCategory::Media, "Native WebRTC transport library is not linked."},
#endif
    };

    return model;
}

} // namespace opennow::ui
