# Blood Bowl 2025 Simulator

A Monte Carlo simulator for Blood Bowl 2020/2025. Runs thousands of games in parallel to estimate win rates, scoring patterns, and casualty statistics for any matchup or Swiss tournament bracket.

## Requirements

- C++23 compiler (GCC 13+ or Clang 17+)
- CMake 3.28+
- OpenMP
- Internet connection on first build (CMake fetches [nlohmann/json](https://github.com/nlohmann/json) v3.11.3 automatically)

## Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

The build copies `bloodbowl-2025-seed.json` and all files from `data/` into the build directory automatically.

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

### Turn structure

Each turn proceeds in this order:

1. **Stand-up phase**: stunned players recover; prone players stand (simplified — no MA cost)
2. **Blocking phase**: up to 3 offensive blocks (each blocker activates once)
   - A successful push causes the blocker to follow up one zone forward, opening a lane for the carrier
3. **Defense blitz**: one defender may move into the carrier's zone and attempt a block
   - Probability varies with turns remaining: 67% early (turns 6–8), 50% mid (3–5), 33% late (1–2), reflecting fresh vs depleted defenses
4. **Ball carrier action**: carrier either passes (if a receiver is open ahead) or runs

**Per-turn action limits enforced (BB2020 §8):**
- One Blitz action per team per turn
- One Pass action per team per turn
- Each player activates at most once per turn

### Skills simulated

| Skill | Effect |
|---|---|
| **Block** | Re-roll Both Down results |
| **Wrestle** | Accept Both Down to bring down opponent |
| **Dodge** | Re-roll one failed dodge per activation |
| **Tackle** | Cancels opponent's Dodge skill |
| **Sure Hands** | Re-roll failed pickup |
| **Catch** | Re-roll failed catch |
| **Pass** | Re-roll failed pass throw |
| **Accurate** | −1 to PA target number |
| **Diving Catch** | −1 to AG catch target (easier to catch) |
| **Diving Tackle** | Fall prone after opponent dodges away to force an extra dodge roll |
| **Stand Firm** | Resist being pushed back when blocked |
| **Mighty Blow** | +1 to armour/injury rolls |
| **Claws** | Armour rolls of 8+ break armour regardless of AV |
| **Guard** | Cancels one effective tackle zone on the ball carrier when in the same zone (cage effect) |
| **Sprint** | One extra zone-crossing attempt per activation |
| **Sure Feet** | Re-roll a failed Sprint roll |
| **Nerves of Steel** | Ignore tackle zone penalties on pass and catch rolls |
| **Pro** | Once per activation: re-roll any failed roll on 3+ |
| **Regeneration** | On a Casualty result: roll 4+ to place in Reserves instead (after apothecary) |
| **Swarming** | Snotling Linemen only — D3 enter the pitch from Reserves each drive, capped by on-pitch Swarming count |

All 175 skills from the 2025 rulebook are stored in a bitmask for O(1) lookup. Skills not listed above are loaded and displayed correctly but have no active simulation effect.

### Injury and recovery

- **Armour roll** (2d6 > AV): determines if the player is injured after being knocked down
- **Injury roll** (2d6): Stunned (2–7), KO (8–9), Casualty (10–12)
- **Apothecary**: once per game, on a Casualty result — opponent re-rolls, coach takes the better outcome
- **Regeneration**: after apothecary, 4+ converts Casualty to Reserves (modelled as KO)
- **KO recovery**: 4+ roll at every drive kickoff

### Special rules

| Rule | Teams | Effect |
|---|---|---|
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

### Parallelism

- Games are distributed across all available CPU cores with OpenMP
- Multiple match files run in parallel (outer loop) with inner per-game parallelism
- Tournament mode parallelises across tournament runs with per-thread RNG seeds and accumulators to avoid contention

---

## Architecture

```
src/
  main.cpp          CLI, argument parsing, output formatting
  loader.cpp/hpp    JSON parsing for match, tournament, and seed files
  simulator.cpp/hpp Game engine: buildTeamState, simulateGame, runSimulations
  tournament.cpp/hpp Swiss bracket: pairing, standings, runTournament
  block.hpp         Block dice resolution, armour/injury rolls, apothecary
  dice.hpp          Thread-local RNG wrapper (mt19937_64)
  models.hpp        All data structures, skill bitmask (SK:: constants)
bloodbowl-2025-seed.json   31 races, 175 skills, star player catalogue
data/              Sample match and tournament JSON files
```
