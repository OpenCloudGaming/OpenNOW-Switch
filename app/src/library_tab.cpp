#include "library_tab.hpp"

#include "app_state.hpp"
#include "game_browser_header.hpp"
#include "game_detail_view.hpp"
#include "game_card_view.hpp"
#include "game_grid_navigation.hpp"
#include "library_sort.hpp"
#include "membership_label.hpp"
#include "ui_action_guard.hpp"
#include "ui_helpers.hpp"
#include "localization.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <stdexcept>
#include <utility>

namespace opennow
{
namespace
{

constexpr size_t kCardsPerRow       = 5;
constexpr size_t kInitialVisibleLibraryLimit = 15;
constexpr std::array<const char*, 6> kStoreFilters = {"All", "Steam", "Epic", "Ubisoft", "Xbox", "Battle.net"};
constexpr std::array<const char*, 4> kLibrarySortModes = {
    "Last Played", "Last Added", "A-Z", "Store"};

brls::Label* MakeParagraph(const std::string& text, float bottom_margin = 16.0f, float font_size = 18.0f)
{
    auto* label = new brls::Label();
    label->setText(Tr(text));
    label->setFontSize(font_size);
    label->setMarginBottom(bottom_margin);
    return label;
}

std::string ToLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool ContainsText(const std::string& haystack, const std::string& needle)
{
    return ToLower(haystack).find(ToLower(needle)) != std::string::npos;
}

std::string PrimaryStore(const GameInfo& game)
{
    if (!game.available_stores.empty())
        return game.available_stores.front();

    if (!game.publisher.empty())
        return game.publisher;

    return "GeForce NOW";
}

bool MatchesStoreFilter(const GameInfo& game, size_t filter_index)
{
    if (filter_index == 0 || filter_index >= kStoreFilters.size())
        return true;

    const std::string filter = kStoreFilters[filter_index];
    if (ContainsText(PrimaryStore(game), filter) || ContainsText(game.publisher, filter))
        return true;

    for (const std::string& store : game.available_stores)
    {
        if (ContainsText(store, filter))
            return true;
    }

    if (filter == "Battle.net")
        return ContainsText(PrimaryStore(game), "battle") || ContainsText(game.publisher, "blizzard");

    return false;
}

} // namespace

LibraryTab::LibraryTab()
    : brls::Box(brls::Axis::COLUMN)
{
    setPadding(18, 32, 18, 32);
    setBackgroundColor(nvgRGB(16, 16, 20));

    search_button_ = ui::MakeGameBrowserActionButton("Y  Search");
    filter_button_ = ui::MakeGameBrowserActionButton("ZL  All stores");
    sort_button_   = ui::MakeGameBrowserActionButton("ZR  Last Played");
    more_button_   = ui::MakeGameBrowserActionButton("X  More / Refresh");
    search_button_->setStyle(&brls::BUTTONSTYLE_HIGHLIGHT);
    toolbar_buttons_ = {search_button_, filter_button_, sort_button_, more_button_};
    addView(ui::MakeGameBrowserHeader("My Library", toolbar_buttons_));

    search_button_->registerClickAction([this](brls::View*) {
        return RunUiAction("library.search.button", [this]() { BeginSearch(); });
    });
    filter_button_->registerClickAction([this](brls::View*) {
        return RunUiAction("library.filter.button", [this]() { CycleStoreFilter(); });
    });
    sort_button_->registerClickAction([this](brls::View*) {
        return RunUiAction("library.sort.button", [this]() { CycleSortMode(); });
    });
    more_button_->registerClickAction([this](brls::View*) {
        return RunUiAction("library.more.button", [this]() { LoadMoreOrRefresh(); });
    });

    status_label_ = MakeParagraph(
        "Open Settings > Account to connect GeForce NOW and load your library.",
        8.0f, 14.0f);
    status_label_->setTextColor(nvgRGB(132, 139, 151));
    addView(status_label_);

    scrolling_frame_ = new brls::ScrollingFrame();
    scrolling_frame_->setGrow(1.0f);
    scrolling_frame_->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);

    grid_container_ = new brls::Box(brls::Axis::COLUMN);
    grid_container_->setPadding(0, 0, 30, 0);
    scrolling_frame_->setContentView(grid_container_);

