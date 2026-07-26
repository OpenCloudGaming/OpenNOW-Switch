#pragma once

#include "internal.hpp"

#include "../stream_settings.hpp"

#include <string>
#include <vector>

namespace opennow::gfn::cloud_session
{

void ApplySessionNetworkInfo(json_t* session, SessionInfo& info);
void CollectIceServersFromObject(json_t* object, SessionInfo& info);

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
    const std::string& proxy_url,
    const StreamSettings& stream_settings);
int ParseSessionStatus(json_t* status);

} // namespace opennow::gfn::cloud_session
