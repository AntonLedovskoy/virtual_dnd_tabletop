#include "core/x25519.h"

#include <algorithm>
#include <functional>

#include "core/random.h"

namespace dnd {
namespace {

// Поле p = 2^255 - 19: пять долей по 51 биту.
constexpr std::uint64_t kLimbMask = (1ULL << 51) - 1;
// Промежуточные произведения долей не влезают в 64 бита.
__extension__ using Wide = unsigned __int128;

using Fe = std::array<std::uint64_t, 5>;

auto Load64(const std::uint8_t* data) -> std::uint64_t {
  std::uint64_t value = 0;
  for (int shift = 0; shift < 64; shift += 8) {
    value |= static_cast<std::uint64_t>(data[shift / 8]) << shift;
  }
  return value;
}

auto Store64(std::uint8_t* data, std::uint64_t value) -> void {
  for (int shift = 0; shift < 64; shift += 8) {
    data[shift / 8] = static_cast<std::uint8_t>(value >> shift);
  }
}

// Переносы между долями: каждая остаётся 51-битной, «лишнее» сверх 2^255
// возвращается в младшую долю как 19 (2^255 = 19 mod p). После вызова все
// доли строго меньше 2^51 — на это опирается вычитание.
auto Carry(Fe& h) -> void {
  for (;;) {
    std::uint64_t carry = 0;
    for (std::uint64_t& limb : h) {
      limb += carry;
      carry = limb >> 51;
      limb &= kLimbMask;
    }
    if (carry == 0) {
      return;
    }
    h[0] += 19 * carry;
  }
}

// Приводит накопленные произведения к пяти 51-битным долям.
auto Reduce(std::array<Wide, 5>& t) -> Fe {
  Fe r;
  r[0] = static_cast<std::uint64_t>(t[0]) & kLimbMask;
  t[1] += t[0] >> 51;
  r[1] = static_cast<std::uint64_t>(t[1]) & kLimbMask;
  t[2] += t[1] >> 51;
  r[2] = static_cast<std::uint64_t>(t[2]) & kLimbMask;
  t[3] += t[2] >> 51;
  r[3] = static_cast<std::uint64_t>(t[3]) & kLimbMask;
  t[4] += t[3] >> 51;
  r[4] = static_cast<std::uint64_t>(t[4]) & kLimbMask;
  r[0] += 19 * static_cast<std::uint64_t>(t[4] >> 51);
  Carry(r);
  return r;
}

auto Add(const Fe& a, const Fe& b) -> Fe {
  Fe r;
  std::ranges::transform(a, b, r.begin(), std::plus<>{});
  Carry(r);
  return r;
}

// a - b mod p. Доли вычитаются с заёмом; если a < b, полученное значение
// на 19 больше остатка, потому что 2^255 = 19 (mod p).
auto Sub(const Fe& a, const Fe& b) -> Fe {
  Fe r;
  std::uint64_t borrow = 0;
  for (std::size_t i = 0; i < r.size(); ++i) {
    const std::uint64_t subtrahend = b[i] + borrow;
    borrow = a[i] < subtrahend ? 1 : 0;
    r[i] = (a[i] - subtrahend) & kLimbMask;
  }
  if (borrow != 0) {
    std::uint64_t take = 19;
    bool underflow = false;
    for (std::uint64_t& limb : r) {
      const std::uint64_t low = limb;
      underflow = low < take;
      limb = (low - take) & kLimbMask;
      take = underflow ? 1 : 0;
    }
    if (underflow) {
      // Модуль меньше 19: вычтенный остаток уже на 19 меньше нужного.
      // После заёма младшая доля не меньше 2^51 - 18, вычитание безопасно.
      r[0] -= 19;
    }
  }
  return r;
}

auto Mul(const Fe& f, const Fe& g) -> Fe {
  std::array<Wide, 5> t{};
  t[0] = static_cast<Wide>(f[0]) * g[0] +
         static_cast<Wide>(19) *
             (static_cast<Wide>(f[1]) * g[4] + static_cast<Wide>(f[2]) * g[3] +
              static_cast<Wide>(f[3]) * g[2] + static_cast<Wide>(f[4]) * g[1]);
  t[1] = static_cast<Wide>(f[0]) * g[1] + static_cast<Wide>(f[1]) * g[0] +
         static_cast<Wide>(19) *
             (static_cast<Wide>(f[2]) * g[4] + static_cast<Wide>(f[3]) * g[3] +
              static_cast<Wide>(f[4]) * g[2]);
  t[2] = static_cast<Wide>(f[0]) * g[2] + static_cast<Wide>(f[1]) * g[1] +
         static_cast<Wide>(f[2]) * g[0] +
         static_cast<Wide>(19) *
             (static_cast<Wide>(f[3]) * g[4] + static_cast<Wide>(f[4]) * g[3]);
  t[3] = static_cast<Wide>(f[0]) * g[3] + static_cast<Wide>(f[1]) * g[2] +
         static_cast<Wide>(f[2]) * g[1] + static_cast<Wide>(f[3]) * g[0] +
         static_cast<Wide>(19) * static_cast<Wide>(f[4]) * g[4];
  t[4] = static_cast<Wide>(f[0]) * g[4] + static_cast<Wide>(f[1]) * g[3] +
         static_cast<Wide>(f[2]) * g[2] + static_cast<Wide>(f[3]) * g[1] +
         static_cast<Wide>(f[4]) * g[0];
  return Reduce(t);
}

auto Square(const Fe& f) -> Fe { return Mul(f, f); }

// Умножение на (A - 2) / 4 = 121665 из лестницы Монтгомери.
auto Mul121665(const Fe& f) -> Fe {
  std::array<Wide, 5> t{};
  for (std::size_t i = 0; i < 5; ++i) {
    t[i] = static_cast<Wide>(f[i]) * 121665;
  }
  return Reduce(t);
}

// Обмен без ветвления: не зависит от значения секретного скаляра.
auto Cswap(std::uint64_t swap, Fe& a, Fe& b) -> void {
  const std::uint64_t mask =
      static_cast<std::uint64_t>(0) - static_cast<std::uint64_t>(swap != 0);
  for (std::size_t i = 0; i < a.size(); ++i) {
    const std::uint64_t difference = mask & (a[i] ^ b[i]);
    a[i] ^= difference;
    b[i] ^= difference;
  }
}

// Обратный элемент по модулю p: z^(p-2), показатель — p - 2 = 2^255 - 21.
auto Invert(const Fe& z) -> Fe {
  std::array<std::uint8_t, 32> exponent;
  exponent.fill(0xFF);
  exponent[0] = 0xEB;
  exponent[31] = 0x7F;

  Fe result{1, 0, 0, 0, 0};
  for (int bit = 254; bit >= 0; --bit) {
    result = Square(result);
    if (((exponent[static_cast<std::size_t>(bit) >> 3] >>
          (static_cast<unsigned>(bit) & 7U)) &
         1U) != 0U) {
      result = Mul(result, z);
    }
  }
  return result;
}

auto FromBytes(const X25519Key& bytes) -> Fe {
  Fe h;
  h[0] = Load64(bytes.data()) & kLimbMask;
  h[1] = (Load64(bytes.data() + 6) >> 3) & kLimbMask;
  h[2] = (Load64(bytes.data() + 12) >> 6) & kLimbMask;
  h[3] = (Load64(bytes.data() + 19) >> 1) & kLimbMask;
  h[4] = (Load64(bytes.data() + 24) >> 12) & kLimbMask;
  return h;
}

auto ToBytes(const Fe& h) -> X25519Key {
  std::array<std::uint64_t, 4> words{};
  words[0] = h[0] | (h[1] << 51);
  words[1] = (h[1] >> 13) | (h[2] << 38);
  words[2] = (h[2] >> 26) | (h[3] << 25);
  words[3] = (h[3] >> 39) | (h[4] << 12);

  X25519Key bytes{};
  for (std::size_t i = 0; i < words.size(); ++i) {
    Store64(bytes.data() + (i * 8), words[i]);
  }
  return bytes;
}

auto ScalarMult(const X25519Key& scalar, const X25519Key& point) -> X25519Key {
  // RFC 7748: у скаляра обнуляются младшие три бита и два старших,
  // предпоследний выставляется; у точки игнорируется старший бит.
  X25519Key k = scalar;
  k[0] &= 248;
  k[31] &= 127;
  k[31] |= 64;
  X25519Key u = point;
  u[31] &= 127;

  const Fe x1 = FromBytes(u);
  Fe x2{1, 0, 0, 0, 0};
  Fe z2{0, 0, 0, 0, 0};
  Fe x3 = x1;
  Fe z3{1, 0, 0, 0, 0};
  std::uint64_t swap = 0;

  for (int bit = 254; bit >= 0; --bit) {
    const std::uint64_t current = (k[static_cast<std::size_t>(bit) >> 3] >>
                                   (static_cast<unsigned>(bit) & 7U)) &
                                  1U;
    swap ^= current;
    Cswap(swap, x2, x3);
    Cswap(swap, z2, z3);
    swap = current;

    const Fe a = Add(x2, z2);
    const Fe aa = Square(a);
    const Fe b = Sub(x2, z2);
    const Fe bb = Square(b);
    const Fe e = Sub(aa, bb);
    const Fe c = Add(x3, z3);
    const Fe d = Sub(x3, z3);
    const Fe da = Mul(d, a);
    const Fe cb = Mul(c, b);
    const Fe da_plus_cb = Add(da, cb);
    const Fe da_minus_cb = Sub(da, cb);
    x3 = Square(da_plus_cb);
    z3 = Mul(x1, Square(da_minus_cb));
    x2 = Mul(aa, bb);
    z2 = Mul(e, Add(aa, Mul121665(e)));
  }

  Cswap(swap, x2, x3);
  Cswap(swap, z2, z3);
  return ToBytes(Mul(x2, Invert(z2)));
}

}  // namespace

auto GenerateX25519KeyPair() -> X25519KeyPair {
  X25519KeyPair pair;
  (void)FillRandom(pair.private_key);
  pair.public_key = X25519PublicKey(pair.private_key);
  return pair;
}

auto X25519PublicKey(const X25519Key& private_key) -> X25519Key {
  X25519Key base_point{};
  base_point[0] = 9;
  return ScalarMult(private_key, base_point);
}

auto X25519SharedSecret(const X25519Key& private_key,
                        const X25519Key& peer_public, X25519Key& shared)
    -> bool {
  shared = ScalarMult(private_key, peer_public);
  return std::ranges::any_of(
      shared, [](std::uint8_t byte) -> bool { return byte != 0; });
}

}  // namespace dnd
