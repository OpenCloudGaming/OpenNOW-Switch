#include "opennow/app/OpenNowSwitchApp.hpp"

#if defined(OPENNOW_PLATFORM_SWITCH)
#if defined(OPENNOW_USE_CUSTOM_UI)
#include "opennow/ui/CustomUiApp.hpp"
#elif defined(OPENNOW_USE_BOREALIS)
#include "opennow/ui/BorealisApp.hpp"
#else
#include "opennow/ui/SwitchSafeApp.hpp"
#endif
#include "platform/SwitchPlatform.hpp"
#else
#include "opennow/gfn/Auth.hpp"
#include "opennow/gfn/AuthService.hpp"
#include "opennow/gfn/OAuthCallbackServer.hpp"
#include "opennow/net/CurlHttpClient.hpp"
#include "opennow/util/Json.hpp"
#include "platform/MockPlatform.hpp"
#endif

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>

#if !defined(OPENNOW_PLATFORM_SWITCH)
namespace {

std::uint16_t parsePort(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--port") {
            return static_cast<std::uint16_t>(std::stoi(argv[i + 1]));
        }
    }
    return 2259;
}

bool hasArg(int argc, char** argv, const std::string& arg) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == arg) {
            return true;
        }
    }
    return false;
}

std::optional<std::string> envValue(const char* name) {
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) == 0 && value) {
        std::string result(value);
        std::free(value);
        return result;
    }
    return std::nullopt;
#else
    if (const char* value = std::getenv(name)) {
        return std::string(value);
    }
    return std::nullopt;
#endif
}

std::filesystem::path authSessionPath() {
    if (auto appData = envValue("APPDATA")) {
        return std::filesystem::path(*appData) / "OpenNOW-Switch" / "auth-session.json";
    }
    return std::filesystem::current_path() / "auth-session.json";
}

std::string jsonEscape(const std::string& value) {
    std::ostringstream out;
    for (const char c : value) {
        switch (c) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20U) {
                out << "\\u00"
                    << std::hex << std::setw(2) << std::setfill('0')
                    << static_cast<int>(static_cast<unsigned char>(c))
                    << std::dec << std::setfill(' ');
            } else {
                out << c;
            }
            break;
        }
    }
    return out.str();
}

void writeStringField(std::ostream& out, const char* key, const std::string& value, bool comma = true) {
    out << "    \"" << key << "\": \"" << jsonEscape(value) << '"';
    if (comma) {
        out << ',';
    }
    out << '\n';
}

std::string serializeSession(const opennow::AuthSession& session) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"provider\": {\n";
    writeStringField(out, "idpId", session.provider.idpId);
    writeStringField(out, "code", session.provider.code);
    writeStringField(out, "displayName", session.provider.displayName);
    writeStringField(out, "streamingServiceUrl", session.provider.streamingServiceUrl);
    out << "    \"priority\": " << session.provider.priority << "\n";
    out << "  },\n";
    out << "  \"tokens\": {\n";
    writeStringField(out, "accessToken", session.tokens.accessToken);
    writeStringField(out, "refreshToken", session.tokens.refreshToken);
    writeStringField(out, "idToken", session.tokens.idToken);
    writeStringField(out, "clientToken", session.tokens.clientToken);
    out << "    \"expiresAtMs\": " << session.tokens.expiresAtMs << ",\n";
    out << "    \"clientTokenExpiresAtMs\": " << session.tokens.clientTokenExpiresAtMs << "\n";
    out << "  },\n";
    out << "  \"user\": {\n";
    writeStringField(out, "userId", session.user.userId);
    writeStringField(out, "displayName", session.user.displayName);
    writeStringField(out, "email", session.user.email);
    writeStringField(out, "avatarUrl", session.user.avatarUrl);
    writeStringField(out, "membershipTier", session.user.membershipTier, false);
    out << "  }\n";
    out << "}\n";
    return out.str();
}

bool saveSession(const opennow::AuthSession& session, std::filesystem::path path, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        error = "failed to create auth directory: " + ec.message();
        return false;
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "failed to open auth session file for writing";
        return false;
    }

    file << serializeSession(session);
    if (!file) {
        error = "failed to write auth session file";
        return false;
    }
    return true;
}

std::string readTextFile(const std::filesystem::path& path, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "failed to open auth session file";
        return {};
    }
    std::ostringstream out;
    out << file.rdbuf();
    return out.str();
}

std::string objectString(const opennow::util::JsonValue* object, const char* key) {
    if (!object) {
        return {};
    }
    const auto* value = object->get(key);
    return value ? value->asString() : std::string{};
}

std::uint64_t objectU64(const opennow::util::JsonValue* object, const char* key) {
    if (!object) {
        return 0;
    }
    const auto* value = object->get(key);
    return value ? static_cast<std::uint64_t>(value->asNumber()) : 0;
}

