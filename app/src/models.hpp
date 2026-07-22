#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace opennow
{

struct LoginProvider
{
    std::string idp_id;
    std::string code;
    std::string display_name;
    std::string streaming_service_url;
    int priority = 0;
};

struct AuthTokens
{
    std::string access_token;
    std::string refresh_token;
    std::string id_token;
    std::string client_token;
    std::string auth_client_id;
    std::int64_t expires_at_ms              = 0;
    std::int64_t client_token_expires_at_ms = 0;
};

struct AuthUser
{
    std::string user_id;
    std::string display_name;
    std::string email;
    std::string avatar_url;
    std::string membership_tier;
    bool membership_tier_verified = false;
};

struct SubscriptionInfo
{
    std::string membership_tier;
    std::string subscription_type;
    std::string subscription_subtype;
    double allotted_hours    = 0.0;
    double purchased_hours   = 0.0;
    double rolled_over_hours = 0.0;
    double used_hours        = 0.0;
    double remaining_hours   = 0.0;
    double total_hours       = 0.0;
    bool is_unlimited        = false;
    bool available           = false;

    bool has_storage         = false;
    bool has_storage_usage   = false;
    double storage_size_gb   = 0.0;
    double storage_used_gb   = 0.0;
    std::string storage_region_name;
    std::string storage_region_code;
};

struct QrLoginChallenge
{
    std::string user_code;
    std::string verification_uri;
    std::string verification_uri_complete;
    std::int64_t expires_at_ms = 0;
    int interval_seconds = 5;
};

struct AuthSession
{
    LoginProvider provider;
    AuthTokens tokens;
    AuthUser user;
    SubscriptionInfo subscription;
    std::int64_t last_refresh_at_ms = 0;
    std::int64_t membership_checked_at_ms = 0;
    bool persistence_enabled = true; // Runtime-only; vault sessions are persistent.
    bool reauthentication_required = false; // Runtime-only; set after invalid_grant/401 refresh failure.
};

struct GameVariant
{
    std::string id;
    std::string store;
    std::string library_status;
    std::string last_played_date;
    std::string gfn_status;
    bool library_selected = false;
};

struct PublicGame
{
    std::string id;
    std::string uuid;
    std::string launch_app_id;
    std::string title;
    std::string store;
    std::string publisher;
    std::string image_url;
    std::string membership_tier_label;
    bool is_in_library = false;
    std::vector<GameVariant> variants;
};

struct GameInfo
{
    std::string id;
    std::string uuid;
    std::string launch_app_id;
    std::string title;
    std::string description;
    std::string publisher;
    std::string image_url;
    std::string membership_tier_label;
    std::string last_played;
    bool is_in_library          = false;
    size_t selected_variant_index = 0;
    std::vector<std::string> available_stores;
    std::vector<GameVariant> variants;
};

struct IceServerInfo
{
    std::string url;
    std::string username;
    std::string credential;
};

struct SessionInfo
{
    std::string session_id;
    int status = 0;
    int queue_position = 0;
    bool app_patching = false;
    
    // WebRTC / GFN Streaming details
    std::string session_token;
    std::string server_ip;
    std::string signaling_url;
    std::string media_ip;
    int media_port = 0;
    std::vector<IceServerInfo> ice_servers;
};

} // namespace opennow
