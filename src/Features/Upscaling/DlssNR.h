#pragma once

// DLSS 5 Neural Rendering, run over the render-resolution colour before DLSS super resolution.
//
// The model is NGX feature 18 in nvngx_dlssnr.dll, which NVIDIA ships in driver packages and documents
// nowhere. It refuses any caller whose module path does not contain "nvngx.dll", so every call goes
// through a forwarder named nvngx.dll_dlssnr.dll (tools/dlssnr_forwarder, from OptiScaler).
// The model is shown the finished frame and returns a complete picture; the composition pass
// (Data\Shaders\Upscaling\DlssNR.hlsl, OptiScaler's shader carrying RenoDX's colour design) blends
// that back over the untouched frame by luminance ratio, so detail and colour strength can be dialled
// without adding anything the model did not draw.
//
// Ported from UpscalingFrameGen_Hax (Fallout 4). The frame handed in is the DLSS output at display
// resolution in the game's kMAIN format; the resolve runs in passthrough (no white-point handling).

#include <d3d12.h>
#include <winrt/base.h>

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace DlssNR
{
	struct Settings
	{
		uint32_t performanceMode = 2;         ///< 0 full resolution model, 1 three quarters, 2 half (default), 3 one third
		uint32_t preset = 0;                  ///< DLSSNR.Hint.Render.Preset: 0 default (model decides), 1..3 presets; not the DLSS SR scale
		uint32_t style = 1;                   ///< DLSSNR.Style: 0 standard (strongest), 1 natural (default), 2 cinematic
		float intensity = 1.0f;               ///< DLSSNR.Intensity
		float localToneStrength = 1.0f;       ///< DLSSNR.LocalToneStrength
		float localStructureStrength = 1.0f;  ///< DLSSNR.LocalStructureStrength
		float skinStructureStrength = 1.0f;   ///< DLSSNR.SkinStructureStrength (the reference default); below 0 follows local structure
		uint32_t useAutoMask = 0;             ///< DLSSNR.UseAutoMask (automatic skin mask); off, as the reference implementation ships
		float detailStrength = 1.0f;          ///< how far the frame moves toward the model's picture
		float colourStrength = 1.0f;          ///< whether the model's colour arrives with its light
		float maxRatio = 2.0f;                ///< cap on per-pixel brightening/darkening ratio
		uint32_t debugView = 0;               ///< 0 off, 1 model input, 2 model output, 3 difference x20
		float editStability = 0.5f;           ///< reduced model only: how much of last frame's reprojected edit is kept (0 off .. 0.95)
		float motionScale = 1.0f;             ///< multiplier on the motion vector scale handed to the model and the stabiliser; 1 = normalised screen units
		uint32_t hdrEncode = 0;               ///< scene-linear (float) frames: 1 = tone map to an sRGB proxy for the model and decode back; 0 = hand the frame over as is (default, as the reference implementation does)
		float hdrWhitePoint = 1.0f;           ///< with hdrEncode: value shown to the model as white before the soft knee
	};

	NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
		Settings,
		performanceMode,
		preset,
		style,
		intensity,
		localToneStrength,
		localStructureStrength,
		skinStructureStrength,
		useAutoMask,
		detailStrength,
		colourStrength,
		maxRatio,
		debugView,
		editStability,
		motionScale,
		hdrEncode,
		hdrWhitePoint);

	/// Enable flag lives beside the settings so the pass can be toggled without touching tuning.
	void SetEnabled(bool a_enabled);
	void SetSettings(const Settings& a_settings);
	Settings GetSettings();
	bool IsEnabled();

	/// Folder holding nvngx_dlssnr.dll and nvngx.dll_dlssnr.dll (the Streamline runtime folder).
	void SetRuntimeDirectory(const std::wstring& a_directory);

	/// Queue the pass is submitted on; only used to read the GPU timestamp frequency for the cost readout.
	void SetCommandQueue(ID3D12CommandQueue* a_queue);

	/// Runs the model over a_color in place. a_color, a_depth and a_motion must be native (not
	/// Streamline proxy) resources in D3D12_RESOURCE_STATE_COMMON; they are left in COMMON. a_device and
	/// a_commandList must be native interfaces as well.
	void Run(
		ID3D12Device* a_device,
		ID3D12GraphicsCommandList* a_commandList,
		ID3D12Resource* a_color,
		ID3D12Resource* a_depth,
		ID3D12Resource* a_motion,
		uint32_t a_renderWidth,
		uint32_t a_renderHeight,
		bool a_reset,
		float a_mvScaleX,
		float a_mvScaleY,
		float a_jitterDeltaX = 0.0f,
		float a_jitterDeltaY = 0.0f);

	bool IsRunning();
	std::string FailureReason();
	void RetryAfterFailure();
	std::string Describe();
	/// Model resolution the pass is currently built for, for the UI.
	void GetModelSize(uint32_t& a_width, uint32_t& a_height);
	/// True when the last run encoded a scene-linear (float) frame for the model.
	bool IsEncodingHdr();
	/// GPU cost of the last measured run in milliseconds (model and composition); false until timing data exists.
	bool GetLastCostMs(float& a_totalMs, float& a_modelMs);
	void Shutdown();
}
