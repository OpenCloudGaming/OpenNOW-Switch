#pragma once

#include "gfn_client.hpp"
#include "models.hpp"

#include <borealis.hpp>

#include <string>
#include <vector>

namespace opennow
{

struct GameDetailData
{
    std::string title;
    std::string game_id;
    std::string subtitle;
    std::string image_url;
    std::string launch_app_id;
    std::string publisher;
    std::string description;
    std::string stores;
    std::string membership_tier_label;
    std::string last_played;
    bool owned = false;
    std::vector<GameVariant> variants;
};

class GameDetailView : public brls::Box
{
  public:
    GameDetailView(const GfnClient& client, GameDetailData data);

  private:
    void Play();
    void ShowStoreSelector(bool launch_after_selection);
    void LaunchSelectedVariant();
    void UpdateStoreButton();
    void OpenNteCredentialsMenu();
    void ConfigureNteCredentials();
    void UpdateNteButton();
    std::string ActiveUserId() const;

    GfnClient client_;
    GameDetailData data_;
    brls::Button* store_button_ = nullptr;
    brls::Button* nte_button_ = nullptr;
    size_t selected_variant_index_ = 0;
    bool launcher_preference_loaded_ = false;
};

GameDetailData MakeLibraryGameDetail(const GameInfo& game);
GameDetailData MakeCatalogGameDetail(const PublicGame& game);

} // namespace opennow
