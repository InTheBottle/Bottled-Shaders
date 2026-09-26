#pragma once

#include "../../Buffer.h"
#include "../../State.h"

#include <cstdint>
#include <d3d11_4.h>
#include <d3d12.h>
#include <limits>
#include <string>
#include <string_view>

#define NV_WINDOWS

#pragma warning(push)
#pragma warning(disable: 4471)
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_matrix_helpers.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <sl_version.h>
#pragma warning(pop)

/**
 * @brief Manages NVIDIA Streamline integration: DLSS super resolution (D3D11 or D3D12),
 * DLSS Frame Generation with multi-frame support (D3D12 only), Reflex and PCL markers.
 *
 * Streamline binds to one render API per process. Without the D3D12 proxy swap chain the
 * interposer runs on the game D3D11 device and DLSS evaluates on the immediate context.
 * With the proxy and DLSS-G selected, the interposer runs on the proxy D3D12 device;
 * DLSS then evaluates on a D3D12 command list fed through shared textures (see
 * DlssD3D12Bridge) so the single Streamline instance can also drive frame generation.
 */
class Streamline
{
public:
	static constexpr const wchar_t* PluginDir = L"Data\\Shaders\\Upscaling\\Streamline";

	Streamline() = default;

	/** @brief Returns the short identifier used for logging. */
	inline std::string GetShortName() { return "Streamline"; }

	bool enabledAtBoot = false;
	bool initialized = false;
	bool triedInitialization = false;
	bool interposerLoaded = false;
	sl::RenderAPI initializedRenderAPI = sl::RenderAPI::eD3D11;

	bool featureDLSS = false;
	bool featureDLSSG = false;
	bool featureReflex = false;
	bool featurePCL = false;
	bool reflexSupportedOnCurrentAdapter = false;
	bool dlssgModulePresent = false;  ///< sl.dlss_g.dll and nvngx_dlssg.dll exist in the plugin folder

	sl::ViewportHandle viewport{ 0 };
	static constexpr uint32_t MAX_RESOLUTION = 8192;
	HMODULE interposer = NULL;
	std::wstring pluginDirAbsolute;

	// SL Interposer Functions
	PFun_slInit* slInit{};
	PFun_slShutdown* slShutdown{};
	PFun_slIsFeatureSupported* slIsFeatureSupported{};
	PFun_slIsFeatureLoaded* slIsFeatureLoaded{};
	PFun_slSetFeatureLoaded* slSetFeatureLoaded{};
	PFun_slEvaluateFeature* slEvaluateFeature{};
	PFun_slAllocateResources* slAllocateResources{};
	PFun_slFreeResources* slFreeResources{};
	PFun_slSetTagForFrame* slSetTagForFrame{};
	PFun_slGetFeatureRequirements* slGetFeatureRequirements{};
	PFun_slGetFeatureVersion* slGetFeatureVersion{};
	PFun_slUpgradeInterface* slUpgradeInterface{};
	PFun_slSetConstants* slSetConstants{};
	PFun_slGetNativeInterface* slGetNativeInterface{};
	PFun_slGetFeatureFunction* slGetFeatureFunction{};
	PFun_slGetNewFrameToken* slGetNewFrameToken{};
	PFun_slSetD3DDevice* slSetD3DDevice{};

	// DLSS specific functions
	PFun_slDLSSGetOptimalSettings* slDLSSGetOptimalSettings{};
	PFun_slDLSSGetState* slDLSSGetState{};
	PFun_slDLSSSetOptions* slDLSSSetOptions{};

	// DLSS-G specific functions
	PFun_slDLSSGGetState* slDLSSGGetState{};
	PFun_slDLSSGSetOptions* slDLSSGSetOptions{};

	// Reflex specific functions
	PFun_slReflexGetState* slReflexGetState{};
	PFun_slReflexSleep* slReflexSleep{};
	PFun_slReflexSetOptions* slReflexSetOptions{};
	PFun_slPCLSetMarker* slPCLSetMarker{};
	PFun_slPCLGetState* slPCLGetState{};
	PFun_slPCLSetOptions* slPCLSetOptions{};

