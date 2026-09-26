#include "State.h"
#include "DX12SwapChain.h"

#include <FidelityFX/api/include/dx12/ffx_api_dx12.hpp>
#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include "../HDRDisplay.h"
#include "../Upscaling.h"
#include "FidelityFX.h"
#include "RTX40MFG/MfgUnlock.h"
#include "Streamline.h"
#include "Utils/D3D.h"
#include "Utils/FrameCosts.h"

namespace
{
	bool IsStreamlineProxy(Streamline& a_streamline, const char* a_name, IUnknown* a_interface)
	{
		if (!a_streamline.UsesD3D12() || !a_streamline.slGetNativeInterface || !a_interface)
			return false;

		void* nativeInterface = nullptr;
		if (SL_FAILED(result, a_streamline.slGetNativeInterface(a_interface, &nativeInterface))) {
			logger::warn("[DX12SwapChain] slGetNativeInterface({}) failed: {}", a_name, magic_enum::enum_name(result));
			return false;
		}

		const bool isProxy = nativeInterface && nativeInterface != a_interface;
		logger::info("[DX12SwapChain] Streamline proxy check {} proxy={} interface={} native={}", a_name, isProxy, static_cast<void*>(a_interface), nativeInterface);
		if (nativeInterface)
			static_cast<IUnknown*>(nativeInterface)->Release();
		return isProxy;
	}

	winrt::com_ptr<ID3DBlob> CompileEmbeddedShader(const char* a_source, const char* a_entry, const char* a_target)
	{
		winrt::com_ptr<ID3DBlob> shader;
		winrt::com_ptr<ID3DBlob> errors;
		const auto result = D3DCompile(a_source, std::strlen(a_source), nullptr, nullptr, nullptr, a_entry, a_target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, shader.put(), errors.put());
		if (FAILED(result) && errors)
			logger::warn("[DX12SwapChain] UI composite shader compile failed: {}", static_cast<const char*>(errors->GetBufferPointer()));
		DX::ThrowIfFailed(result);
		return shader;
	}

	// The UI buffer is drawn onto a cleared target with the game's alpha blending, which is what the
	// FidelityFX path composites with its premultiplied-alpha flag; the same blend is used here.
	constexpr const char* kUICompositeShader = R"(
Texture2D hudless : register(t0);
Texture2D ui : register(t1);
SamplerState pointSampler : register(s0);

struct PSInput
{
	float4 position : SV_POSITION;
	float2 uv : TEXCOORD0;
};

PSInput VSMain(uint vertexId : SV_VertexID)
{
	float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
	PSInput output;
	output.position = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
	output.uv = uv;
	return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
	const float4 scene = hudless.Sample(pointSampler, input.uv);
	const float4 overlay = ui.Sample(pointSampler, input.uv);
	return float4(overlay.rgb + scene.rgb * (1.0f - overlay.a), 1.0f);
}
)";
}

void DX12SwapChain::CreateD3D12Device(IDXGIAdapter* a_adapter)
{
	if (d3d12Device)
		return;

	DX::ThrowIfFailed(D3D12CreateDevice(a_adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d3d12Device)));

	auto& streamline = globals::features::upscaling.streamline;
	proxyD3D12Device = d3d12Device;

	if (streamline.UsesD3D12()) {
		// The unlock verifies the adapter (Ada only) from this device and must patch sl.dlss_g.dll and
		// nvngx_dlssg.dll BEFORE Streamline binds the device: the wrapper computes and caches
		// numFramesToGenerateMax while binding, so a patch applied afterwards changes nothing this session.
		if (MfgUnlock::IsEnabled()) {
			MfgUnlock::ObserveD3D12Device(d3d12Device.get());
			MfgUnlock::Rescan(true);
		}

		if (streamline.slSetD3DDevice) {
			if (SL_FAILED(result, streamline.slSetD3DDevice(d3d12Device.get())))
				logger::warn("[DX12SwapChain] slSetD3DDevice(D3D12) failed: {}", magic_enum::enum_name(result));
		}

		if (streamline.slUpgradeInterface) {
			ID3D12Device* deviceForQueue = d3d12Device.get();
			if (SL_FAILED(result, streamline.slUpgradeInterface(reinterpret_cast<void**>(&deviceForQueue)))) {
				logger::warn("[DX12SwapChain] Could not upgrade D3D12 device for Streamline: {}", magic_enum::enum_name(result));
			} else if (deviceForQueue && deviceForQueue != d3d12Device.get()) {
				proxyD3D12Device.attach(deviceForQueue);
			}
		}
	}

	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
	queueDesc.NodeMask = 0;

	DX::ThrowIfFailed(proxyD3D12Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&commandQueue)));
	logger::info("[DX12SwapChain] D3D12 command queue created via {} device", proxyD3D12Device.get() == d3d12Device.get() ? "native" : "Streamline proxy");
	if (streamline.UsesD3D12())
		IsStreamlineProxy(streamline, "commandQueue", commandQueue.get());

	for (UINT i = 0; i < kBackBufferCount; i++) {
		DX::ThrowIfFailed(d3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocators[i])));
		DX::ThrowIfFailed(d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocators[i].get(), nullptr, IID_PPV_ARGS(&commandLists[i])));
		commandLists[i]->Close();
	}
}

