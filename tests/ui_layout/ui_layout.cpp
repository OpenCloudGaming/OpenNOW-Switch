#include "app_state.hpp"
#include "catalog_tab.hpp"
#include "cover_image_cache.hpp"
#include "game_card_view.hpp"
#include "game_detail_view.hpp"
#include "library_tab.hpp"
#include "library_row_view.hpp"
#include "localization.hpp"
#include "queue_view.hpp"
#include "settings_tab.hpp"
#include "stream_overlay_view.hpp"
#include "stream_settings.hpp"
#include "top_bar_frame.hpp"
#include "ui_layout_fixtures.hpp"
#include "ui_theme.hpp"
#include "ui_helpers.hpp"

#include <borealis.hpp>
#include <borealis/extern/glad/glad.h>
#include <borealis/extern/nanovg/stb_truetype.h>
#include <SDL2/SDL.h>
#include <yoga/Yoga.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
int failures = 0;
std::string capture_path;
void Screenshot(const std::string& path);
void Check(bool passed, const std::string& message)
{
    std::printf("%s %s\n", passed ? "PASS" : "FAIL", message.c_str());
    failures += !passed;
}
void Pump(int frames = 24)
{
    for (int index = 0; index < frames; ++index)
        if (!brls::Application::mainLoop())
            throw std::runtime_error("Native event loop exited before assertions completed");
}
void DrainToasts()
{
    auto style = brls::Application::getStyle();
    const auto duration = static_cast<int>(style.getMetric("brls/animations/notification_timeout") +
        2 * style.getMetric("brls/animations/notification_show") + 100);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(duration);
    while (std::chrono::steady_clock::now() < deadline) Pump(1);
}
void Until(const std::function<bool()>& predicate, const std::string& message, int seconds = 5)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) Pump(1);
    Check(predicate(), message);
    Pump();
}
brls::View* CurrentRoot()
{
    const auto stack = brls::Application::getActivitiesStack();
    return stack.empty() ? nullptr : stack.back()->getContentView();
}
void Walk(brls::View* root, const std::function<void(brls::View*)>& visit)
{
    if (!root || root->getVisibility() == brls::Visibility::GONE) return;
    visit(root);
    if (auto* box = dynamic_cast<brls::Box*>(root))
        for (auto* child : box->getChildren()) Walk(child, visit);
}
std::string Text(brls::View* root)
{
    std::string text;
    Walk(root, [&](brls::View* view) {
        if (auto* label = dynamic_cast<brls::Label*>(view)) text += label->getFullText() + "\n";
    });
    return text;
}
brls::View* Required(brls::View* root, const std::string& id)
{
    auto* view = root ? root->getView(id) : nullptr;
    Check(view != nullptr, "semantic view exists: " + id);
    if (!view) throw std::runtime_error("Missing production view ID: " + id);
    return view;
}
brls::View* Action(brls::View* root, const std::string& title)
{
    brls::View* result = nullptr;
    Walk(root, [&](brls::View* view) {
        if (view->isFocusable() && Text(view).find(opennow::Tr(title)) != std::string::npos) result = view;
    });
    Check(result != nullptr, "production action exists: " + title);
    if (!result) throw std::runtime_error("Missing production action: " + title);
    return result;
}
void Key(brls::ControllerButton button)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (brls::Application::isInputBlocks() && std::chrono::steady_clock::now() < deadline) Pump(1);
    Check(!brls::Application::isInputBlocks(), "controller dispatch is not blocked by a transition");
    std::printf("INPUT button=%d focus=%s\n", static_cast<int>(button),
        brls::Application::getCurrentFocus() ? brls::Application::getCurrentFocus()->describe().c_str() : "none");
    brls::Application::onControllerButtonPressed(button, false);
    Pump();
}
void Activate(brls::View* view)
{
    brls::Application::giveFocus(view);
    Pump();
    Check(brls::Application::getCurrentFocus() == view->getDefaultFocus(), "action owns native focus before A");
    Key(brls::BUTTON_A);
}
void Tap(brls::View* target)
{
    const auto frame = target->getFrame();
    brls::Point point(frame.getMinX() + frame.getWidth() * 0.5f, frame.getMinY() + frame.getHeight() * 0.5f);
    auto* responder = CurrentRoot()->hitTest(point);
    Check(responder != nullptr, "native hitTest finds touch responder");
    if (!responder) throw std::runtime_error("Touch target is outside the production activity");
    auto* ancestor = responder;
    while (ancestor && ancestor != target) ancestor = ancestor->hasParent() ? ancestor->getParent() : nullptr;
    Check(ancestor == target, "touch responder belongs to the painted action");
    brls::TouchState touch;
    touch.position = point;
    touch.view = responder;
    touch.phase = brls::TouchPhase::START;
    responder->gestureRecognizerRequest(touch, {}, responder);
    touch.phase = brls::TouchPhase::END;
    responder->gestureRecognizerRequest(touch, {}, responder);
    Pump();
}
void Search(brls::View* root, const std::string& query)
{
    const std::string id = root->getView("catalog") ? "catalog-search" : "library-search";
    brls::Application::giveFocus(Required(root, id));
    Pump();
    const auto count = brls::Application::getActivitiesStack().size();
    Key(brls::BUTTON_Y);
    Check(brls::Application::getActivitiesStack().size() == count + 1, "Y opens actual native SDL IME dialog");
    auto* input = dynamic_cast<brls::Label*>(Required(CurrentRoot(), "brls/dialog/label"));
    auto* count_label = dynamic_cast<brls::Label*>(Required(CurrentRoot(), "brls/dialog/count"));
    if (!input || !count_label) throw std::runtime_error("Native IME text/count label missing");
    size_t deletes = 0;
    while (!count_label->getFullText().starts_with("0/") && deletes++ < 64)
        brls::Application::onControllerButtonPressed(brls::BUTTON_BACK, false);
    Check(count_label->getFullText().starts_with("0/"), "native IME deletion clears previous query");
    if (!query.empty())
    {
        SDL_Event event {};
        event.type = SDL_TEXTINPUT;
        std::snprintf(event.text.text, sizeof(event.text.text), "%s", query.c_str());
        Check(SDL_PushEvent(&event) == 1, "SDL accepts actual text-input event");
        Pump();
        Check(input->getFullText() == query, "native IME receives SDL text-input query");
    }
    Key(brls::BUTTON_A);
    Check(brls::Application::getActivitiesStack().size() == count, "native IME submit returns to production page");
}
void CheckChildren(brls::Box* box, const std::string& path)
{
    const auto bounds = box->getFrame();
    float previous_right = bounds.getMinX();
    size_t index = 0;
    for (auto* child : box->getChildren())
    {
        if (child->getVisibility() == brls::Visibility::GONE) continue;
        const auto rect = child->getFrame();
        const auto name = path + "/" + std::to_string(index++);
        std::printf("RECT %s %.1f %.1f %.1f %.1f\n", name.c_str(), rect.getMinX(), rect.getMinY(), rect.getWidth(), rect.getHeight());
        Check(rect.getMinX() >= bounds.getMinX() - 2 && rect.getMaxX() <= bounds.getMaxX() + 2, name + " horizontal containment");
        if (!dynamic_cast<brls::ScrollingFrame*>(box))
            Check(rect.getMinY() >= bounds.getMinY() - 2 && rect.getMaxY() <= bounds.getMaxY() + 2, name + " vertical containment");
        const bool flow = YGNodeStyleGetPositionType(child->getYGNode()) != YGPositionTypeAbsolute;
        if (box->getAxis() == brls::Axis::ROW && !dynamic_cast<brls::ScrollingFrame*>(box) && flow)
            Check(rect.getMinX() >= previous_right - 2, name + " no flow sibling overlap");
        if (flow) previous_right = rect.getMaxX();
        if (auto* nested = dynamic_cast<brls::Box*>(child)) CheckChildren(nested, name);
    }
}
struct FontFile
{
    std::vector<unsigned char> bytes;
    stbtt_fontinfo info {};
    explicit FontFile(const std::string& path)
    {
        std::ifstream file(path, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(file), {});
        if (bytes.empty() || !stbtt_InitFont(&info, bytes.data(), stbtt_GetFontOffsetForIndex(bytes.data(), 0)))
            throw std::runtime_error("Cannot inspect bundled font: " + path);
    }
};
std::vector<int> Codepoints(const std::string& text)
{
    std::vector<int> result;
    for (size_t offset = 0; offset < text.size();)
    {
        const unsigned char first = text[offset++];
        int count = first < 0x80 ? 0 : (first & 0xe0) == 0xc0 ? 1 : (first & 0xf0) == 0xe0 ? 2 : 3;
        int code = first & (count == 0 ? 0x7f : count == 1 ? 0x1f : count == 2 ? 0x0f : 0x07);
        while (count-- && offset < text.size()) code = (code << 6) | (static_cast<unsigned char>(text[offset++]) & 0x3f);
        result.push_back(code);
    }
    return result;
}
void CheckFonts(brls::View* root)
{
    const std::string dir = std::string(OPENNOW_UI_SOURCE_ROOT) + "/resources/";
    const std::array<const char*, 5> paths {"font/Nunito-Medium.ttf", "font/Nunito-SemiBold.ttf", "font/Nunito-Bold.ttf", "font/Nunito-ExtraBold.ttf", "font/IBMPlexMono-Medium.ttf"};
    const std::array<const char*, 5> names {"regular", "opennow-semibold", "opennow-bold", "opennow-extrabold", "opennow-mono"};
    std::vector<FontFile> bases;
    std::vector<int> handles;
    for (size_t index = 0; index < paths.size(); ++index)
    {
        handles.push_back(opennow::ui::Font(static_cast<opennow::ui::FontRole>(index)));
        Check(handles.back() != brls::FONT_INVALID && brls::Application::getFont(names[index]) == handles.back(), "real named OpenNOW font role registered: " + std::to_string(index));
        bases.emplace_back(dir + paths[index]);
    }
    Check(brls::Application::getDefaultFont() == handles.front(), "stock label default uses initialized Nunito 500");
    std::vector<FontFile> fallbacks;
    for (const char* path : {"font/OpenNOW-CJK.ttf", "font/switch_font.ttf", "font/switch_icons.ttf", "material/MaterialIcons-Regular.ttf"}) fallbacks.emplace_back(dir + path);
    Check(brls::Application::getFont("opennow-cjk") != brls::FONT_INVALID, "bundled Chinese fallback registered");
    Walk(root, [&](brls::View* view) {
        auto* label = dynamic_cast<brls::Label*>(view);
        if (!label) return;
        const auto found = std::find(handles.begin(), handles.end(), label->getFont());
        Check(found != handles.end(), "production label uses an initialized OpenNOW font");
        if (found == handles.end()) return;
        const size_t index = found - handles.begin();
        for (int code : Codepoints(label->getFullText()))
        {
            if (code <= 32) continue;
            bool present = stbtt_FindGlyphIndex(&bases[index].info, code) != 0;
            for (auto& fallback : fallbacks) present = present || stbtt_FindGlyphIndex(&fallback.info, code) != 0;
            if (!present) Check(false, "actual bundled font chain lacks codepoint " + std::to_string(code));
        }
    });
    auto* vg = brls::Application::getNVGContext();
    for (int handle : handles)
    {
        nvgFontFaceId(vg, handle);
        nvgFontSize(vg, 18);
        float bounds[4] {};
        const float measured = nvgTextBounds(vg, 0, 0, "OpenNOW Жї 玩家设置", nullptr, bounds);
        Check(std::isfinite(measured) && measured > 100 && bounds[3] > bounds[1], "actual font chain measures mixed Latin/Cyrillic/Chinese text");
    }
}
struct GlyphMetrics { float width, left_bearing, right_overhang; };
GlyphMetrics MeasureGlyphs(brls::Label* label, const std::string& text, const ui_fixture::TextDraw* draw = nullptr)
{
    auto* vg = brls::Application::getNVGContext();
    nvgSave(vg);
    nvgFontFaceId(vg, label->getFont());
    nvgFontSize(vg, label->getFontSize());
    nvgFontQuality(vg, label->getFontQuality());
    nvgTextLineHeight(vg, label->getLineHeight());
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    if (draw)
    {
        nvgResetTransform(vg);
        const auto& transform = draw->transform;
        nvgTransform(vg, transform[0], transform[1], transform[2], transform[3], transform[4], transform[5]);
    }
    const float x = draw ? draw->raw_x : 0, y = draw ? draw->raw_y : 0;
    float bounds[4] {};
    const float advance = nvgTextBounds(vg, x, y, text.c_str(), nullptr, bounds);
    nvgRestore(vg);
    return {bounds[2] - bounds[0], std::min(0.0f, bounds[0] - x), std::max(0.0f, bounds[2] - x - advance)};
}
float GlyphWidth(brls::Label* label) { return MeasureGlyphs(label, label->getFullText()).width; }
std::vector<ui_fixture::TextDraw> CaptureText()
{
    ui_fixture::BeginTextCapture();
    Pump(1);
    return ui_fixture::EndTextCapture();
}
void CheckPaintedLabel(brls::Label* label, const std::vector<ui_fixture::TextDraw>& draws,
                       const std::string& name, bool require_ellipsis = false,
                       const brls::Rect* row_bounds = nullptr, const brls::Rect* scissor_bounds = nullptr)
{
    const auto full = label->getFullText();
    if (full.empty()) return;
    const auto frame = label->getFrame();
    bool found = false;
    for (const auto& draw : draws)
    {
        const bool ellipsis = draw.text.ends_with("…");
        const bool identity = draw.text == full ||
            (ellipsis && full.starts_with(draw.text.substr(0, draw.text.size() - std::string("…").size())));
        if (!identity || std::abs(draw.y - frame.getMidY()) > 1 ||
            draw.bounds[2] < frame.getMinX() || draw.bounds[0] > frame.getMaxX()) continue;
        found = true;
        const auto glyphs = MeasureGlyphs(label, draw.text, &draw);
        std::printf("GLYPHS %s text=[%s] ink=%.2f,%.2f,%.2f,%.2f frame=%.2f,%.2f,%.2f,%.2f scrolling=%d\n",
            name.c_str(), draw.text.c_str(), draw.bounds[0], draw.bounds[1], draw.bounds[2], draw.bounds[3],
            frame.getMinX(), frame.getMinY(), frame.getMaxX(), frame.getMaxY(), draw.scrolling_path);
        Check(!draw.scrolling_path, name + " paints static text, not a scissored marquee");
        Check(std::abs(draw.x - frame.getMinX()) <= 0.5f, name + " native text anchor remains at allocated label origin");
        const float raster_pixel = 1.0f / brls::Application::windowScale;
        Check(draw.bounds[0] >= frame.getMinX() + glyphs.left_bearing - raster_pixel &&
            draw.bounds[2] <= frame.getMaxX() + glyphs.right_overhang + raster_pixel,
            name + " painted glyphs fit label width with actual measured bearings");
        std::printf("BEARINGS %s anchor=%.2f expected=%.2f left=%.2f right=%.2f raster_pixel=%.5f\n",
            name.c_str(), draw.x, frame.getMinX(), glyphs.left_bearing, glyphs.right_overhang, raster_pixel);
        for (const auto* bounds : {row_bounds, scissor_bounds})
            if (bounds) Check(draw.bounds[0] >= bounds->getMinX() - 0.5f && draw.bounds[2] <= bounds->getMaxX() + 0.5f &&
                draw.bounds[1] >= bounds->getMinY() - 0.5f && draw.bounds[3] <= bounds->getMaxY() + 0.5f,
                name + " actual glyph ink remains inside row and scissor bounds");
        Check(draw.bounds[1] >= frame.getMinY() - 1 && draw.bounds[3] <= frame.getMaxY() + 1,
            name + " actual painted glyphs fit allocated label height");
        if (require_ellipsis) Check(ellipsis, name + " overflowing game identity has a complete static ellipsis");
    }
    Check(found, name + " observed actual NanoVG text draw");
}
void CheckActionRow(opennow::ui::ActionRow* row, const std::vector<ui_fixture::TextDraw>& draws,
                    const std::string& name, bool full_caption = false)
{
    const auto frame = row->getFrame();
    const float left = frame.getMinX() + YGNodeLayoutGetPadding(row->getYGNode(), YGEdgeLeft);
    const float right = frame.getMaxX() - YGNodeLayoutGetPadding(row->getYGNode(), YGEdgeRight);
    float previous_right = left;
    brls::Rect scissor;
    bool has_scissor = false;
    auto* parent = row->hasParent() ? row->getParent() : nullptr;
    while (parent)
    {
        if (auto* viewport = dynamic_cast<brls::ScrollingFrame*>(parent))
        {
            scissor = viewport->getFrame();
            has_scissor = true;
            break;
        }
        parent = parent->hasParent() ? parent->getParent() : nullptr;
    }
    for (auto* child : row->getChildren())
    {
        auto* label = dynamic_cast<brls::Label*>(child);
        if (!label || label->getVisibility() != brls::Visibility::VISIBLE || label->getFullText().empty()) continue;
        const auto label_frame = label->getFrame();
        Check(label_frame.getMinX() >= left - 0.5f && label_frame.getMaxX() <= right + 0.5f,
            name + " title/value label stays inside row content padding");
        Check(label_frame.getMinX() >= previous_right - 0.5f, name + " title and value do not overlap");
        previous_right = label_frame.getMaxX();
        const float required = GlyphWidth(label);
        std::printf("CAPTION %s text=[%s] font=%d size=%.1f glyph_width=%.2f available=%.2f\n",
            name.c_str(), label->getFullText().c_str(), label->getFont(), label->getFontSize(), required, label_frame.getWidth());
        if (full_caption) Check(required <= label_frame.getWidth() + 0.5f, name + " complete fixed caption fits measured content width");
        CheckPaintedLabel(label, draws, name, false, &frame, has_scissor ? &scissor : nullptr);
    }
}
void CheckVisibleActionRows(brls::View* root, const std::string& name)
{
    const auto draws = CaptureText();
    Walk(root, [&](brls::View* view) {
        auto* row = dynamic_cast<opennow::ui::ActionRow*>(view);
        if (!row) return;
        const auto frame = row->getFrame();
        auto* parent = row->hasParent() ? row->getParent() : nullptr;
        while (parent)
        {
            if (auto* viewport = dynamic_cast<brls::ScrollingFrame*>(parent))
            {
                const auto bounds = viewport->getFrame();
                if (frame.getMinY() < bounds.getMinY() || frame.getMaxY() > bounds.getMaxY()) return;
            }
            parent = parent->hasParent() ? parent->getParent() : nullptr;
        }
        auto title = Text(row);
        std::replace(title.begin(), title.end(), '\n', ' ');
        CheckActionRow(row, draws, name + "/" + title);
    });
}
void CheckLibraryToolbar(brls::View* root)
{
    const auto draws = CaptureText();
    for (const char* id : {"library-search", "library-filter", "library-sort", "library-more", "library-previous", "library-next"})
    {
        auto* row = dynamic_cast<opennow::ui::ActionRow*>(Required(root, id));
        if (!row) throw std::runtime_error("Library toolbar is not a production ActionRow");
        CheckActionRow(row, draws, id, true);
    }
}
void CheckStaticGameTitle(brls::View* owner, brls::Label* title, const std::string& name)
{
    Check(title != nullptr, name + " native title label exists");
    if (!title) throw std::runtime_error("Missing production game title label");
    brls::Application::giveFocus(owner); Pump(); DrainToasts();
    const bool overflow = GlyphWidth(title) > title->getWidth() + 1;
    Check(overflow, name + " fixture genuinely exceeds native title width");
    CheckPaintedLabel(title, CaptureText(), name + "/focused", overflow);
    auto read_title_pixels = [&] {
        const auto frame = title->getFrame();
        const float scale = brls::Application::windowScale;
        const int x = std::lround(frame.getMinX() * scale), width = std::lround(frame.getWidth() * scale);
        const int y = brls::Application::windowHeight - std::lround(frame.getMaxY() * scale), height = std::lround(frame.getHeight() * scale);
        std::vector<unsigned char> pixels(width * height * 3);
        glReadBuffer(GL_BACK); glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(x, y, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
        return pixels;
    };
    const auto first_pixels = read_title_pixels();
    const std::string title_name = name.starts_with("library") ? "library" : "store";
    Screenshot(capture_path + "." + title_name + "-title-focused.ppm");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2200);
    while (std::chrono::steady_clock::now() < deadline) Pump(1);
    CheckPaintedLabel(title, CaptureText(), name + "/after-scroll-timer", overflow);
    Check(first_pixels == read_title_pixels(), name + " title framebuffer pixels remain unchanged after native scroll timer");
    Screenshot(capture_path + "." + title_name + "-title-after-scroll-timer.ppm");
}
float CheckScrollDecorations(brls::ScrollingFrame* viewport, const std::string& name)
{
    float reserved_right = 0;
    for (auto* child : viewport->getChildren())
    {
        if (!child->isDetached() || child->getWidth() != 4 || child->getAlpha() <= 0.001f) continue;
        reserved_right = std::max(reserved_right, viewport->getFrame().getMaxX() - child->getFrame().getMinX());
    }
    Check(reserved_right == 0, name + " has no visible scrollbar over art, labels or focus border");
    return reserved_right;
}
void CheckFocusedScrollInsets(brls::View* focused, const std::string& name)
{
    auto* parent = focused->hasParent() ? focused->getParent() : nullptr;
    while (parent && !dynamic_cast<brls::ScrollingFrame*>(parent)) parent = parent->hasParent() ? parent->getParent() : nullptr;
    auto* viewport = dynamic_cast<brls::ScrollingFrame*>(parent);
    Check(viewport != nullptr, name + " is inside a native scrolling viewport");
    if (!viewport) throw std::runtime_error("Missing native scrolling ancestor");
    const auto bounds = viewport->getFrame(), frame = focused->getFrame();
    const float reserved = CheckScrollDecorations(viewport, name);
    Check(frame.getMinX() - 4 >= bounds.getMinX() - 0.5f && frame.getMaxX() + 4 <= bounds.getMaxX() - reserved + 0.5f,
        name + " complete focus border fits horizontally inside scrolling viewport");
}
void CheckSettingsActionRows(brls::View* root, const std::string& category)
{
    auto* viewport = dynamic_cast<brls::ScrollingFrame*>(Required(root, "settings/options"));
    if (!viewport) throw std::runtime_error("Missing production settings scissor viewport");
    std::vector<opennow::ui::ActionRow*> rows;
    Walk(viewport, [&](brls::View* view) { if (auto* row = dynamic_cast<opennow::ui::ActionRow*>(view)) rows.push_back(row); });
    Check(!rows.empty(), "Settings category contains production action rows: " + category);
    auto* original_focus = brls::Application::getCurrentFocus();
    const auto bounds = viewport->getFrame();
    for (auto* row : rows)
    {
        brls::Application::giveFocus(row); Pump(16);
        const auto frame = row->getFrame();
        auto title = Text(row);
        std::replace(title.begin(), title.end(), '\n', ' ');
        const auto name = "settings/" + category + "/" + title;
        const float scrollbar_reservation = CheckScrollDecorations(viewport, name);
        Check(brls::Application::getCurrentFocus() == row, name + " actual row owns focus");
        Check(frame.getMinX() - 4 >= bounds.getMinX() - 0.5f, name + " focus border has left scissor inset");
        Check(frame.getMaxX() + 4 <= bounds.getMaxX() - scrollbar_reservation + 0.5f,
            name + " focus border clears actual right scissor and scrollbar reservation");
        Check(frame.getMinY() - 4 >= bounds.getMinY() - 0.5f && frame.getMaxY() + 4 <= bounds.getMaxY() + 0.5f,
            name + " focused row and border fit vertical scissor viewport");
        CheckActionRow(row, CaptureText(), name);
    }
    if (original_focus) { brls::Application::giveFocus(original_focus); Pump(); }
}
void CheckTabLanguage(brls::View* root)
{
    Check(Text(Required(root, "shell-tabs")) == opennow::Tr("Store") + "\n" +
        opennow::Tr("Library") + "\n" + opennow::Tr("Settings") + "\n",
        "all three retained shell tab labels reflect current interface language");
}
void SaveLanguageThroughSettings(brls::View* root, const std::string& language)
{
    auto* frame = dynamic_cast<opennow::TopBarFrame*>(root);
    if (!frame) throw std::runtime_error("Language regression requires production shell");
    frame->focusTab(2); Pump();
    Activate(Required(root, "settings/category/app"));
    Activate(Action(root, "App language"));
    Check(dynamic_cast<brls::Dropdown*>(CurrentRoot()) != nullptr, "actual App language opens native selector");
    const auto& options = opennow::InterfaceLanguageOptions();
    const auto current = std::find_if(options.begin(), options.end(), [](const auto& option) {
        return option.code == opennow::GetInterfaceLanguage();
    });
    const auto target = std::find_if(options.begin(), options.end(), [&](const auto& option) { return option.code == language; });
    if (current == options.end() || target == options.end()) throw std::runtime_error("Unsupported language regression fixture");
    const auto difference = target - current;
    for (int index = 0; index < std::abs(difference); ++index)
        Key(difference > 0 ? brls::BUTTON_NAV_DOWN : brls::BUTTON_NAV_UP);
    Key(brls::BUTTON_A); Key(brls::BUTTON_X);
    Check(opennow::LoadStreamSettings().interface_language == language && opennow::GetInterfaceLanguage() == language,
        "actual Settings selector and X Save persist and apply interface language");
    CheckTabLanguage(root);
    frame->focusTab(1); Pump();
    for (const auto& [id, key] : std::array<std::pair<const char*, const char*>, 3> {{
        {"library-search", "Search"}, {"library-filter", "All"}, {"library-sort", "Last Added"}}})
        std::printf("LOCALE language=%s id=%s actual=[%s] expected=[%s]\n",
            language.c_str(), id, Text(Required(root, id)).c_str(), opennow::Tr(key).c_str());
    std::printf("LOCALE language=%s heading_expected=[%s] heading_present=%d\n", language.c_str(),
        opennow::Tr("My Library").c_str(), Text(root).find(opennow::Tr("My Library")) != std::string::npos);
    Check(Text(root).find(opennow::Tr("My Library")) != std::string::npos &&
        Text(Required(root, "library-search")).find(opennow::Tr("Search")) != std::string::npos &&
        Text(Required(root, "library-filter")).find(opennow::Tr("All")) != std::string::npos &&
        Text(Required(root, "library-sort")).find(opennow::Tr("Last Added")) != std::string::npos,
        "retained Library heading and controls refresh after actual language Save");
    Check(Text(Required(root, "library-preview-title")).find(ui_fixture::Library().front().title) != std::string::npos,
        "retained Library preserves selected game identity across language Save");
}
void Screenshot(const std::string& path)
{
    const int width = brls::Application::windowWidth, height = brls::Application::windowHeight;
    std::vector<unsigned char> pixels(width * height * 3);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    const size_t bottom_right = static_cast<size_t>(width - 1) * 3;
    Check(pixels[bottom_right] != 0 || pixels[bottom_right + 1] != 0 || pixels[bottom_right + 2] != 0, "native frame covers bottom-right framebuffer pixel");
    std::ofstream file(path, std::ios::binary);
    file << "P6\n" << width << ' ' << height << "\n255\n";
    for (int y = height - 1; y >= 0; --y) file.write(reinterpret_cast<const char*>(pixels.data() + y * width * 3), width * 3);
    Check(file.good(), "native framebuffer capture written");
}