    addView(scrolling_frame_);

    registerAction("Search", brls::BUTTON_Y, [this](brls::View* view) {
        (void)view;
        return RunUiAction("library.search.hotkey", [this]() { BeginSearch(); });
    }, false, false);

    registerAction("Login / More / Refresh", brls::BUTTON_X, [this](brls::View* view) {
        (void)view;
        return RunUiAction("library.more.hotkey", [this]() { LoadMoreOrRefresh(); });
    }, false, false);

    registerAction("Store Filter", brls::BUTTON_LT, [this](brls::View* view) {
        (void)view;
        return RunUiAction("library.filter.hotkey", [this]() { CycleStoreFilter(); });
    }, false, false);

    registerAction("Sort", brls::BUTTON_RT, [this](brls::View* view) {
        (void)view;
        return RunUiAction("library.sort.hotkey", [this]() { CycleSortMode(); });
    }, false, false);
}

LibraryTab::~LibraryTab()
{
    alive_->store(false);
    LogUiAction("library", "destroy");
}

void LibraryTab::BeginSearch()
{
    const auto alive = alive_;
    brls::Application::giveFocus(search_button_);
    brls::Application::getImeManager()->openForText(
        [this, alive](std::string text) {
            if (!alive->load())
                return;
            RunUiAction("library.search.result", [this, text = std::move(text)]() mutable {
                MoveFocusBeforeDestroy(grid_container_, search_button_);
                search_query_ = std::move(text);
                visible_limit_ = kInitialVisibleLibraryLimit;
                RebuildGrid();
            });
        },
        "Search Library", "Enter a title to filter your games", 64, search_query_);
}

void LibraryTab::willAppear(bool resetState)
{
    brls::Box::willAppear(resetState);

    EnsureSessionLoaded();
    UpdateSessionUi();

    auto& state = AppState::Instance();
    bool displayed_cached_library = false;
    if (state.HasLibraryGames())
    {
        games_ = state.library_games();
        RebuildGrid();
        displayed_cached_library = !games_.empty();
    }

    const auto now = std::chrono::steady_clock::now();
    const bool server_refresh_due =
        last_library_sync_.time_since_epoch().count() == 0 ||
        now - last_library_sync_ >= std::chrono::minutes(2);
    if (state.HasSession() && !state.session()->reauthentication_required &&
        !loading_ && (!displayed_cached_library || server_refresh_due))
        ReloadLibrary(displayed_cached_library);
}

void LibraryTab::EnsureSessionLoaded()
{
    auto& state = AppState::Instance();
    if (state.IsSessionLoaded())
        return;

    AuthSession session;
    if (client_.LoadSavedSession(session))
        state.SetSession(std::move(session));
    else
        state.MarkSessionLoaded();
}

void LibraryTab::UpdateSessionUi()
{
    const auto& state = AppState::Instance();
    if (!state.HasSession())
    {
        status_label_->setText("Open Settings > Account to connect GeForce NOW and load your library.");
        return;
    }

    const AuthSession& session = *state.session();

    if (session.reauthentication_required)
    {
        status_label_->setText(
            "Reconnect this account from Settings > Account before refreshing the library.");
    }
}

void LibraryTab::ReloadLibrary(bool background)
{
    auto& state = AppState::Instance();
    if (loading_ || !state.HasSession())
    {
        UpdateSessionUi();
        RebuildGrid();
        return;
    }

    loading_ = true;
    // Throttle automatic retries even when NVIDIA temporarily rejects a
    // refresh, otherwise every tab appearance immediately repeats it.
    last_library_sync_ = std::chrono::steady_clock::now();
    status_label_->setText("Syncing your GeForce NOW library...");

    AuthSession session = *state.session();
    GfnClient client = client_;
    const auto alive = alive_;
    brls::async([this, alive, client, session = std::move(session), background]() mutable {
        try
        {
            std::vector<GameInfo> games = client.FetchLibraryGames(session);
            brls::sync([this, alive, games = std::move(games), session = std::move(session), background]() mutable {
                if (!alive->load())
                    return;
                auto& current = AppState::Instance();
                if (!current.HasSession() ||
                    current.session()->user.user_id != session.user.user_id)
                {
                    loading_ = false;
                    return;
                }
                games_ = std::move(games);
                last_library_sync_ = std::chrono::steady_clock::now();
                current.SetSession(std::move(session));
                current.SetLibraryGames(games_);
                loading_ = false;
                UpdateSessionUi();
                RebuildGrid();
                if (!background)
                    brls::Application::notify("Library refreshed");
            });
        }
        catch (const std::exception& ex)
        {
            const std::string error = ex.what();
            brls::sync([this, alive, error, background]() {
                if (!alive->load())
                    return;
                loading_ = false;
                if (background && !games_.empty())
                {
                    status_label_->setText(
                        "Showing cached library. NVIDIA background refresh is temporarily unavailable.");
                    return;
                }
                ShowError("Library Sync Failed", error);
                UpdateSessionUi();
            });
        }
    }, false);
}

