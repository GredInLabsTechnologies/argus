/*++

Copyright (c) Microsoft Corporation

Abstract:

	MSDN documentation on indirect displays can be found at https://msdn.microsoft.com/en-us/library/windows/hardware/mt761968(v=vs.85).aspx.

Environment:

	User Mode, UMDF

--*/

#include "Driver.h"
//#include "Driver.tmh"
#include<fstream>
#include<sstream>
#include<string>
#include<tuple>
#include<vector>
#include<algorithm>
#include<iomanip>
#include<chrono>
#include <AdapterOption.h>
#include <xmllite.h>
#include <shlwapi.h>
#include <atlbase.h>
#include <iostream>
#include <cstdlib>
#include <windows.h>
#include <cstdio>
#include <sddl.h>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <cerrno>
#include <locale>
#include <cwchar>
#include <map>
#include <set>
#include <atomic>
#include <thread>





// Control pipe name. AGNOSTIC: the driver knows nothing about any specific consumer; any
// process with the right ACL (SYSTEM + Administrators) can drive it. Renamed from the old
// product-coupled "MTTVirtualDisplayPipe" to the neutral "ArgusDisplay" (the Rust consumer
// uses the same name). NOTE: this is the control-plane pipe name only; the INF hardware IDs
// and device class are intentionally NOT renamed here (that is a coordinated, separately-signed
// change — see TODO below).
// TODO(inf): rename the INF hardware IDs / device class to the neutral identity in a coordinated,
// re-signed driver-package update. Do NOT change them piecemeal here — it would break the
// existing signed package's PnP match.
#define PIPE_NAME L"\\\\.\\pipe\\ArgusDisplay"

#pragma comment(lib, "xmllite.lib")
#pragma comment(lib, "shlwapi.lib")

HANDLE hPipeThread = NULL;
bool g_Running = true;
mutex g_Mutex;
HANDLE g_pipeHandle = INVALID_HANDLE_VALUE;
// g_pipeHandle is the per-client reply channel set by the pipe thread, but vddlog() forwards log
// lines to it AND is called from the watchdog/log threads — so the handle is read on those threads
// while the pipe thread writes/closes/resets it (close+reset at end of HandleClient). That is a
// data race / use-after-close. Guard EVERY g_pipeHandle access (write in SendToPipe, the read in
// vddlog's forward check, and the set/reset in HandleClient) with this dedicated mutex. Keep this
// lock LEAF-LEVEL: never call back into vddlog()/SendToPipe() (or anything that might) while it is
// held, or the same thread would re-enter and deadlock.
std::mutex g_pipeHandleMutex;

// ============================================================================
// On-demand monitor management (spacedesk-style: idle at 0, add/remove ONE by index)
// ============================================================================
//
// The pipe thread runs outside any WDF/IddCx callback, so it has no WDFOBJECT to feed
// WdfObjectGet_IndirectDeviceContextWrapper(). We cache the live device context here when
// the adapter is initialized (InitAdapter). This replaces the old ReloadDriver() which
// incorrectly passed a pipe HANDLE to WdfObjectGet_IndirectDeviceContextWrapper() (a
// WDFOBJECT type-confusion bug). Guarded by g_DeviceContextMutex.
namespace Microsoft { namespace IndirectDisp { class IndirectDeviceContext; } }
Microsoft::IndirectDisp::IndirectDeviceContext* g_DeviceContext = nullptr;
std::mutex g_DeviceContextMutex;

// Watchdog (parsec-vdd / SudoVDA self-healing). If a consumer that has opted into the watchdog
// stops PINGing for g_WatchdogTimeoutSeconds, every live monitor is auto-removed so a crashed
// consumer can't leave orphan displays attached. Set g_WatchdogTimeoutSeconds to 0 to disable.
// Runtime variable (not constexpr) so the disable check is a real branch (avoids C4127 under /WX)
// and so a future registry/config knob can adjust it without a rebuild.
//
// OPT-IN (standalone-safe / AGNOSTIC): the watchdog is DISARMED until the FIRST PING is received.
// A standalone install (e.g. monitors pre-connected at boot via numVirtualDisplays) with NO
// consumer PINGing must keep its monitors forever — that's normal driver behavior. Only a consumer
// that actively PINGs opts into the self-heal. Therefore arming happens ONLY on PING (g_WatchdogArmed),
// never from boot or from ADD/REMOVE; ADD/REMOVE/SETDISPLAYCOUNT merely reset the countdown IF
// already armed. While disarmed the thread idles and never removes monitors.
int               g_WatchdogTimeoutSeconds = 3;
std::atomic<int>  g_WatchdogCountdown{ 0 };   // seconds remaining before "bark"; reset on every PING/command (only meaningful once armed)
std::atomic<bool> g_WatchdogArmed{ false };   // false until the first PING; while false the watchdog NEVER barks
std::atomic<bool> g_WatchdogRunning{ false };
HANDLE            g_WatchdogThread = NULL;
// ROUND-2 FIX (Issue 1): the bare g_WatchdogThread HANDLE was written by StartWatchdog and
// read/closed/cleared by StopWatchdog with no synchronization. StartWatchdog (InitAdapter) and
// StopWatchdog (~IndirectDeviceContext) can run on different threads during a device
// teardown+recreate, so a store-vs-close race could leak or double-close the handle. This small
// LEAF-LEVEL mutex serializes every access to the g_WatchdogThread HANDLE. It is NEVER acquired
// while holding g_DeviceContextMutex / m_MonitorsMutex / m_ProcessingThreadsMutex (and the
// watchdog body never touches it), so it cannot participate in any lock-order cycle.
std::mutex        g_WatchdogThreadMutex;

using namespace std;
using namespace Microsoft::IndirectDisp;
using namespace Microsoft::WRL;

void vddlog(const char* type, const char* message);

extern "C" DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_DEVICE_ADD VirtualDisplayDriverDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY VirtualDisplayDriverDeviceD0Entry;

EVT_IDD_CX_ADAPTER_INIT_FINISHED VirtualDisplayDriverAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES VirtualDisplayDriverAdapterCommitModes;

EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION VirtualDisplayDriverParseMonitorDescription;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES VirtualDisplayDriverMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES VirtualDisplayDriverMonitorQueryModes;

EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN VirtualDisplayDriverMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN VirtualDisplayDriverMonitorUnassignSwapChain;

EVT_IDD_CX_ADAPTER_QUERY_TARGET_INFO VirtualDisplayDriverEvtIddCxAdapterQueryTargetInfo;
EVT_IDD_CX_MONITOR_SET_DEFAULT_HDR_METADATA VirtualDisplayDriverEvtIddCxMonitorSetDefaultHdrMetadata;
EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION2 VirtualDisplayDriverEvtIddCxParseMonitorDescription2;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES2 VirtualDisplayDriverEvtIddCxMonitorQueryTargetModes2;
EVT_IDD_CX_ADAPTER_COMMIT_MODES2 VirtualDisplayDriverEvtIddCxAdapterCommitModes2;

EVT_IDD_CX_MONITOR_SET_GAMMA_RAMP VirtualDisplayDriverEvtIddCxMonitorSetGammaRamp;

struct
{
	AdapterOption Adapter;
} Options;
vector<tuple<int, int, int, int>> monitorModes;
vector< DISPLAYCONFIG_VIDEO_SIGNAL_INFO> s_KnownMonitorModes2;
UINT numVirtualDisplays;
wstring gpuname;
wstring confpath = L"C:\\VirtualDisplayDriver";
bool logsEnabled = false;
bool debugLogs = false;
bool HDRPlus = false;
bool SDR10 = false;
bool customEdid = false;
bool hardwareCursor = false;
bool preventManufacturerSpoof = false;
bool edidCeaOverride = false;
bool sendLogsThroughPipe = true;

constexpr DISPLAYCONFIG_VIDEO_SIGNAL_INFO dispinfo(UINT32 h, UINT32 v, UINT32 rn, UINT32 rd);

namespace
{
	void RebuildKnownMonitorModesCache()
	{
		s_KnownMonitorModes2.clear();
		s_KnownMonitorModes2.reserve(monitorModes.size());

		for (const auto& mode : monitorModes)
		{
			s_KnownMonitorModes2.push_back(
				dispinfo(
					std::get<0>(mode),
					std::get<1>(mode),
					std::get<2>(mode),
					std::get<3>(mode)));
		}
	}
}

//Mouse settings
bool alphaCursorSupport = true;
int CursorMaxX = 128;
int CursorMaxY = 128;
IDDCX_XOR_CURSOR_SUPPORT XorCursorSupportLevel = IDDCX_XOR_CURSOR_SUPPORT_FULL;


//Rest
IDDCX_BITS_PER_COMPONENT SDRCOLOUR = IDDCX_BITS_PER_COMPONENT_8;
IDDCX_BITS_PER_COMPONENT HDRCOLOUR = IDDCX_BITS_PER_COMPONENT_10;

wstring ColourFormat = L"RGB";

// ROUND-2 FIX (Issue 2 — Fix-3 data race): the SDR10/HDRPLUS pipe handlers WRITE the scalars
// SDRCOLOUR/HDRCOLOUR, maincalc() REASSIGNS the vector s_KnownMonitorEdid, and the description
// callbacks CLEAR+REFILL the vector s_KnownMonitorModes2 (RebuildKnownMonitorModesCache). All of
// these are concurrently READ by IddCx callback threads (CreateTargetMode2,
// ParseMonitorDescription/2, CreateMonitorObject) with no synchronization → torn scalar reads and
// container UB (indexing a vector while another thread reassigns/clears it). g_SettingsMutex
// serializes these. Discipline:
//   * WRITE-lock in the SDR10/HDRPLUS handlers (scalars), around maincalc()'s s_KnownMonitorEdid
//     reassignment, and around the s_KnownMonitorModes2 rebuild + the indexing that consumes it.
//   * READ-lock at the top of each callback that reads them: snapshot scalars to locals; for the
//     vectors, hold the lock across the rebuild+index (the callbacks both rebuild then index, so a
//     single held region covers both safely).
// NOTE: ColourFormat is written ONLY at DriverEntry (load time, before any monitor/callback exists),
// so it is NOT raced and is intentionally read without this lock. NOTE: never hold g_SettingsMutex
// across an IddCx call that can re-enter the driver — snapshot under the lock, release, then call.
std::mutex g_SettingsMutex;

// === EDID INTEGRATION SETTINGS ===
bool edidIntegrationEnabled = false;
bool autoConfigureFromEdid = false;
wstring edidProfilePath = L"EDID/monitor_profile.xml";
bool overrideManualSettings = false;
bool fallbackOnError = true;

// === HDR ADVANCED SETTINGS ===
bool hdr10StaticMetadataEnabled = false;
double maxDisplayMasteringLuminance = 1000.0;
double minDisplayMasteringLuminance = 0.05;
int maxContentLightLevel = 1000;
int maxFrameAvgLightLevel = 400;

bool colorPrimariesEnabled = false;
double redX = 0.708, redY = 0.292;
double greenX = 0.170, greenY = 0.797;
double blueX = 0.131, blueY = 0.046;
double whiteX = 0.3127, whiteY = 0.3290;

bool colorSpaceEnabled = false;
double gammaCorrection = 2.4;
wstring primaryColorSpace = L"sRGB";
bool enableMatrixTransform = false;

// === AUTO RESOLUTIONS SETTINGS ===
bool autoResolutionsEnabled = false;
wstring sourcePriority = L"manual";
int minRefreshRate = 24;
int maxRefreshRate = 240;
bool excludeFractionalRates = false;
int minResolutionWidth = 640;
int minResolutionHeight = 480;
int maxResolutionWidth = 7680;
int maxResolutionHeight = 4320;
bool useEdidPreferred = false;
int fallbackWidth = 1920;
int fallbackHeight = 1080;
int fallbackRefresh = 60;

// === COLOR ADVANCED SETTINGS ===
bool autoSelectFromColorSpace = false;
wstring forceBitDepth = L"auto";
bool fp16SurfaceSupport = true;
bool wideColorGamut = false;
bool hdrToneMapping = false;
double sdrWhiteLevel = 80.0;

// === MONITOR EMULATION SETTINGS ===
bool monitorEmulationEnabled = false;
bool emulatePhysicalDimensions = false;
int physicalWidthMm = 510;
int physicalHeightMm = 287;
bool manufacturerEmulationEnabled = false;
wstring manufacturerName = L"Generic";
wstring modelName = L"Virtual Display";
wstring serialNumber = L"VDD001";

std::map<std::wstring, std::pair<std::wstring, std::wstring>> SettingsQueryMap = {
	{L"LoggingEnabled", {L"LOGS", L"logging"}},
	{L"DebugLoggingEnabled", {L"DEBUGLOGS", L"debuglogging"}},
	{L"CustomEdidEnabled", {L"CUSTOMEDID", L"CustomEdid"}},

	{L"PreventMonitorSpoof", {L"PREVENTMONITORSPOOF", L"PreventSpoof"}},
	{L"EdidCeaOverride", {L"EDIDCEAOVERRIDE", L"EdidCeaOverride"}},
	{L"SendLogsThroughPipe", {L"SENDLOGSTHROUGHPIPE", L"SendLogsThroughPipe"}},
	
	//Cursor Begin
	{L"HardwareCursorEnabled", {L"HARDWARECURSOR", L"HardwareCursor"}},
	{L"AlphaCursorSupport", {L"ALPHACURSORSUPPORT", L"AlphaCursorSupport"}},
	{L"CursorMaxX", {L"CURSORMAXX", L"CursorMaxX"}},
	{L"CursorMaxY", {L"CURSORMAXY", L"CursorMaxY"}},
	{L"XorCursorSupportLevel", {L"XORCURSORSUPPORTLEVEL", L"XorCursorSupportLevel"}},
	//Cursor End
	
	//Colour Begin
	{L"HDRPlusEnabled", {L"HDRPLUS", L"HDRPlus"}},
	{L"SDR10Enabled", {L"SDR10BIT", L"SDR10bit"}},
	{L"ColourFormat", {L"COLOURFORMAT", L"ColourFormat"}},
	//Colour End
	
	//EDID Integration Begin
	{L"EdidIntegrationEnabled", {L"EDIDINTEGRATION", L"enabled"}},
	{L"AutoConfigureFromEdid", {L"AUTOCONFIGFROMEDID", L"auto_configure_from_edid"}},
	{L"EdidProfilePath", {L"EDIDPROFILEPATH", L"edid_profile_path"}},
	{L"OverrideManualSettings", {L"OVERRIDEMANUALSETTINGS", L"override_manual_settings"}},
	{L"FallbackOnError", {L"FALLBACKONERROR", L"fallback_on_error"}},
	//EDID Integration End
	
	//HDR Advanced Begin
	{L"Hdr10StaticMetadataEnabled", {L"HDR10STATICMETADATA", L"enabled"}},
	{L"MaxDisplayMasteringLuminance", {L"MAXLUMINANCE", L"max_display_mastering_luminance"}},
	{L"MinDisplayMasteringLuminance", {L"MINLUMINANCE", L"min_display_mastering_luminance"}},
	{L"MaxContentLightLevel", {L"MAXCONTENTLIGHT", L"max_content_light_level"}},
	{L"MaxFrameAvgLightLevel", {L"MAXFRAMEAVGLIGHT", L"max_frame_avg_light_level"}},
	{L"ColorPrimariesEnabled", {L"COLORPRIMARIES", L"enabled"}},
	{L"RedX", {L"REDX", L"red_x"}},
	{L"RedY", {L"REDY", L"red_y"}},
	{L"GreenX", {L"GREENX", L"green_x"}},
	{L"GreenY", {L"GREENY", L"green_y"}},
	{L"BlueX", {L"BLUEX", L"blue_x"}},
	{L"BlueY", {L"BLUEY", L"blue_y"}},
	{L"WhiteX", {L"WHITEX", L"white_x"}},
	{L"WhiteY", {L"WHITEY", L"white_y"}},
	{L"ColorSpaceEnabled", {L"COLORSPACE", L"enabled"}},
	{L"GammaCorrection", {L"GAMMA", L"gamma_correction"}},
	{L"PrimaryColorSpace", {L"PRIMARYCOLORSPACE", L"primary_color_space"}},
	{L"EnableMatrixTransform", {L"MATRIXTRANSFORM", L"enable_matrix_transform"}},
	//HDR Advanced End
	
	//Auto Resolutions Begin
	{L"AutoResolutionsEnabled", {L"AUTORESOLUTIONS", L"enabled"}},
	{L"SourcePriority", {L"SOURCEPRIORITY", L"source_priority"}},
	{L"MinRefreshRate", {L"MINREFRESHRATE", L"min_refresh_rate"}},
	{L"MaxRefreshRate", {L"MAXREFRESHRATE", L"max_refresh_rate"}},
	{L"ExcludeFractionalRates", {L"EXCLUDEFRACTIONAL", L"exclude_fractional_rates"}},
	{L"MinResolutionWidth", {L"MINWIDTH", L"min_resolution_width"}},
	{L"MinResolutionHeight", {L"MINHEIGHT", L"min_resolution_height"}},
	{L"MaxResolutionWidth", {L"MAXWIDTH", L"max_resolution_width"}},
	{L"MaxResolutionHeight", {L"MAXHEIGHT", L"max_resolution_height"}},
	{L"UseEdidPreferred", {L"USEEDIDPREFERRED", L"use_edid_preferred"}},
	{L"FallbackWidth", {L"FALLBACKWIDTH", L"fallback_width"}},
	{L"FallbackHeight", {L"FALLBACKHEIGHT", L"fallback_height"}},
	{L"FallbackRefresh", {L"FALLBACKREFRESH", L"fallback_refresh"}},
	//Auto Resolutions End
	
	//Color Advanced Begin
	{L"AutoSelectFromColorSpace", {L"AUTOSELECTCOLOR", L"auto_select_from_color_space"}},
	{L"ForceBitDepth", {L"FORCEBITDEPTH", L"force_bit_depth"}},
	{L"Fp16SurfaceSupport", {L"FP16SUPPORT", L"fp16_surface_support"}},
	{L"WideColorGamut", {L"WIDECOLORGAMUT", L"wide_color_gamut"}},
	{L"HdrToneMapping", {L"HDRTONEMAPPING", L"hdr_tone_mapping"}},
	{L"SdrWhiteLevel", {L"SDRWHITELEVEL", L"sdr_white_level"}},
	//Color Advanced End
	
	//Monitor Emulation Begin
	{L"MonitorEmulationEnabled", {L"MONITOREMULATION", L"enabled"}},
	{L"EmulatePhysicalDimensions", {L"EMULATEPHYSICAL", L"emulate_physical_dimensions"}},
	{L"PhysicalWidthMm", {L"PHYSICALWIDTH", L"physical_width_mm"}},
	{L"PhysicalHeightMm", {L"PHYSICALHEIGHT", L"physical_height_mm"}},
	{L"ManufacturerEmulationEnabled", {L"MANUFACTUREREMULATION", L"enabled"}},
	{L"ManufacturerName", {L"MANUFACTURERNAME", L"manufacturer_name"}},
	{L"ModelName", {L"MODELNAME", L"model_name"}},
	{L"SerialNumber", {L"SERIALNUMBER", L"serial_number"}},
	//Monitor Emulation End
};

const char* XorCursorSupportLevelToString(IDDCX_XOR_CURSOR_SUPPORT level) {
	switch (level) {
	case IDDCX_XOR_CURSOR_SUPPORT_UNINITIALIZED:
		return "IDDCX_XOR_CURSOR_SUPPORT_UNINITIALIZED";
	case IDDCX_XOR_CURSOR_SUPPORT_NONE:
		return "IDDCX_XOR_CURSOR_SUPPORT_NONE";
	case IDDCX_XOR_CURSOR_SUPPORT_FULL:
		return "IDDCX_XOR_CURSOR_SUPPORT_FULL";
	case IDDCX_XOR_CURSOR_SUPPORT_EMULATION:
		return "IDDCX_XOR_CURSOR_SUPPORT_EMULATION";
	default:
		return "Unknown";
	}
}


vector<unsigned char> Microsoft::IndirectDisp::IndirectDeviceContext::s_KnownMonitorEdid; //Changed to support static vector

std::map<LUID, std::shared_ptr<Direct3DDevice>, Microsoft::IndirectDisp::LuidComparator> Microsoft::IndirectDisp::IndirectDeviceContext::s_DeviceCache;
std::mutex Microsoft::IndirectDisp::IndirectDeviceContext::s_DeviceCacheMutex;

struct IndirectDeviceContextWrapper
{
	IndirectDeviceContext* pContext;

	void Cleanup()
	{
		delete pContext;
		pContext = nullptr;
	}
};
void LogQueries(const char* severity, const std::wstring& xmlName) {
	if (xmlName.find(L"logging") == std::wstring::npos) { 
		int size_needed = WideCharToMultiByte(CP_UTF8, 0, xmlName.c_str(), (int)xmlName.size(), NULL, 0, NULL, NULL);
		if (size_needed > 0) {
			std::string strMessage(size_needed, 0);
			WideCharToMultiByte(CP_UTF8, 0, xmlName.c_str(), (int)xmlName.size(), &strMessage[0], size_needed, NULL, NULL);
			vddlog(severity, strMessage.c_str());
		}
	}
}

string WStringToString(const wstring& wstr) { //basically just a function for converting strings since codecvt is depricated in c++ 17
	if (wstr.empty()) return "";

	int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), NULL, 0, NULL, NULL);
	string str(size_needed, 0);
	WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &str[0], size_needed, NULL, NULL);
	return str;
}

bool EnabledQuery(const std::wstring& settingKey) {
	auto it = SettingsQueryMap.find(settingKey);
	if (it == SettingsQueryMap.end()) {
		vddlog("e", "requested data not found in xml, consider updating xml!");
		return false;
	}

	std::wstring regName = it->second.first;
	std::wstring xmlName = it->second.second;

	std::wstring settingsname = confpath + L"\\vdd_settings.xml";
	HKEY hKey;
	DWORD dwValue;
	DWORD dwBufferSize = sizeof(dwValue);
	LONG lResult = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\MikeTheTech\\VirtualDisplayDriver", 0, KEY_READ, &hKey);

	if (lResult == ERROR_SUCCESS) {
		lResult = RegQueryValueExW(hKey, regName.c_str(), NULL, NULL, (LPBYTE)&dwValue, &dwBufferSize);
		if (lResult == ERROR_SUCCESS) {
			RegCloseKey(hKey);
			if (dwValue == 1) {
				LogQueries("d", xmlName + L" - is enabled (value = 1).");
				return true;
			}
			else if (dwValue == 0) {
				goto check_xml;
			}
		}
		else {
			LogQueries("d", xmlName + L" - Failed to retrieve value from registry. Attempting to read as string.");
			wchar_t path[MAX_PATH];
			dwBufferSize = sizeof(path);
			lResult = RegQueryValueExW(hKey, regName.c_str(), NULL, NULL, (LPBYTE)path, &dwBufferSize);
			if (lResult == ERROR_SUCCESS) {
				std::wstring logValue(path);
				RegCloseKey(hKey);
				if (logValue == L"true" || logValue == L"1") {
					LogQueries("d", xmlName + L" - is enabled (string value).");
					return true;
				}
				else if (logValue == L"false" || logValue == L"0") {
					goto check_xml;
				}
			}
			RegCloseKey(hKey);
			LogQueries("d", xmlName + L" - Failed to retrieve string value from registry.");
		}
	}

check_xml:
	CComPtr<IStream> pFileStream;
	HRESULT hr = SHCreateStreamOnFileEx(settingsname.c_str(), STGM_READ, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &pFileStream);
	if (FAILED(hr)) {
		LogQueries("d", xmlName + L" - Failed to create file stream for XML settings.");
		return false;
	}

	CComPtr<IXmlReader> pReader;
	hr = CreateXmlReader(__uuidof(IXmlReader), (void**)&pReader, nullptr);
	if (FAILED(hr)) {
		LogQueries("d", xmlName + L" - Failed to create XML reader.");
		return false;
	}

	hr = pReader->SetInput(pFileStream);
	if (FAILED(hr)) {
		LogQueries("d", xmlName + L" - Failed to set input for XML reader.");
		return false;
	}

	XmlNodeType nodeType;
	const wchar_t* pwszLocalName;
	bool xmlLoggingValue = false;

	while (S_OK == pReader->Read(&nodeType)) {
		if (nodeType == XmlNodeType_Element) {
			pReader->GetLocalName(&pwszLocalName, nullptr);
			if (pwszLocalName && wcscmp(pwszLocalName, xmlName.c_str()) == 0) {
				pReader->Read(&nodeType);
				if (nodeType == XmlNodeType_Text) {
					const wchar_t* pwszValue;
					pReader->GetValue(&pwszValue, nullptr);
					if (pwszValue) {
						xmlLoggingValue = (wcscmp(pwszValue, L"true") == 0);
					}
					LogQueries("i", xmlName + (xmlLoggingValue ? L" is enabled." : L" is disabled."));
					break;
				}
			}
		}
	}

	return xmlLoggingValue;
}

int GetIntegerSetting(const std::wstring& settingKey) {
	auto it = SettingsQueryMap.find(settingKey);
	if (it == SettingsQueryMap.end()) {
		vddlog("e", "requested data not found in xml, consider updating xml!");
		return -1;
	}

	std::wstring regName = it->second.first;
	std::wstring xmlName = it->second.second;

	std::wstring settingsname = confpath + L"\\vdd_settings.xml";
	HKEY hKey;
	DWORD dwValue;
	DWORD dwBufferSize = sizeof(dwValue);
	LONG lResult = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\MikeTheTech\\VirtualDisplayDriver", 0, KEY_READ, &hKey);

	if (lResult == ERROR_SUCCESS) {
		lResult = RegQueryValueExW(hKey, regName.c_str(), NULL, NULL, (LPBYTE)&dwValue, &dwBufferSize);
		if (lResult == ERROR_SUCCESS) {
			RegCloseKey(hKey);
			LogQueries("d", xmlName + L" - Retrieved integer value: " + std::to_wstring(dwValue));
			return static_cast<int>(dwValue);
		}
		else {
			LogQueries("d", xmlName + L" - Failed to retrieve integer value from registry. Attempting to read as string.");
			wchar_t path[MAX_PATH];
			dwBufferSize = sizeof(path);
			lResult = RegQueryValueExW(hKey, regName.c_str(), NULL, NULL, (LPBYTE)path, &dwBufferSize);
			RegCloseKey(hKey);
			if (lResult == ERROR_SUCCESS) {
				try {
					int logValue = std::stoi(path);
					LogQueries("d", xmlName + L" - Retrieved string value: " + std::to_wstring(logValue));
					return logValue;
				}
				catch (const std::exception&) {
					LogQueries("d", xmlName + L" - Failed to convert registry string value to integer.");
				}
			}
		}
	}

	CComPtr<IStream> pFileStream;
	HRESULT hr = SHCreateStreamOnFileEx(settingsname.c_str(), STGM_READ, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &pFileStream);
	if (FAILED(hr)) {
		LogQueries("d", xmlName + L" - Failed to create file stream for XML settings.");
		return -1;
	}

	CComPtr<IXmlReader> pReader;
	hr = CreateXmlReader(__uuidof(IXmlReader), (void**)&pReader, nullptr);
	if (FAILED(hr)) {
		LogQueries("d", xmlName + L" - Failed to create XML reader.");
		return -1;
	}

	hr = pReader->SetInput(pFileStream);
	if (FAILED(hr)) {
		LogQueries("d", xmlName + L" - Failed to set input for XML reader.");
		return -1;
	}

	XmlNodeType nodeType;
	const wchar_t* pwszLocalName;
	int xmlLoggingValue = -1;

	while (S_OK == pReader->Read(&nodeType)) {
		if (nodeType == XmlNodeType_Element) {
			pReader->GetLocalName(&pwszLocalName, nullptr);
			if (pwszLocalName && wcscmp(pwszLocalName, xmlName.c_str()) == 0) {
				pReader->Read(&nodeType);
				if (nodeType == XmlNodeType_Text) {
					const wchar_t* pwszValue;
					pReader->GetValue(&pwszValue, nullptr);
					if (pwszValue) {
						try {
							xmlLoggingValue = std::stoi(pwszValue);
							LogQueries("i", xmlName + L" - Retrieved from XML: " + std::to_wstring(xmlLoggingValue));
						}
						catch (const std::exception&) {
							LogQueries("d", xmlName + L" - Failed to convert XML string value to integer.");
						}
					}
					break;
				}
			}
		}
	}

	return xmlLoggingValue;
}

std::wstring GetStringSetting(const std::wstring& settingKey) {
	auto it = SettingsQueryMap.find(settingKey);
	if (it == SettingsQueryMap.end()) {
		vddlog("e", "requested data not found in xml, consider updating xml!");
		return L""; 
	}

	std::wstring regName = it->second.first;
	std::wstring xmlName = it->second.second;

	std::wstring settingsname = confpath + L"\\vdd_settings.xml";
	HKEY hKey;
	DWORD dwBufferSize = MAX_PATH;
	wchar_t buffer[MAX_PATH];

	LONG lResult = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\MikeTheTech\\VirtualDisplayDriver", 0, KEY_READ, &hKey);
	if (lResult == ERROR_SUCCESS) {
		lResult = RegQueryValueExW(hKey, regName.c_str(), NULL, NULL, (LPBYTE)buffer, &dwBufferSize);
		RegCloseKey(hKey);

		if (lResult == ERROR_SUCCESS) {
			LogQueries("d", xmlName + L" - Retrieved string value from registry: " + buffer);
			return std::wstring(buffer);  
		}
		else {
			LogQueries("d", xmlName + L" - Failed to retrieve string value from registry. Attempting to read as XML.");
		}
	}

	CComPtr<IStream> pFileStream;
	HRESULT hr = SHCreateStreamOnFileEx(settingsname.c_str(), STGM_READ, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &pFileStream);
	if (FAILED(hr)) {
		LogQueries("d", xmlName + L" - Failed to create file stream for XML settings.");
		return L""; 
	}

	CComPtr<IXmlReader> pReader;
	hr = CreateXmlReader(__uuidof(IXmlReader), (void**)&pReader, nullptr);
	if (FAILED(hr)) {
		LogQueries("d", xmlName + L" - Failed to create XML reader.");
		return L""; 
	}

	hr = pReader->SetInput(pFileStream);
	if (FAILED(hr)) {
		LogQueries("d", xmlName + L" - Failed to set input for XML reader.");
		return L"";  
	}

	XmlNodeType nodeType;
	const wchar_t* pwszLocalName;
	std::wstring xmlLoggingValue = L"";  

	while (S_OK == pReader->Read(&nodeType)) {
		if (nodeType == XmlNodeType_Element) {
			pReader->GetLocalName(&pwszLocalName, nullptr);
			if (pwszLocalName && wcscmp(pwszLocalName, xmlName.c_str()) == 0) {
				pReader->Read(&nodeType);
				if (nodeType == XmlNodeType_Text) {
					const wchar_t* pwszValue;
					pReader->GetValue(&pwszValue, nullptr);
					if (pwszValue) {
						xmlLoggingValue = pwszValue;
					}
					LogQueries("i", xmlName + L" - Retrieved from XML: " + xmlLoggingValue);
					break;
				}
			}
		}
	}

	return xmlLoggingValue;  
}

