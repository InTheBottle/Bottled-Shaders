#pragma once

#include "Buffer.h"
#include "Feature.h"

struct OrderIndependentTransparency final : Feature
{
	virtual inline std::string GetName() override { return "Order Independent Transparency"; }
	virtual std::string GetDisplayName() override { return T("feature.oit.name", "Order Independent Transparency"); }
	virtual inline std::string GetShortName() override { return "OrderIndependentTransparency"; }
	virtual inline std::string_view GetShaderDefineName() override { return "OIT"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kMaterials; }
	virtual bool HasShaderDefine(RE::BSShader::Type a_type) override;
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override;

	enum class OITMethod : uint32_t
	{
		Disabled,
		Visualize,
		Balanced,
		Fast,
		Stable,
	};

	static constexpr float kInfiniteDistance = 200000.0f;

	struct Settings
	{
		OITMethod Method = OITMethod::Fast;
		uint BufferSize = 8;
		uint MaxLayers = 8;
		float AlphaThreshold = 0.0f;
		float DistanceThreshold = kInfiniteDistance;
		bool CaptureMultiplicativeLayer = true;
		bool OverrideRenderTargets = false;
		bool WriteDepth = true;
		float WriteDepthThreshold = 0.0f;
		float WBOITAdditiveAlphaScale = 0.1f;
		float WBOITMinProjectedDistance = 0.2f;
		float WBOITWeightMin = 0.1f;
		float WBOITWeightMax = 1.0f;
		float SSRAlphaScale = 1.0f;
	};

	struct FeatureCB
	{
		uint Enabled = 0;
		float AlphaThreshold = 0.0f;
		float WriteDepthThreshold = 0.0f;
		float SSRAlphaScale = 1.0f;
		float WBOITAdditiveAlphaScale = 0.1f;
		float WBOITMinProjectedDistance = 0.2f;
		float WBOITWeightMin = 0.1f;
		float WBOITWeightMax = 1.0f;
	};
	static_assert(sizeof(FeatureCB) == 32);

	Settings settings;

	[[nodiscard]] FeatureCB GetCommonBufferData() const;

	virtual void PostPostLoad() override;
	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	virtual void GenerateShaderPermutations(RE::BSShader* a_shader) override;
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	[[nodiscard]] bool ShouldCapture() const { return capturing; }
	[[nodiscard]] uint32_t GetMaterialVariant() const;
	[[nodiscard]] const char* GetNodeCountDefine() const;
	[[nodiscard]] bool IsSettingOwnBlendState() const { return settingOwnBlendState; }

	bool BeginAlphaGroup();
	void EndAlphaGroup();
	void BeginWater();
	void PreSetStateDirty();
	void PreDrawHack();
	void ResolveTechnique(const RE::BSShader& a_shader, uint32_t& a_pixelDescriptor, bool a_skipPixelShader);
	void SetupGeometry(RE::BSRenderPass* a_pass);
	void RestoreGeometry();

	float passTime = 0.0f;
	float compositeTime = 0.0f;
	uint32_t capturedDraws = 0;
	uint32_t pendingDraws = 0;

private:
	struct FragmentListNode
	{
		uint next;
		float nearness;
		uint packedColor[2];
	};
	static_assert(sizeof(FragmentListNode) == 16);

	struct BlendVariantKey
	{
		ID3D11BlendState* engineState;
		bool capture;
		bool depth;
		bool operator==(const BlendVariantKey&) const = default;
	};

	struct BlendVariantKeyHash
	{
		size_t operator()(const BlendVariantKey& a_key) const noexcept
		{
			return std::hash<void*>{}(a_key.engineState) ^ (static_cast<size_t>(a_key.capture) << 1) ^ (static_cast<size_t>(a_key.depth) << 2);
		}
	};

	struct BlendVariant
	{
		winrt::com_ptr<ID3D11BlendState> engineState;
		winrt::com_ptr<ID3D11BlendState> variant;
	};

	[[nodiscard]] uint32_t GetMethodVariant() const;
	void NormalizeSettings();
	void OnMethodChanged();
	void SyncMaterialNodeCount();
	bool EnsureResources();
	void ReleaseMethodResources();
	void CompileShaders();
	[[nodiscard]] uint GetNodeCount() const;
	[[nodiscard]] bool UsesFragmentList() const;
	[[nodiscard]] float GetFarDepth() const;
	[[nodiscard]] ID3D11ShaderResourceView* GetMainDepthSRV() const;
	void BindCaptureTargets();
	void ApplyBlendVariant(bool a_capture, bool a_depth);
	ID3D11BlendState* GetBlendVariant(ID3D11BlendState* a_engineState, bool a_capture, bool a_depth);
	void SetOwnBlendState(ID3D11BlendState* a_state);
	void DisableForFailure(std::string_view a_reason);

	std::optional<Texture2D> headerTexture;
	std::optional<Buffer> nodesBuffer;
	std::optional<Buffer> rovColorBuffer;
	std::optional<Buffer> rovDepthBuffer;
	std::optional<Texture2D> accumFrontTexture;
	std::optional<Texture2D> accumAllTexture;
	std::optional<Texture2D> revealageTexture;
	std::optional<Texture2D> writeDepthTexture;

	winrt::com_ptr<ID3D11PixelShader> resolvePS;
	winrt::com_ptr<ID3D11PixelShader> depthPS;
	OITMethod compiledMethod = OITMethod::Disabled;
	uint compiledNodeCount = 0;

	winrt::com_ptr<ID3D11BlendState> compositeBlendState;
	winrt::com_ptr<ID3D11DepthStencilState> compositeDepthState;
	winrt::com_ptr<ID3D11DepthStencilState> depthPassState;
	winrt::com_ptr<ID3D11RasterizerState> compositeRasterState;
	std::unordered_map<BlendVariantKey, BlendVariant, BlendVariantKeyHash> blendVariants;

	std::array<ID3D11RenderTargetView*, 7> captureRTVs{};
	std::array<ID3D11UnorderedAccessView*, 3> captureUAVs{};
	ID3D11DepthStencilView* captureDSV = nullptr;
	UINT captureRTVCount = 0;
	UINT captureUAVCount = 0;

	ID3D11BlendState* boundVariant = nullptr;
	ID3D11BlendState* boundEngineState = nullptr;
	bool boundCapture = false;
	bool boundDepth = false;

	OITMethod resourceMethod = OITMethod::Disabled;
	uint resourceBufferSize = 0;
	uint resourceNodeCount = 0;
	uint materialNodeCount = 0;
	bool resourcesFailed = false;
	bool alphaOnlyReady = false;
	bool rovSupported = false;
	bool rovChecked = false;
	bool compositeValid = false;

	uint editBufferSize = 0;
	uint editMaxLayers = 0;
	bool editingBufferSize = false;
	bool editingMaxLayers = false;

	bool capturing = false;
	bool settingOwnBlendState = false;
	bool techniqueCaptureReady = false;
	bool techniquePending = false;
	bool drawWriteDepth = false;
	bool drawTooFar = false;
	RE::NiTransform cameraWorldInverse;
	uint32_t frameCapturedDraws = 0;
	uint32_t framePendingDraws = 0;
	uint32_t frameDepthDraws = 0;
};
