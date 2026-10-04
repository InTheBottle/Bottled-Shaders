#include "DlssD3D12Bridge.h"

#include <directx/d3dx12.h>

#include "../../State.h"
#include "../../Utils/FrameCosts.h"
#include "../Upscaling.h"
#include "DX12SwapChain.h"
#include "DlssNR.h"
#include "Streamline.h"

namespace
{
	bool TryGetTexture2DDesc(ID3D11Resource* a_resource, D3D11_TEXTURE2D_DESC& a_desc)
	{
		if (!a_resource)
			return false;
		winrt::com_ptr<ID3D11Texture2D> texture;
		if (FAILED(a_resource->QueryInterface(IID_PPV_ARGS(texture.put()))) || !texture)
			return false;
		texture->GetDesc(&a_desc);
		return true;
	}

	bool SameShape(const D3D11_TEXTURE2D_DESC& a_lhs, const D3D11_TEXTURE2D_DESC& a_rhs)
	{
		return a_lhs.Width == a_rhs.Width && a_lhs.Height == a_rhs.Height && a_lhs.Format == a_rhs.Format;
	}

	D3D11_TEXTURE2D_DESC SharedDesc(const D3D11_TEXTURE2D_DESC& a_source, UINT a_bindFlags)
	{
		D3D11_TEXTURE2D_DESC desc = a_source;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = 0;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = a_bindFlags;
		desc.CPUAccessFlags = 0;
		desc.MiscFlags = 0;
		return desc;
	}
}

DlssD3D12Bridge::~DlssD3D12Bridge()
{
	Destroy();
}

bool DlssD3D12Bridge::IsReady() const
{
	return !quarantined && d3d11Fence && d3d12Fence;
}

void DlssD3D12Bridge::Quarantine(const std::string& a_reason)
{
	if (!quarantined)
		logger::critical("[DLSS Bridge] {}; the D3D12 DLSS bridge will not be used again this session", a_reason);
	quarantined = true;
	quarantineReason = a_reason;
}

bool DlssD3D12Bridge::EnsureInterop()
{
	auto& swapChain = globals::features::upscaling.dx12SwapChain;

	if (!globals::d3d::device || !globals::d3d::context)
		return false;

	try {
		if (!swapChain.d3d11Device)
			swapChain.SetD3D11Device(globals::d3d::device);
		if (!swapChain.d3d11Context)
			swapChain.SetD3D11DeviceContext(globals::d3d::context);

		if (!swapChain.d3d12Device) {
			winrt::com_ptr<IDXGIDevice> dxgiDevice;
			DX::ThrowIfFailed(globals::d3d::device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put())));

			winrt::com_ptr<IDXGIAdapter> adapter;
			DX::ThrowIfFailed(dxgiDevice->GetAdapter(adapter.put()));
			swapChain.CreateD3D12Device(adapter.get());
		}

		if (!d3d12Fence || !d3d11Fence) {
			winrt::handle sharedFenceHandle;
			DX::ThrowIfFailed(swapChain.d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(d3d12Fence.put())));
			DX::ThrowIfFailed(swapChain.d3d12Device->CreateSharedHandle(d3d12Fence.get(), nullptr, GENERIC_ALL, nullptr, sharedFenceHandle.put()));
			DX::ThrowIfFailed(swapChain.d3d11Device->OpenSharedFence(sharedFenceHandle.get(), IID_PPV_ARGS(d3d11Fence.put())));
			fenceValue = 1;
			for (auto& commandContext : commandContexts)
				commandContext.fenceValue = 0;
			commandContextCursor = 0;
		}

		if (!EnsureCommandContexts())
			return false;

		if (!queryHeap && swapChain.commandQueue) {
			D3D12_QUERY_HEAP_DESC queryDesc{};
			queryDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
			queryDesc.Count = kCommandContextCount * kTimestampsPerContext;
			if (SUCCEEDED(swapChain.d3d12Device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(queryHeap.put())))) {
				const auto readbackDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(uint64_t) * queryDesc.Count);
				const auto readbackHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
				if (FAILED(swapChain.d3d12Device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(queryReadback.put()))) ||
					FAILED(swapChain.commandQueue->GetTimestampFrequency(&timestampFrequency)) || timestampFrequency == 0) {
					queryHeap = nullptr;
					queryReadback = nullptr;
					timestampFrequency = 0;
					logger::warn("[DLSS Bridge] GPU timestamps unavailable; the bridge runs unmeasured");
				}
			}
			stampsPending.fill(false);
		}

		DlssNR::SetCommandQueue(swapChain.commandQueue.get());
	} catch (const std::exception& e) {
		Quarantine(std::format("interop setup failed: {}", e.what()));
		return false;
	} catch (...) {
		Quarantine("interop setup failed with an unknown exception");
		return false;
	}

	return swapChain.d3d11Device.get() &&
	       swapChain.d3d11Context.get() &&
	       swapChain.d3d12Device.get() &&
	       swapChain.commandQueue.get() &&
	       d3d11Fence.get() &&
	       d3d12Fence.get();
}

