#include "opennow/ui/BorealisApp.hpp"

#include "opennow/core/Log.hpp"
#include "opennow/gfn/Auth.hpp"
#include "opennow/gfn/AuthService.hpp"
#include "opennow/gfn/AuthSessionStore.hpp"
#include "opennow/gfn/Catalog.hpp"
#include "opennow/gfn/InputEncoder.hpp"
#include "opennow/gfn/SignalingClient.hpp"
#include "opennow/media/NativeMediaPipeline.hpp"
#include "opennow/net/CurlHttpClient.hpp"
#include "opennow/ui/Dashboard.hpp"

#include <borealis.hpp>
#include <borealis/views/cells/cell_detail.hpp>

#include <algorithm>
#include <chrono>
#include <exception>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#if defined(OPENNOW_PLATFORM_SWITCH)
#include <switch.h>
#include <unistd.h>
#endif

namespace opennow::ui {
namespace {

void borealisBootLog(const char* message) {
    std::fprintf(stderr, "[INFO][APP] %s\n", message);
    std::fflush(stderr);
}

#if defined(OPENNOW_PLATFORM_SWITCH)
class NxlinkDebugStdio {
public:
    NxlinkDebugStdio() {
        fd_ = nxlinkStdioForDebug();
        if (fd_ >= 0) {
            borealisBootLog("nxlink stderr connected for Borealis UI.");
        } else {
            borealisBootLog("nxlink stderr not connected for Borealis UI.");
        }
    }

