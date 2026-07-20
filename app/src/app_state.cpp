#include "app_state.hpp"

namespace opennow
{

AppState& AppState::Instance()
{
    static AppState state;
    return state;
}

bool AppState::HasProviders() const
{
    return !providers_.empty();
}

bool AppState::HasPublicGames() const
{
    return !public_games_.empty();
}

bool AppState::HasLibraryGames() const
{
    return !library_games_.empty();
}

bool AppState::IsSessionLoaded() const
{
    return session_loaded_;
}

bool AppState::HasSession() const
{
    return session_.has_value();
}

const std::vector<LoginProvider>& AppState::providers() const
{
    return providers_;
}

const std::vector<PublicGame>& AppState::public_games() const
{
    return public_games_;
}

const std::vector<GameInfo>& AppState::library_games() const
{
    return library_games_;
}

const std::optional<AuthSession>& AppState::session() const
{
    return session_;
}

void AppState::SetProviders(std::vector<LoginProvider> providers)
{
    providers_ = std::move(providers);
}

void AppState::SetPublicGames(std::vector<PublicGame> games)
{
    public_games_ = std::move(games);
}

void AppState::SetLibraryGames(std::vector<GameInfo> games)
{
    library_games_ = std::move(games);
}

void AppState::MarkGamePlayed(const std::string& game_id, const std::string& title,
                              const std::string& timestamp)
{
    for (GameInfo& game : library_games_)
    {
        if ((!game_id.empty() && (game.id == game_id || game.uuid == game_id)) ||
            (!title.empty() && game.title == title))
            game.last_played = timestamp;
    }
}

void AppState::SetSession(AuthSession session)
{
    session_        = std::move(session);
    session_loaded_ = true;
}

void AppState::ClearSession()
{
    session_.reset();
    session_loaded_ = true;
}

void AppState::MarkSessionLoaded()
{
    session_loaded_ = true;
}

} // namespace opennow
