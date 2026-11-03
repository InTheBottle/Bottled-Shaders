#pragma once

#include <dxgiformat.h>

#include <array>

namespace DCLF::ReverseDepth
{
	bool Active();
	bool Reversed(DXGI_FORMAT a_format);
	void MirrorRange(float& a_minDepth, float& a_maxDepth);
	void ReverseCamera(std::array<float, 16>& a_proj, std::array<float, 16>& a_viewProj);
}
