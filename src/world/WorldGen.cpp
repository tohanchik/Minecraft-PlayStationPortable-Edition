// WorldGen.cpp

#include "WorldGen.h"
#include "BiomeSource.h"
#include "Blocks.h"
#include "NoiseGen.h"
#include "Random.h"
#include "TreeFeature.h"
#include "chunk_defs.h"
#include <string.h>
#include <math.h>

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

static float lerp(float a, float b, float t) {
  return a + (b - a) * t;
}

static float sampleDensity(int wx, int wy, int wz, int64_t worldSeed) {
  float biomeBase = NoiseGen::octaveNoise(wx / 256.0f, wz / 256.0f, worldSeed ^ 0x5A21C3D7LL, 4, 0.5f) * 2.0f - 1.0f;
  float biomeHills = NoiseGen::octaveNoise(wx / 96.0f, wz / 96.0f, worldSeed ^ 0x13F2A64BLL, 4, 0.5f) * 2.0f - 1.0f;
  float detail = NoiseGen::octaveNoise(wx / 42.0f, wz / 42.0f, worldSeed ^ 0x71A9B17DLL, 4, 0.5f) * 2.0f - 1.0f;
  float ridged = fabsf(NoiseGen::octaveNoise(wx / 144.0f, wz / 144.0f, worldSeed ^ 0x3C9E4A11LL, 3, 0.5f) * 2.0f - 1.0f);

  float targetHeight = 64.0f + biomeBase * 18.0f + biomeHills * 12.0f + detail * 8.0f - ridged * 6.0f;
  float falloff = (wy - targetHeight) / 14.0f;
  float caves = NoiseGen::octaveNoise((wx + wy * 0.8f) / 36.0f, (wz - wy * 0.8f) / 36.0f, worldSeed ^ 0x11D4E57BLL, 3, 0.5f) * 2.0f - 1.0f;
  return -falloff + caves * 0.45f;
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

// Get terrain height
int WorldGen::getTerrainHeight(int wx, int wz, int64_t seed) {
  for (int y = CHUNK_SIZE_Y - 1; y >= 1; --y) {
    if (sampleDensity(wx, y, wz, seed) > 0.0f) return y;
  }
  return 1;
}

// Generate chunk
void WorldGen::generateChunk(
    uint8_t out[CHUNK_SIZE_X][CHUNK_SIZE_Z][CHUNK_SIZE_Y], int cx, int cz,
    int64_t worldSeed) {

  memset(out, BLOCK_AIR, CHUNK_SIZE_X * CHUNK_SIZE_Z * CHUNK_SIZE_Y);

  Random rng(worldSeed ^ ((int64_t)cx * 341873128712LL) ^
             ((int64_t)cz * 132897987541LL));

  // === Base Terrain (3D density + trilinear interpolation, MCPE-like stage) ===
  const int seaLevel = 62;
  const int cellX = 4;
  const int cellY = 8;
  const int cellZ = 4;
  const int gridX = CHUNK_SIZE_X / cellX;
  const int gridY = CHUNK_SIZE_Y / cellY;
  const int gridZ = CHUNK_SIZE_Z / cellZ;

  float densityGrid[gridX + 1][gridZ + 1][gridY + 1];
  for (int gx = 0; gx <= gridX; ++gx) {
    for (int gz = 0; gz <= gridZ; ++gz) {
      for (int gy = 0; gy <= gridY; ++gy) {
        int wx = cx * CHUNK_SIZE_X + gx * cellX;
        int wz = cz * CHUNK_SIZE_Z + gz * cellZ;
        int wy = gy * cellY;
        densityGrid[gx][gz][gy] = sampleDensity(wx, wy, wz, worldSeed);
      }
    }
  }

  for (int gx = 0; gx < gridX; ++gx) {
    for (int gz = 0; gz < gridZ; ++gz) {
      for (int gy = 0; gy < gridY; ++gy) {
        float d000 = densityGrid[gx][gz][gy];
        float d001 = densityGrid[gx][gz][gy + 1];
        float d010 = densityGrid[gx][gz + 1][gy];
        float d011 = densityGrid[gx][gz + 1][gy + 1];
        float d100 = densityGrid[gx + 1][gz][gy];
        float d101 = densityGrid[gx + 1][gz][gy + 1];
        float d110 = densityGrid[gx + 1][gz + 1][gy];
        float d111 = densityGrid[gx + 1][gz + 1][gy + 1];

        for (int lxIn = 0; lxIn < cellX; ++lxIn) {
          float tx = (float)lxIn / (float)cellX;
          for (int lzIn = 0; lzIn < cellZ; ++lzIn) {
            float tz = (float)lzIn / (float)cellZ;
            for (int lyIn = 0; lyIn < cellY; ++lyIn) {
              float ty = (float)lyIn / (float)cellY;

              float d00 = lerp(d000, d001, ty);
              float d01 = lerp(d010, d011, ty);
              float d10 = lerp(d100, d101, ty);
              float d11 = lerp(d110, d111, ty);
              float d0 = lerp(d00, d01, tz);
              float d1 = lerp(d10, d11, tz);
              float d = lerp(d0, d1, tx);

              int lx = gx * cellX + lxIn;
              int lz = gz * cellZ + lzIn;
              int y = gy * cellY + lyIn;
              if (y <= 0) {
                out[lx][lz][y] = BLOCK_BEDROCK;
              } else if (d > 0.0f) {
                out[lx][lz][y] = BLOCK_STONE;
              } else if (y <= seaLevel) {
                int wx = cx * CHUNK_SIZE_X + lx;
                int wz = cz * CHUNK_SIZE_Z + lz;
                BiomeData biome = BiomeSource::sampleBiome(wx, wz, worldSeed);
                out[lx][lz][y] = (biome.temperature < 0.15f && y == seaLevel)
                                     ? BLOCK_ICE
                                     : BLOCK_WATER_STILL;
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
  for (int lx = 0; lx < CHUNK_SIZE_X; ++lx) {
    for (int lz = 0; lz < CHUNK_SIZE_Z; ++lz) {
      int wx = cx * CHUNK_SIZE_X + lx;
      int wz = cz * CHUNK_SIZE_Z + lz;
      BiomeData biome = BiomeSource::sampleBiome(wx, wz, worldSeed);

      float sandN = NoiseGen::octaveNoise(wx / 32.0f, wz / 32.0f, worldSeed ^ 0x5A17D3LL, 4, 0.5f) * 2.0f - 1.0f;
      float gravelN = NoiseGen::octaveNoise(wx / 32.0f, wz / 32.0f, worldSeed ^ 0x193A49LL, 4, 0.5f) * 2.0f - 1.0f;
      float depthN = NoiseGen::octaveNoise(wx / 16.0f, wz / 16.0f, worldSeed ^ 0x77B41CLL, 4, 0.5f) * 2.0f - 1.0f;

      bool useSand = (sandN + rng.nextFloat() * 0.2f) > 0.0f;
      bool useGravel = (gravelN + rng.nextFloat() * 0.2f) > 0.15f;
      int runDepth = (int)(depthN / 3.0f + 3.0f + rng.nextFloat() * 0.25f);

      int run = -1;
      uint8_t top = biome.topBlock;
      uint8_t filler = biome.fillerBlock;

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
            top = biome.topBlock;
            filler = biome.fillerBlock;
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
            top = (biome.temperature < 0.15f) ? BLOCK_ICE : BLOCK_WATER_STILL;
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
