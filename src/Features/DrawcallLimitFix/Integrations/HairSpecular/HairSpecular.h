#pragma once

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>

namespace DCLF::Integrations::HairSpecular
{
	inline constexpr std::uint32_t kTangentShiftRegister = 73;

	Api::Feature Integration();
}
