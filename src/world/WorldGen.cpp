// WorldGen.cpp

#include "WorldGen.h"
#include "Blocks.h"
#include "NoiseGen.h"
#include "Random.h"
#include "TreeFeature.h"
#include "chunk_defs.h"
#include <string.h>
#include <math.h>

enum GenBiomeType {
  BIOME_TUNDRA = 0,
  BIOME_TAIGA = 1,
  BIOME_PLAINS = 2,
  BIOME_FOREST = 3,
  BIOME_DESERT = 4
};

struct GenBiomeSurface {
  uint8_t top;
  uint8_t filler;
};

static GenBiomeType pickBiome(float temperature, float downfall) {
  if (temperature < 0.22f) return BIOME_TUNDRA;
  if (temperature < 0.40f) return BIOME_TAIGA;
  if (temperature > 0.78f && downfall < 0.22f) return BIOME_DESERT;
  if (downfall > 0.55f) return BIOME_FOREST;
  return BIOME_PLAINS;
}

static GenBiomeSurface getBiomeSurface(GenBiomeType biome) {
  GenBiomeSurface out = {BLOCK_GRASS, BLOCK_DIRT};
  if (biome == BIOME_DESERT) {
    out.top = BLOCK_SAND;
    out.filler = BLOCK_SAND;
  } else if (biome == BIOME_TUNDRA) {
    out.top = BLOCK_SNOW;
    out.filler = BLOCK_DIRT;
  }
  return out;
}

static void getClimateAt(int wx, int wz, int64_t seed, float &temperature,
                         float &downfall, GenBiomeType &biome) {
  const float zoom = 2.0f;
  const float tempScale = zoom / 80.0f;
  const float downfallScale = zoom / 40.0f;
  const float noiseScale = 1.0f / 4.0f;

  float t = NoiseGen::octaveNoise(wx * tempScale, wz * tempScale,
                                  seed * 9871LL + 0x111F23LL, 4, 0.5f);
  float d = NoiseGen::octaveNoise(wx * downfallScale, wz * downfallScale,
                                  seed * 39811LL + 0x29A3C1LL, 4, 0.5f);
  float n = NoiseGen::octaveNoise(wx * noiseScale, wz * noiseScale,
                                  seed * 543321LL + 0x58D2B5LL, 2, 0.5f);

  float noise = (n * 1.1f + 0.5f);
  float tMix = (t * 0.15f + 0.7f) * 0.99f + noise * 0.01f;
  float dMix = (d * 0.15f + 0.5f) * 0.998f + noise * 0.002f;

  tMix = 1.0f - ((1.0f - tMix) * (1.0f - tMix));
  if (tMix < 0.0f) tMix = 0.0f;
  if (dMix < 0.0f) dMix = 0.0f;
  if (tMix > 1.0f) tMix = 1.0f;
  if (dMix > 1.0f) dMix = 1.0f;

  temperature = tMix;
  downfall = dMix;
  biome = pickBiome(temperature, downfall);
}

static float getDensityAt(int wx, int y, int wz, int64_t seed) {
  float n1 = NoiseGen::octaveNoise3d(wx / 96.0f, y / 48.0f, wz / 96.0f,
                                     seed ^ 0x6A09E667LL, 8, 0.5f) *
                 2.0f -
             1.0f;
  float n2 = NoiseGen::octaveNoise3d(wx / 48.0f, y / 32.0f, wz / 48.0f,
                                     seed ^ 0xBB67AE85LL, 6, 0.5f) *
                 2.0f -
             1.0f;
  float n3 = NoiseGen::octaveNoise3d(wx / 24.0f, y / 24.0f, wz / 24.0f,
                                     seed ^ 0x3C6EF372LL, 4, 0.5f) *
                 2.0f -
             1.0f;
  float blend = (NoiseGen::octaveNoise3d(wx / 128.0f, y / 64.0f, wz / 128.0f,
                                         seed ^ 0xA54FF53ALL, 4, 0.5f) *
                     2.0f -
                 1.0f) *
                    0.5f +
                0.5f;
  if (blend < 0.0f) blend = 0.0f;
  if (blend > 1.0f) blend = 1.0f;
  float terrain = (n1 * (1.0f - blend) + n2 * blend) + n3 * 0.30f;
  float vertical = ((float)y - 64.0f) / 28.0f;
  return terrain - vertical;
}

