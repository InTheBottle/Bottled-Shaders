#include "Streamline.h"

#include <algorithm>
#include <cmath>
#include <dxgi.h>
#include <dxgi1_3.h>
#include <filesystem>

#include "../../Deferred.h"
#include "../../Hooks.h"
#include "../../State.h"
#include "../../Util.h"
#include "../../Utils/FrameCosts.h"
#include "../Upscaling.h"
#include "DX12SwapChain.h"
#include "DlssNR.h"
#include "RTX40MFG/MfgUnlock.h"

namespace
{
	constexpr UINT NVIDIA_VENDOR_ID = 0x10DE;
	constexpr uint32_t kDLSSGStateQueryInterval = 15;
	constexpr uint32_t kDLSSGPresentSafetyFrames = 3;
	// Streamline 2.11.1 removed the typed eInputSample enum value, but Reflex/NVAPI latency
	// reports still expose inputSampleTime for marker value 6.
	constexpr auto kInputSampleMarker = static_cast<sl::PCLMarker>(6);

	void DLSSGAPIErrorCallback(const sl::APIError& a_error)
	{
		logger::warn("[Streamline] DLSS-G present API error hres=0x{:08X}", static_cast<uint32_t>(a_error.hres));
	}

	std::string DLSSGStatusFlags(sl::DLSSGStatus a_status)
	{
		if (a_status == sl::DLSSGStatus::eOk)
			return "eOk";

		std::string flags;
		const auto append = [&](sl::DLSSGStatus a_flag, std::string_view a_name) {
			if (a_status & a_flag) {
				if (!flags.empty())
					flags += '|';
				flags += a_name;
			}
		};

		append(sl::DLSSGStatus::eFailResolutionTooLow, "ResolutionTooLow");
		append(sl::DLSSGStatus::eFailReflexNotDetectedAtRuntime, "ReflexNotDetected");
		append(sl::DLSSGStatus::eFailHDRFormatNotSupported, "HDRFormatNotSupported");
		append(sl::DLSSGStatus::eFailCommonConstantsInvalid, "CommonConstantsInvalid");
		append(sl::DLSSGStatus::eFailGetCurrentBackBufferIndexNotCalled, "GetCurrentBackBufferIndexNotCalled");
		return flags.empty() ? "unknown" : flags;
	}
}

void LoggingCallback(sl::LogType type, const char* msg)
{
	// Remove trailing newlines from the raw message
	std::string rawMsg(msg);
	while (!rawMsg.empty() && (rawMsg.back() == '\n' || rawMsg.back() == '\r'))
		rawMsg.pop_back();

	// Remove leading bracketed metadata
	const char* p = msg;
	while (*p == '[') {
		const char* close = strchr(p, ']');
		if (!close)
			break;
		p = close + 1;
		// Skip whitespace after each bracketed section
		while (*p == ' ' || *p == '\t') ++p;
	}
	// Now p points to the first non-bracketed section (file/line info or message)
	std::string cleanMsg(p);
	// Trim leading/trailing whitespace and newlines
	size_t start = cleanMsg.find_first_not_of(" \t\r\n");
	size_t end = cleanMsg.find_last_not_of(" \t\r\n");
	if (start != std::string::npos && end != std::string::npos)
		cleanMsg = cleanMsg.substr(start, end - start + 1);
	else
		cleanMsg.clear();

	// If the cleaned message is empty or only bracketed tokens, log the raw message
	bool onlyBrackets = true;
	for (char c : cleanMsg) {
		if (c != '[' && c != ']' && c != ' ' && c != '\t') {
			onlyBrackets = false;
			break;
		}
	}
	if (cleanMsg.empty() || onlyBrackets) {
		logger::info("[StreamlineSDK:RAW] {}", rawMsg);
		return;
	}

	// Use a clear prefix
	const char* prefix = "[StreamlineSDK]";
	switch (type) {
	case sl::LogType::eInfo:
		logger::info("{} {}", prefix, cleanMsg);
		break;
	case sl::LogType::eWarn:
		logger::warn("{} {}", prefix, cleanMsg);
		break;
	case sl::LogType::eError:
		logger::error("{} {}", prefix, cleanMsg);
		break;
	}
}

std::vector<std::pair<std::string, std::string>> Streamline::dllVersions = {};

void Streamline::LoadInterposer()
{
	if (interposerLoaded)
		return;

	triedInitialization = true;

	std::wstring interposerPath = std::wstring(Streamline::PluginDir) + L"\\sl.interposer.dll";
	interposer = LoadLibraryW(interposerPath.c_str());
	if (interposer == nullptr) {
		DWORD errorCode = GetLastError();
		logger::info("[Streamline] Failed to load interposer: Error Code {0:x}", errorCode);
		return;
	} else {
		logger::info("[Streamline] Interposer loaded at address: {0:p}", static_cast<void*>(interposer));
	}

	// Dynamically log all DLL versions in the Streamline plugin directory
	std::filesystem::path pluginDir = std::filesystem::path(Streamline::PluginDir);
	Streamline::dllVersions = Util::EnumerateDllVersions(pluginDir);
	for (const auto& [name, versionStr] : Streamline::dllVersions)
		logger::info("[Streamline] {} version: {}", name, versionStr);

	std::error_code pluginPathError;
	auto pluginDirAbsolutePath = std::filesystem::absolute(pluginDir, pluginPathError);
	if (pluginPathError)
		pluginDirAbsolutePath = pluginDir;
	pluginDirAbsolute = pluginDirAbsolutePath.wstring();
	logger::info("[Streamline] Plugin search path: {}", pluginDirAbsolutePath.string());

	std::error_code existsError;
	dlssgModulePresent =
		std::filesystem::exists(pluginDirAbsolutePath / L"sl.dlss_g.dll", existsError) &&
		std::filesystem::exists(pluginDirAbsolutePath / L"nvngx_dlssg.dll", existsError);
	logger::info("[Streamline] DLSS Frame Generation runtime {}", dlssgModulePresent ? "found" : "missing (sl.dlss_g.dll / nvngx_dlssg.dll)");

	DlssNR::SetRuntimeDirectory(pluginDirAbsolute);

	// Hook up all of the functions exported by the SL Interposer Library
	slInit = (PFun_slInit*)GetProcAddress(interposer, "slInit");
	slShutdown = (PFun_slShutdown*)GetProcAddress(interposer, "slShutdown");
	slIsFeatureSupported = (PFun_slIsFeatureSupported*)GetProcAddress(interposer, "slIsFeatureSupported");
	slIsFeatureLoaded = (PFun_slIsFeatureLoaded*)GetProcAddress(interposer, "slIsFeatureLoaded");
	slSetFeatureLoaded = (PFun_slSetFeatureLoaded*)GetProcAddress(interposer, "slSetFeatureLoaded");
	slEvaluateFeature = (PFun_slEvaluateFeature*)GetProcAddress(interposer, "slEvaluateFeature");
	slAllocateResources = (PFun_slAllocateResources*)GetProcAddress(interposer, "slAllocateResources");
	slFreeResources = (PFun_slFreeResources*)GetProcAddress(interposer, "slFreeResources");
	slSetTagForFrame = (PFun_slSetTagForFrame*)GetProcAddress(interposer, "slSetTagForFrame");
	slGetFeatureRequirements = (PFun_slGetFeatureRequirements*)GetProcAddress(interposer, "slGetFeatureRequirements");
	slGetFeatureVersion = (PFun_slGetFeatureVersion*)GetProcAddress(interposer, "slGetFeatureVersion");
	slUpgradeInterface = (PFun_slUpgradeInterface*)GetProcAddress(interposer, "slUpgradeInterface");
	slSetConstants = (PFun_slSetConstants*)GetProcAddress(interposer, "slSetConstants");
	slGetNativeInterface = (PFun_slGetNativeInterface*)GetProcAddress(interposer, "slGetNativeInterface");
	slGetFeatureFunction = (PFun_slGetFeatureFunction*)GetProcAddress(interposer, "slGetFeatureFunction");
	slGetNewFrameToken = (PFun_slGetNewFrameToken*)GetProcAddress(interposer, "slGetNewFrameToken");
	slSetD3DDevice = (PFun_slSetD3DDevice*)GetProcAddress(interposer, "slSetD3DDevice");

	interposerLoaded = slInit && slSetTagForFrame && slEvaluateFeature && slGetNewFrameToken && slSetConstants;
	if (!interposerLoaded)
		logger::error("[Streamline] Interposer is missing required exports (SDK 2.12 or newer required)");
}

