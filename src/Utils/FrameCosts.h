#pragma once

// Per-frame CPU costs that live outside the GPU profiler: time the render thread spends in
// Present (vsync, GPU-bound and frame-generation pacing waits), the frame limiter, the Reflex
// sleep, frame generation setup and the Bottled Shaders interface. Written from the render
// thread, read by the performance overlay's detailed breakdown.
//
// Accumulating counters are double buffered: writers add into the current frame, BeginFrame
// (called from State::Reset at the top of the present hook) publishes it as the last frame and
// clears the current one. Readers only ever see a complete frame, whichever point of the frame
// they run at.

#include <atomic>
#include <cstdint>

#include <Windows.h>

namespace FrameCosts
{
	struct Counter
	{
		std::atomic<float> current{ 0.0f };
		std::atomic<float> last{ 0.0f };

		void Add(float a_ms) { current.store(current.load(std::memory_order_relaxed) + a_ms, std::memory_order_relaxed); }
		void Set(float a_ms) { last.store(a_ms, std::memory_order_relaxed); }
		float Last() const { return last.load(std::memory_order_relaxed); }
		void Flip()
		{
			last.store(current.load(std::memory_order_relaxed), std::memory_order_relaxed);
			current.store(0.0f, std::memory_order_relaxed);
		}
	};

	inline Counter presentMs;         ///< Whole swap chain Present call as the game sees it (Set once per present)
	inline Counter frameLimiterMs;    ///< Upscaling::FrameLimiter (waitable object + sleep)
	inline Counter reflexSleepMs;     ///< slReflexSleep
	inline Counter frameGenSetupMs;   ///< FidelityFX configure/dispatch or DLSS-G options/tagging on the CPU
	inline Counter uiDrawMs;          ///< ImGui frame: NewFrame through draw data submission (Set once per frame)
	inline Counter overlayDrawMs;     ///< The performance overlay window alone (Set once per frame)
	inline Counter dlssBridgeCpuMs;   ///< D3D12 DLSS bridge: copies, submit and the CPU-side fence wait
	inline Counter dlssEvalGpuMs;     ///< DLSS super resolution on the D3D12 queue (timestamp queries; lags a few frames)
	inline Counter dlssBridgeGpuMs;   ///< Whole D3D12 bridge command list: DLSS plus neural rendering (timestamp queries)
	inline Counter drawHookMs;        ///< Bottled Shaders per-draw hook (State::Draw) summed over the frame; only measured while the overlay's draw timing runs

	inline double TicksToMs()
	{
		static double ticksToMs = [] {
			LARGE_INTEGER frequency{};
			QueryPerformanceFrequency(&frequency);
			return frequency.QuadPart ? 1000.0 / static_cast<double>(frequency.QuadPart) : 0.0;
		}();
		return ticksToMs;
	}

	inline int64_t Now()
	{
		LARGE_INTEGER now{};
		QueryPerformanceCounter(&now);
		return now.QuadPart;
	}

	inline float ElapsedMs(int64_t a_start)
	{
		return static_cast<float>(static_cast<double>(Now() - a_start) * TicksToMs());
	}

	/** @brief Stores the elapsed milliseconds of a scope as the counter's last value (one sample per frame). */
	class Scope
	{
	public:
		explicit Scope(Counter& a_target) :
			target(a_target), start(Now()) {}
		~Scope() { target.Set(ElapsedMs(start)); }
		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;

	private:
		Counter& target;
		int64_t start;
	};

	/** @brief Adds the elapsed milliseconds of a scope onto the counter's current frame (split measurements). */
	class AccumulatingScope
	{
	public:
		explicit AccumulatingScope(Counter& a_target) :
			target(a_target), start(Now()) {}
		~AccumulatingScope() { target.Add(ElapsedMs(start)); }
		AccumulatingScope(const AccumulatingScope&) = delete;
		AccumulatingScope& operator=(const AccumulatingScope&) = delete;

	private:
		Counter& target;
		int64_t start;
	};

	/** @brief Publishes the accumulated counters of the frame that just ended and starts a new one. */
	inline void BeginFrame()
	{
		frameGenSetupMs.Flip();
		dlssBridgeCpuMs.Flip();
		frameLimiterMs.Flip();
		reflexSleepMs.Flip();
		drawHookMs.Flip();
	}
}