static inline bool inChunk(int lx, int ly, int lz) {
  return lx >= 0 && lx < CHUNK_SIZE_X && lz >= 0 && lz < CHUNK_SIZE_Z &&
         ly > 0 && ly < CHUNK_SIZE_Y;
}

static inline bool isAirOrWater(uint8_t id) {
  return id == BLOCK_AIR || id == BLOCK_WATER_STILL || id == BLOCK_WATER_FLOW;
}

static int topSolidY(
    uint8_t out[CHUNK_SIZE_X][CHUNK_SIZE_Z][CHUNK_SIZE_Y],
    int lx, int lz) {
  for (int y = CHUNK_SIZE_Y - 1; y >= 0; --y) {
    if (!isAirOrWater(out[lx][lz][y])) return y;
  }
  return 0;
}

static uint8_t shoreFillFromTop(uint8_t topId) {
  if (topId == BLOCK_SAND || topId == BLOCK_GRAVEL || topId == BLOCK_STONE) return topId;
  if (topId == BLOCK_GRASS) return BLOCK_DIRT;
  if (topId == BLOCK_DIRT) return BLOCK_DIRT;
  return BLOCK_DIRT;
}

static inline bool isTopSurfaceBlock(uint8_t id) {
  return id == BLOCK_GRASS || id == BLOCK_SAND || id == BLOCK_GRAVEL;
}

static uint8_t majorityTopBlock(
    uint8_t out[CHUNK_SIZE_X][CHUNK_SIZE_Z][CHUNK_SIZE_Y],
    int lx, int lz, int y) {
  int grass = 0;
  int sand = 0;
  int gravel = 0;

  const int nx[4] = {-1, 1, 0, 0};
  const int nz[4] = {0, 0, -1, 1};
  for (int i = 0; i < 4; ++i) {
    int xx = lx + nx[i];
    int zz = lz + nz[i];
    if (xx < 0 || xx >= CHUNK_SIZE_X || zz < 0 || zz >= CHUNK_SIZE_Z) continue;
    uint8_t n = out[xx][zz][y];
    if (n == BLOCK_GRASS) ++grass;
    else if (n == BLOCK_SAND) ++sand;
    else if (n == BLOCK_GRAVEL) ++gravel;
  }

  if (grass >= 3) return BLOCK_GRASS;
  if (sand >= 3) return BLOCK_SAND;
  if (gravel >= 3) return BLOCK_GRAVEL;
  return 0;
}

static void placeOreVein(
    uint8_t out[CHUNK_SIZE_X][CHUNK_SIZE_Z][CHUNK_SIZE_Y],
    int cx, int cz, Random &rng, int wx, int wy, int wz, uint8_t oreId, int veinSize) {
  float angle = rng.nextFloat() * 3.14159265f;
  float dx = sinf(angle) * (veinSize / 8.0f);
  float dz = cosf(angle) * (veinSize / 8.0f);

  float x0 = wx + 8 + dx;
  float x1 = wx + 8 - dx;
  float z0 = wz + 8 + dz;
  float z1 = wz + 8 - dz;
  float y0 = wy + rng.nextInt(3) - 2;
  float y1 = wy + rng.nextInt(3) - 2;

  const int xo = cx * CHUNK_SIZE_X;
  const int zo = cz * CHUNK_SIZE_Z;

  for (int i = 0; i < veinSize; ++i) {
    float t = (float)i / (float)veinSize;
    float px = x0 + (x1 - x0) * t;
    float py = y0 + (y1 - y0) * t;
    float pz = z0 + (z1 - z0) * t;

    float radius = (sinf(t * 3.14159265f) + 1.0f) * rng.nextFloat() * veinSize / 32.0f + 1.0f;
    int minX = (int)floorf(px - radius);
    int maxX = (int)floorf(px + radius);
    int minY = (int)floorf(py - radius);
    int maxY = (int)floorf(py + radius);
    int minZ = (int)floorf(pz - radius);
    int maxZ = (int)floorf(pz + radius);

    for (int x = minX; x <= maxX; ++x) {
      float nx = ((float)x + 0.5f - px) / radius;
      if (nx * nx >= 1.0f) continue;
      for (int y = minY; y <= maxY; ++y) {
        float ny = ((float)y + 0.5f - py) / radius;
        if (nx * nx + ny * ny >= 1.0f) continue;
        for (int z = minZ; z <= maxZ; ++z) {
          float nz = ((float)z + 0.5f - pz) / radius;
          if (nx * nx + ny * ny + nz * nz >= 1.0f) continue;

          int lx = x - xo;
          int lz = z - zo;
          if (!inChunk(lx, y, lz)) continue;
          if (out[lx][lz][y] == BLOCK_STONE) out[lx][lz][y] = oreId;
        }
      }
    }
  }
}

