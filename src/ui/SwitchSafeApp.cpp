#include "opennow/ui/SwitchSafeApp.hpp"

#include "opennow/core/Log.hpp"
#include "opennow/gfn/Auth.hpp"
#include "opennow/gfn/AuthService.hpp"
#include "opennow/gfn/AuthSessionStore.hpp"
#include "opennow/net/CurlHttpClient.hpp"
#include "opennow/util/Json.hpp"

#include <switch.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <unistd.h>
#include <vector>

namespace opennow::ui {
namespace {

struct SafeLogLine {
    LogLevel level = LogLevel::Info;
    LogCategory category = LogCategory::App;
    std::string message;
};

struct SafeUiState {
    std::mutex mutex;
    std::atomic_bool alive = true;
    bool busy = false;
    bool signedIn = false;
    bool showLogs = false;
    bool importReady = false;
    std::string importToken;
    std::string status = "Ready";
    std::string url = "https://login.nvidia.com/activate";
    std::string code = "---- ----";
    std::string expires = "Waiting";
    std::string profile = "Not signed in";
    std::string membership = "Unknown";
    std::string sessionPath = gfn::defaultAuthSessionPath();
    bool polling = false;
    LoginProvider pollProvider;
    std::string pollDeviceCode;
    std::uint32_t pollInterval = 5;
    std::uint64_t nextPollMs = 0;
    std::uint64_t pollDeadlineMs = 0;
    std::vector<SafeLogLine> logs;
};

struct SafeUiSnapshot {
    bool busy = false;
    bool signedIn = false;
    bool showLogs = false;
    bool importReady = false;
    std::string importToken;
    std::string status;
    std::string url;
    std::string code;
    std::string expires;
    std::string profile;
    std::string membership;
    std::string sessionPath;
    std::vector<SafeLogLine> logs;
};

void addLog(const std::shared_ptr<SafeUiState>& state, LogLevel level, LogCategory category, std::string message) {
    const auto stdoutLine = formatLogLine(level, category, message);
    std::fprintf(stderr, "%s\n", stdoutLine.c_str());
    std::fflush(stderr);

    std::lock_guard<std::mutex> guard(state->mutex);
    state->logs.push_back({level, category, std::move(message)});
    if (state->logs.size() > 120) {
        state->logs.erase(state->logs.begin(), state->logs.begin() + static_cast<std::ptrdiff_t>(state->logs.size() - 120));
    }
}

void setStatus(const std::shared_ptr<SafeUiState>& state, std::string status) {
    std::lock_guard<std::mutex> guard(state->mutex);
    state->status = std::move(status);
}

void setBusy(const std::shared_ptr<SafeUiState>& state, bool busy) {
    std::lock_guard<std::mutex> guard(state->mutex);
    state->busy = busy;
}

bool beginBusy(const std::shared_ptr<SafeUiState>& state, const char* message) {
    {
        std::lock_guard<std::mutex> guard(state->mutex);
        if (!state->busy) {
            state->busy = true;
            return true;
        }
    }

    addLog(state, LogLevel::Warning, LogCategory::Auth, message);
    return false;
}

bool isSignedInOrBusy(const std::shared_ptr<SafeUiState>& state) {
    std::lock_guard<std::mutex> guard(state->mutex);
    return state->signedIn || state->busy;
}

void applyAuthorization(const std::shared_ptr<SafeUiState>& state, const gfn::DeviceAuthorizationInfo& info) {
    std::lock_guard<std::mutex> guard(state->mutex);
    state->url = info.verificationUriComplete.empty() ? info.verificationUri : info.verificationUriComplete;
    state->code = info.userCode.empty() ? "---- ----" : info.userCode;
    state->expires = std::to_string(info.expiresIn) + " seconds";
    state->status = "Open URL on phone/PC and enter code";
}

void applySession(const std::shared_ptr<SafeUiState>& state, const AuthSession& session) {
    std::lock_guard<std::mutex> guard(state->mutex);
    state->signedIn = true;
    state->profile = session.user.displayName.empty() ? session.user.userId : session.user.displayName;
    state->membership = session.user.membershipTier.empty() ? "Unknown" : session.user.membershipTier;
}

void setImportReady(const std::shared_ptr<SafeUiState>& state, bool ready) {
    std::lock_guard<std::mutex> guard(state->mutex);
    state->importReady = ready;
}

std::string importToken(const std::shared_ptr<SafeUiState>& state) {
    std::lock_guard<std::mutex> guard(state->mutex);
    return state->importToken;
}

bool ensureSessionDirectory(const std::string& path, std::string& error) {
    const auto slash = path.find_last_of("/\\");
    if (slash == std::string::npos) {
        return true;
    }
    const auto directory = path.substr(0, slash);

    std::string current;
    std::size_t index = 0;
    const auto colon = directory.find(':');
    if (colon != std::string::npos) {
        current = directory.substr(0, colon + 1);
        index = colon + 1;
    }

    while (index < directory.size()) {
        while (index < directory.size() && (directory[index] == '/' || directory[index] == '\\')) {
            if (!current.empty() && current.back() != '/' && current.back() != '\\') {
                current.push_back(directory[index]);
            }
            ++index;
        }
        const auto next = directory.find_first_of("/\\", index);
        const auto part = directory.substr(index, next == std::string::npos ? std::string::npos : next - index);
        if (!part.empty()) {
            if (!current.empty() && current.back() != '/' && current.back() != '\\') {
                current.push_back('/');
            }
            current += part;
            if (mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
                error = "failed to create " + current;
                return false;
            }
        }
        if (next == std::string::npos) {
            break;
        }
        index = next + 1;
    }
    return true;
}

bool saveRawSession(const std::shared_ptr<SafeUiState>& state, const std::string& body, std::string& error) {
    const auto parsed = util::parseJson(body);
    if (!parsed.ok) {
        error = "import body is not JSON: " + parsed.error;
        return false;
    }
    const auto* tokens = parsed.value.get("tokens");
    const auto* refresh = tokens ? tokens->get("refreshToken") : nullptr;
    if (!refresh || refresh->asString().empty()) {
        error = "import body does not contain tokens.refreshToken";
        return false;
    }

    if (!ensureSessionDirectory(state->sessionPath, error)) {
        return false;
    }

    std::ofstream file(state->sessionPath, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "failed to open session file for writing";
        return false;
    }
    file << body;
    if (!file) {
        error = "failed to write session file";
        return false;
    }
    return true;
}

int startImportServer(const std::shared_ptr<SafeUiState>& state) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        addLog(state, LogLevel::Error, LogCategory::Network, "Import server socket() failed.");
        return -1;
    }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(29146);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        addLog(state, LogLevel::Error, LogCategory::Network, "Import server bind(:29146) failed.");
        close(fd);
        return -1;
    }
    if (listen(fd, 2) != 0) {
        addLog(state, LogLevel::Error, LogCategory::Network, "Import server listen() failed.");
        close(fd);
        return -1;
    }

    addLog(state, LogLevel::Info, LogCategory::Network, "Import server listening on http://<switch-ip>:29146/auth-session");
    setImportReady(state, true);
    return fd;
}

