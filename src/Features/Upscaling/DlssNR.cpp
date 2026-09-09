#include "DlssNR.h"

#include <d3dcompiler.h>
#include <directx/d3dx12.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <iterator>
#include <mutex>
#include <optional>
#include <vector>

#include "../../Utils/D3D.h"

namespace DlssNR
{
	namespace
	{
		constexpr uint32_t kSrvCount = 4;  // source, model, original, motion
		constexpr uint32_t kUavCount = 3;  // target, keep, motion out
		constexpr uint32_t kCbvCount = 1;
		constexpr uint32_t kDescriptorsPerSlot = kSrvCount + kUavCount + kCbvCount;
		constexpr uint32_t kDispatchSlots = 16;  // downsample + stabilise + resolve, with frames in flight to spare
		constexpr uint32_t kThreads = 8;
		constexpr uint32_t kSettleFrames = 30;
		constexpr int kRetireFrames = 32;
		constexpr unsigned long long kAppId = 0x24480451ull;  // OptiScaler's generic id, what the forwarder uses
		constexpr int kNgxSdkVersion = 0x15;
		constexpr int kNgxSuccess = 1;

		enum Mode : uint32_t
		{
			kEncode = 0,
			kResolve = 1,
			kDownsample = 2,
			kStabilize = 4,  // 3 is the meter in the shader, unused here
			kConvertMotion = 5,  // the model's motion input: pixels of the guide plus the jitter delta
		};

		// Mirrors cbuffer Params in DlssNR.hlsl, field for field. Padded to the 256-byte CBV alignment on purpose.
#pragma warning(push)
#pragma warning(disable: 4324)
		struct alignas(256) Constants
		{
			uint32_t Mode;
			float WhitePoint;
			uint32_t Width;
			uint32_t Height;
			float TransferStrength;
			float ColourStrength;
			uint32_t DebugView;
			float MaxRatio;
			uint32_t Passthrough;
			float MvScaleX;
			float MvScaleY;
			uint32_t GuideWidth;
			uint32_t GuideHeight;
			uint32_t CompareMode;
			float CompareSplit;
			float CompareZoom;
			uint32_t CompareSwap;
			uint32_t Transfer;
			float DebugScale;
			uint32_t Reset;     // stabilise: the history is not this scene
			float Stability;    // stabilise: share of the reprojected history kept
			float JitterDeltaX;  // convert: change in sample-position jitter since last frame, guide pixels
			float JitterDeltaY;
		};
#pragma warning(pop)

		// The forwarder's exports. Positional, matching dlssnr_forwarder.cpp exactly.
		using PFN_Create = void*(__cdecl*)(const wchar_t*, const wchar_t*, ID3D12Device*, ID3D12GraphicsCommandList*, void*, unsigned int, unsigned int, int, float, int, float, float, float, int, int);
		using PFN_Evaluate = int(__cdecl*)(ID3D12GraphicsCommandList*, void*, void*, ID3D12Resource*, ID3D12Resource*, ID3D12Resource*, ID3D12Resource*, unsigned int, unsigned int, unsigned int, unsigned int, int, int, float, int, float, float, float, int, float, float);
		using PFN_Release = void(__cdecl*)(void*);
		using PFN_SetExtras = void(__cdecl*)(void*, float, ID3D12Resource*, ID3D12Resource*, ID3D12Resource*, unsigned int, unsigned int, unsigned int, unsigned int);
		using PFN_SetFloatSlot = void(__cdecl*)(int);
		using PFN_ProbeFloat = void(__cdecl*)(void*, const char*, float, int);

		// The driver core's exports we touch. Init is only needed if Streamline has not brought the core
		// up already, which it has whenever DLSS is running.
		using PFN_NgxInitExt = int(__cdecl*)(unsigned long long, const wchar_t*, ID3D12Device*, const void*, int);
		using PFN_NgxGetCapabilityParameters = int(__cdecl*)(void**);

		// NVSDK_NGX_Parameter's vtable: eight Set overloads then eight Get overloads, in the order ULL,
		// float, double, uint, int, ID3D11Resource*, ID3D12Resource*, void*. Only the float getter is
		// needed here, for the slot probe.
		using PFN_ParamGetFloat = int (*)(void*, const char*, float*);
		// MSVC lays out same-name overloads in reverse declaration order, which is why the driver's
		// block keeps its float setter at slot 6 rather than the header's 1. Whatever slot the setter
		// turns out to be, its getter is eight further on; the header-order getter is kept as a fallback.
		constexpr int kVtGetterOffset = 8;
		constexpr int kVtGetFloatHeaderOrder = 9;

		struct Retired
		{
			void* feature = nullptr;
			ID3D12Resource* resource = nullptr;
			int framesLeft = kRetireFrames;
		};

		struct State
		{
			Settings settings;
			bool enabled = false;
			std::wstring runtimeDirectory;
			std::wstring dataPath;
			ID3D12CommandQueue* commandQueue = nullptr;

			HMODULE forwarder = nullptr;
			PFN_Create create = nullptr;
			PFN_Evaluate evaluate = nullptr;
			PFN_Release release = nullptr;
			PFN_SetExtras setExtras = nullptr;
			PFN_SetFloatSlot setFloatSlot = nullptr;
			PFN_ProbeFloat probeFloat = nullptr;
			int* lastInit = nullptr;
			int* lastCreate = nullptr;

			void* capabilityParams = nullptr;
			bool floatSlotKnown = false;

			void* feature = nullptr;
			uint32_t width = 0;
			uint32_t height = 0;
			uint32_t workWidth = 0;
			uint32_t workHeight = 0;
			uint32_t guideWidth = 0;
			uint32_t guideHeight = 0;
			bool reset = true;

			ID3D12Resource* keep = nullptr;          // the frame as DLSS wrote it, untouched; also the model's full-size proxy for display-referred frames
			ID3D12Resource* proxy = nullptr;         // scene-linear frames: the encoded (soft knee + sRGB) full-size picture the model is shown
			ID3D12Resource* modelOut = nullptr;      // the model's answer (work size)
			ID3D12Resource* smallProxy = nullptr;
			ID3D12Resource* nrMotion = nullptr;      // motion for the model: the game's vectors in guide pixels plus the jitter delta (RG16F, guide size)    // the proxy shrunk to work size, when reduced
			bool encodeHdr = false;                  // the frame is float scene-linear and goes through the encode/decode
			ID3D12Resource* modelStable = nullptr;   // reduced: the model's answer with its edit steadied over time (work size, fp16)
			ID3D12Resource* editHistory[2] = {};     // reduced: last frame's steadied edit, ping-ponged (work size, fp16)
			uint32_t historyIndex = 0;
			bool historyValid = false;

