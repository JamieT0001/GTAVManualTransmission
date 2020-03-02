#include "script.h"

#include <string>
#include <algorithm>
#include <thread>
#include <mutex>
#include <filesystem>

#include <inc/natives.h>
#include <inc/enums.h>
#include <inc/main.h>
#include <inc/types.h>

#include <menu.h>
#include <fmt/format.h>

#include "ScriptSettings.hpp"
#include "VehicleData.hpp"

#include "Memory/MemoryPatcher.hpp"
#include "Memory/Offsets.hpp"
#include "Memory/VehicleFlags.h"

#include "Input/CarControls.hpp"

#include "Util/Logger.hpp"
#include "Util/Paths.h"
#include "Util/MathExt.h"
#include "Util/Files.h"
#include "Util/UIUtils.h"
#include "Util/Timer.h"
#include "Util/ValueTimer.h"
#include "Util/GameSound.h"

#include "UpdateChecker.h"
#include "Constants.h"
#include "Compatibility.h"
#include "CustomSteering.h"
#include "WheelInput.h"
#include "ScriptUtils.h"
#include "VehicleConfig.h"

namespace fs = std::filesystem;

ReleaseInfo g_releaseInfo;
std::mutex g_releaseInfoMutex;

bool g_notifyUpdate;
std::mutex g_notifyUpdateMutex;

bool g_checkUpdateDone;
std::mutex g_checkUpdateDoneMutex;

int g_textureWheelId;
int g_textureAbsId;
int g_textureTcsId;
int g_textureEspId;
int g_textureBrkId;

NativeMenu::Menu g_menu;
CarControls g_controls;
ScriptSettings g_settings;

Player g_player;
Ped g_playerPed;
Vehicle g_playerVehicle;
Vehicle g_lastPlayerVehicle;

VehiclePeripherals g_peripherals;
VehicleGearboxStates g_gearStates;
WheelPatchStates g_wheelPatchStates;

VehicleExtensions g_ext;
VehicleData g_vehData(g_ext);

std::vector<VehicleConfig> g_vehConfigs;
VehicleConfig* g_activeConfig;

bool g_focused;
Timer g_wheelInitDelayTimer(0);

std::vector<ValueTimer<float>> g_speedTimers;

GameSound g_gearRattle("DAMAGED_TRUCK_IDLE", "", "");

void updateShifting();
void blockButtons();
void startStopEngine();
void functionAutoReverse();
void functionRealReverse();
void updateLastInputDevice();
void updateSteeringMultiplier();

///////////////////////////////////////////////////////////////////////////////
//                           Mod functions: Shifting
///////////////////////////////////////////////////////////////////////////////

void shiftTo(int gear, bool autoClutch);
void functionHShiftTo(int i);
void functionHShiftKeyboard();
void functionHShiftWheel();
void functionSShift();
void functionAShift();

///////////////////////////////////////////////////////////////////////////////
//                   Mod functions: Gearbox features
///////////////////////////////////////////////////////////////////////////////

void functionClutchCatch();
void functionEngStall();
void functionEngDamage();
void functionEngBrake();
void functionEngLock();

///////////////////////////////////////////////////////////////////////////////
//                       Mod functions: Gearbox control
///////////////////////////////////////////////////////////////////////////////

void handleBrakePatch();
void handleRPM();
void functionLimiter();

///////////////////////////////////////////////////////////////////////////////
//                             Misc features
///////////////////////////////////////////////////////////////////////////////

void functionAutoLookback();
void functionAutoGear1();
void functionHillGravity();

void setVehicleConfig(Vehicle vehicle) {
    g_activeConfig = nullptr;
    g_settings.SetVehicleConfig(nullptr);
    if (!g_settings.MTOptions.Override)
        return;

    if (ENTITY::DOES_ENTITY_EXIST(vehicle)) {
        auto currModel = ENTITY::GET_ENTITY_MODEL(vehicle);
        auto it = std::find_if(g_vehConfigs.begin(), g_vehConfigs.end(),
            [currModel, vehicle](const VehicleConfig & config) {
                auto modelsIt = std::find_if(config.ModelNames.begin(), config.ModelNames.end(),
                    [currModel](const std::string & modelName) {
                        return joaat(modelName.c_str()) == currModel;
                    });
                auto platesIt = std::find_if(config.Plates.begin(), config.Plates.end(),
                    [vehicle](const std::string & plate) {
                        return StrUtil::toLower(plate) == StrUtil::toLower(VEHICLE::GET_VEHICLE_NUMBER_PLATE_TEXT(vehicle));
                    });
                if (platesIt != config.Plates.end())
                    return true;
                if (modelsIt != config.ModelNames.end())
                    return true;
                return false;
            });
        if (it != g_vehConfigs.end()) {
            g_activeConfig = &*it;
            g_settings.SetVehicleConfig(g_activeConfig);
            UI::Notify(INFO, fmt::format("Config [{}] loaded.", g_activeConfig->Name));
        }
    }
}

void update_player() {
    g_player = PLAYER::PLAYER_ID();
    g_playerPed = PLAYER::PLAYER_PED_ID();
}

void update_vehicle() {
    g_playerVehicle = PED::GET_VEHICLE_PED_IS_IN(g_playerPed, false);
    // Reset vehicle stats on vehicle change (or leave)
    if (g_playerVehicle != g_lastPlayerVehicle) {
        g_peripherals = VehiclePeripherals();
        g_gearStates = VehicleGearboxStates();
        g_wheelPatchStates = WheelPatchStates();
        g_vehData.SetVehicle(g_playerVehicle); // assign new vehicle;
        functionHidePlayerInFPV(true);
        setVehicleConfig(g_playerVehicle);
        g_gearRattle.Stop();
    }
    if (Util::VehicleAvailable(g_playerVehicle, g_playerPed)) {
        g_vehData.Update(); // Update before doing anything else
    }
    if (g_playerVehicle != g_lastPlayerVehicle && Util::VehicleAvailable(g_playerVehicle, g_playerPed)) {
        if (g_vehData.mGearTop == 1 || g_vehData.mFlags[1] & FLAG_IS_ELECTRIC)
            g_gearStates.FakeNeutral = false;
        else
            g_gearStates.FakeNeutral = g_settings.GameAssists.DefaultNeutral;
    }

    if (g_settings.Debug.Metrics.EnableTimers && ENTITY::DOES_ENTITY_EXIST(g_playerVehicle)) {
        for(auto& valueTimer : g_speedTimers) {
            float speed;
            switch(joaat(valueTimer.mUnit.c_str())) {
                case (joaat("kph")):
                    speed = g_vehData.mVelocity.y * 3.6f;
                    break;
                case (joaat("mph")):
                    speed = g_vehData.mVelocity.y / 0.44704f;
                    break;
                default:
                    speed = g_vehData.mVelocity.y;
            }
            valueTimer.Update(speed);
        }
    }
    
    g_lastPlayerVehicle = g_playerVehicle;
}

// Read inputs
void update_inputs() {
    if (g_focused != SysUtil::IsWindowFocused()) {
        // no focus -> focus
        if (!g_focused) {
            logger.Write(DEBUG, "[Wheel] Window focus gained: re-initializing FFB");
            g_wheelInitDelayTimer.Reset(100);
        }
        else {
            logger.Write(DEBUG, "[Wheel] Window focus lost");
        }
    }
    g_focused = SysUtil::IsWindowFocused();

    if (g_wheelInitDelayTimer.Expired() && g_wheelInitDelayTimer.Period() > 0) {
        initWheel();
        g_wheelInitDelayTimer.Reset(0);
    }

    updateLastInputDevice();
    g_controls.UpdateValues(g_controls.PrevInput, false);
}

void update_hud() {
    if (!Util::PlayerAvailable(g_player, g_playerPed) || !Util::VehicleAvailable(g_playerVehicle, g_playerPed)) {
        return;
    }

    if (g_settings.Debug.DisplayInfo) {
        drawDebugInfo();
    }
    if (g_settings.Debug.DisplayWheelInfo) {
        drawVehicleWheelInfo();
    }
    if (g_settings.Debug.Metrics.GForce.Enable) {
        drawGForces();
    }
    if (g_settings.HUD.Enable && g_vehData.mDomain == VehicleDomain::Road &&
        (g_settings.MTOptions.Enable || g_settings.HUD.Always)) {
        drawHUD();
    }
    if (g_settings.HUD.Enable &&
        (g_vehData.mDomain == VehicleDomain::Road || g_vehData.mDomain == VehicleDomain::Water) &&
        (g_controls.PrevInput == CarControls::Wheel || g_settings.HUD.Wheel.Always) &&
        g_settings.HUD.Wheel.Enable && g_textureWheelId != -1) {
        drawInputWheelInfo();
    }
    if (g_settings.HUD.Enable && g_vehData.mDomain == VehicleDomain::Road &&
        g_settings.HUD.DashIndicators.Enable) {
        drawWarningLights();
    }

    if (g_settings.Debug.DisplayFFBInfo) {
        WheelInput::DrawDebugLines();
    }
}

void wheelControlWater() {
    WheelInput::CheckButtons();
    WheelInput::HandlePedalsArcade(g_controls.ThrottleVal, g_controls.BrakeVal);
    WheelInput::DoSteering();
    WheelInput::PlayFFBWater();
}

void wheelControlRoad() {
    WheelInput::CheckButtons();
    if (g_settings.MTOptions.Enable && 
        !(g_vehData.mClass == VehicleClass::Bike && g_settings.GameAssists.SimpleBike)) {
        WheelInput::HandlePedals(g_controls.ThrottleVal, g_controls.BrakeVal);
    }
    else {
        WheelInput::HandlePedalsArcade(g_controls.ThrottleVal, g_controls.BrakeVal);
    }
    WheelInput::DoSteering();
    if (g_vehData.mIsAmphibious && ENTITY::GET_ENTITY_SUBMERGED_LEVEL(g_playerVehicle) > 0.10f) {
        WheelInput::PlayFFBWater();
    }
    else {
        WheelInput::PlayFFBGround();
    }
}

// Apply input as controls for selected devices
void update_input_controls() {
    if (!Util::PlayerAvailable(g_player, g_playerPed)) {
        return;
    }

    blockButtons();
    startStopEngine();

    if (!g_settings.Wheel.Options.Enable || g_controls.PrevInput != CarControls::Wheel) {
        return;
    }

    if (!g_controls.WheelAvailable())
        return;

    if (Util::VehicleAvailable(g_playerVehicle, g_playerPed)) {
        switch (g_vehData.mDomain) {
        case VehicleDomain::Road: {
            wheelControlRoad();
            break;
        }
        case VehicleDomain::Water: {
            // TODO: Option to disable for water
            wheelControlWater();
            break;
        }
        case VehicleDomain::Air: {
            // not supported
            break;
        }
        case VehicleDomain::Bicycle: {
            // not supported
            break;
        }
        case VehicleDomain::Rail: {
            // not supported
            break;
        }
        case VehicleDomain::Unknown: {
            break;
        }
        default: { }
        }
    }
}

// don't write with VehicleExtensions and dont set clutch state
void update_misc_features() {
    if (!Util::PlayerAvailable(g_player, g_playerPed))
        return;

    if (Util::VehicleAvailable(g_playerVehicle, g_playerPed)) {
        if (g_settings.GameAssists.AutoLookBack && g_vehData.mClass != VehicleClass::Heli) {
            functionAutoLookback();
        }
    }

    functionHidePlayerInFPV(false);
}

