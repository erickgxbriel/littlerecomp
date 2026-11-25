#pragma once

#include <string>

namespace i18n {

enum class Lang : int {
    Auto = 0,
    En = 1,
    PtBr = 2,
};

enum class Str : int {
    LangLabel = 0,
    LangAuto,
    LangEn,
    LangPtBr,

    // Setup Wizard (Onboarding BYOG)
    WizardTitle,
    WizardSubtitle,
    WizardStep1Title,
    WizardStep1Desc,
    WizardStep2Title,
    WizardStep2Desc,
    WizardStep3Title,
    WizardStep3Desc,
    BtnSelectIso,
    BtnSelectFolder,
    BtnGrantAllFiles,
    BtnRetryBoot,
    BtnOptions,
    WizardStatusLabel,
    WizardSelectedFile,
    WizardHardwareLabel,
    WizardCreditsFooter,

    // Boot / BYOG status & errors
    StatusWaitingByog,
    StatusLoadingGame,
    StatusBootingGuest,
    StatusRunningBeta,
    ErrNonSeekableUri,
    ErrNotXboxIso,
    ErrPermissionNeeded,
    ErrMissingXbeInIso,
    ErrMissingXbeInDir,
    WarnSha1Mismatch,

    // HUD
    HudGpu,
    HudBat,
    ThermalOk,
    ThermalWarm,
    ThermalHot,
    ThermalCrit,
    HudCompactFmt,

    // Options Tabs & Labels
    OptionsTitle,
    TabGraphics,
    TabControls,
    TabGameLang,
    TabCredits,

    LblScale,
    LblScale10,
    LblScale15,
    LblScale20,
    LblUpscaler,
    LblExperimental60Fps,
    LblNativeAuxRt,
    LblNativeAuxRtHint,
    LblThermalDrs,
    LblThermalDrsHint,
    LblPinBigCores,
    LblHudMode,
    HudModeOff,
    HudModeCompact,
    HudModeFull,

    LblTouchControls,
    LblTouchOpacity,
    LblModernPad,
    LblPhysicalGamepad,
    GamepadConnected,
    GamepadDisconnected,

    LblUiLanguage,
    LblSkipIntro,
    LblUnlockCheats,
    LblCurrentGamePath,
    LblApplyPathRetry,
    LblSaveSettings,
    LblResume,

    // Credits Tab
    CreditsHeading,
    CreditsBody,
    CreditsTechInfo,

    Count
};

void SetLanguage(Lang setting, const std::string& system_lang_code);
Lang GetLanguageSetting();
bool IsPtBr();
const char* T(Str id);

}  // namespace i18n
