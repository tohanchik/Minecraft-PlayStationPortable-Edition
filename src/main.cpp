// MinecraftPSP - main.cpp
// PSP Entry point, basic game loop

#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspkernel.h>
#include <psppower.h>

#include "input/PSPInput.h"
#include "render/BlockHighlight.h"
#include "render/ChunkRenderer.h"
#include "render/CloudRenderer.h"
#include "world/Level.h"
#include "world/AABB.h"
#include "render/PSPRenderer.h"
#include "render/SkyRenderer.h"
#include "render/TextureAtlas.h"
#include "world/Blocks.h"
#include "world/Mth.h"
#include "world/Random.h"
#include "world/Raycast.h"
#include <math.h>

// PSP module metadata
PSP_MODULE_INFO("MinecraftPSP", PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024); // Use all available RAM minus 1MB for the kernel

// Exit callback (HOME button)
int exit_callback(int arg1, int arg2, void *common) {
  sceKernelExitGame();
  return 0;
}

int callback_thread(SceSize args, void *argp) {
  int cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
  sceKernelRegisterExitCallback(cbid);
  sceKernelSleepThreadCB();
  return 0;
}

void setup_callbacks() {
  int thid = sceKernelCreateThread("update_thread", callback_thread, 0x11,
                                   0xFA0, PSP_THREAD_ATTR_USER, NULL);
  if (thid >= 0)
    sceKernelStartThread(thid, 0, NULL);
}

// Player state
struct PlayerState {
  float x, y, z;          // position
  float yaw, pitch;       // camera rotation (degrees)
  float velY;             // vertical velocity (gravity)
  bool onGround;
  bool isFlying;          // creative flight active
  float jumpDoubleTapTimer; // countdown for double-tap detection
};

// Global state
static PlayerState g_player;
static Level *g_level = nullptr;
static SkyRenderer *g_skyRenderer = nullptr;
static CloudRenderer *g_cloudRenderer = nullptr;
static ChunkRenderer *g_chunkRenderer = nullptr;
static TextureAtlas *g_atlas = nullptr;
static RayHit g_hitResult;       // Block the player is currently looking at
static uint8_t g_heldBlock = BLOCK_COBBLESTONE; // Block to place
static bool g_inventoryOpen = false;
static int g_hotbarSel = 0;
static int g_inventorySel = 0;
static int g_inventoryScroll = 0;
static bool g_hotbarAssignMode = false;
static int g_pendingInventoryItem = -1;
static uint8_t g_hotbar[9] = {
  BLOCK_COBBLESTONE, BLOCK_STONE, BLOCK_DIRT, BLOCK_WOOD_PLANK, BLOCK_GLASS,
  BLOCK_SAND, BLOCK_LOG, BLOCK_LEAVES, BLOCK_WATER_STILL
};
static const uint8_t g_inventoryItems[] = {
  BLOCK_STONE, BLOCK_GRASS, BLOCK_DIRT, BLOCK_COBBLESTONE,
  BLOCK_WOOD_PLANK, BLOCK_SAND, BLOCK_GRAVEL, BLOCK_LOG,
  BLOCK_LEAVES, BLOCK_GLASS, BLOCK_SANDSTONE, BLOCK_WOOL,
  BLOCK_GOLD_BLOCK, BLOCK_IRON_BLOCK, BLOCK_BRICK, BLOCK_BOOKSHELF,
  BLOCK_MOSSY_COBBLE, BLOCK_OBSIDIAN, BLOCK_GLOWSTONE, BLOCK_PUMPKIN,
  BLOCK_FLOWER, BLOCK_ROSE, BLOCK_SAPLING, BLOCK_TALLGRASS, BLOCK_WATER_STILL
};

struct HudColVert {
  uint32_t color;
  float x, y, z;
};

struct HudTexVert {
  float u, v;
  float x, y, z;
};

static inline void hudDrawRect(float x, float y, float w, float h, uint32_t abgr) {
  sceGuDisable(GU_TEXTURE_2D);
  HudColVert *v = (HudColVert *)sceGuGetMemory(2 * sizeof(HudColVert));
  v[0].color = abgr; v[0].x = x;     v[0].y = y;     v[0].z = 0.0f;
  v[1].color = abgr; v[1].x = x + w; v[1].y = y + h; v[1].z = 0.0f;
  sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, 0, v);
  sceGuEnable(GU_TEXTURE_2D);
}