// Only when mod is working or writes clutch stuff.
// Called inside manual transmission part!
// Mostly optional stuff.
void update_manual_features() {
    if (g_settings.GameAssists.HillGravity) {
        functionHillGravity();
    }

    if (g_controls.PrevInput != CarControls::InputDevices::Wheel) {
        if (g_vehData.mClass == VehicleClass::Bike && g_settings.GameAssists.SimpleBike) {
            functionAutoReverse();
        }
        else {
            functionRealReverse();
        }
    }

    if (g_settings.MTOptions.EngBrake) {
        functionEngBrake();
    }
    else {
        g_wheelPatchStates.EngBrakeActive = false;
    }

    // Engine damage: RPM Damage
    if (g_settings.MTOptions.EngDamage && g_vehData.mHasClutch) {
        functionEngDamage();
    }

    if (g_settings.MTOptions.EngLock) {
        functionEngLock();
    }
    else {
        g_wheelPatchStates.EngLockActive = false;
    }

    if (!g_gearStates.FakeNeutral &&
        !(g_settings.GameAssists.SimpleBike && g_vehData.mClass == VehicleClass::Bike) && g_vehData.mHasClutch) {
        // Stalling
        if (g_settings.MTOptions.EngStallH && g_settings().MTOptions.ShiftMode == EShiftMode::HPattern ||
            g_settings.MTOptions.EngStallS && g_settings().MTOptions.ShiftMode == EShiftMode::Sequential) {
            functionEngStall();
        }

        // Simulate "catch point"
        // When the clutch "grabs" and the car starts moving without input
        if (g_settings().MTOptions.ClutchCreep && VEHICLE::GET_IS_VEHICLE_ENGINE_RUNNING(g_playerVehicle)) {
            functionClutchCatch();
        }
    }

    handleBrakePatch();
}

// Manual Transmission part of the mod
void update_manual_transmission() {
    if (!Util::PlayerAvailable(g_player, g_playerPed) || !Util::VehicleAvailable(g_playerVehicle, g_playerPed)) {
        return;
    }

    //updateSteeringMultiplier();

    if (g_controls.ButtonJustPressed(CarControls::KeyboardControlType::Toggle) ||
        g_controls.ButtonHeld(CarControls::WheelControlType::Toggle, 500) ||
        g_controls.ButtonHeld(CarControls::ControllerControlType::Toggle) ||
        g_controls.PrevInput == CarControls::Controller	&& g_controls.ButtonHeld(CarControls::LegacyControlType::Toggle)) {
        toggleManual(!g_settings.MTOptions.Enable);
        return;
    }

    if (g_vehData.mDomain != VehicleDomain::Road || !g_settings.MTOptions.Enable)
        return;

    ///////////////////////////////////////////////////////////////////////////
    //          Active whenever Manual is enabled from here
    ///////////////////////////////////////////////////////////////////////////

    if (g_controls.ButtonJustPressed(CarControls::KeyboardControlType::ToggleH) ||
        g_controls.ButtonJustPressed(CarControls::WheelControlType::ToggleH) ||
        g_controls.ButtonHeld(CarControls::ControllerControlType::ToggleH) ||
        g_controls.PrevInput == CarControls::Controller	&& g_controls.ButtonHeld(CarControls::LegacyControlType::ToggleH)) {
        setShiftMode(Next(g_settings().MTOptions.ShiftMode));
        g_settings.SaveGeneral();
    }

    if (MemoryPatcher::NumGearboxPatched != MemoryPatcher::NumGearboxPatches) {
        MemoryPatcher::ApplyGearboxPatches();
    }
    update_manual_features();

    switch(g_settings().MTOptions.ShiftMode) {
        case EShiftMode::Sequential: {
            functionSShift();
            if (g_settings.GameAssists.AutoGear1) {
                functionAutoGear1();
            }
            break;
        }
        case EShiftMode::HPattern: {
            if (g_controls.PrevInput == CarControls::Wheel) {
                functionHShiftWheel();
            }
            if (g_controls.PrevInput == CarControls::Keyboard ||
                g_controls.PrevInput == CarControls::Wheel && g_settings.Wheel.Options.HPatternKeyboard) {
                functionHShiftKeyboard();
            }
            break;
        }
        case EShiftMode::Automatic: {
            functionAShift();
            break;
        }
        default: break;
    }

    functionLimiter();

    updateShifting();

    // Finally, update memory each loop
    handleRPM();
    g_ext.SetGearCurr(g_playerVehicle, g_gearStates.LockGear);
    g_ext.SetGearNext(g_playerVehicle, g_gearStates.LockGear);
}

///////////////////////////////////////////////////////////////////////////////
//                           Mod functions: Mod control
///////////////////////////////////////////////////////////////////////////////

void clearPatches() {
    resetSteeringMultiplier();
    if (MemoryPatcher::NumGearboxPatched != 0) {
        MemoryPatcher::RevertGearboxPatches();
    }
    if (MemoryPatcher::SteeringControlPatcher.Patched()) {
        MemoryPatcher::RestoreSteeringControl();
    }
    if (MemoryPatcher::SteeringAssistPatcher.Patched()) {
        MemoryPatcher::RestoreSteeringAssist();
    }
    if (MemoryPatcher::BrakePatcher.Patched()) {
        MemoryPatcher::RestoreBrake();
    }
}

void toggleManual(bool enable) {
    // Don't need to do anything
    if (g_settings.MTOptions.Enable == enable)
        return;
    g_settings.MTOptions.Enable = enable;
    g_settings.SaveGeneral();
    if (g_settings.MTOptions.Enable) {
        UI::Notify(INFO, "MT Enabled");
    }
    else {
        UI::Notify(INFO, "MT Disabled");
    }
    readSettings();
    initWheel();
    clearPatches();
}

void initWheel() {
    g_controls.InitWheel();
    g_controls.CheckGUIDs(g_settings.Wheel.InputDevices.RegisteredGUIDs);
    logger.Write(INFO, "[Wheel] Steering wheel initialization finished");
}

void update_steering() {
    bool isCar = g_vehData.mClass == VehicleClass::Car;
    bool useWheel = g_controls.PrevInput == CarControls::Wheel;
    bool customSteering = g_settings.CustomSteering.Mode > 0 && !useWheel;

    if (isCar && (useWheel || customSteering)) {
        if (!MemoryPatcher::SteeringAssistPatcher.Patched())
            MemoryPatcher::PatchSteeringAssist();
    }
    else {
        if (MemoryPatcher::SteeringAssistPatcher.Patched())
            MemoryPatcher::RestoreSteeringAssist();
    }

    if (isCar && (useWheel || customSteering)) {
        if (!MemoryPatcher::SteeringControlPatcher.Patched())
            MemoryPatcher::PatchSteeringControl();
    }
    else {
        if (MemoryPatcher::SteeringControlPatcher.Patched())
            MemoryPatcher::RestoreSteeringControl();
    }

    if (Util::VehicleAvailable(g_playerVehicle, g_playerPed)) {
        updateSteeringMultiplier();
    }

    if (customSteering) {
        CustomSteering::Update();
    }
}

void updateSteeringMultiplier() {
    float mult;

    if (g_controls.PrevInput == CarControls::Wheel) {
        mult = g_settings.Wheel.Steering.SteerMult;
    }
    else {
        mult = g_settings.CustomSteering.SteeringMult;
    }
    g_ext.SetSteeringMultiplier(g_playerVehicle, mult);
}

void resetSteeringMultiplier() {
    if (g_playerVehicle != 0) {
        g_ext.SetSteeringMultiplier(g_playerVehicle, 1.0f);
    }
}

void updateLastInputDevice() {
    if (g_settings.Debug.DisableInputDetect)
        return;

    if (!PED::IS_PED_IN_ANY_VEHICLE(g_playerPed, true))
        return;

    if (g_controls.PrevInput != g_controls.GetLastInputDevice(g_controls.PrevInput,g_settings.Wheel.Options.Enable)) {
        g_controls.PrevInput = g_controls.GetLastInputDevice(g_controls.PrevInput, g_settings.Wheel.Options.Enable);
        std::string message = "Input: ";
        switch (g_controls.PrevInput) {
            case CarControls::Keyboard:
                message += "Keyboard";
                break;
            case CarControls::Controller:
                message += "Controller";
                if (g_settings().MTOptions.ShiftMode == EShiftMode::HPattern &&
                    g_settings.Controller.BlockHShift) {
                    message += "~n~Mode: Sequential";
                    if (g_settings.MTOptions.Override) {
                        
                    }
                    else {
                        setShiftMode(EShiftMode::Sequential);
                    }
                }
                break;
            case CarControls::Wheel:
                message += "Steering wheel";
                break;
            default: break;
        }
        // Suppress notification when not in car
        if (Util::VehicleAvailable(g_playerVehicle, g_playerPed)) {
            UI::Notify(INFO, message);
        }
    }
    if (g_controls.PrevInput == CarControls::Wheel) {
        CONTROLS::STOP_PAD_SHAKE(0);
    }
}

///////////////////////////////////////////////////////////////////////////////
//                           Mod functions: Shifting
///////////////////////////////////////////////////////////////////////////////

void setShiftMode(EShiftMode shiftMode) {
    if (Util::VehicleAvailable(g_playerVehicle, g_playerPed) && shiftMode != EShiftMode::HPattern && g_vehData.mGearCurr > 1)
        g_gearStates.FakeNeutral = false;

    EShiftMode tempMode = shiftMode;
    if (shiftMode == EShiftMode::HPattern &&
        g_controls.PrevInput == CarControls::Controller && 
        g_settings.Controller.BlockHShift) {
        tempMode = EShiftMode::Automatic;
    }

    if (g_settings.MTOptions.Override && g_activeConfig != nullptr) {
        g_activeConfig->MTOptions.ShiftMode = tempMode;
        g_settings.SetVehicleConfig(g_activeConfig);
    }
    else {
        g_settings.MTOptions.ShiftMode = tempMode;
    }

    std::string mode = "Mode: ";
    // ReSharper disable once CppDefaultCaseNotHandledInSwitchStatement
    switch (g_settings().MTOptions.ShiftMode) {
        case EShiftMode::Sequential:    mode += "Sequential";   break;
        case EShiftMode::HPattern:      mode += "H-Pattern";    break;
        case EShiftMode::Automatic:     mode += "Automatic";    break;
        default:                                                break;
    }
    UI::Notify(INFO, mode);
}

bool isClutchPressed() {
    return g_controls.ClutchVal > 1.0f - g_settings().MTParams.ClutchThreshold;
}

void shiftTo(int gear, bool autoClutch) {
    if (autoClutch) {
        if (g_gearStates.Shifting)
            return;
        g_gearStates.NextGear = gear;
        g_gearStates.Shifting = true;
        g_gearStates.ClutchVal = 0.0f;
        g_gearStates.ShiftDirection = gear > g_gearStates.LockGear ? ShiftDirection::Up : ShiftDirection::Down;
    }
    else {
        g_gearStates.LockGear = gear;
    }
}

void functionHShiftTo(int i) {
    bool shiftPass;

    bool checkShift = g_settings.MTOptions.ClutchShiftH && g_vehData.mHasClutch;

    // shifting from neutral into gear is OK when rev matched
    float expectedRPM = g_vehData.mWheelAverageDrivenTyreSpeed / (g_vehData.mDriveMaxFlatVel / g_vehData.mGearRatios[i]);
    bool rpmInRange = Math::Near(g_vehData.mRPM, expectedRPM, g_settings().ShiftOptions.RPMTolerance);

    bool clutchPass = g_controls.ClutchVal > 1.0f - g_settings().MTParams.ClutchThreshold;

    if (!checkShift)
        shiftPass = true;
    else if (rpmInRange)
        shiftPass = true;
    else if (clutchPass)
        shiftPass = true;
    else
        shiftPass = false;

    if (shiftPass) {
        shiftTo(i, false);
        g_gearStates.FakeNeutral = false;
    }
    else {
        g_gearStates.FakeNeutral = true;
        if (g_settings.MTOptions.EngDamage && g_vehData.mHasClutch) {
            VEHICLE::SET_VEHICLE_ENGINE_HEALTH(
                g_playerVehicle,
                VEHICLE::GET_VEHICLE_ENGINE_HEALTH(g_playerVehicle) - g_settings().MTParams.MisshiftDamage);
        }
    }
}