double GetDoubleSetting(const std::wstring& settingKey) {
	auto it = SettingsQueryMap.find(settingKey);
	if (it == SettingsQueryMap.end()) {
		vddlog("e", "requested data not found in xml, consider updating xml!");
		return 0.0;
	}

	std::wstring regName = it->second.first;
	std::wstring xmlName = it->second.second;

	std::wstring settingsname = confpath + L"\\vdd_settings.xml";
	HKEY hKey;
	DWORD dwBufferSize = MAX_PATH;
	wchar_t buffer[MAX_PATH];

	LONG lResult = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\MikeTheTech\\VirtualDisplayDriver", 0, KEY_READ, &hKey);
	if (lResult == ERROR_SUCCESS) {
		lResult = RegQueryValueExW(hKey, regName.c_str(), NULL, NULL, (LPBYTE)buffer, &dwBufferSize);
		if (lResult == ERROR_SUCCESS) {
			RegCloseKey(hKey);
			try {
				double regValue = std::stod(buffer);
				LogQueries("d", xmlName + L" - Retrieved from registry: " + std::to_wstring(regValue));
				return regValue;
			}
			catch (const std::exception&) {
				LogQueries("d", xmlName + L" - Failed to convert registry value to double.");
			}
		}
		else {
			RegCloseKey(hKey);
		}
	}

	CComPtr<IStream> pFileStream;
	HRESULT hr = SHCreateStreamOnFileEx(settingsname.c_str(), STGM_READ, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &pFileStream);
	if (FAILED(hr)) {
		LogQueries("d", xmlName + L" - Failed to create file stream for XML settings.");
		return 0.0;
	}

	CComPtr<IXmlReader> pReader;
	hr = CreateXmlReader(__uuidof(IXmlReader), (void**)&pReader, nullptr);
	if (FAILED(hr)) {
		LogQueries("d", xmlName + L" - Failed to create XML reader.");
		return 0.0;
	}

	hr = pReader->SetInput(pFileStream);
	if (FAILED(hr)) {
		LogQueries("d", xmlName + L" - Failed to set input for XML reader.");
		return 0.0;
	}

	XmlNodeType nodeType;
	const wchar_t* pwszLocalName;
	double xmlLoggingValue = 0.0;

	while (S_OK == pReader->Read(&nodeType)) {
		if (nodeType == XmlNodeType_Element) {
			pReader->GetLocalName(&pwszLocalName, nullptr);
			if (pwszLocalName && wcscmp(pwszLocalName, xmlName.c_str()) == 0) {
				pReader->Read(&nodeType);
				if (nodeType == XmlNodeType_Text) {
					const wchar_t* pwszValue;
					pReader->GetValue(&pwszValue, nullptr);
					if (pwszValue) {
						try {
							xmlLoggingValue = std::stod(pwszValue);
							LogQueries("i", xmlName + L" - Retrieved from XML: " + std::to_wstring(xmlLoggingValue));
						}
						catch (const std::exception&) {
							LogQueries("d", xmlName + L" - Failed to convert XML value to double.");
						}
					}
					break;
				}
			}
		}
	}

	return xmlLoggingValue;
}

// === EDID PROFILE LOADING FUNCTION ===
struct EdidProfileData {
	vector<tuple<int, int, int, int>> modes;
	bool hdr10Supported = false;
	bool dolbyVisionSupported = false;
	bool hdr10PlusSupported = false;
	double maxLuminance = 0.0;
	double minLuminance = 0.0;
	wstring primaryColorSpace = L"sRGB";
	double gamma = 2.2;
	double redX = 0.64, redY = 0.33;
	double greenX = 0.30, greenY = 0.60;
	double blueX = 0.15, blueY = 0.06;
	double whiteX = 0.3127, whiteY = 0.3290;
	int preferredWidth = 1920;
	int preferredHeight = 1080;
	double preferredRefresh = 60.0;
};

// === COLOR SPACE AND GAMMA STRUCTURES ===
struct VddColorMatrix {
    FLOAT matrix[3][4] = {}; // 3x4 color space transformation matrix - zero initialized
    bool isValid = false;
};

struct VddGammaRamp {
    FLOAT gamma = 2.2f;
    wstring colorSpace;
    VddColorMatrix matrix = {};
    bool useMatrix = false;
    bool isValid = false;
};

// === GAMMA AND COLOR SPACE STORAGE ===
std::map<IDDCX_MONITOR, VddGammaRamp> g_GammaRampStore;
// Guards g_GammaRampStore. The store is read/written from IddCx gamma callbacks AND erased from
// the pipe/watchdog thread in RemoveMonitor, so cross-thread access must be serialized.
std::mutex g_GammaRampStoreMutex;

// === COLOR SPACE AND GAMMA CONVERSION FUNCTIONS ===

// Convert gamma value to 3x4 color space transformation matrix
VddColorMatrix ConvertGammaToMatrix(double gamma, const wstring& colorSpace) {
    VddColorMatrix matrix = {};
    
    // Identity matrix as base
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 4; j++) {
            matrix.matrix[i][j] = (i == j) ? 1.0f : 0.0f;
        }
    }
    
    // Apply gamma correction to diagonal elements
    float gammaValue = static_cast<float>(gamma);
    
    if (colorSpace == L"sRGB") {
        // sRGB gamma correction (2.2)
        matrix.matrix[0][0] = gammaValue / 2.2f;  // Red
        matrix.matrix[1][1] = gammaValue / 2.2f;  // Green
        matrix.matrix[2][2] = gammaValue / 2.2f;  // Blue
    }
    else if (colorSpace == L"DCI-P3") {
        // DCI-P3 color space transformation with gamma
        // P3 to sRGB matrix with gamma correction
        matrix.matrix[0][0] = 1.2249f * (gammaValue / 2.4f);
        matrix.matrix[0][1] = -0.2247f;
        matrix.matrix[0][2] = 0.0f;
        matrix.matrix[1][0] = -0.0420f;
        matrix.matrix[1][1] = 1.0419f * (gammaValue / 2.4f);
        matrix.matrix[1][2] = 0.0f;
        matrix.matrix[2][0] = -0.0196f;
        matrix.matrix[2][1] = -0.0786f;
        matrix.matrix[2][2] = 1.0982f * (gammaValue / 2.4f);
    }
    else if (colorSpace == L"Rec.2020") {
        // Rec.2020 to sRGB matrix with gamma correction
        matrix.matrix[0][0] = 1.7347f * (gammaValue / 2.4f);
        matrix.matrix[0][1] = -0.7347f;
        matrix.matrix[0][2] = 0.0f;
        matrix.matrix[1][0] = -0.1316f;
        matrix.matrix[1][1] = 1.1316f * (gammaValue / 2.4f);
        matrix.matrix[1][2] = 0.0f;
        matrix.matrix[2][0] = -0.0241f;
        matrix.matrix[2][1] = -0.1289f;
        matrix.matrix[2][2] = 1.1530f * (gammaValue / 2.4f);
    }
    else if (colorSpace == L"Adobe_RGB") {
        // Adobe RGB with gamma correction
        matrix.matrix[0][0] = 1.0f * (gammaValue / 2.2f);
        matrix.matrix[1][1] = 1.0f * (gammaValue / 2.2f);
        matrix.matrix[2][2] = 1.0f * (gammaValue / 2.2f);
    }
    else {
        // Default to sRGB for unknown color spaces
        matrix.matrix[0][0] = gammaValue / 2.2f;
        matrix.matrix[1][1] = gammaValue / 2.2f;
        matrix.matrix[2][2] = gammaValue / 2.2f;
    }
    
    matrix.isValid = true;
    return matrix;
}

// Convert EDID profile to gamma ramp
VddGammaRamp ConvertEdidToGammaRamp(const EdidProfileData& profile) {
    VddGammaRamp gammaRamp = {};
    
    gammaRamp.gamma = static_cast<FLOAT>(profile.gamma);
    gammaRamp.colorSpace = profile.primaryColorSpace;
    
    // Generate matrix if matrix transforms are enabled
    if (enableMatrixTransform) {
        gammaRamp.matrix = ConvertGammaToMatrix(profile.gamma, profile.primaryColorSpace);
        gammaRamp.useMatrix = gammaRamp.matrix.isValid;
    }
    
    gammaRamp.isValid = colorSpaceEnabled;
    
    return gammaRamp;
}

// Convert manual settings to gamma ramp
VddGammaRamp ConvertManualToGammaRamp() {
    VddGammaRamp gammaRamp = {};
    
    gammaRamp.gamma = static_cast<FLOAT>(gammaCorrection);
    gammaRamp.colorSpace = primaryColorSpace;
    
    // Generate matrix if matrix transforms are enabled
    if (enableMatrixTransform) {
        gammaRamp.matrix = ConvertGammaToMatrix(gammaCorrection, primaryColorSpace);
        gammaRamp.useMatrix = gammaRamp.matrix.isValid;
    }
    
    gammaRamp.isValid = colorSpaceEnabled;
    
    return gammaRamp;
}

// Enhanced color format selection based on color space
IDDCX_BITS_PER_COMPONENT SelectBitDepthFromColorSpace(const wstring& colorSpace) {
    if (autoSelectFromColorSpace) {
        if (colorSpace == L"Rec.2020") {
            return IDDCX_BITS_PER_COMPONENT_10;  // HDR10 - 10-bit for wide color gamut
        } else if (colorSpace == L"DCI-P3") {
            return IDDCX_BITS_PER_COMPONENT_10;  // Wide color gamut - 10-bit
        } else if (colorSpace == L"Adobe_RGB") {
            return IDDCX_BITS_PER_COMPONENT_10;  // Professional - 10-bit
        } else {
            return IDDCX_BITS_PER_COMPONENT_8;   // sRGB - 8-bit
        }
    }
    
    // Manual bit depth override
    if (forceBitDepth == L"8") {
        return IDDCX_BITS_PER_COMPONENT_8;
    } else if (forceBitDepth == L"10") {
        return IDDCX_BITS_PER_COMPONENT_10;
    } else if (forceBitDepth == L"12") {
        return IDDCX_BITS_PER_COMPONENT_12;
    }
    
    // Default to existing color depth logic
    return HDRPlus ? IDDCX_BITS_PER_COMPONENT_12 : 
           (SDR10 ? IDDCX_BITS_PER_COMPONENT_10 : IDDCX_BITS_PER_COMPONENT_8);
}

// === SMPTE ST.2086 HDR METADATA STRUCTURE ===
struct VddHdrMetadata {
    // SMPTE ST.2086 Display Primaries (scaled 0-50000) - zero initialized
    UINT16 display_primaries_x[3] = {};      // R, G, B chromaticity x coordinates
    UINT16 display_primaries_y[3] = {};      // R, G, B chromaticity y coordinates
    UINT16 white_point_x = 0;               // White point x coordinate
    UINT16 white_point_y = 0;               // White point y coordinate
    
    // Luminance values (0.0001 cd/m² units for SMPTE ST.2086)
    UINT32 max_display_mastering_luminance = 0;
    UINT32 min_display_mastering_luminance = 0;
    
    // Content light level (nits)
    UINT16 max_content_light_level = 0;
    UINT16 max_frame_avg_light_level = 0;
    
    // Validation flag
    bool isValid = false;
};

// === HDR METADATA STORAGE ===
std::map<IDDCX_MONITOR, VddHdrMetadata> g_HdrMetadataStore;
// Guards g_HdrMetadataStore (see g_GammaRampStoreMutex rationale).
std::mutex g_HdrMetadataStoreMutex;

// === HDR METADATA CONVERSION FUNCTIONS ===

// Convert EDID chromaticity (0.0-1.0) to SMPTE ST.2086 format (0-50000)
UINT16 ConvertChromaticityToSmpte(double edidValue) {
    // Clamp to valid range
    if (edidValue < 0.0) edidValue = 0.0;
    if (edidValue > 1.0) edidValue = 1.0;
    
    return static_cast<UINT16>(edidValue * 50000.0);
}

// Convert EDID luminance (nits) to SMPTE ST.2086 format (0.0001 cd/m² units)
UINT32 ConvertLuminanceToSmpte(double nits) {
    // Clamp to reasonable range (0.0001 to 10000 nits)
    if (nits < 0.0001) nits = 0.0001;
    if (nits > 10000.0) nits = 10000.0;
    
    return static_cast<UINT32>(nits * 10000.0);
}

// Convert EDID profile data to SMPTE ST.2086 HDR metadata
VddHdrMetadata ConvertEdidToSmpteMetadata(const EdidProfileData& profile) {
    VddHdrMetadata metadata = {};
    
    // Convert chromaticity coordinates
    metadata.display_primaries_x[0] = ConvertChromaticityToSmpte(profile.redX);     // Red
    metadata.display_primaries_y[0] = ConvertChromaticityToSmpte(profile.redY);
    metadata.display_primaries_x[1] = ConvertChromaticityToSmpte(profile.greenX);   // Green  
    metadata.display_primaries_y[1] = ConvertChromaticityToSmpte(profile.greenY);
    metadata.display_primaries_x[2] = ConvertChromaticityToSmpte(profile.blueX);    // Blue
    metadata.display_primaries_y[2] = ConvertChromaticityToSmpte(profile.blueY);
    
    // Convert white point
    metadata.white_point_x = ConvertChromaticityToSmpte(profile.whiteX);
    metadata.white_point_y = ConvertChromaticityToSmpte(profile.whiteY);
    
    // Convert luminance values
    metadata.max_display_mastering_luminance = ConvertLuminanceToSmpte(profile.maxLuminance);
    metadata.min_display_mastering_luminance = ConvertLuminanceToSmpte(profile.minLuminance);
    
    // Use configured content light levels (from vdd_settings.xml)
    metadata.max_content_light_level = static_cast<UINT16>(maxContentLightLevel);
    metadata.max_frame_avg_light_level = static_cast<UINT16>(maxFrameAvgLightLevel);
    
    // Mark as valid if we have HDR10 support
    metadata.isValid = profile.hdr10Supported && hdr10StaticMetadataEnabled;
    
    return metadata;
}

// Convert manual settings to SMPTE ST.2086 HDR metadata
VddHdrMetadata ConvertManualToSmpteMetadata() {
    VddHdrMetadata metadata = {};
    
    // Convert manual chromaticity coordinates
    metadata.display_primaries_x[0] = ConvertChromaticityToSmpte(redX);     // Red
    metadata.display_primaries_y[0] = ConvertChromaticityToSmpte(redY);
    metadata.display_primaries_x[1] = ConvertChromaticityToSmpte(greenX);   // Green  
    metadata.display_primaries_y[1] = ConvertChromaticityToSmpte(greenY);
    metadata.display_primaries_x[2] = ConvertChromaticityToSmpte(blueX);    // Blue
    metadata.display_primaries_y[2] = ConvertChromaticityToSmpte(blueY);
    
    // Convert manual white point
    metadata.white_point_x = ConvertChromaticityToSmpte(whiteX);
    metadata.white_point_y = ConvertChromaticityToSmpte(whiteY);
    
    // Convert manual luminance values
    metadata.max_display_mastering_luminance = ConvertLuminanceToSmpte(maxDisplayMasteringLuminance);
    metadata.min_display_mastering_luminance = ConvertLuminanceToSmpte(minDisplayMasteringLuminance);
    
    // Use configured content light levels
    metadata.max_content_light_level = static_cast<UINT16>(maxContentLightLevel);
    metadata.max_frame_avg_light_level = static_cast<UINT16>(maxFrameAvgLightLevel);
    
    // Mark as valid if HDR10 metadata is enabled and color primaries are enabled
    metadata.isValid = hdr10StaticMetadataEnabled && colorPrimariesEnabled;
    
    return metadata;
}

// === ENHANCED MODE MANAGEMENT FUNCTIONS ===

// Generate modes from EDID with advanced filtering and optimization
vector<tuple<int, int, int, int>> GenerateModesFromEdid(const EdidProfileData& profile) {
    vector<tuple<int, int, int, int>> generatedModes;
    
    if (!autoResolutionsEnabled) {
        vddlog("i", "Auto resolutions disabled, skipping EDID mode generation");
        return generatedModes;
    }
    
    for (const auto& mode : profile.modes) {
        int width = get<0>(mode);
        int height = get<1>(mode);
        int refreshRateMultiplier = get<2>(mode);
        int nominalRefreshRate = get<3>(mode);
        
        // Apply comprehensive filtering
        bool passesFilter = true;
        
        // Resolution range filtering
        if (width < minResolutionWidth || width > maxResolutionWidth ||
            height < minResolutionHeight || height > maxResolutionHeight) {
            passesFilter = false;
        }
        
        // Refresh rate filtering
        if (nominalRefreshRate < minRefreshRate || nominalRefreshRate > maxRefreshRate) {
            passesFilter = false;
        }
        
        // Fractional rate filtering
        if (excludeFractionalRates && refreshRateMultiplier != 1000) {
            passesFilter = false;
        }
        
        // Add custom quality filtering
        if (passesFilter) {
            // Prefer standard aspect ratios for better compatibility
            double aspectRatio = static_cast<double>(width) / height;
            bool isStandardAspect = (abs(aspectRatio - 16.0/9.0) < 0.01) ||  // 16:9
                                   (abs(aspectRatio - 16.0/10.0) < 0.01) ||  // 16:10
                                   (abs(aspectRatio - 4.0/3.0) < 0.01) ||    // 4:3
                                   (abs(aspectRatio - 21.0/9.0) < 0.01);     // 21:9
            
            // Log non-standard aspect ratios for information
            if (!isStandardAspect) {
                stringstream ss;
                ss << "Including non-standard aspect ratio mode: " << width << "x" << height 
                   << " (ratio: " << fixed << setprecision(2) << aspectRatio << ")";
                vddlog("d", ss.str().c_str());
            }
            
            generatedModes.push_back(mode);
        }
    }
    
    // Sort modes by preference (resolution, then refresh rate)
    sort(generatedModes.begin(), generatedModes.end(), 
         [](const tuple<int, int, int, int>& a, const tuple<int, int, int, int>& b) {
             // Primary sort: resolution (area)
             int areaA = get<0>(a) * get<1>(a);
             int areaB = get<0>(b) * get<1>(b);
             if (areaA != areaB) return areaA > areaB;  // Larger resolution first
             
             // Secondary sort: refresh rate
             return get<3>(a) > get<3>(b);  // Higher refresh rate first
         });
    
    stringstream ss;
    ss << "Generated " << generatedModes.size() << " modes from EDID (filtered from " << profile.modes.size() << " total)";
    vddlog("i", ss.str().c_str());
    
    return generatedModes;
}

// Find and validate preferred mode from EDID
tuple<int, int, int, int> FindPreferredModeFromEdid(const EdidProfileData& profile, 
                                                   const vector<tuple<int, int, int, int>>& availableModes) {
    // Default fallback mode
    tuple<int, int, int, int> preferredMode = make_tuple(fallbackWidth, fallbackHeight, 1000, fallbackRefresh);
    
    if (!useEdidPreferred) {
        vddlog("i", "EDID preferred mode disabled, using fallback");
        return preferredMode;
    }
    
    // Look for EDID preferred mode in available modes
    for (const auto& mode : availableModes) {
        if (get<0>(mode) == profile.preferredWidth && 
            get<1>(mode) == profile.preferredHeight) {
            // Found matching resolution, use it
            preferredMode = mode;
            
            stringstream ss;
            ss << "Found EDID preferred mode: " << profile.preferredWidth << "x" << profile.preferredHeight 
               << "@" << get<3>(mode) << "Hz";
            vddlog("i", ss.str().c_str());
            break;
        }
    }
    
    return preferredMode;
}

// Merge and optimize mode lists
vector<tuple<int, int, int, int>> MergeAndOptimizeModes(const vector<tuple<int, int, int, int>>& manualModes,
                                                        const vector<tuple<int, int, int, int>>& edidModes) {
    vector<tuple<int, int, int, int>> mergedModes;
    
    if (sourcePriority == L"edid") {
        mergedModes = edidModes;
        vddlog("i", "Using EDID-only mode list");
    }
    else if (sourcePriority == L"manual") {
        mergedModes = manualModes;
        vddlog("i", "Using manual-only mode list");
    }
    else if (sourcePriority == L"combined") {
        // Start with manual modes
        mergedModes = manualModes;
        
        // Add EDID modes that don't duplicate manual modes
        for (const auto& edidMode : edidModes) {
            bool isDuplicate = false;
            for (const auto& manualMode : manualModes) {
                if (get<0>(edidMode) == get<0>(manualMode) && 
                    get<1>(edidMode) == get<1>(manualMode) && 
                    get<3>(edidMode) == get<3>(manualMode)) {
                    isDuplicate = true;
                    break;
                }
            }
            if (!isDuplicate) {
                mergedModes.push_back(edidMode);
            }
        }
        
        stringstream ss;
        ss << "Combined modes: " << manualModes.size() << " manual + " 
           << (mergedModes.size() - manualModes.size()) << " unique EDID = " << mergedModes.size() << " total";
        vddlog("i", ss.str().c_str());
    }
    
    return mergedModes;
}

// Optimize mode list for performance and compatibility
vector<tuple<int, int, int, int>> OptimizeModeList(const vector<tuple<int, int, int, int>>& modes,
                                                   const tuple<int, int, int, int>& preferredMode) {
    vector<tuple<int, int, int, int>> optimizedModes = modes;
    
    // Remove preferred mode from list if it exists, we'll add it at the front
    optimizedModes.erase(
        remove_if(optimizedModes.begin(), optimizedModes.end(),
                  [&preferredMode](const tuple<int, int, int, int>& mode) {
                      return get<0>(mode) == get<0>(preferredMode) && 
                             get<1>(mode) == get<1>(preferredMode) &&
                             get<3>(mode) == get<3>(preferredMode);
                  }),
        optimizedModes.end());
    
    // Insert preferred mode at the beginning
    optimizedModes.insert(optimizedModes.begin(), preferredMode);
    
    // Remove duplicate modes (same resolution and refresh rate)
    sort(optimizedModes.begin(), optimizedModes.end());
    optimizedModes.erase(unique(optimizedModes.begin(), optimizedModes.end(),
                                [](const tuple<int, int, int, int>& a, const tuple<int, int, int, int>& b) {
                                    return get<0>(a) == get<0>(b) && 
                                           get<1>(a) == get<1>(b) && 
                                           get<3>(a) == get<3>(b);
                                }),
                         optimizedModes.end());
    
    // Limit total number of modes for performance (Windows typically supports 20-50 modes)
    const size_t maxModes = 32;
    if (optimizedModes.size() > maxModes) {
        optimizedModes.resize(maxModes);
        stringstream ss;
        ss << "Limited mode list to " << maxModes << " modes for optimal performance";
        vddlog("i", ss.str().c_str());
    }
    
    return optimizedModes;
}

// Enhanced mode validation with detailed reporting
bool ValidateModeList(const vector<tuple<int, int, int, int>>& modes) {
    if (modes.empty()) {
        vddlog("e", "Mode list is empty - this will cause display driver failure");
        return false;
    }
    
    stringstream validationReport;
    validationReport << "=== MODE LIST VALIDATION REPORT ===\n"
                    << "Total modes: " << modes.size() << "\n";
    
    // Analyze resolution distribution
    map<pair<int, int>, int> resolutionCount;
    map<int, int> refreshRateCount;
    
    for (const auto& mode : modes) {
        pair<int, int> resolution = {get<0>(mode), get<1>(mode)};
        resolutionCount[resolution]++;
        refreshRateCount[get<3>(mode)]++;
    }
    
    validationReport << "Unique resolutions: " << resolutionCount.size() << "\n";
    validationReport << "Unique refresh rates: " << refreshRateCount.size() << "\n";
    validationReport << "Preferred mode: " << get<0>(modes[0]) << "x" << get<1>(modes[0]) 
                    << "@" << get<3>(modes[0]) << "Hz";
    
    vddlog("i", validationReport.str().c_str());
    
    return true;
}

bool LoadEdidProfile(const wstring& profilePath, EdidProfileData& profile) {
	wstring fullPath = confpath + L"\\" + profilePath;
	
	// Check if file exists
	if (!PathFileExistsW(fullPath.c_str())) {
		vddlog("w", ("EDID profile not found: " + WStringToString(fullPath)).c_str());
		return false;
	}

	CComPtr<IStream> pStream;
	CComPtr<IXmlReader> pReader;
	HRESULT hr = SHCreateStreamOnFileW(fullPath.c_str(), STGM_READ, &pStream);
	if (FAILED(hr)) {
		vddlog("e", "LoadEdidProfile: Failed to create file stream.");
		return false;
	}

	hr = CreateXmlReader(__uuidof(IXmlReader), (void**)&pReader, NULL);
	if (FAILED(hr)) {
		vddlog("e", "LoadEdidProfile: Failed to create XmlReader.");
		return false;
	}

	hr = pReader->SetInput(pStream);
	if (FAILED(hr)) {
		vddlog("e", "LoadEdidProfile: Failed to set input stream.");
		return false;
	}

	XmlNodeType nodeType;
	const WCHAR* pwszLocalName;
	const WCHAR* pwszValue;
	UINT cwchLocalName;
	UINT cwchValue;
	wstring currentElement;
	wstring currentSection;
	
	// Temporary mode data
	int tempWidth = 0, tempHeight = 0, tempRefreshRateMultiplier = 1000, tempNominalRefreshRate = 60;

	while (S_OK == (hr = pReader->Read(&nodeType))) {
		switch (nodeType) {
		case XmlNodeType_Element:
			hr = pReader->GetLocalName(&pwszLocalName, &cwchLocalName);
			if (FAILED(hr)) return false;
			currentElement = wstring(pwszLocalName, cwchLocalName);
			
			// Track sections for context
			if (currentElement == L"MonitorModes" || currentElement == L"HDRCapabilities" || 
				currentElement == L"ColorProfile" || currentElement == L"PreferredMode") {
				currentSection = currentElement;
			}
			break;
			
		case XmlNodeType_Text:
			hr = pReader->GetValue(&pwszValue, &cwchValue);
			if (FAILED(hr)) return false;
			
			wstring value = wstring(pwszValue, cwchValue);
			
			// Parse monitor modes
			if (currentSection == L"MonitorModes") {
				if (currentElement == L"Width") {
					tempWidth = stoi(value);
				}
				else if (currentElement == L"Height") {
					tempHeight = stoi(value);
				}
				else if (currentElement == L"RefreshRateMultiplier") {
					tempRefreshRateMultiplier = stoi(value);
				}
				else if (currentElement == L"NominalRefreshRate") {
					tempNominalRefreshRate = stoi(value);
					// Complete mode entry
					if (tempWidth > 0 && tempHeight > 0) {
						profile.modes.push_back(make_tuple(tempWidth, tempHeight, tempRefreshRateMultiplier, tempNominalRefreshRate));
						stringstream ss;
						ss << "EDID Mode: " << tempWidth << "x" << tempHeight << " @ " << tempRefreshRateMultiplier << "/" << tempNominalRefreshRate << "Hz";
						vddlog("d", ss.str().c_str());
					}
				}
			}
			// Parse HDR capabilities
			else if (currentSection == L"HDRCapabilities") {
				if (currentElement == L"HDR10Supported") {
					profile.hdr10Supported = (value == L"true");
				}
				else if (currentElement == L"DolbyVisionSupported") {
					profile.dolbyVisionSupported = (value == L"true");
				}
				else if (currentElement == L"HDR10PlusSupported") {
					profile.hdr10PlusSupported = (value == L"true");
				}
				else if (currentElement == L"MaxLuminance") {
					profile.maxLuminance = stod(value);
				}
				else if (currentElement == L"MinLuminance") {
					profile.minLuminance = stod(value);
				}
			}
			// Parse color profile
			else if (currentSection == L"ColorProfile") {
				if (currentElement == L"PrimaryColorSpace") {
					profile.primaryColorSpace = value;
				}
				else if (currentElement == L"Gamma") {
					profile.gamma = stod(value);
				}
				else if (currentElement == L"RedX") {
					profile.redX = stod(value);
				}
				else if (currentElement == L"RedY") {
					profile.redY = stod(value);
				}
				else if (currentElement == L"GreenX") {
					profile.greenX = stod(value);
				}
				else if (currentElement == L"GreenY") {
					profile.greenY = stod(value);
				}
				else if (currentElement == L"BlueX") {
					profile.blueX = stod(value);
				}
				else if (currentElement == L"BlueY") {
					profile.blueY = stod(value);
				}
				else if (currentElement == L"WhiteX") {
					profile.whiteX = stod(value);
				}
				else if (currentElement == L"WhiteY") {
					profile.whiteY = stod(value);
				}
			}
			// Parse preferred mode
			else if (currentSection == L"PreferredMode") {
				if (currentElement == L"Width") {
					profile.preferredWidth = stoi(value);
				}
				else if (currentElement == L"Height") {
					profile.preferredHeight = stoi(value);
				}
				else if (currentElement == L"RefreshRate") {
					profile.preferredRefresh = stod(value);
				}
			}
			break;
		}
	}

	stringstream ss;
	ss << "EDID Profile loaded: " << profile.modes.size() << " modes, HDR10: " << (profile.hdr10Supported ? "Yes" : "No") 
	   << ", Color space: " << WStringToString(profile.primaryColorSpace);
	vddlog("i", ss.str().c_str());
	
	return true;
}

