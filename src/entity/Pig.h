#pragma once

class Level;

struct Pig {
  float x, y, z;
  float yaw;
  float velY;
  bool onGround;
  bool active;
  float animTime;
};

void Pig_Init(Pig &pig, Level *level, float playerX, float playerY, float playerZ, float playerYawDeg);
void Pig_Update(Pig &pig, Level *level, float dt, float playerX, float playerY, float playerZ, float playerYawDeg);
