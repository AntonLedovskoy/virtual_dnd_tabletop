cmake_minimum_required(VERSION 3.25)

# Версия сборки — из ближайшего тега: на самом теге `v0.5.2`, после коммитов
# `v0.5.2-3-gabc123`, с правками в рабочей копии `...-dirty`. Без git, вне
# репозитория или без тегов — `0.0.0-dirty`: такая сборка считается
# developer build, и автообновление ей выключается.
find_program(DND_GIT_EXECUTABLE NAMES git)
set(DND_VERSION "0.0.0-dirty")
set(DND_VERSION_NUMERIC "0.0.0")
if(DND_GIT_EXECUTABLE)
  execute_process(
    COMMAND ${DND_GIT_EXECUTABLE} describe --tags --always --dirty
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE DND_GIT_DESCRIBE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE DND_GIT_RESULT
  )
  if(DND_GIT_RESULT EQUAL 0 AND
     DND_GIT_DESCRIBE MATCHES
     "^[vV]?([0-9]+)\\.([0-9]+)\\.([0-9]+)(-.*)?$")
    set(DND_VERSION "${DND_GIT_DESCRIBE}")
    set(DND_VERSION_NUMERIC
      "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
  endif()
endif()

project(virtual_dnd_tabletop VERSION ${DND_VERSION_NUMERIC} LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Предупреждения только для собственного кода: зависимости собираются со
# своими настройками и шумят. Недостающие поля в агрегатной инициализации —
# осознанный стиль: у структур есть значения по умолчанию, и перечисляются
# только значимые поля.
set(DND_WARNINGS -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)

include(FetchContent)

# std::thread в фоновой записи кампании: на MinGW нужен winpthreads.
find_package(Threads REQUIRED)

# Собираем все зависимости статически, чтобы исполняемый файл был автономным.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

set(SDL3_VERSION       release-3.4.20)
set(SDL3_TTF_VERSION   release-3.2.2)
set(SDL3_IMAGE_VERSION release-3.4.8)

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

# SDL3 должен быть доступен первым: дочерние библиотеки переиспользуют
# SDL3::Headers и SDL3::SDL3, если эти цели уже определены, вместо поиска
# установленного пакета SDL3.
FetchContent_MakeAvailable(SDL3 SDL3_image SDL3_ttf)

add_library(dnd_core STATIC
  src/core/aoe.cpp
  src/core/attack.cpp
  src/core/autosave.cpp
  src/core/bestiary.cpp
  src/core/byte_io.cpp
  src/core/camera.cpp
  src/core/campaign.cpp
  src/core/campaign_io.cpp
  src/core/campaign_save.cpp
  src/core/chacha20.cpp
  src/core/chat_command.cpp
  src/core/chat_input.cpp
  src/core/client_view.cpp
  src/core/conditions.cpp
  src/core/damage.cpp
  src/core/delete_confirm.cpp
  src/core/dice.cpp
  src/core/editor_paint.cpp
  src/core/game_server.cpp
  src/core/game_state.cpp
  src/core/grid_map.cpp
  src/core/image_name.cpp
  src/core/levels.cpp
  src/core/local_transport.cpp
  src/core/log_history.cpp
  src/core/log_scroll.cpp
  src/core/map_background.cpp
  src/core/menu_model.cpp
  src/core/movement.cpp
  src/core/navigation.cpp
  src/core/password.cpp
  src/core/player_name.cpp
  src/core/random.cpp
  src/core/scene_io.cpp
  src/core/serialization.cpp
  src/core/session_crypto.cpp
  src/core/session_log.cpp
  src/core/sha256.cpp
  src/core/ui_hint.cpp
  src/core/utf8.cpp
  src/core/version.cpp
  src/core/visibility.cpp
  src/core/x25519.cpp
)
target_include_directories(dnd_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(dnd_core PUBLIC Threads::Threads)
target_compile_options(dnd_core PRIVATE ${DND_WARNINGS})
if(WIN32)
  # rand_s из UCRT объявлен только при этом флаге: им заполняются ключи и
  # запросы рукопожатия системным генератором.
  target_compile_definitions(dnd_core PRIVATE _CRT_RAND_S)
endif()

add_library(dnd_net STATIC
  src/net/net_client.cpp
  src/net/net_server.cpp
  src/net/protocol.cpp
  src/net/tcp_socket.cpp
)
target_link_libraries(dnd_net PUBLIC dnd_core)
target_compile_options(dnd_net PRIVATE ${DND_WARNINGS})
if(WIN32)
  target_link_libraries(dnd_net PUBLIC ws2_32)
endif()

add_executable(${PROJECT_NAME}
  src/main.cpp
  src/app/app.cpp
  src/app/file_log.cpp
  src/render/editor_renderer.cpp
  src/render/grid_renderer.cpp
  src/render/menu_renderer.cpp
  src/render/text_renderer.cpp
  src/render/ui_renderer.cpp
)

# GUI-сборка без окна консоли: диагностика идёт в virtual_dnd_tabletop.log
# рядом с .exe.
if(WIN32)
  set_target_properties(${PROJECT_NAME} PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

# Версия из git describe: строка `DND_VERSION` уходит в `--version`, в
# метаданные SDL и в решение об автообновлении (суффикс = developer build).
target_compile_definitions(${PROJECT_NAME} PRIVATE
  SDL_MAIN_USE_CALLBACKS
  DND_VERSION="${DND_VERSION}"
)
target_compile_options(${PROJECT_NAME} PRIVATE ${DND_WARNINGS})

target_link_libraries(${PROJECT_NAME} PRIVATE
  dnd_core
  dnd_net
  SDL3::SDL3
  SDL3_image::SDL3_image
  SDL3_ttf::SDL3_ttf
)

# MinGW по умолчанию тянет libstdc++-6.dll, libgcc_s_seh-1.dll и
# libwinpthread-1.dll из MSYS2: на машине игрока их нет, и exe не стартует.
# Runtime линкуется статически, иначе архив релиза — не один файл.
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_link_options(${PROJECT_NAME} PRIVATE
    -static-libgcc
    -static-libstdc++
    -static
  )
endif()

enable_testing()

add_executable(dnd_core_tests
  tests/core_tests.cpp
)
target_link_libraries(dnd_core_tests PRIVATE dnd_core)
target_compile_options(dnd_core_tests PRIVATE ${DND_WARNINGS})

add_test(NAME core_tests COMMAND dnd_core_tests)

add_executable(dnd_net_tests
  tests/net_tests.cpp
)
target_link_libraries(dnd_net_tests PRIVATE dnd_net)
target_compile_options(dnd_net_tests PRIVATE ${DND_WARNINGS})

add_test(NAME net_tests COMMAND dnd_net_tests)