			uint32_t builtPreset = 0;
			uint32_t builtStyle = 0;
			float builtIntensity = 0.0f;
			float builtLocalStructure = 0.0f;
			float builtLocalTone = 0.0f;
			float builtSkin = 0.0f;
			uint32_t builtAutoMask = 0;
			unsigned long long settledAt = 0;
			unsigned long long frames = 0;

			bool failed = false;
			std::string reason;

			// The composition pass.
			ID3D12Device* passDevice = nullptr;
			winrt::com_ptr<ID3D12RootSignature> rootSignature;
			winrt::com_ptr<ID3D12PipelineState> pipeline;
			winrt::com_ptr<ID3D12DescriptorHeap> heap;
			winrt::com_ptr<ID3D12Resource> constantBuffers[kDispatchSlots];
			uint32_t slot = 0;
			bool passReady = false;

			std::vector<Retired> retired;
			std::mutex mutex;

			// GPU timestamps: four per run (start, before model, after model, end), a ring of slots read
			// back three runs later so nothing ever waits on the GPU.
			winrt::com_ptr<ID3D12QueryHeap> queryHeap;
			winrt::com_ptr<ID3D12Resource> queryReadback;
			uint32_t timingSlot = 0;
			uint64_t timestampFrequency = 0;
			std::optional<double> lastTotalMs;
			std::optional<double> lastModelMs;
			unsigned long long lastCostLogFrame = 0;
		};

		constexpr uint32_t kTimingSlots = 4;
		constexpr uint32_t kTimestampsPerSlot = 4;

		State g;

		std::string Narrow(const std::wstring& a_text)
		{
			if (a_text.empty())
				return {};
			const int needed = WideCharToMultiByte(CP_UTF8, 0, a_text.c_str(), -1, nullptr, 0, nullptr, nullptr);
			if (needed <= 1)
				return {};
			std::string result(static_cast<size_t>(needed - 1), '\0');
			WideCharToMultiByte(CP_UTF8, 0, a_text.c_str(), -1, result.data(), needed, nullptr, nullptr);
			return result;
		}

		void Fail(std::string a_reason)
		{
			g.failed = true;
			g.reason = std::move(a_reason);
			logger::error("[DLSS-NR] unavailable: {}", g.reason);
		}

		const char* NgxResultName(unsigned int a_result)
		{
			switch (a_result) {
			case 0x1:
				return "Success";
			case 0xBAD00001:
				return "FAIL_FeatureNotSupported";
			case 0xBAD00002:
				return "FAIL_PlatformError";
			case 0xBAD00003:
				return "FAIL_FeatureAlreadyExists";
			case 0xBAD00004:
				return "FAIL_FeatureNotFound";
			case 0xBAD00005:
				return "FAIL_InvalidParameter";
			case 0xBAD00006:
				return "FAIL_ScratchBufferTooSmall";
			case 0xBAD00007:
				return "FAIL_NotInitialized";
			case 0xBAD00008:
				return "FAIL_UnsupportedInputFormat";
			case 0xBAD00009:
				return "FAIL_RWFlagMissing";
			case 0xBAD0000A:
				return "FAIL_MissingInput";
			case 0xBAD0000B:
				return "FAIL_UnableToInitializeFeature";
			case 0xBAD0000C:
				return "FAIL_OutOfDate";
			case 0xBAD0000D:
				return "FAIL_OutOfGPUMemory";
			case 0xBAD0000E:
				return "FAIL_UnsupportedFormat";
			case 0xBAD0000F:
				return "FAIL_UnableToWriteToAppDataPath";
			case 0xBAD00010:
				return "FAIL_UnsupportedParameter";
			case 0xBAD00011:
				return "FAIL_Denied";
			case 0xBAD00012:
				return "FAIL_NotImplemented";
			default:
				return "unknown";
			}
		}

		std::wstring RuntimeFile(const wchar_t* a_name)
		{
			std::wstring path = g.runtimeDirectory;
			if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
				path.push_back(L'\\');
			path += a_name;
			return path;
		}

		bool EnsureDataPath()
		{
			if (!g.dataPath.empty())
				return true;
			wchar_t temp[MAX_PATH]{};
			const DWORD length = GetTempPathW(MAX_PATH, temp);
			if (length == 0 || length >= MAX_PATH)
				return false;
			std::filesystem::path path(temp);
			path /= L"CommunityShaders";
			path /= L"DlssNR";
			std::error_code ec;
			std::filesystem::create_directories(path, ec);
			g.dataPath = path.wstring();
			return true;
		}

		bool EnsureForwarder()
		{
			if (g.forwarder)
				return g.create != nullptr;
			const auto path = RuntimeFile(L"nvngx.dll_dlssnr.dll");
			if (!std::filesystem::exists(path)) {
				Fail(std::format("nvngx.dll_dlssnr.dll is missing from {}", Narrow(g.runtimeDirectory)));
				return false;
			}
			g.forwarder = LoadLibraryW(path.c_str());
			if (!g.forwarder) {
				Fail(std::format("nvngx.dll_dlssnr.dll would not load ({})", GetLastError()));
				return false;
			}
			g.create = reinterpret_cast<PFN_Create>(GetProcAddress(g.forwarder, "dlssnr_call_create"));
			g.evaluate = reinterpret_cast<PFN_Evaluate>(GetProcAddress(g.forwarder, "dlssnr_call_evaluate"));
			g.release = reinterpret_cast<PFN_Release>(GetProcAddress(g.forwarder, "dlssnr_call_release"));
			g.setExtras = reinterpret_cast<PFN_SetExtras>(GetProcAddress(g.forwarder, "dlssnr_call_set_extras"));
			g.setFloatSlot = reinterpret_cast<PFN_SetFloatSlot>(GetProcAddress(g.forwarder, "dlssnr_call_set_float_slot"));
			g.probeFloat = reinterpret_cast<PFN_ProbeFloat>(GetProcAddress(g.forwarder, "dlssnr_call_probe_float"));
			g.lastInit = reinterpret_cast<int*>(GetProcAddress(g.forwarder, "dlssnr_call_last_init"));
			g.lastCreate = reinterpret_cast<int*>(GetProcAddress(g.forwarder, "dlssnr_call_last_create"));
			if (!g.create || !g.evaluate) {
				Fail("the forwarder is missing its exports");
				return false;
			}
			logger::info("[DLSS-NR] forwarder loaded from {}", Narrow(path));
			return true;
		}

