#include "library_tab.hpp"

#include "app_state.hpp"
#include "cover_image_cache.hpp"
#include "game_detail_view.hpp"
#include "game_detail_policy.hpp"
#include "game_grid_navigation.hpp"
#include "library_row_view.hpp"
#include "library_sort.hpp"
#include "library_timetable_policy.hpp"
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

constexpr size_t kRowsPerPage = 15;
constexpr std::array<const char*, 6> kStoreFilters = {"All", "Steam", "Epic", "Ubisoft", "Xbox", "Battle.net"};
constexpr std::array<const char*, 4> kLibrarySortModes = {
    "Last Played", "Last Added", "A-Z", "Store"};

brls::Label* MakeParagraph(const std::string& text, float bottom_margin = 16.0f, float font_size = 18.0f)
{
    auto* label = ui::MakeLabel(Tr(text), font_size, ui::Muted());
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
    for (const auto& store : game.available_stores)
        if (!game_detail::IsUnknownMetadata(store))
            return store;
    return Tr("Unknown store");
}

std::string GameIdentity(const GameInfo& game)
{
    return game.uuid.empty() ? game.id : game.uuid;
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
    setId("library");
    setPadding(10, 48, 14, 48);
    setBackgroundColor(ui::Ground());
    library_session_generation_ = AppState::Instance().session_generation();

    auto* body = new brls::Box(brls::Axis::ROW);
    body->setGrow(1);
    auto* timetable = new brls::Box(brls::Axis::COLUMN);
    timetable->setWidth(660);
    timetable->setShrink(0);
    timetable->setMarginRight(40);
    heading_ = ui::MakeLabel(Tr("My Library"), 28, ui::Text(), ui::FontRole::Heading);
    heading_->setGrow(1);
    heading_->setShrink(1);
    heading_->setSingleLine(true);

    auto make_control = [](const std::string& id, const std::string& text, float width,
                           std::function<bool(brls::View*)> action) {
        auto* control = new ui::ActionRow(text, {}, std::move(action));
        control->setId(id);
        control->setWidth(width);
        control->setHeight(38);
        control->setPadding(0, 8, 0, 8);
        return control;
    };
    search_button_ = make_control("library-search", "Y  " + Tr("Search"), 88,
        [this](brls::View*) { return RunUiAction("library.search.button", [this] { BeginSearch(); }); });
    filter_button_ = make_control("library-filter", "ZL  " + Tr("All"), 100,
        [this](brls::View*) { return RunUiAction("library.filter.button", [this] { CycleStoreFilter(); }); });
    sort_button_ = make_control("library-sort", "ZR  " + Tr("Last Played"), 124,
        [this](brls::View*) { return RunUiAction("library.sort.button", [this] { CycleSortMode(); }); });
    more_button_ = make_control("library-more", "X  " + Tr("More"), 96,
        [this](brls::View*) { return RunUiAction("library.more.button", [this] { LoadMoreOrRefresh(); }); });
    toolbar_buttons_ = {search_button_, filter_button_, sort_button_, more_button_};
    for (auto* control : {search_button_, filter_button_, sort_button_, more_button_})
    {
        control->SetFontSize(14);
        control->setPadding(0, 4, 0, 4);
    }
    auto* toolbar = new brls::Box(brls::Axis::ROW);
    toolbar->setHeight(38);
    toolbar->setShrink(0);
    toolbar->setAlignItems(brls::AlignItems::CENTER);
    toolbar->setMarginBottom(4);
    heading_->setMarginRight(12);
    toolbar->addView(heading_);
    for (size_t i = 0; i < toolbar_buttons_.size(); ++i)
    {
        toolbar_buttons_[i]->setMarginRight(i + 1 < toolbar_buttons_.size() ? 8 : 0);
        toolbar->addView(toolbar_buttons_[i]);
    }
    timetable->addView(toolbar);

    status_label_ = MakeParagraph(
        "Open Settings > Account to connect GeForce NOW and load your library.",
        8.0f, 14.0f);
    status_label_->setId("library-status");
    status_label_->setWidthPercentage(100);
    status_label_->setMaxHeight(60);
    timetable->addView(status_label_);

    scrolling_frame_ = new brls::ScrollingFrame();
    scrolling_frame_->setGrow(1.0f);
    scrolling_frame_->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);

    list_container_ = new brls::Box(brls::Axis::COLUMN);
    list_container_->setId("library-list");
    list_container_->setPadding(4, 4, 8, 4);
    scrolling_frame_->setContentView(list_container_);
    timetable->addView(scrolling_frame_);
    auto* paging = new brls::Box(brls::Axis::ROW);
    paging->setMarginTop(8);
    previous_button_ = make_control("library-previous", Tr("Previous page"), 320,
        [this](brls::View*) { return RunUiAction("library.previous.button", [this] { PreviousPage(); }); });
    previous_button_->setMarginRight(20);
    next_button_ = make_control("library-next", Tr("More / Refresh"), 320,
        [this](brls::View*) { return RunUiAction("library.next.button", [this] { LoadMoreOrRefresh(); }); });
    paging->addView(previous_button_);
    paging->addView(next_button_);
    timetable->addView(paging);
    body->addView(timetable);

    preview_container_ = new brls::Box(brls::Axis::COLUMN);
    preview_container_->setGrow(1);
    preview_container_->setShrink(1);
    preview_container_->setPaddingTop(6);
    preview_image_ = new CachedImage();
    preview_image_->setId("library-preview-image");
    preview_image_->setWidthPercentage(100);
    preview_image_->setHeight(272);
    preview_image_->setShrink(0);
    preview_image_->setCornerRadius(12);
    preview_image_->setScalingType(brls::ImageScalingType::FILL);
    preview_image_->setMarginBottom(10);
    preview_container_->addView(preview_image_);
    selection_label_ = ui::MakeLabel(Tr("Now selected"), 12, ui::Green(), ui::FontRole::Medium);
    selection_label_->setHeight(18);
    preview_container_->addView(selection_label_);
    preview_title_ = ui::MakeLabel({}, 28, ui::Text(), ui::FontRole::Heading);
    preview_title_->setId("library-preview-title");
    preview_title_->setWidthPercentage(100);
    preview_title_->setHeight(40);
    preview_title_->setSingleLine(true);
    preview_container_->addView(preview_title_);
    auto metadata = [this](const std::string& id) {
        auto* label = ui::MakeLabel({}, 14, ui::Muted());
        label->setId(id);
        label->setWidthPercentage(100);
        label->setHeight(20);
        label->setSingleLine(true);
        preview_container_->addView(label);
        return label;
    };
    preview_last_played_ = metadata("library-preview-last-played");
    preview_store_ = metadata("library-preview-store");
    preview_membership_ = metadata("library-preview-membership");
    stream_summary_ = new ui::NextStreamSummaryView(LoadStreamSettings());
    stream_summary_->setMarginTop(8);
    preview_container_->addView(stream_summary_);
    preview_container_->setVisibility(brls::Visibility::INVISIBLE);
    body->addView(preview_container_);
    addView(body);

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
    WireVerticalGridNavigation({toolbar_buttons_, {previous_button_, next_button_}});
}

