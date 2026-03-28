#include "PigMob.h"

#include "../world/AABB.h"
#include "../world/Blocks.h"
#include "../world/Level.h"
#include "../world/Mth.h"
#include "TextureAtlas.h"

#include <malloc.h>
#include <math.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "../stb_image.h"

namespace {
static const int PIG_TEX_W = 64;
static const int PIG_TEX_H = 32;
static uint32_t *g_pigTex = nullptr; // ABGR8888

static float frand01() { return (float)rand() / (float)RAND_MAX; }

static float wrapYaw(float y) {
  while (y > 180.0f) y -= 360.0f;
  while (y < -180.0f) y += 360.0f;
  return y;
}

static uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return 0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
}

static void putPx(int x, int y, uint32_t c) {
  if (!g_pigTex) return;
  if (x < 0 || y < 0 || x >= PIG_TEX_W || y >= PIG_TEX_H) return;
  g_pigTex[y * PIG_TEX_W + x] = c;
}

static void fillRect(int x, int y, int w, int h, uint32_t c) {
  for (int yy = y; yy < y + h; ++yy)
    for (int xx = x; xx < x + w; ++xx) putPx(xx, yy, c);
}



static unsigned char *readFile(const char *path, int *outSize) {
  SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
  if (fd < 0) return nullptr;

  int size = (int)sceIoLseek(fd, 0, PSP_SEEK_END);
  sceIoLseek(fd, 0, PSP_SEEK_SET);
  if (size <= 0 || size > 4 * 1024 * 1024) {
    sceIoClose(fd);
    return nullptr;
  }

  unsigned char *buf = (unsigned char *)malloc(size);
  if (!buf) {
    sceIoClose(fd);
    return nullptr;
  }

  sceIoRead(fd, buf, size);
  sceIoClose(fd);
  if (outSize) *outSize = size;
  return buf;
}

static bool loadPigTexturePng(const char *path) {
  int fileSize = 0;
  unsigned char *fileData = readFile(path, &fileSize);
  if (!fileData) return false;

  int w = 0, h = 0, ch = 0;
  unsigned char *pixels = stbi_load_from_memory(fileData, fileSize, &w, &h, &ch, 4);
  free(fileData);
  if (!pixels) return false;

  bool ok = (w == PIG_TEX_W && h == PIG_TEX_H);
  if (ok) {
    if (!g_pigTex) {
      g_pigTex = (uint32_t *)memalign(16, PIG_TEX_W * PIG_TEX_H * sizeof(uint32_t));
      if (!g_pigTex) ok = false;
    }
    if (ok) {
      // RGBA -> ABGR layout used by GU in this project.
      for (int i = 0; i < PIG_TEX_W * PIG_TEX_H; ++i) {
        unsigned char *p = pixels + i * 4;
        uint8_t r = p[0], g = p[1], b = p[2];
        uint8_t a = 255; // force opaque to avoid accidental full transparency on PSP
        g_pigTex[i] = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
      }
      sceKernelDcacheWritebackInvalidateRange(g_pigTex,
          PIG_TEX_W * PIG_TEX_H * sizeof(uint32_t));
    }
  }

  stbi_image_free(pixels);
  return ok;
}

