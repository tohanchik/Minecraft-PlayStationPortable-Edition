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

static float findGroundYBelow(Level *level, int wx, int wz, int startY) {
  if (!level) return 1.0f;
  if (startY >= CHUNK_SIZE_Y) startY = CHUNK_SIZE_Y - 1;
  if (startY < 0) startY = 0;
  for (int y = startY; y >= 0; --y) {
    uint8_t id = level->getBlock(wx, y, wz);
    if (id != BLOCK_AIR && g_blockProps[id].isSolid()) {
      return (float)y + 1.0f;
    }
  }
  return 1.0f;
}

static void spawnAtGroundColumn(Pig &pig, Level *level, float playerX, float playerY, float playerZ, float playerYawDeg) {
  const float worldMaxX = (float)(WORLD_CHUNKS_X * CHUNK_SIZE_X - 1);
  const float worldMaxZ = (float)(WORLD_CHUNKS_Z * CHUNK_SIZE_Z - 1);
  float clampedX = playerX;
  float clampedZ = playerZ;
  if (clampedX < 0.5f) clampedX = 0.5f;
  if (clampedX > worldMaxX - 0.5f) clampedX = worldMaxX - 0.5f;
  if (clampedZ < 0.5f) clampedZ = 0.5f;
  if (clampedZ > worldMaxZ - 0.5f) clampedZ = worldMaxZ - 0.5f;

  int sx = (int)floorf(clampedX);
  int sz = (int)floorf(clampedZ);
  int sy = (int)floorf(playerY);
  float groundY = findGroundYBelow(level, sx, sz, sy);

  pig.x = clampedX;
  pig.z = clampedZ;
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
