#include "loader.hpp"
#include <format>
#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Helper: parse a StrategyOverride from a JSON object (all fields optional).
// ---------------------------------------------------------------------------
static StrategyOverride parseStrategyOverride(const json& j) {
    StrategyOverride s;
    if (j.contains("wrestle"))      s.wrestle      = j["wrestle"].get<float>();
    if (j.contains("standFirm"))    s.standFirm    = j["standFirm"].get<float>();
    if (j.contains("divingTackle")) s.divingTackle = j["divingTackle"].get<float>();
    if (j.contains("pro"))          s.pro          = j["pro"].get<float>();
    return s;
}

// ---------------------------------------------------------------------------
// Helper: parse a full PlayerStrategy from a JSON object (all fields optional,
// uses struct defaults when absent).
// ---------------------------------------------------------------------------
static PlayerStrategy parseFullStrategy(const json& j) {
    PlayerStrategy s;  // starts with sensible defaults
    if (j.contains("wrestle"))      s.wrestle      = j["wrestle"].get<float>();
    if (j.contains("standFirm"))    s.standFirm    = j["standFirm"].get<float>();
    if (j.contains("divingTackle")) s.divingTackle = j["divingTackle"].get<float>();
    if (j.contains("pro"))          s.pro          = j["pro"].get<float>();
    return s;
}

// ---------------------------------------------------------------------------
// Load seed data
// ---------------------------------------------------------------------------
std::expected<SeedData, std::string> loadSeedData(std::string_view path) {
    std::ifstream f{std::string{path}};
    if (!f) return std::unexpected(std::format("Cannot open seed file: {}", path));

    json j;
    try { f >> j; }
    catch (const std::exception& e) {
        return std::unexpected(std::format("JSON parse error in seed: {}", e.what()));
    }

    SeedData seed;

    // ── Skills + category map ────────────────────────────────────────────────
    if (j.contains("skills")) {
        for (const auto& sk : j["skills"]) {
            std::string name = sk.value("name", "");
            std::string cat  = sk.value("category", "");
            if (!name.empty()) {
                seed.skillNames.push_back(name);
                seed.skillCategories[name] = cat;
            }
        }
    }

    // ── Races (teamNames) ────────────────────────────────────────────────────
    if (!j.contains("teamNames"))
        return std::unexpected("Seed JSON missing 'teamNames' key");

    for (const auto& rj : j["teamNames"]) {
        Race race;
        race.name              = rj.value("name", "");
        race.tier              = rj.value("tier", 1);
        race.rerollCost        = rj.value("rerollCost", 60);
        race.canHaveApothecary = rj.value("canHaveApothecary", false);

        if (rj.contains("rosterPositions")) {
            for (const auto& pj : rj["rosterPositions"]) {
                RosterPosition pos;
                pos.name     = pj.value("name", "");
                pos.cost     = pj.value("cost", 50000);
                pos.maxCount = pj.value("maxCount", 16);
                pos.ma       = pj.value("ma", 6);
                pos.st       = pj.value("st", 3);
                pos.ag       = pj.value("ag", 3);
                pos.pa       = pj["pa"].is_null() ? std::nullopt
                                                  : std::optional{pj.value("pa", 4)};
                pos.av       = pj.value("av", 8);
                pos.keywords = pj.value("keywords", "");

                auto loadSkillList = [](const json& arr, Skills& out) {
                    if (arr.is_array())
                        for (const auto& s : arr)
                            out.push_back(s.get<std::string>());
                };
                loadSkillList(pj.value("startingSkills",    json::array()), pos.startingSkills);
                loadSkillList(pj.value("normalSkillAccess", json::array()), pos.normalSkillAccess);
                loadSkillList(pj.value("doubleSkillAccess", json::array()), pos.doubleSkillAccess);

                race.positions.push_back(std::move(pos));
            }
        }
        seed.races.push_back(std::move(race));
    }

    return seed;
}

