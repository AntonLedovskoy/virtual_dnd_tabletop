#include "core/game_state.h"

#include <algorithm>
#include <iterator>
#include <random>
#include <string_view>
#include <type_traits>
#include <utility>

#include "core/conditions.h"
#include "core/hit_points.h"
#include "core/levels.h"
#include "core/player_name.h"
#include "core/units.h"

namespace dnd {
namespace {

// Бонусы листа ограничены так же, как в командах сервера.
constexpr int kMaxSheetBonus = 100;

// Порядок открытых клеток: по строкам, затем по столбцам. Тот же порядок
// дают обходы области в RevealArea/HideArea, поэтому слияния идут за один
// проход, а IsRevealed обходится двоичным поиском.
auto CellOrder(const Cell& left, const Cell& right) -> bool {
  if (left.y != right.y) {
    return left.y < right.y;
  }
  return left.x < right.x;
}

// Клетки карты внутри круга, по возрастанию CellOrder.
auto CellsInArea(const GridMap& map, Cell center, int radius)
    -> std::vector<Cell> {
  std::vector<Cell> cells;
  const int radius_squared = radius * radius;
  for (int y = center.y - radius; y <= center.y + radius; ++y) {
    for (int x = center.x - radius; x <= center.x + radius; ++x) {
      const int delta_x = x - center.x;
      const int delta_y = y - center.y;
      if ((delta_x * delta_x) + (delta_y * delta_y) > radius_squared) {
        continue;
      }
      const Cell cell{.x = x, .y = y};
      if (map.Contains(cell)) {
        cells.push_back(cell);
      }
    }
  }
  return cells;
}

auto AddConditionIfMissing(Token& token, Condition condition) -> bool {
  const bool exists = std::ranges::any_of(
      token.conditions, [&condition](const Condition& other) -> bool {
        return other.name == condition.name;
      });
  if (exists) {
    return false;
  }
  token.conditions.push_back(std::move(condition));
  return true;
}

auto RemoveCondition(Token& token, std::string_view name) -> bool {
  return std::erase_if(token.conditions,
                       [name](const Condition& condition) -> bool {
                         return condition.name == name;
                       }) > 0;
}

// Гибель: «без сознания» заменяется на «мёртв»; стабильность больше не
// действует.
auto KillToken(Token& token) -> void {
  RemoveCondition(token, kUnconscious);
  RemoveCondition(token, kStable);
  AddConditionIfMissing(token, Condition{std::string(kDead)});
}

// Длинный отдых снимает ровно один уровень истощения; уровни хранятся
// отдельными записями состояния.
auto RemoveExhaustionLevel(Token& token) -> bool {
  const auto it = std::ranges::find_if(token.conditions,
                                       [](const Condition& condition) -> bool {
                                         return condition.name == kExhaustion;
                                       });
  if (it == token.conditions.end()) {
    return false;
  }
  token.conditions.erase(it);
  return true;
}

// Стабильный персонаж приходит в себя с 1 HP, когда истекает срок
// «стабилен» (1d4 часа по 2024).
auto RecoverStable(Token& token) -> void {
  token.hp = 1;
  token.death_save_successes = 0;
  token.death_save_failures = 0;
  RemoveCondition(token, kUnconscious);
}

// Прокручивает игровое время для состояний одного токена: срочные
// уменьшаются на seconds, нулевые держатся до ручного снятия. Возвращает
// true, если набор состояний изменился.
auto TickConditions(Token& token, int seconds) -> bool {
  if (seconds <= 0) {
    return false;
  }
  const std::size_t before = token.conditions.size();
  bool stable_expired = false;
  for (auto it = token.conditions.begin(); it != token.conditions.end();) {
    if (it->seconds <= 0) {
      ++it;
      continue;
    }
    it->seconds -= seconds;
    if (it->seconds > 0) {
      ++it;
      continue;
    }
    if (it->name == kStable) {
      stable_expired = true;
    }
    it = token.conditions.erase(it);
  }
  if (stable_expired && token.hp == 0 && !HasCondition(token, kDead)) {
    RecoverStable(token);
  }
  return token.conditions.size() != before;
}

// Отдых по правилам 2024 тоже двигает игровое время.
constexpr int kShortRestSeconds = 1 * kSecondsPerHour;
constexpr int kLongRestSeconds = 8 * kSecondsPerHour;

// Повышение уровня по правилам 2024: за каждый новый уровень максимум HP
// растёт на среднее кости хитов плюс модификатор Телосложения (не меньше 1),
// текущие HP — на столько же, если персонаж не лежит без сознания. На
// рубежах 5/9/13/17 растёт бонус мастерства: он уже включён в итоговые
// бонусы атаки и спасброска листа, поэтому их значение поднимается на 1.
auto ApplyLevelGains(Token& token, int from_level, int to_level, bool gain_hp)
    -> void {
  if (token.kind != TokenKind::kPlayer || to_level <= from_level) {
    return;
  }
  const int hp_gain = HitPointsPerLevel(token.hit_die, token.con_modifier) *
                      (to_level - from_level);
  if (gain_hp) {
    token.max_hp += hp_gain;
  }
  if (gain_hp && token.hp > 0) {
    token.hp += hp_gain;
  }
  const int bonus_gain =
      ProficiencyBonus(to_level) - ProficiencyBonus(from_level);
  if (bonus_gain > 0) {
    token.attack_bonus = std::clamp(token.attack_bonus + bonus_gain,
                                    -kMaxSheetBonus, kMaxSheetBonus);
    token.save_bonus = std::clamp(token.save_bonus + bonus_gain,
                                  -kMaxSheetBonus, kMaxSheetBonus);
  }
}

}  // namespace

GameState::GameState(GridMap map) : map_(std::move(map)) {}

GameState::GameState(GameSnapshot snapshot)
    : map_(std::move(snapshot.map)),
      tokens_(std::move(snapshot.tokens)),
      next_token_id_(snapshot.next_token_id),
      encounter_(std::move(snapshot.encounter)),
      aoe_(snapshot.aoe),
      background_(std::move(snapshot.background)),
      revealed_(std::move(snapshot.revealed)),
      game_time_seconds_(std::max(0, snapshot.game_time_seconds)),
      last_long_rest_seconds_(snapshot.last_long_rest_seconds) {
  last_long_rest_seconds_ =
      std::min(last_long_rest_seconds_, game_time_seconds_);
  // Старые сохранения и события могли прийти в порядке открытия: приводим
  // срез к порядку CellOrder и убираем повторы.
  std::ranges::sort(revealed_, CellOrder);
  const auto duplicates = std::ranges::unique(revealed_);
  revealed_.erase(duplicates.begin(), duplicates.end());
  // Монстры и NPC всегда принадлежат DM: старые файлы и отменённые действия
  // могли оставить их без владельца, а свободных монстров за столом нет.
  for (Token& token : tokens_) {
    if (token.kind != TokenKind::kPlayer && token.owner.empty()) {
      token.owner = std::string(kDmOwner);
    }
    // Инвариант авто-режима: после загрузки, отмены и смены сцены максимум
    // HP совпадает с костями.
    RecomputeAutoHp(token);
  }
}

auto GameState::Snapshot() const -> GameSnapshot {
  return GameSnapshot{
      .map = map_,
      .tokens = tokens_,
      .next_token_id = next_token_id_,
      .encounter = encounter_,
      .aoe = aoe_,
      .background = background_,
      .revealed = revealed_,
      .game_time_seconds = game_time_seconds_,
      .last_long_rest_seconds = last_long_rest_seconds_,
  };
}

auto GameState::TokenCopy(TokenId id) const -> std::optional<Token> {
  const Token* token = FindToken(id);
  if (token == nullptr) {
    return std::nullopt;
  }
  return *token;
}

auto GameState::AddToken(std::string name, Cell cell, std::string owner,
                         TokenKind kind, int max_hp, std::uint32_t color,
                         int ac, int speed, std::string notes, int attack_bonus,
                         int save_bonus, std::string damage, int attack_range,
                         std::uint32_t resist, std::uint32_t vulnerable,
                         std::uint32_t immune, int hit_die, int con_modifier,
                         int hit_dice_count, bool auto_hp) -> TokenId {
  const TokenId id = next_token_id_++;
  const int resolved_max_hp = std::max(1, max_hp);
  Token token{.id = id,
              .name = std::move(name),
              .owner = std::move(owner),
              .hp = resolved_max_hp,
              .max_hp = resolved_max_hp,
              .hit_die = hit_die,
              .con_modifier = con_modifier,
              .auto_hp = auto_hp,
              .hit_dice_count = hit_dice_count,
              .ac = ac,
              .speed = speed,
              .notes = std::move(notes),
              .attack_bonus = attack_bonus,
              .save_bonus = save_bonus,
              .damage = std::move(damage),
              .resist = resist,
              .vulnerable = vulnerable,
              .immune = immune,
              .attack_range = attack_range,
              .kind = kind,
              .color = color,
              .cell = cell};
  // Новый токен только что создан, поэтому сдвиг текущих HP не нужен: в
  // авто-режиме сразу берём максимум из костей.
  if (auto_hp) {
    token.max_hp = AutoMaxHp(token);
    token.hp = token.max_hp;
  }
  tokens_.push_back(std::move(token));
  // В бой новый токен добавляет сервер: он бросает инициативу и ставит
  // участника в конец раунда.
  return id;
}

auto GameState::FindToken(TokenId id) -> Token* {
  for (auto& token : tokens_) {
    if (token.id == id) {
      return &token;
    }
  }
  return nullptr;
}

auto GameState::FindToken(TokenId id) const -> const Token* {
  for (const auto& token : tokens_) {
    if (token.id == id) {
      return &token;
    }
  }
  return nullptr;
}

auto GameState::RemoveToken(TokenId id) -> bool {
  const auto it = std::ranges::find_if(
      tokens_, [id](const Token& token) -> bool { return token.id == id; });
  if (it == tokens_.end()) {
    return false;
  }
  tokens_.erase(it);
  if (encounter_.active) {
    std::erase(encounter_.order, id);
    if (encounter_.order.empty()) {
      encounter_ = Encounter{};
    } else if (encounter_.turn_index >= encounter_.order.size()) {
      encounter_.turn_index = 0;
    }
  }
  return true;
}

auto GameState::SetTokenHp(TokenId id, int hp) -> std::optional<Token> {
  Token* token = FindToken(id);
  if (token == nullptr) {
    return std::nullopt;
  }
  token->hp = std::clamp(hp, 0, token->max_hp);
  return *token;
}

auto GameState::DamageToken(TokenId id, int amount, bool critical)
    -> std::optional<Token> {
  Token* token = FindToken(id);
  if (token == nullptr) {
    return std::nullopt;
  }
  const bool dead = HasCondition(*token, kDead);
  const bool was_down = token->hp == 0 && !dead;
  int remaining = std::max(0, amount);
  const int absorbed = std::min(token->temp_hp, remaining);
  token->temp_hp -= absorbed;
  remaining -= absorbed;
  // Остаток урона, который уходит ниже нуля. Не меньше максимума HP —
  // мгновенная смерть, даже если персонаж ещё стоял.
  const int overflow = std::max(0, remaining - token->hp);
  token->hp = std::max(0, token->hp - remaining);
  if (overflow >= token->max_hp) {
    KillToken(*token);
    return *token;
  }
  if (token->hp == 0 && !was_down && !dead) {
    token->death_save_successes = 0;
    token->death_save_failures = 0;
    AddConditionIfMissing(*token, Condition{std::string(kUnconscious)});
  }
  if (was_down && amount > 0) {
    // Урон снимает стабильность: лежачий снова бросает спасброски. Удар по
    // лежачему — провал спасброска от смерти, критический — два.
    RemoveCondition(*token, kStable);
    const int failures = critical ? 2 : 1;
    token->death_save_failures =
        std::min(3, token->death_save_failures + failures);
    if (token->death_save_failures >= 3) {
      KillToken(*token);
    }
  }
  return *token;
}

auto GameState::HealToken(TokenId id, int amount) -> std::optional<Token> {
  Token* token = FindToken(id);
  if (token == nullptr) {
    return std::nullopt;
  }
  token->hp = std::min(token->max_hp, token->hp + std::max(0, amount));
  token->death_save_successes = 0;
  token->death_save_failures = 0;
  if (token->hp > 0) {
    RemoveCondition(*token, kUnconscious);
    RemoveCondition(*token, kStable);
  }
  return *token;
}

auto GameState::DeathSave(TokenId id, int roll, int penalty, int stable_seconds)
    -> std::optional<Token> {
  Token* token = FindToken(id);
  if (token == nullptr || token->hp > 0) {
    return std::nullopt;
  }
  // Мёртвый и стабильный спасброски не бросают.
  if (HasCondition(*token, kDead) || HasCondition(*token, kStable) ||
      token->death_save_successes >= 3) {
    return std::nullopt;
  }
  if (roll == 20) {
    token->hp = 1;
    token->death_save_successes = 0;
    token->death_save_failures = 0;
    RemoveCondition(*token, kUnconscious);
    return *token;
  }
  const int effective = roll - std::max(0, penalty);
  if (effective >= 10) {
    token->death_save_successes = std::min(3, token->death_save_successes + 1);
    // Три успеха: персонаж стабилен, но всё ещё без сознания на нуле HP.
    // «Без сознания» снимает только лечение, возврат HP или истёкший срок
    // стабильности (1d4 часа).
    if (token->death_save_successes >= 3 && stable_seconds > 0) {
      AddConditionIfMissing(*token,
                            Condition{std::string(kStable), stable_seconds});
    }
    return *token;
  }
  // Натуральная 1 — два провала, остальные числа ниже 10 — один.
  token->death_save_failures =
      std::min(3, token->death_save_failures + (roll == 1 ? 2 : 1));
  if (token->death_save_failures >= 3) {
    KillToken(*token);
  }
  return *token;
}

auto GameState::StabilizeToken(TokenId id, int seconds)
    -> std::optional<Token> {
  Token* token = FindToken(id);
  if (token == nullptr || token->hp > 0 || HasCondition(*token, kDead) ||
      HasCondition(*token, kStable)) {
    return std::nullopt;
  }
  token->death_save_successes = 3;
  AddConditionIfMissing(*token,
                        Condition{std::string(kStable), std::max(0, seconds)});
  return *token;
}

auto GameState::RestParty(bool long_rest, std::mt19937& rng, int max_dice)
    -> std::vector<RestChange> {
  std::vector<RestChange> changed;
  const int rest_seconds = long_rest ? kLongRestSeconds : kShortRestSeconds;
  const int rest_started = game_time_seconds_;
  for (Token& token : tokens_) {
    const int old_hp = token.hp;
    const int old_temp = token.temp_hp;
    const int old_spent = token.hit_dice_spent;
    const int old_successes = token.death_save_successes;
    const int old_failures = token.death_save_failures;
    const std::size_t old_conditions = token.conditions.size();
    // Время отдыха идёт для всех: срочные состояния истекают и у монстров.
    TickConditions(token, rest_seconds);
    // Отдых приносит пользу только персонажу хотя бы с 1 HP: мёртвый,
    // стабильный на нуле и лежачий без сознания не встаёт.
    const bool benefits = token.kind == TokenKind::kPlayer && token.hp > 0 &&
                          !HasCondition(token, kDead);
    int exhaustion_removed = 0;
    if (long_rest) {
      if (benefits) {
        token.hp = token.max_hp;
        token.temp_hp = 0;
        token.hit_dice_spent = 0;
        exhaustion_removed = RemoveExhaustionLevel(token) ? 1 : 0;
      }
    } else if (benefits) {
      // Короткий отдых тратит кости хитов, пока не восстановлен максимум
      // HP, не кончился запас или не достигнут выбор игрока («/rest N»).
      const int level = LevelForXp(token.xp);
      const int available = HitDiceAvailable(level, token.hit_dice_spent);
      const int limit =
          max_dice > 0 ? std::min(max_dice, available) : available;
      std::uniform_int_distribution<int> die(1, token.hit_die);
      int spent = 0;
      while (token.hp < token.max_hp && spent < limit) {
        const int heal = HitDieHeal(die(rng), token.con_modifier);
        token.hp = std::min(token.max_hp, token.hp + heal);
        ++spent;
      }
      token.hit_dice_spent += spent;
      if (token.hp > 0) {
        RemoveCondition(token, kUnconscious);
        token.death_save_successes = 0;
        token.death_save_failures = 0;
      }
    }
    if (token.hp != old_hp || token.temp_hp != old_temp ||
        token.hit_dice_spent != old_spent ||
        token.death_save_successes != old_successes ||
        token.death_save_failures != old_failures ||
        token.conditions.size() != old_conditions) {
      changed.push_back(
          RestChange{.token = token,
                     .dice_spent = token.hit_dice_spent - old_spent,
                     .hp_restored = token.hp - old_hp,
                     .exhaustion_removed = exhaustion_removed});
    }
  }
  if (long_rest) {
    last_long_rest_seconds_ = rest_started;
  }
  game_time_seconds_ =
      std::min(kMaxGameTimeSeconds, game_time_seconds_ + rest_seconds);
  return changed;
}

auto GameState::LongRestReadyInSeconds() const -> int {
  if (last_long_rest_seconds_ < 0) {
    return 0;
  }
  const int elapsed = std::max(0, game_time_seconds_ - last_long_rest_seconds_);
  return std::max(0, kLongRestCooldownSeconds - elapsed);
}

auto GameState::SetInitiative(TokenId id, int value) -> std::optional<Token> {
  Token* token = FindToken(id);
  if (token == nullptr) {
    return std::nullopt;
  }
  const TokenId active = ActiveTokenId();
  token->initiative = value;
  if (encounter_.active) {
    RebuildOrder(active);
  }
  return *token;
}

auto GameState::SetTokenOwner(TokenId id, std::string owner)
    -> std::optional<Token> {
  Token* token = FindToken(id);
  if (token == nullptr) {
    return std::nullopt;
  }
  // Монстров и NPC нельзя ни освободить, ни передать игроку: пока в проекте
  // нет союзных NPC под управлением игрока, их владелец всегда DM.
  if (token->kind != TokenKind::kPlayer && !SamePlayerName(owner, kDmOwner)) {
    return std::nullopt;
  }
  token->owner = std::move(owner);
  return *token;
}

auto GameState::RenameToken(TokenId id, std::string name)
    -> std::optional<Token> {
  Token* token = FindToken(id);
  if (token == nullptr) {
    return std::nullopt;
  }
  token->name = std::move(name);
  return *token;
}

auto GameState::SetTokenMaxHp(TokenId id, int max_hp) -> std::optional<Token> {
  Token* token = FindToken(id);
  if (token == nullptr || token->auto_hp) {
    return std::nullopt;
  }
  token->max_hp = std::max(1, max_hp);
  token->hp = std::clamp(token->hp, 0, token->max_hp);
  return *token;
}

auto GameState::AddXp(TokenId id, int amount) -> std::optional<Token> {
  if (amount <= 0) {
    return std::nullopt;
  }
  Token* token = FindToken(id);
  if (token == nullptr) {
    return std::nullopt;
  }
  const int before = LevelForXp(token->xp);
  token->xp = std::min(kMaxXp, token->xp + amount);
  const int after = LevelForXp(token->xp);
  // В авто-режиме максимум HP выводится из уровня, поэтому его пересчитываем;
  // ручной режим получает прирост за каждый новый уровень.
  ApplyLevelGains(*token, before, after, !token->auto_hp);
  RecomputeAutoHp(*token);
  return *token;
}

auto GameState::SetTokenColor(TokenId id, std::uint32_t color)
    -> std::optional<Token> {
  Token* token = FindToken(id);
  if (token == nullptr) {
    return std::nullopt;
  }
  token->color = color & 0xFFFFFFU;
  return *token;
}

auto GameState::SetTokenPortrait(TokenId id, std::string portrait)
    -> std::optional<Token> {
  if (!portrait.empty() && !IsSafeImageName(portrait)) {
    return std::nullopt;
  }
  Token* token = FindToken(id);
  if (token == nullptr) {
    return std::nullopt;
  }
  token->portrait = std::move(portrait);
  return *token;
}

auto GameState::UpdateTokenSheet(
    TokenId id, int ac, int speed, std::string notes, int attack_bonus,
    int save_bonus, int initiative_bonus, std::string damage, int temp_hp,
    int hit_die, int con_modifier, int attack_range, std::uint32_t resist,
    std::uint32_t vulnerable, std::uint32_t immune, int swim_speed,
    int climb_speed, int darkvision_cells, int max_hp, bool auto_hp,
    int hit_dice_count) -> std::optional<Token> {
  Token* token = FindToken(id);
  if (token == nullptr) {
    return std::nullopt;
  }
  token->ac = ac;
  token->speed = speed;
  token->notes = std::move(notes);
  token->attack_bonus = attack_bonus;
  token->save_bonus = save_bonus;
  token->initiative_bonus = initiative_bonus;
  token->damage = std::move(damage);
  // Временные HP не складываются: остаётся большее (SRD 5.2.1).
  token->temp_hp = std::max(token->temp_hp, temp_hp);
  token->hit_die = hit_die;
  token->con_modifier = con_modifier;
  token->auto_hp = auto_hp;
  token->hit_dice_count = hit_dice_count;
  if (auto_hp) {
    // Правка кости, Телосложения или числа костей сразу меняет максимум.
    RecomputeAutoHp(*token);
  } else {
    token->max_hp = std::max(1, max_hp);
    token->hp = std::clamp(token->hp, 0, token->max_hp);
  }
  token->attack_range = attack_range;
  token->resist = resist & kAllDamageTypes;
  token->vulnerable = vulnerable & kAllDamageTypes;
  token->immune = immune & kAllDamageTypes;
  token->swim_speed = swim_speed;
  token->climb_speed = climb_speed;
  token->darkvision_cells = std::max(0, darkvision_cells);
  return *token;
}

auto GameState::SetTokenLight(TokenId id, int bright_cells, int dim_cells)
    -> std::optional<Token> {
  Token* token = FindToken(id);
  if (token == nullptr) {
    return std::nullopt;
  }
  token->light_bright_cells = std::max(0, bright_cells);
  token->light_dim_cells = std::max(token->light_bright_cells, dim_cells);
  return *token;
}

auto GameState::UpdateTokenCombat(TokenId id, std::vector<Condition> conditions)
    -> std::optional<Token> {
  Token* token = FindToken(id);
  if (token == nullptr) {
    return std::nullopt;
  }
  token->conditions = std::move(conditions);
  // Шестой уровень истощения убивает (SRD 5.2.1).
  if (ExhaustionLevel(*token) >= kMaxExhaustionLevel &&
      !HasCondition(*token, kDead)) {
    token->hp = 0;
    KillToken(*token);
  }
  return *token;
}

auto GameState::AddTokenCondition(TokenId id, Condition condition)
    -> std::optional<Token> {
  Token* token = FindToken(id);
  if (token == nullptr) {
    return std::nullopt;
  }
  AddConditionIfMissing(*token, std::move(condition));
  if (ExhaustionLevel(*token) >= kMaxExhaustionLevel &&
      !HasCondition(*token, kDead)) {
    token->hp = 0;
    KillToken(*token);
  }
  return *token;
}

auto GameState::RemoveTokenCondition(TokenId id, std::string_view name)
    -> std::optional<Token> {
  Token* token = FindToken(id);
  if (token == nullptr) {
    return std::nullopt;
  }
  RemoveCondition(*token, name);
  return *token;
}

auto GameState::SpendMovement(TokenId id, int steps, int diagonal_parity)
    -> std::optional<Token> {
  Token* token = FindToken(id);
  if (token == nullptr) {
    return std::nullopt;
  }
  token->movement_used += std::max(0, steps);
  token->diagonal_parity = diagonal_parity & 1;
  return *token;
}

auto GameState::MarkHazardDone(TokenId id, std::uint8_t hazard)
    -> std::optional<Token> {
  Token* token = FindToken(id);
  if (token == nullptr) {
    return std::nullopt;
  }
  const auto it = std::ranges::lower_bound(token->hazards_done, hazard);
  if (it == token->hazards_done.end() || *it != hazard) {
    token->hazards_done.insert(it, hazard);
  }
  return *token;
}

auto GameState::SpendAction(TokenId id) -> std::optional<Token> {
  Token* token = FindToken(id);
  if (token == nullptr) {
    return std::nullopt;
  }
  token->action_used = true;
  return *token;
}

auto GameState::SpendReaction(TokenId id) -> std::optional<Token> {
  Token* token = FindToken(id);
  if (token == nullptr) {
    return std::nullopt;
  }
  token->reaction_used = true;
  return *token;
}

auto GameState::UseTurnAction(TokenId id, TurnActionKind kind)
    -> std::optional<Token> {
  if (!IsValidTurnAction(kind)) {
    return std::nullopt;
  }
  Token* token = FindToken(id);
  if (token == nullptr) {
    return std::nullopt;
  }
  token->action_used = true;
  switch (kind) {
    case TurnActionKind::kDash:
      // Рывок даёт ещё столько же движения, сколько обычный запас с учётом
      // истощения и состояний.
      token->movement_bonus = EffectiveSpeedCells(*token);
      break;
    case TurnActionKind::kDisengage:
      token->disengaged = true;
      break;
    case TurnActionKind::kDodge:
      token->dodging = true;
      break;
    case TurnActionKind::kHelp:
    case TurnActionKind::kHide:
    case TurnActionKind::kSearch:
    case TurnActionKind::kStudy:
    case TurnActionKind::kUseObject:
      // Подсказки для стола: отдельных автоматических эффектов у этих
      // действий в первой версии нет.
      break;
  }
  return *token;
}

auto GameState::ResetTurnEconomy(Token& token) -> void {
  token.movement_used = 0;
  token.diagonal_parity = 0;
  token.action_used = false;
  token.bonus_action_used = false;
  token.reaction_used = false;
  token.movement_bonus = 0;
  token.disengaged = false;
  token.dodging = false;
  token.hazards_done.clear();
}

auto GameState::ActiveTokenId() const -> TokenId {
  if (!encounter_.active || encounter_.turn_index >= encounter_.order.size()) {
    return 0;
  }
  return encounter_.order[encounter_.turn_index];
}

auto GameState::StartEncounter(std::vector<TokenId> participants) -> bool {
  if (tokens_.empty()) {
    return false;
  }
  if (participants.empty()) {
    participants.reserve(tokens_.size());
    for (const Token& token : tokens_) {
      participants.push_back(token.id);
    }
  }
  // Участники приходят от сервера: чужой или удалённый id отбрасываем,
  // повторы не нужны.
  std::vector<TokenId> valid;
  valid.reserve(participants.size());
  for (const TokenId id : participants) {
    if (FindToken(id) != nullptr &&
        std::ranges::find(valid, id) == valid.end()) {
      valid.push_back(id);
    }
  }
  if (valid.empty()) {
    return false;
  }
  encounter_ = Encounter{};
  encounter_.active = true;
  encounter_.round = 1;
  for (Token& token : tokens_) {
    ResetTurnEconomy(token);
  }
  encounter_.order = std::move(valid);
  RebuildOrder(0);
  return encounter_.active;
}

auto GameState::JoinEncounter(TokenId id) -> bool {
  if (!encounter_.active) {
    return false;
  }
  Token* token = FindToken(id);
  if (token == nullptr || IsEncounterParticipant(id)) {
    return false;
  }
  ResetTurnEconomy(*token);
  encounter_.order.push_back(id);
  return true;
}

auto GameState::LeaveEncounter(TokenId id) -> bool {
  if (!encounter_.active) {
    return false;
  }
  const auto it = std::ranges::find(encounter_.order, id);
  if (it == encounter_.order.end()) {
    return false;
  }
  const auto index = static_cast<std::size_t>(it - encounter_.order.begin());
  encounter_.order.erase(it);
  if (encounter_.order.empty()) {
    encounter_ = Encounter{};
    return true;
  }
  if (index < encounter_.turn_index) {
    --encounter_.turn_index;
  } else if (index == encounter_.turn_index) {
    if (encounter_.turn_index >= encounter_.order.size()) {
      encounter_.turn_index = 0;
      ++encounter_.round;
    }
    // Следующий по порядку начинает свой ход с чистым запасом движения.
    if (Token* next = FindToken(encounter_.order[encounter_.turn_index])) {
      ResetTurnEconomy(*next);
    }
  }
  return true;
}

auto GameState::IsEncounterParticipant(TokenId id) const -> bool {
  return encounter_.active &&
         std::ranges::find(encounter_.order, id) != encounter_.order.end();
}

auto GameState::EndEncounter() -> void {
  encounter_ = Encounter{};
  for (Token& token : tokens_) {
    ResetTurnEconomy(token);
  }
}

auto GameState::SetAoe(AoeTemplate aoe) -> bool {
  if (!IsValidAoe(aoe) ||
      (aoe.shape != AoeShape::kNone && !map_.Contains(aoe.origin))) {
    return false;
  }
  aoe_ = aoe;
  return true;
}

auto GameState::SetBackground(MapBackground background) -> bool {
  if (!IsValidMapBackground(background)) {
    return false;
  }
  background_ = std::move(background);
  return true;
}

auto GameState::IsRevealed(Cell cell) const -> bool {
  return std::ranges::binary_search(revealed_, cell, CellOrder);
}

auto GameState::RevealArea(Cell center, int radius) -> bool {
  if (radius <= 0 || !map_.Contains(center)) {
    return false;
  }
  const std::vector<Cell> area = CellsInArea(map_, center, radius);
  std::vector<Cell> merged;
  merged.reserve(revealed_.size() + area.size());
  std::ranges::set_union(revealed_, area, std::back_inserter(merged),
                         CellOrder);
  if (merged.size() == revealed_.size()) {
    return false;
  }
  revealed_ = std::move(merged);
  return true;
}

auto GameState::HideArea(Cell center, int radius) -> bool {
  if (radius <= 0 || !map_.Contains(center)) {
    return false;
  }
  const std::vector<Cell> area = CellsInArea(map_, center, radius);
  std::vector<Cell> kept;
  kept.reserve(revealed_.size());
  std::ranges::set_difference(revealed_, area, std::back_inserter(kept),
                              CellOrder);
  if (kept.size() == revealed_.size()) {
    return false;
  }
  revealed_ = std::move(kept);
  return true;
}

auto GameState::ClearRevealed() -> bool {
  if (revealed_.empty()) {
    return false;
  }
  revealed_.clear();
  return true;
}

auto GameState::NextTurn() -> bool {
  if (!encounter_.active || encounter_.order.empty()) {
    return false;
  }
  ++encounter_.turn_index;
  if (encounter_.turn_index >= encounter_.order.size()) {
    encounter_.turn_index = 0;
    ++encounter_.round;
  }
  // Новый активный токен начинает ход с полным запасом движения. Время в
  // бою идёт по 6 секунд за ход: срочные состояния этого токена уменьшаются
  // в начале его хода; состояния с seconds == 0 держатся до ручного снятия.
  if (Token* token = FindToken(ActiveTokenId())) {
    ResetTurnEconomy(*token);
    TickConditions(*token, kSecondsPerRound);
  }
  return true;
}

auto GameState::AdvanceTime(int seconds) -> std::vector<Token> {
  std::vector<Token> changed;
  for (Token& token : tokens_) {
    if (TickConditions(token, seconds)) {
      changed.push_back(token);
    }
  }
  game_time_seconds_ =
      std::min(kMaxGameTimeSeconds, game_time_seconds_ + std::max(0, seconds));
  return changed;
}

auto GameState::RebuildOrder(TokenId keep) -> void {
  // Порядок хранит только участников боя: токены вне его остаются вне боя,
  // пока сервер не добавит их отдельно.
  std::erase_if(encounter_.order, [this](TokenId id) -> bool {
    return FindToken(id) == nullptr;
  });
  std::ranges::sort(encounter_.order,
                    [this](TokenId left, TokenId right) -> bool {
                      const Token* left_token = FindToken(left);
                      const Token* right_token = FindToken(right);
                      if (left_token == nullptr || right_token == nullptr) {
                        return left < right;
                      }
                      if (left_token->initiative != right_token->initiative) {
                        return left_token->initiative > right_token->initiative;
                      }
                      if (left_token->name != right_token->name) {
                        return left_token->name < right_token->name;
                      }
                      return left < right;
                    });
  if (encounter_.order.empty()) {
    encounter_.active = false;
    encounter_.round = 0;
    encounter_.turn_index = 0;
    return;
  }
  const auto it = std::ranges::find(encounter_.order, keep);
  encounter_.turn_index =
      it != encounter_.order.end()
          ? static_cast<std::size_t>(it - encounter_.order.begin())
          : 0;
}

auto GameState::ApplyCommand(const Command& command) -> std::optional<Event> {
  return std::visit(
      [this](const auto& cmd) -> std::optional<Event> {
        using T = std::decay_t<decltype(cmd)>;
        if constexpr (std::is_same_v<T, MoveToken>) {
          if (!map_.Contains(cmd.target) || map_.IsBlocked(cmd.target)) {
            return std::nullopt;
          }
          Token* token = FindToken(cmd.id);
          if (token == nullptr || token->cell == cmd.target) {
            return std::nullopt;
          }
          const Cell from = token->cell;
          token->cell = cmd.target;
          return Event{
              TokenMoved{.id = token->id, .from = from, .to = cmd.target}};
        }
        return std::nullopt;
      },
      command);
}

auto GameState::ApplyEvent(const Event& event) -> void {
  std::visit(
      [this](const auto& ev) -> void {
        using T = std::decay_t<decltype(ev)>;
        if constexpr (std::is_same_v<T, TokenMoved>) {
          if (Token* token = FindToken(ev.id)) {
            token->cell = ev.to;
          }
        }
      },
      event);
}

}  // namespace dnd
