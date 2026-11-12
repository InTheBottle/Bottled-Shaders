#pragma once

#include "Buffer.h"

#include <span>

struct CapsuleShadows : Feature
{
	virtual inline std::string GetName() override { return "Capsule Shadows"; }
	virtual std::string GetDisplayName() override { return T("feature.capsule_shadows.name", "Capsule Shadows"); }
	virtual inline std::string GetShortName() override { return "CapsuleShadows"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kShadows; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.capsule_shadows.description", "Grounds characters with soft shadows built from their skeleton, including under lights that cast no shadows."),
			{ T("feature.capsule_shadows.key_feature_1", "Soft contact darkening under and around characters, indoors and out"),
				T("feature.capsule_shadows.key_feature_2", "Soft shadows away from the strongest nearby light that casts none of its own"),
				T("feature.capsule_shadows.key_feature_3", "Follows animation, and adds no cost to surface shaders") } };
	}

	static constexpr uint32_t kMaxActors = 8;
	static constexpr uint32_t kMaxCapsulesPerActor = 12;
	static constexpr uint32_t kMaxCapsules = kMaxActors * kMaxCapsulesPerActor;
	static constexpr size_t kSkeletonNodes = 22;

	struct Settings
	{
		bool Enabled = true;
		float AmbientStrength = 0.75f;
		float ShadowStrength = 0.6f;
		float MaxDistance = 2048.0f;
	};

	struct Capsule
	{
		float3 A;
		float Radius;
		float3 B;
		float pad;
	};
	static_assert(sizeof(Capsule) == 32);

	struct ActorData
	{
		float3 Center;
		float BoundRadius;
		float3 LightPosition;
		float LightStrength;
		uint FirstCapsule;
		uint CapsuleCount;
		float ShadowLength;
		float pad;
	};
	static_assert(sizeof(ActorData) == 48);

	struct FrameData
	{
		uint ActorCount;
		float AmbientStrength;
		float ShadowStrength;
		float pad;
	};
	static_assert(sizeof(FrameData) % 16 == 0);

	Settings settings;

	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	virtual void Prepass() override;
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	void Draw();
	ID3D11ShaderResourceView* GetOcclusionSRV() const;

private:
	struct NearbyLight
	{
		const RE::NiAVObject* node = nullptr;
		RE::NiPoint3 position;
		float radius = 0.0f;
		float luminance = 0.0f;
	};

	struct ActorState
	{
		RE::NiPoint3 position;
		float strength = 0.0f;
		uint32_t frame = 0;
		RE::NiPointer<RE::NiAVObject> root;
		std::array<RE::NiPointer<RE::NiAVObject>, kSkeletonNodes> bones;
	};

	void CollectLights();
	void AddActor(RE::Actor* a_actor, float a_deltaTime);
	uint32_t AddSkeletonCapsules(const ActorState& a_state, float a_scale);
	void RefreshBones(ActorState& a_state, RE::NiAVObject* a_root);
	void ReleaseBones(ActorState& a_state);
	void ForgetAllActors();
	void FlushReleases();
	void AddBoundCapsule(RE::Actor* a_actor, RE::NiAVObject* a_root);
	bool FindDominantLight(const RE::NiPoint3& a_position, const RE::NiAVObject* a_root, std::span<const Capsule> a_body, RE::NiPoint3& a_light, float& a_dominance) const;
	bool EnsureTexture(uint32_t a_width, uint32_t a_height);
	ID3D11ComputeShader* GetShader();

	std::vector<NearbyLight> lights;
	std::vector<std::pair<float, RE::ActorPtr>> candidates;
	std::unordered_map<RE::FormID, ActorState> actorStates;
	std::vector<RE::NiPointer<RE::NiAVObject>> pendingRelease;
	std::array<Capsule, kMaxCapsules> capsules{};
	std::array<ActorData, kMaxActors> actors{};
	uint32_t capsuleCount = 0;
	uint32_t actorCount = 0;
	float sunLuminance = 0.0f;
	bool occlusionValid = false;

	eastl::unique_ptr<Texture2D> occlusionTexture;
	eastl::unique_ptr<Buffer> capsuleBuffer;
	eastl::unique_ptr<Buffer> actorBuffer;
	eastl::unique_ptr<ConstantBuffer> frameConstants;
	winrt::com_ptr<ID3D11ComputeShader> computeShader;
	bool shaderCompileFailed = false;
};