		// Which vtable slot this block keeps floats in. The public header says 1; the driver's own block
		// does not honour that, so each candidate is written and read back until one round-trips.
		void DiscoverFloatSlot()
		{
			if (g.floatSlotKnown || !g.capabilityParams || !g.probeFloat || !g.setFloatSlot)
				return;
			g.floatSlotKnown = true;

			// 6 first: that is where OptiScaler found the float setter on the driver's block. 5 is the
			// double setter under MSVC's reversed overload order and was the false positive: a float written
			// through it comes back with the same low 32 bits, so a 4-byte comparison cannot tell them apart.
			// The guard below can: a float getter writes 4 bytes and leaves the upper half alone, a double
			// getter overwrites all 8.
			static const char* kProbeKey = "DLSSNR.CSFloatProbe";
			static const int kCandidates[] = { 6, 1, 2, 7, 4, 3, 0, 5 };
			const float expected = 0.375f;
			constexpr uint32_t kGuard = 0xA5C3D2E1u;

			void** vtable = *reinterpret_cast<void***>(g.capabilityParams);

			for (int slot : kCandidates) {
				g.probeFloat(g.capabilityParams, kProbeKey, expected, slot);
				for (int getterSlot : { slot + kVtGetterOffset, kVtGetFloatHeaderOrder }) {
					if (getterSlot < 0 || getterSlot >= 16)
						continue;
					auto getFloat = reinterpret_cast<PFN_ParamGetFloat>(vtable[getterSlot]);
					alignas(8) uint32_t guarded[2] = { 0u, kGuard };
					const int result = getFloat(g.capabilityParams, kProbeKey, reinterpret_cast<float*>(&guarded[0]));
					float readBack = 0.0f;
					std::memcpy(&readBack, &guarded[0], sizeof(readBack));
					if (result == kNgxSuccess && readBack == expected && guarded[1] == kGuard) {
						g.setFloatSlot(slot);
						logger::info("[DLSS-NR] float parameters go through vtable slot {} (read back through {})", slot, getterSlot);
						return;
					}
					if (result == kNgxSuccess && guarded[1] != kGuard)
						logger::info("[DLSS-NR] vtable slot {} round-trips 8 bytes through {}; that is the double setter, skipping", slot, getterSlot);
				}
			}
			logger::error("[DLSS-NR] could not find the float setter; intensity and the strengths will have no effect");
		}

		std::wstring NgxCorePathFromRegistry()
		{
			HKEY key = nullptr;
			if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Services\\nvlddmkm\\NGXCore", 0, KEY_READ, &key) != ERROR_SUCCESS)
				return {};
			wchar_t value[MAX_PATH]{};
			DWORD size = sizeof(value);
			const auto status = RegQueryValueExW(key, L"NGXPath", nullptr, nullptr, reinterpret_cast<LPBYTE>(value), &size);
			RegCloseKey(key);
			if (status != ERROR_SUCCESS)
				return {};
			std::filesystem::path path(value);
			path /= L"_nvngx.dll";
			return path.wstring();
		}

		// The model needs the driver core's own capability block: it carries the snippet and preset
		// callbacks a feature expects at create time. Streamline's DLSS has the core loaded already.
		bool EnsureCapabilityParams(ID3D12Device* a_device)
		{
			if (g.capabilityParams)
				return true;

			HMODULE core = GetModuleHandleW(L"_nvngx.dll");
			if (!core)
				core = GetModuleHandleW(L"nvngx.dll");
			if (!core) {
				const auto path = NgxCorePathFromRegistry();
				if (!path.empty()) {
					core = LoadLibraryW(path.c_str());
					logger::info("[DLSS-NR] NGX core loaded from {}", Narrow(path));
				}
			}
			if (!core) {
				Fail("the NGX core (_nvngx.dll) is not loaded and was not found in the registry");
				return false;
			}

			auto getCaps = reinterpret_cast<PFN_NgxGetCapabilityParameters>(GetProcAddress(core, "NVSDK_NGX_D3D12_GetCapabilityParameters"));
			auto init = reinterpret_cast<PFN_NgxInitExt>(GetProcAddress(core, "NVSDK_NGX_D3D12_Init_Ext"));
			if (!getCaps) {
				Fail("the NGX core does not export NVSDK_NGX_D3D12_GetCapabilityParameters");
				return false;
			}

			void* params = nullptr;
			int result = getCaps(&params);
			if (result != kNgxSuccess || !params) {
				if (init && EnsureDataPath()) {
					const int initResult = init(kAppId, g.dataPath.c_str(), a_device, nullptr, kNgxSdkVersion);
					logger::info("[DLSS-NR] NGX core init returned 0x{:X} ({})", static_cast<unsigned int>(initResult), NgxResultName(static_cast<unsigned int>(initResult)));
					params = nullptr;
					result = getCaps(&params);
				}
			}
			if (result != kNgxSuccess || !params) {
				Fail(std::format("the NGX core refused its capability parameters (0x{:X} {})", static_cast<unsigned int>(result), NgxResultName(static_cast<unsigned int>(result))));
				return false;
			}

			g.capabilityParams = params;
			DiscoverFloatSlot();
			return true;
		}

		// ------------------------------------------------------------------------------------------
		// The composition pass
		// ------------------------------------------------------------------------------------------

		DXGI_FORMAT ViewFormat(DXGI_FORMAT a_format)
		{
			switch (a_format) {
			case DXGI_FORMAT_R32_TYPELESS:
				return DXGI_FORMAT_R32_FLOAT;
			case DXGI_FORMAT_R16_TYPELESS:
				return DXGI_FORMAT_R16_UNORM;
			case DXGI_FORMAT_R24G8_TYPELESS:
				return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
			case DXGI_FORMAT_R32G8X24_TYPELESS:
				return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
			case DXGI_FORMAT_R16G16_TYPELESS:
				return DXGI_FORMAT_R16G16_FLOAT;
			case DXGI_FORMAT_R8G8B8A8_TYPELESS:
				return DXGI_FORMAT_R8G8B8A8_UNORM;
			case DXGI_FORMAT_R16G16B16A16_TYPELESS:
				return DXGI_FORMAT_R16G16B16A16_FLOAT;
			default:
				return a_format;
			}
		}

