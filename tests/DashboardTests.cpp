#include "opennow/ui/Dashboard.hpp"

#include <cassert>
#include <string>

int main() {
    const auto model = opennow::ui::buildStartupDashboard();
    const opennow::ui::DashboardRenderer renderer(60);
    const auto screen = renderer.render(model);

    assert(screen.find("OpenNOW Switch") != std::string::npos);
    assert(screen.find("Account") != std::string::npos);
    assert(screen.find("Off-Device Login") != std::string::npos);
    assert(screen.find("Steam Deck-style device authorization") != std::string::npos);
    assert(screen.find("Games") != std::string::npos);
    assert(screen.find("My Library") != std::string::npos);
    assert(screen.find("[OK] WSS signaling") != std::string::npos);

    return 0;
}