void Streamline::Initialize(sl::RenderAPI a_renderAPI)
{
	if (!interposerLoaded)
		LoadInterposer();
	if (!interposerLoaded)
		return;

	if (initialized) {
		if (initializedRenderAPI != a_renderAPI)
			logger::warn("[Streamline] Already initialized for render API {}; cannot switch to {}", static_cast<uint32_t>(initializedRenderAPI), static_cast<uint32_t>(a_renderAPI));
		return;
	}

	const bool d3d12 = a_renderAPI == sl::RenderAPI::eD3D12;
	logger::info("[Streamline] Initializing Streamline for {}", d3d12 ? "D3D12" : "D3D11");
	// The unlock follows the interposer's own LoadLibrary calls from here on, so the DLSS-G plugin
	// and provider are inspected as they load rather than on the next rescan.
	if (d3d12 && MfgUnlock::IsEnabled())
		MfgUnlock::InstallLoaderDiscovery(interposer);

	sl::Preferences pref;

	sl::Feature d3d11Features[] = { sl::kFeatureDLSS, sl::kFeatureReflex, sl::kFeaturePCL };
	sl::Feature d3d12Features[] = { sl::kFeatureDLSS, sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL };
	sl::Feature d3d12FeaturesNoFG[] = { sl::kFeatureDLSS, sl::kFeatureReflex, sl::kFeaturePCL };

	if (d3d12 && dlssgModulePresent) {
		pref.featuresToLoad = d3d12Features;
		pref.numFeaturesToLoad = _countof(d3d12Features);
	} else if (d3d12) {
		pref.featuresToLoad = d3d12FeaturesNoFG;
		pref.numFeaturesToLoad = _countof(d3d12FeaturesNoFG);
	} else {
		pref.featuresToLoad = d3d11Features;
		pref.numFeaturesToLoad = _countof(d3d11Features);
	}

	// Set log level from settings
	switch (globals::features::upscaling.settings.streamlineLogLevel) {
	case 2:
		pref.logLevel = sl::LogLevel::eVerbose;
		break;
	case 1:
		pref.logLevel = sl::LogLevel::eDefault;
		break;
	case 0:
	default:
		pref.logLevel = sl::LogLevel::eOff;
		break;
	}
	pref.logMessageCallback = LoggingCallback;
	pref.showConsole = false;

	static const wchar_t* pluginPaths[1]{};
	pluginPaths[0] = pluginDirAbsolute.c_str();
	pref.pathsToPlugins = pluginPaths;
	pref.numPathsToPlugins = 1;

	pref.engine = sl::EngineType::eCustom;
	pref.engineVersion = "1.0.0";
	pref.projectId = "f8776929-c969-43bd-ac2b-294b4de58aac";

	pref.renderAPI = a_renderAPI;
	pref.flags = sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;

	if (SL_FAILED(res, slInit(pref, sl::kSDKVersion))) {
		logger::critical("[Streamline] Failed to initialize Streamline: {}", magic_enum::enum_name(res));
		return;
	}

	initialized = true;
	initializedRenderAPI = a_renderAPI;
	ResetRuntimeState();
	logger::info("[Streamline] Successfully initialized Streamline ({})", d3d12 ? "D3D12" : "D3D11");

	if (d3d12 && dlssgModulePresent && MfgUnlock::IsEnabled()) {
		// slInit loaded sl.dlss_g.dll; the NGX provider is loaded lazily by Streamline, so the unlock
		// pulls it in now to get its patch in before the first feature creation.
		MfgUnlock::PreloadProvider(pluginDirAbsolute);
		MfgUnlock::Rescan(true);
	}
}

void Streamline::ResetRuntimeState()
{
	featureDLSS = false;
	featureDLSSG = false;
	featureReflex = false;
	featurePCL = false;
	reflexSupportedOnCurrentAdapter = false;
	reflexOptionsCache = {};
	lastReflexSleepFrame = UINT32_MAX;
	dlssgActive = false;
	dlssgStateKnown = false;
	maxFramesToGenerate = 1;
	dynamicMFGSupported = false;
	mfgUnlockReady = false;
	dlssgPresentedFrames = 0;
	dlssgLastStatus = 0;
	dlssgPresentSafetyFrames = 0;
	lastDLSSGStateQueryFrame = UINT32_MAX;
	lastDLSSGLoggedStatus = UINT32_MAX;
	lastDLSSGLoggedPresented = UINT32_MAX;
	loggedDynamicMFGUnsupported = false;
	dlssgOptionsCache = {};
	d3d12DLSSOptionsCache = {};
	frameToken = nullptr;
	presentFrameToken = nullptr;
	constantsFrameIndex = UINT32_MAX;
}

void Streamline::CheckFeatures(IDXGIAdapter* a_adapter)
{
	if (!initialized || !a_adapter)
		return;

	logger::info("[Streamline] Checking features");
	DXGI_ADAPTER_DESC adapterDesc;
	a_adapter->GetDesc(&adapterDesc);
	reflexSupportedOnCurrentAdapter = adapterDesc.VendorId == NVIDIA_VENDOR_ID;

	sl::AdapterInfo adapterInfo;
	adapterInfo.deviceLUID = (uint8_t*)&adapterDesc.AdapterLuid;
	adapterInfo.deviceLUIDSizeInBytes = sizeof(LUID);

	auto checkFeatureAvailability = [&](sl::Feature feature, const char* featureName, bool& outAvailable) {
		outAvailable = false;
		bool loaded = false;
		if (SL_FAILED(result, slIsFeatureLoaded(feature, loaded))) {
			logger::warn("[Streamline] {} load-state query failed: {}", featureName, magic_enum::enum_name(result));
			return;
		}
		if (!loaded) {
			logger::info("[Streamline] {} feature is not loaded", featureName);
			sl::FeatureRequirements featureRequirements;
			sl::Result requirementsResult = slGetFeatureRequirements(feature, featureRequirements);
			if (requirementsResult != sl::Result::eOk) {
				logger::info("[Streamline] {} feature failed to load due to: {}", featureName, magic_enum::enum_name(requirementsResult));
			}
			return;
		}

		logger::info("[Streamline] {} feature is loaded", featureName);
		const auto support = slIsFeatureSupported(feature, adapterInfo);
		outAvailable = support == sl::Result::eOk;
		if (!outAvailable)
			logger::info("[Streamline] {} feature is not supported on this adapter: {}", featureName, magic_enum::enum_name(support));
	};

	checkFeatureAvailability(sl::kFeatureDLSS, "DLSS", featureDLSS);
	if (UsesD3D12() && dlssgModulePresent) {
		checkFeatureAvailability(sl::kFeatureDLSS_G, "DLSS-G", featureDLSSG);
	} else {
		featureDLSSG = false;
		if (UsesD3D12())
			logger::info("[Streamline] DLSS-G skipped: runtime not present in the Streamline folder");
	}
	if (reflexSupportedOnCurrentAdapter) {
		checkFeatureAvailability(sl::kFeatureReflex, "Reflex", featureReflex);
		checkFeatureAvailability(sl::kFeaturePCL, "PCL", featurePCL);
	} else {
		featureReflex = false;
		featurePCL = false;
	}

	if (featureDLSS) {
		isRTXBelow40series = IsRTXAndBelow40Series(a_adapter);

		if (isRTXBelow40series)
			logger::info("[Streamline] Older RTX GPU detected, DLSS 4.0 presets will be used instead of DLSS 4.5");
		else
			logger::info("[Streamline] Newer RTX GPU detected, DLSS 4.5 presets will be used");
	}

	logger::info("[Streamline] DLSS {} available", featureDLSS ? "is" : "is not");
	logger::info("[Streamline] DLSS-G {} available", featureDLSSG ? "is" : "is not");
	if (reflexSupportedOnCurrentAdapter) {
		logger::info("[Streamline] Reflex {} available", featureReflex ? "is" : "is not");
		logger::info("[Streamline] PCL {} available", featurePCL ? "is" : "is not");
	} else {
		logger::info("[Streamline] Reflex/PCL disabled on non-NVIDIA adapter");
	}
	reflexOptionsCache = {};
	lastReflexSleepFrame = UINT32_MAX;
}