static inline void hudDrawTile(TextureAtlas *atlas, int tx, int ty, float x, float y, float size) {
  if (!atlas) return;
  atlas->bind();
  // Force neutral vertex color so HUD icons are not tinted by previous draws.
  sceGuColor(0xFFFFFFFF);
  sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
  sceGuTexFilter(GU_NEAREST, GU_NEAREST);
  HudTexVert *v = (HudTexVert *)sceGuGetMemory(2 * sizeof(HudTexVert));
  // 2D sprite path on PSP expects texel-space UVs.
  float u0 = (float)(tx * 16) + 0.5f;
  float v0 = (float)(ty * 16) + 0.5f;
  float us = 15.0f;
  float vs = 15.0f;
  v[0].u = u0;      v[0].v = v0;      v[0].x = x;        v[0].y = y;        v[0].z = 0.0f;
  v[1].u = u0 + us; v[1].v = v0 + vs; v[1].x = x + size; v[1].y = y + size; v[1].z = 0.0f;
  sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, 0, v);
}

static inline void hudGetIconTile(uint8_t id, int &tx, int &ty) {
  tx = g_blockUV[id].top_x;
  ty = g_blockUV[id].top_y;
  if (tx == 0 && ty == 0 && id != BLOCK_GRASS) {
    tx = g_blockUV[id].side_x;
    ty = g_blockUV[id].side_y;
    if (tx == 0 && ty == 0) {
      tx = 1; // stone fallback
      ty = 0;
    }
  }
}

static void drawHotbarHUD() {
  const float slot = 24.0f;
  const float pad = 3.0f;
  const float totalW = 9 * slot + 8 * pad;
  const float startX = (480.0f - totalW) * 0.5f;
  const float y = 272.0f - slot - 8.0f;

  for (int i = 0; i < 9; ++i) {
    float sx = startX + i * (slot + pad);
    bool selected = (i == g_hotbarSel && (!g_inventoryOpen || g_hotbarAssignMode));
    hudDrawRect(sx - 1, y - 1, slot + 2, slot + 2, selected ? 0xD0FFFFFF : 0x90303030);
    hudDrawRect(sx, y, slot, slot, 0x90000000);
    uint8_t id = g_hotbar[i];
    int tx, ty;
    hudGetIconTile(id, tx, ty);
    hudDrawTile(g_atlas, tx, ty, sx + 3, y + 3, slot - 6);
  }

  if (g_inventoryOpen) {
    const int invCount = (int)(sizeof(g_inventoryItems) / sizeof(g_inventoryItems[0]));
    const int cols = 6;
    const int rows = 4;
    const float cell = 30.0f;
    const float panelW = cols * cell + 10.0f;
    const float panelH = rows * cell + 10.0f;
    float panelX = (480.0f - panelW) * 0.5f;
    float panelY = (272.0f - panelH) * 0.30f;
    hudDrawRect(panelX - 3, panelY - 3, panelW + 6, panelH + 6, 0xC0303030);
    hudDrawRect(panelX, panelY, panelW, panelH, 0x70000000);

    int start = g_inventoryScroll * cols;
    for (int r = 0; r < rows; ++r) {
      for (int c = 0; c < cols; ++c) {
        int idx = start + r * cols + c;
        float sx = panelX + 5 + c * cell;
        float sy = panelY + 5 + r * cell;
        hudDrawRect(sx, sy, cell - 3, cell - 3, 0x90303030);
        if (idx >= invCount) continue;
        bool selected = (idx == g_inventorySel && !g_hotbarAssignMode);
        if (selected) hudDrawRect(sx - 1, sy - 1, cell - 1, cell - 1, 0xD0FFFFFF);
        uint8_t id = g_inventoryItems[idx];
        int tx, ty;
        hudGetIconTile(id, tx, ty);
        hudDrawTile(g_atlas, tx, ty, sx + 3, sy + 3, cell - 9);
      }
    }

    if (g_pendingInventoryItem >= 0) {
      float px = panelX + panelW * 0.5f - 14.0f;
      float py = panelY - 22.0f;
      hudDrawRect(px - 2, py - 2, 28, 28, 0xD0FFFFFF);
      uint8_t id = g_inventoryItems[g_pendingInventoryItem];
      int tx, ty;
      hudGetIconTile(id, tx, ty);
      hudDrawTile(g_atlas, tx, ty, px, py, 24);
    }

    // Scroll indicator
    int maxScroll = (invCount + cols - 1) / cols - rows;
    if (maxScroll < 0) maxScroll = 0;
    if (maxScroll > 0) {
      float t = (float)g_inventoryScroll / (float)maxScroll;
      hudDrawRect(panelX + panelW + 6, panelY, 4, panelH, 0x70303030);
      hudDrawRect(panelX + panelW + 6, panelY + t * (panelH - 32), 4, 32, 0xD0FFFFFF);
    }
  }
}