    ~NxlinkDebugStdio() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    NxlinkDebugStdio(const NxlinkDebugStdio&) = delete;
    NxlinkDebugStdio& operator=(const NxlinkDebugStdio&) = delete;

private:
    int fd_ = -1;
};
#endif

brls::Label* makeLabel(const std::string& text, float size, NVGcolor color, bool singleLine = false) {
    auto* label = new brls::Label();
    label->setText(text);
    label->setFontSize(size);
    label->setTextColor(color);
    label->setHorizontalAlign(brls::HorizontalAlign::LEFT);
    label->setVerticalAlign(brls::VerticalAlign::CENTER);
    label->setSingleLine(singleLine);
    return label;
}

brls::Box* makeColumn(float padding = 22.0f) {
    auto* box = new brls::Box(brls::Axis::COLUMN);
    box->setPadding(padding);
    box->setAlignItems(brls::AlignItems::STRETCH);
    return box;
}

brls::Box* makePanel(float padding = 14.0f) {
    auto* panel = new brls::Box(brls::Axis::COLUMN);
    panel->setPadding(padding);
    panel->setMargins(0.0f, 0.0f, 18.0f, 0.0f);
    panel->setBackground(brls::ViewBackground::SHAPE_COLOR);
    panel->setBackgroundColor(nvgRGBA(24, 27, 33, 255));
    panel->setBorderColor(nvgRGBA(64, 70, 82, 255));
    panel->setBorderThickness(1.0f);
    panel->setCornerRadius(7.0f);
    panel->setAlignItems(brls::AlignItems::STRETCH);
    return panel;
}

brls::DetailCell* makeDetail(const std::string& label, const std::string& value, StatusTone tone = StatusTone::Pending) {
    auto* cell = new brls::DetailCell();
    cell->setText(label);
    cell->setDetailText(value);

    switch (tone) {
    case StatusTone::Ok:
        cell->setDetailTextColor(nvgRGB(97, 214, 147));
        break;
    case StatusTone::Warning:
        cell->setDetailTextColor(nvgRGB(238, 194, 88));
        break;
    case StatusTone::Blocked:
        cell->setDetailTextColor(nvgRGB(239, 105, 100));
        break;
    case StatusTone::Pending:
        cell->setDetailTextColor(nvgRGB(176, 188, 207));
        break;
    }

    return cell;
}

brls::Button* makeButton(const std::string& text, const brls::ButtonStyle* style) {
    auto* button = new brls::Button();
    button->setText(text);
    button->setStyle(style);
    button->setHeight(46.0f);
    button->setMargins(8.0f, 0.0f, 0.0f, 0.0f);
    return button;
}

brls::ScrollingFrame* makeRightScroll(brls::View* content) {
    auto* scroll = new brls::ScrollingFrame();
    scroll->setGrow(1.0f);
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    scroll->setContentView(content);
    return scroll;
}

brls::HScrollingFrame* makeHorizontalScroll(brls::View* content) {
    auto* scroll = new brls::HScrollingFrame();
    scroll->setGrow(1.0f);
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    scroll->setContentView(content);
    return scroll;
}

brls::Box* makeButtonRow() {
    auto* row = new brls::Box(brls::Axis::ROW);
    row->setAlignItems(brls::AlignItems::CENTER);
    row->setMargins(0.0f, 6.0f, 0.0f, 0.0f);
    return row;
}

brls::Box* makePill(const std::string& text, NVGcolor background, NVGcolor foreground) {
    auto* pill = new brls::Box(brls::Axis::ROW);
    pill->setPadding(5.0f, 10.0f, 5.0f, 10.0f);
    pill->setBackgroundColor(background);
    pill->setCornerRadius(8.0f);
    pill->setShrink(0.0f);
    pill->addView(makeLabel(text, 16.0f, foreground, true));
    return pill;
}

std::string shortTokenState(const std::string& token) {
    return token.empty() ? "Missing" : "Saved";
}

std::string savedTokenState(const AuthSession& session) {
    if (!session.tokens.refreshToken.empty()) {
        return "Refresh token saved";
    }
    if (!session.tokens.clientToken.empty()) {
        return "Client token saved";
    }
    if (!session.tokens.accessToken.empty()) {
        return "Access token saved";
    }
    return "Missing";
}

std::string tokenSummary(const AuthSession& session) {
    return "access=" + std::string(session.tokens.accessToken.empty() ? "missing" : "saved")
        + " id=" + std::string(session.tokens.idToken.empty() ? "missing" : "saved")
        + " refresh=" + std::string(session.tokens.refreshToken.empty() ? "missing" : "saved")
        + " client=" + std::string(session.tokens.clientToken.empty() ? "missing" : "saved");
}

std::string authUrl(const DashboardModel& model) {
    if (!model.account.deviceAuthorization.verificationUriComplete.empty()) {
        return model.account.deviceAuthorization.verificationUriComplete;
    }
    if (!model.account.deviceAuthorization.verificationUri.empty()) {
        return model.account.deviceAuthorization.verificationUri;
    }
    return "https://login.nvidia.com/activate";
}

struct RuntimeLogStore {
    std::mutex mutex;
    std::vector<LogLine> lines;
};

StatusTone toneForLog(LogLevel level) {
    if (level == LogLevel::Info || level == LogLevel::Debug) {
        return StatusTone::Ok;
    }
    if (level == LogLevel::Warning) {
        return StatusTone::Warning;
    }
    return StatusTone::Blocked;
}

void appendRuntimeLog(const std::shared_ptr<RuntimeLogStore>& store, LogLevel level, LogCategory category, std::string message) {
    const auto line = formatLogLine(level, category, message);
    std::fprintf(stderr, "%s\n", line.c_str());
    std::fflush(stderr);

    if (!store) {
        return;
    }

    std::lock_guard<std::mutex> guard(store->mutex);
    store->lines.push_back({level, category, std::move(message)});
    if (store->lines.size() > 160) {
        store->lines.erase(store->lines.begin(), store->lines.begin() + static_cast<std::ptrdiff_t>(store->lines.size() - 160));
    }
}

std::vector<LogLine> snapshotRuntimeLogs(const std::shared_ptr<RuntimeLogStore>& store) {
    if (!store) {
        return {};
    }

    std::lock_guard<std::mutex> guard(store->mutex);
    return store->lines;
}

struct AccountUiState {
    std::mutex mutex;
    bool alive = true;
    bool busy = false;
    bool polling = false;
    std::shared_ptr<RuntimeLogStore> logs;
    std::string sessionPath = gfn::defaultAuthSessionPath();
    LoginProvider pollProvider;
    std::string pollDeviceCode;
    std::uint32_t pollInterval = 5;
    std::uint64_t nextPollMs = 0;
    std::uint64_t pollDeadlineMs = 0;
    brls::DetailCell* status = nullptr;
    brls::DetailCell* profile = nullptr;
    brls::DetailCell* membership = nullptr;
    brls::DetailCell* refreshToken = nullptr;
    brls::DetailCell* clientToken = nullptr;
    brls::DetailCell* url = nullptr;
    brls::DetailCell* code = nullptr;
    brls::DetailCell* expires = nullptr;
    brls::Button* startButton = nullptr;
    brls::Button* refreshButton = nullptr;
};

template <typename Func>
void syncAccount(const std::shared_ptr<AccountUiState>& state, Func func) {
    brls::sync([state, func] {
        std::lock_guard<std::mutex> guard(state->mutex);
        if (!state->alive) {
            return;
        }
        func();
    });
}

void setBusy(const std::shared_ptr<AccountUiState>& state, bool busy) {
    {
        std::lock_guard<std::mutex> guard(state->mutex);
        state->busy = busy;
    }

    syncAccount(state, [state] {
        if (state->startButton) {
            state->startButton->setState(state->busy ? brls::ButtonState::DISABLED : brls::ButtonState::ENABLED);
        }
        if (state->refreshButton) {
            state->refreshButton->setState(state->busy ? brls::ButtonState::DISABLED : brls::ButtonState::ENABLED);
        }
    });
}

bool beginBusy(const std::shared_ptr<AccountUiState>& state, const std::string& duplicateMessage) {
    {
        std::lock_guard<std::mutex> guard(state->mutex);
        if (state->busy) {
            appendRuntimeLog(state->logs, LogLevel::Warning, LogCategory::Auth, duplicateMessage);
            return false;
        }
        state->busy = true;
    }

    syncAccount(state, [state] {
        if (state->startButton) {
            state->startButton->setState(brls::ButtonState::DISABLED);
        }
        if (state->refreshButton) {
            state->refreshButton->setState(brls::ButtonState::DISABLED);
        }
    });
    return true;
}

void showStatus(const std::shared_ptr<AccountUiState>& state, const std::string& text, StatusTone tone) {
    syncAccount(state, [state, text, tone] {
        if (state->status) {
            state->status->setDetailText(text);
            switch (tone) {
            case StatusTone::Ok: state->status->setDetailTextColor(nvgRGB(97, 214, 147)); break;
            case StatusTone::Warning: state->status->setDetailTextColor(nvgRGB(238, 194, 88)); break;
            case StatusTone::Blocked: state->status->setDetailTextColor(nvgRGB(239, 105, 100)); break;
            case StatusTone::Pending: state->status->setDetailTextColor(nvgRGB(176, 188, 207)); break;
            }
        }
    });
}

void showAuthorization(const std::shared_ptr<AccountUiState>& state, const gfn::DeviceAuthorizationInfo& info) {
    syncAccount(state, [state, info] {
        if (state->url) state->url->setDetailText(info.verificationUri.empty() ? "https://login.nvidia.com/activate" : info.verificationUri);
        if (state->code) state->code->setDetailText(info.userCode.empty() ? "---- ----" : info.userCode);
        if (state->expires) state->expires->setDetailText(std::to_string(info.expiresIn) + " seconds");
    });
}

void showSession(const std::shared_ptr<AccountUiState>& state, const AuthSession& session) {
    syncAccount(state, [state, session] {
        if (state->status) state->status->setDetailText("Signed in");
        if (state->profile) state->profile->setDetailText(session.user.displayName.empty() ? session.user.userId : session.user.displayName);
        if (state->membership) state->membership->setDetailText(session.user.membershipTier.empty() ? "UNKNOWN" : session.user.membershipTier);
        if (state->refreshToken) state->refreshToken->setDetailText(savedTokenState(session));
        if (state->clientToken) state->clientToken->setDetailText(shortTokenState(session.tokens.clientToken));
    });
}

bool isBusy(const std::shared_ptr<AccountUiState>& state) {
    std::lock_guard<std::mutex> guard(state->mutex);
    return state->busy;
}

void runDeviceLogin(const std::shared_ptr<AccountUiState>& state) {
#if defined(OPENNOW_HAS_CURL)
    appendRuntimeLog(state->logs, LogLevel::Info, LogCategory::Auth, "Start Device Login pressed.");
    if (!beginBusy(state, "Ignored Start Device Login because another auth operation is already running.")) {
        return;
    }
    showStatus(state, "Requesting device code", StatusTone::Pending);
    appendRuntimeLog(state->logs, LogLevel::Info, LogCategory::Auth, "Requesting NVIDIA device authorization code.");

    try {
        appendRuntimeLog(state->logs, LogLevel::Info, LogCategory::Network, "Requesting device code on UI loop.");
        net::CurlHttpClient http;
        auto builder = gfn::AuthRequestBuilder::steamDeck();
        appendRuntimeLog(state->logs, LogLevel::Info, LogCategory::Auth, std::string("Using auth profile: ") + builder.profileName());
        gfn::AuthService service(http, builder);
        const auto provider = builder.defaultProvider();

        const auto authorization = service.requestDeviceAuthorization({
            .deviceId = gfn::generateOpaqueToken(24),
            .displayName = "Steam Deck",
            .idpId = provider.idpId,
        });
        if (!authorization.ok) {
            appendRuntimeLog(state->logs, LogLevel::Error, LogCategory::Auth, "Device authorization failed: " + authorization.error);
            showStatus(state, authorization.error, StatusTone::Blocked);
            setBusy(state, false);
            return;
        }

        const auto interval = std::max<std::uint32_t>(authorization.authorization.interval, 5);
        const auto now = gfn::unixTimeMs();
        const auto timeoutMs = static_cast<std::uint64_t>(std::max<std::uint32_t>(authorization.authorization.expiresIn, 600)) * 1000ULL;
        {
            std::lock_guard<std::mutex> guard(state->mutex);
            state->polling = true;
            state->pollProvider = provider;
            state->pollDeviceCode = authorization.authorization.deviceCode;
            state->pollInterval = interval;
            state->nextPollMs = now + static_cast<std::uint64_t>(interval) * 1000ULL;
            state->pollDeadlineMs = now + timeoutMs;
            state->busy = true;
        }

        appendRuntimeLog(state->logs, LogLevel::Info, LogCategory::Auth, "Device code received. Polling every " + std::to_string(interval) + " seconds.");
        showAuthorization(state, authorization.authorization);
        showStatus(state, "Waiting for NVIDIA login", StatusTone::Warning);
    } catch (const std::exception& ex) {
        appendRuntimeLog(state->logs, LogLevel::Error, LogCategory::Auth, std::string("Auth exception: ") + ex.what());
        showStatus(state, std::string("Login failed: ") + ex.what(), StatusTone::Blocked);
        setBusy(state, false);
    } catch (...) {
        appendRuntimeLog(state->logs, LogLevel::Error, LogCategory::Auth, "Unknown auth exception.");
        showStatus(state, "Login failed", StatusTone::Blocked);
        setBusy(state, false);
    }
#else
    appendRuntimeLog(state->logs, LogLevel::Error, LogCategory::Auth, "Start Device Login failed: build has no HTTPS client.");
    showStatus(state, "This build has no HTTPS client", StatusTone::Blocked);
#endif
}

void runRefresh(const std::shared_ptr<AccountUiState>& state) {
#if defined(OPENNOW_HAS_CURL)
    appendRuntimeLog(state->logs, LogLevel::Info, LogCategory::Auth, "Refresh Token pressed.");
    if (!beginBusy(state, "Ignored Refresh Token because another auth operation is already running.")) {
        return;
    }
    showStatus(state, "Refreshing saved session", StatusTone::Pending);
    appendRuntimeLog(state->logs, LogLevel::Info, LogCategory::Auth, "Loading saved session from " + state->sessionPath + ".");

    try {
        const auto loaded = gfn::loadAuthSession(state->sessionPath);
        if (!loaded.ok) {
            appendRuntimeLog(state->logs, LogLevel::Error, LogCategory::Auth, "Saved session load failed: " + loaded.error);
            showStatus(state, loaded.error, StatusTone::Blocked);
            setBusy(state, false);
            return;
        }
        appendRuntimeLog(state->logs, LogLevel::Info, LogCategory::Network, "Refreshing NVIDIA OAuth token on UI loop.");
        net::CurlHttpClient http;
        auto builder = gfn::AuthRequestBuilder::steamDeck();
        appendRuntimeLog(state->logs, LogLevel::Info, LogCategory::Auth, std::string("Refreshing with auth profile: ") + builder.profileName());
        gfn::AuthService service(http, builder);
        gfn::AuthResult refreshed;
        std::string refreshSource;
        if (!loaded.session.tokens.clientToken.empty()) {
            refreshSource = "client token";
            refreshed = service.refreshWithClientToken(loaded.session);
        }
        if (!refreshed.ok && !loaded.session.tokens.refreshToken.empty()) {
            if (!refreshed.error.empty()) {
                appendRuntimeLog(state->logs, LogLevel::Warning, LogCategory::Auth, "Client-token refresh failed, trying refresh token: " + refreshed.error);
            }
            refreshSource = "refresh token";
            refreshed = service.refreshWithRefreshToken(loaded.session);
        }
        if (refreshSource.empty()) {
            appendRuntimeLog(state->logs, LogLevel::Warning, LogCategory::Auth, "Saved session has no client token or refresh token; keeping current access token session.");
            showStatus(state, "No refreshable token saved", StatusTone::Warning);
            showSession(state, loaded.session);
            setBusy(state, false);
            return;
        }
        if (!refreshed.ok) {
            appendRuntimeLog(state->logs, LogLevel::Error, LogCategory::Auth, "Token refresh failed: " + refreshed.error);
            showStatus(state, refreshed.error, StatusTone::Blocked);
            setBusy(state, false);
            return;
        }

        std::string saveError;
        if (!gfn::saveAuthSession(refreshed.session, state->sessionPath, saveError)) {
            appendRuntimeLog(state->logs, LogLevel::Warning, LogCategory::Auth, "Refresh succeeded but session save failed: " + saveError);
            showStatus(state, "Refresh ok, save failed: " + saveError, StatusTone::Warning);
        } else {
            appendRuntimeLog(state->logs, LogLevel::Info, LogCategory::Auth, "Session refreshed via " + refreshSource + " and saved. " + tokenSummary(refreshed.session));
            showStatus(state, "Session refreshed", StatusTone::Ok);
        }
        showSession(state, refreshed.session);
        setBusy(state, false);
    } catch (const std::exception& ex) {
        appendRuntimeLog(state->logs, LogLevel::Error, LogCategory::Auth, std::string("Refresh exception: ") + ex.what());
        showStatus(state, std::string("Refresh failed: ") + ex.what(), StatusTone::Blocked);
        setBusy(state, false);
    } catch (...) {
        appendRuntimeLog(state->logs, LogLevel::Error, LogCategory::Auth, "Unknown refresh exception.");
        showStatus(state, "Refresh failed", StatusTone::Blocked);
        setBusy(state, false);
    }
#else
    appendRuntimeLog(state->logs, LogLevel::Error, LogCategory::Auth, "Refresh failed: build has no HTTPS client.");
    showStatus(state, "This build has no HTTPS client", StatusTone::Blocked);
#endif
}

void pollDeviceLoginIfDue(const std::shared_ptr<AccountUiState>& state) {
#if defined(OPENNOW_HAS_CURL)
    LoginProvider provider;
    std::string deviceCode;
    std::uint32_t interval = 5;
    const auto now = gfn::unixTimeMs();
    {
        std::lock_guard<std::mutex> guard(state->mutex);
        if (!state->polling || now < state->nextPollMs) {
            return;
        }
        if (now >= state->pollDeadlineMs) {
            state->polling = false;
            state->busy = false;
            deviceCode.clear();
        } else {
            provider = state->pollProvider;
            deviceCode = state->pollDeviceCode;
            interval = state->pollInterval;
            state->nextPollMs = now + static_cast<std::uint64_t>(interval) * 1000ULL;
        }
    }

    if (deviceCode.empty()) {
        appendRuntimeLog(state->logs, LogLevel::Error, LogCategory::Auth, "Login timed out.");
        showStatus(state, "Login timed out", StatusTone::Blocked);
        setBusy(state, false);
        return;
    }

    try {
        net::CurlHttpClient http;
        auto builder = gfn::AuthRequestBuilder::steamDeck();
        gfn::AuthService service(http, builder);
        const auto polled = service.pollDeviceCode(provider, deviceCode);
        if (polled.status == gfn::DeviceCodePollStatus::Authorized) {
            appendRuntimeLog(state->logs, LogLevel::Info, LogCategory::Auth, "NVIDIA approved device login.");
            std::string saveError;
            if (!gfn::saveAuthSession(polled.session, state->sessionPath, saveError)) {
                appendRuntimeLog(state->logs, LogLevel::Warning, LogCategory::Auth, "Login succeeded but session save failed: " + saveError);
                showStatus(state, "Login ok, save failed", StatusTone::Warning);
            } else {
                appendRuntimeLog(
                    state->logs,
                    LogLevel::Info,
                    LogCategory::Auth,
                    "Saved auth session to " + state->sessionPath + ". " + tokenSummary(polled.session));
                showStatus(state, "Signed in and saved", StatusTone::Ok);
            }
            showSession(state, polled.session);
            {
                std::lock_guard<std::mutex> guard(state->mutex);
                state->polling = false;
                state->busy = false;
            }
            setBusy(state, false);
            return;
        }
        if (polled.status == gfn::DeviceCodePollStatus::Pending) {
            appendRuntimeLog(state->logs, LogLevel::Debug, LogCategory::Auth, "Login still pending.");
            showStatus(state, "Waiting for approval", StatusTone::Warning);
            return;
        }
        if (polled.status == gfn::DeviceCodePollStatus::SlowDown) {
            {
                std::lock_guard<std::mutex> guard(state->mutex);
                state->pollInterval += 5;
            }
            appendRuntimeLog(state->logs, LogLevel::Warning, LogCategory::Auth, "NVIDIA requested slower polling.");
            showStatus(state, "Polling slowed by NVIDIA", StatusTone::Warning);
            return;
        }

        if (polled.status == gfn::DeviceCodePollStatus::Expired) {
            appendRuntimeLog(state->logs, LogLevel::Error, LogCategory::Auth, "Login code expired.");
            showStatus(state, "Login code expired", StatusTone::Blocked);
        } else if (polled.status == gfn::DeviceCodePollStatus::AccessDenied) {
            appendRuntimeLog(state->logs, LogLevel::Error, LogCategory::Auth, "Login denied.");
            showStatus(state, "Login denied", StatusTone::Blocked);
        } else {
            appendRuntimeLog(state->logs, LogLevel::Error, LogCategory::Auth, polled.error.empty() ? "Login failed." : "Login failed: " + polled.error);
            showStatus(state, polled.error.empty() ? "Login failed" : polled.error, StatusTone::Blocked);
        }
        {
            std::lock_guard<std::mutex> guard(state->mutex);
            state->polling = false;
            state->busy = false;
        }
        setBusy(state, false);
    } catch (const std::exception& ex) {
        appendRuntimeLog(state->logs, LogLevel::Error, LogCategory::Auth, std::string("Poll exception: ") + ex.what());
        showStatus(state, "Poll exception", StatusTone::Blocked);
        {
            std::lock_guard<std::mutex> guard(state->mutex);
            state->polling = false;
            state->busy = false;
        }
        setBusy(state, false);
    }
#endif
}

class AccountAuthView : public brls::Box {
public:
    AccountAuthView(DashboardModel model, std::shared_ptr<RuntimeLogStore> logs)
        : brls::Box(brls::Axis::COLUMN), state_(std::make_shared<AccountUiState>()) {
        state_->logs = std::move(logs);
        setPadding(22.0f);
        setAlignItems(brls::AlignItems::STRETCH);

        auto* summary = makePanel();
        summary->addView(makeLabel("Account", 28.0f, nvgRGB(245, 247, 250), true));
        state_->status = makeDetail("State", "Loading saved session", StatusTone::Pending);
        state_->profile = makeDetail("Profile", "Not signed in", StatusTone::Pending);
        state_->refreshToken = makeDetail("Refresh token", "Missing", StatusTone::Pending);
        summary->addView(state_->status);
        summary->addView(state_->profile);
        summary->addView(state_->refreshToken);
        addView(summary);

        auto* login = makePanel();
        login->addView(makeLabel("NVIDIA Login", 28.0f, nvgRGB(245, 247, 250), true));
        state_->url = makeDetail("URL", authUrl(model), StatusTone::Pending);
        state_->code = makeDetail("Code", model.account.deviceAuthorization.userCode.empty() ? "---- ----" : model.account.deviceAuthorization.userCode, StatusTone::Warning);
        state_->expires = makeDetail("Expires", "Waiting for authorization request", StatusTone::Pending);
        login->addView(state_->url);
        login->addView(state_->code);
        login->addView(state_->expires);

        auto* row = makeButtonRow();
        state_->startButton = makeButton("Start Login", &brls::BUTTONSTYLE_PRIMARY);
        state_->startButton->registerClickAction([state = state_](brls::View*) {
            runDeviceLogin(state);
            return true;
        });
        state_->refreshButton = makeButton("Refresh", &brls::BUTTONSTYLE_DEFAULT);
        state_->refreshButton->registerClickAction([state = state_](brls::View*) {
            runRefresh(state);
            return true;
        });
        row->addView(state_->startButton);
        row->addView(state_->refreshButton);
        login->addView(row);
        addView(login);

        const auto loaded = gfn::loadAuthSession(state_->sessionPath);
        if (loaded.ok) {
            appendRuntimeLog(state_->logs, LogLevel::Info, LogCategory::Auth, "Loaded saved auth session from " + state_->sessionPath + ".");
            showStatus(state_, "Saved session loaded", StatusTone::Ok);
            showSession(state_, loaded.session);
        } else {
            appendRuntimeLog(state_->logs, LogLevel::Info, LogCategory::Auth, "No saved auth session loaded: " + loaded.error);
            showStatus(state_, "Not signed in", StatusTone::Pending);
        }

        pollTimer_.setCallback([state = state_] {
            pollDeviceLoginIfDue(state);
        });
        pollTimer_.start(1000);
    }

