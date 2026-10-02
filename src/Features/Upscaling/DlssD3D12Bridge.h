#pragma once

// D3D11 to D3D12 bridge for the NVIDIA passes that only exist on D3D12.
//
// Two jobs share it:
//   1. DLSS super resolution when Streamline is bound to the D3D12 proxy device (the DLSS-G path).
//      The game's render-resolution colour, typed depth, dilated motion vectors and the two hint
//      masks are copied into shared textures, DLSS evaluates on a D3D12 command list, and the
//      display-resolution output is copied back into the D3D11 sharpener texture.
//   2. DLSS 5 Neural Rendering, which is an NGX D3D12 feature, on either DLSS path. It runs at
//      render resolution, one to one, on the colour input BEFORE super resolution (the FO4
//      reference order): the render-resolution rectangle of the game's colour is copied into a
//      shared texture of exactly that size, the model rewrites it in place, and DLSS then upscales
//      the result. With DLSS on D3D11 the rewritten rectangle is copied back into the game's colour
//      first.
//
// Modelled on FidelityFX::DispatchRuntimeUpscalerSingle: one shared fence pair, a small ring of
// command allocators/lists, and D3D11 waits on the D3D12 signal before it reads the output.

#include <array>
#include <cstdint>
#include <d3d11_4.h>
#include <d3d12.h>
#include <winrt/base.h>

class WrappedResource;

class DlssD3D12Bridge
{
public:
	struct Inputs
	{
		ID3D11Resource* dlssInput = nullptr;   ///< Game colour (kMAIN); its top-left render-resolution rectangle is the DLSS and NR input
		ID3D11Resource* output = nullptr;      ///< Display-resolution colour: DLSS output target (unused when only NR runs)
		ID3D11Resource* depth = nullptr;       ///< Typed R32_FLOAT depth (render-resolution region)
		ID3D11Resource* motionVectors = nullptr;
		ID3D11Resource* reactiveMask = nullptr;
		ID3D11Resource* transparencyMask = nullptr;
		uint32_t renderWidth = 0;
		uint32_t renderHeight = 0;
		uint32_t displayWidth = 0;
		uint32_t displayHeight = 0;
		bool hdr = false;
		bool evaluateDLSS = false;          ///< DLSS evaluates here (Streamline on D3D12); else the caller runs it on D3D11 afterwards
		bool runNeuralRendering = false;    ///< Neural rendering on the render-resolution colour before DLSS
		bool resetHistory = false;
		float jitterDeltaX = 0.0f;  ///< Current minus previous frame's render-resolution jitter, pixels; for the neural rendering model
		float jitterDeltaY = 0.0f;
	};

	~DlssD3D12Bridge();

	/** @brief True after the D3D12 device, fences and command ring exist. */
	bool IsReady() const;

	/** @brief Runs the requested passes for this frame. Returns true when the output texture holds a result. */
	bool Dispatch(const Inputs& a_inputs);

	/** @brief Drops shared textures (size or format change). The device and fences stay. */
	void ReleaseSharedResources(bool a_waitForIdle = true);

	/** @brief Full teardown. */
	void Destroy();

	/** @brief Set when the last dispatch faulted; the bridge refuses further work this session. */
	bool IsQuarantined() const { return quarantined; }
	const std::string& GetQuarantineReason() const { return quarantineReason; }

private:
	static constexpr uint32_t kCommandContextCount = 4;
	struct CommandContext
	{
		winrt::com_ptr<ID3D12CommandAllocator> allocator;
		winrt::com_ptr<ID3D12GraphicsCommandList4> list;
		uint64_t fenceValue = 0;
	};

	bool EnsureInterop();
	bool EnsureCommandContexts();
	CommandContext* AcquireCommandContext();
	bool WaitForFence(uint64_t a_value);
	void WaitForIdle();
	bool EnsureSharedResources(const Inputs& a_inputs);
	void Quarantine(const std::string& a_reason);

	/// A caller-owned D3D11 texture created with D3D11_RESOURCE_MISC_SHARED_NTHANDLE, opened on
	/// the D3D12 device so the bridge reads or writes it in place. Falls back to the copy path
	/// (resource stays null) when the texture is not shareable.
	struct Alias
	{
		ID3D11Resource* source = nullptr;
		winrt::com_ptr<ID3D12Resource> resource;
	};
	bool OpenAlias(Alias& a_alias, ID3D11Resource* a_source);
	Alias aliasOutput, aliasDepth, aliasMotion, aliasReactive, aliasTransparency;

	winrt::com_ptr<ID3D11Fence> d3d11Fence;
	winrt::com_ptr<ID3D12Fence> d3d12Fence;
	uint64_t fenceValue = 1;
	std::array<CommandContext, kCommandContextCount> commandContexts{};
	uint32_t commandContextCursor = 0;

	WrappedResource* colorInShared = nullptr;
	WrappedResource* colorOutShared = nullptr;
	WrappedResource* depthShared = nullptr;
	WrappedResource* motionShared = nullptr;
	WrappedResource* reactiveShared = nullptr;
	WrappedResource* transparencyShared = nullptr;

	D3D11_TEXTURE2D_DESC colorInDesc{};
	D3D11_TEXTURE2D_DESC colorOutDesc{};
	D3D11_TEXTURE2D_DESC depthDesc{};
	D3D11_TEXTURE2D_DESC motionDesc{};
	D3D11_TEXTURE2D_DESC reactiveDesc{};
	D3D11_TEXTURE2D_DESC transparencyDesc{};

	// GPU timing: three timestamps per command context (start, after DLSS, after neural rendering),
	// resolved into a readback buffer and read the next time that context is reused, when its fence
	// has completed. Optional: if the heap cannot be created the bridge runs unmeasured.
	static constexpr uint32_t kTimestampsPerContext = 3;
	winrt::com_ptr<ID3D12QueryHeap> queryHeap;
	winrt::com_ptr<ID3D12Resource> queryReadback;
	uint64_t timestampFrequency = 0;
	std::array<bool, kCommandContextCount> stampsPending{};
	void ReadTimestamps(uint32_t a_slot);

	bool quarantined = false;
	std::string quarantineReason;
};
