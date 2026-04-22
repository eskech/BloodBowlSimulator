// Convert a TourPlay data zip file to a bloodbowl tournament JSON.
// Reads all files whose name contains "roster", parses the semicolon-separated
// CSV format exported by TourPlay, and writes a tournament JSON compatible
// with the bloodbowl simulator.
//
// Usage:
//   tourplay-convert <zipfile> [options]
//
// Options:
//   -s, --seed FILE        Seed data file          (default: bloodbowl-2025-seed.json)
//   -o, --output FILE      Output file             (default: tournament.json)
//   -r, --rounds N         Swiss rounds            (default: 6)
//   -t, --tournaments N    Number of simulations   (default: 1000)
//   -g, --games N          Games per match         (default: 1)
//   -p, --pairing SYSTEM   dutch | monrad          (default: dutch)

#include <miniz.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <print>
#include <ranges>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using json = nlohmann::json;

// ── String helpers ────────────────────────────────────────────────────────────

static std::string toLower(std::string s) {
    std::ranges::transform(s, s.begin(), ::tolower);
    return s;
}

// TourPlay sometimes encodes apostrophes as Unicode curly quotes (U+2018/U+2019).
// Normalize to straight ASCII apostrophe for reliable comparisons.
static std::string normalizeQuotes(std::string s) {
    // Replace UTF-8 encodings of U+2018 (E2 80 98) and U+2019 (E2 80 99) with '
    std::string result;
    result.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        unsigned char c0 = static_cast<unsigned char>(s[i]);
        if (c0 == 0xE2 && i + 2 < s.size() &&
            static_cast<unsigned char>(s[i+1]) == 0x80 &&
            (static_cast<unsigned char>(s[i+2]) == 0x98 ||
             static_cast<unsigned char>(s[i+2]) == 0x99)) {
            result += '\'';
            i += 3;
        } else {
            result += s[i++];
        }
    }
    return result;
}

static std::string_view trimSV(std::string_view sv) {
    size_t a = sv.find_first_not_of(" \t\r\n");
    if (a == std::string_view::npos) return {};
    size_t b = sv.find_last_not_of(" \t\r\n");
    return sv.substr(a, b - a + 1);
}

static std::string trim(std::string_view sv) { return std::string(trimSV(sv)); }

// Strip UTF-8 BOM if present.
static std::string_view stripBOM(std::string_view sv) {
    if (sv.size() >= 3 &&
        static_cast<unsigned char>(sv[0]) == 0xEF &&
        static_cast<unsigned char>(sv[1]) == 0xBB &&
        static_cast<unsigned char>(sv[2]) == 0xBF)
        return sv.substr(3);
    return sv;
}

static std::vector<std::string> split(std::string_view sv, char delim) {
    std::vector<std::string> result;
    size_t start = 0;
    for (size_t i = 0; i <= sv.size(); ++i) {
        if (i == sv.size() || sv[i] == delim) {
            result.emplace_back(sv.substr(start, i - start));
            start = i + 1;
        }
    }
    return result;
}

// TourPlay uses abbreviated skill names in some cases. Map them to seed names.
static const std::map<std::string, std::string, std::less<>> kSkillAliases = {
    // TourPlay shortens "Pogo Stick" to "Pogo"
    {"pogo", "Pogo Stick"},
};

// Normalize a skill name from TourPlay format to seed format.
// 1. Strips trailing numeric-only parentheticals added by TourPlay for team IDs:
//      "Hatred (102)"  → "Hatred"
//      "Loner (4+)"    → "Loner (4+)"   (non-numeric suffix: keep)
// 2. Applies known TourPlay short-form aliases (e.g. "Pogo" → "Pogo Stick").
static std::string normalizeSkillName(std::string_view s) {
    s = trimSV(s);
    // Step 1: strip numeric-only parenthetical suffix
    std::string result;
    {
        auto p = s.rfind('(');
        auto q = s.rfind(')');
        if (p != std::string_view::npos && q != std::string_view::npos && q > p) {
            std::string_view inner = s.substr(p + 1, q - p - 1);
            bool numericOnly = !inner.empty() &&
                std::ranges::all_of(inner, [](char c){ return std::isdigit((unsigned char)c); });
            if (numericOnly) {
                result = std::string(s.substr(0, p));
                while (!result.empty() && result.back() == ' ') result.pop_back();
            }
        }
        if (result.empty()) result = std::string(s);
    }
    // Step 2: apply alias map
    if (auto it = kSkillAliases.find(toLower(result)); it != kSkillAliases.end())
        return it->second;
    return result;
}

