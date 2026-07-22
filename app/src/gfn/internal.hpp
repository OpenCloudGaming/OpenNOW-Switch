#pragma once

#include "../gfn_client.hpp"

#include <jansson.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace opennow::gfn::detail
{

using JsonPtr = std::unique_ptr<json_t, decltype(&json_decref)>;

std::string Trim(const std::string& value);
std::int64_t NowMs();
std::string GetAppHome();
void EnsureAppHome();
JsonPtr LoadJson(const std::string& body);
std::string JsonString(json_t* value);
std::string GetString(json_t* object, const char* key);
bool GetBool(json_t* object, const char* key, bool fallback = false);
int GetInteger(json_t* object, const char* key, int fallback = 0);
std::string EnsureTrailingSlash(std::string value);
LoginProvider DefaultProvider();
std::vector<unsigned char> GenerateRandomBytes(size_t length);
std::string HexEncode(const unsigned char* data, size_t length);
std::string GenerateDeviceId();
std::string UrlEncode(const std::string& input, bool plus_for_space = false);
std::string ReadTextFile(const std::string& path);
void WriteTextFileAtomically(const std::string& path, const std::string& content);
void WriteJsonToFile(const std::string& path, json_t* root);
void AppendAuthLog(const std::string& line);
std::string Lowercase(std::string value);
std::string JsonForTrace(const std::string& body);
std::string DumpJson(json_t* value);
std::string ResolveSessionJwt(const AuthSession& session);

} // namespace opennow::gfn::detail
