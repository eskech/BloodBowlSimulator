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
    ts.name           = cfg.name;
    ts.race           = cfg.race;
    ts.hasApothecary  = cfg.hasApothecary;
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

        // Resolve effective strategy: player override merged into team default
        PlayerStrategy strategy = pc.strategy.mergedWith(cfg.defaultStrategy);

        PlayerState ps;
        ps.stats    = std::move(stats);
        ps.strategy = strategy;
        ps.zone     = Zone::OwnHalf;
        ts.players[ts.playerCount++] = std::move(ps);
    }

    // Riotous Rookies: build a Snotling Lineman template for use at game start.
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

// Assign starting positions on kickoff
// offenseTeam receives the ball.
void setupKickoff(TeamState& offense, TeamState& defense, Dice& dice) {
    // KO recovery: each KO'd player rolls 4+ to return at every new drive (BB2020 §4.2)
    for (auto& p : offense.allPlayers()) {
        if (p.ko && !p.casualty && dice.d6() >= 4) {
            p.ko = p.prone = p.stunned = false;
        }
    }
    for (auto& p : defense.allPlayers()) {
        if (p.ko && !p.casualty && dice.d6() >= 4) {
            p.ko = p.prone = p.stunned = false;
        }
    }

    // Swarming: D3 Swarming-trait players from Reserves (inReserves) enter the pitch.
    // Cap = number of Swarming players already being fielded this drive (BB2020).
    for (TeamState* team : {&offense, &defense}) {
        int onPitch = 0;
        for (const auto& p : team->allPlayers())
            if (!p.ko && !p.casualty && !p.inReserves && p.stats.has(SK::Swarming)) ++onPitch;
        if (onPitch == 0) continue;

        int canEnter = std::min(dice.d3(), onPitch);
        for (auto& p : team->allPlayers()) {
            if (canEnter <= 0) break;
            if (p.inReserves && !p.casualty && p.stats.has(SK::Swarming)) {
                p.inReserves = false;
                --canEnter;
            }
        }
    }

    // Reset per-drive states
    for (auto& p : offense.allPlayers()) {
        if (!p.isOnPitch()) continue;
        p.prone   = false;
        p.hasBall = false;
        // Stunned players stand up at start of their turn;
        // on a new half KOs may recover (50%)
    }
    for (auto& p : defense.allPlayers()) {
        if (!p.isOnPitch()) continue;
        p.prone   = false;
        p.hasBall = false;
    }

    // Place players in zones
    // Offense: 3 on Line of Scrimmage (Midfield), rest in OwnHalf
    // Defense: 3 on Line of Scrimmage (Midfield), rest in OppHalf (from their view = OwnHalf here)
    int offLiners = 0, defLiners = 0;
    for (auto& p : offense.allPlayers()) {
        if (!p.isOnPitch()) continue;
        p.zone = (offLiners++ < 3) ? Zone::Midfield : Zone::OwnHalf;
    }
    for (auto& p : defense.allPlayers()) {
        if (!p.isOnPitch()) continue;
        // From offense perspective, defense is in OppHalf/OppEndZone
        p.zone = (defLiners++ < 3) ? Zone::Midfield : Zone::OppHalf;
    }

    // Ball placed in offense's OwnHalf (scatter from kick)
    // Give ball to first usable player in OwnHalf
    for (auto& p : offense.allPlayers()) {
        if (p.isActive() && p.zone == Zone::OwnHalf) {
            p.hasBall = true;
            break;
        }
    }
    // If no one in OwnHalf (unlikely at start), give to first active player
    if (!offense.ballCarrier()) {
        for (auto& p : offense.allPlayers()) {
            if (p.isActive()) { p.hasBall = true; break; }
        }
    }
}

// Count defenders in a zone (opponents from offense PoV)
int defendersInZone(const TeamState& defense, Zone z) {
    int n = 0;
    for (const auto& p : defense.allPlayers())
        if (p.isActive() && !p.prone && p.zone == z) ++n;
    return n;
}

