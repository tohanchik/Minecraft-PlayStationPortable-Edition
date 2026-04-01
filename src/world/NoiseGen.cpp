// NoiseGen.cpp - 2D noise implementation with octaves
// Ported/adapted from Minecraft.World/ImprovedNoise.cpp (4J Studios)

#include "NoiseGen.h"
#include "PerlinNoise.h"
#include "Random.h"
#include <math.h>
#include <stddef.h>

static float hash2d(int ix, int iz, int64_t seed) {
  uint32_t h = (uint32_t)(seed ^ (ix * 374761393LL) ^ (iz * 668265263LL));
  h = (h ^ (h >> 13)) * 1274126177;
  h = h ^ (h >> 16);
  return (float)(h & 0xFFFF) / 65535.0f;
}

static float smoothNoise2d(float x, float z, int64_t seed) {
  int ix = (int)floorf(x);
  int iz = (int)floorf(z);
  float fx = x - ix;
  float fz = z - iz;

  // Smoothstep interpolation
  float ux = fx * fx * (3.0f - 2.0f * fx);
  float uz = fz * fz * (3.0f - 2.0f * fz);

  float a = hash2d(ix, iz, seed);
  float b = hash2d(ix + 1, iz, seed);
  float c = hash2d(ix, iz + 1, seed);
  float d = hash2d(ix + 1, iz + 1, seed);

  return a + (b - a) * ux + (c - a) * uz + (a - b - c + d) * ux * uz;
}

static float hash3d(int ix, int iy, int iz, int64_t seed) {
  uint32_t h = (uint32_t)(seed ^ (ix * 374761393LL) ^ (iy * 668265263LL) ^
                          (iz * 2147483647LL));
  h = (h ^ (h >> 13)) * 1274126177;
  h = h ^ (h >> 16);
  return (float)(h & 0xFFFF) / 65535.0f;
}

static float smoothNoise3d(float x, float y, float z, int64_t seed) {
  int ix = (int)floorf(x);
  int iy = (int)floorf(y);
  int iz = (int)floorf(z);
  float fx = x - ix;
  float fy = y - iy;
  float fz = z - iz;

  float ux = fx * fx * (3.0f - 2.0f * fx);
  float uy = fy * fy * (3.0f - 2.0f * fy);
  float uz = fz * fz * (3.0f - 2.0f * fz);

  float c000 = hash3d(ix, iy, iz, seed);
  float c100 = hash3d(ix + 1, iy, iz, seed);
  float c010 = hash3d(ix, iy + 1, iz, seed);
  float c110 = hash3d(ix + 1, iy + 1, iz, seed);
  float c001 = hash3d(ix, iy, iz + 1, seed);
  float c101 = hash3d(ix + 1, iy, iz + 1, seed);
  float c011 = hash3d(ix, iy + 1, iz + 1, seed);
  float c111 = hash3d(ix + 1, iy + 1, iz + 1, seed);

  float x00 = c000 + (c100 - c000) * ux;
  float x10 = c010 + (c110 - c010) * ux;
  float x01 = c001 + (c101 - c001) * ux;
  float x11 = c011 + (c111 - c011) * ux;
  float y0 = x00 + (x10 - x00) * uy;
  float y1 = x01 + (x11 - x01) * uy;
  return y0 + (y1 - y0) * uz;
}

float NoiseGen::noise2d(float x, float z, int64_t seed) {
  return octaveNoise(x, z, seed, 1, 0.5f);
}

float NoiseGen::noise3d(float x, float y, float z, int64_t seed) {
  return octaveNoise3d(x, y, z, seed, 1, 0.5f);
}

float NoiseGen::octaveNoise(float x, float z, int64_t seed, int octaves,
                            float persistence) {
  (void)persistence;
  struct CacheEntry {
    int64_t seed;
    int octaves;
    PerlinNoise *noise;
  };
  static CacheEntry cache[8];
  static bool init = false;
  if (!init) {
    for (int i = 0; i < 8; ++i) {
      cache[i].seed = 0;
      cache[i].octaves = 0;
      cache[i].noise = NULL;
    }
    init = true;
  }

  PerlinNoise *pn = NULL;
  for (int i = 0; i < 8; ++i) {
    if (cache[i].noise && cache[i].seed == seed && cache[i].octaves == octaves) {
      pn = cache[i].noise;
      break;
    }
  }
  if (!pn) {
    int slot = (int)((seed ^ (seed >> 32) ^ octaves) & 7);
    if (cache[slot].noise) delete cache[slot].noise;
    Random rnd(seed);
    cache[slot].seed = seed;
    cache[slot].octaves = octaves;
    cache[slot].noise = new PerlinNoise(&rnd, octaves);
    pn = cache[slot].noise;
  }

  float v = pn->getValue(x, z);
  if (v < -1.0f) v = -1.0f;
  if (v > 1.0f) v = 1.0f;
  return v * 0.5f + 0.5f;
}

float NoiseGen::octaveNoise3d(float x, float y, float z, int64_t seed,
                              int octaves, float persistence) {
  (void)persistence;
  struct CacheEntry3D {
    int64_t seed;
    int octaves;
    PerlinNoise *noise;
  };
  static CacheEntry3D cache3d[8];
  static bool init3d = false;
  if (!init3d) {
    for (int i = 0; i < 8; ++i) {
      cache3d[i].seed = 0;
      cache3d[i].octaves = 0;
      cache3d[i].noise = NULL;
    }
    init3d = true;
  }

  PerlinNoise *pn = NULL;
  for (int i = 0; i < 8; ++i) {
    if (cache3d[i].noise && cache3d[i].seed == seed &&
        cache3d[i].octaves == octaves) {
      pn = cache3d[i].noise;
      break;
    }
  }
  if (!pn) {
    int slot = (int)((seed ^ (seed >> 32) ^ (octaves << 4)) & 7);
    if (cache3d[slot].noise) delete cache3d[slot].noise;
    Random rnd(seed);
    cache3d[slot].seed = seed;
    cache3d[slot].octaves = octaves;
    cache3d[slot].noise = new PerlinNoise(&rnd, octaves);
    pn = cache3d[slot].noise;
  }

  float v = pn->getValue(x, y, z);
  if (v < -1.0f) v = -1.0f;
  if (v > 1.0f) v = 1.0f;
  return v * 0.5f + 0.5f;
}
