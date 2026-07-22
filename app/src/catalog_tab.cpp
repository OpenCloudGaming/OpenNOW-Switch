#include "catalog_tab.hpp"

#include "app_state.hpp"
#include "game_detail_view.hpp"
#include "game_card_view.hpp"
#include "game_grid_navigation.hpp"
#include "ui_action_guard.hpp"
#include "ui_helpers.hpp"
#include "ui_text_policy.hpp"
#include "localization.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <utility>

namespace opennow
{
namespace
{

constexpr size_t kInitialVisibleGameLimit = 15;
constexpr size_t kCardsPerRow      = 5;
constexpr std::array<const char*, 6> kStoreFilters = {"All", "Steam", "Epic", "Ubisoft", "Xbox", "Battle.net"};
constexpr std::array<const char*, 3> kCatalogSortModes = {"A-Z", "Store", "Publisher"};

brls::Label* MakeParagraph(const std::string& text, float bottom_margin = 16.0f)
{
    auto* label = new brls::Label();
    label->setText(Tr(text));
    label->setFontSize(18);
    label->setMarginBottom(bottom_margin);
    return label;
}

brls::Button* MakeToolbarButton(const std::string& text)
{
    auto* button = new brls::Button();
    const std::string localized = Tr(text);
    button->setText(localized);
    button->setFontSize(15);
    button->setStyle(&brls::BUTTONSTYLE_BORDERED);
    button->setHeight(42);
    button->setWidth(ui::ToolbarButtonWidth(localized));
    button->setMarginRight(10);
    return button;
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

bool MatchesStoreFilter(const PublicGame& game, size_t filter_index)
{
    if (filter_index == 0 || filter_index >= kStoreFilters.size())
        return true;

    const std::string filter = kStoreFilters[filter_index];
    if (ContainsText(game.store, filter) || ContainsText(game.publisher, filter))
        return true;

    if (filter == "Battle.net")
        return ContainsText(game.store, "battle") || ContainsText(game.publisher, "blizzard");

    return false;
}

} // namespace

CatalogTab::CatalogTab()
    : brls::Box(brls::Axis::COLUMN)
{
    setPadding(18, 32, 18, 32);
    setBackgroundColor(nvgRGB(16, 16, 20));

    auto* heading = new brls::Box(brls::Axis::ROW);
    heading->setAlignItems(brls::AlignItems::CENTER);
    heading->setMarginBottom(10);
    auto* accent = new brls::Rectangle();
    accent->setWidth(4);
    accent->setHeight(30);
    accent->setMarginRight(12);
    accent->setColor(nvgRGB(88, 217, 138));
    heading->addView(accent);
    auto* title = MakeParagraph("Store", 0.0f);
    title->setFontSize(28);
    title->setTextColor(nvgRGB(248, 249, 251));
    heading->addView(title);
    addView(heading);

    auto* toolbar = new brls::Box(brls::Axis::ROW);
    toolbar->setAlignItems(brls::AlignItems::CENTER);
    toolbar->setMarginBottom(8);
    search_button_ = MakeToolbarButton("Y  Search");
    filter_button_ = MakeToolbarButton("ZL  All stores");
    sort_button_   = MakeToolbarButton("ZR  A-Z");
    more_button_   = MakeToolbarButton("X  More / Refresh");
    search_button_->setStyle(&brls::BUTTONSTYLE_HIGHLIGHT);
    toolbar_buttons_ = {search_button_, filter_button_, sort_button_, more_button_};
    for (brls::View* button : toolbar_buttons_)
        toolbar->addView(button);
    addView(toolbar);

    search_button_->registerClickAction([this](brls::View*) {
        return RunUiAction("catalog.search.button", [this]() { BeginSearch(); });
    });
    filter_button_->registerClickAction([this](brls::View*) {
        return RunUiAction("catalog.filter.button", [this]() { CycleStoreFilter(); });
    });
    sort_button_->registerClickAction([this](brls::View*) {
        return RunUiAction("catalog.sort.button", [this]() { CycleSortMode(); });
    });
    more_button_->registerClickAction([this](brls::View*) {
        return RunUiAction("catalog.more.button", [this]() { LoadMoreOrRefresh(); });
    });

    status_label_ = MakeParagraph("Loading the supported GeForce NOW catalog...", 8.0f);
    status_label_->setFontSize(14);
    status_label_->setTextColor(nvgRGB(132, 139, 151));
    addView(status_label_);

    scrolling_frame_ = new brls::ScrollingFrame();
    scrolling_frame_->setGrow(1.0f);
    scrolling_frame_->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);

    list_container_ = new brls::Box(brls::Axis::COLUMN);
    list_container_->setPadding(0, 0, 32, 0);
    scrolling_frame_->setContentView(list_container_);

