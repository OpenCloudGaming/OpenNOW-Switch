#include "avatar_utils.hpp"
#include "cover_image_cache.hpp"
#include "home_shortcut.hpp"
#include "nte_credentials.hpp"
#include "ui_helpers.hpp"

namespace opennow
{

CachedImage::~CachedImage() = default;
void CachedImage::draw(NVGcontext* vg, float x, float y, float width, float height,
                       brls::Style style, brls::FrameContext* ctx)
{
    brls::Image::draw(vg, x, y, width, height, style, ctx);
}
void SetCachedCoverImage(CachedImage* image, const std::string&)
{
    image->setImageFromRes("img/opennow-logo-mark.png");
}
void SetCachedAvatarImage(CachedImage*, const std::string&) {}
std::string ResolveAvatarUrl(const AuthUser&) { return "fixture-avatar"; }
int GetCurrentQueuePosition() { return 123; }
bool IsQueueMinimized() { return true; }
void RestoreMinimizedQueueDialog() {}
GfnClient::GfnClient() = default;
std::string GfnClient::LoadLauncherPreference(const std::string&, const std::string&) const { return {}; }
void GfnClient::SaveLauncherPreference(const std::string&, const std::string&, const std::string&) const {}
void ShowError(const std::string&, const std::string&) {}
void LaunchSessionDialog(const GfnClient&, const AuthSession&, const std::string&, const std::string&,
                         const std::string&, const std::string&, const std::string&, const std::string&) {}
bool IsNevernessToEverness(const std::string&) { return false; }
bool NteCredentials::valid() const { return false; }
NteCredentials LoadNteCredentials() { return {}; }
bool SaveNteCredentials(const NteCredentials&) { return false; }
bool ClearNteCredentials() { return true; }
std::string NteCredentialsPath() { return {}; }

namespace shortcut
{
CreateResult CreateGameShortcut(LaunchRequest) { return {}; }
bool StartForwarderInstaller(const std::string&, const std::string&, std::string&) { return false; }
}

}