void Streamline::PostDevice()
{
	if (!initialized)
		return;

	// Hook up all of the feature functions using the sl function slGetFeatureFunction

	if (featureDLSS) {
		slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", (void*&)slDLSSGetOptimalSettings);
		slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSGetState", (void*&)slDLSSGetState);
		slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", (void*&)slDLSSSetOptions);
	}

	slDLSSGGetState = nullptr;
	slDLSSGSetOptions = nullptr;
	if (featureDLSSG) {
		slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", (void*&)slDLSSGGetState);
		slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", (void*&)slDLSSGSetOptions);
		if (!slDLSSGGetState || !slDLSSGSetOptions) {
			logger::warn("[Streamline] DLSS-G functions are missing; frame generation will be unavailable");
			featureDLSSG = false;
		} else if (MfgUnlock::IsEnabled()) {
			// The module owning this pointer is the live DLSS-G wrapper the unlock has to patch.
			MfgUnlock::ObserveDlssgFunction(reinterpret_cast<void*>(slDLSSGSetOptions));
			MfgUnlock::Rescan(true);
		}
	}

	slReflexGetState = nullptr;
	slReflexSleep = nullptr;
	slReflexSetOptions = nullptr;
	slPCLSetMarker = nullptr;
	featureReflex = false;
	featurePCL = false;

	if (slGetFeatureFunction && reflexSupportedOnCurrentAdapter) {
		if (slSetFeatureLoaded) {
			// Reflex/PCL availability can change after device bind; request explicit load here.
			const auto requestFeatureLoad = [&](sl::Feature feature, const char* featureName) {
				const sl::Result loadResult = slSetFeatureLoaded(feature, true);
				if (loadResult != sl::Result::eOk)
					logger::warn("[Streamline] Failed to request {} load: {}", featureName, magic_enum::enum_name(loadResult));
			};

			requestFeatureLoad(sl::kFeatureReflex, "Reflex");
			requestFeatureLoad(sl::kFeaturePCL, "PCL");
		}

		const auto bindFeatureFn = [&](sl::Feature feature, const char* functionName, void*& fn) {
			fn = nullptr;
			const sl::Result bindResult = slGetFeatureFunction(feature, functionName, fn);
			if (bindResult != sl::Result::eOk)
				logger::warn("[Streamline] {} bind failed with {}", functionName, magic_enum::enum_name(bindResult));
			return bindResult == sl::Result::eOk && fn != nullptr;
		};

		// Keep runtime controls strict: only advertise Reflex/PCL as available when required entry points bind.
		bool reflexFnsBound = true;
		reflexFnsBound &= bindFeatureFn(sl::kFeatureReflex, "slReflexGetState", (void*&)slReflexGetState);
		reflexFnsBound &= bindFeatureFn(sl::kFeatureReflex, "slReflexSleep", (void*&)slReflexSleep);
		reflexFnsBound &= bindFeatureFn(sl::kFeatureReflex, "slReflexSetOptions", (void*&)slReflexSetOptions);
		featureReflex = reflexFnsBound && slReflexSetOptions && slReflexSleep;

		if (!featureReflex) {
			logger::warn("[Streamline] Reflex functions are missing; Reflex runtime controls will be disabled");
		} else {
			logger::info("[Streamline] Reflex runtime controls are available");
		}

		bool pclFnBound = bindFeatureFn(sl::kFeaturePCL, "slPCLSetMarker", (void*&)slPCLSetMarker);
		featurePCL = pclFnBound && slPCLSetMarker;
		if (!featurePCL) {
			logger::warn("[Streamline] PCL marker function is unavailable; latency markers will not be emitted");
		} else {
			logger::info("[Streamline] PCL marker interface is available");
			slPCLGetState = nullptr;
			slPCLSetOptions = nullptr;
			slGetFeatureFunction(sl::kFeaturePCL, "slPCLGetState", (void*&)slPCLGetState);
			slGetFeatureFunction(sl::kFeaturePCL, "slPCLSetOptions", (void*&)slPCLSetOptions);

			if (slPCLSetOptions) {
				sl::PCLOptions options{};
				// Leave idThread unset so PCL posts the stats message to the foreground game
				// window; setting it makes Streamline use PostThreadMessageW, bypassing the WndProc hook.
				options.idThread = 0;
				if (SL_FAILED(result, slPCLSetOptions(options)))
					logger::warn("[Streamline] Could not set PCL options: {}", magic_enum::enum_name(result));
			}

			pclStatsWindowMessage = 0;
			if (slPCLGetState) {
				sl::PCLState state{};
				if (SL_SUCCEEDED(result, slPCLGetState(state)))
					pclStatsWindowMessage = state.statsWindowMessage;
			}
			if (pclStatsWindowMessage == 0)
				pclStatsWindowMessage = RegisterWindowMessageW(L"PC_Latency_Stats_Ping");
			logger::info("[Streamline] PCL stats message id {}", pclStatsWindowMessage);
		}
	} else if (!reflexSupportedOnCurrentAdapter) {
		logger::info("[Streamline] Skipping Reflex/PCL binding on non-NVIDIA adapter");
	}

	if (featureDLSSG && !featureReflex)
		logger::warn("[Streamline] DLSS-G requires Reflex; frame generation will report ReflexNotDetected until Reflex binds");

	reflexOptionsCache = {};
	lastReflexSleepFrame = UINT32_MAX;
}

bool Streamline::EnsureFrameToken()
{
	if (!initialized || !slGetNewFrameToken || !globals::state)
		return false;

	if (!frameChecker.IsNewFrame())
		return frameToken != nullptr;

	if (SL_FAILED(result, slGetNewFrameToken(frameToken, &globals::state->frameCount))) {
		logger::error("[Streamline] Could not get frame token: {}", magic_enum::enum_name(result));
		frameToken = nullptr;
		return false;
	}

	return frameToken != nullptr;
}

sl::FrameToken* Streamline::AcquireFrameToken(uint32_t a_frameIndex)
{
	if (!initialized || !slGetNewFrameToken)
		return nullptr;

	sl::FrameToken* token = nullptr;
	if (SL_FAILED(result, slGetNewFrameToken(token, &a_frameIndex))) {
		logger::error("[Streamline] Could not get frame token for frame {}: {}", a_frameIndex, magic_enum::enum_name(result));
		return nullptr;
	}
	return token;
}

void Streamline::SetPCLMarker(sl::PCLMarker a_marker, sl::FrameToken* a_token)
{
	auto* token = a_token ? a_token : frameToken;
	if (!emitPCLMarkers || !featurePCL || !slPCLSetMarker || !token)
		return;

	if (SL_FAILED(result, slPCLSetMarker(a_marker, *token))) {
		static bool loggedFailure = false;
		if (!loggedFailure) {
			loggedFailure = true;
			logger::warn("[Streamline] slPCLSetMarker({}) failed: {}", static_cast<uint32_t>(a_marker), magic_enum::enum_name(result));
		}
	}
}

bool Streamline::CheckFrameConstants(sl::ViewportHandle p_viewport)
{
	if (!initialized || !globals::state)
		return false;

	if (!EnsureFrameToken())
		return false;

	// Common constants go in once per frame: Streamline rejects a second set for the same
	// token (eErrorDuplicatedConstants) and the DLSS-G guide forbids it anyway.
	const uint32_t frameIndex = globals::state->frameCount;
	if (constantsFrameIndex == frameIndex)
		return true;

	return SetCommonConstants(*frameToken, frameIndex, p_viewport);
}

bool Streamline::PrepareDLSSGPresent()
{
	if (!initialized || !globals::state)
		return false;

	// The present hook runs State::Reset() (frameCount++) before the proxy presents, so the
	// frame on its way out is frameCount - 1. Its token must carry the constants and the
	// DLSS-G tags; the render-side token for the new count belongs to the next frame.
	const uint32_t frameCount = globals::state->frameCount;
	const uint32_t presentIndex = frameCount ? frameCount - 1 : 0;
	presentFrameToken = AcquireFrameToken(presentIndex);
	if (!presentFrameToken)
		return false;

	if (constantsFrameIndex == presentIndex)
		return true;

	// The upscaler did not run this frame (TAA/FSR, or a menu); DLSS-G still needs the camera.
	return SetCommonConstants(*presentFrameToken, presentIndex, viewport);
}

