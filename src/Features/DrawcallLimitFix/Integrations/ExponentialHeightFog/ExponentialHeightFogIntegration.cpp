// Exponential Height Fog: its integrated light scattering volume (t19), a frame texture. The render thread's callback reads the feature live.

#include "Features/DrawcallLimitFix/Integrations/ExponentialHeightFog/ExponentialHeightFog.h"

#include "Features/ExponentialHeightFog.h"

namespace DCLF::Integrations::ExponentialHeightFog
{
	namespace
	{
		// The view Prepass binds at t19 under its own conditions (every path past them ends in BindIntegratedLightScattering; otherwise
		// ReleaseVolumetricResources binds null). Prepass runs after the faces' commit, so they read last frame's volume, or the previous
		// texture's while Prepass remakes it (a grid change), held by their binding. Null without the feature or the volume: the captured view.
		bool VolumeBound()
		{
			const auto& fog = globals::features::exponentialHeightFog;
			const auto& settings = fog.settings;
			return fog.loaded && settings.enabled && settings.volumetricFogEnabled && settings.volumetricFogExtinctionScale > 0.0f;
		}

		void* LightScatteringView()
		{
			return VolumeBound() ? globals::features::exponentialHeightFog.IntegratedLightScatteringView() : nullptr;
		}

		void* FarLightScatteringView()
		{
			return VolumeBound() ? globals::features::exponentialHeightFog.IntegratedLightScatteringFarView() : nullptr;
		}

		void* FogIBLColorView()
		{
			const auto& fog = globals::features::exponentialHeightFog;
			return fog.loaded && fog.settings.enabled ? fog.FogIBLColorView() : nullptr;
		}

		void Install(Api::InstallContext& a_context)
		{
			a_context.RegisterFrameTexture({ .name = "exponential height fog light scattering", .textureRegister = kLightScatteringRegister,
				.view = &LightScatteringView, .capturedWhenNull = true });
			a_context.RegisterFrameTexture({ .name = "exponential height fog far light scattering", .textureRegister = kFarLightScatteringRegister,
				.view = &FarLightScatteringView, .capturedWhenNull = true });
			a_context.RegisterFrameTexture({ .name = "exponential height fog ibl colour", .textureRegister = kFogIBLColorRegister, .view = &FogIBLColorView,
				.capturedWhenNull = true });
		}
	}

	Api::Feature Integration()
	{
		return { "exponential height fog", &Install };
	}
}