LibraryTab::~LibraryTab()
{
    alive_->store(false);
    LogUiAction("library", "destroy");
}

void LibraryTab::BeginSearch()
{
    if (page_pending_)
        return;
    const auto alive = alive_;
    const auto generation = AppState::Instance().session_generation();
    brls::Application::giveFocus(search_button_);
    brls::Application::getImeManager()->openForText(
        [this, alive, generation](std::string text) {
            if (!alive->load() || AppState::Instance().session_generation() != generation)
                return;
            RunUiAction("library.search.result", [this, text = std::move(text)]() mutable {
                MoveFocusBeforeDestroy(list_container_, search_button_);
                search_query_ = std::move(text);
                page_index_ = 0;
                RebuildList();
            });
        },
        "Search Library", "Enter a title to filter your games", 64, search_query_);
}

void LibraryTab::willAppear(bool resetState)
{
    brls::Box::willAppear(resetState);

    EnsureSessionLoaded();
    auto& state = AppState::Instance();
    if (library_session_generation_ != state.session_generation())
    {
        library_session_generation_ = state.session_generation();
        selected_identity_.clear();
        page_index_ = 0;
        last_library_sync_ = {};
    }
    games_ = state.HasSession() ? state.library_games() : std::vector<GameInfo>{};
    stream_summary_->Update(LoadStreamSettings());
    RebuildList();
    const bool displayed_cached_library = !games_.empty();

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
        status_label_->setText(Tr("Open Settings > Account to connect GeForce NOW and load your library."));
        return;
    }

    const AuthSession& session = *state.session();

    if (session.reauthentication_required)
    {
        status_label_->setText(
            Tr("Reconnect this account from Settings > Account before refreshing the library."));
    }
}