// Count tackle zones on the ball carrier's path.
// Only a fraction of defenders in the zone are directly on the carrier's path –
// cap at 2 to reflect that blockers screen most defenders.
int countTackleZones(const TeamState& defense, Zone ballZone) {
    return std::min(2, defendersInZone(defense, ballZone));
}

// Attempt a block: attacker vs nearest opponent in same/adjacent zone
// Returns true if defender was knocked down / removed from path
bool attemptBlock(PlayerState& attacker, TeamState& defenseTeam,
                  Dice& dice, TeamState& attackerTeam)
{
    // Find nearest opponent to block (same zone preferred, else adjacent)
    PlayerState* target = nullptr;
    for (auto& p : defenseTeam.allPlayers()) {
        if (!p.canAct()) continue;
        if (p.zone == attacker.zone) { target = &p; break; }
    }
    if (!target) {
        for (auto& p : defenseTeam.allPlayers()) {
            if (!p.canAct()) continue;
            int diff = std::abs(p.zone - attacker.zone);
            if (diff <= 1) { target = &p; break; }
        }
    }
    if (!target) return false;

    // Count assists (simplified: 1 per adjacent same-team player not in tackle zone)
    int attackAssists = 0;
    for (const auto& p : attackerTeam.allPlayers()) {
        if (&p == &attacker || !p.canAct()) continue;
        if (std::abs(p.zone - attacker.zone) <= 1) ++attackAssists;
    }
    int defAssists = 0;
    for (const auto& p : defenseTeam.allPlayers()) {
        if (&p == target || !p.canAct()) continue;
        if (std::abs(p.zone - target->zone) <= 1) ++defAssists;
    }

    bool attackerHasBall = attacker.hasBall;
    BlockResult result = resolveBlock(attacker, *target, attackAssists, defAssists,
                                      dice, attackerHasBall, attackerTeam, defenseTeam);

    ++attackerTeam.blocksAttempted;

    if (result.outcome == BlockOutcome::DefenderDown     ||
        result.outcome == BlockOutcome::DefenderStumbles ||
        result.outcome == BlockOutcome::DefenderPushed)
    {
        ++attackerTeam.blocksSuccessful;
    }

    if (result.defenderInjured) {
        if (target->casualty) ++attackerTeam.casualties;
        else if (target->ko)  ++attackerTeam.knockouts;
    }

    return result.turnover;  // true = turnover (attacker fell)
}

