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
    "name": "Amazon Warriors",
    "race": "Amazons",
    "rerolls": 3,
    "hasApothecary": true,
    "riotousRookies": false,
    "strategy": {
      "wrestle": 0.9,
      "standFirm": 0.0,
      "divingTackle": 0.7,
      "pro": 0.6
    },
    "players": [
      {
        "position": "Eagle Warrior Linewoman",
        "name": "Artemis",
        "extraSkills": ["Wrestle"],
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

| Field | Description |
|---|---|
| `position` | Must match a roster position in the seed data for the team's race |
| `name` | Display name |
| `extraSkills` | Additional skills beyond starting skills (validated against allowed categories) |
| `strategy` | Per-player strategy overrides (merged with team defaults) |

Rosters can have 11–16 players. Positional limits from the seed data are enforced at load time.

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
| `matchGames` | Games per head-to-head match (majority wins the match) |
| `pairingSystem` | `"dutch"` (top half vs bottom half) or `"monrad"` (consecutive pairs) |

**Scoring:** Win = 3 pts, Draw = 1 pt, Loss = 0 pt. Tiebreakers: net score → net casualties → random.

### Seed file

`bloodbowl-2025-seed.json` contains all 31 races, their roster positions with stats and starting skills, and the full 175-skill catalogue. It is loaded at startup and is required for validation. Normally it does not need to be edited.

## Sample data files

| File | Description |
|---|---|
| `example_match.json` | Amazons vs Dwarves |
| `humans_vs_orcs.json` | Humans vs Orcs |
| `owa_vs_dark_elves.json` | Old World Alliance vs Dark Elves |
| `owa_vs_imperial.json` | Old World Alliance vs Imperial Nobility |
| `owa_vs_orcs.json` | Old World Alliance vs Orcs |
| `snotlings_vs_chaos_dwarves.json` | Snotlings (with Riotous Rookies) vs Chaos Dwarves |
| `wood_elves_vs_nurgle.json` | Wood Elves vs Nurgle |
| `example_tournament.json` | 8-team Swiss tournament, Dutch pairing |
| `league_16teams.json` | 16-team Swiss tournament, Monrad pairing |

---

## What is simulated

### Pitch model

The game is modelled as **5 abstract zones** rather than the full 26×15 grid:

```
OwnEndZone → OwnHalf → Midfield → OppHalf → OppEndZone
```

Player positions, tackle zones, and movement are all zone-based. Individual square coordinates are not tracked.

### Game flow

- **Two halves** of 8 turns each, both teams alternating turns per half
- **Coin toss** determines which team receives first
- **Kickoff** at the start of each half and after every touchdown
- **KO recovery** at every kickoff: each KO'd player rolls 4+ to return
- **Touchdown** ends the current drive and resets positions

### Turn actions (per-turn limits enforced)

Each turn the following BB2020 action limits are enforced:
- One **Blitz** action per team per turn (defence may also blitz the ball carrier)
- One **Pass** action per team per turn
- Blocking phase: up to 3 Block actions per turn (simplified)
- Each player may only **activate once** per turn

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
| **Guard** | Counts as an assist even when in a tackle zone |
| **Sprint** | One extra zone-crossing attempt per activation |
| **Sure Feet** | Re-roll a failed Sprint roll |
| **Nerves of Steel** | Ignore tackle zone penalties on pass and catch |
| **Pro** | Once per activation: re-roll any failed roll on 3+ |
| **Regeneration** | On a Casualty result: roll 4+ to place in Reserves instead (after apothecary) |
| **Swarming** | Snotling Linemen only — D3 enter the pitch from Reserves each drive, capped by on-pitch Swarming count |

All 175 skills from the 2025 rulebook are stored in a bitmask for O(1) lookup. Skills not listed above are loaded and displayed correctly but have no active simulation effect.

### Injury and recovery

- **Armour roll** (2d6 > AV): determines if the player is injured after being knocked down
- **Injury roll** (2d6): Stunned (2–7), KO (8–9), Casualty (10–12)
- **Apothecary**: once per game, on a Casualty result — re-roll and take the better outcome
- **Regeneration**: after apothecary, 4+ converts Casualty to Reserves (modelled as KO)
- **KO recovery**: 4+ roll at every drive kickoff

### Inducements

| Inducement | Teams | Effect |
|---|---|---|
| **Riotous Rookies** | Snotlings | Adds 2D3+1 Snotling Lineman Journeymen before the game; they enter via Swarming each drive |

### Swiss tournament

- **Dutch pairing**: sort standings, split into top and bottom halves, pair across
- **Bye**: lowest-ranked team without a previous bye gets a free win (3 pts, +1 net score)
- **Rematch avoidance**: one-pass adjacent swap when pairing would create a rematch
- **Monrad pairing**: sort standings, pair consecutive (1st vs 2nd, 3rd vs 4th, …)

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
