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
static bool g_inventoryOpen = false;
static const uint8_t PLACEABLE[] = {
  BLOCK_STONE, BLOCK_GRASS, BLOCK_DIRT, BLOCK_COBBLESTONE, BLOCK_WOOD_PLANK, BLOCK_SAND,
  BLOCK_GRAVEL, BLOCK_LOG, BLOCK_LEAVES, BLOCK_GLASS, BLOCK_SANDSTONE, BLOCK_WOOL,
  BLOCK_GOLD_BLOCK, BLOCK_IRON_BLOCK, BLOCK_BRICK, BLOCK_BOOKSHELF, BLOCK_MOSSY_COBBLE, BLOCK_OBSIDIAN,
  BLOCK_GLOWSTONE, BLOCK_PUMPKIN, BLOCK_FLOWER, BLOCK_ROSE, BLOCK_SAPLING, BLOCK_TALLGRASS,

  BLOCK_BEDROCK, BLOCK_GOLD_ORE, BLOCK_IRON_ORE, BLOCK_COAL_ORE, BLOCK_LAPIS_ORE, BLOCK_DIAMOND_ORE,
  BLOCK_DIAMOND_BLOCK, BLOCK_CRAFTING_TABLE, BLOCK_FURNACE, BLOCK_CHEST, BLOCK_CLAY, BLOCK_SNOW_BLOCK,
  BLOCK_ICE, BLOCK_NETHERRACK, BLOCK_SOULSAND, BLOCK_CACTUS, BLOCK_FARMLAND, BLOCK_WATER_STILL,
  BLOCK_LAVA_STILL, BLOCK_REEDS, BLOCK_TORCH, BLOCK_TNT, BLOCK_COBBLE_STAIR, BLOCK_WOOD_STAIR
};
static const int NUM_PLACEABLE = sizeof(PLACEABLE) / sizeof(PLACEABLE[0]);
static const int INVENTORY_COLS = 6;
static const int INVENTORY_ROWS = 4;
static const int INVENTORY_VISIBLE_SLOTS = INVENTORY_COLS * INVENTORY_ROWS;
static uint8_t g_hotbar[9] = {0}; // 0 = empty slot
static int g_hotbarSelected = 0;
static int g_inventoryCursor = 0;
static int g_inventoryPage = 0;
static bool g_inventoryDragging = false;
static uint8_t g_dragBlock = BLOCK_AIR;

struct HudVertex {
  uint32_t color;
  int16_t x, y, z;
};

struct HudTexVertex {
  int16_t u, v;
  uint32_t color;
  int16_t x, y, z;
};

static void drawHudRect(int x0, int y0, int x1, int y1, uint32_t color) {
  HudVertex *v = (HudVertex *)sceGuGetMemory(2 * sizeof(HudVertex));
  v[0].color = color; v[0].x = (int16_t)x0; v[0].y = (int16_t)y0; v[0].z = 0;
  v[1].color = color; v[1].x = (int16_t)x1; v[1].y = (int16_t)y1; v[1].z = 0;
  sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                 2, 0, v);
}

static void drawHudLine(int x0, int y0, int x1, int y1, uint32_t color) {
  HudVertex *v = (HudVertex *)sceGuGetMemory(2 * sizeof(HudVertex));
  v[0].color = color; v[0].x = (int16_t)x0; v[0].y = (int16_t)y0; v[0].z = 0;
  v[1].color = color; v[1].x = (int16_t)x1; v[1].y = (int16_t)y1; v[1].z = 0;
  sceGuDrawArray(GU_LINES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                 2, 0, v);
}

