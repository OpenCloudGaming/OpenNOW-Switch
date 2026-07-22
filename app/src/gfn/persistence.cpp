#include "persistence_internal.hpp"

#include <mbedtls/gcm.h>
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace opennow
{
using namespace gfn::detail;

namespace
{
std::string GetSessionPath()
{
    return GetAppHome() + "/auth_session.json";
}

std::string GetAccountsPath()
{
    return GetAppHome() + "/auth_accounts.json";
}

std::string GetLauncherPreferencesPath()
{
    return GetAppHome() + "/launcher_preferences.json";
}

std::string GetNativeCredentialsPath()
{
    return GetAppHome() + "/auth_credentials.vault";
}
std::string AuthSessionHealth(const AuthSession& session)
{
    const std::int64_t remaining_ms = session.tokens.expires_at_ms - NowMs();
    const std::int64_t remaining_minutes = remaining_ms > 0 ? remaining_ms / 60000 : 0;
    return "user=" + std::string(session.user.user_id.empty() ? "missing" : "present") +
           " accessMin=" + std::to_string(remaining_minutes) +
           " refresh=" + std::string(session.tokens.refresh_token.empty() ? "missing" : "present") +
           " client=" + std::string(session.tokens.client_token.empty() ? "missing" : "present") +
           " persistent=" + std::to_string(session.persistence_enabled ? 1 : 0);
}
constexpr const char* kTokenVaultHeader = "OPENNOW_TOKEN_VAULT_V1";
constexpr const char* kTokenVaultAad = "OpenNOW Switch token vault v1";

std::vector<unsigned char> HexDecode(const std::string& input)
{
    if ((input.size() & 1U) != 0)
        throw std::runtime_error("Invalid vault hex length");

    auto value = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };

    std::vector<unsigned char> output(input.size() / 2);
    for (size_t i = 0; i < output.size(); ++i)
    {
        const int high = value(input[i * 2]);
        const int low = value(input[i * 2 + 1]);
        if (high < 0 || low < 0)
            throw std::runtime_error("Invalid vault hex data");
        output[i] = static_cast<unsigned char>((high << 4) | low);
    }
    return output;
}

std::array<unsigned char, 32> TokenVaultKey()
{
    const std::string material =
        "OpenNOW-Switch-vault-key-v1|" + GenerateDeviceId() + "|05004F4E4F575358";
    std::array<unsigned char, 32> key {};
    int rc = 0;
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
    rc = mbedtls_sha256(
        reinterpret_cast<const unsigned char*>(material.data()),
        material.size(), key.data(), 0);
#else
    rc = mbedtls_sha256_ret(
        reinterpret_cast<const unsigned char*>(material.data()),
        material.size(), key.data(), 0);
#endif
    if (rc != 0)
    {
        throw std::runtime_error("Unable to derive token vault key");
    }
    return key;
}

std::string EncryptTokenVault(const std::string& plaintext)
{
    const auto key = TokenVaultKey();
    const auto nonce = GenerateRandomBytes(12);
    std::array<unsigned char, 16> tag {};
    std::vector<unsigned char> ciphertext(plaintext.size());

    mbedtls_gcm_context context;
    mbedtls_gcm_init(&context);
    int rc = mbedtls_gcm_setkey(&context, MBEDTLS_CIPHER_ID_AES, key.data(), 256);
    if (rc == 0)
    {
        rc = mbedtls_gcm_crypt_and_tag(
            &context, MBEDTLS_GCM_ENCRYPT, plaintext.size(), nonce.data(), nonce.size(),
            reinterpret_cast<const unsigned char*>(kTokenVaultAad), std::strlen(kTokenVaultAad),
            reinterpret_cast<const unsigned char*>(plaintext.data()), ciphertext.data(),
            tag.size(), tag.data());
    }
    mbedtls_gcm_free(&context);
    if (rc != 0)
        throw std::runtime_error("Unable to encrypt token vault");

    return std::string(kTokenVaultHeader) + "\n" +
           HexEncode(nonce.data(), nonce.size()) + "\n" +
           HexEncode(tag.data(), tag.size()) + "\n" +
           HexEncode(ciphertext.data(), ciphertext.size()) + "\n";
}

std::string DecryptTokenVault(const std::string& encoded)
{
    std::istringstream input(encoded);
    std::string header;
    std::string nonce_hex;
    std::string tag_hex;
    std::string ciphertext_hex;
    std::getline(input, header);
    std::getline(input, nonce_hex);
    std::getline(input, tag_hex);
    std::getline(input, ciphertext_hex);
    if (header != kTokenVaultHeader)
        return encoded;

    const auto nonce = HexDecode(nonce_hex);
    const auto tag = HexDecode(tag_hex);
    const auto ciphertext = HexDecode(ciphertext_hex);
    if (nonce.size() != 12 || tag.size() != 16 || ciphertext.empty())
        throw std::runtime_error("Invalid token vault structure");

    const auto key = TokenVaultKey();
    std::string plaintext(ciphertext.size(), '\0');
    mbedtls_gcm_context context;
    mbedtls_gcm_init(&context);
    int rc = mbedtls_gcm_setkey(&context, MBEDTLS_CIPHER_ID_AES, key.data(), 256);
    if (rc == 0)
    {
        rc = mbedtls_gcm_auth_decrypt(
            &context, ciphertext.size(), nonce.data(), nonce.size(),
            reinterpret_cast<const unsigned char*>(kTokenVaultAad), std::strlen(kTokenVaultAad),
            tag.data(), tag.size(), ciphertext.data(),
            reinterpret_cast<unsigned char*>(plaintext.data()));
    }
    mbedtls_gcm_free(&context);
    if (rc != 0)
        throw std::runtime_error("Token vault authentication failed");
    return plaintext;
}
bool ParseSessionObject(json_t* root, AuthSession& session)
{
    if (!root || !json_is_object(root))
        return false;

    session.provider.idp_id                = GetString(json_object_get(root, "provider"), "idp_id");
    session.provider.code                  = GetString(json_object_get(root, "provider"), "code");
    session.provider.display_name          = GetString(json_object_get(root, "provider"), "display_name");
    session.provider.streaming_service_url =
        EnsureTrailingSlash(GetString(json_object_get(root, "provider"), "streaming_service_url"));
    session.provider.priority = GetInteger(json_object_get(root, "provider"), "priority", 0);

    json_t* tokens = json_object_get(root, "tokens");
    session.tokens.access_token  = GetString(tokens, "access_token");
    session.tokens.refresh_token = GetString(tokens, "refresh_token");
    session.tokens.id_token      = GetString(tokens, "id_token");
    session.tokens.client_token  = GetString(tokens, "client_token");

    json_t* expires = tokens ? json_object_get(tokens, "expires_at_ms") : nullptr;
    if (json_is_integer(expires))
        session.tokens.expires_at_ms = static_cast<std::int64_t>(json_integer_value(expires));

    json_t* client_expires = tokens ? json_object_get(tokens, "client_token_expires_at_ms") : nullptr;
    if (json_is_integer(client_expires))
    {
        session.tokens.client_token_expires_at_ms =
            static_cast<std::int64_t>(json_integer_value(client_expires));
    }

    json_t* user = json_object_get(root, "user");
    session.user.user_id         = GetString(user, "user_id");
    session.user.display_name    = GetString(user, "display_name");
    session.user.email           = GetString(user, "email");
    session.user.avatar_url      = GetString(user, "avatar_url");
    session.user.membership_tier = GetString(user, "membership_tier");
    if (session.user.membership_tier.empty())
        session.user.membership_tier = "FREE";

    json_t* last_refresh = json_object_get(root, "last_refresh_at_ms");
    if (json_is_integer(last_refresh))
        session.last_refresh_at_ms = static_cast<std::int64_t>(json_integer_value(last_refresh));

    if (session.provider.idp_id.empty())
        session.provider = DefaultProvider();

    return !session.tokens.access_token.empty() && !session.user.user_id.empty();
}

JsonPtr BuildSessionObject(const AuthSession& session)
{
    JsonPtr root(json_object(), &json_decref);
    JsonPtr provider(json_object(), &json_decref);
    JsonPtr tokens(json_object(), &json_decref);
    JsonPtr user(json_object(), &json_decref);

    json_object_set_new(provider.get(), "idp_id", json_string(session.provider.idp_id.c_str()));
    json_object_set_new(provider.get(), "code", json_string(session.provider.code.c_str()));
    json_object_set_new(provider.get(), "display_name", json_string(session.provider.display_name.c_str()));
    json_object_set_new(
        provider.get(),
        "streaming_service_url",
        json_string(session.provider.streaming_service_url.c_str()));
    json_object_set_new(provider.get(), "priority", json_integer(session.provider.priority));

    json_object_set_new(tokens.get(), "access_token", json_string(session.tokens.access_token.c_str()));
    json_object_set_new(tokens.get(), "refresh_token", json_string(session.tokens.refresh_token.c_str()));
    json_object_set_new(tokens.get(), "id_token", json_string(session.tokens.id_token.c_str()));
    json_object_set_new(tokens.get(), "client_token", json_string(session.tokens.client_token.c_str()));
    json_object_set_new(tokens.get(), "expires_at_ms", json_integer(session.tokens.expires_at_ms));
    json_object_set_new(
        tokens.get(),
        "client_token_expires_at_ms",
        json_integer(session.tokens.client_token_expires_at_ms));

    json_object_set_new(user.get(), "user_id", json_string(session.user.user_id.c_str()));
    json_object_set_new(user.get(), "display_name", json_string(session.user.display_name.c_str()));
    json_object_set_new(user.get(), "email", json_string(session.user.email.c_str()));
    json_object_set_new(user.get(), "avatar_url", json_string(session.user.avatar_url.c_str()));
    json_object_set_new(user.get(), "membership_tier", json_string(session.user.membership_tier.c_str()));

    json_object_set_new(root.get(), "provider", json_incref(provider.get()));
    json_object_set_new(root.get(), "tokens", json_incref(tokens.get()));
    json_object_set_new(root.get(), "user", json_incref(user.get()));
    json_object_set_new(root.get(), "last_refresh_at_ms", json_integer(session.last_refresh_at_ms));
    return root;
}
std::vector<NativeCredentials> LoadNativeCredentialEntries()
{
    const std::string stored = ReadTextFile(GetNativeCredentialsPath());
    if (stored.empty())
        return {};
    try
    {
        JsonPtr root = LoadJson(DecryptTokenVault(stored));
        json_t* entries = json_object_get(root.get(), "entries");
        if (!json_is_array(entries))
            return {};
        std::vector<NativeCredentials> result;
        size_t index = 0;
        json_t* item = nullptr;
        json_array_foreach(entries, index, item)
        {
            NativeCredentials credentials;
            credentials.provider_id = GetString(item, "provider_id");
            credentials.email = GetString(item, "email");
            credentials.password = GetString(item, "password");
            if (!credentials.provider_id.empty() && !credentials.email.empty() &&
                !credentials.password.empty())
            {
                result.push_back(std::move(credentials));
            }
        }
        return result;
    }
    catch (const std::exception& e)
    {
        AppendAuthLog(std::string("auth-native: credential vault load failed error=") + e.what());
        return {};
    }
}

void SaveNativeCredentialEntries(const std::vector<NativeCredentials>& entries)
{
    EnsureAppHome();
    if (entries.empty())
    {
        std::remove(GetNativeCredentialsPath().c_str());
        std::remove((GetNativeCredentialsPath() + ".bak").c_str());
        return;
    }

    JsonPtr root(json_object(), &json_decref);
    JsonPtr values(json_array(), &json_decref);
    for (const NativeCredentials& credentials : entries)
    {
        JsonPtr item(json_object(), &json_decref);
        json_object_set_new(item.get(), "provider_id", json_string(credentials.provider_id.c_str()));
        json_object_set_new(item.get(), "email", json_string(credentials.email.c_str()));
        json_object_set_new(item.get(), "password", json_string(credentials.password.c_str()));
        json_array_append_new(values.get(), json_incref(item.get()));
    }
    json_object_set_new(root.get(), "schema_version", json_integer(1));
    json_object_set_new(root.get(), "entries", json_incref(values.get()));
    char* dump = json_dumps(root.get(), JSON_COMPACT | JSON_SORT_KEYS);
    if (!dump)
        throw std::runtime_error("Failed to serialize the credential vault");
    std::unique_ptr<char, decltype(&std::free)> plaintext(dump, &std::free);
    const std::string encrypted = EncryptTokenVault(plaintext.get());
    if (DecryptTokenVault(encrypted) != plaintext.get())
        throw std::runtime_error("Credential vault round-trip verification failed");
    WriteTextFileAtomically(GetNativeCredentialsPath(), encrypted);
    AppendAuthLog("auth-native: credential vault saved entries=" + std::to_string(entries.size()));
}

bool LoadLegacySession(AuthSession& session)
{
    for (const std::string& path : {GetSessionPath(), GetSessionPath() + ".bak"})
    {
        const std::string body = ReadTextFile(path);
        if (body.empty())
            continue;
        try
        {
            JsonPtr root = LoadJson(body);
            if (ParseSessionObject(root.get(), session))
                return true;
        }
        catch (...) {}
    }

    return false;
}


} // namespace

