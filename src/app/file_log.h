#pragma once

namespace dnd {

// Имя файла технического журнала рядом с .exe.
inline constexpr const char* kFileLogName = "virtual_dnd_tabletop.log";

// GUI-сборка запускается без окна консоли, поэтому диагностика SDL_Log
// дописывается в virtual_dnd_tabletop.log рядом с .exe: строки прошлых
// запусков сохраняются.
// Установка безопасна до SDL_Init() и повторный вызов ничего не делает.
auto InstallFileLog() -> void;

// Дописывает хвост буфера, закрывает файл и возвращает стандартный вывод
// SDL. Вызывается при выходе, после него InstallFileLog() снова работает.
auto ShutdownFileLog() -> void;

}  // namespace dnd