    ~AccountAuthView() override {
        pollTimer_.stop();
        std::lock_guard<std::mutex> guard(state_->mutex);
        state_->alive = false;
    }

private:
    std::shared_ptr<AccountUiState> state_;
    brls::RepeatingTimer pollTimer_;
};

brls::View* createAccountTab(DashboardModel model, std::shared_ptr<RuntimeLogStore> logs) {
    return new AccountAuthView(std::move(model), std::move(logs));
}

struct CatalogUiState {
    std::mutex mutex;
    bool alive = true;
    bool busy = false;
    std::shared_ptr<RuntimeLogStore> logs;
    std::string sessionPath = gfn::defaultAuthSessionPath();
    brls::DetailCell* status = nullptr;
    brls::DetailCell* region = nullptr;
    brls::DetailCell* count = nullptr;
    brls::DetailCell* account = nullptr;
    brls::Box* list = nullptr;
    brls::Button* libraryButton = nullptr;
    brls::Button* allButton = nullptr;
    brls::Button* previousButton = nullptr;
    brls::Button* nextButton = nullptr;
    std::vector<gfn::CatalogGame> games;
    std::size_t page = 0;
    std::size_t pageSize = 80;
    std::size_t selected = 0;
    bool libraryOnly = true;
};

template <typename Func>
void syncCatalog(const std::shared_ptr<CatalogUiState>& state, Func func) {
    brls::sync([state, func] {
        std::lock_guard<std::mutex> guard(state->mutex);
        if (!state->alive) {
            return;
        }
        func();
    });
}

void setCatalogBusy(const std::shared_ptr<CatalogUiState>& state, bool busy) {
    {
        std::lock_guard<std::mutex> guard(state->mutex);
        state->busy = busy;
    }

    syncCatalog(state, [state] {
        if (state->libraryButton) {
            state->libraryButton->setState(state->busy ? brls::ButtonState::DISABLED : brls::ButtonState::ENABLED);
        }
        if (state->allButton) {
            state->allButton->setState(state->busy ? brls::ButtonState::DISABLED : brls::ButtonState::ENABLED);
        }
        if (state->previousButton) {
            state->previousButton->setState(state->busy || state->page == 0 ? brls::ButtonState::DISABLED : brls::ButtonState::ENABLED);
        }
        if (state->nextButton) {
            const auto pageCount = state->games.empty() ? 1 : ((state->games.size() + state->pageSize - 1) / state->pageSize);
            state->nextButton->setState(state->busy || state->page + 1 >= pageCount ? brls::ButtonState::DISABLED : brls::ButtonState::ENABLED);
        }
    });
}

bool beginCatalogBusy(const std::shared_ptr<CatalogUiState>& state, const std::string& duplicateMessage) {
    {
        std::lock_guard<std::mutex> guard(state->mutex);
        if (state->busy) {
            appendRuntimeLog(state->logs, LogLevel::Warning, LogCategory::Session, duplicateMessage);
            return false;
        }
        state->busy = true;
    }
    syncCatalog(state, [state] {
        if (state->libraryButton) {
            state->libraryButton->setState(brls::ButtonState::DISABLED);
        }
        if (state->allButton) {
            state->allButton->setState(brls::ButtonState::DISABLED);
        }
        if (state->previousButton) {
            state->previousButton->setState(brls::ButtonState::DISABLED);
        }
        if (state->nextButton) {
            state->nextButton->setState(brls::ButtonState::DISABLED);
        }
    });
    return true;
}

std::string selectedStoreSummary(const gfn::CatalogGame& game) {
    std::string stores;
    for (const auto& variant : game.variants) {
        if (variant.store.empty()) {
            continue;
        }
        if (stores.find(variant.store) != std::string::npos) {
            continue;
        }
        if (!stores.empty()) {
            stores += ", ";
        }
        stores += variant.store;
    }
    return stores.empty() ? "Unknown store" : stores;
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
        if (variant.libraryStatus != "NOT_OWNED" && !variant.libraryStatus.empty()) {
            return "READY TO PLAY";
        }
    }
    if (game.playabilityState == "PLAYABLE" || game.playabilityState == "STREAMABLE" || game.playabilityState == "READY_TO_PLAY") {
        return "READY TO PLAY";
    }
    if (game.playabilityState.empty()) {
        return "AVAILABLE";
    }
    std::string status = game.playabilityState;
    std::replace(status.begin(), status.end(), '_', ' ');
    return status;
}

std::string gameDeckLine(const gfn::CatalogGame& game) {
    return primaryStore(game) + " | " + readableStatus(game);
}

StatusTone catalogTone(const gfn::CatalogGame& game) {
    if (game.playabilityState == "PLAYABLE" || game.playabilityState == "STREAMABLE" || game.playabilityState == "READY_TO_PLAY") {
        return StatusTone::Ok;
    }
    for (const auto& variant : game.variants) {
        if (variant.libraryStatus != "NOT_OWNED" && !variant.libraryStatus.empty()) {
            return StatusTone::Ok;
        }
    }
    if (game.playabilityState.find("UPGRADE") != std::string::npos) {
        return StatusTone::Warning;
    }
    return StatusTone::Pending;
}

std::string catalogDetail(const gfn::CatalogGame& game) {
    std::ostringstream out;
    out << selectedStoreSummary(game);
    if (!game.playType.empty()) {
        out << " | " << game.playType;
    }
    if (!game.minimumTier.empty()) {
        out << " | " << game.minimumTier;
    }
    return out.str();
}

void renderCatalogPageLocked(const std::shared_ptr<CatalogUiState>& state) {
    if (!state->list) {
        return;
    }

    state->list->clearViews(true);
    if (state->games.empty()) {
        state->list->addView(makeDetail(state->libraryOnly ? "My Library" : "Catalog", "No games returned", StatusTone::Pending));
    } else {
        const auto pageCount = (state->games.size() + state->pageSize - 1) / state->pageSize;
        if (state->page >= pageCount) {
            state->page = pageCount - 1;
        }
        const auto first = state->page * state->pageSize;
        const auto last = std::min(first + state->pageSize, state->games.size());

        std::ostringstream pageLabel;
        pageLabel << state->games.size() << " loaded";
        if (pageCount > 1) {
            pageLabel << " | Page " << (state->page + 1) << " of " << pageCount;
        }
        state->list->addView(makeDetail(state->libraryOnly ? "My Library" : "All Games", pageLabel.str(), StatusTone::Pending));

        for (std::size_t i = first; i < last; ++i) {
            const auto& game = state->games[i];
            state->list->addView(makeDetail(game.title, gameDeckLine(game), catalogTone(game)));
        }
    }

    if (state->previousButton) {
        state->previousButton->setState(state->busy || state->page == 0 ? brls::ButtonState::DISABLED : brls::ButtonState::ENABLED);
    }
    if (state->nextButton) {
        const auto pageCount = state->games.empty() ? 1 : ((state->games.size() + state->pageSize - 1) / state->pageSize);
        state->nextButton->setState(state->busy || state->page + 1 >= pageCount ? brls::ButtonState::DISABLED : brls::ButtonState::ENABLED);
    }
}

void stepCatalogPage(const std::shared_ptr<CatalogUiState>& state, int delta) {
    syncCatalog(state, [state, delta] {
        if (state->games.empty()) {
            return;
        }
        const auto pageCount = (state->games.size() + state->pageSize - 1) / state->pageSize;
        if (delta < 0) {
            if (state->page > 0) {
                --state->page;
            }
        } else if (delta > 0 && state->page + 1 < pageCount) {
            ++state->page;
        }
        renderCatalogPageLocked(state);
    });
}

void renderCatalogResult(const std::shared_ptr<CatalogUiState>& state, const gfn::CatalogResult& result, bool libraryOnly) {
    syncCatalog(state, [state, result, libraryOnly] {
        state->games = result.games;
        state->page = 0;
        state->selected = 0;
        state->libraryOnly = libraryOnly;
        if (state->status) {
            state->status->setDetailText(libraryOnly ? "My Library loaded" : "All Games loaded");
            state->status->setDetailTextColor(nvgRGB(97, 214, 147));
        }
        if (state->region) {
            state->region->setDetailText(result.vpcId.empty() ? "Unknown" : result.vpcId);
        }
        if (state->count) {
            std::ostringstream count;
            count << result.games.size();
            if (result.totalCount > 0) {
                count << " shown of " << result.totalCount;
            }
            state->count->setDetailText(count.str());
        }
        if (!state->list) {
            return;
        }
        renderCatalogPageLocked(state);
    });
}

void renderCatalogError(const std::shared_ptr<CatalogUiState>& state, const std::string& error) {
    syncCatalog(state, [state, error] {
        state->games.clear();
        state->page = 0;
        if (state->status) {
            state->status->setDetailText(error);
            state->status->setDetailTextColor(nvgRGB(239, 105, 100));
        }
        if (state->list) {
            state->list->clearViews(true);
            state->list->addView(makeDetail("Catalog", error, StatusTone::Blocked));
        }
        if (state->previousButton) {
            state->previousButton->setState(brls::ButtonState::DISABLED);
        }
        if (state->nextButton) {
            state->nextButton->setState(brls::ButtonState::DISABLED);
        }
    });
}

bool refreshSavedSessionForCatalog(const std::shared_ptr<CatalogUiState>& state, AuthSession& session) {
    if (session.tokens.clientToken.empty() && session.tokens.refreshToken.empty()) {
        appendRuntimeLog(state->logs, LogLevel::Warning, LogCategory::Auth, "Catalog session cannot refresh: no client token or refresh token saved. " + tokenSummary(session));
        return false;
    }

    appendRuntimeLog(state->logs, LogLevel::Info, LogCategory::Auth, "Refreshing saved session before retrying catalog. " + tokenSummary(session));
    net::CurlHttpClient refreshHttp;
    auto builder = gfn::AuthRequestBuilder::steamDeck();
    gfn::AuthService service(refreshHttp, builder);
    gfn::AuthResult refreshed;
    std::string source;
    if (!session.tokens.clientToken.empty()) {
        source = "client token";
        refreshed = service.refreshWithClientToken(session);
    }
    if (!refreshed.ok && !session.tokens.refreshToken.empty()) {
        if (!refreshed.error.empty()) {
            appendRuntimeLog(state->logs, LogLevel::Warning, LogCategory::Auth, "Catalog client-token refresh failed, trying refresh token: " + refreshed.error);
        }
        source = "refresh token";
        refreshed = service.refreshWithRefreshToken(session);
    }
    if (!refreshed.ok) {
        appendRuntimeLog(state->logs, LogLevel::Error, LogCategory::Auth, "Catalog session refresh failed: " + refreshed.error);
        return false;
    }

    std::string saveError;
    if (!gfn::saveAuthSession(refreshed.session, state->sessionPath, saveError)) {
        appendRuntimeLog(state->logs, LogLevel::Warning, LogCategory::Auth, "Catalog refresh succeeded but session save failed: " + saveError);
    } else {
        appendRuntimeLog(state->logs, LogLevel::Info, LogCategory::Auth, "Catalog session refreshed via " + source + " and saved. " + tokenSummary(refreshed.session));
    }
    session = std::move(refreshed.session);
    return true;
}

void runCatalogFetch(const std::shared_ptr<CatalogUiState>& state, bool libraryOnly) {
#if defined(OPENNOW_HAS_CURL)
    appendRuntimeLog(state->logs, LogLevel::Info, LogCategory::Session, libraryOnly ? "Loading GFN library catalog." : "Loading public GFN catalog.");
    if (!beginCatalogBusy(state, "Ignored catalog request because another catalog request is already running.")) {
        return;
    }
    syncCatalog(state, [state, libraryOnly] {
        if (state->status) {
            state->status->setDetailText(libraryOnly ? "Loading My Library" : "Loading All Games");
            state->status->setDetailTextColor(nvgRGB(176, 188, 207));
        }
        if (state->list) {
            state->list->clearViews(true);
            state->list->addView(makeDetail("Catalog", "Loading from GeForce NOW", StatusTone::Pending));
        }
        if (state->previousButton) {
            state->previousButton->setState(brls::ButtonState::DISABLED);
        }
        if (state->nextButton) {
            state->nextButton->setState(brls::ButtonState::DISABLED);
        }
    });

    try {
        AuthSession session;
        const AuthSession* sessionPtr = nullptr;
        if (libraryOnly) {
            const auto loaded = gfn::loadAuthSession(state->sessionPath);
            if (!loaded.ok) {
                appendRuntimeLog(state->logs, LogLevel::Error, LogCategory::Auth, "Catalog library load failed: " + loaded.error);
                renderCatalogError(state, "Sign in before loading My Library");
                setCatalogBusy(state, false);
                return;
            }
            session = loaded.session;
            sessionPtr = &session;
            appendRuntimeLog(state->logs, LogLevel::Info, LogCategory::Auth, "Catalog auth tokens loaded. " + tokenSummary(session));
            syncCatalog(state, [state, session] {
                if (state->account) {
                    state->account->setDetailText(session.user.displayName.empty() ? session.user.userId : session.user.displayName);
                }
            });
        }

        appendRuntimeLog(state->logs, LogLevel::Info, LogCategory::Network, "Catalog HTTP request running on UI loop for Switch stability.");
        net::CurlHttpClient http;
        gfn::CatalogService catalog(http);
        gfn::CatalogFetchOptions options;
        options.libraryOnly = libraryOnly;
        options.first = libraryOnly ? 80 : 50;
        auto result = catalog.fetchCatalog(sessionPtr, options);
        if (!result.ok && libraryOnly && result.error.find("HTTP 401") != std::string::npos && refreshSavedSessionForCatalog(state, session)) {
            appendRuntimeLog(state->logs, LogLevel::Info, LogCategory::Session, "Retrying GFN library catalog after token refresh.");
            sessionPtr = &session;
            result = catalog.fetchCatalog(sessionPtr, options);
        }
        if (!result.ok) {
            appendRuntimeLog(state->logs, LogLevel::Error, LogCategory::Session, "Catalog fetch failed: " + result.error);
            renderCatalogError(state, result.error);
            setCatalogBusy(state, false);
            return;
        }
        appendRuntimeLog(
            state->logs,
            LogLevel::Info,
            LogCategory::Session,
            "Catalog loaded from " + result.vpcId + ": " + std::to_string(result.games.size()) + " items.");
        renderCatalogResult(state, result, libraryOnly);
        setCatalogBusy(state, false);
    } catch (const std::exception& ex) {
        appendRuntimeLog(state->logs, LogLevel::Error, LogCategory::Session, std::string("Catalog exception: ") + ex.what());
        renderCatalogError(state, std::string("Catalog failed: ") + ex.what());
        setCatalogBusy(state, false);
    } catch (...) {
        appendRuntimeLog(state->logs, LogLevel::Error, LogCategory::Session, "Catalog failed with an unknown exception.");
        renderCatalogError(state, "Catalog failed");
        setCatalogBusy(state, false);
    }
#else
    appendRuntimeLog(state->logs, LogLevel::Error, LogCategory::Session, "Catalog fetch failed: build has no HTTPS client.");
    renderCatalogError(state, "This build has no HTTPS client");
#endif
}

class GamesCatalogView : public brls::Box {
public:
    explicit GamesCatalogView(std::shared_ptr<RuntimeLogStore> logs)
        : brls::Box(brls::Axis::ROW), state_(std::make_shared<CatalogUiState>()) {
        state_->logs = std::move(logs);
        setPadding(18.0f);
        setAlignItems(brls::AlignItems::STRETCH);

        auto* controls = makePanel(14.0f);
        controls->setWidth(330.0f);
        controls->setShrink(0.0f);
        controls->setMargins(0.0f, 14.0f, 0.0f, 0.0f);
        controls->addView(makeLabel("Games", 30.0f, nvgRGB(246, 248, 252), true));
        auto* subtitle = makeLabel("GeForce NOW library", 16.0f, nvgRGB(168, 178, 196), true);
        subtitle->setMargins(0.0f, 0.0f, 8.0f, 0.0f);
        controls->addView(subtitle);
        controls->addView(makePill("Steam Deck auth", nvgRGBA(19, 41, 31, 245), nvgRGB(105, 232, 151)));

        state_->status = makeDetail("State", "Ready", StatusTone::Pending);
        state_->region = makeDetail("Region", "Unknown", StatusTone::Pending);
        state_->count = makeDetail("Items", "Not loaded", StatusTone::Pending);
        state_->account = makeDetail("Account", "Not signed in", StatusTone::Pending);
        controls->addView(state_->status);
        controls->addView(state_->region);
        controls->addView(state_->count);
        controls->addView(state_->account);

        auto* row = makeButtonRow();
        state_->libraryButton = makeButton("Library", &brls::BUTTONSTYLE_PRIMARY);
        state_->libraryButton->registerClickAction([state = state_](brls::View*) {
            runCatalogFetch(state, true);
            return true;
        });
        state_->allButton = makeButton("All", &brls::BUTTONSTYLE_DEFAULT);
        state_->allButton->registerClickAction([state = state_](brls::View*) {
            runCatalogFetch(state, false);
            return true;
        });
        row->addView(state_->libraryButton);
        row->addView(state_->allButton);
        controls->addView(row);

        auto* pageRow = makeButtonRow();
        state_->previousButton = makeButton("Prev", &brls::BUTTONSTYLE_DEFAULT);
        state_->previousButton->setState(brls::ButtonState::DISABLED);
        state_->previousButton->registerClickAction([state = state_](brls::View*) {
            stepCatalogPage(state, -1);
            return true;
        });
        state_->nextButton = makeButton("Next", &brls::BUTTONSTYLE_DEFAULT);
        state_->nextButton->setState(brls::ButtonState::DISABLED);
        state_->nextButton->registerClickAction([state = state_](brls::View*) {
            stepCatalogPage(state, 1);
            return true;
        });
        pageRow->addView(state_->previousButton);
        pageRow->addView(state_->nextButton);
        controls->addView(pageRow);
        addView(controls);

        auto* right = makePanel(14.0f);
        right->setGrow(1.0f);
        right->setMargins(0.0f, 0.0f, 0.0f, 0.0f);
        right->addView(makeLabel("My Library", 30.0f, nvgRGB(246, 248, 252), true));
        auto* hint = makeLabel("Select Library or All to load games", 16.0f, nvgRGB(168, 178, 196), true);
        hint->setMargins(0.0f, 0.0f, 8.0f, 0.0f);
        right->addView(hint);
        state_->list = makeColumn(0.0f);
        state_->list->addView(makeDetail("Catalog", "Press Library or All to load games", StatusTone::Pending));
        right->addView(makeRightScroll(state_->list));
        addView(right);

        const auto loaded = gfn::loadAuthSession(state_->sessionPath);
        if (loaded.ok) {
            appendRuntimeLog(state_->logs, LogLevel::Info, LogCategory::Auth, "Catalog found saved auth session.");
            if (state_->account) {
                state_->account->setDetailText(loaded.session.user.displayName.empty() ? loaded.session.user.userId : loaded.session.user.displayName);
            }
        } else {
            appendRuntimeLog(state_->logs, LogLevel::Info, LogCategory::Auth, "Catalog has no saved auth session: " + loaded.error);
            if (state_->status) {
                state_->status->setDetailText("Ready");
            }
        }
    }

