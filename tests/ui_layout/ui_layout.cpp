#include "app_state.hpp"
#include "game_browser_header.hpp"
#include "game_card_view.hpp"
#include "game_detail_view.hpp"
#include "game_grid_navigation.hpp"
#include "localization.hpp"
#include "top_bar_frame.hpp"

#include <borealis.hpp>
#include <borealis/extern/glad/glad.h>
#include <SDL2/SDL.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace
{
int failures = 0;

void Check(bool passed, const std::string& message)
{
    std::printf("%s %s\n", passed ? "PASS" : "FAIL", message.c_str());
    failures += !passed;
}

void CheckChildren(brls::Box* box, const std::string& path)
{
    const auto bounds = box->getFrame();
    float previous_right = bounds.getMinX();
    size_t index = 0;
    for (auto* child : box->getChildren())
    {
        if (child->getVisibility() == brls::Visibility::GONE)
            continue;
        const auto rect = child->getFrame();
        const auto name = path + "/" + std::to_string(index++);
        std::printf("RECT %s %.1f %.1f %.1f %.1f\n", name.c_str(),
                    rect.getMinX(), rect.getMinY(), rect.getWidth(), rect.getHeight());
        Check(rect.getMinX() >= bounds.getMinX() - 2.0f &&
              rect.getMaxX() <= bounds.getMaxX() + 2.0f, name + " horizontal containment");
        Check(rect.getMinY() >= bounds.getMinY() - 2.0f &&
              rect.getMaxY() <= bounds.getMaxY() + 2.0f, name + " vertical containment");
        if (box->getAxis() == brls::Axis::ROW && !dynamic_cast<brls::ScrollingFrame*>(box))
            Check(rect.getMinX() >= previous_right - 2.0f, name + " no sibling overlap");
        previous_right = rect.getMaxX();
        if (auto* nested = dynamic_cast<brls::Box*>(child))
            CheckChildren(nested, name);
    }
}

void Screenshot(const std::string& path)
{
    const int width = brls::Application::windowWidth;
    const int height = brls::Application::windowHeight;
    std::vector<unsigned char> pixels(width * height * 3);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    const size_t bottom_right = static_cast<size_t>(width - 1) * 3;
    Check(pixels[bottom_right] != 0 || pixels[bottom_right + 1] != 0 || pixels[bottom_right + 2] != 0,
          "native frame covers bottom-right framebuffer pixel");
    std::ofstream file(path, std::ios::binary);
    file << "P6\n" << width << ' ' << height << "\n255\n";
    for (int y = height - 1; y >= 0; --y)
        file.write(reinterpret_cast<const char*>(pixels.data() + y * width * 3), width * 3);
}
}