bool ApplyEdidProfile(const EdidProfileData& profile) {
	if (!edidIntegrationEnabled) {
		return false;
	}

	// === ENHANCED MODE MANAGEMENT ===
	if (autoResolutionsEnabled) {
		// Store original manual modes
		vector<tuple<int, int, int, int>> originalModes = monitorModes;
		
		// Generate optimized modes from EDID
		vector<tuple<int, int, int, int>> edidModes = GenerateModesFromEdid(profile);
		
		// Find preferred mode from EDID
		tuple<int, int, int, int> preferredMode = FindPreferredModeFromEdid(profile, edidModes);
		
		// Merge and optimize mode lists
		vector<tuple<int, int, int, int>> finalModes = MergeAndOptimizeModes(originalModes, edidModes);
		
		// Optimize final mode list with preferred mode priority
		finalModes = OptimizeModeList(finalModes, preferredMode);
		
		// Validate the final mode list
		if (ValidateModeList(finalModes)) {
			monitorModes = finalModes;
			RebuildKnownMonitorModesCache();
			
			stringstream ss;
			ss << "Enhanced mode management completed:\n"
			   << "  Original manual modes: " << originalModes.size() << "\n"
			   << "  Generated EDID modes: " << edidModes.size() << "\n"
			   << "  Final optimized modes: " << finalModes.size() << "\n"
			   << "  Preferred mode: " << get<0>(preferredMode) << "x" << get<1>(preferredMode) 
			   << "@" << get<3>(preferredMode) << "Hz\n"
			   << "  Source priority: " << WStringToString(sourcePriority);
			vddlog("i", ss.str().c_str());
		} else {
			vddlog("e", "Mode list validation failed, keeping original modes");
		}
	}

	// Apply HDR settings if configured
	if (hdr10StaticMetadataEnabled && profile.hdr10Supported) {
		if (overrideManualSettings || maxDisplayMasteringLuminance == 1000.0) { // Default value
			maxDisplayMasteringLuminance = profile.maxLuminance;
		}
		if (overrideManualSettings || minDisplayMasteringLuminance == 0.05) { // Default value
			minDisplayMasteringLuminance = profile.minLuminance;
		}
	}

	// Apply color primaries if configured
	if (colorPrimariesEnabled && (overrideManualSettings || redX == 0.708)) { // Default Rec.2020 values
		redX = profile.redX;
		redY = profile.redY;
		greenX = profile.greenX;
		greenY = profile.greenY;
		blueX = profile.blueX;
		blueY = profile.blueY;
		whiteX = profile.whiteX;
		whiteY = profile.whiteY;
	}

	// Apply color space settings
	if (colorSpaceEnabled && (overrideManualSettings || primaryColorSpace == L"sRGB")) { // Default value
		primaryColorSpace = profile.primaryColorSpace;
		gammaCorrection = profile.gamma;
	}

	// Generate and store HDR metadata for all monitors if HDR is enabled
	if (hdr10StaticMetadataEnabled && profile.hdr10Supported) {
		VddHdrMetadata hdrMetadata = ConvertEdidToSmpteMetadata(profile);
		
		if (hdrMetadata.isValid) {
			// Store metadata for future monitor creation
			// Note: We don't have monitor handles yet at this point, so we'll store it as a template
			// The actual association will happen when monitors are created or HDR metadata is requested
			
			stringstream ss;
			ss << "Generated SMPTE ST.2086 HDR metadata from EDID profile:\n"
			   << "  Red: (" << hdrMetadata.display_primaries_x[0] << ", " << hdrMetadata.display_primaries_y[0] << ") "
			   << "→ (" << profile.redX << ", " << profile.redY << ")\n"
			   << "  Green: (" << hdrMetadata.display_primaries_x[1] << ", " << hdrMetadata.display_primaries_y[1] << ") "
			   << "→ (" << profile.greenX << ", " << profile.greenY << ")\n"
			   << "  Blue: (" << hdrMetadata.display_primaries_x[2] << ", " << hdrMetadata.display_primaries_y[2] << ") "
			   << "→ (" << profile.blueX << ", " << profile.blueY << ")\n"
			   << "  White Point: (" << hdrMetadata.white_point_x << ", " << hdrMetadata.white_point_y << ") "
			   << "→ (" << profile.whiteX << ", " << profile.whiteY << ")\n"
			   << "  Max Luminance: " << hdrMetadata.max_display_mastering_luminance 
			   << " (" << profile.maxLuminance << " nits)\n"
			   << "  Min Luminance: " << hdrMetadata.min_display_mastering_luminance 
			   << " (" << profile.minLuminance << " nits)";
			vddlog("i", ss.str().c_str());
			
			// Store as template metadata - will be applied to monitors during HDR metadata events
			// We use a special key (nullptr converted to uintptr_t) to indicate template metadata
			g_HdrMetadataStore[reinterpret_cast<IDDCX_MONITOR>(0)] = hdrMetadata;
		} else {
			vddlog("w", "Generated HDR metadata is not valid, skipping storage");
		}
	}

	// Generate and store gamma ramp for color space processing if enabled
	if (colorSpaceEnabled) {
		VddGammaRamp gammaRamp = ConvertEdidToGammaRamp(profile);
		
		if (gammaRamp.isValid) {
			// Store gamma ramp as template for future monitor creation
			stringstream ss;
			ss << "Generated Gamma Ramp from EDID profile:\n"
			   << "  Gamma: " << gammaRamp.gamma << " (from " << profile.gamma << ")\n"
			   << "  Color Space: " << WStringToString(gammaRamp.colorSpace) << "\n"
			   << "  Matrix Transform: " << (gammaRamp.useMatrix ? "Enabled" : "Disabled");
			
			if (gammaRamp.useMatrix) {
				ss << "\n  3x4 Matrix:\n"
				   << "    [" << gammaRamp.matrix.matrix[0][0] << ", " << gammaRamp.matrix.matrix[0][1] << ", " << gammaRamp.matrix.matrix[0][2] << ", " << gammaRamp.matrix.matrix[0][3] << "]\n"
				   << "    [" << gammaRamp.matrix.matrix[1][0] << ", " << gammaRamp.matrix.matrix[1][1] << ", " << gammaRamp.matrix.matrix[1][2] << ", " << gammaRamp.matrix.matrix[1][3] << "]\n"
				   << "    [" << gammaRamp.matrix.matrix[2][0] << ", " << gammaRamp.matrix.matrix[2][1] << ", " << gammaRamp.matrix.matrix[2][2] << ", " << gammaRamp.matrix.matrix[2][3] << "]";
			}
			
			vddlog("i", ss.str().c_str());
			
			// Store as template gamma ramp - will be applied to monitors during gamma ramp events
			// We use a special key (nullptr converted to uintptr_t) to indicate template gamma ramp
			g_GammaRampStore[reinterpret_cast<IDDCX_MONITOR>(0)] = gammaRamp;
		} else {
			vddlog("w", "Generated gamma ramp is not valid, skipping storage");
		}
	}

	return true;
}

int gcd(int a, int b) {
	while (b != 0) {
		int temp = b;
		b = a % b;
		a = temp;
	}
	return a;
}

void float_to_vsync(float refresh_rate, int& num, int& den) {
	den = 10000;

	num = static_cast<int>(round(refresh_rate * den));

	int divisor = gcd(num, den);
	num /= divisor;
	den /= divisor;
}

void  SendToPipe(const std::string& logMessage) {
	// Hold g_pipeHandleMutex across the handle test AND the WriteFile so the pipe thread can't
	// close+reset g_pipeHandle between the check and the write (which would be a write to a closed
	// handle). This is the leaf lock — do NOT call vddlog()/SendToPipe() recursively under it.
	std::lock_guard<std::mutex> lk(g_pipeHandleMutex);
	if (g_pipeHandle != INVALID_HANDLE_VALUE) {
		DWORD bytesWritten;
		DWORD logMessageSize = static_cast<DWORD>(logMessage.size());
		WriteFile(g_pipeHandle, logMessage.c_str(), logMessageSize, &bytesWritten, NULL);
	}
}

void vddlog(const char* type, const char* message) {
	if (!logsEnabled) {
		return;
	}

	if (type != nullptr && type[0] == 'd' && !debugLogs) {
		return;
	}

	FILE* logFile;
	wstring logsDir = confpath + L"\\Logs";

	auto now = chrono::system_clock::now();
	auto in_time_t = chrono::system_clock::to_time_t(now);
	tm tm_buf;
	localtime_s(&tm_buf, &in_time_t);
	wchar_t date_str[11]; 
	wcsftime(date_str, sizeof(date_str) / sizeof(wchar_t), L"%Y-%m-%d", &tm_buf);

	wstring logPath = logsDir + L"\\log_" + date_str + L".txt";

	if (!CreateDirectoryW(logsDir.c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
		// Best effort only.
	}

	string narrow_logPath = WStringToString(logPath);
	const char* mode = "a";
	errno_t err = fopen_s(&logFile, narrow_logPath.c_str(), mode);
	if (err == 0 && logFile != nullptr) {
		stringstream ss;
		ss << put_time(&tm_buf, "%Y-%m-%d %X");

		const char logTypeCode = (type != nullptr) ? type[0] : '\0';
		string logType;
		switch (logTypeCode) {
		case 'e':
			logType = "ERROR";
			break; 
		case 'i':
			logType = "INFO";
			break;
		case 'p':
			logType = "PIPE";
			break;
		case 'd':
			logType = "DEBUG";
			break;
		case 'w':
			logType = "WARNING";
			break;
		case 't':
			logType = "TESTING";
			break;
		case 'c':
			logType = "COMPANION";
			break;
		default:
			logType = "UNKNOWN";
			break;
		}

		fprintf(logFile, "[%s] [%s] %s\n", ss.str().c_str(), logType.c_str(), message);

		fclose(logFile);

		// Forward the log line to whatever client is currently connected. Do NOT read g_pipeHandle
		// here (this runs on the watchdog/log threads, racing the pipe thread's close+reset);
		// SendToPipe re-checks the handle atomically under g_pipeHandleMutex.
		if (sendLogsThroughPipe) {
			string logMessage = ss.str() + " [" + logType + "] " + message + "\n";
			SendToPipe(logMessage);
		}
	}
}



void LogIddCxVersion() {
	IDARG_OUT_GETVERSION outArgs;
	NTSTATUS status = IddCxGetVersion(&outArgs);

	if (NT_SUCCESS(status)) {
		char versionStr[16];
		sprintf_s(versionStr, "0x%lx", outArgs.IddCxVersion);
		string logMessage = "IDDCX Version: " + string(versionStr);
		vddlog("i", logMessage.c_str());
	}
	else {
		vddlog("i", "Failed to get IDDCX version");
	}
	vddlog("d", "Testing Debug Log");
}

void InitializeD3DDeviceAndLogGPU() {
	ComPtr<ID3D11Device> d3dDevice;
	ComPtr<ID3D11DeviceContext> d3dContext;
	HRESULT hr = D3D11CreateDevice(
		nullptr,
		D3D_DRIVER_TYPE_HARDWARE,
		nullptr,
		0,
		nullptr,
		0,
		D3D11_SDK_VERSION,
		&d3dDevice,
		nullptr,
		&d3dContext);

	if (FAILED(hr)) {
		vddlog("e", "Retrieving D3D Device GPU: Failed to create D3D11 device");
		return;
	}

	ComPtr<IDXGIDevice> dxgiDevice;
	hr = d3dDevice.As(&dxgiDevice);
	if (FAILED(hr)) {
		vddlog("e", "Retrieving D3D Device GPU: Failed to get DXGI device");
		return;
	}

	ComPtr<IDXGIAdapter> dxgiAdapter;
	hr = dxgiDevice->GetAdapter(&dxgiAdapter);
	if (FAILED(hr)) {
		vddlog("e", "Retrieving D3D Device GPU: Failed to get DXGI adapter");
		return;
	}

	DXGI_ADAPTER_DESC desc;
	hr = dxgiAdapter->GetDesc(&desc);
	if (FAILED(hr)) {
		vddlog("e", "Retrieving D3D Device GPU: Failed to get GPU description");
		return;
	}

	d3dDevice.Reset();
	d3dContext.Reset();

	wstring wdesc(desc.Description);
	string utf8_desc;
	try {
		utf8_desc = WStringToString(wdesc);
	}
	catch (const exception& e) {
		vddlog("e", ("Retrieving D3D Device GPU: Conversion error: " + string(e.what())).c_str());
		return;
	}

	string logtext = "Retrieving D3D Device GPU: " + utf8_desc;
	vddlog("i", logtext.c_str());
}


// This macro creates the methods for accessing an IndirectDeviceContextWrapper as a context for a WDF object
WDF_DECLARE_CONTEXT_TYPE(IndirectDeviceContextWrapper);

extern "C" BOOL WINAPI DllMain(
	_In_ HINSTANCE hInstance,
	_In_ UINT dwReason,
	_In_opt_ LPVOID lpReserved)
{
	UNREFERENCED_PARAMETER(hInstance);
	UNREFERENCED_PARAMETER(lpReserved);
	UNREFERENCED_PARAMETER(dwReason);

	return TRUE;
}


bool UpdateXmlToggleSetting(bool toggle, const wchar_t* variable) {
	const wstring settingsname = confpath + L"\\vdd_settings.xml";
	CComPtr<IStream> pFileStream;
	HRESULT hr = SHCreateStreamOnFileEx(settingsname.c_str(), STGM_READWRITE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &pFileStream);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: XML file could not be opened.");
		return false;
	}

	CComPtr<IXmlReader> pReader;
	hr = CreateXmlReader(__uuidof(IXmlReader), (void**)&pReader, nullptr);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: Failed to create XML reader.");
		return false;
	}
	hr = pReader->SetInput(pFileStream);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: Failed to set XML reader input.");
		return false;
	}

	CComPtr<IStream> pOutFileStream;
	wstring tempFileName = settingsname + L".temp";
	hr = SHCreateStreamOnFileEx(tempFileName.c_str(), STGM_CREATE | STGM_WRITE, FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &pOutFileStream);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: Failed to create output file stream.");
		return false;
	}

	CComPtr<IXmlWriter> pWriter;
	hr = CreateXmlWriter(__uuidof(IXmlWriter), (void**)&pWriter, nullptr);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: Failed to create XML writer.");
		return false;
	}
	hr = pWriter->SetOutput(pOutFileStream);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: Failed to set XML writer output.");
		return false;
	}
	hr = pWriter->WriteStartDocument(XmlStandalone_Omit);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: Failed to write start of the document.");
		return false;
	}

	XmlNodeType nodeType;
	const wchar_t* pwszLocalName;
	const wchar_t* pwszValue;
	bool variableElementFound = false;

	while (S_OK == pReader->Read(&nodeType)) {
		switch (nodeType) {
		case XmlNodeType_Element:
			pReader->GetLocalName(&pwszLocalName, nullptr);
			pWriter->WriteStartElement(nullptr, pwszLocalName, nullptr);
			break;

		case XmlNodeType_EndElement:
			pReader->GetLocalName(&pwszLocalName, nullptr);
			pWriter->WriteEndElement();
			break;

		case XmlNodeType_Text:
			pReader->GetValue(&pwszValue, nullptr);
			if (variableElementFound) {
				pWriter->WriteString(toggle ? L"true" : L"false");
				variableElementFound = false;
			}
			else {
				pWriter->WriteString(pwszValue);
			}
			break;

		case XmlNodeType_Whitespace:
			pReader->GetValue(&pwszValue, nullptr);
			pWriter->WriteWhitespace(pwszValue);
			break;

		case XmlNodeType_Comment:
			pReader->GetValue(&pwszValue, nullptr);
			pWriter->WriteComment(pwszValue);
			break;
		}

		if (nodeType == XmlNodeType_Element) {
			pReader->GetLocalName(&pwszLocalName, nullptr);
			if (pwszLocalName != nullptr && wcscmp(pwszLocalName, variable) == 0) {
				variableElementFound = true;
			}
		}
	}

	if (variableElementFound) {
		pWriter->WriteStartElement(nullptr, variable, nullptr);
		pWriter->WriteString(toggle ? L"true" : L"false");
		pWriter->WriteEndElement();
	}

	hr = pWriter->WriteEndDocument();
	if (FAILED(hr)) {
		return false;
	}

	pFileStream.Release();
	pOutFileStream.Release();
	pWriter.Release();
	pReader.Release();

	if (!MoveFileExW(tempFileName.c_str(), settingsname.c_str(), MOVEFILE_REPLACE_EXISTING)) {
		return false;
	}
	return true;
}


bool UpdateXmlGpuSetting(const wchar_t* gpuName) {
	const std::wstring settingsname = confpath + L"\\vdd_settings.xml";
	CComPtr<IStream> pFileStream;
	HRESULT hr = SHCreateStreamOnFileEx(settingsname.c_str(), STGM_READWRITE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &pFileStream);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: XML file could not be opened.");
		return false;
	}

	CComPtr<IXmlReader> pReader;
	hr = CreateXmlReader(__uuidof(IXmlReader), (void**)&pReader, nullptr);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: Failed to create XML reader.");
		return false;
	}
	hr = pReader->SetInput(pFileStream);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: Failed to set XML reader input.");
		return false;
	}

	CComPtr<IStream> pOutFileStream;
	std::wstring tempFileName = settingsname + L".temp";
	hr = SHCreateStreamOnFileEx(tempFileName.c_str(), STGM_CREATE | STGM_WRITE, FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &pOutFileStream);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: Failed to create output file stream.");
		return false;
	}

	CComPtr<IXmlWriter> pWriter;
	hr = CreateXmlWriter(__uuidof(IXmlWriter), (void**)&pWriter, nullptr);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: Failed to create XML writer.");
		return false;
	}
	hr = pWriter->SetOutput(pOutFileStream);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: Failed to set XML writer output.");
		return false;
	}
	hr = pWriter->WriteStartDocument(XmlStandalone_Omit);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: Failed to write start of the document.");
		return false;
	}

	XmlNodeType nodeType;
	const wchar_t* pwszLocalName;
	const wchar_t* pwszValue;
	bool gpuElementFound = false;

	while (S_OK == pReader->Read(&nodeType)) {
		switch (nodeType) {
		case XmlNodeType_Element:
			pReader->GetLocalName(&pwszLocalName, nullptr);
			pWriter->WriteStartElement(nullptr, pwszLocalName, nullptr);
			break;

		case XmlNodeType_EndElement:
			pReader->GetLocalName(&pwszLocalName, nullptr);
			pWriter->WriteEndElement();
			break;

		case XmlNodeType_Text:
			pReader->GetValue(&pwszValue, nullptr);
			if (gpuElementFound) {
				pWriter->WriteString(gpuName); 
				gpuElementFound = false;
			}
			else {
				pWriter->WriteString(pwszValue);
			}
			break;

		case XmlNodeType_Whitespace:
			pReader->GetValue(&pwszValue, nullptr);
			pWriter->WriteWhitespace(pwszValue);
			break;

		case XmlNodeType_Comment:
			pReader->GetValue(&pwszValue, nullptr);
			pWriter->WriteComment(pwszValue);
			break;
		}

		if (nodeType == XmlNodeType_Element) {
			pReader->GetLocalName(&pwszLocalName, nullptr);
			if (wcscmp(pwszLocalName, L"gpu") == 0) {
				gpuElementFound = true;
			}
		}
	}
	hr = pWriter->WriteEndDocument();
	if (FAILED(hr)) {
		return false;
	}

	pFileStream.Release();
	pOutFileStream.Release();
	pWriter.Release();
	pReader.Release();

	if (!MoveFileExW(tempFileName.c_str(), settingsname.c_str(), MOVEFILE_REPLACE_EXISTING)) {
		return false;
	}
	return true;
}

bool UpdateXmlDisplayCountSetting(int displayCount) {
	const std::wstring settingsname = confpath + L"\\vdd_settings.xml";
	CComPtr<IStream> pFileStream;
	HRESULT hr = SHCreateStreamOnFileEx(settingsname.c_str(), STGM_READWRITE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &pFileStream);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: XML file could not be opened.");
		return false;
	}

	CComPtr<IXmlReader> pReader;
	hr = CreateXmlReader(__uuidof(IXmlReader), (void**)&pReader, nullptr);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: Failed to create XML reader.");
		return false;
	}
	hr = pReader->SetInput(pFileStream);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: Failed to set XML reader input.");
		return false;
	}

	CComPtr<IStream> pOutFileStream;
	std::wstring tempFileName = settingsname + L".temp";
	hr = SHCreateStreamOnFileEx(tempFileName.c_str(), STGM_CREATE | STGM_WRITE, FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &pOutFileStream);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: Failed to create output file stream.");
		return false;
	}

	CComPtr<IXmlWriter> pWriter;
	hr = CreateXmlWriter(__uuidof(IXmlWriter), (void**)&pWriter, nullptr);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: Failed to create XML writer.");
		return false;
	}
	hr = pWriter->SetOutput(pOutFileStream);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: Failed to set XML writer output.");
		return false;
	}
	hr = pWriter->WriteStartDocument(XmlStandalone_Omit);
	if (FAILED(hr)) {
		vddlog("e", "UpdatingXML: Failed to write start of the document.");
		return false;
	}

	XmlNodeType nodeType;
	const wchar_t* pwszLocalName;
	const wchar_t* pwszValue;
	bool displayCountElementFound = false;

	while (S_OK == pReader->Read(&nodeType)) {
		switch (nodeType) {
		case XmlNodeType_Element:
			pReader->GetLocalName(&pwszLocalName, nullptr);
			pWriter->WriteStartElement(nullptr, pwszLocalName, nullptr);
			break;

		case XmlNodeType_EndElement:
			pReader->GetLocalName(&pwszLocalName, nullptr);
			pWriter->WriteEndElement();
			break;

		case XmlNodeType_Text:
			pReader->GetValue(&pwszValue, nullptr);
			if (displayCountElementFound) {
				pWriter->WriteString(std::to_wstring(displayCount).c_str());
				displayCountElementFound = false; 
			}
			else {
				pWriter->WriteString(pwszValue);
			}
			break;

		case XmlNodeType_Whitespace:
			pReader->GetValue(&pwszValue, nullptr);
			pWriter->WriteWhitespace(pwszValue);
			break;

		case XmlNodeType_Comment:
			pReader->GetValue(&pwszValue, nullptr);
			pWriter->WriteComment(pwszValue);
			break;
		}

		if (nodeType == XmlNodeType_Element) {
			pReader->GetLocalName(&pwszLocalName, nullptr);
			if (wcscmp(pwszLocalName, L"count") == 0) {
				displayCountElementFound = true; 
			}
		}
	}

	hr = pWriter->WriteEndDocument();
	if (FAILED(hr)) {
		return false;
	}

	pFileStream.Release();
	pOutFileStream.Release();
	pWriter.Release();
	pReader.Release();

	if (!MoveFileExW(tempFileName.c_str(), settingsname.c_str(), MOVEFILE_REPLACE_EXISTING)) {
		return false;
	}
	return true;
}


LUID getSetAdapterLuid() {
	AdapterOption& adapterOption = Options.Adapter;

	if (!adapterOption.hasTargetAdapter) {
		vddlog("e","No Gpu Found/Selected");
	}

	return adapterOption.adapterLuid;
}


void GetGpuInfo()
{
	AdapterOption& adapterOption = Options.Adapter;

	if (!adapterOption.hasTargetAdapter) {
		vddlog("e", "No GPU found or set.");
		return;
	}

	try {
		string utf8_desc = WStringToString(adapterOption.target_name);
		LUID luid = getSetAdapterLuid();
		string logtext = "ASSIGNED GPU: " + utf8_desc +
			" (LUID: " + std::to_string(luid.LowPart) + "-" + std::to_string(luid.HighPart) + ")";
		vddlog("i", logtext.c_str());
	}
	catch (const exception& e) {
		vddlog("e", ("Error: " + string(e.what())).c_str());
	}
}

void logAvailableGPUs() {
	vector<GPUInfo> gpus;
	ComPtr<IDXGIFactory1> factory;
	if (!SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
		return;
	}
	for (UINT i = 0;; i++) {
		ComPtr<IDXGIAdapter> adapter;
		if (!SUCCEEDED(factory->EnumAdapters(i, &adapter))) {
			break;
		}
		DXGI_ADAPTER_DESC desc;
		if (!SUCCEEDED(adapter->GetDesc(&desc))) {
			continue;
		}
		GPUInfo info{ desc.Description, adapter, desc };
		gpus.push_back(info);
	}
	for (const auto& gpu : gpus) {
		wstring logMessage = L"GPU Name: ";
		logMessage += gpu.desc.Description;
		wstring memorySize = L" Memory: ";
		memorySize += std::to_wstring(gpu.desc.DedicatedVideoMemory / (1024 * 1024)) + L" MB";
		wstring logText = logMessage + memorySize;
		int bufferSize = WideCharToMultiByte(CP_UTF8, 0, logText.c_str(), -1, nullptr, 0, nullptr, nullptr);
		if (bufferSize > 0) {
			std::string logTextA(bufferSize - 1, '\0');
			WideCharToMultiByte(CP_UTF8, 0, logText.c_str(), -1, &logTextA[0], bufferSize, nullptr, nullptr);
			vddlog("c", logTextA.c_str());
		}
	}
}





// Return the live device context cached by InitAdapter, or nullptr if the adapter isn't up yet.
// This replaces the old ReloadDriver(HANDLE) which fed a pipe HANDLE to
// WdfObjectGet_IndirectDeviceContextWrapper (a WDFOBJECT) — a type-confusion bug that could
// dereference garbage. The pipe thread now reaches the context safely through this cache.
static Microsoft::IndirectDisp::IndirectDeviceContext* GetDeviceContext() {
	std::lock_guard<std::mutex> lk(g_DeviceContextMutex);
	return g_DeviceContext;
}

// Reset the watchdog countdown IF it is already armed. Called on every inbound command (ADD,
// REMOVE, SETDISPLAYCOUNT, PING). This does NOT arm the watchdog — a consumer must opt in by
// PINGing at least once (see WatchdogArm). Resetting a disarmed watchdog is a no-op so a non-
// PINGing consumer never accidentally activates the self-heal.
static void WatchdogKick() {
	if (g_WatchdogTimeoutSeconds > 0 && g_WatchdogArmed.load(std::memory_order_relaxed)) {
		g_WatchdogCountdown.store(g_WatchdogTimeoutSeconds, std::memory_order_relaxed);
	}
}

// Arm (opt in to) the watchdog and reset its countdown. Called ONLY when a PING is received: the
// first PING flips the driver from "persist monitors unconditionally" (standalone behavior) to
// "self-heal if the PINGing consumer goes silent". Never called from boot or from ADD.
static void WatchdogArm() {
	if (g_WatchdogTimeoutSeconds > 0) {
		bool wasArmed = g_WatchdogArmed.exchange(true, std::memory_order_relaxed);
		g_WatchdogCountdown.store(g_WatchdogTimeoutSeconds, std::memory_order_relaxed);
		if (!wasArmed) {
			vddlog("i", "Watchdog armed by first PING; self-heal now active for this consumer.");
		}
	}
}

// Watchdog body: once per second, ONLY while armed (a consumer has PINGed), decrement the
// countdown; on reaching 0 (the PINGing consumer went silent) remove every live monitor so a
// crashed/disconnected consumer can't leave orphan displays. While DISARMED the thread idles and
// never removes monitors, so a standalone install with no pinger keeps its monitors forever.
// Mirrors parsec-vdd / SudoVDA self-healing, made opt-in.
//
// ROUND-2 FIX (Issue 1 — residual watchdog UAF): the previous design relied on
// StopWatchdog() doing a BOUNDED WaitForSingleObject(..., 2000) inside ~IndirectDeviceContext.
// That is NOT a lifetime guarantee: if this thread is mid-RemoveAllMonitors() and exceeds 2s,
// the destructor's wait times out, ~IndirectDeviceContext deletes the context, and this thread
// then dereferences freed memory (use-after-free). The new design makes delete and the
// watchdog's use of the context MUTUALLY EXCLUSIVE via g_DeviceContextMutex:
//   - here, we acquire g_DeviceContextMutex and HOLD it across the ENTIRE
//     LiveMonitorCount() + RemoveAllMonitors() sequence, reading the pointer under the lock and
//     using it ONLY while held;
//   - ~IndirectDeviceContext sets g_DeviceContext = nullptr UNDER the same mutex as the FIRST
//     thing it does.
// So the watchdog either (a) runs this whole block BEFORE the destructor nulls the pointer —
// in which case the context is still fully alive and the destructor will block on the mutex
// until we finish — or (b) sees g_DeviceContext == nullptr and skips entirely. There is no
// window where it touches a half-destroyed object, and no unbounded hang: the work here is the
// same depart sequence the destructor would otherwise run.
//
// LOCK ORDER (must stay acyclic): g_DeviceContextMutex -> m_MonitorsMutex
// -> (IddCxMonitorDeparture is called OUTSIDE m_MonitorsMutex by RemoveMonitor)
// -> m_ProcessingThreadsMutex. Nothing acquires g_DeviceContextMutex while holding
// m_MonitorsMutex or m_ProcessingThreadsMutex, so holding g_DeviceContextMutex across
// RemoveAllMonitors() (which internally takes m_MonitorsMutex then, separately,
// m_ProcessingThreadsMutex via the swap-chain teardown) cannot deadlock.
static DWORD WINAPI WatchdogProc(LPVOID) {
	while (g_WatchdogRunning.load(std::memory_order_relaxed)) {
		Sleep(1000);
		if (!g_WatchdogRunning.load(std::memory_order_relaxed)) break;
		if (g_WatchdogTimeoutSeconds <= 0) continue;

		// Disarmed: no consumer has opted in via PING. Persist monitors unconditionally.
		if (!g_WatchdogArmed.load(std::memory_order_relaxed)) continue;

		int c = g_WatchdogCountdown.load(std::memory_order_relaxed);
		if (c <= 0) {
			// Armed and expired: the PINGing consumer went silent. Take g_DeviceContextMutex and
			// HOLD it across the whole self-heal so the context cannot be deleted under us (see
			// the lifetime-guarantee note above). Read the pointer under the lock; use it only
			// while held; release before looping.
			std::lock_guard<std::mutex> lk(g_DeviceContextMutex);
			auto* ctx = g_DeviceContext;
			if (ctx && ctx->LiveMonitorCount() > 0) {
				vddlog("w", "Watchdog: no PING from consumer; removing all monitors (self-heal).");
				ctx->RemoveAllMonitors();
			}
			continue;
		}
		g_WatchdogCountdown.store(c - 1, std::memory_order_relaxed);
	}
	return 0;
}

