#include "CombatLens.h"

#include "Globals.h"
#include "Utils/MathUtils.h"

#include <algorithm>

namespace CameraFXLens
{
	namespace
	{
		enum Cooldown : uint32_t
		{
			kCoolBlood,
			kCoolElement,
			kCoolSlash,
			kCoolShake,
			kCoolOwnBlood,
			kCoolGrit,
			kCoolBlur,
			kCoolSpellBlur
		};

		enum class Element
		{
			kNone,
			kFire,
			kFrost,
			kShock,
			kPoison
		};

		constexpr float kSafeZone = 0.22f;
		constexpr float kScanInterval = 0.2f;
		constexpr float kMeleeBloodDistance = 420.0f;
		constexpr float kRangedBloodDistance = 280.0f;
		constexpr float kBigActorHeight = 260.0f;
		constexpr float kSafeFlashCap = 0.25f;
		constexpr float kFlashInterval = 0.5f;
		constexpr float kCalmBreathsPerMinute = 14.0f;
		constexpr float kWindedBreathPeriod = 1.3f;
		constexpr float kMaxShake = 1.5f;
		constexpr float kHitBlurDecay = 2.2f;
		constexpr float kBlurInterval = 0.12f;
		constexpr float kSpellBlurInterval = 1.0f;

		class HitSink final : public RE::BSTEventSink<RE::TESHitEvent>
		{
		public:
			explicit HitSink(CombatLens* a_lens) :
				lens(a_lens) {}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_event, RE::BSTEventSource<RE::TESHitEvent>*) override
			{
				if (a_event && lens)
					lens->OnHit(*a_event);
				return RE::BSEventNotifyControl::kContinue;
			}

		private:
			CombatLens* lens;
		};

		bool ProjectPoint(const RE::NiPoint3& a_world, float& o_u, float& o_v)
		{
			auto* camera = RE::Main::WorldRootCamera();
			float x = 0.0f;
			float y = 0.0f;
			float z = 0.0f;
			if (!camera || !camera->WorldPtToScreenPt3(a_world, x, y, z, 1e-5f) || z <= 0.0f || !std::isfinite(x) || !std::isfinite(y))
				return false;
			o_u = std::clamp(x, 0.05f, 0.95f);
			o_v = std::clamp(1.0f - y, 0.05f, 0.95f);
			return true;
		}

		Element ClassifySpell(RE::MagicItem* a_spell)
		{
			if (!a_spell)
				return Element::kNone;
			for (const auto effect : a_spell->effects) {
				const auto base = effect ? effect->baseEffect : nullptr;
				if (!base || !base->IsHostile())
					continue;
				switch (base->data.resistVariable) {
				case RE::ActorValue::kResistFire:
					return Element::kFire;
				case RE::ActorValue::kResistFrost:
					return Element::kFrost;
				case RE::ActorValue::kResistShock:
					return Element::kShock;
				case RE::ActorValue::kPoisonResist:
					return Element::kPoison;
				default:
					break;
				}
			}
			return Element::kNone;
		}

		bool GetBloodType(RE::Actor* a_actor, SplatType& o_type)
		{
			if (!a_actor || a_actor->IsGhost())
				return false;
			if (a_actor->HasKeywordString("ActorTypeDwarven"))
				o_type = SplatType::kOil;
			else if (a_actor->HasKeywordString("ActorTypeUndead"))
				o_type = SplatType::kDarkBlood;
			else
				o_type = SplatType::kBlood;
			return true;
		}

		float GetLife(SplatType a_type)
		{
			switch (a_type) {
			case SplatType::kIce:
				return 3.5f;
			case SplatType::kFire:
				return 1.3f;
			case SplatType::kGrit:
				return 3.0f;
			default:
				return 8.0f;
			}
		}

		float GetSize(SplatType a_type, float a_strength)
		{
			switch (a_type) {
			case SplatType::kVenom:
				return 0.14f;
			case SplatType::kIce:
				return 0.18f;
			case SplatType::kFire:
				return 0.2f;
			case SplatType::kGrit:
				return 0.16f;
			default:
				return 0.07f + 0.06f * std::clamp(a_strength, 0.0f, 1.0f);
			}
		}

