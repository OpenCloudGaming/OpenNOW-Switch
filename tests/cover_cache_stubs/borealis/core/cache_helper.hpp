#pragma once

namespace brls
{
class TextureCache
{
  public:
    static TextureCache& instance()
    {
        static TextureCache cache;
        return cache;
    }
    void removeCache(int) { ++releases; }
    int releases = 0;
};
}
