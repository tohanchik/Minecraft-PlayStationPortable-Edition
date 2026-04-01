#include "PerlinNoise.h"
#include "ImprovedNoise.h"

void PerlinNoise::init(int levels_) {
  levels = levels_;
  noiseLevels = new ImprovedNoise *[levels];
  for (int i = 0; i < levels; i++) noiseLevels[i] = new ImprovedNoise(rndPtr);
}

PerlinNoise::PerlinNoise(int levels_) {
  rndPtr = &random;
  init(levels_);
}

PerlinNoise::PerlinNoise(Random *random_, int levels_) {
  rndPtr = random_;
  init(levels_);
}

PerlinNoise::~PerlinNoise() {
  for (int i = 0; i < levels; ++i) delete noiseLevels[i];
  delete[] noiseLevels;
}

float PerlinNoise::getValue(float x, float y) {
  float value = 0;
  float pow = 1;
  for (int i = 0; i < levels; i++) {
    value += noiseLevels[i]->getValue(x * pow, y * pow) / pow;
    pow /= 2;
  }
  return value;
}

float PerlinNoise::getValue(float x, float y, float z) {
  float value = 0;
  float pow = 1;
  for (int i = 0; i < levels; i++) {
    value += noiseLevels[i]->getValue(x * pow, y * pow, z * pow) / pow;
    pow /= 2;
  }
  return value;
}

float *PerlinNoise::getRegion(float *buffer, float x, float y, float z,
                              int xSize, int ySize, int zSize, float xScale,
                              float yScale, float zScale) {
  const int size = xSize * ySize * zSize;
  if (buffer == 0) buffer = new float[size];
  for (int i = 0; i < size; i++) buffer[i] = 0;

  float pow = 1;
  for (int i = 0; i < levels; i++) {
    noiseLevels[i]->add(buffer, x, y, z, xSize, ySize, zSize, xScale * pow,
                        yScale * pow, zScale * pow, pow);
    pow /= 2;
  }
  return buffer;
}
