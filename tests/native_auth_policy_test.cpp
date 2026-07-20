#include "native_auth_policy.hpp"

#include <cassert>

using opennow::auth::ClassifyNativeStage;
using opennow::auth::NativeStageAction;

static_assert(ClassifyNativeStage("EnterPassword") == NativeStageAction::Password);
static_assert(ClassifyNativeStage("NFactorChallengeSelect") == NativeStageAction::SelectMfa);
static_assert(ClassifyNativeStage("NFactorTOTPChallenge") == NativeStageAction::VerifyTotp);
static_assert(ClassifyNativeStage("NFactorEmailAuthWait") == NativeStageAction::WaitForEmail);
static_assert(ClassifyNativeStage("RememberLogIn") == NativeStageAction::Advance);
static_assert(ClassifyNativeStage("Finish") == NativeStageAction::Finish);
static_assert(ClassifyNativeStage("Captcha") == NativeStageAction::Fallback);
static_assert(ClassifyNativeStage("PasskeyChallenge") == NativeStageAction::Fallback);

int main()
{
    assert(ClassifyNativeStage("Consent") == NativeStageAction::Consent);
    assert(ClassifyNativeStage("") == NativeStageAction::Fallback);
    return 0;
}