void DX12SwapChain::CreateSwapChain(IDXGIAdapter* adapter, DXGI_SWAP_CHAIN_DESC a_swapChainDesc)
{
	CreateD3D12Device(adapter);

	IDXGIFactory4* dxgiFactory;
	DX::ThrowIfFailed(adapter->GetParent(IID_PPV_ARGS(&dxgiFactory)));

	// Runtime format negotiation for swap chain
	DXGI_FORMAT attemptedFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
	DXGI_FORMAT negotiatedFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
	bool fallbackUsed = false;

	// Test R10G10B10A2 support for HDR capability
	D3D12_FEATURE_DATA_FORMAT_SUPPORT formatSupport = { DXGI_FORMAT_R10G10B10A2_UNORM, D3D12_FORMAT_SUPPORT1_RENDER_TARGET, D3D12_FORMAT_SUPPORT2_NONE };
	if (SUCCEEDED(d3d12Device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &formatSupport, sizeof(formatSupport)))) {
		if ((formatSupport.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) == 0) {
			logger::warn("[DX12SwapChain] R10G10B10A2_UNORM not supported as render target, falling back to R8G8B8A8_UNORM");
			negotiatedFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
			fallbackUsed = true;
		}
	} else {
		logger::warn("[DX12SwapChain] CheckFeatureSupport failed for R10G10B10A2_UNORM, falling back to R8G8B8A8_UNORM");
		negotiatedFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
		fallbackUsed = true;
	}

	logger::info("[DX12SwapChain] Swap chain format negotiation: attempted={}, negotiated={}, fallback={}",
		static_cast<uint32_t>(attemptedFormat),
		static_cast<uint32_t>(negotiatedFormat),
		fallbackUsed ? "true" : "false");

	swapChainDesc = {};
	swapChainDesc.Width = a_swapChainDesc.BufferDesc.Width;
	swapChainDesc.Height = a_swapChainDesc.BufferDesc.Height;
	swapChainDesc.Format = negotiatedFormat;
	swapChainDesc.SampleDesc.Count = 1;
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.BufferCount = kBackBufferCount;
	swapChainDesc.SwapEffect = a_swapChainDesc.SwapEffect;
	swapChainDesc.Flags = a_swapChainDesc.Flags;

	auto& upscaling = globals::features::upscaling;
	auto& streamline = upscaling.streamline;
	auto& fidelityFX = upscaling.fidelityFX;

	swapChainIsStreamlineProxy = false;
	outputWindow = a_swapChainDesc.OutputWindow;

	if (useDlssgSwapChain) {
		// DLSS-G intercepts Present on a swap chain created through the Streamline proxied factory.
		IDXGIFactory4* factoryForSwapChain = dxgiFactory;
		winrt::com_ptr<IDXGIFactory4> upgradedFactory;
		if (streamline.UsesD3D12() && streamline.slUpgradeInterface) {
			if (SL_FAILED(result, streamline.slUpgradeInterface(reinterpret_cast<void**>(&factoryForSwapChain)))) {
				logger::warn("[DX12SwapChain] Could not upgrade DXGI factory for Streamline: {}", magic_enum::enum_name(result));
				factoryForSwapChain = dxgiFactory;
			}
		}
		if (factoryForSwapChain == dxgiFactory)
			upgradedFactory.copy_from(dxgiFactory);
		else
			upgradedFactory.attach(factoryForSwapChain);

		winrt::com_ptr<IDXGISwapChain1> swapChain1;
		DX::ThrowIfFailed(upgradedFactory->CreateSwapChainForHwnd(commandQueue.get(), a_swapChainDesc.OutputWindow, &swapChainDesc, nullptr, nullptr, swapChain1.put()));
		DX::ThrowIfFailed(swapChain1->QueryInterface(IID_PPV_ARGS(&swapChain)));

		swapChainIsStreamlineProxy = IsStreamlineProxy(streamline, "swapChain", swapChain);
		if (!swapChainIsStreamlineProxy && streamline.UsesD3D12() && streamline.slUpgradeInterface) {
			IDXGISwapChain* upgradedSwapChain = swapChain;
			if (SL_FAILED(result, streamline.slUpgradeInterface(reinterpret_cast<void**>(&upgradedSwapChain)))) {
				logger::warn("[DX12SwapChain] Could not upgrade swap chain for Streamline: {}", magic_enum::enum_name(result));
			} else if (upgradedSwapChain && upgradedSwapChain != swapChain) {
				IDXGISwapChain4* upgradedSwapChain4 = nullptr;
				DX::ThrowIfFailed(upgradedSwapChain->QueryInterface(IID_PPV_ARGS(&upgradedSwapChain4)));
				upgradedSwapChain->Release();
				swapChain->Release();
				swapChain = upgradedSwapChain4;
				swapChainIsStreamlineProxy = IsStreamlineProxy(streamline, "swapChain.afterUpgrade", swapChain);
			}
		}
		if (!swapChainIsStreamlineProxy)
			logger::warn("[DX12SwapChain] D3D12 swap chain is not a Streamline proxy; DLSS-G present interception will not run");
		else
			logger::info("[DX12SwapChain] DLSS-G swap chain created through the Streamline proxy");
	} else {
		ffx::CreateContextDescFrameGenerationSwapChainForHwndDX12 ffxSwapChainDesc{};

		ffxSwapChainDesc.desc = &swapChainDesc;
		ffxSwapChainDesc.dxgiFactory = dxgiFactory;
		ffxSwapChainDesc.fullscreenDesc = nullptr;
		ffxSwapChainDesc.gameQueue = commandQueue.get();
		ffxSwapChainDesc.hwnd = a_swapChainDesc.OutputWindow;
		ffxSwapChainDesc.swapchain = &swapChain;

		if (ffx::CreateContext(fidelityFX.swapChainContext, nullptr, ffxSwapChainDesc) != ffx::ReturnCode::Ok) {
			logger::critical("[FidelityFX] Failed to create swap chain context!");
		}
	}

	for (UINT i = 0; i < kBackBufferCount; i++)
		DX::ThrowIfFailed(swapChain->GetBuffer(i, IID_PPV_ARGS(&swapChainBuffers[i])));

	frameIndex = swapChain->GetCurrentBackBufferIndex();

	// Set color space based on HDR Display feature state and negotiated format
	auto* hdr = globals::features::hdrDisplay.loaded ? &globals::features::hdrDisplay : nullptr;
	bool enableHDR = hdr && hdr->settings.enableHDR;
	// Only set HDR color space if not falling back to SDR format
	SetColorSpace(enableHDR && !fallbackUsed);

	if (!useDlssgSwapChain)
		fidelityFX.SetupFrameGeneration();
}

void DX12SwapChain::CreateInterop()
{
	HANDLE sharedFenceHandle;
	DX::ThrowIfFailed(d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&d3d12Fence)));
	DX::ThrowIfFailed(d3d12Device->CreateSharedHandle(d3d12Fence.get(), nullptr, GENERIC_ALL, nullptr, &sharedFenceHandle));
	DX::ThrowIfFailed(d3d11Device->OpenSharedFence(sharedFenceHandle, IID_PPV_ARGS(&d3d11Fence)));
	CloseHandle(sharedFenceHandle);

	swapChainProxy = new DXGISwapChainProxy(swapChain);

	RecreateWrappedResources(swapChainDesc);
}

void DX12SwapChain::RecreateWrappedResources(const DXGI_SWAP_CHAIN_DESC1& desc)
{
	D3D11_TEXTURE2D_DESC texDesc11{};
	texDesc11.Width = desc.Width;
	texDesc11.Height = desc.Height;
	texDesc11.MipLevels = 1;
	texDesc11.ArraySize = 1;
	texDesc11.Format = desc.Format;
	texDesc11.SampleDesc.Count = 1;
	texDesc11.SampleDesc.Quality = 0;
	texDesc11.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;

	// Build both replacements before releasing the active resources so a failed
	// allocation cannot leave the proxy with only half of its interop textures.
	auto newSwapChainBuffer = std::make_unique<WrappedResource>(texDesc11, d3d11Device.get(), d3d12Device.get(), "DX12SwapChain::SwapChainBuffer");

	// UI buffer uses R8G8B8A8_UNORM - vanilla UI is SDR and 8-bit precision
	texDesc11.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	auto newUiBuffer = std::make_unique<WrappedResource>(texDesc11, d3d11Device.get(), d3d12Device.get(), "DX12SwapChain::UIBuffer");

	delete swapChainBufferWrapped;
	delete uiBufferWrapped;
	swapChainBufferWrapped = newSwapChainBuffer.release();
	uiBufferWrapped = newUiBuffer.release();

	const float clearColor[4]{};
	d3d11Context->ClearRenderTargetView(swapChainBufferWrapped->rtv, clearColor);
	d3d11Context->ClearRenderTargetView(uiBufferWrapped->rtv, clearColor);
}

