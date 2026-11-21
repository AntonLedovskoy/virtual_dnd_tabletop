#pragma once

#include <cstddef>
#include <optional>

namespace dnd {

// Навигация по спискам стола (сцены, файлы кампаний): вынесена из App, чтобы
// крайние случаи проверялись тестами без окна.

// Соседний индекс по кругу. Пустой список — 0.
[[nodiscard]] auto WrapIndex(std::size_t current, int delta, std::size_t count)
    -> std::size_t;

// Соседний индекс без перехода через край: стрелка на первом/последнем
// элементе ничего не меняет.
[[nodiscard]] auto StepIndex(std::size_t current, int delta, std::size_t count)
    -> std::size_t;

// Что открыть после удаления элемента removed, если осталось remaining.
// Удалённая первая сцена заменяется следующей, иначе открывается предыдущая.
[[nodiscard]] auto IndexAfterDelete(std::size_t removed, std::size_t remaining)
    -> std::size_t;

// Индекс по цифре 1–9; nullopt, если такого элемента нет.
[[nodiscard]] auto IndexForDigit(int digit, std::size_t count)
    -> std::optional<std::size_t>;

}  // namespace dnd
