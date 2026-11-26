#include "core/serialization.h"

#include <algorithm>
#include <string>
#include <type_traits>
#include <utility>

#include "core/byte_io.h"
#include "core/conditions.h"
#include "core/levels.h"
#include "core/map_background.h"
#include "core/movement.h"

namespace dnd {
namespace {

constexpr std::uint32_t kMaxDimension = 4096;
constexpr std::uint32_t kMaxTokens = 100000;
// Сколько клеток DM может открыть вручную в одном снапшоте/событии.
constexpr std::uint32_t kMaxRevealedCells = 1000000;
// Туман хранится на каждого игрока плюс общий обзор партии.
constexpr std::uint32_t kMaxFogRecords = 32;
constexpr std::uint32_t kMaxNameLength = 256;
constexpr std::uint32_t kMaxChatText = 256;
constexpr std::uint32_t kMaxLogText = 512;
constexpr std::uint32_t kMaxLogAuthor = 64;
constexpr std::uint32_t kMaxNotes = 512;
constexpr std::uint32_t kMaxConditions = 16;
constexpr std::uint32_t kMaxConditionLength = 32;
constexpr std::uint32_t kMaxHazards = 64;
constexpr std::uint32_t kMaxDamageExpression = 64;

constexpr std::uint8_t kCommandMoveToken = 1;
constexpr std::uint8_t kCommandSay = 2;
constexpr std::uint8_t kCommandRollDice = 3;
constexpr std::uint8_t kCommandAddToken = 4;
constexpr std::uint8_t kCommandRemoveToken = 5;
constexpr std::uint8_t kCommandSetTokenHp = 6;
constexpr std::uint8_t kCommandSetInitiative = 7;
constexpr std::uint8_t kCommandStartEncounter = 8;
constexpr std::uint8_t kCommandEndEncounter = 9;
constexpr std::uint8_t kCommandNextTurn = 10;
constexpr std::uint8_t kCommandClaimToken = 11;
constexpr std::uint8_t kCommandSetTokenOwner = 12;
constexpr std::uint8_t kCommandRenameToken = 13;
constexpr std::uint8_t kCommandSetTokenMaxHp = 14;
constexpr std::uint8_t kCommandSetTokenColor = 15;
constexpr std::uint8_t kCommandUpdateTokenSheet = 16;
constexpr std::uint8_t kCommandReleaseToken = 17;
constexpr std::uint8_t kCommandRollExpression = 18;
constexpr std::uint8_t kCommandUpdateTokenCombat = 19;
constexpr std::uint8_t kCommandSetAoe = 20;
constexpr std::uint8_t kCommandDamageToken = 21;
constexpr std::uint8_t kCommandHealToken = 22;
constexpr std::uint8_t kCommandRestParty = 23;
constexpr std::uint8_t kCommandAoeDamage = 24;
constexpr std::uint8_t kCommandDeathSave = 25;
constexpr std::uint8_t kCommandPing = 26;
constexpr std::uint8_t kCommandSetMapBackground = 27;
constexpr std::uint8_t kCommandSetTokenPortrait = 28;
constexpr std::uint8_t kCommandRevealArea = 29;
constexpr std::uint8_t kCommandClearRevealed = 30;
constexpr std::uint8_t kCommandAwardXp = 31;
constexpr std::uint8_t kCommandAttackToken = 32;
constexpr std::uint8_t kCommandHideArea = 33;
constexpr std::uint8_t kCommandAdvanceTime = 34;
constexpr std::uint8_t kCommandUseTurnAction = 35;
constexpr std::uint8_t kCommandSetEncounterComposition = 36;
constexpr std::uint8_t kCommandFallToken = 37;
constexpr std::uint8_t kCommandSetTokenLight = 38;

constexpr std::uint8_t kEventTokenMoved = 1;
constexpr std::uint8_t kEventViewUpdated = 2;
constexpr std::uint8_t kEventLogLineAdded = 3;
constexpr std::uint8_t kEventTokenAdded = 4;
constexpr std::uint8_t kEventTokenRemoved = 5;
constexpr std::uint8_t kEventTokenUpdated = 6;
constexpr std::uint8_t kEventEncounterUpdated = 7;
constexpr std::uint8_t kEventAoeUpdated = 8;
constexpr std::uint8_t kEventPingMarker = 9;
constexpr std::uint8_t kEventMapBackgroundUpdated = 10;
constexpr std::uint8_t kEventRevealedUpdated = 11;
constexpr std::uint8_t kEventFogChanged = 12;

auto WriteToken(ByteWriter& writer, const Token& token,
                const SnapshotFormat& format) -> void {
  writer.U32(token.id);
  writer.String(token.name);
  writer.String(token.owner);
  writer.I32(token.hp);
  writer.I32(token.max_hp);
  writer.I32(token.temp_hp);
  writer.I32(token.initiative);
  writer.I32(token.ac);
  writer.I32(token.speed);
  writer.U8(static_cast<std::uint8_t>(token.kind));
  writer.U32(token.color);
  writer.I32(token.cell.x);
  writer.I32(token.cell.y);
  writer.String(token.notes);
  writer.I32(token.attack_bonus);
  writer.I32(token.save_bonus);
  writer.I32(token.initiative_bonus);
  writer.String(token.damage);
  writer.U32(static_cast<std::uint32_t>(token.conditions.size()));
  for (const Condition& condition : token.conditions) {
    writer.String(condition.name);
    writer.I32(condition.seconds);
    writer.I32(condition.save_dc);
  }
  writer.I32(token.movement_used);
  writer.I32(token.diagonal_parity);
  writer.I32(token.death_save_successes);
  writer.I32(token.death_save_failures);
  writer.String(token.portrait);
  writer.I32(token.xp);
  writer.I32(token.hit_die);
  writer.I32(token.con_modifier);
  writer.I32(token.hit_dice_spent);
  // Экономика хода появилась в сохранении 28, защиты и дистанция — в 29.
  if (format.has_turn_economy) {
    writer.U8(token.action_used ? 1 : 0);
    writer.U8(token.bonus_action_used ? 1 : 0);
    writer.U8(token.reaction_used ? 1 : 0);
    writer.I32(token.movement_bonus);
    writer.U8(token.disengaged ? 1 : 0);
    writer.U8(token.dodging ? 1 : 0);
  }
  if (format.has_damage_profile) {
    writer.U32(token.resist);
    writer.U32(token.vulnerable);
    writer.U32(token.immune);
    writer.I32(token.attack_range);
  }
  if (format.has_cell_props) {
    writer.I32(token.swim_speed);
    writer.I32(token.climb_speed);
    writer.U32(static_cast<std::uint32_t>(token.hazards_done.size()));
    for (const std::uint8_t hazard : token.hazards_done) {
      writer.U8(hazard);
    }
  }
  if (format.has_vision) {
    writer.I32(token.darkvision_cells);
    writer.I32(token.light_bright_cells);
    writer.I32(token.light_dim_cells);
  }
}

auto ReadToken(ByteReader& reader, Token& token, const SnapshotFormat& format)
    -> bool {
  std::uint32_t id = 0;
  std::string name;
  std::string owner;
  std::int32_t hp = 0;
  std::int32_t max_hp = 0;
  std::int32_t temp_hp = 0;
  std::int32_t initiative = 0;
  std::int32_t ac = 10;
  std::int32_t speed = 9;
  std::uint8_t kind = 0;
  std::uint32_t color = 0;
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::string notes;
  std::int32_t attack_bonus = 0;
  std::int32_t save_bonus = 0;
  std::int32_t initiative_bonus = 0;
  std::string damage;
  std::uint32_t condition_count = 0;
  std::vector<Condition> conditions;
  std::int32_t movement_used = 0;
  std::int32_t diagonal_parity = 0;
  std::int32_t death_successes = 0;
  std::int32_t death_failures = 0;
  std::string portrait;
  std::int32_t xp = 0;
  std::int32_t hit_die = 8;
  std::int32_t con_modifier = 0;
  std::int32_t hit_dice_spent = 0;
  std::uint8_t action_used = 0;
  std::uint8_t bonus_action_used = 0;
  std::uint8_t reaction_used = 0;
  std::int32_t movement_bonus = 0;
  std::uint8_t disengaged = 0;
  std::uint8_t dodging = 0;
  std::uint32_t resist = 0;
  std::uint32_t vulnerable = 0;
  std::uint32_t immune = 0;
  std::int32_t attack_range = 0;
  std::int32_t swim_speed = 0;
  std::int32_t climb_speed = 0;
  std::uint32_t hazards_done_count = 0;
  std::vector<std::uint8_t> hazards_done;
  std::int32_t darkvision_cells = 0;
  std::int32_t light_bright_cells = 0;
  std::int32_t light_dim_cells = 0;
  if (!reader.U32(id) || !reader.String(name, kMaxNameLength) ||
      !reader.String(owner, kMaxNameLength) || !reader.I32(hp) ||
      !reader.I32(max_hp) || !reader.I32(temp_hp) || !reader.I32(initiative) ||
      !reader.I32(ac) || !reader.I32(speed) || !reader.U8(kind) ||
      !reader.U32(color) || !reader.I32(x) || !reader.I32(y) ||
      !reader.String(notes, kMaxNotes) || !reader.I32(attack_bonus) ||
      !reader.I32(save_bonus) || !reader.I32(initiative_bonus) ||
      !reader.String(damage, kMaxNotes) || !reader.U32(condition_count) ||
      condition_count > kMaxConditions) {
    return false;
  }
  conditions.reserve(condition_count);
  for (std::uint32_t i = 0; i < condition_count; ++i) {
    std::string condition_name;
    std::int32_t seconds = 0;
    std::int32_t save_dc = 0;
    if (!reader.String(condition_name, kMaxConditionLength) ||
        !reader.I32(seconds) || seconds < 0 || seconds > kMaxConditionSeconds ||
        !reader.I32(save_dc) || save_dc < 0 || save_dc > kMaxSaveDc) {
      return false;
    }
    conditions.emplace_back(std::move(condition_name), seconds, save_dc);
  }
  if (!reader.I32(movement_used) || !reader.I32(diagonal_parity) ||
      !reader.I32(death_successes) || !reader.I32(death_failures) ||
      !reader.String(portrait, kMaxImageNameLength) || !reader.I32(xp) ||
      !reader.I32(hit_die) || !reader.I32(con_modifier) ||
      !reader.I32(hit_dice_spent) || diagonal_parity < 0 ||
      diagonal_parity > 1 || death_successes < 0 || death_successes > 3 ||
      death_failures < 0 || death_failures > 3) {
    return false;
  }
  if (format.has_turn_economy &&
      (!reader.U8(action_used) || !reader.U8(bonus_action_used) ||
       !reader.U8(reaction_used) || !reader.I32(movement_bonus) ||
       !reader.U8(disengaged) || !reader.U8(dodging))) {
    return false;
  }
  if (format.has_damage_profile &&
      (!reader.U32(resist) || !reader.U32(vulnerable) || !reader.U32(immune) ||
       !reader.I32(attack_range))) {
    return false;
  }
  if (format.has_cell_props) {
    if (!reader.I32(swim_speed) || !reader.I32(climb_speed) ||
        !reader.U32(hazards_done_count) || hazards_done_count > kMaxHazards) {
      return false;
    }
    hazards_done.reserve(hazards_done_count);
    for (std::uint32_t i = 0; i < hazards_done_count; ++i) {
      std::uint8_t hazard = 0;
      if (!reader.U8(hazard) || hazard >= kMaxHazards) {
        return false;
      }
      hazards_done.push_back(hazard);
    }
  }
  if (format.has_vision &&
      (!reader.I32(darkvision_cells) || !reader.I32(light_bright_cells) ||
       !reader.I32(light_dim_cells))) {
    return false;
  }
  if (max_hp < 0 || max_hp > 1000000 || temp_hp < 0 || temp_hp > 1000 ||
      initiative < -1000000 || initiative > 1000000 || ac <= 0 || ac > 1000 ||
      speed < 0 || speed > 1000 || attack_bonus < -100 || attack_bonus > 100 ||
      save_bonus < -100 || save_bonus > 100 || initiative_bonus < -100 ||
      initiative_bonus > 100 || movement_used < 0 || movement_used > 1000 ||
      xp < 0 || xp > kMaxXp || !IsValidHitDie(hit_die) ||
      con_modifier < -kMaxConModifier || con_modifier > kMaxConModifier ||
      hit_dice_spent < 0 || hit_dice_spent > kMaxLevel || action_used > 1 ||
      bonus_action_used > 1 || reaction_used > 1 || movement_bonus < 0 ||
      movement_bonus > 1000 || disengaged > 1 || dodging > 1 ||
      kind > static_cast<std::uint8_t>(TokenKind::kMonster) ||
      (color & 0xFF000000U) != 0 ||
      (!portrait.empty() && !IsSafeImageName(portrait)) ||
      !IsValidDamageTypeBit(resist) || !IsValidDamageTypeBit(vulnerable) ||
      !IsValidDamageTypeBit(immune) || attack_range < 0 ||
      attack_range > 1000 || swim_speed < 0 || swim_speed > 1000 ||
      climb_speed < 0 || climb_speed > 1000 || darkvision_cells < 0 ||
      darkvision_cells > kMaxLightCells || light_bright_cells < 0 ||
      light_bright_cells > kMaxLightCells || light_dim_cells < 0 ||
      light_dim_cells > kMaxLightCells ||
      light_dim_cells < light_bright_cells) {
    return false;
  }
  if (!std::ranges::is_sorted(hazards_done) ||
      std::adjacent_find(hazards_done.begin(), hazards_done.end()) !=
          hazards_done.end()) {
    return false;
  }
  token = Token{
      .id = id,
      .name = std::move(name),
      .owner = std::move(owner),
      .hp = hp,
      .max_hp = max_hp,
      .temp_hp = temp_hp,
      .hit_die = hit_die,
      .con_modifier = con_modifier,
      .hit_dice_spent = hit_dice_spent,
      .initiative = initiative,
      .ac = ac,
      .speed = speed,
      .swim_speed = swim_speed,
      .climb_speed = climb_speed,
      .darkvision_cells = darkvision_cells,
      .light_bright_cells = light_bright_cells,
      .light_dim_cells = light_dim_cells,
      .notes = std::move(notes),
      .attack_bonus = attack_bonus,
      .save_bonus = save_bonus,
      .initiative_bonus = initiative_bonus,
      .damage = std::move(damage),
      .resist = resist,
      .vulnerable = vulnerable,
      .immune = immune,
      .attack_range = attack_range,
      .conditions = std::move(conditions),
      .xp = xp,
      .movement_used = movement_used,
      .diagonal_parity = diagonal_parity,
      .death_save_successes = death_successes,
      .death_save_failures = death_failures,
      .kind = static_cast<TokenKind>(kind),
      .color = color,
      .portrait = std::move(portrait),
      .cell = Cell{.x = x, .y = y},
      .action_used = action_used != 0,
      .bonus_action_used = bonus_action_used != 0,
      .reaction_used = reaction_used != 0,
      .movement_bonus = movement_bonus,
      .disengaged = disengaged != 0,
      .dodging = dodging != 0,
      .hazards_done = std::move(hazards_done),
  };
  return true;
}

auto ReadPosition(ByteReader& reader, Cell& cell) -> bool {
  std::int32_t x = 0;
  std::int32_t y = 0;
  if (!reader.I32(x) || !reader.I32(y)) {
    return false;
  }
  cell = Cell{.x = x, .y = y};
  return true;
}

auto WriteEncounter(ByteWriter& writer, const Encounter& encounter) -> void {
  writer.U8(encounter.active ? 1 : 0);
  writer.I32(encounter.round);
  writer.U32(static_cast<std::uint32_t>(encounter.order.size()));
  for (const TokenId id : encounter.order) {
    writer.U32(id);
  }
  writer.U32(static_cast<std::uint32_t>(encounter.turn_index));
}

auto ReadEncounter(ByteReader& reader, Encounter& encounter) -> bool {
  std::uint8_t active = 0;
  std::int32_t round = 0;
  std::uint32_t count = 0;
  if (!reader.U8(active) || !reader.I32(round) || !reader.U32(count) ||
      count > kMaxTokens) {
    return false;
  }
  std::vector<TokenId> order;
  order.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint32_t id = 0;
    if (!reader.U32(id)) {
      return false;
    }
    order.push_back(id);
  }
  std::uint32_t turn_index = 0;
  if (!reader.U32(turn_index)) {
    return false;
  }
  if (round < 0 || turn_index > count) {
    return false;
  }
  if (count == 0 && active != 0) {
    return false;
  }
  if (count > 0 && turn_index >= count) {
    return false;
  }
  encounter = Encounter{
      .active = active != 0,
      .round = round,
      .order = std::move(order),
      .turn_index = turn_index,
  };
  return true;
}

auto WriteAoe(ByteWriter& writer, const AoeTemplate& aoe) -> void {
  writer.U8(static_cast<std::uint8_t>(aoe.shape));
  writer.I32(aoe.origin.x);
  writer.I32(aoe.origin.y);
  writer.I32(aoe.direction);
  writer.I32(aoe.radius);
  writer.I32(aoe.width);
}

auto ReadAoe(ByteReader& reader, AoeTemplate& aoe) -> bool {
  std::uint8_t shape = 0;
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::int32_t direction = 0;
  std::int32_t radius = 0;
  std::int32_t width = 0;
  if (!reader.U8(shape) || !reader.I32(x) || !reader.I32(y) ||
      !reader.I32(direction) || !reader.I32(radius) || !reader.I32(width)) {
    return false;
  }
  if (shape > static_cast<std::uint8_t>(AoeShape::kLine)) {
    return false;
  }
  const AoeTemplate decoded{
      .shape = static_cast<AoeShape>(shape),
      .origin = Cell{.x = x, .y = y},
      .direction = direction,
      .radius = radius,
      .width = width,
  };
  if (!IsValidAoe(decoded)) {
    return false;
  }
  aoe = decoded;
  return true;
}

auto DecodeSetAoeCommand(ByteReader& reader) -> std::optional<Command> {
  AoeTemplate aoe;
  if (!ReadAoe(reader, aoe) || !reader.Done()) {
    return std::nullopt;
  }
  return Command{SetAoe{.aoe = aoe}};
}

auto WriteMapBackground(ByteWriter& writer, const MapBackground& background)
    -> void {
  writer.String(background.image);
  writer.I32(background.opacity);
  writer.I32(background.scale);
}

auto ReadMapBackground(ByteReader& reader, MapBackground& background) -> bool {
  std::string image;
  std::int32_t opacity = 0;
  std::int32_t scale = 0;
  if (!reader.String(image, kMaxImageNameLength) || !reader.I32(opacity) ||
      !reader.I32(scale)) {
    return false;
  }
  const MapBackground decoded{
      .image = std::move(image),
      .opacity = opacity,
      .scale = scale,
  };
  if (!IsValidMapBackground(decoded)) {
    return false;
  }
  background = decoded;
  return true;
}

auto DecodeSetMapBackgroundCommand(ByteReader& reader)
    -> std::optional<Command> {
  MapBackground background;
  if (!ReadMapBackground(reader, background) || !reader.Done()) {
    return std::nullopt;
  }
  return Command{SetMapBackground{.background = std::move(background)}};
}

auto DecodeDamageCommand(ByteReader& reader) -> std::optional<Command> {
  std::uint32_t id = 0;
  std::int32_t amount = 0;
  std::uint8_t damage_type = 0;
  if (!reader.U32(id) || !reader.I32(amount) || !reader.U8(damage_type) ||
      damage_type > static_cast<std::uint8_t>(DamageType::kThunder) ||
      !reader.Done()) {
    return std::nullopt;
  }
  return Command{
      DamageToken{.id = id,
                  .amount = amount,
                  .damage_type = static_cast<DamageType>(damage_type)}};
}

auto DecodeHealCommand(ByteReader& reader) -> std::optional<Command> {
  std::uint32_t id = 0;
  std::int32_t amount = 0;
  if (!reader.U32(id) || !reader.I32(amount) || !reader.Done()) {
    return std::nullopt;
  }
  return Command{HealToken{.id = id, .amount = amount}};
}

auto DecodeRestCommand(ByteReader& reader) -> std::optional<Command> {
  std::uint8_t long_rest = 0;
  std::int32_t hit_dice = 0;
  if (!reader.U8(long_rest) || long_rest > 1 || !reader.I32(hit_dice) ||
      hit_dice < 0 || hit_dice > kMaxLevel || !reader.Done()) {
    return std::nullopt;
  }
  return Command{RestParty{.long_rest = long_rest != 0, .hit_dice = hit_dice}};
}

auto DecodeFallCommand(ByteReader& reader) -> std::optional<Command> {
  std::uint32_t id = 0;
  std::int32_t feet = 0;
  if (!reader.U32(id) || !reader.I32(feet) || feet <= 0 ||
      feet > kMaxFallFeet || !reader.Done()) {
    return std::nullopt;
  }
  return Command{FallToken{.id = id, .feet = feet}};
}

auto DecodeAdvanceTimeCommand(ByteReader& reader) -> std::optional<Command> {
  std::int32_t seconds = 0;
  if (!reader.I32(seconds) || !reader.Done() || seconds <= 0 ||
      seconds > kMaxConditionSeconds) {
    return std::nullopt;
  }
  return Command{AdvanceTime{.seconds = seconds}};
}

auto DecodeAoeDamageCommand(ByteReader& reader) -> std::optional<Command> {
  std::string expression;
  std::int32_t save_dc = 0;
  std::uint8_t damage_type = 0;
  if (!reader.String(expression, kMaxNotes) || !reader.I32(save_dc) ||
      !reader.U8(damage_type) ||
      damage_type > static_cast<std::uint8_t>(DamageType::kThunder) ||
      !reader.Done()) {
    return std::nullopt;
  }
  return Command{
      AoeDamage{.expression = std::move(expression),
                .save_dc = save_dc,
                .damage_type = static_cast<DamageType>(damage_type)}};
}

auto DecodeDeathSaveCommand(ByteReader& reader) -> std::optional<Command> {
  std::uint32_t id = 0;
  if (!reader.U32(id) || !reader.Done()) {
    return std::nullopt;
  }
  return Command{DeathSave{.id = id}};
}

auto DecodeAttackCommand(ByteReader& reader) -> std::optional<Command> {
  std::uint32_t attacker = 0;
  std::uint32_t target = 0;
  std::uint8_t flags = 0;
  if (!reader.U32(attacker) || !reader.U32(target) || !reader.U8(flags) ||
      (flags & ~0x7U) != 0 || (flags & 0x3U) == 0x3U || !reader.Done()) {
    return std::nullopt;
  }
  return Command{AttackToken{.attacker = attacker,
                             .target = target,
                             .advantage = (flags & 0x1U) != 0,
                             .disadvantage = (flags & 0x2U) != 0,
                             .reaction = (flags & 0x4U) != 0}};
}

auto DecodeUseTurnActionCommand(ByteReader& reader) -> std::optional<Command> {
  std::uint32_t id = 0;
  std::uint8_t kind = 0;
  if (!reader.U32(id) || !reader.U8(kind) ||
      kind >= static_cast<std::uint8_t>(kTurnActionCount) || !reader.Done()) {
    return std::nullopt;
  }
  return Command{
      UseTurnAction{.id = id, .kind = static_cast<TurnActionKind>(kind)}};
}

auto DecodePingCommand(ByteReader& reader) -> std::optional<Command> {
  std::int32_t x = 0;
  std::int32_t y = 0;
  if (!reader.I32(x) || !reader.I32(y) || !reader.Done()) {
    return std::nullopt;
  }
  return Command{PingAt{.cell = Cell{.x = x, .y = y}}};
}

auto DecodeStartEncounterCommand(ByteReader& reader) -> std::optional<Command> {
  if (!reader.Done()) {
    return std::nullopt;
  }
  return Command{StartEncounter{}};
}

auto DecodeEndEncounterCommand(ByteReader& reader) -> std::optional<Command> {
  if (!reader.Done()) {
    return std::nullopt;
  }
  return Command{EndEncounter{}};
}

auto DecodeNextTurnCommand(ByteReader& reader) -> std::optional<Command> {
  if (!reader.Done()) {
    return std::nullopt;
  }
  return Command{NextTurn{}};
}

auto DecodeSetEncounterCompositionCommand(ByteReader& reader)
    -> std::optional<Command> {
  std::uint32_t id = 0;
  std::uint8_t joined = 0;
  if (!reader.U32(id) || !reader.U8(joined) || joined > 1 || !reader.Done()) {
    return std::nullopt;
  }
  return Command{SetEncounterComposition{.id = id, .joined = joined != 0}};
}

auto DecodeMoveCommand(ByteReader& reader) -> std::optional<Command> {
  std::uint32_t id = 0;
  Cell cell;
  std::uint8_t flags = 0;
  if (!reader.U32(id) || !ReadPosition(reader, cell) || !reader.U8(flags) ||
      (flags & ~0x3U) != 0 || (flags & 0x3U) == 0x3U || !reader.Done()) {
    return std::nullopt;
  }
  return Command{MoveToken{.id = id,
                           .target = cell,
                           .crawl = (flags & 0x1U) != 0,
                           .jump = (flags & 0x2U) != 0}};
}

auto DecodeSayCommand(ByteReader& reader) -> std::optional<Command> {
  std::string text;
  if (!reader.String(text, kMaxChatText) || !reader.Done()) {
    return std::nullopt;
  }
  return Command{Say{.text = std::move(text)}};
}

auto DecodeRollCommand(ByteReader& reader) -> std::optional<Command> {
  std::uint32_t count = 0;
  std::uint32_t sides = 0;
  if (!reader.U32(count) || !reader.U32(sides) || !reader.Done()) {
    return std::nullopt;
  }
  return Command{RollDice{
      .count = static_cast<int>(count),
      .sides = static_cast<int>(sides),
  }};
}

auto DecodeRollExpressionCommand(ByteReader& reader) -> std::optional<Command> {
  std::string expression;
  std::string label;
  if (!reader.String(expression, kMaxNotes) ||
      !reader.String(label, kMaxNotes) || !reader.Done()) {
    return std::nullopt;
  }
  return Command{RollExpression{.expression = std::move(expression),
                                .label = std::move(label)}};
}

auto DecodeAddCommand(ByteReader& reader) -> std::optional<Command> {
  std::string name;
  Cell cell;
  std::string owner;
  std::uint8_t kind = 0;
  std::int32_t max_hp = 0;
  std::int32_t ac = 10;
  std::int32_t speed = 9;
  std::string notes;
  std::int32_t attack_bonus = 0;
  std::int32_t save_bonus = 0;
  std::string damage;
  std::int32_t attack_range = 0;
  std::uint32_t resist = 0;
  std::uint32_t vulnerable = 0;
  std::uint32_t immune = 0;
  if (!reader.String(name, kMaxNameLength) || !ReadPosition(reader, cell) ||
      !reader.String(owner, kMaxNameLength) || !reader.U8(kind) ||
      !reader.I32(max_hp) || !reader.I32(ac) || !reader.I32(speed) ||
      !reader.String(notes, kMaxNotes) || !reader.I32(attack_bonus) ||
      !reader.I32(save_bonus) || !reader.String(damage, kMaxNotes) ||
      !reader.I32(attack_range) || !reader.U32(resist) ||
      !reader.U32(vulnerable) || !reader.U32(immune) || !reader.Done()) {
    return std::nullopt;
  }
  if (kind > static_cast<std::uint8_t>(TokenKind::kMonster) || max_hp <= 0 ||
      max_hp > 1000000 || ac <= 0 || ac > 1000 || speed < 0 || speed > 1000 ||
      attack_bonus < -100 || attack_bonus > 100 || save_bonus < -100 ||
      save_bonus > 100 || attack_range < 0 || attack_range > 1000 ||
      !IsValidDamageTypeBit(resist) || !IsValidDamageTypeBit(vulnerable) ||
      !IsValidDamageTypeBit(immune)) {
    return std::nullopt;
  }
  return Command{AddToken{
      .name = std::move(name),
      .cell = cell,
      .owner = std::move(owner),
      .kind = static_cast<TokenKind>(kind),
      .max_hp = max_hp,
      .ac = ac,
      .speed = speed,
      .notes = std::move(notes),
      .attack_bonus = attack_bonus,
      .save_bonus = save_bonus,
      .damage = std::move(damage),
      .attack_range = attack_range,
      .resist = resist,
      .vulnerable = vulnerable,
      .immune = immune,
  }};
}

auto DecodeRemoveCommand(ByteReader& reader) -> std::optional<Command> {
  std::uint32_t id = 0;
  if (!reader.U32(id) || !reader.Done()) {
    return std::nullopt;
  }
  return Command{RemoveToken{.id = id}};
}

auto DecodeSetHpCommand(ByteReader& reader) -> std::optional<Command> {
  std::uint32_t id = 0;
  std::int32_t hp = 0;
  if (!reader.U32(id) || !reader.I32(hp) || !reader.Done()) {
    return std::nullopt;
  }
  return Command{SetTokenHp{.id = id, .hp = hp}};
}

auto DecodeSetInitiativeCommand(ByteReader& reader) -> std::optional<Command> {
  std::uint32_t id = 0;
  std::int32_t value = 0;
  if (!reader.U32(id) || !reader.I32(value) || !reader.Done()) {
    return std::nullopt;
  }
  return Command{SetInitiative{.id = id, .value = value}};
}

auto DecodeClaimTokenCommand(ByteReader& reader) -> std::optional<Command> {
  std::uint32_t id = 0;
  if (!reader.U32(id) || !reader.Done()) {
    return std::nullopt;
  }
  return Command{ClaimToken{.id = id}};
}

auto DecodeReleaseTokenCommand(ByteReader& reader) -> std::optional<Command> {
  std::uint32_t id = 0;
  if (!reader.U32(id) || !reader.Done()) {
    return std::nullopt;
  }
  return Command{ReleaseToken{.id = id}};
}

auto DecodeSetTokenOwnerCommand(ByteReader& reader) -> std::optional<Command> {
  std::uint32_t id = 0;
  std::string owner;
  if (!reader.U32(id) || !reader.String(owner, kMaxNameLength) ||
      !reader.Done()) {
    return std::nullopt;
  }
  return Command{SetTokenOwner{.id = id, .owner = std::move(owner)}};
}

auto DecodeRenameTokenCommand(ByteReader& reader) -> std::optional<Command> {
  std::uint32_t id = 0;
  std::string name;
  if (!reader.U32(id) || !reader.String(name, kMaxNameLength) ||
      !reader.Done()) {
    return std::nullopt;
  }
  return Command{RenameToken{.id = id, .name = std::move(name)}};
}

auto DecodeSetTokenMaxHpCommand(ByteReader& reader) -> std::optional<Command> {
  std::uint32_t id = 0;
  std::int32_t max_hp = 0;
  if (!reader.U32(id) || !reader.I32(max_hp) || !reader.Done()) {
    return std::nullopt;
  }
  return Command{SetTokenMaxHp{.id = id, .max_hp = max_hp}};
}

auto DecodeSetTokenColorCommand(ByteReader& reader) -> std::optional<Command> {
  std::uint32_t id = 0;
  std::uint32_t color = 0;
  if (!reader.U32(id) || !reader.U32(color) || !reader.Done()) {
    return std::nullopt;
  }
  return Command{SetTokenColor{.id = id, .color = color}};
}

auto DecodeSetTokenPortraitCommand(ByteReader& reader)
    -> std::optional<Command> {
  std::uint32_t id = 0;
  std::string portrait;
  if (!reader.U32(id) || !reader.String(portrait, kMaxImageNameLength) ||
      !reader.Done() || (!portrait.empty() && !IsSafeImageName(portrait))) {
    return std::nullopt;
  }
  return Command{SetTokenPortrait{.id = id, .portrait = std::move(portrait)}};
}

auto DecodeRevealAreaCommand(ByteReader& reader) -> std::optional<Command> {
  Cell cell;
  std::uint32_t radius = 0;
  if (!ReadPosition(reader, cell) || !reader.U32(radius) || !reader.Done() ||
      radius == 0 || radius > kMaxRevealRadiusCells) {
    return std::nullopt;
  }
  return Command{RevealArea{.cell = cell, .radius = static_cast<int>(radius)}};
}

auto DecodeClearRevealedCommand(ByteReader& reader) -> std::optional<Command> {
  if (!reader.Done()) {
    return std::nullopt;
  }
  return Command{ClearRevealed{}};
}

auto DecodeHideAreaCommand(ByteReader& reader) -> std::optional<Command> {
  Cell cell;
  std::uint32_t radius = 0;
  if (!ReadPosition(reader, cell) || !reader.U32(radius) || !reader.Done() ||
      radius == 0 || radius > kMaxRevealRadiusCells) {
    return std::nullopt;
  }
  return Command{HideArea{.cell = cell, .radius = static_cast<int>(radius)}};
}

auto DecodeAwardXpCommand(ByteReader& reader) -> std::optional<Command> {
  std::uint32_t id = 0;
  std::int32_t amount = 0;
  std::uint8_t party = 0;
  if (!reader.U32(id) || !reader.I32(amount) || !reader.U8(party) ||
      !reader.Done() || amount <= 0 || amount > kMaxXpAward || party > 1) {
    return std::nullopt;
  }
  return Command{AwardXp{.id = id, .amount = amount, .party = party != 0}};
}

auto DecodeUpdateTokenSheetCommand(ByteReader& reader)
    -> std::optional<Command> {
  std::uint32_t id = 0;
  std::int32_t ac = 10;
  std::int32_t speed = 9;
  std::string notes;
  std::int32_t attack_bonus = 0;
  std::int32_t save_bonus = 0;
  std::int32_t initiative_bonus = 0;
  std::string damage;
  std::int32_t temp_hp = 0;
  std::int32_t hit_die = 8;
  std::int32_t con_modifier = 0;
  std::int32_t attack_range = 0;
  std::uint32_t resist = 0;
  std::uint32_t vulnerable = 0;
  std::uint32_t immune = 0;
  std::int32_t swim_speed = 0;
  std::int32_t climb_speed = 0;
  std::int32_t darkvision_cells = 0;
  if (!reader.U32(id) || !reader.I32(ac) || !reader.I32(speed) ||
      !reader.String(notes, kMaxNotes) || !reader.I32(attack_bonus) ||
      !reader.I32(save_bonus) || !reader.I32(initiative_bonus) ||
      !reader.String(damage, kMaxNotes) || !reader.I32(temp_hp) ||
      !reader.I32(hit_die) || !reader.I32(con_modifier) ||
      !reader.I32(attack_range) || !reader.U32(resist) ||
      !reader.U32(vulnerable) || !reader.U32(immune) ||
      !reader.I32(swim_speed) || !reader.I32(climb_speed) ||
      !reader.I32(darkvision_cells) || !reader.Done() ||
      !IsValidHitDie(hit_die) || con_modifier < -kMaxConModifier ||
      con_modifier > kMaxConModifier || attack_range < 0 ||
      attack_range > 1000 || swim_speed < 0 || swim_speed > 1000 ||
      climb_speed < 0 || climb_speed > 1000 || darkvision_cells < 0 ||
      darkvision_cells > kMaxLightCells || !IsValidDamageTypeBit(resist) ||
      !IsValidDamageTypeBit(vulnerable) || !IsValidDamageTypeBit(immune)) {
    return std::nullopt;
  }
  return Command{UpdateTokenSheet{.id = id,
                                  .ac = ac,
                                  .speed = speed,
                                  .notes = std::move(notes),
                                  .attack_bonus = attack_bonus,
                                  .save_bonus = save_bonus,
                                  .initiative_bonus = initiative_bonus,
                                  .damage = std::move(damage),
                                  .temp_hp = temp_hp,
                                  .hit_die = hit_die,
                                  .con_modifier = con_modifier,
                                  .attack_range = attack_range,
                                  .resist = resist,
                                  .vulnerable = vulnerable,
                                  .immune = immune,
                                  .swim_speed = swim_speed,
                                  .climb_speed = climb_speed,
                                  .darkvision_cells = darkvision_cells}};
}

auto DecodeSetTokenLightCommand(ByteReader& reader) -> std::optional<Command> {
  std::uint32_t id = 0;
  std::int32_t bright = 0;
  std::int32_t dim = 0;
  if (!reader.U32(id) || !reader.I32(bright) || !reader.I32(dim) ||
      !reader.Done() || bright < 0 || bright > kMaxLightCells || dim < bright ||
      dim > kMaxLightCells) {
    return std::nullopt;
  }
  return Command{
      SetTokenLight{.id = id, .bright_cells = bright, .dim_cells = dim}};
}

auto DecodeUpdateTokenCombatCommand(ByteReader& reader)
    -> std::optional<Command> {
  std::uint32_t id = 0;
  std::uint32_t count = 0;
  if (!reader.U32(id) || !reader.U32(count) || count > kMaxConditions) {
    return std::nullopt;
  }
  std::vector<Condition> conditions;
  conditions.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    std::string condition_name;
    std::int32_t seconds = 0;
    std::int32_t save_dc = 0;
    if (!reader.String(condition_name, kMaxConditionLength) ||
        !reader.I32(seconds) || seconds < 0 || seconds > kMaxConditionSeconds ||
        !reader.I32(save_dc) || save_dc < 0 || save_dc > kMaxSaveDc) {
      return std::nullopt;
    }
    conditions.emplace_back(std::move(condition_name), seconds, save_dc);
  }
  if (!reader.Done()) {
    return std::nullopt;
  }
  return Command{
      UpdateTokenCombat{.id = id, .conditions = std::move(conditions)}};
}