static uint8_t pickShoreMaterial(
    uint8_t out[CHUNK_SIZE_X][CHUNK_SIZE_Z][CHUNK_SIZE_Y],
    int lx, int lz, int y) {
  const uint8_t preferred[] = {
      BLOCK_SAND, BLOCK_GRAVEL, BLOCK_DIRT, BLOCK_GRASS, BLOCK_STONE};

  for (int i = 0; i < 5; ++i) {
    uint8_t want = preferred[i];
    if (lx > 0 && out[lx - 1][lz][y] == want) return want;
    if (lx + 1 < CHUNK_SIZE_X && out[lx + 1][lz][y] == want) return want;
    if (lz > 0 && out[lx][lz - 1][y] == want) return want;
    if (lz + 1 < CHUNK_SIZE_Z && out[lx][lz + 1][y] == want) return want;
  }

  uint8_t below = out[lx][lz][y - 1];
  if (below != BLOCK_AIR && below != BLOCK_WATER_STILL) return below;
  return BLOCK_STONE;
}

static int countSolidHorizontalNeighbors(
    uint8_t out[CHUNK_SIZE_X][CHUNK_SIZE_Z][CHUNK_SIZE_Y],
    int lx, int lz, int y) {
  int solid = 0;
  if (lx > 0) {
    uint8_t b = out[lx - 1][lz][y];
    if (b != BLOCK_AIR && b != BLOCK_WATER_STILL) solid++;
  }
  if (lx + 1 < CHUNK_SIZE_X) {
    uint8_t b = out[lx + 1][lz][y];
    if (b != BLOCK_AIR && b != BLOCK_WATER_STILL) solid++;
  }
  if (lz > 0) {
    uint8_t b = out[lx][lz - 1][y];
    if (b != BLOCK_AIR && b != BLOCK_WATER_STILL) solid++;
  }
  if (lz + 1 < CHUNK_SIZE_Z) {
    uint8_t b = out[lx][lz + 1][y];
    if (b != BLOCK_AIR && b != BLOCK_WATER_STILL) solid++;
  }
  return solid;
}