void LibraryTab::RebuildGrid()
{
    if (rebuilding_)
        return;
    rebuilding_ = true;
    struct ResetFlag { bool& flag; ~ResetFlag() { flag = false; } } reset {rebuilding_};

    MoveFocusBeforeDestroy(grid_container_, search_button_);
    grid_container_->clearViews();
    card_rows_.clear();
    first_card_ = nullptr;
    load_more_button_ = nullptr;
    current_grid_size_ = 0;

    const auto& state = AppState::Instance();
    if (!state.HasSession())
    {
        WireVerticalGridNavigation({toolbar_buttons_});
        grid_container_->addView(MakeParagraph(
            "After login, this screen will show your owned GeForce NOW titles with cover art and store labels.",
            0.0f));
        return;
    }

    LoadMore();
}

void LibraryTab::LoadMore()
{
    const bool load_more_had_focus = focus_new_cards_after_load_ ||
        (load_more_button_ && brls::Application::getCurrentFocus() == load_more_button_);
    focus_new_cards_after_load_ = false;

    std::vector<size_t> filtered_indices;
    std::string lower_query = ToLower(search_query_);

    for (size_t i = 0; i < games_.size(); ++i)
    {
        const GameInfo& game = games_[i];
        const bool matches_query = lower_query.empty() || ToLower(game.title).find(lower_query) != std::string::npos;
        if (matches_query && MatchesStoreFilter(game, store_filter_index_))
        {
            filtered_indices.push_back(i);
        }
    }

    SortLibraryIndices(
        filtered_indices, games_, static_cast<LibrarySortMode>(sort_mode_index_));

    filtered_count_ = filtered_indices.size();
    const size_t visible_count = std::min(filtered_count_, visible_limit_);
    if (load_more_button_)
    {
        MoveFocusBeforeDestroy(load_more_button_, more_button_);
        grid_container_->removeView(load_more_button_);
        load_more_button_ = nullptr;
    }
    
    std::string status = "Loaded " + std::to_string(games_.size()) + " games.";
    status += " Filter: " + std::string(kStoreFilters[store_filter_index_]) + ".";
    status += " Sort: " + std::string(kLibrarySortModes[sort_mode_index_]) + ".";
    if (!search_query_.empty())
        status += " Found " + std::to_string(filtered_count_) + " matches.";
    if (filtered_count_ > visible_count)
        status += " Showing first " + std::to_string(visible_count) + ". Press X or select Show more.";
    else
        status += " Press X to refresh.";
    
    status_label_->setText(status);
    filter_button_->setText("ZL  " + Tr(kStoreFilters[store_filter_index_]));
    sort_button_->setText("ZR  " + Tr(kLibrarySortModes[sort_mode_index_]));

    if (filtered_indices.empty())
    {
        WireVerticalGridNavigation({toolbar_buttons_});
        std::string empty_msg = search_query_.empty()
            ? "This account is logged in, but no owned games were returned by the current GeForce NOW library feed."
            : "No games found matching your search.";
        grid_container_->addView(MakeParagraph(empty_msg, 0.0f));
        return;
    }

    brls::View* first_new_card = nullptr;

    // Always start on a new row. To be perfectly accurate, we should pad the last row if it was incomplete.
    // For simplicity, we assume we load in multiples of kCardsPerRow, or we just append rows.
    size_t start_index = current_grid_size_;
    for (size_t start = start_index; start < visible_count; start += kCardsPerRow)
    {
        auto* row = new brls::Box(brls::Axis::ROW);
        row->setMarginBottom(2);
        std::vector<brls::View*> card_row;

        const size_t end = std::min(start + kCardsPerRow, visible_count);
        for (size_t i = start; i < end; ++i)
        {
            const size_t index = filtered_indices[i];
            const GameInfo& game = games_[index];

            GameCardDisplay display;
            display.title     = game.title;
            display.subtitle  = PrimaryStore(game);
            display.badge     = BuildMembershipBadge(
                game.last_played.empty() ? "In library" : "Recently played",
                game.membership_tier_label);
            display.image_url = game.image_url;

            auto* card = new GameCardView(display, [this, index]() {
                OpenGameDialog(index);
            });
            card->setMarginRight(i + 1 < end ? 24.0f : 0.0f);

            if (!first_new_card)
                first_new_card = card;
            if (!first_card_)
                first_card_ = card;

            row->addView(card);
            card_row.push_back(card);
        }

        grid_container_->addView(row);
        card_rows_.push_back(std::move(card_row));
    }

    std::vector<std::vector<brls::View*>> navigation_rows {toolbar_buttons_};
    navigation_rows.insert(navigation_rows.end(), card_rows_.begin(), card_rows_.end());
    WireVerticalGridNavigation(navigation_rows);
    
    current_grid_size_ = visible_count;

    if (filtered_count_ > visible_count)
    {
        load_more_button_ = new brls::Button();
        load_more_button_->setText(
            "Show more games (" + std::to_string(filtered_count_ - visible_count) + " left)");
        load_more_button_->setStyle(&brls::BUTTONSTYLE_PRIMARY);
        load_more_button_->setMarginTop(14);
        load_more_button_->setMarginBottom(24);
        load_more_button_->registerClickAction([this](brls::View* view) {
            (void)view;
            return RunUiAction("library.show_more.button", [this]() {
                if (filtered_count_ > visible_limit_)
                {
                    focus_new_cards_after_load_ = true;
                    MoveFocusBeforeDestroy(load_more_button_, more_button_);
                    visible_limit_ += kInitialVisibleLibraryLimit;
                    const auto alive = alive_;
                    brls::sync([this, alive]() {
                        if (alive->load())
                            LoadMore();
                    });
                }
            });
        });
        grid_container_->addView(load_more_button_);

        navigation_rows.push_back({load_more_button_});
        WireVerticalGridNavigation(navigation_rows);
    }

    if (first_new_card && (load_more_had_focus || !brls::Application::getCurrentFocus()))
    {
        brls::Application::giveFocus(first_new_card);
    }
}