auto DecodeAoeUpdatedEvent(ByteReader& reader) -> std::optional<Event> {
  AoeTemplate aoe;
  if (!ReadAoe(reader, aoe) || !reader.Done()) {
    return std::nullopt;
  }
  return Event{AoeUpdated{.aoe = aoe}};
}

auto DecodePingMarkerEvent(ByteReader& reader) -> std::optional<Event> {
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::string author;
  if (!reader.I32(x) || !reader.I32(y) ||
      !reader.String(author, kMaxNameLength) || !reader.Done()) {
    return std::nullopt;
  }
  return Event{
      PingMarker{.cell = Cell{.x = x, .y = y}, .author = std::move(author)}};
}

auto DecodeMapBackgroundUpdatedEvent(ByteReader& reader)
    -> std::optional<Event> {
  MapBackground background;
  if (!ReadMapBackground(reader, background) || !reader.Done()) {
    return std::nullopt;
  }
  return Event{MapBackgroundUpdated{.background = std::move(background)}};
}

auto DecodeRevealedUpdatedEvent(ByteReader& reader) -> std::optional<Event> {
  std::uint32_t count = 0;
  if (!reader.U32(count) || count > kMaxRevealedCells) {
    return std::nullopt;
  }
  std::vector<Cell> cells;
  cells.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    Cell cell;
    if (!ReadPosition(reader, cell) || cell.x < 0 || cell.y < 0 ||
        std::cmp_greater_equal(cell.x, kMaxDimension) ||
        std::cmp_greater_equal(cell.y, kMaxDimension)) {
      return std::nullopt;
    }
    cells.push_back(cell);
  }
  if (!reader.Done()) {
    return std::nullopt;
  }
  return Event{RevealedUpdated{.cells = std::move(cells)}};
}

