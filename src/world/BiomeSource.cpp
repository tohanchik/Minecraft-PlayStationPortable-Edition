#include "BiomeSource.h"
#include "Blocks.h"
#include "NoiseGen.h"

static GenBiomeType pickBiome(float temperature, float downfall) {
  downfall *= temperature;
  if (temperature < 0.10f) return GEN_BIOME_TUNDRA;
  if (downfall < 0.20f) {
    if (temperature < 0.50f) return GEN_BIOME_TUNDRA;
    if (temperature < 0.95f) return GEN_BIOME_SAVANNA;
    return GEN_BIOME_DESERT;
  }
  if (downfall > 0.5f && temperature < 0.7f) return GEN_BIOME_SWAMPLAND;
  if (temperature < 0.50f) return GEN_BIOME_TAIGA;
  if (temperature < 0.97f) {
    if (downfall < 0.35f) return GEN_BIOME_SHRUBLAND;
    return GEN_BIOME_FOREST;
  }
  if (downfall < 0.45f) return GEN_BIOME_PLAINS;
  if (downfall < 0.90f) return GEN_BIOME_SEASONAL_FOREST;
  return GEN_BIOME_RAINFOREST;
}

BiomeData BiomeSource::sampleBiome(int wx, int wz, int64_t worldSeed) {
  const float tempScale = 1.0f / 40.0f;
  const float downfallScale = 1.0f / 20.0f;
  const float noiseScale = 0.25f;

  float t = NoiseGen::octaveNoise(wx * tempScale, wz * tempScale, worldSeed * 9871LL, 4, 0.5f);
  float d = NoiseGen::octaveNoise(wx * downfallScale, wz * downfallScale, worldSeed * 39811LL, 4, 0.5f);
  float n = NoiseGen::octaveNoise(wx * noiseScale, wz * noiseScale, worldSeed * 543321LL, 2, 0.5f);

  float noise = n * 1.1f + 0.5f;
  float temperature = (t * 0.15f + 0.7f) * 0.99f + noise * 0.01f;
  float downfall = (d * 0.15f + 0.5f) * 0.998f + noise * 0.002f;
  temperature = 1.0f - (1.0f - temperature) * (1.0f - temperature);

  if (temperature < 0.0f) temperature = 0.0f;
  if (temperature > 1.0f) temperature = 1.0f;
  if (downfall < 0.0f) downfall = 0.0f;
  if (downfall > 1.0f) downfall = 1.0f;

  BiomeData out;
  out.temperature = temperature;
  out.downfall = downfall;
  out.type = pickBiome(temperature, downfall);
  out.topBlock = BLOCK_GRASS;
  out.fillerBlock = BLOCK_DIRT;

  if (out.type == GEN_BIOME_DESERT) {
    out.topBlock = BLOCK_SAND;
    out.fillerBlock = BLOCK_SAND;
  }
  return out;
}

