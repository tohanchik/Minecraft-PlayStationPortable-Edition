// WorldGen.cpp

#include "WorldGen.h"
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
  // MCPE-like layered terrain blend: broad continents + detail hills.
  float base = NoiseGen::octaveNoise(wx / 192.0f, wz / 192.0f, seed ^ 0x51A9B17D) * 2.0f - 1.0f;
  float hills = NoiseGen::octaveNoise(wx / 72.0f, wz / 72.0f, seed ^ 0x7F4A7C15) * 2.0f - 1.0f;
  float detail = NoiseGen::octaveNoise(wx / 28.0f, wz / 28.0f, seed ^ 0x1D872B41) * 2.0f - 1.0f;
  int h = 64 + (int)(base * 18.0f + hills * 14.0f + detail * 6.0f);
  if (h < 4) h = 4;
  if (h > CHUNK_SIZE_Y - 2) h = CHUNK_SIZE_Y - 2;
  return h;
}

// Generate chunk
void WorldGen::generateChunk(
    uint8_t out[CHUNK_SIZE_X][CHUNK_SIZE_Z][CHUNK_SIZE_Y], int cx, int cz,
    int64_t worldSeed) {

  memset(out, BLOCK_AIR, CHUNK_SIZE_X * CHUNK_SIZE_Z * CHUNK_SIZE_Y);

  Random rng(worldSeed ^ ((int64_t)cx * 341873128712LL) ^
             ((int64_t)cz * 132897987541LL));

  // === Base Terrain ===
  for (int lx = 0; lx < CHUNK_SIZE_X; lx++) {
    for (int lz = 0; lz < CHUNK_SIZE_Z; lz++) {
      int wx = cx * CHUNK_SIZE_X + lx;
      int wz = cz * CHUNK_SIZE_Z + lz;

      int surfaceY = getTerrainHeight(wx, wz, worldSeed);
      if (surfaceY >= CHUNK_SIZE_Y)
        surfaceY = CHUNK_SIZE_Y - 1;

      // Base density fill for surface-pass: solid stone + bedrock only.
      for (int y = 0; y <= surfaceY; y++) {
        out[lx][lz][y] = (y == 0) ? BLOCK_BEDROCK : BLOCK_STONE;
      }

      // Water at sea level (MCPE-style ~62).
      const int seaLevel = 62;
      if (surfaceY < seaLevel) {
        for (int y = surfaceY + 1; y <= seaLevel; y++) {
          if (y < CHUNK_SIZE_Y)
            out[lx][lz][y] = BLOCK_WATER_STILL;
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

      float sandN = NoiseGen::octaveNoise(wx / 32.0f, wz / 32.0f, worldSeed ^ 0x5A17D3LL, 4, 0.5f) * 2.0f - 1.0f;
      float gravelN = NoiseGen::octaveNoise(wx / 32.0f, wz / 32.0f, worldSeed ^ 0x193A49LL, 4, 0.5f) * 2.0f - 1.0f;
      float depthN = NoiseGen::octaveNoise(wx / 16.0f, wz / 16.0f, worldSeed ^ 0x77B41CLL, 4, 0.5f) * 2.0f - 1.0f;

      bool useSand = (sandN + rng.nextFloat() * 0.2f) > 0.0f;
      bool useGravel = (gravelN + rng.nextFloat() * 0.2f) > 0.15f;
      int runDepth = (int)(depthN / 3.0f + 3.0f + rng.nextFloat() * 0.25f);

      int run = -1;
      uint8_t top = BLOCK_GRASS;
      uint8_t filler = BLOCK_DIRT;

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
            top = BLOCK_GRASS;
            filler = BLOCK_DIRT;
            if (useGravel) {
              top = BLOCK_AIR;
              filler = BLOCK_GRAVEL;
            }
            if (useSand) {
              top = BLOCK_SAND;
              filler = BLOCK_SAND;
            }
          }

          if (y < seaLevel && top == BLOCK_AIR) top = BLOCK_WATER_STILL;

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
