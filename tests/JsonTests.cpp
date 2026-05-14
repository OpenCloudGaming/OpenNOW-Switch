#include "opennow/util/Json.hpp"

#include <cassert>

int main() {
    const auto parsed = opennow::util::parseJson(R"({
        "name": "OpenNOW",
        "count": 3,
        "enabled": true,
        "items": [{"id": "a"}, {"id": "b"}],
        "escaped": "line\nnext",
        "unicode": "\u0041"
    })");

    assert(parsed.ok);
    assert(parsed.value.get("name")->asString() == "OpenNOW");
    assert(parsed.value.get("count")->asNumber() == 3);
    assert(parsed.value.get("enabled")->asBool() == true);
    assert(parsed.value.get("items")->at(1)->get("id")->asString() == "b");
    assert(parsed.value.get("escaped")->asString() == "line\nnext");
    assert(parsed.value.get("unicode")->asString() == "A");

    const auto bad = opennow::util::parseJson(R"({"broken":)");
    assert(!bad.ok);
    assert(!bad.error.empty());

    return 0;
}
