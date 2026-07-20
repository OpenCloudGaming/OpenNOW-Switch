#pragma once

#include <string_view>

namespace opennow::auth
{

enum class NativeStageAction
{
    Password,
    SelectMfa,
    VerifyTotp,
    WaitForEmail,
    Consent,
    Advance,
    Finish,
    Fallback,
};

constexpr NativeStageAction ClassifyNativeStage(std::string_view page)
{
    if (page == "EnterPassword" || page == "LogIn")
        return NativeStageAction::Password;
    if (page == "NFactorChallengeSelect")
        return NativeStageAction::SelectMfa;
    if (page == "NFactorTOTPChallenge")
        return NativeStageAction::VerifyTotp;
    if (page == "NFactorEmailAuthWait")
        return NativeStageAction::WaitForEmail;
    if (page == "Consent")
        return NativeStageAction::Consent;
    if (page == "RememberLogIn" || page == "UpdateRememberLogIn")
        return NativeStageAction::Advance;
    if (page == "Finish" || page == "BackToClient" || page == "ErrorBackToClient")
        return NativeStageAction::Finish;
    return NativeStageAction::Fallback;
}

} // namespace opennow::auth