namespace gfn::detail
{

std::recursive_mutex& AccountsMutex()
{
    static std::recursive_mutex mutex;
    return mutex;
}

std::vector<AuthSession> LoadAccountsFromDisk(std::string* active_user_id)
{
    std::lock_guard<std::recursive_mutex> lock(AccountsMutex());
    for (const std::string& path : {GetAccountsPath(), GetAccountsPath() + ".bak"})
    {
        const std::string stored = ReadTextFile(path);
        if (stored.empty())
            continue;

        try
        {
            const std::string body = DecryptTokenVault(stored);
            JsonPtr root = LoadJson(body);
            if (!json_is_object(root.get()))
                continue;

            json_t* accounts = json_object_get(root.get(), "accounts");
            if (!accounts || !json_is_array(accounts))
                continue;

            std::vector<AuthSession> sessions;
            size_t index = 0;
            json_t* item = nullptr;
            json_array_foreach(accounts, index, item)
            {
                AuthSession session;
                if (ParseSessionObject(item, session))
                    sessions.push_back(std::move(session));
            }

            if (sessions.empty())
                continue;

            if (active_user_id)
                *active_user_id = GetString(root.get(), "active_user_id");
            if (path.find(".bak") != std::string::npos)
                AppendAuthLog("auth: recovered accounts from backup");
            else if (stored.rfind(kTokenVaultHeader, 0) != 0)
            {
                AppendAuthLog("auth: loaded legacy plaintext account store; migration scheduled");
                std::string active = GetString(root.get(), "active_user_id");
                if (active.empty())
                    active = sessions.front().user.user_id;
                SaveAccountsToDisk(sessions, active);
                AppendAuthLog("auth: legacy account store migrated to encrypted vault");
            }
            return sessions;
        }
        catch (const std::exception& e)
        {
            AppendAuthLog("auth: account store parse failed path=" + path + " error=" + e.what());
        }
    }

    return {};
}

void SaveAccountsToDisk(const std::vector<AuthSession>& sessions, const std::string& active_user_id)
{
    std::lock_guard<std::recursive_mutex> lock(AccountsMutex());
    EnsureAppHome();

    JsonPtr root(json_object(), &json_decref);
    JsonPtr accounts(json_array(), &json_decref);

    for (const AuthSession& session : sessions)
    {
        JsonPtr item = BuildSessionObject(session);
        json_array_append_new(accounts.get(), json_incref(item.get()));
    }

    json_object_set_new(root.get(), "schema_version", json_integer(3));
    json_object_set_new(root.get(), "active_user_id", json_string(active_user_id.c_str()));
    json_object_set_new(root.get(), "accounts", json_incref(accounts.get()));
    char* dump = json_dumps(root.get(), JSON_COMPACT | JSON_SORT_KEYS);
    if (!dump)
        throw std::runtime_error("Failed to serialize token vault");
    std::unique_ptr<char, decltype(&std::free)> plaintext(dump, &std::free);
    const std::string encrypted = EncryptTokenVault(plaintext.get());
    if (DecryptTokenVault(encrypted) != plaintext.get())
        throw std::runtime_error("Token vault round-trip verification failed");
    WriteTextFileAtomically(GetAccountsPath(), encrypted);

    const std::string backup_path = GetAccountsPath() + ".bak";
    const std::string backup = ReadTextFile(backup_path);
    if (!backup.empty() && backup.rfind(kTokenVaultHeader, 0) != 0)
        std::remove(backup_path.c_str());

    // The encrypted multi-account vault replaces the old plaintext session copy.
    std::remove(GetSessionPath().c_str());
    std::remove((GetSessionPath() + ".bak").c_str());
    AppendAuthLog("auth: token vault saved accounts=" + std::to_string(sessions.size()) +
                  " encrypted=1 roundTrip=ok");
}

} // namespace gfn::detail