std::size_t parseContentLength(const std::string& headers) {
    const auto key = std::string("Content-Length:");
    auto pos = headers.find(key);
    if (pos == std::string::npos) {
        pos = headers.find("content-length:");
    }
    if (pos == std::string::npos) {
        return 0;
    }
    pos += key.size();
    while (pos < headers.size() && headers[pos] == ' ') {
        ++pos;
    }
    return static_cast<std::size_t>(std::strtoul(headers.c_str() + pos, nullptr, 10));
}

bool requestHasImportToken(const std::string& request, const std::string& token) {
    if (token.empty()) {
        return false;
    }
    const auto header = "X-OpenNOW-Import-Token: " + token;
    if (request.find(header) != std::string::npos) {
        return true;
    }
    return request.find("token=" + token) != std::string::npos;
}

void sendHttpResponse(int client, int status, const char* text) {
    std::ostringstream out;
    out << "HTTP/1.1 " << status << (status == 200 ? " OK" : " Bad Request") << "\r\n"
        << "Connection: close\r\n"
        << "Content-Type: text/plain\r\n"
        << "Content-Length: " << std::strlen(text) << "\r\n\r\n"
        << text;
    const auto response = out.str();
    send(client, response.data(), response.size(), 0);
}

void handleImportServer(int fd, const std::shared_ptr<SafeUiState>& state) {
    if (fd < 0) {
        return;
    }

    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(fd, &readSet);
    timeval timeout {};
    const int ready = select(fd + 1, &readSet, nullptr, nullptr, &timeout);
    if (ready <= 0 || !FD_ISSET(fd, &readSet)) {
        return;
    }

    sockaddr_in clientAddr {};
    socklen_t clientLen = sizeof(clientAddr);
    const int client = accept(fd, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
    if (client < 0) {
        return;
    }

    std::string request;
    char buffer[2048];
    while (request.size() < 196 * 1024) {
        const auto read = recv(client, buffer, sizeof(buffer), 0);
        if (read <= 0) {
            break;
        }
        request.append(buffer, buffer + read);
        const auto headerEnd = request.find("\r\n\r\n");
        if (headerEnd != std::string::npos) {
            const auto headers = request.substr(0, headerEnd);
            const auto contentLength = parseContentLength(headers);
            if (request.size() >= headerEnd + 4 + contentLength) {
                break;
            }
        }
    }

    const auto headerEnd = request.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        sendHttpResponse(client, 400, "missing headers\n");
        close(client);
        return;
    }
    const auto expectedToken = importToken(state);
    if (!requestHasImportToken(request, expectedToken)) {
        addLog(state, LogLevel::Warning, LogCategory::Auth, "Rejected unauthenticated session import request.");
        sendHttpResponse(client, 400, "missing import token\n");
        close(client);
        return;
    }

    const auto headers = request.substr(0, headerEnd);
    const auto contentLength = parseContentLength(headers);
    const auto bodyStart = headerEnd + 4;
    const auto body = request.substr(bodyStart, std::min(contentLength, request.size() - bodyStart));
    std::string error;
    if (!saveRawSession(state, body, error)) {
        addLog(state, LogLevel::Error, LogCategory::Auth, "Session import failed: " + error);
        sendHttpResponse(client, 400, "import failed\n");
        close(client);
        return;
    }

    const auto loaded = gfn::loadAuthSession(state->sessionPath);
    if (loaded.ok) {
        applySession(state, loaded.session);
        setStatus(state, "Imported saved session");
        addLog(state, LogLevel::Info, LogCategory::Auth, "Imported auth session over HTTP.");
        sendHttpResponse(client, 200, "imported\n");
    } else {
        addLog(state, LogLevel::Error, LogCategory::Auth, "Imported session could not be loaded: " + loaded.error);
        sendHttpResponse(client, 400, "imported but invalid\n");
    }
    close(client);
}

