// WorldGen.cpp

#include "WorldGen.h"
#include "Blocks.h"
#include "NoiseGen.h"
#include "PerlinNoise.h"
#include "Random.h"
#include "chunk_defs.h"
#include <string.h>
#include <math.h>

struct GenBiomeSurface {
  uint8_t top;
  uint8_t filler;
};

static int pickBiome(float temperature, float downfall) {
  downfall *= temperature;
  if (temperature < 0.10f) return WorldGen::BIOME_TUNDRA;
  if (downfall < 0.20f) {
    if (temperature < 0.50f) return WorldGen::BIOME_TUNDRA;
    if (temperature < 0.95f) return WorldGen::BIOME_SAVANNA;
    return WorldGen::BIOME_DESERT;
  }
  if (downfall > 0.50f && temperature < 0.70f) return WorldGen::BIOME_SWAMPLAND;
  if (temperature < 0.50f) return WorldGen::BIOME_TAIGA;
  if (temperature < 0.97f) {
    if (downfall < 0.35f) return WorldGen::BIOME_SHRUBLAND;
    return WorldGen::BIOME_FOREST;
  }
  if (downfall < 0.45f) return WorldGen::BIOME_PLAINS;
  if (downfall < 0.90f) return WorldGen::BIOME_SEASONAL_FOREST;
  return WorldGen::BIOME_RAINFOREST;
}

static GenBiomeSurface getBiomeSurface(int biome) {
  GenBiomeSurface out = {BLOCK_GRASS, BLOCK_DIRT};
  if (biome == WorldGen::BIOME_DESERT || biome == WorldGen::BIOME_ICE_DESERT) {
    out.top = BLOCK_SAND;
    out.filler = BLOCK_SAND;
  } else if (biome == WorldGen::BIOME_TUNDRA) {
    out.top = BLOCK_SNOW;
    out.filler = BLOCK_DIRT;
  }
  return out;
}