DXGISwapChainProxy* DX12SwapChain::GetSwapChainProxy()
{
	return swapChainProxy;
}

void DX12SwapChain::SetD3D11Device(ID3D11Device* a_d3d11Device)
{
	DX::ThrowIfFailed(a_d3d11Device->QueryInterface(IID_PPV_ARGS(&d3d11Device)));
}

void DX12SwapChain::SetD3D11DeviceContext(ID3D11DeviceContext* a_d3d11Context)
{
	DX::ThrowIfFailed(a_d3d11Context->QueryInterface(IID_PPV_ARGS(&d3d11Context)));
}

HRESULT DX12SwapChain::GetBuffer(UINT buffer, REFIID riid, void** ppSurface)
{
	if (!ppSurface)
		return E_POINTER;

	*ppSurface = nullptr;
	if (buffer != 0 || !swapChainBufferWrapped || !swapChainBufferWrapped->resource11)
		return DXGI_ERROR_INVALID_CALL;

	// IDXGISwapChain::GetBuffer returns an owned COM reference. Returning the raw
	// pointer here let the caller's Release destroy the shared texture while the
	// D3D12 side still retained and submitted its corresponding resource.
	return swapChainBufferWrapped->resource11->QueryInterface(riid, ppSurface);
}

HRESULT DX12SwapChain::ResizeBuffers(UINT bufferCount, UINT width, UINT height, DXGI_FORMAT format, UINT flags)
{
	if (!swapChain)
		return DXGI_ERROR_INVALID_CALL;

	// The DLSS-G guide requires frame generation off before any window manipulation.
	if (useDlssgSwapChain)
		globals::features::upscaling.streamline.DisableDLSSG();

	// DXGI defines zero as "preserve the current buffer count". FidelityFX's
	// frame-generation swap-chain stores the supplied value verbatim and uses it
	// as its replacement-buffer count, so forwarding zero leaves it with no valid
	// source resource at the next Present.
	// The proxy owns its buffer count: the game asks for its own two (or zero, meaning keep), and
	// the real chain keeps kBackBufferCount whatever it asks.
	const UINT effectiveBufferCount = kBackBufferCount;
	if (bufferCount && bufferCount != kBackBufferCount) {
		static bool loggedCount = false;
		if (!loggedCount) {
			loggedCount = true;
			logger::info("[DX12SwapChain] Game asked for {} buffers on resize; the proxy keeps {}", bufferCount, kBackBufferCount);
		}
	}

	// These references are to the swap chain buffers. They must not keep
	// the old generation alive across the resize, and must be refreshed
	// before CS records another copy.
	for (auto& buffer : swapChainBuffers)
		buffer = nullptr;
	const HRESULT result = swapChain->ResizeBuffers(effectiveBufferCount, width, height, format, flags);
	if (FAILED(result))
		return result;

	DXGI_SWAP_CHAIN_DESC1 resizedDesc{};
	const HRESULT descResult = swapChain->GetDesc1(&resizedDesc);
	if (FAILED(descResult))
		return descResult;

	const bool wrappedResourcesChanged = resizedDesc.Width != swapChainDesc.Width ||
	                                     resizedDesc.Height != swapChainDesc.Height ||
	                                     resizedDesc.Format != swapChainDesc.Format;
	if (wrappedResourcesChanged)
		RecreateWrappedResources(resizedDesc);
	swapChainDesc = resizedDesc;

	for (UINT i = 0; i < kBackBufferCount; i++)
		DX::ThrowIfFailed(swapChain->GetBuffer(i, IID_PPV_ARGS(swapChainBuffers[i].put())));
	frameIndex = swapChain->GetCurrentBackBufferIndex();
	return S_OK;
}

bool DX12SwapChain::EnsureUIComposite()
{
	if (uiCompositePipeline && uiCompositeFormat == swapChainDesc.Format)
		return true;

	uiCompositeRootSignature = nullptr;
	uiCompositePipeline = nullptr;
	uiCompositeSrvHeap = nullptr;
	uiCompositeRtvHeap = nullptr;
	uiCompositeFormat = DXGI_FORMAT_UNKNOWN;

	try {
		D3D12_DESCRIPTOR_RANGE range{};
		range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		range.NumDescriptors = 2;
		range.BaseShaderRegister = 0;
		range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		D3D12_ROOT_PARAMETER rootParameter{};
		rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		rootParameter.DescriptorTable.NumDescriptorRanges = 1;
		rootParameter.DescriptorTable.pDescriptorRanges = &range;
		rootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

		D3D12_STATIC_SAMPLER_DESC sampler{};
		sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
		sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		sampler.ShaderRegister = 0;
		sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

		D3D12_ROOT_SIGNATURE_DESC rootDesc{};
		rootDesc.NumParameters = 1;
		rootDesc.pParameters = &rootParameter;
		rootDesc.NumStaticSamplers = 1;
		rootDesc.pStaticSamplers = &sampler;

		winrt::com_ptr<ID3DBlob> rootBlob;
		winrt::com_ptr<ID3DBlob> rootError;
		DX::ThrowIfFailed(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, rootBlob.put(), rootError.put()));
		DX::ThrowIfFailed(d3d12Device->CreateRootSignature(0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(), IID_PPV_ARGS(uiCompositeRootSignature.put())));

		auto vertexShader = CompileEmbeddedShader(kUICompositeShader, "VSMain", "vs_5_0");
		auto pixelShader = CompileEmbeddedShader(kUICompositeShader, "PSMain", "ps_5_0");

		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
		psoDesc.pRootSignature = uiCompositeRootSignature.get();
		psoDesc.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
		psoDesc.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
		psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		psoDesc.DepthStencilState.DepthEnable = FALSE;
		psoDesc.DepthStencilState.StencilEnable = FALSE;
		psoDesc.SampleMask = UINT_MAX;
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		psoDesc.NumRenderTargets = 1;
		psoDesc.RTVFormats[0] = swapChainDesc.Format;
		psoDesc.SampleDesc.Count = 1;
		DX::ThrowIfFailed(d3d12Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(uiCompositePipeline.put())));

		D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
		srvHeapDesc.NumDescriptors = kBackBufferCount * 2;  // two SRVs per back buffer slot
		srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		DX::ThrowIfFailed(d3d12Device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(uiCompositeSrvHeap.put())));

		D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
		rtvHeapDesc.NumDescriptors = kBackBufferCount;  // one RTV per back buffer slot
		rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		DX::ThrowIfFailed(d3d12Device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(uiCompositeRtvHeap.put())));
	} catch (const std::exception& e) {
		logger::error("[DX12SwapChain] UI composite resources could not be created: {}", e.what());
		uiCompositeRootSignature = nullptr;
		uiCompositePipeline = nullptr;
		uiCompositeSrvHeap = nullptr;
		uiCompositeRtvHeap = nullptr;
		return false;
	}

	uiCompositeFormat = swapChainDesc.Format;
	return true;
}

