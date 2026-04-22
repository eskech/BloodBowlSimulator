# Blood Bowl 2025 Simulator

A Monte Carlo simulator for Blood Bowl 2020/2025. Runs thousands of games in parallel to estimate win rates, scoring patterns, and casualty statistics for any matchup or Swiss tournament bracket.

## Requirements

- C++23 compiler (GCC 13+ or Clang 17+)
- CMake 3.28+
- OpenMP
- Internet connection on first build (CMake fetches [nlohmann/json](https://github.com/nlohmann/json) v3.11.3 and [miniz](https://github.com/richgel999/miniz) 3.0.2 automatically)

## Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

The build produces two executables in the build directory:
- `bloodbowl` — the simulator
- `tourplay-convert` — converts a TourPlay data export to a tournament JSON (see below)

The build also copies `bloodbowl-2025-seed.json` and all files from `data/` into the build directory automatically.

## Running

### Single matchup

```bash
./bloodbowl example_match.json
```

### Multiple matchups in parallel

```bash
./bloodbowl owa_vs_dark_elves.json owa_vs_orcs.json owa_vs_imperial.json
```

### Swiss tournament

```bash
./bloodbowl --tournament example_tournament.json
./bloodbowl --tournament league_16teams.json
```

### Export game samples to CSV

```bash
./bloodbowl example_match.json --runs 10000 --output results
# writes results_team1_vs_team2.csv
```

### All options

```
Usage: bloodbowl <match.json> [match2.json ...] [options]
       bloodbowl --tournament <tournament.json> [options]

  -s, --seed FILE        Seed data file (default: bloodbowl-2025-seed.json)
  -t, --tournament FILE  Run Swiss tournament from FILE
  -r, --runs N           Collect N game samples → CSV (single matchup only)
  -o, --output STEM      Output file stem (default: results)
```

## TourPlay import

`tourplay-convert` reads a TourPlay data export zip and writes a tournament JSON ready for `bloodbowl --tournament`.

```bash
./tourplay-convert punchbowl-2-data.zip
# writes tournament.json with 6 rounds, dutch pairing, 1000 simulations

./tourplay-convert punchbowl-2-data.zip -r 7 -p monrad -o grand-prix.json
```

### All options

```
Usage: tourplay-convert <zipfile> [options]

  -s, --seed FILE        Seed data file           (default: bloodbowl-2025-seed.json)
  -o, --output FILE      Output tournament JSON    (default: tournament.json)
  -r, --rounds N         Swiss rounds             (default: 6)
  -t, --tournaments N    Simulation count          (default: 1000)
  -g, --games N          Games per match           (default: 1)
  -p, --pairing SYSTEM   dutch | monrad            (default: dutch)
```

The tool processes every file in the zip whose name contains `roster`. It prints a summary line per team (with `+Name` for each star player) and emits `NOTE:` diagnostics for skills that were dropped (unknown skill names or Trait-category skills absent from the position's base kit — these cannot be gained through leveling and would fail validation).

**Star players**: roster entries with level `Star` are skipped. Inducement rows are checked against the seed's star player catalogue; recognised names are added to the team's `starPlayers` array. Non-star inducements (`Bribes`, `HalflingMasterChef`, etc.) are silently ignored. TourPlay encodes apostrophes in names as Unicode curly quotes — the tool normalises these to ASCII before the lookup.

**`RiotousRookies`** inducements set the `riotousRookies` flag on the team.

**Position and race name mapping**: TourPlay uses slightly different names from the seed in several places (e.g. `Chaos Dwarf` → `Chaos Dwarves`, `Skink Lineman` → `Skink Runner Lineman`, `Tomb Kings Blitzer` → `Anointed Blitzer`). The tool resolves these automatically via exact match, substring match, and an explicit fallback table.

**Skill normalization**: TourPlay appends team IDs to some skills (`Hatred (102)` → `Hatred`) and uses short forms for others (`Pogo` → `Pogo Stick`). Annotated seed names (`Animosity (Underworld Goblin Linemen)`) are matched against the plain TourPlay form by comparing root names.

**Zip files are gitignored** (`data/*.zip`).

---

## Input file format

### Match file

```json
{
  "simulations": 50000,
  "team1": {
    "name": "Reikland Reavers",
    "race": "Humans",
    "rerolls": 3,
    "hasApothecary": true,
    "riotousRookies": false,
    "strategy": {
      "wrestle": 0.5,
      "standFirm": 0.4,
      "divingTackle": 0.5,
      "pro": 0.5
    },
    "players": [
      {
        "position": "Human Blitzer",
        "name": "Iron Fist",
        "extraSkills": ["Guard"],
        "isTeamCaptain": true
      },
      {
        "position": "Human Lineman",
        "name": "Aldric",
        "strategy": { "wrestle": 1.0 }
      }
    ]
  },
  "team2": { ... }
}
```

**Team fields**

| Field | Type | Default | Description |
|---|---|---|---|
| `name` | string | `"Unknown Team"` | Display name |
| `race` | string | `"Humans"` | Must match a race in the seed file |
| `rerolls` | int | `2` | Team re-rolls available per half |
| `hasApothecary` | bool | `false` | Enables apothecary (once per game) |
| `riotousRookies` | bool | `false` | Snotlings inducement — adds 2D3+1 Snotling Lineman Journeymen before each game |
| `starPlayers` | array | `[]` | Names of hired star players (must exist in seed and be allowed for the team's race) |
| `strategy` | object | see below | Team-wide strategy defaults |

**Strategy fields** (all `0.0`–`1.0`, probability of using the skill/tactic)

| Field | Default | Description |
|---|---|---|
| `wrestle` | `0.8` | Use Wrestle on Both Down results |
| `standFirm` | `0.7` | Use Stand Firm when pushed |
| `divingTackle` | `0.6` | Use Diving Tackle after an opponent dodges away |
| `pro` | `0.5` | Use Pro to re-roll a failed roll |

Strategy can be set at team level and overridden per player. Each player also inherits the team default for fields not specified.

**Player fields**

| Field | Type | Description |
|---|---|---|
| `position` | string | Must match a roster position in the seed data for the team's race |
| `name` | string | Display name |
| `extraSkills` | array | Additional skills beyond starting skills (validated against allowed categories) |
| `strategy` | object | Per-player strategy overrides (merged with team defaults) |
| `isTeamCaptain` | bool | Designate this player as Team Captain (see below) |

Rosters can have 11–16 players. Positional limits from the seed data are enforced at load time. At most 11 players are fielded per drive; extras sit in the Reserves box and return at the next kickoff.

### Star players

Star players are hired as inducements and listed by name:

```json
{
  "name": "Reikland Reavers",
  "race": "Humans",
  "rerolls": 3,
  "hasApothecary": true,
  "starPlayers": ["Morg 'n' Thorg"],
  "players": [ ... ]
}
```

Each name must match an entry in the `starPlayers` catalogue in `bloodbowl-2025-seed.json`. If `allowedTeams` in the catalogue is non-empty, the team's race must appear in that list (e.g. Fungus the Loon is restricted to Orcs, Goblins, Skaven, and a few others; Morg 'n' Thorg may be hired by any team).

Star players use their own stats from the seed rather than any roster position. All their skills — including **Loner** — are set automatically. Loner means: before the team may spend a re-roll on behalf of that player, a D6 is rolled; on a **4+** the re-roll is granted, otherwise it is declined (the re-roll count is not decremented). Star players are always fielded; they are not subject to the 11-player cap bench logic.

Star players are displayed with `★` in the roster printout.

### Team Captain

The Team Captain special rule is available to **Humans** and **Orcs** only. The loader rejects `isTeamCaptain: true` on any other race.

Rules enforced:
- Exactly one captain per team
- Big Guys cannot be captain
- Captain automatically gains **Pro** (free, bypasses normal skill access checks)
- While the captain is active on the pitch: before spending a team re-roll, roll a D6 — on a **natural 6** the re-roll is free and the count is not decremented
- Captain is always fielded if able (never benched by the 11-player cap)

The captain is marked with `★` and `[Pro (C)]` in the roster display.

### Tournament file

```json
{
  "tournaments": 1000,
  "rounds": 5,
  "matchGames": 1,
  "pairingSystem": "dutch",
  "teams": [
    {
      "name": "Old World Wanderers",
      "race": "Old World Alliance",
      "rerolls": 3,
      "hasApothecary": true,
      "players": [ ... ]
    }
  ]
}
```

| Field | Description |
|---|---|
| `tournaments` | Number of full bracket simulations to run |
| `rounds` | Swiss rounds per tournament |
| `matchGames` | Games per head-to-head match (majority wins the match). Use 1 for realistic variance; higher values converge to expected win rate |
| `pairingSystem` | `"dutch"` (top half vs bottom half) or `"monrad"` (consecutive pairs) |

**Scoring:** Win = 3 pts, Draw = 1 pt, Loss = 0 pt. Tiebreakers: net score → net casualties → random.

### Seed file

`bloodbowl-2025-seed.json` contains all 31 races, their roster positions with stats and starting skills, and the full 175-skill catalogue. It is loaded at startup and is required for validation. Normally it does not need to be edited.

## Sample data files

| File | Description |
|---|---|
| `example_match.json` | Amazons vs Dwarves |
| `humans_vs_orcs.json` | Humans (with captain) vs Orcs (with captain) |
| `owa_vs_dark_elves.json` | Old World Alliance vs Dark Elves |
| `owa_vs_imperial.json` | Old World Alliance vs Imperial Nobility |
| `owa_vs_orcs.json` | Old World Alliance vs Orcs (with captain) |
| `snotlings_vs_chaos_dwarves.json` | Snotlings (with Riotous Rookies) vs Chaos Dwarves |
| `wood_elves_vs_nurgle.json` | Wood Elves vs Nurgle |
| `example_tournament.json` | 8-team Swiss tournament, Dutch pairing |
| `league_16teams.json` | 16-team Swiss tournament, Monrad pairing |
| `grand_tournament_32teams.json` | 32-team grand tournament covering all 31 races, Monrad pairing |

---

## What is simulated

### Zone system

The real Blood Bowl pitch is a 26×15 grid of squares (390 squares total). The simulator abstracts this into **5 linear zones**, each representing a band of columns across the full width of the pitch:

```
   Defense                                              Offense
   end zone                                             end zone
      │                                                    │
      ▼                                                    ▼
┌──────────┬──────────────┬──────────┬──────────────┬──────────┐
│OwnEndZone│   OwnHalf    │ Midfield │   OppHalf    │OppEndZone│
│ cols 1-2 │  cols 3-13   │ cols13-14│  cols 14-24  │ cols25-26│
└──────────┴──────────────┴──────────┴──────────────┴──────────┘
     0             1            2             3            4
```

All zones are expressed from the **offense's perspective** — the ball carrier scores by reaching `OppEndZone` (zone 4). Zones flip at every new drive so each team always attacks toward zone 4.

**What the zone model captures:**

- **Player distribution**: how many players each team has in each band of the pitch. Three players start on the Line of Scrimmage (Midfield), the rest in OwnHalf or OppHalf.
- **Tackle zones**: the number of active defenders in the same zone as the ball carrier, which determines how hard it is to dodge through that area.
- **Blocking range**: a player can block any opponent in the same zone or an adjacent zone (±1).
- **Pass range**: a pass to the next zone ahead is a short pass; a pass directly to OppEndZone incurs a range modifier.

**What the zone model does not capture:**

- Individual square positions, formation geometry, or exact player coordinates.
- Wide Zone column restrictions (max 2 players per wide zone) — not representable in a 5-zone model.
- Cage corners, sideline pressure, or exact tackle zone coverage counts per square.

The tradeoff is speed: the zone model runs over 70,000 games per second on a 16-core machine, enabling statistically reliable estimates from tens of thousands of simulations.

### Drive setup

At the start of each drive the following rules are applied in order:

1. **Return benched players** — players that sat out last drive due to the 11-player cap return to the available pool
2. **KO recovery** — each KO'd player rolls 4+ to return
3. **Swarming** — Swarming-trait players in Reserves may enter (see Swarming below)
4. **11-player cap** — at most 11 players per team may be fielded per drive; excess are placed in the Reserves box and return automatically at the next kickoff. The Team Captain is always fielded first and is never benched by this cap. Benching priority: from the end of the roster (typically reserve linemen)
5. **Zone placement** — at least 3 players per team placed on the Line of Scrimmage (`Midfield`); remaining active players fill `OwnHalf` (offense) or `OppHalf` (defense)

The Wide Zone restriction (maximum 2 players per wide zone) is a lateral constraint that does not map to the zone model and is not enforced.

### Movement

Ball carrier movement each turn is gated by **MA (Movement Allowance)**, which determines how many zone-crossing attempts the carrier gets per activation:

| MA | Zone attempts | Typical positions |
|----|--------------|-------------------|
| 4  | 1 | Dwarves, Tomb King skeletons, Nurgle Rotspawn |
| 5  | 1 | Chaos Warriors, Orcs, most Pestigors |
| 6  | 2 | Humans, Elf linemen, most standard positions |
| 7  | 2 | Blitzers |
| 8  | 2 | Elf Catchers, Wardancers, Gutter Runners |
| 9+ | 3 | Very rare (certain big guys and special players) |

Sprint adds +1 attempt on a 2+ roll (with Sure Feet as a re-roll). This means a Dwarf ball carrier grinds one zone per turn while an Elf Catcher covers two — matching the real game's pace difference.

Each zone-crossing attempt requires the carrier to dodge through any tackle zones present in their current zone. Failing a dodge drops the ball and may trigger a turnover.

### Tackle zones and screening

The effective number of tackle zones on a player in a given zone is calculated as:

```
raw        = number of active, non-prone defenders in the zone
screened   = (Guards in zone × 1) + (other active offensive blockers in zone ÷ 2)
effective  = clamp(raw − screened, 0, 4)
```

**Cage effect**: offensive players in the same zone as the ball carrier reduce the effective tackle zones. A player with the Guard skill cancels one tackle zone outright; regular blockers cancel at two-for-one. A well-formed cage of four blockers can reduce four tackle zones to zero, making the carrier nearly impossible to reach without a Blitz action.

**Cap of 4**: being surrounded by four or more defenders is meaningfully worse than facing two. With four tackle zones, even an AG 2 carrier (normally 83% dodge success) succeeds only 17% of the time on each crossing attempt.

This makes zone control a genuine strategic factor. Bash teams that mass players in one zone protect their carrier effectively; teams that spread thin lose the cage benefit.

### Game flow

- **Two halves** of 8 turns each, both teams alternating turns per half
- **Coin toss** determines which team receives first
- **Kickoff** at the start of each half and after every touchdown, following the drive setup sequence above
- **Touchdown** ends the current drive and resets positions

### Weather

At the start of each game the simulator rolls 2D6 to determine weather for the full game:

| Roll | Condition | Effect |
|---|---|---|
| 2 | Sweltering Heat | Before each drive, each on-pitch player rolls D6 — on a 6 they are KO'd |
| 3–4 | Very Sunny | +1 to PA target number (harder to pass) |
| 5–10 | Nice | No effect |
| 11 | Pouring Rain | +1 to PA target and +1 to catch/pickup target |
| 12 | Blizzard | No passing allowed at all |

### Kickoff events

After each kickoff setup, a D8 is rolled to determine the kickoff event:

| Roll | Event | Effect |
|---|---|---|
| 1 | Riot | Coin flip: one team loses a re-roll and the other gains one |
| 2 | Blitz | Defense gets one free block on the ball carrier before the drive begins |
| 3 | High Kick | Ball carrier advances one zone for free |
| 4 | Cheering Fans | Coin flip: one team gains a re-roll |
| 5 | Brilliant Coaching | Coin flip: one team gains a re-roll |
| 6 | Quick Snap | All offense players advance one zone before the defense can react |
| 7 | Perfect Defense | No modelled effect |
| 8 | Pitch Invasion | Each on-pitch player rolls D6 — on a 5–6 they go prone |

### Turn structure

Each turn proceeds in this order:

1. **Stand-up phase**: stunned players recover; prone players stand (simplified — no MA cost)
2. **Blocking phase**: up to 3 offensive blocks (each blocker activates once)
   - Bone Head players must pass a D6 check (2+) before acting; failure wastes the action and the player's tackle zone
   - A successful push causes the blocker to follow up one zone forward (unless the defender has Fend)
   - Frenzy players must follow up and immediately block again
3. **Defense blitz**: one defender may move into the carrier's zone and attempt a block
   - Probability varies with turns remaining: 67% early (turns 6–8), 50% mid (3–5), 33% late (1–2), reflecting fresh vs depleted defenses
   - A Sidestep carrier raises the blitz threshold by 2 (defense less eager to blitz)
4. **Ball carrier action**: carrier may hand off to an adjacent teammate, pass to a receiver ahead, or run
   - A pass is subject to interception: the most-agile eligible defender in the pass path gets one attempt (target = max(4, AG+2))
5. **Foul**: one offensive player may foul a prone opponent (one foul per turn)

**Per-turn action limits enforced (BB2020 §8):**
- One Blitz action per team per turn
- One Pass action per team per turn
- One Hand-off action per team per turn
- One Foul action per team per turn
- Each player activates at most once per turn

### Skills simulated

| Skill | Effect |
|---|---|
| **Accurate** | −1 to PA target number |
| **Block** | Re-roll Both Down results |
| **Bone Head** | Before acting, roll D6: on a 1 the player is Distracted (no action, no tackle zone this turn); team re-roll may be used |
| **Catch** | Re-roll one failed catch per activation |
| **Claws** | Armour rolls of 8+ break armour regardless of AV |
| **Dirty Player** | +1 to armour and injury rolls when fouling |
| **Diving Catch** | −1 to AG catch target (easier to catch) |
| **Diving Tackle** | Fall prone after opponent dodges away to force an extra dodge roll |
| **Dodge** | Re-roll one failed dodge per activation |
| **Fend** | Attacker cannot follow up after a push result |
| **Frenzy** | Must follow up after any push/stumble/down and block again immediately |
| **Grab** | Negates the Sidestep skill on the defender |
| **Guard** | Cancels one effective tackle zone on the ball carrier when in the same zone (cage effect) |
| **Mighty Blow** | +1 to armour/injury rolls |
| **Nerves of Steel** | Ignore tackle zone penalties on pass and catch rolls |
| **Pass** | Re-roll one failed pass throw per activation |
| **Pro** | Once per activation: re-roll any failed roll on 3+ |
| **Regeneration** | On a Casualty result: roll 4+ to place in Reserves instead (after apothecary) |
| **Sidestep** | On a push result, defender chooses any adjacent square instead of being pushed back |
| **Sneaky Git** | Avoid ejection on doubles when fouling |
| **Sprint** | One extra zone-crossing attempt per activation |
| **Stand Firm** | Resist being pushed back when blocked |
| **Strip Ball** | On a push result against a ball carrier, the ball is knocked loose (no knockdown) |
| **Sure Feet** | Re-roll a failed Sprint roll |
| **Sure Hands** | Re-roll one failed pickup per activation |
| **Swarming** | Snotling Linemen only — D3 enter the pitch from Reserves each drive, capped by on-pitch Swarming count |
| **Tackle** | Cancels opponent's Dodge skill |
| **Tentacles** | Before the ball carrier leaves a zone, each Tentacles defender in that zone contests ST vs ST (ties go to Tentacles); on a win the carrier is held and drops the ball |
| **Titchy** | Cannot make or receive assists |
| **Wrestle** | Accept Both Down to bring down opponent |

All 175 skills from the 2025 rulebook are stored in a bitmask for O(1) lookup. Skills not listed above are loaded and displayed correctly but have no active simulation effect.

### Injury and recovery

- **Armour roll** (2d6 > AV): determines if the player is injured after being knocked down
- **Injury roll** (2d6): Stunned (2–7), KO (8–9), Casualty (10–12)
- **Apothecary**: once per game, on a Casualty result — opponent re-rolls, coach takes the better outcome
- **Regeneration**: after apothecary, 4+ converts Casualty to Reserves (modelled as KO)
- **KO recovery**: 4+ roll at every drive kickoff

### Crowd surfing

When a defender is pushed and their zone is already `OwnEndZone`, they are pushed off the pitch into the crowd. The crowd makes an armour roll against the player's own AV; if the roll exceeds AV the player is injured normally. If armour holds, the player is still KO'd (removed from the pitch for the remainder of the drive).

### Special rules

| Rule | Teams | Effect |
|---|---|---|
| **Stunty** | Halflings, Snotlings, Goblin-type positions | −1 to all dodge target numbers (effectively one extra tackle zone); opponents get +1 assist when blocking a Stunty player |
| **Team Captain** | Humans, Orcs | One non-Big-Guy player gains Pro for free; natural 6 before a team re-roll makes it free; captain always fielded if able |

### Inducements

| Inducement | Teams | Effect |
|---|---|---|
| **Riotous Rookies** | Snotlings | Adds 2D3+1 Snotling Lineman Journeymen before the game; they start in Reserves and enter via Swarming each drive |

### Swiss tournament

- **Dutch pairing**: sort standings, split into top and bottom halves, pair across
- **Monrad pairing**: sort standings, pair consecutive (1st vs 2nd, 3rd vs 4th, …)
- **Bye**: lowest-ranked team without a previous bye gets a free win (3 pts, +1 net score)
- **Rematch avoidance**: one-pass adjacent swap when pairing would create a rematch
- **Match pool**: before tournament rounds begin, all N×(N−1)/2 pairings are pre-simulated 512 times in parallel. Each round then samples from this pool instead of running new games, giving a ~20× speedup for large tournaments with many rounds

### Parallelism

- Games are distributed across all available CPU cores with OpenMP
- Multiple match files run in parallel (outer loop) with inner per-game parallelism
- Tournament mode parallelises across tournament runs with per-thread RNG seeds and accumulators to avoid contention

---

## Architecture

```
src/
  main.cpp           CLI, argument parsing, output formatting
  loader.cpp/hpp     JSON parsing for match, tournament, and seed files
  simulator.cpp/hpp  Game engine: buildTeamState, simulateGame, runSimulations
                     weather (rollWeather), kickoff events (kickoffEvent),
                     hand-off (attemptHandoff), foul (attemptFoul),
                     interception (attemptInterception), Tentacles, Bone Head,
                     crowd surfing, Frenzy, Fend, Strip Ball, Sidestep, Stunty,
                     useTeamReroll (captain bonus + Loner gate)
  tournament.cpp/hpp Swiss bracket: pairing, standings, runTournament
                     match pool (buildMatchPool / sampleMatch) for fast sampling
  block.hpp          Block dice resolution, armour/injury rolls, apothecary,
                     crowd surf helper, resolvePush (Stand Firm / Sidestep / Grab)
  dice.hpp           Thread-local RNG wrapper (mt19937_64)
  models.hpp         All data structures, skill bitmask (SK:: constants),
                     GameContext (weather state per game), StarPlayer
tools/
  tourplay_convert.cpp  TourPlay zip → tournament JSON converter
bloodbowl-2025-seed.json   31 races, 175 skills, star player catalogue
data/              Sample match and tournament JSON files (zip files gitignored)
```