void StoreChecks(brls::View* root, bool empty, bool guest)
{
    Until([] { return ui_fixture::Snapshot().catalog_requests + ui_fixture::Snapshot().public_requests > 0; }, "production Store fetch callback reached external fixture");
    std::vector<opennow::GameCardView*> cards;
    Walk(root, [&](brls::View* view) { if (auto* card = dynamic_cast<opennow::GameCardView*>(view)) cards.push_back(card); });
    if (empty) { Check(cards.empty(), "empty real Store feed creates no game cards"); return; }
    Check(cards.size() == 15, "production Store page retains fifteen games");
    CheckStaticGameTitle(cards.front(), dynamic_cast<brls::Label*>(cards.front()->getView("catalog-card-title")), "store/game-title");
    CheckFocusedScrollInsets(cards.front(), "store/first-column");
    for (size_t index = 0; index < cards.size(); ++index)
    {
        const auto rect = cards[index]->getFrame();
        Check(std::abs(rect.getWidth() - 220) < 0.01 && std::abs(rect.getHeight() - 210) < 0.01, "Store card has exact 220x210 logical geometry");
        if (index % 5 != 0) Check(std::abs(rect.getMinX() - cards[index - 1]->getFrame().getMinX() - 245) < 0.01, "Store card origins have exact 245 spacing");
        const auto row = cards[index]->getParent()->getFrame();
        Check(std::abs(row.getWidth() - 1200) < 0.01, "Store row spans 1200 logical pixels");
        Check(rect.getMinX() >= row.getMinX() && rect.getMaxX() <= row.getMaxX() + 0.01, "Store card remains within the 1200-wide row");
    }
    brls::Application::giveFocus(cards[4]); Pump(); Key(brls::BUTTON_NAV_DOWN);
    Check(brls::Application::getCurrentFocus() == cards[9], "real Store down navigation preserves fifth column");
    Key(brls::BUTTON_NAV_UP);
    Check(brls::Application::getCurrentFocus() == cards[4], "real Store up navigation returns to fifth column");
    CheckFocusedScrollInsets(cards[4], "store/fifth-column");
    brls::Application::giveFocus(Required(root, "catalog-filter")); Key(brls::BUTTON_LT);
    Check(Text(root).find("Epic\n") == std::string::npos, "Store Steam filter removes Epic cards");
    for (int index = 0; index < 5; ++index) Key(brls::BUTTON_LT);
    Key(brls::BUTTON_RT);
    Check(Text(Required(root, "catalog-sort")).find(opennow::Tr("Store")) != std::string::npos, "real Store sort cycles to Store");
    Check(dynamic_cast<brls::Box*>(Required(root, "catalog-row-0"))->getChildren().front() == root->getView("catalog-card-1001"), "Store sort actually reorders Epic before Steam instead of just changing its label");
    Key(brls::BUTTON_RT);
    Check(dynamic_cast<brls::Box*>(Required(root, "catalog-row-0"))->getChildren().front() == root->getView("catalog-card-1001"), "Publisher sort actually places the Alpha publisher first");
    Key(brls::BUTTON_RT);
    Search(root, "Fixture game 1");
    if (guest) Check(root->getView("catalog-card-1000") == nullptr && ui_fixture::Snapshot().catalog_requests == 0, "guest Store search filters actual public feed without authenticated requests");
    else Until([] { const auto calls = ui_fixture::Snapshot(); return !calls.search_queries.empty() && calls.search_queries.back() == "Fixture game 1"; }, "Y search passes actual query to catalog service");
    Search(root, "");
    if (!guest) Until([] { return ui_fixture::Snapshot().catalog_requests >= 3; }, "clearing catalog search reloads feed");
    Activate(Required(root, "catalog-next"));
    if (guest) Check(root->getView("catalog-card-1015") != nullptr && ui_fixture::Snapshot().public_requests == 1, "guest Store next page uses already-loaded public data without another fetch");
    else Until([] { const auto calls = ui_fixture::Snapshot(); return std::find(calls.cursors.begin(), calls.cursors.end(), "fixture-page-2") != calls.cursors.end(); }, "next page consumes actual catalog cursor");
    Activate(Required(root, "catalog-previous"));
    brls::Application::giveFocus(Required(root, "catalog-card-1000")); Pump();
    const size_t size = brls::Application::getActivitiesStack().size(); Key(brls::BUTTON_A);
    Check(brls::Application::getActivitiesStack().size() == size + 1 && dynamic_cast<opennow::GameDetailView*>(CurrentRoot()), "Store A opens actual production detail activity");
    Key(brls::BUTTON_B);
    Check(brls::Application::getActivitiesStack().size() == size, "detail B returns to real Store");
}
void LibraryChecks(brls::View* root, bool guest, bool empty)
{
    CheckLibraryToolbar(root);
    if (guest || empty)
    {
        Check(Text(root).find(ui_fixture::Library().empty() ? "Fixture game" : ui_fixture::Library().front().title) == std::string::npos, "guest or empty library has no fabricated games");
        return;
    }
    Until([] { return ui_fixture::Snapshot().library_requests > 0; }, "real Library fetch callback reached fixture");
    auto* preview = Required(root, "library-preview-title");
    opennow::CachedImage* preview_image = nullptr;
    Walk(preview->getParent(), [&](brls::View* view) {
        if (auto* image = dynamic_cast<opennow::CachedImage*>(view)) preview_image = image;
    });
    Check(preview_image != nullptr, "actual Library preview has a native cover image");
    auto check_preview_cover = [&](const std::string& url) {
        const auto calls = ui_fixture::Snapshot();
        const auto request = std::find_if(calls.cover_requests.rbegin(), calls.cover_requests.rend(),
            [&](const ui_fixture::CoverCall& call) { return call.image == preview_image; });
        Check(request != calls.cover_requests.rend() && request->url == url,
            "actual Library preview cover identity matches selected game at image boundary");
    };
    auto* row = Required(root, "library-row-fixture-1000");
    brls::Label* game_title = nullptr;
    Walk(row, [&](brls::View* view) {
        if (auto* label = dynamic_cast<brls::Label*>(view); label && label->getFullText() == ui_fixture::Library().front().title)
            game_title = label;
    });
    CheckStaticGameTitle(row, game_title, "library/game-title");
    CheckFocusedScrollInsets(row, "library/selected-row");
    brls::Application::giveFocus(row); Pump();
    Check(Text(preview).find(ui_fixture::Library().front().title) != std::string::npos, "focused Library game identity updates actual preview title");
    check_preview_cover(ui_fixture::Library().front().image_url);
    Key(brls::BUTTON_NAV_DOWN);
    Check(brls::Application::getCurrentFocus() != row, "Library down moves to another selectable row");
    Check(Text(preview).find(ui_fixture::Library().front().title) == std::string::npos, "Library preview follows changed focus identity");
    const auto* selected_row = brls::Application::getCurrentFocus();
    const auto games = ui_fixture::Library();
    const auto selected = std::find_if(games.begin(), games.end(), [&](const opennow::GameInfo& game) {
        return selected_row == root->getView("library-row-" + game.uuid);
    });
    Check(selected != games.end(), "native focused Library row maps to actual fixture identity");
    if (selected != games.end()) check_preview_cover(selected->image_url);
    Activate(Required(root, "library-sort"));
    Check(Text(Required(root, "library-sort")).find(opennow::Tr("Last Added")) != std::string::npos, "Library sort callback cycles to actual Last Added feed order");
    Tap(Required(root, "library-filter"));
    Check(Text(Required(root, "library-filter")).find("Steam") != std::string::npos, "native touch activates real Library store filter");
    for (int index = 0; index < 5; ++index) Key(brls::BUTTON_LT);
    Search(root, "Fixture game 1");
    Check(root->getView("library-row-fixture-1000") == nullptr, "real Library search removes unmatched row");
    Search(root, "");
    const int fetches = ui_fixture::Snapshot().library_requests;
    size_t count = 0;
    Walk(Required(root, "library-list"), [&count](brls::View* view) { count += dynamic_cast<opennow::LibraryRowView*>(view) != nullptr; });
    Check(count <= 15 && count > 0, "actual Library page bounds number of native row views");
    Activate(Required(root, "library-next"));
    Check(root->getView("library-row-fixture-1015") && !root->getView("library-row-fixture-1000"), "Library next exposes next actual feed slice without retaining prior rows");
    Activate(Required(root, "library-previous"));
    Check(root->getView("library-row-fixture-1000") && ui_fixture::Snapshot().library_requests == fetches, "Library previous restores game 1000 without another network fetch");
    brls::Application::giveFocus(Required(root, "library-row-fixture-1000")); Pump();
    const auto size = brls::Application::getActivitiesStack().size(); Key(brls::BUTTON_A);
    Check(dynamic_cast<opennow::GameDetailView*>(CurrentRoot()) != nullptr, "Library A opens actual production detail");
    Key(brls::BUTTON_B);
    Check(brls::Application::getActivitiesStack().size() == size, "Library detail B returns without quitting");
    Key(brls::BUTTON_LB);
    Check(root->getView("catalog") != nullptr, "real L action switches to Store");
    Key(brls::BUTTON_RB);
    Check(root->getView("library") && root->getView("library-row-fixture-1000"), "real R action returns to live Library with page identity");
    brls::Application::giveFocus(Required(root, "library-row-fixture-1000")); Pump();
    const auto original_language = opennow::GetInterfaceLanguage();
    const auto before_language = ui_fixture::Snapshot();
    SaveLanguageThroughSettings(root, original_language == "ru" ? "en" : "ru");
    SaveLanguageThroughSettings(root, original_language);
    const auto after_language = ui_fixture::Snapshot();
    Check(after_language.library_requests == before_language.library_requests &&
        after_language.catalog_requests == before_language.catalog_requests &&
        after_language.public_requests == before_language.public_requests &&
        after_language.region_requests == before_language.region_requests,
        "actual language Save and retained-tab refresh cause no new feed or region requests");
    const int before_caption_cycles = ui_fixture::Snapshot().library_requests;
    for (int sort = 0; sort < 4; ++sort)
    {
        for (int filter = 0; filter < 6; ++filter)
        {
            CheckLibraryToolbar(root);
            Key(brls::BUTTON_LT);
        }
        Key(brls::BUTTON_RT);
    }
    Check(ui_fixture::Snapshot().library_requests == before_caption_cycles, "all native toolbar caption states remain local without feed requests");
    CheckLibraryToolbar(root);
    brls::Application::giveFocus(Required(root, "library-row-fixture-1000")); Pump();
}
void SettingsChecks(brls::View* root, const std::string& capture)
{
    auto capture_category = [&](const std::string& category) {
        CheckSettingsActionRows(root, category);
        CheckChildren(dynamic_cast<brls::Box*>(root), "settings-" + category);
        CheckFonts(root); DrainToasts(); Screenshot(capture + ".settings-" + category + ".ppm");
    };
    capture_category("account");
    Activate(Required(root, "settings/category/stream"));
    Check(root->getView("settings/option/zortos-community-proxy") == nullptr,
        "retired community proxy is absent from the Stream settings page");
    auto* bitrate = Action(root, "Bitrate"); Activate(bitrate); Key(brls::BUTTON_X);
    Check(opennow::LoadStreamSettings().bitrate_kbps == 16000, "real A bitrate cycle and X Save persist 16 Mbps");
    Activate(bitrate); Key(brls::BUTTON_Y); Key(brls::BUTTON_X);
    Check(opennow::LoadStreamSettings().bitrate_kbps == 16000, "real Y Revert prevents unsaved bitrate persistence");
    capture_category("stream");
    Activate(Required(root, "settings/category/app"));
    auto* reminder = Action(root, "Queue notify at"); Activate(reminder); Key(brls::BUTTON_X);
    Check(opennow::LoadStreamSettings().queue_notify_threshold == 20, "queue reminder alone marks settings dirty and is saved by real X callback");
    Activate(reminder); Key(brls::BUTTON_Y); Key(brls::BUTTON_X);
    Check(opennow::LoadStreamSettings().queue_notify_threshold == 20, "reminder Y Revert retains saved value");
    capture_category("app");
    Activate(Required(root, "settings/category/preferences")); capture_category("preferences");
    for (const char* category : {"preferences", "account", "stream"}) Activate(Required(root, std::string("settings/category/") + category));
    brls::Application::giveFocus(Action(root, "Bitrate")); Pump();
    Check(!Text(Required(root, "settings/help")).empty(), "actual focused setting help is populated");
    Check(ui_fixture::Snapshot().region_requests == 0 && ui_fixture::Snapshot().latency_measurements == 0, "category construction and focused help do not trigger implicit region or latency requests");
}
void DetailChecks(opennow::GameDetailView* root)
{
    CheckVisibleActionRows(root, "detail/actions");
    auto* description = dynamic_cast<brls::ScrollingFrame*>(Required(root, "detail/description"));
    Check(description && description->getChildren().front()->getHeight() > description->getHeight(), "actual production description overflows bounded scroll viewport");
    brls::Application::giveFocus(Required(root, "detail/play")); Pump(); Key(brls::BUTTON_NAV_UP);
    Check(brls::Application::getCurrentFocus() == description, "detail Up routes from Play to real description scroller");
    SDL_Event scroll {};
    scroll.type = SDL_KEYDOWN; scroll.key.keysym.scancode = SDL_SCANCODE_DOWN; scroll.key.keysym.sym = SDLK_DOWN;
    SDL_PushEvent(&scroll); Pump(16); scroll.type = SDL_KEYUP; SDL_PushEvent(&scroll); Pump();
    Check(description->getContentOffsetY() > 0, "held native SDL Down scrolls actual overflow description");
    Activate(Action(root, "Play on GeForce NOW"));
    Check(ui_fixture::Snapshot().launches.size() == 1, "actual detail Play callback reaches launch boundary once");
    const auto call = ui_fixture::Snapshot().launches.front();
    Check(call.app_id == "1000" && call.store == "Steam" && call.user_id == "native-fixture-user" && call.title == ui_fixture::Library().front().title, "actual Play passes selected launch ID, Steam, current account identity and internal title");
    Until([] { return !ui_fixture::Snapshot().handed_off_sessions.empty(); }, "real queue worker reaches recording stream handoff");
    Check(ui_fixture::Snapshot().played_ids == std::vector<std::string>{"fixture-1000"}, "real queue worker records selected history identity");
    Check(ui_fixture::Snapshot().start_settings == ui_fixture::Snapshot().handoff_settings, "actual Play forwards complete request settings to stream handoff unchanged");
    Activate(Action(root, "Store")); Key(brls::BUTTON_B);
    Activate(Action(root, "Create Switch shortcut"));
    Until([] { return !ui_fixture::Snapshot().shortcuts.empty(); }, "actual shortcut worker reaches external shortcut boundary");
    const auto request = ui_fixture::Snapshot().shortcuts.front();
    Check(request.launch_app_id == "1000" && request.store == "Steam" && request.game_id == "fixture-1000", "actual shortcut request preserves launch/store/game IDs");
    if (CurrentRoot() != root) Key(brls::BUTTON_B);
    auto data = opennow::MakeLibraryGameDetail(ui_fixture::Library().front());
    data.game_id = "fixture-multi";
    data.variants.push_back({"2000", "Epic", "OWNED", {}, "AVAILABLE", true});
    ui_fixture::ClearLauncherPreference();
    auto* multi = new opennow::GameDetailView(opennow::GfnClient(), data);
    brls::Application::pushActivity(new brls::Activity(multi)); Pump();
    const size_t starts = ui_fixture::Snapshot().launches.size();
    Activate(Required(multi, "detail/play"));
    Check(dynamic_cast<brls::Dropdown*>(CurrentRoot()) != nullptr, "multi-variant Play opens actual native store selector");
    Key(brls::BUTTON_NAV_DOWN); Key(brls::BUTTON_A);
    Until([starts] { return ui_fixture::Snapshot().launches.size() > starts; }, "native selector dismissal launches selected variant");
    Check(ui_fixture::Snapshot().launches.back().app_id == "2000" && ui_fixture::Snapshot().launches.back().store == "Epic", "actual selector launches Epic variant, not default Steam variant");
    Check(ui_fixture::Snapshot().saved_variants.back() == "2000", "actual selector persists selected launcher preference");
    Until([starts] { return ui_fixture::Snapshot().handed_off_sessions.size() > starts; }, "selected variant completes real queue handoff");
    Key(brls::BUTTON_B);
    ui_fixture::ClearLauncherPreference();
    auto* guarded = new opennow::GameDetailView(opennow::GfnClient(), data);
    brls::Application::pushActivity(new brls::Activity(guarded)); Pump();
    Activate(Required(guarded, "detail/play")); Key(brls::BUTTON_NAV_DOWN);
    const size_t guarded_starts = ui_fixture::Snapshot().launches.size();
    const auto auth = *opennow::AppState::Instance().session();
    brls::Application::onControllerButtonPressed(brls::BUTTON_A, false);
    opennow::AppState::Instance().ActivateSession(auth); Pump(48);
    Check(ui_fixture::Snapshot().launches.size() == guarded_starts, "account change between selection and deferred dismiss prevents stale-account launch");
    Key(brls::BUTTON_B);
    description->setContentOffsetY(0, false); Pump();
}

