#include "tournament.hpp"
#include "simulator.hpp"
#include "dice.hpp"
#include <algorithm>
#include <numeric>
#include <omp.h>
#include <random>
#include <ranges>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// True if standing A is ranked higher than B.
static bool standingBetter(const TournamentStanding& a, const TournamentStanding& b) {
    if (a.points         != b.points)         return a.points         > b.points;
    if (a.netScore       != b.netScore)       return a.netScore       > b.netScore;
    if (a.netCasualties  != b.netCasualties)  return a.netCasualties  > b.netCasualties;
    return a.tiebreakRand < b.tiebreakRand;   // random for equal teams (ensures varied round-1)
}

// Result of simulating matchGames games between two pre-built TeamStates.
struct MatchOutcome {
    int pts1{}, pts2{};   // 3/0 win-loss, or 1/1 draw
    int netScore1{};      // sum(TDs1 - TDs2) across all games
    int netCas1{};        // sum(cas1 - cas2) across all games
};

static MatchOutcome simulateMatch(const TeamState& base1, const TeamState& base2,
                                   int matchGames, Dice& dice)
{
    int wins1 = 0, wins2 = 0;
    int totScore1 = 0, totScore2 = 0;
    int totCas1 = 0, totCas2 = 0;

    for (int g = 0; g < matchGames; ++g) {
        GameResult r = simulateGame(base1, base2, dice);
        totScore1 += r.score1;   totScore2 += r.score2;
        totCas1   += r.casualties1; totCas2 += r.casualties2;
        if      (r.score1 > r.score2) ++wins1;
        else if (r.score2 > r.score1) ++wins2;
        // equal score = draw game; not a win for either
    }

    MatchOutcome out;
    if      (wins1 > wins2) { out.pts1 = 3; out.pts2 = 0; }
    else if (wins2 > wins1) { out.pts1 = 0; out.pts2 = 3; }
    else                    { out.pts1 = 1; out.pts2 = 1; }
    out.netScore1 = totScore1 - totScore2;
    out.netCas1   = totCas1   - totCas2;
    return out;
}

// ---------------------------------------------------------------------------
// Pairing engine
// Returns (pairings, byeTeamIdx) where byeTeamIdx == -1 if no bye needed.
// standings is indexed by teamIdx (standings[i] always refers to team i).
// ---------------------------------------------------------------------------

// Try to fix rematches in a pairing list by swapping opponents.
// One-pass left-to-right: for pair i, if it is a rematch, try swapping the
// second element with the second element of pair i+1.
static void fixRematches(std::vector<std::pair<int,int>>& pairs,
                          const std::vector<TournamentStanding>& standings)
{
    for (int i = 0; i < static_cast<int>(pairs.size()) - 1; ++i) {
        auto [a, b] = pairs[i];
        if (!std::ranges::contains(standings[a].facedTeams, b)) continue;

        auto [c, d] = pairs[i + 1];
        // Try swap: a vs d, c vs b
        if (!std::ranges::contains(standings[a].facedTeams, d) &&
            !std::ranges::contains(standings[c].facedTeams, b))
        {
            pairs[i]     = {a, d};
            pairs[i + 1] = {c, b};
        }
        // else: can't fix this rematch without creating another — leave it
    }
}

// Assign bye to the lowest-ranked eligible team (prefers teams without a bye).
// Removes byeIdx from `order` in-place.
static int assignBye(std::vector<int>& order,
                      const std::vector<TournamentStanding>& standings)
{
    // Prefer lowest-ranked team that hasn't had a bye yet
    for (int i = static_cast<int>(order.size()) - 1; i >= 0; --i) {
        if (!standings[order[i]].hadBye) {
            int byeIdx = order[i];
            order.erase(order.begin() + i);
            return byeIdx;
        }
    }
    // All have had byes — give to lowest ranked
    int byeIdx = order.back();
    order.pop_back();
    return byeIdx;
}

// Dutch pairing: sort teams, split into top half / bottom half, pair across.
static std::pair<std::vector<std::pair<int,int>>, int>
pairDutch(const std::vector<TournamentStanding>& standings)
{
    int n = static_cast<int>(standings.size());
    std::vector<int> order(static_cast<size_t>(n));
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(),
        [&](int a, int b) { return standingBetter(standings[a], standings[b]); });

    int byeIdx = -1;
    if (n % 2 == 1)
        byeIdx = assignBye(order, standings);

    int m    = static_cast<int>(order.size());  // even
    int half = m / 2;

    std::vector<std::pair<int,int>> pairs;
    pairs.reserve(static_cast<size_t>(half));
    for (int i = 0; i < half; ++i)
        pairs.push_back({order[i], order[half + i]});

    fixRematches(pairs, standings);
    return {pairs, byeIdx};
}