static void stabilizeSeaWaterAndShore(
    uint8_t out[CHUNK_SIZE_X][CHUNK_SIZE_Z][CHUNK_SIZE_Y],
    int seaLevel) {
  // MC classic/PE-style fixup: generation can leave shoreline "hanging water"
  // pockets until liquid ticks run. Resolve at gen-time:
  // 1) flood underwater air close to water.
  // 2) optionally seal only *tiny* pits with matching shore material
  //    (to avoid creating large artificial terraces).
  //
  // This keeps lakes/oceans visually stable right after chunk creation.
  if (seaLevel < 1) return;
  int maxY = seaLevel;
  if (maxY > CHUNK_SIZE_Y - 1) maxY = CHUNK_SIZE_Y - 1;

  for (int pass = 0; pass < 8; ++pass) {
    bool changed = false;
    for (int lx = 0; lx < CHUNK_SIZE_X; ++lx) {
      for (int lz = 0; lz < CHUNK_SIZE_Z; ++lz) {
        for (int y = maxY; y >= 1; --y) {
          if (out[lx][lz][y] != BLOCK_AIR) continue;

          bool nearWater = false;
          if (y < maxY && out[lx][lz][y + 1] == BLOCK_WATER_STILL) nearWater = true;
          if (!nearWater && out[lx][lz][y - 1] == BLOCK_WATER_STILL) nearWater = true;
          if (!nearWater && lx > 0 && out[lx - 1][lz][y] == BLOCK_WATER_STILL) nearWater = true;
          if (!nearWater && lx + 1 < CHUNK_SIZE_X && out[lx + 1][lz][y] == BLOCK_WATER_STILL) nearWater = true;
          if (!nearWater && lz > 0 && out[lx][lz - 1][y] == BLOCK_WATER_STILL) nearWater = true;
          if (!nearWater && lz + 1 < CHUNK_SIZE_Z && out[lx][lz + 1][y] == BLOCK_WATER_STILL) nearWater = true;

          if (nearWater) {
            bool solidBelow = out[lx][lz][y - 1] != BLOCK_AIR &&
                              out[lx][lz][y - 1] != BLOCK_WATER_STILL;
            int solidSides = countSolidHorizontalNeighbors(out, lx, lz, y);

            // Only seal very small enclosed shoreline potholes.
            // Open/elongated spaces should become water.
            if (solidBelow && solidSides >= 3) {
              out[lx][lz][y] = pickShoreMaterial(out, lx, lz, y);
            } else {
              out[lx][lz][y] = BLOCK_WATER_STILL;
            }
            changed = true;
          }
        }
      }
    }
    if (!changed) break;
  }
}

// Get terrain height
int WorldGen::getTerrainHeight(int wx, int wz, int64_t seed) {
  int h = 1;
  for (int y = CHUNK_SIZE_Y - 2; y >= 1; --y) {
    if (getDensityAt(wx, y, wz, seed) > 0.0f) {
      h = y;
      break;
    }
  }
  return h;
}

