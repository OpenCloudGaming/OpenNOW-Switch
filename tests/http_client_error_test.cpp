#include "http_client.hpp"

#include <cassert>
#include <stdexcept>
#include <string>

namespace
{
int handle;
CURLcode request_result = CURLE_PEER_FAILED_VERIFICATION;
}

#undef curl_easy_setopt
extern "C"
{
CURL* curl_easy_init()
{
    return reinterpret_cast<CURL*>(&handle);
}
void curl_easy_cleanup(CURL*) {}
CURLcode curl_easy_setopt(CURL*, CURLoption, ...)
{
    return CURLE_OK;
}
CURLcode curl_easy_perform(CURL*)
{
    return request_result;
}
}

int main()
{
    for (const auto result : {CURLE_PEER_FAILED_VERIFICATION, CURLE_SSL_CACERT_BADFILE,
             CURLE_COULDNT_CONNECT})
    {
        request_result = result;
        bool failed = false;
        try
        {
            opennow::HttpClient().Get(
                "https://username:password@example.test/private-path?token=private-query#private-fragment",
                "test-client", {}, "http://proxy-user:proxy-password@proxy.test");
        }
        catch (const std::runtime_error& error)
        {
            failed = true;
            const std::string message = error.what();
            assert(message.find("https://example.test") != std::string::npos);
            assert(message.find("username") == std::string::npos);
            assert(message.find("password") == std::string::npos);
            assert(message.find("private-") == std::string::npos);
            assert(message.find("proxy.test") == std::string::npos);
            assert(message.find("configured proxy") != std::string::npos);
            if (result != CURLE_COULDNT_CONNECT)
            {
                assert(message.find("TLS backend:") != std::string::npos);
                assert(message.find("UTC") != std::string::npos);
                assert(message.find("verification remains enabled") != std::string::npos);
            }
        }
        assert(failed);
    }
}