// ── Seed data (minimal parser) ────────────────────────────────────────────────

struct SeedPosition {
    std::string name;
    std::set<std::string, std::less<>> baseSkills; // lower-cased for O(1) diff
};

// Global set of all skill names known to the seed (lower-cased).
// Skills not in this set are silently dropped from extraSkills.
using KnownSkills = std::set<std::string, std::less<>>;
using TraitSkills  = std::set<std::string, std::less<>>; // Trait category — cannot be leveled

struct SeedRace {
    std::string name;
    std::vector<SeedPosition> positions;
};

struct SeedStarPlayer {
    std::string name;
    std::vector<std::string> allowedTeams;  // empty = any team
};

struct SeedData {
    std::vector<SeedRace> races;
    std::vector<SeedStarPlayer> starPlayers;
    KnownSkills knownSkills;
    TraitSkills traitSkills;   // Trait-category skills: illegal as extraSkills

    // Case-insensitive name lookup. Returns the canonical seed name or empty.
    std::string findStarPlayer(const std::string& tpName) const {
        const std::string ltp = toLower(tpName);
        for (const auto& sp : starPlayers)
            if (toLower(sp.name) == ltp) return sp.name;
        return {};
    }

    bool starPlayerAllowed(const std::string& seedName, const std::string& race) const {
        for (const auto& sp : starPlayers) {
            if (sp.name != seedName) continue;
            if (sp.allowedTeams.empty()) return true;
            for (const auto& t : sp.allowedTeams)
                if (t == race) return true;
            return false;
        }
        return false;
    }
};

// Return the root of a skill name (text before the first '(' if any, trimmed).
// Used to match TourPlay's shortened forms against seed's annotated forms:
//   "Animosity (Underworld Goblin Linemen)" → "animosity"
//   "Loner (4+)" → "loner"
static std::string skillRoot(std::string_view s) {
    s = trimSV(s);
    auto p = s.find('(');
    if (p == std::string_view::npos) return toLower(std::string(s));
    std::string r(s.substr(0, p));
    while (!r.empty() && r.back() == ' ') r.pop_back();
    return toLower(r);
}

static SeedData loadSeed(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Cannot open seed file: " + path);
    json j = json::parse(f);

    SeedData sd;

    // Build known-skill catalogue and trait set.
    for (const auto& sk : j.at("skills")) {
        std::string name = sk.at("name").get<std::string>();
        sd.knownSkills.insert(toLower(name));
        sd.knownSkills.insert(skillRoot(name));
        std::string cat = sk.value("category", "");
        if (cat == "Trait") {
            sd.traitSkills.insert(toLower(name));
            sd.traitSkills.insert(skillRoot(name));
        }
    }

    for (const auto& t : j.at("teamNames")) {
        SeedRace race;
        race.name = t.at("name").get<std::string>();
        for (const auto& p : t.value("rosterPositions", json::array())) {
            SeedPosition pos;
            pos.name = p.at("name").get<std::string>();
            for (const auto& sk : p.value("startingSkills", json::array())) {
                std::string s = sk.get<std::string>();
                pos.baseSkills.insert(toLower(s));
                // Also insert the root form so TourPlay's undecorated names match.
                // e.g. "Animosity (Underworld Goblin Linemen)" → "animosity"
                pos.baseSkills.insert(skillRoot(s));
            }
            race.positions.push_back(std::move(pos));
        }
        sd.races.push_back(std::move(race));
    }

    // Load star player catalogue for inducement resolution.
    for (const auto& sp : j.value("starPlayers", json::array())) {
        SeedStarPlayer ssp;
        ssp.name = sp.value("name", "");
        if (ssp.name.empty()) continue;
        for (const auto& t : sp.value("allowedTeams", json::array()))
            ssp.allowedTeams.push_back(t.get<std::string>());
        sd.starPlayers.push_back(std::move(ssp));
    }

    return sd;
}

// ── Race name normalization ───────────────────────────────────────────────────

