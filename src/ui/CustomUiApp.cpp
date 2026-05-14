#include "opennow/ui/CustomUiApp.hpp"

#include "opennow/core/Settings.hpp"
#include "opennow/gfn/Auth.hpp"
#include "opennow/gfn/AuthService.hpp"
#include "opennow/gfn/AuthSessionStore.hpp"
#include "opennow/gfn/Catalog.hpp"
#include "opennow/gfn/CloudMatch.hpp"
#include "opennow/input/ControllerMapper.hpp"
#include "opennow/media/NativeMediaPipeline.hpp"
#include "opennow/media/VideoFrameStore.hpp"
#include "opennow/net/CurlHttpClient.hpp"
#include "opennow/util/Json.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <switch.h>
#include <unistd.h>
#include <sys/stat.h>

#include <GLFW/glfw3.h>
#include <GLES2/gl2.h>

#define NANOVG_GLES2_IMPLEMENTATION
#include <nanovg.h>
#include <nanovg_gl.h>

namespace opennow::ui {
namespace {

constexpr float kUiW = 1280.0f;
constexpr float kUiH = 720.0f;

int gNxlinkFd = -1;

struct LoadedImage {
    int handle = 0;
    int width = 0;
    int height = 0;
    bool queued = false;
    bool loading = false;
    bool failed = false;
};

struct DownloadedImage {
    std::string url;
    std::string bytes;
    bool ok = false;
};

struct SubscriptionInfo {
    bool loaded = false;
    bool ok = false;
    bool unlimited = false;
    bool gameplayAllowed = true;
    double allottedHours = 0.0;
    double purchasedHours = 0.0;
    double rolledOverHours = 0.0;
    double usedHours = 0.0;
    double remainingHours = 0.0;
    double totalHours = 0.0;
    std::string tier = "FREE";
    std::string type;
    std::string subType;
    std::string state;
    std::string periodStart;
    std::string periodEnd;
    std::string error;
};

struct StreamLaunchState {
    bool active = false;
    bool inFlight = false;
    bool ready = false;
    bool failed = false;
    bool stopRequested = false;
    std::string stage = "Idle";
    std::string message;
    std::string gameTitle;
    std::string appId;
    std::string sessionId;
    std::string serverIp;
    std::string signalingUrl;
    std::string mediaMessage;
    std::uint32_t status = 0;
    std::uint32_t queuePosition = 0;
};

enum class Screen {
    Home,
    Library,
    Store,
    Search,
    Settings,
    Details,
    Stream,
};

enum class SettingsSection {
    General,
    Account,
    Display,
    Logs,
    About,
};

struct UiState {
    NVGcontext* vg = nullptr;
    net::CurlHttpClient http;
    std::map<std::string, LoadedImage> images;
    std::deque<std::string> imageQueue;
    std::vector<DownloadedImage> completedImages;
    std::mutex imageMutex;
    std::condition_variable imageCv;
    std::thread imageThread;
    bool imageWorkerStop = false;
    std::thread launchThread;
    std::mutex launchMutex;
    StreamLaunchState launch;
    int streamVideoImage = 0;
    std::uint64_t streamVideoFrameId = 0;
    int streamVideoWidth = 0;
    int streamVideoHeight = 0;
    std::mutex logsMutex;
    std::size_t logsScroll = 0;
    std::vector<gfn::CatalogGame> games;
    AuthSession session;
    bool signedIn = false;
    bool authPending = false;
    bool authPopupVisible = false;
    bool loading = false;
    bool libraryMode = true;
    bool showLogs = false;
    Screen screen = Screen::Library;
    Screen previousScreen = Screen::Library;
    SettingsSection settingsSection = SettingsSection::General;
    StreamSettings streamSettings;
    std::size_t resolutionIndex = 0;
    std::size_t selected = 0;
    float shelfScroll = 0.0f;
    float swipeStartX = -1.0f;
    float swipeStartY = -1.0f;
    float lastTouchX = -1.0f;
    float lastTouchY = -1.0f;
    float swipeOffset = 0.0f;
    bool touchWasDown = false;
    bool mouseWasDown = false;
    std::string status = "Ready";
    std::string userCode;
    std::string verificationUrl = "https://login.nvidia.com/activate";
    LoginProvider pollProvider;
    std::string pollDeviceCode;
    std::uint64_t nextPollMs = 0;
    std::uint64_t pollDeadlineMs = 0;
    std::uint32_t pollInterval = 5;
    std::string vpcId = "NP-AMS-08";
    SubscriptionInfo subscription;
    std::vector<std::string> logs;
};

struct HitRect {
    float x = 0;
    float y = 0;
    float w = 0;
    float h = 0;
};

bool hit(const HitRect& rect, float x, float y) {
    return x >= rect.x && x <= rect.x + rect.w && y >= rect.y && y <= rect.y + rect.h;
}

std::uint64_t nowMs() {
    return gfn::unixTimeMs();
}

void log(UiState& ui, const std::string& message) {
    std::fprintf(stdout, "%s\n", message.c_str());
    std::fflush(stdout);
    std::fprintf(stderr, "%s\n", message.c_str());
    std::fflush(stderr);
    (void)::mkdir("sdmc:/switch", 0777);
    (void)::mkdir("sdmc:/switch/OpenNOW", 0777);
    if (FILE* file = std::fopen("sdmc:/switch/OpenNOW/runtime.log", "ab")) {
        std::fprintf(file, "%llu %s\n", static_cast<unsigned long long>(nowMs()), message.c_str());
        std::fflush(file);
        std::fclose(file);
    }
    if (gNxlinkFd >= 0) {
        const auto line = message + "\n";
        (void)::write(gNxlinkFd, line.data(), line.size());
    }
    std::lock_guard<std::mutex> lock(ui.logsMutex);
    ui.logs.push_back(message);
    if (ui.logs.size() > 80) {
        ui.logs.erase(ui.logs.begin(), ui.logs.begin() + static_cast<std::ptrdiff_t>(ui.logs.size() - 80));
        ui.logsScroll = std::min<std::size_t>(ui.logsScroll, ui.logs.size() > 13 ? ui.logs.size() - 13 : 0);
    }
}

std::vector<std::string> logSnapshot(UiState& ui) {
    std::lock_guard<std::mutex> lock(ui.logsMutex);
    return ui.logs;
}

std::string accountName(const UiState& ui) {
    if (!ui.signedIn) {
        return "Not signed in";
    }
    if (!ui.session.user.displayName.empty()) {
        return ui.session.user.displayName;
    }
    if (!ui.session.user.userId.empty()) {
        return ui.session.user.userId;
    }
    return "Signed in";
}

std::string membership(const UiState& ui) {
    if (!ui.signedIn) {
        return "OFFLINE";
    }
    if (ui.subscription.ok && !ui.subscription.tier.empty()) {
        return ui.subscription.tier;
    }
    return ui.session.user.membershipTier.empty() ? "FREE" : ui.session.user.membershipTier;
}

std::string formatHours(double hours) {
    if (!std::isfinite(hours) || hours <= 0.0) {
        return "0h";
    }
    const int totalMinutes = static_cast<int>(std::round(hours * 60.0));
    const int wholeHours = totalMinutes / 60;
    const int minutes = totalMinutes % 60;
    if (minutes == 0) {
        return std::to_string(wholeHours) + "h";
    }
    return std::to_string(wholeHours) + "h " + (minutes < 10 ? "0" : "") + std::to_string(minutes) + "m";
}

std::string subscriptionTimeLabel(const UiState& ui) {
    if (!ui.signedIn) {
        return "Sign in";
    }
    if (!ui.subscription.loaded) {
        return "Checking time";
    }
    if (!ui.subscription.ok) {
        return "Time unknown";
    }
    return ui.subscription.unlimited ? "Unlimited time" : formatHours(ui.subscription.remainingHours) + " left";
}

std::string primaryStore(const gfn::CatalogGame& game) {
    for (const auto& variant : game.variants) {
        if (!variant.store.empty()) {
            return variant.store;
        }
    }
    return "GFN";
}

std::string readableStatus(const gfn::CatalogGame& game) {
    for (const auto& variant : game.variants) {
        if (!variant.libraryStatus.empty() && variant.libraryStatus != "NOT_OWNED") {
            return "READY TO PLAY";
        }
    }
    if (game.playabilityState == "PLAYABLE" || game.playabilityState == "STREAMABLE" || game.playabilityState == "READY_TO_PLAY") {
        return "READY TO PLAY";
    }
    if (game.playabilityState.empty()) {
        return "AVAILABLE";
    }
    auto status = game.playabilityState;
    std::replace(status.begin(), status.end(), '_', ' ');
    return status;
}

std::string deckLine(const gfn::CatalogGame& game) {
    return primaryStore(game) + " | " + readableStatus(game);
}

std::string shortText(const std::string& value, std::size_t maxChars) {
    if (value.size() <= maxChars) {
        return value;
    }
    if (maxChars <= 3) {
        return value.substr(0, maxChars);
    }
    return value.substr(0, maxChars - 3) + "...";
}

const gfn::CatalogGame* selectedGame(const UiState& ui) {
    if (ui.games.empty() || ui.selected >= ui.games.size()) {
        return nullptr;
    }
    return &ui.games[ui.selected];
}

bool isNumericString(const std::string& value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return c >= '0' && c <= '9';
    });
}

std::string selectedLaunchAppId(const gfn::CatalogGame& game) {
    for (const auto& variant : game.variants) {
        if (variant.selected && isNumericString(variant.id)) {
            return variant.id;
        }
    }
    for (const auto& variant : game.variants) {
        if (!variant.libraryStatus.empty() && variant.libraryStatus != "NOT_OWNED" && isNumericString(variant.id)) {
            return variant.id;
        }
    }
    for (const auto& variant : game.variants) {
        if (isNumericString(variant.id)) {
            return variant.id;
        }
    }
    return isNumericString(game.id) ? game.id : "";
}

StreamLaunchState launchSnapshot(UiState& ui) {
    std::lock_guard<std::mutex> lock(ui.launchMutex);
    return ui.launch;
}

void updateLaunch(UiState& ui, const StreamLaunchState& state) {
    std::lock_guard<std::mutex> lock(ui.launchMutex);
    const bool stopRequested = ui.launch.stopRequested;
    ui.launch = state;
    ui.launch.stopRequested = ui.launch.stopRequested || stopRequested;
}

std::uint64_t nowUs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

float normalizeStickAxis(s32 value) {
    return std::clamp(static_cast<float>(value) / 32767.0f, -1.0f, 1.0f);
}

input::ControllerState controllerStateFromPad(PadState& pad) {
    padUpdate(&pad);
    const auto buttons = padGetButtons(&pad);
    const auto left = padGetStickPos(&pad, 0);
    const auto right = padGetStickPos(&pad, 1);

    input::ControllerState state;
    state.a = (buttons & HidNpadButton_A) != 0;
    state.b = (buttons & HidNpadButton_B) != 0;
    state.x = (buttons & HidNpadButton_X) != 0;
    state.y = (buttons & HidNpadButton_Y) != 0;
    state.dpadUp = (buttons & HidNpadButton_Up) != 0;
    state.dpadDown = (buttons & HidNpadButton_Down) != 0;
    state.dpadLeft = (buttons & HidNpadButton_Left) != 0;
    state.dpadRight = (buttons & HidNpadButton_Right) != 0;
    state.plus = (buttons & HidNpadButton_Plus) != 0;
    state.minus = (buttons & HidNpadButton_Minus) != 0;
    state.l = (buttons & HidNpadButton_L) != 0;
    state.r = (buttons & HidNpadButton_R) != 0;
    state.zl = (buttons & HidNpadButton_ZL) != 0;
    state.zr = (buttons & HidNpadButton_ZR) != 0;
    state.leftStickPress = (buttons & HidNpadButton_StickL) != 0;
    state.rightStickPress = (buttons & HidNpadButton_StickR) != 0;
    state.leftX = normalizeStickAxis(left.x);
    state.leftY = normalizeStickAxis(left.y);
    state.rightX = normalizeStickAxis(right.x);
    state.rightY = normalizeStickAxis(right.y);
    state.leftTrigger = state.zl ? 1.0f : 0.0f;
    state.rightTrigger = state.zr ? 1.0f : 0.0f;
    return state;
}

void patchLaunch(UiState& ui, const std::string& stage, const std::string& message) {
    std::lock_guard<std::mutex> lock(ui.launchMutex);
    ui.launch.stage = stage;
    ui.launch.message = message;
}

bool launchStopRequested(UiState& ui) {
    std::lock_guard<std::mutex> lock(ui.launchMutex);
    return ui.launch.stopRequested;
}

void requestLaunchStop(UiState& ui) {
    std::lock_guard<std::mutex> lock(ui.launchMutex);
    ui.launch.stopRequested = true;
    if (ui.launch.inFlight) {
        ui.launch.stage = "Stopping";
        ui.launch.message = "Stopping the GeForce NOW session.";
    }
}

std::string heroUrl(const gfn::CatalogGame& game) {
    if (!game.heroImageUrl.empty()) return game.heroImageUrl;
    if (!game.bannerImageUrl.empty()) return game.bannerImageUrl;
    if (!game.keyArtUrl.empty()) return game.keyArtUrl;
    return game.boxArtUrl;
}