void startDeviceLogin(const std::shared_ptr<SafeUiState>& state) {
#if defined(OPENNOW_HAS_CURL)
    addLog(state, LogLevel::Info, LogCategory::Auth, "Start Device Login pressed.");
    if (!beginBusy(state, "Ignored login request because auth is already running.")) {
        return;
    }
    setStatus(state, "Requesting NVIDIA device code");

    try {
        addLog(state, LogLevel::Info, LogCategory::Network, "Requesting device code on main loop.");
        net::CurlHttpClient http;
        auto builder = gfn::AuthRequestBuilder::steamDeck();
        addLog(state, LogLevel::Info, LogCategory::Auth, std::string("Using auth profile: ") + builder.profileName());
        gfn::AuthService service(http, builder);
        const auto provider = builder.defaultProvider();

        const auto authorization = service.requestDeviceAuthorization({
            .deviceId = gfn::generateOpaqueToken(24),
            .displayName = "Steam Deck",
            .idpId = provider.idpId,
        });
        if (!authorization.ok) {
            addLog(state, LogLevel::Error, LogCategory::Auth, "Device authorization failed: " + authorization.error);
            if (authorization.error.find("Device flow is not allowed") != std::string::npos) {
                addLog(state, LogLevel::Warning, LogCategory::Auth, "NVIDIA rejected the Steam Deck device-code profile.");
            }
            setStatus(state, "Device authorization failed");
            setBusy(state, false);
            return;
        }

        addLog(state, LogLevel::Info, LogCategory::Auth, "Device code received.");
        applyAuthorization(state, authorization.authorization);

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
        addLog(state, LogLevel::Info, LogCategory::Auth, "Polling token endpoint every " + std::to_string(interval) + " seconds.");
    } catch (const std::exception& ex) {
        addLog(state, LogLevel::Error, LogCategory::Auth, std::string("Auth exception: ") + ex.what());
        setStatus(state, "Login exception");
        setBusy(state, false);
    } catch (...) {
        addLog(state, LogLevel::Error, LogCategory::Auth, "Unknown auth exception.");
        setStatus(state, "Login exception");
        setBusy(state, false);
    }
#else
    addLog(state, LogLevel::Error, LogCategory::Auth, "This build has no HTTPS client.");
    setStatus(state, "No HTTPS client");
#endif
}