auto DecodeFogChangedEvent(ByteReader& reader) -> std::optional<Event> {
  std::uint32_t count = 0;
  if (!reader.U32(count) || count > kMaxRevealedCells) {
    return std::nullopt;
  }
  std::vector<FogCell> cells;
  cells.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    Cell cell;
    std::uint8_t flags = 0;
    std::uint8_t light = 0;
    if (!ReadPosition(reader, cell) || cell.x < 0 || cell.y < 0 ||
        std::cmp_greater_equal(cell.x, kMaxDimension) ||
        std::cmp_greater_equal(cell.y, kMaxDimension) || !reader.U8(flags) ||
        flags > 3 || !reader.U8(light) ||
        light > static_cast<std::uint8_t>(LightLevel::kBright)) {
      return std::nullopt;
    }
    cells.push_back(FogCell{
        .cell = cell,
        .visible = (flags & 1) != 0,
        .explored = (flags & 2) != 0,
        .light = light,
    });
  }
  if (!reader.Done()) {
    return std::nullopt;
  }
  return Event{FogChanged{.cells = std::move(cells)}};
}

auto DecodeEncounterUpdatedEvent(ByteReader& reader) -> std::optional<Event> {
  Encounter encounter;
  if (!ReadEncounter(reader, encounter) || !reader.Done()) {
    return std::nullopt;
  }
  return Event{EncounterUpdated{.encounter = std::move(encounter)}};
}

