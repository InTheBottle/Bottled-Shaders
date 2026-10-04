#pragma once

struct FurShells : Feature
{
	virtual inline std::string GetName() override { return "Fur Shells"; }
	virtual std::string GetDisplayName() override { return T("feature.fur_shells.name", "Fur Shells"); }
	virtual inline std::string GetShortName() override { return "FurShells"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kCharacters; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.fur_shells.description", "Grows layered fur on armors and creatures at draw time from the shell textures that fur mesh mods ship, so the layered meshes themselves are no longer needed."),
			{ T("feature.fur_shells.key_feature_1", "Uses the _shell.dds texture next to an armor or creature texture"),
				T("feature.fur_shells.key_feature_2", "Works on any body shape, refit or physics mesh"),
				T("feature.fur_shells.key_feature_3", "Shell count drops with distance"),
				T("feature.fur_shells.key_feature_4", "Adjustable length, density and droop") } };
	}

	struct Settings
	{
		bool Enabled = true;
		uint32_t ShellCount = 12;
		float Length = 1.2f;
		float BodyLength = 0.4f;
		float Droop = 0.15f;
		float RootThreshold = 1.0f;
		float TipThreshold = 1.0f;
		float RootDarkening = 0.6f;
		float ShellColor = 1.0f;
		float FadeStart = 700.0f;
		float FadeEnd = 2500.0f;
	} settings;

	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual void SetupResources() override;
	virtual void PostPostLoad() override;
	virtual void Reset() override;
	virtual void GenerateShaderPermutations(RE::BSShader* a_shader) override;

	void BeginPass(RE::BSShader* a_shader, RE::BSRenderPass* a_pass);
	void EndPass();
	bool DrawShells(UINT a_indexCount, UINT a_startIndexLocation, INT a_baseVertexLocation);
	uint32_t GetInstanceCount() const { return instanceCount; }

private:
	struct Hooks;

	struct PerPass
	{
		float Length;
		float ShellCount;
		float Droop;
		float RootThreshold;
		float TipThreshold;
		float RootDarkening;
		float ShellColor;
		float Pad;
	};
	static_assert(sizeof(PerPass) == 32);

	struct Entry
	{
		RE::BSFixedString name;
		RE::NiSourceTexturePtr shell;
	};

	struct DepthStates
	{
		winrt::com_ptr<ID3D11DepthStencilState> source;
		winrt::com_ptr<ID3D11DepthStencilState> prepass;
		winrt::com_ptr<ID3D11DepthStencilState> shade;
	};

	RE::NiSourceTexture* FindShell(RE::BSRenderPass* a_pass);
	const DepthStates* GetDepthStates(ID3D11DepthStencilState* a_source);

	eastl::unique_ptr<ConstantBuffer> perPassCB;
	winrt::com_ptr<ID3D11BlendState> noColorWrite;
	ankerl::unordered_dense::map<ID3D11DepthStencilState*, DepthStates> depthStates;
	ankerl::unordered_dense::map<RE::NiSourceTexture*, Entry> entries;
	ankerl::unordered_dense::set<std::string> missingShells;
	RE::NiSourceTexture* lastDiffuse = nullptr;
	RE::NiSourceTexture* lastShell = nullptr;
	PerPass lastPerPass{};
	winrt::com_ptr<ID3D11VertexShader> savedVertexShader;
	winrt::com_ptr<ID3D11PixelShader> savedPixelShader;
	ID3D11PixelShader* furPixelShader = nullptr;
	ID3D11PixelShader* depthPixelShader = nullptr;
	uint32_t instanceCount = 0;
	uint32_t resolveBudget = 0;
	uint32_t passCount = 0;
	uint32_t lastPassCount = 0;
};
