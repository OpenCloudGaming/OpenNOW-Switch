#include "game_detail_view.hpp"

#include "app_state.hpp"
#include "cover_image_cache.hpp"
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

brls::Label* MakeLabel(const std::string& text, float size, NVGcolor color, float bottom_margin = 10.0f)
{
    auto* label = new brls::Label();
    label->setText(Tr(text));
    label->setFontSize(size);
    label->setTextColor(color);
    label->setMarginBottom(bottom_margin);
    return label;
}

std::string PrimaryStore(const GameInfo& game)
{
    if (!game.available_stores.empty())
        return game.available_stores.front();

    if (!game.publisher.empty())
        return game.publisher;

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
        stream << game.available_stores[i];
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

    setPadding(28, 40, 28, 40);

    auto* header = new brls::Header();
    header->setTitle(data_.title);
    header->setSubtitle(data_.subtitle);
    addView(header);

    auto* content = new brls::Box(brls::Axis::ROW);
    content->setGrow(1.0f);
    content->setMarginTop(16);

    auto* poster_column = new brls::Box(brls::Axis::COLUMN);
    poster_column->setWidth(350);
    poster_column->setMarginRight(32);

    auto* poster = new brls::Image();
    poster->setWidth(330);
    poster->setHeight(470);
    poster->setCornerRadius(12);
    poster->setScalingType(brls::ImageScalingType::FILL);
    poster->setMarginBottom(16);
    SetCachedCoverImage(poster, data_.image_url);
    poster_column->addView(poster);

    auto* play_button = new brls::Button();
    play_button->setStyle(&brls::BUTTONSTYLE_PRIMARY);
    play_button->setText(Tr(data_.owned ? "Play on GeForce NOW" : "Play from Store"));
    play_button->registerClickAction([this](brls::View* view) {
        (void)view;
        Play();
        return true;
    });
    poster_column->addView(play_button);

    store_button_ = new brls::Button();
    store_button_->setMarginTop(10);
    store_button_->registerClickAction([this](brls::View*) {
        ShowStoreSelector(false);
        return true;
    });
    UpdateStoreButton();
    poster_column->addView(store_button_);

    content->addView(poster_column);

    auto* info_column = new brls::Box(brls::Axis::COLUMN);
    info_column->setGrow(1.0f);

    info_column->addView(MakeLabel(data_.title, 34.0f, nvgRGB(245, 246, 248), 8.0f));
    info_column->addView(MakeLabel(Tr(data_.owned ? "In your library" : "Supported in GeForce NOW"), 20.0f, nvgRGB(88, 230, 146), 22.0f));
    info_column->addView(MakeLabel("Stores: " + SafeText(data_.stores, "Unknown"), 19.0f, nvgRGB(210, 214, 220)));
    if (!data_.membership_tier_label.empty())
    {
        info_column->addView(MakeLabel(
            "Membership: " + data_.membership_tier_label,
            19.0f,
            nvgRGB(246, 196, 80)));
    }
    info_column->addView(MakeLabel("Publisher: " + SafeText(data_.publisher, "Unknown"), 19.0f, nvgRGB(210, 214, 220)));
    info_column->addView(MakeLabel("Last played: " + SafeText(data_.last_played, "Never"), 19.0f, nvgRGB(210, 214, 220)));
    info_column->addView(MakeLabel("Launch App ID: " + SafeText(data_.launch_app_id, "Unavailable"), 17.0f, nvgRGB(140, 148, 158), 24.0f));

    if (IsNevernessToEverness(data_.title))
    {
        nte_button_ = new brls::Button();
        nte_button_->setMarginBottom(18);
        nte_button_->registerClickAction([this](brls::View*) {
            OpenNteCredentialsMenu();
            return true;
        });
        UpdateNteButton();
        info_column->addView(nte_button_);
    }

    auto* description = MakeLabel(
        SafeText(data_.description, "No description is available yet for this title."),
        20.0f,
        nvgRGB(232, 235, 240),
        0.0f);
    description->setSingleLine(false);

    auto* description_frame = new brls::ScrollingFrame();
    description_frame->setGrow(1.0f);
    description_frame->setContentView(description);
    info_column->addView(description_frame);

    content->addView(info_column);
    addView(content);

    registerAction("Back", brls::BUTTON_B, [](brls::View* view) {
        (void)view;
        brls::Application::popActivity();
        return true;
    });
}

void GameDetailView::Play()
{
    const auto& session = AppState::Instance().session();
    if (!session)
    {
        ShowError("Not Logged In", "Sign in from Library before starting a GeForce NOW session.");
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
        ? SafeText(data_.variants[selected_variant_index_].store, "Unknown")
        : SafeText(data_.stores, "Unknown");
    store_button_->setText(Tr("Store") + ": " + store + (data_.variants.size() > 1 ? " (" + Tr("change") + ")" : ""));
}

void GameDetailView::UpdateNteButton()
{
    if (!nte_button_)
        return;
    nte_button_->setText(
        LoadNteCredentials().valid()
            ? "NTE Auto-login: Ready (L + X in game)"
            : "NTE Auto-login: Set email and password");
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
    dialog->addButton("Edit credentials", [this] { ConfigureNteCredentials(); });
    dialog->addButton("Clear credentials", [this] {
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
    if (data_.variants.empty())
    {
        LaunchSelectedVariant();
        return;
    }

    std::vector<std::string> labels;
    labels.reserve(data_.variants.size());
    for (const auto& variant : data_.variants)
        labels.push_back(VariantLabel(variant));

    auto* dropdown = new brls::Dropdown(
        Tr("Choose game store"), labels,
        [this](int selected) {
            if (selected < 0 || static_cast<size_t>(selected) >= data_.variants.size())
                return;
            selected_variant_index_ = static_cast<size_t>(selected);
            launcher_preference_loaded_ = true;
            const auto& variant = data_.variants[selected_variant_index_];
            data_.launch_app_id = variant.id;
            client_.SaveLauncherPreference(ActiveUserId(), data_.game_id, variant.id);
            UpdateStoreButton();
            brls::Application::notify("Store selected: " + SafeText(variant.store, variant.id));
        }, static_cast<int>(selected_variant_index_),
        [this, launch_after_selection](int selected) {
            if (launch_after_selection && selected >= 0)
                LaunchSelectedVariant();
        });
    brls::Application::pushActivity(new brls::Activity(dropdown));
}

void GameDetailView::LaunchSelectedVariant()
{
    std::string launch_app_id = data_.launch_app_id;
    std::string store = data_.stores;
    if (selected_variant_index_ < data_.variants.size())
    {
        launch_app_id = data_.variants[selected_variant_index_].id;
        store = data_.variants[selected_variant_index_].store;
    }

    if (launch_app_id.empty())
    {
        ShowError("Launch Error", "This game has no launch App ID.");
        return;
    }

    LaunchSessionDialog(client_, *AppState::Instance().session(), launch_app_id,
                        data_.title, store, data_.title, data_.game_id,
                        data_.image_url);
}

GameDetailData MakeLibraryGameDetail(const GameInfo& game)
{
    GameDetailData data;
    data.title         = game.title;
    data.game_id       = game.uuid.empty() ? game.id : game.uuid;
    data.subtitle      = PrimaryStore(game) + " library title";
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
    data.subtitle      = game.store + (game.is_in_library ? " library title" : " catalog title");
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