void LibraryTab::LoadMoreOrRefresh()
{
    if (loading_)
        return;

    if (!AppState::Instance().HasSession())
    {
        brls::Application::notify("Connect an account from Settings > Account");
        return;
    }

    if (filtered_count_ > visible_limit_)
    {
        MoveFocusBeforeDestroy(grid_container_, more_button_);
        visible_limit_ += kInitialVisibleLibraryLimit;
        LoadMore();
        return;
    }

    ReloadLibrary();
}

void LibraryTab::CycleStoreFilter()
{
    MoveFocusBeforeDestroy(grid_container_, filter_button_);
    store_filter_index_ = (store_filter_index_ + 1) % kStoreFilters.size();
    visible_limit_      = kInitialVisibleLibraryLimit;
    RebuildGrid();
    brls::Application::notify("Store filter: " + std::string(kStoreFilters[store_filter_index_]));
}

void LibraryTab::CycleSortMode()
{
    MoveFocusBeforeDestroy(grid_container_, sort_button_);
    sort_mode_index_ = (sort_mode_index_ + 1) % kLibrarySortModes.size();
    RebuildGrid();
    brls::Application::notify("Sort: " + std::string(kLibrarySortModes[sort_mode_index_]));
}

bool LibraryTab::OpenGameDialog(size_t index)
{
    if (index >= games_.size())
        return false;

    brls::Application::pushActivity(new brls::Activity(new GameDetailView(
        client_,
        MakeLibraryGameDetail(games_[index]))));
    return true;
}

} // namespace opennow
