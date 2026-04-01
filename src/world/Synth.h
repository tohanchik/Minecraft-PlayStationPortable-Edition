#pragma once

class Synth {
public:
  virtual ~Synth();
  virtual float getValue(float x, float y) = 0;
  int getDataSize(int width, int height);
  void create(int width, int height, float *result);
};