	// PC latency stats: the Reflex overlay pings the game window and expects a marker back.
	uint32_t pclStatsWindowMessage = 0;
	uint32_t pclPingCount = 0;
	bool pclLatencyReportAvailable = false;

	Util::FrameChecker frameChecker;
	sl::FrameToken* frameToken = nullptr;          ///< Token for the frame being rendered (State::frameCount)
	sl::FrameToken* presentFrameToken = nullptr;   ///< Token for the frame being presented (see PrepareDLSSGPresent)
	uint32_t constantsFrameIndex = UINT32_MAX;     ///< Frame index the common constants were last set for

	bool isRTXBelow40series = false;

	struct ReflexOptionsCache
	{
		bool valid = false;
		sl::ReflexMode mode = sl::ReflexMode::eOff;
		uint32_t frameLimitUs = 0;
		bool useMarkersToOptimize = false;
	};
	ReflexOptionsCache reflexOptionsCache{};
	uint32_t lastReflexSleepFrame = UINT32_MAX;
	uint32_t lastRenderSubmitFrame = UINT32_MAX;

	// DLSS-G runtime state
	bool dlssgActive = false;               ///< slDLSSGSetOptions currently holds a non-off mode
	bool dlssgStateKnown = false;           ///< numFramesToGenerateMax / dynamic MFG support queried
	uint32_t maxFramesToGenerate = 1;       ///< Runtime clamp reported by DLSS-G
	bool dynamicMFGSupported = false;       ///< DLSSGMode::eDynamic accepted by the runtime
	bool loggedDynamicMFGUnlockBlocked = false;
	bool mfgUnlockReady = false;            ///< RTX 40 unlock patches landed on the live modules
	uint32_t dlssgPresentedFrames = 0;      ///< numFramesActuallyPresented from the last state query
	uint32_t dlssgLastStatus = 0;           ///< DLSSGStatus bits from the last state query
	uint32_t dlssgPresentSafetyFrames = 0;  ///< Presents to keep DLSS-G rules after it was disabled
	uint32_t lastDLSSGStateQueryFrame = UINT32_MAX;
	uint32_t lastDLSSGLoggedStatus = UINT32_MAX;
	uint32_t lastDLSSGLoggedPresented = UINT32_MAX;
	bool loggedDynamicMFGUnsupported = false;

	struct DLSSGOptionsCache
	{
		bool valid = false;
		sl::DLSSGMode mode = sl::DLSSGMode::eOff;
		uint32_t generatedFrames = 0;
		uint32_t dynamicTargetFPS = 0;
		uint32_t renderWidth = 0;
		uint32_t renderHeight = 0;
		uint32_t displayWidth = 0;
		uint32_t displayHeight = 0;
		DXGI_FORMAT colorFormat = DXGI_FORMAT_UNKNOWN;
		DXGI_FORMAT mvecFormat = DXGI_FORMAT_UNKNOWN;
		DXGI_FORMAT depthFormat = DXGI_FORMAT_UNKNOWN;
		uint32_t backBuffers = 0;
	};
	DLSSGOptionsCache dlssgOptionsCache{};

	// D3D12 DLSS options cache (avoid re-sending identical options every frame)
	struct D3D12DLSSOptionsCache
	{
		bool valid = false;
		sl::DLSSMode mode = sl::DLSSMode::eOff;
		uint32_t outputWidth = 0;
		uint32_t outputHeight = 0;
		uint32_t preset = UINT32_MAX;
		bool hdr = false;
	};
	D3D12DLSSOptionsCache d3d12DLSSOptionsCache{};

	// Cached DLL version info for Streamline plugin directory
	static std::vector<std::pair<std::string, std::string>> dllVersions;

	/** @brief True when the interposer was initialised for D3D12 (the proxy swap chain path). */
	bool UsesD3D12() const { return initialized && initializedRenderAPI == sl::RenderAPI::eD3D12; }

	/** @brief Loads sl.interposer.dll and binds its exports. Does not initialise the SDK. */
	void LoadInterposer();

	/**
	 * @brief Initialises the SDK for the given render API. Must be called once, before any
	 * device is bound. D3D12 additionally requests the DLSS-G plugin.
	 */
	void Initialize(sl::RenderAPI a_renderAPI);

