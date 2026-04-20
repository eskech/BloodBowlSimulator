#pragma once
#include "models.hpp"
#include <expected>
#include <string>
#include <string_view>

// Load bloodbowl-2025-seed.json
std::expected<SeedData, std::string> loadSeedData(std::string_view path);

// Load a match JSON (two teams + simulation count)
std::expected<MatchConfig, std::string> loadMatchConfig(std::string_view path,
                                                         const SeedData& seed);

// Load a tournament JSON (N teams, rounds, pairing system, etc.)
std::expected<TournamentConfig, std::string> loadTournamentConfig(std::string_view path,
                                                                    const SeedData& seed);