void pollDeviceLoginIfDue(const std::shared_ptr<SafeUiState>& state) {
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
            state->status = "Login timed out";
            deviceCode.clear();
        } else {
            provider = state->pollProvider;
            deviceCode = state->pollDeviceCode;
            interval = state->pollInterval;
            state->nextPollMs = now + static_cast<std::uint64_t>(interval) * 1000ULL;
        }
    }
    if (deviceCode.empty()) {
        addLog(state, LogLevel::Error, LogCategory::Auth, "Login timed out.");
        return;
    }

    try {
        net::CurlHttpClient http;
        auto builder = gfn::AuthRequestBuilder::steamDeck();
        gfn::AuthService service(http, builder);
        const auto polled = service.pollDeviceCode(provider, deviceCode);
        if (polled.status == gfn::DeviceCodePollStatus::Authorized) {
            addLog(state, LogLevel::Info, LogCategory::Auth, "NVIDIA approved device login.");
            std::string saveError;
            if (!gfn::saveAuthSession(polled.session, state->sessionPath, saveError)) {
                addLog(state, LogLevel::Warning, LogCategory::Auth, "Session save failed: " + saveError);
                setStatus(state, "Login ok, save failed");
            } else {
                addLog(state, LogLevel::Info, LogCategory::Auth, "Saved auth session to " + state->sessionPath + ".");
                setStatus(state, "Signed in and saved");
            }
            applySession(state, polled.session);
            std::lock_guard<std::mutex> guard(state->mutex);
            state->polling = false;
            state->busy = false;
            return;
        }
        if (polled.status == gfn::DeviceCodePollStatus::Pending) {
            addLog(state, LogLevel::Debug, LogCategory::Auth, "Login still pending.");
            setStatus(state, "Waiting for approval");
            return;
        }
        if (polled.status == gfn::DeviceCodePollStatus::SlowDown) {
            {
                std::lock_guard<std::mutex> guard(state->mutex);
                state->pollInterval += 5;
                state->status = "Polling slowed by NVIDIA";
            }
            addLog(state, LogLevel::Warning, LogCategory::Auth, "NVIDIA requested slower polling.");
            return;
        }

        if (polled.status == gfn::DeviceCodePollStatus::Expired) {
            addLog(state, LogLevel::Error, LogCategory::Auth, "Login code expired.");
            setStatus(state, "Login code expired");
        } else if (polled.status == gfn::DeviceCodePollStatus::AccessDenied) {
            addLog(state, LogLevel::Error, LogCategory::Auth, "Login denied.");
            setStatus(state, "Login denied");
        } else {
            addLog(state, LogLevel::Error, LogCategory::Auth, polled.error.empty() ? "Login failed." : "Login failed: " + polled.error);
            setStatus(state, "Login failed");
        }
        std::lock_guard<std::mutex> guard(state->mutex);
        state->polling = false;
        state->busy = false;
    } catch (const std::exception& ex) {
        addLog(state, LogLevel::Error, LogCategory::Auth, std::string("Poll exception: ") + ex.what());
        setStatus(state, "Poll exception");
        std::lock_guard<std::mutex> guard(state->mutex);
        state->polling = false;
        state->busy = false;
    }
#endif
}

