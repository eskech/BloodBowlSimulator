#include "simulator.hpp"
#include "block.hpp"
#include "dice.hpp"
#include "models.hpp"
#include <algorithm>
#include <cassert>
#include <omp.h>
#include <ranges>
#include <vector>

// ============================================================
// Build a TeamState from configuration
// ============================================================
TeamState buildTeamState(const TeamConfig& cfg, const SeedData& seed) {
    TeamState ts;
    ts.name             = cfg.name;
    ts.race             = cfg.race;
    ts.hasApothecary    = cfg.hasApothecary;
    ts.rerollsRemaining = cfg.rerolls;

    const Race* race = seed.findRace(cfg.race);

    for (const auto& pc : cfg.players) {
        PlayerStats stats;
        stats.name     = pc.name;
        stats.position = pc.position;

        auto setBit = [&](const std::string& sk) {
            int idx = skillNameToIndex(sk);
            if (idx >= 0)
                stats.skillFlags[static_cast<unsigned>(idx) >> 6]
                    |= (1ULL << (idx & 63));
        };

        if (race) {
            const RosterPosition* pos = race->findPosition(pc.position);
            if (pos) {
                stats.ma = pos->ma;
                stats.st = pos->st;
                stats.ag = pos->ag;
                stats.pa = pos->pa;
                stats.av = pos->av;
                for (const auto& sk : pos->startingSkills) setBit(sk);
            }
        }
        for (const auto& sk : pc.extraSkills) setBit(sk);
        if (pc.isTeamCaptain) setBit("Pro");
        // Loner (3+) implies Loner too, so both bits are set.
        if (stats.has(SK::LonerThreePlus)) setBit("Loner");

        PlayerStrategy strategy = pc.strategy.mergedWith(cfg.defaultStrategy);

        PlayerState ps;
        ps.stats         = std::move(stats);
        ps.strategy      = strategy;
        ps.zone          = Zone::OwnHalf;
        ps.isTeamCaptain = pc.isTeamCaptain;
        ts.players[ts.playerCount++] = std::move(ps);
    }

    // Star players: use their own stats from the seed, not a roster position.
    for (const auto& spName : cfg.starPlayers) {
        const StarPlayer* sp = seed.findStarPlayer(spName);
        if (!sp || ts.playerCount >= static_cast<int>(ts.players.size())) continue;

        PlayerStats stats;
        stats.name     = sp->name;
        stats.position = "Star Player";
        stats.ma = sp->ma;  stats.st = sp->st;
        stats.ag = sp->ag;  stats.pa = sp->pa;
        stats.av = sp->av;

        auto setBitSP = [&](const std::string& sk) {
            int idx = skillNameToIndex(sk);
            if (idx >= 0)
                stats.skillFlags[static_cast<unsigned>(idx) >> 6]
                    |= (1ULL << (idx & 63));
        };
        for (const auto& sk : sp->skills) setBitSP(sk);
        if (stats.has(SK::LonerThreePlus)) setBitSP("Loner");

        PlayerState ps;
        ps.stats     = std::move(stats);
        ps.strategy  = cfg.defaultStrategy;
        ps.zone      = Zone::OwnHalf;
        ts.players[ts.playerCount++] = std::move(ps);
    }

    if (cfg.riotousRookies && race) {
        ts.riotousRookies = true;
        const RosterPosition* pos = race->findPosition("Snotling Lineman");
        if (pos) {
            PlayerStats tmplStats;
            tmplStats.name     = "Riotous Rookie";
            tmplStats.position = pos->name;
            tmplStats.ma = pos->ma;  tmplStats.st = pos->st;
            tmplStats.ag = pos->ag;  tmplStats.pa = pos->pa;
            tmplStats.av = pos->av;
            auto setTmplBit = [&](const std::string& sk) {
                int idx = skillNameToIndex(sk);
                if (idx >= 0)
                    tmplStats.skillFlags[static_cast<unsigned>(idx) >> 6]
                        |= (1ULL << (idx & 63));
            };
            for (const auto& sk : pos->startingSkills) setTmplBit(sk);
            ts.riotousRookieTemplate.stats    = std::move(tmplStats);
            ts.riotousRookieTemplate.strategy = cfg.defaultStrategy;
            ts.riotousRookieTemplate.inReserves = true;
        }
    }

    return ts;
}

