#pragma once
// WorldGen.h - World generator (port/adaptation of RandomLevelSource.cpp 4J Studios)
// Uses NoiseGen and TreeFeature as separate classes
#include "chunk_defs.h"
#include <stdint.h>

class WorldGen {
public:
  enum BiomeId {
    BIOME_RAINFOREST = 0,
    BIOME_SWAMPLAND = 1,
    BIOME_SEASONAL_FOREST = 2,
    BIOME_FOREST = 3,
    BIOME_SAVANNA = 4,
    BIOME_SHRUBLAND = 5,
    BIOME_TAIGA = 6,
    BIOME_DESERT = 7,
    BIOME_PLAINS = 8,
    BIOME_ICE_DESERT = 9,
    BIOME_TUNDRA = 10
  };

  // Generates blocks for a chunk (cx, cz) into output[x][z][y]
  static void
  generateChunk(uint8_t out[CHUNK_SIZE_X][CHUNK_SIZE_Z][CHUNK_SIZE_Y], int cx,
                int cz, int64_t worldSeed);

  // Returns terrain height at world coordinate (wx, wz)
  static int getTerrainHeight(int wx, int wz, int64_t seed);

  // Returns biome id at world x/z for worldgen/post-process alignment.
  static int getBiomeId(int wx, int wz, int64_t seed);

  // Returns climate temperature at world x/z.
  static float getTemperature(int wx, int wz, int64_t seed);
};
