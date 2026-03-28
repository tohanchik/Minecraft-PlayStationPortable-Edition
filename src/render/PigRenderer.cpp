#include "PigRenderer.h"

#include "entity/Pig.h"

#include <pspgu.h>
#include <pspgum.h>
#include <math.h>

namespace {
struct ColVert {
  unsigned int color;
  float x, y, z;
};

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

  out[n++] = {sideColor, v100.x, v100.y, v100.z}; out[n++] = {sideColor, v110.x, v110.y, v110.z}; out[n++] = {sideColor, v111.x, v111.y, v111.z};
  out[n++] = {sideColor, v100.x, v100.y, v100.z}; out[n++] = {sideColor, v111.x, v111.y, v111.z}; out[n++] = {sideColor, v101.x, v101.y, v101.z};

  out[n++] = {sideColor, v000.x, v000.y, v000.z}; out[n++] = {sideColor, v011.x, v011.y, v011.z}; out[n++] = {sideColor, v010.x, v010.y, v010.z};
  out[n++] = {sideColor, v000.x, v000.y, v000.z}; out[n++] = {sideColor, v001.x, v001.y, v001.z}; out[n++] = {sideColor, v011.x, v011.y, v011.z};

  out[n++] = {sideColor, v001.x, v001.y, v001.z}; out[n++] = {sideColor, v111.x, v111.y, v111.z}; out[n++] = {sideColor, v011.x, v011.y, v011.z};
  out[n++] = {sideColor, v001.x, v001.y, v001.z}; out[n++] = {sideColor, v101.x, v101.y, v101.z}; out[n++] = {sideColor, v111.x, v111.y, v111.z};

  out[n++] = {sideColor, v000.x, v000.y, v000.z}; out[n++] = {sideColor, v010.x, v010.y, v010.z}; out[n++] = {sideColor, v110.x, v110.y, v110.z};
  out[n++] = {sideColor, v000.x, v000.y, v000.z}; out[n++] = {sideColor, v110.x, v110.y, v110.z}; out[n++] = {sideColor, v100.x, v100.y, v100.z};

  out[n++] = {topColor, v010.x, v010.y, v010.z}; out[n++] = {topColor, v011.x, v011.y, v011.z}; out[n++] = {topColor, v111.x, v111.y, v111.z};
  out[n++] = {topColor, v010.x, v010.y, v010.z}; out[n++] = {topColor, v111.x, v111.y, v111.z}; out[n++] = {topColor, v110.x, v110.y, v110.z};

  out[n++] = {bottomColor, v000.x, v000.y, v000.z}; out[n++] = {bottomColor, v101.x, v101.y, v101.z}; out[n++] = {bottomColor, v001.x, v001.y, v001.z};
  out[n++] = {bottomColor, v000.x, v000.y, v000.z}; out[n++] = {bottomColor, v100.x, v100.y, v100.z}; out[n++] = {bottomColor, v101.x, v101.y, v101.z};
}
}

void PigRenderer_Init() {}

void PigRenderer_Render(const Pig &pig) {
  if (!pig.active) return;

  sceGuDisable(GU_BLEND);
  sceGuDisable(GU_ALPHA_TEST);
  sceGuDisable(GU_CULL_FACE);
  sceGuEnable(GU_DEPTH_TEST);
  sceGuDisable(GU_TEXTURE_2D);

  sceGumMatrixMode(GU_MODEL);
  sceGumPushMatrix();
  sceGumLoadIdentity();

  const float bob = 0.03f * sinf(pig.animTime * 3.0f);
  ScePspFVector3 pos = {pig.x, pig.y + bob + 0.65f, pig.z};
  sceGumTranslate(&pos);

  sceGumRotateY(-(pig.yaw * (3.14159265f / 180.0f)));

  ColVert* verts = (ColVert*)sceGuGetMemory(sizeof(ColVert) * 216);
  int n = 0;

  const unsigned int side = 0xFFB469FFu;
  const unsigned int top =  0xFFD296FFu;
  const unsigned int bot =  0xFF783CC8u;

  emitBox(verts, n, -0.45f, -0.30f, -0.25f, 0.45f, 0.30f, 0.25f, side, top, bot);
  emitBox(verts, n, -0.20f, -0.12f, 0.25f, 0.20f, 0.18f, 0.58f, side, top, bot);
  emitBox(verts, n, -0.38f, -0.80f, -0.18f, -0.24f, -0.30f, -0.04f, side, top, bot);
  emitBox(verts, n,  0.24f, -0.80f, -0.18f,  0.38f, -0.30f, -0.04f, side, top, bot);
  emitBox(verts, n, -0.38f, -0.80f,  0.04f, -0.24f, -0.30f,  0.18f, side, top, bot);
  emitBox(verts, n,  0.24f, -0.80f,  0.04f,  0.38f, -0.30f,  0.18f, side, top, bot);

  sceGuColor(0xFFFFFFFF);
  sceGuDrawArray(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, n, 0, verts);

  sceGumPopMatrix();
  sceGumMatrixMode(GU_MODEL);

  sceGuEnable(GU_TEXTURE_2D);
  sceGuEnable(GU_ALPHA_TEST);
  sceGuEnable(GU_CULL_FACE);
}

void PigRenderer_Shutdown() {}