static void getClimateAt(int wx, int wz, int64_t seed, float &temperature,
                         float &downfall, int &biome) {
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

static void fillHeightsFromMcpeModel(float *buffer, int x, int y, int z,
                                     int xSize, int ySize, int zSize,
                                     int64_t worldSeed,
                                     float tempMap[CHUNK_SIZE_X][CHUNK_SIZE_Z],
                                     float downfallMap[CHUNK_SIZE_X][CHUNK_SIZE_Z]) {
  Random random(worldSeed);
  PerlinNoise lperlinNoise1(&random, 16);
  PerlinNoise lperlinNoise2(&random, 16);
  PerlinNoise perlinNoise1(&random, 8);
  PerlinNoise scaleNoise(&random, 10);
  PerlinNoise depthNoise(&random, 16);

  float s = 684.412f;
  float hs = 684.412f;

  const int size2d = xSize * zSize;
  const int size3d = xSize * ySize * zSize;
  float sr[size2d];
  float dr[size2d];
  float pnr[size3d];
  float ar[size3d];
  float br[size3d];

  scaleNoise.getRegion(sr, (float)x, 0.0f, (float)z, xSize, 1, zSize, 1.121f,
                       1.0f, 1.121f);
  depthNoise.getRegion(dr, (float)x, 0.0f, (float)z, xSize, 1, zSize, 200.0f,
                       1.0f, 200.0f);
  perlinNoise1.getRegion(pnr, (float)x, (float)y, (float)z, xSize, ySize, zSize,
                         s / 80.0f, hs / 160.0f, s / 80.0f);
  lperlinNoise1.getRegion(ar, (float)x, (float)y, (float)z, xSize, ySize, zSize,
                          s, hs, s);
  lperlinNoise2.getRegion(br, (float)x, (float)y, (float)z, xSize, ySize, zSize,
                          s, hs, s);

  int p = 0;
  int pp = 0;
  int wScale = 16 / xSize;

  for (int xx = 0; xx < xSize; xx++) {
    int xp = xx * wScale + wScale / 2;
    if (xp < 0) xp = 0;
    if (xp > CHUNK_SIZE_X - 1) xp = CHUNK_SIZE_X - 1;

    for (int zz = 0; zz < zSize; zz++) {
      int zp = zz * wScale + wScale / 2;
      if (zp < 0) zp = 0;
      if (zp > CHUNK_SIZE_Z - 1) zp = CHUNK_SIZE_Z - 1;

      float temperature = tempMap[xp][zp];
      float downfall = downfallMap[xp][zp] * temperature;
      float dd = 1.0f - downfall;
      dd = dd * dd;
      dd = dd * dd;
      dd = 1.0f - dd;

      float scale = ((sr[pp] + 256.0f) / 512.0f);
      scale *= dd;
      if (scale > 1.0f) scale = 1.0f;

      float depth = (dr[pp] / 8000.0f);
      if (depth < 0) depth = -depth * 0.3f;
      depth = depth * 3.0f - 2.0f;

      if (depth < 0) {
        depth = depth / 2.0f;
        if (depth < -1.0f) depth = -1.0f;
        depth = depth / 1.4f;
        depth /= 2.0f;
        scale = 0.0f;
      } else {
        if (depth > 1.0f) depth = 1.0f;
        depth = depth / 8.0f;
      }

      if (scale < 0.0f) scale = 0.0f;
      scale = scale + 0.5f;
      depth = depth * ySize / 16.0f;
      float yCenter = ySize / 2.0f + depth * 4.0f;
      pp++;

      for (int yy = 0; yy < ySize; yy++) {
        float yOffs = (yy - yCenter) * 12.0f / scale;
        if (yOffs < 0.0f) yOffs *= 4.0f;

        float bb = ar[p] / 512.0f;
        float cc = br[p] / 512.0f;
        float v = (pnr[p] / 10.0f + 1.0f) / 2.0f;
        float val = (v < 0.0f) ? bb : (v > 1.0f ? cc : bb + (cc - bb) * v);
        val -= yOffs;

        if (yy > ySize - 4) {
          float slide = (yy - (ySize - 4)) / 3.0f;
          val = val * (1.0f - slide) + -10.0f * slide;
        }
        buffer[p++] = val;
      }
    }
  }
}

static inline bool inChunk(int lx, int ly, int lz) {
  return lx >= 0 && lx < CHUNK_SIZE_X && lz >= 0 && lz < CHUNK_SIZE_Z &&
         ly > 0 && ly < CHUNK_SIZE_Y;
}

static inline int blockIndex(int lx, int ly, int lz) {
  return (lx * CHUNK_SIZE_Z + lz) * CHUNK_SIZE_Y + ly;
}

static void addTunnelMcpeCave(uint8_t *flat, int xOffs, int zOffs, Random &rnd,
                              int64_t worldSeed, float xCave, float yCave,
                              float zCave, float thickness, float yRot,
                              float xRot, int step, int dist, float yScale) {
  const float PI = 3.14159265f;
  float xMid = (float)(xOffs * 16 + 8);
  float zMid = (float)(zOffs * 16 + 8);

  float yRota = 0.0f;
  float xRota = 0.0f;
  Random local(rnd.nextLong());

  const int radius = 8;
  if (dist <= 0) {
    int max = radius * 16 - 16;
    dist = max - local.nextInt(max / 4);
  }
  bool singleStep = false;
  if (step == -1) {
    step = dist / 2;
    singleStep = true;
  }

  int splitPoint = local.nextInt(dist / 2) + dist / 4;
  bool steep = local.nextInt(6) == 0;

  for (; step < dist; step++) {
    float rad = 1.5f + (sinf(step * PI / dist) * thickness);
    float yRad = rad * yScale;

    float xc = cosf(xRot);
    float xs = sinf(xRot);
    xCave += cosf(yRot) * xc;
    yCave += xs;
    zCave += sinf(yRot) * xc;

    xRot *= steep ? 0.92f : 0.7f;
    xRot += xRota * 0.1f;
    yRot += yRota * 0.1f;
    xRota *= 0.90f;
    yRota *= 0.75f;
    xRota += (local.nextFloat() - local.nextFloat()) * local.nextFloat() * 2.0f;
    yRota += (local.nextFloat() - local.nextFloat()) * local.nextFloat() * 4.0f;

    if (!singleStep && step == splitPoint && thickness > 1.0f) {
      addTunnelMcpeCave(flat, xOffs, zOffs, rnd, worldSeed, xCave, yCave, zCave,
                        local.nextFloat() * 0.5f + 0.5f, yRot - PI / 2.0f,
                        xRot / 3.0f, step, dist, 1.0f);
      addTunnelMcpeCave(flat, xOffs, zOffs, rnd, worldSeed, xCave, yCave, zCave,
                        local.nextFloat() * 0.5f + 0.5f, yRot + PI / 2.0f,
                        xRot / 3.0f, step, dist, 1.0f);
      return;
    }
    if (!singleStep && local.nextInt(4) == 0) continue;

    float xd = xCave - xMid;
    float zd = zCave - zMid;
    float remaining = (float)(dist - step);
    float rr = (thickness + 2.0f) + 16.0f;
    if (xd * xd + zd * zd - (remaining * remaining) > rr * rr) return;

    if (xCave < xMid - 16 - rad * 2 || zCave < zMid - 16 - rad * 2 ||
        xCave > xMid + 16 + rad * 2 || zCave > zMid + 16 + rad * 2)
      continue;

    int x0 = (int)floorf(xCave - rad) - xOffs * 16 - 1;
    int x1 = (int)floorf(xCave + rad) - xOffs * 16 + 1;
    int y0 = (int)floorf(yCave - yRad) - 1;
    int y1 = (int)floorf(yCave + yRad) + 1;
    int z0 = (int)floorf(zCave - rad) - zOffs * 16 - 1;
    int z1 = (int)floorf(zCave + rad) - zOffs * 16 + 1;
    if (x0 < 0) x0 = 0;
    if (x1 > 16) x1 = 16;
    if (y0 < 1) y0 = 1;
    if (y1 > 120) y1 = 120;
    if (z0 < 0) z0 = 0;
    if (z1 > 16) z1 = 16;

    bool detectedWater = false;
    for (int xx = x0; !detectedWater && xx < x1; xx++) {
      for (int zz = z0; !detectedWater && zz < z1; zz++) {
        for (int yy = y1 + 1; !detectedWater && yy >= y0 - 1; yy--) {
          if (yy < 0 || yy >= CHUNK_SIZE_Y) continue;
          uint8_t b = flat[blockIndex(xx, yy, zz)];
          if (b == BLOCK_WATER_FLOW || b == BLOCK_WATER_STILL) detectedWater = true;
          if (yy != y0 - 1 && xx != x0 && xx != x1 - 1 && zz != z0 && zz != z1 - 1)
            yy = y0;
        }
      }
    }
    if (detectedWater) continue;

    for (int xx = x0; xx < x1; xx++) {
      float xdn = ((xx + xOffs * 16 + 0.5f) - xCave) / rad;
      for (int zz = z0; zz < z1; zz++) {
        float zdn = ((zz + zOffs * 16 + 0.5f) - zCave) / rad;
        bool hasGrass = false;
        if (xdn * xdn + zdn * zdn >= 1.0f) continue;
        for (int yy = y1 - 1; yy >= y0; yy--) {
          float ydn = (yy + 0.5f - yCave) / yRad;
          if (ydn <= -0.7f || xdn * xdn + ydn * ydn + zdn * zdn >= 1.0f) continue;
          int idx = blockIndex(xx, yy, zz);
          uint8_t block = flat[idx];
          if (block == BLOCK_GRASS) hasGrass = true;
          if (block == BLOCK_STONE || block == BLOCK_DIRT || block == BLOCK_GRASS) {
            if (yy < 10) flat[idx] = BLOCK_LAVA_STILL;
            else {
              flat[idx] = BLOCK_AIR;
              if (hasGrass && yy > 0 && flat[blockIndex(xx, yy - 1, zz)] == BLOCK_DIRT)
                flat[blockIndex(xx, yy - 1, zz)] = BLOCK_GRASS;
            }
          }
        }
      }
    }
    if (singleStep) break;
  }
}

static void applyLargeCaveFeature(uint8_t *flat, int cx, int cz, int64_t worldSeed) {
  Random random(worldSeed);
  int64_t xScale = random.nextLong() / 2LL * 2LL + 1LL;
  int64_t zScale = random.nextLong() / 2LL * 2LL + 1LL;
  const int radius = 8;

  for (int x = cx - radius; x <= cx + radius; ++x) {
    for (int z = cz - radius; z <= cz + radius; ++z) {
      random.setSeed(((int64_t)x * xScale + (int64_t)z * zScale) ^ worldSeed);
      int caves = random.nextInt(random.nextInt(random.nextInt(40) + 1) + 1);
      if (random.nextInt(15) != 0) caves = 0;

      for (int cave = 0; cave < caves; cave++) {
        float xCave = (float)(x * 16 + random.nextInt(16));
        float yCave = (float)(random.nextInt(random.nextInt(120) + 8));
        float zCave = (float)(z * 16 + random.nextInt(16));
        int tunnels = 1;
        if (random.nextInt(4) == 0) {
          addTunnelMcpeCave(flat, cx, cz, random, worldSeed, xCave, yCave, zCave,
                            1.0f + random.nextFloat() * 6.0f, 0.0f, 0.0f, -1,
                            -1, 0.5f);
          tunnels += random.nextInt(4);
        }
        for (int i = 0; i < tunnels; i++) {
          float yRot = random.nextFloat() * 3.14159265f * 2.0f;
          float xRot = ((random.nextFloat() - 0.5f) * 2.0f) / 8.0f;
          float thick = random.nextFloat() * 2.0f + random.nextFloat();
          addTunnelMcpeCave(flat, cx, cz, random, worldSeed, xCave, yCave, zCave,
                            thick, yRot, xRot, 0, 0, 1.0f);
        }
      }
    }
  }
}

// Get terrain height
int WorldGen::getTerrainHeight(int wx, int wz, int64_t seed) {
  float tempMap[CHUNK_SIZE_X][CHUNK_SIZE_Z];
  float downfallMap[CHUNK_SIZE_X][CHUNK_SIZE_Z];
  for (int x = 0; x < CHUNK_SIZE_X; ++x) {
    for (int z = 0; z < CHUNK_SIZE_Z; ++z) {
      int biome = BIOME_PLAINS;
      getClimateAt(wx, wz, seed, tempMap[x][z], downfallMap[x][z], biome);
    }
  }

  const int ySize = CHUNK_SIZE_Y / 8 + 1;
  float col[1][1][ySize];
  fillHeightsFromMcpeModel(&col[0][0][0], wx / 4, 0, wz / 4, 1, ySize, 1, seed,
                           tempMap, downfallMap);

  int top = 1;
  for (int gy = 0; gy < ySize - 1; ++gy) {
    float d0 = col[0][0][gy];
    float d1 = col[0][0][gy + 1];
    for (int iy = 0; iy < 8; ++iy) {
      int y = gy * 8 + iy;
      if (y >= CHUNK_SIZE_Y) break;
      float t = (float)iy / 8.0f;
      float d = d0 + (d1 - d0) * t;
      if (d > 0.0f) top = y;
    }
  }
  return top;
}

int WorldGen::getBiomeId(int wx, int wz, int64_t seed) {
  float t = 0.5f;
  float d = 0.5f;
  int biome = BIOME_PLAINS;
  getClimateAt(wx, wz, seed, t, d, biome);
  return biome;
}

// Generate chunk
void WorldGen::generateChunk(
    uint8_t out[CHUNK_SIZE_X][CHUNK_SIZE_Z][CHUNK_SIZE_Y], int cx, int cz,
    int64_t worldSeed) {

  memset(out, BLOCK_AIR, CHUNK_SIZE_X * CHUNK_SIZE_Z * CHUNK_SIZE_Y);

  Random rng(worldSeed ^ ((int64_t)cx * 341873128712LL) ^
             ((int64_t)cz * 132897987541LL));
  const int seaLevel = 62;

  float temperatureMap[CHUNK_SIZE_X][CHUNK_SIZE_Z];
  float downfallMap[CHUNK_SIZE_X][CHUNK_SIZE_Z];
  int biomeMap[CHUNK_SIZE_X][CHUNK_SIZE_Z];

  for (int lx = 0; lx < CHUNK_SIZE_X; ++lx) {
    for (int lz = 0; lz < CHUNK_SIZE_Z; ++lz) {
      int wx = cx * CHUNK_SIZE_X + lx;
      int wz = cz * CHUNK_SIZE_Z + lz;
      float temperature = 0.5f;
      float downfall = 0.5f;
      int biome = BIOME_PLAINS;
      getClimateAt(wx, wz, worldSeed, temperature, downfall, biome);
      temperatureMap[lx][lz] = temperature;
      downfallMap[lx][lz] = downfall;
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

  fillHeightsFromMcpeModel(&density[0][0][0], cx * (CHUNK_SIZE_X / cellX), 0,
                           cz * (CHUNK_SIZE_Z / cellZ), sx, sy, sz, worldSeed,
                           temperatureMap, downfallMap);

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

  // === Cave carve pass (LargeCaveFeature-style, MCPE 0.6.1 order) ===
  applyLargeCaveFeature(&out[0][0][0], cx, cz, worldSeed);

  // === Surface shaping pass (MCPE RandomLevelSource::buildSurfaces-like) ===
  // Converts top stone layers into biome-like top/filler (grass/dirt/sand/gravel)
  // and adds a small sandstone run beneath sand.
  for (int lx = 0; lx < CHUNK_SIZE_X; ++lx) {
    for (int lz = 0; lz < CHUNK_SIZE_Z; ++lz) {
      int wx = cx * CHUNK_SIZE_X + lx;
      int wz = cz * CHUNK_SIZE_Z + lz;
      float temperature = temperatureMap[lx][lz];
      int biome = biomeMap[lx][lz];
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

  int xo = cx * CHUNK_SIZE_X;
  int zo = cz * CHUNK_SIZE_Z;

}
