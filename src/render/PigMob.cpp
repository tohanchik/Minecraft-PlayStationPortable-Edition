#include "PigMob.h"

#include <math.h>
#include <stdlib.h>

#include <pspgu.h>
#include <pspgum.h>
#include <pspiofilemgr.h>

namespace {
struct PigState {
  float x, y, z;
  float yawDeg;
  float animTime;
  bool active;
  bool hasPigTexture;
};

struct ColVert {
  unsigned int color;
  float x, y, z;
};

static PigState g_pig = {0};

static bool fileExists(const char* path) {
  SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
  if (fd < 0) return false;
  sceIoClose(fd);
  return true;
}

static void spawnNearPlayer(float playerX, float playerY, float playerZ, float playerYawDeg) {
  // Spawn at player's X/Z and lift the pig above ground/player body.
  g_pig.x = playerX;
  g_pig.z = playerZ;
  g_pig.y = playerY + 2.0f;
  g_pig.yawDeg = playerYawDeg;
}

static void emitBox(ColVert* out, int& n,
                    float x0, float y0, float z0,
                    float x1, float y1, float z1,
                    unsigned int sideColor,
                    unsigned int topColor,
                    unsigned int bottomColor) {
  const ColVert v000 = {bottomColor, x0, y0, z0};
  const ColVert v001 = {bottomColor, x0, y0, z1};
  const ColVert v010 = {topColor,    x0, y1, z0};
  const ColVert v011 = {topColor,    x0, y1, z1};
  const ColVert v100 = {bottomColor, x1, y0, z0};
  const ColVert v101 = {bottomColor, x1, y0, z1};
  const ColVert v110 = {topColor,    x1, y1, z0};
  const ColVert v111 = {topColor,    x1, y1, z1};

  // +X
  out[n++] = {sideColor, v100.x, v100.y, v100.z}; out[n++] = {sideColor, v110.x, v110.y, v110.z}; out[n++] = {sideColor, v111.x, v111.y, v111.z};
  out[n++] = {sideColor, v100.x, v100.y, v100.z}; out[n++] = {sideColor, v111.x, v111.y, v111.z}; out[n++] = {sideColor, v101.x, v101.y, v101.z};
  // -X
  out[n++] = {sideColor, v000.x, v000.y, v000.z}; out[n++] = {sideColor, v011.x, v011.y, v011.z}; out[n++] = {sideColor, v010.x, v010.y, v010.z};
  out[n++] = {sideColor, v000.x, v000.y, v000.z}; out[n++] = {sideColor, v001.x, v001.y, v001.z}; out[n++] = {sideColor, v011.x, v011.y, v011.z};
  // +Z
  out[n++] = {sideColor, v001.x, v001.y, v001.z}; out[n++] = {sideColor, v111.x, v111.y, v111.z}; out[n++] = {sideColor, v011.x, v011.y, v011.z};
  out[n++] = {sideColor, v001.x, v001.y, v001.z}; out[n++] = {sideColor, v101.x, v101.y, v101.z}; out[n++] = {sideColor, v111.x, v111.y, v111.z};
  // -Z
  out[n++] = {sideColor, v000.x, v000.y, v000.z}; out[n++] = {sideColor, v010.x, v010.y, v010.z}; out[n++] = {sideColor, v110.x, v110.y, v110.z};
  out[n++] = {sideColor, v000.x, v000.y, v000.z}; out[n++] = {sideColor, v110.x, v110.y, v110.z}; out[n++] = {sideColor, v100.x, v100.y, v100.z};
  // +Y
  out[n++] = {topColor, v010.x, v010.y, v010.z}; out[n++] = {topColor, v011.x, v011.y, v011.z}; out[n++] = {topColor, v111.x, v111.y, v111.z};
  out[n++] = {topColor, v010.x, v010.y, v010.z}; out[n++] = {topColor, v111.x, v111.y, v111.z}; out[n++] = {topColor, v110.x, v110.y, v110.z};
  // -Y
  out[n++] = {bottomColor, v000.x, v000.y, v000.z}; out[n++] = {bottomColor, v101.x, v101.y, v101.z}; out[n++] = {bottomColor, v001.x, v001.y, v001.z};
  out[n++] = {bottomColor, v000.x, v000.y, v000.z}; out[n++] = {bottomColor, v100.x, v100.y, v100.z}; out[n++] = {bottomColor, v101.x, v101.y, v101.z};
}
} // namespace

