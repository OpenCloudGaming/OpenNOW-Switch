#pragma once

#include "models.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace opennow
{

class AppState
{
  public:
    static AppState& Instance();

    bool HasProviders() const;
    bool HasPublicGames() const;
    bool HasLibraryGames() const;
    bool IsSessionLoaded() const;
    bool HasSession() const;

    const std::vector<LoginProvider>& providers() const;
    const std::vector<PublicGame>& public_games() const;
    const std::vector<GameInfo>& library_games() const;
    const std::optional<AuthSession>& session() const;
    std::uint64_t session_generation() const;
    bool IsCurrentSession(std::uint64_t generation) const;

    void SetProviders(std::vector<LoginProvider> providers);
    void SetPublicGames(std::vector<PublicGame> games);
    void SetLibraryGames(std::vector<GameInfo> games);
    void MarkGamePlayed(const std::string& game_id, const std::string& title,
                        const std::string& timestamp);
    void SetSession(AuthSession session);
    void ActivateSession(AuthSession session);
    void ClearSession();
    void MarkSessionLoaded();

  private:
    std::vector<LoginProvider> providers_;
    std::vector<PublicGame> public_games_;
    std::vector<GameInfo> library_games_;
    std::optional<AuthSession> session_;
    bool session_loaded_ = false;
    std::uint64_t session_generation_ = 0;
};

} // namespace opennow
