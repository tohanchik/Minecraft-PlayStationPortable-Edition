// ChunkRenderer.cpp
#include "ChunkRenderer.h"
#include "../math/Frustum.h"
#include "PSPRenderer.h"
#include "Tesselator.h"
#include "TileRenderer.h"
#include <malloc.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspkernel.h>
#include <string.h>
#include <algorithm>

#define MAX_VERTS_PER_SUB_CHUNK 8000

static CraftPSPVertex g_opaqueBuf[SUBCHUNK_COUNT][MAX_VERTS_PER_SUB_CHUNK];
static CraftPSPVertex g_transBuf[SUBCHUNK_COUNT][MAX_VERTS_PER_SUB_CHUNK];
static CraftPSPVertex g_transFancyBuf[SUBCHUNK_COUNT][MAX_VERTS_PER_SUB_CHUNK];
static CraftPSPVertex g_emitBuf[SUBCHUNK_COUNT][MAX_VERTS_PER_SUB_CHUNK];

static bool uploadMeshSafe(CraftPSPVertex *&dst, int &dstCount, int &dstCap,
                           int newCount, int growPad, CraftPSPVertex *src) {
  if (newCount <= 0) {
    if (dst) {
      free(dst);
      dst = nullptr;
    }
    dstCap = 0;
    dstCount = 0;
    return true;
  }

  CraftPSPVertex *target = dst;
  int targetCap = dstCap;
  if (newCount > targetCap) {
    int nextCap = newCount + growPad;
    CraftPSPVertex *newBuf = (CraftPSPVertex *)memalign(16, nextCap * sizeof(CraftPSPVertex));
    if (!newBuf) {
      // Low-memory fallback: keep existing buffer and upload a truncated mesh
      // rather than leaving the chunk permanently invisible/dirty.
      if (target && targetCap > 0) {
        int clamped = targetCap;
        memcpy(target, src, clamped * sizeof(CraftPSPVertex));
        sceKernelDcacheWritebackInvalidateRange(target,
                                                clamped * sizeof(CraftPSPVertex));
        dst = target;
        dstCap = targetCap;
        dstCount = clamped;
        return true;
      }
      // No existing mesh yet: try progressively smaller allocations and upload
      // a reduced mesh so the chunk still renders instead of disappearing.
      int fallbackCap = (newCount / 2) - ((newCount / 2) % 3);
      while (!newBuf && fallbackCap >= 256) {
        newBuf = (CraftPSPVertex *)memalign(16, fallbackCap * sizeof(CraftPSPVertex));
        if (!newBuf) {
          fallbackCap /= 2;
          fallbackCap -= (fallbackCap % 3);
        }
      }
      if (newBuf && fallbackCap > 0) {
        memcpy(newBuf, src, fallbackCap * sizeof(CraftPSPVertex));
        sceKernelDcacheWritebackInvalidateRange(newBuf,
                                                fallbackCap * sizeof(CraftPSPVertex));
        dst = newBuf;
        dstCap = fallbackCap;
        dstCount = fallbackCap;
        return true;
      }
      // Still out of memory: report failure so caller keeps the subchunk dirty
      // and retries compilation/upload later.
      dstCount = 0;
      return false;
    }
    if (dst) free(dst);
    target = newBuf;
    targetCap = nextCap;
  }

  memcpy(target, src, newCount * sizeof(CraftPSPVertex));
  sceKernelDcacheWritebackInvalidateRange(target, newCount * sizeof(CraftPSPVertex));

  // If geometry density dropped significantly, shrink the resident mesh buffer
  // to reduce long-run fragmentation and peak RAM usage on PSP.
  if (target && targetCap > newCount * 2) {
    int shrinkCap = newCount + growPad;
    CraftPSPVertex *shrunk =
        (CraftPSPVertex *)memalign(16, shrinkCap * sizeof(CraftPSPVertex));
    if (shrunk) {
      memcpy(shrunk, target, newCount * sizeof(CraftPSPVertex));
      sceKernelDcacheWritebackInvalidateRange(shrunk,
                                              newCount * sizeof(CraftPSPVertex));
      if (target != dst) free(target);
      if (dst) free(dst);
      target = shrunk;
      targetCap = shrinkCap;
    }
  }

  dst = target;
  dstCap = targetCap;
  dstCount = newCount;
  return true;
}

