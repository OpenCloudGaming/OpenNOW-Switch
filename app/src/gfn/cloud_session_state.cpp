#include "cloud_session_internal.hpp"

#include "../server_location_policy.hpp"
#include "../stream_diagnostics.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace opennow
{
using namespace gfn::detail;

namespace gfn::cloud_session
{
namespace
{

std::string GetSessionTracePath()
{
    return GetAppHome() + "/session_trace.log";
}

} // namespace

std::string SessionTraceHint()
{
    return StreamDiagnosticsEnabled()
        ? "\nDetails: sdmc:/switch/SwitchNOW/session_trace.log"
        : "";
}

void ResetSessionTraceLog(const std::string& reason)
{
    if (!StreamDiagnosticsEnabled())
        return;
    EnsureAppHome();

    std::ofstream stream(GetSessionTracePath(), std::ios::binary | std::ios::trunc);
    if (!stream.is_open())
        return;

    stream << "SwitchNOW session trace\n";
    stream << "reason=" << reason << "\n";
    stream << "timestamp_ms=" << NowMs() << "\n\n";
}

void AppendSessionTraceLog(const std::string& line)
{
    if (!StreamDiagnosticsEnabled())
        return;
    EnsureAppHome();

    std::ofstream stream(GetSessionTracePath(), std::ios::binary | std::ios::app);
    if (!stream.is_open())
        return;

    stream << "[" << NowMs() << "] " << line << '\n';
}

std::string HeadersForTrace(const std::vector<std::string>& headers)
{
    std::ostringstream out;
    for (const std::string& header : headers)
    {
        const std::string lower = Lowercase(header);
        if (lower.rfind("authorization:", 0) == 0)
            out << "Authorization: <redacted>\n";
        else
            out << header << "\n";
    }
    return out.str();
}

namespace
{

std::string IntFieldText(json_t* object, const char* key)
{
    if (!object || !json_is_object(object))
        return "";

    json_t* value = json_object_get(object, key);
    if (json_is_integer(value))
        return std::to_string(json_integer_value(value));

    if (json_is_string(value))
        return JsonString(value);

    return "";
}

std::string CloudMatchErrorDetails(int http_status, json_t* root)
{
    std::ostringstream out;
    out << "HTTP " << http_status;

    json_t* req_status = root ? json_object_get(root, "requestStatus") : nullptr;
    if (req_status)
    {
        const std::string status_code = IntFieldText(req_status, "statusCode");
        const std::string status_description = GetString(req_status, "statusDescription");
        const std::string unified_error = IntFieldText(req_status, "unifiedErrorCode");

        if (!status_code.empty())
            out << ", statusCode=" << status_code;
        if (!status_description.empty())
            out << ", statusDescription=" << status_description;
        if (!unified_error.empty())
            out << ", unifiedErrorCode=" << unified_error;
    }

    json_t* sess = root ? json_object_get(root, "session") : nullptr;
    if (sess)
    {
        const std::string session_id = GetString(sess, "sessionId");
        const std::string session_status = IntFieldText(sess, "status");
        const std::string error_code = IntFieldText(sess, "errorCode");
        const std::string error_description = GetString(sess, "errorDescription");

        if (!session_id.empty())
            out << ", sessionId=" << session_id;
        if (!session_status.empty())
            out << ", sessionStatus=" << session_status;
        if (!error_code.empty())
            out << ", sessionErrorCode=" << error_code;
        if (!error_description.empty())
            out << ", sessionErrorDescription=" << error_description;
    }

    return out.str();
}

} // namespace

std::string BuildCloudMatchErrorMessage(const std::string& stage, int http_status, json_t* root)
{
    const std::string details = CloudMatchErrorDetails(http_status, root);
    std::string message = stage + " failed: " + details;
    json_t* request_status = root ? json_object_get(root, "requestStatus") : nullptr;
    if (request_status && GetInteger(request_status, "statusCode") == 81) {
        message += "\nThis game is not available with the current GeForce NOW membership. "
                   "If the game page says Premium, upgrade the membership to play it.";
    }
    return message + SessionTraceHint();
}

bool IsAppPatchingResponse(json_t* root)
{
    json_t* request_status = root ? json_object_get(root, "requestStatus") : nullptr;
    if (!request_status || GetInteger(request_status, "statusCode") != 41)
        return false;

    return GetString(request_status, "statusDescription").find("APP_PATCHING_STATUS") != std::string::npos;
}

void ApplySeatSetupInfo(json_t* sess, SessionInfo& info)
{
    json_t* seat_setup = sess ? json_object_get(sess, "seatSetupInfo") : nullptr;
    if (!seat_setup || !json_is_object(seat_setup))
        return;

    json_t* queue_position = json_object_get(seat_setup, "queuePosition");
    if (queue_position && json_is_integer(queue_position))
        info.queue_position = json_integer_value(queue_position);
}

namespace
{

std::string GetActiveCloudSessionPath()
{
    return GetAppHome() + "/active_cloud_session.json";
}

} // namespace

void RememberActiveCloudSession(
    const AuthSession& auth,
    const std::string& session_id,
    const std::string& launch_app_id,
    const std::string& streaming_base_url)
{
    if (session_id.empty())
        return;

    try
    {
        JsonPtr root(json_object(), &json_decref);
        json_object_set_new(root.get(), "session_id", json_string(session_id.c_str()));
        json_object_set_new(root.get(), "user_id", json_string(auth.user.user_id.c_str()));
        json_object_set_new(root.get(), "launch_app_id", json_string(launch_app_id.c_str()));
        json_object_set_new(
            root.get(), "streaming_base_url", json_string(streaming_base_url.c_str()));
        json_object_set_new(root.get(), "created_at_ms", json_integer(NowMs()));
        WriteJsonToFile(GetActiveCloudSessionPath(), root.get());
    }
    catch (const std::exception& e)
    {
        AppendAuthLog("session: unable to persist active session error=" + std::string(e.what()));
    }
}

std::string LoadActiveCloudSessionStreamingBaseUrl(
    const AuthSession& auth,
    const std::string& session_id)
{
    const std::string body = ReadTextFile(GetActiveCloudSessionPath());
    if (body.empty())
        return {};

    try
    {
        JsonPtr root = LoadJson(body);
        if (GetString(root.get(), "user_id") != auth.user.user_id ||
            GetString(root.get(), "session_id") != session_id)
            return {};
        return server_location::NormalizeStreamingBaseUrl(
            GetString(root.get(), "streaming_base_url"));
    }
    catch (...)
    {
        return {};
    }
}

std::string ResolveConfiguredStreamingBaseUrl(const AuthSession& session)
{
    const StreamSettings settings = LoadStreamSettings();
    const std::string base_url = server_location::ResolveStreamingBaseUrl(
        settings.region, session.provider.streaming_service_url);
    if (base_url.empty())
        throw std::runtime_error("The selected GeForce NOW server location is invalid.");
    return base_url;
}

std::string LoadActiveCloudSessionId(const AuthSession& auth)
{
    const std::string body = ReadTextFile(GetActiveCloudSessionPath());
    if (body.empty())
        return "";

    try
    {
        JsonPtr root = LoadJson(body);
        if (GetString(root.get(), "user_id") != auth.user.user_id)
            return "";
        return GetString(root.get(), "session_id");
    }
    catch (...)
    {
        return "";
    }
}

void ForgetActiveCloudSession(const std::string& session_id)
{
    const std::string body = ReadTextFile(GetActiveCloudSessionPath());
    if (!body.empty())
    {
        try
        {
            JsonPtr root = LoadJson(body);
            if (!session_id.empty() && GetString(root.get(), "session_id") != session_id)
                return;
        }
        catch (...) {}
    }

    std::remove(GetActiveCloudSessionPath().c_str());
    std::remove((GetActiveCloudSessionPath() + ".bak").c_str());
}
std::string SessionInfoTraceSummary(const SessionInfo& info)
{
    std::ostringstream out;
    out << "sessionId=" << info.session_id
        << " status=" << info.status
        << " queuePosition=" << info.queue_position
        << " appPatching=" << (info.app_patching ? 1 : 0)
        << " serverIp=" << info.server_ip
        << " signalingUrl=" << info.signaling_url
        << " media=" << info.media_ip << ":" << info.media_port
        << " iceServers=" << info.ice_servers.size()
        << " sessionToken=" << (info.session_token.empty() ? "missing" : "present");
    return out.str();
}


} // namespace gfn::cloud_session
} // namespace opennow