		bool EnsurePass(ID3D12Device* a_device)
		{
			if (g.passReady && g.passDevice == a_device)
				return true;
			g.passReady = false;
			g.passDevice = a_device;
			g.rootSignature = nullptr;
			g.pipeline = nullptr;
			g.heap = nullptr;

			try {
				D3D12_DESCRIPTOR_RANGE ranges[3]{};
				ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
				ranges[0].NumDescriptors = kSrvCount;
				ranges[0].BaseShaderRegister = 0;
				ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
				ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
				ranges[1].NumDescriptors = kUavCount;
				ranges[1].BaseShaderRegister = 0;
				ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
				ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
				ranges[2].NumDescriptors = kCbvCount;
				ranges[2].BaseShaderRegister = 0;
				ranges[2].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

				D3D12_ROOT_PARAMETER parameter{};
				parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
				parameter.DescriptorTable.NumDescriptorRanges = static_cast<UINT>(std::size(ranges));
				parameter.DescriptorTable.pDescriptorRanges = ranges;
				parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

				D3D12_STATIC_SAMPLER_DESC sampler{};
				sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
				sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
				sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
				sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
				sampler.MaxLOD = D3D12_FLOAT32_MAX;
				sampler.ShaderRegister = 0;
				sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

				D3D12_ROOT_SIGNATURE_DESC rootDesc{};
				rootDesc.NumParameters = 1;
				rootDesc.pParameters = &parameter;
				rootDesc.NumStaticSamplers = 1;
				rootDesc.pStaticSamplers = &sampler;

				winrt::com_ptr<ID3DBlob> rootBlob;
				winrt::com_ptr<ID3DBlob> rootError;
				DX::ThrowIfFailed(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, rootBlob.put(), rootError.put()));
				DX::ThrowIfFailed(a_device->CreateRootSignature(0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(), IID_PPV_ARGS(g.rootSignature.put())));

				const wchar_t* shaderPath = L"Data\\Shaders\\Upscaling\\DlssNR.hlsl";
				if (!std::filesystem::exists(shaderPath)) {
					Fail("Data\\Shaders\\Upscaling\\DlssNR.hlsl is missing");
					return false;
				}
				winrt::com_ptr<ID3DBlob> shader;
				winrt::com_ptr<ID3DBlob> errors;
				const auto compileResult = D3DCompileFromFile(shaderPath, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "CSMain", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, shader.put(), errors.put());
				if (FAILED(compileResult)) {
					Fail(std::format("DlssNR.hlsl did not compile: {}", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown error"));
					return false;
				}

				D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
				psoDesc.pRootSignature = g.rootSignature.get();
				psoDesc.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
				DX::ThrowIfFailed(a_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(g.pipeline.put())));

				D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
				heapDesc.NumDescriptors = kDescriptorsPerSlot * kDispatchSlots;
				heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
				heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
				DX::ThrowIfFailed(a_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(g.heap.put())));

				const auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(Constants));
				const auto upload = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
				for (auto& buffer : g.constantBuffers) {
					buffer = nullptr;
					DX::ThrowIfFailed(a_device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(buffer.put())));
				}

