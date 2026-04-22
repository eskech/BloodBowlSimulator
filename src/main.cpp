#include "loader.hpp"
#include "models.hpp"
#include "simulator.hpp"
#include "tournament.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <numeric>
#include <omp.h>
#include <print>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

// ============================================================
// CLI argument bag
// ============================================================
struct Args {
    std::vector<std::string> matchPaths;                      // one or more match JSON files
    std::string              tournamentPath;                  // set when --tournament is used
    std::string              seedPath   = "bloodbowl-2025-seed.json";
    int                      runs       = 0;                  // 0 = no CSV output
    std::string              outputStem = "results";
};

static Args parseArgs(int argc, char* argv[]) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "-s" || arg == "--seed") {
            if (++i < argc) a.seedPath = argv[i];
        } else if (arg == "-t" || arg == "--tournament") {
            if (++i < argc) a.tournamentPath = argv[i];
        } else if (arg == "-r" || arg == "--runs") {
            if (++i < argc) a.runs = std::stoi(argv[i]);
        } else if (arg == "-o" || arg == "--output") {
            if (++i < argc) a.outputStem = argv[i];
        } else if (arg == "-h" || arg == "--help") {
            std::println("Usage: bloodbowl <match.json> [match2.json ...] [options]");
            std::println("       bloodbowl --tournament <tournament.json> [options]");
            std::println("");
            std::println("  -s, --seed FILE        Seed data file (default: bloodbowl-2025-seed.json)");
            std::println("  -t, --tournament FILE  Run Swiss tournament from FILE");
            std::println("  -r, --runs N           Collect N game samples → CSV (single matchup only)");
            std::println("  -o, --output STEM      Output file stem (default: results)");
            std::exit(EXIT_SUCCESS);
        } else if (arg.starts_with('-')) {
            std::println(std::cerr, "Unknown option: {}", arg);
            std::exit(EXIT_FAILURE);
        } else {
            a.matchPaths.emplace_back(arg);
        }
    }
    if (a.matchPaths.empty() && a.tournamentPath.empty())
        a.matchPaths.emplace_back("example_match.json");
    return a;
}

// ============================================================
// Console output helpers
// ============================================================
static void printBanner() {
    std::println("╔══════════════════════════════════════════════════════╗");
    std::println("║         Blood Bowl 2025 Simulator  v0.1              ║");
    std::println("╚══════════════════════════════════════════════════════╝");
    std::println("");
}

static void printTeamRoster(const TeamConfig& cfg, const SeedData& seed) {
    const Race* race = seed.findRace(cfg.race);
    std::println("  Team : {}", cfg.name);
    std::println("  Race : {} (Tier {})", cfg.race, race ? race->tier : 0);
    std::println("  Re-rolls: {}  Apothecary: {}", cfg.rerolls,
                 cfg.hasApothecary ? "Yes" : "No");

    const auto& ds = cfg.defaultStrategy;
    std::println("  Default strategy: wrestle={:.0f}%  standFirm={:.0f}%  "
                 "divingTackle={:.0f}%  pro={:.0f}%",
                 ds.wrestle * 100.f, ds.standFirm * 100.f,
                 ds.divingTackle * 100.f, ds.pro * 100.f);

    std::println("  Players ({}):", cfg.players.size() + cfg.starPlayers.size());
    for (const auto& p : cfg.players) {
        const RosterPosition* pos = race ? race->findPosition(p.position) : nullptr;
        std::string skills;
        if (pos) for (const auto& s : pos->startingSkills) skills += s + ", ";
        for (const auto& s : p.extraSkills) skills += "[" + s + "], ";
        if (p.isTeamCaptain) skills += "[Pro (C)], ";
        if (skills.size() >= 2) skills.resize(skills.size() - 2);

        if (pos) {
            std::println("    {:32s}  MA:{} ST:{} AG:{} AV:{:2d}  {}",
                         std::format("{}{} ({})", p.isTeamCaptain ? "★ " : "",
                                     p.name, p.position),
                         pos->ma, pos->st, pos->ag, pos->av, skills);
        } else {
            std::println("    {} ({})", p.name, p.position);
        }

        PlayerStrategy eff = p.strategy.mergedWith(cfg.defaultStrategy);
        bool hasOverride =
            (p.strategy.wrestle      >= 0.f && p.strategy.wrestle      != ds.wrestle)      ||
            (p.strategy.standFirm    >= 0.f && p.strategy.standFirm    != ds.standFirm)    ||
            (p.strategy.divingTackle >= 0.f && p.strategy.divingTackle != ds.divingTackle) ||
            (p.strategy.pro          >= 0.f && p.strategy.pro          != ds.pro);
        if (hasOverride) {
            std::println("      └ strategy: wrestle={:.0f}%  standFirm={:.0f}%  "
                         "divingTackle={:.0f}%  pro={:.0f}%",
                         eff.wrestle * 100.f, eff.standFirm * 100.f,
                         eff.divingTackle * 100.f, eff.pro * 100.f);
        }
    }
    for (const auto& spName : cfg.starPlayers) {
        const StarPlayer* sp = seed.findStarPlayer(spName);
        if (!sp) continue;
        std::string skills;
        for (const auto& s : sp->skills) skills += s + ", ";
        if (skills.size() >= 2) skills.resize(skills.size() - 2);
        std::println("    ★ {:30s}  MA:{} ST:{} AG:{} AV:{:2d}  {}",
                     std::format("{} (Star Player)", sp->name),
                     sp->ma, sp->st, sp->ag, sp->av, skills);
    }
    std::println("");
}

