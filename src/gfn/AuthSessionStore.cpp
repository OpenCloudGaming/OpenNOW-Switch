#include "opennow/gfn/AuthSessionStore.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <cerrno>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include "opennow/util/Json.hpp"

namespace opennow::gfn {
namespace {

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
                out << "\\u00";
                constexpr char hex[] = "0123456789abcdef";
                out << hex[(static_cast<unsigned char>(c) >> 4U) & 0xfU]
                    << hex[static_cast<unsigned char>(c) & 0xfU];
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

std::string serializeSession(const AuthSession& session) {
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

bool mkdirOne(const std::string& path) {
    if (path.empty()) {
        return true;
    }
#if defined(_WIN32)
    return _mkdir(path.c_str()) == 0 || errno == EEXIST;
#else
    return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
#endif
}

std::string parentPath(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    if (slash == std::string::npos) {
        return {};
    }
    return path.substr(0, slash);
}

bool ensureDirectories(const std::string& directory, std::string& error) {
    if (directory.empty()) {
        return true;
    }

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
            if (!mkdirOne(current)) {
                error = "failed to create directory: " + current;
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

std::string objectString(const util::JsonValue* object, const char* key, const std::string& fallback = {}) {
    if (!object) {
        return fallback;
    }
    const auto* value = object->get(key);
    return value ? value->asString(fallback) : fallback;
}

std::uint64_t objectU64(const util::JsonValue* object, const char* key) {
    if (!object) {
        return 0;
    }
    const auto* value = object->get(key);
    return value ? static_cast<std::uint64_t>(value->asNumber()) : 0;
}

} // namespace

std::string defaultAuthSessionPath() {
#if defined(OPENNOW_PLATFORM_SWITCH)
    return "sdmc:/switch/OpenNOW/auth-session.json";
#elif defined(_WIN32)
    return "auth-session.json";
#else
    return "auth-session.json";
#endif
}

bool saveAuthSession(const AuthSession& session, const std::string& path, std::string& error) {
    if (!ensureDirectories(parentPath(path), error)) {
        return false;
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "failed to open auth session for writing: " + path;
        return false;
    }

    file << serializeSession(session);
    if (!file) {
        error = "failed to write auth session: " + path;
        return false;
    }
    return true;
}

StoredAuthSession loadAuthSession(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {false, {}, "no saved auth session"};
    }

    std::ostringstream body;
    body << file.rdbuf();
    const auto parsed = util::parseJson(body.str());
    if (!parsed.ok) {
        return {false, {}, "failed to parse saved auth session: " + parsed.error};
    }

    const auto* provider = parsed.value.get("provider");
    const auto* tokens = parsed.value.get("tokens");
    const auto* user = parsed.value.get("user");

    AuthSession session;
    session.provider.idpId = objectString(provider, "idpId");
    session.provider.code = objectString(provider, "code", "NVIDIA");
    session.provider.displayName = objectString(provider, "displayName", "NVIDIA");
    session.provider.streamingServiceUrl = objectString(provider, "streamingServiceUrl", "https://prod.cloudmatchbeta.nvidiagrid.net/");
    session.provider.priority = static_cast<std::uint32_t>(objectU64(provider, "priority"));
    session.tokens.accessToken = objectString(tokens, "accessToken");
    if (session.tokens.accessToken.empty()) session.tokens.accessToken = objectString(tokens, "access_token");
    session.tokens.refreshToken = objectString(tokens, "refreshToken");
    if (session.tokens.refreshToken.empty()) session.tokens.refreshToken = objectString(tokens, "refresh_token");
    session.tokens.idToken = objectString(tokens, "idToken");
    if (session.tokens.idToken.empty()) session.tokens.idToken = objectString(tokens, "id_token");
    session.tokens.clientToken = objectString(tokens, "clientToken");
    if (session.tokens.clientToken.empty()) session.tokens.clientToken = objectString(tokens, "client_token");
    session.tokens.expiresAtMs = objectU64(tokens, "expiresAtMs");
    session.tokens.clientTokenExpiresAtMs = objectU64(tokens, "clientTokenExpiresAtMs");
    session.user.userId = objectString(user, "userId");
    session.user.displayName = objectString(user, "displayName", "User");
    session.user.email = objectString(user, "email");
    session.user.avatarUrl = objectString(user, "avatarUrl");
    session.user.membershipTier = objectString(user, "membershipTier", "FREE");

    if (session.tokens.refreshToken.empty() && session.tokens.accessToken.empty()) {
        return {false, {}, "saved session does not contain an access token or refresh token"};
    }
    return {true, session, {}};
}

} // namespace opennow::gfn
