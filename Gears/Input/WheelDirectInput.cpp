#include "WheelDirectInput.hpp"
#include "../Util/TimeHelper.hpp"
#include "../Util/Logger.hpp"
#include <sstream>

#include <winerror.h>
#include <chrono>
#include <vector>

// TO/DO Force feedback enumeration
// Update: FFB Enumeration is skipped in favor of a user-specified axis

// TO/DO Fix unknown/non-G27 layout detections
// Update: G920 gives trouble, G29 "works perfectly". Not sure, but all DInput
// axes are handled. Anyway if this to-do has been here for a while, it's been
// handled by the new all-inputs-have-a-device set up.

// TODO Look into crashes
// Update: G920 gives trouble. Disable RPM LEDs on demand. Untested but should
// be fixed if writing to random addresses didn't already hurt.

WheelDirectInput::WheelDirectInput(Logger &logAlt) : nEntry(0),
                                                    logger(logAlt),
                                                    pCFEffect{nullptr},
                                                    pFREffect{nullptr} { }

WheelDirectInput::~WheelDirectInput() {

}

bool WheelDirectInput::InitWheel() {
	logger.Write("WHEEL: Init steering wheel"); 
	
	// Setting up DirectInput Object
	if (FAILED(DirectInput8Create(GetModuleHandle(nullptr),
		DIRECTINPUT_VERSION,
		IID_IDirectInput8,
		reinterpret_cast<void**>(&lpDi),
		nullptr))) {
		NoFeedback = true;
		logger.Write("WHEEL: DirectInput create failed");
		return false;
	}

	foundGuids.clear();
	djs.enumerate(lpDi);
	nEntry = djs.getEntryCount();
	logger.Write("WHEEL: Found " + std::to_string(nEntry) + " device(s)");

	if (nEntry < 1) {
		NoFeedback = true;
		logger.Write("WHEEL: No wheel detected");
		return false;
	}

	for (int i = 0; i < nEntry; i++) {
		auto device = djs.getEntry(i);
		std::wstring wDevName = device->diDeviceInstance.tszInstanceName;
		logger.Write("WHEEL: Device: " + std::string(wDevName.begin(), wDevName.end()));

		GUID guid = device->diDeviceInstance.guidInstance;
		wchar_t szGuidW[40] = { 0 };
		StringFromGUID2(guid, szGuidW, 40);
		std::wstring wGuid = szGuidW;//std::wstring(szGuidW);
		logger.Write("WHEEL: GUID:   " + std::string(wGuid.begin(), wGuid.end()));
		foundGuids.push_back(guid);
	}

	djs.update();
	logger.Write("WHEEL: Init steering wheel success");
	return true;
}

bool WheelDirectInput::InitFFB(GUID guid, DIAxis ffAxis) {
	logger.Write("WHEEL: Init FFB device");
	auto e = FindEntryFromGUID(guid);
	
	if (!e) {
		logger.Write("WHEEL: FFB device not found");
		return false;
	}

	e->diDevice->Unacquire();
	HRESULT hr;
	if (FAILED(hr = e->diDevice->SetCooperativeLevel(
			GetForegroundWindow(),
		DISCL_EXCLUSIVE | DISCL_FOREGROUND))) {
		std::string hrStr;
		switch (hr) {
			case DI_OK: hrStr = "DI_OK";
				break;
			case DIERR_INVALIDPARAM: hrStr = "DIERR_INVALIDPARAM";
				break;
			case DIERR_NOTINITIALIZED: hrStr = "DIERR_NOTINITIALIZED";
				break;
			case DIERR_ALREADYINITIALIZED: hrStr = "DIERR_ALREADYINITIALIZED";
				break;
			case DIERR_INPUTLOST: hrStr = "DIERR_INPUTLOST";
				break;
			case DIERR_ACQUIRED: hrStr = "DIERR_ACQUIRED";
				break;
			case DIERR_NOTACQUIRED: hrStr = "DIERR_NOTACQUIRED";
				break;
			case E_HANDLE: hrStr = "E_HANDLE";
				break;
			default: hrStr = "UNKNOWN";
				break;
		}
		logger.Write("WHEEL: Acquire FFB device error");
		logger.Write("WHEEL: HRESULT = " + hrStr);
		std::stringstream ss;
		ss << std::hex << hr;
		logger.Write("WHEEL: ERRCODE = " + ss.str());
		ss.str(std::string());
		ss << std::hex << GetForegroundWindow();
		logger.Write("WHEEL: HWND =    " + ss.str());
		return false;
	}
	logger.Write("WHEEL: Init FFB effect on axis " + DIAxisHelper[ffAxis]);
	if (!CreateConstantForceEffect(e, ffAxis)) {
		logger.Write("WHEEL: Init FFB effect failed");
		NoFeedback = true;
	} else {
		logger.Write("WHEEL: Init FFB success");
	}
	return true;
}

