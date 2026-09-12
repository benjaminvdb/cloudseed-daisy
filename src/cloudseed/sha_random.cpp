#include "sha_random.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

namespace cloudseed {
namespace sha_random {
namespace {

/*
 * SHA-256 after Olivier Gay's FIPS 180-2 implementation, as updated to C++
 * by zedwood.com in 2012 and used by CloudSeed (Utils/Sha256.cpp); this copy
 * drops the std::vector.
 *
 * Copyright (C) 2005, 2007 Olivier Gay <olivier.gay@a3.epfl.ch>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#define SHA2_SHFR(x, n) (x >> n)
#define SHA2_ROTR(x, n) ((x >> n) | (x << ((sizeof(x) << 3) - n)))
#define SHA2_CH(x, y, z) ((x & y) ^ (~x & z))
#define SHA2_MAJ(x, y, z) ((x & y) ^ (x & z) ^ (y & z))
#define SHA256_F1(x) (SHA2_ROTR(x, 2) ^ SHA2_ROTR(x, 13) ^ SHA2_ROTR(x, 22))
#define SHA256_F2(x) (SHA2_ROTR(x, 6) ^ SHA2_ROTR(x, 11) ^ SHA2_ROTR(x, 25))
#define SHA256_F3(x) (SHA2_ROTR(x, 7) ^ SHA2_ROTR(x, 18) ^ SHA2_SHFR(x, 3))
#define SHA256_F4(x) (SHA2_ROTR(x, 17) ^ SHA2_ROTR(x, 19) ^ SHA2_SHFR(x, 10))
#define SHA2_UNPACK32(x, str)            \
  {                                      \
    *((str) + 3) = (uint8_t)((x));       \
    *((str) + 2) = (uint8_t)((x) >> 8);  \
    *((str) + 1) = (uint8_t)((x) >> 16); \
    *((str) + 0) = (uint8_t)((x) >> 24); \
  }
#define SHA2_PACK32(str, x)                                                 \
  {                                                                         \
    *(x) = ((uint32_t)*((str) + 3)) | ((uint32_t)*((str) + 2) << 8) |       \
           ((uint32_t)*((str) + 1) << 16) | ((uint32_t)*((str) + 0) << 24); \
  }

class Sha256 {
 public:
  static constexpr unsigned int kDigestSize = 256 / 8;

  void Init();
  void Update(const unsigned char* message, unsigned int len);
  void Final(unsigned char* digest);

 private:
  static constexpr unsigned int kBlockSize = 512 / 8;
  static const uint32_t k_[64];

  void Transform(const unsigned char* message, unsigned int block_nb);

  unsigned int tot_len_;
  unsigned int len_;
  unsigned char block_[2 * kBlockSize];
  uint32_t h_[8];
};

const uint32_t Sha256::k_[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

void Sha256::Transform(const unsigned char* message, unsigned int block_nb) {
  uint32_t w[64];
  uint32_t wv[8];
  uint32_t t1, t2;
  const unsigned char* sub_block;
  int i;
  int j;
  for (i = 0; i < (int)block_nb; i++) {
    sub_block = message + (i << 6);
    for (j = 0; j < 16; j++) {
      SHA2_PACK32(&sub_block[j << 2], &w[j]);
    }
    for (j = 16; j < 64; j++) {
      w[j] = SHA256_F4(w[j - 2]) + w[j - 7] + SHA256_F3(w[j - 15]) + w[j - 16];
    }
    for (j = 0; j < 8; j++) {
      wv[j] = h_[j];
    }
    for (j = 0; j < 64; j++) {
      t1 = wv[7] + SHA256_F2(wv[4]) + SHA2_CH(wv[4], wv[5], wv[6]) + k_[j] +
           w[j];
      t2 = SHA256_F1(wv[0]) + SHA2_MAJ(wv[0], wv[1], wv[2]);
      wv[7] = wv[6];
      wv[6] = wv[5];
      wv[5] = wv[4];
      wv[4] = wv[3] + t1;
      wv[3] = wv[2];
      wv[2] = wv[1];
      wv[1] = wv[0];
      wv[0] = t1 + t2;
    }
    for (j = 0; j < 8; j++) {
      h_[j] += wv[j];
    }
  }
}

void Sha256::Init() {
  h_[0] = 0x6a09e667;
  h_[1] = 0xbb67ae85;
  h_[2] = 0x3c6ef372;
  h_[3] = 0xa54ff53a;
  h_[4] = 0x510e527f;
  h_[5] = 0x9b05688c;
  h_[6] = 0x1f83d9ab;
  h_[7] = 0x5be0cd19;
  len_ = 0;
  tot_len_ = 0;
}

void Sha256::Update(const unsigned char* message, unsigned int len) {
  unsigned int block_nb;
  unsigned int new_len, rem_len, tmp_len;
  const unsigned char* shifted_message;
  tmp_len = kBlockSize - len_;
  rem_len = len < tmp_len ? len : tmp_len;
  memcpy(&block_[len_], message, rem_len);
  if (len_ + len < kBlockSize) {
    len_ += len;
    return;
  }
  new_len = len - rem_len;
  block_nb = new_len / kBlockSize;
  shifted_message = message + rem_len;
  Transform(block_, 1);
  Transform(shifted_message, block_nb);
  rem_len = new_len % kBlockSize;
  memcpy(block_, &shifted_message[block_nb << 6], rem_len);
  len_ = rem_len;
  tot_len_ += (block_nb + 1) << 6;
}

void Sha256::Final(unsigned char* digest) {
  unsigned int block_nb;
  unsigned int pm_len;
  unsigned int len_b;
  int i;
  block_nb = (1 + ((kBlockSize - 9) < (len_ % kBlockSize)));
  len_b = (tot_len_ + len_) << 3;
  pm_len = block_nb << 6;
  memset(block_ + len_, 0, pm_len - len_);
  block_[len_] = 0x80;
  SHA2_UNPACK32(len_b, block_ + pm_len - 4);
  Transform(block_, block_nb);
  for (i = 0; i < 8; i++) {
    SHA2_UNPACK32(h_[i], &digest[i << 2]);
  }
}

constexpr int kMaxIterations = kMaxCount * 4 / 32 + 1;

// Byte for byte the procedure of CloudSeed's ShaRandom::Generate: the eight
// bytes of the seed are hashed, then the first eight bytes of each digest are
// hashed again for the next 32 bytes, and the concatenated digests are read
// as 32-bit unsigned integers scaled to 0..1.
void GenerateSeries(long long seed, int count, double* out) {
  if (count > kMaxCount) count = kMaxCount;
  const int iterations = count * 4 / 32 + 1;
  unsigned char bytes[Sha256::kDigestSize];
  unsigned char list[kMaxIterations * Sha256::kDigestSize];

  memcpy(bytes, &seed, 8);
  for (int i = 0; i < iterations; i++) {
    Sha256 ctx;
    ctx.Init();
    ctx.Update(bytes, 8);
    ctx.Final(bytes);
    memcpy(list + i * Sha256::kDigestSize, bytes, Sha256::kDigestSize);
  }

  for (int i = 0; i < count; i++) {
    uint32_t value;
    memcpy(&value, list + 4 * i, 4);
    out[i] = value / (double)UINT_MAX;
  }
}

}  // namespace

void Generate(long long seed, int count, double* out) {
  GenerateSeries(seed, count, out);
}

void Generate(long long seed, int count, double cross_seed, double* out) {
  if (count > kMaxCount) count = kMaxCount;
  double series_a[kMaxCount];
  double series_b[kMaxCount];
  GenerateSeries(seed, count, series_a);
  GenerateSeries(~seed, count, series_b);
  for (int i = 0; i < count; i++) {
    out[i] = series_a[i] * (1.0 - cross_seed) + series_b[i] * cross_seed;
  }
}

}  // namespace sha_random
}  // namespace cloudseed