bool GfnClient::LoadSavedSession(AuthSession& session) const
{
    std::lock_guard<std::recursive_mutex> lock(AccountsMutex());
    std::string active_user_id;
    std::vector<AuthSession> sessions = LoadAccountsFromDisk(&active_user_id);
    if (!sessions.empty())
    {
        for (const AuthSession& saved : sessions)
        {
            if (saved.user.user_id == active_user_id)
            {
                session = saved;
                AppendAuthLog("auth: selected saved account " + AuthSessionHealth(session));
                return true;
            }
        }

        session = sessions.front();
        SaveAccountsToDisk(sessions, session.user.user_id);
        AppendAuthLog("auth: selected fallback account " + AuthSessionHealth(session));
        return true;
    }

    if (LoadLegacySession(session))
    {
        SaveSession(session);
        AppendAuthLog("auth: selected migrated legacy account " + AuthSessionHealth(session));
        return true;
    }

    return false;
}

std::vector<AuthSession> GfnClient::LoadSavedSessions() const
{
    std::lock_guard<std::recursive_mutex> lock(AccountsMutex());
    std::vector<AuthSession> sessions = LoadAccountsFromDisk();
    if (!sessions.empty())
        return sessions;

    AuthSession legacy;
    if (LoadLegacySession(legacy))
    {
        SaveSession(legacy);
        sessions.push_back(std::move(legacy));
    }

    return sessions;
}

