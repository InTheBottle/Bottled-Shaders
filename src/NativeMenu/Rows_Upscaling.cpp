#include "NativeMenu/NativeMenu.h"

#include "Features/Upscaling.h"
#include "Features/Upscaling/Streamline.h"
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
		return Streamline::GetSingleton()->IsDLSSSupported();
	}

	std::vector<Method> AvailableMethods()
	{
		std::vector<Method> methods{ Method::kNONE, Method::kTAA, Method::kFSR };
		if (Streamline::GetSingleton()->IsXeSSSupported())
			methods.push_back(Method::kXeSS);
		if (HasDLSS())
			methods.push_back(Method::kDLSS);
		return methods;
	}

	uint& SelectedMethod()
	{
		auto& settings = UpscalingRoot::Live();
		return HasDLSS() ? settings.upscaleMethod : settings.upscaleMethodNoDLSS;
	}

	float IndexOf(Method a_method)
	{
		const auto methods = AvailableMethods();
		const auto it = std::ranges::find(methods, a_method);
		return static_cast<float>(it != methods.end() ? std::distance(methods.begin(), it) : 0);
	}

	std::vector<std::string> MethodOptions()
	{
		std::vector<std::string> options;
		for (const auto method : AvailableMethods()) {
			switch (method) {
			case Method::kNONE:
				options.emplace_back(T(TKEY("method_none"), "None"));
				break;
			case Method::kTAA:
				options.emplace_back(T(TKEY("method_taa"), "TAA"));
				break;
			case Method::kFSR:
				options.emplace_back("AMD FSR");
				break;
			case Method::kXeSS:
				options.emplace_back("Intel XeSS");
				break;
			case Method::kDLSS:
				options.emplace_back("NVIDIA DLSS");
				break;
			}
		}
		return options;
	}

	float __stdcall GetMethod()
	{
		return IndexOf(static_cast<Method>(SelectedMethod()));
	}

	void __stdcall SetMethod(float v)
	{
		const auto methods = AvailableMethods();
		const auto index = static_cast<size_t>(std::clamp(v, 0.0f, static_cast<float>(methods.size() - 1)));
		SelectedMethod() = static_cast<uint>(methods[index]);
	}

	float DefaultMethod()
	{
		const auto defaults = UpscalingRoot::Defaults();
		return IndexOf(static_cast<Method>(HasDLSS() ? defaults.upscaleMethod : defaults.upscaleMethodNoDLSS));
	}

	bool __stdcall IsUpscalerActive()
	{
		const auto method = globals::features::upscaling.GetUpscaleMethod();
		return method == Method::kFSR || method == Method::kXeSS || method == Method::kDLSS;
	}

	std::vector<std::string> QualityOptions()
	{
		return {
			T(TKEY("preset_native"), "Native"),
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

	float __stdcall GetSharpness()
	{
		return std::round(UpscalingRoot::Live().sharpnessFSR * kSharpnessSteps);
	}

	void __stdcall SetSharpness(float v)
	{
		UpscalingRoot::Live().sharpnessFSR = v / kSharpnessSteps;
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