static void printAggregateStats(const SimulationStats& s,
                                 const std::string& name1, const std::string& name2)
{
    double n = static_cast<double>(s.totalGames);
    auto pct = [&](int v)      { return 100.0 * v / n; };
    auto avg = [&](long long v){ return v / n; };

    std::println("╔══════════════════════════════════════════════════════╗");
    std::println("║                 Simulation Results                   ║");
    std::println("╠══════════════════════════════════════════════════════╣");
    std::println("║  Games simulated : {:>6}                             ║", s.totalGames);
    std::println("╠══════════════════════════╦══════════════╦════════════╣");
    std::println("║  Metric                  ║  {:^12s}║  {:^10s}║",
                 name1.substr(0, 12), name2.substr(0, 10));
    std::println("╠══════════════════════════╬══════════════╬════════════╣");
    std::println("║  Wins                    ║  {:>8.1f} %  ║  {:>6.1f} %  ║", pct(s.wins1), pct(s.wins2));
    std::println("║  Draws                   ║  {:>8.1f} %  ║            ║", pct(s.draws));
    std::println("║  Avg touchdowns          ║  {:>10.3f}  ║  {:>8.3f}  ║", avg(s.totalScore1), avg(s.totalScore2));
    std::println("║  Avg casualties caused   ║  {:>10.3f}  ║  {:>8.3f}  ║", avg(s.totalCasualties1), avg(s.totalCasualties2));
    std::println("║  Avg KOs caused          ║  {:>10.3f}  ║  {:>8.3f}  ║", avg(s.totalKO1), avg(s.totalKO2));
    std::println("║  Avg blocks (success)    ║  {:>10.3f}  ║  {:>8.3f}  ║", avg(s.totalBlocks1), avg(s.totalBlocks2));
    std::println("║  Avg completions         ║  {:>10.3f}  ║  {:>8.3f}  ║", avg(s.totalPasses1), avg(s.totalPasses2));
    std::println("╚══════════════════════════╩══════════════╩════════════╝");
    std::println("");
    if (s.wins1 > s.wins2)
        std::println("  ► {} is favoured ({:.1f}% win rate)", name1, pct(s.wins1));
    else if (s.wins2 > s.wins1)
        std::println("  ► {} is favoured ({:.1f}% win rate)", name2, pct(s.wins2));
    else
        std::println("  ► Match-up is evenly balanced");
    std::println("");
}

// ============================================================
// CSV helpers
// ============================================================

