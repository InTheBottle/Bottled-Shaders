#include "Features/DrawcallLimitFix/Integrations/SnowCover/SnowCover.h"

#include "Globals.h"

#include "Features/SnowCover.h"

#include <utility>

namespace DCLF::Integrations::SnowCover
{
	namespace
	{
		static_assert(std::tuple_size_v<decltype(::SnowCover::views)> == kTextureRegisters.size());

		template <std::size_t I>
		void* SnowView()
		{
			const auto& snowCover = globals::features::snowCover;
			if (!snowCover.loaded || !snowCover.wsettings.EnableSnowCover)
				return nullptr;
			return snowCover.views[I].get();
		}

		template <std::size_t... I>
		void RegisterViews(Api::InstallContext& a_context, std::index_sequence<I...>)
		{
			(a_context.RegisterFrameTexture({ .name = kTextureNames[I], .textureRegister = kTextureRegisters[I], .view = &SnowView<I>, .capturedWhenNull = true }), ...);
		}

		void Install(Api::InstallContext& a_context)
		{
			RegisterViews(a_context, std::make_index_sequence<kTextureRegisters.size()>{});
		}
	}

	Api::Feature Integration()
	{
		return { "snow cover", &Install };
	}
}