void functionHShiftKeyboard() {
    int clamp = g_numGears - 1;
    if (g_vehData.mGearTop <= clamp) {
        clamp = g_vehData.mGearTop;
    }
    for (uint8_t i = 0; i <= clamp; i++) {
        if (g_controls.ButtonJustPressed(static_cast<CarControls::KeyboardControlType>(i))) {
            functionHShiftTo(i);
        }
    }
    if (g_controls.ButtonJustPressed(CarControls::KeyboardControlType::HN) && g_vehData.mHasClutch) {
        g_gearStates.FakeNeutral = !g_gearStates.FakeNeutral;
    }
}

bool isHShifterJustNeutral() {
    auto minGear = CarControls::WheelControlType::HR;
    auto topGear = CarControls::WheelControlType::H10;
    for (auto gear = static_cast<uint32_t>(minGear); gear <= static_cast<uint32_t>(topGear); ++gear) {
        if (g_controls.ButtonReleased(static_cast<CarControls::WheelControlType>(gear)))
            return true;
    }
    return false;
}

void functionHShiftWheel() {
    int clamp = g_numGears - 1;
    if (g_vehData.mGearTop <= clamp) {
        clamp = g_vehData.mGearTop;
    }
    for (uint8_t i = 0; i <= clamp; i++) {
        if (g_controls.ButtonJustPressed(static_cast<CarControls::WheelControlType>(i))) {
            functionHShiftTo(i);
        }
    }

    if (isHShifterJustNeutral()) {
        g_gearStates.FakeNeutral = g_vehData.mHasClutch;
    }

    if (g_controls.ButtonReleased(CarControls::WheelControlType::HR)) {
        shiftTo(1, false);
        g_gearStates.FakeNeutral = g_vehData.mHasClutch;
    }
}

bool isUIActive() {
    return PED::IS_PED_RUNNING_MOBILE_PHONE_TASK(g_playerPed) ||
        g_menu.IsThisOpen() || TrainerV::Active();
}

/*
 * Delayed shifting with clutch. Active for automatic and sequential shifts.
 * Shifting process:
 * 1. Init shift, next gear set
 * 2. Clutch disengages
 * 3. Next gear active
 * 4. Clutch engages again
 * 5. Done shifting
 */
void updateShifting() {
    if (!g_gearStates.Shifting)
        return;

    auto handlingPtr = g_ext.GetHandlingPtr(g_playerVehicle);
    // This is x Clutch per second? e.g. changerate 2.5 -> clutch fully (dis)engages in 1/2.5 seconds? or whole thing?
    float rateUp = *reinterpret_cast<float*>(handlingPtr + hOffsets.fClutchChangeRateScaleUpShift);
    float rateDown = *reinterpret_cast<float*>(handlingPtr + hOffsets.fClutchChangeRateScaleDownShift);

    float shiftRate = g_gearStates.ShiftDirection == ShiftDirection::Up ? rateUp : rateDown;
    shiftRate *= g_settings().ShiftOptions.ClutchRateMult;

    /*
     * 4.0 gives similar perf as base - probably the whole shift takes 1/rate seconds
     * with my extra disengage step, the whole thing should take also 1/rate seconds
     */
    shiftRate = shiftRate * GAMEPLAY::GET_FRAME_TIME() * 4.0f;

    // Something went wrong, abort and just shift to NextGear.
    if (g_gearStates.ClutchVal > 1.5f) {
        g_gearStates.ClutchVal = 0.0f;
        g_gearStates.Shifting = false;
        g_gearStates.LockGear = g_gearStates.NextGear;
        return;
    }

    if (g_gearStates.NextGear != g_gearStates.LockGear) {
        g_gearStates.ClutchVal += shiftRate;
    }
    if (g_gearStates.ClutchVal >= 1.0f && g_gearStates.LockGear != g_gearStates.NextGear) {
        g_gearStates.LockGear = g_gearStates.NextGear;
        return;
    }
    if (g_gearStates.NextGear == g_gearStates.LockGear) {
        g_gearStates.ClutchVal -= shiftRate;
    }

    if (g_gearStates.ClutchVal < 0.0f && g_gearStates.NextGear == g_gearStates.LockGear) {
        g_gearStates.ClutchVal = 0.0f;
        g_gearStates.Shifting = false;
    }
}

void functionSShift() {
    auto xcTapStateUp = g_controls.ButtonTapped(CarControls::ControllerControlType::ShiftUp);
    auto xcTapStateDn = g_controls.ButtonTapped(CarControls::ControllerControlType::ShiftDown);

    auto ncTapStateUp = g_controls.ButtonTapped(CarControls::LegacyControlType::ShiftUp);
    auto ncTapStateDn = g_controls.ButtonTapped(CarControls::LegacyControlType::ShiftDown);

    if (g_settings.Controller.IgnoreShiftsUI && isUIActive()) {
        xcTapStateUp = xcTapStateDn = XInputController::TapState::ButtonUp;
        ncTapStateUp = ncTapStateDn = NativeController::TapState::ButtonUp;
    }

    bool allowKeyboard = g_controls.PrevInput == CarControls::Keyboard || 
        g_controls.PrevInput == CarControls::Controller;

    // Shift up
    if (g_controls.PrevInput == CarControls::Controller	&& xcTapStateUp == XInputController::TapState::Tapped ||
        g_controls.PrevInput == CarControls::Controller	&& ncTapStateUp == NativeController::TapState::Tapped ||
        allowKeyboard && g_controls.ButtonJustPressed(CarControls::KeyboardControlType::ShiftUp) ||
        g_controls.PrevInput == CarControls::Wheel			&& g_controls.ButtonJustPressed(CarControls::WheelControlType::ShiftUp)) {
        if (!g_vehData.mHasClutch) {
            if (g_vehData.mGearCurr < g_vehData.mGearTop) {
                shiftTo(g_gearStates.LockGear + 1, true);
            }
            g_gearStates.FakeNeutral = false;
            return;
        }

        // Shift block /w clutch shifting for seq.
        if (g_settings.MTOptions.ClutchShiftS && 
            !isClutchPressed()) {
            return;
        }

        // Reverse to Neutral
        if (g_vehData.mGearCurr == 0 && !g_gearStates.FakeNeutral) {
            shiftTo(1, false);
            g_gearStates.FakeNeutral = true;
            return;
        }

        // Neutral to 1
        if (g_vehData.mGearCurr == 1 && g_gearStates.FakeNeutral) {
            g_gearStates.FakeNeutral = false;
            return;
        }

        // 1 to X
        if (g_vehData.mGearCurr < g_vehData.mGearTop) {
            shiftTo(g_gearStates.LockGear + 1, true);
            g_gearStates.FakeNeutral = false;
            return;
        }
    }

    // Shift down

    if (g_controls.PrevInput == CarControls::Controller	&& xcTapStateDn == XInputController::TapState::Tapped ||
        g_controls.PrevInput == CarControls::Controller	&& ncTapStateDn == NativeController::TapState::Tapped ||
        allowKeyboard && g_controls.ButtonJustPressed(CarControls::KeyboardControlType::ShiftDown) ||
        g_controls.PrevInput == CarControls::Wheel			&& g_controls.ButtonJustPressed(CarControls::WheelControlType::ShiftDown)) {
        if (!g_vehData.mHasClutch) {
            if (g_vehData.mGearCurr > 0) {
                shiftTo(g_gearStates.LockGear - 1, false);
                g_gearStates.FakeNeutral = false;
            }
            return;
        }

        // Shift block /w clutch shifting for seq.
        if (g_settings.MTOptions.ClutchShiftS &&
            !isClutchPressed()) {
            return;
        }

        // 1 to Neutral
        if (g_vehData.mGearCurr == 1 && !g_gearStates.FakeNeutral) {
            g_gearStates.FakeNeutral = true;
            return;
        }

        // Neutral to R
        if (g_vehData.mGearCurr == 1 && g_gearStates.FakeNeutral) {
            shiftTo(0, false);
            g_gearStates.FakeNeutral = false;
            return;
        }

        // X to 1
        if (g_vehData.mGearCurr > 1) {
            shiftTo(g_gearStates.LockGear - 1, true);
            g_gearStates.FakeNeutral = false;
        }
        return;
    }
}

/*
 * Manual part of the automatic transmission (R<->N<->D)
 */
bool subAutoShiftSequential() {
    auto xcTapStateUp = g_controls.ButtonTapped(CarControls::ControllerControlType::ShiftUp);
    auto xcTapStateDn = g_controls.ButtonTapped(CarControls::ControllerControlType::ShiftDown);

    auto ncTapStateUp = g_controls.ButtonTapped(CarControls::LegacyControlType::ShiftUp);
    auto ncTapStateDn = g_controls.ButtonTapped(CarControls::LegacyControlType::ShiftDown);

    if (g_settings.Controller.IgnoreShiftsUI && isUIActive()) {
        xcTapStateUp = xcTapStateDn = XInputController::TapState::ButtonUp;
        ncTapStateUp = ncTapStateDn = NativeController::TapState::ButtonUp;
    }

    bool allowKeyboard = g_controls.PrevInput == CarControls::Keyboard ||
        g_controls.PrevInput == CarControls::Controller;

    // Shift up
    if (g_controls.PrevInput == CarControls::Controller	&& xcTapStateUp == XInputController::TapState::Tapped ||
        g_controls.PrevInput == CarControls::Controller	&& ncTapStateUp == NativeController::TapState::Tapped ||
        allowKeyboard  && g_controls.ButtonJustPressed(CarControls::KeyboardControlType::ShiftUp) ||
        g_controls.PrevInput == CarControls::Wheel			&& g_controls.ButtonJustPressed(CarControls::WheelControlType::ShiftUp)) {
        // Reverse to Neutral
        if (g_vehData.mGearCurr == 0 && !g_gearStates.FakeNeutral) {
            shiftTo(1, false);
            g_gearStates.FakeNeutral = true;
            return true;
        }

        // Neutral to 1
        if (g_vehData.mGearCurr == 1 && g_gearStates.FakeNeutral) {
            g_gearStates.FakeNeutral = false;
            return true;
        }

        // Invalid + N to 1
        if (g_vehData.mGearCurr == 0 && g_gearStates.FakeNeutral) {
            shiftTo(1, false);
            g_gearStates.FakeNeutral = false;
            return true;
        }
    }

    // Shift down
    if (g_controls.PrevInput == CarControls::Controller	&& xcTapStateDn == XInputController::TapState::Tapped ||
        g_controls.PrevInput == CarControls::Controller	&& ncTapStateDn == NativeController::TapState::Tapped ||
        allowKeyboard && g_controls.ButtonJustPressed(CarControls::KeyboardControlType::ShiftDown) ||
        g_controls.PrevInput == CarControls::Wheel			&& g_controls.ButtonJustPressed(CarControls::WheelControlType::ShiftDown)) {
        // 1 to Neutral
        if (g_vehData.mGearCurr == 1 && !g_gearStates.FakeNeutral) {
            g_gearStates.FakeNeutral = true;
            return true;
        }

        // Neutral to R
        if (g_vehData.mGearCurr == 1 && g_gearStates.FakeNeutral) {
            shiftTo(0, false);
            g_gearStates.FakeNeutral = false;
            return true;
        }

        // Invalid + N to R
        if (g_vehData.mGearCurr == 0 && g_gearStates.FakeNeutral) {
            shiftTo(0, false);
            g_gearStates.FakeNeutral = false;
            return true;
        }
    }
    return false;
}

/*
 * Manual part of the automatic transmission (Direct selection)
 */