std::string cardUrl(const gfn::CatalogGame& game) {
    if (!game.boxArtUrl.empty()) return game.boxArtUrl;
    if (!game.keyArtUrl.empty()) return game.keyArtUrl;
    if (!game.bannerImageUrl.empty()) return game.bannerImageUrl;
    return game.heroImageUrl;
}

void enqueueImage(UiState& ui, const std::string& url) {
    if (url.empty()) {
        return;
    }
    auto& image = ui.images[url];
    if (image.handle || image.failed || image.queued || image.loading) {
        return;
    }
    image.queued = true;
    {
        std::lock_guard<std::mutex> lock(ui.imageMutex);
        ui.imageQueue.push_back(url);
    }
    ui.imageCv.notify_one();
}

LoadedImage& loadImage(UiState& ui, const std::string& url) {
    auto& image = ui.images[url];
    if (url.empty() || image.handle || image.failed) {
        return image;
    }
    enqueueImage(ui, url);
    return image;
}

void imageWorkerLoop(UiState* ui) {
    net::CurlHttpClient http;
    for (;;) {
        std::string url;
        {
            std::unique_lock<std::mutex> lock(ui->imageMutex);
            ui->imageCv.wait(lock, [&] { return ui->imageWorkerStop || !ui->imageQueue.empty(); });
            if (ui->imageWorkerStop) {
                return;
            }
            url = std::move(ui->imageQueue.front());
            ui->imageQueue.pop_front();
        }

        DownloadedImage result;
        result.url = url;
        try {
            const auto response = http.send({
                net::HttpMethod::Get,
                url,
                {
                    {"Accept", "image/avif,image/webp,image/apng,image/svg+xml,image/*,*/*;q=0.8"},
                    {"User-Agent", "Mozilla/5.0 (X11; Linux x86_64; Steam Deck) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36"},
                },
                "",
            });
            result.ok = response.status >= 200 && response.status < 300 && !response.body.empty();
            if (result.ok) {
                result.bytes = std::move(response.body);
            }
        } catch (...) {
            result.ok = false;
        }

        {
            std::lock_guard<std::mutex> lock(ui->imageMutex);
            ui->completedImages.push_back(std::move(result));
        }
    }
}

void processImageCompletions(UiState& ui, std::size_t maxPerFrame = 3) {
    std::vector<DownloadedImage> completed;
    {
        std::lock_guard<std::mutex> lock(ui.imageMutex);
        const auto count = std::min(maxPerFrame, ui.completedImages.size());
        completed.insert(completed.end(), ui.completedImages.begin(), ui.completedImages.begin() + static_cast<std::ptrdiff_t>(count));
        ui.completedImages.erase(ui.completedImages.begin(), ui.completedImages.begin() + static_cast<std::ptrdiff_t>(count));
    }

    for (auto& item : completed) {
        auto& image = ui.images[item.url];
        image.loading = false;
        image.queued = false;
        if (!item.ok || item.bytes.empty()) {
            image.failed = true;
            continue;
        }
        image.handle = nvgCreateImageMem(
            ui.vg,
            0,
            reinterpret_cast<unsigned char*>(item.bytes.data()),
            static_cast<int>(item.bytes.size()));
        if (image.handle == 0) {
            image.failed = true;
            continue;
        }
        nvgImageSize(ui.vg, image.handle, &image.width, &image.height);
    }
}

void prefetchVisibleImages(UiState& ui) {
    if (ui.games.empty()) {
        return;
    }
    const auto selected = std::min(ui.selected, ui.games.size() - 1);
    enqueueImage(ui, heroUrl(ui.games[selected]));
    const std::size_t start = selected > 4 ? selected - 4 : 0;
    const std::size_t end = std::min(ui.games.size(), selected + 10);
    for (std::size_t i = start; i < end; ++i) {
        enqueueImage(ui, cardUrl(ui.games[i]));
    }
}

NVGcolor rgb(int r, int g, int b, int a = 255) {
    return nvgRGBA(r, g, b, a);
}

void text(UiState& ui, float x, float y, float size, NVGcolor color, const std::string& value, int align = NVG_ALIGN_LEFT | NVG_ALIGN_TOP) {
    nvgFontFace(ui.vg, "switch");
    nvgFontSize(ui.vg, size);
    nvgFillColor(ui.vg, color);
    nvgTextAlign(ui.vg, align);
    nvgText(ui.vg, x, y, value.c_str(), nullptr);
}

void textBox(UiState& ui, float x, float y, float width, float size, NVGcolor color, const std::string& value, int align = NVG_ALIGN_LEFT | NVG_ALIGN_TOP) {
    nvgFontFace(ui.vg, "switch");
    nvgFontSize(ui.vg, size);
    nvgFillColor(ui.vg, color);
    nvgTextAlign(ui.vg, align);
    nvgTextBox(ui.vg, x, y, width, value.c_str(), nullptr);
}

void rounded(UiState& ui, float x, float y, float w, float h, float r, NVGcolor color) {
    nvgBeginPath(ui.vg);
    nvgRoundedRect(ui.vg, x, y, w, h, r);
    nvgFillColor(ui.vg, color);
    nvgFill(ui.vg);
}

void strokeRounded(UiState& ui, float x, float y, float w, float h, float r, NVGcolor color, float width) {
    nvgBeginPath(ui.vg);
    nvgRoundedRect(ui.vg, x, y, w, h, r);
    nvgStrokeColor(ui.vg, color);
    nvgStrokeWidth(ui.vg, width);
    nvgStroke(ui.vg);
}

