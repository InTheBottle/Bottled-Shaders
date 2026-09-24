#pragma once

#include "Feature.h"
#include "Upscaling/DX12SwapChain.h"
#include "Upscaling/DlssD3D12Bridge.h"
#include "Upscaling/DlssNR.h"
#include "Upscaling/FidelityFX.h"
#include "Upscaling/RCAS/RCAS.h"
#include "Upscaling/Streamline.h"
#include <d3d11_4.h>
#include <d3d12.h>
#include <winrt/base.h>

/**
 * @brief Provides upscaling functionality including DLSS, FSR and TAA.
 *
 * This feature handles various upscaling methods and frame generation technologies
 * to improve performance while maintaining visual quality.
 */
struct Upscaling : Feature
{
private:
	static constexpr std::string_view MOD_ID = "156952";

public:
	// Feature interface
	virtual inline std::string GetName() override { return "Upscaling"; }
	virtual std::string GetDisplayName() override { return T("feature.upscaling.name", "Upscaling"); }
	virtual inline std::string GetShortName() override { return "Upscaling"; }
	virtual inline std::string GetFeatureModLink() override { return MakeNexusModURL(MOD_ID); }
	virtual inline bool IsCore() const override { return false; }
	virtual inline std::string_view GetCategory() const override { return FeatureCategories::kDisplay; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.upscaling.description", "Advanced upscaling and frame generation technologies for improved performance"),
			{ T("feature.upscaling.key_feature_1", "DLSS (Deep Learning Super Sampling) support"),
				T("feature.upscaling.key_feature_2", "FSR (FidelityFX Super Resolution) support"),
				T("feature.upscaling.key_feature_3", "TAA (Temporal Anti-Aliasing) support"),
				T("feature.upscaling.key_feature_4", "Frame generation for supported systems") } };
	};

	float2 jitter = { 0, 0 };
	float2 neuralPreviousJitter = { 0, 0 };            ///< Last frame's jitter, for the neural rendering model's motion input
	uint32_t neuralPreviousJitterFrame = UINT32_MAX;

	enum class UpscaleMethod
	{
		kNONE,
		kTAA,
		kFSR,
		kDLSS
	};

	/// Which frame generation runtime drives the D3D12 proxy swap chain. Resolved once at device
	/// creation; switching needs a restart.
	enum class FrameGenerationTech : uint32_t
	{
		kAuto = 0,   ///< DLSS Frame Generation on NVIDIA when its runtime is present, else FSR
		kFSR = 1,    ///< AMD FidelityFX Frame Generation
		kDLSSG = 2,  ///< NVIDIA DLSS Frame Generation (multi-frame capable)
	};

	static constexpr uint32_t kFsr4RuntimeSelectionSchemaVersion = 1;

	struct Settings
	{
		uint upscaleMethod = (uint)UpscaleMethod::kDLSS;
		uint upscaleMethodNoDLSS = (uint)UpscaleMethod::kFSR;
		uint qualityMode = 1;  // Default to Quality (1=Quality, 2=Balanced, 3=Performance, 4=Ultra Performance, 0=Native AA)
		uint frameLimitMode = 0;  ///< Off by default; caps the rendered rate at refresh / multiplier while frame generation is on
		bool frameGenerationFPSLimitEnabled = false;  ///< Cap the presented rate while frame generation is on, independent of the refresh-rate limiter
		float frameGenerationFPSLimit = 120.0f;       ///< Presented frames per second; the rendered rate is this divided by the multiplier
		uint frameGenerationMode = 1;
		uint frameGenerationForceEnable = 0;
		bool frameGenerationAllowInMenus = false;
		uint frameGenerationTech = (uint)FrameGenerationTech::kAuto;
		uint dlssgGeneratedFrames = 0;    // Index: 0 = one generated frame (2x) .. 4 = five (6x); the runtime clamps
		bool dynamicMFGEnabled = false;   // DLSS-G Dynamic Multi Frame Generation when the runtime supports it
		uint dynamicMFGTargetFPS = 0;     // Dynamic MFG target output rate; 0 = display refresh
		bool rtx40MFGUnlock = false;      // Patch DLSS-G in memory so RTX 40 (Ada) can generate more than one frame; needs a restart
		uint streamlineLogLevel = 0;      // 0=Off, 1=Default, 2=Verbose
		float sharpnessFSR = 0.0f;
		bool sharpnessEnabledDLSS = false;
		float sharpnessDLSS = 0.0f;
		uint presetDLSS = 0;  // 0=Default, 1=J, 2=K, 3=L, 4=M
		uint reflexMode = 1;  // 0=Off, 1=On, 2=On + Boost
		bool reflexUseFPSLimit = false;
		float reflexFPSLimit = 60.0f;

		// Opt in to AMD's runtime FSR4 upscaler DLL on eligible AMD hardware instead of the
		// host-linked FSR3 SDK; falls back to FSR3 on any failure.
		bool fsr4RuntimeEnable = false;

		// Tracks whether fsr4RuntimeEnable has been auto-migrated for the detected adapter.
		// Defaults to current so a fresh config needs no migration; LoadSettings resets it to
		// 0 when absent from JSON so pre-existing configs run the migration once.
		uint32_t fsr4RuntimeSelectionSchemaVersion = kFsr4RuntimeSelectionSchemaVersion;

		// DLSS 5 Neural Rendering over the DLSS output (needs nvngx_dlssnr.dll in the Streamline folder)
		bool dlssHintMasks = false;  ///< Tag the TAA-derived bias and transparency hints for DLSS; off matches the reference implementation
		bool neuralRenderingEnabled = false;
		DlssNR::Settings neuralRendering;
	};

	Settings settings;

	// fsr4RuntimeEnable requires a restart: CreateUpscalingTextureResources (which allocates
	// runtimeFsrDepthTexture) only runs on an upscale-method change, so a mid-session flip
	// could otherwise select the runtime provider with no typed depth texture to feed it.
	// Latched in SetupResources, after the D3D device hook has run the adapter probe and the
	// one-shot migration -- LoadSettings is too early for both.
	bool fsr4RuntimeEnableBoot = false;

	struct JitterCB
	{
		float2 jitter;
		float useWideKernel;
		float pad0;
	};

	struct UpscalingDataCB
	{
		float2 trueSamplingDim;
		float2 pad0;
	};

	ConstantBuffer* jitterCB = nullptr;
	ConstantBuffer* upscalingDataCB = nullptr;

	// Runtime state
	bool isWindowed = false;
	bool lowRefreshRate = false;
	bool fidelityFXMissing = false;
	bool d3d12SwapChainActive = false;
	bool activeFrameGenIsDLSSG = false;  ///< Resolved frame generation technology for this session (valid with d3d12SwapChainActive)
	bool rtx40MFGUnlockBoot = false;     ///< Unlock setting as read at device creation (the setting needs a restart)
	std::atomic<bool> windowFocused{ true };  ///< False while the window is minimised; frame generation pauses
	std::atomic<bool> windowActive{ true };   ///< False while another application is in the foreground (WM_ACTIVATEAPP); the DLSS-G presenter rejects presents then

	// Timing and scaling
	double refreshRate = 0.0f;
	float2 resolutionScale = { 1.0f, 1.0f };
	LARGE_INTEGER qpf;

	// FG FPS Measurement for Overlay
	bool IsFrameGenerationDx12PathActive() const;
	bool IsFrameGenerationActive() const;

	/** @brief True for menus that must run without frame generation (loading, main menu, and the map/skills menus unless allowed). */
	bool IsFrameGenerationBlockedByMenu() const;
	bool ShouldUseFrameGenerationThisFrame() const;
	float GetFrameGenerationFrameTime() const;
	/** @brief Presented frames per rendered frame: DLSS-G's reported multiplier, or 2 for FSR. 1 when off. */
	uint32_t GetFrameGenerationMultiplier() const;
	bool IsUpscalingActive() const;

	/** @brief True when the D3D12 proxy is presenting through DLSS Frame Generation. */
	bool IsDlssFrameGenerationPathActive() const { return d3d12SwapChainActive && activeFrameGenIsDLSSG; }
	/** @brief True when the D3D12 proxy is the FidelityFX frame-generation swap chain. */
	bool IsFsrFrameGenerationPathActive() const { return d3d12SwapChainActive && !activeFrameGenIsDLSSG; }
	/** @brief True when DLSS super resolution evaluates on the D3D12 bridge (Streamline bound to D3D12). */
	bool UsesD3D12DLSS() const { return streamline.UsesD3D12(); }

	// Feature interface overrides
	virtual void DrawSettings() override;
	virtual void SaveSettings(json& o_json) override;
	virtual void LoadSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual void DataLoaded() override;

	/**
	 * @brief Installs Direct3D-related hooks for device and factory creation.
	 *
	 * Loads FidelityFX support and patches the import address table (IAT) to redirect D3D11 device and DXGI factory creation functions to custom hook implementations.
	**/
	virtual void Load() override;

	/** @brief Re-applies the device creation IAT hook after RenderDoc rewrote the import table; see Hooks::ReapplyEarlyHooks. */
	void ReapplyDeviceHook();
	virtual void PostPostLoad() override;
	virtual void SetupResources() override;

	UpscaleMethod GetUpscaleMethod() const;

	void CheckResources(UpscaleMethod a_upscalemethod);
	void CreateUpscalingTextureResources(UpscaleMethod a_upscalemethod);
	void DestroyUpscalingTextureResources(UpscaleMethod a_upscalemethod);

	winrt::com_ptr<ID3D11ComputeShader> encodeTexturesCS[4];          // One for each UpscaleMethod (kNONE, kTAA, kFSR, kDLSS)
	winrt::com_ptr<ID3D11ComputeShader> encodeTexturesCSDepthOutput;  // FSR: converts R24G8_TYPELESS depth to R32_FLOAT
	winrt::com_ptr<ID3D11ComputeShader> encodeTexturesCSDepthOutputDLSS;  // DLSS: same, feeds the D3D12 bridge
	ID3D11ComputeShader* GetEncodeTexturesCS();

	winrt::com_ptr<ID3D11PixelShader> depthRefractionUpscalePS;
	ID3D11PixelShader* GetDepthRefractionUpscalePS();

	winrt::com_ptr<ID3D11PixelShader> underwaterMaskUpscalePS;
	ID3D11PixelShader* GetUnderwaterMaskUpscalePS();

	winrt::com_ptr<ID3D11VertexShader> upscaleVS;
	ID3D11VertexShader* GetUpscaleVS();

	winrt::com_ptr<ID3D11DepthStencilState> upscaleDepthStencilState;
	winrt::com_ptr<ID3D11BlendState> upscaleBlendState;
	winrt::com_ptr<ID3D11RasterizerState> upscaleRasterizerState;

	// Helper: Create a Texture2D matching source format at a given size
	static eastl::unique_ptr<Texture2D> CreateTextureFromSource(ID3D11Resource* src, uint32_t width, uint32_t height,
		bool copyBindFlags = false, bool createSRV = false, bool createUAV = false, const char* name = nullptr);

	void ConfigureTAA();
	void ConfigureUpscaling(RE::BSGraphics::State* a_state);
	void Upscale();

	// D3D11 textures
	Texture2D* reactiveMaskTexture = nullptr;
	Texture2D* transparencyCompositionMaskTexture = nullptr;
	Texture2D* motionVectorCopyTexture = nullptr;
	Texture2D* sharpenerTexture = nullptr;
	Texture2D* runtimeFsrDepthTexture = nullptr;
	Texture2D* dlssDepthTexture = nullptr;  ///< Typed R32_FLOAT depth for the D3D12 bridge (DLSS on D3D12, DLSS-NR)

	virtual void ClearShaderCache() override;

	// Static instances instead of singletons
	static inline Streamline streamline;
	static inline FidelityFX fidelityFX;  ///< Only for frame generation
	static inline DX12SwapChain dx12SwapChain;
	static inline RCAS rcas;  ///< Standalone RCAS sharpening for DLSS
	static inline DlssD3D12Bridge dlssBridge;  ///< D3D12 DLSS super resolution and DLSS-NR

	winrt::com_ptr<ID3D11PixelShader> copyDepthToSharedBufferPS;

	float projectionPosScaleX = 0.0f;
	float projectionPosScaleY = 0.0f;

	float dynamicResolutionWidthRatio = 1.0f;
	float dynamicResolutionHeightRatio = 1.0f;

	bool previousUpscalingWasActive = false;
	bool depthUpscaleUseWideKernel = false;
	bool dlssD3D12FailureLogged = false;

	/**
	 * Set by MenuOpenCloseEventHandler when LoadingMenu closes (cell/worldspace transitions,
	 * initial load). Consumed at the start of Upscale() to force a one-frame DLSS feature
	 * rebuild.
	 */
	std::atomic<bool> pendingDLSSReset{ false };

	void CopySharedD3D12Resources();
	void PostDisplay();
	void PerformUpscaling();
	void UpscaleDepth();

	/**
	 * @brief Applies RCAS sharpening to the main render target after DLSS upscaling.
	 *
	 * Runs in HDR space before tonemapping. Only called when DLSS is active and sharpness > 0.
	 */
	void ApplySharpening();

	static void TimerSleepQPC(int64_t targetQPC);

	void FrameLimiter();

	static double GetRefreshRate(HWND a_window);

	// Unified interface methods - external code should use these instead of direct access
	void LoadUpscalingSDKs();  // Loads all SDKs at once
	HANDLE GetFrameLatencyWaitableObject() const;
	float GetFrameTime() const;

	// Backend interface methods
	bool IsBackendInitialized() const;
	void CheckBackendFeatures(IDXGIAdapter* adapter);
	void UpgradeBackendInterface(void** ppInterface);
	void SetBackendD3DDevice(ID3D11Device* device);
	void PostBackendDevice();

	// Module availability methods
	bool HasFrameGenModule() const;
	bool HasFsrFrameGenModule() const;
	bool HasDlssFrameGenModule() const;

	// Proxy interface methods
	void SetProxyD3D11Device(ID3D11Device* device);
	void SetProxyD3D11DeviceContext(ID3D11DeviceContext* context);
	void CreateProxySwapChain(IDXGIAdapter* adapter, DXGI_SWAP_CHAIN_DESC swapChainDesc);
	void CreateProxyInterop();
	IDXGISwapChain* GetProxySwapChain();

	using BlurResources = DX12SwapChain::BlurResources;

	// Get all D3D11 resources needed for background blur when D3D12 swap chain is active
	BlurResources GetBlurResources() const;

	/** @brief Pushes the neural rendering settings and enable flag to the DLSS-NR pass. */
	void SyncNeuralRenderingSettings();

private:
	void DrawUpscalingTab();
	void DrawNeuralRenderingTab();

	struct Main_UpdateJitter
	{
		static void thunk(RE::BSGraphics::State* a_state);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct MenuManagerDrawInterfaceStartHook
	{
		static void thunk(int64_t a1);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct Main_PostProcessing
	{
		static void thunk(RE::ImageSpaceManager* a_this, uint32_t a3, RE::RENDER_TARGET a_target, void* a_4, bool a_5);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct SetScissorRect
	{
		static void thunk(RE::BSGraphics::Renderer* This, int a_left, int a_top, int a_right, int a_bottom);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct Main_RenderPrecipitation
	{
		static void thunk();
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSFaceGenManager_UpdatePendingCustomizationTextures
	{
		static void thunk();
		static inline REL::Relocation<decltype(thunk)> func;
	};

	class MenuOpenCloseEventHandler : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		virtual RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;
		static bool Register();
	};
};