void QueueChecks(const opennow::AuthSession& session, const std::string& capture)
{
    opennow::QueueDisplayState presentation {"Queue presentation fixture", "Checking your NVIDIA account", "Waiting for authorization", 123, 0, false};
    auto* presentation_view = new opennow::QueueView(presentation, opennow::LoadStreamSettings(), [] {}, [] {});
    brls::Application::pushActivity(new brls::Activity(presentation_view)); Pump();
    for (int stage : {0, 1, 2, 3})
    {
        presentation.stage = stage;
        presentation.status = stage == 0 ? "Checking your NVIDIA account" : stage == 1 ? "Waiting in queue..." : stage == 2 ? "Preparing your cloud rig" : "Starting the video stream";
        presentation_view->Update(presentation); Pump();
        const auto expected = stage == 1 ? brls::Visibility::VISIBLE : brls::Visibility::GONE;
        Check(Required(presentation_view, "queue/position")->getParent()->getVisibility() == expected,
            "queue number cards only occupy space during the actual queue phase");
        Check(Required(presentation_view, "queue/position-heading")->getVisibility() == expected,
            "position-in-queue heading is only visible with the actual number cards");
        CheckChildren(presentation_view, "queue-presentation-" + std::to_string(stage));
        Screenshot(capture + ".phase-" + std::to_string(stage) + ".ppm");
    }
    presentation.stage = 1; presentation.position = -1;
    presentation_view->Update(presentation); Pump();
    Check(Required(presentation_view, "queue/position")->getParent()->getVisibility() == brls::Visibility::GONE,
        "unknown queue position has no placeholder number card");
    presentation.position = 123; presentation.failed = true;
    presentation_view->Update(presentation); Pump();
    Check(Required(presentation_view, "queue/position")->getParent()->getVisibility() == brls::Visibility::GONE,
        "failed session hides stale queue number cards");
    brls::Application::popActivity(brls::TransitionAnimation::NONE); Pump();
    ui_fixture::SetSessionMode(ui_fixture::SessionMode::Waiting);
    opennow::LaunchSessionDialog(opennow::GfnClient(), session, "1000", "Native queue fixture", "Steam", "Native queue fixture", "fixture-1000", "fixture://cover/1000");
    Until([] { return opennow::GetCurrentQueuePosition() == 123; }, "real queue worker publishes actual three-digit position");
    Check(Text(Required(CurrentRoot(), "queue/position")).find("123") != std::string::npos, "native queue displays all three position digits");
    CheckChildren(dynamic_cast<brls::Box*>(CurrentRoot()), "queue"); CheckFonts(CurrentRoot());
    CheckVisibleActionRows(CurrentRoot(), "queue/actions"); Screenshot(capture + ".queue.ppm");
    Tap(Required(CurrentRoot(), "queue/minimize"));
    Check(opennow::IsQueueMinimized(), "actual Minimize dismisses UI without canceling worker");
    const auto original = opennow::LoadStreamSettings();
    auto changed = original;
    changed.bitrate_kbps = 25000; changed.queue_notify_threshold = 5; changed.region = "https://changed-region.fixture.invalid/";
    Check(opennow::SaveStreamSettings(changed), "fixture saves new settings while actual queue is minimized");
    const size_t starts_before_second = ui_fixture::Snapshot().launches.size();
    opennow::LaunchSessionDialog(opennow::GfnClient(), session, "9999", "Blocked second launch", "Steam"); Pump();
    Check(ui_fixture::Snapshot().launches.size() == starts_before_second, "second launch is blocked while actual queue exists");
    Check(Text(Required(CurrentRoot(), "queue/stream-summary")).find("12 Mbps") != std::string::npos &&
          Text(Required(CurrentRoot(), "queue/region")).find(opennow::ui::ConfiguredLocation(original.region)) != std::string::npos &&
          Text(Required(CurrentRoot(), "queue/reminder")).find("10") != std::string::npos, "restored actual queue retains original configured bitrate, region and reminder");
    Activate(Required(CurrentRoot(), "queue/minimize"));
    const int polls = ui_fixture::Snapshot().polls;
    Until([polls] { return ui_fixture::Snapshot().polls >= polls + 2; }, "real minimized queue keeps polling without another worker", 12);
    const auto notifications = ui_fixture::Snapshot().notifications;
    const auto reminder = opennow::Tr("Queue almost ready. Tap the Queue chip to return.");
    Check(std::count(notifications.begin(), notifications.end(), reminder) == 1, "actual minimized queue reminds once using original threshold despite changed saved settings");
    opennow::RestoreMinimizedQueueDialog(); Pump();
    Check(!opennow::IsQueueMinimized() && CurrentRoot()->getView("queue/position"), "actual restore reattaches native queue activity");
    Activate(Required(CurrentRoot(), "queue/cancel"));
    Until([] { return !ui_fixture::Snapshot().stopped_sessions.empty(); }, "actual queue cancellation stops adopted remote session");
    Check(opennow::GetCurrentQueuePosition() == -1 && !opennow::IsQueueMinimized(), "cancel clears active queue ownership");
    ui_fixture::SetSessionMode(ui_fixture::SessionMode::BlockStart);
    const size_t before = ui_fixture::Snapshot().launches.size(), stops = ui_fixture::Snapshot().stopped_sessions.size();
    opennow::LaunchSessionDialog(opennow::GfnClient(), session, "1001", "Cancel-before-adopt fixture", "Epic");
    Until([before] { return ui_fixture::Snapshot().launches.size() > before; }, "actual start worker reaches controlled external gate");
    Activate(Required(CurrentRoot(), "queue/cancel")); ui_fixture::ReleaseStart();
    Until([stops] { return ui_fixture::Snapshot().stopped_sessions.size() > stops; }, "session returned after cancel is stopped instead of adopted");
    Check(ui_fixture::Snapshot().handed_off_sessions.empty(), "canceled queue never hands off a stream");
    opennow::SaveStreamSettings(original);
    for (const auto mode : {ui_fixture::SessionMode::Unknown, ui_fixture::SessionMode::Patching, ui_fixture::SessionMode::Confirmation, ui_fixture::SessionMode::Failure})
    {
        ui_fixture::SetSessionMode(mode);
        opennow::LaunchSessionDialog(opennow::GfnClient(), session, "1000", "Actual phase fixture", "Steam"); Pump(48);
        if (mode == ui_fixture::SessionMode::Unknown)
            Check(Text(Required(CurrentRoot(), "queue/position")).find(opennow::Tr("Unknown")) != std::string::npos, "real queue with unavailable position stays unknown");
        else if (mode == ui_fixture::SessionMode::Patching)
            Check(Text(Required(CurrentRoot(), "queue/status")).find(opennow::Tr("Updating the game")) != std::string::npos, "actual patching state takes priority over positive queue position");
        else if (mode == ui_fixture::SessionMode::Confirmation)
            Check(Text(CurrentRoot()).find(opennow::Tr("Waiting for NVIDIA session ads or confirmation...")) != std::string::npos, "actual confirmation state takes priority over positive queue position");
        else { CheckChildren(dynamic_cast<brls::Box*>(CurrentRoot()), "queue-failure"); Check(opennow::GetCurrentQueuePosition() == -1, "actual failed queue relinquishes running ownership"); }
        Check(Required(CurrentRoot(), "queue/position")->getParent()->getVisibility() == brls::Visibility::GONE,
            "unknown, patching, confirmation and failure states never render queue number cards");
        Activate(Required(CurrentRoot(), "queue/cancel"));
    }
    ui_fixture::SetSessionMode(ui_fixture::SessionMode::BlockStart);
    const size_t epoch_starts = ui_fixture::Snapshot().launches.size(), epoch_stops = ui_fixture::Snapshot().stopped_sessions.size();
    opennow::LaunchSessionDialog(opennow::GfnClient(), session, "1002", "Account guard fixture", "Steam");
    Until([epoch_starts] { return ui_fixture::Snapshot().launches.size() > epoch_starts; }, "account-guarded worker reaches controlled start boundary");
    auto replacement = session; replacement.user.user_id = "replacement-native-fixture-user";
    opennow::AppState::Instance().ActivateSession(replacement); ui_fixture::ReleaseStart();
    Until([epoch_stops] { return ui_fixture::Snapshot().stopped_sessions.size() > epoch_stops; }, "actual account-generation guard cancels and cleans up prior account session");
    Check(ui_fixture::Snapshot().handed_off_sessions.empty(), "prior-account queue never hands off a stream");
    opennow::AppState::Instance().ActivateSession(session); opennow::SaveStreamSettings(original);
    ui_fixture::SetSessionMode(ui_fixture::SessionMode::BlockStart);
    const size_t frozen_starts = ui_fixture::Snapshot().launches.size(), frozen_handoffs = ui_fixture::Snapshot().handed_off_sessions.size();
    opennow::LaunchSessionDialog(opennow::GfnClient(), session, "1003", "Frozen settings handoff fixture", "Steam");
    Until([frozen_starts] { return ui_fixture::Snapshot().launches.size() > frozen_starts; }, "gated actual request captures explicit stream settings");
    Activate(Required(CurrentRoot(), "queue/minimize"));
    Check(opennow::SaveStreamSettings(changed), "saved settings change while real start request is gated and minimized");
    opennow::RestoreMinimizedQueueDialog(); Pump();
    Check(Text(Required(CurrentRoot(), "queue/stream-summary")).find("12 Mbps") != std::string::npos, "restored gated queue still displays original configuration");
    Screenshot(capture + ".restored.ppm"); ui_fixture::ReleaseStart();
    Until([frozen_handoffs] { return ui_fixture::Snapshot().handed_off_sessions.size() > frozen_handoffs; }, "gated real worker completes actual stream handoff");
    Check(ui_fixture::Snapshot().start_settings.back() == original && ui_fixture::Snapshot().handoff_settings.back() == original, "StartSession and PresentCloudStream receive same entire frozen StreamSettings after saved-settings change");
    Check(opennow::LoadStreamSettings() == changed, "handoff does not roll back independently saved next-stream settings");
    ui_fixture::SetSessionMode(ui_fixture::SessionMode::BlockStart);
    const size_t covered_starts = ui_fixture::Snapshot().launches.size(), covered_handoffs = ui_fixture::Snapshot().handed_off_sessions.size();
    const size_t original_stack = brls::Application::getActivitiesStack().size();
    opennow::LaunchSessionDialog(opennow::GfnClient(), session, "1004", "Covered queue handoff fixture", "Steam");
    Until([covered_starts] { return ui_fixture::Snapshot().launches.size() > covered_starts; }, "covered queue worker reaches external gate");
    auto* dialog = new brls::Dialog("Native dialog covering the production queue");
    dialog->addButton("Close", [] {}); dialog->setCancelable(true); dialog->open(); Pump(); ui_fixture::ReleaseStart();
    Until([covered_handoffs] { return ui_fixture::Snapshot().handed_off_sessions.size() > covered_handoffs; }, "covered real queue hands off exactly when ready");
    Check(CurrentRoot() == dialog && brls::Application::getActivitiesStack().size() == original_stack + 2, "covered queue readiness does not pop unrelated top dialog");
    Check(ui_fixture::Snapshot().handed_off_sessions.size() == covered_handoffs + 1, "covered queue invokes actual handoff callback exactly once");
    Key(brls::BUTTON_B);
    Until([original_stack] { return brls::Application::getActivitiesStack().size() == original_stack; }, "closing cover dialog removes stale queue on resume");
}
void OverlayChecks()
{
    opennow::StreamOverlaySample sample;
    const auto unknown = opennow::FormatStreamOverlay(sample);
    Check(unknown.metrics[2] == opennow::Tr("Unknown") && unknown.metrics[3] == opennow::Tr("Unknown"), "real overlay formatter keeps unavailable RTT and zero-packet loss unknown");
    sample.game_title = "A long live game title Жї 玩家"; sample.provider = "GeForce NOW";
    sample.presented_fps = 60; sample.incoming_fps = 60; sample.decoded_fps = 59; sample.bitrate_mbps = 15.8;
    sample.rtt_ms = 18; sample.width = 1280; sample.height = 720; sample.decode_us_p95 = 6100; sample.render_us_p95 = 2400;
    sample.queue_size = 1; sample.queue_high_water = 4; sample.packets_received = 9998; sample.sequence_gaps = 2;
    sample.codec = "H.264"; sample.location = "Auto"; sample.network = "Wi-Fi 5 GHz"; sample.peer_connected = true;
    auto* view = new opennow::StreamOverlayView(); view->Update(sample); view->UpdateElapsed(2538);
    std::printf("OVERLAY loss=%s decode=%s render=%s queue=%s elapsed=%s\n", view->Display().metrics[3].c_str(), view->Display().details[2].c_str(), view->Display().details[3].c_str(), view->Display().details[4].c_str(), view->Display().elapsed.c_str());
    Check(view->Display().metrics[3] == "0.02", "actual overlay loss estimate uses received plus sequence gaps denominator");
    Check(view->Display().details[2] == "6.1 ms" && view->Display().details[3] == "2.4 ms", "actual overlay preserves measured p95 fractions");
    Check(view->Display().details[4] == "1 / 4" && view->Display().elapsed == "42:18", "actual overlay formats queue high water and elapsed time");
    view->setDimensions(1280, 720); brls::Application::pushActivity(new brls::Activity(view)); Pump();
}
}