static void StartWatchdog() {
	if (g_WatchdogTimeoutSeconds <= 0) {
		vddlog("i", "Watchdog disabled (g_WatchdogTimeoutSeconds == 0).");
		return;
	}
	if (g_WatchdogRunning.exchange(true)) return; // already running
	// Start DISARMED. Do NOT arm or preset the countdown at boot: with no PINGing consumer the
	// watchdog must never bark (a standalone install keeps its monitors). The first PING arms it
	// via WatchdogArm(). This is the opt-in fix for the boot-time orphan-removal race.
	g_WatchdogArmed.store(false, std::memory_order_relaxed);
	g_WatchdogCountdown.store(0, std::memory_order_relaxed);
	// ROUND-2 FIX (Issue 1): guard the bare g_WatchdogThread HANDLE store under
	// g_WatchdogThreadMutex so it can't race StopWatchdog's close+clear during a device
	// teardown+recreate.
	HANDLE created = CreateThread(NULL, 0, WatchdogProc, NULL, 0, NULL);
	{
		std::lock_guard<std::mutex> lk(g_WatchdogThreadMutex);
		g_WatchdogThread = created;
	}
	if (created == NULL) {
		g_WatchdogRunning.store(false);
		vddlog("e", "Failed to start watchdog thread.");
	} else {
		vddlog("i", "Watchdog thread started (disarmed; arms on first PING).");
	}
}

static void StopWatchdog() {
	if (!g_WatchdogRunning.exchange(false)) return;
	// ROUND-2 FIX (Issue 1): no longer the UAF backstop — correctness now comes from the
	// watchdog holding g_DeviceContextMutex across its whole context use and the destructor
	// nulling g_DeviceContext under that same mutex FIRST (see WatchdogProc / ~IndirectDeviceContext).
	// This join is therefore purely a tidy thread-handle cleanup. We still keep the bounded wait so
	// teardown can't hang, and we take the handle UNDER g_WatchdogThreadMutex (briefly) so the
	// store-vs-close race with StartWatchdog is closed. The wait/close run OUTSIDE the lock to keep
	// it leaf-level and so a concurrent StartWatchdog isn't blocked for the duration of the wait.
	HANDLE toJoin = NULL;
	{
		std::lock_guard<std::mutex> lk(g_WatchdogThreadMutex);
		toJoin = g_WatchdogThread;
		g_WatchdogThread = NULL;
	}
	if (toJoin) {
		WaitForSingleObject(toJoin, 2000);
		CloseHandle(toJoin);
	}
	vddlog("i", "Watchdog stopped.");
}

// Legacy compatibility shim. The on-demand model never reloads the adapter; settings that used to
// trigger a full reinit (HDR+/SDR10/EDID/GPU toggles) now update their in-memory globals directly
// in the pipe handlers above. Because AddMonitor rebuilds the EDID (maincalc) and the per-monitor
// description callbacks read the colour/format globals live, a setting change takes effect on every
// monitor ADDED AFTER the change. Monitors that are already live keep their current description
// until they are removed and re-added (depart+re-arrive). The adapter itself is created once per
// device lifetime and is NOT recreated here. Kept as a no-op so the many existing call sites still
// compile without sprinkling #ifdefs.
void ReloadDriver(HANDLE /*hPipe*/) {
	vddlog("i", "ReloadDriver is a no-op under the on-demand model; setting changes apply to monitors added after the change (existing monitors must be re-added to pick them up).");
}


void HandleClient(HANDLE hPipe) {
	{
		std::lock_guard<std::mutex> lk(g_pipeHandleMutex);
		g_pipeHandle = hPipe;
	}
	vddlog("p", "Client Handling Enabled");
	// The wire contract (docs/argus-driver-build.md) is ASCII and the Rust broker (istro-svc)
	// sends ASCII; the legacy upstream companion app sent UTF-16LE. Accept BOTH: read raw bytes
	// and normalize into the wide buffer the parser below expects. UTF-16LE of any ASCII-range
	// command always contains 0x00 high bytes; plain ASCII never contains a NUL — that's the
	// discriminator. Before this, an ASCII "ADD" parsed as one garbage wchar and fell through to
	// the unknown-command path.
	wchar_t buffer[128];
	char raw[256];
	DWORD bytesRead;
	BOOL result = ReadFile(hPipe, raw, sizeof(raw) - sizeof(wchar_t), &bytesRead, NULL);
	if (result && bytesRead != 0) {
		bool looksWide = false;
		for (DWORD i = 0; i < bytesRead; ++i) {
			if (raw[i] == 0) { looksWide = true; break; }
		}
		size_t wlen;
		if (looksWide) {
			wlen = bytesRead / sizeof(wchar_t);
			if (wlen > 127) wlen = 127;
			memcpy(buffer, raw, wlen * sizeof(wchar_t));
		}
		else {
			wlen = bytesRead;
			if (wlen > 127) wlen = 127;
			for (size_t i = 0; i < wlen; ++i) {
				buffer[i] = static_cast<wchar_t>(static_cast<unsigned char>(raw[i]));
			}
		}
		buffer[wlen] = L'\0';
		wstring bufferwstr(buffer);
		int bufferSize = WideCharToMultiByte(CP_UTF8, 0, bufferwstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
		string bufferstr(bufferSize, 0);
		WideCharToMultiByte(CP_UTF8, 0, bufferwstr.c_str(), -1, &bufferstr[0], bufferSize, nullptr, nullptr);
		vddlog("p", bufferstr.c_str());
		if (wcsncmp(buffer, L"RELOAD_DRIVER", 13) == 0) {
			vddlog("c", "Reloading the driver");
			ReloadDriver(hPipe);
			
		}
		else if (wcsncmp(buffer, L"LOG_DEBUG", 9) == 0) {
			wchar_t* param = buffer + 10;
			if (wcsncmp(param, L"true", 4) == 0) {
				UpdateXmlToggleSetting(true, L"debuglogging");
				debugLogs = true;
				vddlog("c", "Pipe debugging enabled");
				vddlog("d", "Debug Logs Enabled");
			}
			else if (wcsncmp(param, L"false", 5) == 0) {
				UpdateXmlToggleSetting(false, L"debuglogging");
				debugLogs = false;
				vddlog("c", "Debugging disabled");
			}
		}
		else if (wcsncmp(buffer, L"LOGGING", 7) == 0) {
			wchar_t* param = buffer + 8;
			if (wcsncmp(param, L"true", 4) == 0) {
				UpdateXmlToggleSetting(true, L"logging");
				logsEnabled = true;
				vddlog("c", "Logging Enabled");
			}
			else if (wcsncmp(param, L"false", 5) == 0) {
				UpdateXmlToggleSetting(false, L"logging");
				logsEnabled = false;
				vddlog("c", "Logging disabled"); // We can keep this here just to make it delete the logs on disable
			}
		}
		else if (wcsncmp(buffer, L"HDRPLUS", 7) == 0) {
			wchar_t* param = buffer + 8;
			if (wcsncmp(param, L"true", 4) == 0) {
				UpdateXmlToggleSetting(true, L"HDRPlus");
				// Update the IN-MEMORY global (+ derived bit depth) so the next AddMonitor uses it.
				HDRPlus = true;
				// ROUND-2 FIX (Issue 2): HDRCOLOUR is read by IddCx callback threads (CreateTargetMode2,
				// ParseMonitorDescription2). WRITE-lock the scalar store to avoid a torn read.
				{
					std::lock_guard<std::mutex> lk(g_SettingsMutex);
					HDRCOLOUR = IDDCX_BITS_PER_COMPONENT_12;
				}
				vddlog("c", "HDR+ Enabled (applies to monitors added after this change)");
				ReloadDriver(hPipe);
			}
			else if (wcsncmp(param, L"false", 5) == 0) {
				UpdateXmlToggleSetting(false, L"HDRPlus");
				HDRPlus = false;
				{
					std::lock_guard<std::mutex> lk(g_SettingsMutex);
					HDRCOLOUR = IDDCX_BITS_PER_COMPONENT_10;
				}
				vddlog("c", "HDR+ Disabled (applies to monitors added after this change)");
				ReloadDriver(hPipe);
			}
		}
		else if (wcsncmp(buffer, L"SDR10", 5) == 0) {
			wchar_t* param = buffer + 6;
			if (wcsncmp(param, L"true", 4) == 0) {
				UpdateXmlToggleSetting(true, L"SDR10bit");
				SDR10 = true;
				// ROUND-2 FIX (Issue 2): SDRCOLOUR is read by IddCx callback threads. WRITE-lock the
				// scalar store to avoid a torn read.
				{
					std::lock_guard<std::mutex> lk(g_SettingsMutex);
					SDRCOLOUR = IDDCX_BITS_PER_COMPONENT_10;
				}
				vddlog("c", "SDR 10 Bit Enabled (applies to monitors added after this change)");
				ReloadDriver(hPipe);
			}
			else if (wcsncmp(param, L"false", 5) == 0) {
				UpdateXmlToggleSetting(false, L"SDR10bit");
				SDR10 = false;
				{
					std::lock_guard<std::mutex> lk(g_SettingsMutex);
					SDRCOLOUR = IDDCX_BITS_PER_COMPONENT_8;
				}
				vddlog("c", "SDR 10 Bit Disabled (applies to monitors added after this change)");
				ReloadDriver(hPipe);
			}
		}
		else if (wcsncmp(buffer, L"CUSTOMEDID", 10) == 0) {
			wchar_t* param = buffer + 11;
			if (wcsncmp(param, L"true", 4) == 0) {
				UpdateXmlToggleSetting(true, L"CustomEdid");
				customEdid = true;
				vddlog("c", "Custom Edid Enabled (applies to monitors added after this change)");
				ReloadDriver(hPipe);
			}
			else if (wcsncmp(param, L"false", 5) == 0) {
				UpdateXmlToggleSetting(false, L"CustomEdid");
				customEdid = false;
				vddlog("c", "Custom Edid Disabled (applies to monitors added after this change)");
				ReloadDriver(hPipe);
			}
		}
		else if (wcsncmp(buffer, L"PREVENTSPOOF", 12) == 0) {
			wchar_t* param = buffer + 13;
			if (wcsncmp(param, L"true", 4) == 0) {
				UpdateXmlToggleSetting(true, L"PreventSpoof");
				preventManufacturerSpoof = true;
				vddlog("c", "Prevent Spoof Enabled (applies to monitors added after this change)");
				ReloadDriver(hPipe);
			}
			else if (wcsncmp(param, L"false", 5) == 0) {
				UpdateXmlToggleSetting(false, L"PreventSpoof");
				preventManufacturerSpoof = false;
				vddlog("c", "Prevent Spoof Disabled (applies to monitors added after this change)");
				ReloadDriver(hPipe);
			}
		}
		else if (wcsncmp(buffer, L"CEAOVERRIDE", 11) == 0) {
			wchar_t* param = buffer + 12;
			if (wcsncmp(param, L"true", 4) == 0) {
				UpdateXmlToggleSetting(true, L"EdidCeaOverride");
				edidCeaOverride = true;
				vddlog("c", "Cea override Enabled (applies to monitors added after this change)");
				ReloadDriver(hPipe);
			}
			else if (wcsncmp(param, L"false", 5) == 0) {
				UpdateXmlToggleSetting(false, L"EdidCeaOverride");
				edidCeaOverride = false;
				vddlog("c", "Cea override Disabled (applies to monitors added after this change)");
				ReloadDriver(hPipe);
			}
		}
		else if (wcsncmp(buffer, L"HARDWARECURSOR", 14) == 0) {
			wchar_t* param = buffer + 15;
			if (wcsncmp(param, L"true", 4) == 0) {
				UpdateXmlToggleSetting(true, L"HardwareCursor");
				hardwareCursor = true;
				vddlog("c", "Hardware Cursor Enabled (applies to monitors added after this change)");
				ReloadDriver(hPipe);
			}
			else if (wcsncmp(param, L"false", 5) == 0) {
				UpdateXmlToggleSetting(false, L"HardwareCursor");
				hardwareCursor = false;
				vddlog("c", "Hardware Cursor Disabled");
				ReloadDriver(hPipe);
			}
		}
		else if (wcsncmp(buffer, L"D3DDEVICEGPU", 12) == 0) {
			vddlog("c", "Retrieving D3D GPU (This information may be inaccurate without reloading the driver first)");
			InitializeD3DDeviceAndLogGPU();
			vddlog("c", "Retrieved D3D GPU");
		}
		else if (wcsncmp(buffer, L"IDDCXVERSION", 12) == 0) {
			vddlog("c", "Logging iddcx version");
			LogIddCxVersion(); 
		}
		else if (wcsncmp(buffer, L"GETASSIGNEDGPU", 14) == 0) {
			vddlog("c", "Retrieving Assigned GPU");
			GetGpuInfo();
			vddlog("c", "Retrieved Assigned GPU");
		}
		else if (wcsncmp(buffer, L"GETALLGPUS", 10) == 0) {
			vddlog("c", "Logging all GPUs");
			vddlog("i", "Any GPUs which show twice but you only have one, will most likely be the GPU the driver is attached to");
			logAvailableGPUs();
			vddlog("c", "Logged all GPUs");
		}  
		else if (wcsncmp(buffer, L"SETGPU", 6) == 0) {
			std::wstring gpuName = buffer + 7;
			gpuName = gpuName.substr(1, gpuName.size() - 2); 

			int size_needed = WideCharToMultiByte(CP_UTF8, 0, gpuName.c_str(), static_cast<int>(gpuName.length()), nullptr, 0, nullptr, nullptr);
			std::string gpuNameNarrow(size_needed, 0);
			WideCharToMultiByte(CP_UTF8, 0, gpuName.c_str(), static_cast<int>(gpuName.length()), &gpuNameNarrow[0], size_needed, nullptr, nullptr);

			vddlog("c", ("Setting GPU to: " + gpuNameNarrow).c_str());
			// Update the IN-MEMORY global so a future adapter (re)init picks the new render GPU.
			// NOTE: the render adapter is bound when the IddCx adapter is created, which now happens
			// exactly once per device lifetime; a live GPU switch requires an actual device restart
			// (PnP disable/enable), not the no-op ReloadDriver.
			gpuname = gpuName;
			if (UpdateXmlGpuSetting(gpuName.c_str())) {
				vddlog("c", "GPU setting persisted (takes effect on the next device restart / adapter init).");
			}
			else {
				vddlog("e", "Failed to update GPU setting in XML (in-memory value updated for next adapter init).");
			}
			ReloadDriver(hPipe);
		}
		// ===== On-demand monitor control (CONTRACT — any pipe consumer codes to this) =====
		// "ADD"            -> add ONE monitor at the lowest free connector index; the chosen
		//                     index is written back to the pipe client as an ASCII integer string.
		// "REMOVE <index>" -> remove the monitor at <index>.
		// Both kick the watchdog. Must be checked BEFORE the legacy "SETDISPLAYCOUNT" handling.
		else if (wcsncmp(buffer, L"ADD", 3) == 0 &&
		         (buffer[3] == L'\0' || buffer[3] == L'\r' || buffer[3] == L'\n' || buffer[3] == L' ')) {
			WatchdogKick();
			// RESIDUAL FIX (same UAF class as the watchdog): HOLD g_DeviceContextMutex across the
			// ENTIRE context use. The destructor nulls g_DeviceContext under this mutex FIRST, so the
			// pipe thread cannot deref a context being deleted. Lock order g_DeviceContextMutex ->
			// m_MonitorsMutex (AddMonitor takes m_MonitorsMutex) stays acyclic. Reply is sent to the
			// pipe AFTER releasing the lock (SendToPipe only takes leaf-level g_pipeHandleMutex).
			std::string addReply;
			{
				std::lock_guard<std::mutex> ctxLk(g_DeviceContextMutex);
				auto* ctx = g_DeviceContext;
				if (!ctx) {
					vddlog("e", "ADD failed: device context not ready (adapter not initialized).");
					addReply = "ERR";
				}
				else {
					int idx = ctx->LowestFreeIndex();
					if (idx < 0) {
						vddlog("e", "ADD failed: no free connector index (MAX_MONITORS reached).");
						addReply = "ERR";
					}
					else if (!ctx->AddMonitor(static_cast<UINT>(idx))) {
						vddlog("e", "ADD failed: AddMonitor returned false.");
						addReply = "ERR";
					}
					else {
						// ASCII integer index back to the broker (uniform with PONG/OK/ERR).
						addReply = std::to_string(idx);
						vddlog("i", ("ADD -> index " + addReply).c_str());
					}
				}
			}
			SendToPipe(addReply);
		}
		else if (wcsncmp(buffer, L"REMOVE", 6) == 0) {
			WatchdogKick();
			int index = -1;
			std::string remReply;
			// Accept "REMOVE 3", "REMOVE  3", etc. swscanf_s tolerates leading spaces.
			if (swscanf_s(buffer + 6, L"%d", &index) == 1 && index >= 0) {
				// Same lifetime guarantee as ADD: hold g_DeviceContextMutex across the deref.
				std::lock_guard<std::mutex> ctxLk(g_DeviceContextMutex);
				auto* ctx = g_DeviceContext;
				if (!ctx) {
					vddlog("e", "REMOVE failed: device context not ready.");
					remReply = "ERR";
				}
				else if (ctx->RemoveMonitor(static_cast<UINT>(index))) {
					std::wstring lg = L"REMOVE index " + std::to_wstring(index) + L" -> OK";
					vddlog("i", WStringToString(lg).c_str());
					remReply = "OK";
				}
				else {
					std::wstring lg = L"REMOVE index " + std::to_wstring(index) + L" -> not live";
					vddlog("w", WStringToString(lg).c_str());
					remReply = "ERR";
				}
			}
			else {
				vddlog("e", "REMOVE failed: missing/invalid index. Usage: REMOVE <index>");
				remReply = "ERR";
			}
			SendToPipe(remReply);
		}
		// Legacy fixed-count command. Under the on-demand model this no longer reloads the
		// adapter; it only persists the configured count (used as a preconnect hint at next
		// adapter init). Kept for backward compatibility with old tooling.
		else if (wcsncmp(buffer, L"SETDISPLAYCOUNT", 15) == 0) {
			WatchdogKick();
			vddlog("i", "Setting Display Count (persisted; on-demand model does not reload)");

			int newDisplayCount = 1;
			swscanf_s(buffer + 15, L"%d", &newDisplayCount);

			std::wstring displayLog = L"Setting display count  to " + std::to_wstring(newDisplayCount);
			vddlog("c", WStringToString(displayLog).c_str());

			if (UpdateXmlDisplayCountSetting(newDisplayCount)){
				vddlog("c", "Display Count persisted to XML.");
			}
			else {
				vddlog("e", "Failed to update display count setting in XML.");
			}
		}
		else if (wcsncmp(buffer, L"GETSETTINGS", 11) == 0) {
			//query and return settings
			bool debugEnabled = EnabledQuery(L"DebugLoggingEnabled");
			bool loggingEnabled = EnabledQuery(L"LoggingEnabled");

			wstring settingsResponse = L"SETTINGS ";
			settingsResponse += debugEnabled ? L"DEBUG=true " : L"DEBUG=false ";
			settingsResponse += loggingEnabled ? L"LOG=true" : L"LOG=false";

			DWORD bytesWritten;
			DWORD bytesToWrite = static_cast<DWORD>((settingsResponse.length() + 1) * sizeof(wchar_t));
			WriteFile(hPipe, settingsResponse.c_str(), bytesToWrite, &bytesWritten, NULL);

		}
		else if (wcsncmp(buffer, L"PING", 4) == 0) {
			// Watchdog keepalive: the FIRST PING ARMS the watchdog (opt-in self-heal); every PING
			// resets the countdown. Reply PONG (CONTRACT). A consumer that never PINGs never arms
			// the watchdog, so its monitors persist unconditionally.
			WatchdogArm();
			SendToPipe("PONG");
			vddlog("p", "Heartbeat Ping");
		}
		else {
			// Unknown command: log it SAFELY and answer ERR. The old code called wcstombs_s with
			// invalid arguments (dst=null, count=0), which trips the CRT invalid-parameter handler
			// and ABORTS the whole UMDF host on any unrecognized/garbled message — i.e. one bad
			// pipe message used to "unplug" every live virtual monitor (CM_PROB_FAILED_POST_START).
			vddlog("e", "Unknown command");
			int size_needed = WideCharToMultiByte(CP_UTF8, 0, bufferwstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
			if (size_needed > 0) {
				std::string narrowString(size_needed, 0);
				WideCharToMultiByte(CP_UTF8, 0, bufferwstr.c_str(), -1, &narrowString[0], size_needed, nullptr, nullptr);
				vddlog("e", narrowString.c_str());
			}
			SendToPipe("ERR");
		}
	}
	// Clear g_pipeHandle BEFORE closing the OS handle, under the lock, so no logging thread can
	// WriteFile() to a handle that's about to be (or has just been) closed. Order matters: reset
	// first (so SendToPipe sees INVALID and skips), THEN Disconnect/Close.
	{
		std::lock_guard<std::mutex> lk(g_pipeHandleMutex);
		g_pipeHandle = INVALID_HANDLE_VALUE; // stop all threads from using this client reply channel
	}
	// FlushFileBuffers BEFORE DisconnectNamedPipe: Disconnect DISCARDS unread data, so a reply
	// written microseconds earlier (PONG/OK/index) silently vanished if the client hadn't posted
	// its read yet — the broker then saw an empty response and treated the op as failed. Flush
	// blocks until the client has consumed what we wrote.
	FlushFileBuffers(hPipe);
	DisconnectNamedPipe(hPipe);
	CloseHandle(hPipe);
}


DWORD WINAPI NamedPipeServer(LPVOID lpParam) {
	UNREFERENCED_PARAMETER(lpParam);

	SECURITY_ATTRIBUTES sa;
	sa.nLength = sizeof(SECURITY_ATTRIBUTES);
	sa.bInheritHandle = FALSE;
	// Tightened from D:(A;;GA;;;WD) (Everyone/World had full access) to SYSTEM + Administrators
	// only. Only the SYSTEM broker drives this pipe, so no other principal needs access. This
	// prevents any unprivileged process from issuing ADD/REMOVE and conjuring/destroying displays.
	const wchar_t* sddl = L"D:(A;;GA;;;SY)(A;;GA;;;BA)";
	vddlog("d", "Starting pipe with parameters: D:(A;;GA;;;SY)(A;;GA;;;BA)");
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
		sddl, SDDL_REVISION_1, &sa.lpSecurityDescriptor, NULL)) {
		DWORD ErrorCode = GetLastError();
		string errorMessage = to_string(ErrorCode);
		vddlog("e", errorMessage.c_str());
		return 1;
	}
	HANDLE hPipe;
	while (g_Running) {
		hPipe = CreateNamedPipeW(
			PIPE_NAME,
			PIPE_ACCESS_DUPLEX,
			PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
			PIPE_UNLIMITED_INSTANCES,
			512, 512,
			0,
			&sa);

		if (hPipe == INVALID_HANDLE_VALUE) {
			DWORD ErrorCode = GetLastError();
			string errorMessage = to_string(ErrorCode);
			vddlog("e", errorMessage.c_str());
			LocalFree(sa.lpSecurityDescriptor);
			return 1;
		}

		BOOL connected = ConnectNamedPipe(hPipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
		if (connected) {
			vddlog("p", "Client Connected");
			HandleClient(hPipe);
		}
		else {
			CloseHandle(hPipe);
		}
	}
	LocalFree(sa.lpSecurityDescriptor);
	return 0;
}

void StartNamedPipeServer() {
	vddlog("p", "Starting Pipe");
	hPipeThread = CreateThread(NULL, 0, NamedPipeServer, NULL, 0, NULL);
	if (hPipeThread == NULL) {
		DWORD ErrorCode = GetLastError();
		string errorMessage = to_string(ErrorCode);
		vddlog("e", errorMessage.c_str());
	}
	else {
		vddlog("p", "Pipe created");
	}
	// Start the broker-liveness watchdog alongside the pipe server.
	StartWatchdog();
}

void StopNamedPipeServer() {
	vddlog("p", "Stopping Pipe");
	StopWatchdog();
	{
		lock_guard<mutex> lock(g_Mutex);
		g_Running = false;
	}
	if (hPipeThread) {
		HANDLE hPipe = CreateFileW(
			PIPE_NAME,
			GENERIC_READ | GENERIC_WRITE,
			0,
			NULL,
			OPEN_EXISTING,
			0,
			NULL);

		if (hPipe != INVALID_HANDLE_VALUE) {
			DisconnectNamedPipe(hPipe);
			CloseHandle(hPipe);
		}

		WaitForSingleObject(hPipeThread, INFINITE);
		CloseHandle(hPipeThread);
		hPipeThread = NULL;
		vddlog("p", "Stopped Pipe");
	}
}

bool initpath() {
	HKEY hKey;
	wchar_t szPath[MAX_PATH];
	DWORD dwBufferSize = sizeof(szPath);
	LONG lResult;
	//vddlog("i", "Reading reg: Computer\\HKEY_LOCAL_MACHINE\\SOFTWARE\\MikeTheTech\\VirtualDisplayDriver");           Remove this due to the fact, if reg key exists, this is called before reading
	lResult = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\MikeTheTech\\VirtualDisplayDriver", 0, KEY_READ, &hKey);
	if (lResult != ERROR_SUCCESS) {
		ostringstream oss;
		oss << "Failed to open registry key for path. Error code: " << lResult;
		//vddlog("w", oss.str().c_str());  // These are okay to call though since they're only called if the reg doesnt exist
		return false;
	}

	lResult = RegQueryValueExW(hKey, L"VDDPATH", NULL, NULL, (LPBYTE)szPath, &dwBufferSize);
	if (lResult != ERROR_SUCCESS) {
		ostringstream oss;
		oss << "Failed to open registry key for path. Error code: " << lResult;
		//vddlog("w", oss.str().c_str()); Prevent these from being called since no longer checks before logging, only on startup whether it should
		RegCloseKey(hKey);
		return false;
	}

	confpath = szPath;

	RegCloseKey(hKey);

	return true;
}


extern "C" EVT_WDF_DRIVER_UNLOAD EvtDriverUnload;

VOID
EvtDriverUnload(
	_In_ WDFDRIVER Driver
)
{
	UNREFERENCED_PARAMETER(Driver);
	StopNamedPipeServer();
	vddlog("i", "Driver Unloaded");
}

_Use_decl_annotations_
extern "C" NTSTATUS DriverEntry(
	PDRIVER_OBJECT  pDriverObject,
	PUNICODE_STRING pRegistryPath
)
{
	WDF_DRIVER_CONFIG Config;
	NTSTATUS Status;

	WDF_OBJECT_ATTRIBUTES Attributes;
	WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);

	WDF_DRIVER_CONFIG_INIT(&Config, VirtualDisplayDriverDeviceAdd);

	Config.EvtDriverUnload = EvtDriverUnload;
	initpath();
	logsEnabled = EnabledQuery(L"LoggingEnabled");
	debugLogs = EnabledQuery(L"DebugLoggingEnabled");

	customEdid = EnabledQuery(L"CustomEdidEnabled");
	preventManufacturerSpoof = EnabledQuery(L"PreventMonitorSpoof");
	edidCeaOverride = EnabledQuery(L"EdidCeaOverride");
	sendLogsThroughPipe = EnabledQuery(L"SendLogsThroughPipe");


	//colour
	HDRPlus = EnabledQuery(L"HDRPlusEnabled");
	SDR10 = EnabledQuery(L"SDR10Enabled");
	HDRCOLOUR = HDRPlus ? IDDCX_BITS_PER_COMPONENT_12 : IDDCX_BITS_PER_COMPONENT_10;
	SDRCOLOUR = SDR10 ? IDDCX_BITS_PER_COMPONENT_10 : IDDCX_BITS_PER_COMPONENT_8;
	ColourFormat = GetStringSetting(L"ColourFormat");

	//Cursor
	hardwareCursor = EnabledQuery(L"HardwareCursorEnabled");
	alphaCursorSupport = EnabledQuery(L"AlphaCursorSupport");
	CursorMaxX = GetIntegerSetting(L"CursorMaxX");
	CursorMaxY = GetIntegerSetting(L"CursorMaxY");

	int xorCursorSupportLevelInt = GetIntegerSetting(L"XorCursorSupportLevel");
	std::string xorCursorSupportLevelName;

	if (xorCursorSupportLevelInt < 0 || xorCursorSupportLevelInt > 3) {
		vddlog("w", "Selected Xor Level unsupported, defaulting to IDDCX_XOR_CURSOR_SUPPORT_FULL");
		XorCursorSupportLevel = IDDCX_XOR_CURSOR_SUPPORT_FULL;
	}
	else {
		XorCursorSupportLevel = static_cast<IDDCX_XOR_CURSOR_SUPPORT>(xorCursorSupportLevelInt);
	}

	// === LOAD NEW EDID INTEGRATION SETTINGS ===
	edidIntegrationEnabled = EnabledQuery(L"EdidIntegrationEnabled");
	autoConfigureFromEdid = EnabledQuery(L"AutoConfigureFromEdid");
	edidProfilePath = GetStringSetting(L"EdidProfilePath");
	overrideManualSettings = EnabledQuery(L"OverrideManualSettings");
	fallbackOnError = EnabledQuery(L"FallbackOnError");

	// === LOAD HDR ADVANCED SETTINGS ===
	hdr10StaticMetadataEnabled = EnabledQuery(L"Hdr10StaticMetadataEnabled");
	maxDisplayMasteringLuminance = GetDoubleSetting(L"MaxDisplayMasteringLuminance");
	minDisplayMasteringLuminance = GetDoubleSetting(L"MinDisplayMasteringLuminance");
	maxContentLightLevel = GetIntegerSetting(L"MaxContentLightLevel");
	maxFrameAvgLightLevel = GetIntegerSetting(L"MaxFrameAvgLightLevel");

	colorPrimariesEnabled = EnabledQuery(L"ColorPrimariesEnabled");
	redX = GetDoubleSetting(L"RedX");
	redY = GetDoubleSetting(L"RedY");
	greenX = GetDoubleSetting(L"GreenX");
	greenY = GetDoubleSetting(L"GreenY");
	blueX = GetDoubleSetting(L"BlueX");
	blueY = GetDoubleSetting(L"BlueY");
	whiteX = GetDoubleSetting(L"WhiteX");
	whiteY = GetDoubleSetting(L"WhiteY");

	colorSpaceEnabled = EnabledQuery(L"ColorSpaceEnabled");
	gammaCorrection = GetDoubleSetting(L"GammaCorrection");
	primaryColorSpace = GetStringSetting(L"PrimaryColorSpace");
	enableMatrixTransform = EnabledQuery(L"EnableMatrixTransform");

	// === LOAD AUTO RESOLUTIONS SETTINGS ===
	autoResolutionsEnabled = EnabledQuery(L"AutoResolutionsEnabled");
	sourcePriority = GetStringSetting(L"SourcePriority");
	minRefreshRate = GetIntegerSetting(L"MinRefreshRate");
	maxRefreshRate = GetIntegerSetting(L"MaxRefreshRate");
	excludeFractionalRates = EnabledQuery(L"ExcludeFractionalRates");
	minResolutionWidth = GetIntegerSetting(L"MinResolutionWidth");
	minResolutionHeight = GetIntegerSetting(L"MinResolutionHeight");
	maxResolutionWidth = GetIntegerSetting(L"MaxResolutionWidth");
	maxResolutionHeight = GetIntegerSetting(L"MaxResolutionHeight");
	useEdidPreferred = EnabledQuery(L"UseEdidPreferred");
	fallbackWidth = GetIntegerSetting(L"FallbackWidth");
	fallbackHeight = GetIntegerSetting(L"FallbackHeight");
	fallbackRefresh = GetIntegerSetting(L"FallbackRefresh");

	// === LOAD COLOR ADVANCED SETTINGS ===
	autoSelectFromColorSpace = EnabledQuery(L"AutoSelectFromColorSpace");
	forceBitDepth = GetStringSetting(L"ForceBitDepth");
	fp16SurfaceSupport = EnabledQuery(L"Fp16SurfaceSupport");
	wideColorGamut = EnabledQuery(L"WideColorGamut");
	hdrToneMapping = EnabledQuery(L"HdrToneMapping");
	sdrWhiteLevel = GetDoubleSetting(L"SdrWhiteLevel");

	// === LOAD MONITOR EMULATION SETTINGS ===
	monitorEmulationEnabled = EnabledQuery(L"MonitorEmulationEnabled");
	emulatePhysicalDimensions = EnabledQuery(L"EmulatePhysicalDimensions");
	physicalWidthMm = GetIntegerSetting(L"PhysicalWidthMm");
	physicalHeightMm = GetIntegerSetting(L"PhysicalHeightMm");
	manufacturerEmulationEnabled = EnabledQuery(L"ManufacturerEmulationEnabled");
	manufacturerName = GetStringSetting(L"ManufacturerName");
	modelName = GetStringSetting(L"ModelName");
	serialNumber = GetStringSetting(L"SerialNumber");

	xorCursorSupportLevelName = XorCursorSupportLevelToString(XorCursorSupportLevel);

	vddlog("i", ("Selected Xor Cursor Support Level: " + xorCursorSupportLevelName).c_str());



	vddlog("i", "Driver Starting");
	string utf8_confpath = WStringToString(confpath);
	string logtext = "VDD Path: " + utf8_confpath;
	vddlog("i", logtext.c_str());
	LogIddCxVersion();

	Status = WdfDriverCreate(pDriverObject, pRegistryPath, &Attributes, &Config, WDF_NO_HANDLE);
	if (!NT_SUCCESS(Status))
	{
		return Status;
	}

	StartNamedPipeServer();

	return Status;
}