static inline float decodeLight01(uint8_t l) {
  float v = 1.0f - (float)l / 15.0f;
  return (1.0f - v) / (v * 3.0f + 1.0f);
}

static inline uint32_t mulColor(uint32_t baseColor, float b) {
  uint8_t a = (baseColor >> 24) & 0xFF;
  uint8_t bb = (baseColor >> 16) & 0xFF;
  uint8_t g = (baseColor >> 8) & 0xFF;
  uint8_t r = baseColor & 0xFF;
  bb = (uint8_t)(bb * b);
  g = (uint8_t)(g * b);
  r = (uint8_t)(r * b);
  return (a << 24) | (bb << 16) | (g << 8) | r;
}

static inline bool isOpaqueCube(uint8_t id) {
  const BlockProps &bp = g_blockProps[id];
  return bp.isSolid() && !bp.isTransparent() && !bp.isLiquid();
}

static void emitGreedyOpaqueRuns(Level *level, Chunk *c, int sy, Tesselator &tess) {
  const int yStart = sy * 16;
  const int yEnd = yStart + 16;
  const float ts = 1.0f / 16.0f;
  const float eps = 0.125f / 256.0f;
  const float sunScale = level->getSunBrightness() * 0.85f + 0.15f;
  const int MAX_RUN = 2;

  auto faceBrightness = [&](int wx, int wy, int wz) -> float {
    uint8_t sl = (wy < 0 || wy >= CHUNK_SIZE_Y) ? 15 : level->getSkyLight(wx, wy, wz);
    uint8_t bl = (wy < 0 || wy >= CHUNK_SIZE_Y) ? 0 : level->getBlockLight(wx, wy, wz);
    float sky = decodeLight01(sl) * sunScale;
    float blk = decodeLight01(bl);
    return (blk > sky + 0.05f) ? blk : sky;
  };

  // TOP/BOTTOM runs (merge along X).
  for (int y = yStart; y < yEnd; ++y) {
    for (int lz = 0; lz < CHUNK_SIZE_Z; ++lz) {
      int x = 0;
      while (x < CHUNK_SIZE_X) {
        uint8_t id = c->blocks[x][lz][y];
        int wx = c->cx * CHUNK_SIZE_X + x;
        int wz = c->cz * CHUNK_SIZE_Z + lz;
        if (!isOpaqueCube(id)) { x++; continue; }
        bool topExposed = !g_blockProps[level->getBlock(wx, y + 1, wz)].isOpaque();
        if (!topExposed) { x++; continue; }
        int x0 = x;
        while (x + 1 < CHUNK_SIZE_X) {
          if ((x - x0 + 1) >= MAX_RUN) break;
          uint8_t nid = c->blocks[x + 1][lz][y];
          int nwx = c->cx * CHUNK_SIZE_X + (x + 1);
          if (nid != id) break;
          if (g_blockProps[level->getBlock(nwx, y + 1, wz)].isOpaque()) break;
          x++;
        }
        int x1 = x;
        const BlockUV &uv = g_blockUV[id];
        float u0 = uv.top_x * ts + eps, v0 = uv.top_y * ts + eps;
        float u1 = (uv.top_x + 1) * ts - eps, v1 = (uv.top_y + 1) * ts - eps;
        float b = faceBrightness(c->cx * CHUNK_SIZE_X + x0, y + 1, wz);
        uint32_t col = mulColor(0xFFFFFFFF, b);
        float fx0 = (float)(c->cx * CHUNK_SIZE_X + x0);
        float fx1 = (float)(c->cx * CHUNK_SIZE_X + x1 + 1);
        float fy = (float)y + 1.0f;
        float fz0 = (float)(c->cz * CHUNK_SIZE_Z + lz);
        float fz1 = fz0 + 1.0f;
        tess.addQuad(u0, v0, u1, v1, col, col, col, col,
                     fx0, fy, fz0, fx1, fy, fz0, fx0, fy, fz1, fx1, fy, fz1);
        x++;
      }
    }
  }

  for (int y = yStart; y < yEnd; ++y) {
    for (int lz = 0; lz < CHUNK_SIZE_Z; ++lz) {
      int x = 0;
      while (x < CHUNK_SIZE_X) {
        uint8_t id = c->blocks[x][lz][y];
        int wx = c->cx * CHUNK_SIZE_X + x;
        int wz = c->cz * CHUNK_SIZE_Z + lz;
        if (!isOpaqueCube(id)) { x++; continue; }
        bool botExposed = !g_blockProps[level->getBlock(wx, y - 1, wz)].isOpaque();
        if (!botExposed) { x++; continue; }
        int x0 = x;
        while (x + 1 < CHUNK_SIZE_X) {
          if ((x - x0 + 1) >= MAX_RUN) break;
          uint8_t nid = c->blocks[x + 1][lz][y];
          int nwx = c->cx * CHUNK_SIZE_X + (x + 1);
          if (nid != id) break;
          if (g_blockProps[level->getBlock(nwx, y - 1, wz)].isOpaque()) break;
          x++;
        }
        int x1 = x;
        const BlockUV &uv = g_blockUV[id];
        float u0 = uv.bot_x * ts + eps, v0 = uv.bot_y * ts + eps;
        float u1 = (uv.bot_x + 1) * ts - eps, v1 = (uv.bot_y + 1) * ts - eps;
        float b = faceBrightness(c->cx * CHUNK_SIZE_X + x0, y - 1, wz);
        uint32_t col = mulColor(0xFF999999, b);
        float fx0 = (float)(c->cx * CHUNK_SIZE_X + x0);
        float fx1 = (float)(c->cx * CHUNK_SIZE_X + x1 + 1);
        float fy = (float)y;
        float fz0 = (float)(c->cz * CHUNK_SIZE_Z + lz);
        float fz1 = fz0 + 1.0f;
        tess.addQuad(u0, v0, u1, v1, col, col, col, col,
                     fx0, fy, fz1, fx1, fy, fz1, fx0, fy, fz0, fx1, fy, fz0);
        x++;
      }
    }
  }

  // Z- faces (north), merge along X.
  for (int y = yStart; y < yEnd; ++y) {
    for (int lz = 0; lz < CHUNK_SIZE_Z; ++lz) {
      int x = 0;
      while (x < CHUNK_SIZE_X) {
        uint8_t id = c->blocks[x][lz][y];
        int wx = c->cx * CHUNK_SIZE_X + x;
        int wz = c->cz * CHUNK_SIZE_Z + lz;
        if (!isOpaqueCube(id)) { x++; continue; }
        bool exposed = !g_blockProps[level->getBlock(wx, y, wz - 1)].isOpaque();
        if (!exposed) { x++; continue; }
        int x0 = x;
        while (x + 1 < CHUNK_SIZE_X) {
          if ((x - x0 + 1) >= MAX_RUN) break;
          uint8_t nid = c->blocks[x + 1][lz][y];
          int nwx = c->cx * CHUNK_SIZE_X + (x + 1);
          if (nid != id) break;
          if (g_blockProps[level->getBlock(nwx, y, wz - 1)].isOpaque()) break;
          x++;
        }
        int x1 = x;
        const BlockUV &uv = g_blockUV[id];
        float u0 = uv.side_x * ts + eps, v0 = uv.side_y * ts + eps;
        float u1 = (uv.side_x + 1) * ts - eps, v1 = (uv.side_y + 1) * ts - eps;
        float b = faceBrightness(c->cx * CHUNK_SIZE_X + x0, y, wz - 1);
        uint32_t col = mulColor(0xFFCCCCCC, b);
        float fx0 = (float)(c->cx * CHUNK_SIZE_X + x0);
        float fx1 = (float)(c->cx * CHUNK_SIZE_X + x1 + 1);
        float fy0 = (float)y, fy1 = (float)y + 1.0f;
        float fz = (float)wz;
        tess.addQuad(u0, v0, u1, v1, col, col, col, col,
                     fx0, fy1, fz, fx1, fy1, fz, fx0, fy0, fz, fx1, fy0, fz);
        x++;
      }
    }
  }

  // Z+ faces (south), merge along X.
  for (int y = yStart; y < yEnd; ++y) {
    for (int lz = 0; lz < CHUNK_SIZE_Z; ++lz) {
      int x = 0;
      while (x < CHUNK_SIZE_X) {
        uint8_t id = c->blocks[x][lz][y];
        int wx = c->cx * CHUNK_SIZE_X + x;
        int wz = c->cz * CHUNK_SIZE_Z + lz;
        if (!isOpaqueCube(id)) { x++; continue; }
        bool exposed = !g_blockProps[level->getBlock(wx, y, wz + 1)].isOpaque();
        if (!exposed) { x++; continue; }
        int x0 = x;
        while (x + 1 < CHUNK_SIZE_X) {
          if ((x - x0 + 1) >= MAX_RUN) break;
          uint8_t nid = c->blocks[x + 1][lz][y];
          int nwx = c->cx * CHUNK_SIZE_X + (x + 1);
          if (nid != id) break;
          if (g_blockProps[level->getBlock(nwx, y, wz + 1)].isOpaque()) break;
          x++;
        }
        int x1 = x;
        const BlockUV &uv = g_blockUV[id];
        float u0 = uv.side_x * ts + eps, v0 = uv.side_y * ts + eps;
        float u1 = (uv.side_x + 1) * ts - eps, v1 = (uv.side_y + 1) * ts - eps;
        float b = faceBrightness(c->cx * CHUNK_SIZE_X + x0, y, wz + 1);
        uint32_t col = mulColor(0xFFCCCCCC, b);
        float fx0 = (float)(c->cx * CHUNK_SIZE_X + x0);
        float fx1 = (float)(c->cx * CHUNK_SIZE_X + x1 + 1);
        float fy0 = (float)y, fy1 = (float)y + 1.0f;
        float fz = (float)wz + 1.0f;
        tess.addQuad(u0, v0, u1, v1, col, col, col, col,
                     fx1, fy1, fz, fx0, fy1, fz, fx1, fy0, fz, fx0, fy0, fz);
        x++;
      }
    }
  }

  // X- faces (west), merge along Z.
  for (int y = yStart; y < yEnd; ++y) {
    for (int lx = 0; lx < CHUNK_SIZE_X; ++lx) {
      int z = 0;
      while (z < CHUNK_SIZE_Z) {
        uint8_t id = c->blocks[lx][z][y];
        int wx = c->cx * CHUNK_SIZE_X + lx;
        int wz = c->cz * CHUNK_SIZE_Z + z;
        if (!isOpaqueCube(id)) { z++; continue; }
        bool exposed = !g_blockProps[level->getBlock(wx - 1, y, wz)].isOpaque();
        if (!exposed) { z++; continue; }
        int z0 = z;
        while (z + 1 < CHUNK_SIZE_Z) {
          if ((z - z0 + 1) >= MAX_RUN) break;
          uint8_t nid = c->blocks[lx][z + 1][y];
          int nwz = c->cz * CHUNK_SIZE_Z + (z + 1);
          if (nid != id) break;
          if (g_blockProps[level->getBlock(wx - 1, y, nwz)].isOpaque()) break;
          z++;
        }
        int z1 = z;
        const BlockUV &uv = g_blockUV[id];
        float u0 = uv.side_x * ts + eps, v0 = uv.side_y * ts + eps;
        float u1 = (uv.side_x + 1) * ts - eps, v1 = (uv.side_y + 1) * ts - eps;
        float b = faceBrightness(wx - 1, y, c->cz * CHUNK_SIZE_Z + z0);
        uint32_t col = mulColor(0xFFCCCCCC, b);
        float fz0 = (float)(c->cz * CHUNK_SIZE_Z + z0);
        float fz1 = (float)(c->cz * CHUNK_SIZE_Z + z1 + 1);
        float fy0 = (float)y, fy1 = (float)y + 1.0f;
        float fx = (float)wx;
        tess.addQuad(u0, v0, u1, v1, col, col, col, col,
                     fx, fy1, fz1, fx, fy1, fz0, fx, fy0, fz1, fx, fy0, fz0);
        z++;
      }
    }
  }

  // X+ faces (east), merge along Z.
  for (int y = yStart; y < yEnd; ++y) {
    for (int lx = 0; lx < CHUNK_SIZE_X; ++lx) {
      int z = 0;
      while (z < CHUNK_SIZE_Z) {
        uint8_t id = c->blocks[lx][z][y];
        int wx = c->cx * CHUNK_SIZE_X + lx;
        int wz = c->cz * CHUNK_SIZE_Z + z;
        if (!isOpaqueCube(id)) { z++; continue; }
        bool exposed = !g_blockProps[level->getBlock(wx + 1, y, wz)].isOpaque();
        if (!exposed) { z++; continue; }
        int z0 = z;
        while (z + 1 < CHUNK_SIZE_Z) {
          if ((z - z0 + 1) >= MAX_RUN) break;
          uint8_t nid = c->blocks[lx][z + 1][y];
          int nwz = c->cz * CHUNK_SIZE_Z + (z + 1);
          if (nid != id) break;
          if (g_blockProps[level->getBlock(wx + 1, y, nwz)].isOpaque()) break;
          z++;
        }
        int z1 = z;
        const BlockUV &uv = g_blockUV[id];
        float u0 = uv.side_x * ts + eps, v0 = uv.side_y * ts + eps;
        float u1 = (uv.side_x + 1) * ts - eps, v1 = (uv.side_y + 1) * ts - eps;
        float b = faceBrightness(wx + 1, y, c->cz * CHUNK_SIZE_Z + z0);
        uint32_t col = mulColor(0xFFCCCCCC, b);
        float fz0 = (float)(c->cz * CHUNK_SIZE_Z + z0);
        float fz1 = (float)(c->cz * CHUNK_SIZE_Z + z1 + 1);
        float fy0 = (float)y, fy1 = (float)y + 1.0f;
        float fx = (float)wx + 1.0f;
        tess.addQuad(u0, v0, u1, v1, col, col, col, col,
                     fx, fy1, fz0, fx, fy1, fz1, fx, fy0, fz0, fx, fy0, fz1);
        z++;
      }
    }
  }
}