// Generate chunk
void WorldGen::generateChunk(
    uint8_t out[CHUNK_SIZE_X][CHUNK_SIZE_Z][CHUNK_SIZE_Y], int cx, int cz,
    int64_t worldSeed) {

  memset(out, BLOCK_AIR, CHUNK_SIZE_X * CHUNK_SIZE_Z * CHUNK_SIZE_Y);

  Random rng(worldSeed ^ ((int64_t)cx * 341873128712LL) ^
             ((int64_t)cz * 132897987541LL));

  float temperatureMap[CHUNK_SIZE_X][CHUNK_SIZE_Z];
  GenBiomeType biomeMap[CHUNK_SIZE_X][CHUNK_SIZE_Z];

  for (int lx = 0; lx < CHUNK_SIZE_X; ++lx) {
    for (int lz = 0; lz < CHUNK_SIZE_Z; ++lz) {
      int wx = cx * CHUNK_SIZE_X + lx;
      int wz = cz * CHUNK_SIZE_Z + lz;
      float temperature = 0.5f;
      float downfall = 0.5f;
      GenBiomeType biome = BIOME_PLAINS;
      getClimateAt(wx, wz, worldSeed, temperature, downfall, biome);
      temperatureMap[lx][lz] = temperature;
      biomeMap[lx][lz] = biome;
    }
  }

  // === Base Terrain ===
  // Generate density on a coarse 4x4x8 grid and trilinearly interpolate,
  // similar in spirit to MCPE prepareHeights/getHeights, but with our
  // existing noise backend.
  const int cellX = 4;
  const int cellY = 8;
  const int cellZ = 4;
  const int sx = CHUNK_SIZE_X / cellX + 1;
  const int sy = CHUNK_SIZE_Y / cellY + 1;
  const int sz = CHUNK_SIZE_Z / cellZ + 1;
  float density[sx][sz][sy];

  for (int gx = 0; gx < sx; ++gx) {
    for (int gz = 0; gz < sz; ++gz) {
      for (int gy = 0; gy < sy; ++gy) {
        int wx = cx * CHUNK_SIZE_X + gx * cellX;
        int wz = cz * CHUNK_SIZE_Z + gz * cellZ;
        int y = gy * cellY;
        density[gx][gz][gy] = getDensityAt(wx, y, wz, worldSeed);
      }
    }
  }

  for (int gx = 0; gx < sx - 1; ++gx) {
    for (int gz = 0; gz < sz - 1; ++gz) {
      for (int gy = 0; gy < sy - 1; ++gy) {
        float d000 = density[gx][gz][gy];
        float d100 = density[gx + 1][gz][gy];
        float d010 = density[gx][gz + 1][gy];
        float d110 = density[gx + 1][gz + 1][gy];
        float d001 = density[gx][gz][gy + 1];
        float d101 = density[gx + 1][gz][gy + 1];
        float d011 = density[gx][gz + 1][gy + 1];
        float d111 = density[gx + 1][gz + 1][gy + 1];

        for (int lx0 = 0; lx0 < cellX; ++lx0) {
          float fx = (float)lx0 / (float)cellX;
          for (int lz0 = 0; lz0 < cellZ; ++lz0) {
            float fz = (float)lz0 / (float)cellZ;
            int lx = gx * cellX + lx0;
            int lz = gz * cellZ + lz0;
            float temperature = temperatureMap[lx][lz];

            for (int ly0 = 0; ly0 < cellY; ++ly0) {
              float fy = (float)ly0 / (float)cellY;
              int y = gy * cellY + ly0;
              if (y >= CHUNK_SIZE_Y) continue;

              float d00 = d000 + (d100 - d000) * fx;
              float d10 = d010 + (d110 - d010) * fx;
              float d01 = d001 + (d101 - d001) * fx;
              float d11 = d011 + (d111 - d011) * fx;
              float d0 = d00 + (d10 - d00) * fz;
              float d1 = d01 + (d11 - d01) * fz;
              float d = d0 + (d1 - d0) * fy;

              if (y == 0) {
                out[lx][lz][y] = BLOCK_BEDROCK;
              } else if (d > 0.0f) {
                out[lx][lz][y] = BLOCK_STONE;
              } else if (y <= 62) {
                if (y == 62 && temperature < 0.5f) out[lx][lz][y] = BLOCK_ICE;
                else out[lx][lz][y] = BLOCK_WATER_STILL;
              }
            }
          }
        }
      }
    }
  }

  // === Surface shaping pass (MCPE RandomLevelSource::buildSurfaces-like) ===
  // Converts top stone layers into biome-like top/filler (grass/dirt/sand/gravel)
  // and adds a small sandstone run beneath sand.
  const int seaLevel = 62;
  for (int lx = 0; lx < CHUNK_SIZE_X; ++lx) {
    for (int lz = 0; lz < CHUNK_SIZE_Z; ++lz) {
      int wx = cx * CHUNK_SIZE_X + lx;
      int wz = cz * CHUNK_SIZE_Z + lz;
      float temperature = temperatureMap[lx][lz];
      GenBiomeType biome = biomeMap[lx][lz];
      GenBiomeSurface surface = getBiomeSurface(biome);

      float sandN = NoiseGen::octaveNoise(wx / 32.0f, wz / 32.0f, worldSeed ^ 0x5A17D3LL, 4, 0.5f) * 2.0f - 1.0f;
      float gravelN = NoiseGen::octaveNoise(wx / 32.0f, wz / 32.0f, worldSeed ^ 0x193A49LL, 4, 0.5f) * 2.0f - 1.0f;
      float depthN = NoiseGen::octaveNoise(wx / 16.0f, wz / 16.0f, worldSeed ^ 0x77B41CLL, 4, 0.5f) * 2.0f - 1.0f;

      bool useSand = (sandN + rng.nextFloat() * 0.2f) > 0.0f;
      bool useGravel = (gravelN + rng.nextFloat() * 0.2f) > 0.15f;
      int runDepth = (int)(depthN / 3.0f + 3.0f + rng.nextFloat() * 0.25f);

      int run = -1;
      uint8_t top = surface.top;
      uint8_t filler = surface.filler;

      for (int y = CHUNK_SIZE_Y - 1; y >= 0; --y) {
        if (y <= rng.nextInt(5)) {
          out[lx][lz][y] = BLOCK_BEDROCK;
          continue;
        }

        uint8_t old = out[lx][lz][y];
        if (old == BLOCK_AIR) {
          run = -1;
          continue;
        }
        if (old != BLOCK_STONE) continue;

        if (run == -1) {
          if (runDepth <= 0) {
            top = BLOCK_AIR;
            filler = BLOCK_STONE;
          } else if (y >= seaLevel - 4 && y <= seaLevel + 1) {
            top = surface.top;
            filler = surface.filler;
            if (useGravel) {
              top = BLOCK_AIR;
              filler = BLOCK_GRAVEL;
            }
            if (useSand) {
              top = BLOCK_SAND;
              filler = BLOCK_SAND;
            }
          }

          if (y < seaLevel && top == BLOCK_AIR) {
            if (temperature < 0.15f) top = BLOCK_ICE;
            else top = BLOCK_WATER_STILL;
          }

          run = runDepth;
          out[lx][lz][y] = (y >= seaLevel - 1) ? top : filler;
        } else if (run > 0) {
          run--;
          out[lx][lz][y] = filler;

          if (run == 0 && filler == BLOCK_SAND) {
            run = rng.nextInt(4);
            filler = BLOCK_SANDSTONE;
          }
        }
      }
    }
  }

  // Remove tiny 1-block "salt-and-pepper" surface noise (grass/sand/gravel).
  // This keeps beaches cleaner and avoids random mixed patches inland.
  uint8_t topFix[CHUNK_SIZE_X][CHUNK_SIZE_Z];
  memset(topFix, 0, sizeof(topFix));
  for (int lx = 1; lx < CHUNK_SIZE_X - 1; ++lx) {
    for (int lz = 1; lz < CHUNK_SIZE_Z - 1; ++lz) {
      int y = topSolidY(out, lx, lz);
      if (y <= 0 || y >= CHUNK_SIZE_Y - 1) continue;
      uint8_t cur = out[lx][lz][y];
      if (!isTopSurfaceBlock(cur)) continue;
      uint8_t maj = majorityTopBlock(out, lx, lz, y);
      if (maj != 0 && maj != cur) topFix[lx][lz] = maj;
    }
  }
  for (int lx = 1; lx < CHUNK_SIZE_X - 1; ++lx) {
    for (int lz = 1; lz < CHUNK_SIZE_Z - 1; ++lz) {
      uint8_t fix = topFix[lx][lz];
      if (fix == 0) continue;
      int y = topSolidY(out, lx, lz);
      out[lx][lz][y] = fix;
      if (y > 0 && out[lx][lz][y - 1] != BLOCK_BEDROCK) {
        out[lx][lz][y - 1] = (fix == BLOCK_GRASS) ? BLOCK_DIRT : fix;
      }
    }
  }

  // === Shoreline smoothing near sea level ===
  // Build a small "natural rim" around water and avoid immediate drops right
  // behind the coast (which looks like a dam/wall).
  int distToWater[CHUNK_SIZE_X][CHUNK_SIZE_Z];
  for (int lx = 0; lx < CHUNK_SIZE_X; ++lx)
    for (int lz = 0; lz < CHUNK_SIZE_Z; ++lz)
      distToWater[lx][lz] = 99;

  for (int lx = 0; lx < CHUNK_SIZE_X; ++lx) {
    for (int lz = 0; lz < CHUNK_SIZE_Z; ++lz) {
      if (out[lx][lz][seaLevel] == BLOCK_WATER_STILL) distToWater[lx][lz] = 0;
    }
  }
  for (int pass = 0; pass < 2; ++pass) {
    for (int lx = 0; lx < CHUNK_SIZE_X; ++lx) {
      for (int lz = 0; lz < CHUNK_SIZE_Z; ++lz) {
        int d = distToWater[lx][lz];
        if (d >= 2) continue;
        if (lx > 0 && distToWater[lx - 1][lz] > d + 1) distToWater[lx - 1][lz] = d + 1;
        if (lx + 1 < CHUNK_SIZE_X && distToWater[lx + 1][lz] > d + 1) distToWater[lx + 1][lz] = d + 1;
        if (lz > 0 && distToWater[lx][lz - 1] > d + 1) distToWater[lx][lz - 1] = d + 1;
        if (lz + 1 < CHUNK_SIZE_Z && distToWater[lx][lz + 1] > d + 1) distToWater[lx][lz + 1] = d + 1;
      }
    }
  }

  for (int lx = 0; lx < CHUNK_SIZE_X; ++lx) {
    for (int lz = 0; lz < CHUNK_SIZE_Z; ++lz) {
      int d = distToWater[lx][lz];
      if (d <= 0 || d > 2) continue;
      if (out[lx][lz][seaLevel] == BLOCK_WATER_STILL) continue;

      int topY = topSolidY(out, lx, lz);
      if (topY <= 0) continue;
      uint8_t fillId = shoreFillFromTop(out[lx][lz][topY]);

      int target = (d == 1) ? seaLevel : (seaLevel - 1);
      if (topY >= target) continue;

      for (int y = topY + 1; y <= target && y < CHUNK_SIZE_Y; ++y) {
        if (isAirOrWater(out[lx][lz][y])) out[lx][lz][y] = fillId;
      }
    }
  }

  // === Vegetation ===
  int xo = cx * CHUNK_SIZE_X;
  int zo = cz * CHUNK_SIZE_Z;

  // 1-2 grass clusters per chunk
  int grassClusters = 1 + rng.nextInt(2);
  for (int i = 0; i < grassClusters; i++) {
    int x = xo + rng.nextInt(16);
    int z = zo + rng.nextInt(16);
    int y = rng.nextInt(CHUNK_SIZE_Y);
    
    // TallGrassFeature spreads 128 times around the center
    for (int j = 0; j < 128; j++) {
      int x2 = x + rng.nextInt(8) - rng.nextInt(8);
      int y2 = y + rng.nextInt(4) - rng.nextInt(4);
      int z2 = z + rng.nextInt(8) - rng.nextInt(8);
      
      int lx = x2 - xo;
      int ly = y2;
      int lz = z2 - zo;
      
      if (lx >= 0 && lx < CHUNK_SIZE_X && lz >= 0 && lz < CHUNK_SIZE_Z && ly > 0 && ly < CHUNK_SIZE_Y) {
        if (out[lx][lz][ly] == BLOCK_AIR && out[lx][lz][ly - 1] == BLOCK_GRASS) {
          out[lx][lz][ly] = BLOCK_TALLGRASS;
        }
      }
    }
  }

  // Flowers (2 clusters)
  for (int i = 0; i < 2; i++) {
    int x = xo + rng.nextInt(16);
    int z = zo + rng.nextInt(16);
    int y = rng.nextInt(CHUNK_SIZE_Y);
    
    // FlowerFeature spreads 64 times
    for (int j = 0; j < 64; j++) {
      int x2 = x + rng.nextInt(8) - rng.nextInt(8);
      int y2 = y + rng.nextInt(4) - rng.nextInt(4);
      int z2 = z + rng.nextInt(8) - rng.nextInt(8);
      
      int lx = x2 - xo;
      int ly = y2;
      int lz = z2 - zo;
      
      if (lx >= 0 && lx < CHUNK_SIZE_X && lz >= 0 && lz < CHUNK_SIZE_Z && ly > 0 && ly < CHUNK_SIZE_Y) {
        if (out[lx][lz][ly] == BLOCK_AIR && out[lx][lz][ly - 1] == BLOCK_GRASS) {
          out[lx][lz][ly] = BLOCK_FLOWER;
        }
      }
    }
    
    // Rose (25% chance of second flower patch being red)
    if (rng.nextInt(4) == 0) {
      x = xo + rng.nextInt(16);
      z = zo + rng.nextInt(16);
      y = rng.nextInt(CHUNK_SIZE_Y);
      for (int j = 0; j < 64; j++) {
        int x2 = x + rng.nextInt(8) - rng.nextInt(8);
        int y2 = y + rng.nextInt(4) - rng.nextInt(4);
        int z2 = z + rng.nextInt(8) - rng.nextInt(8);
        
        int lx = x2 - xo;
        int ly = y2;
        int lz = z2 - zo;
        
        if (lx >= 0 && lx < CHUNK_SIZE_X && lz >= 0 && lz < CHUNK_SIZE_Z && ly > 0 && ly < CHUNK_SIZE_Y) {
          if (out[lx][lz][ly] == BLOCK_AIR && out[lx][lz][ly - 1] == BLOCK_GRASS) {
            out[lx][lz][ly] = BLOCK_ROSE;
          }
        }
      }
    }
  }

  // === Underground features (MCPE 0.6.1-like ore distribution pass) ===
  // If some original tiles are missing in this port, we map to nearest analog.
  for (int i = 0; i < 20; ++i) {
    int wx = xo + rng.nextInt(16);
    int wy = rng.nextInt(128);
    int wz = zo + rng.nextInt(16);
    placeOreVein(out, cx, cz, rng, wx, wy, wz, BLOCK_DIRT, 32);
  }

  for (int i = 0; i < 10; ++i) {
    int wx = xo + rng.nextInt(16);
    int wy = rng.nextInt(128);
    int wz = zo + rng.nextInt(16);
    placeOreVein(out, cx, cz, rng, wx, wy, wz, BLOCK_GRAVEL, 32);
  }

  for (int i = 0; i < 16; ++i) {
    int wx = xo + rng.nextInt(16);
    int wy = rng.nextInt(128);
    int wz = zo + rng.nextInt(16);
    placeOreVein(out, cx, cz, rng, wx, wy, wz, BLOCK_COAL_ORE, 14);
  }

  for (int i = 0; i < 14; ++i) {
    int wx = xo + rng.nextInt(16);
    int wy = rng.nextInt(64);
    int wz = zo + rng.nextInt(16);
    placeOreVein(out, cx, cz, rng, wx, wy, wz, BLOCK_IRON_ORE, 10);
  }

  for (int i = 0; i < 2; ++i) {
    int wx = xo + rng.nextInt(16);
    int wy = rng.nextInt(32);
    int wz = zo + rng.nextInt(16);
    placeOreVein(out, cx, cz, rng, wx, wy, wz, BLOCK_GOLD_ORE, 9);
  }

  for (int i = 0; i < 6; ++i) {
    int wx = xo + rng.nextInt(16);
    int wy = rng.nextInt(16);
    int wz = zo + rng.nextInt(16);
    placeOreVein(out, cx, cz, rng, wx, wy, wz, BLOCK_REDSTONE_ORE, 8);
  }

  for (int i = 0; i < 3; ++i) {
    int wx = xo + rng.nextInt(16);
    int wy = rng.nextInt(16);
    int wz = zo + rng.nextInt(16);
    placeOreVein(out, cx, cz, rng, wx, wy, wz, BLOCK_EMERALD_ORE, 6);
  }

  for (int i = 0; i < 1; ++i) {
    int wx = xo + rng.nextInt(16);
    int wy = rng.nextInt(16) + rng.nextInt(16);
    int wz = zo + rng.nextInt(16);
    placeOreVein(out, cx, cz, rng, wx, wy, wz, BLOCK_LAPIS_ORE, 6);
  }

}
