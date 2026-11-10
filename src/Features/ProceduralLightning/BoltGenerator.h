// SPDX-License-Identifier: MIT AND (LicenseRef-GPL-3.0-with-Modding-Exception WITH GPL-3.0-linking-source-exception)
// Bolt generation adapted from bevy_procedural_lightning, Copyright (c) 2026 Procedural Lightning Contributors (MIT)

#pragma once

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace Lightning
{
	inline constexpr float kTau = 6.28318530718f;

	struct BoltConfig
	{
		float Alpha = 0.5f;
		float Beta = 0.4f;
		float Gamma = 0.3f;
		uint32_t MaxDepth = 8;
		uint32_t MaxBranchDepth = 3;
	};

	struct BoltNode
	{
		float3 Position;
		float Energy = 0.0f;
		float Arrival = 0.0f;
	};

	struct BoltSegment
	{
		BoltNode Start;
		BoltNode End;
		uint32_t BranchDepth = 0;
	};

	template <class Rng>
	void GenerateBolt(const float3& a_start, const float3& a_end, const BoltConfig& a_config, size_t a_maxSegments, Rng& a_rng, std::vector<BoltSegment>& o_segments)
	{
		struct Pending
		{
			BoltNode Start;
			BoltNode End;
			uint32_t Depth;
			uint32_t BranchDepth;
		};

		auto uniform = [&a_rng](float a_min, float a_max) {
			return std::uniform_real_distribution<float>(a_min, a_max)(a_rng);
		};

		const float boltLength = (a_end - a_start).Length();
		if (boltLength < 1.0f || a_maxSegments == 0)
			return;

		std::vector<Pending> pending;
		pending.reserve(64);
		pending.push_back({ { a_start, 1.0f, 0.0f }, { a_end, 0.8f, 1.0f }, 0, 0 });

		const size_t firstSegment = o_segments.size();
		for (size_t next = 0; next < pending.size(); ++next) {
			const Pending current = pending[next];

			const bool hasBudget = (o_segments.size() - firstSegment) + (pending.size() - next - 1) + 3 <= a_maxSegments;
			const float3 segment = current.End.Position - current.Start.Position;
			const float length = segment.Length();
			if (current.Depth >= a_config.MaxDepth || !hasBudget || length < 1.0f) {
				o_segments.push_back({ current.Start, current.End, current.BranchDepth });
				continue;
			}

			const float3 direction = segment / length;
			const float3 reference = std::abs(direction.z) < 0.9f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
			float3 side = direction.Cross(reference);
			side.Normalize();
			const float3 up = side.Cross(direction);

			auto perpendicular = [&]() {
				const float angle = uniform(0.0f, kTau);
				return side * std::cos(angle) + up * std::sin(angle);
			};

			const BoltNode middle{
				(current.Start.Position + current.End.Position) * 0.5f + perpendicular() * (uniform(-1.0f, 1.0f) * a_config.Beta * length),
				(current.Start.Energy + current.End.Energy) * 0.5f,
				(current.Start.Arrival + current.End.Arrival) * 0.5f
			};

			pending.push_back({ current.Start, middle, current.Depth + 1, current.BranchDepth });
			pending.push_back({ middle, current.End, current.Depth + 1, current.BranchDepth });

			if (current.BranchDepth >= a_config.MaxBranchDepth)
				continue;

			const float branchChance = a_config.Gamma * std::exp(-a_config.Alpha * static_cast<float>(current.Depth + current.BranchDepth));
			if (uniform(0.0f, 1.0f) >= branchChance)
				continue;

			const float branchLength = length * 0.5f * std::exp(-a_config.Alpha * static_cast<float>(current.BranchDepth));
			float3 toTarget = a_end - middle.Position;
			toTarget.Normalize();
			float3 branchDirection = perpendicular() * uniform(-0.6f, 0.6f) + toTarget * uniform(0.3f, 0.8f);
			branchDirection.Normalize();

			const BoltNode tip{ middle.Position + branchDirection * branchLength, 0.0f, middle.Arrival + branchLength / boltLength };
			pending.push_back({ middle, tip, current.Depth + 1, current.BranchDepth + 1 });
		}
	}
}
