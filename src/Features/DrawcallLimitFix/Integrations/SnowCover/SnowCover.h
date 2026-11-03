#pragma once

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace DCLF::Integrations::SnowCover
{
	inline constexpr std::array<std::uint32_t, 7> kTextureRegisters{ 38, 39, 40, 41, 42, 43, 44 };
	inline constexpr std::array<std::string_view, 7> kTextureNames{ "snow cover snow albedo", "snow cover snow normal", "snow cover snow rmaos",
		"snow cover ice albedo", "snow cover ice normal", "snow cover ice rmaos", "snow cover snow map" };

	Api::Feature Integration();
}