bool Streamline::SetCommonConstants(sl::FrameToken& a_token, uint32_t a_frameIndex, sl::ViewportHandle p_viewport)
{
	sl::Constants slConstants = {};

	slConstants.cameraAspectRatio = (float)globals::game::graphicsState->screenWidth / (float)globals::game::graphicsState->screenHeight;

	slConstants.cameraFOV = Util::GetVerticalFOVRad();
	slConstants.cameraNear = *globals::game::cameraNear;
	slConstants.cameraFar = *globals::game::cameraFar;

	auto viewMatrix = globals::game::frameBufferCached.GetCameraViewInverse().Transpose();
	auto cameraViewToClip = globals::game::frameBufferCached.GetCameraProjUnjittered().Transpose();

	slConstants.cameraMotionIncluded = sl::Boolean::eTrue;
	slConstants.cameraPinholeOffset = { 0.f, 0.f };
	slConstants.cameraRight = { viewMatrix._11, viewMatrix._12, viewMatrix._13 };
	slConstants.cameraUp = { viewMatrix._21, viewMatrix._22, viewMatrix._23 };
	slConstants.cameraFwd = { viewMatrix._31, viewMatrix._32, viewMatrix._33 };
	slConstants.cameraPos = *(sl::float3*)&globals::game::frameBufferCached.GetCameraPosAdjust();
	slConstants.cameraViewToClip = *(sl::float4x4*)&cameraViewToClip;
	slConstants.depthInverted = sl::Boolean::eFalse;

	recalculateCameraMatrices(slConstants);

	auto& upscaling = globals::features::upscaling;
	auto jitter = upscaling.jitter;
	slConstants.jitterOffset = { -jitter.x, -jitter.y };
	slConstants.reset = sl::Boolean::eFalse;

	slConstants.mvecScale = { 1.0f, 1.0f };
	slConstants.motionVectors3D = sl::Boolean::eFalse;
	slConstants.motionVectorsInvalidValue = FLT_MIN;
	slConstants.orthographicProjection = sl::Boolean::eFalse;
	slConstants.motionVectorsDilated = sl::Boolean::eFalse;
	slConstants.motionVectorsJittered = sl::Boolean::eFalse;

	if (SL_FAILED(res, slSetConstants(slConstants, a_token, p_viewport))) {
		if (res == sl::Result::eErrorDuplicatedConstants) {
			constantsFrameIndex = a_frameIndex;
			return true;
		}
		static uint32_t loggedFailures = 0;
		if (loggedFailures < 8) {
			++loggedFailures;
			logger::error("[Streamline] Could not set constants for frame {}: {}", a_frameIndex, magic_enum::enum_name(res));
		}
		return false;
	}

	constantsFrameIndex = a_frameIndex;
	return true;
}

bool Streamline::IsRTXAndBelow40Series(IDXGIAdapter* a_adapter)
{
	DXGI_ADAPTER_DESC adapterDesc = {};

	a_adapter->GetDesc(&adapterDesc);

	UINT vendorId = adapterDesc.VendorId;
	UINT deviceId = adapterDesc.DeviceId;

	// Check if NVIDIA
	if (vendorId != 0x10DE)
		return false;

	// RTX 30 series (Ampere) - 0x2200-0x25FF
	if (deviceId >= 0x2200 && deviceId <= 0x2600)
		return true;

	// RTX 20 series (Turing with RT cores) - 0x1E00-0x1FFF
	if (deviceId >= 0x1E00 && deviceId <= 0x1FFF)
		return true;

	return false;
}

void Streamline::BuildDLSSOptions(sl::DLSSOptions& dlssOptions, uint32_t a_outputWidth, uint32_t a_outputHeight, bool a_hdr) const
{
	// Map quality mode to DLSS mode
	uint32_t qualityMode = globals::features::upscaling.settings.qualityMode;
	switch (qualityMode) {
	case 1:
		dlssOptions.mode = sl::DLSSMode::eMaxQuality;
		break;
	case 2:
		dlssOptions.mode = sl::DLSSMode::eBalanced;
		break;
	case 3:
		dlssOptions.mode = sl::DLSSMode::eMaxPerformance;
		break;
	case 4:
		dlssOptions.mode = sl::DLSSMode::eUltraPerformance;
		break;
	default:
		dlssOptions.mode = sl::DLSSMode::eDLAA;
		break;
	}

	dlssOptions.outputWidth = a_outputWidth;
	dlssOptions.outputHeight = a_outputHeight;
	dlssOptions.colorBuffersHDR = a_hdr ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	dlssOptions.useAutoExposure = sl::Boolean::eTrue;

	std::optional<sl::DLSSPreset> customPreset;
	switch (globals::features::upscaling.settings.presetDLSS) {
	case 1:
		customPreset = sl::DLSSPreset::ePresetJ;
		break;
	case 2:
		customPreset = sl::DLSSPreset::ePresetK;
		break;
	case 3:
		customPreset = sl::DLSSPreset::ePresetL;
		break;
	case 4:
		customPreset = sl::DLSSPreset::ePresetM;
		break;
	}

	if (customPreset.has_value()) {
		dlssOptions.dlaaPreset = customPreset.value();
		dlssOptions.ultraQualityPreset = customPreset.value();
		dlssOptions.qualityPreset = customPreset.value();
		dlssOptions.balancedPreset = customPreset.value();
		dlssOptions.performancePreset = customPreset.value();
		dlssOptions.ultraPerformancePreset = customPreset.value();
	} else if (isRTXBelow40series) {
		dlssOptions.dlaaPreset = sl::DLSSPreset::ePresetJ;
		dlssOptions.ultraQualityPreset = sl::DLSSPreset::ePresetJ;
		dlssOptions.qualityPreset = sl::DLSSPreset::ePresetJ;
		dlssOptions.balancedPreset = sl::DLSSPreset::ePresetJ;
		dlssOptions.performancePreset = sl::DLSSPreset::ePresetJ;
		dlssOptions.ultraPerformancePreset = sl::DLSSPreset::ePresetM;
	} else {
		// DLSS 4.5 recommendations: K for DLAA/Quality/Balanced, M for Performance, L for Ultra Performance
		dlssOptions.dlaaPreset = sl::DLSSPreset::ePresetK;
		dlssOptions.ultraQualityPreset = sl::DLSSPreset::ePresetK;
		dlssOptions.qualityPreset = sl::DLSSPreset::ePresetK;
		dlssOptions.balancedPreset = sl::DLSSPreset::ePresetK;
		dlssOptions.performancePreset = sl::DLSSPreset::ePresetM;
		dlssOptions.ultraPerformancePreset = sl::DLSSPreset::ePresetL;
	}

	dlssOptions.preExposure = 1.0f;
}

void Streamline::SetDLSSOptions(sl::ViewportHandle p_viewport, uint32_t width)
{
	sl::DLSSOptions dlssOptions{};

	// Detect HDR from kMAIN format at runtime
	bool isHDR = false;
	{
		auto renderer = globals::game::renderer;
		auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		D3D11_TEXTURE2D_DESC mainDesc;
		static_cast<ID3D11Texture2D*>(main.texture)->GetDesc(&mainDesc);
		isHDR = mainDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM;
	}

	BuildDLSSOptions(dlssOptions, width, (uint)globals::game::graphicsState->screenHeight, isHDR);

	if (SL_FAILED(result, slDLSSSetOptions(p_viewport, dlssOptions))) {
		logger::critical("[Streamline] Could not enable DLSS");
	}
}

bool Streamline::EnsureD3D12DLSSOptions(uint32_t a_outputWidth, uint32_t a_outputHeight, bool a_hdr)
{
	sl::DLSSOptions dlssOptions{};
	BuildDLSSOptions(dlssOptions, a_outputWidth, a_outputHeight, a_hdr);

	const uint32_t preset = globals::features::upscaling.settings.presetDLSS;
	if (d3d12DLSSOptionsCache.valid &&
		d3d12DLSSOptionsCache.mode == dlssOptions.mode &&
		d3d12DLSSOptionsCache.outputWidth == a_outputWidth &&
		d3d12DLSSOptionsCache.outputHeight == a_outputHeight &&
		d3d12DLSSOptionsCache.preset == preset &&
		d3d12DLSSOptionsCache.hdr == a_hdr) {
		return true;
	}

	if (SL_FAILED(result, slDLSSSetOptions(viewport, dlssOptions))) {
		logger::warn("[Streamline] Could not set D3D12 DLSS options: {}", magic_enum::enum_name(result));
		return false;
	}

	d3d12DLSSOptionsCache.valid = true;
	d3d12DLSSOptionsCache.mode = dlssOptions.mode;
	d3d12DLSSOptionsCache.outputWidth = a_outputWidth;
	d3d12DLSSOptionsCache.outputHeight = a_outputHeight;
	d3d12DLSSOptionsCache.preset = preset;
	d3d12DLSSOptionsCache.hdr = a_hdr;
	return true;
}