auto DecodeTokenMovedEvent(ByteReader& reader) -> std::optional<Event> {
  std::uint32_t id = 0;
  Cell from;
  Cell to;
  if (!reader.U32(id) || !ReadPosition(reader, from) ||
      !ReadPosition(reader, to) || !reader.Done()) {
    return std::nullopt;
  }
  return Event{TokenMoved{.id = id, .from = from, .to = to}};
}

auto DecodeViewUpdatedEvent(ByteReader& reader) -> std::optional<Event> {
  std::uint32_t snapshot_size = 0;
  std::vector<std::uint8_t> snapshot_bytes;
  if (!reader.U32(snapshot_size) ||
      !reader.Bytes(snapshot_size, snapshot_bytes)) {
    return std::nullopt;
  }
  std::optional<GameSnapshot> snapshot =
      DecodeSnapshot(snapshot_bytes.data(), snapshot_bytes.size());
  if (!snapshot.has_value()) {
    return std::nullopt;
  }

  std::uint32_t fog_size = 0;
  std::vector<std::uint8_t> fog_bytes;
  if (!reader.U32(fog_size) || !reader.Bytes(fog_size, fog_bytes) ||
      !reader.Done()) {
    return std::nullopt;
  }
  std::optional<FogSnapshot> fog =
      DecodeFog(fog_bytes.data(), fog_bytes.size());
  if (!fog.has_value()) {
    return std::nullopt;
  }
  return Event{ViewUpdated{
      .snapshot = std::move(*snapshot),
      .fog = std::move(*fog),
  }};
}

