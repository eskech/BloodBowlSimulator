#pragma once
#include "models.hpp"
#include "dice.hpp"
#include <algorithm>
#include <span>

// ---------------------------------------------------------------------------
// Block resolution following Blood Bowl 2020/2025 rules
// ---------------------------------------------------------------------------

enum class BlockOutcome {
    NoEffect,
    AttackerDown,     // Attacker falls (turnover!)
    BothDown,         // Both fall (turnover if attacker has ball)
    DefenderPushed,   // Defender pushed back one zone (or surfed, or side-stepped)
    DefenderStumbles, // Defender falls unless Dodge saves them
    DefenderDown      // Defender knocked down → armour roll
};

struct BlockResult {
    BlockOutcome outcome{};
    bool attackerInjured{false};
    bool defenderInjured{false};
    bool turnover{false};
    bool followUpBlocked{false};  // Fend: attacker cannot follow up
    bool ballDropped{false};       // Strip Ball on Push — ball loose without knockdown
    bool crowdSurfed{false};       // Defender pushed off the pitch into crowd
    Dice::Injury attackerInjury{Dice::Injury::Stunned};
    Dice::Injury defenderInjury{Dice::Injury::Stunned};
};

// ---------------------------------------------------------------------------
// Dice count: positive = attacker picks best, negative = defender picks best
// ---------------------------------------------------------------------------
inline int blockDiceCount(const PlayerStats& attacker, const PlayerStats& defender,
                           int attackerAssists, int defenderAssists)
{
    int aST = attacker.st + attackerAssists;
    int dST = defender.st + defenderAssists;
    if (aST >= dST * 2) return  2;
    if (dST >= aST * 2) return -2;
    return 1;
}

inline BlockFace pickBestFaceForAttacker(std::span<const BlockFace> faces) {
    static constexpr auto rank = [](BlockFace f) -> int {
        switch (f) {
            case BlockFace::DefenderDown:     return 6;
            case BlockFace::DefenderStumbles: return 5;
            case BlockFace::Push:             return 4;
            case BlockFace::BothDown:         return 3;
            case BlockFace::AttackerDown:     return 0;
        }
        return 0;
    };
    return *std::ranges::max_element(faces, {}, rank);
}

inline BlockFace pickBestFaceForDefender(std::span<const BlockFace> faces) {
    static constexpr auto rank = [](BlockFace f) -> int {
        switch (f) {
            case BlockFace::AttackerDown:     return 6;
            case BlockFace::BothDown:         return 5;
            case BlockFace::Push:             return 4;
            case BlockFace::DefenderStumbles: return 3;
            case BlockFace::DefenderDown:     return 0;
        }
        return 0;
    };
    return *std::ranges::max_element(faces, {}, rank);
}

