#include "game_detail_view.hpp"

#include "app_state.hpp"
#include "cover_image_cache.hpp"
#include "game_detail_policy.hpp"
#include "home_shortcut.hpp"
#include "nte_credentials.hpp"
#include "ui_helpers.hpp"
#include "localization.hpp"

#ifdef __SWITCH__
#include <switch.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <sstream>

namespace opennow
{
namespace
{

class CoverHero final : public CachedImage
{
  public:
    brls::View* hitTest(brls::Point) override { return nullptr; }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override
    {
        CachedImage::draw(vg, x, y, width, height, style, ctx);
        nvgBeginPath(vg);
        nvgRect(vg, x, y, width, height);
        nvgFillPaint(vg, nvgLinearGradient(vg, x + width * 0.25f, y,
            x + width * 0.85f, y, ui::Ground(), nvgRGBA(11, 12, 14, 60)));
        nvgFill(vg);
        nvgBeginPath(vg);
        nvgRect(vg, x, y, width, height);
        nvgFillPaint(vg, nvgLinearGradient(vg, x, y + height * 0.6f,
            x, y + height, nvgRGBA(11, 12, 14, 0), nvgRGBA(11, 12, 14, 160)));
        nvgFill(vg);
    }
};

std::string PrimaryStore(const GameInfo& game)
{
    if (!game.available_stores.empty())
        return game_detail::DisplayStore(game.available_stores.front());

    return "GeForce NOW";
}

std::string JoinStores(const GameInfo& game)
{
    if (game.available_stores.empty())
        return PrimaryStore(game);

    std::ostringstream stream;
    for (size_t i = 0; i < game.available_stores.size(); ++i)
    {
        if (i > 0)
            stream << ", ";
        stream << game_detail::DisplayStore(game.available_stores[i]);
    }
    return stream.str();
}

std::string SafeText(const std::string& value, const std::string& fallback)
{
    return value.empty() ? fallback : value;
}

std::string VariantLabel(const GameVariant& variant)
{
    std::string label = SafeText(variant.store, "Unknown store");
    if (variant.library_selected)
        label += " (selected in library)";
    else if (!variant.library_status.empty())
        label += " (" + variant.library_status + ")";
    return label;
}

bool IsNumericLaunchId(const std::string& id)
{
    return !id.empty() && std::all_of(id.begin(), id.end(), [](unsigned char ch) {
        return std::isdigit(ch) != 0;
    });
}

std::string PromptCredential(
    const std::string& title, const std::string& initial, bool password)
{
#ifdef __SWITCH__
    SwkbdConfig keyboard {};
    if (R_FAILED(swkbdCreate(&keyboard, 0)))
        return {};
    if (password)
        swkbdConfigMakePresetPassword(&keyboard);
    else
        swkbdConfigMakePresetDefault(&keyboard);
    swkbdConfigSetType(&keyboard, SwkbdType_Latin);
    swkbdConfigSetStringLenMax(&keyboard, password ? 128 : 254);
    swkbdConfigSetHeaderText(&keyboard, title.c_str());
    const std::string save = Tr("Save");
    swkbdConfigSetOkButtonText(&keyboard, save.c_str());
    if (!initial.empty() && !password)
        swkbdConfigSetInitialText(&keyboard, initial.c_str());
    std::array<char, 512> output {};
    const Result result = swkbdShow(&keyboard, output.data(), output.size());
    swkbdClose(&keyboard);
    if (R_FAILED(result))
        return {};
    return output.data();
#else
    (void)title;
    (void)initial;
    (void)password;
    return {};
#endif
}

} // namespace

GameDetailView::GameDetailView(const GfnClient& client, GameDetailData data)
    : brls::Box(brls::Axis::COLUMN)
    , client_(client)
    , data_(std::move(data))
    , account_generation_(AppState::Instance().session_generation())
{
    const std::string saved_variant = client_.LoadLauncherPreference(ActiveUserId(), data_.game_id);
    for (size_t i = 0; i < data_.variants.size(); ++i)
    {
        if (!saved_variant.empty() && data_.variants[i].id == saved_variant)
        {
            selected_variant_index_ = i;
            launcher_preference_loaded_ = true;
            break;
        }
        if (!launcher_preference_loaded_ && data_.variants[i].id == data_.launch_app_id)
            selected_variant_index_ = i;
    }

    setId("detail");
    setBackgroundColor(ui::Ground());

    auto* header = new brls::Box(brls::Axis::ROW);
    header->setId("detail/header");
    header->setHeight(76);
    header->setShrink(0);
    header->setPadding(0, 48, 0, 48);
    header->setAlignItems(brls::AlignItems::CENTER);
    header->setBorderThickness(1);
    header->setBorderColor(ui::Rule());
    auto* logo = new brls::Image();
    logo->setWidth(56);
    logo->setHeight(32);
    logo->setShrink(0);
    logo->setMarginRight(28);
    logo->setImageFromRes("img/opennow-logo-mark-small.png");
    header->addView(logo);
    auto* route = ui::MakeLabel(Tr(data_.owned ? "Library" : "Store") + "  /", 18, ui::Muted());
    route->setSingleLine(true);
    route->setMaxWidthPercentage(30);
    route->setMarginRight(14);
    header->addView(route);
    auto* breadcrumb = ui::MakeLabel(data_.title, 18, ui::Text(), ui::FontRole::Medium);
    breadcrumb->setSingleLine(true);
    breadcrumb->setGrow(1);
    breadcrumb->setShrink(1);
    header->addView(breadcrumb);
    addView(header);

    auto* content = new brls::Box(brls::Axis::ROW);
    content->setId("detail/content");
    content->setGrow(1);
    content->setShrink(1);
    content->setClipsToBounds(true);

    auto* hero = new CoverHero();
    hero->setId("detail/cover");
    hero->setPositionType(brls::PositionType::ABSOLUTE);
    hero->setPositionTop(0);
    hero->setPositionLeft(0);
    hero->setWidthPercentage(100);
    hero->setHeightPercentage(100);
    hero->setScalingType(brls::ImageScalingType::FILL);
    SetCachedCoverImage(hero, data_.image_url);
    content->addView(hero);

    auto* left = new brls::Box(brls::Axis::COLUMN);
    left->setWidth(640);
    left->setShrink(0);
    left->setPadding(28, 32, 24, 48);

    std::string metadata = game_detail::DisplayStore(data_.stores);
    if (!data_.membership_tier_label.empty())
        metadata += " · " + data_.membership_tier_label;
    if (!game_detail::IsUnknownMetadata(data_.publisher))
        metadata += " · " + data_.publisher;
    auto* metadata_label = ui::MakeLabel(metadata, 14, ui::Muted());
    metadata_label->setId("detail/metadata");
    metadata_label->setSingleLine(true);
    metadata_label->setShrink(0);
    metadata_label->setMarginBottom(8);
    left->addView(metadata_label);

    auto* title = ui::MakeLabel(data_.title, 44, ui::Text(), ui::FontRole::Display);
    title->setId("detail/title");
    title->setSingleLine(true);
    title->setShrink(0);
    title->setMarginBottom(4);
    left->addView(title);

    auto* description = ui::MakeLabel(
        data_.description.empty() ? Tr("No description is available yet for this title.") : data_.description,
        18, ui::Muted());
    description->setSingleLine(false);
    auto* description_frame = new brls::ScrollingFrame();
    description_frame->setId("detail/description");
    description_frame->setHeight(72);
    description_frame->setShrink(0);
    description_frame->setScrollingIndicatorVisible(false);
    description_frame->setMarginBottom(20);
    description_frame->setContentView(description);
    left->addView(description_frame);

    auto* actions = new brls::Box(brls::Axis::COLUMN);
    actions->setId("detail/actions");
    actions->setGrow(1);
    actions->setWidth(472);
    actions->setPadding(4);
    play_button_ = new ui::ActionRow(Tr(data_.owned ? "Play on GeForce NOW" : "Play from Store"), {},
        [this](brls::View*) {
            Play();
            return true;
        }, ui::ActionTone::Primary);
    play_button_->setId("detail/play");
    play_button_->setHeight(64);
    play_button_->setMarginBottom(10);
    play_button_->updateActionHint(brls::BUTTON_A, Tr("Play"));
    actions->addView(play_button_);

    store_button_ = new ui::ActionRow(Tr("Store"), {}, [this](brls::View*) {
        ShowStoreSelector(false);
        return true;
    });
    store_button_->setId("detail/store");
    store_button_->setMarginBottom(10);
    store_button_->updateActionHint(brls::BUTTON_A, Tr("Choose game store"));
    UpdateStoreButton();
    actions->addView(store_button_);

    auto* shortcut_button = new ui::ActionRow(Tr("Create Switch shortcut"), Tr("HOME screen"),
        [this](brls::View*) {
            CreateSwitchShortcut();
            return true;
        });
    shortcut_button->setId("detail/shortcut");
    shortcut_button->updateActionHint(brls::BUTTON_A, Tr("Create Switch shortcut"));
    actions->addView(shortcut_button);

    if (IsNevernessToEverness(data_.title))
    {
        nte_button_ = new ui::ActionRow("NTE Auto-login", {}, [this](brls::View*) {
            OpenNteCredentialsMenu();
            return true;
        });
        nte_button_->setId("detail/nte");
        nte_button_->setMarginTop(10);
        UpdateNteButton();
        actions->addView(nte_button_);
    }
    left->addView(actions);
    content->addView(left);

    auto* right = new brls::Box(brls::Axis::COLUMN);
    right->setGrow(1);
    right->setShrink(1);
    right->setPadding(28, 48, 24, 16);
    right->setJustifyContent(brls::JustifyContent::FLEX_END);
    right->setAlignItems(brls::AlignItems::FLEX_END);
    auto* summary = new ui::NextStreamSummaryView(LoadStreamSettings());
    summary->setId("detail/next-stream");
    summary->setWidth(380);
    if (!data_.last_played.empty())
    {
        auto* history = ui::MakeLabel(Tr("Last played") + " · " + game_detail::FormatLastPlayed(data_.last_played),
            14, ui::Muted());
        history->setSingleLine(true);
        history->setMarginTop(8);
        summary->addView(history);
    }
    right->addView(summary);
    content->addView(right);
    addView(content);

    registerAction(Tr("Back"), brls::BUTTON_B, [](brls::View*) {
        brls::Application::popActivity();
        return true;
    });
    auto* footer = new brls::Box(brls::Axis::ROW);
    footer->setId("detail/footer");
    footer->setHeight(60);
    footer->setShrink(0);
    footer->setPadding(0, 32, 0, 32);
    footer->setAlignItems(brls::AlignItems::CENTER);
    footer->setBorderThickness(1);
    footer->setBorderColor(ui::Rule());
    auto* hints = new brls::Hints();
    hints->setId("detail/hints");
    hints->setAllowAButtonTouch(true);
    hints->setAddUnableAButtonAction(false);
    hints->setMaxWidthPercentage(100);
    hints->applyXMLAttribute("forceShown", "true");
    footer->addView(hints);
    addView(footer);
}

GameDetailView::~GameDetailView()
{
    alive_->store(false);
}

brls::View* GameDetailView::getDefaultFocus()
{
    return play_button_;
}

void GameDetailView::Play()
{
    const auto& session = AppState::Instance().session();
    if (!session)
    {
        ShowError("Not Logged In", "Sign in from Library before starting a GeForce NOW session.");
        return;
    }
    if (!AppState::Instance().IsCurrentSession(account_generation_))
    {
        ShowError("Account Changed", "Open this game again from the current account before playing.");
        return;
    }

    if (data_.variants.size() > 1 && !launcher_preference_loaded_)
    {
        ShowStoreSelector(true);
        return;
    }

    LaunchSelectedVariant();
}

std::string GameDetailView::ActiveUserId() const
{
    const auto& session = AppState::Instance().session();
    return session ? session->user.user_id : "";
}

void GameDetailView::UpdateStoreButton()
{
    if (!store_button_)
        return;
    const std::string store = selected_variant_index_ < data_.variants.size()
        ? game_detail::DisplayStore(data_.variants[selected_variant_index_].store)
        : game_detail::DisplayStore(data_.stores);
    store_button_->SetValue(store + (data_.variants.size() > 1 ? " · " + Tr("change") : ""));
}

void GameDetailView::UpdateNteButton()
{
    if (!nte_button_)
        return;
    nte_button_->SetValue(
        LoadNteCredentials().valid()
            ? "Saved · L + X in game"
            : "Set email and password");
}

void GameDetailView::ConfigureNteCredentials()
{
    NteCredentials credentials = LoadNteCredentials();
    const std::string email = PromptCredential(
        "NTE email address", credentials.email, false);
    if (email.empty())
    {
        brls::Application::notify("NTE credential setup cancelled");
        return;
    }

    const std::string password = PromptCredential("NTE password", {}, true);
    if (password.empty())
    {
        brls::Application::notify("NTE credential setup cancelled");
        return;
    }

    credentials.email = email;
    credentials.password = password;
    if (!SaveNteCredentials(credentials))
    {
        ShowError("NTE Auto-login", "Could not write " + NteCredentialsPath());
        return;
    }
    std::fill(credentials.password.begin(), credentials.password.end(), '\0');
    UpdateNteButton();
    brls::Application::notify("NTE Auto-login saved; press L + X during the NTE sign-in screen");
}

void GameDetailView::OpenNteCredentialsMenu()
{
    const bool configured = LoadNteCredentials().valid();
    if (!configured)
    {
        ConfigureNteCredentials();
        return;
    }

    auto* dialog = new brls::Dialog(
        "NTE Auto-login\n\nCredentials are stored as plain text at:\n" +
        NteCredentialsPath() +
        "\n\nPress L + X on the first NTE email sign-in screen. B cancels an active sequence.");
    const auto alive = alive_;
    const auto generation = account_generation_;
    dialog->addButton("Edit credentials", [this, alive, generation] {
        if (!alive->load() || AppState::Instance().session_generation() != generation)
            return;
        ConfigureNteCredentials();
    });
    dialog->addButton("Clear credentials", [this, alive, generation] {
        if (!alive->load() || AppState::Instance().session_generation() != generation)
            return;
        ClearNteCredentials();
        UpdateNteButton();
        brls::Application::notify("NTE Auto-login credentials removed");
    });
    dialog->addButton("Cancel", [] {});
    dialog->setCancelable(true);
    dialog->open();
}

void GameDetailView::ShowStoreSelector(bool launch_after_selection)
{
    if (AppState::Instance().session_generation() != account_generation_)
        return;
    if (data_.variants.empty())
    {
        if (launch_after_selection)
            LaunchSelectedVariant();
        return;
    }

    std::vector<std::string> labels;
    labels.reserve(data_.variants.size());
    for (const auto& variant : data_.variants)
        labels.push_back(VariantLabel(variant));

    const auto alive = alive_;
    const auto generation = account_generation_;
    const auto user_id = ActiveUserId();
    auto* dropdown = new brls::Dropdown(
        Tr("Choose game store"), labels,
        [this, alive, generation, user_id](int selected) {
            if (!alive->load() || AppState::Instance().session_generation() != generation)
                return;
            if (selected < 0 || static_cast<size_t>(selected) >= data_.variants.size())
                return;
            selected_variant_index_ = static_cast<size_t>(selected);
            launcher_preference_loaded_ = true;
            const auto& variant = data_.variants[selected_variant_index_];
            data_.launch_app_id = variant.id;
            client_.SaveLauncherPreference(user_id, data_.game_id, variant.id);
            UpdateStoreButton();
            brls::Application::notify("Store selected: " + SafeText(variant.store, variant.id));
        }, static_cast<int>(selected_variant_index_),
        [this, alive, generation, launch_after_selection](int selected) {
            if (!alive->load() || !AppState::Instance().IsCurrentSession(generation))
                return;
            if (!launch_after_selection || selected < 0 ||
                static_cast<size_t>(selected) >= data_.variants.size() ||
                static_cast<size_t>(selected) != selected_variant_index_)
                return;
            DeferredLaunchSelectedVariant(generation, data_.variants[static_cast<size_t>(selected)].id);
        });
    brls::Application::pushActivity(new brls::Activity(dropdown));
}

void GameDetailView::LaunchSelectedVariant()
{
    const std::string launch_app_id = selected_variant_index_ < data_.variants.size()
        ? data_.variants[selected_variant_index_].id : data_.launch_app_id;
    DeferredLaunchSelectedVariant(account_generation_, launch_app_id);
}

void GameDetailView::DeferredLaunchSelectedVariant(std::uint64_t generation,
                                                  const std::string& launch_app_id)
{
    const auto& state = AppState::Instance();
    if (!alive_->load() || !state.IsCurrentSession(generation) || generation != account_generation_)
        return;

    const std::string current_launch_app_id = selected_variant_index_ < data_.variants.size()
        ? data_.variants[selected_variant_index_].id : data_.launch_app_id;
    if (launch_app_id != current_launch_app_id)
        return;

    if (!IsNumericLaunchId(launch_app_id))
    {
        ShowError("Launch Error", "This game has no valid numeric launch App ID.");
        return;
    }

    const std::string store = selected_variant_index_ < data_.variants.size()
        ? data_.variants[selected_variant_index_].store : data_.stores;
    LaunchSessionDialog(client_, *state.session(), launch_app_id,
                        data_.title, store, data_.title, data_.game_id,
                        data_.image_url);
}

void GameDetailView::CreateSwitchShortcut()
{
    shortcut::LaunchRequest request;
    request.launch_app_id = data_.launch_app_id;
    request.store = data_.stores;
    if (selected_variant_index_ < data_.variants.size())
    {
        request.launch_app_id = data_.variants[selected_variant_index_].id;
        request.store = data_.variants[selected_variant_index_].store;
    }
    request.game_id = data_.game_id;
    request.title = data_.title;
    request.image_url = data_.image_url;
    if (!shortcut::IsValid(request))
    {
        ShowError(
            "Shortcut Error",
            "This game does not have a valid numeric GeForce NOW App ID.");
        return;
    }

    brls::Application::notify("Creating shortcut for " + request.title);
    const auto alive = alive_;
    brls::async([request = std::move(request), alive]() mutable {
        shortcut::CreateResult result =
            shortcut::CreateGameShortcut(std::move(request));
        brls::sync([result = std::move(result), alive]() {
            if (!alive->load())
                return;
            if (!result.success)
            {
                ShowError("Shortcut Error", result.error);
                return;
            }

            std::string body =
                "OpenNOW created a lightweight launcher with this game's "
                "title and icon.\n\nInstall it to SD storage now to add it "
                "directly to the Horizon HOME screen?";
            if (!result.used_game_cover)
                body += "\n\nThe OpenNOW icon was used because the game cover "
                    "could not be converted.";

            auto* dialog = new brls::Dialog(body);
            dialog->addButton(
                "Install on HOME",
                [path = result.nro_path, title = result.title] {
                    std::string error;
                    if (!shortcut::StartForwarderInstaller(
                            path, title, error))
                        ShowError("HOME Install Failed", error);
                });
            dialog->addButton("Not now", [] {});
            dialog->setCancelable(true);
            dialog->open();
        });
    }, false);
}

GameDetailData MakeLibraryGameDetail(const GameInfo& game)
{
    GameDetailData data;
    data.title         = game.title;
    data.game_id       = game.uuid.empty() ? game.id : game.uuid;
    data.subtitle      = game_detail::HeaderSubtitle(true);
    data.image_url     = game.image_url;
    data.launch_app_id = game.launch_app_id;
    data.publisher     = game.publisher;
    data.description   = game.description;
    data.stores        = JoinStores(game);
    data.membership_tier_label = game.membership_tier_label;
    data.last_played   = game.last_played;
    data.owned         = true;
    data.variants      = game.variants;
    data.variants.erase(
        std::remove_if(data.variants.begin(), data.variants.end(), [](const GameVariant& variant) {
            return !IsNumericLaunchId(variant.id);
        }),
        data.variants.end());
    return data;
}

GameDetailData MakeCatalogGameDetail(const PublicGame& game)
{
    GameDetailData data;
    data.title         = game.title;
    data.game_id       = game.uuid.empty() ? game.id : game.uuid;
    data.subtitle      = game_detail::HeaderSubtitle(game.is_in_library);
    data.image_url     = game.image_url;
    data.launch_app_id = game.launch_app_id.empty() ? game.id : game.launch_app_id;
    data.publisher     = game.publisher;
    data.stores        = game.store;
    data.membership_tier_label = game.membership_tier_label;
    data.owned         = game.is_in_library;
    data.variants      = game.variants;
    if (data.variants.empty())
    {
        GameVariant variant;
        variant.id = game.id;
        variant.store = game.store;
        data.variants.push_back(std::move(variant));
    }
    return data;
}

} // namespace opennow