auto DecodeLogLineEvent(ByteReader& reader) -> std::optional<Event> {
  std::string author;
  std::string text;
  std::uint32_t timestamp = 0;
  if (!reader.String(author, kMaxLogAuthor) ||
      !reader.String(text, kMaxLogText) || !reader.U32(timestamp) ||
      !reader.Done()) {
    return std::nullopt;
  }
  return Event{LogLineAdded{
      .line = LogLine{.author = std::move(author),
                      .text = std::move(text),
                      .timestamp = timestamp},
  }};
}

auto DecodeTokenAddedEvent(ByteReader& reader) -> std::optional<Event> {
  Token token;
  if (!ReadToken(reader, token, SnapshotFormat{}) || !reader.Done()) {
    return std::nullopt;
  }
  return Event{TokenAdded{.token = std::move(token)}};
}

auto DecodeTokenRemovedEvent(ByteReader& reader) -> std::optional<Event> {
  std::uint32_t id = 0;
  if (!reader.U32(id) || !reader.Done()) {
    return std::nullopt;
  }
  return Event{TokenRemoved{.id = id}};
}

auto DecodeTokenUpdatedEvent(ByteReader& reader) -> std::optional<Event> {
  Token token;
  if (!ReadToken(reader, token, SnapshotFormat{}) || !reader.Done()) {
    return std::nullopt;
  }
  return Event{TokenUpdated{.token = std::move(token)}};
}

auto WriteFogRecords(ByteWriter& writer, const std::vector<FogRecord>& records)
    -> void {
  writer.U32(static_cast<std::uint32_t>(records.size()));
  for (const FogRecord& record : records) {
    writer.String(record.owner);
    writer.U32(static_cast<std::uint32_t>(record.explored.size()));
    writer.Bytes(record.explored.data(), record.explored.size());
  }
}

// Каждая запись обязана описывать ровно клетки этой карты: иначе файл чужой.
auto ReadFogRecords(ByteReader& reader, std::uint32_t map_cells,
                    std::vector<FogRecord>& records) -> bool {
  std::uint32_t count = 0;
  if (!reader.U32(count) || count > kMaxFogRecords) {
    return false;
  }
  records.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    FogRecord record;
    std::uint32_t cells = 0;
    if (!reader.String(record.owner, kMaxNameLength) || !reader.U32(cells) ||
        cells != map_cells || !reader.Bytes(cells, record.explored)) {
      return false;
    }
    records.push_back(std::move(record));
  }
  return true;
}

// Палитра урона карты: индекс 0 — «нет урона», остальные ссылаются клетки.
auto WriteHazards(ByteWriter& writer, const std::vector<HazardProfile>& hazards)
    -> void {
  writer.U32(static_cast<std::uint32_t>(hazards.size()));
  for (const HazardProfile& hazard : hazards) {
    writer.String(hazard.damage);
    writer.U8(static_cast<std::uint8_t>(hazard.type));
    writer.U8(hazard.triggers);
    writer.I32(hazard.save_dc);
    writer.U8(hazard.half_on_success ? 1 : 0);
  }
}

auto ReadHazards(ByteReader& reader, std::vector<HazardProfile>& hazards)
    -> bool {
  std::uint32_t count = 0;
  if (!reader.U32(count) || count == 0 || count > kMaxHazards) {
    return false;
  }
  hazards.clear();
  hazards.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    HazardProfile hazard;
    std::uint8_t type = 0;
    std::uint8_t triggers = 0;
    std::uint8_t half = 0;
    if (!reader.String(hazard.damage, kMaxDamageExpression) ||
        !reader.U8(type) || !reader.U8(triggers) ||
        !reader.I32(hazard.save_dc) || !reader.U8(half)) {
      return false;
    }
    if (type > static_cast<std::uint8_t>(DamageType::kThunder) ||
        !IsValidHazardTriggers(triggers) || hazard.save_dc < 0 ||
        hazard.save_dc > 100 || half > 1 ||
        (i == kNoHazard && !hazard.damage.empty())) {
      return false;
    }
    DiceExpression expression;
    if (!hazard.damage.empty() &&
        !ParseDiceExpression(hazard.damage, expression)) {
      return false;
    }
    hazard.type = static_cast<DamageType>(type);
    hazard.triggers = triggers;
    hazard.half_on_success = half != 0;
    hazards.push_back(std::move(hazard));
  }
  return true;
}

