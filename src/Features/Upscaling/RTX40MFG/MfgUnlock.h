#pragma once

// RTX 40 Multi Frame Generation unlock.
//
// Derived from dashdogy/RTX40MFG-Unlock (MIT, see LICENSE in this folder). The upstream project is a
// Cyberpunk ASI that intercepts the game's Streamline imports; this plugin owns every Streamline call
// itself, so only the parts that act on the loaded DLSS-G modules survive here:
//
//   1. sl.dlss_g.dll   NOP a cmov that clamps numFramesToGenerateMax to the hardware value, so the
//                      wrapper reports 5 generated frames on Ada instead of 1.
//   2. nvngx_dlssg.dll NOP the device-support branch that refuses to create a multi-frame feature.
//   3. nvngx_dlssg.dll Publish a rebuilt sm_89 temporal kernel (midpoint_fix) so generated samples
//                      land at their own temporal positions instead of collapsing to the midpoint.
//
// Everything fails closed: unless the active adapter is Ada (compute capability 8.9), the provider
// version is one the byte patterns were validated against, and all three steps succeeded, Ready()
// stays false and the caller keeps the runtime's own clamp. Patches touch mapped memory only.

#include <Windows.h>

#include <cstdint>
#include <string>

struct ID3D12Device;

namespace MfgUnlock
{
	struct Status
	{
		bool enabled = false;
		bool adapterVerified = false;
		bool activeWrapperObserved = false;
		bool activeWrapperPatched = false;
		uint32_t wrapperCandidates = 0;
		uint32_t wrapperPatched = 0;
		uint32_t ngxCandidates = 0;
		uint32_t ngxPatched = 0;
		uint32_t providerModules = 0;      ///< DLSS-G provider modules found; more than one fails closed
		uint32_t compiledMaximum = 0;      ///< The active wrapper's compiled generated-frame maximum
		bool providerVersionKnown = false;
		bool providerVersionSupported = false;
		uint16_t providerMajor = 0;
		uint16_t providerMinor = 0;
		uint16_t providerBuild = 0;
		bool midpointReady = false;
		uint32_t midpointFailure = 0;
		bool ready = false;
	};

	/// Enable or disable the unlock. Disabling never reverts patches already applied; it only stops
	/// new ones and makes Ready() return false so callers fall back to the runtime clamp.
	void SetEnabled(bool a_enabled);
	bool IsEnabled();

	/// Load nvngx_dlssg.dll from the Streamline runtime folder now. Streamline loads it lazily, possibly
	/// inside the first slDLSSGSetOptions, which would create the NGX feature before the device-support
	/// patch exists. Loading the same path early makes it resident (same HMODULE) so the patch lands
	/// first. No-op unless the unlock is enabled.
	void PreloadProvider(const std::wstring& a_runtimeDirectory);

	/// Call with the D3D12 device Streamline will use, before slSetD3DDevice. Verifies the adapter is
	/// Ada through nvcuda and unblocks patching.
	void ObserveD3D12Device(ID3D12Device* a_device);

	/// Call with the pointer slGetFeatureFunction returned for slDLSSGSetOptions or slDLSSGGetState.
	/// Identifies which loaded module is the live DLSS-G wrapper.
	void ObserveDlssgFunction(void* a_function);

	/// Upstream v1.2 loader-return discovery: hooks the LoadLibrary imports of one NVIDIA module
	/// (sl.*, nvngx*, or anything exporting slGetPluginFunction) so a provider it loads is inspected
	/// and patched before the loader returns to it. Never touches the game's own imports.
	void InstallLoaderDiscovery(HMODULE a_module);

	/// Inspect one module (and hook its loader imports if it is NVIDIA's); used by the discovery hooks.
	void InspectLoadedModule(HMODULE a_module);

	/// Re-inspect loaded modules. Cheap when nothing changed since the last call unless a_force.
	void Rescan(bool a_force = false);

	/// True when every patch is in place on the modules that are actually in use.
	bool Ready();

	/// The active wrapper's compiled maximum (1, 3 or 5) when Ready(), otherwise 0 meaning "use the runtime's reported maximum".
	uint32_t MaximumGeneratedFrames();

	Status GetStatus();

	/// One line for the settings page.
	std::string Describe();
}