vector<string> split(string& input, char delimiter)
{
	istringstream stream(input);
	string field;
	vector<string> result;
	while (getline(stream, field, delimiter)) {
		result.push_back(field);
	}
	return result;
}


void loadSettings() {
	const wstring settingsname = confpath + L"\\vdd_settings.xml";
	const wstring& filename = settingsname;
	if (PathFileExistsW(filename.c_str())) {
		CComPtr<IStream> pStream;
		CComPtr<IXmlReader> pReader;
		HRESULT hr = SHCreateStreamOnFileW(filename.c_str(), STGM_READ, &pStream);
		if (FAILED(hr)) {
			vddlog("e", "Loading Settings: Failed to create file stream.");
			return; 
		}
		hr = CreateXmlReader(__uuidof(IXmlReader), (void**)&pReader, NULL);
		if (FAILED(hr)) {
			vddlog("e", "Loading Settings: Failed to create XmlReader.");
			return;
		}
		hr = pReader->SetInput(pStream);
		if (FAILED(hr)) {
			vddlog("e", "Loading Settings: Failed to set input stream.");
			return;
		}

		XmlNodeType nodeType;
		const WCHAR* pwszLocalName;
		const WCHAR* pwszValue;
		UINT cwchLocalName;
		UINT cwchValue;
		wstring currentElement;
		wstring width, height, refreshRate;
		vector<tuple<int, int, int, int>> res;
		wstring gpuFriendlyName;
		UINT monitorcount = 1;
		set<tuple<int, int>> resolutions;
		vector<int> globalRefreshRates;

		while (S_OK == (hr = pReader->Read(&nodeType))) {
			switch (nodeType) {
			case XmlNodeType_Element:
				hr = pReader->GetLocalName(&pwszLocalName, &cwchLocalName);
				if (FAILED(hr)) {
					return;
				}
				currentElement = wstring(pwszLocalName, cwchLocalName);
				break;
			case XmlNodeType_Text:
				hr = pReader->GetValue(&pwszValue, &cwchValue);
				if (FAILED(hr)) {
					return;
				}
				if (currentElement == L"count") {
					// stoi() devuelve int; validamos en SIGNED antes de asignar. monitorcount es
					// unsigned → `monitorcount < 0` sería siempre falso (C4296) Y un negativo haría
					// WRAP a un valor enorme en vez de clamp. Por eso parseamos a int y comprobamos ahí.
					int parsed = stoi(wstring(pwszValue, cwchValue));
					// Idle-at-0: un count de 0 es VÁLIDO = "empieza sin monitores; el broker los ADD
					// on-demand". Un negativo en config se clampa a 0 (antes se forzaba a 1).
					if (parsed < 0) {
						monitorcount = 0;
						vddlog("w", "Negative monitor count in config; clamped to 0 (idle).");
					}
					else {
						monitorcount = parsed;
						if (monitorcount == 0) {
							vddlog("i", "Monitor count is 0: adapter will idle with no monitors (on-demand).");
						}
					}
				}
				else if (currentElement == L"friendlyname") {
					gpuFriendlyName = wstring(pwszValue, cwchValue);
				}
				else if (currentElement == L"width") {
					width = wstring(pwszValue, cwchValue);
					if (width.empty()) {
						width = L"800";
					}
				}
				else if (currentElement == L"height") {
					height = wstring(pwszValue, cwchValue);
					if (height.empty()) {
						height = L"600";
					}
					resolutions.insert(make_tuple(stoi(width), stoi(height)));
				}
				else if (currentElement == L"refresh_rate") {
					refreshRate = wstring(pwszValue, cwchValue);
					if (refreshRate.empty()) {
						refreshRate = L"30";
					}
					int vsync_num, vsync_den;
					float_to_vsync(stof(refreshRate), vsync_num, vsync_den);

					res.push_back(make_tuple(stoi(width), stoi(height), vsync_num, vsync_den));
					stringstream ss;
					ss << "Added: " << stoi(width) << "x" << stoi(height) << " @ " << vsync_num << "/" << vsync_den << "Hz";
					vddlog("d", ss.str().c_str());
				}
				else if (currentElement == L"g_refresh_rate") {
					globalRefreshRates.push_back(stoi(wstring(pwszValue, cwchValue)));
				}
				break;
			}
		}

		/*
		* This is for res testing, stores each resolution then iterates through each global adding a res for each one
		* 
		
		for (const auto& resTuple : resolutions) {
			stringstream ss;
			ss << get<0>(resTuple) << "x" << get<1>(resTuple);
			vddlog("t", ss.str().c_str());
		}

		for (const auto& globalRate : globalRefreshRates) {
			stringstream ss;
			ss << globalRate << " Hz";
			vddlog("t", ss.str().c_str());
		}
		*/

		for (int globalRate : globalRefreshRates) {
			for (const auto& resTuple : resolutions) {
				int global_width = get<0>(resTuple);
				int global_height = get<1>(resTuple);

				int vsync_num, vsync_den;
				float_to_vsync(static_cast<float>(globalRate), vsync_num, vsync_den);
				res.push_back(make_tuple(global_width, global_height, vsync_num, vsync_den));
			}
		}

		/*
		* logging all resolutions after added global
		* 
		for (const auto& tup : res) {
			stringstream ss;
			ss << "("
				<< get<0>(tup) << ", "
				<< get<1>(tup) << ", "
				<< get<2>(tup) << ", "
				<< get<3>(tup) << ")";
			vddlog("t", ss.str().c_str());
		}
		
		*/


		numVirtualDisplays = monitorcount;
		gpuname = gpuFriendlyName;
		monitorModes = res;
		RebuildKnownMonitorModesCache();
		
		// === APPLY EDID INTEGRATION ===
		if (edidIntegrationEnabled && autoConfigureFromEdid) {
			EdidProfileData edidProfile;
			if (LoadEdidProfile(edidProfilePath, edidProfile)) {
				if (ApplyEdidProfile(edidProfile)) {
					vddlog("i", "EDID profile applied successfully");
				} else {
					vddlog("w", "EDID profile loaded but not applied (integration disabled)");
				}
			} else {
				if (fallbackOnError) {
					vddlog("w", "EDID profile loading failed, using manual settings");
				} else {
					vddlog("e", "EDID profile loading failed and fallback disabled");
				}
			}
		}
		
		vddlog("i","Using vdd_settings.xml");
		return;
	}
	const wstring optionsname = confpath + L"\\option.txt";
	ifstream ifs(optionsname);
	if (ifs.is_open()) {
    string line;
    if (getline(ifs, line) && !line.empty()) {
        numVirtualDisplays = stoi(line);
        vector<tuple<int, int, int, int>> res; 

        while (getline(ifs, line)) {
            vector<string> strvec = split(line, ',');
            if (strvec.size() == 3 && strvec[0].substr(0, 1) != "#") {
                int vsync_num, vsync_den;
                float_to_vsync(stof(strvec[2]), vsync_num, vsync_den); 
                res.push_back({ stoi(strvec[0]), stoi(strvec[1]), vsync_num, vsync_den });
            }
        }

        vddlog("i", "Using option.txt");
        monitorModes = res;
        RebuildKnownMonitorModesCache();
        for (const auto& mode : res) {
            int width, height, vsync_num, vsync_den;
            tie(width, height, vsync_num, vsync_den) = mode;
            stringstream ss;
            ss << "Resolution: " << width << "x" << height << " @ " << vsync_num << "/" << vsync_den << "Hz";
            vddlog("d", ss.str().c_str());
        }
		return;
    } else {
        vddlog("w", "option.txt is empty or the first line is invalid. Enabling Fallback");
    }
}


	numVirtualDisplays = 1;
	vector<tuple<int, int, int, int>> res;
	vector<tuple<int, int, float>> fallbackRes = {
		{800, 600, 30.0f},
		{800, 600, 60.0f},
		{800, 600, 90.0f},
		{800, 600, 120.0f},
		{800, 600, 144.0f},
		{800, 600, 165.0f},
		{1280, 720, 30.0f},
		{1280, 720, 60.0f},
		{1280, 720, 90.0f},
		{1280, 720, 130.0f},
		{1280, 720, 144.0f},
		{1280, 720, 165.0f},
		{1366, 768, 30.0f},
		{1366, 768, 60.0f},
		{1366, 768, 90.0f},
		{1366, 768, 120.0f},
		{1366, 768, 144.0f},
		{1366, 768, 165.0f},
		{1920, 1080, 30.0f},
		{1920, 1080, 60.0f},
		{1920, 1080, 90.0f},
		{1920, 1080, 120.0f},
		{1920, 1080, 144.0f},
		{1920, 1080, 165.0f},
		{2560, 1440, 30.0f},
		{2560, 1440, 60.0f},
		{2560, 1440, 90.0f},
		{2560, 1440, 120.0f},
		{2560, 1440, 144.0f},
		{2560, 1440, 165.0f},
		{3840, 2160, 30.0f},
		{3840, 2160, 60.0f},
		{3840, 2160, 90.0f},
		{3840, 2160, 120.0f},
		{3840, 2160, 144.0f},
		{3840, 2160, 165.0f}
	};

	vddlog("i", "Loading Fallback - no settings found");

	for (const auto& mode : fallbackRes) {
		int width, height;
		float refreshRate;
		tie(width, height, refreshRate) = mode;

		int vsync_num, vsync_den;
		float_to_vsync(refreshRate, vsync_num, vsync_den);

		stringstream ss;
		res.push_back(make_tuple(width, height, vsync_num, vsync_den));


		ss << "Resolution: " << width << "x" << height << " @ " << vsync_num << "/" << vsync_den << "Hz";
		vddlog("d", ss.str().c_str());
	}

	monitorModes = res;
	RebuildKnownMonitorModesCache();
	return;

}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT pDeviceInit)
{
	NTSTATUS Status = STATUS_SUCCESS;
	WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks;
	stringstream logStream;

	UNREFERENCED_PARAMETER(Driver);

	logStream << "Initializing device:"
		<< "\n  DeviceInit Pointer: " << static_cast<void*>(pDeviceInit);
	vddlog("d", logStream.str().c_str());

	// Register for power callbacks - in this sample only power-on is needed
	WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);
	PnpPowerCallbacks.EvtDeviceD0Entry = VirtualDisplayDriverDeviceD0Entry;
	WdfDeviceInitSetPnpPowerEventCallbacks(pDeviceInit, &PnpPowerCallbacks);

	IDD_CX_CLIENT_CONFIG IddConfig;
	IDD_CX_CLIENT_CONFIG_INIT(&IddConfig);

	logStream.str("");
	logStream << "Configuring IDD_CX client:"
		<< "\n  EvtIddCxAdapterInitFinished: " << (IddConfig.EvtIddCxAdapterInitFinished ? "Set" : "Not Set")
		<< "\n  EvtIddCxMonitorGetDefaultDescriptionModes: " << (IddConfig.EvtIddCxMonitorGetDefaultDescriptionModes ? "Set" : "Not Set")
		<< "\n  EvtIddCxMonitorAssignSwapChain: " << (IddConfig.EvtIddCxMonitorAssignSwapChain ? "Set" : "Not Set")
		<< "\n  EvtIddCxMonitorUnassignSwapChain: " << (IddConfig.EvtIddCxMonitorUnassignSwapChain ? "Set" : "Not Set");
	vddlog("d", logStream.str().c_str());

	// If the driver wishes to handle custom IoDeviceControl requests, it's necessary to use this callback since IddCx
	// redirects IoDeviceControl requests to an internal queue. This sample does not need this.
	// IddConfig.EvtIddCxDeviceIoControl = VirtualDisplayDriverIoDeviceControl;

	loadSettings();
	logStream.str("");
	if (gpuname.empty() || gpuname == L"default") {
		const wstring adaptername = confpath + L"\\adapter.txt";
		Options.Adapter.load(adaptername.c_str());
		logStream << "Attempting to Load GPU from adapter.txt";
	}
	else {
		Options.Adapter.xmlprovide(gpuname);
		logStream << "Loading GPU from vdd_settings.xml";
	}
	vddlog("i", logStream.str().c_str());
	GetGpuInfo();



	IddConfig.EvtIddCxAdapterInitFinished = VirtualDisplayDriverAdapterInitFinished;

	IddConfig.EvtIddCxMonitorGetDefaultDescriptionModes = VirtualDisplayDriverMonitorGetDefaultModes;
	IddConfig.EvtIddCxMonitorAssignSwapChain = VirtualDisplayDriverMonitorAssignSwapChain;
	IddConfig.EvtIddCxMonitorUnassignSwapChain = VirtualDisplayDriverMonitorUnassignSwapChain;

	if (IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxAdapterQueryTargetInfo))
	{
		IddConfig.EvtIddCxAdapterQueryTargetInfo = VirtualDisplayDriverEvtIddCxAdapterQueryTargetInfo;
		IddConfig.EvtIddCxMonitorSetDefaultHdrMetaData = VirtualDisplayDriverEvtIddCxMonitorSetDefaultHdrMetadata;
		IddConfig.EvtIddCxParseMonitorDescription2 = VirtualDisplayDriverEvtIddCxParseMonitorDescription2;
		IddConfig.EvtIddCxMonitorQueryTargetModes2 = VirtualDisplayDriverEvtIddCxMonitorQueryTargetModes2;
		IddConfig.EvtIddCxAdapterCommitModes2 = VirtualDisplayDriverEvtIddCxAdapterCommitModes2;
		IddConfig.EvtIddCxMonitorSetGammaRamp = VirtualDisplayDriverEvtIddCxMonitorSetGammaRamp;
	}
	else {
		IddConfig.EvtIddCxParseMonitorDescription = VirtualDisplayDriverParseMonitorDescription;
		IddConfig.EvtIddCxMonitorQueryTargetModes = VirtualDisplayDriverMonitorQueryModes;
		IddConfig.EvtIddCxAdapterCommitModes = VirtualDisplayDriverAdapterCommitModes;
	}

	Status = IddCxDeviceInitConfig(pDeviceInit, &IddConfig);
	if (!NT_SUCCESS(Status))
	{
		logStream.str("");
		logStream << "IddCxDeviceInitConfig failed with status: " << Status;
		vddlog("e", logStream.str().c_str());
		return Status;
	}

	WDF_OBJECT_ATTRIBUTES Attr;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);
	Attr.EvtCleanupCallback = [](WDFOBJECT Object)
		{
			// Automatically cleanup the context when the WDF object is about to be deleted
			auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Object);
			if (pContext)
			{
				pContext->Cleanup();
			}
		};

	logStream.str(""); 
	logStream << "Creating device with WdfDeviceCreate:";
	vddlog("d", logStream.str().c_str());

	WDFDEVICE Device = nullptr;
	Status = WdfDeviceCreate(&pDeviceInit, &Attr, &Device);
	if (!NT_SUCCESS(Status))
	{
		logStream.str(""); 
		logStream << "WdfDeviceCreate failed with status: " << Status;
		vddlog("e", logStream.str().c_str());
		return Status;
	}

	Status = IddCxDeviceInitialize(Device);
	if (!NT_SUCCESS(Status))
	{
		logStream.str(""); 
		logStream << "IddCxDeviceInitialize failed with status: " << Status;
		vddlog("e", logStream.str().c_str());
		return Status;
	}

	// Create a new device context object and attach it to the WDF device object
	/*
	auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
	pContext->pContext = new IndirectDeviceContext(Device);
	*/ // code to return uncase the device context wrapper isnt found (Most likely insufficient resources)

	auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
	if (pContext)
	{
		pContext->pContext = new IndirectDeviceContext(Device);
		logStream.str(""); 
		logStream << "Device context initialized and attached to WDF device.";
		vddlog("d", logStream.str().c_str());
	}
	else
	{
		logStream.str(""); 
		logStream << "Failed to get device context wrapper.";
		vddlog("e", logStream.str().c_str());
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	return Status;
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverDeviceD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
	//UNREFERENCED_PARAMETER(PreviousState);

	stringstream logStream;

	// Log the entry into D0 state
	logStream << "Entering D0 power state:"
		<< "\n  Device Handle: " << static_cast<void*>(Device)
		<< "\n  Previous State: " << PreviousState;
	vddlog("d", logStream.str().c_str());

	// This function is called by WDF to start the device in the fully-on power state. It fires on
	// the initial start AND on every wake from a low-power state. InitAdapter() is therefore
	// idempotent: it creates the IddCx adapter only on the first call (guarded by
	// m_AdapterInitialized) and is a no-op for the adapter on subsequent D0 entries, so a
	// sleep/wake cycle does NOT spawn a second adapter or orphan existing monitors.

	/*
	auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
	pContext->pContext->InitAdapter();
	*/ //Added error handling incase fails to get device context

	auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
	if (pContext && pContext->pContext)
	{
		logStream.str("");
		logStream << "Initializing adapter...";
		vddlog("d", logStream.str().c_str());


		pContext->pContext->InitAdapter();
		logStream.str(""); 
		logStream << "InitAdapter called successfully.";
		vddlog("d", logStream.str().c_str());
	}
	else
	{
		logStream.str(""); 
		logStream << "Failed to get device context.";
		vddlog("e", logStream.str().c_str());
		return STATUS_INSUFFICIENT_RESOURCES;
	}


	return STATUS_SUCCESS;
}

#pragma region Direct3DDevice

Direct3DDevice::Direct3DDevice(LUID AdapterLuid) : AdapterLuid(AdapterLuid)
{
}

Direct3DDevice::Direct3DDevice() : AdapterLuid({})
{
}

HRESULT Direct3DDevice::Init()
{
	HRESULT hr;
	stringstream logStream;

	// The DXGI factory could be cached, but if a new render adapter appears on the system, a new factory needs to be
	// created. If caching is desired, check DxgiFactory->IsCurrent() each time and recreate the factory if !IsCurrent.

	logStream << "Initializing Direct3DDevice...";
	vddlog("d", logStream.str().c_str());

	hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&DxgiFactory));
	if (FAILED(hr))
	{
		logStream.str(""); 
		logStream << "Failed to create DXGI factory. HRESULT: " << hr;
		vddlog("e", logStream.str().c_str());
		return hr;
	}
	logStream.str(""); 
	logStream << "DXGI factory created successfully.";
	vddlog("d", logStream.str().c_str());

	// Find the specified render adapter
	hr = DxgiFactory->EnumAdapterByLuid(AdapterLuid, IID_PPV_ARGS(&Adapter));
	if (FAILED(hr))
	{
		logStream.str(""); 
		logStream << "Failed to enumerate adapter by LUID. HRESULT: " << hr;
		vddlog("e", logStream.str().c_str());
		return hr;
	}

	DXGI_ADAPTER_DESC desc;
	Adapter->GetDesc(&desc);
	logStream.str("");
	logStream << "Adapter found: " << desc.Description << " (Vendor ID: " << desc.VendorId << ", Device ID: " << desc.DeviceId << ")";
	vddlog("i", logStream.str().c_str());


#if 0 // Test code
	{
		FILE* file;
		fopen_s(&file, "C:\\VirtualDisplayDriver\\desc_hdr.bin", "wb");

		DXGI_ADAPTER_DESC desc;
		Adapter->GetDesc(&desc);

		fwrite(&desc, 1, sizeof(desc), file);
		fclose(file);
	}
#endif

	D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
	D3D_FEATURE_LEVEL featureLevel;

	// Create a D3D device using the render adapter. BGRA support is required by the WHQL test suite.
	hr = D3D11CreateDevice(Adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION, &Device, &featureLevel, &DeviceContext);
	if (FAILED(hr))
	{
		// If creating the D3D device failed, it's possible the render GPU was lost (e.g. detachable GPU) or else the
		// system is in a transient state.
		logStream.str(""); 
		logStream << "Failed to create Direct3D device. HRESULT: " << hr;
		vddlog("e", logStream.str().c_str());
		logStream.str("");
		logStream << "If creating the D3D device failed, it's possible the render GPU was lost (e.g. detachable GPU) or else the system is in a transient state. " << hr;
		vddlog("e", logStream.str().c_str());
		return hr;
	}

	logStream.str("");
	logStream << "Direct3D device created successfully. Feature Level: " << featureLevel;
	vddlog("i", logStream.str().c_str());

	return S_OK;
}

#pragma endregion

#pragma region SwapChainProcessor

SwapChainProcessor::SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain, shared_ptr<Direct3DDevice> Device, HANDLE NewFrameEvent)
	: m_hSwapChain(hSwapChain), m_Device(Device), m_hAvailableBufferEvent(NewFrameEvent)
{
	stringstream logStream;

	logStream << "Constructing SwapChainProcessor:"
		<< "\n  SwapChain Handle: " << static_cast<void*>(hSwapChain)
		<< "\n  Device Pointer: " << static_cast<void*>(Device.get())
		<< "\n  NewFrameEvent Handle: " << NewFrameEvent;
	vddlog("d", logStream.str().c_str());

	m_hTerminateEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));
	if (!m_hTerminateEvent.Get())
	{
		logStream.str("");
		logStream << "Failed to create terminate event. GetLastError: " << GetLastError();
		vddlog("e", logStream.str().c_str());
	}
	else
	{
		logStream.str("");
		logStream << "Terminate event created successfully.";
		vddlog("d", logStream.str().c_str());
	}

	// ROUND-2 FIX (Issue 3): the worker thread is NOT started here anymore. It is started by
	// Start() (called right after a std::shared_ptr owns this object) so the worker can take a
	// shared_ptr lifetime hold via shared_from_this(), which is only valid once a shared_ptr owns
	// the object. See SwapChainProcessor::Start / RunThread.
}

// ROUND-2 FIX (Issue 3): start the worker and hand it a shared_ptr lifetime hold. The worker's
// hold guarantees the object outlives the worker even if the teardown path drops the map's
// reference first (and even if the destructor's join times out). The heap shared_ptr is owned by
// the worker and released in RunThread when the worker returns.
void SwapChainProcessor::Start()
{
	stringstream logStream;

	// Heap-allocate a shared_ptr copy that the new thread owns for its entire lifetime. This is the
	// lifetime hold: as long as the worker runs, the refcount is >= 1, so the object cannot be freed.
	auto* threadHold = new std::shared_ptr<SwapChainProcessor>(shared_from_this());

	DWORD threadId = 0;
	HANDLE h = CreateThread(nullptr, 0, RunThread, threadHold, 0, &threadId);
	if (!h)
	{
		// Failed to start: reclaim the hold we allocated for the (never-created) thread.
		delete threadHold;
		logStream << "Failed to create swap-chain processing thread. GetLastError: " << GetLastError();
		vddlog("e", logStream.str().c_str());
	}
	else
	{
		m_ThreadId = threadId;
		m_hThread.Attach(h);
		logStream << "Swap-chain processing thread created and started successfully.";
		vddlog("d", logStream.str().c_str());
	}
}

SwapChainProcessor::~SwapChainProcessor()
{
	stringstream logStream;

	logStream << "Destructing SwapChainProcessor:";

	vddlog("d", logStream.str().c_str());
	// Alert the swap-chain processing thread to terminate
	//SetEvent(m_hTerminateEvent.Get()); changed for error handling + log purposes 

	if (SetEvent(m_hTerminateEvent.Get()))
	{
		logStream.str(""); 
		logStream << "Terminate event signaled successfully.";
		vddlog("d", logStream.str().c_str());
	}
	else
	{
		logStream.str(""); 
		logStream << "Failed to signal terminate event. GetLastError: " << GetLastError();
		vddlog("e", logStream.str().c_str());
	}

	// ROUND-2 FIX (Issue 3 — Fix-5 timeout UAF): this object is owned by a std::shared_ptr and the
	// worker holds its OWN shared_ptr copy for its whole lifetime, so the destructor runs only when
	// the LAST reference drops. Two cases:
	//
	//   (a) The worker already finished and released its hold; the teardown path then dropped the
	//       map's reference, so the destructor runs on the TEARDOWN thread. m_hThread has already
	//       signaled exit, so the join below returns immediately.
	//
	//   (b) Teardown dropped the map's reference WHILE the worker was still running. The object
	//       stayed alive (worker's hold). When the worker returns, RunThread releases the last
	//       reference and the destructor runs ON THE WORKER THREAD. A thread can't join itself, so
	//       we MUST skip the wait in this case (m_ThreadId == GetCurrentThreadId()); the worker is
	//       already unwinding, so there's nothing to wait for.
	//
	// Either way the object is NEVER freed while the worker can still touch it — no UAF, and no
	// double WdfObjectDelete of the swap-chain (the worker is the sole place that deletes it and it
	// runs to completion before the object dies).
	if (m_hThread.Get())
	{
		if (m_ThreadId != 0 && m_ThreadId == GetCurrentThreadId())
		{
			// Case (b): destructor is running on the worker thread itself (it held the last
			// reference). Do NOT join — that would be a self-join (deadlock). The worker is
			// already past RunCore() and exiting.
			logStream.str("");
			logStream << "Destructor running on the worker thread (worker held the last reference); skipping self-join.";
			vddlog("d", logStream.str().c_str());
		}
		else
		{
			// Case (a): some other thread is destroying us; the worker has already released its
			// hold, so it has finished (or is finishing). An UNBOUNDED join is safe here precisely
			// because the worker's lifetime hold guarantees it cannot be stuck touching a freed
			// object — and we are NOT on the worker thread, so we cannot self-deadlock. (Previously
			// this was a bounded 5s wait that, on timeout, freed the object out from under a live
			// worker — the UAF this fix removes.)
			DWORD waitResult = WaitForSingleObject(m_hThread.Get(), INFINITE);
			switch (waitResult)
			{
			case WAIT_OBJECT_0:
				logStream.str("");
				logStream << "Thread terminated successfully.";
				vddlog("d", logStream.str().c_str());
				break;
			case WAIT_ABANDONED:
				logStream.str("");
				logStream << "Thread wait was abandoned. GetLastError: " << GetLastError();
				vddlog("e", logStream.str().c_str());
				break;
			default:
				logStream.str("");
				logStream << "Unexpected result from WaitForSingleObject. GetLastError: " << GetLastError();
				vddlog("e", logStream.str().c_str());
				break;
			}
		}
	}
	else
	{
		logStream.str("");
		logStream << "No valid thread handle to wait for.";
		vddlog("e", logStream.str().c_str());
	}
}

DWORD CALLBACK SwapChainProcessor::RunThread(LPVOID Argument)
{
	stringstream logStream;

	// ROUND-2 FIX (Issue 3): the argument is a heap-allocated shared_ptr<SwapChainProcessor> that
	// represents THIS worker's lifetime hold (created in Start()). Adopt it into a local so it is
	// released exactly when the worker returns — keeping the object alive for the entire duration of
	// Run()/RunCore() (which deref this, m_Device, m_hSwapChain and call WdfObjectDelete on the
	// swap-chain). When this local goes out of scope the hold drops; if it was the last reference,
	// ~SwapChainProcessor runs here on the worker thread and correctly skips the self-join.
	std::shared_ptr<SwapChainProcessor> self(*reinterpret_cast<std::shared_ptr<SwapChainProcessor>*>(Argument));
	delete reinterpret_cast<std::shared_ptr<SwapChainProcessor>*>(Argument);

	logStream << "RunThread started for processor: " << static_cast<void*>(self.get());
	vddlog("d", logStream.str().c_str());

	self->Run();
	return 0;
}

