#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/cell.h"
#include "core/damage.h"
#include "core/light.h"

namespace dnd {

// Рельеф клетки. Стена перекрывает и движение, и обзор; дерево проходимо,
// но стоит вдвое дороже и сокращает обзор; вода проходима плаванием (по
// свойствам клетки), пропасть — единственный непроходимый рельеф после стены;
// лава проходима и жжёт.
enum class Terrain : std::uint8_t {
  kFloor = 0,
  kWall = 1,
  kTree = 2,
  kWater = 3,
  kPit = 4,
  kLava = 5,
  // Болото, грязь, снег и заросли — отдельный вид, но одно правило:
  // трудная местность. Лестница проходима с обычной стоимостью.
  kSwamp = 6,
  kMud = 7,
  kSnow = 8,
  kBrush = 9,
  kLadder = 10,
};

inline constexpr std::uint8_t kTerrainCount =
    static_cast<std::uint8_t>(Terrain::kLadder) + 1;

// Глубина воды — свойство клетки, а не рельеф: мелкая вода стоит вдвое,
// глубокая требует плавания. Старый рельеф kWater читается как глубокая вода.
enum class WaterDepth : std::uint8_t {
  kDry = 0,
  kShallow = 1,
  kDeep = 2,
};

// Биты свойств клетки поверх рельефа.
inline constexpr std::uint8_t kCellRoad = 1U << 0U;
inline constexpr std::uint8_t kCellClimbable = 1U << 1U;
inline constexpr std::uint8_t kCellSlippery = 1U << 2U;
inline constexpr std::uint8_t kCellAllFlags =
    kCellRoad | kCellClimbable | kCellSlippery;

// Укрытие по стороне клетки: 2 бита на сторону. Стороны по часам от севера:
// 0 — север, 1 — восток, 2 — юг, 3 — запад.
enum class CoverLevel : std::uint8_t {
  kNone = 0,
  kHalf = 1,
  kThreeQuarters = 2,
  kFull = 3,
};

inline constexpr int kCoverSides = 4;
[[nodiscard]] auto CoverOnSide(std::uint8_t sides, int side) -> CoverLevel;
auto SetCoverOnSide(std::uint8_t& sides, int side, CoverLevel level) -> void;
// Сторона клетки, через которую идёт взгляд из from в to: у цели это сторона,
// обращённая к атакующему.
[[nodiscard]] auto SideTowards(Cell from, Cell to) -> int;

// Бонус к КД и спасброскам Ловкости от укрытия (SRD 5.2.1).
[[nodiscard]] auto CoverBonus(CoverLevel level) -> int;

// Профиль урона при касании. Клетка хранит только индекс в палитре карты.
enum class HazardTrigger : std::uint8_t {
  kEnter = 1U << 0U,
  kTurnStart = 1U << 1U,
  kTurnEnd = 1U << 2U,
  kTouch = 1U << 3U,
};

inline constexpr std::uint8_t kHazardEnter =
    static_cast<std::uint8_t>(HazardTrigger::kEnter);
inline constexpr std::uint8_t kHazardTurnStart =
    static_cast<std::uint8_t>(HazardTrigger::kTurnStart);
inline constexpr std::uint8_t kHazardTurnEnd =
    static_cast<std::uint8_t>(HazardTrigger::kTurnEnd);
inline constexpr std::uint8_t kHazardTouch =
    static_cast<std::uint8_t>(HazardTrigger::kTouch);
inline constexpr std::uint8_t kHazardAllTriggers =
    kHazardEnter | kHazardTurnStart | kHazardTurnEnd | kHazardTouch;

struct HazardProfile {
  // Выражение урона (core/dice); пустое — клетка без урона.
  std::string damage;
  DamageType type = DamageType::kNone;
  std::uint8_t triggers = 0;
  // 0 — без спасброска; иначе успех делит урон пополам.
  int save_dc = 0;
  bool half_on_success = true;

  friend auto operator==(const HazardProfile&, const HazardProfile&)
      -> bool = default;
};

// Индекс 0 в палитре — «нет урона»; дальше готовые профили DMG 2024.
inline constexpr std::uint8_t kNoHazard = 0;
inline constexpr std::uint8_t kLavaHazard = 1;

[[nodiscard]] auto DefaultHazards() -> std::vector<HazardProfile>;
[[nodiscard]] auto IsValidHazardTriggers(std::uint8_t triggers) -> bool;

// Течение: сила 1–3 задаёт сдвиг в клетках и КС проверки Силы (Атлетика).
[[nodiscard]] constexpr auto CurrentShiftCells(int strength) -> int {
  return std::clamp(strength, 0, 3);
}
[[nodiscard]] constexpr auto CurrentDc(int strength) -> int {
  return 10 + (5 * CurrentShiftCells(strength));
}
// Единичное смещение направления течения: 1 — восток, 3 — юг, 0 — нет.
[[nodiscard]] auto CurrentDirectionOffset(std::uint8_t direction) -> Cell;

// Свойства клетки поверх рельефа. Старые сцены читаются нулевыми.
struct CellProps {
  std::uint8_t flags = 0;
  WaterDepth water = WaterDepth::kDry;
  // 0 — нет течения, 1..8 — по часам от востока (как направление шаблона).
  std::uint8_t current_dir = 0;
  std::uint8_t current_strength = 0;
  std::uint8_t hazard = kNoHazard;
  int slippery_dc = 0;
  std::uint8_t cover_sides = 0;
  // Освещённость клетки: LightLevel или kAmbientLight («как вся сцена»).
  std::uint8_t light = kAmbientLight;
  // Затемнение на клетке: Obscurement.
  Obscurement obscurement = Obscurement::kNone;

