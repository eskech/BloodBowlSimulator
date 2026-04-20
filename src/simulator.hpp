#pragma once
#include "models.hpp"
#include "dice.hpp"
#include <vector>

// ---------------------------------------------------------------------------
// Builds a TeamState from a TeamConfig + resolved stats
// ---------------------------------------------------------------------------
TeamState buildTeamState(const TeamConfig& cfg, const SeedData& seed);

// ---------------------------------------------------------------------------
// Simulate a single complete Blood Bowl game.
// ---------------------------------------------------------------------------
GameResult simulateGame(TeamState team1, TeamState team2, Dice& dice);

// ---------------------------------------------------------------------------
// Run N simulations in parallel using OpenMP and return aggregate statistics.
// Use when you only need aggregate numbers (no per-game CSV).
// numThreads=0 means use omp_get_max_threads().
// ---------------------------------------------------------------------------
SimulationStats runSimulations(const TeamConfig& cfg1, const TeamConfig& cfg2,
                               const SeedData& seed, int numSimulations,
                               int numThreads = 0);

// ---------------------------------------------------------------------------
// Run N individual games in parallel and return every GameResult.
// Use when per-game CSV output is needed; aggregate stats can be derived
// from the returned vector using aggregateResults().
// numThreads=0 means use omp_get_max_threads().
// ---------------------------------------------------------------------------
std::vector<GameResult> collectSamples(const TeamConfig& cfg1, const TeamConfig& cfg2,
                                        const SeedData& seed, int numSamples,
                                        int numThreads = 0);

// ---------------------------------------------------------------------------
// Fold a vector of GameResults into aggregate statistics.
// Lets collectSamples() replace runSimulations() so games aren't run twice.
// ---------------------------------------------------------------------------
SimulationStats aggregateResults(const std::vector<GameResult>& results);