// Escape a string for CSV (wrap in quotes if it contains commas/quotes/newlines)
static std::string csvEscape(const std::string& s) {
    if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) { if (c == '"') out += '"'; out += c; }
    out += '"';
    return out;
}

// Describe game result as "team1_win" / "team2_win" / "draw"
static std::string_view resultLabel(const GameResult& r) {
    if (r.score1 > r.score2) return "team1_win";
    if (r.score2 > r.score1) return "team2_win";
    return "draw";
}

// ---- Per-column descriptor used to build both CSVs -------------------------
struct ColDef {
    std::string header;
    std::function<double(const GameResult&)> value;
};

static std::vector<ColDef> makeNumericCols(const std::string& n1, const std::string& n2) {
    using Fn = std::function<double(const GameResult&)>;
    std::vector<ColDef> cols;
    auto add = [&](std::string h, Fn fn) {
        cols.push_back({std::move(h), std::move(fn)});
    };
    add(std::format("score_{}", n1),       Fn([](const GameResult& r){ return (double)r.score1; }));
    add(std::format("score_{}", n2),       Fn([](const GameResult& r){ return (double)r.score2; }));
    add("score_diff",                      Fn([](const GameResult& r){ return (double)(r.score1 - r.score2); }));
    add(std::format("casualties_{}", n1),  Fn([](const GameResult& r){ return (double)r.casualties1; }));
    add(std::format("casualties_{}", n2),  Fn([](const GameResult& r){ return (double)r.casualties2; }));
    add(std::format("ko_{}", n1),          Fn([](const GameResult& r){ return (double)r.ko1; }));
    add(std::format("ko_{}", n2),          Fn([](const GameResult& r){ return (double)r.ko2; }));
    add(std::format("blocks_{}", n1),      Fn([](const GameResult& r){ return (double)r.blocks1; }));
    add(std::format("blocks_{}", n2),      Fn([](const GameResult& r){ return (double)r.blocks2; }));
    add(std::format("completions_{}", n1), Fn([](const GameResult& r){ return (double)r.passes1; }));
    add(std::format("completions_{}", n2), Fn([](const GameResult& r){ return (double)r.passes2; }));
    return cols;
}

// ---- Descriptive stats over a vector<double> --------------------------------
struct DescStats {
    double mean{}, stddev{}, min_{}, p25{}, median{}, p75{}, max_{};
};

static DescStats computeDescStats(std::vector<double> vals) {
    if (vals.empty()) return {};
    std::ranges::sort(vals);
    const size_t n = vals.size();
    double sum = std::accumulate(vals.begin(), vals.end(), 0.0);
    double mean = sum / static_cast<double>(n);
    double sq = 0.0;
    for (double v : vals) sq += (v - mean) * (v - mean);
    double sd = (n > 1) ? std::sqrt(sq / static_cast<double>(n - 1)) : 0.0;

    auto percentile = [&](double p) -> double {
        double idx = p * static_cast<double>(n - 1);
        size_t lo  = static_cast<size_t>(idx);
        double frac = idx - static_cast<double>(lo);
        if (lo + 1 < n) return vals[lo] + frac * (vals[lo + 1] - vals[lo]);
        return vals[lo];
    };

    return { mean, sd, vals.front(), percentile(0.25), percentile(0.50),
             percentile(0.75), vals.back() };
}

// ============================================================
// Write _games.csv — one row per simulated game
// ============================================================
static bool writeGamesCsv(const std::vector<GameResult>& results,
                           const std::string& name1, const std::string& name2,
                           const std::string& path)
{
    std::ofstream f(path);
    if (!f) { std::println(std::cerr, "Cannot write: {}", path); return false; }

    auto cols = makeNumericCols(name1, name2);

    // Header
    f << "game,result";
    for (const auto& c : cols) f << ',' << csvEscape(c.header);
    f << '\n';

    // Rows
    int game = 1;
    for (const auto& r : results) {
        f << game++ << ',' << resultLabel(r);
        for (const auto& c : cols)
            f << ',' << static_cast<int>(c.value(r));
        f << '\n';
    }

    return true;
}

