// tourplay_scraper — fetches Blood Bowl rosters from TourPlay and generates
// a tournament JSON file for the BloodBowl simulator.
//
// Usage:
//   tourplay_scraper --url <tourplay-url> --token <bearer-token>
//                    [--seed bloodbowl-2025-seed.json]
//                    [--output tournament.json]
//                    [--rounds 4] [--tournaments 10000]
//                    [--verbose]
//
// Bearer token: open browser DevTools → Network tab → any authenticated
// API request → copy the value of the Authorization header (without "Bearer ").
//
// TourPlay API (requires Bearer token + Origin: https://tourplay.net):
//   GET /api/inscriptions/{id}       → list of teams in the tournament
//   GET /api/rosters/{id}            → detailed roster for a team

#include <algorithm>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <print>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ─────────────────────────────────────────────────────────────────────────────
// HTTP layer
// ─────────────────────────────────────────────────────────────────────────────

static size_t curlWrite(void* ptr, size_t size, size_t nmemb, std::string* out) {
    out->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

struct Response { int status{}; std::string body; };

static Response httpGet(const std::string& url, const std::string& token, bool verbose = false) {
    CURL* curl = curl_easy_init();
    if (!curl) return {-1, "curl_easy_init failed"};

    Response resp;
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("Authorization: Bearer " + token).c_str());
    headers = curl_slist_append(headers, "Origin: https://tourplay.net");
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers,
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    if (verbose) std::println(std::cerr, "  GET {}", url);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        resp.status = -1;
        resp.body   = curl_easy_strerror(rc);
    } else {
        long code{};
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        resp.status = static_cast<int>(code);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return resp;
}

// ─────────────────────────────────────────────────────────────────────────────
// URL helpers
// ─────────────────────────────────────────────────────────────────────────────

// Extract tournament slug from a URL or return the string as-is if it's just the slug.
// e.g. https://tourplay.net/en/blood-bowl/punchbowl-2/players → punchbowl-2
static std::string extractTournamentId(const std::string& urlOrId) {
    if (urlOrId.find("tourplay.net") == std::string::npos) return urlOrId;

    std::istringstream ss(urlOrId);
    std::string segment, last, prev;
    while (std::getline(ss, segment, '/')) {
        if (!segment.empty() && segment != "en" && segment != "http:"
                             && segment != "https:" && segment != "tourplay.net"
                             && segment != "blood-bowl" && segment != "players"
                             && segment != "teams"      && segment != "standings"
                             && segment != "schedule")
        {
            prev = last;
            last = segment;
        }
    }
    // The slug follows "blood-bowl" in the path
    return last.empty() ? urlOrId : last;
}

// ─────────────────────────────────────────────────────────────────────────────
// Name normalisation
// ─────────────────────────────────────────────────────────────────────────────

static const std::unordered_map<std::string, std::string> RACE_MAP = {
    {"Human",                "Humans"},
    {"Humans",               "Humans"},
    {"Orc",                  "Orcs"},
    {"Orcs",                 "Orcs"},
    {"Dark Elf",             "Dark Elf"},
    {"Dark Elves",           "Dark Elf"},
    {"Dark Elven",           "Dark Elf"},
    {"Wood Elf",             "Wood Elves"},
    {"Wood Elves",           "Wood Elves"},
    {"High Elf",             "High Elves"},
    {"High Elves",           "High Elves"},
    {"Elf Union",            "Elf Union"},
    {"Pro Elf",              "Elf Union"},
    {"Pro Elves",            "Elf Union"},
    {"Dwarf",                "Dwarves"},
    {"Dwarves",              "Dwarves"},
    {"Chaos Dwarf",          "Chaos Dwarves"},
    {"Chaos Dwarves",        "Chaos Dwarves"},
    {"Chaos Dwarfs",         "Chaos Dwarves"},
    {"Skaven",               "Skaven"},
    {"Nurgle",               "Nurgle"},
    {"Nurgle's Rotters",     "Nurgle"},
    {"Undead",               "Shambling Undead"},
    {"Shambling Undead",     "Shambling Undead"},
    {"Necromantic",          "Necromantics"},
    {"Necromantic Horror",   "Necromantics"},
    {"Necromantics",         "Necromantics"},
    {"Chaos",                "Chaos Chosen"},
    {"Chaos Chosen",         "Chaos Chosen"},
    {"Halfling",             "Halflings"},
    {"Halflings",            "Halflings"},
    {"Goblin",               "Goblins"},
    {"Goblins",              "Goblins"},
    {"Snotling",             "Snotlings"},
    {"Snotlings",            "Snotlings"},
    {"Vampire",              "Vampires"},
    {"Vampires",             "Vampires"},
    {"Lizardman",            "Lizardmen"},
    {"Lizardmen",            "Lizardmen"},
    {"Amazon",               "Amazons"},
    {"Amazons",              "Amazons"},
    {"Norse",                "Norse"},
    {"Khorne",               "Khorne"},
    {"Chaos Renegade",       "Chaos Renegades"},
    {"Chaos Renegades",      "Chaos Renegades"},
    {"Bretonnian",           "Bretonnian"},
    {"Bretonnians",          "Bretonnian"},
    {"Old World Alliance",   "Old World Alliance"},
    {"Imperial Nobility",    "Imperial Nobility"},
    {"Ogre",                 "Ogre"},
    {"Ogres",                "Ogre"},
    {"Black Orc",            "Black Orcs"},
    {"Black Orcs",           "Black Orcs"},
    {"Underworld",           "Underworlds Denizens"},
    {"Underworld Denizens",  "Underworlds Denizens"},
    {"Underworlds Denizens", "Underworlds Denizens"},
    {"Slann",                "Slanns"},
    {"Slanns",               "Slanns"},
    {"Gnome",                "Gnomes"},
    {"Gnomes",               "Gnomes"},
    {"Tomb Kings",           "Tomb Kings"},
};

