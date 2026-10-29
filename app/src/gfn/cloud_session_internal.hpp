#pragma once

#include "internal.hpp"

#include "../stream_settings.hpp"

#include <string>
#include <vector>

namespace opennow::gfn::cloud_session
{

inline constexpr const char* kCloudMatchClientVersion = "2.0.89.154";
inline constexpr const char* kCloudMatchUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/152.0.0.0 Safari/537.36";

void ApplySessionNetworkInfo(json_t* session, SessionInfo& info);
void CollectIceServersFromObject(json_t* object, SessionInfo& info);
int WireInteger(json_t* value, int fallback = -1);
SessionInfo ParseSessionResponse(const HttpResponse& response, const std::string& stage,
                                const std::string& expected_id, const std::string& base_url);
std::string ResolveSessionControlBaseUrl(json_t* session, const std::string& fallback);
bool CloudMatchResponseSucceeded(const HttpResponse& response);
std::string BuildQueueAdCancellationBody(const std::string& id);

void ResetSessionTraceLog(const std::string& reason);
void AppendSessionTraceLog(const std::string& line);
std::string HeadersForTrace(const std::vector<std::string>& headers);
std::string SessionTraceHint();
std::string BuildCloudMatchErrorMessage(
    const std::string& stage, int http_status, json_t* root);
bool IsAppPatchingResponse(json_t* root);
void ApplySeatSetupInfo(json_t* session, SessionInfo& info);
std::string SessionInfoTraceSummary(const SessionInfo& info);

void RememberActiveCloudSession(
    const AuthSession& auth,
    const std::string& session_id,
    const std::string& launch_app_id,
    const std::string& streaming_base_url);
std::string LoadActiveCloudSessionStreamingBaseUrl(
    const AuthSession& auth, const std::string& session_id);
void UpdateActiveCloudSessionStreamingBaseUrl(
    const AuthSession& auth, const std::string& session_id, const std::string& base_url);
std::string ResolveConfiguredStreamingBaseUrl(const AuthSession& session);
std::string LoadActiveCloudSessionId(const AuthSession& auth);
void ForgetActiveCloudSession(const std::string& session_id);

std::string BuildSessionBody(
    const std::string& app_id,
    const std::string& internal_title,
    const std::string& device_id,
    const std::string& sub_session_id,
    const std::string& network_test_session_id,
    const StreamSettings& stream_settings);
std::string CreateNetworkTestSession(
    const HttpClient& http_client,
    const std::string& streaming_base_url,
    const std::vector<std::string>& headers,
    const StreamSettings& stream_settings);
int ParseSessionStatus(json_t* status);

} // namespace opennow::gfn::cloud_session