static void drawHudBlockIcon(int x, int y, int size, uint8_t blockId, uint8_t alpha = 0xFF) {
  if (!g_atlas || blockId == BLOCK_AIR) return;

  const BlockUV &uv = g_blockUV[blockId];
  int16_t u0 = (int16_t)(uv.top_x * 16 + 1);
  int16_t v0 = (int16_t)(uv.top_y * 16 + 1);
  int16_t u1 = (int16_t)(u0 + 14);
  int16_t v1 = (int16_t)(v0 + 14);

  HudTexVertex *v = (HudTexVertex *)sceGuGetMemory(2 * sizeof(HudTexVertex));
  v[0].u = u0; v[0].v = v0; v[0].color = ((uint32_t)alpha << 24) | 0x00FFFFFF; v[0].x = (int16_t)x; v[0].y = (int16_t)y; v[0].z = 0;
  v[1].u = u1; v[1].v = v1; v[1].color = ((uint32_t)alpha << 24) | 0x00FFFFFF; v[1].x = (int16_t)(x + size); v[1].y = (int16_t)(y + size); v[1].z = 0;
  sceGuDrawArray(GU_SPRITES,
                 GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                 2, 0, v);
}

static void game_render_hud() {
  const int screenW = 480;
  const int screenH = 272;
  const int centerX = screenW / 2;
  const int centerY = screenH / 2;

  sceGuDisable(GU_DEPTH_TEST);
  sceGuDisable(GU_TEXTURE_2D);
  sceGuDisable(GU_CULL_FACE);
  sceGuEnable(GU_BLEND);
  sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);

  // Crosshair (voxelworld-style center reticle)
  drawHudLine(centerX - 6, centerY, centerX + 6, centerY, 0xD0FFFFFF);
  drawHudLine(centerX, centerY - 6, centerX, centerY + 6, 0xD0FFFFFF);

  // Simple hotbar strip (empty by default; filled from inventory)
  const int slotW = 18;
  const int slotH = 18;
  const int slotGap = 4;
  const int visibleSlots = 9;
  const int barW = visibleSlots * slotW + (visibleSlots - 1) * slotGap;
  const int barX = (screenW - barW) / 2;
  const int barY = screenH - 26;

  for (int i = 0; i < visibleSlots; ++i) {
    int x = barX + i * (slotW + slotGap);
    int y = barY;
    bool isSel = (i == g_hotbarSelected);
    bool hasItem = (g_hotbar[i] != BLOCK_AIR);
    uint32_t bg = hasItem ? 0xA03A3A3A : 0x50101010;
    if (isSel && !g_inventoryOpen) bg = 0xB0707070;
    if (isSel && g_inventoryDragging) bg = 0xC0909090;
    drawHudRect(x, y, x + slotW, y + slotH, bg);
    drawHudLine(x, y, x + slotW, y, 0xC0FFFFFF);
    drawHudLine(x, y + slotH, x + slotW, y + slotH, 0xC0FFFFFF);
    drawHudLine(x, y, x, y + slotH, 0xC0FFFFFF);
    drawHudLine(x + slotW, y, x + slotW, y + slotH, 0xC0FFFFFF);
  }

  // Draw textured block icons in hotbar
  sceGuEnable(GU_TEXTURE_2D);
  g_atlas->bind();
  sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
  for (int i = 0; i < visibleSlots; ++i) {
    if (g_hotbar[i] == BLOCK_AIR) continue;
    int x = barX + i * (slotW + slotGap) + 1;
    int y = barY + 1;
    drawHudBlockIcon(x, y, slotW - 2, g_hotbar[i], 0xFF);
  }
  sceGuDisable(GU_TEXTURE_2D);

  if (g_inventoryOpen) {
    const int cols = INVENTORY_COLS;
    const int invSlots = INVENTORY_VISIBLE_SLOTS;
    const int rows = INVENTORY_ROWS;
    const int invSlotW = 22;
    const int invSlotH = 22;
    const int invGap = 4;
    const int invW = cols * invSlotW + (cols - 1) * invGap + 12;
    const int invH = rows * invSlotH + (rows - 1) * invGap + 12;
    const int invX = (screenW - invW) / 2;
    const int invY = 24;

    drawHudRect(invX, invY, invX + invW, invY + invH, 0xB0101010);
    drawHudLine(invX, invY, invX + invW, invY, 0xD0FFFFFF);
    drawHudLine(invX, invY + invH, invX + invW, invY + invH, 0xD0FFFFFF);
    drawHudLine(invX, invY, invX, invY + invH, 0xD0FFFFFF);
    drawHudLine(invX + invW, invY, invX + invW, invY + invH, 0xD0FFFFFF);

    for (int i = 0; i < invSlots; ++i) {
      int cx = i % cols;
      int cy = i / cols;
      int x = invX + 6 + cx * (invSlotW + invGap);
      int y = invY + 6 + cy * (invSlotH + invGap);
      int itemIndex = g_inventoryPage * INVENTORY_VISIBLE_SLOTS + i;
      bool hasItem = (itemIndex < NUM_PLACEABLE);
      bool isCursor = (i == g_inventoryCursor);
      uint32_t fill = isCursor ? 0xB0606060 : (hasItem ? 0x90202020 : 0x70181818);
      drawHudRect(x, y, x + invSlotW, y + invSlotH, fill);
      drawHudLine(x, y, x + invSlotW, y, 0xC0E0E0FF);
      drawHudLine(x, y + invSlotH, x + invSlotW, y + invSlotH, 0xC0E0E0FF);
      drawHudLine(x, y, x, y + invSlotH, 0xC0E0E0FF);
      drawHudLine(x + invSlotW, y, x + invSlotW, y + invSlotH, 0xC0E0E0FF);
    }

    // Draw textured inventory icons
    sceGuEnable(GU_TEXTURE_2D);
    g_atlas->bind();
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    for (int i = 0; i < invSlots; ++i) {
      int itemIndex = g_inventoryPage * INVENTORY_VISIBLE_SLOTS + i;
      if (itemIndex >= NUM_PLACEABLE) continue;
      int cx = i % cols;
      int cy = i / cols;
      int x = invX + 6 + cx * (invSlotW + invGap) + 2;
      int y = invY + 6 + cy * (invSlotH + invGap) + 2;
      drawHudBlockIcon(x, y, invSlotH - 4, PLACEABLE[itemIndex], 0xFF);
    }
    sceGuDisable(GU_TEXTURE_2D);

    if (g_inventoryDragging && g_dragBlock != BLOCK_AIR) {
      // Drag preview in the top-right corner of inventory
      int px = invX + invW - 26;
      int py = invY + 6;
      drawHudRect(px, py, px + 18, py + 18, 0xD0404040);
      sceGuEnable(GU_TEXTURE_2D);
      g_atlas->bind();
      sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
      drawHudBlockIcon(px + 1, py + 1, 16, g_dragBlock, 0xFF);
      sceGuDisable(GU_TEXTURE_2D);
      drawHudLine(px, py, px + 18, py, 0xE0FFFFFF);
      drawHudLine(px, py + 18, px + 18, py + 18, 0xE0FFFFFF);
      drawHudLine(px, py, px, py + 18, 0xE0FFFFFF);
      drawHudLine(px + 18, py, px + 18, py + 18, 0xE0FFFFFF);
    }
  }

  sceGuDisable(GU_BLEND);
  sceGuEnable(GU_TEXTURE_2D);
  if (g_atlas) {
    g_atlas->bind();
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
  }
  sceGuEnable(GU_CULL_FACE);
  sceGuEnable(GU_DEPTH_TEST);
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

  return true;
}

