cmake_minimum_required(VERSION 3.25)

project(dnd VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# Собираем все зависимости статически, чтобы исполняемый файл был автономным.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

set(SDL3_VERSION       release-3.4.20)
set(SDL3_TTF_VERSION   release-3.2.2)
set(SDL3_IMAGE_VERSION release-3.4.8)
set(SDL3_NET_VERSION   release-3.2.0)

# Ядро SDL3: только статически, без тестов, примеров и установки
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)

# SDL3_ttf: встроенные FreeType/HarfBuzz/plutosvg, без примеров
set(SDLTTF_VENDORED ON CACHE BOOL "" FORCE)
set(SDLTTF_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDLTTF_INSTALL OFF CACHE BOOL "" FORCE)
set(SDLTTF_INSTALL_CPACK OFF CACHE BOOL "" FORCE)

# SDL3_image: встроенные декодеры, только часто нужные форматы
set(SDLIMAGE_VENDORED ON CACHE BOOL "" FORCE)
set(SDLIMAGE_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_TESTS OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_INSTALL OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_INSTALL_CPACK OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_AVIF OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_JXL OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_TIF OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_WEBP OFF CACHE BOOL "" FORCE)

# SDL3_net: без примеров
set(SDLNET_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDLNET_INSTALL OFF CACHE BOOL "" FORCE)

# Дочерние библиотеки хранят встроенные декодеры (FreeType, HarfBuzz,
# libpng, ...) в git-подмодулях, поэтому они скачиваются git-клонами.
# Обычный архив исходников был бы без каталогов external/.
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

# SDL3 должен быть доступен первым: дочерние библиотеки переиспользуют
# SDL3::Headers и SDL3::SDL3, если эти цели уже определены, вместо поиска
# установленного пакета SDL3.
FetchContent_MakeAvailable(SDL3 SDL3_image SDL3_ttf SDL3_net)

# io.h из MinGW объявляет POSIX read()/write(), которые конфликтуют со
# статическими Windows-хелперами с теми же именами внутри SDL_net.
# NO_OLDNAMES скрывает эти устаревшие объявления только для единиц
# трансляции SDL_net.
if(WIN32 AND TARGET SDL3_net-static)
  target_compile_definitions(SDL3_net-static PRIVATE NO_OLDNAMES)
elseif(WIN32 AND TARGET SDL3_net-shared)
  target_compile_definitions(SDL3_net-shared PRIVATE NO_OLDNAMES)
endif()

add_library(dnd_core STATIC
  src/core/aoe.cpp
  src/core/bestiary.cpp
  src/core/byte_io.cpp
  src/core/campaign.cpp
  src/core/campaign_io.cpp
  src/core/client_view.cpp
  src/core/conditions.cpp
  src/core/dice.cpp
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