auto WriteCellProps(ByteWriter& writer, const CellProps& props,
                    const SnapshotFormat& format) -> void {
  writer.U8(props.flags);
  writer.U8(static_cast<std::uint8_t>(props.water));
  writer.U8(props.current_dir);
  writer.U8(props.current_strength);
  writer.U8(props.hazard);
  writer.I32(props.slippery_dc);
  writer.U8(props.cover_sides);
  if (format.has_vision) {
    writer.U8(props.light);
    writer.U8(static_cast<std::uint8_t>(props.obscurement));
  }
}

auto ReadCellProps(ByteReader& reader, CellProps& props,
                   const std::vector<HazardProfile>& hazards,
                   const SnapshotFormat& format) -> bool {
  std::uint8_t flags = 0;
  std::uint8_t water = 0;
  std::uint8_t hazard = 0;
  std::uint8_t light = kAmbientLight;
  std::uint8_t obscurement = 0;
  if (!reader.U8(flags) || !reader.U8(water) || !reader.U8(props.current_dir) ||
      !reader.U8(props.current_strength) || !reader.U8(hazard) ||
      !reader.I32(props.slippery_dc) || !reader.U8(props.cover_sides)) {
    return false;
  }
  if (format.has_vision && (!reader.U8(light) || !reader.U8(obscurement))) {
    return false;
  }
  if ((flags & ~kCellAllFlags) != 0 || water > 2 || props.current_dir > 8 ||
      props.current_strength > 3 || hazard >= hazards.size() ||
      props.slippery_dc < 0 || props.slippery_dc > 100 ||
      !IsValidLightValue(light) || !IsValidObscurement(obscurement)) {
    return false;
  }
  props.flags = flags;
  props.water = static_cast<WaterDepth>(water);
  props.hazard = hazard;
  props.light = light;
  props.obscurement = static_cast<Obscurement>(obscurement);
  return true;
}

}  // namespace

auto EncodeSnapshot(const GameSnapshot& snapshot) -> std::vector<std::uint8_t> {
  return EncodeSnapshot(snapshot, SnapshotFormat{});
}

auto EncodeSnapshot(const GameSnapshot& snapshot, const SnapshotFormat& format)
    -> std::vector<std::uint8_t> {
  ByteWriter writer;
  writer.U32(static_cast<std::uint32_t>(snapshot.map.Width()));
  writer.U32(static_cast<std::uint32_t>(snapshot.map.Height()));
  for (int y = 0; y < snapshot.map.Height(); ++y) {
    for (int x = 0; x < snapshot.map.Width(); ++x) {
      writer.U8(static_cast<std::uint8_t>(
          snapshot.map.TerrainAt(Cell{.x = x, .y = y})));
    }
  }
  if (format.has_cell_props) {
    WriteHazards(writer, snapshot.map.Hazards());
    for (int y = 0; y < snapshot.map.Height(); ++y) {
      for (int x = 0; x < snapshot.map.Width(); ++x) {
        WriteCellProps(writer, snapshot.map.PropsAt(Cell{.x = x, .y = y}),
                       format);
      }
    }
    if (format.has_vision) {
      writer.U8(static_cast<std::uint8_t>(snapshot.map.AmbientLight()));
    }
  }
  writer.U32(static_cast<std::uint32_t>(snapshot.tokens.size()));
  for (const Token& token : snapshot.tokens) {
    WriteToken(writer, token, format);
  }
  writer.U32(snapshot.next_token_id);
  WriteEncounter(writer, snapshot.encounter);
  WriteAoe(writer, snapshot.aoe);
  WriteMapBackground(writer, snapshot.background);
  writer.U32(static_cast<std::uint32_t>(snapshot.revealed.size()));
  for (const Cell cell : snapshot.revealed) {
    writer.I32(cell.x);
    writer.I32(cell.y);
  }
  WriteFogRecords(writer, snapshot.fog);
  if (format.has_game_clock) {
    writer.U32(
        static_cast<std::uint32_t>(std::max(0, snapshot.game_time_seconds)));
    writer.I32(snapshot.last_long_rest_seconds);
  }
  return writer.Data();
}

auto DecodeSnapshot(const std::uint8_t* data, std::size_t size,
                    const SnapshotFormat& format)
    -> std::optional<GameSnapshot> {
  ByteReader reader(data, size);
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  if (!reader.U32(width) || !reader.U32(height)) {
    return std::nullopt;
  }
  if (width == 0 || height == 0 || width > kMaxDimension ||
      height > kMaxDimension) {
    return std::nullopt;
  }

  GridMap map(static_cast<int>(width), static_cast<int>(height));
  for (int y = 0; y < map.Height(); ++y) {
    for (int x = 0; x < map.Width(); ++x) {
      std::uint8_t terrain = 0;
      if (!reader.U8(terrain)) {
        return std::nullopt;
      }
      if (terrain >= kTerrainCount) {
        return std::nullopt;
      }
      map.SetTerrain(Cell{.x = x, .y = y}, static_cast<Terrain>(terrain));
    }
  }
  if (format.has_cell_props) {
    std::vector<HazardProfile> hazards;
    if (!ReadHazards(reader, hazards)) {
      return std::nullopt;
    }
    map.SetHazards(std::move(hazards));
    for (int y = 0; y < map.Height(); ++y) {
      for (int x = 0; x < map.Width(); ++x) {
        CellProps props;
        if (!ReadCellProps(reader, props, map.Hazards(), format)) {
          return std::nullopt;
        }
        map.SetProps(Cell{.x = x, .y = y}, props);
      }
    }
    if (format.has_vision) {
      std::uint8_t ambient = 0;
      if (!reader.U8(ambient) ||
          ambient > static_cast<std::uint8_t>(LightLevel::kBright)) {
        return std::nullopt;
      }
      map.SetAmbientLight(static_cast<LightLevel>(ambient));
    }
  }

  std::uint32_t token_count = 0;
  if (!reader.U32(token_count) || token_count > kMaxTokens) {
    return std::nullopt;
  }
  std::vector<Token> tokens;
  tokens.reserve(token_count);
  for (std::uint32_t i = 0; i < token_count; ++i) {
    Token token;
    if (!ReadToken(reader, token, format) || !map.Contains(token.cell)) {
      return std::nullopt;
    }
    tokens.push_back(std::move(token));
  }

  std::uint32_t next_token_id = 0;
  if (!reader.U32(next_token_id) || next_token_id == 0) {
    return std::nullopt;
  }
  Encounter encounter;
  AoeTemplate aoe;
  MapBackground background;
  if (!ReadEncounter(reader, encounter) || !ReadAoe(reader, aoe) ||
      !ReadMapBackground(reader, background)) {
    return std::nullopt;
  }

  std::uint32_t revealed_count = 0;
  const auto map_cells = static_cast<std::uint32_t>(map.Width()) *
                         static_cast<std::uint32_t>(map.Height());
  if (!reader.U32(revealed_count) || revealed_count > map_cells ||
      revealed_count > kMaxRevealedCells) {
    return std::nullopt;
  }
  std::vector<Cell> revealed;
  revealed.reserve(revealed_count);
  for (std::uint32_t i = 0; i < revealed_count; ++i) {
    Cell cell;
    if (!ReadPosition(reader, cell) || !map.Contains(cell)) {
      return std::nullopt;
    }
    revealed.push_back(cell);
  }
  std::vector<FogRecord> fog;
  if (!ReadFogRecords(reader, map_cells, fog)) {
    return std::nullopt;
  }
  std::uint32_t game_time = 0;
  std::int32_t last_long_rest = -1;
  if (format.has_game_clock) {
    if (!reader.U32(game_time) || !reader.I32(last_long_rest) ||
        game_time > static_cast<std::uint32_t>(kMaxGameTimeSeconds) ||
        last_long_rest < -1) {
      return std::nullopt;
    }
    const auto last_rest_time = static_cast<std::int64_t>(last_long_rest);
    const auto scene_time = static_cast<std::int64_t>(game_time);
    if (last_rest_time >= 0 && last_rest_time > scene_time) {
      return std::nullopt;
    }
  }
  if (!reader.Done()) {
    return std::nullopt;
  }

  return GameSnapshot{
      .map = std::move(map),
      .tokens = std::move(tokens),
      .next_token_id = next_token_id,
      .encounter = std::move(encounter),
      .aoe = aoe,
      .background = std::move(background),
      .revealed = std::move(revealed),
      .fog = std::move(fog),
      .game_time_seconds = static_cast<int>(game_time),
      .last_long_rest_seconds = last_long_rest,
  };
}

auto EncodeFog(const FogSnapshot& fog) -> std::vector<std::uint8_t> {
  ByteWriter writer;
  writer.U32(static_cast<std::uint32_t>(fog.width));
  writer.U32(static_cast<std::uint32_t>(fog.height));
  writer.Raw(fog.visible);
  writer.Raw(fog.explored);
  writer.Raw(fog.light);
  return writer.Data();
}