void DX12SwapChain::CompositeUI(ID3D12GraphicsCommandList* a_commandList, ID3D12Resource* a_backBuffer, ID3D12Resource* a_hudless, ID3D12Resource* a_ui, uint32_t a_slot)
{
	if (!EnsureUIComposite())
		return;

	const auto srvIncrement = d3d12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	auto srvCpu = uiCompositeSrvHeap->GetCPUDescriptorHandleForHeapStart();
	srvCpu.ptr += static_cast<SIZE_T>(a_slot) * 2 * srvIncrement;
	auto srvGpu = uiCompositeSrvHeap->GetGPUDescriptorHandleForHeapStart();
	srvGpu.ptr += static_cast<UINT64>(a_slot) * 2 * srvIncrement;

	for (ID3D12Resource* resource : { a_hudless, a_ui }) {
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = resource->GetDesc().Format;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Texture2D.MipLevels = 1;
		d3d12Device->CreateShaderResourceView(resource, &srvDesc, srvCpu);
		srvCpu.ptr += srvIncrement;
	}

	const auto rtvIncrement = d3d12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	auto rtv = uiCompositeRtvHeap->GetCPUDescriptorHandleForHeapStart();
	rtv.ptr += static_cast<SIZE_T>(a_slot) * rtvIncrement;
	d3d12Device->CreateRenderTargetView(a_backBuffer, nullptr, rtv);

	a_commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
	ID3D12DescriptorHeap* heaps[] = { uiCompositeSrvHeap.get() };
	a_commandList->SetDescriptorHeaps(1, heaps);
	a_commandList->SetGraphicsRootSignature(uiCompositeRootSignature.get());
	a_commandList->SetPipelineState(uiCompositePipeline.get());
	a_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	a_commandList->SetGraphicsRootDescriptorTable(0, srvGpu);
	D3D12_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(swapChainDesc.Width), static_cast<float>(swapChainDesc.Height), 0.0f, 1.0f };
	D3D12_RECT scissor{ 0, 0, static_cast<LONG>(swapChainDesc.Width), static_cast<LONG>(swapChainDesc.Height) };
	a_commandList->RSSetViewports(1, &viewport);
	a_commandList->RSSetScissorRects(1, &scissor);
	a_commandList->DrawInstanced(3, 1, 0, 0);
}

HRESULT DX12SwapChain::Present(UINT SyncInterval, UINT Flags)
{
	// Scale UI brightness BEFORE fence sync so the D3D11 UIBrightnessCS dispatch
	// is covered by the D3D11->D3D12 fence. Without this, the compositor may read
	// uiBufferWrapped on D3D12 before the PQ encoding completes on D3D11.
	// Only runs when HDR Display feature is loaded (UIBrightnessCS may not exist otherwise)
	auto* hdr = globals::features::hdrDisplay.loaded ? &globals::features::hdrDisplay : nullptr;
	if (hdr)
		hdr->ScaleUIBrightnessForFG();

	bool isHDR = hdr && hdr->settings.enableHDR;

	// Wait for D3D11 to finish (includes ApplyHDR scene encoding AND UIBrightnessCS)
	fenceValue++;
	DX::ThrowIfFailed(d3d11Context->Signal(d3d11Fence.get(), fenceValue));
	DX::ThrowIfFailed(commandQueue->Wait(d3d12Fence.get(), fenceValue));

	// New frame, reset
	if (frameFenceValues[frameIndex])
		DX::ThrowIfFailed(d3d12Fence->SetEventOnCompletion(frameFenceValues[frameIndex], nullptr));
	DX::ThrowIfFailed(commandAllocators[frameIndex]->Reset());
	DX::ThrowIfFailed(commandLists[frameIndex]->Reset(commandAllocators[frameIndex].get(), nullptr));

	if (useDlssgSwapChain)
		return PresentDlssg(SyncInterval, Flags, isHDR);
	return PresentFidelityFX(SyncInterval, Flags, isHDR);
}

HRESULT DX12SwapChain::PresentFidelityFX(UINT SyncInterval, UINT Flags, bool a_isHDR)
{
	auto& upscaling = globals::features::upscaling;

	// Copy shared texture to swap chain buffer
	{
		auto fakeSwapChain = swapChainBufferWrapped->resource.get();
		auto realSwapChain = swapChainBuffers[frameIndex].get();
		{
			std::vector<D3D12_RESOURCE_BARRIER> barriers;
			barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(fakeSwapChain, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE));
			barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(realSwapChain, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST));
			commandLists[frameIndex]->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
		}

		commandLists[frameIndex]->CopyResource(realSwapChain, fakeSwapChain);

		{
			std::vector<D3D12_RESOURCE_BARRIER> barriers;
			barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(fakeSwapChain, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON));
			barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(realSwapChain, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT));
			commandLists[frameIndex]->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
		}
	}

	{
		FrameCosts::AccumulatingScope setupScope(FrameCosts::frameGenSetupMs);
		upscaling.fidelityFX.Present(upscaling.ShouldUseFrameGenerationThisFrame(), a_isHDR);
	}

	DX::ThrowIfFailed(commandLists[frameIndex]->Close());

	ID3D12CommandList* commandListsToExecute[] = { commandLists[frameIndex].get() };
	commandQueue->ExecuteCommandLists(1, commandListsToExecute);

	auto& streamline = upscaling.streamline;
	streamline.OnPresentStart();
	// Present the frame
	DX::ThrowIfFailed(swapChain->Present(SyncInterval, Flags));
	streamline.OnPresentEnd();

	// Wait for D3D12 to finish
	fenceValue++;
	DX::ThrowIfFailed(commandQueue->Signal(d3d12Fence.get(), fenceValue));
	frameFenceValues[frameIndex] = fenceValue;
	DX::ThrowIfFailed(d3d11Context->Wait(d3d11Fence.get(), fenceValue));

	// Update the frame index
	frameIndex = swapChain->GetCurrentBackBufferIndex();

	float clearColor[4]{ 0, 0, 0, 0 };
	d3d11Context->ClearRenderTargetView(uiBufferWrapped->rtv, clearColor);

	// If VSync is disabled, use frame limiter to prevent tearing and optimise pacing
	if (SyncInterval == 0)
		upscaling.FrameLimiter();

	return S_OK;
}

