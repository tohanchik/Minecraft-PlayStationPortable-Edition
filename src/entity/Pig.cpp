#include "Pig.h"

#include "world/AABB.h"
#include "world/Blocks.h"
#include "world/Chunk.h"
#include "world/Level.h"
#include <math.h>
#include <vector>

namespace {
static const float PIG_R = 0.45f;
static const float PIG_H = 0.90f;
static const float PIG_SPAWN_ABOVE_GROUND = 3.0f;

static float findGroundY(Level *level, int wx, int wz) {
  if (!level) return 1.0f;
  for (int y = CHUNK_SIZE_Y - 1; y >= 0; --y) {
    uint8_t id = level->getBlock(wx, y, wz);
    if (id != BLOCK_AIR && g_blockProps[id].isSolid()) {
      return (float)y + 1.0f;
    }
  }
  return 1.0f;
}

static void spawnAtGroundColumn(Pig &pig, Level *level, float playerX, float playerY, float playerZ, float playerYawDeg) {
  int sx = (int)floorf(playerX);
  int sz = (int)floorf(playerZ);
  float groundY = findGroundY(level, sx, sz);

  pig.x = (float)sx + 0.5f;
  pig.z = (float)sz + 0.5f;
  pig.y = groundY + PIG_SPAWN_ABOVE_GROUND;
  if (pig.y < playerY + 1.0f) pig.y = playerY + 1.0f;
  pig.yaw = playerYawDeg;
  pig.velY = 0.0f;
  pig.onGround = false;
}
}

void Pig_Init(Pig &pig, Level *level, float playerX, float playerY, float playerZ, float playerYawDeg) {
  pig.active = true;
  pig.animTime = 0.0f;
  spawnAtGroundColumn(pig, level, playerX, playerY, playerZ, playerYawDeg);
}

void Pig_Update(Pig &pig, Level *level, float dt, float playerX, float playerY, float playerZ, float playerYawDeg) {
  if (!pig.active || !level) return;
  (void)playerX;
  (void)playerY;
  (void)playerZ;
  (void)playerYawDeg;

  pig.animTime += dt;
  if (pig.animTime > 10000.0f) pig.animTime = 0.0f;
  pig.velY -= 20.0f * dt;
  float dy = pig.velY * dt;

  AABB aabb(pig.x - PIG_R, pig.y, pig.z - PIG_R,
            pig.x + PIG_R, pig.y + PIG_H, pig.z + PIG_R);

  AABB *expanded = aabb.expand(0.0, dy, 0.0);
  std::vector<AABB> cubes = level->getCubes(*expanded);
  delete expanded;

  float dyOrg = dy;
  for (size_t i = 0; i < cubes.size(); ++i) dy = (float)cubes[i].clipYCollide(&aabb, dy);
  aabb.move(0, dy, 0);

  pig.onGround = (dyOrg != dy && dyOrg < 0.0f);
  if (pig.onGround || dyOrg != dy) pig.velY = 0.0f;

  pig.y = (float)aabb.y0;
}