static std::string normalizeRace(const std::string& tpRace) {
    // Maps TourPlay race names to seed race names.
    static const std::map<std::string, std::string, std::less<>> kMap = {
        {"Slann",              "Slanns"},
        {"Human",              "Humans"},
        {"Chaos Dwarf",        "Chaos Dwarves"},
        {"Underworld Denizen", "Underworlds Denizens"},
        {"High Elf",           "High Elves"},
        {"Snotling",           "Snotlings"},
        {"Halfling",           "Halflings"},
        {"Wood Elf",           "Wood Elves"},
        {"Black Orc",          "Black Orcs"},
    };
    if (auto it = kMap.find(tpRace); it != kMap.end()) return it->second;
    return tpRace;
}

// ── Position lookup ───────────────────────────────────────────────────────────

// TourPlay sometimes prefixes race name ("Dark Elf Runner") or omits a suffix
// ("Halfling Hopeful" vs "Halfling Hopeful Lineman").  We resolve by:
//   1. Exact case-insensitive match.
//   2. Seed name contains TourPlay name as substring, or vice versa (longest wins).
//   3. Explicit fallback table for cases that cannot be resolved by substring.
static const std::map<std::string, std::string, std::less<>> kPositionFallback = {
    {"tomb kings blitzer", "Anointed Blitzer"},
    {"tomb kings thrower", "Anointed Thrower"},
    // TourPlay uses "Skink Lineman"; seed uses "Skink Runner Lineman" (no overlap)
    {"skink lineman",      "Skink Runner Lineman"},
    // TourPlay uses "Snotling Lineman" for Underworlds; seed uses "Underworld Snotling"
    // (exact match handles the Snotlings race itself, so this only fires for Underworlds)
    {"snotling lineman",   "Underworld Snotling"},
};

static const SeedRace* findRace(const SeedData& sd, const std::string& name) {
    for (const auto& r : sd.races)
        if (r.name == name) return &r;
    return nullptr;
}

// Returns the best matching position, or nullptr. Writes a warning to *warn.
static const SeedPosition* findPosition(const SeedRace& race,
                                         const std::string& tpPos,
                                         std::string* warn = nullptr) {
    const std::string ltp = toLower(tpPos);

    // 1. Exact match
    for (const auto& p : race.positions)
        if (toLower(p.name) == ltp) return &p;

    // 2. Substring: prefer the match that covers the most characters
    const SeedPosition* best = nullptr;
    size_t bestLen = 0;
    for (const auto& p : race.positions) {
        std::string ls = toLower(p.name);
        if (ls.find(ltp) != std::string::npos || ltp.find(ls) != std::string::npos) {
            size_t len = std::max(ltp.size(), ls.size());
            if (len > bestLen) { bestLen = len; best = &p; }
        }
    }
    if (best) return best;

    // 3. Fallback table
    if (auto it = kPositionFallback.find(ltp); it != kPositionFallback.end()) {
        for (const auto& p : race.positions)
            if (p.name == it->second) return &p;
    }

    if (warn)
        *warn = std::format("no match for position '{}' in race '{}'",
                            tpPos, race.name);
    return nullptr;
}

// ── CSV parsing ───────────────────────────────────────────────────────────────

struct ParsedPlayer {
    std::string name;
    std::string tpPosition;              // TourPlay position string
    std::vector<std::string> skills;     // normalized skill names
    bool isStar{false};
};

struct ParsedTeam {
    std::string name;
    std::string tpRace;
    int  rerolls{2};
    bool hasApothecary{false};
    bool riotousRookies{false};
    std::vector<ParsedPlayer> players;
    std::vector<std::string> starPlayerNames;  // inducement star players (raw TourPlay names)
};

// Column-index helpers.
static int colIdx(const std::vector<std::string>& header, std::string_view name) {
    std::string ln = toLower(std::string(name));
    for (int i = 0; i < (int)header.size(); ++i)
        if (toLower(trim(header[i])) == ln) return i;
    return -1;
}

static std::string field(const std::vector<std::string>& row, int idx) {
    if (idx < 0 || idx >= (int)row.size()) return {};
    return trim(row[idx]);
}

