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

# ── TourPlay scraper (optional — requires libcurl) ────────────────────────────
find_package(CURL QUIET)
if(CURL_FOUND)
  add_executable(tourplay_scraper tools/tourplay_scraper.cpp)
  target_include_directories(tourplay_scraper PRIVATE src)
  target_link_libraries(tourplay_scraper
    PRIVATE
      CURL::libcurl
      nlohmann_json::nlohmann_json
  )
  message(STATUS "tourplay_scraper: ENABLED  (libcurl ${CURL_VERSION_STRING})")
else()
  message(STATUS "tourplay_scraper: DISABLED (libcurl not found — install libcurl-dev)")
endif()