// Normalise a race name, returning the canonical seed name or the original if unknown.
static std::string normaliseRace(const std::string& raw) {
    // Exact match
    auto it = RACE_MAP.find(raw);
    if (it != RACE_MAP.end()) return it->second;
    // Case-insensitive scan
    std::string lower = raw;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    for (const auto& [k, v] : RACE_MAP) {
        std::string kl = k;
        std::transform(kl.begin(), kl.end(), kl.begin(), ::tolower);
        if (kl == lower) return v;
    }
    return raw;
}

// Normalise a skill name. TourPlay may append modifiers like "(+1)" or use
// BB2016 names. Strip trailing parenthetical modifiers and known aliases.
static std::string normaliseSkill(const std::string& raw) {
    // Strip trailing " (+N)" / "(+N)" modifiers
    std::string s = raw;
    auto paren = s.rfind('(');
    if (paren != std::string::npos) s = s.substr(0, paren);
    while (!s.empty() && s.back() == ' ') s.pop_back();

    // Common BB2016 → BB2020 aliases
    static const std::unordered_map<std::string, std::string> ALIASES = {
        {"Diving Catch",    "Diving Catch"},
        {"Diving Tackle",   "Diving Tackle"},
        {"Sure Hands",      "Sure Hands"},
        {"Sure Feet",       "Sure Feet"},
        {"Stand Firm",      "Stand Firm"},
        {"Break Tackle",    "Break Tackle"},
        {"Strip Ball",      "Strip Ball"},
        {"Safe Pair of Hands", "Safe Pair of Hands"},
    };
    auto it = ALIASES.find(s);
    return (it != ALIASES.end()) ? it->second : s;
}

// ─────────────────────────────────────────────────────────────────────────────
// Seed data — maps race → position → set of starting skills
// ─────────────────────────────────────────────────────────────────────────────

using SeedSkills = std::map<std::string, std::map<std::string, std::set<std::string>>>;

