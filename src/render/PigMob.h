#pragma once

class Level;
class TextureAtlas;

struct PigMob {
  float x, y, z;
  float vx, vy, vz;

  float bodyYawDeg;
  float headYawDeg;
  float headPitchDeg;
  float walkAnim;

  float aiTimer;
  float lookTimer;
  float targetYawDeg;
  float moveSpeed;

  bool onGround;
};

void PigMob_Init(PigMob *pig, Level *level, int seed);
void PigMob_Update(PigMob *pig, Level *level, float dt, float playerX, float playerY,
                   float playerZ);
void PigMob_Render(const PigMob *pig, TextureAtlas *terrainAtlas);
void PigMob_Shutdown();
