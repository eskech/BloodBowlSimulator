// tourplay_scraper — renders TourPlay Blood Bowl pages via headless Chrome/Chromium
// and generates a tournament JSON file for the BloodBowl simulator.
//
// The page is an Angular SPA that loads data through Firebase SDK (no REST API
// calls visible in browser DevTools). Headless Chrome renders the full DOM so
// the scraper can parse the actual content.
//
// Requirements: google-chrome or chromium-browser installed and in PATH.
//
// Usage:
//   tourplay_scraper --url <tourplay-tournament-url>
//                    [--chrome <path-to-chrome>]
//                    [--seed  bloodbowl-2025-seed.json]
//                    [--output tournament.json]
//                    [--rounds 4] [--tournaments 10000]
//                    [--wait  8000]   (ms to wait for Angular render, default 8000)
//                    [--verbose]
//
// Example:
//   tourplay_scraper --url https://tourplay.net/en/blood-bowl/punchbowl-2
//
// How it works:
//   1. Spawn Chrome --headless=new --remote-debugging-port=9222
//   2. Connect to CDP (Chrome DevTools Protocol) via WebSocket
//   3. Navigate to /players page, wait for network idle + Angular render
//   4. Extract team links from the rendered DOM
//   5. For each team, navigate to their roster URL and extract player data
//   6. Map race/skill names to simulator format using the seed file
//   7. Write tournament JSON

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <print>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ─────────────────────────────────────────────────────────────────────────────
// Process management
// ─────────────────────────────────────────────────────────────────────────────

static pid_t g_chromePid = 0;

static void killChrome() {
    if (g_chromePid > 0) { kill(g_chromePid, SIGTERM); waitpid(g_chromePid, nullptr, 0); g_chromePid = 0; }
}

static bool spawnChrome(const std::string& chromeBin, int port, bool headless) {
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        // Child: redirect stdout/stderr to /dev/null and exec Chrome
        int devnull = open("/dev/null", O_WRONLY);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        close(devnull);
        std::string portArg    = std::format("--remote-debugging-port={}", port);
        std::string profileArg = std::format("--user-data-dir=/tmp/tp_scraper_{}", (long)getpid());
        // Build args vector so we can conditionally add --headless=new
        std::vector<const char*> argv;
        argv.push_back(chromeBin.c_str());
        if (headless) {
            argv.push_back("--headless=new");
            argv.push_back("--disable-gpu");
            argv.push_back("--disable-dev-shm-usage");
        }
        argv.push_back("--no-sandbox");
        argv.push_back("--disable-extensions");
        argv.push_back("--disable-background-networking");
        argv.push_back(portArg.c_str());
        argv.push_back(profileArg.c_str());
        argv.push_back("about:blank");
        argv.push_back(nullptr);
        const char* const* argvArr = argv.data();
        execvp(argvArr[0], const_cast<char* const*>(argvArr));
        _exit(1);
    }
    g_chromePid = pid;
    // Wait for Chrome to be ready (poll the debugging port)
    for (int i = 0; i < 30; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        // Try a TCP connect to the debug port
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            close(sock);
            return true;
        }
        close(sock);
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// HTTP helper (libcurl, for CDP discovery + ordinary web requests)
// ─────────────────────────────────────────────────────────────────────────────

