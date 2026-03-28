#include "Pig.h"

#include "world/AABB.h"
#include "world/Level.h"
#include "world/Mth.h"
#include <math.h>
#include <vector>

namespace {
static const float PIG_R = 0.45f;
static const float PIG_H = 0.90f;

static void spawnNearPlayer(Pig &pig, float playerX, float playerY, float playerZ, float playerYawDeg) {
  float yawRad = playerYawDeg * Mth::DEGRAD;
  pig.x = playerX + Mth::sin(yawRad) * 2.4f;
  pig.z = playerZ + Mth::cos(yawRad) * 2.4f;
  pig.y = playerY + 0.6f;
  pig.yaw = playerYawDeg + 180.0f;
  pig.velY = 0.0f;
  pig.onGround = false;
}
}

void Pig_Init(Pig &pig, Level * /*level*/, float playerX, float playerY, float playerZ, float playerYawDeg) {
  pig.active = true;
  pig.animTime = 0.0f;
  spawnNearPlayer(pig, playerX, playerY, playerZ, playerYawDeg);
}

void Pig_Update(Pig &pig, Level *level, float dt, float playerX, float playerY, float playerZ, float playerYawDeg) {
  if (!pig.active || !level) return;

  pig.animTime += dt;
  if (pig.animTime > 10000.0f) pig.animTime = 0.0f;

  float yawRad = playerYawDeg * Mth::DEGRAD;
  float tx = playerX + Mth::sin(yawRad) * 2.4f;
  float tz = playerZ + Mth::cos(yawRad) * 2.4f;

  float follow = dt * 3.0f;
  if (follow > 1.0f) follow = 1.0f;

  float dx = (tx - pig.x) * follow;
  float dz = (tz - pig.z) * follow;

  pig.velY -= 20.0f * dt;
  float dy = pig.velY * dt;

  AABB aabb(pig.x - PIG_R, pig.y, pig.z - PIG_R,
            pig.x + PIG_R, pig.y + PIG_H, pig.z + PIG_R);

  AABB *expanded = aabb.expand(dx, dy, dz);
  std::vector<AABB> cubes = level->getCubes(*expanded);
  delete expanded;

  float dyOrg = dy;
  for (size_t i = 0; i < cubes.size(); ++i) dy = (float)cubes[i].clipYCollide(&aabb, dy);
  aabb.move(0, dy, 0);

  for (size_t i = 0; i < cubes.size(); ++i) dx = (float)cubes[i].clipXCollide(&aabb, dx);
  aabb.move(dx, 0, 0);

  for (size_t i = 0; i < cubes.size(); ++i) dz = (float)cubes[i].clipZCollide(&aabb, dz);
  aabb.move(0, 0, dz);

  pig.onGround = (dyOrg != dy && dyOrg < 0.0f);
  if (pig.onGround || dyOrg != dy) pig.velY = 0.0f;

  pig.x = (float)((aabb.x0 + aabb.x1) * 0.5);
  pig.y = (float)aabb.y0;
  pig.z = (float)((aabb.z0 + aabb.z1) * 0.5);
  pig.yaw = playerYawDeg + 180.0f;

  float ddx = pig.x - playerX;
  float ddz = pig.z - playerZ;
  if (ddx * ddx + ddz * ddz > 100.0f) {
    spawnNearPlayer(pig, playerX, playerY, playerZ, playerYawDeg);
  }
}