void refreshToken(const std::shared_ptr<SafeUiState>& state) {
#if defined(OPENNOW_HAS_CURL)
    addLog(state, LogLevel::Info, LogCategory::Auth, "Refresh Token pressed.");
    if (!beginBusy(state, "Ignored refresh request because auth is already running.")) {
        return;
    }
    setStatus(state, "Refreshing saved session");

    try {
        const auto loaded = gfn::loadAuthSession(state->sessionPath);
        if (!loaded.ok) {
            addLog(state, LogLevel::Error, LogCategory::Auth, "Session load failed: " + loaded.error);
            setStatus(state, "No saved session");
            setBusy(state, false);
            return;
        }

        net::CurlHttpClient http;
        auto builder = gfn::AuthRequestBuilder::steamDeck();
        addLog(state, LogLevel::Info, LogCategory::Auth, std::string("Refreshing with auth profile: ") + builder.profileName());
        gfn::AuthService service(http, builder);
        const auto refreshed = service.refreshWithRefreshToken(loaded.session);
        if (!refreshed.ok) {
            addLog(state, LogLevel::Error, LogCategory::Auth, "Refresh failed: " + refreshed.error);
            setStatus(state, "Refresh failed");
            setBusy(state, false);
            return;
        }

        std::string saveError;
        if (!gfn::saveAuthSession(refreshed.session, state->sessionPath, saveError)) {
            addLog(state, LogLevel::Warning, LogCategory::Auth, "Refresh save failed: " + saveError);
            setStatus(state, "Refresh ok, save failed");
        } else {
            addLog(state, LogLevel::Info, LogCategory::Auth, "Session refreshed and saved.");
            setStatus(state, "Session refreshed");
        }
        applySession(state, refreshed.session);
        setBusy(state, false);
    } catch (const std::exception& ex) {
        addLog(state, LogLevel::Error, LogCategory::Auth, std::string("Refresh exception: ") + ex.what());
        setStatus(state, "Refresh exception");
        setBusy(state, false);
    } catch (...) {
        addLog(state, LogLevel::Error, LogCategory::Auth, "Unknown refresh exception.");
        setStatus(state, "Refresh exception");
        setBusy(state, false);
    }
#else
    addLog(state, LogLevel::Error, LogCategory::Auth, "This build has no HTTPS client.");
    setStatus(state, "No HTTPS client");
#endif
}

SafeUiSnapshot copyState(const std::shared_ptr<SafeUiState>& state) {
    std::lock_guard<std::mutex> guard(state->mutex);
    SafeUiSnapshot copy;
    copy.busy = state->busy;
    copy.signedIn = state->signedIn;
    copy.showLogs = state->showLogs;
    copy.importReady = state->importReady;
    copy.importToken = state->importToken;
    copy.status = state->status;
    copy.url = state->url;
    copy.code = state->code;
    copy.expires = state->expires;
    copy.profile = state->profile;
    copy.membership = state->membership;
    copy.sessionPath = state->sessionPath;
    copy.logs = state->logs;
    return copy;
}

void draw(const std::shared_ptr<SafeUiState>& state) {
    const auto s = copyState(state);
    consoleClear();

    std::printf("OpenNOW Switch - Safe Auth UI\n");
    std::printf("Renderer-free debug build for emulator auth testing\n\n");
    std::printf("Status: %s%s\n", s.status.c_str(), s.busy ? " (busy)" : "");
    std::printf("Profile: %s\n", s.profile.c_str());
    std::printf("Membership: %s\n", s.membership.c_str());
    std::printf("Session: %s\n\n", s.sessionPath.c_str());
    std::printf("Import: %s\n", s.importReady ? "http://<switch-ip>:29146/auth-session" : "not listening");
    if (s.importReady) {
        std::printf("Import token: %s\n", s.importToken.c_str());
    }
    std::printf("\n");

    if (!s.showLogs) {
        std::printf("[A/B] Start Device Login   [X] Refresh Token   [Y] Logs   [+] Exit\n\n");
        std::printf("NVIDIA URL:\n%s\n\n", s.url.c_str());
        std::printf("Code: %s\n", s.code.c_str());
        std::printf("Expires: %s\n\n", s.expires.c_str());
        std::printf("Use the NVIDIA URL on a phone/PC, then leave this screen open while it polls.\n");
    } else {
        std::printf("[Y] Account   [A/B] Start Login   [X] Refresh   [+] Exit\n\n");
        const auto first = s.logs.size() > 22 ? s.logs.size() - 22 : 0;
        for (std::size_t i = first; i < s.logs.size(); ++i) {
            const auto& log = s.logs[i];
            std::printf("%s\n", formatLogLine(log.level, log.category, log.message).c_str());
        }
    }

    consoleUpdate(nullptr);
}

} // namespace