static SeedSkills loadSeed(const std::string& path) {
    SeedSkills result;
    std::ifstream f(path);
    if (!f) { std::println(std::cerr, "Warning: cannot open seed file '{}' — extra skill detection disabled", path); return result; }

    json j;
    try { f >> j; } catch (...) { std::println(std::cerr, "Warning: failed to parse seed file"); return result; }

    for (const auto& race : j.value("teamNames", json::array())) {
        std::string raceName = race.value("name", "");
        for (const auto& pos : race.value("rosterPositions", json::array())) {
            std::string posName = pos.value("name", "");
            std::set<std::string> skills;
            for (const auto& sk : pos.value("startingSkills", json::array()))
                skills.insert(sk.get<std::string>());
            result[raceName][posName] = std::move(skills);
        }
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// JSON field helpers — try multiple field names gracefully
// ─────────────────────────────────────────────────────────────────────────────

static std::string strField(const json& j, std::initializer_list<const char*> keys) {
    for (auto k : keys)
        if (j.contains(k) && j[k].is_string()) return j[k].get<std::string>();
    return "";
}

static int intField(const json& j, std::initializer_list<const char*> keys, int def = 0) {
    for (auto k : keys)
        if (j.contains(k) && j[k].is_number()) return j[k].get<int>();
    return def;
}

static bool boolField(const json& j, std::initializer_list<const char*> keys, bool def = false) {
    for (auto k : keys)
        if (j.contains(k) && j[k].is_boolean()) return j[k].get<bool>();
    return def;
}

// Drill into nested path: e.g. nestedStr(j, {"race","name"})
static std::string nestedStr(const json& j, std::initializer_list<const char*> path) {
    const json* cur = &j;
    for (auto k : path) {
        if (!cur->is_object() || !cur->contains(k)) return "";
        cur = &(*cur)[k];
    }
    return cur->is_string() ? cur->get<std::string>() : "";
}

static int nestedInt(const json& j, std::initializer_list<const char*> path, int def = 0) {
    const json* cur = &j;
    for (auto k : path) {
        if (!cur->is_object() || !cur->contains(k)) return def;
        cur = &(*cur)[k];
    }
    return cur->is_number() ? cur->get<int>() : def;
}

// ─────────────────────────────────────────────────────────────────────────────
// Parse a TourPlay roster JSON into a simulator team object
// ─────────────────────────────────────────────────────────────────────────────

static json buildTeam(const json& roster, const SeedSkills& seed, bool verbose) {
    // Team name & race
    std::string name = strField(roster, {"name", "teamName"});
    std::string raceRaw = nestedStr(roster, {"race", "name"});
    if (raceRaw.empty()) raceRaw = strField(roster, {"raceName", "race"});
    std::string race = normaliseRace(raceRaw);

    // Rerolls
    int rerolls = intField(roster, {"rerolls", "teamRerolls", "rerollCount"}, 2);

    // Apothecary
    bool hasApoth = boolField(roster, {"apothecary", "hasApothecary", "apothecaryStar"});

    // Riotous Rookies inducement
    bool riotousRookies = false;
    for (const auto& ind : roster.value("inducements", json::array())) {
        std::string indName = nestedStr(ind, {"name"});
        if (indName.empty()) indName = strField(ind, {"inducement", "name", "inducementName"});
        if (!indName.empty()) {
            std::string lower = indName;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (lower.find("riotous") != std::string::npos) {
                riotousRookies = true;
                if (verbose) std::println("    Inducement detected: {} → riotousRookies=true", indName);
            } else if (verbose) {
                std::println("    Inducement (unsupported, skipped): {}", indName);
            }
        }
    }

    // Players
    json players = json::array();

    // Locate the players array — TourPlay may use different field names
    const json* playerList = nullptr;
    for (const char* key : {"players", "lineup", "squad", "playerList"}) {
        if (roster.contains(key) && roster[key].is_array()) {
            playerList = &roster[key];
            break;
        }
    }

    if (playerList) {
        const auto& seedRace = [&]() -> const std::map<std::string, std::set<std::string>>* {
            auto it = seed.find(race);
            return (it != seed.end()) ? &it->second : nullptr;
        }();

        for (const auto& tp : *playerList) {
            // Skip dead / missing players
            if (boolField(tp, {"dead", "isDead", "fired", "isFired"})) continue;
            if (boolField(tp, {"missNextGame"}) && verbose)
                std::println("    Note: player misses next game (still included)");

            std::string pName = strField(tp, {"name", "playerName", "nickName"});
            if (pName.empty()) pName = "Unknown";

            std::string posRaw = nestedStr(tp, {"position", "name"});
            if (posRaw.empty()) posRaw = strField(tp, {"positionName", "position"});
            if (posRaw.empty()) posRaw = nestedStr(tp, {"positionMaster", "name"});

            // Collect all skills from TourPlay
            std::vector<std::string> allSkills;
            const json* skillsArr = nullptr;
            for (const char* k : {"skills", "skillList", "playerSkills"})
                if (tp.contains(k) && tp[k].is_array()) { skillsArr = &tp[k]; break; }

            if (skillsArr) {
                for (const auto& sk : *skillsArr) {
                    std::string skName;
                    if (sk.is_string()) skName = sk.get<std::string>();
                    else skName = nestedStr(sk, {"name"});
                    if (skName.empty()) skName = strField(sk, {"skill", "skillName"});
                    if (!skName.empty()) allSkills.push_back(normaliseSkill(skName));
                }
            }

            // Determine extra skills (beyond position starting skills)
            std::vector<std::string> extraSkills;
            if (seedRace && !posRaw.empty()) {
                auto posIt = seedRace->find(posRaw);
                if (posIt != seedRace->end()) {
                    const auto& startingSkills = posIt->second;
                    for (const auto& sk : allSkills)
                        if (startingSkills.find(sk) == startingSkills.end())
                            extraSkills.push_back(sk);
                } else {
                    // Position not found in seed — all skills treated as extra
                    extraSkills = allSkills;
                    if (verbose)
                        std::println("    Warning: position '{}' not found in seed for race '{}'", posRaw, race);
                }
            } else {
                // No seed data — include all skills as extra
                extraSkills = allSkills;
            }

            json player;
            player["position"] = posRaw;
            player["name"]     = pName;
            if (!extraSkills.empty()) {
                player["extraSkills"] = extraSkills;
            }
            players.push_back(std::move(player));

            if (verbose) {
                std::string skStr;
                for (const auto& s : extraSkills) skStr += s + " ";
                std::println("    Player: {} ({})  extras: [{}]", pName, posRaw, skStr);
            }
        }
    }

    json team;
    team["name"]         = name;
    team["race"]         = race;
    team["rerolls"]      = rerolls;
    team["hasApothecary"] = hasApoth;
    if (riotousRookies) team["riotousRookies"] = true;
    team["players"]      = std::move(players);
    return team;
}

// ─────────────────────────────────────────────────────────────────────────────
// Fetch all inscriptions and their roster IDs
// ─────────────────────────────────────────────────────────────────────────────

struct Inscription { std::string name; int rosterId{}; };

static std::vector<Inscription> fetchInscriptions(const std::string& tournId,
                                                    const std::string& token,
                                                    bool verbose)
{
    std::string url = "https://tourplay.net/api/inscriptions/" + tournId;
    auto resp = httpGet(url, token, verbose);

    if (resp.status != 200) {
        std::println(std::cerr, "Error fetching inscriptions (HTTP {}): {}", resp.status, resp.body);
        return {};
    }

    std::vector<Inscription> result;
    json j;
    try { j = json::parse(resp.body); } catch (...) {
        std::println(std::cerr, "Error parsing inscriptions JSON");
        return {};
    }

    // Handle wrapped response: {"data": [...]} or {"inscriptions": [...]} or bare array
    const json* arr = nullptr;
    if (j.is_array()) {
        arr = &j;
    } else {
        for (const char* key : {"data", "inscriptions", "items", "results"})
            if (j.contains(key) && j[key].is_array()) { arr = &j[key]; break; }
    }

    if (!arr) {
        std::println(std::cerr, "Could not locate inscriptions array in response.\nRaw: {:.200s}", resp.body);
        return {};
    }

    for (const auto& entry : *arr) {
        // Roster ID — try several paths
        int rid = intField(entry, {"rosterId", "roster_id"});
        if (rid == 0) rid = nestedInt(entry, {"roster", "id"});
        if (rid == 0) rid = nestedInt(entry, {"team", "id"});
        if (rid == 0) rid = intField(entry, {"id"});

        // Team name — try several paths
        std::string tname = nestedStr(entry, {"team", "name"});
        if (tname.empty()) tname = strField(entry, {"teamName", "name"});

        if (rid == 0) {
            if (verbose) std::println(std::cerr, "  Warning: could not find roster ID for inscription '{}'", tname);
            continue;
        }
        result.push_back({tname, rid});
        if (verbose) std::println("  Found team '{}' → roster #{}", tname, rid);
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Argument parsing
// ─────────────────────────────────────────────────────────────────────────────

struct Args {
    std::string url;
    std::string token;
    std::string seedPath   = "bloodbowl-2025-seed.json";
    std::string outputPath = "tournament_scraped.json";
    int rounds       = 4;
    int tournaments  = 10000;
    bool verbose     = false;
};

static void printUsage(const char* prog) {
    std::println("Usage: {} --url <tourplay-url-or-id> --token <bearer-token> [options]", prog);
    std::println("");
    std::println("Required:");
    std::println("  --url URL          TourPlay tournament URL or slug (e.g. punchbowl-2)");
    std::println("  --token TOKEN      Bearer token (copy from browser DevTools → Network →");
    std::println("                     any API request → Authorization header value)");
    std::println("");
    std::println("Options:");
    std::println("  --seed FILE        Seed data file (default: bloodbowl-2025-seed.json)");
    std::println("  --output FILE      Output JSON file (default: tournament_scraped.json)");
    std::println("  --rounds N         Swiss rounds (default: 4)");
    std::println("  --tournaments N    Simulation runs (default: 10000)");
    std::println("  --verbose          Print detailed scraping progress");
    std::println("  --help             Show this message");
    std::println("");
    std::println("Example:");
    std::println("  {} --url https://tourplay.net/en/blood-bowl/punchbowl-2 --token eyJ...", prog);
}

static std::optional<Args> parseArgs(int argc, char* argv[]) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (++i >= argc) { std::println(std::cerr, "Missing value for {}", arg); std::exit(1); }
            return argv[i];
        };
        if      (arg == "--url")         a.url        = next();
        else if (arg == "--token")       a.token      = next();
        else if (arg == "--seed")        a.seedPath   = next();
        else if (arg == "--output")      a.outputPath = next();
        else if (arg == "--rounds")      a.rounds     = std::stoi(next());
        else if (arg == "--tournaments") a.tournaments = std::stoi(next());
        else if (arg == "--verbose")     a.verbose    = true;
        else if (arg == "--help")        { printUsage(argv[0]); std::exit(0); }
        else { std::println(std::cerr, "Unknown argument: {}", arg); return std::nullopt; }
    }
    if (a.url.empty() || a.token.empty()) {
        printUsage(argv[0]);
        return std::nullopt;
    }
    return a;
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    auto argsOpt = parseArgs(argc, argv);
    if (!argsOpt) return 1;
    const auto& args = *argsOpt;

    curl_global_init(CURL_GLOBAL_DEFAULT);

    std::string tournId = extractTournamentId(args.url);
    std::println("Tournament ID : {}", tournId);
    std::println("Output file   : {}", args.outputPath);
    std::println("Seed file     : {}", args.seedPath);
    std::println("");

    // Load seed data for extra-skill detection
    SeedSkills seed = loadSeed(args.seedPath);
    if (!seed.empty()) std::println("Loaded seed data: {} races", seed.size());

    // Step 1: fetch inscription list
    std::println("Fetching inscription list ...");
    auto inscriptions = fetchInscriptions(tournId, args.token, args.verbose);
    if (inscriptions.empty()) {
        std::println(std::cerr, "No inscriptions found — check token and tournament ID.");
        curl_global_cleanup();
        return 1;
    }
    std::println("Found {} team(s).", inscriptions.size());

    // Step 2: fetch each roster
    json teams = json::array();
    int ok = 0, fail = 0;

    for (const auto& ins : inscriptions) {
        std::println("  Fetching roster #{} ({}) ...", ins.rosterId, ins.name);

        std::string rosterUrl = std::format("https://tourplay.net/api/rosters/{}", ins.rosterId);
        auto resp = httpGet(rosterUrl, args.token, args.verbose);

        if (resp.status != 200) {
            std::println(std::cerr, "    HTTP {} — skipping", resp.status);
            ++fail;
            continue;
        }

        json roster;
        try { roster = json::parse(resp.body); }
        catch (...) { std::println(std::cerr, "    JSON parse error — skipping"); ++fail; continue; }

        json team = buildTeam(roster, seed, args.verbose);

        if (team["players"].empty()) {
            std::println(std::cerr, "    Warning: no players parsed for '{}'", ins.name);
        }

        std::println("    {} ({}) — {} players, {} rerolls, apoth={}",
            team.value("name", ins.name),
            team.value("race", "?"),
            team["players"].size(),
            team.value("rerolls", 0),
            team.value("hasApothecary", false));

        teams.push_back(std::move(team));
        ++ok;
    }

    std::println("");
    std::println("Scraped: {} ok, {} failed", ok, fail);

    if (teams.empty()) {
        std::println(std::cerr, "No teams scraped — aborting.");
        curl_global_cleanup();
        return 1;
    }

    // Step 3: build tournament JSON
    json tournament;
    tournament["tournaments"]   = args.tournaments;
    tournament["rounds"]        = args.rounds;
    tournament["matchGames"]    = 1;
    tournament["pairingSystem"] = "monrad";
    tournament["teams"]         = std::move(teams);

    std::ofstream out(args.outputPath);
    if (!out) {
        std::println(std::cerr, "Cannot write to '{}'", args.outputPath);
        curl_global_cleanup();
        return 1;
    }
    out << tournament.dump(2) << "\n";
    std::println("Written to '{}'", args.outputPath);

    curl_global_cleanup();
    return 0;
}