// Game loop update
static void game_update(float dt) {
  PSPInput_Update();

  static bool lrComboWasHeld = false;
  bool lrComboHeld = PSPInput_IsHeld(PSP_CTRL_LTRIGGER) && PSPInput_IsHeld(PSP_CTRL_RTRIGGER);
  if (lrComboHeld && !lrComboWasHeld) {
    g_inventoryOpen = !g_inventoryOpen;
    if (!g_inventoryOpen) {
      g_inventoryDragging = false;
      g_dragBlock = BLOCK_AIR;
    }
  }
  lrComboWasHeld = lrComboHeld;

  if (g_inventoryOpen) {
    if (PSPInput_JustPressed(PSP_CTRL_CIRCLE)) {
      g_inventoryOpen = false;
      g_inventoryDragging = false;
      g_dragBlock = BLOCK_AIR;
      return;
    }

    const int cols = INVENTORY_COLS;
    const int rows = INVENTORY_ROWS;
    const int maxPages = (NUM_PLACEABLE + INVENTORY_VISIBLE_SLOTS - 1) / INVENTORY_VISIBLE_SLOTS;
    int cx = g_inventoryCursor % cols;
    int cy = g_inventoryCursor / cols;

    if (!g_inventoryDragging) {
      // Page inventory: hold Triangle + D-pad Up/Down
      if (PSPInput_IsHeld(PSP_CTRL_TRIANGLE) && PSPInput_JustPressed(PSP_CTRL_UP)) {
        g_inventoryPage = (g_inventoryPage - 1 + maxPages) % maxPages;
      } else if (PSPInput_IsHeld(PSP_CTRL_TRIANGLE) && PSPInput_JustPressed(PSP_CTRL_DOWN)) {
        g_inventoryPage = (g_inventoryPage + 1) % maxPages;
      } else {
        if (PSPInput_JustPressed(PSP_CTRL_LEFT)) cx--;
        if (PSPInput_JustPressed(PSP_CTRL_RIGHT)) cx++;
        if (PSPInput_JustPressed(PSP_CTRL_UP)) cy--;
        if (PSPInput_JustPressed(PSP_CTRL_DOWN)) cy++;
      }

      if (cx < 0) cx = cols - 1;
      if (cx >= cols) cx = 0;
      if (cy < 0) cy = rows - 1;
      if (cy >= rows) cy = 0;

      int next = cy * cols + cx;
      int maxIndexOnPage = NUM_PLACEABLE - g_inventoryPage * INVENTORY_VISIBLE_SLOTS - 1;
      if (maxIndexOnPage < 0) maxIndexOnPage = 0;
      if (maxIndexOnPage >= INVENTORY_VISIBLE_SLOTS) maxIndexOnPage = INVENTORY_VISIBLE_SLOTS - 1;
      if (next > maxIndexOnPage) next = maxIndexOnPage;
      g_inventoryCursor = next;
    }

    // Move hotbar focus with dpad while dragging selected block from inventory
    if (g_inventoryDragging) {
      if (PSPInput_JustPressed(PSP_CTRL_LEFT))
        g_hotbarSelected = (g_hotbarSelected - 1 + 9) % 9;
      if (PSPInput_JustPressed(PSP_CTRL_RIGHT))
        g_hotbarSelected = (g_hotbarSelected + 1) % 9;
    }

    // Select block from inventory with X
    if (PSPInput_JustPressed(PSP_CTRL_CROSS)) {
      if (!g_inventoryDragging) {
        int itemIndex = g_inventoryPage * INVENTORY_VISIBLE_SLOTS + g_inventoryCursor;
        if (itemIndex < NUM_PLACEABLE) {
          g_dragBlock = PLACEABLE[itemIndex];
          g_inventoryDragging = true;
        }
      } else {
        g_hotbar[g_hotbarSelected] = g_dragBlock;
        g_inventoryDragging = false;
        g_dragBlock = BLOCK_AIR;
      }
    }

    return;
  }

  if (g_level) {
    g_level->tick();
  }

  float moveSpeed = (g_player.isFlying ? 10.0f : 5.0f) * dt;
  float lookSpeed = 120.0f * dt;

  // Rotation with right stick (Face Buttons)
  float lx = PSPInput_StickX(1);
  float ly = PSPInput_StickY(1);
  g_player.yaw += lx * lookSpeed;
  g_player.pitch += ly * lookSpeed;
  g_player.pitch = Mth::clamp(g_player.pitch, -89.0f, 89.0f);

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
    g_player.velY -= 20.0f * dt;
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
          g_chunkRenderer->rebuildChunkNow(bx >> 4, bz >> 4, (by + 1) >> 4);
      }

      // Rebuild the central chunk synchronously
      int cx = bx >> 4, cz = bz >> 4, sy = by >> 4;
      g_chunkRenderer->rebuildChunkNow(cx, cz, sy);

      // Rebuild neighbor chunks
      if ((bx & 0xF) == 0  && cx > 0)
        g_chunkRenderer->rebuildChunkNow(cx - 1, cz, sy);
      if ((bx & 0xF) == 15 && cx < WORLD_CHUNKS_X - 1)
        g_chunkRenderer->rebuildChunkNow(cx + 1, cz, sy);
      if ((bz & 0xF) == 0  && cz > 0)
        g_chunkRenderer->rebuildChunkNow(cx, cz - 1, sy);
      if ((bz & 0xF) == 15 && cz < WORLD_CHUNKS_Z - 1)
        g_chunkRenderer->rebuildChunkNow(cx, cz + 1, sy);
      if ((by & 0xF) == 0  && sy > 0)
        g_chunkRenderer->rebuildChunkNow(cx, cz, sy - 1);
      if ((by & 0xF) == 15 && sy < 3)
        g_chunkRenderer->rebuildChunkNow(cx, cz, sy + 1);
    }
  }

  // Place block
  if (PSPInput_JustPressed(PSP_CTRL_RTRIGGER) && g_hitResult.hit) {
    uint8_t heldBlock = g_hotbar[g_hotbarSelected];
    if (heldBlock != BLOCK_AIR) {
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
      if (heldBlock == BLOCK_SAPLING || heldBlock == BLOCK_TALLGRASS || heldBlock == BLOCK_FLOWER ||
          heldBlock == BLOCK_ROSE || heldBlock == BLOCK_MUSHROOM_BROWN || heldBlock == BLOCK_MUSHROOM_RED) {
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
        g_level->setBlock(px, py, pz, heldBlock);
        g_level->markDirty(px, py, pz);

        // Immediately rebuild the central subchunk
        int cx = px >> 4, cz = pz >> 4, sy = py >> 4;
        g_chunkRenderer->rebuildChunkNow(cx, cz, sy);

        // Synchronously rebuild neighbor chunks at chunk boundaries
        if ((px & 0xF) == 0  && cx > 0)
          g_chunkRenderer->rebuildChunkNow(cx - 1, cz, sy);
        if ((px & 0xF) == 15 && cx < WORLD_CHUNKS_X - 1)
          g_chunkRenderer->rebuildChunkNow(cx + 1, cz, sy);
        if ((pz & 0xF) == 0  && cz > 0)
          g_chunkRenderer->rebuildChunkNow(cx, cz - 1, sy);
        if ((pz & 0xF) == 15 && cz < WORLD_CHUNKS_Z - 1)
          g_chunkRenderer->rebuildChunkNow(cx, cz + 1, sy);
        if ((py & 0xF) == 0  && sy > 0)
          g_chunkRenderer->rebuildChunkNow(cx, cz, sy - 1);
        if ((py & 0xF) == 15 && sy < 3)
          g_chunkRenderer->rebuildChunkNow(cx, cz, sy + 1);
      }
    }
  }

  // Cycle active hotbar slot
  if (PSPInput_JustPressed(PSP_CTRL_LEFT))
    g_hotbarSelected = (g_hotbarSelected - 1 + 9) % 9;
  if (PSPInput_JustPressed(PSP_CTRL_RIGHT))
    g_hotbarSelected = (g_hotbarSelected + 1) % 9;
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

  game_render_hud();

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
