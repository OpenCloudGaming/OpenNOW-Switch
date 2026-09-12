#include "webrtc/decode_recovery_state.hpp"

#include <cassert>

int main()
{
    using State = opennow::webrtc::DecodeRecoveryState;
    using Completion = State::Completion;

    State state;
    assert(state.admit(false));
    assert(!state.take_decoder_reset());
    assert(!state.take_keyframe_request());

    state.require_resync();
    assert(state.waiting_for_idr());
    assert(!state.admit(false));
    assert(state.take_keyframe_request());
    assert(!state.take_keyframe_request());
    state.defer_keyframe_request();
    assert(state.take_keyframe_request());

    const auto old_idr = state.admit(true);
    assert(old_idr);
    assert(!state.waiting_for_idr());
    assert(state.admit(false) == old_idr);
    state.defer_keyframe_request();
    assert(!state.take_keyframe_request());
    assert(state.take_decoder_reset());
    assert(!state.take_decoder_reset());

    state.require_resync();
    assert(!state.is_current(*old_idr));
    assert(state.complete(*old_idr, true) == Completion::Stale);
    assert(state.waiting_for_idr());
    assert(!state.admit(false));
    assert(state.take_keyframe_request());
    assert(state.take_decoder_reset());

    const auto new_idr = state.admit(true);
    assert(new_idr && new_idr != old_idr);
    assert(state.admit(false) == new_idr);
    assert(state.complete(*old_idr, false) == Completion::Stale);
    assert(state.is_current(*new_idr));
    assert(!state.take_decoder_reset());
    assert(!state.take_keyframe_request());
    assert(state.complete(*new_idr, true) == Completion::Accepted);
    assert(state.admit(false) == new_idr);

    assert(state.complete(*new_idr, false) == Completion::Failed);
    assert(!state.is_current(*new_idr));
    assert(state.waiting_for_idr());
    assert(!state.admit(false));
    assert(state.take_decoder_reset());
    assert(state.take_keyframe_request());
    assert(state.complete(*new_idr, true) == Completion::Stale);
    assert(state.waiting_for_idr());

    const auto retry = state.admit(true);
    assert(retry && retry != new_idr);
    assert(state.admit(false) == retry);
    assert(state.complete(*retry, false) == Completion::Failed);
    assert(!state.admit(false));
    assert(state.take_keyframe_request());
}