// The TourPlay CSV layout (sections separated by blank lines):
//   Section 1: team-header row, then team-data row
//   Section 2: player-header row, then N player rows
//   Section 3: inducement-header row, then inducement rows
static ParsedTeam parseRosterCSV(std::string_view raw) {
    const std::string_view content = stripBOM(raw);

    // Split into non-blank lines, tagging each.
    std::vector<std::string> lines;
    {
        std::istringstream ss{std::string(content)};
        std::string ln;
        while (std::getline(ss, ln)) {
            if (!ln.empty() && ln.back() == '\r') ln.pop_back();
            lines.push_back(std::move(ln));
        }
    }

    ParsedTeam team;
    enum class State { TeamHeader, TeamData, PlayerHeader, Players, Inducements };
    State state = State::TeamHeader;

    // Column indices for each section
    int tName = -1, tRace = -1, tRerolls = -1, tApo = -1;
    int pName = -1, pPos = -1, pSkills = -1, pLevel = -1;

    for (const auto& rawLine : lines) {
        const std::string_view trimmed = trimSV(rawLine);
        if (trimmed.empty()) continue;  // blank separators

        switch (state) {
        case State::TeamHeader: {
            auto cols = split(trimmed, ';');
            tName    = colIdx(cols, "teamname");
            tRace    = colIdx(cols, "teamrace");
            tRerolls = colIdx(cols, "rerolls");
            tApo     = colIdx(cols, "apothecary");
            state = State::TeamData;
            break;
        }
        case State::TeamData: {
            auto cols = split(trimmed, ';');
            team.name  = field(cols, tName);
            team.tpRace = field(cols, tRace);
            try   { team.rerolls = std::stoi(field(cols, tRerolls)); }
            catch (...) {}
            {
                std::string a = toLower(field(cols, tApo));
                team.hasApothecary = (a == "true" || a == "1");
            }
            state = State::PlayerHeader;
            break;
        }
        case State::PlayerHeader: {
            // Look for the player header row.
            auto cols = split(trimmed, ';');
            std::string first = toLower(trim(cols[0]));
            if (first == "number" || first == "nr") {
                pName  = colIdx(cols, "name");
                pPos   = colIdx(cols, "position");
                pSkills = colIdx(cols, "skills");
                pLevel = colIdx(cols, "level");
                state = State::Players;
            }
            // else: unexpected row, stay in PlayerHeader
            break;
        }
        case State::Players: {
            auto cols = split(trimmed, ';');
            std::string first = toLower(trim(cols[0]));
            if (first == "inducement") {
                state = State::Inducements;
                break;  // header row of inducements — skip
            }

            ParsedPlayer p;
            p.name       = field(cols, pName);
            p.tpPosition = field(cols, pPos);

            if (pLevel >= 0) {
                std::string lvl = toLower(field(cols, pLevel));
                p.isStar = (lvl == "star");
            }
            if (toLower(p.tpPosition) == "star player") p.isStar = true;

            const std::string skillStr = field(cols, pSkills);
            if (!skillStr.empty()) {
                for (const auto& tok : split(skillStr, ',')) {
                    std::string sk = normalizeSkillName(tok);
                    if (!sk.empty()) p.skills.push_back(std::move(sk));
                }
            }

            if (!p.name.empty() && !p.tpPosition.empty())
                team.players.push_back(std::move(p));
            break;
        }
        case State::Inducements: {
            auto cols = split(trimmed, ';');
            std::string ind = normalizeQuotes(trim(cols[0]));
            std::string indLower = toLower(ind);
            if (indLower == "riotousrookies" || indLower == "riotous rookies")
                team.riotousRookies = true;
            else
                team.starPlayerNames.push_back(ind);  // resolved against seed later
            break;
        }
        }
    }

    return team;
}

// ── Build JSON for one team ───────────────────────────────────────────────────