// ============================================================
// Internal helpers
// ============================================================
namespace {

// Consume one team re-roll.  Returns true if a re-roll was granted.
//   • If actor has Loner, they must first roll 4+ on a D6 to use the re-roll
//     (the re-roll is not consumed on failure).
//   • If the team captain is active and the D6 rolls a natural 6, the
//     re-roll is free (count not decremented).
static bool useTeamReroll(TeamState& team, Dice& dice,
                           const PlayerState* actor = nullptr) {
    // Loner: star players and certain big guys must roll to use a team re-roll.
    // Loner (3+): 3+ to use (67% success).  Loner (4+) or plain Loner: 4+ (50%).
    if (actor && actor->stats.has(SK::LonerThreePlus) && dice.d6() < 3) return false;
    else if (actor && actor->stats.has(SK::Loner) && !actor->stats.has(SK::LonerThreePlus)
             && dice.d6() < 4) return false;

    // Leader: once per half, a Leader player on the pitch provides a free re-roll.
    if (!team.leaderRerollUsedThisHalf) {
        bool leaderOnPitch = false;
        for (const auto& p : team.allPlayers())
            if (p.isActive() && !p.stunned && p.stats.has(SK::Leader))
                { leaderOnPitch = true; break; }
        if (leaderOnPitch) {
            team.leaderRerollUsedThisHalf = true;
            return true;   // free — does not decrement rerollsRemaining
        }
    }

    if (team.rerollsRemaining <= 0) return false;
    if (!team.captainActive() || dice.d6() != 6) --team.rerollsRemaining;
    return true;
}

// Forward declarations
bool simulateTurn(TeamState& offense, TeamState& defense, Dice& dice,
                  int turnsLeft, const GameContext& ctx);

// ── 9. Weather ────────────────────────────────────────────────────────────────
// Roll 2D6 at the start of each game.  5-10 = Nice (no effect).
GameContext rollWeather(Dice& dice) {
    GameContext ctx;
    int roll = dice.d6() + dice.d6();
    if      (roll == 2)  ctx.swelteringHeat = true;        // Sweltering Heat
    else if (roll <= 4)  ctx.paModifier = 1;               // Very Sunny
    else if (roll == 11) { ctx.paModifier = 1; ctx.catchModifier = 1; } // Pouring Rain
    else if (roll == 12) ctx.blizzard = true;              // Blizzard
    // 5-10: Nice — no modifiers
    return ctx;
}

// ── 1. Kickoff events ────────────────────────────────────────────────────────
// Roll D8 after each kickoff setup.
void kickoffEvent(TeamState& offense, TeamState& defense, Dice& dice,
                  const GameContext& ctx)
{
    // Sweltering Heat: each player on the pitch rolls 6+ or is KO'd before the drive.
    if (ctx.swelteringHeat) {
        for (auto& p : offense.allPlayers())
            if (p.isOnPitch() && !p.ko && dice.d6() >= 6) p.ko = true;
        for (auto& p : defense.allPlayers())
            if (p.isOnPitch() && !p.ko && dice.d6() >= 6) p.ko = true;
    }

    int roll = dice.d8();
    switch (roll) {
        case 1: // Riot — random team loses/gains a turn (model: reroll swap)
            if (dice.d6() >= 4) {
                if (offense.rerollsRemaining > 0) { --offense.rerollsRemaining; ++defense.rerollsRemaining; }
            }
            break;

        case 2: {// Blitz — defense gets a free block on the ball carrier before offense moves
            PlayerState* carrier = offense.ballCarrier();
            if (carrier) {
                for (auto& d : defense.allPlayers()) {
                    if (!d.canAct()) continue;
                    if (std::abs(d.zone - carrier->zone) <= 1) {
                        d.zone = carrier->zone;
                        // Simple free blitz — don't count stats, don't cause game turnover
                        resolveBlock(d, *carrier, 0, 0, dice, false, defense, offense, true);
                        break;
                    }
                }
            }
            break;
        }

        case 3: // High Kick — receiver repositions; advance ball carrier one zone for free
            if (PlayerState* c = offense.ballCarrier())
                if (c->zone < Zone::OppEndZone) c->zone = c->zone + 1;
            break;

        case 4: // Cheering Fans — coin flip for 1 extra re-roll
        case 5: // Brilliant Coaching — same effect
            if (dice.d6() >= 4) ++offense.rerollsRemaining;
            else                ++defense.rerollsRemaining;
            break;

        case 6: // Quick Snap — offense advances all players one zone before defense reacts
            for (auto& p : offense.allPlayers())
                if (p.isActive() && p.zone < Zone::OppEndZone) p.zone = p.zone + 1;
            break;

        case 7: // Perfect Defense — no practical effect in zone model
            break;

        case 8: // Pitch Invasion — random players on both sides go prone (5-6 on D6)
            for (auto& p : offense.allPlayers())
                if (p.isOnPitch() && dice.d6() >= 5) p.prone = true;
            for (auto& p : defense.allPlayers())
                if (p.isOnPitch() && dice.d6() >= 5) p.prone = true;
            break;
    }
}

// ── Kickoff setup ────────────────────────────────────────────────────────────
// isFirstKickoff = true only for the very first kickoff of the game; Secret
// Weapon ejection is skipped then (no previous drive has occurred).
void setupKickoff(TeamState& offense, TeamState& defense, Dice& dice,
                  const GameContext& ctx, bool isFirstKickoff = false)
{
    // Secret Weapon players are ejected after every drive (not before the first).
    if (!isFirstKickoff) {
        for (TeamState* team : {&offense, &defense})
            for (auto& p : team->allPlayers())
                if (p.stats.has(SK::SecretWeapon) && p.isOnPitch())
                    p.casualty = true;  // ejected — out for the rest of the game
    }

    // Return benched (non-Swarming) players to the pitch.
    for (TeamState* team : {&offense, &defense})
        for (auto& p : team->allPlayers())
            if (p.inReserves && !p.stats.has(SK::Swarming)) p.inReserves = false;

    // KO recovery (4+ per KO'd player).
    for (auto& p : offense.allPlayers())
        if (p.ko && !p.casualty && dice.d6() >= 4) p.ko = p.prone = p.stunned = false;
    for (auto& p : defense.allPlayers())
        if (p.ko && !p.casualty && dice.d6() >= 4) p.ko = p.prone = p.stunned = false;

    // Swarming: D3 Swarming players from reserves enter (capped by on-pitch count).
    for (TeamState* team : {&offense, &defense}) {
        int onPitch = 0;
        for (const auto& p : team->allPlayers())
            if (!p.ko && !p.casualty && !p.inReserves && p.stats.has(SK::Swarming)) ++onPitch;
        if (!onPitch) continue;
        int canEnter = std::min(dice.d3(), onPitch);
        for (auto& p : team->allPlayers()) {
            if (canEnter <= 0) break;
            if (p.inReserves && !p.casualty && p.stats.has(SK::Swarming)) { p.inReserves = false; --canEnter; }
        }
    }

    // 11-player cap.
    for (TeamState* team : {&offense, &defense}) {
        int active = 0;
        for (const auto& p : team->allPlayers()) if (!p.ko && !p.casualty && !p.inReserves) ++active;
        int toBench = active - 11;
        for (int i = team->playerCount - 1; i >= 0 && toBench > 0; --i) {
            auto& p = team->players[static_cast<size_t>(i)];
            if (p.ko || p.casualty || p.inReserves || p.isTeamCaptain) continue;
            p.inReserves = true; --toBench;
        }
    }

    // Reset per-drive states.
    for (auto& p : offense.allPlayers()) { if (!p.isOnPitch()) continue; p.prone = p.hasBall = false; }
    for (auto& p : defense.allPlayers()) { if (!p.isOnPitch()) continue; p.prone = p.hasBall = false; }

    // Zone placement.
    int offLiners = 0, defLiners = 0;
    for (auto& p : offense.allPlayers()) {
        if (!p.isOnPitch()) continue;
        p.zone = (offLiners++ < 3) ? Zone::Midfield : Zone::OwnHalf;
    }
    for (auto& p : defense.allPlayers()) {
        if (!p.isOnPitch()) continue;
        p.zone = (defLiners++ < 3) ? Zone::Midfield : Zone::OppHalf;
    }

    // Ball to first usable player in OwnHalf.
    for (auto& p : offense.allPlayers()) {
        if (p.isActive() && p.zone == Zone::OwnHalf) { p.hasBall = true; break; }
    }
    if (!offense.ballCarrier()) {
        for (auto& p : offense.allPlayers()) { if (p.isActive()) { p.hasBall = true; break; } }
    }

    // 1. Kickoff event roll.
    kickoffEvent(offense, defense, dice, ctx);
}

// ── Activation rolls: Bone Head and Really Stupid (BB2025) ───────────────────
// Called after a player declares an action.  Returns true if the player becomes
// Distracted and loses their action and tackle zone for this turn.
//
// Bone Head:      roll D6 — 2+ acts normally, 1 = Distracted.
//                 Team re-rolls and Loner are honoured.
//
// Really Stupid:  roll D6 (+2 modifier if any adjacent standing team-mate who
//                 is not Distracted and does not have Really Stupid is present).
//                 4+ acts normally, <4 = Distracted.
//                 No team re-rolls (the rule text does not allow them).
bool checkActivationRoll(PlayerState& p, const TeamState& team, Dice& dice) {
    if (p.stats.has(SK::BoneHead)) {
        bool distracted = (dice.d6() == 1);
        if (distracted && useTeamReroll(const_cast<TeamState&>(team), dice, &p))
            distracted = (dice.d6() == 1);
        if (distracted) { p.activated = p.distractedThisTurn = true; return true; }
        return false;
    }
    if (p.stats.has(SK::ReallyStupid)) {
        // Count adjacent standing team-mates without Really Stupid that haven't
        // themselves become Distracted this turn.
        int supporters = 0;
        for (const auto& tm : team.allPlayers()) {
            if (&tm == &p) continue;
            if (!tm.isActive() || tm.prone || tm.distractedThisTurn) continue;
            if (tm.stats.has(SK::ReallyStupid)) continue;
            if (std::abs(tm.zone - p.zone) <= 1) ++supporters;
        }
        bool distracted = (dice.d6() + (supporters > 0 ? 2 : 0)) < 4;
        if (distracted) { p.activated = p.distractedThisTurn = true; return true; }
        return false;
    }
    // Animal Savagery: on a 1/6 the player goes Wild.  Simplified: Distracted
    // (the zone model cannot represent a forced team-mate block).
    if (p.stats.has(SK::AnimalSavagery)) {
        if (dice.d6() == 1) { p.activated = p.distractedThisTurn = true; return true; }
        return false;
    }
    return false;
}

// ── Unchannelled Fury ─────────────────────────────────────────────────────────
// For non-Block/Blitz actions (running, passing): must pass an Agility test
// (D6 >= AG) or become Distracted.
static bool checkUnchannelledFury(PlayerState& p, Dice& dice) {
    if (!p.stats.has(SK::UnchannelledFury)) return false;
    if (dice.d6() >= p.stats.ag) return false;
    p.activated = p.distractedThisTurn = true;
    return true;
}

// ── Take Root ────────────────────────────────────────────────────────────────
// Roll 2+ at activation; on a 1 the player is Rooted for the turn.
// Rooted players may still make a Block action but cannot move, follow up,
// or be pushed.  Unlike Bone Head, this does NOT prevent the action entirely.
// Returns true if the player is now rooted.
static bool checkTakeRoot(PlayerState& p, Dice& dice) {
    if (!p.stats.has(SK::TakeRoot)) return false;
    if (dice.d6() >= 2) return false;
    p.rootedThisTurn = true;
    return true;
}

// ── Zone helpers ─────────────────────────────────────────────────────────────
// Distracted players (Bone Head / Really Stupid) and Titchy players do not
// apply a tackle zone penalty to dodge rolls.
int defendersInZone(const TeamState& defense, Zone z) {
    int n = 0;
    for (const auto& p : defense.allPlayers())
        if (p.isActive() && !p.prone && !p.distractedThisTurn
                         && !p.stats.has(SK::Titchy) && p.zone == z) ++n;
    return n;
}

int countTackleZones(const TeamState& defense, const TeamState& offense, Zone ballZone) {
    int raw = defendersInZone(defense, ballZone);
    int guards = 0, blockers = 0;
    for (const auto& p : offense.allPlayers()) {
        if (!p.isActive() || p.prone || p.hasBall || p.zone != ballZone) continue;
        if (p.stats.has(SK::Guard)) ++guards;
        else                        ++blockers;
    }
    int screened = guards + blockers / 2;
    return std::max(0, std::min(4, raw - screened));
}

// ── 3. Tentacles check ───────────────────────────────────────────────────────
// Before a player leaves a zone, each Tentacles defender in that zone gets a
// ST vs ST contest.  If any Tentacles player wins (ties go to tentacles), the
// mover is held and cannot advance this attempt.
bool blockedByTentacles(const PlayerStats& mover, const TeamState& opponents,
                        Zone fromZone, Dice& dice)
{
    for (const auto& opp : opponents.allPlayers()) {
        if (!opp.isActive() || opp.prone || opp.zone != fromZone) continue;
        if (!opp.stats.has(SK::Tentacles)) continue;
        int moverRoll = dice.d6() + mover.st;
        int tentRoll  = dice.d6() + opp.stats.st;
        if (tentRoll >= moverRoll) return true;  // held
    }
    return false;
}

// ── Block ─────────────────────────────────────────────────────────────────────
// Returns true if the block caused a turnover.
bool attemptBlock(PlayerState& attacker, TeamState& defenseTeam,
                  Dice& dice, TeamState& attackerTeam, bool isBlitz = false)
{
    PlayerState* target = nullptr;
    for (auto& p : defenseTeam.allPlayers()) {
        if (!p.canAct()) continue;
        if (p.zone == attacker.zone) { target = &p; break; }
    }
    if (!target) {
        for (auto& p : defenseTeam.allPlayers()) {
            if (!p.canAct()) continue;
            if (std::abs(p.zone - attacker.zone) <= 1) { target = &p; break; }
        }
    }
    if (!target) return false;

    int attackAssists = 0;
    for (const auto& p : attackerTeam.allPlayers()) {
        if (&p == &attacker || !p.canAct()) continue;
        if (std::abs(p.zone - attacker.zone) <= 1) ++attackAssists;
    }
    // Stunty: blocker gets one extra assist against a Stunty target.
    if (target->stats.has(SK::Stunty)) ++attackAssists;

    // Horns: +1 ST on a Blitz action.
    if (isBlitz && attacker.stats.has(SK::Horns)) ++attackAssists;

    int defAssists = 0;
    for (const auto& p : defenseTeam.allPlayers()) {
        if (&p == target || !p.canAct()) continue;
        if (std::abs(p.zone - target->zone) <= 1) ++defAssists;
    }

    // Dauntless: when blocking a higher-ST opponent, roll D6+own ST; on
    // equal-or-higher vs opponent's ST, proceed as if the STs are equal.
    if (attacker.stats.has(SK::Dauntless) && target->stats.st > attacker.stats.st) {
        if (dice.d6() + attacker.stats.st >= target->stats.st)
            attackAssists += target->stats.st - attacker.stats.st;
    }

    bool attackerHasBall = attacker.hasBall;
    BlockResult result = resolveBlock(attacker, *target, attackAssists, defAssists,
                                      dice, attackerHasBall, attackerTeam, defenseTeam,
                                      isBlitz);

    ++attackerTeam.blocksAttempted;
    if (result.outcome == BlockOutcome::DefenderDown     ||
        result.outcome == BlockOutcome::DefenderStumbles ||
        result.outcome == BlockOutcome::DefenderPushed)
        ++attackerTeam.blocksSuccessful;

    if (result.defenderInjured) {
        if (target->casualty) ++attackerTeam.casualties;
        else if (target->ko)  ++attackerTeam.knockouts;
    }

    // ── 5. Frenzy: must follow up and block again on any push/stumble/down ──
    bool frenzied = attacker.stats.has(SK::Frenzy)
                 && !result.followUpBlocked
                 && !result.turnover
                 && (result.outcome == BlockOutcome::DefenderPushed  ||
                     result.outcome == BlockOutcome::DefenderStumbles ||
                     result.outcome == BlockOutcome::DefenderDown);

    if (frenzied && target->isActive()) {
        if (!attackerHasBall) attacker.zone = target->zone;

        int atkA2 = 0, defA2 = 0;
        for (const auto& p : attackerTeam.allPlayers()) {
            if (&p == &attacker || !p.canAct()) continue;
            if (std::abs(p.zone - attacker.zone) <= 1) ++atkA2;
        }
        for (const auto& p : defenseTeam.allPlayers()) {
            if (&p == target || !p.canAct()) continue;
            if (std::abs(p.zone - target->zone) <= 1) ++defA2;
        }

        BlockResult r2 = resolveBlock(attacker, *target, atkA2, defA2,
                                       dice, attacker.hasBall, attackerTeam, defenseTeam);
        ++attackerTeam.blocksAttempted;
        if (r2.outcome == BlockOutcome::DefenderDown     ||
            r2.outcome == BlockOutcome::DefenderStumbles ||
            r2.outcome == BlockOutcome::DefenderPushed)
            ++attackerTeam.blocksSuccessful;
        if (r2.defenderInjured) {
            if (target->casualty) ++attackerTeam.casualties;
            else if (target->ko)  ++attackerTeam.knockouts;
        }
        if (r2.turnover) return true;
    } else if (!attackerHasBall && !result.followUpBlocked && !attacker.rootedThisTurn &&
               (result.outcome == BlockOutcome::DefenderPushed ||
                result.outcome == BlockOutcome::DefenderStumbles) &&
               attacker.zone < Zone::OppEndZone) {
        // Non-Frenzy follow-up: blocker steps forward into vacated space
        attacker.zone = attacker.zone + 1;
    }

    return result.turnover;
}

// ── 4. Interception attempt ───────────────────────────────────────────────────
// The most-agile defender in the pass path (receiver's or passer's zone) gets
// one attempt.  Target = max(4, ag + 2) on D6 (lower AG = harder to intercept).
// Returns true if ball was intercepted (turnover).
bool attemptInterception(PlayerState& passer, PlayerState& receiver,
                          TeamState& defense, Dice& dice)
{
    PlayerState* interceptor = nullptr;
    int bestAg = 99;
    for (auto& d : defense.allPlayers()) {
        if (!d.isActive() || d.prone) continue;
        if (d.zone == receiver.zone || d.zone == passer.zone) {
            if (d.stats.ag < bestAg) { bestAg = d.stats.ag; interceptor = &d; }
        }
    }
    if (!interceptor) return false;

    int target = std::max(4, interceptor->stats.ag + 2);
    if (dice.d6() >= target) {
        passer.hasBall    = false;
        receiver.hasBall  = false;
        interceptor->hasBall = true;
        return true;
    }
    return false;
}

// ── Advance ball carrier ─────────────────────────────────────────────────────
// Now takes GameContext for Tentacles and weather-adjusted pickup.
bool advanceBallCarrier(TeamState& offense, TeamState& defense, Dice& dice,
                        const GameContext& ctx)
{
    PlayerState* carrier = offense.ballCarrier();
    if (!carrier || !carrier->isActive() || carrier->stunned || carrier->prone) return false;

    int maxZoneAttempts = std::max(1, carrier->stats.ma / 3);
    if (carrier->stats.has(SK::Sprint)) {
        bool sprintOk = dice.d6() >= 2;
        if (!sprintOk && carrier->stats.has(SK::SureFeet)) sprintOk = dice.d6() >= 2;
        if (sprintOk) ++maxZoneAttempts;
    }

    bool dodgeReroll = carrier->stats.has(SK::Dodge);
    bool proReroll   = carrier->stats.has(SK::Pro);

    for (int attempt = 0; attempt < maxZoneAttempts; ++attempt) {
        Zone current = carrier->zone;
        if (current == Zone::OppEndZone) {
            ++offense.score; ++offense.touchdowns;
            carrier->hasBall = false;
            return true;
        }

        // ── 3. Tentacles: each Tentacles defender in current zone gets to hold the carrier ──
        if (blockedByTentacles(carrier->stats, defense, current, dice)) {
            // Carrier is held — failed dodge equivalent (carrier stays, no ball drop from Tentacles)
            carrier->prone   = true;
            carrier->hasBall = false;
            // Defense tries to pick up the loose ball.
            for (auto& p : defense.allPlayers()) {
                if (!p.canAct() || p.zone != current) continue;
                bool sh = p.stats.has(SK::SureHands);
                int pickupTarget = std::clamp(p.stats.ag + ctx.catchModifier, 2, 6);
                bool picked = dice.d6() >= pickupTarget;
                if (!picked && sh) picked = dice.d6() >= pickupTarget;
                if (!picked && useTeamReroll(defense, dice))
                    picked = dice.d6() >= pickupTarget;
                if (picked) p.hasBall = true;
                break;
            }
            return false;
        }

        int tz = countTackleZones(defense, offense, current);
        if (carrier->stats.has(SK::Stunty)) ++tz;
        if (tz > 0) {
            // Effective AG for dodge rolls:
            //   Break Tackle: may substitute ST for AG (beneficial when ST < AG).
            //   Titchy:       +1 modifier to own dodge (equivalent to -1 on target, i.e. use ag-1).
            int dodgeAg = carrier->stats.has(SK::BreakTackle)
                          ? std::min(carrier->stats.ag, carrier->stats.st)
                          : carrier->stats.ag;
            if (carrier->stats.has(SK::Titchy)) --dodgeAg;
            auto tryDodge = [&](int ag, int zones) -> bool {
                bool dodgeSkillBefore = dodgeReroll;
                bool dodged = dice.dodgeRoll(ag, zones, dodgeReroll);
                bool anyRerollUsed = dodgeSkillBefore && !dodgeReroll;

                if (!dodged && !anyRerollUsed && proReroll
                            && dice.useSkill(carrier->strategy.pro)
                            && dice.d6() >= 3) {
                    dodged = dice.successRoll(std::clamp(ag + zones, 2, 6));
                    proReroll = false;
                    anyRerollUsed = true;
                }
                if (!dodged && !anyRerollUsed && useTeamReroll(offense, dice, carrier))
                    dodged = dice.successRoll(std::clamp(ag + zones, 2, 6));
                return dodged;
            };

            auto defensePickup = [&]() {
                for (auto& p : defense.allPlayers()) {
                    if (!p.canAct() || p.zone != current) continue;
                    bool sureHands = p.stats.has(SK::SureHands);
                    int pickupTarget = std::clamp(p.stats.ag + ctx.catchModifier, 2, 6);
                    bool picked = dice.d6() >= pickupTarget;
                    bool skillUsed = p.stats.has(SK::SureHands) && !sureHands;
                    if (!picked && !skillUsed && useTeamReroll(defense, dice))
                        picked = dice.d6() >= pickupTarget;
                    if (picked) p.hasBall = true;
                    break;
                }
            };

            if (!tryDodge(dodgeAg, tz)) {
                carrier->prone   = true;
                carrier->hasBall = false;
                defensePickup();
                return false;
            }

            // Diving Tackle
            for (auto& def : defense.allPlayers()) {
                if (!def.canAct() || def.zone != current) continue;
                if (!def.stats.has(SK::DivingTackle)) continue;
                if (!dice.useSkill(def.strategy.divingTackle)) continue;
                def.prone = true;
                if (!tryDodge(dodgeAg, 1)) {
                    carrier->prone   = true;
                    carrier->hasBall = false;
                    defensePickup();
                    return false;
                }
                break;
            }
        }

        carrier->zone = current + 1;
    }

    if (carrier->zone == Zone::OppEndZone) {
        ++offense.score; ++offense.touchdowns;
        carrier->hasBall = false;
        return true;
    }
    return false;
}

// ── 4. Pass ───────────────────────────────────────────────────────────────────
// Now respects weather context (blizzard, paModifier, catchModifier) and checks
// for interceptions after a successful throw.
bool attemptPass(TeamState& offense, const TeamState& defense, Dice& dice,
                 const GameContext& ctx)
{
    // 9. Blizzard: no passing at all.
    if (ctx.blizzard) return false;

    PlayerState* carrier = offense.ballCarrier();
    if (!carrier || !carrier->isActive() || carrier->stunned || carrier->prone) return false;
    if (!carrier->stats.pa.has_value()) return false;

    Zone targetZone = std::min(Zone::OppEndZone, carrier->zone + 1);

    PlayerState* receiver = nullptr;
    for (auto& p : offense.allPlayers()) {
        if (&p == carrier || !p.canAct() || p.hasBall) continue;
        if (p.zone == targetZone) { receiver = &p; break; }
    }
    if (!receiver) return false;

    int rangeMod = (targetZone == Zone::OppEndZone) ? 1 : 0;
    ++offense.passesAttempted;

    int effectivePa = *carrier->stats.pa + ctx.paModifier;
    if (carrier->stats.has(SK::Accurate)) --effectivePa;
    if (!carrier->stats.has(SK::NervesOfSteel))
        effectivePa += countTackleZones(defense, offense, carrier->zone);

    bool passReroll = carrier->stats.has(SK::Pass);
    bool thrown = dice.passRoll(effectivePa, rangeMod, passReroll);
    bool passSkillUsed = carrier->stats.has(SK::Pass) && !passReroll;
    if (!thrown && !passSkillUsed && useTeamReroll(offense, dice, carrier))
        thrown = dice.successRoll(std::clamp(effectivePa + rangeMod, 2, 6));
    if (!thrown) return false;

    // ── 4. Interception: most agile eligible defender gets one attempt ────────
    // Cast away const — interception modifies defender state (hasBall).
    if (attemptInterception(*carrier, *receiver,
                             const_cast<TeamState&>(defense), dice)) {
        carrier->hasBall = false;
        return false;  // turnover — defense caught it
    }

    // ── Catch ─────────────────────────────────────────────────────────────────
    int effectiveCatchAg = receiver->stats.ag + ctx.catchModifier;
    if (receiver->stats.has(SK::DivingCatch)) --effectiveCatchAg;
    if (!receiver->stats.has(SK::NervesOfSteel))
        effectiveCatchAg += countTackleZones(defense, offense, receiver->zone);

    bool catchReroll = receiver->stats.has(SK::Catch);
    bool caught = dice.catchRoll(effectiveCatchAg, catchReroll);
    bool catchSkillUsed = receiver->stats.has(SK::Catch) && !catchReroll;
    if (!caught && !catchSkillUsed && useTeamReroll(offense, dice, receiver))
        caught = dice.successRoll(std::clamp(effectiveCatchAg, 2, 6));

    if (caught) {
        carrier->hasBall  = false;
        receiver->hasBall = true;
        ++offense.passesCompleted;
        return true;
    }

    carrier->hasBall = false;
    return false;
}

// ── 7. Hand-off ───────────────────────────────────────────────────────────────
// Carrier throws to an adjacent teammate using AG (not PA).
// Returns true if the hand-off succeeded (new carrier has ball).
// Sets carrier->hasBall = false on any attempt regardless of success.
bool attemptHandoff(TeamState& offense, const TeamState& defense, Dice& dice,
                    const GameContext& ctx)
{
    PlayerState* carrier = offense.ballCarrier();
    if (!carrier || !carrier->isActive() || carrier->stunned || carrier->prone) return false;

    Zone ahead = std::min(Zone::OppEndZone, carrier->zone + 1);

    // Find a target in same zone or ahead who hasn't activated yet.
    PlayerState* target = nullptr;
    for (auto& p : offense.allPlayers()) {
        if (&p == carrier || !p.canAct() || p.hasBall) continue;
        if (p.zone == ahead || p.zone == carrier->zone) { target = &p; break; }
    }
    if (!target) return false;

    carrier->activated = true;
    ++offense.passesAttempted;

    // Throw: AG roll, +1 per TZ on the carrier (unless Nerves of Steel).
    int tzOnCarrier = countTackleZones(defense, offense, carrier->zone);
    if (carrier->stats.has(SK::NervesOfSteel)) tzOnCarrier = 0;
    int throwTarget = std::clamp(carrier->stats.ag + tzOnCarrier, 2, 6);
    bool throwOk = dice.d6() >= throwTarget;
    if (!throwOk && useTeamReroll(offense, dice, carrier))
        throwOk = dice.d6() >= throwTarget;

    if (!throwOk) {
        carrier->hasBall = false;
        return false;  // fumble — turnover
    }

    // Catch: AG roll, modified by TZ on the receiver and weather.
    int catchAg = target->stats.ag + ctx.catchModifier;
    if (target->stats.has(SK::Catch)) catchAg = std::max(2, catchAg - 1);
    int tzAtTarget = countTackleZones(defense, offense, target->zone);
    if (target->stats.has(SK::NervesOfSteel)) tzAtTarget = 0;
    int catchTarget = std::clamp(catchAg + tzAtTarget, 2, 6);
    bool catchReroll = target->stats.has(SK::Catch);
    bool caught = dice.d6() >= catchTarget;
    bool catchSkillUsed = target->stats.has(SK::Catch) && !catchReroll;
    if (!caught && !catchSkillUsed && useTeamReroll(offense, dice, target))
        caught = dice.d6() >= catchTarget;

    carrier->hasBall = false;
    if (caught) {
        target->hasBall = true;
        ++offense.passesCompleted;
        return true;
    }
    return false;  // incomplete — ball is loose, turnover
}

// ── 6. Foul ───────────────────────────────────────────────────────────────────
// One foul per turn. Fouler makes an unassisted armor roll against a prone
// opponent. Dirty Player (+1 armor + injury). Sneaky Git avoids ejection on doubles.
void attemptFoul(TeamState& offense, TeamState& defense, Dice& dice)
{
    // Find the most valuable prone target (prefer ball carrier or team captain).
    PlayerState* foulTarget = nullptr;
    for (auto& d : defense.allPlayers()) {
        if (!d.isOnPitch() || !d.prone) continue;
        if (!foulTarget || d.hasBall || d.isTeamCaptain) foulTarget = &d;
    }
    if (!foulTarget) return;

    // Find an adjacent fouler who hasn't acted yet.
    PlayerState* fouler = nullptr;
    for (auto& p : offense.allPlayers()) {
        if (!p.canAct() || p.hasBall) continue;
        if (std::abs(p.zone - foulTarget->zone) <= 1) { fouler = &p; break; }
    }
    if (!fouler) return;

    fouler->activated = true;

    bool dirtyPlayer = fouler->stats.has(SK::DirtyPlayer);
    bool sneakyGit   = fouler->stats.has(SK::SneakyGit);
    int bonus        = dirtyPlayer ? 1 : 0;

    int die1 = dice.d6(), die2 = dice.d6();
    int total = die1 + die2 + bonus;

    // Ejection on doubles (unless Sneaky Git).
    if (die1 == die2 && !sneakyGit) {
        fouler->casualty = true;  // sent off for the game
    }

    if (total > foulTarget->stats.av) {
        auto inj = dice.injuryRoll(bonus);
        if (inj == Dice::Injury::Casualty && defense.hasApothecary && !defense.apothecaryUsed) {
            defense.apothecaryUsed = true;
            auto reroll = dice.injuryRoll(0);
            if (reroll < inj) inj = reroll;
        }
        if (inj == Dice::Injury::Casualty && foulTarget->stats.has(SK::Regeneration) && dice.d6() >= 4)
            inj = Dice::Injury::KO;

        if      (inj == Dice::Injury::KO)       { foulTarget->ko = true;       ++offense.knockouts; }
        else if (inj == Dice::Injury::Casualty) { foulTarget->casualty = true; ++offense.casualties; }
        else                                     { foulTarget->stunned = true;  }
    }
}

// ============================================================
// Simulate one team's turn
// ============================================================
bool simulateTurn(TeamState& offense, TeamState& defense, Dice& dice,
                  int turnsLeft, const GameContext& ctx)
{
    for (auto& p : offense.allPlayers()) { p.activated = false; p.distractedThisTurn = false; p.rootedThisTurn = false; }
    for (auto& p : defense.allPlayers()) { p.activated = false; p.distractedThisTurn = false; p.rootedThisTurn = false; }

    bool offenseBlitzUsed   = false;
    bool offensePassUsed    = false;
    bool offenseHandoffUsed = false;
    bool offenseFoulUsed    = false;
    bool defenseBlitzUsed   = false;

    // Stand up prone/stunned players.
    for (auto& p : offense.allPlayers()) {
        if (!p.isActive()) continue;
        if (p.stunned) { p.stunned = p.prone = false; }
        else if (p.prone) { p.prone = false; }
    }
    for (auto& p : defense.allPlayers()) {
        if (!p.isActive()) continue;
        if (p.stunned) { p.stunned = p.prone = false; }
        else if (p.prone) { p.prone = false; }
    }

    // ── Blocking phase ─────────────────────────────────────────────────────
    int blocksThisTurn = 0;
    for (auto& p : offense.allPlayers()) {
        if (!p.canAct() || p.hasBall || blocksThisTurn >= 3) continue;
        bool any = false;
        for (const auto& d : defense.allPlayers()) {
            if (!d.canAct()) continue;
            if (std::abs(d.zone - p.zone) <= 1) { any = true; break; }
        }
        if (!any) continue;
        if (checkActivationRoll(p, offense, dice)) continue;
        checkTakeRoot(p, dice);  // rooted players may still block in place
        // First offensive block each turn is the Blitz action (eligible for Horns).
        bool thisIsBlitz = !offenseBlitzUsed;
        if (thisIsBlitz) offenseBlitzUsed = true;
        p.activated = true;
        bool turnover = attemptBlock(p, defense, dice, offense, thisIsBlitz);
        ++blocksThisTurn;
        if (turnover) return false;
    }

    // ── Defense blitz ───────────────────────────────────────────────────────
    {
        PlayerState* carrier = offense.ballCarrier();
        int blitzThreshold = (turnsLeft >= 6) ? 3 : (turnsLeft >= 3) ? 4 : 5;
        // Sidestep: pushing the carrier stays neutral in our model, so a blitz
        // only helps if it knocks them down.  Raise the threshold so the defense
        // is less eager to blitz a Sidestep carrier.
        if (carrier && carrier->stats.has(SK::Sidestep)) blitzThreshold += 2;
        if (carrier && !defenseBlitzUsed && dice.d6() >= blitzThreshold) {
            for (auto& d : defense.allPlayers()) {
                if (!d.canAct()) continue;
                if (std::abs(d.zone - carrier->zone) <= 1) {
                    if (checkActivationRoll(d, defense, dice)) break;
                    // Take Root: rooted player can't blitz (requires movement).
                    if (checkTakeRoot(d, dice)) break;
                    defenseBlitzUsed = true;
                    d.activated      = true;
                    d.zone = carrier->zone;

                    int defAssists = 0;
                    for (const auto& od : defense.allPlayers()) {
                        if (&od == &d || !od.canAct()) continue;
                        if (std::abs(od.zone - d.zone) <= 1) ++defAssists;
                    }
                    if (carrier->stats.has(SK::Stunty)) ++defAssists;
                    // Horns: +1 ST on the Blitz action.
                    if (d.stats.has(SK::Horns)) ++defAssists;

                    int atkAssists = 0;
                    for (const auto& op : offense.allPlayers()) {
                        if (!op.canAct() || op.hasBall) continue;
                        if (std::abs(op.zone - carrier->zone) <= 1) ++atkAssists;
                    }

                    // Dauntless: blitzer with lower ST contests to equalize.
                    if (d.stats.has(SK::Dauntless) && carrier->stats.st > d.stats.st) {
                        if (dice.d6() + d.stats.st >= carrier->stats.st)
                            defAssists += carrier->stats.st - d.stats.st;
                    }

                    bool hadBall = carrier->hasBall;
                    BlockResult res = resolveBlock(d, *carrier, defAssists, atkAssists,
                                                   dice, false, defense, offense, true);
                    ++defense.blocksAttempted;
                    if (res.outcome == BlockOutcome::DefenderDown     ||
                        res.outcome == BlockOutcome::DefenderStumbles ||
                        res.outcome == BlockOutcome::DefenderPushed)
                        ++defense.blocksSuccessful;
                    if (res.defenderInjured) {
                        if (carrier->casualty) ++defense.casualties;
                        else if (carrier->ko)  ++defense.knockouts;
                    }

                    // ── 5. Frenzy follow-up for the blitzing defender ──────
                    bool defFrenzied = d.stats.has(SK::Frenzy)
                                    && !res.followUpBlocked
                                    && !res.turnover
                                    && (res.outcome == BlockOutcome::DefenderPushed  ||
                                        res.outcome == BlockOutcome::DefenderStumbles ||
                                        res.outcome == BlockOutcome::DefenderDown);
                    if (defFrenzied && carrier->isActive()) {
                        d.zone = carrier->zone;
                        int da2 = 0, aa2 = 0;
                        for (const auto& od : defense.allPlayers()) {
                            if (&od == &d || !od.canAct()) continue;
                            if (std::abs(od.zone - d.zone) <= 1) ++da2;
                        }
                        for (const auto& op : offense.allPlayers()) {
                            if (!op.canAct() || op.hasBall) continue;
                            if (std::abs(op.zone - carrier->zone) <= 1) ++aa2;
                        }
                        BlockResult r2 = resolveBlock(d, *carrier, da2, aa2,
                                                       dice, false, defense, offense);
                        ++defense.blocksAttempted;
                        if (r2.defenderInjured) {
                            if (carrier->casualty) ++defense.casualties;
                            else if (carrier->ko)  ++defense.knockouts;
                        }
                        res = r2;
                    }

                    // Turnover if carrier lost the ball (knockdown OR Strip Ball).
                    if (hadBall && !carrier->hasBall) {
                        for (auto& p2 : defense.allPlayers()) {
                            if (!p2.canAct() || p2.zone != carrier->zone) continue;
                            bool sureHands = p2.stats.has(SK::SureHands);
                            int pickupTarget = std::clamp(p2.stats.ag + ctx.catchModifier, 2, 6);
                            bool picked = dice.d6() >= pickupTarget;
                            bool skillUsed = p2.stats.has(SK::SureHands) && !sureHands;
                            if (!picked && !skillUsed && useTeamReroll(defense, dice))
                                picked = dice.d6() >= pickupTarget;
                            if (picked) p2.hasBall = true;
                            break;
                        }
                        return false;  // turnover for offense
                    }
                    break;
                }
            }
        }
    }

    // ── 6. Foul phase ──────────────────────────────────────────────────────
    if (!offenseFoulUsed) {
        bool hasProne = false;
        for (const auto& d : defense.allPlayers())
            if (d.isOnPitch() && d.prone) { hasProne = true; break; }
        if (hasProne) {
            offenseFoulUsed = true;
            attemptFoul(offense, defense, dice);
        }
    }

    // ── Ball carrier advance ────────────────────────────────────────────────
    PlayerState* carrier = offense.ballCarrier();

    // ── Activation rolls: Bone Head / Really Stupid / Animal Savagery ───────
    if (carrier && carrier->canAct() && checkActivationRoll(*carrier, offense, dice))
        return false;

    // ── Take Root: carrier rolls 2+; on 1 they're rooted and can't advance ──
    if (carrier && carrier->canAct()) {
        checkTakeRoot(*carrier, dice);
        if (carrier->rootedThisTurn) {
            carrier->activated = true;
            return false;
        }
    }

    // ── Unchannelled Fury: AG test before non-Block/Blitz actions ────────────
    if (carrier && carrier->canAct() && checkUnchannelledFury(*carrier, dice))
        return false;

    // ── Animosity: 1/6 chance carrier refuses to pass, hand-off, or run ─────
    // (Simplified: real Animosity only fires vs specific sub-races; here we
    //  approximate as a flat 1/6 activation failure for any action.)
    if (carrier && carrier->canAct() && carrier->stats.has(SK::Animosity)
                && dice.d6() == 1) {
        carrier->activated = true;
        return false;
    }

    // ── 2. Stalling: if ahead and carrier is safely in opponent territory,
    //    prefer to delay the TD rather than give defense time to equalise.
    //    Always score on the last 2 turns of the half (turnsLeft ≤ 2).
    if (carrier && carrier->canAct() && offense.score > defense.score
               && carrier->zone == Zone::OppHalf
               && turnsLeft > 2
               && dice.useSkill(0.70f)) {
        // Stall: carrier holds position this turn — no advance action taken.
        carrier->activated = true;
        return false;
    }

    if (carrier && carrier->canAct()) {
        // ── 7. Hand-off: prefer when carrier can't pass or a faster teammate is ahead.
        bool triedHandoff = false;
        if (!offenseHandoffUsed && !carrier->stats.pa.has_value() && turnsLeft >= 2) {
            Zone ahead = std::min(Zone::OppEndZone, carrier->zone + 1);
            bool targetAhead = std::ranges::any_of(offense.allPlayers(), [&](const auto& p) {
                return &p != carrier && p.canAct() && (p.zone == ahead || p.zone == carrier->zone);
            });
            if (targetAhead && dice.useSkill(0.40f)) {
                offenseHandoffUsed = true;
                triedHandoff = true;
                bool ok = attemptHandoff(offense, defense, dice, ctx);
                if (!offense.ballCarrier()) return false;  // ball lost
                carrier = offense.ballCarrier();
                if (!ok) return false;
                // Fall through: new carrier (if any) may still advance.
            }
        }

        // ── Pass or run ────────────────────────────────────────────────────
        carrier = offense.ballCarrier();
        if (carrier && carrier->canAct()) {
            bool triedPass = false;
            if (!offensePassUsed && !triedHandoff && carrier->stats.pa.has_value()
                                 && turnsLeft >= 3) {
                Zone ahead = std::min(Zone::OppEndZone, carrier->zone + 1);
                bool receiverAhead = std::ranges::any_of(offense.allPlayers(),
                    [&](const auto& p) {
                        return &p != carrier && p.canAct() && p.zone == ahead;
                    });
                if (receiverAhead && dice.d6() >= 4) {
                    offensePassUsed = true;
                    carrier->activated = true;
                    triedPass = attemptPass(offense, defense, dice, ctx);
                    if (!offense.ballCarrier()) return false;
                }
            }

            if (!triedPass) {
                carrier = offense.ballCarrier();
                if (carrier && carrier->canAct()) {
                    carrier->activated = true;
                    return advanceBallCarrier(offense, defense, dice, ctx);
                }
            }
        }
    }

    return false;
}

} // namespace