void WheelDirectInput::UpdateCenterSteering(GUID guid, DIAxis steerAxis) {
	UpdateState(); // TODO: I don't understand
	UpdateState(); // Why do I need to call this twice?
	prevTime = std::chrono::steady_clock::now().time_since_epoch().count(); // 1ns
	prevPosition = GetAxisValue(steerAxis, guid);
}

/*
 * Return NULL when device isn't found
 */
const DiJoyStick::Entry *WheelDirectInput::FindEntryFromGUID(GUID guid) {
	if (nEntry > 0) {
		if (guid == GUID_NULL) {
			return nullptr;
		}

		for (int i = 0; i < nEntry; i++) {
			auto tempEntry = djs.getEntry(i);
			if (guid == tempEntry->diDeviceInstance.guidInstance) {
				return tempEntry;
			}
		}
	}
	return nullptr;
}

void WheelDirectInput::UpdateState() {
	djs.update();
}

bool WheelDirectInput::IsConnected(GUID device) {
	auto e = FindEntryFromGUID(device);
	if (!e) {
		return false;
	}
	return true;
}

// Mental note: buttonType in these args means physical button number
// like how they are in DirectInput.
// If it matches the cardinal stuff the button is a POV hat thing

bool WheelDirectInput::IsButtonPressed(int buttonType, GUID device) {
	auto e = FindEntryFromGUID(device);

	if (!e) {
		/*wchar_t szGuidW[40] = { 0 };
		StringFromGUID2(device, szGuidW, 40);
		std::wstring wGuid = szGuidW;
		log.Write("DBG: Button " + std::to_string(buttonType) + " with GUID " + std::string(wGuid.begin(), wGuid.end()));*/
		return false;
	}

	if (buttonType > 127) {
		switch (buttonType) {
			case N:
				if (e->joystate.rgdwPOV[0] == 0) {
					return true;
				}
			case NE:
			case E:
			case SE:
			case S:
			case SW:
			case W:
			case NW:
				if (buttonType == e->joystate.rgdwPOV[0])
					return true;
			default:
				return false;
		}
	}
	if (e->joystate.rgbButtons[buttonType])
		return true;
	return false;
}

bool WheelDirectInput::IsButtonJustPressed(int buttonType, GUID device) {
	if (buttonType > 127) { // POV
		povButtonCurr[buttonType] = IsButtonPressed(buttonType,device);

		// raising edge
		if (povButtonCurr[buttonType] && !povButtonPrev[buttonType]) {
			return true;
		}
		return false;
	}
	rgbButtonCurr[buttonType] = IsButtonPressed(buttonType,device);

	// raising edge
	if (rgbButtonCurr[buttonType] && !rgbButtonPrev[buttonType]) {
		return true;
	}
	return false;
}

bool WheelDirectInput::IsButtonJustReleased(int buttonType, GUID device) {
	if (buttonType > 127) { // POV
		povButtonCurr[buttonType] = IsButtonPressed(buttonType,device);

		// falling edge
		if (!povButtonCurr[buttonType] && povButtonPrev[buttonType]) {
			return true;
		}
		return false;
	}
	rgbButtonCurr[buttonType] = IsButtonPressed(buttonType,device);

	// falling edge
	if (!rgbButtonCurr[buttonType] && rgbButtonPrev[buttonType]) {
		return true;
	}
	return false;
}