std::optional<opennow::AuthSession> loadSession(const std::filesystem::path& path, std::string& error) {
    auto text = readTextFile(path, error);
    if (!error.empty()) {
        return std::nullopt;
    }

    const auto parsed = opennow::util::parseJson(text);
    if (!parsed.ok) {
        error = "failed to parse auth session file: " + parsed.error;
        return std::nullopt;
    }

    const auto* provider = parsed.value.get("provider");
    const auto* tokens = parsed.value.get("tokens");
    const auto* user = parsed.value.get("user");

    opennow::AuthSession session;
    session.provider.idpId = objectString(provider, "idpId");
    session.provider.code = objectString(provider, "code");
    session.provider.displayName = objectString(provider, "displayName");
    session.provider.streamingServiceUrl = objectString(provider, "streamingServiceUrl");
    session.provider.priority = static_cast<std::uint32_t>(objectU64(provider, "priority"));
    session.tokens.accessToken = objectString(tokens, "accessToken");
    session.tokens.refreshToken = objectString(tokens, "refreshToken");
    session.tokens.idToken = objectString(tokens, "idToken");
    session.tokens.clientToken = objectString(tokens, "clientToken");
    session.tokens.expiresAtMs = objectU64(tokens, "expiresAtMs");
    session.tokens.clientTokenExpiresAtMs = objectU64(tokens, "clientTokenExpiresAtMs");
    session.user.userId = objectString(user, "userId");
    session.user.displayName = objectString(user, "displayName");
    session.user.email = objectString(user, "email");
    session.user.avatarUrl = objectString(user, "avatarUrl");
    session.user.membershipTier = objectString(user, "membershipTier");
    if (session.tokens.refreshToken.empty()) {
        error = "auth session file does not contain a refresh token";
        return std::nullopt;
    }
    return session;
}

void printSessionSummary(const opennow::AuthSession& session) {
    std::cout << "User: " << session.user.displayName << " (" << session.user.userId << ")\n"
              << "Tier: " << session.user.membershipTier << '\n'
              << "Refresh token: " << (session.tokens.refreshToken.empty() ? "missing" : "received") << '\n'
              << "Client token: " << (session.tokens.clientToken.empty() ? "missing" : "received") << '\n';
}

int runHostLogin(int argc, char** argv) {
    const auto port = parsePort(argc, argv);
    opennow::gfn::AuthRequestBuilder builder;
    const auto provider = builder.defaultProvider();
    const auto verifier = opennow::gfn::generateCodeVerifier();
    const auto challenge = opennow::gfn::buildCodeChallenge(verifier);
    const auto nonce = opennow::gfn::generateOpaqueToken();
    const auto deviceId = opennow::gfn::generateOpaqueToken(24);

    const auto authUrl = builder.buildAuthUrl({
        .provider = provider,
        .challenge = challenge,
        .redirectPort = port,
        .nonce = nonce,
        .deviceId = deviceId,
    });

    std::cout << "Open this NVIDIA login URL in your browser:\n\n"
              << authUrl << "\n\n"
              << "Waiting for http://localhost:" << port << " callback...\n";

    opennow::gfn::OAuthCallbackServer callbackServer;
    auto callback = callbackServer.waitForCode(port, 300000);
    if (!callback.ok) {
        std::cerr << "Login callback failed: " << callback.error << '\n';
        return 1;
    }

    opennow::net::CurlHttpClient http;
    opennow::gfn::AuthService service(http, builder);
    const auto result = service.exchangeAuthorizationCode(provider, callback.code, verifier, port);
    if (!result.ok) {
        std::cerr << "Login exchange failed: " << result.error << '\n';
        return 1;
    }

    const auto path = authSessionPath();
    std::string saveError;
    if (!saveSession(result.session, path, saveError)) {
        std::cerr << "Login succeeded, but saving tokens failed: " << saveError << '\n';
        return 1;
    }

    std::cout << "Login succeeded.\n";
    printSessionSummary(result.session);
    std::cout << "Saved auth session: " << path.string() << '\n';
    return 0;
}

int runHostRefresh() {
    const auto path = authSessionPath();
    std::string loadError;
    auto session = loadSession(path, loadError);
    if (!session) {
        std::cerr << "Refresh failed: " << loadError << '\n';
        return 1;
    }

    opennow::net::CurlHttpClient http;
    opennow::gfn::AuthService service(http);
    const auto refreshed = service.refreshWithRefreshToken(*session);
    if (!refreshed.ok) {
        std::cerr << "Refresh failed: " << refreshed.error << '\n';
        return 1;
    }

    std::string saveError;
    if (!saveSession(refreshed.session, path, saveError)) {
        std::cerr << "Refresh succeeded, but saving tokens failed: " << saveError << '\n';
        return 1;
    }

    std::cout << "Refresh succeeded.\n";
    printSessionSummary(refreshed.session);
    std::cout << "Saved auth session: " << path.string() << '\n';
    return 0;
}

}
#endif

int main(int argc, char** argv) {
#if !defined(OPENNOW_PLATFORM_SWITCH)
    if (hasArg(argc, argv, "--login")) {
        return runHostLogin(argc, argv);
    }
    if (hasArg(argc, argv, "--refresh")) {
        return runHostRefresh();
    }
#else
    (void)argc;
    (void)argv;
#endif

#if defined(OPENNOW_PLATFORM_SWITCH)
#if defined(OPENNOW_USE_CUSTOM_UI)
    return opennow::ui::runCustomUiApp();
#elif defined(OPENNOW_USE_BOREALIS)
    return opennow::ui::runBorealisApp();
#else
    return opennow::ui::runSwitchSafeApp();
#endif
#else
    auto platform = std::make_unique<opennow::MockPlatform>();
#endif

#if !defined(OPENNOW_PLATFORM_SWITCH)
    opennow::OpenNowSwitchApp app(std::move(platform));
    return app.run();
#endif
}