// ============================================================
// Simulate a full game
// ============================================================
GameResult simulateGame(TeamState team1, TeamState team2, Dice& dice) {
    // 9. Roll weather once per game.
    GameContext ctx = rollWeather(dice);

    // Riotous Rookies.
    auto addRiotousRookies = [&](TeamState& team) {
        if (!team.riotousRookies) return;
        int count = dice.d3() + dice.d3() + 1;
        for (int i = 0; i < count; ++i) {
            if (team.playerCount >= static_cast<int>(team.players.size())) break;
            PlayerState rookie = team.riotousRookieTemplate;
            rookie.stats.name = std::format("Rookie {}", i + 1);
            team.players[team.playerCount++] = std::move(rookie);
        }
    };
    addRiotousRookies(team1);
    addRiotousRookies(team2);

    bool team1Receives = (dice.d6() >= 4);
    constexpr int TURNS_PER_HALF = 8;

    for (int half = 0; half < 2; ++half) {
        team1.leaderRerollUsedThisHalf = false;
        team2.leaderRerollUsedThisHalf = false;

        TeamState& offense = team1Receives ? team1 : team2;
        TeamState& defense = team1Receives ? team2 : team1;

        setupKickoff(offense, defense, dice, ctx, half == 0);

        for (int t = 0; t < TURNS_PER_HALF; ++t) {
            if (&offense == &team1) {
                int turnsLeft = TURNS_PER_HALF - t;
                bool td = simulateTurn(team1, team2, dice, turnsLeft, ctx);
                if (td && t < TURNS_PER_HALF - 1) {
                    team1Receives = !team1Receives;
                    TeamState& no = team1Receives ? team1 : team2;
                    TeamState& nd = team1Receives ? team2 : team1;
                    setupKickoff(no, nd, dice, ctx);
                }
            } else {
                int turnsLeft = TURNS_PER_HALF - t;
                bool td = simulateTurn(team2, team1, dice, turnsLeft, ctx);
                if (td && t < TURNS_PER_HALF - 1) {
                    team1Receives = !team1Receives;
                    TeamState& no = team1Receives ? team1 : team2;
                    TeamState& nd = team1Receives ? team2 : team1;
                    setupKickoff(no, nd, dice, ctx);
                }
            }

            {
                TeamState& off2 = team1Receives ? team2 : team1;
                TeamState& def2 = team1Receives ? team1 : team2;
                if (&off2 != &offense) {
                    int turnsLeft = TURNS_PER_HALF - t;
                    bool td = simulateTurn(off2, def2, dice, turnsLeft, ctx);
                    if (td && t < TURNS_PER_HALF - 1) {
                        team1Receives = !team1Receives;
                        TeamState& no = team1Receives ? team1 : team2;
                        TeamState& nd = team1Receives ? team2 : team1;
                        setupKickoff(no, nd, dice, ctx);
                    }
                }
            }
        }

        team1Receives = !team1Receives;
    }

    GameResult r;
    r.score1      = team1.score;       r.score2      = team2.score;
    r.casualties1 = team1.casualties;  r.casualties2 = team2.casualties;
    r.ko1         = team1.knockouts;   r.ko2         = team2.knockouts;
    r.blocks1     = team1.blocksSuccessful; r.blocks2 = team2.blocksSuccessful;
    r.passes1     = team1.passesCompleted;  r.passes2 = team2.passesCompleted;
    return r;
}