void GfnClient::SaveSession(const AuthSession& session) const
{
    if (!session.persistence_enabled)
        return;

    std::lock_guard<std::recursive_mutex> lock(AccountsMutex());
    EnsureAppHome();

    std::vector<AuthSession> sessions = LoadAccountsFromDisk();
    bool replaced = false;
    for (AuthSession& saved : sessions)
    {
        if (saved.user.user_id == session.user.user_id)
        {
            saved = session;
            replaced = true;
            break;
        }
    }

    if (!replaced)
        sessions.push_back(session);

    SaveAccountsToDisk(sessions, session.user.user_id);
}

bool GfnClient::SetActiveSavedSession(const std::string& user_id) const
{
    std::lock_guard<std::recursive_mutex> lock(AccountsMutex());
    std::vector<AuthSession> sessions = LoadAccountsFromDisk();
    for (const AuthSession& session : sessions)
    {
        if (session.user.user_id == user_id)
        {
            SaveAccountsToDisk(sessions, session.user.user_id);
            return true;
        }
    }

    return false;
}

void GfnClient::ClearSavedSession() const
{
    std::lock_guard<std::recursive_mutex> lock(AccountsMutex());
    AuthSession active;
    if (!LoadSavedSession(active))
    {
        std::remove(GetSessionPath().c_str());
        std::remove((GetSessionPath() + ".bak").c_str());
        return;
    }

    std::vector<AuthSession> sessions = LoadAccountsFromDisk();
    sessions.erase(
        std::remove_if(
            sessions.begin(),
            sessions.end(),
            [&active](const AuthSession& session) {
                return session.user.user_id == active.user.user_id;
            }),
        sessions.end());

    if (sessions.empty())
    {
        std::remove(GetAccountsPath().c_str());
        std::remove((GetAccountsPath() + ".bak").c_str());
        std::remove(GetSessionPath().c_str());
        std::remove((GetSessionPath() + ".bak").c_str());
        return;
    }

    SaveAccountsToDisk(sessions, sessions.front().user.user_id);
}