HRESULT DX12SwapChain::PresentDlssg(UINT SyncInterval, UINT Flags, bool)
{
	auto& upscaling = globals::features::upscaling;
	auto& streamline = upscaling.streamline;
	auto* commandList = commandLists[frameIndex].get();

	auto hudless = swapChainBufferWrapped->resource.get();
	auto ui = uiBufferWrapped->resource.get();
	auto backBuffer = swapChainBuffers[frameIndex].get();

	const bool frameGenerationThisFrame = upscaling.ShouldUseFrameGenerationThisFrame();
	// While frame generation composites, the UI lives in the UI buffer and the wrapped
	// back buffer is the HUD-less scene; otherwise the wrapped buffer already carries the UI.
	const bool compositeUI = frameGenerationThisFrame;

	// Scene (HUD-less) to the real back buffer
	{
		D3D12_RESOURCE_BARRIER barriers[] = {
			CD3DX12_RESOURCE_BARRIER::Transition(hudless, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST)
		};
		commandList->ResourceBarrier(static_cast<UINT>(std::size(barriers)), barriers);
	}
	commandList->CopyResource(backBuffer, hudless);

	if (compositeUI) {
		D3D12_RESOURCE_BARRIER barriers[] = {
			CD3DX12_RESOURCE_BARRIER::Transition(hudless, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(backBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET),
			CD3DX12_RESOURCE_BARRIER::Transition(ui, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
		};
		commandList->ResourceBarrier(static_cast<UINT>(std::size(barriers)), barriers);

		CompositeUI(commandList, backBuffer, hudless, ui, frameIndex);

		D3D12_RESOURCE_BARRIER after[] = {
			CD3DX12_RESOURCE_BARRIER::Transition(hudless, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
			CD3DX12_RESOURCE_BARRIER::Transition(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT),
			CD3DX12_RESOURCE_BARRIER::Transition(ui, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON)
		};
		commandList->ResourceBarrier(static_cast<UINT>(std::size(after)), after);
	} else {
		D3D12_RESOURCE_BARRIER after[] = {
			CD3DX12_RESOURCE_BARRIER::Transition(hudless, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
			CD3DX12_RESOURCE_BARRIER::Transition(backBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT)
		};
		commandList->ResourceBarrier(static_cast<UINT>(std::size(after)), after);
	}

	if (!loggedPresentParameters) {
		loggedPresentParameters = true;
		logger::info("[DX12SwapChain] Game presents with SyncInterval={} Flags=0x{:X}; swap chain flags=0x{:X}", SyncInterval, Flags, swapChainDesc.Flags);
	}

	// DLSS-G options and input tags for this frame. After a menu the conditions only have to hold
	// for two frames before DLSS-G comes back (the reference implementation's figure). After a
	// window event (minimise, occlusion, a failed present) the settle is longer, since re-enabling
	// on the first frame after an alt-tab is what produced a stuck swap chain, and a resume whose
	// first present fails doubles the wait before the next attempt instead of retrying forever.
	const auto& settings = upscaling.settings;
	// A latch or a stepped-down multiplier belongs to the settings it was taken under. Changing
	// the generated-frame count or toggling frame generation is the user asking for another go.
	if (dlssgLatchSettingGenerated != settings.dlssgGeneratedFrames || dlssgLatchSettingMode != settings.frameGenerationMode) {
		if (dlssgFailureLatched || dlssgGeneratedFramesCap)
			logger::info("[DX12SwapChain] Frame generation settings changed; clearing the present-failure latch and the multiplier cap");
		dlssgLatchSettingGenerated = settings.dlssgGeneratedFrames;
		dlssgLatchSettingMode = settings.frameGenerationMode;
		dlssgFailureLatched = false;
		dlssgGeneratedFramesCap = 0;
		dlssgResumeFailures = 0;
		dlssgPresentFailures = 0;
	}
	// Coming back to the foreground lifts a failure latch: the failures it counted were presents
	// the DLSS-G presenter rejected while the game was alt-tabbed away. So does time: a latch
	// is a pause, not a verdict on the session.
	const bool windowActive = upscaling.windowActive.load(std::memory_order_relaxed);
	if (windowActive && !dlssgLastWindowActive && (dlssgFailureLatched || dlssgResumeFailures)) {
		logger::info("[DX12SwapChain] Application is in the foreground again; clearing the DLSS-G present-failure latch");
		dlssgFailureLatched = false;
		dlssgResumeFailures = 0;
		dlssgPresentFailures = 0;
	}
	dlssgLastWindowActive = windowActive;
	constexpr uint64_t kDlssgLatchRetryMs = 30000;
	if (dlssgFailureLatched && GetTickCount64() - dlssgLatchedAtTick >= kDlssgLatchRetryMs) {
		logger::info("[DX12SwapChain] Retrying DLSS-G after the {} s latch", kDlssgLatchRetryMs / 1000);
		dlssgFailureLatched = false;
		dlssgResumeFailures = 0;
		dlssgPresentFailures = 0;
	}
	const bool windowUnavailable = presentOccluded || !upscaling.windowFocused.load(std::memory_order_relaxed) || !windowActive;
	const bool dlssgConditions = frameGenerationThisFrame && streamline.featureDLSSG && swapChainIsStreamlineProxy &&
	                             depthBufferShared12 && motionVectorBufferShared12 && !presentOccluded && !dlssgFailureLatched;
	if (!dlssgConditions && (windowUnavailable || dlssgResumeFailures))
		dlssgDroppedByWindowEvent = true;  // cleared once DLSS-G is wanted again
	const uint32_t kDlssgResumeStableFrames = (dlssgDroppedByWindowEvent ? 30u : 2u) << std::min(dlssgResumeFailures, 4u);
	if (dlssgConditions)
		dlssgStableFrames = std::min(dlssgStableFrames + 1, kDlssgResumeStableFrames + 1);
	else
		dlssgStableFrames = 0;
	const bool wantDLSSG = dlssgConditions && (streamline.dlssgActive || dlssgStableFrames > kDlssgResumeStableFrames);
	if (wantDLSSG)
		dlssgDroppedByWindowEvent = false;
	if (!loggedDlssgWantedKnown || loggedDlssgWanted != wantDLSSG) {
		// Log every transition with the reason, so a log alone explains why frame generation
		// dropped out (alt-tab, a menu, occlusion, a failed present) and when it came back.
		loggedDlssgWantedKnown = true;
		loggedDlssgWanted = wantDLSSG;
		std::string reason;
		if (!wantDLSSG) {
			if (!settings.frameGenerationMode)
				reason += "frame generation disabled in settings; ";
			if (!upscaling.windowFocused.load(std::memory_order_relaxed))
				reason += "window minimised; ";
			if (!windowActive)
				reason += "another application is in the foreground; ";
			if (upscaling.IsFrameGenerationBlockedByMenu())
				reason += "full-screen menu (loading, main, map or skills); ";
			if (presentOccluded)
				reason += "present occluded; ";
			if (dlssgFailureLatched)
				reason += "latched off after repeated present failures; ";
			if (!streamline.featureDLSSG)
				reason += "DLSS-G feature unavailable; ";
			if (reason.empty())
				reason = dlssgConditions ? std::format("settling ({} of {} stable frames)", dlssgStableFrames, kDlssgResumeStableFrames) : "conditions not met";
		}
		logger::info("[DX12SwapChain] DLSS-G {}{}", wantDLSSG ? "resuming" : "off: ", reason);
	}
	bool dlssgThisFrameTagged = false;
	{
	FrameCosts::AccumulatingScope setupScope(FrameCosts::frameGenSetupMs);
	bool dlssgThisFrame = false;
	if (wantDLSSG && streamline.PrepareDLSSGPresent()) {
		const float2 screenSize{ static_cast<float>(globals::game::graphicsState->screenWidth), static_cast<float>(globals::game::graphicsState->screenHeight) };
		const auto renderSize = screenSize * upscaling.resolutionScale;
		const auto renderWidth = std::max(1u, static_cast<uint32_t>(renderSize.x));
		const auto renderHeight = std::max(1u, static_cast<uint32_t>(renderSize.y));

		D3D11_TEXTURE2D_DESC mvecDesc{};
		motionVectorBufferShared12->resource11->GetDesc(&mvecDesc);
		D3D11_TEXTURE2D_DESC depthDesc{};
		depthBufferShared12->resource11->GetDesc(&depthDesc);

		uint32_t generatedFrames = settings.dlssgGeneratedFrames + 1;
		if (dlssgGeneratedFramesCap)
			generatedFrames = std::min(generatedFrames, dlssgGeneratedFramesCap);
		if (streamline.UpdateDLSSG(true, generatedFrames, settings.dynamicMFGEnabled && !dynamicMFGBlocked, settings.dynamicMFGTargetFPS,
				renderWidth, renderHeight, swapChainDesc.Width, swapChainDesc.Height,
				swapChainDesc.Format, mvecDesc.Format, depthDesc.Format, swapChainDesc.BufferCount) &&
			streamline.dlssgActive) {
			dlssgThisFrame = streamline.TagDLSSGResources(commandList, hudless, depthBufferShared12->resource.get(), motionVectorBufferShared12->resource.get(),
				renderWidth, renderHeight, swapChainDesc.Width, swapChainDesc.Height);
		}
	}
	if (!dlssgThisFrame) {
		streamline.DisableDLSSG();
		if (streamline.NeedsDLSSGPresentSafety())
			streamline.ClearDLSSGResourceTags(commandList, swapChainDesc.Width, swapChainDesc.Height);
	}
	dlssgThisFrameTagged = dlssgThisFrame;
	}

	DX::ThrowIfFailed(commandList->Close());

	ID3D12CommandList* commandListsToExecute[] = { commandList };
	commandQueue->ExecuteCommandLists(1, commandListsToExecute);

	// Measured behaviour of the DLSS-G proxy swap chain: while frame generation is on it wants
	// SyncInterval 0 (it paces the presents itself) and no tearing flag; while it is off, a
	// SyncInterval 0 present is rejected with DXGI_ERROR_INVALID_CALL on every frame and only
	// the game's own interval goes through. So the interval follows DLSS-G's state exactly,
	// with no override during the frames right after it switches off.
	const bool dlssgPresentSafety = streamline.NeedsDLSSGPresentSafety();
	const bool dlssgPresenting = streamline.dlssgActive && dlssgThisFrameTagged;
	const UINT presentSyncInterval = dlssgPresenting ? 0u : SyncInterval;
	const UINT presentFlags = dlssgPresenting ? (Flags & ~DXGI_PRESENT_ALLOW_TEARING) : Flags;
	if (dlssgPresenting != loggedVsyncSupport) {
		loggedVsyncSupport = dlssgPresenting;
		logger::info("[DX12SwapChain] Presenting with SyncInterval {} flags 0x{:X} (DLSS-G {})", presentSyncInterval, presentFlags, dlssgPresenting ? "on" : "off");
	}

	streamline.OnPresentStart();
	const HRESULT result = swapChain->Present(presentSyncInterval, presentFlags);
	streamline.OnPresentEnd();
	// An occluded window (minimised, alt-tabbed away) must not keep DLSS-G running: the
	// guide warns of deadlocks around window manipulation while frame generation is on.
	const bool occludedNow = result == DXGI_STATUS_OCCLUDED;
	if (occludedNow != presentOccluded) {
		presentOccluded = occludedNow;
		logger::info("[DX12SwapChain] Present {} occluded", occludedNow ? "became" : "no longer");
		if (occludedNow)
			streamline.DisableDLSSG();
	}
	if (FAILED(result)) {
		const HRESULT removedReason = d3d12Device ? d3d12Device->GetDeviceRemovedReason() : S_OK;
		++dlssgPresentFailures;
		if (dlssgPresentFailures <= 10 || (dlssgPresentFailures % 300) == 0) {
			logger::error("[DX12SwapChain] Present failed result=0x{:08X} d3d12Removed=0x{:08X} sync={} flags=0x{:X} frameIndex={} dlssgActive={} failures={}",
				static_cast<uint32_t>(result), static_cast<uint32_t>(removedReason), presentSyncInterval, presentFlags, frameIndex, streamline.dlssgActive, dlssgPresentFailures);
		}
		// Recovery: DLSS-G off with its resources released, and a settle period before it may
		// return. If plain presents keep failing with DLSS-G off the runtime is wedged; leave it off.
		const uint32_t failedGenerated = streamline.currentGeneratedFrames();
		streamline.DisableDLSSG(true);
		dlssgStableFrames = 0;
		if (dlssgPresenting && !windowActive) {
			// Expected: the presenter rejects presents while another application is in front.
			dlssgDroppedByWindowEvent = true;
		} else if (dlssgPresenting) {
			++dlssgResumeFailures;
			dlssgResumeSuccessFrames = 0;
			if (settings.dynamicMFGEnabled && !dynamicMFGBlocked && dlssgResumeFailures >= 2) {
				dynamicMFGBlocked = true;
				dlssgResumeFailures = 0;
				logger::warn("[DX12SwapChain] The runtime rejects presents in dynamic MFG mode; using the fixed multiplier for the rest of this session");
			} else if (dlssgResumeFailures >= 3 && SUCCEEDED(removedReason) && failedGenerated > 1) {
				// The runtime accepts a lower multiplier more often than none at all: step down one
				// generated frame and try again before giving up on frame generation.
				dlssgGeneratedFramesCap = failedGenerated - 1;
				dlssgResumeFailures = 0;
				logger::warn("[DX12SwapChain] DLSS-G rejects presents at {}x; trying {}x", failedGenerated + 1, dlssgGeneratedFramesCap + 1);
			} else if (dlssgResumeFailures >= 6 && !dlssgFailureLatched && SUCCEEDED(removedReason)) {
				dlssgFailureLatched = true;
				dlssgLatchedAtTick = GetTickCount64();
				logger::error("[DX12SwapChain] DLSS-G fails on every resume; frame generation is off for 30 s or until the window or its settings change");
			}
		}
		if (dlssgPresentFailures >= 60 && !dlssgFailureLatched && SUCCEEDED(removedReason)) {
			dlssgFailureLatched = true;
			dlssgLatchedAtTick = GetTickCount64();
			logger::error("[DX12SwapChain] DLSS-G present keeps failing; frame generation is off for 30 s or until the window or its settings change");
		}
	} else if (dlssgPresentFailures) {
		logger::info("[DX12SwapChain] Present recovered after {} failures", dlssgPresentFailures);
		dlssgPresentFailures = 0;
	}

	if (SUCCEEDED(result) && dlssgPresenting && ++dlssgResumeSuccessFrames >= 30)
		dlssgResumeFailures = 0;

	// Wait for D3D12 to finish. This runs even after a failed present so the D3D11 side never
	// waits on a fence value that was never signalled and the frame index keeps tracking.
	fenceValue++;
	DX::ThrowIfFailed(commandQueue->Signal(d3d12Fence.get(), fenceValue));
	frameFenceValues[frameIndex] = fenceValue;
	DX::ThrowIfFailed(d3d11Context->Wait(d3d11Fence.get(), fenceValue));

	// Update the frame index (DLSS-G requires GetCurrentBackBufferIndex on its proxy each frame)
	frameIndex = swapChain->GetCurrentBackBufferIndex();

	if (FAILED(result))
		return result;

	if (dlssgPresentSafety)
		streamline.QueryDLSSGState("post-present");

	// The wrapped buffers are cleared by the present hook after the screenshot capture has read them.

	// DLSS-G paces its own presents; otherwise the frame limiter keeps the swap chain in step when
	// the game itself presents unthrottled
	if (!dlssgPresenting && presentSyncInterval == 0)
		upscaling.FrameLimiter();

	return S_OK;
}

HRESULT DX12SwapChain::GetDevice(REFIID uuid, void** ppDevice)
{
	if (uuid == __uuidof(ID3D11Device) || uuid == __uuidof(ID3D11Device1) || uuid == __uuidof(ID3D11Device2) || uuid == __uuidof(ID3D11Device3) || uuid == __uuidof(ID3D11Device4) || uuid == __uuidof(ID3D11Device5)) {
		*ppDevice = d3d11Device.get();
		return S_OK;
	}

	return swapChain->GetDevice(uuid, ppDevice);
}

HANDLE DX12SwapChain::GetFrameLatencyWaitableObject()
{
	return swapChain->GetFrameLatencyWaitableObject();
}

float DX12SwapChain::GetFrameTime() const
{
	// Calculate frame time based on swap chain presentation
	static float lastPresentTime = 0.0f;
	static float frameTime = 1.0f / 60.0f;  // Default to 60 fps
	static LARGE_INTEGER frequency = {};
	static LARGE_INTEGER currentTime = {};

	if (frequency.QuadPart == 0) {
		QueryPerformanceFrequency(&frequency);
	}

	QueryPerformanceCounter(&currentTime);
	float time = static_cast<float>(currentTime.QuadPart) / static_cast<float>(frequency.QuadPart);

	if (lastPresentTime > 0.0f) {
		frameTime = time - lastPresentTime;
	}
	lastPresentTime = time;

	return frameTime;
}

WrappedResource::WrappedResource(D3D11_TEXTURE2D_DESC a_texDesc, ID3D11Device5* a_d3d11Device, ID3D12Device* a_d3d12Device, const std::string& a_name)
{
	// Create D3D11 shared texture directly instead of wrapping D3D12 resource
	a_texDesc.MiscFlags |= D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
	auto throwIfFailed = [&](HRESULT a_result, const char* a_operation) {
		if (FAILED(a_result)) {
			logger::error(
				"[DX12SwapChain] Wrapped resource '{}' {} failed: HRESULT 0x{:08X}, dimensions {}x{}, format {}, bind flags 0x{:X}, misc flags 0x{:X}",
				a_name.empty() ? "<unnamed>" : a_name.c_str(),
				a_operation,
				static_cast<uint32_t>(a_result),
				a_texDesc.Width,
				a_texDesc.Height,
				static_cast<uint32_t>(a_texDesc.Format),
				a_texDesc.BindFlags,
				a_texDesc.MiscFlags);
		}
		DX::ThrowIfFailed(a_result);
	};

	throwIfFailed(a_d3d11Device->CreateTexture2D(&a_texDesc, nullptr, &resource11), "CreateTexture2D");
	if (!a_name.empty())
		Util::SetResourceName(resource11, "%s", a_name.c_str());

	// Get shared handle from D3D11 texture to enable D3D12 access
	winrt::com_ptr<IDXGIResource1> dxgiResource;
	throwIfFailed(resource11->QueryInterface(IID_PPV_ARGS(dxgiResource.put())), "QueryInterface(IDXGIResource1)");
	HANDLE sharedHandle = nullptr;
	throwIfFailed(dxgiResource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &sharedHandle), "CreateSharedHandle");

	// Open the shared D3D11 texture as D3D12 resource. Close the NT handle
	// unconditionally before checking the result -- a thrown failure must
	// not leak it.
	const HRESULT openResult = a_d3d12Device->OpenSharedHandle(sharedHandle, IID_PPV_ARGS(resource.put()));
	CloseHandle(sharedHandle);
	throwIfFailed(openResult, "OpenSharedHandle");

	if (a_texDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = a_texDesc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = 1;

		throwIfFailed(a_d3d11Device->CreateShaderResourceView(resource11, &srvDesc, &srv), "CreateShaderResourceView");
		if (!a_name.empty())
			Util::SetResourceName(srv, "%s SRV", a_name.c_str());
	}

	if (a_texDesc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) {
		if (a_texDesc.ArraySize > 1) {
			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.Format = a_texDesc.Format;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
			uavDesc.Texture2DArray.FirstArraySlice = 0;
			uavDesc.Texture2DArray.ArraySize = a_texDesc.ArraySize;

			throwIfFailed(a_d3d11Device->CreateUnorderedAccessView(resource11, &uavDesc, &uav), "CreateUnorderedAccessView");
		} else {
			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.Format = a_texDesc.Format;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			uavDesc.Texture2D.MipSlice = 0;

			throwIfFailed(a_d3d11Device->CreateUnorderedAccessView(resource11, &uavDesc, &uav), "CreateUnorderedAccessView");
		}
		if (!a_name.empty())
			Util::SetResourceName(uav, "%s UAV", a_name.c_str());
	}

	if (a_texDesc.BindFlags & D3D11_BIND_RENDER_TARGET) {
		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
		rtvDesc.Format = a_texDesc.Format;
		rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		rtvDesc.Texture2D.MipSlice = 0;
		throwIfFailed(a_d3d11Device->CreateRenderTargetView(resource11, &rtvDesc, &rtv), "CreateRenderTargetView");
		if (!a_name.empty())
			Util::SetResourceName(rtv, "%s RTV", a_name.c_str());
	}
}

WrappedResource::~WrappedResource()
{
	if (resource11) {
		resource11->Release();
		resource11 = nullptr;
	}
	if (srv) {
		srv->Release();
		srv = nullptr;
	}
	if (uav) {
		uav->Release();
		uav = nullptr;
	}
	if (rtv) {
		rtv->Release();
		rtv = nullptr;
	}
	// resource (winrt::com_ptr) will be automatically released
}

DXGISwapChainProxy::DXGISwapChainProxy(IDXGISwapChain4* a_swapChain)
{
	swapChain = a_swapChain;
}

/****IUknown****/
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::QueryInterface(REFIID riid, void** ppvObj)
{
	auto ret = swapChain->QueryInterface(riid, ppvObj);
	if (*ppvObj)
		*ppvObj = this;
	return ret;
}

ULONG STDMETHODCALLTYPE DXGISwapChainProxy::AddRef()
{
	return swapChain->AddRef();
}

ULONG STDMETHODCALLTYPE DXGISwapChainProxy::Release()
{
	return swapChain->Release();
}

/****IDXGIObject****/
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetPrivateData(_In_ REFGUID Name, UINT DataSize, _In_reads_bytes_(DataSize) const void* pData)
{
	return swapChain->SetPrivateData(Name, DataSize, pData);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetPrivateDataInterface(_In_ REFGUID Name, _In_opt_ const IUnknown* pUnknown)
{
	return swapChain->SetPrivateDataInterface(Name, pUnknown);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetPrivateData(_In_ REFGUID Name, _Inout_ UINT* pDataSize, _Out_writes_bytes_(*pDataSize) void* pData)
{
	return swapChain->GetPrivateData(Name, pDataSize, pData);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetParent(_In_ REFIID riid, _COM_Outptr_ void** ppParent)
{
	return swapChain->GetParent(riid, ppParent);
}

/****IDXGIDeviceSubObject****/
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetDevice(_In_ REFIID riid, _COM_Outptr_ void** ppDevice)
{
	return globals::features::upscaling.dx12SwapChain.GetDevice(riid, ppDevice);
}

/****IDXGISwapChain****/
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::Present(UINT SyncInterval, UINT Flags)
{
	return globals::features::upscaling.dx12SwapChain.Present(SyncInterval, Flags);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetBuffer(UINT buffer, _In_ REFIID riid, _COM_Outptr_ void** ppSurface)
{
	return globals::features::upscaling.dx12SwapChain.GetBuffer(buffer, riid, ppSurface);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetFullscreenState(BOOL Fullscreen, _In_opt_ IDXGIOutput* pTarget)
{
	return swapChain->SetFullscreenState(Fullscreen, pTarget);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetFullscreenState(_Out_opt_ BOOL* pFullscreen, _COM_Outptr_opt_result_maybenull_ IDXGIOutput** ppTarget)
{
	return swapChain->GetFullscreenState(pFullscreen, ppTarget);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetDesc(_Out_ DXGI_SWAP_CHAIN_DESC* pDesc)
{
	return swapChain->GetDesc(pDesc);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
	return globals::features::upscaling.dx12SwapChain.ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::ResizeTarget(_In_ const DXGI_MODE_DESC* pNewTargetParameters)
{
	return swapChain->ResizeTarget(pNewTargetParameters);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetContainingOutput(_COM_Outptr_ IDXGIOutput** ppOutput)
{
	return swapChain->GetContainingOutput(ppOutput);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetFrameStatistics(_Out_ DXGI_FRAME_STATISTICS* pStats)
{
	return swapChain->GetFrameStatistics(pStats);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetLastPresentCount(_Out_ UINT* pLastPresentCount)
{
	return swapChain->GetLastPresentCount(pLastPresentCount);
}

void DX12SwapChain::SetColorSpace(bool enableHDR)
{
	if (!swapChain)
		return;

	if (enableHDR) {
		swapChain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
		logger::info("[DX12SwapChain] Set color space to HDR10 (PQ/BT.2020)");
	} else {
		swapChain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
		logger::info("[DX12SwapChain] Set color space to SDR (sRGB)");
	}
}

DX12SwapChain::BlurResources DX12SwapChain::GetBlurResources() const
{
	BlurResources res;
	if (swapChainBufferWrapped) {
		res.backbufferTex = swapChainBufferWrapped->resource11;
		res.backbufferRTV = swapChainBufferWrapped->rtv;
		res.backbufferSRV = swapChainBufferWrapped->srv;
	}
	if (uiBufferWrapped) {
		res.uiBufferSRV = uiBufferWrapped->srv;
		res.uiBufferRTV = uiBufferWrapped->rtv;
	}
	return res;
}

void DX12SwapChain::CreateSharedResources()
{
	auto renderer = globals::game::renderer;

	// Create depth buffer
	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	D3D11_TEXTURE2D_DESC texDesc{};
	main.texture->GetDesc(&texDesc);
	texDesc.Format = DXGI_FORMAT_R32_FLOAT;
	depthBufferShared12 = new WrappedResource(texDesc, d3d11Device.get(), d3d12Device.get(), "DX12SwapChain::DepthBufferShared");

	// Create motion vector buffer
	auto& motionVector = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
	motionVector.texture->GetDesc(&texDesc);
	motionVectorBufferShared12 = new WrappedResource(texDesc, d3d11Device.get(), d3d12Device.get(), "DX12SwapChain::MotionVectorBufferShared");
}
