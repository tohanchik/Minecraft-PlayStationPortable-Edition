#pragma once

#include "Synth.h"
class Random;

class ImprovedNoise : public Synth {
public:
  ImprovedNoise();
  explicit ImprovedNoise(Random *random);
  void init(Random *random);

  float noise(float x, float y, float z);
  float getValue(float x, float y);
  float getValue(float x, float y, float z);

  void add(float *buffer, float x, float y, float z, int xSize, int ySize,
           int zSize, float xs, float ys, float zs, float pow);

  float scale;
  float xo, yo, zo;

private:
  float lerp(float t, float a, float b) const;
  float grad2(int hash, float x, float z) const;
  float grad(int hash, float x, float y, float z) const;

  int p[512];
};