void GfnClient::ClearAllSavedSessions() const
{
    std::lock_guard<std::recursive_mutex> lock(AccountsMutex());
    std::remove(GetAccountsPath().c_str());
    std::remove((GetAccountsPath() + ".bak").c_str());
    std::remove(GetSessionPath().c_str());
    std::remove((GetSessionPath() + ".bak").c_str());
}

std::optional<NativeCredentials> GfnClient::LoadNativeCredentials(
    const std::string& provider_id) const
{
    std::lock_guard<std::recursive_mutex> lock(AccountsMutex());
    for (NativeCredentials& credentials : LoadNativeCredentialEntries())
    {
        if (credentials.provider_id == provider_id)
            return credentials;
    }
    return std::nullopt;
}

void GfnClient::SaveNativeCredentials(const NativeCredentials& credentials) const
{
    if (credentials.provider_id.empty() || credentials.email.empty() || credentials.password.empty())
        throw std::runtime_error("Cannot save incomplete NVIDIA credentials");
    std::lock_guard<std::recursive_mutex> lock(AccountsMutex());
    std::vector<NativeCredentials> entries = LoadNativeCredentialEntries();
    bool replaced = false;
    for (NativeCredentials& saved : entries)
    {
        if (saved.provider_id == credentials.provider_id)
        {
            saved = credentials;
            replaced = true;
            break;
        }
    }
    if (!replaced)
        entries.push_back(credentials);
    SaveNativeCredentialEntries(entries);
}