int main(int argc, char** argv)
{
    try
    {
        const std::string language = argc > 1 ? argv[1] : "en", account = argc > 2 ? argv[2] : "Player";
        const std::string capture = argc > 3 ? argv[3] : "native.ppm", scenario = argc > 5 ? argv[5] : "library";
        capture_path = capture;
        const int physical_width = argc > 4 ? std::stoi(argv[4]) : 1280;
        const bool guest = account == "guest", empty = scenario == "empty" || scenario == "empty-store";
        ui_fixture::Reset(capture + ".storage", empty);
        opennow::StreamSettings initial; initial.interface_language = language;
        Check(opennow::SaveStreamSettings(initial), "real settings fixture initializes isolated scratch persistence");
        opennow::SetInterfaceLanguage(language); brls::Logger::setLogLevel(brls::LogLevel::LOG_ERROR);
        if (!brls::Application::init()) return 2;
        brls::Application::createWindow("OpenNOW production native UI tests"); opennow::ui::InitializeThemeAndFonts();
        brls::Application::setGlobalQuit(false);
        if (physical_width == 1920)
        {
            SDL_SetWindowSize(SDL_GL_GetCurrentWindow(), 1920, 1080);
            int width, height; SDL_GL_GetDrawableSize(SDL_GL_GetCurrentWindow(), &width, &height);
            glViewport(0, 0, width, height); brls::Application::setWindowSize(1920, 1080);
        }
        opennow::AuthSession session;
        session.user.user_id = "native-fixture-user"; session.user.display_name = account;
        session.user.membership_tier = "Ultimate"; session.user.membership_tier_verified = true;
        session.subscription.available = true; session.subscription.remaining_hours = 100.5;
        session.subscription.has_storage = true; session.subscription.storage_size_gb = 1000;
        auto& state = opennow::AppState::Instance();
        if (!guest) state.SetSession(session); else state.MarkSessionLoaded();
        state.SetLibraryGames(ui_fixture::Library());
        auto* frame = new opennow::TopBarFrame();
        frame->addTab("Store", [] { return new opennow::CatalogTab(); });
        frame->addTab("Library", [] { return new opennow::LibraryTab(); });
        frame->addTab("Settings", [] { return new opennow::SettingsTab(); });
        frame->focusTab(scenario == "store" || scenario == "empty-store" ? 0 : scenario == "settings" ? 2 : 1);
        brls::Application::pushActivity(new brls::Activity(frame)); Pump();
        int width, height, viewport[4] {}; SDL_GL_GetDrawableSize(SDL_GL_GetCurrentWindow(), &width, &height); glGetIntegerv(GL_VIEWPORT, viewport);
        Check(width == physical_width && height == physical_width * 9 / 16, "native drawable reaches requested resolution");
        Check(viewport[2] == width && viewport[3] == height, "OpenGL viewport covers drawable");
        Check(brls::Application::windowWidth == static_cast<unsigned>(width) && brls::Application::windowHeight == static_cast<unsigned>(height), "physical dimensions match drawable");
        Check(brls::Application::contentWidth == 1280 && brls::Application::contentHeight == 720, "layout remains logical 1280x720");
        CheckChildren(frame, "production");
        const auto header = Required(frame, "shell-header")->getFrame(), footer = Required(frame, "shell-footer")->getFrame();
        Check(header.getHeight() == 76 && footer.getHeight() == 60 && footer.getMinY() == 660, "production shell retains 76-pixel header and single 60-pixel footer at y660");
        Walk(Required(frame, "shell-tabs"), [](brls::View* view) {
            if (auto* label = dynamic_cast<brls::Label*>(view)) Check(label->isSingleLine() && label->getHeight() <= 20, "production tab label stays on one line");
        });
        CheckFonts(frame);
        CheckVisibleActionRows(frame, "initial-actions");
        opennow::StreamSettings fractional; fractional.bitrate_kbps = 12345;
        Check(opennow::ui::StreamSummary(fractional).find("12.345") != std::string::npos, "configured stream summary preserves 12345 kbps");
        fractional.bitrate_kbps = 12500;
        Check(opennow::ui::StreamSummary(fractional).find("12.5") != std::string::npos, "configured stream summary preserves 12500 kbps");
        fractional.bitrate_kbps = 12000;
        Check(opennow::ui::StreamSummary(fractional).find("12 Mbps") != std::string::npos, "configured stream summary preserves integer 12000 kbps");
        if (scenario == "store" || scenario == "empty-store") StoreChecks(frame, empty, guest);
        else if (scenario == "settings") SettingsChecks(frame, capture);
        else if (scenario == "queue") { DrainToasts(); QueueChecks(session, capture); }
        else if (scenario == "overlay") OverlayChecks();
        else if (scenario == "detail")
        {
            auto data = opennow::MakeLibraryGameDetail(ui_fixture::Library().front());
            auto* detail = new opennow::GameDetailView(opennow::GfnClient(), std::move(data));
            brls::Application::pushActivity(new brls::Activity(detail)); Pump();
            CheckChildren(detail, "detail"); CheckFonts(detail); DetailChecks(detail);
        }
        else LibraryChecks(frame, guest, empty);
        DrainToasts(); Screenshot(capture);
        Check(dynamic_cast<brls::Dialog*>(CurrentRoot()) == nullptr && dynamic_cast<brls::Dropdown*>(CurrentRoot()) == nullptr, "production callbacks leave no unexpected modal activity");
        brls::Application::quit(); while (brls::Application::mainLoop()) {}
    }
    catch (const std::exception& error)
    {
        Check(false, std::string("native harness exception: ") + error.what());
        if (argc > 3 && CurrentRoot()) Screenshot(std::string(argv[3]) + ".failure.ppm");
    }
    std::printf("RESULT failures=%d\n", failures);
    return failures ? 1 : 0;
}