void Streamline::EvaluateDLSS(sl::ViewportHandle vp,
	ID3D11Resource* colorIn, ID3D11Resource* colorOut, ID3D11Resource* depth,
	ID3D11Resource* mvec, ID3D11Resource* reactiveMask, ID3D11Resource* transparencyMask,
	const sl::Extent& extentIn, const sl::Extent& extentOut, uint32_t outputWidth)
{
	auto context = globals::d3d::context;

	sl::Resource colorInRes = { sl::ResourceType::eTex2d, colorIn, 0 };
	sl::Resource colorOutRes = { sl::ResourceType::eTex2d, colorOut, 0 };
	sl::Resource depthRes = { sl::ResourceType::eTex2d, depth, 0 };
	sl::Resource mvecRes = { sl::ResourceType::eTex2d, mvec, 0 };
	sl::Resource reactiveMaskRes = { sl::ResourceType::eTex2d, reactiveMask, 0 };
	sl::Resource transparencyMaskRes = { sl::ResourceType::eTex2d, transparencyMask, 0 };

	if (!CheckFrameConstants(vp))
		return;

	SetDLSSOptions(vp, outputWidth);

	sl::ResourceTag tags[] = {
		{ &colorInRes, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow, &extentIn },
		{ &colorOutRes, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow, &extentOut },
		{ &depthRes, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &extentIn },
		{ &mvecRes, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &extentIn },
		{ &reactiveMaskRes, sl::kBufferTypeBiasCurrentColorHint, sl::ResourceLifecycle::eValidUntilPresent, &extentIn },
		{ &transparencyMaskRes, sl::kBufferTypeTransparencyHint, sl::ResourceLifecycle::eValidUntilPresent, &extentIn }
	};

	uint32_t numTags = _countof(tags);
	if (!reactiveMask || !transparencyMask)
		numTags -= 2;

	if (SL_FAILED(tagResult, slSetTagForFrame(*frameToken, vp, tags, numTags, context))) {
		static bool tagErrorLogged = false;
		if (!tagErrorLogged) {
			tagErrorLogged = true;
			logger::error("[Streamline] slSetTagForFrame failed: {}", magic_enum::enum_name(tagResult));
		}
		return;
	}

	sl::ViewportHandle view(vp);
	const sl::BaseStructure* inputs[] = { &view };

	auto state = globals::state;
	if (state->frameAnnotations)
		state->BeginPerfEvent("DLSS Evaluate");

	sl::Result evalResult = slEvaluateFeature(sl::kFeatureDLSS, *frameToken, inputs, _countof(inputs), context);

	if (state->frameAnnotations)
		state->EndPerfEvent();

	if (evalResult != sl::Result::eOk) {
		static bool evalErrorLogged = false;
		if (!evalErrorLogged) {
			evalErrorLogged = true;
			logger::error("[Streamline] slEvaluateFeature failed result={}", magic_enum::enum_name(evalResult));
		}
	}
}

void Streamline::Upscale(ID3D11Resource* a_upscalingTexture, ID3D11Resource* a_reactiveMask, ID3D11Resource* a_transparencyCompositionMask, ID3D11Resource* a_motionVectors)
{
	auto renderer = globals::game::renderer;
	auto& depthTexture = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

	float2 screenSize{ (float)globals::game::graphicsState->screenWidth, (float)globals::game::graphicsState->screenHeight };
	auto renderSize = Util::ConvertToDynamic(screenSize);

	// DLSS input and output must not alias. Always write to the intermediate texture,
	// then either sharpen or copy the result back to kMAIN.
	auto& upscaling = globals::features::upscaling;
	ID3D11Resource* colorOut =
		upscaling.sharpenerTexture ? upscaling.sharpenerTexture->resource.get() : a_upscalingTexture;

	sl::Extent extentIn{ 0, 0, (uint)renderSize.x, (uint)renderSize.y };
	sl::Extent extentOut{ 0, 0, (uint)screenSize.x, (uint)screenSize.y };

	EvaluateDLSS(viewport,
		a_upscalingTexture, colorOut,
		depthTexture.texture, a_motionVectors, a_reactiveMask, a_transparencyCompositionMask,
		extentIn, extentOut, (uint)screenSize.x);
}

bool Streamline::UpscaleD3D12(ID3D12GraphicsCommandList* a_commandList,
	ID3D12Resource* a_colorIn, ID3D12Resource* a_colorOut, ID3D12Resource* a_depth,
	ID3D12Resource* a_motionVectors, ID3D12Resource* a_reactiveMask, ID3D12Resource* a_transparencyMask,
	uint32_t a_renderWidth, uint32_t a_renderHeight, uint32_t a_displayWidth, uint32_t a_displayHeight, bool a_hdr)
{
	if (!UsesD3D12() || !featureDLSS || !slDLSSSetOptions || !slEvaluateFeature || !slSetTagForFrame ||
		!a_commandList || !a_colorIn || !a_colorOut || !a_depth || !a_motionVectors) {
		return false;
	}

	if (!CheckFrameConstants(viewport))
		return false;

	if (!EnsureD3D12DLSSOptions(a_displayWidth, a_displayHeight, a_hdr))
		return false;

	sl::Extent extentIn{ 0, 0, a_renderWidth, a_renderHeight };
	sl::Extent extentOut{ 0, 0, a_displayWidth, a_displayHeight };

	sl::Resource colorInRes = { sl::ResourceType::eTex2d, a_colorIn, nullptr, nullptr, D3D12_RESOURCE_STATE_COMMON };
	sl::Resource colorOutRes = { sl::ResourceType::eTex2d, a_colorOut, nullptr, nullptr, D3D12_RESOURCE_STATE_COMMON };
	sl::Resource depthRes = { sl::ResourceType::eTex2d, a_depth, nullptr, nullptr, D3D12_RESOURCE_STATE_COMMON };
	sl::Resource mvecRes = { sl::ResourceType::eTex2d, a_motionVectors, nullptr, nullptr, D3D12_RESOURCE_STATE_COMMON };
	sl::Resource reactiveRes = { sl::ResourceType::eTex2d, a_reactiveMask, nullptr, nullptr, D3D12_RESOURCE_STATE_COMMON };
	sl::Resource transparencyRes = { sl::ResourceType::eTex2d, a_transparencyMask, nullptr, nullptr, D3D12_RESOURCE_STATE_COMMON };

	sl::ResourceTag tags[] = {
		{ &colorInRes, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow, &extentIn },
		{ &colorOutRes, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow, &extentOut },
		{ &depthRes, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &extentIn },
		{ &mvecRes, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &extentIn },
		{ &reactiveRes, sl::kBufferTypeBiasCurrentColorHint, sl::ResourceLifecycle::eOnlyValidNow, &extentIn },
		{ &transparencyRes, sl::kBufferTypeTransparencyHint, sl::ResourceLifecycle::eOnlyValidNow, &extentIn }
	};
	uint32_t numTags = _countof(tags);
	if (!a_reactiveMask || !a_transparencyMask)
		numTags -= 2;

	if (SL_FAILED(tagResult, slSetTagForFrame(*frameToken, viewport, tags, numTags, a_commandList))) {
		static bool tagErrorLogged = false;
		if (!tagErrorLogged) {
			tagErrorLogged = true;
			logger::error("[Streamline] D3D12 slSetTagForFrame failed: {}", magic_enum::enum_name(tagResult));
		}
		return false;
	}

	sl::ViewportHandle view(viewport);
	const sl::BaseStructure* inputs[] = { &view };

	const sl::Result evalResult = slEvaluateFeature(sl::kFeatureDLSS, *frameToken, inputs, _countof(inputs), a_commandList);
	if (evalResult != sl::Result::eOk) {
		static bool evalErrorLogged = false;
		if (!evalErrorLogged) {
			evalErrorLogged = true;
			logger::error("[Streamline] D3D12 slEvaluateFeature failed result={}", magic_enum::enum_name(evalResult));
		}
		return false;
	}

	return true;
}