void LibraryTab::ReloadLibrary(bool background)
{
    auto& state = AppState::Instance();
    if (loading_ || !state.HasSession() || state.session()->reauthentication_required)
    {
        UpdateSessionUi();
        RebuildList();
        return;
    }

    loading_ = true;
    // Throttle automatic retries even when NVIDIA temporarily rejects a
    // refresh, otherwise every tab appearance immediately repeats it.
    last_library_sync_ = std::chrono::steady_clock::now();
    status_label_->setText(Tr("Syncing your GeForce NOW library..."));

    AuthSession session = *state.session();
    const auto generation = state.session_generation();
    GfnClient client = client_;
    const auto alive = alive_;
    brls::async([this, alive, client, session = std::move(session), background, generation]() mutable {
        try
        {
            std::vector<GameInfo> games = client.FetchLibraryGames(session);
            brls::sync([this, alive, games = std::move(games), session = std::move(session), background, generation]() mutable {
                if (!alive->load())
                    return;
                auto& current = AppState::Instance();
                if (!current.IsCurrentSession(generation) ||
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
                library_session_generation_ = generation;
                RebuildList();
                if (!background)
                    brls::Application::notify("Library refreshed");
            });
        }
        catch (const std::exception& ex)
        {
            const std::string error = ex.what();
            brls::sync([this, alive, error, background, generation]() {
                if (!alive->load())
                    return;
                loading_ = false;
                if (!AppState::Instance().IsCurrentSession(generation))
                    return;
                if (background && !games_.empty())
                {
                    status_label_->setText(
                        Tr("Showing cached library. NVIDIA background refresh is temporarily unavailable."));
                    return;
                }
                ShowError("Library Sync Failed", error);
                UpdateSessionUi();
            });
        }
    }, false);
}

void LibraryTab::RebuildList()
{
    if (rebuilding_)
        return;
    rebuilding_ = true;
    struct ResetFlag { bool& flag; ~ResetFlag() { flag = false; } } reset {rebuilding_};

    const bool restore_row_focus = IsViewInside(list_container_, brls::Application::getCurrentFocus());
    MoveFocusBeforeDestroy(list_container_, search_button_);
    setLastFocusedView(search_button_);
    WireVerticalGridNavigation({toolbar_buttons_, {previous_button_, next_button_}});
    list_container_->clearViews();
    rows_.clear();
    page_pending_ = false;
    previous_button_->SetTitle(Tr("Previous page"));
    previous_button_->SetValue({});
    heading_->setText(Tr("My Library"));
    selection_label_->setText(Tr("Now selected"));
    search_button_->SetTitle("Y  " + Tr("Search"));
    filter_button_->SetTitle("ZL  " + Tr(kStoreFilters[store_filter_index_]));
    sort_button_->SetTitle("ZR  " + Tr(kLibrarySortModes[sort_mode_index_]));

    const auto& state = AppState::Instance();
    if (!state.HasSession())
    {
        selected_identity_.clear();
        filtered_count_ = 0;
        UpdatePreview();
        UpdateSessionUi();
        next_button_->SetTitle(Tr("Connect an account"));
        list_container_->addView(MakeParagraph(
            "After login, this screen will show your owned GeForce NOW titles with cover art and store labels.",
            0.0f));
        return;
    }

    std::vector<size_t> filtered_indices;
    const std::string lower_query = ToLower(search_query_);

    for (size_t i = 0; i < games_.size(); ++i)
    {
        const GameInfo& game = games_[i];
        const bool matches_query = lower_query.empty() || ToLower(game.title).find(lower_query) != std::string::npos;
        if (matches_query && MatchesStoreFilter(game, store_filter_index_))
        {
            filtered_indices.push_back(i);
        }
    }

    const auto now = std::chrono::system_clock::now();
    SortLibraryIndices(
        filtered_indices, games_, static_cast<LibrarySortMode>(sort_mode_index_));
    if (sort_mode_index_ == 0)
        std::stable_sort(filtered_indices.begin(), filtered_indices.end(), [this, now](size_t left, size_t right) {
            return library::PresentTimestamp(games_[left].last_played, now).bucket <
                library::PresentTimestamp(games_[right].last_played, now).bucket;
        });

    filtered_count_ = filtered_indices.size();
    const size_t total_pages = std::max<size_t>(1, (filtered_count_ + kRowsPerPage - 1) / kRowsPerPage);
    const auto selected = std::find_if(filtered_indices.begin(), filtered_indices.end(), [this](size_t index) {
        return GameIdentity(games_[index]) == selected_identity_;
    });
    if (selected != filtered_indices.end())
        page_index_ = static_cast<size_t>(selected - filtered_indices.begin()) / kRowsPerPage;
    page_index_ = std::min(page_index_, total_pages - 1);
    const size_t page_start = std::min(filtered_count_, page_index_ * kRowsPerPage);
    const size_t page_end = std::min(filtered_count_, page_start + kRowsPerPage);
    status_label_->setText(TrFormat("{0} games · {1} matches · {2} · {3} · {4}/{5}", {
        std::to_string(games_.size()), std::to_string(filtered_count_),
        Tr(kLibrarySortModes[sort_mode_index_]), Tr(kStoreFilters[store_filter_index_]),
        std::to_string(page_index_ + 1), std::to_string(total_pages)}));
    UpdateSessionUi();
    next_button_->SetTitle(Tr(page_end < filtered_count_ ? "Next page" : "Refresh library"));
    previous_button_->SetValue(page_index_ > 0 ? std::to_string(page_index_) : "");
    more_button_->SetTitle("X  " + Tr(page_end < filtered_count_ ? "More" : "Refresh"));

    if (filtered_indices.empty())
    {
        selected_identity_.clear();
        UpdatePreview();
        std::string empty_msg = search_query_.empty()
            ? "This account is logged in, but no owned games were returned by the current GeForce NOW library feed."
            : "No games found matching your search.";
        list_container_->addView(MakeParagraph(empty_msg, 0.0f));
        return;
    }

    std::optional<library::DateBucket> previous_bucket;
    brls::View* selected_row = nullptr;
    for (size_t i = page_start; i < page_end; ++i)
    {
        const auto& game = games_[filtered_indices[i]];
        const auto timestamp = library::PresentTimestamp(game.last_played, now);
        if (sort_mode_index_ == 0 && previous_bucket != timestamp.bucket)
        {
            auto* group = MakeParagraph(library::BucketLabel(timestamp.bucket), 2, 12);
            group->setHeight(16);
            group->setMarginTop(2);
            list_container_->addView(group);
            previous_bucket = timestamp.bucket;
        }
        const auto identity = GameIdentity(game);
        auto* row = new LibraryRowView(
            {identity, game.title, PrimaryStore(game),
             game_detail::IsUnknownMetadata(game.membership_tier_label) ? "" : game.membership_tier_label,
             Tr(timestamp.label), game.image_url},
            [this, identity] { SelectGame(identity); },
            [this, identity] { OpenGameDialog(identity); });
        if (identity == selected_identity_)
            selected_row = row;
        list_container_->addView(row);
        rows_.push_back(row);
    }
    if (!selected_row)
        SelectGame(GameIdentity(games_[filtered_indices[page_start]]));
    else
        UpdatePreview();
    std::vector<std::vector<brls::View*>> navigation_rows{toolbar_buttons_};
    for (auto* row : rows_)
        navigation_rows.push_back({row});
    navigation_rows.push_back({previous_button_, next_button_});
    WireVerticalGridNavigation(navigation_rows);
    if (scrolling_frame_)
        scrolling_frame_->setContentOffsetY(0, false);
    setLastFocusedView(selected_row ? selected_row : rows_.front());
    if (restore_row_focus)
        brls::Application::giveFocus(selected_row ? selected_row : rows_.front());
}

void LibraryTab::LoadMoreOrRefresh()
{
    if (loading_ || page_pending_)
        return;

    if (!AppState::Instance().HasSession())
    {
        brls::Application::notify("Connect an account from Settings > Account");
        return;
    }

    if ((page_index_ + 1) * kRowsPerPage < filtered_count_)
    {
        ChangePage(page_index_ + 1);
        return;
    }

    ReloadLibrary();
}

void LibraryTab::PreviousPage()
{
    if (!loading_ && !page_pending_ && page_index_ > 0)
        ChangePage(page_index_ - 1);
}

void LibraryTab::ChangePage(size_t page)
{
    page_pending_ = true;
    brls::Application::giveFocus(more_button_);
    const auto alive = alive_;
    const auto generation = AppState::Instance().session_generation();
    brls::sync([this, alive, generation, page] {
        if (!alive->load())
            return;
        page_pending_ = false;
        if (!AppState::Instance().IsCurrentSession(generation))
            return;
        page_index_ = page;
        selected_identity_.clear();
        RebuildList();
        if (!rows_.empty())
            brls::Application::giveFocus(rows_.front());
    });
}

void LibraryTab::SelectGame(const std::string& identity)
{
    selected_identity_ = identity;
    UpdatePreview();
}

void LibraryTab::UpdatePreview()
{
    const auto game = std::find_if(games_.begin(), games_.end(), [this](const GameInfo& value) {
        return GameIdentity(value) == selected_identity_;
    });
    if (!AppState::Instance().IsCurrentSession(library_session_generation_) ||
        game == games_.end() || selected_identity_.empty())
    {
        preview_container_->setVisibility(brls::Visibility::INVISIBLE);
        if (!preview_identity_.empty())
            SetCachedCoverImage(preview_image_, {});
        preview_identity_.clear();
        preview_image_url_.clear();
        preview_title_->setText({});
        return;
    }
    preview_container_->setVisibility(brls::Visibility::VISIBLE);
    if (preview_identity_ != selected_identity_ || preview_image_url_ != game->image_url)
    {
        SetCachedCoverImage(preview_image_, game->image_url);
        preview_identity_ = selected_identity_;
        preview_image_url_ = game->image_url;
    }
    preview_title_->setText(game->title);
    preview_store_->setText(Tr("Store") + " · " + PrimaryStore(*game));
    preview_membership_->setText(game_detail::IsUnknownMetadata(game->membership_tier_label) ? "" :
        Tr("Membership") + " · " + game->membership_tier_label);
    preview_last_played_->setText(Tr("Last played") + " · " +
        game_detail::FormatLastPlayed(game->last_played));
}

void LibraryTab::CycleStoreFilter()
{
    if (page_pending_)
        return;
    MoveFocusBeforeDestroy(list_container_, filter_button_);
    store_filter_index_ = (store_filter_index_ + 1) % kStoreFilters.size();
    page_index_ = 0;
    RebuildList();
    brls::Application::notify("Store filter: " + std::string(kStoreFilters[store_filter_index_]));
}

void LibraryTab::CycleSortMode()
{
    if (page_pending_)
        return;
    MoveFocusBeforeDestroy(list_container_, sort_button_);
    sort_mode_index_ = (sort_mode_index_ + 1) % kLibrarySortModes.size();
    page_index_ = 0;
    RebuildList();
    brls::Application::notify("Sort: " + std::string(kLibrarySortModes[sort_mode_index_]));
}

bool LibraryTab::OpenGameDialog(const std::string& identity)
{
    if (!AppState::Instance().IsCurrentSession(library_session_generation_))
        return false;
    const auto game = std::find_if(games_.begin(), games_.end(), [&identity](const GameInfo& value) {
        return GameIdentity(value) == identity;
    });
    if (game == games_.end())
        return false;

    brls::Application::pushActivity(new brls::Activity(new GameDetailView(
        client_,
        MakeLibraryGameDetail(*game))));
    return true;
}

} // namespace opennow
