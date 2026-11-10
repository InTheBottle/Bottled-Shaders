#include "NativeMenu/NativeMenu.h"

#include "Features/Upscaling.h"
#include "Globals.h"
#include "I18n/I18n.h"

#include <algorithm>
#include <cmath>
#include <format>

#define I18N_KEY_PREFIX "feature.upscaling."

namespace
{
	using Method = Upscaling::UpscaleMethod;
	using Settings = Upscaling::Settings;

	constexpr float kSharpnessSteps = 10.0f;

	struct UpscalingRoot
	{
		static Settings& Live() { return globals::features::upscaling.settings; }
		static Settings Defaults() { return Settings{}; }
	};

	bool HasDLSS()
	{
		return globals::features::upscaling.streamline.featureDLSS;
	}

	uint& SelectedMethod()
	{
		auto& settings = UpscalingRoot::Live();
		return HasDLSS() ? settings.upscaleMethod : settings.upscaleMethodNoDLSS;
	}

	std::vector<std::string> MethodOptions()
	{
		std::vector<std::string> options{
			T(TKEY("method_none"), "None"),
			T(TKEY("method_taa"), "TAA"),
			"AMD FSR 3.1",
		};
		if (HasDLSS())
			options.emplace_back("NVIDIA DLSS");
		return options;
	}

	float __stdcall GetMethod()
	{
		return static_cast<float>(SelectedMethod());
	}

	void __stdcall SetMethod(float v)
	{
		const auto highest = HasDLSS() ? Method::kDLSS : Method::kFSR;
		SelectedMethod() = static_cast<uint>(std::clamp(v, 0.0f, static_cast<float>(highest)));
	}

	float DefaultMethod()
	{
		const auto defaults = UpscalingRoot::Defaults();
		return static_cast<float>(HasDLSS() ? defaults.upscaleMethod : defaults.upscaleMethodNoDLSS);
	}

	bool __stdcall IsUpscalerActive()
	{
		const auto method = globals::features::upscaling.GetUpscaleMethod();
		return method == Method::kFSR || method == Method::kDLSS;
	}

	std::vector<std::string> QualityOptions()
	{
		return {
			T(TKEY("preset_native_aa"), "Native AA"),
			T(TKEY("preset_quality"), "Quality"),
			T(TKEY("preset_balanced"), "Balanced"),
			T(TKEY("preset_performance"), "Performance"),
			T(TKEY("preset_ultra_performance"), "Ultra Performance"),
		};
	}

	std::vector<std::string> SharpnessOptions()
	{
		std::vector<std::string> options{ T("menu.features.disabled", "Disabled") };
		for (int step = 1; step <= static_cast<int>(kSharpnessSteps); ++step)
			options.push_back(std::format("{:.1f}", step / kSharpnessSteps));
		return options;
	}

	bool UsesDLSSSharpness()
	{
		return globals::features::upscaling.GetUpscaleMethod() == Method::kDLSS;
	}

	float __stdcall GetSharpness()
	{
		const auto& settings = UpscalingRoot::Live();
		const auto dlssSharpness = settings.sharpnessEnabledDLSS ? settings.sharpnessDLSS : 0.0f;
		return std::round((UsesDLSSSharpness() ? dlssSharpness : settings.sharpnessFSR) * kSharpnessSteps);
	}

	void __stdcall SetSharpness(float v)
	{
		auto& settings = UpscalingRoot::Live();
		const auto sharpness = v / kSharpnessSteps;
		if (UsesDLSSSharpness()) {
			settings.sharpnessDLSS = sharpness;
			settings.sharpnessEnabledDLSS = sharpness > 0.0f;
		} else {
			settings.sharpnessFSR = sharpness;
		}
	}
}

namespace NativeMenu
{
	std::vector<Row> UpscalingRows()
	{
		if (!globals::features::upscaling.loaded)
			return {};

		return {
			NATIVE_MENU_HEADING(T(TKEY("name"), "Upscaling")),

			Dropdown(T(TKEY("method"), "Method"), MethodOptions(), &GetMethod, &SetMethod, DefaultMethod(),
				T("native_menu.upscaling.method_desc", "Chooses how the image is upscaled and anti-aliased.")),

			Dropdown<UpscalingRoot, &Settings::qualityMode>(T(TKEY("upscale_preset"), "Upscale Preset"), QualityOptions(),
				T("native_menu.upscaling.preset_desc", "Lower presets render at a lower resolution for more performance."),
				&IsUpscalerActive),

			Dropdown(T(TKEY("sharpness"), "Sharpness"), SharpnessOptions(), &GetSharpness, &SetSharpness, 0.0f,
				T("native_menu.upscaling.sharpness_desc", "Sharpens the upscaled image."), &IsUpscalerActive),
		};
	}
}

#undef I18N_KEY_PREFIX
