#include "opennow/app/OpenNowSwitchApp.hpp"

#include <sstream>

#include "opennow/gfn/InputEncoder.hpp"
#include "opennow/gfn/SignalingClient.hpp"
#include "opennow/media/NativeMediaPipeline.hpp"
#include "opennow/ui/Dashboard.hpp"

namespace opennow {

OpenNowSwitchApp::OpenNowSwitchApp(std::unique_ptr<Platform> platform)
    : platform_(std::move(platform)) {}

int OpenNowSwitchApp::run() {
    if (platform_->isAppletMode()) {
        platform_->log(LogLevel::Error, LogCategory::Platform, "Album applet mode detected. OpenNOW Switch requires title redirection/full RAM.");
        return 2;
    }

    auto dashboard = ui::buildStartupDashboard();
    dashboard.logs.insert(dashboard.logs.begin(), {LogLevel::Info, LogCategory::Platform, "Platform: " + platform_->name()});
    dashboard.logs.insert(dashboard.logs.begin() + 1, {LogLevel::Info, LogCategory::Media, "Target stream: " + describe(settings_)});

    const auto report = gfn_.capabilityReport(settings_);
    dashboard.logs.push_back({LogLevel::Warning, LogCategory::Session, report.message});

    gfn::InputEncoder encoder;
    encoder.setProtocolVersion(3);
    const auto heartbeat = encoder.encodeHeartbeat();

    std::ostringstream encoded;
    encoded << "GFN input encoder initialized. Heartbeat packet bytes=" << heartbeat.size();
    dashboard.logs.push_back({LogLevel::Info, LogCategory::Input, encoded.str()});

    gfn::SignalingClient signaling({
        "example.invalid:443",
        "session-placeholder",
        "wss://example.invalid/nvst/",
    });
    dashboard.logs.push_back({LogLevel::Info, LogCategory::Signaling, "Signaling boundary ready: " + signaling.signInUrl()});

    media::NativeMediaPipeline media;
    const auto mediaStatus = media.open(settings_, {});
    dashboard.logs.push_back({
        mediaStatus.ready ? LogLevel::Info : LogLevel::Warning,
        LogCategory::Media,
        mediaStatus.message,
    });

    ui::DashboardRenderer renderer(78);
    platform_->showScreen(renderer.render(dashboard));

    platform_->waitForExit();

    return 0;
}

void OpenNowSwitchApp::printStartupSummary() const {
    platform_->log("OpenNOW Switch " OPENNOW_SWITCH_VERSION);
    platform_->log(LogLevel::Info, LogCategory::Platform, "Platform: " + platform_->name());
    platform_->log(LogLevel::Info, LogCategory::Media, "Target stream: " + describe(settings_));
}

void OpenNowSwitchApp::printRuntimeCategories() const {
    platform_->log(LogLevel::Info, LogCategory::Build, "NRO package validated at build time: NRO0 header + NACP metadata.");
    platform_->log(LogLevel::Info, LogCategory::Auth, "Host login flow implemented: PKCE, OAuth callback, token exchange, refresh, client token.");
    platform_->log(LogLevel::Warning, LogCategory::Auth, "Switch login UI is pending: needs QR/manual-code flow or device-code-style handoff.");
    platform_->log(LogLevel::Info, LogCategory::Network, "HTTP boundary available; Switch TLS packaging still needs live-device validation.");
    platform_->log(LogLevel::Info, LogCategory::Session, "CloudMatch create, poll, stop, claim request builders and response parser are implemented.");
    platform_->log(LogLevel::Info, LogCategory::Signaling, "Native WSS/TLS socket implementation is linked when switch-mbedtls is installed; live SDP exchange is next.");
    platform_->log(LogLevel::Warning, LogCategory::Media, "Native WebRTC/ICE/RTP transport, decoder, renderer, and audio output are still pending.");
    platform_->log(LogLevel::Info, LogCategory::Input, "GFN input encoder and controller mapping are implemented with host tests.");
}

} // namespace opennow
