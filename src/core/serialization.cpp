#include "core/serialization.h"

#include <string>
#include <type_traits>
#include <utility>

#include "core/byte_io.h"
#include "core/map_background.h"

namespace dnd {
namespace {

constexpr std::uint32_t kMaxDimension = 4096;
constexpr std::uint32_t kMaxTokens = 100000;
constexpr std::uint32_t kMaxNameLength = 256;
constexpr std::uint32_t kMaxChatText = 256;
constexpr std::uint32_t kMaxLogText = 512;
constexpr std::uint32_t kMaxLogAuthor = 64;
constexpr std::uint32_t kMaxNotes = 512;
constexpr std::uint32_t kMaxConditions = 16;
constexpr std::uint32_t kMaxConditionLength = 32;
constexpr std::int32_t kMaxConditionRounds = 100;

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

auto WriteToken(ByteWriter& writer, const Token& token) -> void {
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
    writer.I32(condition.rounds);
  }
  writer.I32(token.movement_used);
  writer.I32(token.death_save_successes);
  writer.I32(token.death_save_failures);
  writer.String(token.portrait);
}

auto ReadToken(ByteReader& reader, Token& token) -> bool {
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
  std::int32_t death_successes = 0;
  std::int32_t death_failures = 0;
  std::string portrait;
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
    std::int32_t rounds = 0;
    if (!reader.String(condition_name, kMaxConditionLength) ||
        !reader.I32(rounds) || rounds < 0 || rounds > kMaxConditionRounds) {
      return false;
    }
    conditions.emplace_back(std::move(condition_name), rounds);
  }
  if (!reader.I32(movement_used) || !reader.I32(death_successes) ||
      !reader.I32(death_failures) ||
      !reader.String(portrait, kMaxImageNameLength) || death_successes < 0 ||
      death_successes > 3 || death_failures < 0 || death_failures > 3) {
    return false;
  }
  if (max_hp < 0 || max_hp > 1000000 || temp_hp < 0 || temp_hp > 1000 ||
      initiative < -1000000 || initiative > 1000000 || ac <= 0 || ac > 1000 ||
      speed < 0 || speed > 1000 || attack_bonus < -100 || attack_bonus > 100 ||
      save_bonus < -100 || save_bonus > 100 || initiative_bonus < -100 ||
      initiative_bonus > 100 || movement_used < 0 || movement_used > 1000 ||
      kind > static_cast<std::uint8_t>(TokenKind::kMonster) ||
      (color & 0xFF000000U) != 0 ||
      (!portrait.empty() && !IsSafeImageName(portrait))) {
    return false;
  }
  token = Token{
      .id = id,
      .name = std::move(name),
      .owner = std::move(owner),
      .hp = hp,
      .max_hp = max_hp,
      .temp_hp = temp_hp,
      .initiative = initiative,
      .ac = ac,
      .speed = speed,
      .notes = std::move(notes),
      .attack_bonus = attack_bonus,
      .save_bonus = save_bonus,
      .initiative_bonus = initiative_bonus,
      .damage = std::move(damage),
      .conditions = std::move(conditions),
      .movement_used = movement_used,
      .death_save_successes = death_successes,
      .death_save_failures = death_failures,
      .kind = static_cast<TokenKind>(kind),
      .color = color,
      .portrait = std::move(portrait),
      .cell = Cell{.x = x, .y = y},
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
  if (!reader.U32(id) || !reader.I32(amount) || !reader.Done()) {
    return std::nullopt;
  }
  return Command{DamageToken{.id = id, .amount = amount}};
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
  if (!reader.U8(long_rest) || long_rest > 1 || !reader.Done()) {
    return std::nullopt;
  }
  return Command{RestParty{.long_rest = long_rest != 0}};
}

auto DecodeAoeDamageCommand(ByteReader& reader) -> std::optional<Command> {
  std::string expression;
  std::int32_t save_dc = 0;
  if (!reader.String(expression, kMaxNotes) || !reader.I32(save_dc) ||
      !reader.Done()) {
    return std::nullopt;
  }
  return Command{
      AoeDamage{.expression = std::move(expression), .save_dc = save_dc}};
}

auto DecodeDeathSaveCommand(ByteReader& reader) -> std::optional<Command> {
  std::uint32_t id = 0;
  if (!reader.U32(id) || !reader.Done()) {
    return std::nullopt;
  }
  return Command{DeathSave{.id = id}};
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

auto DecodeMoveCommand(ByteReader& reader) -> std::optional<Command> {
  std::uint32_t id = 0;
  Cell cell;
  if (!reader.U32(id) || !ReadPosition(reader, cell) || !reader.Done()) {
    return std::nullopt;
  }
  return Command{MoveToken{.id = id, .target = cell}};
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
  if (!reader.String(name, kMaxNameLength) || !ReadPosition(reader, cell) ||
      !reader.String(owner, kMaxNameLength) || !reader.U8(kind) ||
      !reader.I32(max_hp) || !reader.I32(ac) || !reader.I32(speed) ||
      !reader.String(notes, kMaxNotes) || !reader.I32(attack_bonus) ||
      !reader.I32(save_bonus) || !reader.String(damage, kMaxNotes) ||
      !reader.Done()) {
    return std::nullopt;
  }
  if (kind > static_cast<std::uint8_t>(TokenKind::kMonster) || max_hp <= 0 ||
      max_hp > 1000000 || ac <= 0 || ac > 1000 || speed < 0 || speed > 1000 ||
      attack_bonus < -100 || attack_bonus > 100 || save_bonus < -100 ||
      save_bonus > 100) {
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
  if (!reader.U32(id) || !reader.I32(ac) || !reader.I32(speed) ||
      !reader.String(notes, kMaxNotes) || !reader.I32(attack_bonus) ||
      !reader.I32(save_bonus) || !reader.I32(initiative_bonus) ||
      !reader.String(damage, kMaxNotes) || !reader.I32(temp_hp) ||
      !reader.Done()) {
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
                                  .temp_hp = temp_hp}};
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
    std::int32_t rounds = 0;
    if (!reader.String(condition_name, kMaxConditionLength) ||
        !reader.I32(rounds) || rounds < 0 || rounds > kMaxConditionRounds) {
      return std::nullopt;
    }
    conditions.emplace_back(std::move(condition_name), rounds);
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
  if (!ReadToken(reader, token) || !reader.Done()) {
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
  if (!ReadToken(reader, token) || !reader.Done()) {
    return std::nullopt;
  }
  return Event{TokenUpdated{.token = std::move(token)}};
}

}  // namespace

auto EncodeSnapshot(const GameSnapshot& snapshot) -> std::vector<std::uint8_t> {
  ByteWriter writer;
  writer.U32(static_cast<std::uint32_t>(snapshot.map.Width()));
  writer.U32(static_cast<std::uint32_t>(snapshot.map.Height()));
  for (int y = 0; y < snapshot.map.Height(); ++y) {
    for (int x = 0; x < snapshot.map.Width(); ++x) {
      writer.U8(snapshot.map.IsBlocked(Cell{.x = x, .y = y}) ? 1 : 0);
    }
  }
  writer.U32(static_cast<std::uint32_t>(snapshot.tokens.size()));
  for (const Token& token : snapshot.tokens) {
    WriteToken(writer, token);
  }
  writer.U32(snapshot.next_token_id);
  WriteEncounter(writer, snapshot.encounter);
  WriteAoe(writer, snapshot.aoe);
  WriteMapBackground(writer, snapshot.background);
  return writer.Data();
}

auto DecodeSnapshot(const std::uint8_t* data, std::size_t size)
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
      std::uint8_t blocked = 0;
      if (!reader.U8(blocked)) {
        return std::nullopt;
      }
      if (blocked != 0) {
        map.SetBlocked(Cell{.x = x, .y = y}, true);
      }
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
    if (!ReadToken(reader, token) || !map.Contains(token.cell)) {
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
      !ReadMapBackground(reader, background) || !reader.Done()) {
    return std::nullopt;
  }

  return GameSnapshot{
      .map = std::move(map),
      .tokens = std::move(tokens),
      .next_token_id = next_token_id,
      .encounter = std::move(encounter),
      .aoe = aoe,
      .background = std::move(background),
  };
}

auto EncodeFog(const FogSnapshot& fog) -> std::vector<std::uint8_t> {
  ByteWriter writer;
  writer.U32(static_cast<std::uint32_t>(fog.width));
  writer.U32(static_cast<std::uint32_t>(fog.height));
  writer.Raw(fog.visible);
  writer.Raw(fog.explored);
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
  if (reader.Remaining() != static_cast<std::size_t>(cells) * 2) {
    return std::nullopt;
  }

  FogSnapshot fog;
  fog.width = static_cast<int>(width);
  fog.height = static_cast<int>(height);
  if (!reader.Bytes(cells, fog.visible) || !reader.Bytes(cells, fog.explored) ||
      !reader.Done()) {
    return std::nullopt;
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
        } else if constexpr (std::is_same_v<T, UpdateTokenCombat>) {
          writer.U8(kCommandUpdateTokenCombat);
          writer.U32(cmd.id);
          writer.U32(static_cast<std::uint32_t>(cmd.conditions.size()));
          for (const Condition& condition : cmd.conditions) {
            writer.String(condition.name);
            writer.I32(condition.rounds);
          }
        } else if constexpr (std::is_same_v<T, StartEncounter>) {
          writer.U8(kCommandStartEncounter);
        } else if constexpr (std::is_same_v<T, EndEncounter>) {
          writer.U8(kCommandEndEncounter);
        } else if constexpr (std::is_same_v<T, NextTurn>) {
          writer.U8(kCommandNextTurn);
        } else if constexpr (std::is_same_v<T, SetAoe>) {
          writer.U8(kCommandSetAoe);
          WriteAoe(writer, cmd.aoe);
        } else if constexpr (std::is_same_v<T, DamageToken>) {
          writer.U8(kCommandDamageToken);
          writer.U32(cmd.id);
          writer.I32(cmd.amount);
        } else if constexpr (std::is_same_v<T, HealToken>) {
          writer.U8(kCommandHealToken);
          writer.U32(cmd.id);
          writer.I32(cmd.amount);
        } else if constexpr (std::is_same_v<T, RestParty>) {
          writer.U8(kCommandRestParty);
          writer.U8(cmd.long_rest ? 1 : 0);
        } else if constexpr (std::is_same_v<T, AoeDamage>) {
          writer.U8(kCommandAoeDamage);
          writer.String(cmd.expression);
          writer.I32(cmd.save_dc);
        } else if constexpr (std::is_same_v<T, DeathSave>) {
          writer.U8(kCommandDeathSave);
          writer.U32(cmd.id);
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
    case kCommandSetAoe:
      return DecodeSetAoeCommand(reader);
    case kCommandDamageToken:
      return DecodeDamageCommand(reader);
    case kCommandHealToken:
      return DecodeHealCommand(reader);
    case kCommandRestParty:
      return DecodeRestCommand(reader);
    case kCommandAoeDamage:
      return DecodeAoeDamageCommand(reader);
    case kCommandDeathSave:
      return DecodeDeathSaveCommand(reader);
    case kCommandPing:
      return DecodePingCommand(reader);
    case kCommandSetMapBackground:
      return DecodeSetMapBackgroundCommand(reader);
    case kCommandSetTokenPortrait:
      return DecodeSetTokenPortraitCommand(reader);
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
          WriteToken(writer, ev.token);
        } else if constexpr (std::is_same_v<T, TokenRemoved>) {
          writer.U8(kEventTokenRemoved);
          writer.U32(ev.id);
        } else if constexpr (std::is_same_v<T, TokenUpdated>) {
          writer.U8(kEventTokenUpdated);
          WriteToken(writer, ev.token);
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
    default:
      return std::nullopt;
  }
}

}  // namespace dnd