// ============================================================
// Run N simulations with OpenMP
// ============================================================
SimulationStats runSimulations(const TeamConfig& cfg1, const TeamConfig& cfg2,
                               const SeedData& seed, int numSimulations,
                               int numThreads)
{
    if (numThreads <= 0) numThreads = omp_get_max_threads();

    TeamState base1 = buildTeamState(cfg1, seed);
    TeamState base2 = buildTeamState(cfg2, seed);

    SimulationStats stats;
    stats.totalGames = numSimulations;

    int wins1 = 0, wins2 = 0, draws = 0;
    long long score1 = 0, score2 = 0;
    long long cas1 = 0, cas2 = 0;
    long long ko1 = 0, ko2 = 0;
    long long blk1 = 0, blk2 = 0;
    long long pass1 = 0, pass2 = 0;

    std::vector<uint64_t> seeds(static_cast<size_t>(numThreads));
    {
        std::mt19937_64 seeder(std::random_device{}());
        for (auto& s : seeds) s = seeder();
    }

#pragma omp parallel num_threads(numThreads) \
        reduction(+:wins1, wins2, draws, score1, score2, \
                    cas1, cas2, ko1, ko2, blk1, blk2, pass1, pass2)
    {
        int tid = omp_get_thread_num();
        Dice dice(seeds[static_cast<size_t>(tid)] ^ static_cast<uint64_t>(tid * 0xDEADBEEFull));

#pragma omp for schedule(dynamic, 64)
        for (int i = 0; i < numSimulations; ++i) {
            GameResult r = simulateGame(base1, base2, dice);
            if      (r.score1 > r.score2) ++wins1;
            else if (r.score2 > r.score1) ++wins2;
            else                          ++draws;
            score1 += r.score1;  score2 += r.score2;
            cas1   += r.casualties1; cas2 += r.casualties2;
            ko1    += r.ko1;         ko2  += r.ko2;
            blk1   += r.blocks1;     blk2 += r.blocks2;
            pass1  += r.passes1;     pass2 += r.passes2;
        }
    }

    stats.wins1 = wins1; stats.wins2 = wins2; stats.draws = draws;
    stats.totalScore1 = score1; stats.totalScore2 = score2;
    stats.totalCasualties1 = cas1; stats.totalCasualties2 = cas2;
    stats.totalKO1 = ko1; stats.totalKO2 = ko2;
    stats.totalBlocks1 = blk1; stats.totalBlocks2 = blk2;
    stats.totalPasses1 = pass1; stats.totalPasses2 = pass2;
    return stats;
}