bool WheelDirectInput::WasButtonHeldForMs(int buttonType, GUID device, int millis) {
	if (buttonType > 127) { // POV
		if (IsButtonJustPressed(buttonType,device)) {
			povPressTime[buttonType] = milliseconds_now();
		}
		if (IsButtonJustReleased(buttonType,device)) {
			povReleaseTime[buttonType] = milliseconds_now();
		}

		if ((povReleaseTime[buttonType] - povPressTime[buttonType]) >= millis) {
			povPressTime[buttonType] = 0;
			povReleaseTime[buttonType] = 0;
			return true;
		}
		return false;
	}
	if (IsButtonJustPressed(buttonType,device)) {
		rgbPressTime[buttonType] = milliseconds_now();
	}
	if (IsButtonJustReleased(buttonType,device)) {
		rgbReleaseTime[buttonType] = milliseconds_now();
	}

	if ((rgbReleaseTime[buttonType] - rgbPressTime[buttonType]) >= millis) {
		rgbPressTime[buttonType] = 0;
		rgbReleaseTime[buttonType] = 0;
		return true;
	}
	return false;
}

void WheelDirectInput::UpdateButtonChangeStates() {
	for (int i = 0; i < MAX_RGBBUTTONS; i++) {
		rgbButtonPrev[i] = rgbButtonCurr[i];
	}
	for (int i = 0; i < SIZEOF_POV; i++) {
		povButtonPrev[i] = povButtonCurr[i];
	}
}

bool WheelDirectInput::CreateConstantForceEffect(const DiJoyStick::Entry *e, DIAxis ffAxis) {
	if (!e || NoFeedback)
		return false;

	// This somehow doesn't work well
	/*DIDEVCAPS didevcaps;

	e->diDevice->GetCapabilities(&didevcaps);

	if (!(didevcaps.dwFlags & DIDC_FORCEFEEDBACK)) {
		logger.Write("Device doesn't have force feedback");
		return false;
	}*/

	DWORD axis;
	if (ffAxis == lX) {
		axis = DIJOFS_X;
	}
	else if (ffAxis == lY) {
		axis = DIJOFS_Y;
	}
	else if (ffAxis == lZ) {
		axis = DIJOFS_Z;
	}
	else if (ffAxis == lRx) {
		axis = DIJOFS_RX;
	}
	else if (ffAxis == lRy) {
		axis = DIJOFS_RY;
	}
	else if (ffAxis == lRz) {
		axis = DIJOFS_RZ;
	}
	else {
		return false;
	}

	DWORD rgdwAxes[1] = { axis };
	LONG rglDirection[1] = {0};
	DICONSTANTFORCE cf = {0};

	DIEFFECT eff;
	ZeroMemory(&eff, sizeof(eff));
	eff.dwSize = sizeof(DIEFFECT);
	eff.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
	eff.dwDuration = INFINITE;
	eff.dwSamplePeriod = 0;
	eff.dwGain = DI_FFNOMINALMAX;
	eff.dwTriggerButton = DIEB_NOTRIGGER;
	eff.dwTriggerRepeatInterval = 0;
	eff.cAxes = 1;
	eff.rgdwAxes = rgdwAxes;
	eff.rglDirection = rglDirection;
	eff.lpEnvelope = nullptr;
	eff.cbTypeSpecificParams = sizeof(DICONSTANTFORCE);
	eff.lpvTypeSpecificParams = &cf;
	eff.dwStartDelay = 0;

	e->diDevice->CreateEffect(
			GUID_ConstantForce,
			&eff,
			&pCFEffect,
			nullptr);

	if (!pCFEffect) {
		return false;
	}

	return true;
}