bool PigMob_Init(float playerX, float playerY, float playerZ, float playerYawDeg) {
  g_pig.active = true;
  g_pig.animTime = 0.0f;
  g_pig.hasPigTexture = fileExists("res/mob/pig.png");
  spawnNearPlayer(playerX, playerY, playerZ, playerYawDeg);
  return true;
}

void PigMob_Update(float dt, float playerX, float playerY, float playerZ, float playerYawDeg) {
  if (!g_pig.active) return;

  g_pig.animTime += dt;
  if (g_pig.animTime > 10000.0f) g_pig.animTime = 0.0f;

  // Keep the pig close to the player so it is always visible during quick tests.
  const float dx = g_pig.x - playerX;
  const float dz = g_pig.z - playerZ;
  const float dist2 = dx * dx + dz * dz;
  if (dist2 > 64.0f) {
    spawnNearPlayer(playerX, playerY, playerZ, playerYawDeg);
  }

  // Simple idle sway.
  g_pig.yawDeg += dt * 20.0f;
  if (g_pig.yawDeg > 360.0f) g_pig.yawDeg -= 360.0f;
}

void PigMob_Render() {
  if (!g_pig.active) return;

  // Enforce an opaque render path (no alpha/cull surprises).
  sceGuDisable(GU_BLEND);
  sceGuDisable(GU_ALPHA_TEST);
  sceGuDisable(GU_CULL_FACE);
  sceGuEnable(GU_DEPTH_TEST);
  sceGuDisable(GU_TEXTURE_2D); // Solid debug mesh fallback is always visible.

  sceGumMatrixMode(GU_MODEL);
  sceGumPushMatrix();
  sceGumLoadIdentity();

  const float bob = 0.03f * sinf(g_pig.animTime * 3.0f);
  ScePspFVector3 pos = {g_pig.x, g_pig.y + bob + 0.65f, g_pig.z};
  sceGumTranslate(&pos);

  ScePspFVector3 rotY = {0.0f, -(g_pig.yawDeg * (3.14159265f / 180.0f)), 0.0f};
  sceGumRotateY(rotY.y);

  // Body + head as two cuboids. Colors are fully opaque.
  ColVert* verts = (ColVert*)sceGuGetMemory(sizeof(ColVert) * 72);
  int n = 0;

  // ABGR colors: fallback is intentionally bright pink for high visibility.
  unsigned int side = g_pig.hasPigTexture ? 0xFF8FA8D6u : 0xFFB469FFu;
  unsigned int top =  g_pig.hasPigTexture ? 0xFFA8C0EAu : 0xFFD296FFu;
  unsigned int bot =  g_pig.hasPigTexture ? 0xFF6A7DA5u : 0xFF783CC8u;

  // Body
  emitBox(verts, n, -0.45f, -0.30f, -0.25f, 0.45f, 0.30f, 0.25f, side, top, bot);
  // Head
  emitBox(verts, n, -0.20f, -0.12f, 0.25f, 0.20f, 0.18f, 0.58f, side, top, bot);

  sceGuColor(0xFFFFFFFF);
  sceGuDrawArray(GU_TRIANGLES,
                 GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D,
                 n, 0, verts);

  sceGumPopMatrix();
  sceGumMatrixMode(GU_MODEL);

  // Restore defaults expected by world renderers.
  sceGuEnable(GU_TEXTURE_2D);
  sceGuEnable(GU_ALPHA_TEST);
  sceGuEnable(GU_CULL_FACE);
}

void PigMob_Shutdown() {
  g_pig = {};
}