static void initPigTexture() {
  if (g_pigTex) return;

  if (loadPigTexturePng("res/mob/pig.png")) {
    return; // Use external exact pig texture if provided.
  }

  g_pigTex = (uint32_t *)memalign(16, PIG_TEX_W * PIG_TEX_H * sizeof(uint32_t));
  if (!g_pigTex) return;

  // Opaque base background to guarantee visibility even if UVs miss islands.
  const uint32_t base = rgb(239, 166, 164);
  for (int i = 0; i < PIG_TEX_W * PIG_TEX_H; ++i) g_pigTex[i] = base;

  const uint32_t dark = rgb(214, 140, 138);
  const uint32_t snout = rgb(232, 167, 170);
  const uint32_t hoof = rgb(177, 118, 116);
  const uint32_t eye = rgb(16, 16, 16);

  // Paint the exact UV islands used by MCPE 0.6.1 PigModel/QuadrupedModel:
  // head(0,0), snout(16,16), body(28,8), legs(0,16).
  // We fill these UV areas so the cuboid renderer can sample them directly.

  // Head islands (8x8x8 at tex origin 0,0)
  fillRect(0, 8, 8, 8, dark);    // left
  fillRect(8, 8, 8, 8, base);    // front
  fillRect(16, 8, 8, 8, dark);   // right
  fillRect(24, 8, 8, 8, base);   // back
  fillRect(8, 0, 8, 8, base);    // top
  fillRect(16, 0, 8, 8, dark);   // bottom

  // Eyes on head front area.
  putPx(10, 11, eye);
  putPx(13, 11, eye);

  // Snout islands (4x3x1 at tex origin 16,16)
  fillRect(16, 17, 1, 3, dark);  // left
  fillRect(17, 17, 4, 3, snout); // front
  fillRect(21, 17, 1, 3, dark);  // right
  fillRect(22, 17, 4, 3, snout); // back
  fillRect(17, 16, 4, 1, snout); // top
  fillRect(21, 16, 4, 1, dark);  // bottom
  putPx(18, 18, eye);
  putPx(20, 18, eye);

    // Body islands (original QuadrupedModel box: 10x16x8 at tex origin 28,8).
  fillRect(28, 16, 8, 16, dark); // left (depth x height)
  fillRect(36, 16, 10, 16, base); // front
  fillRect(46, 16, 8, 16, dark); // right
  fillRect(54, 16, 10, 16, base); // back
  fillRect(36, 8, 10, 8, base); // top
  fillRect(46, 8, 10, 8, dark); // bottom

  // Legs islands (4x6x4 at tex origin 0,16)
  // shared texture for all 4 legs
  fillRect(0, 20, 4, 6, dark);   // left
  fillRect(4, 20, 4, 6, base);   // front
  fillRect(8, 20, 4, 6, dark);   // right
  fillRect(12, 20, 4, 6, base);  // back
  fillRect(4, 16, 4, 4, base);   // top
  fillRect(8, 16, 4, 4, hoof);   // bottom

  sceKernelDcacheWritebackInvalidateRange(g_pigTex,
                                          PIG_TEX_W * PIG_TEX_H * sizeof(uint32_t));
}

struct MobVtx {
  float u, v;
  uint32_t color;
  float x, y, z;
};

static void emitFace(MobVtx *v, int &idx, float u0, float v0, float u1, float v1,
                     uint32_t color, float x0, float y0, float z0, float x1,
                     float y1, float z1, float x2, float y2, float z2, float x3,
                     float y3, float z3) {
  v[idx++] = {u0, v0, color, x0, y0, z0};
  v[idx++] = {u0, v1, color, x2, y2, z2};
  v[idx++] = {u1, v0, color, x1, y1, z1};
  v[idx++] = {u1, v0, color, x1, y1, z1};
  v[idx++] = {u0, v1, color, x2, y2, z2};
  v[idx++] = {u1, v1, color, x3, y3, z3};
}

static void cuboidUV(float texU, float texV, float dx, float dy, float dz,
                     float out[6][4]) {
  // Faces: top, bottom, front, back, left, right
  // Standard MC layout for addBox(texU,texV,dx,dy,dz)
  float u = texU;
  float v = texV;

  // left
  out[4][0] = u;
  out[4][1] = v + dz;
  out[4][2] = u + dz;
  out[4][3] = v + dz + dy;

  // front
  out[2][0] = u + dz;
  out[2][1] = v + dz;
  out[2][2] = u + dz + dx;
  out[2][3] = v + dz + dy;

  // right
  out[5][0] = u + dz + dx;
  out[5][1] = v + dz;
  out[5][2] = u + dz + dx + dz;
  out[5][3] = v + dz + dy;

  // back
  out[3][0] = u + dz + dx + dz;
  out[3][1] = v + dz;
  out[3][2] = u + dz + dx + dz + dx;
  out[3][3] = v + dz + dy;

  // top
  out[0][0] = u + dz;
  out[0][1] = v;
  out[0][2] = u + dz + dx;
  out[0][3] = v + dz;

  // bottom
  out[1][0] = u + dz + dx;
  out[1][1] = v;
  out[1][2] = u + dz + dx + dx;
  out[1][3] = v + dz;
}