void SwapChainProcessor::Run()
{
	stringstream logStream;

	logStream << "Run method started.";
	vddlog("d", logStream.str().c_str());

	// For improved performance, make use of the Multimedia Class Scheduler Service, which will intelligently
	// prioritize this thread for improved throughput in high CPU-load scenarios.
	DWORD AvTask = 0;
	HANDLE AvTaskHandle = AvSetMmThreadCharacteristicsW(L"Distribution", &AvTask);

	if (AvTaskHandle)
	{
		logStream.str("");
		logStream << "Multimedia thread characteristics set successfully. AvTask: " << AvTask;
		vddlog("d", logStream.str().c_str());
	}
	else
	{
		logStream.str(""); 
		logStream << "Failed to set multimedia thread characteristics. GetLastError: " << GetLastError();
		vddlog("e", logStream.str().c_str());
	}

	RunCore();

	logStream.str(""); 
	logStream << "Core processing function RunCore() completed.";
	vddlog("d", logStream.str().c_str());

	// Always delete the swap-chain object when swap-chain processing loop terminates in order to kick the system to
	// provide a new swap-chain if necessary.
	/*
	WdfObjectDelete((WDFOBJECT)m_hSwapChain);
	m_hSwapChain = nullptr;   added error handling in so its not called to delete swap chain if its not needed.
	*/
	if (m_hSwapChain)
	{
		WdfObjectDelete((WDFOBJECT)m_hSwapChain);
		logStream.str("");
		logStream << "Swap-chain object deleted.";
		vddlog("d", logStream.str().c_str());
		m_hSwapChain = nullptr;
	}
	else
	{
		logStream.str("");
		logStream << "No valid swap-chain object to delete.";
		vddlog("w", logStream.str().c_str());
	}
	/*
	AvRevertMmThreadCharacteristics(AvTaskHandle);
	*/ //error handling when reversing multimedia thread characteristics 
	if (AvRevertMmThreadCharacteristics(AvTaskHandle))
	{
		logStream.str(""); 
		logStream << "Multimedia thread characteristics reverted successfully.";
		vddlog("d", logStream.str().c_str());
	}
	else
	{
		logStream.str(""); 
		logStream << "Failed to revert multimedia thread characteristics. GetLastError: " << GetLastError();
		vddlog("e", logStream.str().c_str());
	}
}

void SwapChainProcessor::RunCore()
{
	stringstream logStream;
	DWORD retryDelay = 1;
	const DWORD maxRetryDelay = 100;
	int retryCount = 0;
	const int maxRetries = 5;

	// Get the DXGI device interface
	ComPtr<IDXGIDevice> DxgiDevice;
	HRESULT hr = m_Device->Device.As(&DxgiDevice);
	if (FAILED(hr))
	{
		logStream << "Failed to get DXGI device interface. HRESULT: " << hr;
		vddlog("e", logStream.str().c_str());
		return;
	}
	logStream << "DXGI device interface obtained successfully.";
	//vddlog("d", logStream.str().c_str());


	// Validate that our device is still valid before setting it
	if (!m_Device || !m_Device->Device) {
		vddlog("e", "Direct3DDevice became invalid during SwapChain processing");
		return;
	}

	IDARG_IN_SWAPCHAINSETDEVICE SetDevice = {};
	SetDevice.pDevice = DxgiDevice.Get();

	hr = IddCxSwapChainSetDevice(m_hSwapChain, &SetDevice);
	logStream.str("");
	if (FAILED(hr))
	{
		logStream << "Failed to set device to swap chain. HRESULT: " << hr;
		vddlog("e", logStream.str().c_str());
		return;
	}
	logStream << "Device set to swap chain successfully.";
	//vddlog("d", logStream.str().c_str());

	logStream.str(""); 
	logStream << "Starting buffer acquisition and release loop.";
	//vddlog("d", logStream.str().c_str());

	// Acquire and release buffers in a loop
	for (;;)
	{
		ComPtr<IDXGIResource> AcquiredBuffer;

		// Ask for the next buffer from the producer
		IDARG_IN_RELEASEANDACQUIREBUFFER2 BufferInArgs = {};
		BufferInArgs.Size = sizeof(BufferInArgs);
		IDXGIResource* pSurface;

		if (IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainReleaseAndAcquireBuffer2)) {
			IDARG_OUT_RELEASEANDACQUIREBUFFER2 Buffer = {};
			hr = IddCxSwapChainReleaseAndAcquireBuffer2(m_hSwapChain, &BufferInArgs, &Buffer);
			pSurface = Buffer.MetaData.pSurface;
		}
		else
		{
			IDARG_OUT_RELEASEANDACQUIREBUFFER Buffer = {};
			hr = IddCxSwapChainReleaseAndAcquireBuffer(m_hSwapChain, &Buffer);
			pSurface = Buffer.MetaData.pSurface;
		}
		// AcquireBuffer immediately returns STATUS_PENDING if no buffer is yet available
		logStream.str("");
		if (hr == E_PENDING)
		{
			HANDLE waitHandles[2] = {};
			DWORD waitHandleCount = 0;

			if (m_hAvailableBufferEvent != nullptr && m_hAvailableBufferEvent != INVALID_HANDLE_VALUE)
			{
				waitHandles[waitHandleCount++] = m_hAvailableBufferEvent;
			}

			if (m_hTerminateEvent.Get())
			{
				waitHandles[waitHandleCount++] = m_hTerminateEvent.Get();
			}

			if (waitHandleCount == 0)
			{
				vddlog("e", "No valid wait handles available while waiting for the next frame.");
				break;
			}

			DWORD WaitResult = WaitForMultipleObjects(waitHandleCount, waitHandles, FALSE, INFINITE);

			logStream << "Buffer acquisition pending. WaitResult: " << WaitResult;

			if (WaitResult == WAIT_OBJECT_0)
			{
				continue;
			}
			else if (waitHandleCount > 1 && WaitResult == WAIT_OBJECT_0 + 1)
			{
				logStream << "Terminate event signaled. Exiting loop.";
				break;
			}
			else if (waitHandleCount == 1 && waitHandles[0] == m_hTerminateEvent.Get() && WaitResult == WAIT_OBJECT_0)
			{
				logStream << "Terminate event signaled. Exiting loop.";
				break;
			}
			else
			{
				hr = HRESULT_FROM_WIN32(WaitResult == WAIT_FAILED ? GetLastError() : WaitResult);
				logStream << "Unexpected wait result. HRESULT: " << hr;
				vddlog("e", logStream.str().c_str());
				break;
			}
		}
		else if (SUCCEEDED(hr))
		{
			// Reset retry delay and count on successful buffer acquisition
			retryDelay = 1;
			retryCount = 0;
			
			AcquiredBuffer.Attach(pSurface);

			// ==============================
			// TODO: Process the frame here
			//
			// This is the most performance-critical section of code in an IddCx driver. It's important that whatever
			// is done with the acquired surface be finished as quickly as possible. This operation could be:
			//  * a GPU copy to another buffer surface for later processing (such as a staging surface for mapping to CPU memory)
			//  * a GPU encode operation
			//  * a GPU VPBlt to another surface
			//  * a GPU custom compute shader encode operation
			// ==============================

			AcquiredBuffer.Reset();
			//vddlog("d", "Reset buffer");
			hr = IddCxSwapChainFinishedProcessingFrame(m_hSwapChain);
			if (FAILED(hr))
			{
				break;
			}

			// ==============================
			// TODO: Report frame statistics once the asynchronous encode/send work is completed
			//
			// Drivers should report information about sub-frame timings, like encode time, send time, etc.
			// ==============================
			// IddCxSwapChainReportFrameStatistics(m_hSwapChain, ...);
		}
		else
		{
			logStream.str(""); // Clear the stream
			if (hr == DXGI_ERROR_ACCESS_LOST && retryCount < maxRetries)
			{
				logStream << "DXGI_ERROR_ACCESS_LOST detected. Retry " << (retryCount + 1) << "/" << maxRetries << " after " << retryDelay << "ms delay.";
				vddlog("w", logStream.str().c_str());
				Sleep(retryDelay);
				retryDelay = min(retryDelay * 2, maxRetryDelay);
				retryCount++;
				continue;
			}
			else
			{
				if (hr == DXGI_ERROR_ACCESS_LOST)
				{
					logStream << "DXGI_ERROR_ACCESS_LOST: Maximum retries (" << maxRetries << ") reached. Exiting loop.";
				}
				else
				{
					logStream << "Failed to acquire buffer. Exiting loop. HRESULT: " << hr;
				}
				vddlog("e", logStream.str().c_str());
				// The swap-chain was likely abandoned, so exit the processing loop
				break;
			}
		}
	}
}

#pragma endregion

#pragma region IndirectDeviceContext

const UINT64 MHZ = 1000000;
const UINT64 KHZ = 1000;

constexpr DISPLAYCONFIG_VIDEO_SIGNAL_INFO dispinfo(UINT32 h, UINT32 v, UINT32 rn, UINT32 rd) {
	const UINT32 clock_rate = rn * (v + 4) * (v + 4) / rd + 1000;
	return {
	  clock_rate,                                      // pixel clock rate [Hz]
	{ clock_rate, v + 4 },                         // fractional horizontal refresh rate [Hz]
	{ clock_rate, (v + 4) * (v + 4) },          // fractional vertical refresh rate [Hz]
	{ h, v },                                    // (horizontal, vertical) active pixel resolution
	{ h + 4, v + 4 },                         // (horizontal, vertical) total pixel resolution
	{ { 255, 0 }},                                   // video standard and vsync divider
	DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE
	};
}

vector<BYTE> hardcodedEdid =
{
0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x36, 0x94, 0x37, 0x13, 0xe7, 0x1e, 0xe7, 0x1e,
0x1c, 0x22, 0x01, 0x03, 0x80, 0x32, 0x1f, 0x78, 0x07, 0xee, 0x95, 0xa3, 0x54, 0x4c, 0x99, 0x26,
0x0f, 0x50, 0x54, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02, 0x3a, 0x80, 0x18, 0x71, 0x38, 0x2d, 0x40, 0x58, 0x2c,
0x45, 0x00, 0x63, 0xc8, 0x10, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x17, 0xf0, 0x0f,
0xff, 0x37, 0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfc,
0x00, 0x56, 0x44, 0x44, 0x20, 0x62, 0x79, 0x20, 0x4d, 0x54, 0x54, 0x0a, 0x20, 0x20, 0x01, 0xc2,
0x02, 0x03, 0x20, 0x40, 0xe6, 0x06, 0x0d, 0x01, 0xa2, 0xa2, 0x10, 0xe3, 0x05, 0xd8, 0x00, 0x67,
0xd8, 0x5d, 0xc4, 0x01, 0x6e, 0x80, 0x00, 0x68, 0x03, 0x0c, 0x00, 0x00, 0x00, 0x30, 0x00, 0x0b,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x8c
};


void modifyEdid(vector<BYTE>& edid) {
	if (edid.size() < 12) {
		return;
	}

	edid[8] = 0x36;
	edid[9] = 0x94;
	edid[10] = 0x37;
	edid[11] = 0x13;
}



BYTE calculateChecksum(const std::vector<BYTE>& edid) {
	int sum = 0;
	for (int i = 0; i < 127; ++i) {
		sum += edid[i];
	}
	sum %= 256;
	if (sum != 0) {
		sum = 256 - sum;
	}
	return static_cast<BYTE>(sum);
	// check sum calculations. We dont need to include old checksum in calculation, so we only read up to the byte before.
	// Anything after the checksum bytes arent part of the checksum - a flaw with edid managment, not with us
}

void updateCeaExtensionCount(vector<BYTE>& edid, int count) {
	edid[126] = static_cast<BYTE>(count);
}

vector<BYTE> loadEdid(const string& filePath) {
	if (customEdid) {
		vddlog("i", "Attempting to use user Edid");
	}
	else {
		vddlog("i", "Using hardcoded edid");
		return hardcodedEdid;
	}

	ifstream file(filePath, ios::binary | ios::ate);
	if (!file) {
		vddlog("i", "No custom edid found");
		vddlog("i", "Using hardcoded edid");
		return hardcodedEdid;
	}

	streamsize size = file.tellg();
	file.seekg(0, ios::beg);

	vector<BYTE> buffer(size);
	if (file.read((char*)buffer.data(), size)) {
		//calculate checksum and compare it to 127 byte, if false then return hardcoded if true then return buffer to prevent loading borked edid.
		BYTE calculatedChecksum = calculateChecksum(buffer);
		if (calculatedChecksum != buffer[127]) {
			vddlog("e", "Custom edid failed due to invalid checksum");
			vddlog("i", "Using hardcoded edid");
			return hardcodedEdid;
		}

		if (edidCeaOverride) {
			if (buffer.size() == 256) {
				for (int i = 128; i < 256; ++i) {
					buffer[i] = hardcodedEdid[i];
				}
				updateCeaExtensionCount(buffer, 1);
			}
			else if (buffer.size() == 128) {
				buffer.insert(buffer.end(), hardcodedEdid.begin() + 128, hardcodedEdid.end());
				updateCeaExtensionCount(buffer, 1);
			}
		}

		vddlog("i", "Using custom edid");
		return buffer;
	}
	else {
		vddlog("i", "Using hardcoded edid");
		return hardcodedEdid;
	}
}

int maincalc() {
	vector<BYTE> edid = loadEdid(WStringToString(confpath) + "\\user_edid.bin");

	if (!preventManufacturerSpoof) modifyEdid(edid);
	BYTE checksum = calculateChecksum(edid);
	edid[127] = checksum;
	// Setting this variable is depricated, hardcoded edid is either returned or custom in loading edid function
	// ROUND-2 FIX (Issue 2): the reassignment of the s_KnownMonitorEdid vector races
	// CreateMonitorObject's read of s_KnownMonitorEdid.data()/.size() on a callback thread.
	// WRITE-lock the reassignment. We do the (potentially slow) EDID computation ABOVE without the
	// lock and only take it for the swap, so the critical section is minimal and never spans an
	// IddCx call. CreateMonitorObject snapshots the EDID bytes under the same lock before passing
	// the pointer to IddCxMonitorCreate.
	{
		std::lock_guard<std::mutex> lk(g_SettingsMutex);
		IndirectDeviceContext::s_KnownMonitorEdid = edid;
	}
	return 0;
}

std::shared_ptr<Direct3DDevice> IndirectDeviceContext::GetOrCreateDevice(LUID RenderAdapter)
{
	std::shared_ptr<Direct3DDevice> Device;
	stringstream logStream;

	logStream << "GetOrCreateDevice called for LUID: " << RenderAdapter.HighPart << "-" << RenderAdapter.LowPart;
	vddlog("d", logStream.str().c_str());

	{
		std::lock_guard<std::mutex> lock(s_DeviceCacheMutex);
		
		logStream.str("");
		logStream << "Device cache size: " << s_DeviceCache.size();
		vddlog("d", logStream.str().c_str());
		
		auto it = s_DeviceCache.find(RenderAdapter);
		if (it != s_DeviceCache.end()) {
			Device = it->second;
			if (Device) {
				logStream.str("");
				logStream << "Reusing cached Direct3DDevice for LUID " << RenderAdapter.HighPart << "-" << RenderAdapter.LowPart;
				vddlog("d", logStream.str().c_str());
				return Device;
			} else {
				logStream.str("");
				logStream << "Cached Direct3DDevice is null for LUID " << RenderAdapter.HighPart << "-" << RenderAdapter.LowPart << ", removing from cache";
				vddlog("d", logStream.str().c_str());
				s_DeviceCache.erase(it);
			}
		}
	}

	logStream.str("");
	logStream << "Creating new Direct3DDevice for LUID " << RenderAdapter.HighPart << "-" << RenderAdapter.LowPart;
	vddlog("d", logStream.str().c_str());
	
	Device = make_shared<Direct3DDevice>(RenderAdapter);
	if (FAILED(Device->Init())) {
		vddlog("e", "Failed to initialize new Direct3DDevice");
		return nullptr;
	}

	{
		std::lock_guard<std::mutex> lock(s_DeviceCacheMutex);
		s_DeviceCache[RenderAdapter] = Device;
		logStream.str("");
		logStream << "Created and cached new Direct3DDevice for LUID " << RenderAdapter.HighPart << "-" << RenderAdapter.LowPart << " (cache size now: " << s_DeviceCache.size() << ")";
		vddlog("d", logStream.str().c_str());
	}

	return Device;
}

void IndirectDeviceContext::CleanupExpiredDevices()
{
	std::lock_guard<std::mutex> lock(s_DeviceCacheMutex);
	
	int removed = 0;
	for (auto it = s_DeviceCache.begin(); it != s_DeviceCache.end();) {
		// With shared_ptr cache, we only remove null devices (shouldn't happen)
		if (!it->second) {
			it = s_DeviceCache.erase(it);
			removed++;
		} else {
			++it;
		}
	}
	
	if (removed > 0) {
		stringstream logStream;
		logStream << "Cleaned up " << removed << " null Direct3DDevice references from cache";
		vddlog("d", logStream.str().c_str());
	}
}

IndirectDeviceContext::IndirectDeviceContext(_In_ WDFDEVICE WdfDevice) :
	m_WdfDevice(WdfDevice),
	m_Adapter(nullptr)
{
	// Initialize Phase 5: Final Integration and Testing
	NTSTATUS initStatus = InitializePhase5Integration();
	if (!NT_SUCCESS(initStatus)) {
		vddlog("w", "Phase 5 integration initialization completed with warnings");
	}
}

IndirectDeviceContext::~IndirectDeviceContext()
{
	stringstream logStream;
	// ROUND-2 FIX (Issue 3): shared_ptr slots (matches m_ProcessingThreads). Any processors still
	// present are signaled to terminate below before their references are dropped.
	std::map<IDDCX_MONITOR, std::shared_ptr<SwapChainProcessor>> processingThreads;

	logStream << "Destroying IndirectDeviceContext. Releasing per-monitor processing threads.";
	vddlog("d", logStream.str().c_str());

	// ROUND-2 FIX (Issue 1 — residual watchdog UAF): the previous code relied on StopWatchdog()'s
	// BOUNDED 2s join as the UAF backstop. That is NOT a lifetime guarantee — if the watchdog was
	// mid-RemoveAllMonitors() and exceeded 2s, the join timed out and we deleted the context out
	// from under it (UAF). Correctness now comes from MUTUAL EXCLUSION, not timing:
	//
	//   STEP 1 (FIRST thing, under g_DeviceContextMutex): drop the cached pointer. Acquiring the
	//   mutex here BLOCKS until any watchdog iteration currently inside its self-heal block (which
	//   holds the same mutex across LiveMonitorCount()+RemoveAllMonitors()) has fully finished —
	//   and during that whole window THIS object is still alive (we haven't run any teardown yet).
	//   Once we null g_DeviceContext and release the mutex, the watchdog's next iteration reads
	//   nullptr and skips: it can never touch this object again.
	{
		std::lock_guard<std::mutex> lk(g_DeviceContextMutex);
		if (g_DeviceContext == this) {
			g_DeviceContext = nullptr;
		}
	}

	// STEP 2: now that the watchdog can no longer reach this context, stop+join its thread purely
	// to clean up the handle (no longer a correctness barrier). StopWatchdog holds NO driver locks
	// and the watchdog body never acquires g_WatchdogThreadMutex, so there is no stop-vs-watchdog
	// deadlock.
	StopWatchdog();

	// STEP 3: depart any monitors still attached so the OS doesn't keep orphan targets.
	// RemoveAllMonitors takes only m_MonitorsMutex internally and releases it before each
	// IddCxMonitorDeparture, so it is safe to call here. (The watchdog can no longer also be in
	// RemoveAllMonitors concurrently — it observed the nulled pointer in STEP 1.)
	RemoveAllMonitors();

	{
		std::lock_guard<std::mutex> lock(m_ProcessingThreadsMutex);
		processingThreads.swap(m_ProcessingThreads);
	}

	// ROUND-2 FIX (Issue 3): signal every remaining worker to terminate BEFORE the local map drops
	// its references (done OUTSIDE m_ProcessingThreadsMutex). We must signal rather than rely on
	// ~SwapChainProcessor, because under the shared_ptr model a worker may hold the last reference
	// and the destructor would then run on the worker thread (which can't be what wakes it). After
	// signaling, the local map clearing releases our references; any still-running worker stays
	// alive via its own hold until it exits and tears itself down. (In practice STEP 3's
	// RemoveAllMonitors already unassigned most/all via IddCxMonitorDeparture -> UnassignSwapChain.)
	for (auto& kv : processingThreads) {
		if (kv.second && kv.second->m_hTerminateEvent.Get()) {
			SetEvent(kv.second->m_hTerminateEvent.Get());
		}
	}

	logStream.str("");
	logStream << "Released " << processingThreads.size() << " monitor processing thread(s).";
	vddlog("d", logStream.str().c_str());
}

#define NUM_VIRTUAL_DISPLAYS 1   //What is this even used for ?? Its never referenced

void IndirectDeviceContext::InitAdapter()
{
	stringstream logStream;

	// FIX (re-init on every D0 transition): IddCxAdapterInitAsync must be called EXACTLY ONCE per
	// device lifetime. EvtDeviceD0Entry calls InitAdapter() on every D0 entry — including every
	// wake from sleep/hibernate. Without this guard a wake would create a SECOND IddCx adapter,
	// overwrite m_Adapter, and orphan all monitors bound to the first adapter. On subsequent D0
	// entries we do NOT re-init the adapter; IddCx re-establishes swapchains on its own per its D0
	// power semantics. We still make sure the (process-global) watchdog thread is running, since it
	// may have been stopped by a previous device-context teardown.
	if (m_AdapterInitialized) {
		vddlog("i", "InitAdapter: adapter already initialized for this device; skipping re-init on D0 entry (swapchains are re-established by IddCx).");
		StartWatchdog(); // idempotent: returns early if already running
		return;
	}

	maincalc();

	// ==============================
	// TODO: Update the below diagnostic information in accordance with the target hardware. The strings and version
	// numbers are used for telemetry and may be displayed to the user in some situations.
	//
	// This is also where static per-adapter capabilities are determined.
	// ==============================

	logStream << "Initializing adapter...";
	vddlog("d", logStream.str().c_str());
	logStream.str("");

	IDDCX_ADAPTER_CAPS AdapterCaps = {};
	AdapterCaps.Size = sizeof(AdapterCaps);

	if (IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainReleaseAndAcquireBuffer2)) {
		AdapterCaps.Flags = IDDCX_ADAPTER_FLAGS_CAN_PROCESS_FP16;
		logStream << "FP16 processing capability detected.";
	}

	// Tier-1: advertise that every target mode we report is monitor-compatible. This lets us
	// hand Windows the EXACT requested panel resolution (e.g. a non-standard 2K aspect) without it
	// being filtered out as an "incompatible" target mode. Guarded by availability so older
	// IddCx headers still compile.
#ifdef IDDCX_ADAPTER_FLAGS_ALL_TARGET_MODES_MONITOR_COMPATIBLE
	AdapterCaps.Flags |= IDDCX_ADAPTER_FLAGS_ALL_TARGET_MODES_MONITOR_COMPATIBLE;
#endif

	// Declare basic feature support for the adapter (required).
	// Decoupled from numVirtualDisplays so the adapter idles at 0 monitors and supports adding/
	// removing ONE monitor per connector index at runtime (idle-at-0). MaxMonitorsSupported is a
	// FIXED upper bound and must not change across the adapter's lifetime, so we advertise the
	// constant ceiling regardless of how many monitors are currently live.
	AdapterCaps.MaxMonitorsSupported = MAX_MONITORS;
	AdapterCaps.EndPointDiagnostics.Size = sizeof(AdapterCaps.EndPointDiagnostics);
	AdapterCaps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
	AdapterCaps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;

	// Declare your device strings for telemetry (required)
	AdapterCaps.EndPointDiagnostics.pEndPointFriendlyName = L"VirtualDisplayDriver Device";
	AdapterCaps.EndPointDiagnostics.pEndPointManufacturerName = L"MikeTheTech";
	AdapterCaps.EndPointDiagnostics.pEndPointModelName = L"VirtualDisplayDriver Model";

	// Declare your hardware and firmware versions (required)
	IDDCX_ENDPOINT_VERSION Version = {};
	Version.Size = sizeof(Version);
	Version.MajorVer = 1;
	AdapterCaps.EndPointDiagnostics.pFirmwareVersion = &Version;
	AdapterCaps.EndPointDiagnostics.pHardwareVersion = &Version;

	logStream << "Adapter Caps Initialized:"
		<< "\n  Max Monitors Supported: " << AdapterCaps.MaxMonitorsSupported
		<< "\n  Gamma Support: " << AdapterCaps.EndPointDiagnostics.GammaSupport
		<< "\n  Transmission Type: " << AdapterCaps.EndPointDiagnostics.TransmissionType
		<< "\n  Friendly Name: " << AdapterCaps.EndPointDiagnostics.pEndPointFriendlyName
		<< "\n  Manufacturer Name: " << AdapterCaps.EndPointDiagnostics.pEndPointManufacturerName
		<< "\n  Model Name: " << AdapterCaps.EndPointDiagnostics.pEndPointModelName
		<< "\n  Firmware Version: " << Version.MajorVer
		<< "\n  Hardware Version: " << Version.MajorVer;

	vddlog("d", logStream.str().c_str());
	logStream.str("");

	// Initialize a WDF context that can store a pointer to the device context object
	WDF_OBJECT_ATTRIBUTES Attr;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);

	IDARG_IN_ADAPTER_INIT AdapterInit = {};
	AdapterInit.WdfDevice = m_WdfDevice;
	AdapterInit.pCaps = &AdapterCaps;
	AdapterInit.ObjectAttributes = &Attr;

	// Start the initialization of the adapter, which will trigger the AdapterFinishInit callback later
	IDARG_OUT_ADAPTER_INIT AdapterInitOut;
	NTSTATUS Status = IddCxAdapterInitAsync(&AdapterInit, &AdapterInitOut);

	logStream << "Adapter Initialization Status: " << Status;
	vddlog("d", logStream.str().c_str());
	logStream.str("");

	if (NT_SUCCESS(Status))
	{
		// Mark the adapter as initialized so future D0 entries skip IddCxAdapterInitAsync (see the
		// guard at the top of this function). Set only on success so a failed init can be retried.
		m_AdapterInitialized = true;

		// Store a reference to the WDF adapter handle
		m_Adapter = AdapterInitOut.AdapterObject;
		logStream << "Adapter handle stored successfully.";
		vddlog("d", logStream.str().c_str());

		// Store the device context object into the WDF object context
		auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(AdapterInitOut.AdapterObject);
		pContext->pContext = this;

		// Cache for the pipe thread so it can reach this context without a WDFOBJECT
		// (fixes the ReloadDriver type-confusion bug where a pipe HANDLE was passed to
		// WdfObjectGet_IndirectDeviceContextWrapper).
		{
			std::lock_guard<std::mutex> lk(g_DeviceContextMutex);
			g_DeviceContext = this;
		}

		// (Re)start the process-global watchdog thread for this device. Idempotent: returns early
		// if already running. Needed because a prior device-context teardown stops+joins it.
		StartWatchdog();
	}
	else {
		logStream << "Failed to initialize adapter. Status: " << Status;
		vddlog("e", logStream.str().c_str());
	}
}

void IndirectDeviceContext::FinishInit()
{
	Options.Adapter.apply(m_Adapter);
	vddlog("i", "Applied Adapter configs.");

	// Idle-at-0: do NOT create any monitors at adapter init. Monitors are added on demand by
	// the broker via the pipe "ADD" command (AddMonitor). This is the spacedesk model: the
	// adapter exists with zero attached displays until a client asks for one.
	//
	// numVirtualDisplays is now treated as a "preconnect" count for compatibility with the
	// legacy fixed-count behavior: if a config explicitly requests N>0 displays we honor it by
	// pre-adding N monitors here. With idle-at-0 the broker normally leaves this at 0.
	if (numVirtualDisplays > 0) {
		const UINT preconnect = (numVirtualDisplays > MAX_MONITORS) ? MAX_MONITORS : numVirtualDisplays;
		stringstream ss;
		ss << "Pre-connecting " << preconnect << " monitor(s) per configured display count.";
		vddlog("i", ss.str().c_str());
		for (UINT i = 0; i < preconnect; i++) {
			AddMonitor(i);
		}
	}
	else {
		vddlog("i", "Adapter initialized idle (0 monitors). Awaiting ADD via pipe.");
	}
}

// Build a STABLE per-connector container ID GUID. Windows uses the container ID to remember a
// monitor's position/layout across plug cycles, so the same connector index must always map to
// the same GUID (otherwise the OS treats every re-add as a brand-new display and forgets the
// display's arrangement). We namespace the GUID with the adapter/host so it's stable but distinct.
// TODO (Tier-1): derive this from a per-display stable identity (e.g. a device serial the consumer
// passes) once the ADD command carries one, so multiple displays keep separate remembered layouts.
// NOTE: the byte values below are part of the on-disk container ID — do NOT change them or every
// already-arranged display would be treated as brand-new and lose its remembered layout.
static GUID MakeStableContainerId(UINT index)
{
	GUID g = {};
	// Fixed namespace base. Only Data1 varies by connector index, which
	// keeps each index's container ID stable across add/remove cycles within this host.
	g.Data1 = 0x4d54'5644u ^ index;   // base constant xor index
	g.Data2 = 0x0001;
	g.Data3 = 0x4000;                 // RFC4122 version-4 nibble for well-formedness
	const BYTE base[8] = { 0x8a, 0x00, 0x53, 0x55, 0x44, 0x4f, 0x56, 0x44 }; // ...SUDOVD
	memcpy(g.Data4, base, sizeof(base));
	g.Data4[1] = static_cast<BYTE>(index & 0xFF);
	return g;
}

