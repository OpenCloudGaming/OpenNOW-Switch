#include "home_shortcut_policy.hpp"

#include <cassert>
#include <string>
#include <vector>

int main()
{
    using namespace opennow::shortcut;

    LaunchRequest request;
    request.launch_app_id = "123456";
    request.game_id = "game/uuid";
    request.title = "Tom Clancy's Game = Deluxe";
    request.store = "Steam & Family";
    request.image_url = "https://example.test/cover image.jpg?a=1&b=2";
    request.executable_path = "sdmc:/switch/SwitchNOW/SwitchNOW.nro";

    const std::string manifest = Serialize(request);
    const auto parsed = Parse(manifest);
    assert(parsed);
    assert(parsed->launch_app_id == request.launch_app_id);
    assert(parsed->game_id == request.game_id);
    assert(parsed->title == request.title);
    assert(parsed->store == request.store);
    assert(parsed->image_url == request.image_url);
    assert(parsed->executable_path == request.executable_path);

    assert(!Parse("OPENNOW_SHORTCUT_V1\nlaunch_app_id=not-numeric\ntitle=Bad\n"));
    assert(!Parse("OPENNOW_SHORTCUT_V1\nlaunch_app_id=123\ntitle=Bad%QZ\n"));
    assert(!Parse(std::string(17 * 1024, 'x')));

    const auto arguments = ParseArguments({
        "--launch-app-id=778899",
        "--game-id=game-id",
        "--title=Control",
        "--store=Steam",
    });
    assert(arguments);
    assert(arguments->launch_app_id == "778899");
    assert(arguments->title == "Control");
    assert(!ParseArguments({"--launch-app-id=abc", "--title=Bad"}));
    assert(!ParseArguments({
        "--launch-app-id=123456789012345678901234567890123",
        "--title=Too long",
    }));

    assert(SafeFileStem("Neverness: To Everness?", "game") == "Neverness To Everness");
    assert(SafeFileStem("  A   Game.  ", "game") == "A Game");
    assert(SafeFileStem("***", "game") == "game");
    const std::string unicode_title =
        "界界界界界界界界界界界界界界界界界界界界界界";
    const std::string unicode_stem = SafeFileStem(unicode_title, "game");
    assert(unicode_stem.size() <= 64);
    assert(unicode_stem.size() % 3 == 0);
    return 0;
}