auto DecodeFog(const std::uint8_t* data, std::size_t size)
    -> std::optional<FogSnapshot> {
  ByteReader reader(data, size);
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  if (!reader.U32(width) || !reader.U32(height)) {
    return std::nullopt;
  }
  if (width == 0 || height == 0 || width > kMaxDimension ||
      height > kMaxDimension) {
    return std::nullopt;
  }
  const auto cells = width * height;
  if (reader.Remaining() != static_cast<std::size_t>(cells) * 3) {
    return std::nullopt;
  }

  FogSnapshot fog;
  fog.width = static_cast<int>(width);
  fog.height = static_cast<int>(height);
  if (!reader.Bytes(cells, fog.visible) || !reader.Bytes(cells, fog.explored) ||
      !reader.Bytes(cells, fog.light) || !reader.Done()) {
    return std::nullopt;
  }
  for (const std::uint8_t level : fog.light) {
    if (level > static_cast<std::uint8_t>(LightLevel::kBright)) {
      return std::nullopt;
    }
  }
  return fog;
}

auto EncodeCommand(const Command& command) -> std::vector<std::uint8_t> {
  ByteWriter writer;
  std::visit(
      [&writer](const auto& cmd) -> void {
        using T = std::decay_t<decltype(cmd)>;
        if constexpr (std::is_same_v<T, MoveToken>) {
          writer.U8(kCommandMoveToken);
          writer.U32(cmd.id);
          writer.I32(cmd.target.x);
          writer.I32(cmd.target.y);
          std::uint8_t flags = 0;
          if (cmd.crawl) {
            flags |= 0x1U;
          }
          if (cmd.jump) {
            flags |= 0x2U;
          }
          writer.U8(flags);
        } else if constexpr (std::is_same_v<T, Say>) {
          writer.U8(kCommandSay);
          writer.String(cmd.text);
        } else if constexpr (std::is_same_v<T, RollDice>) {
          writer.U8(kCommandRollDice);
          writer.U32(static_cast<std::uint32_t>(cmd.count));
          writer.U32(static_cast<std::uint32_t>(cmd.sides));
        } else if constexpr (std::is_same_v<T, RollExpression>) {
          writer.U8(kCommandRollExpression);
          writer.String(cmd.expression);
          writer.String(cmd.label);
        } else if constexpr (std::is_same_v<T, AddToken>) {
          writer.U8(kCommandAddToken);
          writer.String(cmd.name);
          writer.I32(cmd.cell.x);
          writer.I32(cmd.cell.y);
          writer.String(cmd.owner);
          writer.U8(static_cast<std::uint8_t>(cmd.kind));
          writer.I32(cmd.max_hp);
          writer.I32(cmd.ac);
          writer.I32(cmd.speed);
          writer.String(cmd.notes);
          writer.I32(cmd.attack_bonus);
          writer.I32(cmd.save_bonus);
          writer.String(cmd.damage);
          writer.I32(cmd.attack_range);
          writer.U32(cmd.resist);
          writer.U32(cmd.vulnerable);
          writer.U32(cmd.immune);
        } else if constexpr (std::is_same_v<T, RemoveToken>) {
          writer.U8(kCommandRemoveToken);
          writer.U32(cmd.id);
        } else if constexpr (std::is_same_v<T, SetTokenHp>) {
          writer.U8(kCommandSetTokenHp);
          writer.U32(cmd.id);
          writer.I32(cmd.hp);
        } else if constexpr (std::is_same_v<T, SetInitiative>) {
          writer.U8(kCommandSetInitiative);
          writer.U32(cmd.id);
          writer.I32(cmd.value);
        } else if constexpr (std::is_same_v<T, ClaimToken>) {
          writer.U8(kCommandClaimToken);
          writer.U32(cmd.id);
        } else if constexpr (std::is_same_v<T, ReleaseToken>) {
          writer.U8(kCommandReleaseToken);
          writer.U32(cmd.id);
        } else if constexpr (std::is_same_v<T, SetTokenOwner>) {
          writer.U8(kCommandSetTokenOwner);
          writer.U32(cmd.id);
          writer.String(cmd.owner);
        } else if constexpr (std::is_same_v<T, RenameToken>) {
          writer.U8(kCommandRenameToken);
          writer.U32(cmd.id);
          writer.String(cmd.name);
        } else if constexpr (std::is_same_v<T, SetTokenMaxHp>) {
          writer.U8(kCommandSetTokenMaxHp);
          writer.U32(cmd.id);
          writer.I32(cmd.max_hp);
        } else if constexpr (std::is_same_v<T, SetTokenColor>) {
          writer.U8(kCommandSetTokenColor);
          writer.U32(cmd.id);
          writer.U32(cmd.color);
        } else if constexpr (std::is_same_v<T, UpdateTokenSheet>) {
          writer.U8(kCommandUpdateTokenSheet);
          writer.U32(cmd.id);
          writer.I32(cmd.ac);
          writer.I32(cmd.speed);
          writer.String(cmd.notes);
          writer.I32(cmd.attack_bonus);
          writer.I32(cmd.save_bonus);
          writer.I32(cmd.initiative_bonus);
          writer.String(cmd.damage);
          writer.I32(cmd.temp_hp);
          writer.I32(cmd.hit_die);
          writer.I32(cmd.con_modifier);
          writer.I32(cmd.attack_range);
          writer.U32(cmd.resist);
          writer.U32(cmd.vulnerable);
          writer.U32(cmd.immune);
          writer.I32(cmd.swim_speed);
          writer.I32(cmd.climb_speed);
          writer.I32(cmd.darkvision_cells);
        } else if constexpr (std::is_same_v<T, SetTokenLight>) {
          writer.U8(kCommandSetTokenLight);
          writer.U32(cmd.id);
          writer.I32(cmd.bright_cells);
          writer.I32(cmd.dim_cells);
        } else if constexpr (std::is_same_v<T, UpdateTokenCombat>) {
          writer.U8(kCommandUpdateTokenCombat);
          writer.U32(cmd.id);
          writer.U32(static_cast<std::uint32_t>(cmd.conditions.size()));
          for (const Condition& condition : cmd.conditions) {
            writer.String(condition.name);
            writer.I32(condition.seconds);
            writer.I32(condition.save_dc);
          }
        } else if constexpr (std::is_same_v<T, StartEncounter>) {
          writer.U8(kCommandStartEncounter);
        } else if constexpr (std::is_same_v<T, EndEncounter>) {
          writer.U8(kCommandEndEncounter);
        } else if constexpr (std::is_same_v<T, NextTurn>) {
          writer.U8(kCommandNextTurn);
        } else if constexpr (std::is_same_v<T, SetEncounterComposition>) {
          writer.U8(kCommandSetEncounterComposition);
          writer.U32(cmd.id);
          writer.U8(cmd.joined ? 1 : 0);
        } else if constexpr (std::is_same_v<T, SetAoe>) {
          writer.U8(kCommandSetAoe);
          WriteAoe(writer, cmd.aoe);
        } else if constexpr (std::is_same_v<T, DamageToken>) {
          writer.U8(kCommandDamageToken);
          writer.U32(cmd.id);
          writer.I32(cmd.amount);
          writer.U8(static_cast<std::uint8_t>(cmd.damage_type));
        } else if constexpr (std::is_same_v<T, HealToken>) {
          writer.U8(kCommandHealToken);
          writer.U32(cmd.id);
          writer.I32(cmd.amount);
        } else if constexpr (std::is_same_v<T, RestParty>) {
          writer.U8(kCommandRestParty);
          writer.U8(cmd.long_rest ? 1 : 0);
          writer.I32(cmd.hit_dice);
        } else if constexpr (std::is_same_v<T, FallToken>) {
          writer.U8(kCommandFallToken);
          writer.U32(cmd.id);
          writer.I32(cmd.feet);
        } else if constexpr (std::is_same_v<T, AdvanceTime>) {
          writer.U8(kCommandAdvanceTime);
          writer.I32(cmd.seconds);
        } else if constexpr (std::is_same_v<T, AoeDamage>) {
          writer.U8(kCommandAoeDamage);
          writer.String(cmd.expression);
          writer.I32(cmd.save_dc);
          writer.U8(static_cast<std::uint8_t>(cmd.damage_type));
        } else if constexpr (std::is_same_v<T, DeathSave>) {
          writer.U8(kCommandDeathSave);
          writer.U32(cmd.id);
        } else if constexpr (std::is_same_v<T, AttackToken>) {
          writer.U8(kCommandAttackToken);
          writer.U32(cmd.attacker);
          writer.U32(cmd.target);
          std::uint8_t flags = 0;
          if (cmd.advantage) {
            flags |= 0x1U;
          }
          if (cmd.disadvantage) {
            flags |= 0x2U;
          }
          if (cmd.reaction) {
            flags |= 0x4U;
          }
          writer.U8(flags);
        } else if constexpr (std::is_same_v<T, UseTurnAction>) {
          writer.U8(kCommandUseTurnAction);
          writer.U32(cmd.id);
          writer.U8(static_cast<std::uint8_t>(cmd.kind));
        } else if constexpr (std::is_same_v<T, PingAt>) {
          writer.U8(kCommandPing);
          writer.I32(cmd.cell.x);
          writer.I32(cmd.cell.y);
        } else if constexpr (std::is_same_v<T, SetMapBackground>) {
          writer.U8(kCommandSetMapBackground);
          WriteMapBackground(writer, cmd.background);
        } else if constexpr (std::is_same_v<T, SetTokenPortrait>) {
          writer.U8(kCommandSetTokenPortrait);
          writer.U32(cmd.id);
          writer.String(cmd.portrait);
        } else if constexpr (std::is_same_v<T, RevealArea>) {
          writer.U8(kCommandRevealArea);
          writer.I32(cmd.cell.x);
          writer.I32(cmd.cell.y);
          writer.U32(static_cast<std::uint32_t>(cmd.radius));
        } else if constexpr (std::is_same_v<T, HideArea>) {
          writer.U8(kCommandHideArea);
          writer.I32(cmd.cell.x);
          writer.I32(cmd.cell.y);
          writer.U32(static_cast<std::uint32_t>(cmd.radius));
        } else if constexpr (std::is_same_v<T, ClearRevealed>) {
          writer.U8(kCommandClearRevealed);
        } else if constexpr (std::is_same_v<T, AwardXp>) {
          writer.U8(kCommandAwardXp);
          writer.U32(cmd.id);
          writer.I32(cmd.amount);
          writer.U8(cmd.party ? 1 : 0);
        }
      },
      command);
  return writer.Data();
}

