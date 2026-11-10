// Crypto1 - cifra do MIFARE Classic.
// Port fiel de crapto1 (projeto MFOC / Proxmark3), GPL v2+.
// Original: Copyright (C) 2008-2009 bla <blapost@gmail.com>
#include "crypto1.h"

#include <Arduino.h>

#define LF_POLY_ODD 0x29CE5Cu
#define LF_POLY_EVEN 0x870804u

static inline uint32_t bit32(uint32_t x, uint32_t n) {
  return (x >> n) & 1u;
}

static inline uint32_t bebit(uint32_t x, uint32_t n) {
  return bit32(x, n ^ 24u);
}

static inline uint32_t bit64(uint64_t x, uint32_t n) {
  return (uint32_t)((x >> n) & 1ULL);
}

static inline uint32_t parity(uint32_t x) {
  x ^= x >> 16;
  x ^= x >> 8;
  x ^= x >> 4;
  return bit32(0x6996u, x & 0xFu);
}

static inline uint32_t crypto1_filter(uint32_t x) {
  uint32_t f;
  f = 0xf22c0u >> (x & 0xFu) & 16u;
  f |= 0x6c9c0u >> (x >> 4 & 0xFu) & 8u;
  f |= 0x3c8b0u >> (x >> 8 & 0xFu) & 4u;
  f |= 0x1e458u >> (x >> 12 & 0xFu) & 2u;
  f |= 0x0d938u >> (x >> 16 & 0xFu) & 1u;
  return bit32(0xEC57E80Au, f);
}

void crypto1_create(Crypto1State* s, uint64_t key) {
  s->odd = 0;
  s->even = 0;
  for (int i = 47; i > 0; i -= 2) {
    s->odd = (s->odd << 1) | bit64(key, (uint32_t)((i - 1) ^ 7));
    s->even = (s->even << 1) | bit64(key, (uint32_t)(i ^ 7));
  }
}

uint8_t crypto1_bit(Crypto1State* s, uint8_t in, int is_encrypted) {
  uint8_t ret = (uint8_t)crypto1_filter(s->odd);

  uint32_t feedin = (uint32_t)ret & (uint32_t)(is_encrypted ? 1 : 0);
  feedin ^= (uint32_t)(in & 1u);
  feedin ^= LF_POLY_ODD & s->odd;
  feedin ^= LF_POLY_EVEN & s->even;

  const uint32_t neweven = (s->even << 1) | parity(feedin);
  s->even = s->odd;
  s->odd = neweven;
  return ret;
}

uint8_t crypto1_byte(Crypto1State* s, uint8_t in, int is_encrypted) {
  uint8_t ret = 0;
  for (uint32_t i = 0; i < 8; ++i) {
    ret |= (uint8_t)(crypto1_bit(s, (uint8_t)bit32(in, i), is_encrypted) << i);
  }
  return ret;
}

uint32_t crypto1_word(Crypto1State* s, uint32_t in, int is_encrypted) {
  uint32_t ret = 0;
  for (uint32_t i = 0; i < 32; ++i) {
    ret |= (uint32_t)crypto1_bit(s, (uint8_t)bebit(in, i), is_encrypted) << (i ^ 24u);
  }
  return ret;
}

uint32_t crypto1_swap_endian(uint32_t x) {
  x = ((x >> 8) & 0x00ff00ffu) | ((x & 0x00ff00ffu) << 8);
  x = (x >> 16) | (x << 16);
  return x;
}

uint32_t crypto1_prng_successor(uint32_t x, uint32_t n) {
  x = crypto1_swap_endian(x);
  while (n--) {
    x = (x >> 1) | ((((x >> 16) ^ (x >> 18) ^ (x >> 19) ^ (x >> 21)) & 1u) << 31);
  }
  return crypto1_swap_endian(x);
}

// ---- autoteste com vetores derivados da referencia (crapto1) ----
bool crypto1_self_test() {
  struct Case {
    uint64_t key;
    uint32_t w0, w1;
    uint8_t b0;
  };
  static const Case cases[] = {
      {0xFFFFFFFFFFFFULL, 0xFF7F02DFu, 0xB9524F94u, 0x72},
      {0xA0A1A2A3A4A5ULL, 0x5A151A5Bu, 0x943CF1C5u, 0x88},
      {0x000000000000ULL, 0x885B86E9u, 0x438C51C5u, 0xFA},
  };

  bool ok = true;
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    Crypto1State s;
    crypto1_create(&s, cases[i].key);
    const uint32_t a = crypto1_word(&s, 0x01020304u, 0);
    const uint32_t b = crypto1_word(&s, 0x00000000u, 0);
    const uint8_t d = crypto1_byte(&s, 0x00, 0);
    if (a != cases[i].w0 || b != cases[i].w1 || d != cases[i].b0) {
      ok = false;
      Serial.printf("C1 case %012llX: got %08X %08X %02X want %08X %08X %02X\n",
                    (unsigned long long)cases[i].key, (unsigned)a, (unsigned)b, (unsigned)d,
                    (unsigned)cases[i].w0, (unsigned)cases[i].w1, (unsigned)cases[i].b0);
    }
  }

  Crypto1State s2;
  crypto1_create(&s2, 0xFFFFFFFFFFFFULL);
  const uint32_t dec = crypto1_word(&s2, 0xDEADBEEFu, 1);
  if (dec != 0xFF3FB6DEu) {
    ok = false;
    Serial.printf("C1 dec: got %08X want FF3FB6DE\n", (unsigned)dec);
  }

  if (crypto1_prng_successor(0x01020304u, 32) != 0xA3BD92D0u) ok = false;
  if (crypto1_prng_successor(0xDEADBEEFu, 64) != 0x40BF8252u) ok = false;
  if (crypto1_swap_endian(0x12345678u) != 0x78563412u) ok = false;

  return ok;
}
