#pragma once
// WorldGen.h - World generator (port/adaptation of RandomLevelSource.cpp 4J Studios)
// Uses NoiseGen and TreeFeature as separate classes
#include "chunk_defs.h"
#include <stdint.h>

class WorldGen {
public:
  enum BiomeId {
    BIOME_TUNDRA = 0,
    BIOME_TAIGA = 1,
    BIOME_PLAINS = 2,
    BIOME_FOREST = 3,
    BIOME_DESERT = 4
  };

  // Generates blocks for a chunk (cx, cz) into output[x][z][y]
  static void
  generateChunk(uint8_t out[CHUNK_SIZE_X][CHUNK_SIZE_Z][CHUNK_SIZE_Y], int cx,
                int cz, int64_t worldSeed);

  // Returns terrain height at world coordinate (wx, wz)
  static int getTerrainHeight(int wx, int wz, int64_t seed);

  // Returns biome id at world x/z for worldgen/post-process alignment.
  static int getBiomeId(int wx, int wz, int64_t seed);
};