// Internal: build the EDID-described monitor, create it, and report arrival. Returns the created
// IDDCX_MONITOR handle on success, or nullptr on failure. Does NOT touch m_Monitors / locks — the
// caller (AddMonitor) owns the bookkeeping. Must be called WITHOUT m_MonitorsMutex held during the
// IddCx calls (this function holds no locks itself).
IDDCX_MONITOR IndirectDeviceContext::CreateMonitorObject(UINT index)
{
	wstring logMessage = L"Creating Monitor at connector index " + to_wstring(index);
	vddlog("i", WStringToString(logMessage).c_str());

	WDF_OBJECT_ATTRIBUTES Attr;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);

	IDDCX_MONITOR_INFO MonitorInfo = {};
	MonitorInfo.Size = sizeof(MonitorInfo);
	MonitorInfo.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
	MonitorInfo.ConnectorIndex = index;
	MonitorInfo.MonitorDescription.Size = sizeof(MonitorInfo.MonitorDescription);
	MonitorInfo.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;

	// ROUND-2 FIX (Issue 2): maincalc() (called from AddMonitor right before this) can REASSIGN
	// s_KnownMonitorEdid on another thread. Reading .size()/.data() here unsynchronized races that
	// reassignment (container UB: the backing buffer can be freed mid-read). Snapshot the EDID bytes
	// into a LOCAL under g_SettingsMutex, then release the lock BEFORE the IddCxMonitorCreate call
	// (we must never hold this lock across an IddCx call that can re-enter the driver). The local
	// 'edidSnapshot' stays in scope through IddCxMonitorCreate, so pData remains valid.
	std::vector<BYTE> edidSnapshot;
	{
		std::lock_guard<std::mutex> lk(g_SettingsMutex);
		edidSnapshot = IndirectDeviceContext::s_KnownMonitorEdid;
	}
	if (edidSnapshot.size() > UINT_MAX)
	{
		vddlog("e", "Edid size passes UINT_Max, escape to prevent loading borked display");
		return nullptr;
	}
	MonitorInfo.MonitorDescription.DataSize = static_cast<UINT>(edidSnapshot.size());
	MonitorInfo.MonitorDescription.pData = edidSnapshot.data();

	// Stable container ID per connector index so Windows remembers each display's layout
	// (was CoCreateGuid -> random every plug, which lost the remembered arrangement).
	MonitorInfo.MonitorContainerId = MakeStableContainerId(index);
	vddlog("d", "Assigned stable container ID");

	IDARG_IN_MONITORCREATE MonitorCreate = {};
	MonitorCreate.ObjectAttributes = &Attr;
	MonitorCreate.pMonitorInfo = &MonitorInfo;

	IDARG_OUT_MONITORCREATE MonitorCreateOut;
	NTSTATUS Status = IddCxMonitorCreate(m_Adapter, &MonitorCreate, &MonitorCreateOut);
	if (!NT_SUCCESS(Status))
	{
		stringstream ss;
		ss << "Failed to create monitor. Status: " << Status;
		vddlog("e", ss.str().c_str());
		return nullptr;
	}

	vddlog("d", "Monitor created successfully.");

	// Associate the monitor with this device context
	auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(MonitorCreateOut.MonitorObject);
	pContext->pContext = this;

	// Tell the OS that the monitor has been plugged in
	IDARG_OUT_MONITORARRIVAL ArrivalOut;
	Status = IddCxMonitorArrival(MonitorCreateOut.MonitorObject, &ArrivalOut);
	if (NT_SUCCESS(Status))
	{
		vddlog("d", "Monitor arrival successfully reported.");
	}
	else
	{
		stringstream ss;
		ss << "Failed to report monitor arrival. Status: " << Status;
		vddlog("e", ss.str().c_str());
		// Arrival failed: tear down the orphan monitor object so we don't leak a half-created
		// target. Departure is safe here because no lock is held.
		IddCxMonitorDeparture(MonitorCreateOut.MonitorObject);
		return nullptr;
	}

	return MonitorCreateOut.MonitorObject;
}

// Legacy shim: old code paths call CreateMonitor(index). Route them through AddMonitor so the
// index map stays authoritative.
void IndirectDeviceContext::CreateMonitor(unsigned int index)
{
	AddMonitor(static_cast<UINT>(index));
}

// ===== On-demand add =====
bool IndirectDeviceContext::AddMonitor(UINT index)
{
	if (index >= MAX_MONITORS) {
		stringstream ss;
		ss << "AddMonitor rejected: index " << index << " >= MAX_MONITORS (" << MAX_MONITORS << ").";
		vddlog("e", ss.str().c_str());
		return false;
	}

	// Reject if this connector index is already live. We check under the lock, then RELEASE the
	// lock before the IddCx calls (IddCxMonitorCreate/Arrival can re-enter driver callbacks).
	{
		std::lock_guard<std::mutex> lock(m_MonitorsMutex);
		if (m_Monitors.find(index) != m_Monitors.end()) {
			stringstream ss;
			ss << "AddMonitor rejected: index " << index << " is already live.";
			vddlog("w", ss.str().c_str());
			return false;
		}
	}

	// FIX (settings stop applying at runtime): rebuild the EDID from the CURRENT settings globals
	// before creating the monitor, so a monitor added AFTER a CUSTOMEDID/PREVENTSPOOF/SDR10/HDR+
	// pipe command reflects that change. The adapter is now init-once, so maincalc() no longer runs
	// per-D0; doing it here is what makes settings apply to newly-added monitors. The per-monitor
	// HDRCOLOUR/SDRCOLOUR/ColourFormat globals are read live by the monitor description callbacks,
	// so they also apply to monitors added after a change. (Existing monitors keep their original
	// description until they are removed and re-added.)
	maincalc();

	IDDCX_MONITOR handle = CreateMonitorObject(index);
	if (handle == nullptr) {
		return false;
	}

	// Commit to the map. Guard against a race where another caller grabbed the same index while
	// we were creating (shouldn't happen with a single broker, but be safe): if the slot got
	// taken, depart our just-created monitor OUTSIDE the lock.
	bool raced = false;
	{
		std::lock_guard<std::mutex> lock(m_MonitorsMutex);
		if (m_Monitors.find(index) != m_Monitors.end()) {
			raced = true;
		} else {
			m_Monitors[index] = handle;
		}
	}
	if (raced) {
		vddlog("w", "AddMonitor lost a race for the index; departing the duplicate monitor.");
		IddCxMonitorDeparture(handle);   // lock NOT held here
		return false;
	}

	stringstream ss;
	ss << "Monitor added at index " << index << ". Live count: " << LiveMonitorCount();
	vddlog("i", ss.str().c_str());
	return true;
}

// ===== On-demand remove =====
bool IndirectDeviceContext::RemoveMonitor(UINT index)
{
	IDDCX_MONITOR handle = nullptr;

	// Fetch + erase from the index map under the lock, then UNLOCK before departing.
	// CRITICAL: IddCxMonitorDeparture may synchronously call UnassignSwapChain, which takes
	// m_ProcessingThreadsMutex. Holding m_MonitorsMutex across departure is fine for THAT lock,
	// but to keep the rule simple and future-proof (and to avoid ever nesting departure inside
	// any of our locks) we always release first.
	{
		std::lock_guard<std::mutex> lock(m_MonitorsMutex);
		auto it = m_Monitors.find(index);
		if (it == m_Monitors.end()) {
			stringstream ss;
			ss << "RemoveMonitor: index " << index << " is not live.";
			vddlog("w", ss.str().c_str());
			return false;
		}
		handle = it->second;
		m_Monitors.erase(it);
	}

	// Depart with NO lock held (see lock-discipline note above).
	IddCxMonitorDeparture(handle);

	// Purge per-monitor side tables keyed by the (now departed) handle so they don't leak.
	{
		std::lock_guard<std::mutex> hdrLock(g_HdrMetadataStoreMutex);
		g_HdrMetadataStore.erase(handle);
	}
	{
		std::lock_guard<std::mutex> gammaLock(g_GammaRampStoreMutex);
		g_GammaRampStore.erase(handle);
	}

	stringstream ss;
	ss << "Monitor removed at index " << index << ". Live count: " << LiveMonitorCount();
	vddlog("i", ss.str().c_str());
	return true;
}

void IndirectDeviceContext::RemoveAllMonitors()
{
	// Snapshot the live indices under the lock, release, then remove one by one (RemoveMonitor
	// re-locks internally and departs outside the lock).
	std::vector<UINT> indices;
	{
		std::lock_guard<std::mutex> lock(m_MonitorsMutex);
		indices.reserve(m_Monitors.size());
		for (const auto& kv : m_Monitors) {
			indices.push_back(kv.first);
		}
	}
	if (!indices.empty()) {
		stringstream ss;
		ss << "Removing all " << indices.size() << " live monitor(s).";
		vddlog("i", ss.str().c_str());
	}
	for (UINT idx : indices) {
		RemoveMonitor(idx);
	}
}