// ---------------------------------------------------------------------------
// Parse one team config
// ---------------------------------------------------------------------------
static std::expected<TeamConfig, std::string>
parseTeamConfig(const json& tj, const SeedData& seed) {
    TeamConfig cfg;
    cfg.name           = tj.value("name", "Unknown Team");
    cfg.race           = tj.value("race", "Humans");
    cfg.rerolls        = tj.value("rerolls", 2);
    cfg.hasApothecary  = tj.value("hasApothecary", false);
    cfg.riotousRookies = tj.value("riotousRookies", false);

    if (!seed.findRace(cfg.race))
        return std::unexpected(std::format("Race '{}' not found in seed data", cfg.race));

    // ── Team-wide strategy defaults ──────────────────────────────────────────
    if (tj.contains("strategy") && tj["strategy"].is_object())
        cfg.defaultStrategy = parseFullStrategy(tj["strategy"]);

    // ── Players ──────────────────────────────────────────────────────────────
    if (!tj.contains("players") || !tj["players"].is_array())
        return std::unexpected(std::format("Team '{}' missing 'players' array", cfg.name));

    const Race* race = seed.findRace(cfg.race);

    for (const auto& pj : tj["players"]) {
        PlayerConfig pc;
        pc.name     = pj.value("name", "Player");
        pc.position = pj.value("position", "");

        const RosterPosition* rpos = race ? race->findPosition(pc.position) : nullptr;
        if (!rpos)
            return std::unexpected(
                std::format("Position '{}' not found in race '{}'", pc.position, cfg.race));

        // ── Extra skills with validation ─────────────────────────────────────
        if (pj.contains("extraSkills") && pj["extraSkills"].is_array()) {
            for (const auto& sk : pj["extraSkills"]) {
                std::string skillName = sk.get<std::string>();

                // Validate against the roster position's allowed skill groups
                auto valid = seed.validateExtraSkill(*rpos, skillName);
                if (!valid)
                    return std::unexpected(
                        std::format("Player '{}' ({}): {}", pc.name, pc.position,
                                    valid.error()));

                pc.extraSkills.push_back(std::move(skillName));
            }
        }

        // ── Per-player strategy overrides ────────────────────────────────────
        if (pj.contains("strategy") && pj["strategy"].is_object())
            pc.strategy = parseStrategyOverride(pj["strategy"]);

        cfg.players.push_back(std::move(pc));
    }

    if (cfg.players.empty())
        return std::unexpected(std::format("Team '{}' has no players", cfg.name));

    return cfg;
}

// ---------------------------------------------------------------------------
// Load tournament configuration
// ---------------------------------------------------------------------------
std::expected<TournamentConfig, std::string> loadTournamentConfig(std::string_view path,
                                                                    const SeedData& seed)
{
    std::ifstream f{std::string{path}};
    if (!f) return std::unexpected(std::format("Cannot open tournament file: {}", path));

    json j;
    try { f >> j; }
    catch (const std::exception& e) {
        return std::unexpected(std::format("JSON parse error in tournament: {}", e.what()));
    }

    if (!j.contains("teams") || !j["teams"].is_array())
        return std::unexpected("Tournament JSON must have a 'teams' array");

    TournamentConfig cfg;
    cfg.numTournaments = j.value("tournaments",    1000);
    cfg.numRounds      = j.value("rounds",         5);
    cfg.matchGames     = j.value("matchGames",     1);
    cfg.pairingSystem  = j.value("pairingSystem",  std::string{"dutch"});

    if (cfg.pairingSystem != "dutch" && cfg.pairingSystem != "monrad")
        return std::unexpected(std::format("Unknown pairing system '{}' (use 'dutch' or 'monrad')",
                                            cfg.pairingSystem));

    for (const auto& tj : j["teams"]) {
        auto result = parseTeamConfig(tj, seed);
        if (!result) return std::unexpected(result.error());
        cfg.teams.push_back(std::move(*result));
    }

    if (cfg.teams.size() < 2)
        return std::unexpected("Tournament needs at least 2 teams");

    return cfg;
}

// ---------------------------------------------------------------------------
// Load match configuration
// ---------------------------------------------------------------------------
std::expected<MatchConfig, std::string> loadMatchConfig(std::string_view path,
                                                          const SeedData& seed) {
    std::ifstream f{std::string{path}};
    if (!f) return std::unexpected(std::format("Cannot open match file: {}", path));

    json j;
    try { f >> j; }
    catch (const std::exception& e) {
        return std::unexpected(std::format("JSON parse error in match: {}", e.what()));
    }

    if (!j.contains("team1") || !j.contains("team2"))
        return std::unexpected("Match JSON must have 'team1' and 'team2' objects");

    auto t1 = parseTeamConfig(j["team1"], seed);
    if (!t1) return std::unexpected(t1.error());

    auto t2 = parseTeamConfig(j["team2"], seed);
    if (!t2) return std::unexpected(t2.error());

    MatchConfig mc;
    mc.team1       = std::move(*t1);
    mc.team2       = std::move(*t2);
    mc.simulations = j.value("simulations", 10'000);
    return mc;
}