		bool IsGore(SplatType a_type)
		{
			return a_type == SplatType::kBlood || a_type == SplatType::kDarkBlood || a_type == SplatType::kOil;
		}
	}

	float GetHealthFraction(RE::Actor* a_actor)
	{
		auto* values = a_actor ? a_actor->AsActorValueOwner() : nullptr;
		if (!values)
			return 1.0f;
		const float maximum = values->GetPermanentActorValue(RE::ActorValue::kHealth);
		const float current = values->GetActorValue(RE::ActorValue::kHealth);
		return std::isfinite(maximum) && maximum > 1.0f && std::isfinite(current) ? std::clamp(current / maximum, 0.0f, 1.0f) : 1.0f;
	}

	void CombatLens::Register()
	{
		static HitSink sink(this);
		if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton())
			holder->AddEventSink<RE::TESHitEvent>(&sink);
	}

	void CombatLens::Disable()
	{
		if (enabled)
			Reset();
		const std::scoped_lock guard(pendingLock);
		enabled = false;
	}

	void CombatLens::Reset()
	{
		{
			const std::scoped_lock guard(pendingLock);
			pending.clear();
			std::fill(std::begin(cooldowns), std::end(cooldowns), 0.0f);
		}
		state = {};
		persistentFire = persistentFrost = persistentShock = 0.0f;
		flashCooldown = 0.0f;
	}

	float CombatLens::NextRandom(uint32_t& a_state)
	{
		a_state ^= a_state << 13;
		a_state ^= a_state >> 17;
		a_state ^= a_state << 5;
		return static_cast<float>(a_state & 0xFFFFFF) / static_cast<float>(0x1000000);
	}

	bool CombatLens::Ready(uint32_t a_key, float a_seconds)
	{
		if (a_key >= std::size(cooldowns) || cooldowns[a_key] > 0.0f)
			return false;
		cooldowns[a_key] = a_seconds;
		return true;
	}

	void CombatLens::Queue(const Pending& a_pending)
	{
		if (pending.size() < kMaxSplats)
			pending.push_back(a_pending);
	}

	void CombatLens::OnHit(const RE::TESHitEvent& a_event)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* target = a_event.target.get();
		auto* cause = a_event.cause.get();
		if (!player || !target || !cause)
			return;

		const std::scoped_lock guard(pendingLock);
		if (!enabled)
			return;
		auto Random = [this]() { return NextRandom(hitRandomState); };

		const bool blocked = a_event.flags.any(RE::TESHitEvent::Flag::kHitBlocked);
		const bool power = a_event.flags.any(RE::TESHitEvent::Flag::kPowerAttack);
		const bool bash = a_event.flags.any(RE::TESHitEvent::Flag::kBashAttack);
		auto* source = a_event.source ? RE::TESForm::LookupByID(a_event.source) : nullptr;
		auto* spell = source ? source->As<RE::MagicItem>() : nullptr;
		auto* weapon = source ? source->As<RE::TESObjectWEAP>() : nullptr;
		const bool ranged = a_event.projectile != 0;

		if (cause == player && target != player) {
			auto* victim = target->As<RE::Actor>();
			SplatType type = SplatType::kBlood;
			if (!victim || blocked || spell || !options.blood || !GetBloodType(victim, type))
				return;
			const float distance = player->GetPosition().GetDistance(target->GetPosition());
			const float maxDistance = ranged ? kRangedBloodDistance : kMeleeBloodDistance;
			if (distance >= maxDistance || !Ready(kCoolBlood, 0.12f))
				return;

			RE::NiPoint3 torso = target->GetPosition();
			torso.z += victim->GetHeight() * 0.6f;
			Pending hit;
			hit.splat = true;
			hit.type = type;
			if (!ProjectPoint(torso, hit.u, hit.v))
				return;
			hit.count = ranged ? 1 : (power ? 3 : 1 + (distance < 200.0f ? 1 : 0));
			hit.strength = (ranged ? 0.6f : (power ? 1.2f : 0.9f)) * (1.0f - 0.5f * Util::Smoothstep(150.0f, maxDistance, distance));
			if (power && weapon)
				hit.flash = 0.12f;
			Queue(hit);
			return;
		}

		if (target != player || cause == player)
			return;

		auto* attacker = cause->As<RE::Actor>();
		const bool dragon = attacker && attacker->HasKeywordString("ActorTypeDragon");
		const bool big = dragon || (attacker && attacker->GetHeight() > kBigActorHeight);

		if (options.shake) {
			float strength = spell ? 0.0f : (bash ? 0.3f : (power ? 0.4f : 0.22f));
			if (ranged && !spell)
				strength = 0.12f;
			if (big)
				strength = std::max(strength, 0.55f);
			if (blocked)
				strength *= 0.5f;
			if (strength > 0.0f && Ready(kCoolShake, 0.15f))
				RE::ShakeCamera(std::clamp(strength * options.shakeStrength, 0.0f, kMaxShake), player->GetPosition(), big ? 0.45f : 0.25f);
		}

		Pending hit;
		float attackerU = 0.5f;
		float attackerV = 0.5f;
		const bool attackerVisible = ProjectPoint(cause->GetPosition() + RE::NiPoint3{ 0.0f, 0.0f, attacker ? attacker->GetHeight() * 0.6f : 0.0f }, attackerU, attackerV);
		hit.u = attackerVisible ? attackerU : (Random() < 0.5f ? 0.15f : 0.85f);
		hit.v = attackerVisible ? attackerV : 0.35f + Random() * 0.3f;

		const Element element = ClassifySpell(spell);
		if (options.hitBlur) {
			float blur = spell ? (element != Element::kNone ? 0.3f : 0.0f) : (bash ? 0.6f : (power ? 0.75f : 0.45f));
			if (ranged && !spell)
				blur = 0.3f;
			if (big)
				blur = std::max(blur, spell ? 0.5f : 0.85f);
			if (blocked)
				blur *= 0.4f;
			if (blur > 0.0f && Ready(spell ? kCoolSpellBlur : kCoolBlur, spell ? kSpellBlurInterval : kBlurInterval)) {
				hit.blur = blur;
				hit.blurU = hit.u;
				hit.blurV = hit.v;
			}
		}

		if (element != Element::kNone && options.elemental && Ready(kCoolElement, 0.2f)) {
			switch (element) {
			case Element::kFire:
				hit.splat = true;
				hit.type = SplatType::kFire;
				hit.count = dragon ? 3 : 1;
				hit.strength = dragon ? 1.3f : 0.9f;
				hit.flash = 0.15f;
				hit.flashWarmth = 1.0f;
				hit.fire = dragon ? 1.0f : 0.5f;
				break;
			case Element::kFrost:
				hit.splat = true;
				hit.type = SplatType::kIce;
				hit.count = dragon ? 3 : 1;
				hit.strength = dragon ? 1.2f : 0.9f;
				hit.frost = dragon ? 0.8f : 0.35f;
				break;
			case Element::kShock:
				hit.shock = 1.0f;
				hit.flash = 0.1f;
				break;
			case Element::kPoison:
				hit.splat = true;
				hit.type = SplatType::kVenom;
				hit.strength = 0.9f;
				break;
			default:
				break;
			}
			Queue(hit);
			return;
		}

		if (!weapon || ranged || blocked) {
			if (hit.blur > 0.0f)
				Queue(hit);
			return;
		}

		if (options.slash && Ready(kCoolSlash, 0.25f)) {
			const float side = hit.u < 0.5f ? 1.0f : -1.0f;
			hit.slash = 1.0f;
			hit.slashAngle = side * (0.6f + 0.4f * Random());
		}
		if (options.flash)
			hit.flash = power ? 0.2f : 0.1f;
		hit.flashWarmth = 0.6f;

		if (options.blood && GetHealthFraction(player) < 0.5f && Random() < 0.5f && Ready(kCoolOwnBlood, 1.0f)) {
			hit.splat = true;
			hit.type = SplatType::kBlood;
			hit.u = 0.2f + 0.6f * Random();
			hit.v = 0.75f + 0.15f * Random();
			hit.strength = 0.7f;
		} else if (big && options.elemental && Ready(kCoolGrit, 0.5f)) {
			hit.splat = true;
			hit.type = SplatType::kGrit;
			hit.count = 2;
			hit.strength = 1.0f;
		}
		Queue(hit);
	}

	void CombatLens::Spawn(const Pending& a_pending, const CombatOptions& a_options)
	{
		const bool flashes = a_pending.flash > 0.0f && a_options.flash;
		const bool slashes = a_pending.slash > 0.0f && a_options.slash;
		if (flashes || slashes) {
			state.hitU = a_pending.u;
			state.hitV = a_pending.v;
			state.hitAge = 0.0f;
		}
		if (flashes) {
			float amount = a_pending.flash * a_options.flashStrength;
			if (a_options.photosensitiveSafe) {
				amount = std::min(amount, kSafeFlashCap);
				if (flashCooldown > 0.0f)
					amount *= 0.3f;
			}
			if (amount > state.flash) {
				state.flash = amount;
				state.flashWarmth = a_pending.flashWarmth;
			}
			flashCooldown = kFlashInterval;
		}
		if (slashes) {
			state.slash = a_pending.slash;
			state.slashAngle = a_pending.slashAngle;
		}
		if (a_pending.blur > 0.0f && a_options.hitBlur) {
			const float amount = std::clamp(a_pending.blur * a_options.hitBlurStrength, 0.0f, 1.0f);
			if (amount > state.hitBlur) {
				state.hitBlur = amount;
				state.hitBlurU = a_pending.blurU;
				state.hitBlurV = a_pending.blurV;
			}
		}
		if (a_options.elemental) {
			if (a_pending.shock > 0.0f) {
				state.shock = std::max(state.shock, a_pending.shock);
				state.shockAge = 0.0f;
				state.shockSeed = static_cast<float>(++shockCount % 4096u);
			}
			state.fire = std::max(state.fire, a_pending.fire);
			state.frost = std::max(state.frost, a_pending.frost);
		}

		if (!a_pending.splat || (IsGore(a_pending.type) ? !a_options.blood : !a_options.elemental))
			return;

		auto Random = [this]() { return NextRandom(randomState); };
		const float scale = IsGore(a_pending.type) ? a_options.bloodStrength : 1.0f;
		const float size = GetSize(a_pending.type, a_pending.strength);
		for (uint32_t i = 0; i < a_pending.count; ++i) {
			Splat splat;
			splat.type = a_pending.type;
			splat.radius = size * (0.75f + 0.5f * Random()) * (i == 0 ? 1.0f : 0.7f);
			splat.u = std::clamp(a_pending.u + (Random() - 0.5f) * 0.12f, 0.03f, 0.97f);
			splat.v = std::clamp(a_pending.v + (Random() - 0.5f) * 0.12f, 0.03f, 0.97f);
			float du = splat.u - 0.5f;
			float dv = splat.v - 0.5f;
			const float distance = std::sqrt(du * du + dv * dv);
			const float clearance = kSafeZone + splat.radius * 0.5f;
			if (distance < 1e-3f) {
				const float angle = Random() * 2.0f * RE::NI_PI;
				du = std::cos(angle);
				dv = std::sin(angle);
			} else {
				du /= distance;
				dv /= distance;
			}
			if (distance < clearance) {
				const float pushed = clearance + Random() * 0.1f;
				splat.u = 0.5f + du * pushed;
				splat.v = 0.5f + dv * pushed;
			}
			splat.angle = std::atan2(dv, du) + (Random() - 0.5f) * 0.8f;
			splat.life = GetLife(splat.type) * (0.85f + 0.3f * Random());
			splat.seed = Random() * 100.0f;
			splat.strength = std::clamp(a_pending.strength * scale, 0.0f, 1.0f);
			if (splat.strength <= 0.01f)
				continue;
			if (state.splats.size() >= kMaxSplats)
				state.splats.erase(state.splats.begin());
			state.splats.push_back(splat);
		}
	}

	void CombatLens::ScanActiveEffects()
	{
		persistentFire = persistentFrost = persistentShock = 0.0f;
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* magicTarget = player ? player->AsMagicTarget() : nullptr;
		auto* effects = magicTarget ? magicTarget->GetActiveEffectList() : nullptr;
		if (!effects)
			return;
		for (auto* effect : *effects) {
			if (!effect || effect->flags.any(RE::ActiveEffect::Flag::kInactive, RE::ActiveEffect::Flag::kDispelled))
				continue;
			const auto* base = effect->GetBaseObject();
			if (!base || !base->IsHostile())
				continue;
			const auto caster = effect->caster.get();
			if (caster && caster.get() == player)
				continue;
			switch (base->data.resistVariable) {
			case RE::ActorValue::kResistFire:
				persistentFire = std::min(persistentFire + 0.5f, 1.0f);
				break;
			case RE::ActorValue::kResistFrost:
				persistentFrost = std::min(persistentFrost + 0.4f, 1.0f);
				break;
			case RE::ActorValue::kResistShock:
				persistentShock = std::min(persistentShock + 0.6f, 1.0f);
				break;
			default:
				break;
			}
		}
	}

	void CombatLens::UpdateBreathing(float a_step)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		float exertion = 0.0f;
		float staminaFraction = 1.0f;
		if (player) {
			if (auto* values = player->AsActorValueOwner()) {
				const float maximum = values->GetPermanentActorValue(RE::ActorValue::kStamina);
				const float current = values->GetActorValue(RE::ActorValue::kStamina);
				if (std::isfinite(maximum) && maximum > 1.0f && std::isfinite(current))
					staminaFraction = std::clamp(current / maximum, 0.0f, 1.0f);
			}
			exertion = 1.0f - Util::Smoothstep(0.15f, 0.7f, staminaFraction);
			if (auto* actorState = player->AsActorState(); actorState && actorState->IsSprinting())
				exertion = std::max(exertion, 0.6f);
			if (player->IsInCombat())
				exertion = std::max(exertion, 0.3f);
			if (player->IsPowerAttacking())
				exertion = std::max(exertion, 0.75f);
		}
		state.exertion = exertion;
		state.staminaLow = enabled && options.stamina ? 1.0f - Util::Smoothstep(0.1f, 0.35f, staminaFraction) : 0.0f;

		state.exhaleAge += a_step;
		breathTimer -= a_step;
		if (breathTimer <= 0.0f) {
			const float calm = 60.0f / kCalmBreathsPerMinute;
			++breathCount;
			const float jitter = 0.85f + 0.3f * std::fmod(breathCount * 0.6180339f, 1.0f);
			breathTimer = std::lerp(calm, kWindedBreathPeriod, exertion) * jitter;
			state.exhaleAge = 0.0f;
			state.exhaleStrength = 0.55f + 0.45f * exertion;
			state.exhaleSeed = static_cast<float>(breathCount % 997u);
		}
	}

	void CombatLens::Update(float a_step, float a_delta, bool a_enabled, const CombatOptions& a_options)
	{
		if (enabled && !a_enabled)
			Reset();

		std::deque<Pending> drained;
		{
			const std::scoped_lock guard(pendingLock);
			options = a_options;
			enabled = a_enabled;
			if (enabled) {
				for (auto& cooldown : cooldowns)
					cooldown = std::max(0.0f, cooldown - a_delta);
				drained.swap(pending);
			}
		}

		auto* camera = RE::PlayerCamera::GetSingleton();
		state.firstPerson = camera && camera->IsInFirstPerson();
		UpdateBreathing(a_step);
		if (!enabled)
			return;

		flashCooldown = std::max(0.0f, flashCooldown - a_delta);
		for (const auto& item : drained)
			Spawn(item, a_options);

		for (auto& splat : state.splats)
			splat.age += a_step;
		std::erase_if(state.splats, [](const Splat& a_splat) { return a_splat.age >= a_splat.life; });

		const float flashDecay = a_options.photosensitiveSafe ? 1.5f : 3.0f;
		state.flash = std::max(0.0f, state.flash - a_step * flashDecay);
		state.slash = std::max(0.0f, state.slash - a_step * 2.5f);
		state.hitBlur = a_options.hitBlur ? std::max(0.0f, state.hitBlur - a_step * kHitBlurDecay) : 0.0f;
		state.hitAge = std::min(state.hitAge + a_step, 99.0f);
		state.shockAge = std::min(state.shockAge + a_step, 99.0f);

		scanTimer -= a_delta;
		if (scanTimer <= 0.0f) {
			scanTimer = kScanInterval;
			ScanActiveEffects();
		}
		const bool elemental = a_options.elemental;
		state.shock = state.shockAge < 1.0f ? state.shock : 0.0f;
		state.shockLasting = Util::Approach(state.shockLasting, elemental ? persistentShock : 0.0f, 3.0f, 1.5f, a_step);
		state.fire = std::max(std::max(0.0f, state.fire - a_step * 0.5f), elemental ? persistentFire : 0.0f);
		state.frost = std::max(std::max(0.0f, state.frost - a_step * 0.2f), elemental ? persistentFrost : 0.0f);
	}
}
