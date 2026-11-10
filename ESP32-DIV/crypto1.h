// Crypto1 - cifra do MIFARE Classic.
// Port fiel de crapto1 (projeto MFOC / Proxmark3).
// Original: Copyright (C) 2008-2009 bla <blapost@gmail.com>
// Licenca original: GNU GPL v2 ou superior.
#pragma once
#include <stdint.h>

struct Crypto1State {
  uint32_t odd;
  uint32_t even;
};

void crypto1_create(Crypto1State* s, uint64_t key);
uint8_t crypto1_bit(Crypto1State* s, uint8_t in, int is_encrypted);
uint8_t crypto1_byte(Crypto1State* s, uint8_t in, int is_encrypted);
uint32_t crypto1_word(Crypto1State* s, uint32_t in, int is_encrypted);
uint32_t crypto1_prng_successor(uint32_t x, uint32_t n);
uint32_t crypto1_swap_endian(uint32_t x);
bool crypto1_self_test();