// ============================================================
// Write _summary.csv — descriptive stats for each numeric column
// ============================================================
static bool writeSummaryCsv(const std::vector<GameResult>& results,
                              const std::string& name1, const std::string& name2,
                              const std::string& path)
{
    std::ofstream f(path);
    if (!f) { std::println(std::cerr, "Cannot write: {}", path); return false; }

    auto cols = makeNumericCols(name1, name2);

    // Header
    f << "stat,mean,stddev,min,p25,median,p75,max\n";

    // Win / draw rates (special non-numeric columns)
    int wins1 = 0, wins2 = 0, draws = 0;
    for (const auto& r : results) {
        if (r.score1 > r.score2) ++wins1;
        else if (r.score2 > r.score1) ++wins2;
        else ++draws;
    }
    double n = static_cast<double>(results.size());
    auto pct = [&](int v){ return 100.0 * v / n; };

    f << std::format("win_rate_{},", name1)
      << std::format("{:.4f},{:.4f},{:.4f},{:.4f},{:.4f},{:.4f},{:.4f}\n",
                     pct(wins1), 0.0, 0.0, 0.0, 0.0, 0.0, 100.0);
    f << std::format("win_rate_{},", name2)
      << std::format("{:.4f},{:.4f},{:.4f},{:.4f},{:.4f},{:.4f},{:.4f}\n",
                     pct(wins2), 0.0, 0.0, 0.0, 0.0, 0.0, 100.0);
    f << std::format("draw_rate,{:.4f},{:.4f},{:.4f},{:.4f},{:.4f},{:.4f},{:.4f}\n",
                     pct(draws), 0.0, 0.0, 0.0, 0.0, 0.0, 100.0);

    // Numeric columns
    for (const auto& col : cols) {
        std::vector<double> vals;
        vals.reserve(results.size());
        for (const auto& r : results) vals.push_back(col.value(r));
        auto s = computeDescStats(std::move(vals));
        f << csvEscape(col.header) << ','
          << std::format("{:.4f},{:.4f},{:.4f},{:.4f},{:.4f},{:.4f},{:.4f}\n",
                         s.mean, s.stddev, s.min_, s.p25, s.median, s.p75, s.max_);
    }

    return true;
}


// ============================================================
// Tournament output
// ============================================================
static void printTournamentStats(const TournamentStats& ts) {
    const double dT = static_cast<double>(ts.numTournaments);

    std::println("╔══════════════════════════════════════════════════════════════════════╗");
    std::println("║                     Tournament Results                               ║");
    std::println("╠══════════════════════════════════════════════════════════════════════╣");
    std::println("║  Tournaments run : {:>6}   Rounds: {}   Match games: {}   System: {:7s}  ║",
                 ts.numTournaments, ts.numRounds, ts.matchGames,
                 ts.pairingSystem);
    std::println("╠══════════════════════════════════════════════════════════════════════╣");
    std::println("║  {:^20s}  {:>6s}  {:>8s}  {:>7s}  {:>8s}  {:>12s}  ║",
                 "Team", "Win%", "Avg Pts", "Net TD", "Net Cas", "W / D / L");
    std::println("╠══════════════════════════════════════════════════════════════════════╣");

    // Build sorted rank order (tournament win% DESC, then avg pts DESC)
    int n = ts.numTeams;
    std::vector<int> rank(static_cast<size_t>(n));
    std::iota(rank.begin(), rank.end(), 0);
    std::stable_sort(rank.begin(), rank.end(), [&](int a, int b) {
        if (ts.tournamentWins[a] != ts.tournamentWins[b])
            return ts.tournamentWins[a] > ts.tournamentWins[b];
        return ts.avgPoints[a] > ts.avgPoints[b];
    });

    for (int r = 0; r < n; ++r) {
        int i = rank[r];
        double winPct = 100.0 * ts.tournamentWins[i] / dT;
        std::println("║  {:^20s}  {:>5.1f}%  {:>8.2f}  {:>+7.2f}  {:>+8.2f}  {:>4.1f}/{:.1f}/{:.1f}  ║",
                     ts.teamNames[i].substr(0, 20),
                     winPct,
                     ts.avgPoints[i],
                     ts.avgNetScore[i],
                     ts.avgNetCasualties[i],
                     ts.avgWins[i], ts.avgDraws[i], ts.avgLosses[i]);
    }
    std::println("╚══════════════════════════════════════════════════════════════════════╝");
    std::println("");
    std::println("  ► {} wins the most tournaments ({:.1f}%)",
                 ts.teamNames[rank[0]],
                 100.0 * ts.tournamentWins[rank[0]] / dT);
    std::println("");
}

