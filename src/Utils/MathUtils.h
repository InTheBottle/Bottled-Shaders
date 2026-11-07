#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

namespace Util
{
	/** @brief Mixes a 32-bit value into a running 64-bit hash. */
	inline std::uint64_t HashCombine(std::uint64_t h, std::uint32_t v) noexcept
	{
		return h ^ (static_cast<std::uint64_t>(v) + 0x9e3779b9ull + (h << 6) + (h >> 2));
	}

	/** @brief Mixes a float into a running 64-bit hash via its bit pattern. */
	inline std::uint64_t HashCombineFloat(std::uint64_t h, float f) noexcept
	{
		return HashCombine(h, std::bit_cast<std::uint32_t>(f));
	}

	/** @brief Rounds a value to the nearest multiple of step, collapsing sub-step jitter before hashing. */
	inline float QuantizeFloat(float f, float step) noexcept
	{
		return std::round(f / step) * step;
	}

	inline float ClampFinite(float value, float fallback, float min, float max) noexcept
	{
		return std::clamp(std::isfinite(value) ? value : fallback, min, max);
	}

	/** @brief Hermite step from edge0 to edge1, matching HLSL smoothstep. */
	inline float Smoothstep(float edge0, float edge1, float x) noexcept
	{
		const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}

	/** @brief Moves current toward target at riseRate or fallRate per unit of step, without overshooting. */
	inline float Approach(float current, float target, float riseRate, float fallRate, float step) noexcept
	{
		return target > current ? std::min(target, current + riseRate * step) : std::max(target, current - fallRate * step);
	}
}