static json buildTeamJson(const ParsedTeam& pt,
                           const SeedData& sd,
                           std::ostream& warnings) {
    const std::string seedRace = normalizeRace(pt.tpRace);
    const SeedRace* race = findRace(sd, seedRace);
    if (!race) {
        warnings << std::format("  WARNING: unknown race '{}' (normalized '{}') "
                                "— team '{}' skipped\n",
                                pt.tpRace, seedRace, pt.name);
        return json{};
    }

    json tj;
    tj["name"]         = pt.name;
    tj["race"]         = seedRace;
    tj["rerolls"]      = pt.rerolls;
    tj["hasApothecary"] = pt.hasApothecary;
    if (pt.riotousRookies) tj["riotousRookies"] = true;

    // Resolve inducement star players against the seed catalogue.
    json starPlayers = json::array();
    for (const auto& tpName : pt.starPlayerNames) {
        std::string seedName = sd.findStarPlayer(tpName);
        if (seedName.empty()) {
            // Not a star player (e.g. "Bribes", "HalflingMasterChef") — skip silently.
            continue;
        }
        if (!sd.starPlayerAllowed(seedName, seedRace)) {
            warnings << std::format("  NOTE: star player '{}' not allowed for race '{}'"
                                    " — skipped\n", seedName, seedRace);
            continue;
        }
        starPlayers.push_back(seedName);
    }
    if (!starPlayers.empty()) tj["starPlayers"] = std::move(starPlayers);

    json players = json::array();
    for (const auto& p : pt.players) {
        if (p.isStar) continue;   // star players are inducements, not roster slots

        std::string posWarn;
        const SeedPosition* seedPos = findPosition(*race, p.tpPosition, &posWarn);
        if (!seedPos) {
            warnings << std::format("  WARNING: {} in team '{}' "
                                    "— player '{}' skipped\n",
                                    posWarn, pt.name, p.name);
            continue;
        }

        // Extra skills = CSV skills that are not part of the position's base kit,
        // are in the seed's skill catalogue, and are not Trait-category (which
        // cannot be gained through leveling and would fail loader validation).
        // Unknown or Trait skills outside the base kit are silently dropped.
        std::vector<std::string> extras;
        for (const auto& sk : p.skills) {
            if (seedPos->baseSkills.contains(toLower(sk))) continue;
            if (!sd.knownSkills.contains(toLower(sk))) {
                warnings << std::format("  NOTE: skill '{}' on '{}' not in seed catalogue"
                                        " — dropped\n", sk, p.name);
                continue;
            }
            if (sd.traitSkills.contains(toLower(sk))) {
                warnings << std::format("  NOTE: Trait skill '{}' on '{}' not in base kit"
                                        " — dropped (seed position may be incomplete)\n",
                                        sk, p.name);
                continue;
            }
            extras.push_back(sk);
        }

        json pj;
        pj["position"] = seedPos->name;
        pj["name"]     = p.name;
        if (!extras.empty()) pj["extraSkills"] = extras;

        players.push_back(std::move(pj));
    }

    tj["players"] = std::move(players);
    return tj;
}

// ── ZIP reading ───────────────────────────────────────────────────────────────

struct ZipEntry { std::string filename; std::string content; };

static std::vector<ZipEntry> readRosterFiles(const std::string& zipPath) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, zipPath.c_str(), 0))
        throw std::runtime_error("Cannot open zip: " + zipPath +
                                 " — " + mz_zip_get_error_string(mz_zip_get_last_error(&zip)));

    std::vector<ZipEntry> result;
    const int n = static_cast<int>(mz_zip_reader_get_num_files(&zip));
    for (int i = 0; i < n; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, static_cast<mz_uint>(i), &stat)) continue;
        if (stat.m_is_directory) continue;

        std::string fname = stat.m_filename;
        if (toLower(fname).find("roster") == std::string::npos) continue;

        size_t sz = 0;
        void* data = mz_zip_reader_extract_to_heap(
            &zip, static_cast<mz_uint>(i), &sz, 0);
        if (!data) {
            std::print(stderr, "  WARNING: failed to extract '{}'\n", fname);
            continue;
        }
        result.push_back({fname, std::string(static_cast<const char*>(data), sz)});
        mz_free(data);
    }

    mz_zip_reader_end(&zip);
    return result;
}

// ── CLI ───────────────────────────────────────────────────────────────────────

struct Options {
    std::string zipFile;
    std::string seedFile    = "bloodbowl-2025-seed.json";
    std::string outputFile  = "tournament.json";
    int  rounds      = 6;
    int  tournaments = 1000;
    int  matchGames  = 1;
    std::string pairing = "dutch";
};

static void printUsage(const char* prog) {
    std::print(stderr,
        "Usage: {} <zipfile> [options]\n"
        "\n"
        "  -s, --seed FILE        Seed data file          (default: bloodbowl-2025-seed.json)\n"
        "  -o, --output FILE      Output tournament JSON   (default: tournament.json)\n"
        "  -r, --rounds N         Swiss rounds             (default: 6)\n"
        "  -t, --tournaments N    Simulation count         (default: 1000)\n"
        "  -g, --games N          Games per match          (default: 1)\n"
        "  -p, --pairing SYSTEM   dutch | monrad           (default: dutch)\n",
        prog);
}