void Streamline::UpdateReflex()
{
	if (!initialized || !reflexSupportedOnCurrentAdapter || !featureReflex || !slReflexSetOptions)
		return;

	auto& upscaling = globals::features::upscaling;
	auto& settings = upscaling.settings;

	// DLSS-G refuses to run without Reflex, so frame generation forces at least Low Latency.
	const bool forceReflexOn = upscaling.IsDlssFrameGenerationPathActive() && settings.frameGenerationMode != 0;

	sl::ReflexOptions options{};
	if (settings.reflexMode == 2)
		options.mode = sl::ReflexMode::eLowLatencyWithBoost;
	else if (settings.reflexMode == 1 || forceReflexOn)
		options.mode = sl::ReflexMode::eLowLatency;
	else
		options.mode = sl::ReflexMode::eOff;

	const float originalReflexFPSLimit = settings.reflexFPSLimit;
	float reflexFPSLimit = originalReflexFPSLimit;
	if (!std::isfinite(reflexFPSLimit)) {
		reflexFPSLimit = 60.0f;
		settings.reflexFPSLimit = reflexFPSLimit;
		logger::warn("[Streamline] reflexFPSLimit is not finite ({}), using {}", originalReflexFPSLimit, reflexFPSLimit);
	}
	float fpsLimit = settings.reflexUseFPSLimit ? std::clamp(reflexFPSLimit, 20.0f, 240.0f) : 0.0f;
	// With DLSS-G on and the frame limiter enabled, the rendered rate is capped so the presented
	// rate lands on the display's refresh rate: refresh / (generated + 1). Reflex sleeps before
	// input, which is where the reference implementation paces as well. Without this cap the
	// presented rate above 2x ran past the display (3x and 4x of a 50 to 60 fps render on a 164 Hz
	// panel) and DLSS-G had to hold and drop generated frames, which showed as jittery movement.
	// The frame generation FPS limit is a second, separate cap on the presented rate that only
	// applies while generation is on; the lowest enabled cap wins.
	if (upscaling.IsDlssFrameGenerationPathActive() && settings.frameGenerationMode != 0) {
		// The setting stores generated frames minus one (0 = 2x), and the presented count is generated + 1.
		const uint32_t generated = currentGeneratedFrames() ? currentGeneratedFrames() : settings.dlssgGeneratedFrames + 1u;
		const uint32_t presentedPerRendered = std::clamp(generated + 1u, 2u, 6u);
		double presentedCap = 0.0;
		if (settings.frameLimitMode && upscaling.refreshRate > 1.0)
			presentedCap = upscaling.refreshRate;
		if (settings.frameGenerationFPSLimitEnabled && std::isfinite(settings.frameGenerationFPSLimit)) {
			const double fgCap = std::clamp(settings.frameGenerationFPSLimit, 30.0f, 480.0f);
			presentedCap = presentedCap > 0.0 ? std::min(presentedCap, fgCap) : fgCap;
		}
		if (presentedCap > 0.0) {
			const float renderedCap = static_cast<float>(presentedCap / static_cast<double>(presentedPerRendered));
			fpsLimit = fpsLimit > 0.0f ? std::min(fpsLimit, renderedCap) : renderedCap;
			static uint32_t loggedPerRendered = 0;
			static double loggedPresentedCap = 0.0;
			if (loggedPerRendered != presentedPerRendered || loggedPresentedCap != presentedCap) {
				loggedPerRendered = presentedPerRendered;
				loggedPresentedCap = presentedCap;
				logger::info("[Streamline] Reflex caps the rendered rate at {:.1f} fps: {} presented per rendered frame for {:.1f} presented", renderedCap, presentedPerRendered, presentedCap);
			}
		}
	}
	options.frameLimitUs = fpsLimit > 0.0f ? static_cast<uint32_t>(std::lround(1000000.0 / static_cast<double>(fpsLimit))) : 0u;
	// Official SL Reflex guidance says to leave marker-based optimization disabled unless the
	// Reflex team advises otherwise. PCL markers are still emitted for latency reporting and
	// DLSS-G frame matching.
	options.useMarkersToOptimize = false;
	options.idThread = 0;

	if (!reflexOptionsCache.valid ||
		reflexOptionsCache.mode != options.mode ||
		reflexOptionsCache.frameLimitUs != options.frameLimitUs ||
		reflexOptionsCache.useMarkersToOptimize != options.useMarkersToOptimize) {
		if (SL_FAILED(result, slReflexSetOptions(options))) {
			logger::error("[Streamline] Failed to apply Reflex options: {}", magic_enum::enum_name(result));
		} else {
			reflexOptionsCache.valid = true;
			reflexOptionsCache.mode = options.mode;
			reflexOptionsCache.frameLimitUs = options.frameLimitUs;
			reflexOptionsCache.useMarkersToOptimize = options.useMarkersToOptimize;
			logger::info("[Streamline] Reflex mode {} frameLimitUs={}", static_cast<uint32_t>(options.mode), options.frameLimitUs);
		}
	}

	// Markers are what the Reflex latency report and DLSS-G frame pacing key off; emit them
	// whenever PCL is available, as the reference implementation does.
	emitPCLMarkers = featurePCL && slPCLSetMarker != nullptr;

	const uint32_t currentFrame = globals::state ? globals::state->frameCount : 0;
	// PollInputDevices can run more than once; sleep and markers happen once per frame token.
	if (lastReflexSleepFrame == currentFrame)
		return;

	if (!EnsureFrameToken())
		return;

	lastReflexSleepFrame = currentFrame;
	// The SDK wants Sleep before the simulation starts and SimulationStart right after it.
	if (slReflexSleep && (options.mode != sl::ReflexMode::eOff || options.frameLimitUs != 0)) {
		FrameCosts::AccumulatingScope sleepScope(FrameCosts::reflexSleepMs);
		if (SL_FAILED(result, slReflexSleep(*frameToken)))
			logger::warn("[Streamline] Reflex sleep call failed: {}", magic_enum::enum_name(result));
	}
	SetPCLMarker(sl::PCLMarker::eSimulationStart);
	SetPCLMarker(kInputSampleMarker);
	// SimulationEnd and RenderSubmitStart follow in OnRenderSubmitStart, when the game begins
	// submitting the player view; RenderSubmitEnd and the present markers are at Present.
}

void Streamline::OnRenderSubmitStart()
{
	const uint32_t currentFrame = globals::state ? globals::state->frameCount : 0;
	// Only after this frame's sleep and SimulationStart, and once per frame.
	if (!emitPCLMarkers || lastReflexSleepFrame != currentFrame || lastRenderSubmitFrame == currentFrame)
		return;
	lastRenderSubmitFrame = currentFrame;
	SetPCLMarker(sl::PCLMarker::eSimulationEnd);
	SetPCLMarker(sl::PCLMarker::eRenderSubmitStart);
}

void Streamline::OnPCLStatsPing()
{
	if (!featurePCL || !slPCLSetMarker || !slGetNewFrameToken || !globals::state)
		return;

	// The ping must be answered on a fresh token so it is not confused with the frame in flight.
	uint32_t nextFrameIndex = globals::state->frameCount + 1;
	sl::FrameToken* pingFrameToken = nullptr;
	if (SL_FAILED(res, slGetNewFrameToken(pingFrameToken, &nextFrameIndex)) || !pingFrameToken) {
		logger::warn("[Streamline] Could not get PCL ping frame token {}: {}", nextFrameIndex, magic_enum::enum_name(res));
		return;
	}

	if (SL_FAILED(res, slPCLSetMarker(sl::PCLMarker::ePCLatencyPing, *pingFrameToken))) {
		logger::warn("[Streamline] PCL ping marker failed: {}", magic_enum::enum_name(res));
		return;
	}
	++pclPingCount;
}

float Streamline::GetReflexLatencyMs()
{
	if (!featureReflex || !slReflexGetState)
		return 0.0f;

	sl::ReflexState state{};
	if (SL_FAILED(result, slReflexGetState(state)) || !state.latencyReportAvailable) {
		pclLatencyReportAvailable = false;
		return 0.0f;
	}
	pclLatencyReportAvailable = true;

	for (int i = static_cast<int>(sl::kReflexFrameReportCount) - 1; i >= 0; --i) {
		const auto& report = state.frameReport[i];
		if (report.frameID == 0 || report.inputSampleTime == 0 || report.presentEndTime <= report.inputSampleTime)
			continue;

		const auto latencyUs = report.presentEndTime - report.inputSampleTime;
		return static_cast<float>(static_cast<double>(latencyUs) / 1000.0);
	}

	return 0.0f;
}