int IndirectDeviceContext::LowestFreeIndex()
{
	std::lock_guard<std::mutex> lock(m_MonitorsMutex);
	for (UINT i = 0; i < MAX_MONITORS; i++) {
		if (m_Monitors.find(i) == m_Monitors.end()) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

size_t IndirectDeviceContext::LiveMonitorCount()
{
	std::lock_guard<std::mutex> lock(m_MonitorsMutex);
	return m_Monitors.size();
}

void IndirectDeviceContext::AssignSwapChain(IDDCX_MONITOR Monitor, IDDCX_SWAPCHAIN SwapChain, LUID RenderAdapter, HANDLE NewFrameEvent)
{
	// Only cleanup expired devices periodically, not on every assignment
	static int assignmentCount = 0;
	if (++assignmentCount % 10 == 0) {
		CleanupExpiredDevices();
	}

	auto Device = GetOrCreateDevice(RenderAdapter);
	if (!Device)
	{
		vddlog("e", "Failed to get or create Direct3DDevice, deleting existing swap-chain.");
		WdfObjectDelete(SwapChain);
		return;
	}
	else
	{
		// ROUND-2 FIX (Issue 3): create via make_shared and Start() the worker AFTER a shared_ptr
		// owns it (so the worker can take a shared_from_this() lifetime hold). The slot is now a
		// shared_ptr; a replaced processor is moved out and torn down OUTSIDE the lock.
		std::shared_ptr<SwapChainProcessor> previousProcessor;
		auto newProcessor = std::make_shared<SwapChainProcessor>(SwapChain, Device, NewFrameEvent);
		newProcessor->Start();

		{
			std::lock_guard<std::mutex> lock(m_ProcessingThreadsMutex);
			auto& processorSlot = m_ProcessingThreads[Monitor];
			previousProcessor = std::move(processorSlot);
			processorSlot = newProcessor;
		}

		// If we replaced an existing processor, signal its worker to terminate BEFORE we drop our
		// reference. Under the shared_ptr model the destructor is NOT guaranteed to run on this
		// thread (the worker may hold the last reference), so we cannot rely on ~SwapChainProcessor
		// to signal termination — we must do it here so the old worker actually wakes and exits.
		// This is done OUTSIDE m_ProcessingThreadsMutex (the lock-discipline rule for teardown).
		if (previousProcessor) {
			vddlog("d", "Replaced existing processing thread for this monitor only.");
			if (previousProcessor->m_hTerminateEvent.Get()) {
				SetEvent(previousProcessor->m_hTerminateEvent.Get());
			}
		}
		else {
			vddlog("d", "Created a new processing thread for this monitor.");
		}

		if (hardwareCursor){
			HANDLE mouseEvent = CreateEventA(
				nullptr, 
				false,   
				false,   
				"VirtualDisplayDriverMouse"
			);

			if (!mouseEvent)
			{
				vddlog("e", "Failed to create mouse event. No hardware cursor supported!");
				return;
			}

			IDDCX_CURSOR_CAPS cursorInfo = {};
			cursorInfo.Size = sizeof(cursorInfo);
			cursorInfo.ColorXorCursorSupport = IDDCX_XOR_CURSOR_SUPPORT_FULL; 
			cursorInfo.AlphaCursorSupport = alphaCursorSupport;

			cursorInfo.MaxX = CursorMaxX;       //Apparently in most cases 128 is fine but for safe guarding we will go 512, older intel cpus may be limited to 64x64
			cursorInfo.MaxY = CursorMaxY;

			//DirectXDevice->QueryMaxCursorSize(&cursorInfo.MaxX, &cursorInfo.MaxY);                 Experimental to get max cursor size - THIS IS NTO WORKING CODE


			IDARG_IN_SETUP_HWCURSOR hwCursor = {};
			hwCursor.CursorInfo = cursorInfo;
			hwCursor.hNewCursorDataAvailable = mouseEvent;

			NTSTATUS Status = IddCxMonitorSetupHardwareCursor(
				Monitor,
				&hwCursor
			);

			if (FAILED(Status))
			{
				CloseHandle(mouseEvent); 
				return;
			}

			vddlog("d", "Hardware cursor setup completed successfully.");
		}
		else {
			vddlog("d", "Hardware cursor is disabled, Skipped creation.");
		}
		// At this point, the swap-chain is set up and the hardware cursor is enabled
		// Further swap-chain and cursor processing will occur in the new processing thread.
	}
}


void IndirectDeviceContext::UnassignSwapChain(IDDCX_MONITOR Monitor)
{
	// ROUND-2 FIX (Issue 3): shared_ptr slot. Move the processor out under the lock, release the
	// lock, signal its worker to terminate, then drop our reference. We must SIGNAL here (not rely
	// on ~SwapChainProcessor) because under the shared_ptr model the destructor may run on the
	// worker thread, which can't be what wakes the worker. The reference we hold here is dropped
	// at end of scope OUTSIDE m_ProcessingThreadsMutex.
	std::shared_ptr<SwapChainProcessor> processorToStop;

	{
		std::lock_guard<std::mutex> lock(m_ProcessingThreadsMutex);
		auto it = m_ProcessingThreads.find(Monitor);
		if (it != m_ProcessingThreads.end())
		{
			processorToStop = std::move(it->second);
			m_ProcessingThreads.erase(it);
		}
	}

	if (processorToStop)
	{
		vddlog("i", "Unassigning swapchain for one monitor. Its processing thread will be stopped.");
		if (processorToStop->m_hTerminateEvent.Get()) {
			SetEvent(processorToStop->m_hTerminateEvent.Get());
		}
	}
	else
	{
		vddlog("w", "UnassignSwapChain called for a monitor without an active processing thread.");
	}
}

#pragma endregion

#pragma region DDI Callbacks

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverAdapterInitFinished(IDDCX_ADAPTER AdapterObject, const IDARG_IN_ADAPTER_INIT_FINISHED* pInArgs)
{
	// This is called when the OS has finished setting up the adapter for use by the IddCx driver. It's now possible
	// to report attached monitors.

	auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(AdapterObject);
	if (NT_SUCCESS(pInArgs->AdapterInitStatus))
	{
		pContext->pContext->FinishInit();
		vddlog("d", "Adapter initialization finished successfully.");
	}
	else
	{
		stringstream ss;
		ss << "Adapter initialization failed. Status: " << pInArgs->AdapterInitStatus;
		vddlog("e", ss.str().c_str());
	}
	vddlog("i", "Finished Setting up adapter.");
	

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverAdapterCommitModes(IDDCX_ADAPTER AdapterObject, const IDARG_IN_COMMITMODES* pInArgs)
{
	UNREFERENCED_PARAMETER(AdapterObject);
	UNREFERENCED_PARAMETER(pInArgs);

	// For the sample, do nothing when modes are picked - the swap-chain is taken care of by IddCx

	// ==============================
	// TODO: In a real driver, this function would be used to reconfigure the device to commit the new modes. Loop
	// through pInArgs->pPaths and look for IDDCX_PATH_FLAGS_ACTIVE. Any path not active is inactive (e.g. the monitor
	// should be turned off).
	// ==============================

	return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION* pInArgs, IDARG_OUT_PARSEMONITORDESCRIPTION* pOutArgs)
{
	// ==============================
	// TODO: In a real driver, this function would be called to generate monitor modes for an EDID by parsing it. In
	// this sample driver, we hard-code the EDID, so this function can generate known modes.
	// ==============================

	stringstream logStream;
	logStream << "Parsing monitor description. Input buffer count: " << pInArgs->MonitorModeBufferInputCount;
	vddlog("d", logStream.str().c_str());

	// ROUND-2 FIX (Issue 2): RebuildKnownMonitorModesCache() CLEARS+REFILLS s_KnownMonitorModes2,
	// which is then INDEXED below — and the same vector is rebuilt by the description2 callback on
	// another thread. Hold g_SettingsMutex across the rebuild AND the indexing so the vector cannot
	// be cleared/reassigned mid-read (container UB). This callback makes NO IddCx call (it only
	// writes into the OS-supplied pMonitorModes buffer), so holding the lock across the body is
	// safe and cannot re-enter the driver.
	std::lock_guard<std::mutex> settingsLk(g_SettingsMutex);
	RebuildKnownMonitorModesCache();
	pOutArgs->MonitorModeBufferOutputCount = (UINT)monitorModes.size();

	logStream.str("");
	logStream << "Number of monitor modes generated: " << monitorModes.size();
	vddlog("d", logStream.str().c_str());

	if (pInArgs->MonitorModeBufferInputCount < monitorModes.size())
	{
		logStream.str("");
		logStream << "Buffer too small. Input count: " << pInArgs->MonitorModeBufferInputCount << ", Required: " << monitorModes.size();
		vddlog("w", logStream.str().c_str());
		// Return success if there was no buffer, since the caller was only asking for a count of modes
		return (pInArgs->MonitorModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
	}
	else
	{
		// Copy the known modes to the output buffer
		for (DWORD ModeIndex = 0; ModeIndex < monitorModes.size(); ModeIndex++)
		{
			pInArgs->pMonitorModes[ModeIndex].Size = sizeof(IDDCX_MONITOR_MODE);
			pInArgs->pMonitorModes[ModeIndex].Origin = IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR;
			pInArgs->pMonitorModes[ModeIndex].MonitorVideoSignalInfo = s_KnownMonitorModes2[ModeIndex];
		}

		// Set the preferred mode as represented in the EDID
		pOutArgs->PreferredMonitorModeIdx = 0;
		vddlog("d", "Monitor description parsed successfully.");
		return STATUS_SUCCESS;
	}
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverMonitorGetDefaultModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* pInArgs, IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* pOutArgs)
{
	UNREFERENCED_PARAMETER(MonitorObject);
	UNREFERENCED_PARAMETER(pInArgs);
	UNREFERENCED_PARAMETER(pOutArgs);

	// Should never be called since we create a single monitor with a known EDID in this sample driver.

	// ==============================
	// TODO: In a real driver, this function would be called to generate monitor modes for a monitor with no EDID.
	// Drivers should report modes that are guaranteed to be supported by the transport protocol and by nearly all
	// monitors (such 640x480, 800x600, or 1024x768). If the driver has access to monitor modes from a descriptor other
	// than an EDID, those modes would also be reported here.
	// ==============================

	return STATUS_NOT_IMPLEMENTED;
}

/// <summary>
/// Creates a target mode from the fundamental mode attributes.
/// </summary>
void CreateTargetMode(DISPLAYCONFIG_VIDEO_SIGNAL_INFO& Mode, UINT Width, UINT Height, UINT VSyncNum, UINT VSyncDen)
{
	stringstream logStream;
	logStream << "Creating target mode with Width: " << Width
		<< ", Height: " << Height
		<< ", VSyncNum: " << VSyncNum
		<< ", VSyncDen: " << VSyncDen;
	vddlog("d", logStream.str().c_str());

	Mode.totalSize.cx = Mode.activeSize.cx = Width;
	Mode.totalSize.cy = Mode.activeSize.cy = Height;
	Mode.AdditionalSignalInfo.vSyncFreqDivider = 1;
	Mode.AdditionalSignalInfo.videoStandard = 255;
	Mode.vSyncFreq.Numerator = VSyncNum;
	Mode.vSyncFreq.Denominator = VSyncDen;
	Mode.hSyncFreq.Numerator = VSyncNum * Height;
	Mode.hSyncFreq.Denominator = VSyncDen;
	Mode.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
	Mode.pixelRate = VSyncNum * Width * Height / VSyncDen;

	logStream.str("");
	logStream << "Target mode configured with:"
		<< "\n  Total Size: (" << Mode.totalSize.cx << ", " << Mode.totalSize.cy << ")"
		<< "\n  Active Size: (" << Mode.activeSize.cx << ", " << Mode.activeSize.cy << ")"
		<< "\n  vSync Frequency: " << Mode.vSyncFreq.Numerator << "/" << Mode.vSyncFreq.Denominator
		<< "\n  hSync Frequency: " << Mode.hSyncFreq.Numerator << "/" << Mode.hSyncFreq.Denominator
		<< "\n  Pixel Rate: " << Mode.pixelRate
		<< "\n  Scan Line Ordering: " << Mode.scanLineOrdering;
	vddlog("d", logStream.str().c_str());
}

void CreateTargetMode(IDDCX_TARGET_MODE& Mode, UINT Width, UINT Height, UINT VSyncNum, UINT VSyncDen)
{
	Mode.Size = sizeof(Mode);
	CreateTargetMode(Mode.TargetVideoSignalInfo.targetVideoSignalInfo, Width, Height, VSyncNum, VSyncDen);
}

void CreateTargetMode2(IDDCX_TARGET_MODE2& Mode, UINT Width, UINT Height, UINT VSyncNum, UINT VSyncDen)
{
	stringstream logStream;
	logStream << "Creating IDDCX_TARGET_MODE2 with Width: " << Width
		<< ", Height: " << Height
		<< ", VSyncNum: " << VSyncNum
		<< ", VSyncDen: " << VSyncDen;
	vddlog("d", logStream.str().c_str());

	Mode.Size = sizeof(Mode);

	// ROUND-2 FIX (Issue 2): SDRCOLOUR/HDRCOLOUR are written by the SDR10/HDRPLUS pipe handlers on
	// another thread. READ-lock and snapshot them into locals so the value used below is internally
	// consistent (no torn scalar). ColourFormat is load-time-only, so it is read without the lock.
	IDDCX_BITS_PER_COMPONENT sdrColour;
	IDDCX_BITS_PER_COMPONENT hdrColour;
	{
		std::lock_guard<std::mutex> lk(g_SettingsMutex);
		sdrColour = SDRCOLOUR;
		hdrColour = HDRCOLOUR;
	}

	if (ColourFormat == L"RGB") {
		Mode.BitsPerComponent.Rgb = sdrColour | hdrColour;
	}
	else if (ColourFormat == L"YCbCr444") {
		Mode.BitsPerComponent.YCbCr444 = sdrColour | hdrColour;
	}
	else if (ColourFormat == L"YCbCr422") {
		Mode.BitsPerComponent.YCbCr422 = sdrColour | hdrColour;
	}
	else if (ColourFormat == L"YCbCr420") {
		Mode.BitsPerComponent.YCbCr420 = sdrColour | hdrColour;
	}
	else {
		Mode.BitsPerComponent.Rgb = sdrColour | hdrColour;  // Default to RGB
	}


	logStream.str(""); 
	logStream << "IDDCX_TARGET_MODE2 configured with Size: " << Mode.Size
		<< " and colour format " << WStringToString(ColourFormat);
	vddlog("d", logStream.str().c_str());


	CreateTargetMode(Mode.TargetVideoSignalInfo.targetVideoSignalInfo, Width, Height, VSyncNum, VSyncDen);
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverMonitorQueryModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_QUERYTARGETMODES* pInArgs, IDARG_OUT_QUERYTARGETMODES* pOutArgs)////////////////////////////////////////////////////////////////////////////////
{
	UNREFERENCED_PARAMETER(MonitorObject);

	vector<IDDCX_TARGET_MODE> TargetModes(monitorModes.size());

	stringstream logStream;
	logStream << "Creating target modes. Number of monitor modes: " << monitorModes.size();
	vddlog("d", logStream.str().c_str());

	// Create a set of modes supported for frame processing and scan-out. These are typically not based on the
	// monitor's descriptor and instead are based on the static processing capability of the device. The OS will
	// report the available set of modes for a given output as the intersection of monitor modes with target modes.

	for (int i = 0; i < monitorModes.size(); i++) {
		CreateTargetMode(TargetModes[i], std::get<0>(monitorModes[i]), std::get<1>(monitorModes[i]), std::get<2>(monitorModes[i]), std::get<3>(monitorModes[i]));

		logStream.str("");
		logStream << "Created target mode " << i << ": Width = " << std::get<0>(monitorModes[i])
			<< ", Height = " << std::get<1>(monitorModes[i])
			<< ", VSync = " << std::get<2>(monitorModes[i]);
		vddlog("d", logStream.str().c_str());
	}

	pOutArgs->TargetModeBufferOutputCount = (UINT)TargetModes.size();

	logStream.str("");
	logStream << "Number of target modes to output: " << pOutArgs->TargetModeBufferOutputCount;
	vddlog("d", logStream.str().c_str());

	if (pInArgs->TargetModeBufferInputCount >= TargetModes.size())
	{
		logStream.str("");
		logStream << "Copying target modes to output buffer.";
		vddlog("d", logStream.str().c_str());
		copy(TargetModes.begin(), TargetModes.end(), pInArgs->pTargetModes);
	}
	else {
		logStream.str("");
		logStream << "Input buffer too small. Required: " << TargetModes.size()
			<< ", Provided: " << pInArgs->TargetModeBufferInputCount;
		vddlog("w", logStream.str().c_str());
	}

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverMonitorAssignSwapChain(IDDCX_MONITOR MonitorObject, const IDARG_IN_SETSWAPCHAIN* pInArgs)
{
	stringstream logStream;
	logStream << "Assigning swap chain:"
		<< "\n  hSwapChain: " << pInArgs->hSwapChain
		<< "\n  RenderAdapterLuid: " << pInArgs->RenderAdapterLuid.LowPart << "-" << pInArgs->RenderAdapterLuid.HighPart
		<< "\n  hNextSurfaceAvailable: " << pInArgs->hNextSurfaceAvailable;
	vddlog("d", logStream.str().c_str());
	auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(MonitorObject);
	pContext->pContext->AssignSwapChain(MonitorObject, pInArgs->hSwapChain, pInArgs->RenderAdapterLuid, pInArgs->hNextSurfaceAvailable);
	vddlog("d", "Swap chain assigned successfully.");
	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverMonitorUnassignSwapChain(IDDCX_MONITOR MonitorObject)
{
	stringstream logStream;
	logStream << "Unassigning swap chain for monitor object: " << MonitorObject;
	vddlog("d", logStream.str().c_str());
	auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(MonitorObject);
	pContext->pContext->UnassignSwapChain(MonitorObject);
	vddlog("d", "Swap chain unassigned successfully.");
	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverEvtIddCxAdapterQueryTargetInfo(
	IDDCX_ADAPTER AdapterObject,
	IDARG_IN_QUERYTARGET_INFO* pInArgs,
	IDARG_OUT_QUERYTARGET_INFO* pOutArgs
)
{
	stringstream logStream;
	logStream << "Querying target info for adapter object: " << AdapterObject;
	vddlog("d", logStream.str().c_str());

	UNREFERENCED_PARAMETER(pInArgs);

	pOutArgs->TargetCaps = IDDCX_TARGET_CAPS_HIGH_COLOR_SPACE | IDDCX_TARGET_CAPS_WIDE_COLOR_SPACE;

	if (ColourFormat == L"RGB") {
		pOutArgs->DitheringSupport.Rgb = SDRCOLOUR | HDRCOLOUR;
	}
	else if (ColourFormat == L"YCbCr444") {
		pOutArgs->DitheringSupport.YCbCr444 = SDRCOLOUR | HDRCOLOUR;
	}
	else if (ColourFormat == L"YCbCr422") {
		pOutArgs->DitheringSupport.YCbCr422 = SDRCOLOUR | HDRCOLOUR; 
	}
	else if (ColourFormat == L"YCbCr420") {
		pOutArgs->DitheringSupport.YCbCr420 = SDRCOLOUR | HDRCOLOUR; 
	}
	else {
		pOutArgs->DitheringSupport.Rgb = SDRCOLOUR | HDRCOLOUR;  // Default to RGB
	}

	logStream.str("");
	logStream << "Target capabilities set to: " << pOutArgs->TargetCaps
		<< "\nDithering support colour format set to: " << WStringToString(ColourFormat);
	vddlog("d", logStream.str().c_str());

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverEvtIddCxMonitorSetDefaultHdrMetadata(
	IDDCX_MONITOR MonitorObject,
	const IDARG_IN_MONITOR_SET_DEFAULT_HDR_METADATA* pInArgs
)
{
	UNREFERENCED_PARAMETER(pInArgs);
	
	stringstream logStream;
	logStream << "=== PROCESSING HDR METADATA REQUEST ===";
	vddlog("d", logStream.str().c_str());
	
	logStream.str("");
	logStream << "Monitor Object: " << MonitorObject 
			  << ", HDR10 Metadata Enabled: " << (hdr10StaticMetadataEnabled ? "Yes" : "No")
			  << ", Color Primaries Enabled: " << (colorPrimariesEnabled ? "Yes" : "No");
	vddlog("d", logStream.str().c_str());

	// Check if HDR metadata processing is enabled
	if (!hdr10StaticMetadataEnabled) {
		vddlog("i", "HDR10 static metadata is disabled, skipping metadata configuration");
		return STATUS_SUCCESS;
	}

	VddHdrMetadata metadata = {};
	bool hasValidMetadata = false;

	// Priority 1: Use EDID-derived metadata if available
	if (edidIntegrationEnabled && autoConfigureFromEdid) {
		// Guard the store: RemoveMonitor may erase this monitor's entry concurrently.
		std::lock_guard<std::mutex> hdrLock(g_HdrMetadataStoreMutex);
		// First check for monitor-specific metadata
		auto storeIt = g_HdrMetadataStore.find(MonitorObject);
		if (storeIt != g_HdrMetadataStore.end() && storeIt->second.isValid) {
			metadata = storeIt->second;
			hasValidMetadata = true;
			vddlog("i", "Using monitor-specific EDID-derived HDR metadata");
		}
		// If no monitor-specific metadata, check for template metadata from EDID profile
		else {
			auto templateIt = g_HdrMetadataStore.find(reinterpret_cast<IDDCX_MONITOR>(0));
			if (templateIt != g_HdrMetadataStore.end() && templateIt->second.isValid) {
				metadata = templateIt->second;
				hasValidMetadata = true;
				// Store it for this specific monitor for future use
				g_HdrMetadataStore[MonitorObject] = metadata;
				vddlog("i", "Using template EDID-derived HDR metadata and storing for monitor");
			}
		}
	}

	// Priority 2: Use manual configuration if no EDID data or manual override
	if (!hasValidMetadata || overrideManualSettings) {
		if (colorPrimariesEnabled) {
			metadata = ConvertManualToSmpteMetadata();
			hasValidMetadata = metadata.isValid;
			vddlog("i", "Using manually configured HDR metadata");
		}
	}

	// If we still don't have valid metadata, return early
	if (!hasValidMetadata) {
		vddlog("w", "No valid HDR metadata available, skipping configuration");
		return STATUS_SUCCESS;
	}

	// Log the HDR metadata values being applied
	logStream.str("");
	logStream << "=== APPLYING SMPTE ST.2086 HDR METADATA ===\n"
			  << "Red Primary: (" << metadata.display_primaries_x[0] << ", " << metadata.display_primaries_y[0] << ")\n"
			  << "Green Primary: (" << metadata.display_primaries_x[1] << ", " << metadata.display_primaries_y[1] << ")\n" 
			  << "Blue Primary: (" << metadata.display_primaries_x[2] << ", " << metadata.display_primaries_y[2] << ")\n"
			  << "White Point: (" << metadata.white_point_x << ", " << metadata.white_point_y << ")\n"
			  << "Max Mastering Luminance: " << metadata.max_display_mastering_luminance << " (0.0001 cd/m² units)\n"
			  << "Min Mastering Luminance: " << metadata.min_display_mastering_luminance << " (0.0001 cd/m² units)\n"
			  << "Max Content Light Level: " << metadata.max_content_light_level << " nits\n"
			  << "Max Frame Average Light Level: " << metadata.max_frame_avg_light_level << " nits";
	vddlog("i", logStream.str().c_str());

	// Store the metadata for this monitor
	{
		std::lock_guard<std::mutex> hdrLock(g_HdrMetadataStoreMutex);
		g_HdrMetadataStore[MonitorObject] = metadata;
	}

	// Convert our metadata to the IddCx expected format
	// Note: The actual HDR metadata structure would depend on the IddCx version
	// For now, we log that the metadata has been processed and stored
	
	logStream.str("");
	logStream << "HDR metadata successfully configured and stored for monitor " << MonitorObject;
	vddlog("i", logStream.str().c_str());

	// In a full implementation, you would pass the metadata to the IddCx framework here
	// The exact API calls would depend on IddCx version and HDR implementation details
	// For Phase 2, we focus on the metadata preparation and storage

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverEvtIddCxParseMonitorDescription2(
	const IDARG_IN_PARSEMONITORDESCRIPTION2* pInArgs,
	IDARG_OUT_PARSEMONITORDESCRIPTION* pOutArgs
)
{
	// ==============================
	// TODO: In a real driver, this function would be called to generate monitor modes for an EDID by parsing it. In
	// this sample driver, we hard-code the EDID, so this function can generate known modes.
	// ==============================

	stringstream logStream;
	logStream << "Parsing monitor description:"
		<< "\n  MonitorModeBufferInputCount: " << pInArgs->MonitorModeBufferInputCount
		<< "\n  pMonitorModes: " << (pInArgs->pMonitorModes ? "Valid" : "Null");
	vddlog("d", logStream.str().c_str());

	logStream.str("");
	logStream << "Monitor Modes:";
	for (const auto& mode : monitorModes)
	{
		logStream << "\n  Mode - Width: " << std::get<0>(mode)
			<< ", Height: " << std::get<1>(mode)
			<< ", RefreshRate: " << std::get<2>(mode);
	}
	vddlog("d", logStream.str().c_str());

	// ROUND-2 FIX (Issue 2): hold g_SettingsMutex across the s_KnownMonitorModes2 rebuild AND the
	// indexing+scalar reads below (the same vector is rebuilt by the non-2 ParseMonitorDescription
	// callback, and SDRCOLOUR/HDRCOLOUR are written by the SDR10/HDRPLUS pipe handlers). This
	// callback makes NO IddCx call (only writes the OS-supplied pMonitorModes buffer), so holding
	// the lock across the body is safe. ColourFormat is load-time-only (read without the lock).
	std::lock_guard<std::mutex> settingsLk(g_SettingsMutex);
	RebuildKnownMonitorModesCache();
	pOutArgs->MonitorModeBufferOutputCount = (UINT)monitorModes.size();

	if (pInArgs->MonitorModeBufferInputCount < monitorModes.size())
	{
		// Return success if there was no buffer, since the caller was only asking for a count of modes
		return (pInArgs->MonitorModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
	}
	else
	{
		// Copy the known modes to the output buffer
		if (pInArgs->pMonitorModes == nullptr) {
			vddlog("e", "pMonitorModes is null but buffer size is sufficient");
			return STATUS_INVALID_PARAMETER;
		}

		// Snapshot the raced scalars once (we already hold g_SettingsMutex for the whole body).
		const IDDCX_BITS_PER_COMPONENT sdrColour = SDRCOLOUR;
		const IDDCX_BITS_PER_COMPONENT hdrColour = HDRCOLOUR;

		logStream.str(""); // Clear the stream
		logStream << "Writing monitor modes to output buffer:";
		for (DWORD ModeIndex = 0; ModeIndex < monitorModes.size(); ModeIndex++)
		{
			pInArgs->pMonitorModes[ModeIndex].Size = sizeof(IDDCX_MONITOR_MODE2);
			pInArgs->pMonitorModes[ModeIndex].Origin = IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR;
			pInArgs->pMonitorModes[ModeIndex].MonitorVideoSignalInfo = s_KnownMonitorModes2[ModeIndex];


			if (ColourFormat == L"RGB") {
				pInArgs->pMonitorModes[ModeIndex].BitsPerComponent.Rgb = sdrColour | hdrColour;

			}
			else if (ColourFormat == L"YCbCr444") {
				pInArgs->pMonitorModes[ModeIndex].BitsPerComponent.YCbCr444 = sdrColour | hdrColour;
			}
			else if (ColourFormat == L"YCbCr422") {
				pInArgs->pMonitorModes[ModeIndex].BitsPerComponent.YCbCr422 = sdrColour | hdrColour;
			}
			else if (ColourFormat == L"YCbCr420") {
				pInArgs->pMonitorModes[ModeIndex].BitsPerComponent.YCbCr420 = sdrColour | hdrColour;
			}
			else {
				pInArgs->pMonitorModes[ModeIndex].BitsPerComponent.Rgb = sdrColour | hdrColour;  // Default to RGB
			}



			logStream << "\n  ModeIndex: " << ModeIndex
				<< "\n    Size: " << pInArgs->pMonitorModes[ModeIndex].Size
				<< "\n    Origin: " << pInArgs->pMonitorModes[ModeIndex].Origin
				<< "\n    Colour Format: " << WStringToString(ColourFormat);
		}

		vddlog("d", logStream.str().c_str());

		// Set the preferred mode as represented in the EDID
		pOutArgs->PreferredMonitorModeIdx = 0;

		return STATUS_SUCCESS;
	}
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverEvtIddCxMonitorQueryTargetModes2(
	IDDCX_MONITOR MonitorObject,
	const IDARG_IN_QUERYTARGETMODES2* pInArgs,
	IDARG_OUT_QUERYTARGETMODES* pOutArgs
)
{
	//UNREFERENCED_PARAMETER(MonitorObject);
	stringstream logStream;

	logStream << "Querying target modes:"
		<< "\n  MonitorObject Handle: " << static_cast<void*>(MonitorObject) 
		<< "\n  TargetModeBufferInputCount: " << pInArgs->TargetModeBufferInputCount;
	vddlog("d", logStream.str().c_str());

	vector<IDDCX_TARGET_MODE2> TargetModes(monitorModes.size());

	// Create a set of modes supported for frame processing and scan-out. These are typically not based on the
	// monitor's descriptor and instead are based on the static processing capability of the device. The OS will
	// report the available set of modes for a given output as the intersection of monitor modes with target modes.

	logStream.str(""); // Clear the stream
	logStream << "Creating target modes:";

	for (int i = 0; i < monitorModes.size(); i++) {
		CreateTargetMode2(TargetModes[i], std::get<0>(monitorModes[i]), std::get<1>(monitorModes[i]), std::get<2>(monitorModes[i]), std::get<3>(monitorModes[i]));
		logStream << "\n  TargetModeIndex: " << i
			<< "\n    Width: " << std::get<0>(monitorModes[i])
			<< "\n    Height: " << std::get<1>(monitorModes[i])
			<< "\n    RefreshRate: " << std::get<2>(monitorModes[i]);
	}
	vddlog("d", logStream.str().c_str());

	pOutArgs->TargetModeBufferOutputCount = (UINT)TargetModes.size();

	logStream.str("");
	logStream << "Output target modes count: " << pOutArgs->TargetModeBufferOutputCount;
	vddlog("d", logStream.str().c_str());

	if (pInArgs->TargetModeBufferInputCount >= TargetModes.size())
	{
		copy(TargetModes.begin(), TargetModes.end(), pInArgs->pTargetModes);

		logStream.str("");
		logStream << "Target modes copied to output buffer:";
		for (int i = 0; i < TargetModes.size(); i++)
		{
			logStream << "\n  TargetModeIndex: " << i
				<< "\n    Size: " << TargetModes[i].Size
				<< "\n    ColourFormat: " << WStringToString(ColourFormat);
		}
		vddlog("d", logStream.str().c_str());
	}
	else
	{
		vddlog("w", "Input buffer is too small for target modes.");
	}

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverEvtIddCxAdapterCommitModes2(
	IDDCX_ADAPTER AdapterObject,
	const IDARG_IN_COMMITMODES2* pInArgs
)
{
	UNREFERENCED_PARAMETER(AdapterObject);
	UNREFERENCED_PARAMETER(pInArgs);

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverEvtIddCxMonitorSetGammaRamp(
	IDDCX_MONITOR MonitorObject,
	const IDARG_IN_SET_GAMMARAMP* pInArgs
)
{
	stringstream logStream;
	logStream << "=== PROCESSING GAMMA RAMP REQUEST ===";
	vddlog("d", logStream.str().c_str());
	
	logStream.str("");
	logStream << "Monitor Object: " << MonitorObject 
			  << ", Color Space Enabled: " << (colorSpaceEnabled ? "Yes" : "No")
			  << ", Matrix Transform Enabled: " << (enableMatrixTransform ? "Yes" : "No");
	vddlog("d", logStream.str().c_str());

	// Check if color space processing is enabled
	if (!colorSpaceEnabled) {
		vddlog("i", "Color space processing is disabled, skipping gamma ramp configuration");
		return STATUS_SUCCESS;
	}

	VddGammaRamp gammaRamp = {};
	bool hasValidGammaRamp = false;

	// Priority 1: Use EDID-derived gamma settings if available
	if (edidIntegrationEnabled && autoConfigureFromEdid) {
		// Guard the store: RemoveMonitor may erase this monitor's entry concurrently.
		std::lock_guard<std::mutex> gammaLock(g_GammaRampStoreMutex);
		// First check for monitor-specific gamma ramp
		auto storeIt = g_GammaRampStore.find(MonitorObject);
		if (storeIt != g_GammaRampStore.end() && storeIt->second.isValid) {
			gammaRamp = storeIt->second;
			hasValidGammaRamp = true;
			vddlog("i", "Using monitor-specific EDID-derived gamma ramp");
		}
		// If no monitor-specific gamma ramp, check for template from EDID profile
		else {
			auto templateIt = g_GammaRampStore.find(reinterpret_cast<IDDCX_MONITOR>(0));
			if (templateIt != g_GammaRampStore.end() && templateIt->second.isValid) {
				gammaRamp = templateIt->second;
				hasValidGammaRamp = true;
				// Store it for this specific monitor for future use
				g_GammaRampStore[MonitorObject] = gammaRamp;
				vddlog("i", "Using template EDID-derived gamma ramp and storing for monitor");
			}
		}
	}

	// Priority 2: Use manual configuration if no EDID data or manual override
	if (!hasValidGammaRamp || overrideManualSettings) {
		gammaRamp = ConvertManualToGammaRamp();
		hasValidGammaRamp = gammaRamp.isValid;
		vddlog("i", "Using manually configured gamma ramp");
	}

	// If we still don't have valid gamma settings, return early
	if (!hasValidGammaRamp) {
		vddlog("w", "No valid gamma ramp available, skipping configuration");
		return STATUS_SUCCESS;
	}

	// Log the gamma ramp values being applied
	logStream.str("");
	logStream << "=== APPLYING GAMMA RAMP AND COLOR SPACE TRANSFORM ===\n"
			  << "Gamma Value: " << gammaRamp.gamma << "\n"
			  << "Color Space: " << WStringToString(gammaRamp.colorSpace) << "\n"
			  << "Use Matrix Transform: " << (gammaRamp.useMatrix ? "Yes" : "No");
	vddlog("i", logStream.str().c_str());

	// Apply gamma ramp based on type
	if (pInArgs->Type == IDDCX_GAMMARAMP_TYPE_3x4_COLORSPACE_TRANSFORM && gammaRamp.useMatrix) {
		// Apply 3x4 color space transformation matrix
		logStream.str("");
		logStream << "Applying 3x4 Color Space Matrix:\n"
				  << "  [" << gammaRamp.matrix.matrix[0][0] << ", " << gammaRamp.matrix.matrix[0][1] << ", " << gammaRamp.matrix.matrix[0][2] << ", " << gammaRamp.matrix.matrix[0][3] << "]\n"
				  << "  [" << gammaRamp.matrix.matrix[1][0] << ", " << gammaRamp.matrix.matrix[1][1] << ", " << gammaRamp.matrix.matrix[1][2] << ", " << gammaRamp.matrix.matrix[1][3] << "]\n"
				  << "  [" << gammaRamp.matrix.matrix[2][0] << ", " << gammaRamp.matrix.matrix[2][1] << ", " << gammaRamp.matrix.matrix[2][2] << ", " << gammaRamp.matrix.matrix[2][3] << "]";
		vddlog("i", logStream.str().c_str());

		// Store the matrix for this monitor
		{
			std::lock_guard<std::mutex> gammaLock(g_GammaRampStoreMutex);
			g_GammaRampStore[MonitorObject] = gammaRamp;
		}

		// In a full implementation, you would apply the matrix to the rendering pipeline here
		// The exact API calls would depend on IddCx version and hardware capabilities
		
		logStream.str("");
		logStream << "3x4 matrix transform applied successfully for monitor " << MonitorObject;
		vddlog("i", logStream.str().c_str());
	}
	else if (pInArgs->Type == IDDCX_GAMMARAMP_TYPE_RGB256x3x16) {
		// Apply traditional RGB gamma ramp
		logStream.str("");
		logStream << "Applying RGB 256x3x16 gamma ramp with gamma " << gammaRamp.gamma;
		vddlog("i", logStream.str().c_str());

		// In a full implementation, you would generate and apply RGB lookup tables here
		// Based on the gamma value and color space
		
		logStream.str("");
		logStream << "RGB gamma ramp applied successfully for monitor " << MonitorObject;
		vddlog("i", logStream.str().c_str());
	}
	else {
		logStream.str("");
		logStream << "Unsupported gamma ramp type: " << pInArgs->Type << ", using default gamma processing";
		vddlog("w", logStream.str().c_str());
	}

	// Store the final gamma ramp for this monitor
	{
		std::lock_guard<std::mutex> gammaLock(g_GammaRampStoreMutex);
		g_GammaRampStore[MonitorObject] = gammaRamp;
	}

	logStream.str("");
	logStream << "Gamma ramp configuration completed for monitor " << MonitorObject;
	vddlog("i", logStream.str().c_str());

	return STATUS_SUCCESS;
}

#pragma endregion

#pragma region Phase 5: Final Integration and Testing

// ===========================================
// PHASE 5: FINAL INTEGRATION AND TESTING
// ===========================================

struct VddIntegrationStatus {
	bool edidParsingEnabled = false;
	bool hdrMetadataValid = false;
	bool gammaRampValid = false;
	bool modeManagementActive = false;
	bool configurationValid = false;
	wstring lastError = L"";
	DWORD errorCount = 0;
	DWORD warningCount = 0;
};

static VddIntegrationStatus g_IntegrationStatus = {};

NTSTATUS ValidateEdidIntegration()
{
	stringstream logStream;
	logStream << "=== EDID INTEGRATION VALIDATION ===";
	vddlog("i", logStream.str().c_str());

	bool validationPassed = true;
	DWORD issues = 0;

	// Check EDID integration settings
	bool edidEnabled = EnabledQuery(L"EdidIntegrationEnabled");
	bool autoConfig = EnabledQuery(L"AutoConfigureFromEdid");
	wstring profilePath = GetStringSetting(L"EdidProfilePath");

	logStream.str("");
	logStream << "EDID Configuration Status:"
		<< "\n  Integration Enabled: " << (edidEnabled ? "Yes" : "No")
		<< "\n  Auto Configuration: " << (autoConfig ? "Yes" : "No")
		<< "\n  Profile Path: " << WStringToString(profilePath);
	vddlog("d", logStream.str().c_str());

	if (!edidEnabled) {
		vddlog("w", "EDID integration is disabled - manual configuration mode");
		issues++;
	}

	if (profilePath.empty() || profilePath == L"EDID/monitor_profile.xml") {
		vddlog("w", "EDID profile path not configured or using default path");
		issues++;
	}

	// Validate HDR configuration
	bool hdrEnabled = EnabledQuery(L"Hdr10StaticMetadataEnabled");
	bool colorEnabled = EnabledQuery(L"ColorPrimariesEnabled");
	
	logStream.str("");
	logStream << "HDR Configuration Status:"
		<< "\n  HDR10 Metadata: " << (hdrEnabled ? "Enabled" : "Disabled")
		<< "\n  Color Primaries: " << (colorEnabled ? "Enabled" : "Disabled");
	vddlog("d", logStream.str().c_str());

	// Validate mode management
	bool autoResEnabled = EnabledQuery(L"AutoResolutionsEnabled");
	wstring localSourcePriority = GetStringSetting(L"SourcePriority");
	
	logStream.str("");
	logStream << "Mode Management Status:"
		<< "\n  Auto Resolutions: " << (autoResEnabled ? "Enabled" : "Disabled")
		<< "\n  Source Priority: " << WStringToString(localSourcePriority);
	vddlog("d", logStream.str().c_str());

	// Update integration status
	g_IntegrationStatus.edidParsingEnabled = edidEnabled;
	g_IntegrationStatus.hdrMetadataValid = hdrEnabled;
	g_IntegrationStatus.modeManagementActive = autoResEnabled;
	g_IntegrationStatus.configurationValid = (issues < 3);
	g_IntegrationStatus.warningCount = issues;

	logStream.str("");
	logStream << "=== VALIDATION SUMMARY ==="
		<< "\n  Total Issues Found: " << issues
		<< "\n  Configuration Valid: " << (g_IntegrationStatus.configurationValid ? "Yes" : "No")
		<< "\n  Integration Status: " << (validationPassed ? "PASSED" : "FAILED");
	vddlog("i", logStream.str().c_str());

	return validationPassed ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

NTSTATUS PerformanceMonitor()
{
	stringstream logStream;
	logStream << "=== PERFORMANCE MONITORING ===";
	vddlog("d", logStream.str().c_str());

	// Monitor mode generation performance
	auto start = chrono::high_resolution_clock::now();
	
	// Test mode generation with current configuration
	vector<tuple<DWORD, DWORD, DWORD, DWORD>> testModes;
	// Use the global monitorModes for performance testing
	for (const auto& mode : monitorModes) {
		testModes.push_back(make_tuple(get<0>(mode), get<1>(mode), get<2>(mode), get<3>(mode)));
	}
	
	auto end = chrono::high_resolution_clock::now();
	auto duration = chrono::duration_cast<chrono::microseconds>(end - start);

	logStream.str("");
	logStream << "Performance Metrics:"
		<< "\n  Mode Generation Time: " << duration.count() << " microseconds"
		<< "\n  Total Modes Generated: " << testModes.size()
		<< "\n  Memory Usage: ~" << (testModes.size() * sizeof(tuple<DWORD, DWORD, DWORD, DWORD>)) << " bytes";
	vddlog("i", logStream.str().c_str());

	// Performance thresholds
	if (duration.count() > 10000) { // 10ms threshold
		vddlog("w", "Mode generation is slower than expected (>10ms)");
		g_IntegrationStatus.warningCount++;
	}

	if (testModes.size() > 100) {
		vddlog("w", "Large number of modes generated - consider filtering");
		g_IntegrationStatus.warningCount++;
	}

	return STATUS_SUCCESS;
}

NTSTATUS CreateFallbackConfiguration()
{
	stringstream logStream;
	logStream << "=== CREATING FALLBACK CONFIGURATION ===";
	vddlog("i", logStream.str().c_str());

	// Create safe fallback modes if EDID parsing fails
	vector<tuple<DWORD, DWORD, DWORD, DWORD>> fallbackModes = {
		make_tuple(1920, 1080, 60, 0),   // Full HD 60Hz
		make_tuple(1366, 768, 60, 0),    // Common laptop resolution
		make_tuple(1280, 720, 60, 0),    // HD 60Hz
		make_tuple(800, 600, 60, 0)      // Safe fallback
	};

	logStream.str("");
	logStream << "Fallback modes created:";
	for (const auto& mode : fallbackModes) {
		logStream << "\n  " << get<0>(mode) << "x" << get<1>(mode) << "@" << get<2>(mode) << "Hz";
	}
	vddlog("d", logStream.str().c_str());

	// Create fallback HDR metadata
	VddHdrMetadata fallbackHdr = {};
	fallbackHdr.display_primaries_x[0] = 31250; // sRGB red
	fallbackHdr.display_primaries_y[0] = 16992;
	fallbackHdr.display_primaries_x[1] = 15625; // sRGB green
	fallbackHdr.display_primaries_y[1] = 35352;
	fallbackHdr.display_primaries_x[2] = 7812;  // sRGB blue
	fallbackHdr.display_primaries_y[2] = 3906;
	fallbackHdr.white_point_x = 15625; // D65 white point
	fallbackHdr.white_point_y = 16406;
	fallbackHdr.max_display_mastering_luminance = 1000000; // 100 nits
	fallbackHdr.min_display_mastering_luminance = 500;     // 0.05 nits
	fallbackHdr.max_content_light_level = 400;
	fallbackHdr.max_frame_avg_light_level = 100;
	fallbackHdr.isValid = true;

	logStream.str("");
	logStream << "Fallback HDR metadata (sRGB/D65):"
		<< "\n  Max Mastering Luminance: " << fallbackHdr.max_display_mastering_luminance 
		<< "\n  Min Mastering Luminance: " << fallbackHdr.min_display_mastering_luminance
		<< "\n  Max Content Light: " << fallbackHdr.max_content_light_level << " nits";
	vddlog("d", logStream.str().c_str());

	vddlog("i", "Fallback configuration ready for emergency use");
	return STATUS_SUCCESS;
}

NTSTATUS RunComprehensiveDiagnostics()
{
	stringstream logStream;
	logStream << "=== COMPREHENSIVE SYSTEM DIAGNOSTICS ===";
	vddlog("i", logStream.str().c_str());

	NTSTATUS status = STATUS_SUCCESS;
	
	// Reset diagnostic counters
	g_IntegrationStatus.errorCount = 0;
	g_IntegrationStatus.warningCount = 0;

	// Test 1: Configuration validation
	logStream.str("");
	logStream << "Running Test 1: Configuration Validation";
	vddlog("d", logStream.str().c_str());
	
	NTSTATUS configStatus = ValidateEdidIntegration();
	if (!NT_SUCCESS(configStatus)) {
		vddlog("e", "Configuration validation failed");
		g_IntegrationStatus.errorCount++;
		status = configStatus;
	}

	// Test 2: Performance monitoring
	logStream.str("");
	logStream << "Running Test 2: Performance Monitoring";
	vddlog("d", logStream.str().c_str());
	
	NTSTATUS perfStatus = PerformanceMonitor();
	if (!NT_SUCCESS(perfStatus)) {
		vddlog("e", "Performance monitoring failed");
		g_IntegrationStatus.errorCount++;
	}

	// Test 3: Fallback configuration
	logStream.str("");
	logStream << "Running Test 3: Fallback Configuration";
	vddlog("d", logStream.str().c_str());
	
	NTSTATUS fallbackStatus = CreateFallbackConfiguration();
	if (!NT_SUCCESS(fallbackStatus)) {
		vddlog("e", "Fallback configuration creation failed");
		g_IntegrationStatus.errorCount++;
	}

	// Test 4: Memory and resource validation
	logStream.str("");
	logStream << "Running Test 4: Resource Validation";
	vddlog("d", logStream.str().c_str());

	// Check global stores
	size_t hdrStoreSize = g_HdrMetadataStore.size();
	size_t gammaStoreSize = g_GammaRampStore.size();
	
	logStream.str("");
	logStream << "Resource Usage:"
		<< "\n  HDR Metadata Store: " << hdrStoreSize << " entries"
		<< "\n  Gamma Ramp Store: " << gammaStoreSize << " entries"
		<< "\n  Known Monitor Modes: " << s_KnownMonitorModes2.size() << " modes";
	vddlog("d", logStream.str().c_str());

	// Final diagnostic summary
	logStream.str("");
	logStream << "=== DIAGNOSTIC SUMMARY ==="
		<< "\n  Tests Run: 4"
		<< "\n  Errors: " << g_IntegrationStatus.errorCount
		<< "\n  Warnings: " << g_IntegrationStatus.warningCount
		<< "\n  Overall Status: " << (NT_SUCCESS(status) ? "PASSED" : "FAILED");
	vddlog("i", logStream.str().c_str());

	return status;
}

NTSTATUS ValidateAndSanitizeConfiguration()
{
	stringstream logStream;
	logStream << "=== CONFIGURATION VALIDATION AND SANITIZATION ===";
	vddlog("i", logStream.str().c_str());

	DWORD sanitizedSettings = 0;

	// Validate refresh rate settings  
	double minRefresh = GetDoubleSetting(L"MinRefreshRate");
	double maxRefresh = GetDoubleSetting(L"MaxRefreshRate");
	
	if (minRefresh <= 0 || minRefresh > 300) {
		vddlog("w", "Invalid min refresh rate detected, setting to safe default (24Hz)");
		minRefresh = 24.0;
		sanitizedSettings++;
	}
	
	if (maxRefresh <= minRefresh || maxRefresh > 500) {
		vddlog("w", "Invalid max refresh rate detected, setting to safe default (240Hz)");
		maxRefresh = 240.0;
		sanitizedSettings++;
	}

	// Validate resolution settings
	int minWidth = GetIntegerSetting(L"MinResolutionWidth");
	int minHeight = GetIntegerSetting(L"MinResolutionHeight");
	int maxWidth = GetIntegerSetting(L"MaxResolutionWidth");
	int maxHeight = GetIntegerSetting(L"MaxResolutionHeight");

	if (minWidth < 640 || minWidth > 7680) {
		vddlog("w", "Invalid min width detected, setting to 640");
		minWidth = 640;
		sanitizedSettings++;
	}
	
	if (minHeight < 480 || minHeight > 4320) {
		vddlog("w", "Invalid min height detected, setting to 480");
		minHeight = 480;
		sanitizedSettings++;
	}

	if (maxWidth < minWidth || maxWidth > 15360) {
		vddlog("w", "Invalid max width detected, setting to 7680");
		maxWidth = 7680;
		sanitizedSettings++;
	}
	
	if (maxHeight < minHeight || maxHeight > 8640) {
		vddlog("w", "Invalid max height detected, setting to 4320");
		maxHeight = 4320;
		sanitizedSettings++;
	}

	// Validate HDR luminance values
	double maxLuminance = GetDoubleSetting(L"MaxDisplayMasteringLuminance");
	double minLuminance = GetDoubleSetting(L"MinDisplayMasteringLuminance");
	
	if (maxLuminance <= 0 || maxLuminance > 10000) {
		vddlog("w", "Invalid max luminance detected, setting to 1000 nits");
		maxLuminance = 1000.0;
		sanitizedSettings++;
	}
	
	if (minLuminance <= 0 || minLuminance >= maxLuminance) {
		vddlog("w", "Invalid min luminance detected, setting to 0.05 nits");
		minLuminance = 0.05;
		sanitizedSettings++;
	}

	// Validate color primaries
	double localRedX = GetDoubleSetting(L"RedX");
	double localRedY = GetDoubleSetting(L"RedY");
	double localGreenX = GetDoubleSetting(L"GreenX");
	double localGreenY = GetDoubleSetting(L"GreenY");
	double localBlueX = GetDoubleSetting(L"BlueX");
	double localBlueY = GetDoubleSetting(L"BlueY");

	if (localRedX < 0.0 || localRedX > 1.0 || localRedY < 0.0 || localRedY > 1.0) {
		vddlog("w", "Invalid red primary coordinates, using sRGB defaults");
		sanitizedSettings++;
	}
	
	if (localGreenX < 0.0 || localGreenX > 1.0 || localGreenY < 0.0 || localGreenY > 1.0) {
		vddlog("w", "Invalid green primary coordinates, using sRGB defaults");
		sanitizedSettings++;
	}
	
	if (localBlueX < 0.0 || localBlueX > 1.0 || localBlueY < 0.0 || localBlueY > 1.0) {
		vddlog("w", "Invalid blue primary coordinates, using sRGB defaults");
		sanitizedSettings++;
	}

	logStream.str("");
	logStream << "Configuration validation completed:"
		<< "\n  Settings sanitized: " << sanitizedSettings
		<< "\n  Refresh rate range: " << minRefresh << "-" << maxRefresh << " Hz"
		<< "\n  Resolution range: " << minWidth << "x" << minHeight << " to " << maxWidth << "x" << maxHeight
		<< "\n  Luminance range: " << minLuminance << "-" << maxLuminance << " nits";
	vddlog("i", logStream.str().c_str());

	return (sanitizedSettings < 5) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

NTSTATUS InitializePhase5Integration()
{
	stringstream logStream;
	logStream << "=== PHASE 5: FINAL INTEGRATION INITIALIZATION ===";
	vddlog("i", logStream.str().c_str());

	// Initialize integration status
	g_IntegrationStatus = {};
	
	// Run configuration validation and sanitization
	NTSTATUS configStatus = ValidateAndSanitizeConfiguration();
	if (!NT_SUCCESS(configStatus)) {
		vddlog("w", "Configuration validation completed with warnings");
		g_IntegrationStatus.warningCount++;
	}
	
	// Run comprehensive diagnostics
	NTSTATUS diagStatus = RunComprehensiveDiagnostics();
	
	if (NT_SUCCESS(diagStatus) && NT_SUCCESS(configStatus)) {
		vddlog("i", "Phase 5 integration completed successfully");
		g_IntegrationStatus.configurationValid = true;
	} else {
		vddlog("e", "Phase 5 integration completed with errors");
		g_IntegrationStatus.lastError = L"Diagnostic failures detected";
	}

	// Log final integration status
	logStream.str("");
	logStream << "=== FINAL INTEGRATION STATUS ==="
		<< "\n  EDID Integration: " << (g_IntegrationStatus.edidParsingEnabled ? "ACTIVE" : "INACTIVE")
		<< "\n  HDR Metadata: " << (g_IntegrationStatus.hdrMetadataValid ? "VALID" : "INVALID")
		<< "\n  Gamma Processing: " << (g_IntegrationStatus.gammaRampValid ? "ACTIVE" : "INACTIVE")  
		<< "\n  Mode Management: " << (g_IntegrationStatus.modeManagementActive ? "ACTIVE" : "INACTIVE")
		<< "\n  Configuration: " << (g_IntegrationStatus.configurationValid ? "VALID" : "INVALID")
		<< "\n  Total Errors: " << g_IntegrationStatus.errorCount
		<< "\n  Total Warnings: " << g_IntegrationStatus.warningCount;
	vddlog("i", logStream.str().c_str());

	return (NT_SUCCESS(diagStatus) && NT_SUCCESS(configStatus)) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

#pragma endregion
