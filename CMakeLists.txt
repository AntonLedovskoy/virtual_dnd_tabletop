cmake_minimum_required(VERSION 3.25)

project(dnd VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# Build every dependency as a static library so the executable is self-contained.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

set(SDL3_VERSION       release-3.4.20)
set(SDL3_TTF_VERSION   release-3.2.2)
set(SDL3_IMAGE_VERSION release-3.4.8)
set(SDL3_NET_VERSION   release-3.2.0)

# SDL3 core: static only, no tests/examples/install rules
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)

# SDL3_ttf: vendor FreeType/HarfBuzz/plutosvg, no samples
set(SDLTTF_VENDORED ON CACHE BOOL "" FORCE)
set(SDLTTF_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDLTTF_INSTALL OFF CACHE BOOL "" FORCE)
set(SDLTTF_INSTALL_CPACK OFF CACHE BOOL "" FORCE)

# SDL3_image: vendor the decoders, keep only the commonly needed formats
set(SDLIMAGE_VENDORED ON CACHE BOOL "" FORCE)
set(SDLIMAGE_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_TESTS OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_INSTALL OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_INSTALL_CPACK OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_AVIF OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_JXL OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_TIF OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_WEBP OFF CACHE BOOL "" FORCE)

# SDL3_net: no samples
set(SDLNET_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDLNET_INSTALL OFF CACHE BOOL "" FORCE)

# The satellite libraries keep their vendored decoders (FreeType, HarfBuzz,
# libpng, ...) in git submodules, so they are fetched as git clones. A plain
# source archive would lack the external/ directories.
FetchContent_Declare(
  SDL3
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG ${SDL3_VERSION}
  GIT_SHALLOW TRUE
  GIT_PROGRESS TRUE
  SYSTEM
)
FetchContent_Declare(
  SDL3_ttf
  GIT_REPOSITORY https://github.com/libsdl-org/SDL_ttf.git
  GIT_TAG ${SDL3_TTF_VERSION}
  GIT_SHALLOW TRUE
  GIT_SUBMODULES_RECURSE TRUE
  GIT_PROGRESS TRUE
  SYSTEM
)
FetchContent_Declare(
  SDL3_image
  GIT_REPOSITORY https://github.com/libsdl-org/SDL_image.git
  GIT_TAG ${SDL3_IMAGE_VERSION}
  GIT_SHALLOW TRUE
  GIT_SUBMODULES_RECURSE TRUE
  GIT_PROGRESS TRUE
  SYSTEM
)
FetchContent_Declare(
  SDL3_net
  GIT_REPOSITORY https://github.com/libsdl-org/SDL_net.git
  GIT_TAG ${SDL3_NET_VERSION}
  GIT_SHALLOW TRUE
  GIT_PROGRESS TRUE
  SYSTEM
)

# SDL3 must be available first: the satellite libraries reuse SDL3::Headers and
# SDL3::SDL3 when those targets are already defined instead of looking for an
# installed SDL3 package.
FetchContent_MakeAvailable(SDL3 SDL3_image SDL3_ttf SDL3_net)

# MinGW's io.h declares POSIX read()/write(), which clash with the static
# Windows helpers of the same name inside SDL_net. NO_OLDNAMES hides those
# legacy declarations for the SDL_net translation units only.
if(WIN32 AND TARGET SDL3_net-static)
  target_compile_definitions(SDL3_net-static PRIVATE NO_OLDNAMES)
elseif(WIN32 AND TARGET SDL3_net-shared)
  target_compile_definitions(SDL3_net-shared PRIVATE NO_OLDNAMES)
endif()

add_library(dnd_core STATIC
  src/core/byte_io.cpp
  src/core/client_view.cpp
  src/core/game_server.cpp
  src/core/game_state.cpp
  src/core/grid_map.cpp
  src/core/local_transport.cpp
  src/core/scene_io.cpp
  src/core/serialization.cpp
  src/core/utf8.cpp
  src/core/visibility.cpp
)
target_include_directories(dnd_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_library(dnd_net STATIC
  src/net/net_client.cpp
  src/net/net_server.cpp
  src/net/protocol.cpp
  src/net/tcp_socket.cpp
)
target_link_libraries(dnd_net PUBLIC dnd_core)
if(WIN32)
  target_link_libraries(dnd_net PUBLIC ws2_32)
endif()

add_executable(${PROJECT_NAME}
  src/main.cpp
  src/app/app.cpp
  src/render/editor_renderer.cpp
  src/render/grid_renderer.cpp
  src/render/menu_renderer.cpp
  src/render/text_renderer.cpp
  src/render/ui_renderer.cpp
)

target_compile_definitions(${PROJECT_NAME} PRIVATE SDL_MAIN_USE_CALLBACKS)

target_link_libraries(${PROJECT_NAME} PRIVATE
  dnd_core
  dnd_net
  SDL3::SDL3
  SDL3_image::SDL3_image
  SDL3_ttf::SDL3_ttf
  SDL3_net::SDL3_net
)

enable_testing()

add_executable(dnd_core_tests
  tests/core_tests.cpp
)
target_link_libraries(dnd_core_tests PRIVATE dnd_core)

add_test(NAME core_tests COMMAND dnd_core_tests)

add_executable(dnd_net_tests
  tests/net_tests.cpp
)
target_link_libraries(dnd_net_tests PRIVATE dnd_net)

add_test(NAME net_tests COMMAND dnd_net_tests)