// ============================================================
// Collect N individual game results with OpenMP
// ============================================================
std::vector<GameResult> collectSamples(const TeamConfig& cfg1, const TeamConfig& cfg2,
                                        const SeedData& seed, int numSamples,
                                        int numThreads)
{
    if (numThreads <= 0) numThreads = omp_get_max_threads();

    TeamState base1 = buildTeamState(cfg1, seed);
    TeamState base2 = buildTeamState(cfg2, seed);

    std::vector<GameResult> results(static_cast<size_t>(numSamples));

    std::vector<uint64_t> seeds(static_cast<size_t>(numThreads));
    {
        std::mt19937_64 seeder(std::random_device{}());
        for (auto& s : seeds) s = seeder();
    }

#pragma omp parallel num_threads(numThreads)
    {
        int tid = omp_get_thread_num();
        Dice dice(seeds[static_cast<size_t>(tid)] ^ static_cast<uint64_t>(tid) * 0xCAFEBABEull);

#pragma omp for schedule(dynamic, 256)
        for (int i = 0; i < numSamples; ++i)
            results[static_cast<size_t>(i)] = simulateGame(base1, base2, dice);
    }

    return results;
}

// ============================================================
// Fold individual results into aggregate statistics
// ============================================================
SimulationStats aggregateResults(const std::vector<GameResult>& results) {
    SimulationStats stats;
    stats.totalGames = static_cast<int>(results.size());
    for (const auto& r : results) {
        if      (r.score1 > r.score2) ++stats.wins1;
        else if (r.score2 > r.score1) ++stats.wins2;
        else                          ++stats.draws;
        stats.totalScore1      += r.score1;
        stats.totalScore2      += r.score2;
        stats.totalCasualties1 += r.casualties1;
        stats.totalCasualties2 += r.casualties2;
        stats.totalKO1         += r.ko1;
        stats.totalKO2         += r.ko2;
        stats.totalBlocks1     += r.blocks1;
        stats.totalBlocks2     += r.blocks2;
        stats.totalPasses1     += r.passes1;
        stats.totalPasses2     += r.passes2;
    }
    return stats;
}