ChunkRenderer::ChunkRenderer(TextureAtlas *atlas)
    : m_level(nullptr), m_atlas(atlas), m_compileStep(0), m_compileChunk(nullptr), m_compileSy(-1) {}

ChunkRenderer::~ChunkRenderer() {}

void ChunkRenderer::setLevel(Level *level) { m_level = level; }

#include <psprtc.h>

void ChunkRenderer::processCompileQueue(float camX, float camY, float camZ) {
  if (!m_level)
    return;

  // Limit compile time
  uint64_t tickStart;
  sceRtcGetCurrentTick(&tickStart);
  uint32_t tickRes = sceRtcGetTickResolution();
  
  while (true) {
    uint64_t currentTick;
    sceRtcGetCurrentTick(&currentTick);
    float elapsedMs = (float)(currentTick - tickStart) / ((float)tickRes / 1000.0f);
    if (elapsedMs > 4.0f) {
      break;
    }

  if (m_compileStep == 0) {
    // Find closest dirty subchunk
    Chunk *closestDirty = nullptr;
    int closestDirtySy = -1;
    float closestDirtyDistSq = 9999999.0f;

    for (int cx = 0; cx < WORLD_CHUNKS_X; cx++) {
      for (int cz = 0; cz < WORLD_CHUNKS_Z; cz++) {
        Chunk *c = m_level->getChunk(cx, cz);
        if (c) {
          for (int sy = 0; sy < SUBCHUNK_COUNT; sy++) {
            if (c->dirty[sy]) {
              float chunkCenterX = c->cx * CHUNK_SIZE_X + CHUNK_SIZE_X / 2.0f;
              float chunkCenterZ = c->cz * CHUNK_SIZE_Z + CHUNK_SIZE_Z / 2.0f;
              float chunkCenterY = sy * 16 + 8.0f;
              float dx = chunkCenterX - camX;
              float dy = chunkCenterY - camY;
              float dz = chunkCenterZ - camZ;
              float distSq = dx * dx + dy * dy + dz * dz;
              if (distSq < closestDirtyDistSq) {
                closestDirtyDistSq = distSq;
                closestDirty = c;
                closestDirtySy = sy;
              }
            }
          }
        }
      }
    }

    if (closestDirty) {
      m_compileChunk = closestDirty;
      m_compileSy = closestDirtySy;
      m_compileStep = 1;
      m_opaqueTess.begin(g_opaqueBuf[m_compileSy], MAX_VERTS_PER_SUB_CHUNK);
      m_transTess.begin(g_transBuf[m_compileSy], MAX_VERTS_PER_SUB_CHUNK);
      m_transFancyTess.begin(g_transFancyBuf[m_compileSy], MAX_VERTS_PER_SUB_CHUNK);
      m_emitTess.begin(g_emitBuf[m_compileSy], MAX_VERTS_PER_SUB_CHUNK);
    }
  }

  if (m_compileStep == 1) {
    // Compile y-layers
    int yStart = m_compileSy * 16;
    int yEnd = yStart + 16;

    TileRenderer tileRenderer(m_level, &m_opaqueTess, &m_transTess, &m_transFancyTess, &m_emitTess);
    for (int lx = 0; lx < CHUNK_SIZE_X; lx++) {
      for (int lz = 0; lz < CHUNK_SIZE_Z; lz++) {
        for (int ly = yStart; ly < yEnd; ly++) {
          uint8_t id = m_compileChunk->blocks[lx][lz][ly];
          if (id == BLOCK_AIR)
            continue;

          const BlockProps &bp = g_blockProps[id];
          if (!bp.isSolid() && !bp.isTransparent() && !bp.isLiquid())
            continue;

          tileRenderer.tesselateBlockInWorld(id, lx, ly, lz, m_compileChunk->cx, m_compileChunk->cz);
        }
      }
    }
    
    // Upload immediately
    Chunk* c = m_compileChunk;
    int sy = m_compileSy;

    int newOpaque = m_opaqueTess.end();
    int newTrans = m_transTess.end();
    int newFancy = m_transFancyTess.end();
    int newEmit = m_emitTess.end();
    bool ok = true;
    ok &= uploadMeshSafe(c->opaqueVertices[sy], c->opaqueTriCount[sy], c->opaqueCapacity[sy], newOpaque, 250, g_opaqueBuf[sy]);
    ok &= uploadMeshSafe(c->transVertices[sy], c->transTriCount[sy], c->transCapacity[sy], newTrans, 250, g_transBuf[sy]);
    ok &= uploadMeshSafe(c->transFancyVertices[sy], c->transFancyTriCount[sy], c->transFancyCapacity[sy], newFancy, 250, g_transFancyBuf[sy]);
    ok &= uploadMeshSafe(c->emitVertices[sy], c->emitTriCount[sy], c->emitCapacity[sy], newEmit, 100, g_emitBuf[sy]);

    c->dirty[sy] = !ok;

    m_compileStep = 0;
    m_compileChunk = nullptr;
  }
  
  // If there are no chunks to compile, exit the while loop early
  if (m_compileStep == 0) {
      break;
  }
  
  } // end while(true)
}