static void emitCuboid(float x0, float y0, float z0, float x1, float y1, float z1,
                       float texU, float texV, float dx, float dy, float dz,
                       uint32_t color) {
  MobVtx *v = (MobVtx *)sceGuGetMemory(36 * sizeof(MobVtx));
  int idx = 0;

  float fuv[6][4];
  cuboidUV(texU, texV, dx, dy, dz, fuv);

  auto u = [](float px) { return (px + 0.5f) / (float)PIG_TEX_W; };
  auto vv = [](float py) { return (py + 0.5f) / (float)PIG_TEX_H; };

  float topShade = 1.0f, sideShade = 0.86f, botShade = 0.72f;
  auto shade = [&](float s) {
    uint8_t r = (uint8_t)((color & 0xFF) * s);
    uint8_t g = (uint8_t)(((color >> 8) & 0xFF) * s);
    uint8_t b = (uint8_t)(((color >> 16) & 0xFF) * s);
    return 0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
  };

  uint32_t cTop = shade(topShade);
  uint32_t cSide = shade(sideShade);
  uint32_t cBot = shade(botShade);

  // top
  emitFace(v, idx, u(fuv[0][0]), vv(fuv[0][1]), u(fuv[0][2]), vv(fuv[0][3]), cTop,
           x0, y1, z0, x1, y1, z0, x0, y1, z1, x1, y1, z1);
  // bottom
  emitFace(v, idx, u(fuv[1][0]), vv(fuv[1][1]), u(fuv[1][2]), vv(fuv[1][3]), cBot,
           x0, y0, z1, x1, y0, z1, x0, y0, z0, x1, y0, z0);
  // front
  emitFace(v, idx, u(fuv[2][0]), vv(fuv[2][1]), u(fuv[2][2]), vv(fuv[2][3]), cSide,
           x0, y1, z1, x1, y1, z1, x0, y0, z1, x1, y0, z1);
  // back
  emitFace(v, idx, u(fuv[3][0]), vv(fuv[3][1]), u(fuv[3][2]), vv(fuv[3][3]), cSide,
           x1, y1, z0, x0, y1, z0, x1, y0, z0, x0, y0, z0);
  // left
  emitFace(v, idx, u(fuv[4][0]), vv(fuv[4][1]), u(fuv[4][2]), vv(fuv[4][3]), cSide,
           x0, y1, z0, x0, y1, z1, x0, y0, z0, x0, y0, z1);
  // right
  emitFace(v, idx, u(fuv[5][0]), vv(fuv[5][1]), u(fuv[5][2]), vv(fuv[5][3]), cSide,
           x1, y1, z1, x1, y1, z0, x1, y0, z1, x1, y0, z0);

  sceGuDrawArray(GU_TRIANGLES,
                 GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF |
                     GU_TRANSFORM_3D,
                 36, 0, v);
}



static void emitSolidBox(float x0, float y0, float z0, float x1, float y1, float z1,
                         uint32_t color) {
  MobVtx *v = (MobVtx *)sceGuGetMemory(36 * sizeof(MobVtx));
  int idx = 0;
  auto face = [&](float ax, float ay, float az, float bx, float by, float bz,
                  float cx, float cy, float cz, float dx, float dy, float dz) {
    v[idx++] = {0, 0, color, ax, ay, az};
    v[idx++] = {0, 0, color, cx, cy, cz};
    v[idx++] = {0, 0, color, bx, by, bz};
    v[idx++] = {0, 0, color, bx, by, bz};
    v[idx++] = {0, 0, color, cx, cy, cz};
    v[idx++] = {0, 0, color, dx, dy, dz};
  };

  face(x0, y1, z0, x1, y1, z0, x0, y1, z1, x1, y1, z1); // top
  face(x0, y0, z1, x1, y0, z1, x0, y0, z0, x1, y0, z0); // bottom
  face(x0, y1, z1, x1, y1, z1, x0, y0, z1, x1, y0, z1); // front
  face(x1, y1, z0, x0, y1, z0, x1, y0, z0, x0, y0, z0); // back
  face(x0, y1, z0, x0, y1, z1, x0, y0, z0, x0, y0, z1); // left
  face(x1, y1, z1, x1, y1, z0, x1, y0, z1, x1, y0, z0); // right

  sceGuDrawArray(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D,
                 36, 0, v);
}

