#pragma once

namespace cloudseed {
namespace sha_random {

constexpr int kMaxCount = 100;

// CloudSeed derives all its "random" delay lengths, tap positions and gains
// from SHA-256 hashes of a seed, so that a preset sounds the same everywhere.
// Fills out[0..count) with values in 0..1; count must be <= kMaxCount.
void Generate(long long seed, int count, double* out);

// Same, but mixing the series of the seed and of its bitwise inverse
// according to cross_seed (0: the seed's series, 1: the inverse's series).
void Generate(long long seed, int count, double cross_seed, double* out);

}  // namespace sha_random
}  // namespace cloudseed