    ~GamesCatalogView() override {
        std::lock_guard<std::mutex> guard(state_->mutex);
        state_->alive = false;
    }

private:
    std::shared_ptr<CatalogUiState> state_;
};

brls::View* createGamesTab(std::shared_ptr<RuntimeLogStore> logs) {
    return new GamesCatalogView(std::move(logs));
}

class RuntimeLogsView : public brls::Box {
public:
    explicit RuntimeLogsView(std::shared_ptr<RuntimeLogStore> logs)
        : brls::Box(brls::Axis::COLUMN), logs_(std::move(logs)) {
        setPadding(22.0f);
        setAlignItems(brls::AlignItems::STRETCH);

        auto* header = makePanel();
        header->addView(makeLabel("Logs", 28.0f, nvgRGB(245, 247, 250), true));
        auto* refresh = makeButton("Refresh Logs", &brls::BUTTONSTYLE_DEFAULT);
        refresh->registerClickAction([this](brls::View*) {
            renderLogs();
            return true;
        });
        previous_ = makeButton("Prev", &brls::BUTTONSTYLE_DEFAULT);
        previous_->registerClickAction([this](brls::View*) {
            if (page_ > 0) {
                --page_;
            }
            renderLogs();
            return true;
        });
        next_ = makeButton("Next", &brls::BUTTONSTYLE_DEFAULT);
        next_->registerClickAction([this](brls::View*) {
            ++page_;
            renderLogs();
            return true;
        });
        auto* row = makeButtonRow();
        row->addView(refresh);
        row->addView(previous_);
        row->addView(next_);
        header->addView(row);
        addView(header);

        list_ = makeColumn(0.0f);
        addView(list_);
        renderLogs();
    }

private:
    void renderLogs() {
        if (!list_) {
            return;
        }

        list_->clearViews(true);
        const auto logs = snapshotRuntimeLogs(logs_);
        if (logs.empty()) {
            list_->addView(makeDetail("Logs", "No logs recorded yet", StatusTone::Pending));
            return;
        }

        const std::size_t pageSize = 5;
        const auto pageCount = (logs.size() + pageSize - 1) / pageSize;
        if (page_ >= pageCount) {
            page_ = pageCount - 1;
        }
        const auto reversePage = pageCount - 1 - page_;
        const auto first = reversePage * pageSize;
        const auto last = std::min(first + pageSize, logs.size());
        std::ostringstream pageLabel;
        pageLabel << "Page " << (page_ + 1) << " of " << pageCount << " (newest first)";
        list_->addView(makeDetail("Logs", pageLabel.str(), StatusTone::Pending));
        for (std::size_t i = first; i < last; ++i) {
            const auto& log = logs[i];
            list_->addView(makeDetail(std::string(toString(log.category)), formatLogLine(log.level, log.category, log.message), toneForLog(log.level)));
        }
        if (previous_) {
            previous_->setState(page_ == 0 ? brls::ButtonState::DISABLED : brls::ButtonState::ENABLED);
        }
        if (next_) {
            next_->setState(page_ + 1 >= pageCount ? brls::ButtonState::DISABLED : brls::ButtonState::ENABLED);
        }
    }