  friend auto operator==(const CellProps&, const CellProps&) -> bool = default;
};

// Как токен проходит клетки: скорость, плавание и лазание из листа.
// GridMap о токенах не знает, поэтому профиль передаёт вызывающий.
struct MovementProfile {
  int speed = 0;
  // 0 — скорости нет: плавание и лазание всё равно возможны, но дороже.
  int swim_speed = 0;
  int climb_speed = 0;
};

class GridMap {
 public:
  GridMap(int width, int height);

  [[nodiscard]] auto Width() const -> int { return width_; }
  [[nodiscard]] auto Height() const -> int { return height_; }
  [[nodiscard]] auto Contains(Cell cell) const -> bool;
  [[nodiscard]] auto TerrainAt(Cell cell) const -> Terrain;
  [[nodiscard]] auto PropsAt(Cell cell) const -> const CellProps&;
  [[nodiscard]] auto Hazards() const -> const std::vector<HazardProfile>& {
    return hazards_;
  }
  [[nodiscard]] auto HazardAt(Cell cell) const -> const HazardProfile*;
  // Непроходимый рельеф: только стена и пропасть. Вода проходима плаванием.
  [[nodiscard]] auto IsBlocked(Cell cell) const -> bool;
  // Перекрывает ли клетка линию обзора. Стена — да, остальной рельеф — нет.
  // Дерево даёт слабое затемнение и обзор не режет (core/visibility).
  [[nodiscard]] auto BlocksSight(Cell cell) const -> bool;
  // Затемнение клетки: свойство либо дерево (слабое). Сильное затемнение
  // (тьма, дым) обрывает луч и прячет существ.
  [[nodiscard]] auto ObscurementAt(Cell cell) const -> Obscurement;
  // Освещённость всей сцены для клеток с light == kAmbientLight.
  [[nodiscard]] auto AmbientLight() const -> LightLevel {
    return ambient_light_;
  }
  auto SetAmbientLight(LightLevel level) -> void { ambient_light_ = level; }
  // Клетка стоит двойного движения (2024: difficult terrain).
  [[nodiscard]] auto IsDifficultTerrain(Cell cell) const -> bool;
  // Стоимость входа в клетку в 5-футовых шагах (1–3); -1 — непроходима.
  // Дорога отменяет трудную местность, но не лазание и не плавание:
  // плавание/лазание — +1, трудная местность — ещё +1 (в сумме до 3).
  [[nodiscard]] auto MovementCost(Cell cell,
                                  const MovementProfile& profile) const -> int;

  auto SetTerrain(Cell cell, Terrain terrain) -> void;
  // Совместимость со старым API: true — стена, false — пол.
  auto SetBlocked(Cell cell, bool blocked) -> void;
  auto SetProps(Cell cell, CellProps props) -> void;
  auto SetHazards(std::vector<HazardProfile> hazards) -> void;

 private:
  [[nodiscard]] auto Index(Cell cell) const -> std::size_t;
  // Согласовать свойства с рельефом после кисти: вода, лава.
  auto ApplyTerrainProps(Cell cell) -> void;

  int width_;
  int height_;
  std::vector<std::uint8_t> terrain_;
  std::vector<CellProps> props_;
  std::vector<HazardProfile> hazards_;
  LightLevel ambient_light_ = LightLevel::kBright;
};

// Результат прокладки пути по опциональному правилу 5–10–5: первая
// диагональ стоит 5 футов (1 клетку), вторая — 10 футов (2 клетки), затем
// снова 5 и так далее. Ортогональный шаг всегда стоит 1 клетку.
struct PathCost {
  int cost = 0;
  // Чётность диагоналей после пути: 0 — следующая диагональ стоит 5 футов,
  // 1 — 10 футов. Хранится в токене, чтобы счёт не сбрасывался между ходами.
  int diagonal_parity = 0;
};

// Клетки, достижимые из origin за budget (в тех же 5-футовых единицах).
// Стены, blocked и углы между двумя препятствиями непроходимы.
// diagonal_parity — сколько диагоналей уже пройдено в этом ходу (по модулю 2).
[[nodiscard]] auto ReachableCells(const GridMap& map, Cell origin, int budget,
                                  int diagonal_parity = 0,
                                  std::span<const Cell> blocked = {},
                                  const MovementProfile& profile = {})
    -> std::vector<Cell>;

// Минимальная стоимость пути с учётом 5–10–5. Пусто, если клетка недостижима,
// за стеной, занята или путь срезал бы угол между двумя препятствиями.
[[nodiscard]] auto PathCostCells(const GridMap& map, Cell from, Cell to,
                                 int diagonal_parity = 0,
                                 std::span<const Cell> blocked = {},
                                 const MovementProfile& profile = {})
    -> std::optional<PathCost>;

// Лучшее укрытие на линии взгляда от attacker к target: учитываются стороны
// всех пересечённых клеток, стена даёт полное укрытие. Укрытие от существ не
// моделируется — его решает DM.
[[nodiscard]] auto CoverBetween(const GridMap& map, Cell attacker, Cell target)
    -> CoverLevel;

}  // namespace dnd