bool subAutoShiftSelect() {
    if (g_controls.ButtonIn(CarControls::WheelControlType::APark)) {
        if (g_gearStates.LockGear != 1) {
            shiftTo(1, false);
        }
        g_gearStates.FakeNeutral = true;
        CONTROLS::_SET_CONTROL_NORMAL(0, ControlVehicleHandbrake, 1.0f);
        return true;
    }
    if (g_controls.ButtonJustPressed(CarControls::WheelControlType::AReverse)) {
        if (ENTITY::GET_ENTITY_SPEED_VECTOR(g_playerVehicle, true).y < 5.0f) {
            shiftTo(0, false);
            g_gearStates.FakeNeutral = false;
        }
        return true;
    }
    if (g_controls.ButtonJustPressed(CarControls::WheelControlType::ADrive)) {
        shiftTo(1, false);
        g_gearStates.FakeNeutral = false;
        return true;
    }
    // Unassigned neutral -> pop into neutral when any gear is released
    if (g_controls.WheelButton[static_cast<int>(CarControls::WheelControlType::ANeutral)] == -1) {
        if (g_controls.ButtonReleased(CarControls::WheelControlType::APark) ||
            g_controls.ButtonReleased(CarControls::WheelControlType::AReverse) ||
            g_controls.ButtonReleased(CarControls::WheelControlType::ADrive)) {
            if (g_gearStates.LockGear != 1) {
                shiftTo(1, false);
            }
            g_gearStates.FakeNeutral = true;
            return true;
        }
    }
    // Assigned neutral -> handle like any other button.
    else {
        if (g_controls.ButtonJustPressed(CarControls::WheelControlType::ANeutral)) {
            if (g_gearStates.LockGear != 1) {
                shiftTo(1, false);
            }
            g_gearStates.FakeNeutral = true;
            return true;
        }
    }
    return false;
}