	/**
	 * @brief Queries available Streamline features (DLSS, DLSS-G, Reflex, PCL) on the given adapter.
	 * @param a_adapter The DXGI adapter to check feature support against.
	 */
	void CheckFeatures(IDXGIAdapter* a_adapter);

	/** @brief Binds DLSS, DLSS-G and Reflex feature functions after the D3D device is created. */
	void PostDevice();

	/** @brief Acquires a new frame token from Streamline for the current frame. */
	bool EnsureFrameToken();

	/** @brief PCL SimulationEnd + RenderSubmitStart, once per frame, when the game starts submitting the player view. */
	void OnRenderSubmitStart();

	/** @brief Token for an explicit frame index; Streamline returns the same token for the same index. */
	sl::FrameToken* AcquireFrameToken(uint32_t a_frameIndex);

	/**
	 * @brief Prepares the DLSS-G present: acquires the token of the frame just rendered (the present
	 * hook has already advanced State::frameCount) and sets the common constants for it if the
	 * upscaler did not already. Must run before TagDLSSGResources / OnPresentStart.
	 */
	bool PrepareDLSSGPresent();
	/**
	 * @brief Sets camera and jitter constants on the Streamline viewport for the current frame.
	 * @param p_viewport The viewport handle to configure.
	 * @return True if constants were set successfully.
	 */
	bool CheckFrameConstants(sl::ViewportHandle p_viewport);

	/**
	 * @brief Detects whether the GPU is an NVIDIA RTX card below the 40-series generation.
	 * @param a_adapter The DXGI adapter to inspect.
	 * @return True if the adapter is RTX 20xx or 30xx series.
	 */
	bool IsRTXAndBelow40Series(IDXGIAdapter* a_adapter);

	/**
	 * @brief Configures DLSS quality mode and resolution options for a viewport (D3D11 path).
	 * @param p_viewport The viewport handle to configure.
	 * @param width The target output width.
	 */
	void SetDLSSOptions(sl::ViewportHandle p_viewport, uint32_t width);

	/** @brief Fills DLSSOptions from settings (mode, presets, HDR) for the given output size. */
	void BuildDLSSOptions(sl::DLSSOptions& a_options, uint32_t a_outputWidth, uint32_t a_outputHeight, bool a_hdr) const;

	/**
	 * @brief Executes DLSS evaluation for a single viewport with the given D3D11 resources.
	 */
	void EvaluateDLSS(sl::ViewportHandle vp,
		ID3D11Resource* colorIn, ID3D11Resource* colorOut, ID3D11Resource* depth,
		ID3D11Resource* mvec, ID3D11Resource* reactiveMask, ID3D11Resource* transparencyMask,
		const sl::Extent& extentIn, const sl::Extent& extentOut, uint32_t outputWidth);

	/**
	 * @brief Dispatches DLSS upscaling for the current frame on D3D11.
	 */
	void Upscale(ID3D11Resource* a_upscalingTexture, ID3D11Resource* a_reactiveMask, ID3D11Resource* a_transparencyCompositionMask, ID3D11Resource* a_motionVectors);

	/**
	 * @brief Dispatches DLSS upscaling on a D3D12 command list. Resources must be native
	 * (non-proxy) D3D12 resources in the COMMON state; they are left in COMMON.
	 * @return True if slEvaluateFeature succeeded.
	 */
	bool UpscaleD3D12(ID3D12GraphicsCommandList* a_commandList,
		ID3D12Resource* a_colorIn, ID3D12Resource* a_colorOut, ID3D12Resource* a_depth,
		ID3D12Resource* a_motionVectors, ID3D12Resource* a_reactiveMask, ID3D12Resource* a_transparencyMask,
		uint32_t a_renderWidth, uint32_t a_renderHeight, uint32_t a_displayWidth, uint32_t a_displayHeight, bool a_hdr);

	/**
	 * @brief Applies the Reflex mode from settings (forced to at least Low Latency while DLSS-G
	 * runs), performs the once-per-frame Reflex sleep and emits the simulation PCL markers.
	 */
	void UpdateReflex();

