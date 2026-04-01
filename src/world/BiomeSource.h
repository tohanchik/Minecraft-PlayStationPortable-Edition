#pragma once

#include <stdint.h>

enum GenBiomeType {
  GEN_BIOME_TUNDRA = 0,
  GEN_BIOME_TAIGA,
  GEN_BIOME_SWAMPLAND,
  GEN_BIOME_SAVANNA,
  GEN_BIOME_SHRUBLAND,
  GEN_BIOME_FOREST,
  GEN_BIOME_PLAINS,
  GEN_BIOME_SEASONAL_FOREST,
  GEN_BIOME_RAINFOREST,
  GEN_BIOME_DESERT
};

struct BiomeData {
  float temperature;
  float downfall;
  uint8_t topBlock;
  uint8_t fillerBlock;
  GenBiomeType type;
};

class BiomeSource {
public:
  static BiomeData sampleBiome(int wx, int wz, int64_t worldSeed);
};

