#include "core/game_state.h"

#include <algorithm>
#include <iterator>
#include <random>
#include <string_view>
#include <type_traits>
#include <utility>

#include "core/conditions.h"
#include "core/levels.h"

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

// Прокручивает игровое время для состояний одного токена: срочные
// уменьшаются на seconds, нулевые держатся до ручного снятия. Возвращает
// true, если набор состояний изменился.
auto TickConditions(Token& token, int seconds) -> bool {
  if (seconds <= 0) {
    return false;
  }
  const std::size_t before = token.conditions.size();
  for (auto it = token.conditions.begin(); it != token.conditions.end();) {
    if (it->seconds <= 0) {
      ++it;
      continue;
    }
    it->seconds -= seconds;
    it = it->seconds <= 0 ? token.conditions.erase(it) : it + 1;
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
auto ApplyLevelGains(Token& token, int from_level, int to_level) -> void {
  if (token.kind != TokenKind::kPlayer || to_level <= from_level) {
    return;
  }
  const int hp_gain = HitPointsPerLevel(token.hit_die, token.con_modifier) *
                      (to_level - from_level);
  token.max_hp += hp_gain;
  if (token.hp > 0) {
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
      revealed_(std::move(snapshot.revealed)) {
  // Старые сохранения и события могли прийти в порядке открытия: приводим
  // срез к порядку CellOrder и убираем повторы.
  std::ranges::sort(revealed_, CellOrder);
  const auto duplicates = std::ranges::unique(revealed_);
  revealed_.erase(duplicates.begin(), duplicates.end());
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
                         int save_bonus, std::string damage) -> TokenId {
  const TokenId active = ActiveTokenId();
  const TokenId id = next_token_id_++;
  const int resolved_max_hp = std::max(1, max_hp);
  tokens_.push_back(Token{.id = id,
                          .name = std::move(name),
                          .owner = std::move(owner),
                          .hp = resolved_max_hp,
                          .max_hp = resolved_max_hp,
                          .ac = ac,
                          .speed = speed,
                          .notes = std::move(notes),
                          .attack_bonus = attack_bonus,
                          .save_bonus = save_bonus,
                          .damage = std::move(damage),
                          .kind = kind,
                          .color = color,
                          .cell = cell});
  if (encounter_.active) {
    RebuildOrder(active);
  }
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

auto GameState::DamageToken(TokenId id, int amount) -> std::optional<Token> {
  Token* token = FindToken(id);
  if (token == nullptr) {
    return std::nullopt;
  }
  const bool was_down = token->hp == 0;
  int remaining = std::max(0, amount);
  const int absorbed = std::min(token->temp_hp, remaining);
  token->temp_hp -= absorbed;
  remaining -= absorbed;
  token->hp = std::max(0, token->hp - remaining);
  if (token->hp == 0 && !was_down) {
    token->death_save_successes = 0;
    token->death_save_failures = 0;
    AddConditionIfMissing(*token, Condition{std::string(kUnconscious)});
  }
  if (was_down && amount > 0) {
    // Удар по лежачему — провал спасброска от смерти.
    token->death_save_failures = std::min(3, token->death_save_failures + 1);
    if (token->death_save_failures >= 3) {
      RemoveCondition(*token, kUnconscious);
      AddConditionIfMissing(*token, Condition{std::string(kDead)});
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
  }
  return *token;
}

auto GameState::DeathSave(TokenId id, int roll) -> std::optional<Token> {
  Token* token = FindToken(id);
  if (token == nullptr || token->hp > 0) {
    return std::nullopt;
  }
  if (roll == 20) {
    token->hp = 1;
    token->death_save_successes = 0;
    token->death_save_failures = 0;
    RemoveCondition(*token, kUnconscious);
    return *token;
  }
  if (roll >= 10) {
    token->death_save_successes = std::min(3, token->death_save_successes + 1);
    if (token->death_save_successes >= 3) {
      // Три успеха: персонаж стабилен, но всё ещё без сознания на нуле HP.
      RemoveCondition(*token, kUnconscious);
    }
  } else {
    token->death_save_failures = std::min(3, token->death_save_failures + 1);
    if (token->death_save_failures >= 3) {
      RemoveCondition(*token, kUnconscious);
      AddConditionIfMissing(*token, Condition{std::string(kDead)});
    }
  }
  return *token;
}

auto GameState::RestParty(bool long_rest, std::mt19937& rng)
    -> std::vector<RestChange> {
  std::vector<RestChange> changed;
  const int rest_seconds = long_rest ? kLongRestSeconds : kShortRestSeconds;
  for (Token& token : tokens_) {
    const int old_hp = token.hp;
    const int old_temp = token.temp_hp;
    const int old_spent = token.hit_dice_spent;
    const int old_successes = token.death_save_successes;
    const int old_failures = token.death_save_failures;
    const std::size_t old_conditions = token.conditions.size();
    // Время отдыха идёт для всех: срочные состояния истекают и у монстров.
    TickConditions(token, rest_seconds);
    if (token.kind != TokenKind::kPlayer) {
      if (token.conditions.size() != old_conditions) {
        changed.push_back(RestChange{.token = token});
      }
      continue;
    }
    if (long_rest) {
      token.hp = token.max_hp;
      token.temp_hp = 0;
      token.conditions.clear();
      token.death_save_successes = 0;
      token.death_save_failures = 0;
      token.hit_dice_spent = 0;
    } else {
      // Короткий отдых тратит кости хитов, пока не восстановлен максимум
      // HP или не кончился запас.
      const int level = LevelForXp(token.xp);
      const int available = HitDiceAvailable(level, token.hit_dice_spent);
      std::uniform_int_distribution<int> die(1, token.hit_die);
      int spent = 0;
      while (token.hp < token.max_hp && spent < available) {
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
                     .hp_restored = token.hp - old_hp});
    }
  }
  return changed;
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
  if (token == nullptr) {
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
  ApplyLevelGains(*token, before, LevelForXp(token->xp));
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

auto GameState::UpdateTokenSheet(TokenId id, int ac, int speed,
                                 std::string notes, int attack_bonus,
                                 int save_bonus, int initiative_bonus,
                                 std::string damage, int temp_hp, int hit_die,
                                 int con_modifier) -> std::optional<Token> {
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
  token->temp_hp = temp_hp;
  token->hit_die = hit_die;
  token->con_modifier = con_modifier;
  return *token;
}

auto GameState::UpdateTokenCombat(TokenId id, std::vector<Condition> conditions)
    -> std::optional<Token> {
  Token* token = FindToken(id);
  if (token == nullptr) {
    return std::nullopt;
  }
  token->conditions = std::move(conditions);
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

auto GameState::ActiveTokenId() const -> TokenId {
  if (!encounter_.active || encounter_.turn_index >= encounter_.order.size()) {
    return 0;
  }
  return encounter_.order[encounter_.turn_index];
}

auto GameState::StartEncounter() -> bool {
  if (tokens_.empty()) {
    return false;
  }
  encounter_ = Encounter{};
  encounter_.active = true;
  encounter_.round = 1;
  for (Token& token : tokens_) {
    token.movement_used = 0;
    token.diagonal_parity = 0;
  }
  RebuildOrder(0);
  return true;
}

auto GameState::EndEncounter() -> void {
  encounter_ = Encounter{};
  for (Token& token : tokens_) {
    token.movement_used = 0;
    token.diagonal_parity = 0;
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
    token->movement_used = 0;
    token->diagonal_parity = 0;
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
  return changed;
}

auto GameState::RebuildOrder(TokenId keep) -> void {
  std::vector<TokenId> order;
  order.reserve(tokens_.size());
  for (const Token& token : tokens_) {
    order.push_back(token.id);
  }
  std::ranges::sort(order, [this](TokenId left, TokenId right) -> bool {
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
  encounter_.order = std::move(order);
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
