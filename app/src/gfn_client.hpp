#pragma once

#include "http_client.hpp"
#include "models.hpp"

#include <functional>
#include <stdexcept>
#include <vector>

namespace opennow
{

class ReauthenticationRequired : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

class GfnClient
{
  public:
    GfnClient();

    std::vector<LoginProvider> FetchLoginProviders() const;
    std::vector<PublicGame> FetchPublicGames() const;
    std::vector<PublicGame> FetchCatalogGames(
        AuthSession& session, const std::string& search_query = {}) const;
    std::vector<GameInfo> FetchLibraryGames(AuthSession& session) const;
    std::string ProvisionCommunityProxy() const;

    AuthSession LoginWithQrCode(
        const LoginProvider& provider,
        const std::function<void(const QrLoginChallenge&)>& on_challenge,
        const std::function<bool()>& is_cancelled) const;
    AuthSession EnsureFreshSession(const AuthSession& session) const;
    AuthSession EnsureFreshSavedSession(const AuthSession& session) const;
    AuthSession ForceRefreshSavedSession(const AuthSession& session) const;
    AuthSession RecoverSavedSession(
        const AuthSession& session,
        bool force_refresh = false) const;

    SessionInfo StartSession(AuthSession& session, const std::string& launch_app_id,
                             const std::string& launch_store = "",
                             const std::string& internal_title = "") const;
    SessionInfo PollSession(AuthSession& session, const std::string& session_id) const;
    void StopSession(AuthSession& session, const std::string& session_id) const;
    void CleanupStaleCloudSession(AuthSession& session) const;

    bool LoadSavedSession(AuthSession& session) const;
    std::vector<AuthSession> LoadSavedSessions() const;
    void SaveSession(const AuthSession& session) const;
    bool SetActiveSavedSession(const std::string& user_id) const;
    void ClearSavedSession() const;
    void ClearAllSavedSessions() const;
    void ClearAllNativeCredentials() const;
    std::string LoadLauncherPreference(const std::string& user_id, const std::string& game_id) const;
    void SaveLauncherPreference(const std::string& user_id, const std::string& game_id,
                                const std::string& variant_id) const;

    static constexpr const char* kUserAgent =
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36 "
        "NVIDIACEFClient/HEAD/debb5919f6 GFN-PC/2.0.80.173";

  private:
    HttpClient http_client_;
    std::string client_id_;
};

} // namespace opennow
