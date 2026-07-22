#include "ui_text_policy.hpp"

#include <cassert>

int main()
{
    using namespace opennow::ui;

    assert(Utf8CodePointCount("Search") == 6);
    assert(Utf8CodePointCount("搜索") == 2);
    assert(ToolbarButtonWidth("Search") == 170.0f);
    assert(ToolbarButtonWidth("This label is intentionally much too long for a toolbar") == 250.0f);
    assert(ToolbarButtonWidth("搜索") == 170.0f);
    return 0;
}