void button(UiState& ui, float x, float y, float w, float h, const std::string& label, bool active = false) {
    rounded(ui, x, y, w, h, 10, active ? rgb(74, 232, 117) : rgb(31, 37, 47, 240));
    text(ui, x + w * 0.5f, y + h * 0.5f - 10.0f, 18, active ? rgb(4, 18, 10) : rgb(235, 240, 249), label, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
}

void drawImageCover(UiState& ui, int handle, float x, float y, float w, float h, float alpha = 1.0f) {
    if (!handle) {
        return;
    }
    int iw = 0;
    int ih = 0;
    nvgImageSize(ui.vg, handle, &iw, &ih);
    if (iw <= 0 || ih <= 0) {
        return;
    }
    const float scale = std::max(w / static_cast<float>(iw), h / static_cast<float>(ih));
    const float sw = static_cast<float>(iw) * scale;
    const float sh = static_cast<float>(ih) * scale;
    const float sx = x + (w - sw) * 0.5f;
    const float sy = y + (h - sh) * 0.5f;
    const auto paint = nvgImagePattern(ui.vg, sx, sy, sw, sh, 0.0f, handle, alpha);
    nvgBeginPath(ui.vg);
    nvgRect(ui.vg, x, y, w, h);
    nvgFillPaint(ui.vg, paint);
    nvgFill(ui.vg);
}

void drawLogo(UiState& ui) {
    rounded(ui, 40, 36, 36, 36, 8, rgb(74, 232, 117));
    text(ui, 51, 43, 22, rgb(4, 18, 11), "Z");
    text(ui, 88, 38, 26, rgb(245, 248, 252), "OpenNOW");
}

void navItem(UiState& ui, float x, const std::string& label, Screen screen) {
    const bool active = ui.screen == screen || (ui.screen == Screen::Details && ui.previousScreen == screen);
    text(ui, x, 47, 18, active ? rgb(245, 248, 252) : rgb(190, 196, 208), label, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
    if (active) {
        rounded(ui, x - 44, 72, 88, 3, 1.5f, rgb(74, 232, 117));
    }
}

Screen nextScreen(Screen screen, int delta) {
    if (screen == Screen::Details) {
        screen = Screen::Library;
    }
    constexpr Screen screens[] = {
        Screen::Home,
        Screen::Library,
        Screen::Store,
        Screen::Search,
        Screen::Settings,
    };
    int index = 0;
    for (int i = 0; i < static_cast<int>(sizeof(screens) / sizeof(screens[0])); ++i) {
        if (screens[i] == screen) {
            index = i;
            break;
        }
    }
    const int count = static_cast<int>(sizeof(screens) / sizeof(screens[0]));
    index = (index + delta + count) % count;
    return screens[index];
}

void drawTopNav(UiState& ui) {
    drawLogo(ui);
    navItem(ui, 410, "Home", Screen::Home);
    navItem(ui, 520, "Library", Screen::Library);
    navItem(ui, 630, "Store", Screen::Store);
    navItem(ui, 740, "Search", Screen::Search);
    navItem(ui, 865, "Settings", Screen::Settings);
    rounded(ui, 1016, 38, 122, 28, 14, rgb(11, 31, 18, 220));
    strokeRounded(ui, 1016, 38, 122, 28, 14, rgb(74, 232, 117), 1.2f);
    text(ui, 1034, 44, 14, rgb(218, 255, 228), subscriptionTimeLabel(ui));
    rounded(ui, 1160, 34, 36, 36, 8, rgb(245, 196, 66));
    text(ui, 1204, 38, 15, rgb(245, 248, 252), accountName(ui));
    text(ui, 1204, 56, 12, rgb(185, 195, 212), membership(ui));
}

void drawHero(UiState& ui) {
    const auto* game = ui.games.empty() ? nullptr : &ui.games[std::min(ui.selected, ui.games.size() - 1)];
    const float x = 40;
    const float y = 96;
    const float w = 1200;
    const float h = 295;

    rounded(ui, x, y, w, h, 18, rgb(9, 13, 19));
    if (game) {
        const auto& image = loadImage(ui, heroUrl(*game));
        if (image.handle) {
            nvgSave(ui.vg);
            nvgIntersectScissor(ui.vg, x, y, w, h);
            drawImageCover(ui, image.handle, x, y, w, h, 0.85f);
            nvgRestore(ui.vg);
        }
    }
    const auto leftShade = nvgLinearGradient(ui.vg, x, y, x + 720, y, rgb(3, 6, 10, 250), rgb(3, 6, 10, 45));
    nvgBeginPath(ui.vg);
    nvgRoundedRect(ui.vg, x, y, w, h, 18);
    nvgFillPaint(ui.vg, leftShade);
    nvgFill(ui.vg);
    const auto bottomShade = nvgLinearGradient(ui.vg, x, y + h * 0.4f, x, y + h, rgb(0, 0, 0, 0), rgb(0, 0, 0, 155));
    nvgBeginPath(ui.vg);
    nvgRoundedRect(ui.vg, x, y, w, h, 18);
    nvgFillPaint(ui.vg, bottomShade);
    nvgFill(ui.vg);
    strokeRounded(ui, x, y, w, h, 18, rgb(55, 65, 78, 170), 1.0f);

    const std::string store = game ? primaryStore(*game) : "NVIDIA";
    const std::string title = game ? game->title : (ui.authPending ? "NVIDIA Login" : "OpenNOW Switch");
    const std::string meta = game ? deckLine(*game) : ui.status;
    text(ui, 90, 126, 18, rgb(105, 232, 151), store);
    text(ui, 88, 168, 50, rgb(248, 250, 252), title);
    text(ui, 90, 244, 18, rgb(228, 234, 244), meta);
    if (ui.authPending) {
        text(ui, 90, 272, 18, rgb(228, 234, 244), "Go to " + ui.verificationUrl);
        text(ui, 90, 300, 34, rgb(105, 232, 151), ui.userCode.empty() ? "Waiting for code" : ui.userCode);
    }

    rounded(ui, 90, 322, 140, 48, 10, rgb(74, 232, 117));
    text(ui, 128, 335, 19, rgb(3, 17, 10), ui.signedIn ? "Resume" : "Login");
    rounded(ui, 250, 322, 142, 48, 10, rgb(31, 36, 45, 230));
    text(ui, 286, 335, 19, rgb(244, 247, 252), ui.libraryMode ? "Library" : "All Games");
}

void drawCard(UiState& ui, const gfn::CatalogGame& game, std::size_t index, float x, float y) {
    const float w = 235;
    const float h = 190;
    const bool selected = index == ui.selected;
    rounded(ui, x, y, w, h, 14, rgb(17, 22, 29));
    const auto& image = loadImage(ui, cardUrl(game));
    if (image.handle) {
        nvgSave(ui.vg);
        nvgIntersectScissor(ui.vg, x, y, w, h);
        drawImageCover(ui, image.handle, x, y, w, h, 0.9f);
        nvgRestore(ui.vg);
    } else {
        const auto paint = nvgLinearGradient(ui.vg, x, y, x + w, y + h, rgb(36, 95, 72), rgb(15, 20, 31));
        nvgBeginPath(ui.vg);
        nvgRoundedRect(ui.vg, x, y, w, h, 14);
        nvgFillPaint(ui.vg, paint);
        nvgFill(ui.vg);
    }
    const auto shade = nvgLinearGradient(ui.vg, x, y + h * 0.35f, x, y + h, rgb(0, 0, 0, 0), rgb(0, 0, 0, 210));
    nvgBeginPath(ui.vg);
    nvgRoundedRect(ui.vg, x, y, w, h, 14);
    nvgFillPaint(ui.vg, shade);
    nvgFill(ui.vg);
    strokeRounded(ui, x, y, w, h, 14, selected ? rgb(74, 232, 117) : rgb(70, 77, 90, 190), selected ? 3.0f : 1.0f);
    text(ui, x + 14, y + h - 58, 13, rgb(222, 230, 242), primaryStore(game));
    text(ui, x + 14, y + h - 37, 15, rgb(248, 250, 252), game.title);
}

void drawLibrary(UiState& ui) {
    drawHero(ui);
    std::string heading = ui.libraryMode ? "My Library" : "All Games";
    if (ui.screen == Screen::Home) heading = "Continue Playing";
    if (ui.screen == Screen::Store) heading = "Store";
    if (ui.screen == Screen::Search) heading = "Search Results";
    text(ui, 40, 410, 26, rgb(248, 250, 252), heading);
    std::ostringstream count;
    count << ui.games.size() << " games";
    text(ui, 158, 416, 15, rgb(160, 170, 188), count.str());

    const float cardW = 235.0f;
    const float gap = 18.0f;
    const float y = 452.0f;
    const float selectedX = 40.0f + static_cast<float>(ui.selected) * (cardW + gap);
    const float maxScroll = std::max(0.0f, static_cast<float>(ui.games.size()) * (cardW + gap) - 1180.0f);
    const float targetScroll = std::clamp(selectedX - 160.0f - ui.swipeOffset, 0.0f, maxScroll);
    ui.shelfScroll += (targetScroll - ui.shelfScroll) * 0.28f;

    if (ui.games.empty()) {
        rounded(ui, 40, y, 1180, 130, 14, rgb(13, 18, 25, 230));
        text(ui, 70, y + 32, 22, rgb(248, 250, 252), ui.signedIn ? "No games loaded" : "Sign in to load your GeForce NOW library");
        text(ui, 70, y + 66, 17, rgb(171, 182, 200), "A: Login or load library   X: Load public catalog");
    } else {
        nvgSave(ui.vg);
        nvgScissor(ui.vg, 35, y - 8, 1210, 215);
        for (std::size_t i = 0; i < ui.games.size(); ++i) {
            const float x = 40.0f + static_cast<float>(i) * (cardW + gap) - ui.shelfScroll;
            if (x > -cardW && x < kUiW + cardW) {
                drawCard(ui, ui.games[i], i, x, y);
            }
        }
        nvgRestore(ui.vg);
    }
}

void drawDetailRow(UiState& ui, float x, float y, const std::string& label, const std::string& value) {
    text(ui, x, y, 15, rgb(142, 156, 178), label);
    text(ui, x + 140, y, 16, rgb(232, 238, 248), value);
}

void drawGameDetails(UiState& ui) {
    const auto* game = selectedGame(ui);
    if (!game) {
        drawLibrary(ui);
        return;
    }

    const float heroX = 40.0f;
    const float heroY = 96.0f;
    const float heroW = 1200.0f;
    const float heroH = 440.0f;
    rounded(ui, heroX, heroY, heroW, heroH, 20, rgb(8, 12, 18));
    const auto& hero = loadImage(ui, heroUrl(*game));
    if (hero.handle) {
        nvgSave(ui.vg);
        nvgIntersectScissor(ui.vg, heroX, heroY, heroW, heroH);
        drawImageCover(ui, hero.handle, heroX, heroY, heroW, heroH, 0.9f);
        nvgRestore(ui.vg);
    }

    const auto leftShade = nvgLinearGradient(ui.vg, heroX, heroY, heroX + 760.0f, heroY, rgb(2, 5, 8, 255), rgb(2, 5, 8, 80));
    nvgBeginPath(ui.vg);
    nvgRoundedRect(ui.vg, heroX, heroY, heroW, heroH, 20);
    nvgFillPaint(ui.vg, leftShade);
    nvgFill(ui.vg);
    const auto bottomShade = nvgLinearGradient(ui.vg, heroX, heroY + 180.0f, heroX, heroY + heroH, rgb(0, 0, 0, 0), rgb(0, 0, 0, 230));
    nvgBeginPath(ui.vg);
    nvgRoundedRect(ui.vg, heroX, heroY, heroW, heroH, 20);
    nvgFillPaint(ui.vg, bottomShade);
    nvgFill(ui.vg);
    strokeRounded(ui, heroX, heroY, heroW, heroH, 20, rgb(58, 70, 86, 170), 1.0f);

    text(ui, 90, 128, 17, rgb(105, 232, 151), primaryStore(*game));
    text(ui, 88, 166, 48, rgb(248, 250, 252), shortText(game->title, 34));
    text(ui, 90, 238, 20, rgb(230, 237, 248), deckLine(*game));
    text(ui, 90, 276, 17, rgb(168, 181, 201), "Stream target: " + describe(ui.streamSettings));

    rounded(ui, 90, 334, 146, 50, 11, rgb(74, 232, 117));
    text(ui, 143, 348, 19, rgb(3, 17, 10), "Play");
    rounded(ui, 256, 334, 146, 50, 11, rgb(31, 36, 45, 230));
    text(ui, 307, 348, 19, rgb(244, 247, 252), "Back");

    rounded(ui, 90, 424, 415, 88, 15, rgb(8, 13, 20, 215));
    drawDetailRow(ui, 116, 446, "Tier", game->minimumTier.empty() ? "Any supported tier" : game->minimumTier);
    drawDetailRow(ui, 116, 476, "Play type", game->playType.empty() ? primaryStore(*game) : game->playType);

    rounded(ui, 775, 384, 400, 128, 16, rgb(7, 12, 18, 220));
    text(ui, 802, 406, 21, rgb(248, 250, 252), "Available stores");
    float y = 440.0f;
    if (game->variants.empty()) {
        text(ui, 802, y, 16, rgb(170, 182, 200), primaryStore(*game));
    } else {
        for (std::size_t i = 0; i < game->variants.size() && i < 3; ++i) {
            const auto& variant = game->variants[i];
            const std::string state = variant.libraryStatus.empty() ? variant.status : variant.libraryStatus;
            text(ui, 802, y, 16, rgb(105, 232, 151), variant.store.empty() ? "GFN" : variant.store);
            text(ui, 910, y, 16, rgb(190, 202, 219), shortText(state.empty() ? "READY" : state, 22));
            y += 25.0f;
        }
    }

    text(ui, 40, 544, 22, rgb(248, 250, 252), "More in this row");
    const float cardW = 160.0f;
    const float gap = 16.0f;
    const std::size_t first = ui.selected > 2 ? ui.selected - 2 : 0;
    for (std::size_t i = first; i < ui.games.size() && i < first + 7; ++i) {
        const float x = 40.0f + static_cast<float>(i - first) * (cardW + gap);
        const float yCard = 570.0f;
        rounded(ui, x, yCard, cardW, 78, 12, rgb(17, 22, 29));
        const auto& image = loadImage(ui, cardUrl(ui.games[i]));
        if (image.handle) {
            nvgSave(ui.vg);
            nvgIntersectScissor(ui.vg, x, yCard, cardW, 78.0f);
            drawImageCover(ui, image.handle, x, yCard, cardW, 78.0f, 0.72f);
            nvgRestore(ui.vg);
        }
        const auto shade = nvgLinearGradient(ui.vg, x, yCard, x, yCard + 78.0f, rgb(0, 0, 0, 35), rgb(0, 0, 0, 215));
        nvgBeginPath(ui.vg);
        nvgRoundedRect(ui.vg, x, yCard, cardW, 78.0f, 12);
        nvgFillPaint(ui.vg, shade);
        nvgFill(ui.vg);
        strokeRounded(ui, x, yCard, cardW, 78.0f, 12, i == ui.selected ? rgb(74, 232, 117) : rgb(63, 74, 88), i == ui.selected ? 2.2f : 1.0f);
        text(ui, x + 10, yCard + 49, 13, rgb(246, 249, 253), shortText(ui.games[i].title, 18));
    }
}

void updateStreamVideoImage(UiState& ui) {
    media::RgbaVideoFrame frame;
    if (!media::consumeLatestVideoFrame(frame) || frame.rgba.empty() || !ui.vg) {
        return;
    }
    if (frame.frameId == ui.streamVideoFrameId) {
        return;
    }

    const bool changedSize = ui.streamVideoWidth != static_cast<int>(frame.width)
        || ui.streamVideoHeight != static_cast<int>(frame.height);
    if (ui.streamVideoImage && changedSize) {
        nvgDeleteImage(ui.vg, ui.streamVideoImage);
        ui.streamVideoImage = 0;
    }
    if (!ui.streamVideoImage) {
        ui.streamVideoImage = nvgCreateImageRGBA(
            ui.vg,
            static_cast<int>(frame.width),
            static_cast<int>(frame.height),
            0,
            frame.rgba.data());
    } else {
        nvgUpdateImage(ui.vg, ui.streamVideoImage, frame.rgba.data());
    }
    if (ui.streamVideoImage) {
        ui.streamVideoFrameId = frame.frameId;
        ui.streamVideoWidth = static_cast<int>(frame.width);
        ui.streamVideoHeight = static_cast<int>(frame.height);
    }
}

void drawStreamScreen(UiState& ui) {
    updateStreamVideoImage(ui);
    const auto launch = launchSnapshot(ui);
    if (ui.streamVideoImage && launch.ready) {
        drawImageCover(ui, ui.streamVideoImage, 0, 0, kUiW, kUiH, 1.0f);
        const auto topShade = nvgLinearGradient(ui.vg, 0, 0, 0, 150, rgb(0, 0, 0, 210), rgb(0, 0, 0, 0));
        nvgBeginPath(ui.vg);
        nvgRect(ui.vg, 0, 0, kUiW, 170);
        nvgFillPaint(ui.vg, topShade);
        nvgFill(ui.vg);
        const auto bottomShade = nvgLinearGradient(ui.vg, 0, 560, 0, 720, rgb(0, 0, 0, 0), rgb(0, 0, 0, 230));
        nvgBeginPath(ui.vg);
        nvgRect(ui.vg, 0, 530, kUiW, 190);
        nvgFillPaint(ui.vg, bottomShade);
        nvgFill(ui.vg);
        text(ui, 40, 38, 15, rgb(105, 232, 151), "NATIVE STREAM");
        text(ui, 40, 66, 30, rgb(248, 250, 252), shortText(launch.gameTitle.empty() ? "Streaming" : launch.gameTitle, 42));
        text(ui, 40, 106, 16, rgb(190, 202, 219), shortText(launch.mediaMessage.empty() ? "H264 video and Opus audio active" : launch.mediaMessage, 80));
        rounded(ui, 40, 580, 150, 42, 10, rgb(74, 232, 117));
        text(ui, 115, 593, 18, rgb(4, 18, 10), "Streaming", NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
        rounded(ui, 210, 580, 140, 42, 10, rgb(24, 31, 42, 235));
        text(ui, 280, 593, 18, rgb(235, 240, 249), "Back", NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
        return;
    }

    const auto* game = selectedGame(ui);
    const bool backendUnavailable = launch.failed && launch.sessionId.empty();
    const auto border = launch.ready
        ? rgb(74, 232, 117)
        : (backendUnavailable ? rgb(245, 196, 66) : (launch.failed ? rgb(239, 83, 80) : rgb(74, 232, 117)));
    const auto accent = launch.ready
        ? rgb(105, 232, 151)
        : (backendUnavailable ? rgb(245, 196, 66) : (launch.failed ? rgb(239, 154, 154) : rgb(105, 232, 151)));
    const auto headline = backendUnavailable ? "Native media stack unavailable" : launch.stage;

    rounded(ui, 40, 96, 1200, 520, 22, rgb(4, 8, 13));
    if (game) {
        const auto& hero = loadImage(ui, heroUrl(*game));
        if (hero.handle) {
            nvgSave(ui.vg);
            nvgIntersectScissor(ui.vg, 40, 96, 1200, 520);
            drawImageCover(ui, hero.handle, 40, 96, 1200, 520, 0.42f);
            nvgRestore(ui.vg);
        }
    }
    const auto shade = nvgLinearGradient(ui.vg, 40, 96, 40, 616, rgb(0, 0, 0, 130), rgb(0, 0, 0, 245));
    nvgBeginPath(ui.vg);
    nvgRoundedRect(ui.vg, 40, 96, 1200, 520, 22);
    nvgFillPaint(ui.vg, shade);
    nvgFill(ui.vg);
    strokeRounded(ui, 40, 96, 1200, 520, 22, border, 1.6f);

    text(ui, 86, 136, 15, accent, launch.ready ? "NATIVE STREAM" : "STREAM SETUP");
    text(ui, 86, 174, 42, rgb(248, 250, 252), shortText(launch.gameTitle.empty() ? "Starting session" : launch.gameTitle, 36));
    text(ui, 86, 242, 24, backendUnavailable ? rgb(255, 236, 179) : (launch.failed ? rgb(255, 205, 210) : rgb(228, 234, 244)), headline);
    textBox(ui, 86, 280, 1060, 17, rgb(177, 190, 210), launch.message);

    if (launch.inFlight && !launch.failed) {
        const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(nowMs() % 1200) / 1200.0f * 6.28318f);
        rounded(ui, 86, 332, 520, 8, 4, rgb(33, 44, 58, 240));
        rounded(ui, 86, 332, 120 + 380 * pulse, 8, 4, rgb(74, 232, 117));
    }

    rounded(ui, 86, 380, 540, 150, 16, rgb(7, 13, 20, 230));
    drawDetailRow(ui, 116, 406, "App ID", launch.appId.empty() ? "Unknown" : launch.appId);
    drawDetailRow(ui, 116, 436, "Session", launch.sessionId.empty() ? "Not created" : shortText(launch.sessionId, 34));
    drawDetailRow(ui, 116, 466, "Status", launch.status == 0 ? "Local preflight" : std::to_string(launch.status));
    drawDetailRow(ui, 116, 496, "Queue", launch.queuePosition == 0 ? "Not queued" : std::to_string(launch.queuePosition));

    rounded(ui, 666, 380, 520, 150, 16, rgb(7, 13, 20, 230));
    drawDetailRow(ui, 696, 406, "Server", launch.serverIp.empty() ? "Pending preflight" : shortText(launch.serverIp, 34));
    drawDetailRow(ui, 696, 436, "Signaling", launch.signalingUrl.empty() ? "WSS backend missing" : shortText(launch.signalingUrl, 36));
    drawDetailRow(ui, 696, 466, "Media", launch.mediaMessage.empty() ? "Backend not linked" : shortText(launch.mediaMessage, 36));
    drawDetailRow(ui, 696, 496, "Decode", backendUnavailable ? "FFmpeg NVTEGRA not linked" : "Switch HW path target: FFmpeg NVTEGRA");

    button(ui, 86, 548, 160, 46, launch.ready ? "Streaming" : (launch.inFlight ? "Loading" : "Unavailable"), launch.ready);
    button(ui, 266, 548, 160, 46, "Back");
}

int settingsIndex(SettingsSection section) {
    switch (section) {
    case SettingsSection::General:
        return 0;
    case SettingsSection::Account:
        return 1;
    case SettingsSection::Display:
        return 2;
    case SettingsSection::Logs:
        return 3;
    case SettingsSection::About:
        return 4;
    }
    return 0;
}

SettingsSection sectionFromIndex(int index) {
    switch ((index % 5 + 5) % 5) {
    case 0:
        return SettingsSection::General;
    case 1:
        return SettingsSection::Account;
    case 2:
        return SettingsSection::Display;
    case 3:
        return SettingsSection::Logs;
    case 4:
        return SettingsSection::About;
    }
    return SettingsSection::General;
}

std::string settingsLabel(SettingsSection section) {
    switch (section) {
    case SettingsSection::General:
        return "General";
    case SettingsSection::Account:
        return "Account";
    case SettingsSection::Display:
        return "Display";
    case SettingsSection::Logs:
        return "Logs";
    case SettingsSection::About:
        return "About";
    }
    return "Settings";
}

const char* settingsIcon(SettingsSection section) {
    switch (section) {
    case SettingsSection::General:
        return "G";
    case SettingsSection::Account:
        return "A";
    case SettingsSection::Display:
        return "D";
    case SettingsSection::Logs:
        return "L";
    case SettingsSection::About:
        return "I";
    }
    return "?";
}

std::string resolutionLabel(std::size_t index) {
    switch (index % 4) {
    case 0:
        return "1280 x 720";
    case 1:
        return "1024 x 576";
    case 2:
        return "960 x 540";
    case 3:
        return "854 x 480";
    }
    return "1280 x 720";
}

void applyResolution(UiState& ui) {
    switch (ui.resolutionIndex % 4) {
    case 0:
        ui.streamSettings.width = 1280;
        ui.streamSettings.height = 720;
        break;
    case 1:
        ui.streamSettings.width = 1024;
        ui.streamSettings.height = 576;
        break;
    case 2:
        ui.streamSettings.width = 960;
        ui.streamSettings.height = 540;
        break;
    case 3:
        ui.streamSettings.width = 854;
        ui.streamSettings.height = 480;
        break;
    }
}

void settingRow(UiState& ui, float x, float y, const std::string& label, const std::string& value, bool accent = false) {
    text(ui, x, y, 17, rgb(170, 181, 199), label);
    text(ui, x + 260, y, 17, accent ? rgb(105, 232, 151) : rgb(230, 236, 246), value);
    nvgBeginPath(ui.vg);
    nvgMoveTo(ui.vg, x, y + 32);
    nvgLineTo(ui.vg, x + 780, y + 32);
    nvgStrokeColor(ui.vg, rgb(65, 74, 89, 120));
    nvgStrokeWidth(ui.vg, 1.0f);
    nvgStroke(ui.vg);
}

void drawSettingsNavItem(UiState& ui, SettingsSection section, float y) {
    const float x = 62.0f;
    const float w = 260.0f;
    const float h = 50.0f;
    const bool active = ui.settingsSection == section;
    if (active) {
        const auto glow = nvgBoxGradient(ui.vg, x - 6, y - 6, w + 12, h + 12, 14, 18, rgb(74, 232, 117, 95), rgb(74, 232, 117, 0));
        nvgBeginPath(ui.vg);
        nvgRoundedRect(ui.vg, x - 6, y - 6, w + 12, h + 12, 14);
        nvgFillPaint(ui.vg, glow);
        nvgFill(ui.vg);
        rounded(ui, x, y, w, h, 12, rgb(17, 45, 30, 210));
        strokeRounded(ui, x, y, w, h, 12, rgb(74, 232, 117), 1.8f);
    }
    rounded(ui, x + 18, y + 14, 22, 22, 11, active ? rgb(74, 232, 117) : rgb(38, 45, 55));
    text(ui, x + 25, y + 16, 14, active ? rgb(5, 18, 10) : rgb(190, 198, 212), settingsIcon(section));
    text(ui, x + 58, y + 13, 18, active ? rgb(248, 250, 252) : rgb(200, 207, 220), settingsLabel(section));
}

void drawSettingsHeader(UiState& ui, const std::string& title, const std::string& subtitle) {
    text(ui, 392, 128, 30, rgb(248, 250, 252), title);
    text(ui, 394, 168, 17, rgb(160, 172, 190), subtitle);
}

void drawSubscriptionCard(UiState& ui, float x, float y, float w, float h) {
    rounded(ui, x, y, w, h, 16, rgb(9, 15, 23, 235));
    strokeRounded(ui, x, y, w, h, 16, ui.subscription.ok ? rgb(74, 232, 117, 210) : rgb(58, 70, 86), ui.subscription.ok ? 1.6f : 1.0f);
    text(ui, x + 24, y + 22, 15, rgb(151, 165, 186), "SUBSCRIPTION");
    text(ui, x + 24, y + 50, 30, rgb(248, 250, 252), membership(ui));
    text(ui, x + w - 24, y + 55, 22, ui.subscription.ok ? rgb(105, 232, 151) : rgb(170, 184, 204), subscriptionTimeLabel(ui), NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);

    if (!ui.signedIn) {
        text(ui, x + 24, y + 100, 17, rgb(166, 179, 198), "Press A to sign in with NVIDIA.");
        return;
    }
    if (!ui.subscription.loaded) {
        text(ui, x + 24, y + 100, 17, rgb(166, 179, 198), "Subscription has not been fetched yet.");
        return;
    }
    if (!ui.subscription.ok) {
        text(ui, x + 24, y + 100, 16, rgb(239, 154, 154), shortText(ui.subscription.error.empty() ? "Subscription unavailable" : ui.subscription.error, 84));
        return;
    }

    const double total = std::max(ui.subscription.totalHours, 0.0);
    const double left = std::max(ui.subscription.remainingHours, 0.0);
    const double used = std::max(ui.subscription.usedHours, 0.0);
    if (!ui.subscription.unlimited && total > 0.0) {
        const float ratio = static_cast<float>(std::clamp(used / total, 0.0, 1.0));
        rounded(ui, x + 24, y + 96, w - 48, 10, 5, rgb(31, 40, 52, 240));
        rounded(ui, x + 24, y + 96, (w - 48) * ratio, 10, 5, rgb(74, 232, 117));
    }

    drawDetailRow(ui, x + 24, y + 120, "Time left", ui.subscription.unlimited ? "Unlimited" : formatHours(left));
    drawDetailRow(ui, x + 24, y + 148, "Used", formatHours(used));
}

void drawSettings(UiState& ui) {
    rounded(ui, 40, 108, 302, 512, 18, rgb(5, 10, 15, 225));
    strokeRounded(ui, 40, 108, 302, 512, 18, rgb(40, 52, 65, 180), 1.0f);
    text(ui, 72, 132, 13, rgb(166, 177, 195), "SETTINGS");
    drawSettingsNavItem(ui, SettingsSection::General, 166);
    drawSettingsNavItem(ui, SettingsSection::Account, 228);
    drawSettingsNavItem(ui, SettingsSection::Display, 290);
    drawSettingsNavItem(ui, SettingsSection::Logs, 352);
    drawSettingsNavItem(ui, SettingsSection::About, 414);

    rounded(ui, 370, 108, 870, 512, 18, rgb(6, 11, 17, 230));
    strokeRounded(ui, 370, 108, 870, 512, 18, rgb(41, 53, 66, 190), 1.0f);

    if (ui.settingsSection == SettingsSection::General) {
        drawSettingsHeader(ui, "General", "App status and runtime profile");
        settingRow(ui, 392, 215, "Status", ui.status, ui.status == "Signed in" || ui.status.find("Loaded") != std::string::npos);
        settingRow(ui, 392, 270, "Catalog", ui.libraryMode ? "Library mode" : "All games");
        settingRow(ui, 392, 325, "Stream target", describe(ui.streamSettings));
        settingRow(ui, 392, 380, "Controls", "L/R pages, Up/Down settings, A select");
        rounded(ui, 392, 460, 785, 74, 14, rgb(13, 20, 29, 235));
        text(ui, 418, 482, 20, rgb(248, 250, 252), "OpenNOW Switch");
        text(ui, 418, 512, 16, rgb(167, 179, 198), "Native Switch GeForce NOW client prototype using Steam Deck NVIDIA auth.");
    } else if (ui.settingsSection == SettingsSection::Account) {
        drawSettingsHeader(ui, "Account", "Profile and GeForce NOW subscription");
        settingRow(ui, 392, 215, "Profile", accountName(ui), ui.signedIn);
        settingRow(ui, 392, 270, "Membership", membership(ui), ui.signedIn);
        settingRow(ui, 392, 325, "Time", subscriptionTimeLabel(ui), ui.subscription.ok);
        drawSubscriptionCard(ui, 392, 374, 785, 182);

        rounded(ui, 392, 566, 160, 42, 10, rgb(74, 232, 117));
        text(ui, 422, 577, 17, rgb(4, 18, 10), ui.signedIn ? "Login Again" : "Sign In");
        rounded(ui, 570, 566, 205, 42, 10, rgb(31, 37, 47, 230));
        text(ui, 596, 577, 17, rgb(235, 240, 249), "Refresh Subscription");
    } else if (ui.settingsSection == SettingsSection::Display) {
        drawSettingsHeader(ui, "Display", "Choose the stream resolution for future sessions");
        settingRow(ui, 392, 215, "Selected", resolutionLabel(ui.resolutionIndex), true);
        settingRow(ui, 392, 270, "Frame rate", std::to_string(ui.streamSettings.fps) + " FPS");
        settingRow(ui, 392, 325, "Codec", toString(ui.streamSettings.codec));
        settingRow(ui, 392, 380, "Bitrate", std::to_string(ui.streamSettings.bitrateKbps) + " kbps");

        const float x0 = 392.0f;
        const float y0 = 456.0f;
        for (std::size_t i = 0; i < 4; ++i) {
            const float x = x0 + static_cast<float>(i) * 190.0f;
            const bool active = i == ui.resolutionIndex;
            rounded(ui, x, y0, 170, 74, 14, active ? rgb(17, 62, 36, 230) : rgb(24, 30, 40, 230));
            strokeRounded(ui, x, y0, 170, 74, 14, active ? rgb(74, 232, 117) : rgb(60, 72, 86), active ? 2.0f : 1.0f);
            text(ui, x + 18, y0 + 18, 19, active ? rgb(248, 250, 252) : rgb(214, 222, 235), resolutionLabel(i));
            text(ui, x + 18, y0 + 46, 14, active ? rgb(105, 232, 151) : rgb(146, 160, 180), i == 0 ? "Recommended" : "Lower bandwidth");
        }
        text(ui, 392, 555, 15, rgb(151, 164, 184), "Use Left/Right or tap a tile. This feeds CloudMatch/media settings when streaming is enabled.");
    } else if (ui.settingsSection == SettingsSection::Logs) {
        drawSettingsHeader(ui, "Logs", "Recent app, network, auth, and catalog events");
        rounded(ui, 392, 205, 785, 360, 14, rgb(3, 7, 11, 235));
        strokeRounded(ui, 392, 205, 785, 360, 14, rgb(47, 59, 73), 1.0f);
        nvgSave(ui.vg);
        nvgScissor(ui.vg, 408, 222, 752, 326);
        float y = 224.0f;
        const auto logs = logSnapshot(ui);
        if (logs.empty()) {
            text(ui, 410, y, 16, rgb(170, 184, 204), "No logs recorded yet.");
        } else {
            constexpr std::size_t visible = 13;
            const std::size_t maxFirst = logs.size() > visible ? logs.size() - visible : 0;
            const std::size_t scroll = std::min(ui.logsScroll, maxFirst);
            const std::size_t first = maxFirst - scroll;
            const std::size_t last = std::min(logs.size(), first + visible);
            for (std::size_t i = first; i < last; ++i) {
                text(ui, 410, y, 14, rgb(178, 190, 208), logs[i]);
                y += 24.0f;
            }
        }
        nvgRestore(ui.vg);
        rounded(ui, 392, 582, 164, 30, 8, rgb(22, 31, 42, 230));
        text(ui, 412, 588, 14, rgb(173, 186, 204), std::to_string(logs.size()) + " entries kept");
        text(ui, 578, 588, 14, rgb(151, 164, 184), "Up/Down scroll, L/R change settings section");
    } else if (ui.settingsSection == SettingsSection::About) {
        drawSettingsHeader(ui, "About", "Build and project information");
        settingRow(ui, 392, 215, "App", "OpenNOW Switch");
        settingRow(ui, 392, 270, "UI", "Custom GLFW + NanoVG");
        settingRow(ui, 392, 325, "Auth profile", "Steam Deck");
        settingRow(ui, 392, 380, "Session", gfn::defaultAuthSessionPath());
        text(ui, 392, 462, 17, rgb(167, 179, 198), "Streaming transport is still being implemented. Auth, token persistence, logs, and catalog browsing are active.");
    }
}

void drawAuthPopup(UiState& ui) {
    if (!ui.authPopupVisible && !ui.authPending) {
        return;
    }
    nvgBeginPath(ui.vg);
    nvgRect(ui.vg, 0, 0, kUiW, kUiH);
    nvgFillColor(ui.vg, rgb(0, 0, 0, 180));
    nvgFill(ui.vg);

    const float x = 310.0f;
    const float y = 150.0f;
    const float w = 660.0f;
    const float h = 390.0f;
    rounded(ui, x, y, w, h, 22, rgb(6, 11, 17, 248));
    strokeRounded(ui, x, y, w, h, 22, rgb(74, 232, 117, 220), 1.8f);
    text(ui, x + 36, y + 34, 14, rgb(105, 232, 151), "NVIDIA LOGIN");
    text(ui, x + 36, y + 68, 32, rgb(248, 250, 252), ui.authPending ? "Approve this device" : "Sign in to GeForce NOW");
    text(ui, x + 36, y + 120, 17, rgb(170, 184, 204), "Use a phone or PC, then keep this popup open while OpenNOW polls NVIDIA.");

    rounded(ui, x + 36, y + 166, w - 72, 92, 16, rgb(11, 18, 27, 245));
    text(ui, x + 60, y + 186, 16, rgb(151, 165, 186), "Go to");
    text(ui, x + 60, y + 214, 20, rgb(235, 241, 250), ui.verificationUrl);
    text(ui, x + w - 60, y + 190, 34, rgb(105, 232, 151), ui.userCode.empty() ? "Waiting" : ui.userCode, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);

    text(ui, x + 36, y + 284, 16, ui.authPending ? rgb(105, 232, 151) : rgb(170, 184, 204), ui.status);
    rounded(ui, x + 36, y + 322, 170, 46, 10, rgb(74, 232, 117));
    text(ui, x + 75, y + 335, 18, rgb(4, 18, 10), ui.authPending ? "Polling" : "Start Login");
    rounded(ui, x + 226, y + 322, 145, 46, 10, rgb(31, 37, 47, 245));
    text(ui, x + 276, y + 335, 18, rgb(235, 240, 249), "Close");
    text(ui, x + w - 36, y + 336, 15, rgb(151, 165, 186), "B closes / cancels", NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
}

void drawFooter(UiState& ui) {
    const auto bg = nvgLinearGradient(ui.vg, 0, 640, 0, 720, rgb(4, 7, 11, 0), rgb(4, 7, 11, 235));
    nvgBeginPath(ui.vg);
    nvgRect(ui.vg, 0, 620, 1280, 100);
    nvgFillPaint(ui.vg, bg);
    nvgFill(ui.vg);
    auto hint = [&](float x, const char* key, const char* label, NVGcolor color) {
        rounded(ui, x, 670, 22, 22, 11, color);
        text(ui, x + 11, 673, 15, rgb(4, 9, 14), key, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        text(ui, x + 32, 681, 17, rgb(218, 225, 237), label, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    };
    const bool settings = ui.screen == Screen::Settings;
    const bool details = ui.screen == Screen::Details;
    hint(40, "A", details ? "Play" : (settings ? "Select" : (ui.signedIn ? "Select" : "Login")), rgb(74, 232, 117));
    hint(150, "B", settings ? "Library" : (details ? "Back" : "Back"), rgb(239, 83, 80));
    hint(250, "Y", settings ? "Logs" : (details ? "Library" : "Library"), rgb(250, 205, 76));
    hint(372, "X", settings ? "Refresh" : (details ? "All Games" : "All Games"), rgb(96, 165, 250));
    text(ui, 1092, 669, 17, rgb(218, 225, 237), "+ Exit");
}

void draw(UiState& ui, int fbW, int fbH) {
    const float scale = std::min(static_cast<float>(fbW) / kUiW, static_cast<float>(fbH) / kUiH);
    const float ox = (static_cast<float>(fbW) - kUiW * scale) * 0.5f;
    const float oy = (static_cast<float>(fbH) - kUiH * scale) * 0.5f;

    glViewport(0, 0, fbW, fbH);
    glClearColor(0.005f, 0.008f, 0.012f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    nvgBeginFrame(ui.vg, fbW, fbH, 1.0f);
    nvgSave(ui.vg);
    nvgTranslate(ui.vg, ox, oy);
    nvgScale(ui.vg, scale, scale);
    processImageCompletions(ui);

    const auto bg = nvgRadialGradient(ui.vg, 620, 120, 80, 900, rgb(20, 90, 62, 155), rgb(1, 4, 8, 255));
    nvgBeginPath(ui.vg);
    nvgRect(ui.vg, 0, 0, kUiW, kUiH);
    nvgFillPaint(ui.vg, bg);
    nvgFill(ui.vg);

    drawTopNav(ui);
    if (ui.screen == Screen::Settings) {
        drawSettings(ui);
    } else if (ui.screen == Screen::Details) {
        drawGameDetails(ui);
    } else if (ui.screen == Screen::Stream) {
        drawStreamScreen(ui);
    } else {
        drawLibrary(ui);
    }
    drawFooter(ui);
    if (ui.loading) {
        rounded(ui, 510, 318, 260, 76, 14, rgb(8, 12, 18, 235));
        strokeRounded(ui, 510, 318, 260, 76, 14, rgb(74, 232, 117), 1.4f);
        text(ui, 640, 344, 20, rgb(248, 250, 252), ui.status, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
    }
    drawAuthPopup(ui);

    nvgRestore(ui.vg);
    nvgEndFrame(ui.vg);
}

void loadSession(UiState& ui) {
    const auto loaded = gfn::loadAuthSession(gfn::defaultAuthSessionPath());
    if (loaded.ok) {
        ui.session = loaded.session;
        ui.signedIn = !ui.session.tokens.accessToken.empty();
        ui.status = "Signed in";
        log(ui, "[INFO][AUTH] Loaded saved auth session.");
    } else {
        ui.status = "Not signed in";
        log(ui, "[INFO][AUTH] No saved auth session: " + loaded.error);
    }
}

void saveSession(UiState& ui, const AuthSession& session) {
    std::string error;
    if (gfn::saveAuthSession(session, gfn::defaultAuthSessionPath(), error)) {
        log(ui, "[INFO][AUTH] Saved auth session.");
    } else {
        log(ui, "[WARN][AUTH] Session save failed: " + error);
    }
}

std::string subscriptionToken(const AuthSession& session) {
    if (!session.tokens.idToken.empty()) {
        return session.tokens.idToken;
    }
    return session.tokens.accessToken;
}

double jsonNumber(const util::JsonValue* object, const char* key, double fallback = 0.0) {
    if (!object) {
        return fallback;
    }
    const auto* value = object->get(key);
    if (!value) {
        return fallback;
    }
    if (value->isNumber()) {
        return value->asNumber(fallback);
    }
    if (value->isString()) {
        const auto textValue = value->asString();
        char* end = nullptr;
        const double parsed = std::strtod(textValue.c_str(), &end);
        if (end && *end == '\0' && std::isfinite(parsed)) {
            return parsed;
        }
    }
    return fallback;
}

std::string jsonString(const util::JsonValue* object, const char* key, const std::string& fallback = {}) {
    if (!object) {
        return fallback;
    }
    const auto* value = object->get(key);
    return value ? value->asString(fallback) : fallback;
}

bool jsonBool(const util::JsonValue* object, const char* key, bool fallback = false) {
    if (!object) {
        return fallback;
    }
    const auto* value = object->get(key);
    return value ? value->asBool(fallback) : fallback;
}

void fetchSubscriptionInfo(UiState& ui) {
    if (!ui.signedIn) {
        ui.subscription = {};
        return;
    }
    const auto token = subscriptionToken(ui.session);
    if (token.empty() || ui.session.user.userId.empty()) {
        ui.subscription.loaded = true;
        ui.subscription.ok = false;
        ui.subscription.error = "Missing subscription token or user id";
        return;
    }

    ui.loading = true;
    ui.status = "Loading subscription";
    log(ui, "[INFO][AUTH] Loading subscription info.");
    try {
        const std::string url = "https://mes.geforcenow.com/v4/subscriptions"
            "?serviceName=gfn_pc"
            "&languageCode=en_US"
            "&vpcId=" + gfn::urlEncode(ui.vpcId.empty() ? "NP-AMS-08" : ui.vpcId) +
            "&userId=" + gfn::urlEncode(ui.session.user.userId);
        const auto response = ui.http.send({
            net::HttpMethod::Get,
            url,
            {
                {"Accept", "application/json"},
                {"Authorization", "GFNJWT " + token},
                {"nv-client-id", "ec7e38d4-03af-4b58-b131-cfb0495903ab"},
                {"nv-client-type", "NATIVE"},
                {"nv-client-version", "2.0.80.173"},
                {"nv-client-streamer", "NVIDIA-CLASSIC"},
                {"nv-device-os", "LINUX"},
                {"nv-device-type", "DESKTOP"},
                {"nv-device-make", "Valve"},
                {"nv-device-model", "Steam Deck"},
                {"User-Agent", "Mozilla/5.0 (X11; Linux x86_64; Steam Deck) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36"},
            },
            "",
        });

        ui.subscription.loaded = true;
        if (response.status < 200 || response.status >= 300) {
            ui.subscription.ok = false;
            ui.subscription.error = "Subscription HTTP " + std::to_string(response.status) + ": " + shortText(response.body, 120);
            ui.status = "Subscription unavailable";
            log(ui, "[ERROR][AUTH] " + ui.subscription.error);
            ui.loading = false;
            return;
        }
        const auto parsed = util::parseJson(response.body);
        if (!parsed.ok) {
            ui.subscription.ok = false;
            ui.subscription.error = "Subscription JSON parse failed: " + parsed.error;
            ui.status = "Subscription unavailable";
            log(ui, "[ERROR][AUTH] " + ui.subscription.error);
            ui.loading = false;
            return;
        }
        const auto& root = parsed.value;
        const double allottedMinutes = jsonNumber(&root, "allottedTimeInMinutes");
        const double purchasedMinutes = jsonNumber(&root, "purchasedTimeInMinutes");
        const double rolledOverMinutes = jsonNumber(&root, "rolledOverTimeInMinutes");
        const double fallbackTotalMinutes = allottedMinutes + purchasedMinutes + rolledOverMinutes;
        const double totalMinutes = jsonNumber(&root, "totalTimeInMinutes", fallbackTotalMinutes);
        const double remainingMinutes = jsonNumber(&root, "remainingTimeInMinutes");
        const double usedMinutes = std::max(totalMinutes - remainingMinutes, 0.0);

        ui.subscription.ok = true;
        ui.subscription.error.clear();
        ui.subscription.tier = jsonString(&root, "membershipTier", "FREE");
        ui.subscription.type = jsonString(&root, "type");
        ui.subscription.subType = jsonString(&root, "subType");
        ui.subscription.unlimited = ui.subscription.subType == "UNLIMITED";
        ui.subscription.allottedHours = allottedMinutes / 60.0;
        ui.subscription.purchasedHours = purchasedMinutes / 60.0;
        ui.subscription.rolledOverHours = rolledOverMinutes / 60.0;
        ui.subscription.remainingHours = remainingMinutes / 60.0;
        ui.subscription.totalHours = totalMinutes / 60.0;
        ui.subscription.usedHours = usedMinutes / 60.0;
        ui.subscription.periodStart = jsonString(&root, "currentSpanStartDateTime");
        ui.subscription.periodEnd = jsonString(&root, "currentSpanEndDateTime");
        if (const auto* currentState = root.get("currentSubscriptionState")) {
            ui.subscription.state = jsonString(currentState, "state");
            ui.subscription.gameplayAllowed = jsonBool(currentState, "isGamePlayAllowed", true);
        }
        ui.session.user.membershipTier = ui.subscription.tier;
        ui.status = "Subscription loaded";
        log(ui, "[INFO][AUTH] Subscription loaded: " + ui.subscription.tier + ", " + subscriptionTimeLabel(ui) + ".");
    } catch (const std::exception& ex) {
        ui.subscription.loaded = true;
        ui.subscription.ok = false;
        ui.subscription.error = ex.what();
        ui.status = "Subscription unavailable";
        log(ui, std::string("[ERROR][AUTH] Subscription exception: ") + ex.what());
    }
    ui.loading = false;
}

bool refreshSession(UiState& ui) {
    if (!ui.signedIn) {
        return false;
    }
    auto builder = gfn::AuthRequestBuilder::steamDeck();
    gfn::AuthService service(ui.http, builder);
    gfn::AuthResult refreshed;
    if (!ui.session.tokens.clientToken.empty()) {
        refreshed = service.refreshWithClientToken(ui.session);
    }
    if (!refreshed.ok && !ui.session.tokens.refreshToken.empty()) {
        refreshed = service.refreshWithRefreshToken(ui.session);
    }
    if (!refreshed.ok) {
        log(ui, "[WARN][AUTH] Refresh failed: " + refreshed.error);
        return false;
    }
    ui.session = refreshed.session;
    ui.signedIn = true;
    saveSession(ui, ui.session);
    fetchSubscriptionInfo(ui);
    return true;
}

void fetchCatalog(UiState& ui, bool library) {
    ui.loading = true;
    ui.status = library ? "Loading library" : "Loading catalog";
    ui.libraryMode = library;
    log(ui, library ? "[INFO][SESSION] Loading GFN library." : "[INFO][SESSION] Loading public catalog.");
    try {
        gfn::CatalogService catalog(ui.http);
        gfn::CatalogFetchOptions options;
        options.libraryOnly = library;
        options.first = library ? 80 : 80;
        auto result = catalog.fetchCatalog(library && ui.signedIn ? &ui.session : nullptr, options);
        if (!result.ok && library && result.error.find("HTTP 401") != std::string::npos && refreshSession(ui)) {
            result = catalog.fetchCatalog(&ui.session, options);
        }
        if (!result.ok) {
            ui.status = result.error;
            log(ui, "[ERROR][SESSION] Catalog failed: " + result.error);
            ui.loading = false;
            return;
        }
        ui.games = std::move(result.games);
        if (!result.vpcId.empty()) {
            ui.vpcId = result.vpcId;
        }
        ui.selected = 0;
        ui.shelfScroll = 0.0f;
        prefetchVisibleImages(ui);
        ui.status = "Loaded " + std::to_string(ui.games.size()) + " games";
        log(ui, "[INFO][SESSION] Catalog loaded from " + result.vpcId + ": " + std::to_string(ui.games.size()) + " items.");
    } catch (const std::exception& ex) {
        ui.status = ex.what();
        log(ui, std::string("[ERROR][SESSION] Catalog exception: ") + ex.what());
    }
    ui.loading = false;
}

void startDeviceLogin(UiState& ui);

void openGameDetails(UiState& ui) {
    if (ui.games.empty() || ui.selected >= ui.games.size()) {
        return;
    }
    if (ui.screen != Screen::Details) {
        ui.previousScreen = ui.screen;
    }
    ui.screen = Screen::Details;
    ui.status = "Details: " + ui.games[ui.selected].title;
    log(ui, "[INFO][UI] Opened details for " + ui.games[ui.selected].title + ".");
}

void stopCloudMatchSession(
    UiState& ui,
    net::CurlHttpClient& http,
    const gfn::CloudMatchRequestBuilder& builder,
    const std::string& token,
    const std::string& clientId,
    const std::string& deviceId,
    const SessionInfo& session,
    const std::string& reason) {
    if (token.empty() || session.sessionId.empty()) {
        return;
    }

    log(ui, "[INFO][SESSION] Stopping CloudMatch session " + session.sessionId + " (" + reason + ").");
    const auto response = http.send(builder.buildStopSessionRequest({
        .token = token,
        .streamingBaseUrl = session.streamingBaseUrl,
        .serverIp = session.serverIp,
        .zone = session.zone,
        .sessionId = session.sessionId,
        .clientId = clientId,
        .deviceId = deviceId,
    }));

    if (response.status >= 200 && response.status < 300) {
        log(ui, "[INFO][SESSION] CloudMatch session stopped.");
    } else {
        log(ui, "[WARN][SESSION] CloudMatch stop returned HTTP " + std::to_string(response.status) + ": " + shortText(response.body, 160));
    }
}

void launchStreamWorker(
    UiState* ui,
    AuthSession session,
    gfn::CatalogGame game,
    std::string appId,
    StreamSettings settings) {
    StreamLaunchState state;
    state.active = true;
    state.inFlight = true;
    state.gameTitle = game.title;
    state.appId = appId;
    state.stage = "Creating CloudMatch session";
    state.message = "Requesting a GeForce NOW rig.";
    updateLaunch(*ui, state);

    net::CurlHttpClient http;
    gfn::CloudMatchRequestBuilder builder;
    gfn::CloudMatchResponseParser parser;
    const auto token = subscriptionToken(session);
    const std::string clientId = gfn::generateOpaqueToken(16);
    const std::string deviceId = gfn::generateOpaqueToken(16);
    const std::string streamingBaseUrl = session.provider.streamingServiceUrl.empty()
        ? "https://prod.cloudmatchbeta.nvidiagrid.net"
        : session.provider.streamingServiceUrl;
    const std::string zone = "prod";
    SessionInfo latest;
    bool stopOnExit = false;

    try {
        if (token.empty()) {
            throw std::runtime_error("No GFN auth token is available. Sign in again.");
        }
        log(*ui, "[INFO][SESSION] CloudMatch create: " + game.title + " appId=" + appId + " settings=" + describe(settings) + ".");

        const auto createRequest = builder.buildCreateSessionRequest({
            .token = token,
            .streamingBaseUrl = streamingBaseUrl,
            .zone = zone,
            .appId = appId,
            .internalTitle = game.title,
            .clientId = clientId,
            .deviceId = deviceId,
            .settings = settings,
            .accountLinked = true,
        });
        const auto createResponse = http.send(createRequest);
        if (createResponse.status < 200 || createResponse.status >= 300) {
            throw std::runtime_error("CloudMatch create failed HTTP " + std::to_string(createResponse.status) + ": " + shortText(createResponse.body, 180));
        }

        auto parsed = parser.parseSessionInfo(createResponse.body, zone, streamingBaseUrl);
        if (!parsed.ok) {
            throw std::runtime_error("CloudMatch create parse failed: " + parsed.error);
        }
        latest = parsed.session;
        stopOnExit = true;
        log(*ui, "[INFO][SESSION] CloudMatch session created id=" + latest.sessionId + " status=" + std::to_string(latest.status) + ".");
        if (launchStopRequested(*ui)) {
            state.inFlight = false;
            state.stage = "Stopped";
            state.message = "Launch cancelled. Stopping GFN session.";
            updateLaunch(*ui, state);
            stopCloudMatchSession(*ui, http, builder, token, clientId, deviceId, latest, "user cancelled launch");
            stopOnExit = false;
            return;
        }
        state.sessionId = latest.sessionId;
        state.status = latest.status;
        state.queuePosition = latest.queuePosition;
        state.serverIp = latest.serverIp;
        state.signalingUrl = latest.signalingUrl;
        state.stage = latest.status == 1 ? "Waiting for rig" : "Cloud session ready";
        state.message = latest.queuePosition > 0
            ? "Queue position " + std::to_string(latest.queuePosition)
            : "Polling session until the rig is ready.";
        updateLaunch(*ui, state);

        for (int attempt = 0; attempt < 240 && !launchStopRequested(*ui); ++attempt) {
            if (latest.status == 2 || latest.status == 3) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::seconds(2));
            if (launchStopRequested(*ui)) {
                state.inFlight = false;
                state.stage = "Stopped";
                state.message = "Launch cancelled. Stopping GFN session.";
                updateLaunch(*ui, state);
                stopCloudMatchSession(*ui, http, builder, token, clientId, deviceId, latest, "user cancelled launch");
                stopOnExit = false;
                return;
            }

            const auto pollResponse = http.send(builder.buildPollSessionRequest({
                .token = token,
                .streamingBaseUrl = latest.streamingBaseUrl.empty() ? streamingBaseUrl : latest.streamingBaseUrl,
                .serverIp = latest.serverIp,
                .zone = zone,
                .sessionId = latest.sessionId,
                .clientId = clientId,
                .deviceId = deviceId,
            }));
            if (pollResponse.status < 200 || pollResponse.status >= 300) {
                throw std::runtime_error("CloudMatch poll failed HTTP " + std::to_string(pollResponse.status) + ": " + shortText(pollResponse.body, 180));
            }
            parsed = parser.parseSessionInfo(pollResponse.body, zone, streamingBaseUrl);
            if (!parsed.ok) {
                throw std::runtime_error("CloudMatch poll parse failed: " + parsed.error);
            }
            latest = parsed.session;
            log(*ui, "[INFO][SESSION] CloudMatch poll status=" + std::to_string(latest.status)
                + " queue=" + std::to_string(latest.queuePosition)
                + (latest.serverIp.empty() ? "" : " server=" + latest.serverIp) + ".");
            state.sessionId = latest.sessionId;
            state.status = latest.status;
            state.queuePosition = latest.queuePosition;
            state.serverIp = latest.serverIp;
            state.signalingUrl = latest.signalingUrl;
            state.stage = latest.queuePosition > 0 ? "In queue" : (latest.status == 1 ? "Launching rig" : "Connecting media");
            state.message = latest.queuePosition > 0
                ? "Queue position " + std::to_string(latest.queuePosition)
                : "Session status " + std::to_string(latest.status);
            updateLaunch(*ui, state);
        }

        if (latest.status != 2 && latest.status != 3) {
            if (launchStopRequested(*ui)) {
                state.inFlight = false;
                state.stage = "Stopped";
                state.message = "Launch cancelled. Stopping GFN session.";
                updateLaunch(*ui, state);
                stopCloudMatchSession(*ui, http, builder, token, clientId, deviceId, latest, "user cancelled launch");
                stopOnExit = false;
                return;
            }
            throw std::runtime_error("Session did not become ready before timeout.");
        }
        if (launchStopRequested(*ui)) {
            state.inFlight = false;
            state.stage = "Stopped";
            state.message = "Launch cancelled. Stopping GFN session.";
            updateLaunch(*ui, state);
            stopCloudMatchSession(*ui, http, builder, token, clientId, deviceId, latest, "user cancelled launch");
            stopOnExit = false;
            return;
        }
        log(*ui, "[INFO][SESSION] CloudMatch ready: signaling=" + latest.signalingUrl
            + " media=" + latest.mediaConnectionInfo.ip + ":" + std::to_string(latest.mediaConnectionInfo.port) + ".");

        state.stage = "Opening native media";
        state.message = "CloudMatch ready. Opening native GFN media pipeline.";
        state.status = latest.status;
        state.serverIp = latest.serverIp;
        state.signalingUrl = latest.signalingUrl;
        updateLaunch(*ui, state);

        media::NativeMediaPipeline pipeline;
        const auto mediaStatus = pipeline.open(settings, latest, &http, [ui](const std::string& message) {
            log(*ui, message);
            if (message.find("[MEDIA]") != std::string::npos || message.find("[SIGNALING]") != std::string::npos) {
                auto current = launchSnapshot(*ui);
                if (current.active) {
                    current.mediaMessage = shortText(message, 110);
                    if (current.inFlight) {
                        current.message = shortText(message, 120);
                    }
                    updateLaunch(*ui, current);
                }
            }
        });
        state.mediaMessage = mediaStatus.message;
        if (!mediaStatus.ready) {
            stopCloudMatchSession(*ui, http, builder, token, clientId, deviceId, latest, "native media unavailable");
            stopOnExit = false;
            state.inFlight = false;
            state.failed = true;
            state.stage = "Cloud session stopped";
            state.message = mediaStatus.message;
            updateLaunch(*ui, state);
            return;
        }

        state.inFlight = false;
        state.ready = true;
        state.stage = "Streaming";
        state.message = "Native media pipeline is active.";
        updateLaunch(*ui, state);

        log(*ui, "[INFO][MEDIA] Native stream loop active. Press B to stop.");
        PadState streamPad;
        padInitializeDefault(&streamPad);
        input::ControllerMapper controllerMapper;
        auto nextUiStats = std::chrono::steady_clock::now();
        auto nextInput = std::chrono::steady_clock::now();
        bool inputReadyLogged = false;
        bool inputWaitLogged = false;
        while (!launchStopRequested(*ui)) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= nextInput) {
                if (pipeline.inputReady()) {
                    if (!inputReadyLogged) {
                        log(*ui, "[INFO][INPUT] GFN input datachannel ready; forwarding Switch controller state.");
                        inputReadyLogged = true;
                    }
                    auto controller = controllerStateFromPad(streamPad);
                    auto packet = controllerMapper.map(controller, {
                        .swapNintendoFaceButtons = true,
                        .leftDeadzone = 0.15f,
                        .rightDeadzone = 0.15f,
                        .controllerId = 0,
                        .timestampUs = nowUs(),
                    });
                    (void)pipeline.sendGamepadInput(packet, 0x0101);
                } else if (!inputWaitLogged && pipeline.mediaConnected()) {
                    log(*ui, "[INFO][INPUT] Waiting for GFN input datachannel handshake.");
                    inputWaitLogged = true;
                }
                nextInput = now + std::chrono::milliseconds(16);
            }
            if (std::chrono::steady_clock::now() >= nextUiStats) {
                state.mediaMessage = "state=" + pipeline.connectionState()
                    + " video=" + std::to_string(pipeline.videoBytesReceived())
                    + "B audio=" + std::to_string(pipeline.audioBytesReceived()) + "B";
                state.message = state.mediaMessage;
                updateLaunch(*ui, state);
                nextUiStats = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        state.inFlight = false;
        state.ready = false;
        state.stage = "Stopping stream";
        state.message = "Closing native media pipeline and stopping the GFN session.";
        updateLaunch(*ui, state);
        pipeline.close();
        media::clearVideoFrames();
        stopCloudMatchSession(*ui, http, builder, token, clientId, deviceId, latest, "user stopped stream");
        stopOnExit = false;
        state.stage = "Stream stopped";
        state.message = "Cloud session stopped.";
        updateLaunch(*ui, state);
    } catch (const std::exception& ex) {
        if (stopOnExit) {
            stopCloudMatchSession(*ui, http, builder, token, clientId, deviceId, latest, "launch failed");
        }
        log(*ui, std::string("[ERROR][SESSION] Stream launch failed: ") + ex.what());
        state.inFlight = false;
        state.failed = true;
        state.stage = "Launch failed";
        state.message = ex.what();
        updateLaunch(*ui, state);
    }
}

void startStreamLaunch(UiState& ui, const gfn::CatalogGame& game) {
    const auto appId = selectedLaunchAppId(game);
    if (appId.empty()) {
        StreamLaunchState state;
        state.active = true;
        state.failed = true;
        state.gameTitle = game.title;
        state.stage = "Missing app ID";
        state.message = "This catalog item does not expose a numeric launch appId yet.";
        updateLaunch(ui, state);
        ui.screen = Screen::Stream;
        log(ui, "[ERROR][SESSION] Could not resolve numeric appId for " + game.title + ".");
        return;
    }

    const auto caps = media::nativeMediaCapabilities();
    if (!caps.transportReady()) {
        StreamLaunchState state;
        state.active = true;
        state.failed = true;
        state.gameTitle = game.title;
        state.appId = appId;
        state.stage = "Native media stack unavailable";
        state.message = "OpenNOW did not create a GeForce NOW rig because the native WebRTC transport backend is not linked yet. This avoids starting and immediately stopping a paid/cloud session.";
        state.mediaMessage = caps.message;
        updateLaunch(ui, state);
        ui.previousScreen = ui.screen == Screen::Stream ? Screen::Details : ui.screen;
        ui.screen = Screen::Stream;
        log(ui, "[ERROR][MEDIA] " + caps.message);
        log(ui, "[WARN][SESSION] Stream launch blocked before CloudMatch create; native media backend is unavailable.");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(ui.launchMutex);
        if (ui.launch.inFlight) {
            ui.screen = Screen::Stream;
            return;
        }
        ui.launch.stopRequested = false;
    }
    if (ui.launchThread.joinable()) {
        ui.launchThread.join();
    }
    media::clearVideoFrames();
    if (ui.streamVideoImage) {
        nvgDeleteImage(ui.vg, ui.streamVideoImage);
        ui.streamVideoImage = 0;
        ui.streamVideoFrameId = 0;
        ui.streamVideoWidth = 0;
        ui.streamVideoHeight = 0;
    }

    StreamLaunchState state;
    state.active = true;
    state.inFlight = true;
    state.gameTitle = game.title;
    state.appId = appId;
    state.stage = "Starting";
    state.message = "Preparing native launch.";
    updateLaunch(ui, state);
    ui.previousScreen = ui.screen == Screen::Stream ? Screen::Details : ui.screen;
    ui.screen = Screen::Stream;
    log(ui, "[INFO][SESSION] Starting CloudMatch session for " + game.title + " appId=" + appId + ".");
    ui.launchThread = std::thread(launchStreamWorker, &ui, ui.session, game, appId, ui.streamSettings);
}

void playSelectedGame(UiState& ui) {
    const auto* game = selectedGame(ui);
    if (!game) {
        return;
    }
    if (!ui.signedIn) {
        ui.settingsSection = SettingsSection::Account;
        ui.previousScreen = Screen::Details;
        ui.screen = Screen::Settings;
        startDeviceLogin(ui);
        return;
    }
    startStreamLaunch(ui, *game);
}

void activateSelected(UiState& ui) {
    if (ui.games.empty()) {
        if (!ui.signedIn) {
            startDeviceLogin(ui);
            return;
        }
        fetchCatalog(ui, true);
    } else {
        openGameDetails(ui);
    }
}

void startDeviceLogin(UiState& ui) {
    ui.authPopupVisible = true;
    ui.loading = true;
    ui.status = "Requesting login code";
    try {
        auto builder = gfn::AuthRequestBuilder::steamDeck();
        gfn::AuthService service(ui.http, builder);
        const auto provider = builder.defaultProvider();
        const auto authorization = service.requestDeviceAuthorization({
            .deviceId = gfn::generateOpaqueToken(24),
            .displayName = "Steam Deck",
            .idpId = provider.idpId,
        });
        if (!authorization.ok) {
            ui.status = authorization.error;
            log(ui, "[ERROR][AUTH] Device authorization failed: " + authorization.error);
            ui.loading = false;
            return;
        }
        ui.authPending = true;
        ui.pollProvider = provider;
        ui.pollDeviceCode = authorization.authorization.deviceCode;
        ui.userCode = authorization.authorization.userCode;
        ui.verificationUrl = authorization.authorization.verificationUri.empty() ? "https://login.nvidia.com/activate" : authorization.authorization.verificationUri;
        ui.pollInterval = std::max<std::uint32_t>(authorization.authorization.interval, 5);
        ui.nextPollMs = nowMs() + static_cast<std::uint64_t>(ui.pollInterval) * 1000ULL;
        ui.pollDeadlineMs = nowMs() + static_cast<std::uint64_t>(std::max<std::uint32_t>(authorization.authorization.expiresIn, 600)) * 1000ULL;
        ui.status = "Waiting for NVIDIA approval";
        log(ui, "[INFO][AUTH] Device code received.");
    } catch (const std::exception& ex) {
        ui.status = ex.what();
        log(ui, std::string("[ERROR][AUTH] Login exception: ") + ex.what());
    }
    ui.loading = false;
}

void handlePointer(UiState& ui, float x, float y) {
    if (ui.authPopupVisible || ui.authPending) {
        if (hit({346, 472, 170, 46}, x, y)) {
            if (!ui.authPending) {
                startDeviceLogin(ui);
            }
            return;
        }
        if (hit({536, 472, 145, 46}, x, y)) {
            if (ui.authPending) {
                ui.authPending = false;
                ui.status = "Login cancelled";
                log(ui, "[INFO][AUTH] Login cancelled.");
            }
            ui.authPopupVisible = false;
            return;
        }
        return;
    }
    if (ui.screen == Screen::Stream) {
        if (hit({266, 548, 160, 46}, x, y)) {
            requestLaunchStop(ui);
            ui.screen = ui.previousScreen == Screen::Stream ? Screen::Details : ui.previousScreen;
            return;
        }
        return;
    }
    if (hit({360, 34, 100, 54}, x, y)) {
        ui.screen = Screen::Home;
        return;
    }
    if (hit({470, 34, 100, 54}, x, y)) {
        ui.screen = Screen::Library;
        if (ui.signedIn && ui.games.empty()) fetchCatalog(ui, true);
        return;
    }
    if (hit({580, 34, 100, 54}, x, y)) {
        ui.screen = Screen::Store;
        fetchCatalog(ui, false);
        return;
    }
    if (hit({690, 34, 110, 54}, x, y)) {
        ui.screen = Screen::Search;
        fetchCatalog(ui, false);
        return;
    }
    if (hit({810, 34, 120, 54}, x, y)) {
        ui.screen = Screen::Settings;
        return;
    }
    if (hit({1016, 34, 122, 36}, x, y)) {
        if (!ui.signedIn) startDeviceLogin(ui);
        return;
    }
    if (ui.screen == Screen::Stream) {
        if (hit({266, 548, 160, 46}, x, y)) {
            requestLaunchStop(ui);
            ui.screen = ui.previousScreen == Screen::Stream ? Screen::Details : ui.previousScreen;
            return;
        }
        return;
    }
    if (ui.screen == Screen::Details) {
        if (hit({90, 334, 146, 50}, x, y)) {
            playSelectedGame(ui);
            return;
        }
        if (hit({256, 334, 146, 50}, x, y)) {
            ui.screen = ui.previousScreen == Screen::Details ? Screen::Library : ui.previousScreen;
            return;
        }
        const float cardW = 160.0f;
        const float gap = 16.0f;
        const std::size_t first = ui.selected > 2 ? ui.selected - 2 : 0;
        if (y >= 570.0f && y <= 648.0f) {
            const int index = static_cast<int>((x - 40.0f) / (cardW + gap));
            const float localX = x - 40.0f - static_cast<float>(index) * (cardW + gap);
            if (index >= 0 && localX >= 0.0f && localX <= cardW) {
                const std::size_t target = first + static_cast<std::size_t>(index);
                if (target < ui.games.size()) {
                    ui.selected = target;
                    prefetchVisibleImages(ui);
                    ui.status = "Details: " + ui.games[ui.selected].title;
                    return;
                }
            }
        }
        return;
    }
    if (ui.screen == Screen::Settings) {
        for (int i = 0; i < 5; ++i) {
            const float rowY = 166.0f + static_cast<float>(i) * 62.0f;
            if (hit({62.0f, rowY, 260.0f, 50.0f}, x, y)) {
                ui.settingsSection = sectionFromIndex(i);
                return;
            }
        }
        if (ui.settingsSection == SettingsSection::Account) {
            if (hit({392, 566, 160, 42}, x, y)) {
                startDeviceLogin(ui);
                return;
            }
            if (hit({570, 566, 205, 42}, x, y)) {
                if (ui.signedIn) {
                    fetchSubscriptionInfo(ui);
                } else {
                    startDeviceLogin(ui);
                }
                return;
            }
        }
        if (ui.settingsSection == SettingsSection::Display) {
            for (std::size_t i = 0; i < 4; ++i) {
                const float tileX = 392.0f + static_cast<float>(i) * 190.0f;
                if (hit({tileX, 456, 170, 74}, x, y)) {
                    ui.resolutionIndex = i;
                    applyResolution(ui);
                    ui.status = "Resolution set to " + resolutionLabel(ui.resolutionIndex);
                    log(ui, "[INFO][SETTINGS] " + ui.status + ".");
                    return;
                }
            }
        }
        return;
    }
    if (hit({90, 322, 140, 48}, x, y)) {
        activateSelected(ui);
        return;
    }
    if (hit({250, 322, 142, 48}, x, y)) {
        fetchCatalog(ui, ui.signedIn);
        return;
    }
    if (hit({250, 664, 104, 36}, x, y)) {
        if (ui.signedIn) fetchCatalog(ui, true);
        else startDeviceLogin(ui);
        return;
    }
    if (hit({372, 664, 140, 36}, x, y)) {
        fetchCatalog(ui, false);
        return;
    }
    if (hit({1084, 664, 100, 36}, x, y)) {
        glfwSetWindowShouldClose(glfwGetCurrentContext(), GLFW_TRUE);
        return;
    }

    const float cardW = 235.0f;
    const float gap = 18.0f;
    const float shelfY = 452.0f;
    if (y >= shelfY && y <= shelfY + 190.0f) {
        const float shelfX = x + ui.shelfScroll - 40.0f;
        const int index = static_cast<int>(shelfX / (cardW + gap));
        const float localX = shelfX - static_cast<float>(index) * (cardW + gap);
        if (index >= 0 && localX >= 0.0f && localX <= cardW && static_cast<std::size_t>(index) < ui.games.size()) {
            ui.selected = static_cast<std::size_t>(index);
            prefetchVisibleImages(ui);
            activateSelected(ui);
        }
    }
}

void pollDeviceLogin(UiState& ui) {
    if (!ui.authPending || nowMs() < ui.nextPollMs) {
        return;
    }
    if (nowMs() >= ui.pollDeadlineMs) {
        ui.authPending = false;
        ui.status = "Login timed out";
        log(ui, "[ERROR][AUTH] Login timed out.");
        return;
    }
    ui.nextPollMs = nowMs() + static_cast<std::uint64_t>(ui.pollInterval) * 1000ULL;
    try {
        auto builder = gfn::AuthRequestBuilder::steamDeck();
        gfn::AuthService service(ui.http, builder);
        const auto poll = service.pollDeviceCode(ui.pollProvider, ui.pollDeviceCode);
        if (poll.status == gfn::DeviceCodePollStatus::Pending || poll.status == gfn::DeviceCodePollStatus::SlowDown) {
            ui.status = "Waiting for NVIDIA approval";
            return;
        }
        if (poll.status != gfn::DeviceCodePollStatus::Authorized) {
            ui.authPending = false;
            ui.status = poll.error.empty() ? "Login failed" : poll.error;
            log(ui, "[ERROR][AUTH] Login failed: " + ui.status);
            return;
        }
        ui.authPending = false;
        ui.session = poll.session;
        ui.signedIn = true;
        ui.status = "Signed in";
        saveSession(ui, ui.session);
        fetchSubscriptionInfo(ui);
        ui.authPopupVisible = false;
        fetchCatalog(ui, true);
    } catch (const std::exception& ex) {
        ui.status = ex.what();
        log(ui, std::string("[ERROR][AUTH] Poll exception: ") + ex.what());
    }
}

void handleInput(UiState& ui, u64 down) {
    if (down & HidNpadButton_Plus) {
        glfwSetWindowShouldClose(glfwGetCurrentContext(), GLFW_TRUE);
    }
    if (ui.authPopupVisible || ui.authPending) {
        if (down & HidNpadButton_B) {
            if (ui.authPending) {
                ui.authPending = false;
                ui.status = "Login cancelled";
                log(ui, "[INFO][AUTH] Login cancelled.");
            }
            ui.authPopupVisible = false;
            return;
        }
        if ((down & HidNpadButton_A) && !ui.authPending) {
            startDeviceLogin(ui);
            return;
        }
        return;
    }
    if (ui.screen == Screen::Stream) {
        if (down & HidNpadButton_B) {
            requestLaunchStop(ui);
            ui.screen = ui.previousScreen == Screen::Stream ? Screen::Details : ui.previousScreen;
        }
        return;
    }
    if (ui.screen == Screen::Details) {
        if (down & HidNpadButton_B) {
            ui.screen = ui.previousScreen == Screen::Details ? Screen::Library : ui.previousScreen;
            return;
        }
        if ((down & HidNpadButton_AnyLeft) && ui.selected > 0) {
            --ui.selected;
            prefetchVisibleImages(ui);
            ui.status = "Details: " + ui.games[ui.selected].title;
        }
        if ((down & HidNpadButton_AnyRight) && ui.selected + 1 < ui.games.size()) {
            ++ui.selected;
            prefetchVisibleImages(ui);
            ui.status = "Details: " + ui.games[ui.selected].title;
        }
        if (down & HidNpadButton_A) {
            playSelectedGame(ui);
        }
        if (down & HidNpadButton_Y) {
            ui.screen = Screen::Library;
            if (ui.signedIn && ui.games.empty()) {
                fetchCatalog(ui, true);
            }
        }
        if (down & HidNpadButton_X) {
            ui.screen = Screen::Store;
            fetchCatalog(ui, false);
        }
        return;
    }
    if (ui.screen != Screen::Settings && (down & HidNpadButton_L)) {
        ui.screen = nextScreen(ui.screen, -1);
        if (ui.screen == Screen::Store || ui.screen == Screen::Search) {
            fetchCatalog(ui, false);
        }
    }
    if (ui.screen != Screen::Settings && (down & HidNpadButton_R)) {
        ui.screen = nextScreen(ui.screen, 1);
        if (ui.screen == Screen::Store || ui.screen == Screen::Search) {
            fetchCatalog(ui, false);
        }
    }
    if (ui.screen == Screen::Settings) {
        if (down & HidNpadButton_B) {
            if (ui.authPending) {
                ui.authPending = false;
                ui.status = "Login cancelled";
                log(ui, "[INFO][AUTH] Login cancelled.");
            } else {
                ui.screen = Screen::Library;
            }
            return;
        }
        if (ui.settingsSection == SettingsSection::Logs) {
            const auto logs = logSnapshot(ui);
            const std::size_t maxScroll = logs.size() > 13 ? logs.size() - 13 : 0;
            if (down & HidNpadButton_AnyUp) {
                ui.logsScroll = std::min(maxScroll, ui.logsScroll + 1);
                return;
            }
            if (down & HidNpadButton_AnyDown) {
                ui.logsScroll = ui.logsScroll == 0 ? 0 : ui.logsScroll - 1;
                return;
            }
            if (down & HidNpadButton_L) {
                ui.settingsSection = sectionFromIndex(settingsIndex(ui.settingsSection) - 1);
                return;
            }
            if (down & HidNpadButton_R) {
                ui.settingsSection = sectionFromIndex(settingsIndex(ui.settingsSection) + 1);
                return;
            }
        }
        if (down & HidNpadButton_AnyUp) {
            ui.settingsSection = sectionFromIndex(settingsIndex(ui.settingsSection) - 1);
        }
        if (down & HidNpadButton_AnyDown) {
            ui.settingsSection = sectionFromIndex(settingsIndex(ui.settingsSection) + 1);
        }
        if (ui.settingsSection == SettingsSection::Display && (down & (HidNpadButton_AnyLeft | HidNpadButton_AnyRight | HidNpadButton_L | HidNpadButton_R))) {
            const int delta = (down & (HidNpadButton_AnyRight | HidNpadButton_R)) ? 1 : -1;
            ui.resolutionIndex = static_cast<std::size_t>((static_cast<int>(ui.resolutionIndex) + delta + 4) % 4);
            applyResolution(ui);
            ui.status = "Resolution set to " + resolutionLabel(ui.resolutionIndex);
            log(ui, "[INFO][SETTINGS] " + ui.status + ".");
        }
        if (down & HidNpadButton_Y) {
            ui.settingsSection = SettingsSection::Logs;
        }
        if (down & HidNpadButton_X) {
            if (ui.settingsSection == SettingsSection::Account && ui.signedIn) {
                fetchSubscriptionInfo(ui);
            } else if (refreshSession(ui)) {
                ui.status = "Token refreshed";
                log(ui, "[INFO][AUTH] Token refreshed from Settings.");
            } else {
                ui.status = "No token refreshed";
                log(ui, "[WARN][AUTH] Token refresh skipped or failed.");
            }
        }
        if (down & HidNpadButton_A) {
            if (ui.settingsSection == SettingsSection::Account) {
                startDeviceLogin(ui);
            } else if (ui.settingsSection == SettingsSection::Display) {
                ui.resolutionIndex = (ui.resolutionIndex + 1) % 4;
                applyResolution(ui);
                ui.status = "Resolution set to " + resolutionLabel(ui.resolutionIndex);
                log(ui, "[INFO][SETTINGS] " + ui.status + ".");
            } else if (ui.settingsSection == SettingsSection::Logs) {
                ui.status = "Logs visible";
            }
        }
        return;
    }
    if (down & HidNpadButton_B) {
        if (ui.authPending) {
            ui.authPending = false;
            ui.status = "Login cancelled";
        }
    }
    if ((down & HidNpadButton_AnyLeft) && ui.selected > 0) {
        --ui.selected;
        prefetchVisibleImages(ui);
    }
    if ((down & HidNpadButton_AnyRight) && ui.selected + 1 < ui.games.size()) {
        ++ui.selected;
        prefetchVisibleImages(ui);
    }
    if (down & HidNpadButton_X) {
        fetchCatalog(ui, false);
    }
    if ((ui.screen == Screen::Store || ui.screen == Screen::Search) && ui.games.empty() && ui.signedIn) {
        fetchCatalog(ui, false);
    }
    if (down & HidNpadButton_Y) {
        if (ui.signedIn) {
            fetchCatalog(ui, true);
        } else {
            startDeviceLogin(ui);
        }
    }
    if (down & HidNpadButton_A) {
        activateSelected(ui);
    }
}

void handleTouch(UiState& ui) {
    HidTouchScreenState state{};
    const auto count = hidGetTouchScreenStates(&state, 1);
    const bool down = count > 0 && state.count > 0;
    if (!down) {
        if (ui.touchWasDown) {
            const float dx = ui.lastTouchX - ui.swipeStartX;
            const float dy = ui.lastTouchY - ui.swipeStartY;
            if (std::abs(dx) > 70.0f && std::abs(dx) > std::abs(dy)) {
                if (dx < 0.0f && ui.selected + 1 < ui.games.size()) ++ui.selected;
                if (dx > 0.0f && ui.selected > 0) --ui.selected;
            } else if (ui.lastTouchX >= 0.0f && ui.lastTouchY >= 0.0f) {
                handlePointer(ui, ui.lastTouchX, ui.lastTouchY);
            }
        }
        ui.touchWasDown = false;
        ui.swipeStartX = -1.0f;
        ui.swipeStartY = -1.0f;
        ui.lastTouchX = -1.0f;
        ui.lastTouchY = -1.0f;
        ui.swipeOffset = 0.0f;
        return;
    }

    const float x = static_cast<float>(state.touches[0].x);
    const float y = static_cast<float>(state.touches[0].y);
    ui.lastTouchX = x;
    ui.lastTouchY = y;
    if (!ui.touchWasDown) {
        ui.touchWasDown = true;
        ui.swipeStartX = x;
        ui.swipeStartY = y;
        return;
    }

    const float dx = x - ui.swipeStartX;
    const float dy = y - ui.swipeStartY;
    ui.swipeOffset = std::clamp(dx, -180.0f, 180.0f);

    (void)dy;
}

void handleMouse(UiState& ui, GLFWwindow* window) {
    const bool down = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    if (down && !ui.mouseWasDown) {
        double x = 0.0;
        double y = 0.0;
        glfwGetCursorPos(window, &x, &y);
        handlePointer(ui, static_cast<float>(x), static_cast<float>(y));
    }
    ui.mouseWasDown = down;
}

} // namespace

int runCustomUiApp() {
    int nxFd = nxlinkStdioForDebug();
    gNxlinkFd = nxFd;
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    if (nxFd >= 0) {
        std::fprintf(stdout, "[INFO][APP] nxlink stdout connected for custom UI.\n");
        std::fprintf(stderr, "[INFO][APP] nxlink stderr connected for custom UI.\n");
        std::fflush(stderr);
        std::fflush(stdout);
    }

    const Result socketRc = socketInitializeDefault();
    const Result romfsRc = romfsInit();
    if (R_FAILED(socketRc)) {
        std::fprintf(stderr, "[ERROR][NETWORK] socketInitializeDefault failed: %d\n", socketRc);
    }
    if (R_FAILED(romfsRc)) {
        std::fprintf(stderr, "[WARN][APP] romfsInit failed: %d\n", romfsRc);
    }

    if (!glfwInit()) {
        std::fprintf(stderr, "[ERROR][APP] glfwInit failed.\n");
        return EXIT_FAILURE;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    GLFWwindow* window = glfwCreateWindow(static_cast<int>(kUiW), static_cast<int>(kUiH), "OpenNOW Switch", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "[ERROR][APP] glfwCreateWindow failed.\n");
        glfwTerminate();
        return EXIT_FAILURE;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    NVGcontext* vg = nvgCreateGLES2(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
    if (!vg) {
        std::fprintf(stderr, "[ERROR][APP] nvgCreateGLES2 failed.\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }
    if (nvgCreateFont(vg, "switch", "romfs:/font/switch_font.ttf") < 0) {
        std::fprintf(stderr, "[WARN][APP] Failed to load romfs font.\n");
    }

    UiState ui;
    ui.vg = vg;
    ui.imageThread = std::thread(imageWorkerLoop, &ui);
    log(ui, "[INFO][APP] Custom full-screen UI started.");
    loadSession(ui);
    if (ui.signedIn) {
        fetchSubscriptionInfo(ui);
        fetchCatalog(ui, true);
    }

    PadState pad;
    padInitializeDefault(&pad);
    PadRepeater repeater;
    padRepeaterInitialize(&repeater, 18, 5);
    hidInitializeTouchScreen();
    hidInitializeMouse();

    while (appletMainLoop() && !glfwWindowShouldClose(window)) {
        glfwPollEvents();
        padUpdate(&pad);
        padRepeaterUpdate(&repeater, padGetButtons(&pad) & (HidNpadButton_AnyLeft | HidNpadButton_AnyRight | HidNpadButton_AnyUp | HidNpadButton_AnyDown | HidNpadButton_L | HidNpadButton_R));
        const auto buttons = padGetButtonsDown(&pad) | padRepeaterGetButtons(&repeater);
        if (buttons) {
            handleInput(ui, buttons);
        }
        handleTouch(ui);
        handleMouse(ui, window);
        pollDeviceLogin(ui);

        int fbW = 0;
        int fbH = 0;
        glfwGetFramebufferSize(window, &fbW, &fbH);
        draw(ui, fbW <= 0 ? static_cast<int>(kUiW) : fbW, fbH <= 0 ? static_cast<int>(kUiH) : fbH);
        glfwSwapBuffers(window);
    }

    {
        requestLaunchStop(ui);
    }
    if (ui.launchThread.joinable()) {
        ui.launchThread.join();
    }

    {
        std::lock_guard<std::mutex> lock(ui.imageMutex);
        ui.imageWorkerStop = true;
        ui.imageQueue.clear();
    }
    ui.imageCv.notify_one();
    if (ui.imageThread.joinable()) {
        ui.imageThread.join();
    }

    for (auto& entry : ui.images) {
        if (entry.second.handle) {
            nvgDeleteImage(vg, entry.second.handle);
        }
    }
    if (ui.streamVideoImage) {
        nvgDeleteImage(vg, ui.streamVideoImage);
    }
    nvgDeleteGLES2(vg);
    glfwDestroyWindow(window);
    glfwTerminate();
    if (R_SUCCEEDED(romfsRc)) {
        romfsExit();
    }
    if (R_SUCCEEDED(socketRc)) {
        socketExit();
    }
    if (nxFd >= 0) {
        close(nxFd);
    }
    gNxlinkFd = -1;
    return EXIT_SUCCESS;
}

} // namespace opennow::ui
