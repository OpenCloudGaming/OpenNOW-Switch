extern "C" {
#include "peer.h"
#include <srtp2/srtp.h>
}

#include <array>
#include <cassert>
#include "webrtc/peer_runtime.hpp"

namespace
{
int sctp_users = 0;
}

extern "C" void sctp_usrsctp_init()
{
    ++sctp_users;
}

extern "C" void sctp_usrsctp_deinit()
{
    --sctp_users;
}

int main()
{
    assert(peer_init() == 0);
    assert(peer_init() == 0);
    assert(sctp_users == 2);
    peer_deinit();
    assert(sctp_users == 1);

    std::array<unsigned char, 30> synthetic_key {};
    srtp_policy_t policy {};
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy.rtp);
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy.rtcp);
    policy.ssrc.type = ssrc_any_outbound;
    policy.key = synthetic_key.data();
    policy.window_size = 128;
    srtp_t stream = nullptr;
    assert(srtp_create(&stream, &policy) == srtp_err_status_ok);
    assert(srtp_dealloc(stream) == srtp_err_status_ok);

    peer_deinit();
    assert(sctp_users == 0);
    assert(peer_init() == 0);
    assert(srtp_create(&stream, &policy) == srtp_err_status_ok);
    assert(srtp_dealloc(stream) == srtp_err_status_ok);
    peer_deinit();
    assert(sctp_users == 0);

    {
        opennow::webrtc::PeerRuntime runtime;
        assert(sctp_users == 1);
    }
    assert(sctp_users == 0);

    struct FailedSession
    {
        opennow::webrtc::PeerRuntime runtime;
        FailedSession() { throw 1; }
    };
    try
    {
        FailedSession session;
    }
    catch (int)
    {
        assert(sctp_users == 0);
    }
}
