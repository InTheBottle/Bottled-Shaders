#pragma once

namespace LODShadowsRE
{
	inline RE::BGSTerrainManager* GetActiveTerrainManager()
	{
		return *reinterpret_cast<RE::BGSTerrainManager**>(REL::RelocationID(516111, 402262).address());
	}

	inline RE::BSSpinLock& GetCameraTerrainLock()
	{
		return *reinterpret_cast<RE::BSSpinLock*>(REL::RelocationID(516179, 402330).address());
	}

	inline RE::NiPointer<RE::NiNode>& GetLODLandRoot()
	{
		return *reinterpret_cast<RE::NiPointer<RE::NiNode>*>(REL::RelocationID(516173, 402324).address());
	}

	inline RE::NiPointer<RE::NiNode>& GetLODObjectRoot()
	{
		return *reinterpret_cast<RE::NiPointer<RE::NiNode>*>(REL::RelocationID(516174, 402325).address());
	}

	inline RE::NiPointer<RE::NiNode>& GetTreeNode()
	{
		return *reinterpret_cast<RE::NiPointer<RE::NiNode>*>(REL::RelocationID(516170, 402321).address());
	}

	inline bool IsLoadScreenUp()
	{
		return *reinterpret_cast<bool*>(REL::RelocationID(516108, 402259).address());
	}

	inline RE::NiPointer<RE::NiTexture>& GetDistantTreeAtlas()
	{
		return *reinterpret_cast<RE::NiPointer<RE::NiTexture>*>(REL::RelocationID(516086, 402199).address());
	}

	inline bool IsMapMode(const RE::BGSTerrainManager* a_manager)
	{
		return *reinterpret_cast<const bool*>(a_manager);
	}

	inline std::uint8_t GetLODMode(RE::NiAVObject* a_object)
	{
		return REL::RelocateMember<std::uint8_t>(a_object, 0x108, 0x120);
	}

	inline const std::uint32_t* GetMeshLODTriangleCounts(RE::BSGeometry* a_geometry)
	{
		return &REL::RelocateMember<std::uint32_t>(a_geometry, 0x160, 0x1A0);
	}

	inline constexpr std::size_t kMeshLODLevels = 3;
}