int main(int argc, char** argv)
{
    opennow::SetInterfaceLanguage(argc > 1 ? argv[1] : "en");
    brls::Logger::setLogLevel(brls::LogLevel::LOG_ERROR);
    if (!brls::Application::init())
        return 2;
    brls::Application::createWindow("OpenNOW native UI layout test");
    brls::Application::getPlatform()->setThemeVariant(brls::ThemeVariant::DARK);
    brls::Application::setGlobalQuit(false);
    if (argc > 4 && std::string(argv[4]) == "1920")
    {
        SDL_SetWindowSize(SDL_GL_GetCurrentWindow(), 1920, 1080);
        int width = 0;
        int height = 0;
        SDL_GL_GetDrawableSize(SDL_GL_GetCurrentWindow(), &width, &height);
        glViewport(0, 0, width, height);
        brls::Application::setWindowSize(1920, 1080);
    }
    opennow::AuthSession session;
    session.user.display_name = argc > 2 ? argv[2] : "Player";
    session.user.membership_tier = "Ultimate";
    session.user.membership_tier_verified = true;
    session.subscription.available = true;
    session.subscription.remaining_hours = 100.5;
    session.subscription.has_storage = true;
    session.subscription.storage_size_gb = 1000;
    if (session.user.display_name != "guest")
        opennow::AppState::Instance().SetSession(session);

    auto* frame = new opennow::TopBarFrame();
    brls::Box* header = nullptr;
    brls::Box* row = nullptr;
    std::vector<brls::View*> actions;
    frame->addTab(opennow::Tr("Store"), [] { return new brls::Box(); });
    frame->addTab(opennow::Tr("Library"), [&] {
        auto* page = new brls::Box(brls::Axis::COLUMN);
        page->setPadding(18, 32, 18, 32);
        for (const auto* text : {"Y  Search", "ZL  All stores", "ZR  Last Played", "X  More / Refresh"})
            actions.push_back(opennow::ui::MakeGameBrowserActionButton(text));
        header = opennow::ui::MakeGameBrowserHeader("My Library", actions);
        page->addView(header);
        row = new brls::Box(brls::Axis::ROW);
        for (int i = 0; i < 5; ++i)
        {
            auto* card = new opennow::GameCardView({"A very long game title for clipping verification", "Steam", "Recently played", ""}, [] {});
            card->setMarginRight(i < 4 ? 24 : 0);
            row->addView(card);
        }
        page->addView(row);
        return page;
    });
    frame->addTab(opennow::Tr("Settings"), [] { return new brls::Box(); });
    frame->focusTab(1);
    brls::Application::pushActivity(new brls::Activity(frame));
    for (int i = 0; i < 20; ++i)
        if (!brls::Application::mainLoop())
            return 2;
    int drawable_width = 0;
    int drawable_height = 0;
    SDL_GL_GetDrawableSize(SDL_GL_GetCurrentWindow(), &drawable_width, &drawable_height);
    const int expected_width = argc > 4 && std::string(argv[4]) == "1920" ? 1920 : 1280;
    Check(drawable_width == expected_width && drawable_height == expected_width * 9 / 16,
          "native drawable reaches requested physical resolution");
    int viewport[4] {};
    glGetIntegerv(GL_VIEWPORT, viewport);
    Check(viewport[2] == drawable_width && viewport[3] == drawable_height,
          "OpenGL viewport covers native drawable");
    Check(brls::Application::windowWidth == static_cast<unsigned int>(drawable_width) &&
          brls::Application::windowHeight == static_cast<unsigned int>(drawable_height),
          "Borealis physical dimensions match native drawable");
    Check(brls::Application::contentWidth == 1280 && brls::Application::contentHeight == 720,
          "1280x720 logical layout");
    CheckChildren(static_cast<brls::Box*>(frame->getChildren()[0]), "topbar");
    auto* tabs = static_cast<brls::Box*>(static_cast<brls::Box*>(frame->getChildren()[0])->getChildren()[1]);
    for (auto* tab : tabs->getChildren())
    {
        auto* label_box = static_cast<brls::Box*>(static_cast<brls::Box*>(tab)->getChildren()[0]);
        Check(label_box->getChildren()[0]->getFrame().getHeight() <= 20, "tab label stays on one line");
    }
    CheckChildren(header, "browser_header");
    CheckChildren(row, "grid");
    for (auto* card : row->getChildren())
        Check(card->getFrame().getWidth() == 224, "grid card retains 224 logical pixels");
    auto& cards = row->getChildren();
    opennow::WireVerticalGridNavigation({actions, cards});
    Check(cards.back()->getCustomNavigationRoutePtr(brls::FocusDirection::UP) == actions.back(),
          "last grid column clamps to last toolbar action");
    Check(cards.back()->getCustomNavigationRoutePtr(brls::FocusDirection::DOWN) == cards.back(),
          "bottom grid edge stays on live card");
    if (argc > 5 && std::string(argv[5]) == "detail")
    {
        opennow::GameDetailData data;
        data.title = "A game title";
        data.subtitle = "GeForce NOW library";
        data.stores = "Steam";
        data.description = "A description for the native layout fixture.";
        data.owned = true;
        auto* detail = new opennow::GameDetailView(opennow::GfnClient(), data);
        brls::Application::pushActivity(new brls::Activity(detail));
        for (int i = 0; i < 20; ++i)
            brls::Application::mainLoop();
        CheckChildren(detail, "detail");
    }
    if (argc > 3)
        Screenshot(argv[3]);
    brls::Application::quit();
    while (brls::Application::mainLoop()) {}
    return failures ? 1 : 0;
}