// Simulate the ball carrier advancing toward the opponent's end zone.
// High-MA players can attempt to cross multiple zones in one turn.
// Applies Diving Tackle (forces a re-dodge) and Pro (re-roll failed dodge).
// Returns true if a touchdown was scored.
bool advanceBallCarrier(TeamState& offense, TeamState& defense, Dice& dice) {
    PlayerState* carrier = offense.ballCarrier();
    // canAct() is not re-checked here: activation is managed by simulateTurn.
    // We only verify the carrier exists and is physically on the pitch.
    if (!carrier || !carrier->isActive() || carrier->stunned || carrier->prone) return false;

    // All players get 2 base zone-crossing attempts per turn.
    // MA does not affect rush count in this zone model.
    [[maybe_unused]] int ma = carrier->stats.ma;
    int maxZoneAttempts = 2;
    // Sprint: one extra rush at 2+ (d6 >= 2). Sure Feet re-rolls one failed sprint attempt.
    if (carrier->stats.has(SK::Sprint)) {
        bool sprintOk = dice.d6() >= 2;
        if (!sprintOk && carrier->stats.has(SK::SureFeet)) sprintOk = dice.d6() >= 2;
        if (sprintOk) ++maxZoneAttempts;
    }

    // Track once-per-activation re-roll availability for the ball carrier.
    bool dodgeReroll = carrier->stats.has(SK::Dodge);
    bool proReroll   = carrier->stats.has(SK::Pro);

    for (int attempt = 0; attempt < maxZoneAttempts; ++attempt) {
        Zone current = carrier->zone;
        if (current == Zone::OppEndZone) {
            ++offense.score; ++offense.touchdowns;
            carrier->hasBall = false;
            return true;
        }

        int tz = countTackleZones(defense, current);
        if (tz > 0) {
            // Helper: attempt one dodge, enforcing exactly one re-roll of any kind.
            // Priority: Dodge skill → Pro → team re-roll (mutually exclusive).
            auto tryDodge = [&](int ag, int zones) -> bool {
                bool dodgeSkillBefore = dodgeReroll;
                bool dodged = dice.dodgeRoll(ag, zones, dodgeReroll);
                bool anyRerollUsed = dodgeSkillBefore && !dodgeReroll;

                if (!dodged && !anyRerollUsed && proReroll
                            && dice.useSkill(carrier->strategy.pro)
                            && dice.d6() >= 3)
                {
                    dodged = dice.successRoll(std::clamp(ag + zones, 2, 6));
                    proReroll = false;
                    anyRerollUsed = true;
                }
                if (!dodged && !anyRerollUsed && offense.rerollsRemaining > 0) {
                    --offense.rerollsRemaining;
                    dodged = dice.successRoll(std::clamp(ag + zones, 2, 6));
                }
                return dodged;
            };

            // Defense pickup helper (called when carrier drops ball).
            auto defensePickup = [&]() {
                for (auto& p : defense.allPlayers()) {
                    if (!p.canAct() || p.zone != current) continue;
                    bool sureHands = p.stats.has(SK::SureHands);
                    bool picked = dice.pickupRoll(p.stats.ag, 0, sureHands);
                    bool skillUsed = p.stats.has(SK::SureHands) && !sureHands;
                    if (!picked && !skillUsed && defense.rerollsRemaining > 0) {
                        --defense.rerollsRemaining;
                        picked = dice.successRoll(std::clamp(p.stats.ag, 2, 6));
                    }
                    if (picked) p.hasBall = true;
                    break;  // only first eligible player attempts
                }
            };

            if (!tryDodge(carrier->stats.ag, tz)) {
                carrier->prone   = true;
                carrier->hasBall = false;
                defensePickup();
                return false;
            }

            // ── Diving Tackle: a defender in this zone may fall prone to
            //    force the carrier to make one more dodge roll ──────────────
            for (auto& def : defense.allPlayers()) {
                if (!def.canAct() || def.zone != current) continue;
                if (!def.stats.has(SK::DivingTackle)) continue;
                if (!dice.useSkill(def.strategy.divingTackle)) continue;

                def.prone = true;
                if (!tryDodge(carrier->stats.ag, 1)) {
                    carrier->prone   = true;
                    carrier->hasBall = false;
                    defensePickup();
                    return false;
                }
                break;  // one Diving Tackle per dodge
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

// Try to pass the ball to an open receiver closer to end zone
bool attemptPass(TeamState& offense, const TeamState& defense, Dice& dice) {
    PlayerState* carrier = offense.ballCarrier();
    // Activation already validated by simulateTurn; just check physical state.
    if (!carrier || !carrier->isActive() || carrier->stunned || carrier->prone) return false;
    if (!carrier->stats.pa.has_value()) return false; // can't pass

    // Look for a receiver one zone ahead
    Zone targetZone = std::min(Zone::OppEndZone, carrier->zone + 1);

    PlayerState* receiver = nullptr;
    for (auto& p : offense.allPlayers()) {
        if (&p == carrier || !p.canAct() || p.hasBall) continue;
        if (p.zone == targetZone) { receiver = &p; break; }
    }
    if (!receiver) return false;

    int rangeMod = (targetZone == Zone::OppEndZone) ? 1 : 0;
    ++offense.passesAttempted;

    // Accurate: -1 to PA target. TZ in passer's zone: +1 per TZ (Nerves of Steel ignores).
    int effectivePa = *carrier->stats.pa;
    if (carrier->stats.has(SK::Accurate)) --effectivePa;
    if (!carrier->stats.has(SK::NervesOfSteel))
        effectivePa += countTackleZones(defense, carrier->zone);

    // Pass: Pass skill re-roll OR team re-roll — not both.
    bool passReroll = carrier->stats.has(SK::Pass);
    bool thrown = dice.passRoll(effectivePa, rangeMod, passReroll);
    bool passSkillUsed = carrier->stats.has(SK::Pass) && !passReroll;
    if (!thrown && !passSkillUsed && offense.rerollsRemaining > 0) {
        --offense.rerollsRemaining;
        thrown = dice.successRoll(std::clamp(effectivePa + rangeMod, 2, 6));
    }
    if (!thrown) return false;

    // Diving Catch: +1 modifier to AG on accurate catches (i.e. -1 to target).
    // TZ in receiver's zone: +1 per TZ (Nerves of Steel ignores).
    int effectiveCatchAg = receiver->stats.ag;
    if (receiver->stats.has(SK::DivingCatch)) --effectiveCatchAg;
    if (!receiver->stats.has(SK::NervesOfSteel))
        effectiveCatchAg += countTackleZones(defense, receiver->zone);

    // Catch: Catch skill re-roll OR team re-roll — not both.
    bool catchReroll = receiver->stats.has(SK::Catch);
    bool caught = dice.catchRoll(effectiveCatchAg, catchReroll);
    bool catchSkillUsed = receiver->stats.has(SK::Catch) && !catchReroll;
    if (!caught && !catchSkillUsed && offense.rerollsRemaining > 0) {
        --offense.rerollsRemaining;
        caught = dice.successRoll(std::clamp(effectiveCatchAg, 2, 6));
    }
    if (caught) {
        carrier->hasBall = false;
        receiver->hasBall = true;
        ++offense.passesCompleted;
        return true;
    }
    // Incomplete pass – ball scatters to receiver's zone (simplified: stays there loose)
    carrier->hasBall = false;
    // Try to recover (no one picks it up this action)
    return false;
}

// ============================================================
// Simulate one team's turn
// Returns true if a touchdown was scored (resets drive)
// ============================================================
bool simulateTurn(TeamState& offense, TeamState& defense, Dice& dice,
                  int turnsLeft)
{
    // --- Reset per-turn activation flags ---
    // Every player starts the turn unactivated; each may take exactly one action.
    for (auto& p : offense.allPlayers()) p.activated = false;
    for (auto& p : defense.allPlayers()) p.activated = false;

    // Per-turn once-only action gates (Blood Bowl 2020 §8)
    // blitzUsed  – only one player per team may make a Blitz action per turn
    // passUsed   – only one Pass action per turn
    // handoffUsed– only one Hand-off action per turn
    // foulUsed   – only one Foul action per turn
    // (Throw Team-mate / Kick Team-mate share the "pass" slot and are not yet simulated)
    [[maybe_unused]] bool offenseBlitzUsed  = false;
    bool offensePassUsed   = false;
    [[maybe_unused]] bool offenseHandoffUsed = false;
    [[maybe_unused]] bool offenseFoulUsed    = false;
    bool defenseBlitzUsed  = false;

    // --- Stand up prone players (they used their stunned turn already) ---
    for (auto& p : offense.allPlayers()) {
        if (!p.isActive()) continue;
        if (p.stunned) {
            p.stunned = false;
            p.prone   = false;  // stood up
        } else if (p.prone) {
            p.prone = false;  // stands up (costs half MA but simplified here)
        }
    }
    for (auto& p : defense.allPlayers()) {
        if (!p.isActive()) continue;
        if (p.stunned) {
            p.stunned = false;
            p.prone   = false;
        } else if (p.prone) {
            p.prone = false;
        }
    }

    // --- Blocking phase ---
    // Each offensive player may take a Block action (not Blitz — no movement).
    // Capped at 3 to reflect that the sim only models ~3 meaningful blocks per turn.
    int blocksThisTurn = 0;
    for (auto& p : offense.allPlayers()) {
        if (!p.canAct() || p.hasBall || blocksThisTurn >= 3) continue;
        // Only block if there are defenders to block nearby
        bool any = false;
        for (const auto& d : defense.allPlayers()) {
            if (!d.canAct()) continue;
            if (std::abs(d.zone - p.zone) <= 1) { any = true; break; }
        }
        if (!any) continue;

        p.activated = true;  // player spends their action on this Block
        bool turnover = attemptBlock(p, defense, dice, offense);
        ++blocksThisTurn;
        if (turnover) return false;  // attacker fell = turnover, no TD
    }

    // --- Defense blitz on ball carrier ---
    // ONE defender per turn may make a Blitz action (move + block).
    // 50% chance the most suitable defender is actually close enough to reach
    // the carrier (reflecting path obstructions and actual board distance).
    {
        PlayerState* carrier = offense.ballCarrier();
        if (carrier && !defenseBlitzUsed && dice.d6() >= 4) {
            for (auto& d : defense.allPlayers()) {
                if (!d.canAct()) continue;
                if (std::abs(d.zone - carrier->zone) <= 1) {
                    // Blitz: defender moves into carrier's zone and blocks.
                    // Mark both the blitz as used and the blitzer as activated.
                    defenseBlitzUsed = true;
                    d.activated      = true;
                    d.zone = carrier->zone;
                    int defAssists = 0;
                    for (const auto& od : defense.allPlayers()) {
                        if (&od == &d || !od.canAct()) continue;
                        if (std::abs(od.zone - d.zone) <= 1) ++defAssists;
                    }
                    int atkAssists = 0;
                    for (const auto& op : offense.allPlayers()) {
                        if (!op.canAct() || op.hasBall) continue;
                        if (std::abs(op.zone - carrier->zone) <= 1) ++atkAssists;
                    }
                    bool hadBall = carrier->hasBall;
                    BlockResult res = resolveBlock(d, *carrier, defAssists, atkAssists,
                                                   dice, false, defense, offense);
                    ++defense.blocksAttempted;
                    if (res.outcome == BlockOutcome::DefenderDown     ||
                        res.outcome == BlockOutcome::DefenderStumbles ||
                        res.outcome == BlockOutcome::DefenderPushed)
                        ++defense.blocksSuccessful;
                    if (res.defenderInjured) {
                        if (carrier->casualty) ++defense.casualties;
                        else if (carrier->ko)  ++defense.knockouts;
                    }
                    // If carrier KD: turnover, ball loose
                    if (res.outcome == BlockOutcome::DefenderDown  ||
                        res.outcome == BlockOutcome::DefenderStumbles ||
                        res.outcome == BlockOutcome::BothDown) {
                        if (carrier->prone || carrier->ko || carrier->casualty) {
                            bool wasBall = hadBall;
                            carrier->hasBall = false;
                            if (wasBall) {
                                // Try defense pick-up (first eligible player only;
                                // Sure Hands OR team re-roll — not both).
                                for (auto& p2 : defense.allPlayers()) {
                                    if (!p2.canAct() || p2.zone != carrier->zone) continue;
                                    bool sureHands = p2.stats.has(SK::SureHands);
                                    bool picked = dice.pickupRoll(p2.stats.ag, 0, sureHands);
                                    bool skillUsed = p2.stats.has(SK::SureHands) && !sureHands;
                                    if (!picked && !skillUsed && defense.rerollsRemaining > 0) {
                                        --defense.rerollsRemaining;
                                        picked = dice.successRoll(std::clamp(p2.stats.ag, 2, 6));
                                    }
                                    if (picked) p2.hasBall = true;
                                    break;
                                }
                                return false;  // turnover
                            }
                        }
                    }
                    break;  // one blitz per turn
                }
            }
        }
    }

    // --- Ball carrier advance ---
    // Decide: pass if receiver is open ahead and near end zone, otherwise run.
    // Either way the carrier takes exactly one action this turn.
    bool scored = false;
    PlayerState* carrier = offense.ballCarrier();
    if (carrier && carrier->canAct()) {
        // Consider passing if there's a viable receiver and the pass action
        // has not already been used this turn.
        bool triedPass = false;
        if (!offensePassUsed && carrier->stats.pa.has_value() && turnsLeft >= 3) {
            Zone ahead = std::min(Zone::OppEndZone, carrier->zone + 1);
            bool receiverAhead = std::ranges::any_of(offense.allPlayers(),
                [&](const auto& p) {
                    return &p != carrier && p.canAct() && p.zone == ahead;
                });
            if (receiverAhead && dice.d6() >= 4) {  // 50% chance to decide to pass
                offensePassUsed = true;
                carrier->activated = true;  // Pass action — carrier is now spent
                triedPass = attemptPass(offense, defense, dice);
                if (!offense.ballCarrier()) return false; // ball lost
            }
        }

        if (!triedPass) {
            carrier->activated = true;  // Move action — carrier is now spent
            scored = advanceBallCarrier(offense, defense, dice);
        }
    }
    return scored;
}


} // namespace

// ============================================================
// Simulate a full game
// ============================================================
GameResult simulateGame(TeamState team1, TeamState team2, Dice& dice) {
    // Riotous Rookies inducement: 2D3+1 Snotling Lineman Journeymen added before the game.
    // They start inReserves and enter via Swarming each drive.
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

    // Coin toss: team1 receives first half by default
    // (or randomise: dice.d6() >= 4 ? team1 receives : team2 receives)
    bool team1Receives = (dice.d6() >= 4);

    constexpr int TURNS_PER_HALF = 8;

    for (int half = 0; half < 2; ++half) {
        // Who receives this half?
        TeamState& offense = team1Receives ? team1 : team2;
        TeamState& defense = team1Receives ? team2 : team1;

        // Kick off
        setupKickoff(offense, defense, dice);

        // 8 pairs of turns (each team gets 8 turns per half)
        for (int t = 0; t < TURNS_PER_HALF; ++t) {
            // Team1 offense turn (if team1 is on offense)
            if (&offense == &team1) {
                // turnsLeft = remaining turns for offense
                int turnsLeft = TURNS_PER_HALF - t;
                bool td = simulateTurn(team1, team2, dice, turnsLeft);
                if (td) {
                    // Reset with defense now receiving if turns remain
                    if (t < TURNS_PER_HALF - 1) {
                        team1Receives = !team1Receives;
                        TeamState& newOff = team1Receives ? team1 : team2;
                        TeamState& newDef = team1Receives ? team2 : team1;
                        setupKickoff(newOff, newDef, dice);
                    }
                }
            } else {
                int turnsLeft = TURNS_PER_HALF - t;
                bool td = simulateTurn(team2, team1, dice, turnsLeft);
                if (td) {
                    if (t < TURNS_PER_HALF - 1) {
                        team1Receives = !team1Receives;
                        TeamState& newOff = team1Receives ? team1 : team2;
                        TeamState& newDef = team1Receives ? team2 : team1;
                        setupKickoff(newOff, newDef, dice);
                    }
                }
            }

            // Defense (team2 if team1 offense) also gets its turn
            {
                // The "defense" team also takes a turn (they're on offense simultaneously
                // in BB each team takes turns). Simplify: same loop, swap roles.
                TeamState& off2 = team1Receives ? team2 : team1;
                TeamState& def2 = team1Receives ? team1 : team2;
                if (&off2 != &offense) {
                    int turnsLeft = TURNS_PER_HALF - t;
                    bool td = simulateTurn(off2, def2, dice, turnsLeft);
                    if (td) {
                        if (t < TURNS_PER_HALF - 1) {
                            team1Receives = !team1Receives;
                            TeamState& newOff = team1Receives ? team1 : team2;
                            TeamState& newDef = team1Receives ? team2 : team1;
                            setupKickoff(newOff, newDef, dice);
                        }
                    }
                }
            }
        }

        // Swap receiving team for second half
        team1Receives = !team1Receives;
    }

    GameResult r;
    r.score1 = team1.score;
    r.score2 = team2.score;
    r.casualties1 = team1.casualties;
    r.casualties2 = team2.casualties;
    r.ko1  = team1.knockouts;
    r.ko2  = team2.knockouts;
    r.blocks1 = team1.blocksSuccessful;
    r.blocks2 = team2.blocksSuccessful;
    r.passes1 = team1.passesCompleted;
    r.passes2 = team2.passesCompleted;
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
            score1 += r.score1;
            score2 += r.score2;
            cas1   += r.casualties1;
            cas2   += r.casualties2;
            ko1    += r.ko1;
            ko2    += r.ko2;
            blk1   += r.blocks1;
            blk2   += r.blocks2;
            pass1  += r.passes1;
            pass2  += r.passes2;
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