// ---------------------------------------------------------------------------
// Main block resolver.
// ---------------------------------------------------------------------------
inline BlockResult resolveBlock(
    PlayerState& attacker, PlayerState& defender,
    int attackerAssists, int defenderAssists,
    Dice& dice, bool attackerHasBall,
    TeamState& attackerTeam, TeamState& defenderTeam,
    bool isBlitz = false)
{
    BlockResult result{};
    const auto& aStats = attacker.stats;
    const auto& dStats = defender.stats;

    // Juggernaut: on a Blitz, Both Down → Push; cancels defender Wrestle,
    // Stand Firm, and Fend.
    const bool juggernauting = isBlitz && aStats.has(SK::Juggernaut);

    int diceCount = blockDiceCount(aStats, dStats, attackerAssists, defenderAssists);
    int numDice   = std::abs(diceCount);

    auto rolled = dice.rollBlockDice(numDice);
    std::span<const BlockFace> faces(rolled.data(), static_cast<size_t>(numDice));

    BlockFace chosen = (diceCount >= 0)
        ? pickBestFaceForAttacker(faces)
        : pickBestFaceForDefender(faces);

    // ── Block skill: attacker may re-roll Both Down ──────────────────────────
    if (chosen == BlockFace::BothDown) {
        // Juggernaut cancels defender Wrestle before the Block re-roll check.
        bool defenderWrestles = !juggernauting
                             && dStats.has(SK::Wrestle)
                             && dice.useSkill(defender.strategy.wrestle);
        bool attackerWrestles = aStats.has(SK::Wrestle)
                             && dice.useSkill(attacker.strategy.wrestle);

        if (!defenderWrestles) {
            if (aStats.has(SK::Block) && diceCount >= 0) {
                rolled = dice.rollBlockDice(numDice);
                faces  = std::span<const BlockFace>(rolled.data(),
                                                     static_cast<size_t>(numDice));
                chosen = pickBestFaceForAttacker(faces);
                (void)attackerWrestles;
            }
        }
        // Juggernaut: after all re-rolls, convert remaining Both Down to Push.
        if (juggernauting && chosen == BlockFace::BothDown)
            chosen = BlockFace::Push;
    }

    // ── Injury helper ────────────────────────────────────────────────────────
    int mightyBlowBonus = aStats.has(SK::MightyBlow) ? 1 : 0;
    bool hasClaws       = aStats.has(SK::Claws);

    auto applyKnockdown = [&](PlayerState& player, int mbBonus, bool claws,
                               TeamState& team) -> bool {
        int raw = dice.d6x2();

        bool brokeWithout = (raw > player.stats.av) || (claws && raw >= 8);
        bool brokeWith    = (raw + mbBonus > player.stats.av)
                         || (claws && (raw + mbBonus) >= 8);

        if (!brokeWith) return false;

        int injBonus = brokeWithout ? mbBonus : 0;
        auto inj = dice.injuryRoll(injBonus);

        // Decay: apothecary cannot be used for this player's Casualty results.
        if (inj == Dice::Injury::Casualty && team.hasApothecary && !team.apothecaryUsed
                                          && !player.stats.has(SK::Decay)) {
            team.apothecaryUsed = true;
            auto reroll = dice.injuryRoll(0);
            if (reroll < inj) inj = reroll;
        }

        if (inj == Dice::Injury::Casualty && player.stats.has(SK::Regeneration) && dice.d6() >= 4)
            inj = Dice::Injury::KO;

        if      (inj == Dice::Injury::KO)       player.ko       = true;
        else if (inj == Dice::Injury::Casualty) player.casualty = true;
        else                                     player.stunned  = true;
        return true;
    };

    // ── Crowd-surf helper ────────────────────────────────────────────────────
    // Called when a defender is pushed off the pitch at Zone::OwnEndZone.
    // Armor roll vs player's own AV; if held → KO (off pitch this drive).
    auto applyCrowdSurf = [&]() {
        result.crowdSurfed = true;
        bool broke = applyKnockdown(defender, 0, false, defenderTeam);
        if (!broke) defender.ko = true;  // armor held but player is still removed from pitch
        result.defenderInjured = true;
        result.defenderInjury  =
            defender.casualty ? Dice::Injury::Casualty :
            defender.ko       ? Dice::Injury::KO       : Dice::Injury::Stunned;
    };

    // ── Push helper: resolve where defender ends up after a push ────────────
    // Returns true if the push resulted in a crowd surf.
    auto resolvePush = [&]() -> bool {
        // Take Root: rooted players cannot be pushed (acts as automatic Stand Firm).
        if (defender.rootedThisTurn) return false;

        // Juggernaut cancels Stand Firm on a Blitz.
        bool defStandsFirm  = !juggernauting
                           && dStats.has(SK::StandFirm) && dice.useSkill(defender.strategy.standFirm);
        bool attackerGrabs  = aStats.has(SK::Grab);
        // Sidestep lets defender move forward (away from their own endzone), negated by Grab
        bool defSidesteps   = dStats.has(SK::Sidestep) && !attackerGrabs;

        if (defStandsFirm) return false;

        if (defSidesteps) {
            // Defender chooses any adjacent square — in the zone model we model
            // this as staying put (sideways / into a safer pocket), NOT as a free
            // advance.  True zone advancement only happens in the carrier's own
            // movement action.
            return false;
        }

        // Normal push: defender moves toward own endzone
        if (defender.zone == Zone::OwnEndZone) {
            // Pushed off the pitch into the crowd
            applyCrowdSurf();
            return true;
        }
        defender.zone = defender.zone - 1;
        return false;
    };

    // ── Resolve the chosen face ──────────────────────────────────────────────
    switch (chosen) {
        case BlockFace::AttackerDown:
            attacker.prone  = true;
            result.outcome  = BlockOutcome::AttackerDown;
            result.turnover = true;
            result.attackerInjured = applyKnockdown(attacker, 0, false, attackerTeam);
            if (result.attackerInjured)
                result.attackerInjury =
                    attacker.ko       ? Dice::Injury::KO :
                    attacker.casualty ? Dice::Injury::Casualty : Dice::Injury::Stunned;
            break;

        case BlockFace::BothDown:
            attacker.prone  = true;
            defender.prone  = true;
            result.outcome  = BlockOutcome::BothDown;
            result.turnover = attackerHasBall;
            result.attackerInjured = applyKnockdown(attacker, 0, false, attackerTeam);
            result.defenderInjured = applyKnockdown(defender, mightyBlowBonus, hasClaws, defenderTeam);
            if (result.attackerInjured)
                result.attackerInjury =
                    attacker.ko       ? Dice::Injury::KO :
                    attacker.casualty ? Dice::Injury::Casualty : Dice::Injury::Stunned;
            if (result.defenderInjured)
                result.defenderInjury =
                    defender.ko       ? Dice::Injury::KO :
                    defender.casualty ? Dice::Injury::Casualty : Dice::Injury::Stunned;
            break;

        case BlockFace::Push: {
            resolvePush();
            if (aStats.has(SK::StripBall) && defender.hasBall) {
                defender.hasBall  = false;
                result.ballDropped = true;
            }
            // Juggernaut cancels Fend on a Blitz.
            result.followUpBlocked = dStats.has(SK::Fend) && !juggernauting;
            result.outcome = BlockOutcome::DefenderPushed;
            break;
        }

        case BlockFace::DefenderStumbles:
            if (dStats.has(SK::Dodge) && !aStats.has(SK::Tackle)) {
                resolvePush();
                if (aStats.has(SK::StripBall) && defender.hasBall) {
                    defender.hasBall   = false;
                    result.ballDropped = true;
                }
                result.followUpBlocked = dStats.has(SK::Fend) && !juggernauting;
                result.outcome = BlockOutcome::DefenderPushed;
            } else {
                defender.prone = true;
                result.outcome = BlockOutcome::DefenderStumbles;
                result.defenderInjured = applyKnockdown(defender, mightyBlowBonus, hasClaws, defenderTeam);
                // Piling On: if armour held, attacker may go prone to re-roll.
                if (!result.defenderInjured && aStats.has(SK::PilingOn) && !attackerHasBall) {
                    attacker.prone = true;
                    result.defenderInjured = applyKnockdown(defender, mightyBlowBonus, hasClaws, defenderTeam);
                }
                if (result.defenderInjured)
                    result.defenderInjury =
                        defender.ko       ? Dice::Injury::KO :
                        defender.casualty ? Dice::Injury::Casualty : Dice::Injury::Stunned;
            }
            break;

        case BlockFace::DefenderDown:
            defender.prone = true;
            result.outcome = BlockOutcome::DefenderDown;
            result.defenderInjured = applyKnockdown(defender, mightyBlowBonus, hasClaws, defenderTeam);
            // Piling On: if armour held, attacker may go prone to re-roll.
            if (!result.defenderInjured && aStats.has(SK::PilingOn) && !attackerHasBall) {
                attacker.prone = true;
                result.defenderInjured = applyKnockdown(defender, mightyBlowBonus, hasClaws, defenderTeam);
            }
            if (result.defenderInjured)
                result.defenderInjury =
                    defender.ko       ? Dice::Injury::KO :
                    defender.casualty ? Dice::Injury::Casualty : Dice::Injury::Stunned;
            break;
    }

    return result;
}