// Finish tessellation and upload vertex data
static void flushSubChunk(Chunk *c, int sy,
                          Tesselator &opT, Tesselator &trT, Tesselator &tfT, Tesselator &emT) {
  int newOpaque = opT.end();
  int newTrans = trT.end();
  int newFancy = tfT.end();
  int newEmit = emT.end();
  bool ok = true;
  ok &= uploadMeshSafe(c->opaqueVertices[sy], c->opaqueTriCount[sy], c->opaqueCapacity[sy], newOpaque, 250, g_opaqueBuf[sy]);
  ok &= uploadMeshSafe(c->transVertices[sy], c->transTriCount[sy], c->transCapacity[sy], newTrans, 250, g_transBuf[sy]);
  ok &= uploadMeshSafe(c->transFancyVertices[sy], c->transFancyTriCount[sy], c->transFancyCapacity[sy], newFancy, 250, g_transFancyBuf[sy]);
  ok &= uploadMeshSafe(c->emitVertices[sy], c->emitTriCount[sy], c->emitCapacity[sy], newEmit, 100, g_emitBuf[sy]);
  c->dirty[sy] = !ok;
}

void ChunkRenderer::rebuildChunkNow(int cx, int cz, int sy) {
  if (!m_level) return;
  Chunk *c = m_level->getChunk(cx, cz);
  if (!c || sy < 0 || sy >= SUBCHUNK_COUNT) return;

  // Tessellate the full subchunk in one call
  m_opaqueTess.begin(g_opaqueBuf[sy], MAX_VERTS_PER_SUB_CHUNK);
  m_transTess.begin(g_transBuf[sy], MAX_VERTS_PER_SUB_CHUNK);
  m_transFancyTess.begin(g_transFancyBuf[sy], MAX_VERTS_PER_SUB_CHUNK);
  m_emitTess.begin(g_emitBuf[sy], MAX_VERTS_PER_SUB_CHUNK);

  TileRenderer tileRenderer(m_level, &m_opaqueTess, &m_transTess, &m_transFancyTess, &m_emitTess);
  int yStart = sy * 16;
  int yEnd   = yStart + 16;
  for (int lx = 0; lx < CHUNK_SIZE_X; lx++) {
    for (int lz = 0; lz < CHUNK_SIZE_Z; lz++) {
      for (int ly = yStart; ly < yEnd; ly++) {
        uint8_t id = c->blocks[lx][lz][ly];
        if (id == BLOCK_AIR) continue;
        const BlockProps &bp = g_blockProps[id];
        if (!bp.isSolid() && !bp.isTransparent() && !bp.isLiquid()) continue;
        tileRenderer.tesselateBlockInWorld(id, lx, ly, lz, c->cx, c->cz);
      }
    }
  }

  flushSubChunk(c, sy, m_opaqueTess, m_transTess, m_transFancyTess, m_emitTess);
}