// ============================================================
// main
// ============================================================
int main(int argc, char* argv[]) {
    printBanner();
    Args args = parseArgs(argc, argv);

    const int totalThreads  = omp_get_max_threads();
    const int numMatchups   = static_cast<int>(args.matchPaths.size());
    // Each matchup gets a fair share of threads; at least 1.
    // Guard: numMatchups may be 0 in tournament mode.
    const int threadsPer    = numMatchups > 0 ? std::max(1, totalThreads / numMatchups) : totalThreads;
    // Outer parallel workers = min(matchups, total threads).
    const int outerThreads  = numMatchups > 0 ? std::min(numMatchups, totalThreads) : 0;
    // Allow nested OpenMP so inner runSimulations can use threadsPer threads
    // while the outer loop is also running in parallel.
    omp_set_max_active_levels(2);

    std::println("  Seed file  : {}", args.seedPath);
    if (args.tournamentPath.empty()) {
        std::println("  Matchups   : {}", numMatchups);
        std::println("  Threads    : {}  ({} outer × {} inner)",
                     totalThreads, outerThreads, threadsPer);
        if (args.runs > 0 && numMatchups == 1)
            std::println("  Sample runs: {}  →  {}_games.csv  {}_summary.csv",
                         args.runs, args.outputStem, args.outputStem);
    } else {
        std::println("  Tournament : {}", args.tournamentPath);
        std::println("  Threads    : {}", totalThreads);
    }
    std::println("");

    // ---- Load seed ----
    auto seedResult = loadSeedData(args.seedPath);
    if (!seedResult) {
        std::println(std::cerr, "Error loading seed: {}", seedResult.error());
        return EXIT_FAILURE;
    }
    const SeedData& seed = *seedResult;
    std::println("  Loaded {} races, {} skills from seed.\n",
                 seed.races.size(), seed.skillNames.size());

    // ---- Tournament mode ----
    if (!args.tournamentPath.empty()) {
        auto tResult = loadTournamentConfig(args.tournamentPath, seed);
        if (!tResult) {
            std::println(std::cerr, "Error loading tournament: {}", tResult.error());
            return EXIT_FAILURE;
        }
        const TournamentConfig& tc = *tResult;
        std::println("  Teams ({}):", tc.teams.size());
        for (const auto& t : tc.teams)
            std::println("    - {} ({})", t.name, t.race);
        std::println("");

        std::println("Running {} Swiss tournament(s) — {} rounds, {} game(s)/match, {} pairing ...",
                     tc.numTournaments, tc.numRounds, tc.matchGames, tc.pairingSystem);

        auto t0 = std::chrono::high_resolution_clock::now();
        TournamentStats ts = runTournament(tc, seed, totalThreads);
        auto t1 = std::chrono::high_resolution_clock::now();

        double elapsed = std::chrono::duration<double>(t1 - t0).count();
        std::println("  Completed in {:.3f} s  ({} games simulated, {:.0f} games/s)\n",
                     elapsed, ts.gamesSimulated,
                     ts.gamesSimulated / elapsed);

        printTournamentStats(ts);
        return EXIT_SUCCESS;
    }

    // ---- Load all match configs ----
    std::vector<MatchConfig> matches;
    matches.reserve(static_cast<size_t>(numMatchups));
    for (const auto& path : args.matchPaths) {
        auto result = loadMatchConfig(path, seed);
        if (!result) {
            std::println(std::cerr, "Error loading '{}': {}", path, result.error());
            return EXIT_FAILURE;
        }
        matches.push_back(std::move(*result));
    }

    // ---- Print rosters (single matchup: full; multiple: one-line each) ----
    auto sanitise = [](std::string s) {
        std::ranges::replace(s, ' ', '_');
        return s;
    };

    if (numMatchups == 1) {
        std::println("─────────────────────────── Team 1 ───────────────────────────");
        printTeamRoster(matches[0].team1, seed);
        std::println("─────────────────────────── Team 2 ───────────────────────────");
        printTeamRoster(matches[0].team2, seed);
    } else {
        std::println("  Matchups:");
        for (int i = 0; i < numMatchups; ++i)
            std::println("    [{:2d}]  {}  vs  {}  ({} games)  —  {}",
                         i + 1,
                         matches[i].team1.name, matches[i].team2.name,
                         matches[i].simulations,
                         args.matchPaths[static_cast<size_t>(i)]);
        std::println("");
    }

    auto t0 = std::chrono::high_resolution_clock::now();

    // ---- Single matchup + CSV sample mode ----
    if (numMatchups == 1 && args.runs > 0) {
        std::println("Simulating {} games...", args.runs);

        std::vector<GameResult> samples = collectSamples(
            matches[0].team1, matches[0].team2, seed, args.runs, threadsPer);

        auto t1 = std::chrono::high_resolution_clock::now();
        double elapsed = std::chrono::duration<double>(t1 - t0).count();
        std::println("  Completed in {:.3f} s  ({} games simulated, {:.0f} games/s)\n",
                     elapsed, static_cast<long>(args.runs),
                     args.runs / elapsed);

        SimulationStats stats = aggregateResults(samples);
        printAggregateStats(stats, matches[0].team1.name, matches[0].team2.name);

        std::string n1 = sanitise(matches[0].team1.name);
        std::string n2 = sanitise(matches[0].team2.name);
        std::string gamesPath   = args.outputStem + "_games.csv";
        std::string summaryPath = args.outputStem + "_summary.csv";

        if (writeGamesCsv(samples, n1, n2, gamesPath))
            std::println("  Wrote {} ({} rows)", gamesPath, samples.size());
        if (writeSummaryCsv(samples, n1, n2, summaryPath))
            std::println("  Wrote {}", summaryPath);
        std::println("");
        return EXIT_SUCCESS;
    }

    // ---- One or more matchups — aggregate mode, parallel across matchups ----
    int totalGames = 0;
    for (const auto& m : matches) totalGames += m.simulations;
    std::println("Simulating {} games across {} matchup(s)...", totalGames, numMatchups);

    std::vector<SimulationStats> allStats(static_cast<size_t>(numMatchups));

#pragma omp parallel for num_threads(outerThreads) schedule(dynamic, 1)
    for (int i = 0; i < numMatchups; ++i) {
        allStats[static_cast<size_t>(i)] = runSimulations(
            matches[static_cast<size_t>(i)].team1,
            matches[static_cast<size_t>(i)].team2,
            seed,
            matches[static_cast<size_t>(i)].simulations,
            threadsPer);
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double>(t1 - t0).count();
    std::println("  Completed in {:.3f} s  ({} games simulated, {:.0f} games/s)\n",
                 elapsed, static_cast<long>(totalGames),
                 totalGames / elapsed);

    // ---- Print results in order ----
    for (int i = 0; i < numMatchups; ++i) {
        if (numMatchups > 1) {
            std::println("──────────────────────────────────────────────────────");
            std::println("  [{:2d}]  {} vs {}",
                         i + 1, matches[i].team1.name, matches[i].team2.name);
            std::println("──────────────────────────────────────────────────────");
        }
        printAggregateStats(allStats[static_cast<size_t>(i)],
                            matches[static_cast<size_t>(i)].team1.name,
                            matches[static_cast<size_t>(i)].team2.name);
    }

    return EXIT_SUCCESS;
}
