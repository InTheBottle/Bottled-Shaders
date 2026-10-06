#pragma once

#include <cstdint>
#include <string>

struct TextureStreaming : Feature
{
	virtual inline std::string GetName() override { return "Texture Streaming"; }
	virtual std::string GetDisplayName() override { return T("feature.texture_streaming.name", "Texture Streaming"); }
	virtual inline std::string GetShortName() override { return "TextureStreaming"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLandscapeAndTextures; }
	virtual inline std::string GetFeatureModLink() override { return "https://github.com/dnai92/skyrimPerformanceSolution"; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.texture_streaming.description", "Shrinks the textures of far objects in VRAM by dropping their top mip levels on the GPU and reloads them at full size from disk or BSA in the background when you come closer. Files on disk are never changed. In budget mode nothing happens until the game nears the VRAM budget Windows grants it."),
			{ T("feature.texture_streaming.key_feature_1", "Distance based downscaling with a time-sliced scene scan, also for objects behind the camera"),
				T("feature.texture_streaming.key_feature_2", "VRAM budget mode: only kicks in above a usage threshold, largest savings first, refills when room frees up"),
				T("feature.texture_streaming.key_feature_3", "Remembered sizes are passed to the DDS loader so the top mips are never read after a loading screen"),
				T("feature.texture_streaming.key_feature_4", "RAM buffer for textures that go back and forth, so they do not hit the disk twice"),
				T("feature.texture_streaming.key_feature_5", "UI, map, LOD, terrain, fonts and inventory previews are always full size") } };
	}

	struct Settings
	{
		bool Enabled = true;
		bool LoadAtRememberedSize = true;
		bool BudgetMode = true;
		float BudgetStartPercent = 85.f;
		bool Refill = true;
		float RefillGapPercent = 10.f;
		float RamCacheMB = 1024.f;
		float SafetyFactor = 2.f;
		float MinEdge = 1024.f;
		float ScanBudgetMs = 0.4f;
		bool DetailedLog = false;
		std::string Exclude = "interface\\,\\maps\\,mapmarker,\\lod\\,terrain\\,fonts\\,book,\\sky\\,effects\\,cubemaps\\";
	};

	Settings settings;

	struct Status
	{
		std::uint64_t vramUsage = 0;
		std::uint64_t vramBudget = 0;
		float vramPercent = -1.f;
		std::uint64_t sharedBytes = 0;
		std::uint32_t managed = 0;
		std::uint32_t reduced = 0;
		std::uint64_t savedBytes = 0;
		std::uint32_t queued = 0;
		std::uint32_t remembered = 0;
		std::uint64_t ramCacheBytes = 0;
		bool pressure = false;
	};

	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	virtual void Load() override;
	virtual void DataLoaded() override;
	virtual void EarlyPrepass() override;

	void Reset(const char* a_reason);
	void RequestCenterProbe();
	Status GetStatus();

	class MenuOpenCloseEventHandler : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		virtual RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;
		static void Register();
	};

	struct Hooks
	{
		struct CreateTextureFromDDS
		{
			static std::int32_t thunk(void* a_device, void* a_stream, void** a_out, void* a_header, std::uint64_t a_maxSize, std::uint64_t a_6);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSShaderResourceManager_CreateRendererTexture
		{
			static void thunk(void* a_manager, RE::NiSourceTexture* a_texture);
			static inline REL::Relocation<decltype(thunk)> func;
		};
	};
};
