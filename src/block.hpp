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
    DefenderPushed,   // Defender pushed back one zone
    DefenderStumbles, // Defender falls unless Dodge saves them
    DefenderDown      // Defender knocked down → armour roll
};

struct BlockResult {
    BlockOutcome outcome{};
    bool attackerInjured{false};
    bool defenderInjured{false};
    bool turnover{false};
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
// Strategy fields in attacker.strategy and defender.strategy are consulted
// for Wrestle (who wants Both Down) and Stand Firm (resist the push).
// ---------------------------------------------------------------------------
inline BlockResult resolveBlock(
    PlayerState& attacker, PlayerState& defender,
    int attackerAssists, int defenderAssists,
    Dice& dice, bool attackerHasBall,
    TeamState& attackerTeam, TeamState& defenderTeam)
{
    BlockResult result{};
    const auto& aStats = attacker.stats;
    const auto& dStats = defender.stats;

    int diceCount = blockDiceCount(aStats, dStats, attackerAssists, defenderAssists);
    int numDice   = std::abs(diceCount);

    auto rolled = dice.rollBlockDice(numDice);
    std::span<const BlockFace> faces(rolled.data(), static_cast<size_t>(numDice));

    BlockFace chosen = (diceCount >= 0)
        ? pickBestFaceForAttacker(faces)
        : pickBestFaceForDefender(faces);

    // ── Block skill: attacker may re-roll Both Down ──────────────────────────
    // Wrestle can intercept here: if the DEFENDER has Wrestle and decides to
    // use it, they lock in the Both Down before the attacker's Block re-roll.
    if (chosen == BlockFace::BothDown) {
        bool defenderWrestles = dStats.has(SK::Wrestle)
                             && dice.useSkill(defender.strategy.wrestle);
        bool attackerWrestles = aStats.has(SK::Wrestle)
                             && dice.useSkill(attacker.strategy.wrestle);

        if (!defenderWrestles) {
            // Defender did not lock the Both Down → attacker may use Block to re-roll
            if (aStats.has(SK::Block) && diceCount >= 0) {
                rolled = dice.rollBlockDice(numDice);
                faces  = std::span<const BlockFace>(rolled.data(),
                                                     static_cast<size_t>(numDice));
                chosen = pickBestFaceForAttacker(faces);
                // If still Both Down after re-roll, attacker may still Wrestle
                if (chosen == BlockFace::BothDown && attackerWrestles) {
                    // Attacker accepts both falling (e.g. doesn't have ball)
                }
            } else if (attackerWrestles) {
                // No Block but attacker has Wrestle – accepts both falling
            }
        }
        // else: defender locked Both Down with Wrestle; Block re-roll skipped
    }

    // ── Injury helper ────────────────────────────────────────────────────────
    int mightyBlowBonus = aStats.has(SK::MightyBlow) ? 1 : 0;
    bool hasClaws       = aStats.has(SK::Claws);
    // Mighty Blow: +1 applied to ONE roll only — armour OR injury, never both.
    //   Optimal play: if armour breaks without the bonus, apply it to injury.
    //                 if the bonus was needed to break armour, it is spent there.
    // Claws: any modified armour roll of 8+ breaks regardless of AV (injury unaffected).

    auto applyKnockdown = [&](PlayerState& player, int mbBonus, bool claws,
                               TeamState& team) {
        int raw = dice.d6x2();

        // Does armour break without Mighty Blow?
        bool brokeWithout = (raw > player.stats.av) || (claws && raw >= 8);
        // Does armour break with Mighty Blow applied to the armour roll?
        bool brokeWith    = (raw + mbBonus > player.stats.av)
                         || (claws && (raw + mbBonus) >= 8);

        if (!brokeWith) return false;  // armour held

        // MB goes to injury only if armour broke without it (bonus was not spent on armour).
        int injBonus = brokeWithout ? mbBonus : 0;

        auto inj = dice.injuryRoll(injBonus);

        // Apothecary: re-roll one casualty result per game, keep the better outcome.
        if (inj == Dice::Injury::Casualty && team.hasApothecary && !team.apothecaryUsed) {
            team.apothecaryUsed = true;
            auto reroll = dice.injuryRoll(0);
            if (reroll < inj) inj = reroll;
        }

        // Regeneration: after any apothecary use, a 4+ converts Casualty to Reserves (BB2020 §7).
        if (inj == Dice::Injury::Casualty && player.stats.has(SK::Regeneration) && dice.d6() >= 4)
            inj = Dice::Injury::KO;

        if      (inj == Dice::Injury::KO)       player.ko       = true;
        else if (inj == Dice::Injury::Casualty) player.casualty = true;
        else                                     player.stunned  = true;
        return true;
    };

    // ── Resolve the chosen face ──────────────────────────────────────────────
    switch (chosen) {
        case BlockFace::AttackerDown:
            attacker.prone  = true;
            result.outcome  = BlockOutcome::AttackerDown;
            result.turnover = true;
            result.attackerInjured = applyKnockdown(attacker, 0, false, attackerTeam);
            if (result.attackerInjured) result.attackerInjury =
                attacker.ko ? Dice::Injury::KO :
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
                    attacker.ko ? Dice::Injury::KO :
                    attacker.casualty ? Dice::Injury::Casualty : Dice::Injury::Stunned;
            if (result.defenderInjured)
                result.defenderInjury =
                    defender.ko ? Dice::Injury::KO :
                    defender.casualty ? Dice::Injury::Casualty : Dice::Injury::Stunned;
            break;

        case BlockFace::Push: {
            // ── Stand Firm: defender may choose to stay in current zone ────
            bool defenderStandsFirm = dStats.has(SK::StandFirm)
                                   && dice.useSkill(defender.strategy.standFirm);
            if (!defenderStandsFirm && defender.zone != Zone::OwnEndZone)
                defender.zone = defender.zone - 1;
            result.outcome = BlockOutcome::DefenderPushed;
            break;
        }

        case BlockFace::DefenderStumbles:
            // Dodge skill (and no Tackle on attacker) keeps the defender upright
            if (dStats.has(SK::Dodge) && !aStats.has(SK::Tackle)) {
                // ── Stand Firm on Stumbles: still pushed but stays up ───────
                bool standsFirm = dStats.has(SK::StandFirm)
                               && dice.useSkill(defender.strategy.standFirm);
                if (!standsFirm && defender.zone != Zone::OwnEndZone)
                    defender.zone = defender.zone - 1;
                result.outcome = BlockOutcome::DefenderPushed;
            } else {
                defender.prone = true;
                result.outcome = BlockOutcome::DefenderStumbles;
                result.defenderInjured = applyKnockdown(defender, mightyBlowBonus, hasClaws, defenderTeam);
                if (result.defenderInjured)
                    result.defenderInjury =
                        defender.ko ? Dice::Injury::KO :
                        defender.casualty ? Dice::Injury::Casualty : Dice::Injury::Stunned;
            }
            break;

        case BlockFace::DefenderDown:
            defender.prone = true;
            result.outcome = BlockOutcome::DefenderDown;
            result.defenderInjured = applyKnockdown(defender, mightyBlowBonus, hasClaws, defenderTeam);
            if (result.defenderInjured)
                result.defenderInjury =
                    defender.ko ? Dice::Injury::KO :
                    defender.casualty ? Dice::Injury::Casualty : Dice::Injury::Stunned;
            break;

    }

    return result;
}