bool DlssD3D12Bridge::EnsureCommandContexts()
{
	auto& swapChain = globals::features::upscaling.dx12SwapChain;
	if (!swapChain.d3d12Device)
		return false;

	try {
		for (auto& commandContext : commandContexts) {
			if (commandContext.allocator && commandContext.list)
				continue;

			commandContext.list = nullptr;
			commandContext.allocator = nullptr;
			commandContext.fenceValue = 0;

			DX::ThrowIfFailed(swapChain.d3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(commandContext.allocator.put())));
			DX::ThrowIfFailed(swapChain.d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandContext.allocator.get(), nullptr, IID_PPV_ARGS(commandContext.list.put())));
			DX::ThrowIfFailed(commandContext.list->Close());
		}
	} catch (const std::exception& e) {
		Quarantine(std::format("command context creation failed: {}", e.what()));
		return false;
	} catch (...) {
		Quarantine("command context creation failed with an unknown exception");
		return false;
	}

	return true;
}

DlssD3D12Bridge::CommandContext* DlssD3D12Bridge::AcquireCommandContext()
{
	if (!d3d12Fence || !EnsureCommandContexts())
		return nullptr;

	const uint64_t completedValue = d3d12Fence->GetCompletedValue();
	const uint32_t count = static_cast<uint32_t>(commandContexts.size());
	for (uint32_t i = 0; i < count; ++i) {
		const uint32_t index = (commandContextCursor + i) % count;
		auto& commandContext = commandContexts[index];
		if (!commandContext.allocator || !commandContext.list)
			continue;
		if (commandContext.fenceValue != 0 && completedValue < commandContext.fenceValue)
			continue;

		commandContext.fenceValue = 0;
		commandContextCursor = (index + 1) % count;
		return &commandContext;
	}

	uint32_t waitIndex = count;
	uint64_t waitValue = std::numeric_limits<uint64_t>::max();
	for (uint32_t i = 0; i < count; ++i) {
		auto& commandContext = commandContexts[i];
		if (!commandContext.allocator || !commandContext.list || commandContext.fenceValue == 0)
			continue;
		if (commandContext.fenceValue < waitValue) {
			waitValue = commandContext.fenceValue;
			waitIndex = i;
		}
	}

	if (waitIndex == count || !WaitForFence(waitValue))
		return nullptr;

	auto& commandContext = commandContexts[waitIndex];
	commandContext.fenceValue = 0;
	commandContextCursor = (waitIndex + 1) % count;
	return &commandContext;
}

bool DlssD3D12Bridge::WaitForFence(uint64_t a_value)
{
	if (!d3d12Fence || a_value == 0)
		return true;
	if (d3d12Fence->GetCompletedValue() >= a_value)
		return true;

	winrt::handle fenceEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr));
	if (!fenceEvent)
		return false;
	if (FAILED(d3d12Fence->SetEventOnCompletion(a_value, fenceEvent.get())))
		return false;

	return WaitForSingleObject(fenceEvent.get(), 5000) == WAIT_OBJECT_0;
}