	/** @brief Answers the PC latency stats ping posted to the game window (see Hooks WndProc). */
	void OnPCLStatsPing();
	uint32_t GetPCLStatsWindowMessage() const { return pclStatsWindowMessage; }

	/** @brief Most recent input-to-present latency from the Reflex frame report, 0 when unavailable. */
	float GetReflexLatencyMs();

	/** @brief Frees DLSS viewport resources through the Streamline SDK. */
	void DestroyDLSSResources();

	// ---- DLSS-G ----------------------------------------------------------------------------

	/**
	 * @brief Applies DLSS-G options for this frame.
	 * @param a_enabled False sends DLSSGMode::eOff (resources retained).
	 * @param a_generatedFrames Requested generated frames (1 = 2x). Clamped to the runtime maximum.
	 * @param a_dynamic Use DLSSGMode::eDynamic when the runtime supports it.
	 * @param a_dynamicTargetFPS Target output rate for dynamic mode; 0 = display refresh.
	 * @return True if the options were accepted (or unchanged).
	 */
	bool UpdateDLSSG(bool a_enabled, uint32_t a_generatedFrames, bool a_dynamic, uint32_t a_dynamicTargetFPS,
		uint32_t a_renderWidth, uint32_t a_renderHeight, uint32_t a_displayWidth, uint32_t a_displayHeight,
		DXGI_FORMAT a_colorFormat, DXGI_FORMAT a_mvecFormat, DXGI_FORMAT a_depthFormat, uint32_t a_backBuffers);

	/** @brief Sends DLSSGMode::eOff now (resources retained). */
	/** @brief Turns DLSS-G off. With a_releaseResources the runtime drops its retained resources so the next enable starts clean (used after a present failure or a dynamic/fixed switch). */
	void DisableDLSSG(bool a_releaseResources = false);

	/**
	 * @brief Tags the DLSS-G inputs for the current frame token on a D3D12 command list.
	 * Resources are native D3D12 resources in the COMMON state.
	 */
	bool TagDLSSGResources(ID3D12GraphicsCommandList* a_commandList,
		ID3D12Resource* a_hudlessColor, ID3D12Resource* a_depth, ID3D12Resource* a_motionVectors,
		uint32_t a_renderWidth, uint32_t a_renderHeight, uint32_t a_displayWidth, uint32_t a_displayHeight);

	/** @brief Clears the DLSS-G tags so a disabled frame does not reuse stale resources. */
	void ClearDLSSGResourceTags(ID3D12GraphicsCommandList* a_commandList, uint32_t a_displayWidth, uint32_t a_displayHeight);

	/** @brief Queries DLSS-G state (max frames, presented frames, status), rate limited while active. */
	void QueryDLSSGState(std::string_view a_phase, bool a_force = false);

	/** @brief True while presents must follow DLSS-G rules (active, or shortly after disable). */
	bool NeedsDLSSGPresentSafety() const { return dlssgActive || dlssgPresentSafetyFrames > 0; }

	/** @brief PCL markers around the swap chain present. */
	void OnPresentStart();
	void OnPresentEnd();

	/** @brief Generated frames in the options currently applied to DLSS-G (0 when off). */
	uint32_t currentGeneratedFrames() const { return dlssgOptionsCache.valid && dlssgOptionsCache.mode != sl::DLSSGMode::eOff ? dlssgOptionsCache.generatedFrames : 0; }
	/** @brief Multiplier actually presented by DLSS-G last query (1 when off). */
	uint32_t GetDLSSGPresentedMultiplier() const { return dlssgActive && dlssgPresentedFrames ? dlssgPresentedFrames : 1; }

private:
	void SetPCLMarker(sl::PCLMarker a_marker, sl::FrameToken* a_token = nullptr);
	bool SetCommonConstants(sl::FrameToken& a_token, uint32_t a_frameIndex, sl::ViewportHandle a_viewport);
	bool EnsureD3D12DLSSOptions(uint32_t a_outputWidth, uint32_t a_outputHeight, bool a_hdr);
	void ResetRuntimeState();
	bool emitPCLMarkers = false;
};