static float clampf(float v, float a, float b) {
  return (v < a) ? a : (v > b ? b : v);
}

} // namespace

static float findGroundY(Level *level, float x, float z) {
  int ix = (int)floorf(x);
  int iz = (int)floorf(z);
  if (ix < 0) ix = 0;
  if (iz < 0) iz = 0;
  int maxX = WORLD_CHUNKS_X * CHUNK_SIZE_X - 1;
  int maxZ = WORLD_CHUNKS_Z * CHUNK_SIZE_Z - 1;
  if (ix > maxX) ix = maxX;
  if (iz > maxZ) iz = maxZ;

  for (int yy = CHUNK_SIZE_Y - 1; yy >= 0; --yy) {
    uint8_t id = level->getBlock(ix, yy, iz);
    if (id != BLOCK_AIR && g_blockProps[id].isSolid()) return (float)(yy + 1);
  }
  return 1.0f;
}

void PigMob_Init(PigMob *pig, Level *level, int seed, float spawnX, float spawnZ) {
  srand(seed);
  initPigTexture();

  pig->x = spawnX;
  pig->z = spawnZ;
  pig->y = findGroundY(level, pig->x, pig->z);

  pig->vx = pig->vy = pig->vz = 0.0f;
  pig->bodyYawDeg = 0.0f;
  pig->headYawDeg = 0.0f;
  pig->headPitchDeg = 0.0f;
  pig->walkAnim = 0.0f;
  pig->aiTimer = 0.2f;
  pig->lookTimer = 0.3f;
  pig->targetYawDeg = 0.0f;
  pig->moveSpeed = 0.85f;
  pig->onGround = false;
}

void PigMob_Update(PigMob *pig, Level *level, float dt, float playerX, float playerY,
                   float playerZ, float playerYawDeg) {
  // Force pig to stay in front of the player camera so it is always visible.
  float yawRadPlayer = playerYawDeg * Mth::DEGRAD;
  float desiredX = playerX + Mth::sin(yawRadPlayer) * 3.0f + Mth::cos(yawRadPlayer) * 0.8f;
  float desiredZ = playerZ + Mth::cos(yawRadPlayer) * 3.0f - Mth::sin(yawRadPlayer) * 0.8f;

  pig->x = desiredX;
  pig->z = desiredZ;

  float gy = findGroundY(level, pig->x, pig->z);
  float pyFeet = playerY - 1.62f;
  pig->y = (gy > pyFeet) ? gy : pyFeet;

  pig->vx = pig->vy = pig->vz = 0.0f;
  pig->moveSpeed = 0.0f;
  pig->bodyYawDeg = wrapYaw(playerYawDeg + 180.0f); // face player
  pig->walkAnim += dt * 2.0f;

  // Idle look motion similar to passive mob head scan.
  pig->lookTimer -= dt;
  if (pig->lookTimer <= 0.0f) {
    float dx = playerX - pig->x;
    float dz = playerZ - pig->z;
    float wantHead = atan2f(dx, dz) / Mth::DEGRAD;
    if ((dx * dx + dz * dz) < 9.0f) {
      pig->headYawDeg = wrapYaw(wantHead - pig->bodyYawDeg);
      pig->headYawDeg = clampf(pig->headYawDeg, -35.0f, 35.0f);
      pig->headPitchDeg = clampf((playerY - (pig->y + 0.7f)) * -10.0f, -20.0f, 20.0f);
    } else {
      pig->headYawDeg *= 0.5f;
      pig->headPitchDeg *= 0.5f;
    }
    pig->lookTimer = 0.2f;
  }

}

