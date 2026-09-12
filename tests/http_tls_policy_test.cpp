#include "http_client.hpp"

#include <cassert>
#include <cstdarg>
#include <string>
#include <vector>

namespace
{
std::vector<CURLoption> options;
CURLoption rejected_option = CURLOPT_URL;
}

#undef curl_easy_setopt
extern "C" CURLcode curl_easy_setopt(CURL*, CURLoption option, ...)
{
    options.push_back(option);
    if (option == rejected_option)
        return CURLE_UNKNOWN_OPTION;
    va_list args;
    va_start(args, option);
    if (option == CURLOPT_SSL_VERIFYPEER)
        assert(va_arg(args, long) == 1L);
    else if (option == CURLOPT_SSL_VERIFYHOST)
        assert(va_arg(args, long) == 2L);
    else if (option == CURLOPT_CAINFO)
        assert(std::string(va_arg(args, const char*)) == "romfs:/certs/DigiCertGlobalRootG3.pem");
    else
        assert(false);
    va_end(args);
    return CURLE_OK;
}

int main()
{
    assert(opennow::ConfigureHttpTls(nullptr));
#ifdef __SWITCH__
    assert((options == std::vector<CURLoption>{
        CURLOPT_SSL_VERIFYPEER, CURLOPT_SSL_VERIFYHOST, CURLOPT_CAINFO}));
#else
    assert((options == std::vector<CURLoption>{
        CURLOPT_SSL_VERIFYPEER, CURLOPT_SSL_VERIFYHOST}));
#endif
    const auto configured_options = options;
    for (const auto option : configured_options)
    {
        options.clear();
        rejected_option = option;
        assert(!opennow::ConfigureHttpTls(nullptr));
        assert(options.back() == option);
    }
}
