#pragma once
#include <random>
#include <array>

// ---------------------------------------------------------------------------
// Block dice face values
// ---------------------------------------------------------------------------
enum class BlockFace : int {
    AttackerDown    = 1,
    BothDown        = 2,
    Push            = 3,  // also face 4
    DefenderStumbles= 5,
    DefenderDown    = 6
};

inline BlockFace toBlockFace(int roll) {
    switch (roll) {
        case 1: return BlockFace::AttackerDown;
        case 2: return BlockFace::BothDown;
        case 3: [[fallthrough]];
        case 4: return BlockFace::Push;
        case 5: return BlockFace::DefenderStumbles;
        default: return BlockFace::DefenderDown;
    }
}

// ---------------------------------------------------------------------------
// Thread-local dice roller
// ---------------------------------------------------------------------------
class Dice {
    std::mt19937_64 rng_;
    std::uniform_int_distribution<int> d6_dist_{1, 6};
    std::uniform_int_distribution<int> d8_dist_{1, 8};

public:
    explicit Dice(uint64_t seed) : rng_(seed) {}

    int d6()    { return d6_dist_(rng_); }
    int d8()    { return d8_dist_(rng_); }
    int d3()    { return (d6() + 1) / 2; }   // 1-2→1, 3-4→2, 5-6→3
    int d6x2()  { return d6() + d6(); }

    // Strategy decision: returns true with the given probability [0,1].
    bool useSkill(float probability) {
        if (probability <= 0.f) return false;
        if (probability >= 1.f) return true;
        return std::uniform_real_distribution<float>{0.f, 1.f}(rng_) < probability;
    }

    // Roll N block dice and return all faces
    std::array<BlockFace, 3> rollBlockDice(int count) {
        std::array<BlockFace, 3> faces{};
        for (int i = 0; i < count && i < 3; ++i)
            faces[i] = toBlockFace(d6());
        return faces;
    }

    // Armor break: 2d6 > av  (strictly greater).
    // Claws: any modified roll of 8+ breaks armour regardless of AV.
    bool armorBreak(int av, int mightyBlowBonus = 0, bool hasClaws = false) {
        int roll = d6x2() + mightyBlowBonus;
        if (hasClaws && roll >= 8) return true;
        return roll > av;
    }

    // Injury roll: 2d6
    // 2-7 = Stunned, 8-9 = KO, 10-12 = Casualty
    enum class Injury { Stunned, KO, Casualty };
    Injury injuryRoll(int mightyBlowBonus = 0) {
        int roll = d6x2() + mightyBlowBonus;
        if (roll >= 10) return Injury::Casualty;
        if (roll >= 8)  return Injury::KO;
        return Injury::Stunned;
    }

    // Generic success roll: roll >= target  (1-6)
    bool successRoll(int target) { return d6() >= target; }

    // Dodge success: BB2020 notation — ag IS the target number (lower = better).
    // Each tackle zone adds 1 to the target (harder to dodge).
    // rerollAvailable is cleared to false after use — enforces once-per-activation.
    bool dodgeRoll(int ag, int tacklezones, bool& rerollAvailable) {
        int target = std::clamp(ag + tacklezones, 2, 6);
        bool success = d6() >= target;
        if (!success && rerollAvailable) {
            success = d6() >= target;
            rerollAvailable = false;
        }
        return success;
    }

    // Pass roll: target = clamp(PA + range_mod, 2, 6)  (lower PA = better).
    // Accurate skill (-1 already applied to pa before calling).
    // Pass skill gives one re-roll on failure; rerollAvailable cleared after use.
    bool passRoll(int pa, int rangeMod, bool& rerollAvailable) {
        int target = std::clamp(pa + rangeMod, 2, 6);
        bool success = d6() >= target;
        if (!success && rerollAvailable) {
            success = d6() >= target;
            rerollAvailable = false;
        }
        return success;
    }

    // Catch roll: BB2020 notation — ag IS the target number.
    // Catch skill gives one re-roll; rerollAvailable cleared after use.
    bool catchRoll(int ag, bool& rerollAvailable) {
        int target = std::clamp(ag, 2, 6);
        bool success = d6() >= target;
        if (!success && rerollAvailable) {
            success = d6() >= target;
            rerollAvailable = false;
        }
        return success;
    }

    // Pick-up roll: BB2020 notation — ag IS the target, +1 per tackle zone.
    // Sure Hands gives one re-roll; rerollAvailable cleared after use.
    bool pickupRoll(int ag, int tacklezones, bool& rerollAvailable) {
        int target = std::clamp(ag + tacklezones, 2, 6);
        bool success = d6() >= target;
        if (!success && rerollAvailable) {
            success = d6() >= target;
            rerollAvailable = false;
        }
        return success;
    }
};