static inline bool isWaterId(uint8_t id) {
  return id == BLOCK_WATER_STILL || id == BLOCK_WATER_FLOW;
}

// Initialization
static bool game_init() {
  // Overclock PSP to max for performance
  scePowerSetClockFrequency(333, 333, 166);

  // Init block tables
  Blocks_Init();

  // Init sin/cos lookup table
  Mth::init();

  // Init PSP renderer (sceGu)
  if (!PSPRenderer_Init())
    return false;

  // Load terrain.png from MS0:/PSP/GAME/MinecraftPSP/res/
  g_atlas = new TextureAtlas();
  if (!g_atlas->load("res/terrain.png"))
    return false;

  g_level = new Level();
  g_skyRenderer = new SkyRenderer(g_level);
  g_cloudRenderer = new CloudRenderer(g_level);

  // Init chunk renderer
  g_chunkRenderer = new ChunkRenderer(g_atlas);
  g_chunkRenderer->setLevel(g_level);

  // Generate a test world
  Random rng(12345LL);
  g_level->generate(&rng);

  // Player start position
  g_player.x = 8.0f;
  g_player.y = 65.0f;
  g_player.z = 8.0f;
  g_player.yaw = 0.0f;
  g_player.pitch = 0.0f;
  g_player.velY = 0.0f;
  g_player.onGround = false;
  g_player.isFlying = false;
  g_player.jumpDoubleTapTimer = 0.0f;
  g_heldBlock = g_hotbar[g_hotbarSel];
  return true;
}