void DlssD3D12Bridge::WaitForIdle()
{
	uint64_t highest = 0;
	for (auto& commandContext : commandContexts)
		highest = std::max(highest, commandContext.fenceValue);
	if (highest)
		WaitForFence(highest);
	for (auto& commandContext : commandContexts)
		commandContext.fenceValue = 0;
}

void DlssD3D12Bridge::ReleaseSharedResources(bool a_waitForIdle)
{
	if (a_waitForIdle)
		WaitForIdle();

	for (WrappedResource** resource : { &colorInShared, &colorOutShared, &depthShared, &motionShared, &reactiveShared, &transparencyShared }) {
		delete *resource;
		*resource = nullptr;
	}
	for (Alias* alias : { &aliasOutput, &aliasDepth, &aliasMotion, &aliasReactive, &aliasTransparency })
		*alias = {};
	colorInDesc = {};
	colorOutDesc = {};
	depthDesc = {};
	motionDesc = {};
	reactiveDesc = {};
	transparencyDesc = {};
}

void DlssD3D12Bridge::Destroy()
{
	ReleaseSharedResources(true);
	for (auto& commandContext : commandContexts) {
		commandContext.list = nullptr;
		commandContext.allocator = nullptr;
		commandContext.fenceValue = 0;
	}
	d3d11Fence = nullptr;
	d3d12Fence = nullptr;
	queryHeap = nullptr;
	queryReadback = nullptr;
	timestampFrequency = 0;
	stampsPending.fill(false);
}

void DlssD3D12Bridge::ReadTimestamps(uint32_t a_slot)
{
	// Called once the context's fence has completed, so the resolved stamps are final.
	if (!stampsPending[a_slot] || !queryReadback || timestampFrequency == 0)
		return;
	stampsPending[a_slot] = false;
	const D3D12_RANGE range{ sizeof(uint64_t) * a_slot * kTimestampsPerContext, sizeof(uint64_t) * (a_slot + 1) * kTimestampsPerContext };
	void* mapped = nullptr;
	if (FAILED(queryReadback->Map(0, &range, &mapped)) || !mapped)
		return;
	const auto* stamps = reinterpret_cast<const uint64_t*>(static_cast<const uint8_t*>(mapped) + range.Begin);
	const double toMs = 1000.0 / static_cast<double>(timestampFrequency);
	if (stamps[1] >= stamps[0] && stamps[2] >= stamps[1]) {
		FrameCosts::dlssEvalGpuMs.Set(static_cast<float>(static_cast<double>(stamps[2] - stamps[1]) * toMs));
		FrameCosts::dlssBridgeGpuMs.Set(static_cast<float>(static_cast<double>(stamps[2] - stamps[0]) * toMs));
	}
	const D3D12_RANGE nothing{ 0, 0 };
	queryReadback->Unmap(0, &nothing);
}

bool DlssD3D12Bridge::OpenAlias(Alias& a_alias, ID3D11Resource* a_source)
{
	if (a_alias.source == a_source && a_alias.resource)
		return true;
	a_alias = {};
	a_alias.source = a_source;
	if (!a_source)
		return false;

	D3D11_TEXTURE2D_DESC desc{};
	if (!TryGetTexture2DDesc(a_source, desc) || !(desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE))
		return false;

	auto& swapChain = globals::features::upscaling.dx12SwapChain;
	if (!swapChain.d3d12Device)
		return false;

	winrt::com_ptr<IDXGIResource1> dxgiResource;
	if (FAILED(a_source->QueryInterface(IID_PPV_ARGS(dxgiResource.put()))) || !dxgiResource)
		return false;
	HANDLE sharedHandle = nullptr;
	if (FAILED(dxgiResource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &sharedHandle)) || !sharedHandle)
		return false;
	const HRESULT opened = swapChain.d3d12Device->OpenSharedHandle(sharedHandle, IID_PPV_ARGS(a_alias.resource.put()));
	CloseHandle(sharedHandle);
	if (FAILED(opened)) {
		a_alias.resource = nullptr;
		logger::warn("[DLSS Bridge] Could not open a shareable texture on D3D12 (0x{:08X}); copying it instead", static_cast<uint32_t>(opened));
		return false;
	}
	return true;
}