void functionAShift() { 
    // Manual part
    if (g_controls.PrevInput == CarControls::Wheel && g_settings.Wheel.Options.UseShifterForAuto) {
        if (subAutoShiftSelect()) 
            return;
    }
    else {
        if (subAutoShiftSequential()) 
            return;
    }
    
    int currGear = g_vehData.mGearCurr;
    if (currGear == 0)
        return;
    if (g_gearStates.Shifting)
        return;

    if (g_controls.ThrottleVal >= g_gearStates.ThrottleHang)
        g_gearStates.ThrottleHang = g_controls.ThrottleVal;
    else if (g_gearStates.ThrottleHang > 0.0f)
        g_gearStates.ThrottleHang -= GAMEPLAY::GET_FRAME_TIME() * g_settings().AutoParams.EcoRate;

    if (g_gearStates.ThrottleHang < 0.0f)
        g_gearStates.ThrottleHang = 0.0f;

    float currSpeed = g_vehData.mWheelAverageDrivenTyreSpeed;

    float nextGearMinSpeed = 0.0f; // don't care about top gear
    if (currGear < g_vehData.mGearTop) {
        nextGearMinSpeed = g_settings().AutoParams.NextGearMinRPM * g_vehData.mDriveMaxFlatVel / g_vehData.mGearRatios[currGear + 1];
    }

    float currGearMinSpeed = g_settings().AutoParams.CurrGearMinRPM * g_vehData.mDriveMaxFlatVel / g_vehData.mGearRatios[currGear];

    float engineLoad = g_gearStates.ThrottleHang - map(g_vehData.mRPM, 0.2f, 1.0f, 0.0f, 1.0f);
    g_gearStates.EngineLoad = engineLoad;
    g_gearStates.UpshiftLoad = g_settings().AutoParams.UpshiftLoad;

    bool skidding = false;
    auto skids = g_ext.GetWheelSkidSmokeEffect(g_playerVehicle);
    for (uint8_t i = 0; i < g_vehData.mWheelCount; ++i) {
        if (abs(skids[i]) > 3.5f && g_ext.IsWheelPowered(g_playerVehicle, i))
            skidding = true;
    }

    // Shift up.
    if (currGear < g_vehData.mGearTop) {
        if (engineLoad < g_settings().AutoParams.UpshiftLoad && currSpeed > nextGearMinSpeed && !skidding) {
            shiftTo(g_vehData.mGearCurr + 1, true);
            g_gearStates.FakeNeutral = false;
            g_gearStates.LastUpshiftTime = GAMEPLAY::GET_GAME_TIMER();
        }
    }

    // Shift down later when ratios are far apart
    float gearRatioRatio = 1.0f;

    if (g_vehData.mGearTop > 1 && currGear < g_vehData.mGearTop) {
        float thisGearRatio = g_vehData.mGearRatios[currGear] / g_vehData.mGearRatios[currGear + 1];
        gearRatioRatio = thisGearRatio;
    }

    float rateUp = *reinterpret_cast<float*>(g_vehData.mHandlingPtr + hOffsets.fClutchChangeRateScaleUpShift);
    float upshiftDuration = 1.0f / (rateUp * g_settings().ShiftOptions.ClutchRateMult);
    bool tpPassed = GAMEPLAY::GET_GAME_TIMER() > g_gearStates.LastUpshiftTime + static_cast<int>(1000.0f * upshiftDuration * g_settings.AutoParams.DownshiftTimeoutMult);
    g_gearStates.DownshiftLoad = g_settings().AutoParams.DownshiftLoad * gearRatioRatio;

    // Shift down
    if (currGear > 1) {
        if (tpPassed && engineLoad > g_settings().AutoParams.DownshiftLoad * gearRatioRatio || currSpeed < currGearMinSpeed) {
            shiftTo(currGear - 1, true);
            g_gearStates.FakeNeutral = false;
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
//                   Mod functions: Gearbox features
///////////////////////////////////////////////////////////////////////////////

void functionClutchCatch() {
    // TODO: Make more subtle / use throttle?
    const float idleThrottle = 0.24f; // TODO -> Settings?

    float clutchRatio = map(g_controls.ClutchVal, 1.0f - g_settings().MTParams.ClutchThreshold, 0.0f, 0.0f, 1.0f);
    clutchRatio = std::clamp(clutchRatio, 0.0f, 1.0f);

    // TODO: Check for settings.MTOptions.ShiftMode == Automatic if anybody complains...

    bool clutchEngaged = !isClutchPressed();
    float minSpeed = 0.2f * (g_vehData.mDriveMaxFlatVel / g_vehData.mGearRatios[g_vehData.mGearCurr]);
    float expectedSpeed = g_vehData.mRPM * (g_vehData.mDriveMaxFlatVel / g_vehData.mGearRatios[g_vehData.mGearCurr]) * clutchRatio;
    float actualSpeed = g_vehData.mWheelAverageDrivenTyreSpeed;
    if (abs(actualSpeed) < abs(minSpeed) &&
    	clutchEngaged) {
        float throttle = map(abs(actualSpeed), 0.0f, abs(expectedSpeed), idleThrottle, 0.0f);
    	throttle = std::clamp(throttle, 0.0f, idleThrottle);

    	bool userThrottle = abs(g_controls.ThrottleVal) > idleThrottle || abs(g_controls.BrakeVal) > idleThrottle;
    	if (!userThrottle) {
    		if (g_vehData.mGearCurr > 0)
                Controls::SetControlADZ(ControlVehicleAccelerate, throttle, 0.25f);
    		else
                Controls::SetControlADZ(ControlVehicleBrake, throttle, 0.25f);
    	}
    }
}

void functionEngStall() {
    const float stallRate = GAMEPLAY::GET_FRAME_TIME() * 3.33f;

    float minSpeed = g_settings().MTParams.StallingRPM * abs(g_vehData.mDriveMaxFlatVel / g_vehData.mGearRatios[g_vehData.mGearCurr]);
    float actualSpeed = g_vehData.mWheelAverageDrivenTyreSpeed;

    // Closer to idle speed = less buildup for stalling
    float speedDiffRatio = map(abs(minSpeed) - abs(actualSpeed), 0.0f, abs(minSpeed), 0.0f, 1.0f);
    speedDiffRatio = std::clamp(speedDiffRatio, 0.0f, 1.0f);
    if (speedDiffRatio < 0.45f)
        speedDiffRatio = 0.0f; //ignore if we're close-ish to idle
    
    bool clutchEngaged = !isClutchPressed();
    bool stallEngaged = g_controls.ClutchVal < 1.0f - g_settings().MTParams.StallingThreshold;

    // this thing is big when the clutch isnt pressed
    float invClutch = 1.0f - g_controls.ClutchVal;

    if (stallEngaged &&
        g_vehData.mRPM < 0.25f && //engine actually has to idle
        abs(actualSpeed) < abs(minSpeed) &&
        VEHICLE::GET_IS_VEHICLE_ENGINE_RUNNING(g_playerVehicle)) {
        float change = invClutch * speedDiffRatio * stallRate;
        g_gearStates.StallProgress += change;
    }
    else if (g_gearStates.StallProgress > 0.0f) {
        float change = stallRate; // "subtract" quickly
        g_gearStates.StallProgress -= change;
    }

    if (g_gearStates.StallProgress > 1.0f) {
        if (VEHICLE::GET_IS_VEHICLE_ENGINE_RUNNING(g_playerVehicle)) {
            VEHICLE::SET_VEHICLE_ENGINE_ON(g_playerVehicle, false, true, true);
        }
        g_gearStates.StallProgress = 0.0f;
    }

    // Simulate push-start
    // We'll just assume the ignition thing is in the "on" position.
    if (actualSpeed > minSpeed && !VEHICLE::GET_IS_VEHICLE_ENGINE_RUNNING(g_playerVehicle) &&
        stallEngaged) {
        VEHICLE::SET_VEHICLE_ENGINE_ON(g_playerVehicle, true, true, true);
    }

    //showText(0.1, 0.00, 0.4, fmt("Stall progress: %.02f", gearStates.StallProgress));
    //showText(0.1, 0.02, 0.4, fmt("Clutch: %d", clutchEngaged));
    //showText(0.1, 0.04, 0.4, fmt("Stall?: %d", stallEngaged));
    //showText(0.1, 0.06, 0.4, fmt("SpeedDiffRatio: %.02f", speedDiffRatio));
}

void functionEngDamage() {
    if (g_settings().MTOptions.ShiftMode == EShiftMode::Automatic ||
        g_vehData.mFlags[1] & eVehicleFlag2::FLAG_IS_ELECTRIC || 
        g_vehData.mGearTop == 1) {
        return;
    }

    if (g_vehData.mGearCurr != g_vehData.mGearTop &&
        g_vehData.mRPM > 0.98f &&
        g_controls.ThrottleVal > 0.98f) {
        VEHICLE::SET_VEHICLE_ENGINE_HEALTH(g_playerVehicle, 
                                           VEHICLE::GET_VEHICLE_ENGINE_HEALTH(g_playerVehicle) - (g_settings().MTParams.RPMDamage));
    }
}

void functionEngLock() {
    if (g_settings().MTOptions.ShiftMode == EShiftMode::Automatic ||
        g_vehData.mFlags[1] & eVehicleFlag2::FLAG_IS_ELECTRIC ||
        g_vehData.mGearTop == 1 || 
        g_vehData.mGearCurr == g_vehData.mGearTop ||
        g_gearStates.FakeNeutral) {
        g_wheelPatchStates.EngLockActive = false;
        return;
    }
    const float reverseThreshold = 2.0f;

    float dashms = abs(ENTITY::GET_ENTITY_SPEED_VECTOR(g_playerVehicle, true).y);

    float speed = dashms;
    auto ratios = g_vehData.mGearRatios;
    float DriveMaxFlatVel = g_vehData.mDriveMaxFlatVel;
    float maxSpeed = DriveMaxFlatVel / ratios[g_vehData.mGearCurr];

    float inputMultiplier = (1.0f - g_controls.ClutchVal);
    auto wheelsSpeed = g_vehData.mWheelAverageDrivenTyreSpeed;

    bool wrongDirection = false;
    if (VEHICLE::GET_IS_VEHICLE_ENGINE_RUNNING(g_playerVehicle)) {
        if (g_vehData.mGearCurr == 0) {
            if (g_vehData.mVelocity.y > reverseThreshold && wheelsSpeed > reverseThreshold) {
                wrongDirection = true;
            }
        }
        else {
            if (g_vehData.mVelocity.y < -reverseThreshold && wheelsSpeed < -reverseThreshold) {
                wrongDirection = true;
            }
        }
    }

    // Wheels are locking up due to bad (down)shifts
    if ((speed > abs(maxSpeed * 1.15f) + 3.334f || wrongDirection) && !isClutchPressed()) {
        g_wheelPatchStates.EngLockActive = true;
        float lockingForce = 60.0f * inputMultiplier;
        auto wheelsToLock = g_vehData.mWheelsDriven;//getDrivenWheels();

        for (int i = 0; i < g_vehData.mWheelCount; i++) {
            if (i >= wheelsToLock.size() || wheelsToLock[i]) {
                g_ext.SetWheelPower(g_playerVehicle, i, -lockingForce * sgn(g_vehData.mVelocity.y));
                g_ext.SetWheelSkidSmokeEffect(g_playerVehicle, i, lockingForce);
            }
            else {
                float inpBrakeForce = *reinterpret_cast<float *>(g_vehData.mHandlingPtr + hOffsets.fBrakeForce) * g_controls.BrakeVal;
                //ext.SetWheelBrakePressure(vehicle, i, inpBrakeForce);
            }
        }
        fakeRev(true, 1.0f);
        float oldEngineHealth = VEHICLE::GET_VEHICLE_ENGINE_HEALTH(g_playerVehicle);
        float damageToApply = g_settings().MTParams.MisshiftDamage * inputMultiplier;
        if (g_settings.MTOptions.EngDamage) {
            if (oldEngineHealth >= damageToApply) {
                VEHICLE::SET_VEHICLE_ENGINE_HEALTH(
                    g_playerVehicle,
                    oldEngineHealth - damageToApply);
            }
            else {
                VEHICLE::SET_VEHICLE_ENGINE_ON(g_playerVehicle, false, true, true);
            }
        }
        if (g_settings.Debug.DisplayInfo) {
            showText(0.5, 0.80, 0.25, fmt::format("Eng block: {:.3f}", inputMultiplier));
            showText(0.5, 0.85, 0.25, fmt::format("Eng block force: {:.3f}", lockingForce));
        }
    }
    else {
        g_wheelPatchStates.EngLockActive = false;
    }
}

void functionEngBrake() {
    // When you let go of the throttle at high RPMs
    float activeBrakeThreshold = g_settings().MTParams.EngBrakeThreshold;

    if (g_vehData.mRPM >= activeBrakeThreshold && ENTITY::GET_ENTITY_SPEED_VECTOR(g_playerVehicle, true).y > 5.0f && !g_gearStates.FakeNeutral) {
        float handlingBrakeForce = *reinterpret_cast<float *>(g_vehData.mHandlingPtr + hOffsets.fBrakeForce);
        float inpBrakeForce = handlingBrakeForce * g_controls.BrakeVal;

        float throttleMultiplier = 1.0f - g_controls.ThrottleVal;
        float clutchMultiplier = 1.0f - g_controls.ClutchVal;
        float inputMultiplier = throttleMultiplier * clutchMultiplier;
        if (inputMultiplier > 0.05f) {
            g_wheelPatchStates.EngBrakeActive = true;
            float rpmMultiplier = (g_vehData.mRPM - activeBrakeThreshold) / (1.0f - activeBrakeThreshold);
            float engBrakeForce = g_settings().MTParams.EngBrakePower * inputMultiplier * rpmMultiplier;
            auto wheelsToBrake = g_vehData.mWheelsDriven;
            for (int i = 0; i < g_vehData.mWheelCount; i++) {
                if (wheelsToBrake[i]) {
                    g_ext.SetWheelBrakePressure(g_playerVehicle, i, inpBrakeForce + engBrakeForce);
                }
                else {
                    g_ext.SetWheelBrakePressure(g_playerVehicle, i, inpBrakeForce);
                }
            }
            if (g_settings.Debug.DisplayInfo) {
                showText(0.85, 0.500, 0.4, fmt::format("EngBrake:\t\t{:.3f}", inputMultiplier), 4);
                showText(0.85, 0.525, 0.4, fmt::format("Pressure:\t\t{:.3f}", engBrakeForce), 4);
                showText(0.85, 0.550, 0.4, fmt::format("BrkInput:\t\t{:.3f}", inpBrakeForce), 4);
            }
        }
        else {
            g_wheelPatchStates.EngBrakeActive = false;
        }
    }
    else {
        g_wheelPatchStates.EngBrakeActive = false;
    }
}

///////////////////////////////////////////////////////////////////////////////
//                       Mod functions: Gearbox control
///////////////////////////////////////////////////////////////////////////////

void handleBrakePatch() {
    bool useABS = false;
    bool useTCS = false;
    bool useESP = false;

    bool espUndersteer = false;
    // average front wheels slip angle
    float understeerAngle = 0.0f; // rad

    bool espOversteer = false;
    bool oppositeLock = false;
    // average rear wheels slip angle
    float oversteerAngle = 0.0f; // rad

    bool ebrk = g_vehData.mHandbrake;
    bool brn = VEHICLE::IS_VEHICLE_IN_BURNOUT(g_playerVehicle);
    auto susps = g_vehData.mSuspensionTravel;
    auto lockUps = g_vehData.mWheelsLockedUp;
    auto speeds = g_vehData.mWheelTyreSpeeds;

    auto slipped = std::vector<bool>(g_vehData.mWheelCount);

    {
        bool lockedUp = false;
        auto brakePressures = g_ext.GetWheelBrakePressure(g_playerVehicle);
        for (int i = 0; i < g_vehData.mWheelCount; i++) {
            if (lockUps[i] && susps[i] > 0.0f && brakePressures[i] > 0.0f)
                lockedUp = true;
        }
        if (ebrk || brn)
            lockedUp = false;
        bool absNativePresent = g_vehData.mHasABS && g_settings().DriveAssists.ABS.Filter;

        if (g_settings().DriveAssists.ABS.Enable && lockedUp && !absNativePresent)
            useABS = true;
    }
    
    {
        bool tractionLoss = false;
        if (g_settings().DriveAssists.TCS.Mode != 0) {
            auto pows = g_ext.GetWheelPower(g_playerVehicle);
            for (int i = 0; i < g_vehData.mWheelCount; i++) {
                if (speeds[i] > g_vehData.mVelocity.y + g_settings().DriveAssists.TCS.SlipMax && 
                    susps[i] > 0.0f && 
                    g_vehData.mWheelsDriven[i] && 
                    pows[i] > 0.1f) {
                    tractionLoss = true;
                    slipped[i] = true;
                }
            }
            if (ebrk || brn)
                tractionLoss = false;
        }

        if (g_settings().DriveAssists.TCS.Mode != 0 && tractionLoss)
            useTCS = true;
    }

    float steerMult = g_settings.CustomSteering.SteeringMult;
    if (g_controls.PrevInput == CarControls::InputDevices::Wheel)
        steerMult = g_settings.Wheel.Steering.SteerMult;
    float avgAngle = g_ext.GetWheelAverageAngle(g_playerVehicle) * steerMult;
    {
        // data
        float speed = ENTITY::GET_ENTITY_SPEED(g_playerVehicle);
        Vector3 vecNextSpd = ENTITY::GET_ENTITY_SPEED_VECTOR(g_playerVehicle, true);
        Vector3 rotVel = ENTITY::GET_ENTITY_ROTATION_VELOCITY(g_playerVehicle);
        Vector3 rotRelative{
            speed * -sin(rotVel.z), 0,
            speed * cos(rotVel.z), 0,
            0, 0
        };

        Vector3 vecPredStr{
            speed * -sin(avgAngle / g_settings.Wheel.Steering.SteerMult), 0,
            speed * cos(avgAngle / g_settings.Wheel.Steering.SteerMult), 0,
            0, 0
        };

        // understeer
        {
            Vector3 vecNextRot = (vecNextSpd + rotRelative) * 0.5f;
            understeerAngle = GetAngleBetween(vecNextRot, vecPredStr);

            float dSpdStr = Distance(vecNextSpd, vecPredStr);
            float aSpdStr = GetAngleBetween(vecNextSpd, vecPredStr);

            float dSpdRot = Distance(vecNextSpd, vecNextRot);
            float aSpdRot = GetAngleBetween(vecNextSpd, vecNextRot);

            if (dSpdStr > dSpdRot && sgn(aSpdRot) == sgn(aSpdStr)) {
                if (abs(understeerAngle) > deg2rad(g_settings.DriveAssists.ESP.UnderMin) && g_vehData.mVelocity.y > 10.0f && abs(avgAngle) > deg2rad(2.0f)) {
                    espUndersteer = true;
                }
            }
        }

        // oversteer
        {
            oversteerAngle = acos(g_vehData.mVelocity.y / speed);
            if (isnan(oversteerAngle))
                oversteerAngle = 0.0;

            if (oversteerAngle > deg2rad(g_settings.DriveAssists.ESP.OverMin) && g_vehData.mVelocity.y > 10.0f) {
                espOversteer = true;

                if (sgn(g_vehData.mVelocity.x) == sgn(avgAngle)) {
                    oppositeLock = true;
                }
            }
        }
        bool anyWheelOnGround = false;
        for (bool value : g_vehData.mWheelsOnGround) {
            anyWheelOnGround |= value;
        }
        if (g_settings().DriveAssists.ESP.Enable && g_vehData.mWheelCount == 4 && anyWheelOnGround) {
            if (espOversteer || espUndersteer) {
                useESP = true;
            }
        }
    }

    float handlingBrakeForce = *reinterpret_cast<float*>(g_vehData.mHandlingPtr + hOffsets.fBrakeForce);
    float bbalF = *reinterpret_cast<float*>(g_vehData.mHandlingPtr + hOffsets.fBrakeBiasFront);
    float bbalR = *reinterpret_cast<float*>(g_vehData.mHandlingPtr + hOffsets.fBrakeBiasRear);
    float inpBrakeForce = handlingBrakeForce * g_controls.BrakeVal;

    if (useTCS && g_settings().DriveAssists.TCS.Mode == 2) {
        CONTROLS::DISABLE_CONTROL_ACTION(2, eControl::ControlVehicleAccelerate, true);
        if (g_settings.Debug.DisplayInfo)
            showText(0.45, 0.75, 1.0, "~r~(TCS/T)");
    }

    if (g_wheelPatchStates.InduceBurnout) {
        if (!MemoryPatcher::BrakePatcher.Patched()) {
            MemoryPatcher::PatchBrake();
        }
        if (!MemoryPatcher::ThrottlePatcher.Patched()) {
            MemoryPatcher::PatchThrottle();
        }
        if (g_settings.Debug.DisplayInfo)
            showText(0.45, 0.75, 1.0, "~r~Burnout");
    }
    else if (g_wheelPatchStates.EngLockActive) {
        if (!MemoryPatcher::ThrottlePatcher.Patched()) {
            MemoryPatcher::PatchThrottle();
        }
        if (g_settings.Debug.DisplayInfo)
            showText(0.45, 0.75, 1.0, "~r~EngLock");
    }
    else if (useABS) {
        if (!MemoryPatcher::BrakePatcher.Patched()) {
            MemoryPatcher::PatchBrake();
        }
        for (uint8_t i = 0; i < lockUps.size(); i++) {
            if (lockUps[i]) {
                g_ext.SetWheelBrakePressure(g_playerVehicle, i, 0.0f);
                g_vehData.mWheelsAbs[i] = true;
            }
            else {                
                g_ext.SetWheelBrakePressure(g_playerVehicle, i, inpBrakeForce);
                g_vehData.mWheelsAbs[i] = false;
            }
        }
        if (g_settings.Debug.DisplayInfo)
            showText(0.45, 0.75, 1.0, "~r~(ABS)");
    }
    else if (useESP) {
        if (!MemoryPatcher::BrakePatcher.Patched()) {
            MemoryPatcher::PatchBrake();
        }
        for (auto i = 0; i < g_vehData.mWheelCount; ++i) {
            g_vehData.mWheelsAbs[i] = false;
        }
        float avgAngle_ = -avgAngle;
        if (oppositeLock) {
            avgAngle_ = avgAngle;
        }
        float oversteerAngleDeg = abs(rad2deg(oversteerAngle));
        float oversteerComp = map(oversteerAngleDeg, 
            g_settings.DriveAssists.ESP.OverMin, g_settings.DriveAssists.ESP.OverMax,
            g_settings.DriveAssists.ESP.OverMinComp, g_settings.DriveAssists.ESP.OverMaxComp);

        float oversteerAdd = handlingBrakeForce * oversteerComp;

        float understeerAngleDeg(abs(rad2deg(understeerAngle)));
        float understeerComp = map(understeerAngleDeg, 
            g_settings.DriveAssists.ESP.UnderMin, g_settings.DriveAssists.ESP.UnderMax,
            g_settings.DriveAssists.ESP.UnderMinComp, g_settings.DriveAssists.ESP.UnderMaxComp);

        float understeerAdd = handlingBrakeForce * understeerComp;
        espOversteer = espOversteer && !espUndersteer;

        g_ext.SetWheelBrakePressure(g_playerVehicle, 0, inpBrakeForce * bbalF + (avgAngle_ < 0.0f && espOversteer ? oversteerAdd : 0.0f));
        g_ext.SetWheelBrakePressure(g_playerVehicle, 1, inpBrakeForce * bbalF + (avgAngle_ > 0.0f && espOversteer ? oversteerAdd : 0.0f));
        g_ext.SetWheelBrakePressure(g_playerVehicle, 2, inpBrakeForce * bbalR + (avgAngle > 0.0f && espUndersteer ? understeerAdd : 0.0f));
        g_ext.SetWheelBrakePressure(g_playerVehicle, 3, inpBrakeForce * bbalR + (avgAngle < 0.0f && espUndersteer ? understeerAdd : 0.0f));
        g_vehData.mWheelsEsp[0] = avgAngle_ < 0.0f && espOversteer ? true : false;
        g_vehData.mWheelsEsp[1] = avgAngle_ > 0.0f && espOversteer ? true : false;
        g_vehData.mWheelsEsp[2] = avgAngle > 0.0f && espUndersteer ? true : false;
        g_vehData.mWheelsEsp[3] = avgAngle < 0.0f && espUndersteer ? true : false;
        
        if (g_settings.Debug.DisplayInfo) {
            std::string         espString;
            if (espUndersteer)  espString = "U";
            else                espString = "O";
            showText(0.45, 0.75, 1.0, fmt::format("~r~(ESP_{})", espString));
        }
    }
    else if (useTCS && g_settings().DriveAssists.TCS.Mode == 1) {
        if (!MemoryPatcher::BrakePatcher.Patched()) {
            MemoryPatcher::PatchBrake();
        }
        for (int i = 0; i < g_vehData.mWheelCount; i++) {
            if (slipped[i]) {
                g_ext.SetWheelBrakePressure(g_playerVehicle, i, 
                    map(speeds[i], g_vehData.mVelocity.y, g_vehData.mVelocity.y + 2.5f, 0.0f, 0.5f));
                g_vehData.mWheelsTcs[i] = true;
            }
            else {
                g_ext.SetWheelBrakePressure(g_playerVehicle, i, inpBrakeForce);
                g_vehData.mWheelsTcs[i] = false;
            }
        }
        if (g_settings.Debug.DisplayInfo)
            showText(0.45, 0.75, 1.0, "~r~(TCS/B)");
    }
    else if (g_wheelPatchStates.EngBrakeActive) {
        if (!MemoryPatcher::BrakePatcher.Patched()) {
            MemoryPatcher::PatchBrake();
        }
        if (g_settings.Debug.DisplayInfo)
            showText(0.45, 0.75, 1.0, "~r~EngBrake");
    }
    else {
        if (MemoryPatcher::BrakePatcher.Patched()) {
            for (int i = 0; i < g_vehData.mWheelCount; i++) {
                if (g_controls.BrakeVal == 0.0f) {
                    g_ext.SetWheelBrakePressure(g_playerVehicle, i, 0.0f);
                }
            }
            MemoryPatcher::RestoreBrake();
        }
        if (MemoryPatcher::ThrottlePatcher.Patched()) {
            MemoryPatcher::RestoreThrottle();
        }
        for (int i = 0; i < g_vehData.mWheelCount; i++) {
            g_vehData.mWheelsTcs[i] = false;
            g_vehData.mWheelsAbs[i] = false;
            g_vehData.mWheelsEsp[i] = false;
        }
    }
}

// TODO: Change ratios for additional param RPM rise speed
void fakeRev(bool customThrottle, float customThrottleVal) {
    float throttleVal = customThrottle ? customThrottleVal : g_controls.ThrottleVal;
    float timeStep = GAMEPLAY::GET_FRAME_TIME();
    float accelRatio = 2.5f * timeStep;
    float rpmValTemp = g_vehData.mRPMPrev > g_vehData.mRPM ? g_vehData.mRPMPrev - g_vehData.mRPM : 0.0f;
    if (g_vehData.mGearCurr == 1) {			// For some reason, first gear revs slower
        rpmValTemp *= 2.0f;
    }
    float rpmVal = g_vehData.mRPM +			// Base value
        rpmValTemp +						// Keep it constant
        throttleVal * accelRatio;	// Addition value, depends on delta T
    //showText(0.4, 0.4, 1.0, fmt("FakeRev %d %.02f", customThrottle, rpmVal));
    g_ext.SetCurrentRPM(g_playerVehicle, rpmVal);
}

void handleRPM() {
    float clutch = g_controls.ClutchVal;

    // Shifting is only true in Automatic and Sequential mode
    if (g_gearStates.Shifting) {
        if (g_gearStates.ClutchVal > clutch)
            clutch = g_gearStates.ClutchVal;

        // Only lift and blip when no clutch used
        if (g_controls.ClutchVal == 0.0f) {
            if (g_gearStates.ShiftDirection == ShiftDirection::Up &&
                g_settings().ShiftOptions.UpshiftCut) {
                CONTROLS::DISABLE_CONTROL_ACTION(0, ControlVehicleAccelerate, true);
            }
            if (g_gearStates.ShiftDirection == ShiftDirection::Down &&
                g_settings().ShiftOptions.DownshiftBlip) {
                float expectedRPM = g_vehData.mWheelAverageDrivenTyreSpeed / (g_vehData.mDriveMaxFlatVel / g_vehData.mGearRatios[g_vehData.mGearCurr - 1]);
                if (g_vehData.mRPM < expectedRPM * 0.75f)
                    CONTROLS::_SET_CONTROL_NORMAL(0, ControlVehicleAccelerate, 0.66f);
            }
        }
    }

    // Ignores clutch 
    if (!g_gearStates.Shifting) {
        if (g_settings().MTOptions.ShiftMode == EShiftMode::Automatic ||
            g_vehData.mClass == VehicleClass::Bike && g_settings.GameAssists.SimpleBike) {
            clutch = 0.0f;
        }
    }

    //bool skip = false;

    // Game wants to shift up. Triggered at high RPM, high speed.
    // Desired result: high RPM, same gear, no more accelerating
    // Result:	Is as desired. Speed may drop a bit because of game clutch.
    // Update 2017-08-12: We know the gear speeds now, consider patching
    // shiftUp completely?
    if (g_vehData.mGearCurr > 0 &&
        (g_gearStates.HitRPMSpeedLimiter && ENTITY::GET_ENTITY_SPEED(g_playerVehicle) > 2.0f)) {
        g_ext.SetThrottle(g_playerVehicle, 1.0f);
        fakeRev(false, 1.0f);
        CONTROLS::DISABLE_CONTROL_ACTION(0, ControlVehicleAccelerate, true);
        float counterForce = 0.25f*-(static_cast<float>(g_vehData.mGearTop) - static_cast<float>(g_vehData.mGearCurr))/static_cast<float>(g_vehData.mGearTop);
        ENTITY::APPLY_FORCE_TO_ENTITY_CENTER_OF_MASS(g_playerVehicle, 1, 0.0f, counterForce, 0.0f, true, true, true, true);
        //showText(0.4, 0.1, 1.0, "REV LIM SPD");
        //showText(0.4, 0.15, 1.0, "CF: " + std::to_string(counterForce));
    }
    if (g_gearStates.HitRPMLimiter) {
        g_ext.SetCurrentRPM(g_playerVehicle, 1.0f);
        //showText(0.4, 0.1, 1.0, "REV LIM RPM");
    }
    
    /*
        Game doesn't rev on disengaged clutch in any gear but 1
        This workaround tries to emulate this
        Default: vehData.mClutch >= 0.6: Normal
        Default: vehData.mClutch < 0.6: Nothing happens
        Fix: Map 0.0-1.0 to 0.6-1.0 (clutchdata)
        Fix: Map 0.0-1.0 to 1.0-0.6 (control)
    */
    float finalClutch = 1.0f - clutch;
    
    if (g_vehData.mGearCurr > 1) {
        finalClutch = map(clutch, 0.0f, 1.0f, 1.0f, 0.6f);

        // The next statement is a workaround for rolling back + brake + gear > 1 because it shouldn't rev then.
        // Also because we're checking on the game Control accel value and not the pedal position
        bool rollingback = ENTITY::GET_ENTITY_SPEED_VECTOR(g_playerVehicle, true).y < 0.0 && 
                           g_controls.BrakeVal > 0.1f && 
                           g_controls.ThrottleVal > 0.05f;

        // When pressing clutch and throttle, handle clutch and RPM
        if (clutch > 0.4f &&
            g_controls.ThrottleVal > 0.05f &&
            !g_gearStates.FakeNeutral && 
            !rollingback &&
            (!g_gearStates.Shifting || g_controls.ClutchVal > 0.4f)) {
            fakeRev(false, 0);
            g_ext.SetThrottle(g_playerVehicle, g_controls.ThrottleVal);
        }
        // Don't care about clutch slippage, just handle RPM now
        if (g_gearStates.FakeNeutral) {
            fakeRev(false, 0);
            g_ext.SetThrottle(g_playerVehicle, g_controls.ThrottleVal);
        }
    }

    if (g_gearStates.FakeNeutral || clutch >= 1.0f) {
        if (ENTITY::GET_ENTITY_SPEED(g_playerVehicle) < 1.0f) {
            finalClutch = -5.0f;
        }
        else {
            finalClutch = -0.5f;
        }
    }

    g_ext.SetClutch(g_playerVehicle, finalClutch);
}

/*
 * Custom limiter thing
 */
void functionLimiter() {
    g_gearStates.HitRPMLimiter = g_vehData.mRPM > 1.0f;

    if (!g_settings.MTOptions.HardLimiter && 
        (g_vehData.mGearCurr == g_vehData.mGearTop || g_vehData.mGearCurr == 0)) {
        g_gearStates.HitRPMSpeedLimiter = false;
        return;
    }

    auto ratios = g_vehData.mGearRatios;
    float DriveMaxFlatVel = g_vehData.mDriveMaxFlatVel;
    float maxSpeed = DriveMaxFlatVel / ratios[g_vehData.mGearCurr];

    if (ENTITY::GET_ENTITY_SPEED_VECTOR(g_playerVehicle, true).y > maxSpeed && g_vehData.mRPM >= 1.0f) {
        g_gearStates.HitRPMSpeedLimiter = true;
    }
    else {
        g_gearStates.HitRPMSpeedLimiter = false;
    }
}

///////////////////////////////////////////////////////////////////////////////
//                   Mod functions: Control override handling
///////////////////////////////////////////////////////////////////////////////

void functionRealReverse() {
    // Forward gear
    // Desired: Only brake
    if (g_vehData.mGearCurr > 0) {
        // LT behavior when stopped: Just brake
        if (g_controls.BrakeVal > 0.01f && g_controls.ThrottleVal < g_controls.BrakeVal &&
            ENTITY::GET_ENTITY_SPEED_VECTOR(g_playerVehicle, true).y < 0.5f && ENTITY::GET_ENTITY_SPEED_VECTOR(g_playerVehicle, true).y >= -0.5f) { // < 0.5 so reverse never triggers
                                                                    //showText(0.3, 0.3, 0.5, "functionRealReverse: Brake @ Stop");
            CONTROLS::DISABLE_CONTROL_ACTION(0, ControlVehicleBrake, true);
            g_ext.SetThrottleP(g_playerVehicle, 0.1f);
            g_ext.SetBrakeP(g_playerVehicle, 1.0f);
            VEHICLE::SET_VEHICLE_BRAKE_LIGHTS(g_playerVehicle, true);
        }
        // LT behavior when rolling back: Brake
        if (g_controls.BrakeVal > 0.01f && g_controls.ThrottleVal < g_controls.BrakeVal &&
            ENTITY::GET_ENTITY_SPEED_VECTOR(g_playerVehicle, true).y < -0.5f) {
            //showText(0.3, 0.3, 0.5, "functionRealReverse: Brake @ Rollback");
            VEHICLE::SET_VEHICLE_BRAKE_LIGHTS(g_playerVehicle, true);
            CONTROLS::DISABLE_CONTROL_ACTION(0, ControlVehicleBrake, true);
            CONTROLS::_SET_CONTROL_NORMAL(0, ControlVehicleAccelerate, g_controls.BrakeVal);
            g_ext.SetThrottle(g_playerVehicle, 0.0f);
            g_ext.SetThrottleP(g_playerVehicle, 0.1f);
            g_ext.SetBrakeP(g_playerVehicle, 1.0f);
        }
        // RT behavior when rolling back: Burnout
        if (!g_gearStates.FakeNeutral && g_controls.ThrottleVal > 0.5f && !isClutchPressed() &&
            ENTITY::GET_ENTITY_SPEED_VECTOR(g_playerVehicle, true).y < -1.0f ) {
            //showText(0.3, 0.3, 0.5, "functionRealReverse: Throttle @ Rollback");
            //CONTROLS::_SET_CONTROL_NORMAL(0, ControlVehicleBrake, carControls.ThrottleVal);
            if (g_controls.BrakeVal < 0.1f) {
                VEHICLE::SET_VEHICLE_BRAKE_LIGHTS(g_playerVehicle, false);
            }
            for (int i = 0; i < g_vehData.mWheelCount; i++) {
                if (g_ext.IsWheelPowered(g_playerVehicle, i)) {
                    g_ext.SetWheelBrakePressure(g_playerVehicle, i, 0.0f);
                    g_ext.SetWheelPower(g_playerVehicle, i, 2.0f * g_ext.GetDriveForce(g_playerVehicle));
                }
                else {
                    float handlingBrakeForce = *reinterpret_cast<float*>(g_vehData.mHandlingPtr + hOffsets.fBrakeForce);
                    float inpBrakeForce = handlingBrakeForce * g_controls.BrakeVal;
                    g_ext.SetWheelPower(g_playerVehicle, i, 0.0f);
                    g_ext.SetWheelBrakePressure(g_playerVehicle, i, inpBrakeForce);
                }
            }
            fakeRev();
            g_ext.SetThrottle(g_playerVehicle, g_controls.ThrottleVal);
            g_ext.SetThrottleP(g_playerVehicle, g_controls.ThrottleVal);
            g_wheelPatchStates.InduceBurnout = true;
        }
        else {
            g_wheelPatchStates.InduceBurnout = false;
        }
    }
    // Reverse gear
    // Desired: RT reverses, LT brakes
    if (g_vehData.mGearCurr == 0) {
        // Enables reverse lights
        g_ext.SetThrottleP(g_playerVehicle, -0.1f);
        // RT behavior
        int throttleAndSomeBrake = 0;
        if (g_controls.ThrottleVal > 0.01f && g_controls.ThrottleVal > g_controls.BrakeVal) {
            throttleAndSomeBrake++;
            //showText(0.3, 0.3, 0.5, "functionRealReverse: Throttle @ Active Reverse");

            CONTROLS::DISABLE_CONTROL_ACTION(0, ControlVehicleAccelerate, true);
            CONTROLS::_SET_CONTROL_NORMAL(0, ControlVehicleBrake, g_controls.ThrottleVal);
        }
        // LT behavior when reversing
        if (g_controls.BrakeVal > 0.01f &&
            ENTITY::GET_ENTITY_SPEED_VECTOR(g_playerVehicle, true).y <= -0.5f) {
            throttleAndSomeBrake++;
            //showText(0.3, 0.35, 0.5, "functionRealReverse: Brake @ Reverse");

            CONTROLS::DISABLE_CONTROL_ACTION(0, ControlVehicleBrake, true);
            CONTROLS::_SET_CONTROL_NORMAL(0, ControlVehicleAccelerate, g_controls.BrakeVal);
        }
        // Throttle > brake  && BrakeVal > 0.1f
        if (throttleAndSomeBrake >= 2) {
            //showText(0.3, 0.4, 0.5, "functionRealReverse: Weird combo + rev it");

            CONTROLS::ENABLE_CONTROL_ACTION(0, ControlVehicleAccelerate, true);
            CONTROLS::_SET_CONTROL_NORMAL(0, ControlVehicleAccelerate, g_controls.BrakeVal);
            fakeRev(false, 0);
        }

        // LT behavior when forward
        if (g_controls.BrakeVal > 0.01f && g_controls.ThrottleVal <= g_controls.BrakeVal &&
            ENTITY::GET_ENTITY_SPEED_VECTOR(g_playerVehicle, true).y > 0.1f) {
            //showText(0.3, 0.3, 0.5, "functionRealReverse: Brake @ Rollforwrd");

            VEHICLE::SET_VEHICLE_BRAKE_LIGHTS(g_playerVehicle, true);
            CONTROLS::_SET_CONTROL_NORMAL(0, ControlVehicleBrake, g_controls.BrakeVal);

            //CONTROLS::DISABLE_CONTROL_ACTION(0, ControlVehicleBrake, true);
            g_ext.SetBrakeP(g_playerVehicle, 1.0f);
        }

        // LT behavior when still
        if (g_controls.BrakeVal > 0.01f && g_controls.ThrottleVal <= g_controls.BrakeVal &&
            ENTITY::GET_ENTITY_SPEED_VECTOR(g_playerVehicle, true).y > -0.5f && ENTITY::GET_ENTITY_SPEED_VECTOR(g_playerVehicle, true).y <= 0.1f) {
            //showText(0.3, 0.3, 0.5, "functionRealReverse: Brake @ Stopped");

            VEHICLE::SET_VEHICLE_BRAKE_LIGHTS(g_playerVehicle, true);
            CONTROLS::DISABLE_CONTROL_ACTION(0, ControlVehicleBrake, true);
            g_ext.SetBrakeP(g_playerVehicle, 1.0f);
        }
    }
}

void functionAutoReverse() {
    // Go forward
    if (CONTROLS::IS_CONTROL_PRESSED(0, ControlVehicleAccelerate) && 
        !CONTROLS::IS_CONTROL_PRESSED(0, ControlVehicleBrake) &&
        ENTITY::GET_ENTITY_SPEED_VECTOR(g_playerVehicle, true).y > -1.0f &&
        g_vehData.mGearCurr == 0) {
        shiftTo(1, false);
    }

    // Reverse
    if (CONTROLS::IS_CONTROL_PRESSED(0, ControlVehicleBrake) && 
        !CONTROLS::IS_CONTROL_PRESSED(0, ControlVehicleAccelerate) &&
        ENTITY::GET_ENTITY_SPEED_VECTOR(g_playerVehicle, true).y < 1.0f &&
        g_vehData.mGearCurr > 0) {
        g_gearStates.FakeNeutral = false;
        shiftTo(0, false);
    }
}

///////////////////////////////////////////////////////////////////////////////
//                       Mod functions: Buttons
///////////////////////////////////////////////////////////////////////////////
// Blocks some vehicle controls on tapping, tries to activate them when holding.
// TODO: Some original "tap" controls don't work.
void blockButtons() {
    if (!g_settings.MTOptions.Enable || !g_settings.Controller.BlockCarControls || g_vehData.mDomain != VehicleDomain::Road ||
        g_controls.PrevInput != CarControls::Controller || !Util::VehicleAvailable(g_playerVehicle, g_playerPed)) {
        return;
    }
    if (g_settings().MTOptions.ShiftMode == EShiftMode::Automatic && g_vehData.mGearCurr > 1) {
        return;
    }

    if (g_settings.Controller.Native.Enable) {
        for (int i = 0; i < static_cast<int>(CarControls::LegacyControlType::SIZEOF_LegacyControlType); i++) {
            if (g_controls.ControlNativeBlocks[i] == -1) continue;
            if (i != static_cast<int>(CarControls::LegacyControlType::ShiftUp) && 
                i != static_cast<int>(CarControls::LegacyControlType::ShiftDown)) continue;

            if (g_controls.ButtonHeldOver(static_cast<CarControls::LegacyControlType>(i), g_settings.Controller.HoldTimeMs)) {
                CONTROLS::_SET_CONTROL_NORMAL(0, g_controls.ControlNativeBlocks[i], 1.0f);
            }
            else {
                CONTROLS::DISABLE_CONTROL_ACTION(0, g_controls.ControlNativeBlocks[i], true);
            }

            if (g_controls.ButtonReleasedAfter(static_cast<CarControls::LegacyControlType>(i), g_settings.Controller.HoldTimeMs)) {
                // todo
            }
        }
        if (g_controls.ControlNativeBlocks[static_cast<int>(CarControls::LegacyControlType::Clutch)] != -1) {
            CONTROLS::DISABLE_CONTROL_ACTION(0, g_controls.ControlNativeBlocks[static_cast<int>(CarControls::LegacyControlType::Clutch)], true);
        }
    }
    else {
        for (int i = 0; i < static_cast<int>(CarControls::ControllerControlType::SIZEOF_ControllerControlType); i++) {
            if (g_controls.ControlXboxBlocks[i] == -1) continue;
            if (i != static_cast<int>(CarControls::ControllerControlType::ShiftUp) && 
                i != static_cast<int>(CarControls::ControllerControlType::ShiftDown)) continue;

            if (g_controls.ButtonHeldOver(static_cast<CarControls::ControllerControlType>(i), g_settings.Controller.HoldTimeMs)) {
                CONTROLS::_SET_CONTROL_NORMAL(0, g_controls.ControlXboxBlocks[i], 1.0f);
            }
            else {
                CONTROLS::DISABLE_CONTROL_ACTION(0, g_controls.ControlXboxBlocks[i], true);
            }

            if (g_controls.ButtonReleasedAfter(static_cast<CarControls::ControllerControlType>(i), g_settings.Controller.HoldTimeMs)) {
                // todo
            }
        }
        if (g_controls.ControlXboxBlocks[static_cast<int>(CarControls::ControllerControlType::Clutch)] != -1) {
            CONTROLS::DISABLE_CONTROL_ACTION(0, g_controls.ControlXboxBlocks[static_cast<int>(CarControls::ControllerControlType::Clutch)], true);
        }
    }
}

// TODO: Proper held values from settings or something
void startStopEngine() {
    bool prevController = g_controls.PrevInput == CarControls::Controller;
    bool prevKeyboard = g_controls.PrevInput == CarControls::Keyboard;
    bool prevWheel = g_controls.PrevInput == CarControls::Wheel;

    bool heldControllerXinput = g_controls.ButtonHeldOver(CarControls::ControllerControlType::Engine, g_settings.Controller.HoldTimeMs);
    bool heldControllerNative = g_controls.ButtonHeldOver(CarControls::LegacyControlType::Engine, g_settings.Controller.HoldTimeMs);

    bool pressedKeyboard = g_controls.ButtonJustPressed(CarControls::KeyboardControlType::Engine);
    bool pressedWheel = g_controls.ButtonJustPressed(CarControls::WheelControlType::Engine);

    bool controllerActive = prevController && heldControllerXinput || prevController && heldControllerNative;
    bool keyboardActive = prevKeyboard && pressedKeyboard;
    bool wheelActive = prevWheel && pressedWheel;

    bool throttleStart = false;
    if (g_settings.GameAssists.ThrottleStart) {
        bool clutchedIn = isClutchPressed();
        bool freeGear = clutchedIn || g_gearStates.FakeNeutral;
        throttleStart = g_controls.ThrottleVal > 0.75f && freeGear;
    }

    if (!VEHICLE::GET_IS_VEHICLE_ENGINE_RUNNING(g_playerVehicle) &&
        (controllerActive || keyboardActive || wheelActive || throttleStart)) {
        VEHICLE::SET_VEHICLE_ENGINE_ON(g_playerVehicle, true, false, true);
    }

    if (VEHICLE::GET_IS_VEHICLE_ENGINE_RUNNING(g_playerVehicle) &&
        (controllerActive && g_settings.Controller.ToggleEngine || keyboardActive || wheelActive)) {
        VEHICLE::SET_VEHICLE_ENGINE_ON(g_playerVehicle, false, true, true);
    }
}



///////////////////////////////////////////////////////////////////////////////
//                             Misc features
///////////////////////////////////////////////////////////////////////////////

void functionAutoLookback() {
    if (g_vehData.mGearTop > 0 && g_vehData.mGearCurr == 0) {
        CONTROLS::_SET_CONTROL_NORMAL(0, ControlVehicleLookBehind, 1.0f);
    }
}

void functionAutoGear1() {
    if (g_vehData.mThrottle < 0.1f && ENTITY::GET_ENTITY_SPEED(g_playerVehicle) < 0.1f && g_vehData.mGearCurr > 1) {
        shiftTo(1, false);
    }
}

void functionHillGravity() {
    // TODO: Needs improvement/proper fix
    if (g_controls.BrakeVal == 0.0f
        && ENTITY::GET_ENTITY_SPEED(g_playerVehicle) < 2.0f &&
        VEHICLE::IS_VEHICLE_ON_ALL_WHEELS(g_playerVehicle)) {
        float pitch = ENTITY::GET_ENTITY_PITCH(g_playerVehicle);;

        float clutchNeutral = g_gearStates.FakeNeutral ? 1.0f : g_controls.ClutchVal;
        if (pitch < 0 || clutchNeutral) {
            ENTITY::APPLY_FORCE_TO_ENTITY_CENTER_OF_MASS(
                g_playerVehicle, 1, 0.0f, -1 * (pitch / 150.0f) * 1.1f * clutchNeutral, 0.0f, true, true, true, true);
        }
        if (pitch > 10.0f || clutchNeutral)
            ENTITY::APPLY_FORCE_TO_ENTITY_CENTER_OF_MASS(
            g_playerVehicle, 1, 0.0f, -1 * (pitch / 90.0f) * 0.35f * clutchNeutral, 0.0f, true, true, true, true);
    }
}

int prevCameraMode;

void functionHidePlayerInFPV(bool optionToggled) {
    bool shouldRun = false;

    int cameraMode = CAM::GET_FOLLOW_PED_CAM_VIEW_MODE();
    if (prevCameraMode != cameraMode) {
        shouldRun = true;
    }
    prevCameraMode = cameraMode;

    if (optionToggled) {
        shouldRun = true;
    }

    if (g_settings.Debug.DisablePlayerHide) {
        shouldRun = false;
    }

    if (!shouldRun)
        return;

    bool visible = ENTITY::IS_ENTITY_VISIBLE(g_playerPed);
    bool shouldHide = false;

    if (g_settings.GameAssists.HidePlayerInFPV && CAM::GET_FOLLOW_PED_CAM_VIEW_MODE() == 4 && Util::VehicleAvailable(g_playerVehicle, g_playerPed)) {
        shouldHide = true;
    }

    if (visible && shouldHide) {
        ENTITY::SET_ENTITY_VISIBLE(g_playerPed, false, false);
    }
    if (!visible && !shouldHide) {
        ENTITY::SET_ENTITY_VISIBLE(g_playerPed, true, false);
    }
}

///////////////////////////////////////////////////////////////////////////////
//                              Script entry
///////////////////////////////////////////////////////////////////////////////

void loadConfigs() {
    logger.Write(DEBUG, "Clearing and reloading vehicle configs...");
    g_vehConfigs.clear();
    const std::string absoluteModPath = Paths::GetModuleFolder(Paths::GetOurModuleHandle()) + Constants::ModDir;
    const std::string vehConfigsPath = absoluteModPath + "\\Vehicles";

    if (!(fs::exists(fs::path(vehConfigsPath)) && fs::is_directory(fs::path(vehConfigsPath)))) {
        logger.Write(WARN, "Directory [%s] not found!", vehConfigsPath.c_str());
        return;
    }

    for (auto& file : fs::directory_iterator(vehConfigsPath)) {
        if (StrUtil::toLower(fs::path(file).extension().string()) != ".ini")
            continue;

        VehicleConfig config(g_settings, file.path().string());
        if (config.ModelNames.empty() && config.Plates.empty()) {
            logger.Write(WARN,
                "Vehicle settings file [%s] contained no model names or plates, skipping...",
                file.path().string().c_str());
            continue;
        }
        g_vehConfigs.push_back(config);
        logger.Write(DEBUG, "Loaded vehicle config [%s]", config.Name.c_str());
    }
    logger.Write(INFO, "Configs loaded: %d", g_vehConfigs.size());
    setVehicleConfig(g_playerVehicle);
}

// Always call *after* settings have been (re)loaded
void initTimers() {
    g_speedTimers.clear();
    if (g_settings.Debug.Metrics.EnableTimers) {
        for (const auto& params : g_settings.Debug.Metrics.Timers) {
            g_speedTimers.emplace_back(params.Unit,
                [](const std::string& msg) { UI::Notify(INFO, msg, false); },
                params.LimA, params.LimB, params.Tolerance);
        }
    }
}

void readSettings() {
    g_settings.Read(&g_controls);
    if (g_settings.Debug.LogLevel > 4)
        g_settings.Debug.LogLevel = 1;
    logger.SetMinLevel(static_cast<LogLevel>(g_settings.Debug.LogLevel));

    g_gearStates.FakeNeutral = g_settings.GameAssists.DefaultNeutral;
    g_menu.ReadSettings();
    initTimers();
    logger.Write(INFO, "Settings read");
}

void threadCheckUpdate(unsigned milliseconds) {
    std::thread([milliseconds]() {
        std::lock_guard releaseInfoLock(g_releaseInfoMutex);
        std::lock_guard checkUpdateLock(g_checkUpdateDoneMutex);
        std::lock_guard notifyUpdateLock(g_notifyUpdateMutex);

        std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));

        bool newAvailable = CheckUpdate(g_releaseInfo);

        if (newAvailable && g_settings.Update.IgnoredVersion != g_releaseInfo.Version) {
            g_notifyUpdate = true;
        }
        g_checkUpdateDone = true;
    }).detach();
}

void update_update_notification() {
    std::unique_lock releaseInfoLock(g_releaseInfoMutex, std::try_to_lock);
    std::unique_lock checkUpdateLock(g_checkUpdateDoneMutex, std::try_to_lock);
    std::unique_lock notifyUpdateLock(g_notifyUpdateMutex, std::try_to_lock);

    if (checkUpdateLock.owns_lock() && releaseInfoLock.owns_lock() && notifyUpdateLock.owns_lock()) {
        if (g_checkUpdateDone) {
            g_checkUpdateDone = false;
            if (g_notifyUpdate) {
                UI::Notify(INFO, fmt::format("Manual Transmission: Update available, new version: {}.",
                                         g_releaseInfo.Version), false);
            }
            else if (g_releaseInfo.Version.empty()) {
                UI::Notify(INFO, "Manual Transmission: Failed to check for update.", false);
            }
            else {
                UI::Notify(INFO, fmt::format("Manual Transmission: No update available, latest version: {}.",
                                         g_releaseInfo.Version), false);
            }
        }
    }
}

void main() {
    logger.Write(INFO, "Script started");
    std::string absoluteModPath = Paths::GetModuleFolder(Paths::GetOurModuleHandle()) + Constants::ModDir;
    std::string settingsGeneralFile = absoluteModPath + "\\settings_general.ini";
    std::string settingsControlsFile = absoluteModPath + "\\settings_controls.ini";
    std::string settingsWheelFile = absoluteModPath + "\\settings_wheel.ini";
    std::string settingsMenuFile = absoluteModPath + "\\settings_menu.ini";
    std::string textureWheelFile = absoluteModPath + "\\texture_wheel.png";
    std::string textureABSFile = absoluteModPath + "\\texture_abs.png";
    std::string textureTCSFile = absoluteModPath + "\\texture_tcs.png";
    std::string textureESPFile = absoluteModPath + "\\texture_esp.png";
    std::string textureBRKFile = absoluteModPath + "\\texture_handbrake.png";

    g_settings.SetFiles(settingsGeneralFile, settingsControlsFile,settingsWheelFile);
    g_menu.RegisterOnMain([] { onMenuInit(); });
    g_menu.RegisterOnExit([] { onMenuClose(); });
    g_menu.SetFiles(settingsMenuFile);
    g_menu.Initialize();
    readSettings();
    loadConfigs();

    if (g_settings.Update.EnableUpdate) {
        threadCheckUpdate(10000);
    }

    g_ext.initOffsets();
    if (!MemoryPatcher::Test()) {
        logger.Write(ERROR, "Patchability test failed!");
        MemoryPatcher::Error = true;
    }

    setupCompatibility();

    initWheel();

    if (FileExists(textureWheelFile)) {
        g_textureWheelId = createTexture(textureWheelFile.c_str());
    }
    else {
        logger.Write(ERROR, textureWheelFile + " does not exist.");
        g_textureWheelId = -1;
    }

    if (FileExists(textureABSFile)) {
        g_textureAbsId = createTexture(textureABSFile.c_str());
    }
    else {
        logger.Write(ERROR, textureABSFile + " does not exist.");
        g_textureAbsId = -1;
    }

    if (FileExists(textureTCSFile)) {
        g_textureTcsId = createTexture(textureTCSFile.c_str());
    }
    else {
        logger.Write(ERROR, textureTCSFile + " does not exist.");
        g_textureTcsId = -1;
    }

    if (FileExists(textureESPFile)) {
        g_textureEspId = createTexture(textureESPFile.c_str());
    }
    else {
        logger.Write(ERROR, textureESPFile + " does not exist.");
        g_textureEspId = -1;
    }

    if (FileExists(textureBRKFile)) {
        g_textureBrkId = createTexture(textureBRKFile.c_str());
    }
    else {
        logger.Write(ERROR, textureBRKFile + " does not exist.");
        g_textureBrkId = -1;
    }

    g_focused = SysUtil::IsWindowFocused();

    logger.Write(DEBUG, "START: Starting with MT:  %s", g_settings.MTOptions.Enable ? "ON" : "OFF");
    logger.Write(INFO, "START: Initialization finished");

    while (true) {
        update_player();
        update_vehicle();
        update_inputs();
        update_steering();
        update_hud();
        update_input_controls();
        update_manual_transmission();
        update_misc_features();
        update_menu();
        update_update_notification();
        WAIT(0);
    }
}

void ScriptMain() {
    srand(GetTickCount());
    main();
}
