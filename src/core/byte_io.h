#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dnd {

// Минимальное little-endian бинарное кодирование, общее для файлов
// сохранения и сетевого протокола.
class ByteWriter {
 public:
  auto U8(std::uint8_t value) -> void;
  auto U32(std::uint32_t value) -> void;
  auto I32(std::int32_t value) -> void;
  auto String(const std::string& value) -> void;
  auto Raw(const std::vector<std::uint8_t>& data) -> void;

  [[nodiscard]] auto Data() const -> const std::vector<std::uint8_t>& {
    return data_;
  }
  [[nodiscard]] auto Size() const -> std::size_t { return data_.size(); }

 private:
  std::vector<std::uint8_t> data_;
};

class ByteReader {
 public:
  ByteReader(const std::uint8_t* data, std::size_t size);
  explicit ByteReader(const std::vector<std::uint8_t>& data);

  auto U8(std::uint8_t& value) -> bool;
  auto U32(std::uint32_t& value) -> bool;
  auto I32(std::int32_t& value) -> bool;
  auto String(std::string& value, std::uint32_t max_length) -> bool;
  auto Bytes(std::uint32_t length, std::vector<std::uint8_t>& out) -> bool;
  auto Skip(std::size_t count) -> bool;

  [[nodiscard]] auto Remaining() const -> std::size_t {
    return size_ - offset_;
  }
  [[nodiscard]] auto Done() const -> bool { return offset_ == size_; }
  [[nodiscard]] auto Data() const -> const std::uint8_t* {
    return data_ + offset_;
  }

 private:
  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t offset_ = 0;
};

}  // namespace dnd