static Options parseArgs(int argc, char* argv[]) {
    Options opts;
    if (argc < 2) { printUsage(argv[0]); std::exit(1); }

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];

        auto nextStr = [&]() -> std::string {
            if (++i >= argc) {
                std::print(stderr, "Expected value after {}\n", arg);
                std::exit(1);
            }
            return argv[i];
        };
        auto nextInt = [&]() -> int {
            std::string s = nextStr();
            try { return std::stoi(s); }
            catch (...) {
                std::print(stderr, "Expected integer after {}, got '{}'\n", arg, s);
                std::exit(1);
            }
        };

        if      (arg == "-s" || arg == "--seed")        opts.seedFile    = nextStr();
        else if (arg == "-o" || arg == "--output")       opts.outputFile  = nextStr();
        else if (arg == "-r" || arg == "--rounds")       opts.rounds      = nextInt();
        else if (arg == "-t" || arg == "--tournaments")  opts.tournaments = nextInt();
        else if (arg == "-g" || arg == "--games")        opts.matchGames  = nextInt();
        else if (arg == "-p" || arg == "--pairing") {
            opts.pairing = nextStr();
            if (opts.pairing != "dutch" && opts.pairing != "monrad") {
                std::print(stderr, "Pairing must be 'dutch' or 'monrad'\n");
                std::exit(1);
            }
        } else if (!arg.starts_with('-')) {
            opts.zipFile = std::string(arg);
        } else {
            std::print(stderr, "Unknown option: {}\n", arg);
            printUsage(argv[0]);
            std::exit(1);
        }
    }

    if (opts.zipFile.empty()) {
        std::print(stderr, "Error: no zip file specified\n");
        printUsage(argv[0]);
        std::exit(1);
    }
    return opts;
}

// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    const Options opts = parseArgs(argc, argv);

    SeedData sd;
    try { sd = loadSeed(opts.seedFile); }
    catch (const std::exception& e) {
        std::print(stderr, "Error loading seed '{}': {}\n", opts.seedFile, e.what());
        return 1;
    }

    std::vector<ZipEntry> entries;
    try { entries = readRosterFiles(opts.zipFile); }
    catch (const std::exception& e) {
        std::print(stderr, "Error reading zip '{}': {}\n", opts.zipFile, e.what());
        return 1;
    }

    if (entries.empty()) {
        std::print(stderr, "No roster files (filenames containing 'roster') found in {}\n",
                   opts.zipFile);
        return 1;
    }

    std::print("Found {} roster file(s) in {}\n", entries.size(), opts.zipFile);

    json teams = json::array();
    int skipped = 0;
    for (const auto& entry : entries) {
        ParsedTeam pt;
        try { pt = parseRosterCSV(entry.content); }
        catch (const std::exception& e) {
            std::print(stderr, "  WARNING: failed to parse '{}': {}\n",
                       entry.filename, e.what());
            ++skipped;
            continue;
        }

        if (pt.name.empty() || pt.tpRace.empty()) {
            std::print(stderr, "  WARNING: incomplete roster in '{}' — skipped\n",
                       entry.filename);
            ++skipped;
            continue;
        }

        const int nonStarCount = static_cast<int>(
            std::ranges::count_if(pt.players, [](const auto& p){ return !p.isStar; }));

        std::string starNote;
        for (const auto& s : pt.starPlayerNames) starNote += " +" + s;
        std::print("  {:30s}  {:25s}  {} rerolls  {}{}{}  ({} players)\n",
                   pt.name,
                   pt.tpRace,
                   pt.rerolls,
                   pt.hasApothecary ? "apo" : "   ",
                   pt.riotousRookies ? "  riotous-rookies" : "",
                   starNote,
                   nonStarCount);

        json tj = buildTeamJson(pt, sd, std::cerr);
        if (tj.is_null() || tj.empty()) { ++skipped; continue; }
        teams.push_back(std::move(tj));
    }

    if (teams.empty()) {
        std::print(stderr, "No valid teams produced.\n");
        return 1;
    }

    json tournament;
    tournament["tournaments"]   = opts.tournaments;
    tournament["rounds"]        = opts.rounds;
    tournament["matchGames"]    = opts.matchGames;
    tournament["pairingSystem"] = opts.pairing;
    tournament["teams"]         = std::move(teams);

    std::ofstream out(opts.outputFile);
    if (!out) {
        std::print(stderr, "Cannot write to '{}'\n", opts.outputFile);
        return 1;
    }
    out << tournament.dump(2) << '\n';

    std::print("\nWrote {} teams to '{}' ({} skipped)\n",
               tournament["teams"].size(), opts.outputFile, skipped);
    return 0;
}
