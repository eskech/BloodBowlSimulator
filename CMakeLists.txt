cmake_minimum_required(VERSION 3.28)
project(BloodBowlSimulator VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build type
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler flags
add_compile_options(
  -Wall -Wextra -Wpedantic
  $<$<CONFIG:Release>:-O3>
  $<$<CONFIG:Release>:-march=native>
  $<$<CONFIG:Debug>:-g>
  $<$<CONFIG:Debug>:-O0>
)

# Profile-Guided Optimisation (PGO)
#
#   Step 1 — instrument build:
#     cmake .. -DCMAKE_BUILD_TYPE=Release -DPGO=instrument
#     make -j$(nproc)
#     ./bloodbowl --tournament <file.json>     # representative workload
#
#   Step 2 — optimised build using collected profiles:
#     cmake .. -DPGO=optimize
#     make -j$(nproc)
#
#   Typical gain: 10-20% on this branch-heavy workload.
#   Reset to normal build with:  cmake .. -DPGO=off  (or omit the flag)
set(PGO "off" CACHE STRING "PGO mode: off | instrument | optimize")
if(PGO STREQUAL "instrument")
  add_compile_options(-fprofile-generate)
  add_link_options(-fprofile-generate)
elseif(PGO STREQUAL "optimize")
  add_compile_options(-fprofile-use -fprofile-correction)
  add_link_options(-fprofile-use)
endif()

# OpenMP
find_package(OpenMP REQUIRED)

# nlohmann/json (single-header via FetchContent)
include(FetchContent)
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# miniz (ZIP reading for tourplay-convert)
FetchContent_Declare(
  miniz
  GIT_REPOSITORY https://github.com/richgel999/miniz.git
  GIT_TAG        3.0.2
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(miniz)

# Sources
set(SOURCES
  src/main.cpp
  src/loader.cpp
  src/simulator.cpp
  src/tournament.cpp
)

add_executable(bloodbowl ${SOURCES})

target_include_directories(bloodbowl PRIVATE src)

target_link_libraries(bloodbowl
  PRIVATE
    OpenMP::OpenMP_CXX
    nlohmann_json::nlohmann_json
)

# Copy seed data to build dir
configure_file(
  ${CMAKE_SOURCE_DIR}/bloodbowl-2025-seed.json
  ${CMAKE_BINARY_DIR}/bloodbowl-2025-seed.json
  COPYONLY
)

# Copy all match files from data/ to build dir
file(GLOB MATCH_FILES ${CMAKE_SOURCE_DIR}/data/*.json)
foreach(MATCH_FILE ${MATCH_FILES})
  get_filename_component(MATCH_NAME ${MATCH_FILE} NAME)
  configure_file(${MATCH_FILE} ${CMAKE_BINARY_DIR}/${MATCH_NAME} COPYONLY)
endforeach()

# tourplay-convert — standalone TourPlay zip → tournament JSON converter
add_executable(tourplay-convert tools/tourplay_convert.cpp)
target_link_libraries(tourplay-convert
  PRIVATE
    miniz
    nlohmann_json::nlohmann_json
)