    std::shared_ptr<RuntimeLogStore> logs_;
    brls::Box* list_ = nullptr;
    brls::Button* previous_ = nullptr;
    brls::Button* next_ = nullptr;
    std::size_t page_ = 0;
};

brls::View* createLogsTab(std::shared_ptr<RuntimeLogStore> logs) {
    return new RuntimeLogsView(std::move(logs));
}

brls::View* createDiagnosticsTab(DashboardModel model) {
    auto* root = makeColumn();

    auto* stream = makePanel();
    stream->addView(makeLabel("Diagnostics", 28.0f, nvgRGB(245, 247, 250), true));
    for (const auto& status : model.status) {
        stream->addView(makeDetail(status.label, status.value, status.tone));
    }
    root->addView(stream);

    auto* transport = makePanel();
    transport->addView(makeLabel("Stream Target", 28.0f, nvgRGB(245, 247, 250), true));
    transport->addView(makeDetail("Resolution", "1280x720", StatusTone::Ok));
    transport->addView(makeDetail("Frame rate", "60 FPS", StatusTone::Ok));
    transport->addView(makeDetail("Codec", "H264", StatusTone::Ok));
    transport->addView(makeDetail("Audio", "Stereo", StatusTone::Ok));
    root->addView(transport);

    return root;
}

DashboardModel createRuntimeDashboard() {
    auto dashboard = buildStartupDashboard();

    gfn::InputEncoder encoder;
    encoder.setProtocolVersion(3);
    const auto heartbeat = encoder.encodeHeartbeat();

    std::ostringstream inputMessage;
    inputMessage << "GFN input encoder initialized. Heartbeat packet bytes=" << heartbeat.size();
    dashboard.logs.push_back({LogLevel::Info, LogCategory::Input, inputMessage.str()});

    gfn::SignalingClient signaling({
        "example.invalid:443",
        "session-placeholder",
        "wss://example.invalid/nvst/",
    });
    dashboard.logs.push_back({LogLevel::Info, LogCategory::Signaling, "Signaling boundary ready: " + signaling.signInUrl()});

    media::NativeMediaPipeline media;
    StreamSettings settings;
    const auto mediaStatus = media.open(settings, {});
    dashboard.logs.push_back({
        mediaStatus.ready ? LogLevel::Info : LogLevel::Warning,
        LogCategory::Media,
        mediaStatus.message,
    });

    return dashboard;
}

class OpenNowActivity : public brls::Activity {
public:
    brls::View* createContentView() override {
        DashboardModel model = createRuntimeDashboard();
        auto runtimeLogs = std::make_shared<RuntimeLogStore>();
        for (const auto& log : model.logs) {
            appendRuntimeLog(runtimeLogs, log.level, log.category, log.message);
        }
        appendRuntimeLog(runtimeLogs, LogLevel::Info, LogCategory::App, "Borealis UI initialized. Account and runtime logs are active.");

        auto* tabs = new brls::TabFrame();
        tabs->addTab("Account", [model, runtimeLogs] { return createAccountTab(model, runtimeLogs); });
        tabs->addTab("Games", [runtimeLogs] { return createGamesTab(runtimeLogs); });
        tabs->addTab("Logs", [runtimeLogs] { return createLogsTab(runtimeLogs); });
        tabs->addTab("Diagnostics", [model] { return createDiagnosticsTab(model); });
        tabs->focusTab(0);

        auto* frame = new brls::AppletFrame(tabs);
        frame->setTitle("OpenNOW Switch");
        return frame;
    }

