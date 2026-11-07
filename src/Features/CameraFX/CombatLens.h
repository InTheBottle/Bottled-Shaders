#pragma once

#include <deque>
#include <mutex>
#include <vector>

namespace CameraFXLens
{
	inline constexpr uint32_t kMaxSplats = 24;

	enum class SplatType : uint32_t
	{
		kBlood,
		kDarkBlood,
		kOil,
		kVenom,
		kIce,
		kFire,
		kGrit
	};

	struct Splat
	{
		SplatType type = SplatType::kBlood;
		float u = 0.5f;
		float v = 0.5f;
		float radius = 0.1f;
		float angle = 0.0f;
		float age = 0.0f;
		float life = 5.0f;
		float seed = 0.0f;
		float strength = 1.0f;
	};

	struct CombatOptions
	{
		bool blood = true;
		float bloodStrength = 1.0f;
		bool elemental = true;
		bool flash = true;
		float flashStrength = 1.0f;
		bool slash = true;
		bool shake = true;
		float shakeStrength = 1.0f;
		bool stamina = true;
		bool photosensitiveSafe = true;
	};

	struct CombatState
	{
		float flash = 0.0f;
		float flashWarmth = 0.0f;
		float slash = 0.0f;
		float slashAngle = 0.0f;
		float hitU = 0.5f;
		float hitV = 0.5f;
		float hitAge = 99.0f;
		float shock = 0.0f;
		float shockAge = 99.0f;
		float shockSeed = 0.0f;
		float shockLasting = 0.0f;
		float fire = 0.0f;
		float frost = 0.0f;
		float staminaLow = 0.0f;
		float exertion = 0.0f;
		float exhaleAge = 99.0f;
		float exhaleStrength = 0.0f;
		float exhaleSeed = 0.0f;
		bool firstPerson = false;
		std::vector<Splat> splats;
	};

	class CombatLens
	{
	public:
		void Register();
		void Update(float a_step, float a_delta, bool a_enabled, const CombatOptions& a_options);
		void Disable();
		void Reset();
		const CombatState& Get() const { return state; }

		void OnHit(const RE::TESHitEvent& a_event);

	private:
		struct Pending
		{
			SplatType type = SplatType::kBlood;
			float u = 0.5f;
			float v = 0.5f;
			uint32_t count = 1;
			float strength = 1.0f;
			float flash = 0.0f;
			float flashWarmth = 0.0f;
			float slash = 0.0f;
			float slashAngle = 0.0f;
			float shock = 0.0f;
			float fire = 0.0f;
			float frost = 0.0f;
			bool splat = false;
		};

		void Queue(const Pending& a_pending);
		void Spawn(const Pending& a_pending, const CombatOptions& a_options);
		void ScanActiveEffects();
		void UpdateBreathing(float a_step);
		static float NextRandom(uint32_t& a_state);
		bool Ready(uint32_t a_key, float a_seconds);

		std::mutex pendingLock;
		std::deque<Pending> pending;
		CombatOptions options;
		bool enabled = false;
		uint32_t hitRandomState = 0x85EBCA6Bu;
		CombatState state;
		float persistentFire = 0.0f;
		float persistentFrost = 0.0f;
		float persistentShock = 0.0f;
		float scanTimer = 0.0f;
		float breathTimer = 0.0f;
		float flashCooldown = 0.0f;
		uint32_t breathCount = 0;
		uint32_t shockCount = 0;
		uint32_t randomState = 0x9E3779B9u;
		float cooldowns[8] = {};
	};
}
