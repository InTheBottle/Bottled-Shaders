#include "MfgUnlock.h"

#include "dlssg_provider_policy.h"
#include "midpoint_fix.h"
#include "universal_wrapper_profile.h"

#include <TlHelp32.h>
#include <winternl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

namespace MfgUnlock
{
	namespace
	{
		static_assert(universal_wrapper_profile::SafeMaximumMultiplier(1) == 2);
		static_assert(universal_wrapper_profile::SafeMaximumMultiplier(3) == 4);
		static_assert(universal_wrapper_profile::SafeMaximumMultiplier(5) == 6);
		static_assert(universal_wrapper_profile::SafeMaximumMultiplier(7) == 2);

		struct ModuleRecord
		{
			HMODULE module = nullptr;
			std::wstring path;
			bool wrapperExport = false;
			bool wrapperCandidate = false;
			bool wrapperPatched = false;
			uint32_t compiledMaximum = 0;  // the wrapper's own mov edx,N immediate: 1, 3 or 5
			bool retained = false;         // module reference held for the plugin's lifetime
			bool ngxExport = false;
			bool ngxCandidate = false;
			bool ngxPatched = false;
			bool ngxTemporalPatched = false;
			bool providerVersionKnown = false;
			bool providerVersionSupported = false;
			dlssg_provider_policy::VersionTriplet providerVersion{};
		};

		std::atomic<bool> gEnabled{ false };
		std::atomic<bool> gDirty{ true };
		std::atomic<bool> gNotificationRegistered{ false };
		std::atomic<bool> gMidpointLogHooked{ false };
		std::atomic<bool> gActiveWrapperObserved{ false };
		std::atomic<bool> gActiveWrapperPatched{ false };
		std::atomic<uintptr_t> gActiveWrapperBase{ 0 };
		std::atomic<uint32_t> gWrapperCandidates{ 0 };
		std::atomic<uint32_t> gPatchedWrappers{ 0 };
		std::atomic<uint32_t> gNgxCandidates{ 0 };
		std::atomic<uint32_t> gNgxBytePatched{ 0 };
		std::atomic<uint32_t> gPatchedNgx{ 0 };
		std::atomic<uint32_t> gProviderModules{ 0 };
		std::atomic<uint32_t> gActiveWrapperCompiledMaximum{ 0 };
		std::atomic<bool> gLoggedReady{ false };
		std::atomic<uint64_t> gLastRetryTick{ 0 };
		std::atomic<HMODULE> gPreloadedProvider{ nullptr };

		// Upstream retries the kernel publication about once a second from a worker thread. This port
		// is called from the render thread, so the retry has to be rate limited or a permanently
		// failing provider costs a PE scan and several SHA-256 passes every frame.
		constexpr uint64_t kRetryIntervalMs = 1000;
		std::mutex gModuleMutex;
		std::vector<ModuleRecord> gModuleRecords;

		std::string Narrow(const wchar_t* a_text)
		{
			if (!a_text || !*a_text) {
				return {};
			}
			const int needed = WideCharToMultiByte(CP_UTF8, 0, a_text, -1, nullptr, 0, nullptr, nullptr);
			if (needed <= 1) {
				return {};
			}
			std::string result(static_cast<size_t>(needed - 1), '\0');
			WideCharToMultiByte(CP_UTF8, 0, a_text, -1, result.data(), needed, nullptr, nullptr);
			return result;
		}

		void MidpointLog(const wchar_t* a_message)
		{
			logger::info("[MFG Unlock] {}", Narrow(a_message));
		}

		bool PatchingAllowed()
		{
			return gEnabled.load(std::memory_order_acquire) && midpoint_fix::AdapterVerified();
		}

		// ---------------------------------------------------------------------------------------
		// PE helpers (ported from RTX40MFG-Unlock patcher.cpp)
		// ---------------------------------------------------------------------------------------