HRESULT WheelDirectInput::SetConstantForce(GUID device, int force) {
	auto e = FindEntryFromGUID(device);
	if (!e)
		return E_HANDLE;

	if (NoFeedback)
		return false;

	HRESULT hr;
	LONG rglDirection[1] = {0};


	DICONSTANTFORCE cf;
	cf.lMagnitude = force;

	DIEFFECT cfEffect;
	ZeroMemory(&cfEffect, sizeof(cfEffect));
	cfEffect.dwSize = sizeof(DIEFFECT);
	cfEffect.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
	cfEffect.cAxes = 1;
	cfEffect.rglDirection = rglDirection;
	cfEffect.lpEnvelope = nullptr;
	cfEffect.cbTypeSpecificParams = sizeof(DICONSTANTFORCE);
	cfEffect.lpvTypeSpecificParams = &cf;
	cfEffect.dwStartDelay = 0;

	hr = pCFEffect->SetParameters(&cfEffect, DIEP_DIRECTION |
	                              DIEP_TYPESPECIFICPARAMS |
	                              DIEP_START);

	e->diDevice->Acquire();
	if (pCFEffect)
		pCFEffect->Start(1, 0);

	return hr;
}

WheelDirectInput::DIAxis WheelDirectInput::StringToAxis(std::string &axisString) {
	for (int i = 0; i < SIZEOF_DIAxis; i++) {
		if (axisString == DIAxisHelper[i]) {
			return static_cast<DIAxis>(i);
		}
	}
	return UNKNOWN_AXIS;
}

// -1 means device not accessible
int WheelDirectInput::GetAxisValue(DIAxis axis, GUID device) {
	auto e = FindEntryFromGUID(device);
	if (!IsConnected(device) || e == nullptr)
		return -1;
	switch (axis) {
		case lX: return  e->joystate.lX;
		case lY: return  e->joystate.lY;
		case lZ: return  e->joystate.lZ;
		case lRx: return e->joystate.lRx;
		case lRy: return e->joystate.lRy;
		case lRz: return e->joystate.lRz;
		case rglSlider0: return e->joystate.rglSlider[0];
		case rglSlider1: return e->joystate.rglSlider[1];
		default: return 0;
	}
}

// Returns in units/s
float WheelDirectInput::GetAxisSpeed(DIAxis axis, GUID device) {
	auto time = std::chrono::steady_clock::now().time_since_epoch().count(); // 1ns
	auto position = GetAxisValue(axis , device);
	auto result = (position - prevPosition) / ((time - prevTime) / 1e9f);

	prevTime = time;
	prevPosition = position;

	samples[averageIndex] = result;
	averageIndex = (averageIndex + 1) % (SAMPLES - 1);

	//return result;
	auto sum = 0.0f;
	for (auto i = 0; i < SAMPLES; i++) {
		sum += samples[i];
	}
	return sum / SAMPLES;
}

std::vector<GUID> WheelDirectInput::GetGuids() {
	return foundGuids;
}

// Only confirmed to work on my own G27
void WheelDirectInput::PlayLedsDInput(GUID guid, const FLOAT currentRPM, const FLOAT rpmFirstLedTurnsOn, const FLOAT rpmRedLine) {
	auto e = FindEntryFromGUID(guid);

	if (!e)
		return;

	CONST DWORD ESCAPE_COMMAND_LEDS = 0;
	CONST DWORD LEDS_VERSION_NUMBER = 0x00000001;

	struct LedsRpmData
	{
		FLOAT currentRPM;
		FLOAT rpmFirstLedTurnsOn;
		FLOAT rpmRedLine;
	};

	struct WheelData
	{
		DWORD size;
		DWORD versionNbr;
		LedsRpmData rpmData;
	};

	
	WheelData wheelData_;
	ZeroMemory(&wheelData_, sizeof(wheelData_));

	wheelData_.size = sizeof(WheelData);
	wheelData_.versionNbr = LEDS_VERSION_NUMBER;
	wheelData_.rpmData.currentRPM = currentRPM;
	wheelData_.rpmData.rpmFirstLedTurnsOn = rpmFirstLedTurnsOn;
	wheelData_.rpmData.rpmRedLine = rpmRedLine;

	DIEFFESCAPE data_;
	ZeroMemory(&data_, sizeof(data_));

	data_.dwSize = sizeof(DIEFFESCAPE);
	data_.dwCommand = ESCAPE_COMMAND_LEDS;
	data_.lpvInBuffer = &wheelData_;
	data_.cbInBuffer = sizeof(wheelData_);

	//HRESULT hr;
	//hr = e->diDevice->Escape(&data_);
	e->diDevice->Escape(&data_);
}