int runSwitchSafeApp() {
    consoleInit(nullptr);
#if defined(OPENNOW_SWITCH_CA_BUNDLE)
    const Result romfsRc = romfsInit();
#else
    const Result romfsRc = 0;
#endif

    const Result socketRc = socketInitializeDefault();
    int nxlinkFd = -1;
    if (R_SUCCEEDED(socketRc)) {
        nxlinkFd = nxlinkStdioForDebug();
    }

    auto state = std::make_shared<SafeUiState>();
    state->importToken = gfn::generateOpaqueToken(12);
    addLog(state, LogLevel::Info, LogCategory::App, "Safe auth UI started.");
#if defined(OPENNOW_SWITCH_CA_BUNDLE)
    if (R_SUCCEEDED(romfsRc)) {
        addLog(state, LogLevel::Info, LogCategory::Network, "romfsInit succeeded for TLS CA bundle.");
    } else {
        addLog(state, LogLevel::Error, LogCategory::Network, "romfsInit failed for TLS CA bundle: " + std::to_string(romfsRc));
    }
#endif
    if (R_SUCCEEDED(socketRc)) {
        addLog(state, LogLevel::Info, LogCategory::Network, "socketInitializeDefault succeeded.");
    } else {
        addLog(state, LogLevel::Error, LogCategory::Network, "socketInitializeDefault failed: " + std::to_string(socketRc));
    }
    if (nxlinkFd >= 0) {
        addLog(state, LogLevel::Info, LogCategory::App, "nxlink stderr connected.");
    } else {
        addLog(state, LogLevel::Warning, LogCategory::App, "nxlink stderr not connected.");
    }
    addLog(state, LogLevel::Info, LogCategory::Auth, "Session path: " + state->sessionPath);
    int importServerFd = -1;
    if (R_SUCCEEDED(socketRc)) {
        importServerFd = startImportServer(state);
    }

    const auto loaded = gfn::loadAuthSession(state->sessionPath);
    if (loaded.ok) {
        addLog(state, LogLevel::Info, LogCategory::Auth, "Loaded saved auth session.");
        applySession(state, loaded.session);
        setStatus(state, "Saved session loaded");
    } else {
        addLog(state, LogLevel::Info, LogCategory::Auth, "No saved auth session: " + loaded.error);
    }

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    bool autoStarted = false;

    while (appletMainLoop()) {
        padUpdate(&pad);
        const auto down = padGetButtonsDown(&pad);
        if ((down & HidNpadButton_Plus) != 0) {
            break;
        }
        if ((down & (HidNpadButton_A | HidNpadButton_B)) != 0) {
            startDeviceLogin(state);
        }
        if ((down & HidNpadButton_X) != 0) {
            refreshToken(state);
        }
        if ((down & HidNpadButton_Y) != 0) {
            std::lock_guard<std::mutex> guard(state->mutex);
            state->showLogs = !state->showLogs;
        }

        draw(state);
        handleImportServer(importServerFd, state);
        if (!autoStarted && !isSignedInOrBusy(state)) {
            autoStarted = true;
            addLog(state, LogLevel::Info, LogCategory::Auth, "Auto-starting device login for emulator debug.");
            startDeviceLogin(state);
        }
        pollDeviceLoginIfDue(state);
        svcSleepThread(100000000ULL);
    }

    state->alive = false;
    if (importServerFd >= 0) {
        close(importServerFd);
    }
    consoleExit(nullptr);
    if (nxlinkFd >= 0) {
        close(nxlinkFd);
    }
    if (R_SUCCEEDED(socketRc)) {
        socketExit();
    }
#if defined(OPENNOW_SWITCH_CA_BUNDLE)
    if (R_SUCCEEDED(romfsRc)) {
        romfsExit();
    }
#endif
    return 0;
}

} // namespace opennow::ui
