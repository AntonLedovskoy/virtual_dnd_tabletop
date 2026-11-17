#include "core/byte_io.h"

#include <utility>

namespace dnd {

auto ByteWriter::U8(std::uint8_t value) -> void { data_.push_back(value); }

auto ByteWriter::U32(std::uint32_t value) -> void {
  for (int shift = 0; shift < 32; shift += 8) {
    data_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
  }
}

auto ByteWriter::I32(std::int32_t value) -> void {
  U32(static_cast<std::uint32_t>(value));
}

auto ByteWriter::String(const std::string& value) -> void {
  U32(static_cast<std::uint32_t>(value.size()));
  data_.insert(data_.end(), value.begin(), value.end());
}

auto ByteWriter::Raw(const std::vector<std::uint8_t>& data) -> void {
  data_.insert(data_.end(), data.begin(), data.end());
}

ByteReader::ByteReader(const std::uint8_t* data, std::size_t size)
    : data_(data), size_(size) {}

ByteReader::ByteReader(const std::vector<std::uint8_t>& data)
    : data_(data.data()), size_(data.size()) {}

auto ByteReader::U8(std::uint8_t& value) -> bool {
  if (Remaining() < 1) {
    return false;
  }
  value = data_[offset_];
  ++offset_;
  return true;
}

auto ByteReader::U32(std::uint32_t& value) -> bool {
  if (Remaining() < 4) {
    return false;
  }
  std::uint32_t result = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    result |= static_cast<std::uint32_t>(data_[offset_]) << shift;
    ++offset_;
  }
  value = result;
  return true;
}

auto ByteReader::I32(std::int32_t& value) -> bool {
  std::uint32_t raw = 0;
  if (!U32(raw)) {
    return false;
  }
  value = static_cast<std::int32_t>(raw);
  return true;
}

auto ByteReader::String(std::string& value, std::uint32_t max_length) -> bool {
  std::uint32_t length = 0;
  if (!U32(length) || length > max_length || Remaining() < length) {
    return false;
  }
  value.assign(reinterpret_cast<const char*>(data_ + offset_), length);
  offset_ += length;
  return true;
}

auto ByteReader::Bytes(std::uint32_t length, std::vector<std::uint8_t>& out)
    -> bool {
  if (Remaining() < length) {
    return false;
  }
  out.assign(data_ + offset_, data_ + offset_ + length);
  offset_ += length;
  return true;
}

auto ByteReader::Skip(std::size_t count) -> bool {
  if (Remaining() < count) {
    return false;
  }
  offset_ += count;
  return true;
}

}  // namespace dnd
