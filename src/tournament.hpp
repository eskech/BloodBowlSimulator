#pragma once
#include "models.hpp"
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Per-team standing inside one tournament run
// ---------------------------------------------------------------------------
struct TournamentStanding {
    int  teamIdx{0};           // index into TournamentConfig::teams
    int  points{0};            // 3=win, 1=draw, 0=loss, 3=bye
    int  wins{0};
    int  draws{0};
    int  losses{0};
    int  netScore{0};          // TDs for minus TDs against (1st tiebreaker)
    int  netCasualties{0};     // casualties for minus casualties against (2nd tiebreaker)
    bool hadBye{false};
    int  tiebreakRand{0};      // random value assigned at start — ensures random round-1 pairings
    std::vector<int> facedTeams;  // teamIdx values of already-played opponents
};

// ---------------------------------------------------------------------------
// Aggregate statistics across all tournament runs
// ---------------------------------------------------------------------------
struct TournamentStats {
    int numTournaments{};
    int numRounds{};
    int matchGames{};
    int numTeams{};
    std::string pairingSystem;
    std::vector<std::string> teamNames;
    std::vector<int>    tournamentWins;       // times each team finished 1st
    std::vector<double> avgPoints;            // average points per tournament
    std::vector<double> avgNetScore;
    std::vector<double> avgNetCasualties;
    std::vector<double> avgWins;
    std::vector<double> avgDraws;
    std::vector<double> avgLosses;
};

// ---------------------------------------------------------------------------
// Run numTournaments Swiss tournaments and return aggregate stats.
// numThreads=0 → use omp_get_max_threads().
// ---------------------------------------------------------------------------
TournamentStats runTournament(const TournamentConfig& cfg, const SeedData& seed,
                               int numThreads = 0);