bool DlssD3D12Bridge::EnsureSharedResources(const Inputs& a_inputs)
{
	auto& swapChain = globals::features::upscaling.dx12SwapChain;
	if (!swapChain.d3d11Device || !swapChain.d3d12Device)
		return false;

	D3D11_TEXTURE2D_DESC outputDesc{};
	D3D11_TEXTURE2D_DESC inputDesc{};
	D3D11_TEXTURE2D_DESC depthSourceDesc{};
	D3D11_TEXTURE2D_DESC motionSourceDesc{};
	if (!TryGetTexture2DDesc(a_inputs.output, outputDesc) ||
		!TryGetTexture2DDesc(a_inputs.depth, depthSourceDesc) ||
		!TryGetTexture2DDesc(a_inputs.motionVectors, motionSourceDesc)) {
		return false;
	}
	if (!TryGetTexture2DDesc(a_inputs.dlssInput, inputDesc))
		return false;
	// Only the render-resolution rectangle is ever read, so the shared input is exactly that size:
	// the copy in is a fraction of a display-sized one, and the neural rendering pass and DLSS see
	// a texture whose whole extent is valid.
	inputDesc.Width = std::min<UINT>(inputDesc.Width, a_inputs.renderWidth);
	inputDesc.Height = std::min<UINT>(inputDesc.Height, a_inputs.renderHeight);

	D3D11_TEXTURE2D_DESC reactiveSourceDesc{};
	D3D11_TEXTURE2D_DESC transparencySourceDesc{};
	const bool hasMasks = TryGetTexture2DDesc(a_inputs.reactiveMask, reactiveSourceDesc) && TryGetTexture2DDesc(a_inputs.transparencyMask, transparencySourceDesc);

	// Caller textures created shareable are used in place; anything else goes through a copy.
	OpenAlias(aliasOutput, a_inputs.evaluateDLSS ? a_inputs.output : nullptr);
	OpenAlias(aliasDepth, a_inputs.depth);
	OpenAlias(aliasMotion, a_inputs.motionVectors);
	OpenAlias(aliasReactive, hasMasks ? a_inputs.reactiveMask : nullptr);
	OpenAlias(aliasTransparency, hasMasks ? a_inputs.transparencyMask : nullptr);

	const auto wantedColorIn = SharedDesc(inputDesc, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
	const auto wantedColorOut = SharedDesc(outputDesc, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
	const auto wantedDepth = SharedDesc(depthSourceDesc, D3D11_BIND_SHADER_RESOURCE);
	const auto wantedMotion = SharedDesc(motionSourceDesc, D3D11_BIND_SHADER_RESOURCE);
	const auto wantedReactive = hasMasks ? SharedDesc(reactiveSourceDesc, D3D11_BIND_SHADER_RESOURCE) : D3D11_TEXTURE2D_DESC{};
	const auto wantedTransparency = hasMasks ? SharedDesc(transparencySourceDesc, D3D11_BIND_SHADER_RESOURCE) : D3D11_TEXTURE2D_DESC{};

	const bool needOut = a_inputs.evaluateDLSS && !aliasOutput.resource;
	const bool needDepth = !aliasDepth.resource;
	const bool needMotion = !aliasMotion.resource;
	const bool needReactive = hasMasks && !aliasReactive.resource;
	const bool needTransparency = hasMasks && !aliasTransparency.resource;

	const auto ok = [](WrappedResource* a_resource, const D3D11_TEXTURE2D_DESC& a_have, const D3D11_TEXTURE2D_DESC& a_want, bool a_needed) {
		return !a_needed || (a_resource && SameShape(a_have, a_want));
	};
	const bool matches =
		colorInShared && SameShape(colorInDesc, wantedColorIn) &&
		ok(colorOutShared, colorOutDesc, wantedColorOut, needOut) &&
		ok(depthShared, depthDesc, wantedDepth, needDepth) &&
		ok(motionShared, motionDesc, wantedMotion, needMotion) &&
		ok(reactiveShared, reactiveDesc, wantedReactive, needReactive) &&
		ok(transparencyShared, transparencyDesc, wantedTransparency, needTransparency);
	if (matches)
		return true;

	// Aliases survive the wrapped-resource rebuild: they belong to the caller's textures.
	const Alias keepOut = aliasOutput, keepDepth = aliasDepth, keepMotion = aliasMotion, keepReactive = aliasReactive, keepTransparency = aliasTransparency;
	ReleaseSharedResources(true);
	aliasOutput = keepOut;
	aliasDepth = keepDepth;
	aliasMotion = keepMotion;
	aliasReactive = keepReactive;
	aliasTransparency = keepTransparency;

	try {
		colorInShared = new WrappedResource(wantedColorIn, swapChain.d3d11Device.get(), swapChain.d3d12Device.get(), "DlssBridge::ColorIn");
		if (needOut)
			colorOutShared = new WrappedResource(wantedColorOut, swapChain.d3d11Device.get(), swapChain.d3d12Device.get(), "DlssBridge::ColorOut");
		if (needDepth)
			depthShared = new WrappedResource(wantedDepth, swapChain.d3d11Device.get(), swapChain.d3d12Device.get(), "DlssBridge::Depth");
		if (needMotion)
			motionShared = new WrappedResource(wantedMotion, swapChain.d3d11Device.get(), swapChain.d3d12Device.get(), "DlssBridge::MotionVectors");
		if (needReactive)
			reactiveShared = new WrappedResource(wantedReactive, swapChain.d3d11Device.get(), swapChain.d3d12Device.get(), "DlssBridge::ReactiveMask");
		if (needTransparency)
			transparencyShared = new WrappedResource(wantedTransparency, swapChain.d3d11Device.get(), swapChain.d3d12Device.get(), "DlssBridge::TransparencyMask");
	} catch (const std::exception& e) {
		ReleaseSharedResources(false);
		Quarantine(std::format("shared resource creation failed: {}", e.what()));
		return false;
	} catch (...) {
		ReleaseSharedResources(false);
		Quarantine("shared resource creation failed with an unknown exception");
		return false;
	}

	colorInDesc = wantedColorIn;
	colorOutDesc = wantedColorOut;
	depthDesc = wantedDepth;
	motionDesc = wantedMotion;
	reactiveDesc = wantedReactive;
	transparencyDesc = wantedTransparency;

	logger::info("[DLSS Bridge] Shared resources: color in {}x{} fmt {} (copied) -> out {}x{} fmt {} ({}), depth {}, mvec {}, masks {}{}",
		wantedColorIn.Width, wantedColorIn.Height, static_cast<uint32_t>(wantedColorIn.Format),
		wantedColorOut.Width, wantedColorOut.Height, static_cast<uint32_t>(wantedColorOut.Format), aliasOutput.resource ? "in place" : "copied",
		aliasDepth.resource ? "in place" : "copied", aliasMotion.resource ? "in place" : "copied",
		hasMasks ? "present" : "absent", hasMasks ? (aliasReactive.resource && aliasTransparency.resource ? " (in place)" : " (copied)") : "");
	return true;
}

bool DlssD3D12Bridge::Dispatch(const Inputs& a_inputs)
{
	if (quarantined)
		return false;
	if (!a_inputs.output || !a_inputs.depth || !a_inputs.motionVectors)
		return false;
	if (!a_inputs.evaluateDLSS && !a_inputs.runNeuralRendering)
		return false;
	if (!a_inputs.dlssInput)
		return false;
	if (!a_inputs.renderWidth || !a_inputs.renderHeight || !a_inputs.displayWidth || !a_inputs.displayHeight)
		return false;

	if (!EnsureInterop())
		return false;
	if (!EnsureSharedResources(a_inputs))
		return false;

	auto& swapChain = globals::features::upscaling.dx12SwapChain;
	auto& streamline = globals::features::upscaling.streamline;

	FrameCosts::AccumulatingScope bridgeScope(FrameCosts::dlssBridgeCpuMs);

	auto* commandContext = AcquireCommandContext();
	if (!commandContext)
		return false;

	auto* commandAllocator = commandContext->allocator.get();
	auto* commandList = commandContext->list.get();
	if (!commandAllocator || !commandList)
		return false;

	const uint32_t timingSlot = static_cast<uint32_t>(commandContext - commandContexts.data());
	ReadTimestamps(timingSlot);
	const auto stamp = [&](uint32_t a_index) {
		if (queryHeap)
			commandList->EndQuery(queryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, timingSlot * kTimestampsPerContext + a_index);
	};

	bool producedOutput = false;
	bool commandListSubmitted = false;
	bool commandFenceTracked = false;
	try {
		auto* context = swapChain.d3d11Context.get();

		const D3D11_BOX renderBox{ 0, 0, 0, colorInDesc.Width, colorInDesc.Height, 1 };
		context->CopySubresourceRegion(colorInShared->resource11, 0, 0, 0, 0, a_inputs.dlssInput, 0, &renderBox);
		if (!aliasDepth.resource)
			context->CopyResource(depthShared->resource11, a_inputs.depth);
		if (!aliasMotion.resource)
			context->CopyResource(motionShared->resource11, a_inputs.motionVectors);
		const bool hasMasks = a_inputs.reactiveMask && a_inputs.transparencyMask &&
		                      (aliasReactive.resource || reactiveShared) && (aliasTransparency.resource || transparencyShared);
		if (hasMasks) {
			if (!aliasReactive.resource)
				context->CopyResource(reactiveShared->resource11, a_inputs.reactiveMask);
			if (!aliasTransparency.resource)
				context->CopyResource(transparencyShared->resource11, a_inputs.transparencyMask);
		}
		ID3D12Resource* depth12 = aliasDepth.resource ? aliasDepth.resource.get() : depthShared->resource.get();
		ID3D12Resource* motion12 = aliasMotion.resource ? aliasMotion.resource.get() : motionShared->resource.get();
		ID3D12Resource* reactive12 = hasMasks ? (aliasReactive.resource ? aliasReactive.resource.get() : reactiveShared->resource.get()) : nullptr;
		ID3D12Resource* transparency12 = hasMasks ? (aliasTransparency.resource ? aliasTransparency.resource.get() : transparencyShared->resource.get()) : nullptr;
		ID3D12Resource* out12 = aliasOutput.resource ? aliasOutput.resource.get() : (colorOutShared ? colorOutShared->resource.get() : nullptr);

		const uint64_t d3d11SubmitFence = fenceValue++;
		DX::ThrowIfFailed(context->Signal(d3d11Fence.get(), d3d11SubmitFence));
		DX::ThrowIfFailed(swapChain.commandQueue->Wait(d3d12Fence.get(), d3d11SubmitFence));

		DX::ThrowIfFailed(commandAllocator->Reset());
		DX::ThrowIfFailed(commandList->Reset(commandAllocator, nullptr));
		stamp(0);

		if (a_inputs.runNeuralRendering) {
			// NGX needs the native device and list, not Streamline's proxies. The model rewrites the
			// render-resolution colour in place; DLSS (here or on D3D11) then upscales that.
			ID3D12GraphicsCommandList* nativeList = commandList;
			winrt::com_ptr<IUnknown> nativeOwner;
			if (streamline.UsesD3D12() && streamline.slGetNativeInterface) {
				void* native = nullptr;
				if (streamline.slGetNativeInterface(commandList, &native) == sl::Result::eOk && native) {
					nativeOwner.attach(static_cast<IUnknown*>(native));
					nativeList = static_cast<ID3D12GraphicsCommandList*>(native);
				}
			}
			DlssNR::Run(swapChain.d3d12Device.get(), nativeList,
				colorInShared->resource.get(), depth12, motion12,
				colorInDesc.Width, colorInDesc.Height, a_inputs.resetHistory, 1.0f, 1.0f, a_inputs.jitterDeltaX, a_inputs.jitterDeltaY);
			// Order the model's UAV writes before DLSS reads the same texture.
			const auto nrToSr = CD3DX12_RESOURCE_BARRIER::UAV(colorInShared->resource.get());
			commandList->ResourceBarrier(1, &nrToSr);
		}
		stamp(1);

		bool dlssOk = true;
		if (a_inputs.evaluateDLSS) {
			dlssOk = out12 && streamline.UpscaleD3D12(commandList,
				colorInShared->resource.get(), out12, depth12,
				motion12,
				reactive12,
				transparency12,
				colorInDesc.Width, colorInDesc.Height, a_inputs.displayWidth, a_inputs.displayHeight, a_inputs.hdr);
		}
		stamp(2);
		if (queryHeap && queryReadback) {
			commandList->ResolveQueryData(queryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, timingSlot * kTimestampsPerContext, kTimestampsPerContext, queryReadback.get(), sizeof(uint64_t) * timingSlot * kTimestampsPerContext);
			stampsPending[timingSlot] = true;
		}
		DX::ThrowIfFailed(commandList->Close());

		ID3D12CommandList* commandListsToExecute[] = { commandList };
		const uint64_t d3d12SubmitFence = fenceValue++;
		swapChain.commandQueue->ExecuteCommandLists(1, commandListsToExecute);
		commandListSubmitted = true;
		DX::ThrowIfFailed(swapChain.commandQueue->Signal(d3d12Fence.get(), d3d12SubmitFence));
		commandContext->fenceValue = d3d12SubmitFence;
		commandFenceTracked = true;
		DX::ThrowIfFailed(context->Wait(d3d11Fence.get(), d3d12SubmitFence));

		if (dlssOk && a_inputs.evaluateDLSS) {
			// With a shareable output DLSS wrote straight into the caller's texture.
			if (!aliasOutput.resource)
				context->CopyResource(a_inputs.output, colorOutShared->resource11);
			producedOutput = true;
		} else if (dlssOk) {
			// DLSS runs on D3D11 next: hand it the neurally rendered rectangle.
			context->CopySubresourceRegion(a_inputs.dlssInput, 0, 0, 0, 0, colorInShared->resource11, 0, nullptr);
			producedOutput = true;
		}

		if (!dlssOk) {
			const HRESULT deviceRemovedReason = swapChain.d3d12Device ? swapChain.d3d12Device->GetDeviceRemovedReason() : S_OK;
			if (FAILED(deviceRemovedReason))
				Quarantine(std::format("D3D12 device removed: 0x{:08X}", static_cast<uint32_t>(deviceRemovedReason)));
		}
	} catch (const std::exception& e) {
		if (!commandListSubmitted) {
			commandContext->fenceValue = 0;
		} else if (!commandFenceTracked) {
			try {
				const uint64_t rescueFence = fenceValue++;
				DX::ThrowIfFailed(swapChain.commandQueue->Signal(d3d12Fence.get(), rescueFence));
				commandContext->fenceValue = rescueFence;
			} catch (...) {
				commandContext->fenceValue = 0;
			}
		}
		producedOutput = false;
		Quarantine(std::format("dispatch threw: {}", e.what()));
	} catch (...) {
		if (!commandListSubmitted) {
			commandContext->fenceValue = 0;
		} else if (!commandFenceTracked) {
			try {
				const uint64_t rescueFence = fenceValue++;
				DX::ThrowIfFailed(swapChain.commandQueue->Signal(d3d12Fence.get(), rescueFence));
				commandContext->fenceValue = rescueFence;
			} catch (...) {
				commandContext->fenceValue = 0;
			}
		}
		producedOutput = false;
		Quarantine("dispatch threw an unknown exception");
	}

	return producedOutput;
}