static size_t curlWrite(void* ptr, size_t size, size_t nmemb, std::string* out) {
    out->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

static std::string httpGetSimple(const std::string& url) {
    CURL* curl = curl_easy_init();
    if (!curl) return {};
    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return body;
}

// ─────────────────────────────────────────────────────────────────────────────
// Minimal WebSocket client (RFC 6455 text frames only — sufficient for CDP)
// ─────────────────────────────────────────────────────────────────────────────

class WsClient {
    int fd_ = -1;
    std::string buf_;   // raw receive buffer

    static std::string base64Encode(const std::string& in) {
        static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        int val = 0, bits = -6;
        for (unsigned char c : in) {
            val = (val << 8) + c;  bits += 8;
            while (bits >= 0) { out += T[(val >> bits) & 0x3F]; bits -= 6; }
        }
        if (bits > -6) out += T[((val << 8) >> (bits + 8)) & 0x3F];
        while (out.size() % 4) out += '=';
        return out;
    }

public:
    bool connect(const std::string& host, int port, const std::string& path) {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(static_cast<uint16_t>(port));
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) return false;

        // WebSocket upgrade handshake
        std::string key = base64Encode("tourplay-cdp-key-1234567890123");
        std::string req = std::format(
            "GET {} HTTP/1.1\r\n"
            "Host: {}:{}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: {}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n",
            path, host, port, key);
        send(fd_, req.data(), req.size(), 0);

        // Read response header (look for 101 Switching Protocols)
        char hdr[4096]{};
        int n = recv(fd_, hdr, sizeof(hdr) - 1, 0);
        return n > 0 && std::string(hdr, n).find("101") != std::string::npos;
    }

    void send_text(const std::string& msg) {
        // Build a masked text frame
        std::vector<uint8_t> frame;
        frame.push_back(0x81);  // FIN + text opcode

        size_t len = msg.size();
        if (len < 126) {
            frame.push_back(0x80 | static_cast<uint8_t>(len));
        } else if (len < 65536) {
            frame.push_back(0x80 | 126);
            frame.push_back((len >> 8) & 0xFF);
            frame.push_back(len & 0xFF);
        } else {
            frame.push_back(0x80 | 127);
            for (int i = 7; i >= 0; --i) frame.push_back((len >> (8 * i)) & 0xFF);
        }

        // Masking key (all zeros — valid per RFC)
        frame.insert(frame.end(), {0,0,0,0});
        for (char c : msg) frame.push_back(static_cast<uint8_t>(c));

        ::send(fd_, frame.data(), frame.size(), 0);
    }

    // Receive the next complete text frame (may block; timeout via SO_RCVTIMEO)
    std::optional<std::string> recv_frame(int timeoutMs = 30000) {
        // Set receive timeout
        timeval tv{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        // Keep reading until we have a complete frame
        while (true) {
            // Read more data
            char tmp[65536];
            int n = recv(fd_, tmp, sizeof(tmp), 0);
            if (n <= 0) return std::nullopt;
            buf_.append(tmp, static_cast<size_t>(n));

            // Try to parse a frame from buf_
            if (buf_.size() < 2) continue;
            auto* b = reinterpret_cast<const uint8_t*>(buf_.data());
            bool fin   = (b[0] & 0x80) != 0;
            uint8_t op = b[0] & 0x0F;
            bool masked = (b[1] & 0x80) != 0;
            uint64_t plen = b[1] & 0x7F;
            size_t hdrLen = 2;

            if (plen == 126) {
                if (buf_.size() < 4) continue;
                plen = (static_cast<uint64_t>(b[2]) << 8) | b[3];
                hdrLen = 4;
            } else if (plen == 127) {
                if (buf_.size() < 10) continue;
                plen = 0;
                for (int i = 0; i < 8; ++i) plen = (plen << 8) | b[2 + i];
                hdrLen = 10;
            }
            if (masked) hdrLen += 4;
            size_t total = hdrLen + plen;
            if (buf_.size() < total) continue;

            std::string payload(buf_.data() + hdrLen, plen);
            buf_.erase(0, total);

            // Ignore ping/pong/close; return text/continuation frames
            if (op == 0x8) return std::nullopt;  // close
            if (op == 0x9) { send_text("");       continue; }  // ping → pong (simplified)
            if (op == 0x0 || op == 0x1) {
                if (fin) return payload;
                // Fragmented — accumulate (simplified: treat continuation as fin)
                return payload;
            }
        }
    }

    ~WsClient() { if (fd_ >= 0) close(fd_); }
};

// ─────────────────────────────────────────────────────────────────────────────
// CDP session
// ─────────────────────────────────────────────────────────────────────────────

class Cdp {
    WsClient ws_;
    int nextId_ = 1;

public:
    bool connect(int port) {
        // Get the list of tabs from CDP HTTP endpoint
        std::string info = httpGetSimple(std::format("http://127.0.0.1:{}/json", port));
        if (info.empty()) return false;
        json tabs;
        try { tabs = json::parse(info); } catch (...) { return false; }

        // Find the first page tab (not devtools)
        for (const auto& tab : tabs) {
            std::string type = tab.value("type", "");
            if (type != "page") continue;
            std::string wsUrl = tab.value("webSocketDebuggerUrl", "");
            if (wsUrl.empty()) continue;
            // wsUrl = "ws://127.0.0.1:PORT/devtools/page/ID"
            std::string path = wsUrl.substr(wsUrl.find('/', 5));  // strip ws://host
            return ws_.connect("127.0.0.1", port, path);
        }
        return false;
    }

    // Send a CDP command and return the result (waits for matching id in response)
    std::optional<json> call(const std::string& method, json params = json::object(),
                             int timeoutMs = 30000) {
        int id = nextId_++;
        json cmd = {{"id", id}, {"method", method}, {"params", params}};
        ws_.send_text(cmd.dump());

        // Drain events until we get our response
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            int remaining = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()).count());
            auto frame = ws_.recv_frame(std::max(remaining, 100));
            if (!frame) return std::nullopt;
            json resp;
            try { resp = json::parse(*frame); } catch (...) { continue; }
            if (resp.contains("id") && resp["id"].get<int>() == id)
                return resp.value("result", json::object());
        }
        return std::nullopt;
    }

    // Navigate and wait for Page.loadEventFired
    bool navigate(const std::string& url, int timeoutMs = 30000) {
        // Enable Page domain so we receive events
        call("Page.enable");
        call("Network.enable");

        // Navigate
        json params = {{"url", url}};
        call("Page.navigate", params, 5000);

        // Drain events looking for Page.loadEventFired
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            int remaining = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()).count());
            auto frame = ws_.recv_frame(std::max(remaining, 100));
            if (!frame) break;
            json msg;
            try { msg = json::parse(*frame); } catch (...) { continue; }
            std::string method = msg.value("method", "");
            if (method == "Page.loadEventFired") return true;
        }
        return false;
    }

    // Wait for Angular to finish rendering by polling document.readyState
    // and checking that the Angular app root has content
    void waitForAngular(int extraMs = 8000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(extraMs));
    }

    // Get the full outer HTML of the document
    std::string getHTML() {
        auto result = call("Runtime.evaluate", {
            {"expression", "document.documentElement.outerHTML"},
            {"returnByValue", true}
        }, 15000);
        if (!result) return {};
        return result->value("result", json{}).value("value", "");
    }

    // Evaluate JavaScript and return the string result
    std::string evalString(const std::string& expr, int timeoutMs = 10000) {
        auto result = call("Runtime.evaluate", {
            {"expression", expr},
            {"returnByValue", true},
            {"awaitPromise", true}
        }, timeoutMs);
        if (!result) return {};
        return result->value("result", json{}).value("value", "");
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Name normalisation (same as before)
// ─────────────────────────────────────────────────────────────────────────────

static const std::unordered_map<std::string, std::string> RACE_MAP = {
    {"Human","Humans"},{"Humans","Humans"},
    {"Orc","Orcs"},{"Orcs","Orcs"},
    {"Dark Elf","Dark Elf"},{"Dark Elves","Dark Elf"},{"Dark Elven","Dark Elf"},
    {"Wood Elf","Wood Elves"},{"Wood Elves","Wood Elves"},
    {"High Elf","High Elves"},{"High Elves","High Elves"},
    {"Elf Union","Elf Union"},{"Pro Elf","Elf Union"},{"Pro Elves","Elf Union"},
    {"Dwarf","Dwarves"},{"Dwarves","Dwarves"},
    {"Chaos Dwarf","Chaos Dwarves"},{"Chaos Dwarves","Chaos Dwarves"},{"Chaos Dwarfs","Chaos Dwarves"},
    {"Skaven","Skaven"},
    {"Nurgle","Nurgle"},{"Nurgle's Rotters","Nurgle"},
    {"Undead","Shambling Undead"},{"Shambling Undead","Shambling Undead"},
    {"Necromantic","Necromantics"},{"Necromantic Horror","Necromantics"},{"Necromantics","Necromantics"},
    {"Chaos","Chaos Chosen"},{"Chaos Chosen","Chaos Chosen"},
    {"Halfling","Halflings"},{"Halflings","Halflings"},
    {"Goblin","Goblins"},{"Goblins","Goblins"},
    {"Snotling","Snotlings"},{"Snotlings","Snotlings"},
    {"Vampire","Vampires"},{"Vampires","Vampires"},
    {"Lizardman","Lizardmen"},{"Lizardmen","Lizardmen"},
    {"Amazon","Amazons"},{"Amazons","Amazons"},
    {"Norse","Norse"},{"Khorne","Khorne"},
    {"Chaos Renegade","Chaos Renegades"},{"Chaos Renegades","Chaos Renegades"},
    {"Bretonnian","Bretonnian"},{"Bretonnians","Bretonnian"},
    {"Old World Alliance","Old World Alliance"},
    {"Imperial Nobility","Imperial Nobility"},
    {"Ogre","Ogre"},{"Ogres","Ogre"},
    {"Black Orc","Black Orcs"},{"Black Orcs","Black Orcs"},
    {"Underworld","Underworlds Denizens"},{"Underworld Denizens","Underworlds Denizens"},{"Underworlds Denizens","Underworlds Denizens"},
    {"Slann","Slanns"},{"Slanns","Slanns"},
    {"Gnome","Gnomes"},{"Gnomes","Gnomes"},
    {"Tomb Kings","Tomb Kings"},
};

static std::string normaliseRace(const std::string& raw) {
    auto it = RACE_MAP.find(raw);
    if (it != RACE_MAP.end()) return it->second;
    std::string lower = raw;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    for (const auto& [k, v] : RACE_MAP) {
        std::string kl = k;
        std::transform(kl.begin(), kl.end(), kl.begin(), ::tolower);
        if (kl == lower) return v;
    }
    return raw;
}

static std::string normaliseSkill(const std::string& raw) {
    std::string s = raw;
    auto paren = s.rfind('(');
    if (paren != std::string::npos) s = s.substr(0, paren);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

// ─────────────────────────────────────────────────────────────────────────────
// Seed data
// ─────────────────────────────────────────────────────────────────────────────

using SeedSkills = std::map<std::string, std::map<std::string, std::set<std::string>>>;

static SeedSkills loadSeed(const std::string& path) {
    SeedSkills result;
    std::ifstream f(path);
    if (!f) { std::println(std::cerr, "Warning: cannot open seed file '{}' — extra skill detection disabled", path); return result; }
    json j;
    try { f >> j; } catch (...) { return result; }
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
// DOM-based data extraction via JavaScript evaluation in CDP
// ─────────────────────────────────────────────────────────────────────────────

// Extract team links from the rendered players/inscriptions page.
// TourPlay shows teams in a list; each has a link to the team detail page.
// We use JS to find all anchor tags containing the tournament path + a team ID.
static std::vector<std::pair<std::string,std::string>> extractTeamLinks(
    Cdp& cdp, const std::string& tournId, bool verbose)
{
    // Get all hrefs containing the tournament name
    std::string jsSimple = std::format(R"js(
(function() {{
  var anchors = Array.from(document.querySelectorAll('a[href]'));
  var tourId = '{}';
  var seen = {{}};
  var results = [];
  anchors.forEach(function(a) {{
    var h = a.getAttribute('href') || '';
    if (h.includes(tourId) && h.length > tourId.length + 10 && !seen[h]) {{
      seen[h] = 1;
      results.push(h + '|' + (a.textContent || '').trim().substring(0,80));
    }}
  }});
  return results.join('\n');
}})()
)js", tournId);

    std::string raw = cdp.evalString(jsSimple);
    if (verbose) std::println("  JS link extraction raw ({} chars): {:.200s}", raw.size(), raw);

    std::vector<std::pair<std::string,std::string>> result;
    std::istringstream ss(raw);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.empty()) continue;
        auto pipe = line.find('|');
        if (pipe == std::string::npos) continue;
        std::string href = line.substr(0, pipe);
        std::string name = line.substr(pipe + 1);
        // Keep only "team detail" type links — exclude nav/schedule/standings links
        if (href.find("/players") != std::string::npos) continue;
        if (href.find("/schedule") != std::string::npos) continue;
        if (href.find("/standings") != std::string::npos) continue;
        if (href.find("/news") != std::string::npos) continue;
        if (href.find("/phases") != std::string::npos) continue;
        result.push_back({href, name});
    }
    return result;
}

// Extract team + player data from a rendered team roster page.
static json extractRosterFromPage(Cdp& cdp, const SeedSkills& seed, bool verbose) {
    // Pull key fields via JS evaluation
    auto evalStr = [&](const std::string& expr) { return cdp.evalString(expr, 8000); };

    // Team name — look for the main heading
    std::string teamName = evalStr(R"js(
(function() {
  var h = document.querySelector('h1,h2,.team-name,.roster-name,.card-title');
  return h ? h.textContent.trim() : document.title;
})()
)js");

    // Race — look for text near race indicators
    std::string raceRaw = evalStr(R"js(
(function() {
  // Try to find race name in page text
  var els = document.querySelectorAll('.race-name,.roster-race,[class*="race"],[class*="Race"]');
  for (var e of els) { var t = e.textContent.trim(); if (t && t.length < 40) return t; }
  // Fall back: look for specific text patterns
  var body = document.body.innerText;
  var m = body.match(/Race[:\s]+([A-Za-z ]+)/);
  return m ? m[1].trim() : '';
})()
)js");

    // Rerolls
    std::string rerollsStr = evalStr(R"js(
(function() {
  var body = document.body.innerText;
  var m = body.match(/Re.?roll[s]?[:\s]+(\d+)/i);
  return m ? m[1] : '2';
})()
)js");

    // Apothecary
    std::string apothStr = evalStr(R"js(
(function() {
  var body = document.body.innerText.toLowerCase();
  return (body.includes('apothecary') && !body.includes('no apothecary') && !body.includes('apothecary: no')) ? 'true' : 'false';
})()
)js");

    // Inducements
    std::string riotousStr = evalStr(R"js(
(function() {
  var body = document.body.innerText.toLowerCase();
  return body.includes('riotous') ? 'true' : 'false';
})()
)js");

    // Players — extract as a JSON string from the DOM table/list
    std::string playersJson = evalStr(R"js(
(function() {
  var players = [];
  // Try mat-table rows or generic table rows
  var rows = document.querySelectorAll('mat-row, tr[class*="row"], tbody tr');
  rows.forEach(function(row) {
    var cells = row.querySelectorAll('mat-cell, td');
    if (cells.length < 2) return;
    var texts = Array.from(cells).map(function(c) { return c.textContent.trim(); });
    // A player row typically has: number, name, position, skills, stats
    // Filter out header rows
    if (texts[0] && texts[0].match(/^\d+$/) && texts[1] && texts[1].length > 0) {
      players.push({num: texts[0], name: texts[1], pos: texts[2] || '', skills: texts[3] || ''});
    }
  });
  // If no table rows found, try card-based layout
  if (players.length === 0) {
    var cards = document.querySelectorAll('[class*="player-card"], [class*="lineup-card"], [class*="player-item"]');
    cards.forEach(function(card) {
      var nameEl = card.querySelector('[class*="name"], [class*="player-name"], h3, h4');
      var posEl  = card.querySelector('[class*="position"], [class*="pos"]');
      var sklEl  = card.querySelector('[class*="skill"], [class*="skills"]');
      if (nameEl) {
        players.push({
          num: '', name: nameEl.textContent.trim(),
          pos: posEl ? posEl.textContent.trim() : '',
          skills: sklEl ? sklEl.textContent.trim() : ''
        });
      }
    });
  }
  return JSON.stringify(players);
})()
)js");

    if (verbose) {
        std::println("  teamName: {}", teamName);
        std::println("  race: {}", raceRaw);
        std::println("  rerolls: {}", rerollsStr);
        std::println("  apothecary: {}", apothStr);
        std::println("  playersJson ({} chars): {:.300s}", playersJson.size(), playersJson);
    }

    std::string race = normaliseRace(raceRaw);
    int rerolls = 2;
    try { rerolls = std::stoi(rerollsStr); } catch (...) {}
    bool hasApoth = (apothStr == "true");
    bool riotous  = (riotousStr == "true");

    const std::map<std::string, std::set<std::string>>* seedRace = nullptr;
    if (auto it = seed.find(race); it != seed.end()) seedRace = &it->second;

    json playersArr = json::array();
    try {
        json plist = json::parse(playersJson);
        for (const auto& p : plist) {
            std::string pName = p.value("name", "");
            std::string posRaw = p.value("pos", "");
            std::string skillStr = p.value("skills", "");

            // Parse comma/newline separated skills
            std::vector<std::string> allSkills;
            std::istringstream skss(skillStr);
            std::string sk;
            while (std::getline(skss, sk, ',')) {
                sk.erase(0, sk.find_first_not_of(" \t\n\r"));
                sk.erase(sk.find_last_not_of(" \t\n\r") + 1);
                if (!sk.empty()) allSkills.push_back(normaliseSkill(sk));
            }

            // Compute extra skills
            std::vector<std::string> extraSkills;
            if (seedRace && !posRaw.empty()) {
                if (auto posIt = seedRace->find(posRaw); posIt != seedRace->end()) {
                    for (const auto& s : allSkills)
                        if (!posIt->second.count(s)) extraSkills.push_back(s);
                } else {
                    extraSkills = allSkills;
                }
            } else {
                extraSkills = allSkills;
            }

            if (pName.empty()) continue;
            json player;
            player["position"] = posRaw;
            player["name"]     = pName;
            if (!extraSkills.empty()) player["extraSkills"] = extraSkills;
            playersArr.push_back(std::move(player));
        }
    } catch (...) {
        if (verbose) std::println(std::cerr, "  Warning: failed to parse player JSON");
    }

    json team;
    team["name"]          = teamName;
    team["race"]          = race;
    team["rerolls"]       = rerolls;
    team["hasApothecary"] = hasApoth;
    if (riotous) team["riotousRookies"] = true;
    team["players"]       = std::move(playersArr);
    return team;
}

// ─────────────────────────────────────────────────────────────────────────────
// Chrome binary discovery
// ─────────────────────────────────────────────────────────────────────────────

static std::string findChrome(const std::string& hint) {
    if (!hint.empty()) return hint;
    for (const char* name : {"google-chrome", "chromium-browser", "chromium", "chrome",
                              "google-chrome-stable", "brave-browser"}) {
        if (system(std::format("which {} >/dev/null 2>&1", name).c_str()) == 0)
            return name;
    }
    return {};
}

// ─────────────────────────────────────────────────────────────────────────────
// Argument parsing
// ─────────────────────────────────────────────────────────────────────────────

struct Args {
    std::string url;
    std::string chromeBin;
    std::string seedPath   = "bloodbowl-2025-seed.json";
    std::string outputPath = "tournament_scraped.json";
    int rounds       = 4;
    int tournaments  = 10000;
    int waitMs       = 8000;
    int debugPort    = 9222;
    bool headless    = true;   // false → visible browser window
    bool connectOnly = false;  // true  → don't spawn, connect to running browser
    bool verbose     = false;
};

static void printUsage(const char* prog) {
    std::println("Usage: {} --url <tourplay-url> [options]", prog);
    std::println("");
    std::println("  --url URL          TourPlay tournament URL");
    std::println("                     e.g. https://tourplay.net/en/blood-bowl/punchbowl-2");
    std::println("");
    std::println("Browser modes (pick one):");
    std::println("  (default)          Launch Chrome headless — no visible window");
    std::println("  --no-headless      Launch Chrome as a visible window");
    std::println("  --connect          Connect to an already-running browser on --port");
    std::println("                     Start your browser first with:");
    std::println("                       google-chrome --remote-debugging-port=9222");
    std::println("                     Then run this tool with --connect");
    std::println("");
    std::println("Options:");
    std::println("  --chrome PATH      Path to Chrome/Chromium (auto-detected if omitted)");
    std::println("  --seed FILE        Seed data file (default: bloodbowl-2025-seed.json)");
    std::println("  --output FILE      Output JSON file (default: tournament_scraped.json)");
    std::println("  --rounds N         Swiss rounds (default: 4)");
    std::println("  --tournaments N    Simulation runs (default: 10000)");
    std::println("  --wait MS          Extra ms to wait for Angular render (default: 8000)");
    std::println("  --port N           Chrome remote debugging port (default: 9222)");
    std::println("  --verbose          Print detailed progress");
    std::println("  --help");
}

static std::optional<Args> parseArgs(int argc, char* argv[]) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (++i >= argc) { std::println(std::cerr, "Missing value for {}", arg); std::exit(1); }
            return argv[i];
        };
        if      (arg == "--url")         a.url         = next();
        else if (arg == "--chrome")      a.chromeBin   = next();
        else if (arg == "--seed")        a.seedPath    = next();
        else if (arg == "--output")      a.outputPath  = next();
        else if (arg == "--rounds")      a.rounds      = std::stoi(next());
        else if (arg == "--tournaments") a.tournaments  = std::stoi(next());
        else if (arg == "--wait")        a.waitMs      = std::stoi(next());
        else if (arg == "--port")        a.debugPort   = std::stoi(next());
        else if (arg == "--no-headless") a.headless    = false;
        else if (arg == "--connect")     a.connectOnly = true;
        else if (arg == "--verbose")     a.verbose     = true;
        else if (arg == "--help")        { printUsage(argv[0]); std::exit(0); }
        else { std::println(std::cerr, "Unknown argument: {}", arg); return std::nullopt; }
    }
    if (a.url.empty()) { printUsage(argv[0]); return std::nullopt; }
    return a;
}

