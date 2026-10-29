# AMD Anti-Lag 2

Anti-Lag 2 automatically accompanies FSR Frame Generation when AMD's driver
interface is available. It reduces latency by delaying CPU work immediately
before Skyrim samples input. There is no independent enable switch or FPS cap.

## Availability

The existing FSR startup path initializes Anti-Lag on its presentation device.
Only `S_OK` enables the integration. Unsupported hardware or drivers retain the
ordinary FSR path without Anti-Lag input hooks or presentation notifications.
Native D3D11 does not initialize Anti-Lag.

Frame Generation requires a restart. Temporary suspension of generation in
menus or loading screens does not change session eligibility. Under
**Frame Generation > Current session**, a loaded FSR session displays
**Anti-Lag 2: Active** or **Unavailable**. Active requires an installed context
and an accepted enabled driver state.

## Integration

The header-only [AMD SDK](https://github.com/GPUOpen-LibrariesAndSDKs/AntiLag2-SDK)
is pinned as `extern/AntiLag2-SDK` at `v2.0.0a`. Initialize it with
`git submodule update --init extern/AntiLag2-SDK` before configuring a build.

`DX12SwapChain` owns initialization, optional input-hook installation and
presentation. The project adapter in `Upscaling/antilag2sdk.h` checks driver
state changes before caching them so rejected changes remain retryable.
The SDK FPS limit stays zero; the existing display-refresh limit remains.

Eligible sessions use a separate presentation specialization. It publishes the
SDK context through FSR's swap-chain private-data handshake and marks rendering
completion after command submission, before Present. Ordinary sessions compile
out these notifications. The context lives until process exit: `DX12SwapChain`
is only destroyed at DLL detach, after the FFX and driver DLLs have detached and
the FFX presenter thread is gone, so it neither drains presents nor releases the
interface there.

## Validation

Runtime checks require supported FSR-FG, unsupported FSR-FG and FG-off
sessions. Initialization logs its HRESULT and availability; runtime failure
messages are limited to the first occurrence per category. Successful calls
produce no per-frame logging. Initialization and API success establish interface
operation, not measured latency reduction or generated-frame presentation.