auto DecodeCommand(const std::uint8_t* data, std::size_t size)
    -> std::optional<Command> {
  ByteReader reader(data, size);
  std::uint8_t kind = 0;
  if (!reader.U8(kind)) {
    return std::nullopt;
  }
  switch (kind) {
    case kCommandMoveToken:
      return DecodeMoveCommand(reader);
    case kCommandSay:
      return DecodeSayCommand(reader);
    case kCommandRollDice:
      return DecodeRollCommand(reader);
    case kCommandRollExpression:
      return DecodeRollExpressionCommand(reader);
    case kCommandAddToken:
      return DecodeAddCommand(reader);
    case kCommandRemoveToken:
      return DecodeRemoveCommand(reader);
    case kCommandSetTokenHp:
      return DecodeSetHpCommand(reader);
    case kCommandSetInitiative:
      return DecodeSetInitiativeCommand(reader);
    case kCommandClaimToken:
      return DecodeClaimTokenCommand(reader);
    case kCommandReleaseToken:
      return DecodeReleaseTokenCommand(reader);
    case kCommandSetTokenOwner:
      return DecodeSetTokenOwnerCommand(reader);
    case kCommandRenameToken:
      return DecodeRenameTokenCommand(reader);
    case kCommandSetTokenMaxHp:
      return DecodeSetTokenMaxHpCommand(reader);
    case kCommandSetTokenColor:
      return DecodeSetTokenColorCommand(reader);
    case kCommandSetTokenLight:
      return DecodeSetTokenLightCommand(reader);
    case kCommandUpdateTokenSheet:
      return DecodeUpdateTokenSheetCommand(reader);
    case kCommandUpdateTokenCombat:
      return DecodeUpdateTokenCombatCommand(reader);
    case kCommandStartEncounter:
      return DecodeStartEncounterCommand(reader);
    case kCommandEndEncounter:
      return DecodeEndEncounterCommand(reader);
    case kCommandNextTurn:
      return DecodeNextTurnCommand(reader);
    case kCommandSetEncounterComposition:
      return DecodeSetEncounterCompositionCommand(reader);
    case kCommandSetAoe:
      return DecodeSetAoeCommand(reader);
    case kCommandDamageToken:
      return DecodeDamageCommand(reader);
    case kCommandHealToken:
      return DecodeHealCommand(reader);
    case kCommandRestParty:
      return DecodeRestCommand(reader);
    case kCommandFallToken:
      return DecodeFallCommand(reader);
    case kCommandAdvanceTime:
      return DecodeAdvanceTimeCommand(reader);
    case kCommandAoeDamage:
      return DecodeAoeDamageCommand(reader);
    case kCommandDeathSave:
      return DecodeDeathSaveCommand(reader);
    case kCommandAttackToken:
      return DecodeAttackCommand(reader);
    case kCommandPing:
      return DecodePingCommand(reader);
    case kCommandSetMapBackground:
      return DecodeSetMapBackgroundCommand(reader);
    case kCommandSetTokenPortrait:
      return DecodeSetTokenPortraitCommand(reader);
    case kCommandRevealArea:
      return DecodeRevealAreaCommand(reader);
    case kCommandHideArea:
      return DecodeHideAreaCommand(reader);
    case kCommandClearRevealed:
      return DecodeClearRevealedCommand(reader);
    case kCommandAwardXp:
      return DecodeAwardXpCommand(reader);
    case kCommandUseTurnAction:
      return DecodeUseTurnActionCommand(reader);
    default:
      return std::nullopt;
  }
}

auto EncodeEvent(const Event& event) -> std::vector<std::uint8_t> {
  ByteWriter writer;
  std::visit(
      [&writer](const auto& ev) -> void {
        using T = std::decay_t<decltype(ev)>;
        if constexpr (std::is_same_v<T, TokenMoved>) {
          writer.U8(kEventTokenMoved);
          writer.U32(ev.id);
          writer.I32(ev.from.x);
          writer.I32(ev.from.y);
          writer.I32(ev.to.x);
          writer.I32(ev.to.y);
        } else if constexpr (std::is_same_v<T, ViewUpdated>) {
          const std::vector<std::uint8_t> snapshot =
              EncodeSnapshot(ev.snapshot);
          const std::vector<std::uint8_t> fog = EncodeFog(ev.fog);
          writer.U8(kEventViewUpdated);
          writer.U32(static_cast<std::uint32_t>(snapshot.size()));
          writer.Raw(snapshot);
          writer.U32(static_cast<std::uint32_t>(fog.size()));
          writer.Raw(fog);
        } else if constexpr (std::is_same_v<T, LogLineAdded>) {
          writer.U8(kEventLogLineAdded);
          writer.String(ev.line.author);
          writer.String(ev.line.text);
          writer.U32(ev.line.timestamp);
        } else if constexpr (std::is_same_v<T, TokenAdded>) {
          writer.U8(kEventTokenAdded);
          WriteToken(writer, ev.token, SnapshotFormat{});
        } else if constexpr (std::is_same_v<T, TokenRemoved>) {
          writer.U8(kEventTokenRemoved);
          writer.U32(ev.id);
        } else if constexpr (std::is_same_v<T, TokenUpdated>) {
          writer.U8(kEventTokenUpdated);
          WriteToken(writer, ev.token, SnapshotFormat{});
        } else if constexpr (std::is_same_v<T, EncounterUpdated>) {
          writer.U8(kEventEncounterUpdated);
          WriteEncounter(writer, ev.encounter);
        } else if constexpr (std::is_same_v<T, AoeUpdated>) {
          writer.U8(kEventAoeUpdated);
          WriteAoe(writer, ev.aoe);
        } else if constexpr (std::is_same_v<T, PingMarker>) {
          writer.U8(kEventPingMarker);
          writer.I32(ev.cell.x);
          writer.I32(ev.cell.y);
          writer.String(ev.author);
        } else if constexpr (std::is_same_v<T, MapBackgroundUpdated>) {
          writer.U8(kEventMapBackgroundUpdated);
          WriteMapBackground(writer, ev.background);
        } else if constexpr (std::is_same_v<T, RevealedUpdated>) {
          writer.U8(kEventRevealedUpdated);
          writer.U32(static_cast<std::uint32_t>(ev.cells.size()));
          for (const Cell cell : ev.cells) {
            writer.I32(cell.x);
            writer.I32(cell.y);
          }
        } else if constexpr (std::is_same_v<T, FogChanged>) {
          writer.U8(kEventFogChanged);
          writer.U32(static_cast<std::uint32_t>(ev.cells.size()));
          for (const FogCell& cell : ev.cells) {
            writer.I32(cell.cell.x);
            writer.I32(cell.cell.y);
            writer.U8(static_cast<std::uint8_t>((cell.visible ? 1 : 0) |
                                                (cell.explored ? 2 : 0)));
            writer.U8(cell.light);
          }
        }
      },
      event);
  return writer.Data();
}

auto DecodeEvent(const std::uint8_t* data, std::size_t size)
    -> std::optional<Event> {
  ByteReader reader(data, size);
  std::uint8_t kind = 0;
  if (!reader.U8(kind)) {
    return std::nullopt;
  }
  switch (kind) {
    case kEventTokenMoved:
      return DecodeTokenMovedEvent(reader);
    case kEventViewUpdated:
      return DecodeViewUpdatedEvent(reader);
    case kEventLogLineAdded:
      return DecodeLogLineEvent(reader);
    case kEventTokenAdded:
      return DecodeTokenAddedEvent(reader);
    case kEventTokenRemoved:
      return DecodeTokenRemovedEvent(reader);
    case kEventTokenUpdated:
      return DecodeTokenUpdatedEvent(reader);
    case kEventEncounterUpdated:
      return DecodeEncounterUpdatedEvent(reader);
    case kEventAoeUpdated:
      return DecodeAoeUpdatedEvent(reader);
    case kEventPingMarker:
      return DecodePingMarkerEvent(reader);
    case kEventMapBackgroundUpdated:
      return DecodeMapBackgroundUpdatedEvent(reader);
    case kEventRevealedUpdated:
      return DecodeRevealedUpdatedEvent(reader);
    case kEventFogChanged:
      return DecodeFogChangedEvent(reader);
    default:
      return std::nullopt;
  }
}

}  // namespace dnd