		const IMAGE_NT_HEADERS64* ImageHeaders(HMODULE a_module)
		{
			const auto* base = reinterpret_cast<const uint8_t*>(a_module);
			if (!base) {
				return nullptr;
			}
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || static_cast<size_t>(dos->e_lfanew) > 1024 * 1024) {
				return nullptr;
			}
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
				return nullptr;
			}
			return nt;
		}

		bool RvaRangeIsValid(const IMAGE_NT_HEADERS64* a_nt, DWORD a_rva, size_t a_size)
		{
			return a_nt && a_rva < a_nt->OptionalHeader.SizeOfImage && a_size <= static_cast<size_t>(a_nt->OptionalHeader.SizeOfImage - a_rva);
		}

		bool ModuleExportsFunction(HMODULE a_module, const char* a_expected)
		{
			const auto* nt = ImageHeaders(a_module);
			if (!nt || !a_expected) {
				return false;
			}

			const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
			if (!directory.VirtualAddress || !RvaRangeIsValid(nt, directory.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY))) {
				return false;
			}

			const auto* base = reinterpret_cast<const uint8_t*>(a_module);
			const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + directory.VirtualAddress);
			const size_t namesSize = static_cast<size_t>(exports->NumberOfNames) * sizeof(DWORD);
			if (!exports->AddressOfNames || !RvaRangeIsValid(nt, exports->AddressOfNames, namesSize)) {
				return false;
			}

			const auto* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
			for (DWORD index = 0; index < exports->NumberOfNames; ++index) {
				const DWORD nameRva = names[index];
				if (!RvaRangeIsValid(nt, nameRva, 1)) {
					continue;
				}
				const char* name = reinterpret_cast<const char*>(base + nameRva);
				const size_t remaining = nt->OptionalHeader.SizeOfImage - nameRva;
				const size_t length = strnlen_s(name, remaining);
				if (length < remaining && std::strcmp(name, a_expected) == 0) {
					return true;
				}
			}
			return false;
		}

		struct PatternPatch
		{
			const char* label;
			const uint8_t* pattern;
			size_t patternSize;
			size_t patchOffset;
			const uint8_t* original;
			const uint8_t* replacement;
			size_t patchSize;
		};

		// sl.dlss_g.dll: mov edx,5 / cmp ecx,edx / cmovb edx,ecx. The cmov takes the hardware maximum
		// when it is smaller than 5; removing it leaves the immediate.
		constexpr std::array<uint8_t, 10> kWrapperPattern{ 0xBA, 0x05, 0x00, 0x00, 0x00, 0x3B, 0xCA, 0x0F, 0x42, 0xD1 };
		constexpr std::array<uint8_t, 3> kWrapperOriginal{ 0x0F, 0x42, 0xD1 };
		constexpr std::array<uint8_t, 3> kWrapperReplacement{ 0x90, 0x90, 0x90 };
		const PatternPatch kWrapperPatch{
			"Streamline maximum", kWrapperPattern.data(), kWrapperPattern.size(), 7,
			kWrapperOriginal.data(), kWrapperReplacement.data(), kWrapperOriginal.size()
		};

		// nvngx_dlssg.dll: test dl,dl / jz +0x103 / mov esi,5. The jz is the device-support refusal.
		constexpr std::array<uint8_t, 13> kNgxPattern{ 0x84, 0xD2, 0x0F, 0x84, 0x03, 0x01, 0x00, 0x00, 0xBE, 0x05, 0x00, 0x00, 0x00 };
		constexpr std::array<uint8_t, 6> kNgxOriginal{ 0x0F, 0x84, 0x03, 0x01, 0x00, 0x00 };
		constexpr std::array<uint8_t, 6> kNgxReplacement{ 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
		const PatternPatch kNgxPatch{
			"NGX device support", kNgxPattern.data(), kNgxPattern.size(), 2,
			kNgxOriginal.data(), kNgxReplacement.data(), kNgxOriginal.size()
		};

		struct PatternPatchResult
		{
			bool candidate = false;
			bool patched = false;
			uint8_t* match = nullptr;
		};

		// Finds the pattern in the module's executable sections. With a_apply the original bytes are
		// replaced; without it the result only reports whether the module is a candidate and whether
		// the replacement is already present.
		PatternPatchResult PatchUniqueExecutablePattern(HMODULE a_module, const std::wstring& a_path, const PatternPatch& a_patch, bool a_apply)
		{
			const auto* base = reinterpret_cast<const uint8_t*>(a_module);
			const auto* nt = ImageHeaders(a_module);
			if (!nt) {
				return {};
			}

			const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
			uint8_t* match = nullptr;
			size_t matchCount = 0;
			for (unsigned index = 0; index < nt->FileHeader.NumberOfSections; ++index, ++section) {
				if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
					continue;
				}
				if (section->VirtualAddress >= nt->OptionalHeader.SizeOfImage) {
					continue;
				}
				auto* begin = const_cast<uint8_t*>(base + section->VirtualAddress);
				const size_t available = nt->OptionalHeader.SizeOfImage - section->VirtualAddress;
				const size_t size = std::min<size_t>(available, std::max<size_t>(section->Misc.VirtualSize, section->SizeOfRawData));
				if (size < a_patch.patternSize) {
					continue;
				}
				const size_t suffixOffset = a_patch.patchOffset + a_patch.patchSize;
				for (size_t offset = 0; offset + a_patch.patternSize <= size; ++offset) {
					const bool prefixMatches = a_patch.patchOffset == 0 || std::memcmp(begin + offset, a_patch.pattern, a_patch.patchOffset) == 0;
					const bool suffixMatches = suffixOffset == a_patch.patternSize || std::memcmp(begin + offset + suffixOffset, a_patch.pattern + suffixOffset, a_patch.patternSize - suffixOffset) == 0;
					const auto* candidate = begin + offset + a_patch.patchOffset;
					const bool patchBytesMatch = std::memcmp(candidate, a_patch.original, a_patch.patchSize) == 0 || std::memcmp(candidate, a_patch.replacement, a_patch.patchSize) == 0;
					// The wrapper's immediate is 1, 3 or 5 depending on the Streamline build (upstream v1.2's
					// universal profile); the NGX pattern is exact.
					const bool matches = (&a_patch == &kWrapperPatch) ? universal_wrapper_profile::Matches(begin + offset, size - offset) : (prefixMatches && suffixMatches && patchBytesMatch);
					if (matches) {
						match = begin + offset;
						++matchCount;
					}
				}
			}

			if (matchCount == 0) {
				return {};
			}
			if (matchCount != 1 || !match) {
				logger::warn("[MFG Unlock] {}: expected one code pattern, found {}: {}", a_patch.label, matchCount, Narrow(a_path.c_str()));
				return { true, false, nullptr };
			}

			uint8_t* address = match + a_patch.patchOffset;
			if (std::memcmp(address, a_patch.replacement, a_patch.patchSize) == 0) {
				return { true, true, match };
			}
			if (std::memcmp(address, a_patch.original, a_patch.patchSize) != 0) {
				logger::warn("[MFG Unlock] {}: matched context but original bytes differ: {}", a_patch.label, Narrow(a_path.c_str()));
				return { true, false, match };
			}
			if (!a_apply) {
				return { true, false, match };
			}

			DWORD oldProtection = 0;
			if (!VirtualProtect(address, a_patch.patchSize, PAGE_EXECUTE_READWRITE, &oldProtection)) {
				logger::warn("[MFG Unlock] {}: VirtualProtect failed ({}): {}", a_patch.label, GetLastError(), Narrow(a_path.c_str()));
				return { true, false, match };
			}
			std::memcpy(address, a_patch.replacement, a_patch.patchSize);
			FlushInstructionCache(GetCurrentProcess(), address, a_patch.patchSize);
			DWORD ignoredProtection = 0;
			const BOOL restored = VirtualProtect(address, a_patch.patchSize, oldProtection, &ignoredProtection);
			if (!restored) {
				logger::warn("[MFG Unlock] {}: protection restore failed ({}): {}", a_patch.label, GetLastError(), Narrow(a_path.c_str()));
				return { true, false, match };
			}

			logger::info("[MFG Unlock] {}: patched RVA 0x{:X}: {}", a_patch.label, static_cast<size_t>(address - base), Narrow(a_path.c_str()));
			return { true, true, match };
		}

		std::wstring LoadedModulePath(HMODULE a_module)
		{
			wchar_t path[32768]{};
			const DWORD length = GetModuleFileNameW(a_module, path, _countof(path));
			return length > 0 && length < _countof(path) ? std::wstring(path, length) : std::wstring{};
		}

		void RecomputeModuleStateLocked()
		{
			uint32_t wrapperCandidates = 0;
			uint32_t patchedWrappers = 0;
			uint32_t ngxCandidates = 0;
			uint32_t ngxBytePatched = 0;
			uint32_t patchedNgx = 0;
			for (const auto& record : gModuleRecords) {
				if (record.wrapperCandidate) {
					++wrapperCandidates;
				}
				if (record.wrapperPatched) {
					++patchedWrappers;
				}
				if (record.ngxCandidate) {
					++ngxCandidates;
				}
				if (record.ngxPatched) {
					++ngxBytePatched;
				}
				if (record.ngxPatched && record.ngxTemporalPatched) {
					++patchedNgx;
				}
			}
			gWrapperCandidates.store(wrapperCandidates, std::memory_order_release);
			gPatchedWrappers.store(patchedWrappers, std::memory_order_release);
			gNgxCandidates.store(ngxCandidates, std::memory_order_release);
			gNgxBytePatched.store(ngxBytePatched, std::memory_order_release);
			gPatchedNgx.store(patchedNgx, std::memory_order_release);
			uint32_t providerModules = 0;
			for (const auto& record : gModuleRecords)
				providerModules += record.ngxExport ? 1u : 0u;
			gProviderModules.store(providerModules, std::memory_order_release);

			const uintptr_t activeBase = gActiveWrapperBase.load(std::memory_order_acquire);
			if (activeBase) {
				for (const auto& record : gModuleRecords) {
					if (reinterpret_cast<uintptr_t>(record.module) == activeBase) {
						gActiveWrapperPatched.store(record.wrapperPatched, std::memory_order_release);
						gActiveWrapperCompiledMaximum.store(record.compiledMaximum, std::memory_order_release);
						break;
					}
				}
			}
		}

		// Applies whatever is still missing on a record. Safe to call repeatedly; each step is
		// idempotent and re-checks the bytes before writing.
		void ApplyPendingLocked(ModuleRecord& a_record)
		{
			const bool apply = PatchingAllowed();

			if (a_record.wrapperExport && !a_record.wrapperPatched) {
				const auto result = PatchUniqueExecutablePattern(a_record.module, a_record.path, kWrapperPatch, apply);
				a_record.wrapperCandidate = result.candidate;
				a_record.wrapperPatched = result.patched;
				if (result.patched && result.match)
					std::memcpy(&a_record.compiledMaximum, result.match + universal_wrapper_profile::kMaximumOffset, sizeof(a_record.compiledMaximum));
			}

			if (a_record.ngxExport) {
				if (!a_record.providerVersionKnown) {
					a_record.providerVersionKnown = dlssg_provider_policy::ReadProviderVersion(a_record.path.c_str(), a_record.providerVersion);
					// v1.2 policy: the version table (310.1 through 310.9) plus the module's export identity,
					// which keeps DLSS 5 super resolution providers (DirectSR) out of the frame generation path.
					a_record.providerVersionSupported = a_record.providerVersionKnown && dlssg_provider_policy::IsSupportedProvider(a_record.module, a_record.path.c_str());
					if (a_record.providerVersionKnown) {
						logger::info("[MFG Unlock] DLSS-G provider {}.{}.{} {}: {}",
							a_record.providerVersion.major, a_record.providerVersion.minor, a_record.providerVersion.build,
							a_record.providerVersionSupported ? "is a validated build" : "is NOT a validated build; patches stay off",
							Narrow(a_record.path.c_str()));
					}
				}

				if (!a_record.ngxPatched) {
					const auto result = PatchUniqueExecutablePattern(a_record.module, a_record.path, kNgxPatch, apply && a_record.providerVersionSupported);
					a_record.ngxCandidate = result.candidate;
					a_record.ngxPatched = result.patched;
				}
				if (a_record.ngxPatched && !a_record.ngxTemporalPatched && apply) {
					a_record.ngxTemporalPatched = midpoint_fix::PatchProvider(a_record.module, a_record.path.c_str());
				}
			}
		}

		ModuleRecord InspectLoadedModule(HMODULE a_module, const std::wstring& a_suppliedPath)
		{
			if (!a_module) {
				return {};
			}
			const std::wstring path = a_suppliedPath.empty() ? LoadedModulePath(a_module) : a_suppliedPath;

			std::lock_guard lock(gModuleMutex);
			// One record per mapped module. Under a virtual file system (Mod Organizer) the same
			// HMODULE is reported with its virtual path by GetModuleFileName and its real path by the
			// module snapshot; keying on the path as well would count one provider twice and fail closed.
			const auto existing = std::find_if(gModuleRecords.begin(), gModuleRecords.end(), [&](const ModuleRecord& a_record) {
				return a_record.module == a_module;
			});
			if (existing != gModuleRecords.end()) {
				ApplyPendingLocked(*existing);
				RecomputeModuleStateLocked();
				return *existing;
			}

			ModuleRecord record{};
			record.module = a_module;
			record.path = path;
			record.wrapperExport = ModuleExportsFunction(a_module, "slGetPluginFunction");
			record.ngxExport =
				dlssg_provider_policy::IsDlssgImplementationModule(a_module) &&
				ModuleExportsFunction(a_module, "NVSDK_NGX_D3D12_CreateFeature") &&
				ModuleExportsFunction(a_module, "NVSDK_NGX_GetGPUArchitecture");
			if (!record.wrapperExport && !record.ngxExport) {
				return record;
			}

			// Upstream v1.2: hold a reference so cached patch addresses and the midpoint fix's kernel
			// descriptors never outlive their DLL, and follow this module's own LoadLibrary calls so
			// a provider it loads is patched before it is used.
			HMODULE retained = nullptr;
			if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(a_module), &retained) && retained)
				record.retained = true;
			InstallLoaderDiscovery(a_module);

			ApplyPendingLocked(record);
			if (record.wrapperCandidate || record.ngxCandidate) {
				logger::info("[MFG Unlock] Module: wrapperCandidate={} wrapperPatched={} ngxCandidate={} ngxPatched={} midpointPatched={} path={}",
					record.wrapperCandidate, record.wrapperPatched, record.ngxCandidate, record.ngxPatched, record.ngxTemporalPatched, Narrow(record.path.c_str()));
			}
			gModuleRecords.push_back(record);
			RecomputeModuleStateLocked();
			return record;
		}

		void RemoveLoadedModule(HMODULE a_module)
		{
			if (!a_module) {
				return;
			}
			{
				std::lock_guard lock(gModuleMutex);
				gModuleRecords.erase(std::remove_if(gModuleRecords.begin(), gModuleRecords.end(), [&](const ModuleRecord& a_record) { return a_record.module == a_module; }), gModuleRecords.end());
				RecomputeModuleStateLocked();
			}
			if (gActiveWrapperBase.load(std::memory_order_acquire) == reinterpret_cast<uintptr_t>(a_module)) {
				gActiveWrapperPatched.store(false, std::memory_order_release);
				gActiveWrapperObserved.store(false, std::memory_order_release);
				gActiveWrapperBase.store(0, std::memory_order_release);
			}
		}

		void InspectAlreadyLoadedModules()
		{
			HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
			if (snapshot == INVALID_HANDLE_VALUE) {
				logger::warn("[MFG Unlock] Could not enumerate loaded modules ({})", GetLastError());
				return;
			}

			std::vector<HMODULE> loadedModules;
			MODULEENTRY32W entry{};
			entry.dwSize = sizeof(entry);
			if (Module32FirstW(snapshot, &entry)) {
				do {
					HMODULE module = reinterpret_cast<HMODULE>(entry.modBaseAddr);
					loadedModules.push_back(module);
					InspectLoadedModule(module, entry.szExePath);
					entry.dwSize = sizeof(entry);
				} while (Module32NextW(snapshot, &entry));
			}
			CloseHandle(snapshot);

			std::vector<HMODULE> removedModules;
			{
				std::lock_guard lock(gModuleMutex);
				for (const ModuleRecord& record : gModuleRecords) {
					if (std::find(loadedModules.begin(), loadedModules.end(), record.module) == loadedModules.end()) {
						removedModules.push_back(record.module);
					}
				}
			}
			for (HMODULE module : removedModules) {
				RemoveLoadedModule(module);
			}
		}

		struct LdrDllLoadedNotificationData
		{
			ULONG flags;
			const UNICODE_STRING* fullDllName;
			const UNICODE_STRING* baseDllName;
			PVOID dllBase;
			ULONG sizeOfImage;
		};

		union LdrDllNotificationData
		{
			LdrDllLoadedNotificationData loaded;
			LdrDllLoadedNotificationData unloaded;
		};

		using LdrDllNotificationFunction = void(CALLBACK*)(ULONG a_reason, const LdrDllNotificationData* a_data, void* a_context);
		using LdrRegisterDllNotificationFn = NTSTATUS(NTAPI*)(ULONG a_flags, LdrDllNotificationFunction a_callback, void* a_context, void** a_cookie);

		void CALLBACK OnDllNotification(ULONG a_reason, const LdrDllNotificationData* a_data, void*)
		{
			constexpr ULONG kDllLoaded = 1;
			constexpr ULONG kDllUnloaded = 2;
			if (a_data && (a_reason == kDllLoaded || a_reason == kDllUnloaded)) {
				gDirty.store(true, std::memory_order_release);
			}
		}

		void RegisterDllNotification()
		{
			if (gNotificationRegistered.load(std::memory_order_acquire)) {
				return;
			}
			HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
			auto* registerNotification = ntdll ? reinterpret_cast<LdrRegisterDllNotificationFn>(GetProcAddress(ntdll, "LdrRegisterDllNotification")) : nullptr;
			if (!registerNotification) {
				return;
			}
			void* cookie = nullptr;
			const NTSTATUS status = registerNotification(0, &OnDllNotification, nullptr, &cookie);
			gNotificationRegistered.store(status >= 0 && cookie != nullptr, std::memory_order_release);
		}

		void EnsureMidpointLog()
		{
			if (!gMidpointLogHooked.exchange(true)) {
				midpoint_fix::SetLogCallback(&MidpointLog);
			}
		}

		const char* MidpointFailureText(uint32_t a_code)
		{
			switch (a_code) {
			case 0: return "ok";
			case 1: return "no D3D12 device seen yet";
			case 2: return "GPU is not RTX 40 (Ada)";
			case 3: return "DLSS-G provider version not validated";
			case 4: return "DLSS-G provider layout not recognised";
			case 5: return "DLSS-G kernel source mismatch";
			case 6: return "kernel decompression failed";
			case 7: return "temporal kernel layout mismatch";
			case 8: return "rebuilt kernel identity mismatch";
			case 9: return "allocation failed";
			case 10: return "kernel publication failed";
			case 11: return "restart required (adapter changed)";
			case 12: return "provider not ready yet";
			default: return "unknown";
			}
		}
	}

	namespace
	{
		std::mutex gLoaderMutex;
		std::atomic<decltype(&::LoadLibraryA)> gLoadLibraryA{ nullptr };
		std::atomic<decltype(&::LoadLibraryW)> gLoadLibraryW{ nullptr };
		std::atomic<decltype(&::LoadLibraryExA)> gLoadLibraryExA{ nullptr };
		std::atomic<decltype(&::LoadLibraryExW)> gLoadLibraryExW{ nullptr };

		HMODULE WINAPI HookLoadLibraryA(LPCSTR a_name)
		{
			const auto original = gLoadLibraryA.load(std::memory_order_acquire);
			if (!original)
				return nullptr;
			const auto module = original(a_name);
			const auto error = GetLastError();
			MfgUnlock::InspectLoadedModule(module);
			SetLastError(error);
			return module;
		}

		HMODULE WINAPI HookLoadLibraryW(LPCWSTR a_name)
		{
			const auto original = gLoadLibraryW.load(std::memory_order_acquire);
			if (!original)
				return nullptr;
			const auto module = original(a_name);
			const auto error = GetLastError();
			MfgUnlock::InspectLoadedModule(module);
			SetLastError(error);
			return module;
		}

		HMODULE WINAPI HookLoadLibraryExA(LPCSTR a_name, HANDLE a_file, DWORD a_flags)
		{
			const auto original = gLoadLibraryExA.load(std::memory_order_acquire);
			if (!original)
				return nullptr;
			const auto module = original(a_name, a_file, a_flags);
			const auto error = GetLastError();
			MfgUnlock::InspectLoadedModule(module);
			SetLastError(error);
			return module;
		}

		HMODULE WINAPI HookLoadLibraryExW(LPCWSTR a_name, HANDLE a_file, DWORD a_flags)
		{
			const auto original = gLoadLibraryExW.load(std::memory_order_acquire);
			if (!original)
				return nullptr;
			const auto module = original(a_name, a_file, a_flags);
			const auto error = GetLastError();
			MfgUnlock::InspectLoadedModule(module);
			SetLastError(error);
			return module;
		}

		// Import-table hook on one module only. The original is published before the slot is
		// exchanged, and a slot that already points somewhere unexpected is left alone.
		template <typename Function>
		bool HookImport(HMODULE a_module, const char* a_name, Function a_replacement, std::atomic<Function>& a_original)
		{
			auto* base = reinterpret_cast<uint8_t*>(a_module);
			const auto* nt = ImageHeaders(a_module);
			if (!nt)
				return false;
			const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
			if (!directory.VirtualAddress || !directory.Size)
				return false;

			bool changed = false;
			auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
			for (; descriptor->Name; ++descriptor) {
				if (!descriptor->OriginalFirstThunk || !descriptor->FirstThunk)
					continue;
				const auto* library = reinterpret_cast<const char*>(base + descriptor->Name);
				if (_stricmp(library, "KERNEL32.dll") != 0 && _stricmp(library, "KERNELBASE.dll") != 0 && _strnicmp(library, "api-ms-win-core-libraryloader-", 30) != 0)
					continue;
				auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->OriginalFirstThunk);
				auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->FirstThunk);
				for (; names->u1.AddressOfData; ++names, ++thunk) {
					if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal))
						continue;
					const auto* import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
					if (std::strcmp(reinterpret_cast<const char*>(import->Name), a_name) != 0)
						continue;
					auto** slot = reinterpret_cast<void**>(&thunk->u1.Function);
					const auto current = reinterpret_cast<Function>(*slot);
					if (current == a_replacement)
						continue;
					const auto expected = a_original.load(std::memory_order_acquire);
					if (expected && expected != current) {
						logger::warn("[MFG Unlock] Skipped a conflicting loader import: {}", a_name);
						continue;
					}
					DWORD protection = 0;
					if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protection))
						continue;
					a_original.store(current, std::memory_order_release);
					InterlockedExchangePointer(reinterpret_cast<void* volatile*>(slot), reinterpret_cast<void*>(a_replacement));
					DWORD ignored = 0;
					if (!VirtualProtect(slot, sizeof(*slot), protection, &ignored))
						logger::warn("[MFG Unlock] Loader import protection restore failed: {}", a_name);
					changed = true;
				}
			}
			return changed;
		}
	}

	void InstallLoaderDiscovery(HMODULE a_module)
	{
		if (!a_module || (reinterpret_cast<uintptr_t>(a_module) & 3) || !IsEnabled())
			return;
		try {
			const std::wstring fullPath = LoadedModulePath(a_module);
			if (fullPath.empty())
				return;
			const auto name = fullPath.substr(fullPath.find_last_of(L"\\/") + 1);
			// Only NVIDIA's loading chain, never the game's or another plugin's loader.
			if (_wcsnicmp(name.c_str(), L"sl.", 3) != 0 && _wcsnicmp(name.c_str(), L"_nvngx", 6) != 0 && _wcsnicmp(name.c_str(), L"nvngx", 5) != 0 && !GetProcAddress(a_module, "slGetPluginFunction"))
				return;
			std::lock_guard lock(gLoaderMutex);
			bool changed = false;
			changed = HookImport(a_module, "LoadLibraryA", &HookLoadLibraryA, gLoadLibraryA) || changed;
			changed = HookImport(a_module, "LoadLibraryW", &HookLoadLibraryW, gLoadLibraryW) || changed;
			changed = HookImport(a_module, "LoadLibraryExA", &HookLoadLibraryExA, gLoadLibraryExA) || changed;
			changed = HookImport(a_module, "LoadLibraryExW", &HookLoadLibraryExW, gLoadLibraryExW) || changed;
			if (changed)
				logger::info("[MFG Unlock] Loader-return discovery installed on {}", Narrow(name.c_str()));
		} catch (...) {
			logger::warn("[MFG Unlock] Loader discovery installation failed");
		}
	}

	void InspectLoadedModule(HMODULE a_module)
	{
		if (!a_module || (reinterpret_cast<uintptr_t>(a_module) & 3) || !IsEnabled())
			return;
		try {
			InspectLoadedModule(a_module, std::wstring{});
		} catch (...) {
			logger::warn("[MFG Unlock] Loaded module inspection failed");
		}
	}

	void SetEnabled(bool a_enabled)
	{
		const bool previous = gEnabled.exchange(a_enabled, std::memory_order_acq_rel);
		if (previous != a_enabled) {
			logger::info("[MFG Unlock] {}", a_enabled ? "enabled" : "disabled");
			gDirty.store(true, std::memory_order_release);
		}
		if (a_enabled) {
			EnsureMidpointLog();
			RegisterDllNotification();
		}
	}

	bool IsEnabled()
	{
		return gEnabled.load(std::memory_order_acquire);
	}

	void ObserveD3D12Device(ID3D12Device* a_device)
	{
		// Recorded even while disabled: the device exists long before the user can flip the setting,
		// and verification is a one-off nvcuda query. Patching itself stays gated on IsEnabled().
		EnsureMidpointLog();
		midpoint_fix::ObserveD3D12Device(a_device);
		gDirty.store(true, std::memory_order_release);
	}

	void ObserveDlssgFunction(void* a_function)
	{
		if (!a_function) {
			return;
		}
		MEMORY_BASIC_INFORMATION memory{};
		if (VirtualQuery(a_function, &memory, sizeof(memory)) != sizeof(memory) || !memory.AllocationBase) {
			return;
		}

		HMODULE module = static_cast<HMODULE>(memory.AllocationBase);
		const ModuleRecord record = InspectLoadedModule(module, LoadedModulePath(module));
		if (!record.wrapperExport) {
			return;
		}

		const uintptr_t base = reinterpret_cast<uintptr_t>(module);
		const uintptr_t previous = gActiveWrapperBase.exchange(base, std::memory_order_acq_rel);
		gActiveWrapperPatched.store(record.wrapperPatched, std::memory_order_release);
		gActiveWrapperObserved.store(true, std::memory_order_release);
		if (previous != base) {
			logger::info("[MFG Unlock] Active DLSS-G wrapper: patched={} path={}", record.wrapperPatched, Narrow(record.path.c_str()));
		}
	}

	void PreloadProvider(const std::wstring& a_runtimeDirectory)
	{
		if (!IsEnabled() || a_runtimeDirectory.empty() || gPreloadedProvider.load(std::memory_order_acquire)) {
			return;
		}
		std::wstring path = a_runtimeDirectory;
		if (path.back() != L'\\' && path.back() != L'/') {
			path.push_back(L'\\');
		}
		path += L"nvngx_dlssg.dll";

		// Kept loaded for the life of the process on purpose: the reference has to outlive any
		// Streamline load/unload of the same file so the patched image never gets remapped clean.
		HMODULE module = LoadLibraryW(path.c_str());
		if (!module) {
			logger::warn("[MFG Unlock] Could not preload {} ({})", Narrow(path.c_str()), GetLastError());
			return;
		}
		gPreloadedProvider.store(module, std::memory_order_release);
		logger::info("[MFG Unlock] Preloaded DLSS-G provider {}", Narrow(path.c_str()));
		gDirty.store(true, std::memory_order_release);
	}

	void Rescan(bool a_force)
	{
		if (!IsEnabled()) {
			return;
		}
		// The midpoint fix needs the provider's kernel table filled in, which happens after the
		// module loads; retry until it publishes, but only once the byte patch is in (which implies a
		// validated provider version) and no more than once a second.
		bool retryMidpoint = false;
		if (midpoint_fix::AdapterVerified() && !midpoint_fix::Ready() && gNgxBytePatched.load(std::memory_order_acquire) > 0) {
			const uint64_t now = GetTickCount64();
			const uint64_t last = gLastRetryTick.load(std::memory_order_acquire);
			if (now - last >= kRetryIntervalMs) {
				gLastRetryTick.store(now, std::memory_order_release);
				retryMidpoint = true;
			}
		}
		if (!a_force && !retryMidpoint && !gDirty.exchange(false, std::memory_order_acq_rel)) {
			return;
		}
		gDirty.store(false, std::memory_order_release);
		InspectAlreadyLoadedModules();

		const bool ready = Ready();
		if (ready && !gLoggedReady.exchange(true)) {
			logger::info("[MFG Unlock] Ready: DLSS-G may generate up to {} frames on this adapter (wrapper compiled maximum {})", MaximumGeneratedFrames(), gActiveWrapperCompiledMaximum.load(std::memory_order_acquire));
		} else if (!ready) {
			gLoggedReady.store(false, std::memory_order_release);
		}
	}

	bool Ready()
	{
		// Never combine a patched wrapper with an ambiguous provider: more than one DLSS-G provider
		// module in the process fails closed to the runtime's own clamp (upstream v1.2).
		return IsEnabled() &&
		       gActiveWrapperObserved.load(std::memory_order_acquire) &&
		       gActiveWrapperPatched.load(std::memory_order_acquire) &&
		       gProviderModules.load(std::memory_order_acquire) == 1 &&
		       gPatchedNgx.load(std::memory_order_acquire) == 1 &&
		       midpoint_fix::Ready();
	}

	uint32_t MaximumGeneratedFrames()
	{
		if (!Ready())
			return 0;
		return universal_wrapper_profile::SafeMaximumMultiplier(gActiveWrapperCompiledMaximum.load(std::memory_order_acquire)) - 1;
	}

	Status GetStatus()
	{
		Status status{};
		status.enabled = IsEnabled();
		status.adapterVerified = midpoint_fix::AdapterVerified();
		status.activeWrapperObserved = gActiveWrapperObserved.load(std::memory_order_acquire);
		status.activeWrapperPatched = gActiveWrapperPatched.load(std::memory_order_acquire);
		status.wrapperCandidates = gWrapperCandidates.load(std::memory_order_acquire);
		status.wrapperPatched = gPatchedWrappers.load(std::memory_order_acquire);
		status.ngxCandidates = gNgxCandidates.load(std::memory_order_acquire);
		status.ngxPatched = gPatchedNgx.load(std::memory_order_acquire);
		status.providerModules = gProviderModules.load(std::memory_order_acquire);
		status.compiledMaximum = gActiveWrapperCompiledMaximum.load(std::memory_order_acquire);
		status.midpointReady = midpoint_fix::Ready();
		status.midpointFailure = midpoint_fix::FailureCode();
		{
			std::lock_guard lock(gModuleMutex);
			for (const auto& record : gModuleRecords) {
				if (record.ngxExport && record.providerVersionKnown) {
					status.providerVersionKnown = true;
					status.providerVersionSupported = record.providerVersionSupported;
					status.providerMajor = record.providerVersion.major;
					status.providerMinor = record.providerVersion.minor;
					status.providerBuild = record.providerVersion.build;
					break;
				}
			}
		}
		status.ready = Ready();
		return status;
	}

	std::string Describe()
	{
		const Status s = GetStatus();
		if (!s.enabled) {
			return "Off";
		}
		if (s.ready) {
			return std::format("Active: DLSS-G {}.{}.{} patched, up to {} generated frames", s.providerMajor, s.providerMinor, s.providerBuild, MaximumGeneratedFrames());
		}
		if (!s.adapterVerified) {
			return std::format("Inactive: {}", MidpointFailureText(s.midpointFailure));
		}
		if (s.providerVersionKnown && !s.providerVersionSupported) {
			return std::format("Inactive: DLSS-G {}.{}.{} is not a validated build (310.1 through 310.9 provider builds are)", s.providerMajor, s.providerMinor, s.providerBuild);
		}
		if (!s.activeWrapperObserved) {
			return "Inactive: DLSS-G runtime not initialised yet";
		}
		if (!s.activeWrapperPatched) {
			return "Inactive: sl.dlss_g.dll pattern not found";
		}
		if (s.providerModules > 1)
			return std::format("Inactive: {} DLSS-G provider modules loaded; the active one is ambiguous", s.providerModules);
		if (s.ngxPatched == 0) {
			if (s.ngxCandidates == 0) {
				return "Inactive: nvngx_dlssg.dll not loaded yet (enable frame generation)";
			}
			return std::format("Inactive: nvngx_dlssg.dll patch pending ({})", MidpointFailureText(s.midpointFailure));
		}
		return std::format("Inactive: {}", MidpointFailureText(s.midpointFailure));
	}
}