// Monrad pairing: sort teams, pair consecutive (1st vs 2nd, 3rd vs 4th, …).
static std::pair<std::vector<std::pair<int,int>>, int>
pairMonrad(const std::vector<TournamentStanding>& standings)
{
    int n = static_cast<int>(standings.size());
    std::vector<int> order(static_cast<size_t>(n));
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(),
        [&](int a, int b) { return standingBetter(standings[a], standings[b]); });

    int byeIdx = -1;
    if (n % 2 == 1)
        byeIdx = assignBye(order, standings);

    int m = static_cast<int>(order.size());

    std::vector<std::pair<int,int>> pairs;
    pairs.reserve(static_cast<size_t>(m / 2));
    for (int i = 0; i < m - 1; i += 2)
        pairs.push_back({order[i], order[i + 1]});

    fixRematches(pairs, standings);
    return {pairs, byeIdx};
}

// ---------------------------------------------------------------------------
// Run one complete tournament instance.
// Returns final standings (indexed by teamIdx).
// ---------------------------------------------------------------------------
static std::vector<TournamentStanding>
runOneTournament(const TournamentConfig& cfg,
                 const std::vector<TeamState>& baseStates,
                 Dice& dice)
{
    int n = static_cast<int>(cfg.teams.size());

    // Initialise standings: one entry per team, indexed by teamIdx
    std::vector<TournamentStanding> standings(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        standings[i].teamIdx = i;
        // Random tiebreak ensures varied round-1 pairings across tournament runs
        standings[i].tiebreakRand = dice.d6() * 1000 + dice.d6() * 100
                                  + dice.d6() * 10   + dice.d6();
    }

    bool useDutch = (cfg.pairingSystem != "monrad");

    for (int round = 0; round < cfg.numRounds; ++round) {
        auto [pairs, byeIdx] = useDutch ? pairDutch(standings)
                                        : pairMonrad(standings);

        // Award bye
        if (byeIdx >= 0) {
            standings[byeIdx].points    += 3;
            standings[byeIdx].wins      += 1;
            standings[byeIdx].netScore  += 1;   // symbolic 1-0 result
            standings[byeIdx].hadBye     = true;
        }

        // Simulate all matches for this round
        for (auto [idxA, idxB] : pairs) {
            MatchOutcome outcome = simulateMatch(baseStates[idxA], baseStates[idxB],
                                                  cfg.matchGames, dice);

            standings[idxA].points        += outcome.pts1;
            standings[idxB].points        += outcome.pts2;
            standings[idxA].netScore      += outcome.netScore1;
            standings[idxB].netScore      -= outcome.netScore1;
            standings[idxA].netCasualties += outcome.netCas1;
            standings[idxB].netCasualties -= outcome.netCas1;

            if (outcome.pts1 > outcome.pts2) {
                ++standings[idxA].wins;   ++standings[idxB].losses;
            } else if (outcome.pts2 > outcome.pts1) {
                ++standings[idxB].wins;   ++standings[idxA].losses;
            } else {
                ++standings[idxA].draws;  ++standings[idxB].draws;
            }

            // Record that these two teams have met
            standings[idxA].facedTeams.push_back(idxB);
            standings[idxB].facedTeams.push_back(idxA);
        }
    }

    return standings;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
TournamentStats runTournament(const TournamentConfig& cfg, const SeedData& seed,
                               int numThreads)
{
    if (numThreads <= 0) numThreads = omp_get_max_threads();

    int n  = static_cast<int>(cfg.teams.size());
    int nT = cfg.numTournaments;

    // Build base TeamStates once — copied for each simulated game
    std::vector<TeamState> baseStates;
    baseStates.reserve(static_cast<size_t>(n));
    for (const auto& tc : cfg.teams)
        baseStates.push_back(buildTeamState(tc, seed));

    // Per-thread accumulators (avoids atomics / false sharing)
    std::vector<std::vector<int>>  wins  (static_cast<size_t>(numThreads), std::vector<int> (static_cast<size_t>(n), 0));
    std::vector<std::vector<long>> pts   (static_cast<size_t>(numThreads), std::vector<long>(static_cast<size_t>(n), 0));
    std::vector<std::vector<long>> netSc (static_cast<size_t>(numThreads), std::vector<long>(static_cast<size_t>(n), 0));
    std::vector<std::vector<long>> netCas(static_cast<size_t>(numThreads), std::vector<long>(static_cast<size_t>(n), 0));
    std::vector<std::vector<long>> winsR (static_cast<size_t>(numThreads), std::vector<long>(static_cast<size_t>(n), 0));
    std::vector<std::vector<long>> drawsR(static_cast<size_t>(numThreads), std::vector<long>(static_cast<size_t>(n), 0));
    std::vector<std::vector<long>> lossR (static_cast<size_t>(numThreads), std::vector<long>(static_cast<size_t>(n), 0));

    // Per-thread RNG seeds
    std::vector<uint64_t> seeds(static_cast<size_t>(numThreads));
    {
        std::mt19937_64 seeder(std::random_device{}());
        for (auto& s : seeds) s = seeder();
    }

#pragma omp parallel for num_threads(numThreads) schedule(dynamic, 16)
    for (int t = 0; t < nT; ++t) {
        int tid = omp_get_thread_num();
        Dice dice(seeds[static_cast<size_t>(tid)] ^ static_cast<uint64_t>(t) * 0xC0FFEE1Dull);

        auto standings = runOneTournament(cfg, baseStates, dice);

        // Determine winner (highest ranked team)
        auto best = std::ranges::max_element(standings, {},
            [](const TournamentStanding& s) {
                return std::tuple{s.points, s.netScore, s.netCasualties};
            });
        wins [static_cast<size_t>(tid)][static_cast<size_t>(best->teamIdx)]++;

        for (const auto& s : standings) {
            size_t i = static_cast<size_t>(s.teamIdx);
            size_t tl = static_cast<size_t>(tid);
            pts   [tl][i] += s.points;
            netSc [tl][i] += s.netScore;
            netCas[tl][i] += s.netCasualties;
            winsR [tl][i] += s.wins;
            drawsR[tl][i] += s.draws;
            lossR [tl][i] += s.losses;
        }
    }

    // Reduce across threads
    TournamentStats stats;
    stats.numTournaments  = nT;
    stats.numRounds       = cfg.numRounds;
    stats.matchGames      = cfg.matchGames;
    stats.numTeams        = n;
    stats.pairingSystem   = cfg.pairingSystem;
    stats.teamNames.resize(static_cast<size_t>(n));
    stats.tournamentWins      .assign(static_cast<size_t>(n), 0);
    stats.avgPoints           .assign(static_cast<size_t>(n), 0.0);
    stats.avgNetScore         .assign(static_cast<size_t>(n), 0.0);
    stats.avgNetCasualties    .assign(static_cast<size_t>(n), 0.0);
    stats.avgWins             .assign(static_cast<size_t>(n), 0.0);
    stats.avgDraws            .assign(static_cast<size_t>(n), 0.0);
    stats.avgLosses           .assign(static_cast<size_t>(n), 0.0);

    for (int i = 0; i < n; ++i)
        stats.teamNames[static_cast<size_t>(i)] = cfg.teams[static_cast<size_t>(i)].name;

    double dT = static_cast<double>(nT);
    for (int i = 0; i < n; ++i) {
        size_t si = static_cast<size_t>(i);
        long totPts = 0, totNS = 0, totNC = 0, totW = 0, totD = 0, totL = 0;
        for (int tl = 0; tl < numThreads; ++tl) {
            size_t stl = static_cast<size_t>(tl);
            stats.tournamentWins[si] += wins[stl][si];
            totPts += pts   [stl][si];
            totNS  += netSc [stl][si];
            totNC  += netCas[stl][si];
            totW   += winsR [stl][si];
            totD   += drawsR[stl][si];
            totL   += lossR [stl][si];
        }
        stats.avgPoints       [si] = totPts / dT;
        stats.avgNetScore     [si] = totNS  / dT;
        stats.avgNetCasualties[si] = totNC  / dT;
        stats.avgWins         [si] = totW   / dT;
        stats.avgDraws        [si] = totD   / dT;
        stats.avgLosses       [si] = totL   / dT;
    }

    return stats;
}
