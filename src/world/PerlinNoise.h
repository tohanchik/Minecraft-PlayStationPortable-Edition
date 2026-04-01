#pragma once

#include "Random.h"
#include "Synth.h"

class ImprovedNoise;

class PerlinNoise : public Synth {
public:
  explicit PerlinNoise(int levels);
  PerlinNoise(Random *random, int levels);
  ~PerlinNoise();

  float getValue(float x, float y);
  float getValue(float x, float y, float z);

  float *getRegion(float *buffer, float x, float y, float z, int xSize,
                   int ySize, int zSize, float xScale, float yScale,
                   float zScale);

private:
  void init(int levels);

  ImprovedNoise **noiseLevels;
  int levels;
  Random random;
  Random *rndPtr;
};