    addView(scrolling_frame_);

    registerAction("Search", brls::BUTTON_Y, [this](brls::View* view) {
        (void)view;
        return RunUiAction("catalog.search.hotkey", [this]() { BeginSearch(); });
    }, false, false);

    registerAction("More / Refresh", brls::BUTTON_X, [this](brls::View* view) {
        (void)view;
        return RunUiAction("catalog.more.hotkey", [this]() { LoadMoreOrRefresh(); });
    }, false, false);

    registerAction("Store Filter", brls::BUTTON_LT, [this](brls::View* view) {
        (void)view;
        return RunUiAction("catalog.filter.hotkey", [this]() { CycleStoreFilter(); });
    }, false, false);

    registerAction("Sort", brls::BUTTON_RT, [this](brls::View* view) {
        (void)view;
        return RunUiAction("catalog.sort.hotkey", [this]() { CycleSortMode(); });
    }, false, false);
}

CatalogTab::~CatalogTab()
{
    alive_->store(false);
    LogUiAction("catalog", "destroy");
}

void CatalogTab::BeginSearch()
{
    const auto alive = alive_;
    brls::Application::giveFocus(search_button_);
    brls::Application::getImeManager()->openForText(
        [this, alive](std::string text) {
            if (!alive->load())
                return;
            RunUiAction("catalog.search.result", [this, text = std::move(text)]() mutable {
                MoveFocusBeforeDestroy(list_container_, search_button_);
                search_query_ = std::move(text);
                page_index_ = 0;
                if (search_query_.empty() && !base_games_.empty())
                {
                    games_ = base_games_;
                    RebuildList();
                }
                else if (!search_query_.empty() && AppState::Instance().HasSession())
                {
                    ReloadCatalog(search_query_);
                }
                else
                {
                    RebuildList();
                }
            });
        },
        "Search Catalog", "Enter a title to filter games", 64, search_query_);
}

void CatalogTab::willAppear(bool resetState)
{
    brls::Box::willAppear(resetState);

    const auto& state = AppState::Instance();
    if (games_.empty() && state.HasPublicGames())
    {
        games_ = state.public_games();
        base_games_ = games_;
        RebuildList();
        return;
    }

    if (games_.empty() && !loading_)
        ReloadCatalog();
}

void CatalogTab::ReloadCatalog(const std::string& server_query)
{
    if (loading_)
        return;

    loading_ = true;
    status_label_->setText(server_query.empty()
        ? "Loading current GeForce NOW catalog..."
        : "Searching the GeForce NOW catalog...");

    GfnClient client = client_;
    const auto alive = alive_;
    const bool authenticated = AppState::Instance().HasSession();
    AuthSession session;
    if (authenticated)
        session = *AppState::Instance().session();
    brls::async([this, alive, client, authenticated, session = std::move(session), server_query]() mutable {
        try
        {
            std::vector<PublicGame> games = authenticated
                ? client.FetchCatalogGames(session, server_query)
                : client.FetchPublicGames();
            brls::sync([this, alive, authenticated, games = std::move(games),
                        session = std::move(session), server_query]() mutable {
                if (!alive->load())
                    return;
                games_ = std::move(games);
                if (authenticated)
                    AppState::Instance().SetSession(std::move(session));
                if (server_query.empty())
                {
                    base_games_ = games_;
                    AppState::Instance().SetPublicGames(games_);
                }
                loading_ = false;
                page_index_ = 0;
                RebuildList();
                brls::Application::notify(server_query.empty()
                    ? "Store catalog refreshed"
                    : "Catalog search completed");
            });
        }
        catch (const std::exception& ex)
        {
            const std::string error = ex.what();
            brls::sync([this, alive, error]() {
                if (!alive->load())
                    return;
                loading_ = false;
                status_label_->setText("Catalog request failed. Press X to retry.");
                ShowError("Catalog Request Failed", error);
            });
        }
    }, false);
}

void CatalogTab::RebuildList()
{
    if (rebuilding_)
        return;
    rebuilding_ = true;
    struct ResetFlag { bool& flag; ~ResetFlag() { flag = false; } } reset {rebuilding_};

    brls::View* current_focus = brls::Application::getCurrentFocus();
    const bool paging_had_focus = current_focus == load_more_button_ ||
        current_focus == previous_page_button_;

    MoveFocusBeforeDestroy(list_container_, search_button_);
    DetachPagingButton();
    list_container_->clearViews();
    card_rows_.clear();
    first_card_ = nullptr;
    load_more_pending_ = false;

    std::vector<size_t> filtered_indices;
    std::string lower_query = ToLower(search_query_);

    for (size_t i = 0; i < games_.size(); ++i)
    {
        const PublicGame& game = games_[i];
        const bool matches_query = lower_query.empty() || ToLower(game.title).find(lower_query) != std::string::npos;
        if (matches_query && MatchesStoreFilter(game, store_filter_index_))
        {
            filtered_indices.push_back(i);
        }
    }

    std::sort(filtered_indices.begin(), filtered_indices.end(), [this](size_t left_index, size_t right_index) {
        const PublicGame& left = games_[left_index];
        const PublicGame& right = games_[right_index];

        if (sort_mode_index_ == 1 && left.store != right.store)
            return left.store < right.store;

        if (sort_mode_index_ == 2 && left.publisher != right.publisher)
            return left.publisher < right.publisher;

        return left.title < right.title;
    });

    filtered_count_ = filtered_indices.size();
    const size_t total_pages = filtered_count_ == 0
        ? 1
        : (filtered_count_ + kInitialVisibleGameLimit - 1) / kInitialVisibleGameLimit;
    if (page_index_ >= total_pages)
        page_index_ = total_pages - 1;
    const size_t page_start = std::min(filtered_count_, page_index_ * kInitialVisibleGameLimit);
    const size_t page_end = std::min(filtered_count_, page_start + kInitialVisibleGameLimit);
    std::string status = "Loaded " + std::to_string(games_.size()) + " supported games.";
    status += " Filter: " + std::string(kStoreFilters[store_filter_index_]) + ".";
    status += " Sort: " + std::string(kCatalogSortModes[sort_mode_index_]) + ".";
    if (!search_query_.empty())
        status += " Found " + std::to_string(filtered_count_) + " matches.";
    if (filtered_count_ > 0)
        status += " Showing " + std::to_string(page_start + 1) + "-" +
            std::to_string(page_end) + " of " + std::to_string(filtered_count_) +
            " (page " + std::to_string(page_index_ + 1) + "/" +
            std::to_string(total_pages) + ").";
    else
        status += " Press X to refresh.";

    status_label_->setText(status);
    filter_button_->setText("ZL  " + Tr(kStoreFilters[store_filter_index_]));
    sort_button_->setText("ZR  " + Tr(kCatalogSortModes[sort_mode_index_]));

    if (filtered_indices.empty())
    {
        std::string empty_msg = search_query_.empty()
            ? "No public titles were returned by the feed."
            : "No games found matching your search.";
        list_container_->addView(MakeParagraph(empty_msg, 0.0f));
        AttachPagingButton(false, 0);
        WireVerticalGridNavigation({toolbar_buttons_, {previous_page_button_, load_more_button_}});
        return;
    }

    for (size_t start = page_start; start < page_end; start += kCardsPerRow)
    {
        auto* row = new brls::Box(brls::Axis::ROW);
        std::vector<brls::View*> card_row;
        const size_t end = std::min(start + kCardsPerRow, page_end);

        for (size_t i = start; i < end; ++i)
        {
            const size_t index = filtered_indices[i];
            const PublicGame& game = games_[index];

            GameCardDisplay display;
            display.title     = game.title;
            display.subtitle  = game.store;
            display.badge     = game.is_in_library
                ? "In library"
                : (game.publisher.empty() ? "GFN catalog" : game.publisher);
            display.image_url = game.image_url;

            auto* card = new GameCardView(display, [this, index]() {
                OpenGameDialog(nullptr, index);
            });

            if (!first_card_)
                first_card_ = card;

            row->addView(card);
            card_row.push_back(card);
        }

        list_container_->addView(row);
        card_rows_.push_back(std::move(card_row));
    }

    std::vector<std::vector<brls::View*>> navigation_rows {toolbar_buttons_};
    navigation_rows.insert(navigation_rows.end(), card_rows_.begin(), card_rows_.end());
    AttachPagingButton(page_end < filtered_count_, filtered_count_ - page_end);
    navigation_rows.push_back({previous_page_button_, load_more_button_});
    WireVerticalGridNavigation(navigation_rows);

    if (paging_had_focus && first_card_)
        brls::Application::giveFocus(first_card_);
    else if (first_card_ && !brls::Application::getCurrentFocus())
        brls::Application::giveFocus(first_card_);
}

void CatalogTab::EnsurePagingButton()
{
    if (paging_container_)
        return;

    paging_container_ = new brls::Box(brls::Axis::ROW);
    paging_container_->setMarginTop(14);
    paging_container_->setMarginBottom(24);

    previous_page_button_ = new brls::Button();
    previous_page_button_->setStyle(&brls::BUTTONSTYLE_BORDERED);
    previous_page_button_->setMarginRight(12);
    previous_page_button_->setGrow(1.0f);
    previous_page_button_->registerClickAction([this](brls::View*) {
        return RunUiAction("catalog.previous_page.button", [this]() {
            HandlePreviousPage();
        });
    });
    paging_container_->addView(previous_page_button_);

    load_more_button_ = new brls::Button();
    load_more_button_->setStyle(&brls::BUTTONSTYLE_PRIMARY);
    load_more_button_->setGrow(1.0f);
    load_more_button_->registerClickAction([this](brls::View*) {
        return RunUiAction("catalog.show_more.button", [this]() {
            HandlePagingButton();
        });
    });
    paging_container_->addView(load_more_button_);
}

void CatalogTab::DetachPagingButton()
{
    if (!paging_container_ || !paging_button_attached_)
        return;

    // Navigation routes store raw view pointers. Keep this object alive while
    // rows are appended so no route can observe a freed paging button.
    list_container_->removeView(paging_container_, false);
    paging_button_attached_ = false;
}

void CatalogTab::AttachPagingButton(bool has_more, size_t remaining)
{
    EnsurePagingButton();
    if (paging_button_attached_)
        DetachPagingButton();

    previous_page_button_->setText(Tr(page_index_ > 0 ? "Previous page" : "First page"));
    load_more_button_->setText(has_more
        ? "Show more games (" + std::to_string(remaining) + " left)"
        : "Refresh catalog");
    list_container_->addView(paging_container_);
    paging_button_attached_ = true;
}

void CatalogTab::HandlePagingButton()
{
    if (loading_ || load_more_pending_)
        return;

    const size_t next_start = (page_index_ + 1) * kInitialVisibleGameLimit;
    if (next_start >= filtered_count_)
    {
        ReloadCatalog();
        return;
    }

    load_more_pending_ = true;
    brls::Application::giveFocus(more_button_);
    const auto alive = alive_;
    brls::sync([this, alive]() {
        if (!alive->load())
            return;

        const std::string before = "page=" + std::to_string(page_index_ + 1) +
            " filtered=" + std::to_string(filtered_count_);
        LogUiAction("catalog.show_more.deferred", "begin", before);
        try
        {
            ++page_index_;
            RebuildList();
            if (first_card_)
                brls::Application::giveFocus(first_card_);
            load_more_pending_ = false;
            LogUiAction("catalog.show_more.deferred", "ok",
                "page=" + std::to_string(page_index_ + 1) +
                " filtered=" + std::to_string(filtered_count_));
        }
        catch (const std::exception& exception)
        {
            load_more_pending_ = false;
            LogUiAction("catalog.show_more.deferred", "error", exception.what());
            ShowError("Store Pagination Failed", exception.what());
        }
        catch (...)
        {
            load_more_pending_ = false;
            LogUiAction("catalog.show_more.deferred", "error", "unknown exception");
            ShowError("Store Pagination Failed", "Unknown interface error.");
        }
    });
}

void CatalogTab::HandlePreviousPage()
{
    if (loading_ || load_more_pending_ || page_index_ == 0)
        return;

    load_more_pending_ = true;
    brls::Application::giveFocus(more_button_);
    const auto alive = alive_;
    brls::sync([this, alive]() {
        if (!alive->load())
            return;
        --page_index_;
        RebuildList();
        if (first_card_)
            brls::Application::giveFocus(first_card_);
        load_more_pending_ = false;
        LogUiAction("catalog.previous_page.deferred", "ok",
            "page=" + std::to_string(page_index_ + 1));
    });
}

void CatalogTab::LoadMoreOrRefresh()
{
    if (loading_)
        return;

    if ((page_index_ + 1) * kInitialVisibleGameLimit < filtered_count_)
    {
        HandlePagingButton();
        return;
    }

    ReloadCatalog();
}

void CatalogTab::CycleStoreFilter()
{
    MoveFocusBeforeDestroy(list_container_, filter_button_);
    store_filter_index_ = (store_filter_index_ + 1) % kStoreFilters.size();
    page_index_         = 0;
    RebuildList();
    brls::Application::notify("Store filter: " + std::string(kStoreFilters[store_filter_index_]));
}

void CatalogTab::CycleSortMode()
{
    MoveFocusBeforeDestroy(list_container_, sort_button_);
    sort_mode_index_ = (sort_mode_index_ + 1) % kCatalogSortModes.size();
    page_index_      = 0;
    RebuildList();
    brls::Application::notify("Sort: " + std::string(kCatalogSortModes[sort_mode_index_]));
}

bool CatalogTab::OpenGameDialog(brls::View* view, size_t index)
{
    (void)view;

    if (index >= games_.size())
        return false;

    brls::Application::pushActivity(new brls::Activity(new GameDetailView(
        client_,
        MakeCatalogGameDetail(games_[index]))));
    return true;
}

} // namespace opennow