// Game loop update
static void game_update(float dt) {
  PSPInput_Update();
  if (g_level) {
    g_level->setSimulationFocus((int)floorf(g_player.x), (int)floorf(g_player.z), 24);
    g_level->tick();
  }

  bool inWater = false;
  {
    int fx = (int)floorf(g_player.x);
    int fz = (int)floorf(g_player.z);
    int bodyY = (int)floorf(g_player.y + 0.1f);
    inWater = isWaterId(g_level->getBlock(fx, bodyY, fz));
  }

  float baseMoveSpeed = g_player.isFlying ? 10.0f : 5.0f;
  if (inWater && !g_player.isFlying) baseMoveSpeed *= 0.45f;
  float moveSpeed = baseMoveSpeed * dt;
  float lookSpeed = 120.0f * dt;

  // Rotation with right stick (Face Buttons)
  if (!g_inventoryOpen) {
    float lx = PSPInput_StickX(1);
    float ly = PSPInput_StickY(1);
    g_player.yaw += lx * lookSpeed;
    g_player.pitch += ly * lookSpeed;
    g_player.pitch = Mth::clamp(g_player.pitch, -89.0f, 89.0f);
  }

  // Movement with left stick (Analog)
  float fx = -PSPInput_StickX(0);
  float fz = -PSPInput_StickY(0);

  float yawRad = g_player.yaw * Mth::DEGRAD;

  float dx = (fx * Mth::cos(yawRad) + fz * Mth::sin(yawRad)) * moveSpeed;
  float dz = (-fx * Mth::sin(yawRad) + fz * Mth::cos(yawRad)) * moveSpeed;

  const float R = 0.3f;   // 4J: setSize(0.6, 1.8)
  const float H = 1.8f;   // 4J: player bounding box height

  // Vertical movement
  float dy = 0.0f;
  if (g_player.isFlying) {
    float flySpeed = 10.0f * dt;
    if (PSPInput_IsHeld(PSP_CTRL_SELECT))
      dy = flySpeed;  // Ascend
    if (PSPInput_IsHeld(PSP_CTRL_DOWN))
      dy = -flySpeed;  // Descend
    g_player.velY = 0.0f;
  } else {
    if (inWater) {
      g_player.velY -= 6.0f * dt;
      if (PSPInput_IsHeld(PSP_CTRL_SELECT)) {
        g_player.velY += 9.0f * dt;
      }
      g_player.velY *= 0.85f;
    } else {
      g_player.velY -= 20.0f * dt;
    }
    dy = g_player.velY * dt;
  }

  // Collision
  AABB player_aabb(g_player.x - R, g_player.y, g_player.z - R,
                   g_player.x + R, g_player.y + H, g_player.z + R);

  AABB* expanded = player_aabb.expand(dx, dy, dz);
  std::vector<AABB> cubes = g_level->getCubes(*expanded);
  delete expanded;

  float dy_org = dy;
  for (auto& cube : cubes) dy = cube.clipYCollide(&player_aabb, dy);
  player_aabb.move(0, dy, 0);

  for (auto& cube : cubes) dx = cube.clipXCollide(&player_aabb, dx);
  player_aabb.move(dx, 0, 0);

  for (auto& cube : cubes) dz = cube.clipZCollide(&player_aabb, dz);
  player_aabb.move(0, 0, dz);

  g_player.onGround = (dy_org != dy && dy_org < 0.0f);
  if (g_player.onGround || dy_org != dy) {
    g_player.velY = 0.0f;
  }
  if (inWater && !g_player.isFlying) {
    g_player.velY *= 0.8f;
  }

  g_player.x = (player_aabb.x0 + player_aabb.x1) / 2.0f;
  g_player.y = player_aabb.y0;
  g_player.z = (player_aabb.z0 + player_aabb.z1) / 2.0f;

  // Enforce world bounds natively
  const float WORLD_MAX_X = (float)(WORLD_CHUNKS_X * CHUNK_SIZE_X - 1);
  const float WORLD_MAX_Z = (float)(WORLD_CHUNKS_Z * CHUNK_SIZE_Z - 1);
  if (g_player.x < 0.5f) g_player.x = 0.5f;
  if (g_player.x > WORLD_MAX_X) g_player.x = WORLD_MAX_X;
  if (g_player.z < 0.5f) g_player.z = 0.5f;
  if (g_player.z > WORLD_MAX_Z) g_player.z = WORLD_MAX_Z;

  // Controls: Jump/Fly
  static const float DOUBLE_TAP_WINDOW = 0.35f;
  if (g_player.jumpDoubleTapTimer > 0.0f)
    g_player.jumpDoubleTapTimer -= dt;

  if (PSPInput_JustPressed(PSP_CTRL_SELECT)) {
    if (g_player.jumpDoubleTapTimer > 0.0f) {
      g_player.isFlying = !g_player.isFlying;
      g_player.velY = 0.0f;
      g_player.jumpDoubleTapTimer = 0.0f;
    } else {
      if (!g_player.isFlying && g_player.onGround) {
        g_player.velY = 6.5f;
        g_player.onGround = false;
      } else if (!g_player.isFlying && inWater) {
        g_player.velY = 2.5f;
      }
      g_player.jumpDoubleTapTimer = DOUBLE_TAP_WINDOW;
    }
  }

  // Raycast block target
  {
    float eyeX = g_player.x;
    float eyeY = g_player.y + 1.6f;
    float eyeZ = g_player.z;
    float pitchRad = g_player.pitch * Mth::DEGRAD;
    float dirX = Mth::sin(yawRad) * Mth::cos(pitchRad);
    float dirY = Mth::sin(pitchRad);
    float dirZ = Mth::cos(yawRad) * Mth::cos(pitchRad);
    g_hitResult = raycast(g_level, eyeX, eyeY, eyeZ, dirX, dirY, dirZ, 5.0f);
  }

  // Block action cooldown
  static float breakCooldown = 0.0f;
  if (breakCooldown > 0.0f) breakCooldown -= dt;

  // Inventory/hotbar controls
  if ((PSPInput_IsHeld(PSP_CTRL_LTRIGGER) && PSPInput_JustPressed(PSP_CTRL_RTRIGGER)) ||
      (PSPInput_IsHeld(PSP_CTRL_RTRIGGER) && PSPInput_JustPressed(PSP_CTRL_LTRIGGER))) {
    g_inventoryOpen = true;
    g_hotbarAssignMode = false;
    g_pendingInventoryItem = -1;
  }
  if (g_inventoryOpen && PSPInput_JustPressed(PSP_CTRL_CIRCLE)) {
    g_inventoryOpen = false;
    g_hotbarAssignMode = false;
    g_pendingInventoryItem = -1;
  }
  if (PSPInput_JustPressed(PSP_CTRL_RIGHT)) {
    if (g_hotbarAssignMode || !g_inventoryOpen) g_hotbarSel = (g_hotbarSel + 1) % 9;
    else {
      int invCount = (int)(sizeof(g_inventoryItems) / sizeof(g_inventoryItems[0]));
      g_inventorySel = (g_inventorySel + 1) % invCount;
    }
  }
  if (PSPInput_JustPressed(PSP_CTRL_LEFT)) {
    if (g_hotbarAssignMode || !g_inventoryOpen) g_hotbarSel = (g_hotbarSel + 8) % 9;
    else {
      int invCount = (int)(sizeof(g_inventoryItems) / sizeof(g_inventoryItems[0]));
      g_inventorySel = (g_inventorySel + invCount - 1) % invCount;
    }
  }
  if (g_inventoryOpen) {
    const int invCount = (int)(sizeof(g_inventoryItems) / sizeof(g_inventoryItems[0]));
    const int cols = 6;
    const int rows = 4;
    const int maxScroll = ((invCount + cols - 1) / cols > rows) ? ((invCount + cols - 1) / cols - rows) : 0;
    if (PSPInput_IsHeld(PSP_CTRL_TRIANGLE)) {
      if (PSPInput_JustPressed(PSP_CTRL_UP) && g_inventoryScroll > 0) g_inventoryScroll--;
      if (PSPInput_JustPressed(PSP_CTRL_DOWN) && g_inventoryScroll < maxScroll) g_inventoryScroll++;
    } else if (!g_hotbarAssignMode) {
      if (PSPInput_JustPressed(PSP_CTRL_UP)) g_inventorySel = (g_inventorySel - cols + invCount) % invCount;
      if (PSPInput_JustPressed(PSP_CTRL_DOWN)) g_inventorySel = (g_inventorySel + cols) % invCount;
    }
    if (PSPInput_JustPressed(PSP_CTRL_CROSS)) {
      if (!g_hotbarAssignMode) {
        g_pendingInventoryItem = g_inventorySel;
        g_hotbarAssignMode = true;
      } else if (g_pendingInventoryItem >= 0) {
        g_hotbar[g_hotbarSel] = g_inventoryItems[g_pendingInventoryItem];
        g_hotbarAssignMode = false;
        g_pendingInventoryItem = -1;
      }
    }
  }
  g_heldBlock = g_hotbar[g_hotbarSel];

  // Block breaking
  bool doBreak = false;
  if (PSPInput_IsHeld(PSP_CTRL_LTRIGGER) && breakCooldown <= 0.0f) {
    doBreak = true;
    breakCooldown = 0.15f;
  }

  if (doBreak && g_hitResult.hit) {
    uint8_t oldBlock = g_level->getBlock(g_hitResult.x, g_hitResult.y, g_hitResult.z);
    if (oldBlock != BLOCK_BEDROCK) {
      int bx = g_hitResult.x, by = g_hitResult.y, bz = g_hitResult.z;
      g_level->setBlock(bx, by, bz, BLOCK_AIR);
      g_level->markDirty(bx, by, bz);

      // Cascading plant break (if we broke the soil, the plant pops off)
      uint8_t topId = g_level->getBlock(bx, by + 1, bz);
      if (topId != BLOCK_AIR && !g_blockProps[topId].isSolid() && !g_blockProps[topId].isLiquid()) {
          g_level->setBlock(bx, by + 1, bz, BLOCK_AIR);
          g_level->markDirty(bx, by + 1, bz);
      }
    }
  }

  // Place block
  if (!g_inventoryOpen && PSPInput_JustPressed(PSP_CTRL_UP) && g_hitResult.hit) {
    int px = g_hitResult.nx;
    int py = g_hitResult.ny;
    int pz = g_hitResult.nz;

    // If we click on a plant, replace the plant directly instead of placing adjacent
    uint8_t hitId = g_level->getBlock(g_hitResult.x, g_hitResult.y, g_hitResult.z);
    if (hitId != BLOCK_AIR && !g_blockProps[hitId].isSolid() && !g_blockProps[hitId].isLiquid()) {
      px = g_hitResult.x;
      py = g_hitResult.y;
      pz = g_hitResult.z;
    }

    // If we are placing a plant, check if the block below is valid soil (grass/dirt/farmland)
    bool canPlace = true;
    if (g_heldBlock == BLOCK_SAPLING || g_heldBlock == BLOCK_TALLGRASS || g_heldBlock == BLOCK_FLOWER || 
        g_heldBlock == BLOCK_ROSE || g_heldBlock == BLOCK_MUSHROOM_BROWN || g_heldBlock == BLOCK_MUSHROOM_RED) {
      uint8_t floorId = g_level->getBlock(px, py - 1, pz);
      if (floorId != BLOCK_GRASS && floorId != BLOCK_DIRT && floorId != BLOCK_FARMLAND) {
        canPlace = false;
      }
    }

    // Don't place if it would overlap with the player
    int playerMinX = (int)floorf(g_player.x - R);
    int playerMaxX = (int)floorf(g_player.x + R);
    int playerMinY = (int)floorf(g_player.y);
    int playerMaxY = (int)floorf(g_player.y + H);
    int playerMinZ = (int)floorf(g_player.z - R);
    int playerMaxZ = (int)floorf(g_player.z + R);

    bool overlaps = (px >= playerMinX && px <= playerMaxX &&
                     py >= playerMinY && py <= playerMaxY &&
                     pz >= playerMinZ && pz <= playerMaxZ);

    uint8_t targetBlock = g_level->getBlock(px, py, pz);
    bool canReplaceTarget = (targetBlock == BLOCK_AIR || (!g_blockProps[targetBlock].isSolid() && !g_blockProps[targetBlock].isLiquid()));

    if (canPlace && !overlaps && canReplaceTarget) {
      g_level->setBlock(px, py, pz, g_heldBlock);
      g_level->markDirty(px, py, pz);
    }
  }

}

