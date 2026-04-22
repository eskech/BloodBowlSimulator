#include "tournament.hpp"
#include "simulator.hpp"
#include "dice.hpp"
#include <algorithm>
#include <numeric>
#include <omp.h>
#include <random>
#include <ranges>

// ---------------------------------------------------------------------------
// Pairwise match pool
// ---------------------------------------------------------------------------

// Number of game results stored per team-pair.  Larger → more accurate
// sampling distribution; smaller → faster precomputation.
// 512 gives ~4% sampling error (1/√512) which is negligible for tournament
// standings that already have inherent variance.
static constexpr int POOL_SIZE = 512;

// pool[i * nTeams + j] holds POOL_SIZE game results for team-i vs team-j
// (only populated for i < j; access is always normalised so i < j).
struct MatchPool {
    int nTeams{};
    std::vector<GameResult> data;  // size = nTeams * nTeams * POOL_SIZE

    const GameResult& get(int i, int j, int k) const {
        // Always access with i < j for consistent storage
        if (i > j) std::swap(i, j);
        return data[(static_cast<size_t>(i) * static_cast<size_t>(nTeams)
                     + static_cast<size_t>(j))
                    * static_cast<size_t>(POOL_SIZE)
                    + static_cast<size_t>(k)];
    }
    GameResult& at(int i, int j, int k) {
        if (i > j) std::swap(i, j);
        return data[(static_cast<size_t>(i) * static_cast<size_t>(nTeams)
                     + static_cast<size_t>(j))
                    * static_cast<size_t>(POOL_SIZE)
                    + static_cast<size_t>(k)];
    }
};