/**
 * @brief Releases DLSS resources and disables DLSS for the current viewport.
 *
 * Sets the DLSS mode to off and frees all DLSS-related resources associated with the viewport.
 */
void Streamline::DestroyDLSSResources()
{
	d3d12DLSSOptionsCache = {};
	if (!initialized || !featureDLSS || !slDLSSSetOptions || !slFreeResources)
		return;

	sl::DLSSOptions dlssOptions{};
	dlssOptions.mode = sl::DLSSMode::eOff;

	slDLSSSetOptions(viewport, dlssOptions);
	slFreeResources(sl::kFeatureDLSS, viewport);
}

// ---------------------------------------------------------------------------------------------
// DLSS-G
// ---------------------------------------------------------------------------------------------

bool Streamline::UpdateDLSSG(bool a_enabled, uint32_t a_generatedFrames, bool a_dynamic, uint32_t a_dynamicTargetFPS,
	uint32_t a_renderWidth, uint32_t a_renderHeight, uint32_t a_displayWidth, uint32_t a_displayHeight,
	DXGI_FORMAT a_colorFormat, DXGI_FORMAT a_mvecFormat, DXGI_FORMAT a_depthFormat, uint32_t a_backBuffers)
{
	if (!featureDLSSG || !slDLSSGSetOptions) {
		dlssgActive = false;
		return false;
	}

	// RTX 40 MFG unlock. nvngx_dlssg.dll can load lazily, so keep inspecting until every patch is in
	// place; once it is, the wrapper reports a higher numFramesToGenerateMax and the state is re-read.
	if (MfgUnlock::IsEnabled()) {
		MfgUnlock::Rescan();
		const uint32_t unlockMaxFrames = MfgUnlock::MaximumGeneratedFrames();
		if ((unlockMaxFrames > 0) != mfgUnlockReady) {
			mfgUnlockReady = unlockMaxFrames > 0;
			dlssgStateKnown = false;
			logger::info("[Streamline] RTX 40 MFG unlock {}", mfgUnlockReady ? "active" : "inactive");
			if (mfgUnlockReady && dlssgActive)
				logger::warn("[Streamline] DLSS-G was created before the unlock finished; restart the game for more than one generated frame");
		}
		// The wrapper's compiled limit (1, 3 or 5) caps what the runtime may be asked for, whatever
		// the patched state query advertises.
		if (mfgUnlockReady && unlockMaxFrames)
			maxFramesToGenerate = std::min(maxFramesToGenerate, unlockMaxFrames);
	}

	if (!dlssgStateKnown)
		QueryDLSSGState("options", true);

	const bool hasSizes = a_renderWidth && a_renderHeight && a_displayWidth && a_displayHeight;
	sl::DLSSGMode mode = sl::DLSSGMode::eOff;
	if (a_enabled && hasSizes)
		mode = a_dynamic ? sl::DLSSGMode::eDynamic : sl::DLSSGMode::eOn;

	// Dynamic MFG is a Blackwell feature. With the RTX 40 unlock the patched runtime reports it as
	// supported, but every DLSS-G present is then rejected with DXGI_ERROR_INVALID_CALL, so an Ada
	// card always runs the fixed multiplier.
	if (mode == sl::DLSSGMode::eDynamic && MfgUnlock::IsEnabled()) {
		if (!loggedDynamicMFGUnlockBlocked) {
			logger::warn("[Streamline] Dynamic MFG is not available on an RTX 40 card through the unlock; using the fixed multiplier");
			loggedDynamicMFGUnlockBlocked = true;
		}
		mode = sl::DLSSGMode::eOn;
	}

	if (mode == sl::DLSSGMode::eDynamic && !dynamicMFGSupported) {
		if (!loggedDynamicMFGUnsupported) {
			logger::warn("[Streamline] Dynamic MFG requested but the runtime reports it unsupported; using a fixed multiplier");
			loggedDynamicMFGUnsupported = true;
		}
		mode = sl::DLSSGMode::eOn;
	}

	if (mode == sl::DLSSGMode::eOff) {
		DisableDLSSG();
		return true;
	}

	// Switching between the dynamic and fixed modes reuses nothing: resources retained by the old
	// mode leave the runtime rejecting presents for several attempts, so release them first.
	if (dlssgOptionsCache.valid && dlssgOptionsCache.mode != sl::DLSSGMode::eOff && dlssgOptionsCache.mode != mode)
		DisableDLSSG(true);

	// Clamp to what the runtime reports, never to what the unlock believes: a request above the
	// wrapper's maximum is rejected with eErrorInvalidState and frame generation stays off entirely.
	const uint32_t generatedFrames = std::clamp<uint32_t>(a_generatedFrames, 1u, std::max<uint32_t>(1u, maxFramesToGenerate));
	const uint32_t dynamicTargetFPS = mode == sl::DLSSGMode::eDynamic ? a_dynamicTargetFPS : 0u;
	const uint32_t backBuffers = a_backBuffers ? a_backBuffers : 2u;

	if (dlssgOptionsCache.valid &&
		dlssgOptionsCache.mode == mode &&
		dlssgOptionsCache.generatedFrames == generatedFrames &&
		dlssgOptionsCache.dynamicTargetFPS == dynamicTargetFPS &&
		dlssgOptionsCache.renderWidth == a_renderWidth &&
		dlssgOptionsCache.renderHeight == a_renderHeight &&
		dlssgOptionsCache.displayWidth == a_displayWidth &&
		dlssgOptionsCache.displayHeight == a_displayHeight &&
		dlssgOptionsCache.colorFormat == a_colorFormat &&
		dlssgOptionsCache.mvecFormat == a_mvecFormat &&
		dlssgOptionsCache.depthFormat == a_depthFormat &&
		dlssgOptionsCache.backBuffers == backBuffers) {
		dlssgActive = true;
		dlssgPresentSafetyFrames = 0;
		return true;
	}

	sl::DLSSGOptions options{};
	options.mode = mode;
	options.numFramesToGenerate = generatedFrames;
	options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff | sl::DLSSGFlags::eEnableFullscreenMenuDetection;
	options.dynamicTargetFrameRate = static_cast<float>(dynamicTargetFPS);
	options.numBackBuffers = backBuffers;
	options.mvecDepthWidth = a_renderWidth;
	options.mvecDepthHeight = a_renderHeight;
	options.colorWidth = a_displayWidth;
	options.colorHeight = a_displayHeight;
	options.colorBufferFormat = static_cast<uint32_t>(a_colorFormat);
	options.mvecBufferFormat = static_cast<uint32_t>(a_mvecFormat);
	options.depthBufferFormat = static_cast<uint32_t>(a_depthFormat);
	options.hudLessBufferFormat = static_cast<uint32_t>(a_colorFormat);
	options.uiBufferFormat = 0;
	// No UI color/alpha tag is provided: the runtime derives the UI from the difference between
	// the presented back buffer and the HUD-less color it is handed.
	options.enableUserInterfaceRecomposition = sl::Boolean::eTrue;
	options.onErrorCallback = DLSSGAPIErrorCallback;

	if (SL_FAILED(result, slDLSSGSetOptions(viewport, options))) {
		logger::warn("[Streamline] Could not set DLSS-G mode {} frames={}: {}", static_cast<uint32_t>(mode), generatedFrames, magic_enum::enum_name(result));
		dlssgActive = false;
		dlssgOptionsCache.valid = false;
		return false;
	}

	logger::info("[Streamline] DLSS-G options applied: mode={} generatedFrames={} dynamicTargetFPS={} render={}x{} display={}x{} formats color={} mvec={} depth={}",
		static_cast<uint32_t>(mode), generatedFrames, dynamicTargetFPS, a_renderWidth, a_renderHeight, a_displayWidth, a_displayHeight,
		static_cast<uint32_t>(a_colorFormat), static_cast<uint32_t>(a_mvecFormat), static_cast<uint32_t>(a_depthFormat));

	dlssgOptionsCache.valid = true;
	dlssgOptionsCache.mode = mode;
	dlssgOptionsCache.generatedFrames = generatedFrames;
	dlssgOptionsCache.dynamicTargetFPS = dynamicTargetFPS;
	dlssgOptionsCache.renderWidth = a_renderWidth;
	dlssgOptionsCache.renderHeight = a_renderHeight;
	dlssgOptionsCache.displayWidth = a_displayWidth;
	dlssgOptionsCache.displayHeight = a_displayHeight;
	dlssgOptionsCache.colorFormat = a_colorFormat;
	dlssgOptionsCache.mvecFormat = a_mvecFormat;
	dlssgOptionsCache.depthFormat = a_depthFormat;
	dlssgOptionsCache.backBuffers = backBuffers;
	dlssgActive = true;
	dlssgPresentSafetyFrames = 0;
	return true;
}

