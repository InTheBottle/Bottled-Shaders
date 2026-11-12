#pragma once

#include "Buffer.h"
#include "PostProcessFeature.h"

/** @brief Screen-space sun and moon rays, the Effects 11 [RAYS] model for the Post Processing pipeline. */
struct SunRays : public PostProcessFeature
{
	virtual inline std::string GetType() const override { return "Sun Rays"; }
	virtual inline std::string GetDisplayName() const override { return T("feature.post_processing.sun_rays.name", "Sun and Moon Rays"); }
	virtual inline std::string GetDesc() const override { return T("feature.post_processing.sun_rays.description", "Light shafts that stream from the sun or moon past clouds and the horizon, blurred toward the light on screen."); }
	virtual inline bool DisableInMainLoadingMenu() const override { return true; }

	struct Settings
	{
		bool EnableSunRays = true;
		bool EnableMoonRays = false;
		float SunRaysMultiplier = 1.0f;
		float MoonRaysMultiplier = 1.0f;
		float SkyColorAmount = 0.0f;
		float CloudGapRays = 1.0f;
		float CloudGapRaysSpread = 30.0f;
		int Quality = 1;
		int Type = 0;
		int MoonType = 0;
		bool UseLinearMath = false;
	} settings;

	struct alignas(16) SunRaysCB
	{
		float4x4 InvViewProj;
		float2 LightUV;
		float2 UVScale;
		float2 UVMax;
		float BlurFactor;
		uint StepCount;
		float3 LightDirection;
		float LightBillboardTan;
		float3 RaysColor;
		float MaskBrightness;
		float MaskExponent;
		uint WeightedSteps;
		uint UseNoise;
		float InvScreenWidth;
		float CloudGapIntensity;
		float CloudGapExponent;
		float LightDiscTan;
		uint ReverseZ;
		float2 DynRes;
		uint FrameCount;
		float pad0;
	};
	STATIC_ASSERT_ALIGNAS_16(SunRaysCB);

	eastl::unique_ptr<ConstantBuffer> raysCB = nullptr;
	eastl::unique_ptr<Texture2D> texRaysA = nullptr;
	eastl::unique_ptr<Texture2D> texRaysB = nullptr;
	eastl::unique_ptr<Texture2D> texOutput = nullptr;
	winrt::com_ptr<ID3D11SamplerState> linearSampler = nullptr;

	winrt::com_ptr<ID3D11PixelShader> maskPS = nullptr;
	winrt::com_ptr<ID3D11PixelShader> blurPS = nullptr;
	winrt::com_ptr<ID3D11PixelShader> compositePS = nullptr;

	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	void CompileRasterShaders();

	virtual void RestoreDefaultSettings() override;
	virtual void LoadSettings(json&) override;
	virtual void SaveSettings(json&) override;

	virtual void DrawSettings() override;

	virtual void Draw(TextureInfo&) override;

private:
	struct Light
	{
		float3 color;
		RE::NiPoint3 direction;
		float billboardTan = 0.0f;
		float maskExponent = 4.0f;
		bool isSun = false;
	};

	bool SelectLight(RE::Sky* a_sky, Light& a_light) const;
	float3 GetRaysColor(const Light& a_light, float a_lightPeak, RE::Sky* a_sky) const;
	bool EnsureTargets(const D3D11_TEXTURE2D_DESC& a_sceneDesc);
};
