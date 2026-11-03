#pragma once

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>

namespace DCLF::Integrations::WetnessEffects
{
	inline constexpr std::uint32_t kPrecipitationOcclusionRegister = 70;

	Api::Feature Integration();
}