void Streamline::DisableDLSSG(bool a_releaseResources)
{
	if (!featureDLSSG || !slDLSSGSetOptions) {
		dlssgActive = false;
		return;
	}

	if (!a_releaseResources && !dlssgActive && dlssgOptionsCache.valid && dlssgOptionsCache.mode == sl::DLSSGMode::eOff)
		return;

	sl::DLSSGOptions options{};
	options.mode = sl::DLSSGMode::eOff;
	options.flags = a_releaseResources ? sl::DLSSGFlags{} : sl::DLSSGFlags::eRetainResourcesWhenOff;
	options.onErrorCallback = DLSSGAPIErrorCallback;

	if (SL_FAILED(result, slDLSSGSetOptions(viewport, options))) {
		logger::warn("[Streamline] Could not disable DLSS-G: {}", magic_enum::enum_name(result));
	}

	if (dlssgActive)
		dlssgPresentSafetyFrames = kDLSSGPresentSafetyFrames;
	dlssgActive = false;
	dlssgOptionsCache = {};
	dlssgOptionsCache.valid = true;
	dlssgOptionsCache.mode = sl::DLSSGMode::eOff;
}

bool Streamline::TagDLSSGResources(ID3D12GraphicsCommandList* a_commandList,
	ID3D12Resource* a_hudlessColor, ID3D12Resource* a_depth, ID3D12Resource* a_motionVectors,
	uint32_t a_renderWidth, uint32_t a_renderHeight, uint32_t a_displayWidth, uint32_t a_displayHeight)
{
	if (!dlssgActive || !slSetTagForFrame || !a_commandList || !a_hudlessColor || !a_depth || !a_motionVectors)
		return false;

	if (!presentFrameToken)
		return false;

	constexpr auto lifecycle = sl::ResourceLifecycle::eValidUntilPresent;

	sl::Extent lowResExtent{ 0, 0, a_renderWidth, a_renderHeight };
	sl::Extent fullExtent{ 0, 0, a_displayWidth, a_displayHeight };

	sl::Resource hudless = { sl::ResourceType::eTex2d, a_hudlessColor, nullptr, nullptr, D3D12_RESOURCE_STATE_COMMON };
	sl::Resource depth = { sl::ResourceType::eTex2d, a_depth, nullptr, nullptr, D3D12_RESOURCE_STATE_COMMON };
	sl::Resource mvec = { sl::ResourceType::eTex2d, a_motionVectors, nullptr, nullptr, D3D12_RESOURCE_STATE_COMMON };

	sl::ResourceTag backbufferTag = { nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle{}, &fullExtent };
	sl::ResourceTag hudlessTag = { &hudless, sl::kBufferTypeHUDLessColor, lifecycle, &fullExtent };
	sl::ResourceTag depthTag = { &depth, sl::kBufferTypeDepth, lifecycle, &lowResExtent };
	sl::ResourceTag mvecTag = { &mvec, sl::kBufferTypeMotionVectors, lifecycle, &lowResExtent };

	sl::ResourceTag resourceTags[] = { backbufferTag, hudlessTag, depthTag, mvecTag };
	if (SL_FAILED(result, slSetTagForFrame(*presentFrameToken, viewport, resourceTags, _countof(resourceTags), a_commandList))) {
		static bool loggedFailure = false;
		if (!loggedFailure) {
			loggedFailure = true;
			logger::warn("[Streamline] Could not tag DLSS-G resources: {}", magic_enum::enum_name(result));
		}
		return false;
	}
	return true;
}

void Streamline::ClearDLSSGResourceTags(ID3D12GraphicsCommandList* a_commandList, uint32_t a_displayWidth, uint32_t a_displayHeight)
{
	if (!slSetTagForFrame || !a_commandList)
		return;
	auto* token = presentFrameToken ? presentFrameToken : frameToken;
	if (!token)
		return;

	const sl::Extent fullExtent{ 0, 0, a_displayWidth, a_displayHeight };

	sl::ResourceTag backbufferTag = { nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle{}, &fullExtent };
	sl::ResourceTag hudlessTag = { nullptr, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle{} };
	sl::ResourceTag depthTag = { nullptr, sl::kBufferTypeDepth, sl::ResourceLifecycle{} };
	sl::ResourceTag mvecTag = { nullptr, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle{} };

	sl::ResourceTag resourceTags[] = { backbufferTag, hudlessTag, depthTag, mvecTag };
	if (SL_FAILED(result, slSetTagForFrame(*token, viewport, resourceTags, _countof(resourceTags), a_commandList))) {
		static bool loggedFailure = false;
		if (!loggedFailure) {
			loggedFailure = true;
			logger::warn("[Streamline] Could not clear DLSS-G resource tags: {}", magic_enum::enum_name(result));
		}
	}
}

void Streamline::QueryDLSSGState(std::string_view a_phase, bool a_force)
{
	if (!featureDLSSG || !slDLSSGGetState)
		return;

	const uint32_t currentFrame = globals::state ? globals::state->frameCount : lastDLSSGStateQueryFrame + 1;
	if (!a_force && dlssgActive &&
		lastDLSSGStateQueryFrame != UINT32_MAX &&
		currentFrame - lastDLSSGStateQueryFrame < kDLSSGStateQueryInterval) {
		return;
	}
	lastDLSSGStateQueryFrame = currentFrame;

	sl::DLSSGState state{};
	if (SL_FAILED(result, slDLSSGGetState(viewport, state, nullptr))) {
		logger::warn("[Streamline] Could not query DLSS-G state: {}", magic_enum::enum_name(result));
		return;
	}

	maxFramesToGenerate = std::max<uint32_t>(1, state.numFramesToGenerateMax);
	dynamicMFGSupported = state.bIsDynamicMFGSupported == sl::Boolean::eTrue;
	dlssgStateKnown = true;
	dlssgPresentedFrames = state.numFramesActuallyPresented;
	dlssgLastStatus = static_cast<uint32_t>(state.status);

	if (lastDLSSGLoggedStatus != dlssgLastStatus || lastDLSSGLoggedPresented != state.numFramesActuallyPresented) {
		logger::info(
			"[Streamline] DLSS-G state phase={} status={}({}) requested={} actuallyPresented={} max={} dynamicMFG={} active={}",
			a_phase,
			dlssgLastStatus,
			DLSSGStatusFlags(state.status),
			dlssgOptionsCache.generatedFrames,
			state.numFramesActuallyPresented,
			state.numFramesToGenerateMax,
			dynamicMFGSupported,
			dlssgActive);
		lastDLSSGLoggedStatus = dlssgLastStatus;
		lastDLSSGLoggedPresented = state.numFramesActuallyPresented;
	}

	if (mfgUnlockReady && maxFramesToGenerate <= 1)
		logger::warn("[Streamline] RTX 40 MFG unlock patched but DLSS-G still reports numFramesToGenerateMax=1; restart the game so the patch precedes device binding");

	if (dlssgActive && state.status != sl::DLSSGStatus::eOk) {
		logger::warn("[Streamline] DLSS-G disabled due to runtime status {}", DLSSGStatusFlags(state.status));
		DisableDLSSG();
	}
}

void Streamline::OnPresentStart()
{
	// Present markers belong to the frame going out, which is the present token when DLSS-G
	// prepared one this frame; otherwise the render token is the best match available.
	auto* token = presentFrameToken ? presentFrameToken : frameToken;
	SetPCLMarker(sl::PCLMarker::eRenderSubmitEnd, token);
	SetPCLMarker(sl::PCLMarker::ePresentStart, token);
}

void Streamline::OnPresentEnd()
{
	auto* token = presentFrameToken ? presentFrameToken : frameToken;
	SetPCLMarker(sl::PCLMarker::ePresentEnd, token);
	presentFrameToken = nullptr;
	if (!dlssgActive && dlssgPresentSafetyFrames > 0)
		--dlssgPresentSafetyFrames;
}