static void game_render() {
  float _tod = g_level->getTimeOfDay();

  // Camera setup
  ScePspFVector3 camPos = {g_player.x, g_player.y + 1.62f, g_player.z}; // 4J: heightOffset = 1.62
  
  float yawRad = g_player.yaw * Mth::DEGRAD;
  float pitchRad = g_player.pitch * Mth::DEGRAD;

  ScePspFVector3 lookDir = {
      Mth::sin(yawRad) * Mth::cos(pitchRad), // X
      Mth::sin(pitchRad),                    // Y
      Mth::cos(yawRad) * Mth::cos(pitchRad)  // Z
  };

  ScePspFVector3 lookAt = {camPos.x + lookDir.x, camPos.y + lookDir.y,
                           camPos.z + lookDir.z};

  // Compute fog color
  uint32_t clearColor = 0xFF000000;
  if (g_skyRenderer) {
      clearColor = g_skyRenderer->getFogColor(_tod, lookDir);
  }
  {
    int wx = (int)floorf(camPos.x);
    int wy = (int)floorf(camPos.y);
    int wz = (int)floorf(camPos.z);
    if (isWaterId(g_level->getBlock(wx, wy, wz))) {
      // Underwater tint/fog approximation.
      clearColor = 0xFF4A1C06;
    }
  }

  PSPRenderer_BeginFrame(clearColor);

  PSPRenderer_SetCamera(&camPos, &lookAt);

  if (g_skyRenderer)
    g_skyRenderer->renderSky(g_player.x, g_player.y, g_player.z, lookDir);

  // Render chunks
  g_chunkRenderer->render(g_player.x, g_player.y, g_player.z);

  // Render block highlight wireframe
  if (g_hitResult.hit) {
    BlockHighlight_Draw(g_hitResult.x, g_hitResult.y, g_hitResult.z, g_hitResult.id);
  }

  if (g_cloudRenderer)
    g_cloudRenderer->renderClouds(g_player.x, g_player.y, g_player.z, 0.0f);

  // 2D HUD pass
  sceGuDisable(GU_DEPTH_TEST);
  sceGuDisable(GU_CULL_FACE);
  sceGuEnable(GU_BLEND);
  sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
  drawHotbarHUD();
  sceGuDisable(GU_BLEND);
  sceGuEnable(GU_CULL_FACE);
  sceGuEnable(GU_DEPTH_TEST);

  PSPRenderer_EndFrame();
}

// Main entry point
int main(int argc, char *argv[]) {
  setup_callbacks();

  if (!game_init()) {
    pspDebugScreenInit();
    pspDebugScreenPrintf("Init error!\n");
    sceKernelSleepThread();
    return 1;
  }

  uint64_t lastTime = sceKernelGetSystemTimeWide();

  while (true) {
    uint64_t now = sceKernelGetSystemTimeWide();
    float dt = (float)(now - lastTime) / 1000000.0f; // microseconds -> seconds
    if (dt > 0.05f)
      dt = 0.05f; // cap at 20 FPS min
    lastTime = now;

    game_update(dt);
    game_render();
  }

  return 0;
}