void GfnClient::ClearNativeCredentials(const std::string& provider_id) const
{
    std::lock_guard<std::recursive_mutex> lock(AccountsMutex());
    std::vector<NativeCredentials> entries = LoadNativeCredentialEntries();
    entries.erase(
        std::remove_if(entries.begin(), entries.end(), [&](const NativeCredentials& entry) {
            return entry.provider_id == provider_id;
        }),
        entries.end());
    SaveNativeCredentialEntries(entries);
    AppendAuthLog("auth-native: saved password removed");
}

void GfnClient::ClearAllNativeCredentials() const
{
    std::lock_guard<std::recursive_mutex> lock(AccountsMutex());
    SaveNativeCredentialEntries({});
    AppendAuthLog("auth-native: all saved passwords removed");
}

std::string GfnClient::LoadLauncherPreference(
    const std::string& user_id, const std::string& game_id) const
{
    if (user_id.empty() || game_id.empty())
        return "";
    std::lock_guard<std::recursive_mutex> lock(AccountsMutex());
    try {
        const std::string stored = ReadTextFile(GetLauncherPreferencesPath());
        if (stored.empty())
            return "";
        JsonPtr root = LoadJson(stored);
        return GetString(root.get(), (user_id + ":" + game_id).c_str());
    } catch (const std::exception& e) {
        AppendAuthLog("launcher: preference load failed error=" + std::string(e.what()));
        return "";
    }
}

void GfnClient::SaveLauncherPreference(
    const std::string& user_id, const std::string& game_id,
    const std::string& variant_id) const
{
    if (user_id.empty() || game_id.empty() || variant_id.empty())
        return;
    std::lock_guard<std::recursive_mutex> lock(AccountsMutex());
    try {
        JsonPtr root(json_object(), &json_decref);
        const std::string stored = ReadTextFile(GetLauncherPreferencesPath());
        if (!stored.empty()) {
            try {
                JsonPtr loaded = LoadJson(stored);
                if (json_is_object(loaded.get()))
                    root = std::move(loaded);
            } catch (...) {
            }
        }
        json_object_set_new(root.get(), (user_id + ":" + game_id).c_str(),
                            json_string(variant_id.c_str()));
        WriteJsonToFile(GetLauncherPreferencesPath(), root.get());
        AppendAuthLog("launcher: preference saved game=" + game_id + " variant=" + variant_id);
    } catch (const std::exception& e) {
        AppendAuthLog("launcher: preference save failed error=" + std::string(e.what()));
    }
}

[[maybe_unused]] static std::string GenerateUUID()
{
    auto bytes = GenerateRandomBytes(16);
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    std::string hex = HexEncode(bytes.data(), 16);
    return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" + hex.substr(16, 4) + "-" + hex.substr(20);
}

} // namespace opennow