    void onContentAvailable() override {
        registerExitAction(brls::BUTTON_START);
    }
};

} // namespace

int runBorealisApp() {
#if defined(OPENNOW_PLATFORM_SWITCH)
    NxlinkDebugStdio nxlink;
#endif
    borealisBootLog("Borealis UI startup entered.");
    brls::Logger::setLogLevel(brls::LogLevel::LOG_INFO);
    brls::Platform::APP_LOCALE_DEFAULT = brls::LOCALE_AUTO;

    borealisBootLog("Calling brls::Application::init().");
    if (!brls::Application::init()) {
        borealisBootLog("brls::Application::init() failed.");
        brls::Logger::error("Unable to init Borealis application");
        return EXIT_FAILURE;
    }

    borealisBootLog("Creating Borealis window.");
    brls::Application::createWindow("OpenNOW Switch");
    borealisBootLog("Borealis window created.");
    brls::Application::getPlatform()->setThemeVariant(brls::ThemeVariant::DARK);
    brls::Application::setGlobalQuit(false);
    borealisBootLog("Pushing OpenNOW activity.");
    brls::Application::pushActivity(new OpenNowActivity());
    borealisBootLog("Entering Borealis main loop.");

    while (brls::Application::mainLoop()) {
    }

    borealisBootLog("Borealis main loop exited.");
    return EXIT_SUCCESS;
}

} // namespace opennow::ui