void PigMob_Render(const PigMob *pig, TextureAtlas *terrainAtlas) {
  (void)terrainAtlas;
  if (!g_pigTex) return;

  sceGuTexMode(GU_PSM_8888, 0, 0, 0);
  sceGuTexImage(0, PIG_TEX_W, PIG_TEX_H, PIG_TEX_W, g_pigTex);
  sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
  sceGuTexFilter(GU_NEAREST, GU_NEAREST);
  sceGuTexWrap(GU_CLAMP, GU_CLAMP);
  sceGuEnable(GU_TEXTURE_2D);
  sceGuDisable(GU_CULL_FACE);
  sceGuDisable(GU_ALPHA_TEST);

  sceGumMatrixMode(GU_MODEL);
  sceGumPushMatrix();

  ScePspFVector3 pos = {pig->x, pig->y, pig->z};
  sceGumTranslate(&pos);

  ScePspFVector3 ry = {0.0f, pig->bodyYawDeg * Mth::DEGRAD, 0.0f};
  sceGumRotateXYZ(&ry);

  // Debug safety mesh: always-visible pig body core (solid pink) in case texturing fails.
  sceGuDisable(GU_TEXTURE_2D);
  emitSolidBox(-5.f / 16.f, 6.f / 16.f, -8.f / 16.f, 5.f / 16.f, 14.f / 16.f,
               8.f / 16.f, rgb(255, 128, 180));
  sceGuEnable(GU_TEXTURE_2D);

  // Body from original model: addBox(10x16x8) then rotate X by 90 degrees.
  sceGumPushMatrix();
  ScePspFVector3 bp = {0.0f, 12.f / 16.f, 2.f / 16.f};
  sceGumTranslate(&bp);
  ScePspFVector3 brot = {90.0f * Mth::DEGRAD, 0.0f, 0.0f};
  sceGumRotateXYZ(&brot);
  emitCuboid(-5.f / 16.f, -10.f / 16.f, -4.f / 16.f, 5.f / 16.f, 6.f / 16.f,
             4.f / 16.f, 28, 8, 10, 16, 8, rgb(255, 255, 255));
  sceGumPopMatrix();

  // Head transform (pivot around neck).
  sceGumPushMatrix();
  ScePspFVector3 hp = {0.0f, 8.f / 16.f, -6.f / 16.f};
  sceGumTranslate(&hp);
  ScePspFVector3 hrot = {pig->headPitchDeg * Mth::DEGRAD,
                         pig->headYawDeg * Mth::DEGRAD, 0.0f};
  sceGumRotateXYZ(&hrot);

  emitCuboid(-4.f / 16.f, -4.f / 16.f, -4.f / 16.f, 4.f / 16.f, 4.f / 16.f,
             4.f / 16.f, 0, 0, 8, 8, 8, rgb(255, 255, 255));
  emitCuboid(-2.f / 16.f, -1.f / 16.f, -5.f / 16.f, 2.f / 16.f, 2.f / 16.f,
             -4.f / 16.f, 16, 16, 4, 3, 1, rgb(255, 255, 255));
  sceGumPopMatrix();

  float leg = Mth::cos(pig->walkAnim) * 0.85f;
  struct LegDef {
    float px, pz, phase;
  } legs[4] = {{-3.f / 16.f, 7.f / 16.f, leg}, {3.f / 16.f, 7.f / 16.f, -leg},
               {-3.f / 16.f, -5.f / 16.f, -leg}, {3.f / 16.f, -5.f / 16.f, leg}};

  for (int i = 0; i < 4; ++i) {
    sceGumPushMatrix();
    ScePspFVector3 lp = {legs[i].px, 6.f / 16.f, legs[i].pz};
    sceGumTranslate(&lp);
    ScePspFVector3 lrot = {legs[i].phase, 0.0f, 0.0f};
    sceGumRotateXYZ(&lrot);
    emitCuboid(-2.f / 16.f, -6.f / 16.f, -2.f / 16.f, 2.f / 16.f, 0.f,
               2.f / 16.f, 0, 16, 4, 6, 4, rgb(255, 255, 255));
    sceGumPopMatrix();
  }

  sceGumPopMatrix();

  // Restore default render state for next frame pass.
  sceGuEnable(GU_CULL_FACE);
  sceGuEnable(GU_ALPHA_TEST);
}

void PigMob_Shutdown() {
  if (g_pigTex) {
    free(g_pigTex);
    g_pigTex = nullptr;
  }
}