// Build pool by simulating POOL_SIZE games for every (i,j) pair in parallel.
// The work is distributed as a flat list of pair indices so OpenMP can
// schedule it evenly across threads.
static MatchPool buildMatchPool(const std::vector<TeamState>& baseStates,
                                 int numThreads)
{
    int n = static_cast<int>(baseStates.size());
    MatchPool pool;
    pool.nTeams = n;
    pool.data.resize(static_cast<size_t>(n) * static_cast<size_t>(n)
                     * static_cast<size_t>(POOL_SIZE));

    // Flat index over the upper triangle: pair p → (i, j) with i < j
    int numPairs = n * (n - 1) / 2;
    std::vector<std::pair<int,int>> pairs;
    pairs.reserve(static_cast<size_t>(numPairs));
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            pairs.push_back({i, j});

    std::vector<uint64_t> seeds(static_cast<size_t>(numThreads));
    {
        std::mt19937_64 seeder(std::random_device{}());
        for (auto& s : seeds) s = seeder();
    }

#pragma omp parallel for num_threads(numThreads) schedule(dynamic, 4)
    for (int p = 0; p < numPairs; ++p) {
        int tid = omp_get_thread_num();
        Dice dice(seeds[static_cast<size_t>(tid)]
                  ^ static_cast<uint64_t>(p) * 0xABCDEF01ULL);

        auto [i, j] = pairs[static_cast<size_t>(p)];
        for (int k = 0; k < POOL_SIZE; ++k)
            pool.at(i, j, k) = simulateGame(baseStates[i], baseStates[j], dice);
    }

    return pool;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool standingBetter(const TournamentStanding& a, const TournamentStanding& b) {
    if (a.points         != b.points)         return a.points         > b.points;
    if (a.netScore       != b.netScore)       return a.netScore       > b.netScore;
    if (a.netCasualties  != b.netCasualties)  return a.netCasualties  > b.netCasualties;
    return a.tiebreakRand < b.tiebreakRand;
}

struct MatchOutcome {
    int pts1{}, pts2{};
    int netScore1{};
    int netCas1{};
};

// Pick a uniform random index in [0, POOL_SIZE).
// POOL_SIZE == 512 == 8^3, so three d8 rolls give exact uniform coverage.
static int poolIndex(Dice& dice) {
    return (dice.d8() - 1) + (dice.d8() - 1) * 8 + (dice.d8() - 1) * 64;
}

// Sample matchGames results from the pool for the pair (idxA, idxB).
// Each game draw uses the same GameResult for all fields (score, casualties).
static MatchOutcome sampleMatch(const MatchPool& pool,
                                 int idxA, int idxB,
                                 int matchGames, Dice& dice)
{
    bool swapped = (idxA > idxB);

    int wins1 = 0, wins2 = 0, netScore = 0, netCas = 0;

    for (int g = 0; g < matchGames; ++g) {
        const GameResult& r = pool.get(idxA, idxB, poolIndex(dice));

        int s1 = swapped ? r.score2      : r.score1;
        int s2 = swapped ? r.score1      : r.score2;
        int c1 = swapped ? r.casualties2 : r.casualties1;
        int c2 = swapped ? r.casualties1 : r.casualties2;

        netScore += s1 - s2;
        netCas   += c1 - c2;
        if      (s1 > s2) ++wins1;
        else if (s2 > s1) ++wins2;
    }

    MatchOutcome out;
    if      (wins1 > wins2) { out.pts1 = 3; out.pts2 = 0; }
    else if (wins2 > wins1) { out.pts1 = 0; out.pts2 = 3; }
    else                    { out.pts1 = 1; out.pts2 = 1; }
    out.netScore1 = netScore;
    out.netCas1   = netCas;
    return out;
}

// ---------------------------------------------------------------------------
// Pairing engine
// ---------------------------------------------------------------------------

static void fixRematches(std::vector<std::pair<int,int>>& pairs,
                          const std::vector<TournamentStanding>& standings)
{
    for (int i = 0; i < static_cast<int>(pairs.size()) - 1; ++i) {
        auto [a, b] = pairs[i];
        if (!std::ranges::contains(standings[a].facedTeams, b)) continue;
        auto [c, d] = pairs[i + 1];
        if (!std::ranges::contains(standings[a].facedTeams, d) &&
            !std::ranges::contains(standings[c].facedTeams, b))
        {
            pairs[i]     = {a, d};
            pairs[i + 1] = {c, b};
        }
    }
}

static int assignBye(std::vector<int>& order,
                      const std::vector<TournamentStanding>& standings)
{
    for (int i = static_cast<int>(order.size()) - 1; i >= 0; --i) {
        if (!standings[order[i]].hadBye) {
            int byeIdx = order[i];
            order.erase(order.begin() + i);
            return byeIdx;
        }
    }
    int byeIdx = order.back();
    order.pop_back();
    return byeIdx;
}

static std::pair<std::vector<std::pair<int,int>>, int>
pairDutch(const std::vector<TournamentStanding>& standings)
{
    int n = static_cast<int>(standings.size());
    std::vector<int> order(static_cast<size_t>(n));
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(),
        [&](int a, int b) { return standingBetter(standings[a], standings[b]); });

    int byeIdx = -1;
    if (n % 2 == 1) byeIdx = assignBye(order, standings);

    int m = static_cast<int>(order.size()), half = m / 2;
    std::vector<std::pair<int,int>> pairs;
    pairs.reserve(static_cast<size_t>(half));
    for (int i = 0; i < half; ++i)
        pairs.push_back({order[i], order[half + i]});

    fixRematches(pairs, standings);
    return {pairs, byeIdx};
}

static std::pair<std::vector<std::pair<int,int>>, int>
pairMonrad(const std::vector<TournamentStanding>& standings)
{
    int n = static_cast<int>(standings.size());
    std::vector<int> order(static_cast<size_t>(n));
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(),
        [&](int a, int b) { return standingBetter(standings[a], standings[b]); });

    int byeIdx = -1;
    if (n % 2 == 1) byeIdx = assignBye(order, standings);

    int m = static_cast<int>(order.size());
    std::vector<std::pair<int,int>> pairs;
    pairs.reserve(static_cast<size_t>(m / 2));
    for (int i = 0; i < m - 1; i += 2)
        pairs.push_back({order[i], order[i + 1]});

    fixRematches(pairs, standings);
    return {pairs, byeIdx};
}