// Extract tournament base URL (strip trailing page name like /players)
static std::string tournamentBase(const std::string& url) {
    static const std::vector<std::string> SUFFIXES = {"/players","/standings","/schedule","/news","/phases"};
    std::string base = url;
    for (const auto& suf : SUFFIXES) {
        if (base.size() > suf.size() && base.substr(base.size() - suf.size()) == suf)
            base = base.substr(0, base.size() - suf.size());
    }
    return base;
}

static std::string extractTournamentId(const std::string& url) {
    std::string base = tournamentBase(url);
    auto pos = base.rfind('/');
    return (pos != std::string::npos) ? base.substr(pos + 1) : base;
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    auto argsOpt = parseArgs(argc, argv);
    if (!argsOpt) return 1;
    const auto& args = *argsOpt;

    std::string tournId    = extractTournamentId(args.url);
    std::string baseUrl    = tournamentBase(args.url);
    std::string playersUrl = baseUrl + "/players";

    std::println("Tournament   : {}", tournId);
    std::println("Players page : {}", playersUrl);
    std::println("Output       : {}", args.outputPath);
    std::println("");

    // Load seed data
    curl_global_init(CURL_GLOBAL_DEFAULT);
    SeedSkills seed = loadSeed(args.seedPath);
    if (!seed.empty()) std::println("Loaded seed: {} races", seed.size());

    if (args.connectOnly) {
        // ── Connect mode: attach to a browser already running with --remote-debugging-port
        std::println("Connect mode: attaching to browser on port {} ...", args.debugPort);
        std::println("(Make sure your browser is running with --remote-debugging-port={})", args.debugPort);
    } else {
        // ── Spawn mode: launch Chrome/Chromium ourselves
        std::string chrome = findChrome(args.chromeBin);
        if (chrome.empty()) {
            std::println(std::cerr, "Error: Chrome/Chromium not found. Install google-chrome or chromium-browser,");
            std::println(std::cerr, "       or use --connect to attach to a browser you started manually.");
            curl_global_cleanup();
            return 1;
        }
        std::string mode = args.headless ? "headless" : "visible window";
        std::println("Launching Chrome {} (port {}) ...", mode, args.debugPort);
        if (!spawnChrome(chrome, args.debugPort, args.headless)) {
            std::println(std::cerr, "Failed to start Chrome.");
            curl_global_cleanup();
            return 1;
        }
        std::println("Chrome started (PID {}).", static_cast<int>(g_chromePid));
    }

    // Connect CDP
    Cdp cdp;
    std::println("Connecting to CDP ...");
    for (int attempt = 0; attempt < 5; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (cdp.connect(args.debugPort)) break;
        if (attempt == 4) {
            std::println(std::cerr, "Failed to connect to CDP.");
            killChrome();
            curl_global_cleanup();
            return 1;
        }
    }
    std::println("CDP connected.");

    // ── Step 1: Navigate to players page ────────────────────────────────────
    std::println("Navigating to {} ...", playersUrl);
    cdp.navigate(playersUrl, 20000);
    std::println("Waiting {}ms for Angular to render ...", args.waitMs);
    cdp.waitForAngular(args.waitMs);

    // ── Step 2: Extract team links ───────────────────────────────────────────
    std::println("Extracting team links ...");
    auto teamLinks = extractTeamLinks(cdp, tournId, args.verbose);
    std::println("Found {} potential team link(s).", teamLinks.size());

    if (teamLinks.empty()) {
        // Dump page title and first 500 chars to help diagnose
        std::string title = cdp.evalString("document.title");
        std::string snippet = cdp.evalString("document.body.innerText.substring(0,500)");
        std::println(std::cerr, "Page title: {}", title);
        std::println(std::cerr, "Page snippet: {}", snippet);
        std::println(std::cerr, "\nNo team links found. The page may require login or the DOM structure differs.");
        std::println(std::cerr, "Try --verbose for more detail or increase --wait.");
        killChrome();
        curl_global_cleanup();
        return 1;
    }

    // ── Step 3: Visit each team roster page ──────────────────────────────────
    json teams = json::array();
    int ok = 0, fail = 0;
    std::set<std::string> visited;

    for (const auto& [href, linkText] : teamLinks) {
        if (visited.count(href)) continue;
        visited.insert(href);

        std::string fullUrl = "https://tourplay.net" + href;
        std::println("  Visiting {} ({}) ...", href, linkText.substr(0, 40));

        cdp.navigate(fullUrl, 20000);
        cdp.waitForAngular(args.waitMs);

        json team = extractRosterFromPage(cdp, seed, args.verbose);

        int nPlayers = static_cast<int>(team["players"].size());
        std::println("    {} ({}) — {} players, {} rerolls, apoth={}",
            team.value("name", "?"),
            team.value("race", "?"),
            nPlayers,
            team.value("rerolls", 0),
            team.value("hasApothecary", false));

        if (nPlayers == 0) {
            std::println(std::cerr, "    Warning: no players found — skipping");
            ++fail;
            continue;
        }
        teams.push_back(std::move(team));
        ++ok;
    }

    std::println("\nScraped: {} ok, {} failed/skipped", ok, fail);

    if (teams.empty()) {
        std::println(std::cerr, "No teams scraped. Check that the tournament is public and try --verbose.");
        killChrome();
        curl_global_cleanup();
        return 1;
    }

    // ── Step 4: Write tournament JSON ─────────────────────────────────────────
    json tournament;
    tournament["tournaments"]   = args.tournaments;
    tournament["rounds"]        = args.rounds;
    tournament["matchGames"]    = 1;
    tournament["pairingSystem"] = "monrad";
    tournament["teams"]         = std::move(teams);

    std::ofstream out(args.outputPath);
    if (!out) {
        std::println(std::cerr, "Cannot write to '{}'", args.outputPath);
        killChrome();
        curl_global_cleanup();
        return 1;
    }
    out << tournament.dump(2) << "\n";
    std::println("Written to '{}'", args.outputPath);

    killChrome();
    curl_global_cleanup();
    return 0;
}