				// Timing is optional: if anything here fails the pass still runs, just unmeasured.
				g.queryHeap = nullptr;
				g.queryReadback = nullptr;
				g.timestampFrequency = 0;
				D3D12_QUERY_HEAP_DESC queryDesc{};
				queryDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
				queryDesc.Count = kTimingSlots * kTimestampsPerSlot;
				if (SUCCEEDED(a_device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(g.queryHeap.put())))) {
					const auto readbackDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(uint64_t) * kTimingSlots * kTimestampsPerSlot);
					const auto readbackHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
					if (FAILED(a_device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(g.queryReadback.put()))))
						g.queryHeap = nullptr;
				}
				if (g.commandQueue) {
					if (FAILED(g.commandQueue->GetTimestampFrequency(&g.timestampFrequency)))
						g.timestampFrequency = 0;
				}
			} catch (const std::exception& e) {
				Fail(std::format("the composition pass could not be created: {}", e.what()));
				return false;
			}

			g.passReady = true;
			return true;
		}

		void Dispatch(ID3D12GraphicsCommandList* a_commandList, const Constants& a_constants, ID3D12Resource* a_source, ID3D12Resource* a_model, ID3D12Resource* a_original, ID3D12Resource* a_motion, ID3D12Resource* a_target, ID3D12Resource* a_keep, ID3D12Resource* a_motionOut = nullptr)
		{
			auto* device = g.passDevice;
			const uint32_t slot = g.slot;
			g.slot = (g.slot + 1) % kDispatchSlots;

			const auto increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			auto cpu = g.heap->GetCPUDescriptorHandleForHeapStart();
			cpu.ptr += static_cast<SIZE_T>(slot) * kDescriptorsPerSlot * increment;
			auto gpu = g.heap->GetGPUDescriptorHandleForHeapStart();
			gpu.ptr += static_cast<UINT64>(slot) * kDescriptorsPerSlot * increment;

			// Every slot gets a view; an unbound descriptor is a read from nothing, so the source stands
			// in wherever a mode has nothing of its own.
			ID3D12Resource* const srvs[kSrvCount] = {
				a_source,
				a_model ? a_model : a_source,
				a_original ? a_original : a_source,
				a_motion ? a_motion : a_source,
			};
			for (uint32_t i = 0; i < kSrvCount; ++i) {
				D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
				desc.Format = ViewFormat(srvs[i]->GetDesc().Format);
				desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				desc.Texture2D.MipLevels = 1;
				device->CreateShaderResourceView(srvs[i], &desc, cpu);
				cpu.ptr += increment;
			}

			ID3D12Resource* const uavs[kUavCount] = {
				a_target,
				a_keep ? a_keep : a_target,
				a_motionOut ? a_motionOut : a_target,
			};
			for (uint32_t i = 0; i < kUavCount; ++i) {
				D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
				desc.Format = ViewFormat(uavs[i]->GetDesc().Format);
				desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
				device->CreateUnorderedAccessView(uavs[i], nullptr, &desc, cpu);
				cpu.ptr += increment;
			}

			auto& constantBuffer = g.constantBuffers[slot];
			void* mapped = nullptr;
			const D3D12_RANGE noRead{ 0, 0 };
			if (SUCCEEDED(constantBuffer->Map(0, &noRead, &mapped)) && mapped) {
				std::memcpy(mapped, &a_constants, sizeof(a_constants));
				constantBuffer->Unmap(0, nullptr);
			}
			D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
			cbvDesc.BufferLocation = constantBuffer->GetGPUVirtualAddress();
			cbvDesc.SizeInBytes = sizeof(Constants);
			device->CreateConstantBufferView(&cbvDesc, cpu);

			ID3D12DescriptorHeap* heaps[] = { g.heap.get() };
			a_commandList->SetDescriptorHeaps(1, heaps);
			a_commandList->SetComputeRootSignature(g.rootSignature.get());
			a_commandList->SetPipelineState(g.pipeline.get());
			a_commandList->SetComputeRootDescriptorTable(0, gpu);
			a_commandList->Dispatch((a_constants.Width + kThreads - 1) / kThreads, (a_constants.Height + kThreads - 1) / kThreads, 1);
		}

		// Several transitions in one call: the driver merges them into a single sync point instead of
		// draining the pipeline once per resource.
		struct Transition
		{
			ID3D12Resource* resource;
			D3D12_RESOURCE_STATES from;
			D3D12_RESOURCE_STATES to;
		};

		void Barriers(ID3D12GraphicsCommandList* a_commandList, std::initializer_list<Transition> a_transitions)
		{
			D3D12_RESOURCE_BARRIER barriers[8]{};
			UINT count = 0;
			for (const auto& t : a_transitions) {
				if (!t.resource || t.from == t.to || count >= 8)
					continue;
				barriers[count++] = CD3DX12_RESOURCE_BARRIER::Transition(t.resource, t.from, t.to);
			}
			if (count)
				a_commandList->ResourceBarrier(count, barriers);
		}

		void Stamp(ID3D12GraphicsCommandList* a_commandList, uint32_t a_index)
		{
			if (g.queryHeap)
				a_commandList->EndQuery(g.queryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, g.timingSlot * kTimestampsPerSlot + a_index);
		}

		// Resolves this run's four stamps and reads the slot written three runs ago.
		void FinishTiming(ID3D12GraphicsCommandList* a_commandList)
		{
			if (!g.queryHeap || !g.queryReadback || g.timestampFrequency == 0)
				return;
			const uint32_t slot = g.timingSlot;
			a_commandList->ResolveQueryData(g.queryHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * kTimestampsPerSlot, kTimestampsPerSlot, g.queryReadback.get(), sizeof(uint64_t) * slot * kTimestampsPerSlot);
			g.timingSlot = (g.timingSlot + 1) % kTimingSlots;

			const uint32_t oldest = (slot + 1) % kTimingSlots;
			if (g.frames < kTimingSlots)
				return;
			const D3D12_RANGE range{ sizeof(uint64_t) * oldest * kTimestampsPerSlot, sizeof(uint64_t) * (oldest + 1) * kTimestampsPerSlot };
			void* mapped = nullptr;
			if (FAILED(g.queryReadback->Map(0, &range, &mapped)) || !mapped)
				return;
			const auto* stamps = reinterpret_cast<const uint64_t*>(static_cast<const uint8_t*>(mapped) + range.Begin);
			const double toMs = 1000.0 / static_cast<double>(g.timestampFrequency);
			if (stamps[3] > stamps[0] && stamps[2] >= stamps[1]) {
				g.lastTotalMs = static_cast<double>(stamps[3] - stamps[0]) * toMs;
				g.lastModelMs = static_cast<double>(stamps[2] - stamps[1]) * toMs;
			}
			const D3D12_RANGE nothing{ 0, 0 };
			g.queryReadback->Unmap(0, &nothing);

			if (g.lastTotalMs && g.lastModelMs && g.frames - g.lastCostLogFrame >= 600) {
				g.lastCostLogFrame = g.frames;
				logger::info("[DLSS-NR] cost: {:.2f} ms total = {:.2f} ms model + {:.2f} ms composition (model {}x{})", *g.lastTotalMs, *g.lastModelMs, *g.lastTotalMs - *g.lastModelMs, g.workWidth, g.workHeight);
			}
		}

		ID3D12Resource* CreateScratch(ID3D12Device* a_device, DXGI_FORMAT a_format, uint32_t a_width, uint32_t a_height)
		{
			D3D12_HEAP_PROPERTIES heap{};
			heap.Type = D3D12_HEAP_TYPE_DEFAULT;
			D3D12_RESOURCE_DESC desc{};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			desc.Width = a_width;
			desc.Height = a_height;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.Format = a_format;
			desc.SampleDesc.Count = 1;
			desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
			ID3D12Resource* resource = nullptr;
			a_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&resource));
			return resource;
		}

		// Retired features and surfaces are freed a comfortable number of frames later: with frame
		// generation the GPU is several frames behind, and freeing under in-flight work loses the device.
		void ParkFeature(void*& a_feature)
		{
			if (a_feature) {
				g.retired.push_back({ a_feature, nullptr });
				a_feature = nullptr;
			}
		}

		void ParkResource(ID3D12Resource*& a_resource)
		{
			if (a_resource) {
				g.retired.push_back({ nullptr, a_resource });
				a_resource = nullptr;
			}
		}

		void TickRetired()
		{
			for (size_t i = 0; i < g.retired.size();) {
				if (--g.retired[i].framesLeft > 0) {
					++i;
					continue;
				}
				if (g.retired[i].feature && g.release)
					g.release(g.retired[i].feature);
				if (g.retired[i].resource)
					g.retired[i].resource->Release();
				g.retired.erase(g.retired.begin() + static_cast<std::ptrdiff_t>(i));
			}
		}

		bool TuningMatchesFeature(const Settings& s)
		{
			return g.builtPreset == s.preset && g.builtStyle == s.style && g.builtIntensity == s.intensity &&
			       g.builtLocalStructure == s.localStructureStrength && g.builtLocalTone == s.localToneStrength &&
			       g.builtSkin == s.skinStructureStrength && g.builtAutoMask == s.useAutoMask;
		}

		bool TuningMatchesPending(const Settings& a, const Settings& b)
		{
			return a.preset == b.preset && a.style == b.style && a.intensity == b.intensity &&
			       a.localStructureStrength == b.localStructureStrength && a.localToneStrength == b.localToneStrength &&
			       a.skinStructureStrength == b.skinStructureStrength && a.useAutoMask == b.useAutoMask;
		}

		void RecordBuiltTuning(const Settings& s)
		{
			g.builtPreset = s.preset;
			g.builtStyle = s.style;
			g.builtIntensity = s.intensity;
			g.builtLocalStructure = s.localStructureStrength;
			g.builtLocalTone = s.localToneStrength;
			g.builtSkin = s.skinStructureStrength;
			g.builtAutoMask = s.useAutoMask;
		}

		[[maybe_unused]] float WorkScale(uint32_t a_performanceMode)
		{
			switch (a_performanceMode) {
			case 1:
				return 0.75f;
			case 2:
				return 0.5f;
			case 3:
				return 1.0f / 3.0f;
			default:
				return 1.0f;
			}
		}
	}

	void SetEnabled(bool a_enabled)
	{
		std::scoped_lock lock(g.mutex);
		g.enabled = a_enabled;
	}

	void SetSettings(const Settings& a_settings)
	{
		std::scoped_lock lock(g.mutex);
		g.settings = a_settings;
	}

	Settings GetSettings()
	{
		std::scoped_lock lock(g.mutex);
		return g.settings;
	}

	bool IsEnabled()
	{
		std::scoped_lock lock(g.mutex);
		return g.enabled;
	}

	void SetRuntimeDirectory(const std::wstring& a_directory)
	{
		std::scoped_lock lock(g.mutex);
		g.runtimeDirectory = a_directory;
	}

	void SetCommandQueue(ID3D12CommandQueue* a_queue)
	{
		std::scoped_lock lock(g.mutex);
		g.commandQueue = a_queue;
	}

	void Run(ID3D12Device* a_device, ID3D12GraphicsCommandList* a_commandList, ID3D12Resource* a_color, ID3D12Resource* a_depth, ID3D12Resource* a_motion, uint32_t a_renderWidth, uint32_t a_renderHeight, bool a_reset, float a_mvScaleX, float a_mvScaleY, float a_jitterDeltaX, float a_jitterDeltaY)
	{
		std::scoped_lock lock(g.mutex);
		const Settings settings = g.settings;

		if (!g.enabled || g.failed || !a_device || !a_commandList || !a_color || !a_depth || !a_motion)
			return;

		const auto colorDesc = a_color->GetDesc();
		const auto width = static_cast<uint32_t>(colorDesc.Width);
		const auto height = colorDesc.Height;
		if (width == 0 || height == 0)
			return;

		const auto depthDesc = a_depth->GetDesc();
		const uint32_t guideWidth = a_renderWidth ? a_renderWidth : static_cast<uint32_t>(depthDesc.Width);
		const uint32_t guideHeight = a_renderHeight ? a_renderHeight : depthDesc.Height;

		if (!EnsurePass(a_device) || !EnsureForwarder() || !EnsureCapabilityParams(a_device) || !EnsureDataPath())
			return;

		// The pass runs at render resolution before super resolution, so the model always sees the
		// full picture; the reduced-model path is kept in the shader but no longer selected.
		const float workScale = 1.0f;
		const auto workWidth = static_cast<uint32_t>(static_cast<float>(width) * workScale + 0.5f);
		const auto workHeight = static_cast<uint32_t>(static_cast<float>(height) * workScale + 0.5f);
		const bool reduced = workWidth != width || workHeight != height;

		// A float frame here is scene-linear with values past 1.0 (the game tone maps later). The
		// model copes with that as it is, so by default it is handed over untouched (passthrough).
		// Opt-in: encode it for the model (soft knee at the white point, then sRGB) and decode the
		// answer back, the pass's own HDR path, for anyone who prefers the model to see a finished
		// picture.
		const bool floatFrame =
			colorDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
			colorDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
			colorDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
			colorDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB &&
			colorDesc.Format != DXGI_FORMAT_R10G10B10A2_UNORM;
		const bool hdrFrame = floatFrame && settings.hdrEncode != 0;
		const DXGI_FORMAT modelFormat = hdrFrame ? DXGI_FORMAT_R8G8B8A8_UNORM : colorDesc.Format;
		const float whitePoint = std::isfinite(settings.hdrWhitePoint) ? std::max(settings.hdrWhitePoint, 0.01f) : 1.0f;

		++g.frames;
		TickRetired();
		if (a_reset)
			g.reset = true;

		const bool resolutionChanged = g.width != width || g.height != height || g.workWidth != workWidth || g.workHeight != workHeight || g.encodeHdr != hdrFrame;
		const bool tuningChanged = !TuningMatchesFeature(settings);
		bool rebuildBlockedBySettle = false;
		if (g.feature && (resolutionChanged || tuningChanged)) {
			// A slider being dragged reports a new value every frame; a rebuild per frame exhausts the
			// driver's feature latches. Wait for it to hold still.
			if (tuningChanged && !resolutionChanged) {
				// Restarted on every frame the value differs from the pending one, so a drag rebuilds
				// once after release rather than every kSettleFrames during it.
				static Settings pending{};
				if (g.settledAt == 0 || !TuningMatchesPending(settings, pending)) {
					pending = settings;
					g.settledAt = g.frames;
				}
				if (g.frames - g.settledAt < kSettleFrames)
					rebuildBlockedBySettle = true;
			}
			if (!rebuildBlockedBySettle) {
				g.settledAt = 0;
				ParkFeature(g.feature);
				if (resolutionChanged) {
					ParkResource(g.keep);
					ParkResource(g.proxy);
					ParkResource(g.modelOut);
					ParkResource(g.smallProxy);
					ParkResource(g.nrMotion);
					ParkResource(g.modelStable);
					ParkResource(g.editHistory[0]);
					ParkResource(g.editHistory[1]);
					g.historyValid = false;
				}
			}
		}

		if (!g.keep) {
			g.keep = CreateScratch(a_device, colorDesc.Format, width, height);
			g.modelOut = CreateScratch(a_device, modelFormat, workWidth, workHeight);
			g.nrMotion = CreateScratch(a_device, DXGI_FORMAT_R16G16_FLOAT, guideWidth, guideHeight);
			g.proxy = hdrFrame ? CreateScratch(a_device, modelFormat, width, height) : nullptr;
			g.encodeHdr = hdrFrame;
			g.width = width;
			g.height = height;
			g.workWidth = workWidth;
			g.workHeight = workHeight;
			g.guideWidth = guideWidth;
			g.guideHeight = guideHeight;
			if (!g.keep || !g.modelOut || !g.nrMotion || (hdrFrame && !g.proxy)) {
				Fail("scratch surfaces could not be created");
				return;
			}
		}
		if (reduced && !g.smallProxy)
			g.smallProxy = CreateScratch(a_device, modelFormat, workWidth, workHeight);
		if (reduced && settings.editStability > 0.0f && !g.modelStable) {
			// Half floats: the edit is signed and small, and an 8-bit surface would round most of it away.
			g.modelStable = CreateScratch(a_device, DXGI_FORMAT_R16G16B16A16_FLOAT, workWidth, workHeight);
			g.editHistory[0] = CreateScratch(a_device, DXGI_FORMAT_R16G16B16A16_FLOAT, workWidth, workHeight);
			g.editHistory[1] = CreateScratch(a_device, DXGI_FORMAT_R16G16B16A16_FLOAT, workWidth, workHeight);
			g.historyValid = false;
		}

		if (!g.feature) {
			const auto snippet = RuntimeFile(L"nvngx_dlssnr.dll");
			if (!std::filesystem::exists(snippet)) {
				Fail(std::format("nvngx_dlssnr.dll is not in {} (it ships in NVIDIA driver packages; copy it beside sl.interposer.dll)", Narrow(g.runtimeDirectory)));
				return;
			}
			if (g.setExtras)
				g.setExtras(g.capabilityParams, 1.0f, nullptr, nullptr, nullptr, 0, 0, 0, 0);
			g.feature = g.create(snippet.c_str(), g.dataPath.c_str(), a_device, a_commandList, g.capabilityParams, workWidth, workHeight,
				static_cast<int>(settings.preset), settings.intensity, static_cast<int>(settings.style), settings.localStructureStrength,
				settings.localToneStrength, settings.skinStructureStrength, settings.useAutoMask ? 1 : 0, 1);
			if (!g.feature) {
				const auto initResult = static_cast<unsigned int>(g.lastInit ? *g.lastInit : 0);
				const auto createResult = static_cast<unsigned int>(g.lastCreate ? *g.lastCreate : 0);
				Fail(std::format("the model would not initialise (init 0x{:X} {}, create 0x{:X} {})", initResult, NgxResultName(initResult), createResult, NgxResultName(createResult)));
				return;
			}
			RecordBuiltTuning(settings);
			g.reset = true;
			logger::info("[DLSS-NR] running at {}x{} (model {}x{}, guides {}x{}, preset {}, style {}, intensity {}, motion scale x{} -> {} px/unit to the model, edit stability {}, {} frame{})", width, height, workWidth, workHeight, guideWidth, guideHeight, settings.preset, settings.style, settings.intensity, settings.motionScale, static_cast<float>(guideWidth) * settings.motionScale * static_cast<float>(workWidth) / static_cast<float>(width), reduced ? settings.editStability : 0.0f, hdrFrame ? "scene-linear" : "display-referred", hdrFrame ? std::format(", white point {}", whitePoint) : std::string{});
			// Creating and evaluating in the same list is what hung GPUs upstream. The creation goes
			// through this frame's submit; the first evaluate happens next frame.
			return;
		}

		// Detail strength 0 hands back the frame bit for bit, so the model has nothing to contribute.
		if (settings.detailStrength <= 0.0f && settings.debugView == 0) {
			g.reset = true;
			return;
		}

		// The frame is display-referred, so the upstream encode pass is a copy: keep the untouched frame
		// for the resolve and let the model read the DLSS output itself. One copy engine op instead of a
		// full-screen dispatch, and no separate proxy surface.
		const D3D12_RESOURCE_STATES common = D3D12_RESOURCE_STATE_COMMON;
		const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		const D3D12_RESOURCE_STATES uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		ID3D12Resource* smallIn = reduced ? g.smallProxy : nullptr;
		const bool stabilise = reduced && settings.editStability > 0.0f && g.modelStable && g.editHistory[0] && g.editHistory[1];
		ID3D12Resource* modelForResolve = g.modelOut;
		const bool resetThisRun = g.reset;

		// The game's motion vectors are in normalised screen units: Streamline is told mvecScale {1,1} for
		// them, which its guide defines as the normalised range, and the FSR path multiplies them by the
		// render size. So the number that turns them into pixels is a width, not 1. Handing the model 1
		// told it nothing ever moves, and a temporal model with no motion re-decides its detail every
		// frame. The model gets what OptiScaler gives it: units to render pixels, times its own size over
		// the frame's. The multiplier is there so the convention can be checked in game (0.5 if the range
		// turns out to be -1..1).
		const float mvUnitsToRenderX = static_cast<float>(guideWidth) * settings.motionScale * a_mvScaleX;
		const float mvUnitsToRenderY = static_cast<float>(guideHeight) * settings.motionScale * a_mvScaleY;

		const uint32_t passthrough = hdrFrame ? 0u : 1u;
		ID3D12Resource* fullProxy = hdrFrame ? g.proxy : g.keep;

		Stamp(a_commandList, 0);
		// The guides are transitioned here too: nothing between this point and the model touches them,
		// and NGX wants them readable at evaluate time.
		if (hdrFrame) {
			// Encode: the untouched frame into keep, the soft-knee'd sRGB picture into the proxy.
			Barriers(a_commandList, { { a_color, common, read },
										{ a_depth, common, read },
										{ a_motion, common, read } });
			Constants encode{};
			encode.Mode = kEncode;
			encode.WhitePoint = whitePoint;
			encode.Width = width;
			encode.Height = height;
			encode.Passthrough = 0;
			Dispatch(a_commandList, encode, a_color, nullptr, nullptr, nullptr, g.proxy, g.keep);
			Barriers(a_commandList, { { g.proxy, uav, read },
										{ g.keep, uav, read } });
		} else {
			Barriers(a_commandList, { { a_color, common, D3D12_RESOURCE_STATE_COPY_SOURCE },
										{ g.keep, uav, D3D12_RESOURCE_STATE_COPY_DEST },
										{ a_depth, common, read },
										{ a_motion, common, read } });
			a_commandList->CopyResource(g.keep, a_color);
			Barriers(a_commandList, { { g.keep, D3D12_RESOURCE_STATE_COPY_DEST, read },
										{ a_color, D3D12_RESOURCE_STATE_COPY_SOURCE, read } });
		}

		ID3D12Resource* modelInput = hdrFrame ? g.proxy : a_color;
		if (smallIn) {
			Constants down{};
			down.Mode = kDownsample;
			down.Width = workWidth;
			down.Height = workHeight;
			down.Passthrough = passthrough;
			Dispatch(a_commandList, down, modelInput, nullptr, nullptr, nullptr, smallIn, nullptr);
			Barriers(a_commandList, { { smallIn, uav, read } });
			modelInput = smallIn;
		}

		const float mvToWork = static_cast<float>(workWidth) / static_cast<float>(width);
		// The model's motion input: the game's vectors as pixels of the guide with the change in
		// sample-position jitter since last frame folded in, the way the reference implementation
		// feeds its model. The raw vectors still go to the resolve.
		{
			Constants convert{};
			convert.Mode = kConvertMotion;
			convert.Width = guideWidth;
			convert.Height = guideHeight;
			convert.MvScaleX = mvUnitsToRenderX * mvToWork;
			convert.MvScaleY = mvUnitsToRenderY * mvToWork;
			convert.JitterDeltaX = a_jitterDeltaX * mvToWork;
			convert.JitterDeltaY = a_jitterDeltaY * mvToWork;
			Dispatch(a_commandList, convert, a_color, nullptr, nullptr, a_motion, g.modelOut, nullptr, g.nrMotion);
			Barriers(a_commandList, { { g.nrMotion, uav, read } });
		}
		if (g.setExtras)
			g.setExtras(g.capabilityParams, 1.0f, nullptr, nullptr, nullptr, 0, 0, 0, 0);
		Stamp(a_commandList, 1);
		const int result = g.evaluate(a_commandList, g.feature, g.capabilityParams, modelInput, a_depth, g.nrMotion, g.modelOut,
			workWidth, workHeight, guideWidth, guideHeight, 0, g.reset ? 1 : 0, settings.intensity, static_cast<int>(settings.style),
			settings.localStructureStrength, settings.localToneStrength, settings.skinStructureStrength, settings.useAutoMask ? 1 : 0,
			1.0f, 1.0f);
		Stamp(a_commandList, 2);
		g.reset = false;

		if (result == kNgxSuccess) {
			Barriers(a_commandList, { { g.modelOut, uav, read } });

			if (stabilise) {
				// The reduced model's edit, steadied against its own reprojected history at model size,
				// before it is carried up. Neighbourhood-clamped, so a wrong or missing motion vector
				// degrades to the current frame's edit rather than to a smear.
				const uint32_t readIndex = g.historyIndex;
				const uint32_t writeIndex = readIndex ^ 1u;
				Constants stab{};
				stab.Mode = kStabilize;
				stab.Width = workWidth;
				stab.Height = workHeight;
				stab.Passthrough = passthrough;
				stab.MvScaleX = static_cast<float>(workWidth) * settings.motionScale * a_mvScaleX;
				stab.MvScaleY = static_cast<float>(workHeight) * settings.motionScale * a_mvScaleY;
				stab.GuideWidth = guideWidth;
				stab.GuideHeight = guideHeight;
				stab.Reset = (resetThisRun || !g.historyValid) ? 1u : 0u;
				stab.Stability = std::clamp(settings.editStability, 0.0f, 0.95f);
				Barriers(a_commandList, { { g.editHistory[readIndex], uav, read } });
				Dispatch(a_commandList, stab, smallIn, g.modelOut, g.editHistory[readIndex], a_motion, g.modelStable, g.editHistory[writeIndex]);
				Barriers(a_commandList, { { g.editHistory[readIndex], read, uav },
											{ g.modelStable, uav, read } });
				g.historyIndex = writeIndex;
				g.historyValid = true;
				modelForResolve = g.modelStable;
			} else {
				g.historyValid = false;
			}

			Constants resolve{};
			resolve.Mode = kResolve;
			resolve.WhitePoint = hdrFrame ? whitePoint : 1.0f;
			resolve.Width = width;
			resolve.Height = height;
			resolve.TransferStrength = settings.detailStrength;
			resolve.ColourStrength = settings.colourStrength;
			resolve.DebugView = settings.debugView;
			resolve.MaxRatio = settings.maxRatio;
			resolve.Passthrough = passthrough;
			resolve.MvScaleX = static_cast<float>(width) * settings.motionScale * a_mvScaleX;
			resolve.MvScaleY = static_cast<float>(height) * settings.motionScale * a_mvScaleY;
			resolve.GuideWidth = guideWidth;
			resolve.GuideHeight = guideHeight;
			resolve.Transfer = 1;
			resolve.CompareZoom = 1.0f;
			resolve.DebugScale = 1.0f;

			Barriers(a_commandList, { { a_color, read, uav } });
			Dispatch(a_commandList, resolve, smallIn ? smallIn : fullProxy, modelForResolve, g.keep, a_motion, a_color, nullptr);
			// Everything back where the next frame expects it, in one call.
			Barriers(a_commandList, { { a_color, uav, common },
										{ g.nrMotion, read, uav },
										{ g.modelOut, read, uav },
										{ stabilise ? g.modelStable : nullptr, read, uav },
										{ a_depth, read, common },
										{ a_motion, read, common },
										{ g.keep, read, uav },
										{ smallIn, read, uav },
										{ hdrFrame ? g.proxy : nullptr, read, uav } });
		} else {
			g.historyValid = false;
			Barriers(a_commandList, { { a_color, read, common },
										{ g.nrMotion, read, uav },
										{ a_depth, read, common },
										{ a_motion, read, common },
										{ g.keep, read, uav },
										{ smallIn, read, uav },
										{ hdrFrame ? g.proxy : nullptr, read, uav } });
			Fail(std::format("the model refused to run (0x{:X} {})", static_cast<unsigned int>(result), NgxResultName(static_cast<unsigned int>(result))));
		}

		Stamp(a_commandList, 3);
		FinishTiming(a_commandList);
	}

	bool IsRunning()
	{
		std::scoped_lock lock(g.mutex);
		return g.feature != nullptr && !g.failed;
	}

	std::string FailureReason()
	{
		std::scoped_lock lock(g.mutex);
		return g.failed ? g.reason : std::string{};
	}

	void RetryAfterFailure()
	{
		std::scoped_lock lock(g.mutex);
		g.failed = false;
		g.reason.clear();
		g.reset = true;
	}

	std::string Describe()
	{
		std::scoped_lock lock(g.mutex);
		if (!g.enabled)
			return "Off";
		if (g.failed)
			return std::format("Off for this session: {}", g.reason);
		if (g.feature) {
			if (g.lastTotalMs && g.lastModelMs)
				return std::format("Running at {}x{} (model {}x{}), {:.2f} ms per frame ({:.2f} ms model)", g.width, g.height, g.workWidth, g.workHeight, *g.lastTotalMs, *g.lastModelMs);
			return std::format("Running at {}x{} (model {}x{})", g.width, g.height, g.workWidth, g.workHeight);
		}
		return "Waiting for the DLSS upscaler to run";
	}

	void GetModelSize(uint32_t& a_width, uint32_t& a_height)
	{
		std::scoped_lock lock(g.mutex);
		a_width = g.workWidth;
		a_height = g.workHeight;
	}

	bool IsEncodingHdr()
	{
		std::scoped_lock lock(g.mutex);
		return g.encodeHdr;
	}

	bool GetLastCostMs(float& a_totalMs, float& a_modelMs)
	{
		std::scoped_lock lock(g.mutex);
		if (!g.enabled || !g.feature || !g.lastTotalMs || !g.lastModelMs)
			return false;
		a_totalMs = static_cast<float>(*g.lastTotalMs);
		a_modelMs = static_cast<float>(*g.lastModelMs);
		return true;
	}

	void Shutdown()
	{
		std::scoped_lock lock(g.mutex);
		if (g.feature && g.release) {
			g.release(g.feature);
			g.feature = nullptr;
		}
		for (auto& r : g.retired) {
			if (r.feature && g.release)
				g.release(r.feature);
			if (r.resource)
				r.resource->Release();
		}
		g.retired.clear();
		for (ID3D12Resource** r : { &g.keep, &g.proxy, &g.modelOut, &g.smallProxy, &g.nrMotion, &g.modelStable, &g.editHistory[0], &g.editHistory[1] }) {
			if (*r) {
				(*r)->Release();
				*r = nullptr;
			}
		}
	}
}
