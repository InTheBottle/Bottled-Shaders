#include "Features/DrawcallLimitFix/Core/Engine/ReverseDepth.h"

#include "Features/ReverseZ.h"
#include "Globals.h"

#include <cmath>

namespace DCLF::ReverseDepth
{
	bool Active()
	{
		const auto& reverseZ = globals::features::reverseZ;
		return reverseZ.IsActive() && reverseZ.IsConvertedDepthTarget(static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN));
	}

	bool Reversed(DXGI_FORMAT a_format)
	{
		switch (a_format) {
		case DXGI_FORMAT_R32G8X24_TYPELESS:
		case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
		case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
			return Active();
		default:
			return false;
		}
	}

	void MirrorRange(float& a_minDepth, float& a_maxDepth)
	{
		const float minDepth = a_minDepth;
		a_minDepth = (1.0f - a_maxDepth) * globals::features::reverseZ.GetViewportBiasScale();
		a_maxDepth = 1.0f - minDepth;
	}

	void ReverseCamera(std::array<float, 16>& a_proj, std::array<float, 16>& a_viewProj)
	{
		const bool perspective = std::abs(a_proj[15]) < 1e-4f && std::abs(std::abs(a_proj[11]) - 1.0f) < 1e-3f;
		if (!perspective || a_proj[10] * a_proj[11] <= 0.0f)
			return;
		for (std::uint32_t row = 0; row < 4; ++row) {
			a_proj[row * 4 + 2] = a_proj[row * 4 + 3] - a_proj[row * 4 + 2];
			a_viewProj[row * 4 + 2] = a_viewProj[row * 4 + 3] - a_viewProj[row * 4 + 2];
		}
	}
}