void ChunkRenderer::render(float camX, float camY, float camZ) {
  if (!m_level)
    return;

  m_atlas->bind();

  ScePspFMatrix4 vp;
  PSPRenderer_GetViewProjMatrix(&vp);
  Frustum frustum;
  frustum.update(vp);

  // 2 chunks draw distance (2 * 16 blocks)
  static const float RENDER_DISTANCE = 32.0f;
  static const float FANCY_LOD_DIST = 32.0f; 



  struct RenderChunk {
    Chunk *chunk;
    int subChunkIdx;
    float distSq;
    float distSqHoriz;
  };
  RenderChunk visibleChunks[WORLD_CHUNKS_X * WORLD_CHUNKS_Z * SUBCHUNK_COUNT];
  int visibleCount = 0;

  // Process compile queue
  processCompileQueue(camX, camY, camZ);

  // Render loop
  for (int cx = 0; cx < WORLD_CHUNKS_X; cx++) {
    for (int cz = 0; cz < WORLD_CHUNKS_Z; cz++) {
      Chunk *c = m_level->getChunk(cx, cz);
      if (!c)
        continue;

      for (int sy = 0; sy < SUBCHUNK_COUNT; sy++) {
        if ((c->opaqueTriCount[sy] == 0 && c->transTriCount[sy] == 0 &&
             c->transFancyTriCount[sy] == 0 && c->emitTriCount[sy] == 0) ||
            (!c->opaqueVertices[sy] && !c->transVertices[sy] &&
             !c->transFancyVertices[sy] && !c->emitVertices[sy]))
          continue;

        float chunkCenterX = c->cx * CHUNK_SIZE_X + CHUNK_SIZE_X / 2.0f;
        float chunkCenterZ = c->cz * CHUNK_SIZE_Z + CHUNK_SIZE_Z / 2.0f;
        float chunkCenterY = sy * 16 + 8.0f;

        float dx = chunkCenterX - camX;
        float dy = chunkCenterY - camY;
        float dz = chunkCenterZ - camZ;
        
        // Distance culling
        float maxDist = RENDER_DISTANCE;
        float distSqHoriz = dx * dx + dz * dz;
        if (distSqHoriz > maxDist * maxDist)
          continue;

        float distSq = dx * dx + dy * dy + dz * dz;

        AABB box;

        box.x0 = c->cx * CHUNK_SIZE_X - 4.0f;
        box.y0 = sy * 16 - 4.0f;
        box.z0 = c->cz * CHUNK_SIZE_Z - 4.0f;
        box.x1 = c->cx * CHUNK_SIZE_X + CHUNK_SIZE_X + 4.0f;
        box.y1 = sy * 16 + 16 + 4.0f;
        box.z1 = c->cz * CHUNK_SIZE_Z + CHUNK_SIZE_Z + 4.0f;

        // 3D Cubic Frustum Culling
        if (frustum.testAABB(box) == Frustum::OUTSIDE)
          continue;

        visibleChunks[visibleCount].chunk = c;
        visibleChunks[visibleCount].subChunkIdx = sy;
        visibleChunks[visibleCount].distSq = distSq;
        visibleChunks[visibleCount].distSqHoriz = distSqHoriz;
        visibleCount++;
      }
    }
  }

  // Sort visible chunks front-to-back
  for (int i = 0; i < visibleCount - 1; i++) {
    for (int j = 0; j < visibleCount - i - 1; j++) {
      if (visibleChunks[j].distSq > visibleChunks[j + 1].distSq) {
        RenderChunk temp = visibleChunks[j];
        visibleChunks[j] = visibleChunks[j + 1];
        visibleChunks[j + 1] = temp;
      }
    }
  }

  // Draw opaque chunks

  // Helper: set model matrix to identity (vertices are already in absolute world space)
  auto setChunkMatrix = [](Chunk *c) {
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    sceGumUpdateMatrix();
  };

  sceGuDisable(GU_ALPHA_TEST);
  sceGuDisable(GU_BLEND);
  // Chunk vertices are already lit in TileRenderer (sky/block light + sun scale).
  // Keep fixed-function lighting off here, otherwise GU ambient flattens contrast
  // and makes the world look uniformly bright.
  sceGuDisable(GU_LIGHTING);

  for (int i = 0; i < visibleCount; i++) {
    Chunk *c = visibleChunks[i].chunk;
    int sy = visibleChunks[i].subChunkIdx;
    if (c->opaqueTriCount[sy] == 0 || !c->opaqueVertices[sy]) continue;
    setChunkMatrix(c);
    sceGumDrawArray(GU_TRIANGLES,
                    GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D,
                    c->opaqueTriCount[sy], nullptr, c->opaqueVertices[sy]);
  }

  // Draw emissive chunks
  // Keep alpha test enabled so cutout textures (e.g. leaves/glass in emit-pass)
  // don't render their transparent texels as dark quads.
  sceGuEnable(GU_ALPHA_TEST);
  sceGuAmbient(0xFFFFFFFF);
  for (int i = 0; i < visibleCount; i++) {
    Chunk *c = visibleChunks[i].chunk;
    int sy = visibleChunks[i].subChunkIdx;
    if (c->emitTriCount[sy] == 0 || !c->emitVertices[sy]) continue;
    setChunkMatrix(c);
    sceGumDrawArray(GU_TRIANGLES,
                    GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D,
                    c->emitTriCount[sy], nullptr, c->emitVertices[sy]);
  }

  // Draw inner leaves (Back-to-Front check)
  sceGuDisable(GU_LIGHTING);
  sceGuEnable(GU_ALPHA_TEST);
  sceGuEnable(GU_BLEND);
  sceGuDisable(GU_CULL_FACE); // Allow plants/water to be seen from both sides

  for (int i = visibleCount - 1; i >= 0; i--) {
    Chunk *c = visibleChunks[i].chunk;
    int sy = visibleChunks[i].subChunkIdx;
    if (c->transFancyTriCount[sy] == 0 || !c->transFancyVertices[sy]) continue;
    float distSqHoriz = visibleChunks[i].distSqHoriz;
    if (distSqHoriz > FANCY_LOD_DIST * FANCY_LOD_DIST || camY > 128.0f) continue;
    setChunkMatrix(c);
    sceGumDrawArray(GU_TRIANGLES,
                    GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D,
                    c->transFancyTriCount[sy], nullptr, c->transFancyVertices[sy]);
  }

  // Draw transparent chunks (Back-to-Front)
  for (int i = visibleCount - 1; i >= 0; i--) {
    Chunk *c = visibleChunks[i].chunk;
    int sy = visibleChunks[i].subChunkIdx;
    if (c->transTriCount[sy] == 0 || !c->transVertices[sy]) continue;
    setChunkMatrix(c);
    sceGumDrawArray(GU_TRIANGLES,
                    GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D,
                    c->transTriCount[sy], nullptr, c->transVertices[sy]);
  }

  sceGuEnable(GU_CULL_FACE); // Restore CULL_FACE
  sceGuDisable(GU_LIGHTING); // Restore default state
}