// ---------------------------------------------------------------------------
// Run one complete tournament instance (uses pre-built match pool).
// ---------------------------------------------------------------------------
static std::vector<TournamentStanding>
runOneTournament(const TournamentConfig& cfg,
                 const MatchPool& pool,
                 Dice& dice)
{
    int n = static_cast<int>(cfg.teams.size());

    std::vector<TournamentStanding> standings(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        standings[i].teamIdx      = i;
        standings[i].tiebreakRand = dice.d6() * 1000 + dice.d6() * 100
                                  + dice.d6() * 10   + dice.d6();
    }

    bool useDutch = (cfg.pairingSystem != "monrad");

    for (int round = 0; round < cfg.numRounds; ++round) {
        auto [pairs, byeIdx] = useDutch ? pairDutch(standings) : pairMonrad(standings);

        if (byeIdx >= 0) {
            standings[byeIdx].points   += 3;
            standings[byeIdx].wins     += 1;
            standings[byeIdx].netScore += 1;
            standings[byeIdx].hadBye    = true;
        }

        for (auto [idxA, idxB] : pairs) {
            MatchOutcome outcome = sampleMatch(pool, idxA, idxB, cfg.matchGames, dice);

            standings[idxA].points        += outcome.pts1;
            standings[idxB].points        += outcome.pts2;
            standings[idxA].netScore      += outcome.netScore1;
            standings[idxB].netScore      -= outcome.netScore1;
            standings[idxA].netCasualties += outcome.netCas1;
            standings[idxB].netCasualties -= outcome.netCas1;

            if      (outcome.pts1 > outcome.pts2) { ++standings[idxA].wins;  ++standings[idxB].losses; }
            else if (outcome.pts2 > outcome.pts1) { ++standings[idxB].wins;  ++standings[idxA].losses; }
            else                                  { ++standings[idxA].draws; ++standings[idxB].draws;  }

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

    std::vector<TeamState> baseStates;
    baseStates.reserve(static_cast<size_t>(n));
    for (const auto& tc : cfg.teams)
        baseStates.push_back(buildTeamState(tc, seed));

    // ── Phase 1: Build match pool in parallel ─────────────────────────────
    // All N*(N-1)/2 matchups are simulated POOL_SIZE times concurrently.
    // Tournament rounds then sample from this pool instead of re-running
    // simulateGame, so the tournament count no longer drives runtime.
    MatchPool pool = buildMatchPool(baseStates, numThreads);

    // ── Phase 2: Run tournament instances in parallel ─────────────────────
    std::vector<std::vector<int>>  wins  (static_cast<size_t>(numThreads), std::vector<int> (static_cast<size_t>(n), 0));
    std::vector<std::vector<long>> pts   (static_cast<size_t>(numThreads), std::vector<long>(static_cast<size_t>(n), 0));
    std::vector<std::vector<long>> netSc (static_cast<size_t>(numThreads), std::vector<long>(static_cast<size_t>(n), 0));
    std::vector<std::vector<long>> netCas(static_cast<size_t>(numThreads), std::vector<long>(static_cast<size_t>(n), 0));
    std::vector<std::vector<long>> winsR (static_cast<size_t>(numThreads), std::vector<long>(static_cast<size_t>(n), 0));
    std::vector<std::vector<long>> drawsR(static_cast<size_t>(numThreads), std::vector<long>(static_cast<size_t>(n), 0));
    std::vector<std::vector<long>> lossR (static_cast<size_t>(numThreads), std::vector<long>(static_cast<size_t>(n), 0));

    std::vector<uint64_t> seeds(static_cast<size_t>(numThreads));
    {
        std::mt19937_64 seeder(std::random_device{}());
        for (auto& s : seeds) s = seeder();
    }

#pragma omp parallel for num_threads(numThreads) schedule(static)
    for (int t = 0; t < nT; ++t) {
        int tid = omp_get_thread_num();
        Dice dice(seeds[static_cast<size_t>(tid)] ^ static_cast<uint64_t>(t) * 0xC0FFEE1Dull);

        auto standings = runOneTournament(cfg, pool, dice);

        auto best = std::ranges::max_element(standings, {},
            [](const TournamentStanding& s) {
                return std::tuple{s.points, s.netScore, s.netCasualties};
            });
        wins[static_cast<size_t>(tid)][static_cast<size_t>(best->teamIdx)]++;

        for (const auto& s : standings) {
            size_t i  = static_cast<size_t>(s.teamIdx);
            size_t tl = static_cast<size_t>(tid);
            pts   [tl][i] += s.points;
            netSc [tl][i] += s.netScore;
            netCas[tl][i] += s.netCasualties;
            winsR [tl][i] += s.wins;
            drawsR[tl][i] += s.draws;
            lossR [tl][i] += s.losses;
        }
    }

    // ── Reduce ────────────────────────────────────────────────────────────
    TournamentStats stats;
    stats.numTournaments = nT;
    stats.numRounds      = cfg.numRounds;
    stats.matchGames     = cfg.matchGames;
    stats.numTeams       = n;
    stats.pairingSystem  = cfg.pairingSystem;
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
