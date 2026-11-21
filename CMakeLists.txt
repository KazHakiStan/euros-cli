cmake_minimum_required(VERSION 3.20)
project(euros C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

set(EUROS_VERSION "0.0.0-dev" CACHE STRING "Version baked into the binary (release builds: the tag without v)")
set(EUROS_REPO "KazHakiStan/euros-cli" CACHE STRING "GitHub owner/name that releases and installers come from")
option(EUROS_STATIC "Link libcurl and libsodium statically (release builds on Linux and macOS)" OFF)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl>=8.11) # WebSocket API is stable since 8.11

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter)
endif()

add_library(cjson STATIC third_party/cjson/cJSON.c)
target_include_directories(cjson PUBLIC third_party/cjson)
target_compile_options(cjson PRIVATE -w)

add_library(stb STATIC third_party/stb/stb.c)
target_include_directories(stb PUBLIC third_party/stb)
target_compile_options(stb PRIVATE -w)

# Everything except main(): shared by the app and the tests.
add_library(euros_core STATIC
  src/platform.c
  src/term.c
  src/cli.c
  src/markup.c
  src/version.c
  src/config.c
  src/http.c
  src/keys.c
  src/session.c
  src/api.c
  src/ws.c
  src/realtime.c
  src/room.c
  src/input.c
  src/lineedit.c
  src/chat.c
  src/letters.c
  src/forum.c
  src/picture.c
  src/update.c
)
target_include_directories(euros_core PUBLIC src)
target_compile_definitions(euros_core PUBLIC
  EUROS_VERSION="${EUROS_VERSION}"
  EUROS_REPO="${EUROS_REPO}"
)
target_link_libraries(euros_core PUBLIC cjson stb Threads::Threads)

if(EUROS_STATIC)
  # Archives by full path: next to a .dylib/.so the linker would otherwise pick the shared one.
  find_library(CURL_ARCHIVE NAMES libcurl.a HINTS ${CURL_LIBRARY_DIRS} NO_DEFAULT_PATH REQUIRED)
  find_library(SODIUM_ARCHIVE NAMES libsodium.a HINTS ${SODIUM_LIBRARY_DIRS} REQUIRED)
  target_include_directories(euros_core PUBLIC ${CURL_INCLUDE_DIRS} ${SODIUM_INCLUDE_DIRS})
  target_compile_definitions(euros_core PUBLIC CURL_STATICLIB SODIUM_STATIC)
  target_link_libraries(euros_core PUBLIC ${CURL_ARCHIVE} ${SODIUM_ARCHIVE})
  if(APPLE)
    find_library(CORE_FOUNDATION CoreFoundation REQUIRED)
    find_library(SECURITY Security REQUIRED)
    find_library(SYSTEM_CONFIGURATION SystemConfiguration REQUIRED)
    target_link_libraries(euros_core PUBLIC ${CORE_FOUNDATION} ${SECURITY} ${SYSTEM_CONFIGURATION} z)
  else()
    target_link_libraries(euros_core PUBLIC ssl crypto z)
    target_link_options(euros_core INTERFACE -static)
  endif()
else()
  target_link_libraries(euros_core PUBLIC PkgConfig::SODIUM PkgConfig::CURL)
endif()

target_link_libraries(euros_core PUBLIC m)
if(WIN32)
  target_link_libraries(euros_core PUBLIC ws2_32 shell32) # select() on the WebSocket; UTF-16 argv
endif()

add_executable(euros src/main.c)
target_link_libraries(euros PRIVATE euros_core)

enable_testing()
add_executable(test_vectors tests/test_vectors.c)
target_link_libraries(test_vectors PRIVATE euros_core)
add_test(NAME crypto_vectors
         COMMAND test_vectors ${CMAKE_CURRENT_SOURCE_DIR}/tests/crypto-vectors.json)

add_executable(test_room tests/test_room.c)
target_link_libraries(test_room PRIVATE euros_core)
add_test(NAME room_protocol COMMAND test_room)

add_executable(test_letters tests/test_letters.c)
target_link_libraries(test_letters PRIVATE euros_core)
add_test(NAME letters COMMAND test_letters)

add_executable(test_version tests/test_version.c)
target_link_libraries(test_version PRIVATE euros_core)
add_test(NAME version COMMAND test_version)

add_executable(test_markup tests/test_markup.c)
target_link_libraries(test_markup PRIVATE euros_core)
add_test(NAME markup COMMAND test_markup)

add_executable(test_picture tests/test_picture.c)
target_link_libraries(test_picture PRIVATE euros_core)
add_test(NAME picture COMMAND test_picture)
