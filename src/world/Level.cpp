#include "Level.h"
#include "Random.h"
#include "WorldGen.h"
#include "TreeFeature.h"
#include <vector>
#include <functional>
#include <string.h>

struct LightNode {
  int x, y, z;
};

bool Level::isWaterBlock(uint8_t id) const {
  return id == BLOCK_WATER_STILL || id == BLOCK_WATER_FLOW;
}

void Level::setSimulationFocus(int wx, int wy, int wz, int radius) {
  m_simFocusX = wx;
  m_simFocusY = wy;
  m_simFocusZ = wz;
  m_simFocusRadius = radius;
}

void Level::tick() {
  m_time += 1;
  if (!m_waterDirty) return;

  // MCPE-like liquid logic is expensive; run every few world ticks.
  if (++m_waterTickAccum >= 6) {
    m_waterTickAccum = 0;
    tickWater();
    if (m_waterWakeTicks > 0) m_waterWakeTicks--;
  }
}

void Level::tickWater() {
  // NOTE: This is intentionally metadata-lite MCPE-style water simulation.
  // It tracks a per-block depth value (0..7) to drive both behavior and render heights.
  struct WaterOp {
    int x, y, z;
    uint8_t id;
    uint8_t depth;
  };
  std::vector<WaterOp> ops;
  ops.reserve(8192);
  const int maxWaterCellsPerTick = 2048;
  int processedWaterCells = 0;
  bool budgetReached = false;

  const int maxX = WORLD_CHUNKS_X * CHUNK_SIZE_X;
  const int maxZ = WORLD_CHUNKS_Z * CHUNK_SIZE_Z;
  int simMinY = 0;
  int simMaxY = CHUNK_SIZE_Y - 1;
  int simMinX = 0;
  int simMaxX = maxX - 1;
  int simMinZ = 0;
  int simMaxZ = maxZ - 1;
  int focusX = m_simFocusX;
  int focusY = m_simFocusY;
  int focusZ = m_simFocusZ;
  int focusRadius = m_simFocusRadius;
  int focusYRadius = m_simFocusYRadius;
  if (m_waterWakeTicks > 0 && m_waterWakeX >= 0 && m_waterWakeZ >= 0) {
    focusX = m_waterWakeX;
    focusY = m_waterWakeY;
    focusZ = m_waterWakeZ;
    focusRadius = m_waterWakeRadius;
    focusYRadius = m_waterWakeRadius;
  }

  if (focusX >= 0 && focusZ >= 0 && focusRadius > 0) {
    simMinX = focusX - focusRadius;
    simMaxX = focusX + focusRadius;
    simMinZ = focusZ - focusRadius;
    simMaxZ = focusZ + focusRadius;
    if (simMinX < 0) simMinX = 0;
    if (simMinZ < 0) simMinZ = 0;
    if (simMaxX >= maxX) simMaxX = maxX - 1;
    if (simMaxZ >= maxZ) simMaxZ = maxZ - 1;
  }
  if (focusY >= 0) {
    simMinY = focusY - focusYRadius;
    simMaxY = focusY + focusYRadius;
    if (simMinY < 0) simMinY = 0;
    if (simMaxY >= CHUNK_SIZE_Y) simMaxY = CHUNK_SIZE_Y - 1;
  }

  auto inBounds = [&](int x, int y, int z) {
    return x >= 0 && x < maxX && z >= 0 && z < maxZ && y >= 0 && y < CHUNK_SIZE_Y;
  };

  auto queueSet = [&](int x, int y, int z, uint8_t id, uint8_t depth) {
    if (!inBounds(x, y, z)) return;
    uint8_t cur = getBlock(x, y, z);
    uint8_t curDepth = getWaterDepth(x, y, z);
    if (cur == id && curDepth == depth) return;
    ops.push_back({x, y, z, id, depth});
  };

  auto canFlowInto = [&](int x, int y, int z) {
    if (!inBounds(x, y, z)) return false;
    uint8_t t = getBlock(x, y, z);
    if (t == BLOCK_AIR) return true;
    if (isWaterBlock(t)) return true;
    return !g_blockProps[t].isSolid();
  };

  auto hasDownwardExit = [&](int x, int y, int z) {
    int by = y - 1;
    return by >= 0 && canFlowInto(x, by, z);
  };

  static const int flowDx[4] = {-1, 1, 0, 0};
  static const int flowDz[4] = {0, 0, -1, 1};
  static const int oppositeDir[4] = {1, 0, 3, 2};
  const int maxFlowSearch = 7;

  std::function<int(int, int, int, int, int)> flowCost =
      [&](int x, int y, int z, int dist, int fromDir) -> int {
    if (hasDownwardExit(x, y, z)) return dist;
    if (dist >= maxFlowSearch) return 999;

    int best = 999;
    for (int i = 0; i < 4; ++i) {
      if (fromDir >= 0 && i == oppositeDir[fromDir]) continue;
      int nx = x + flowDx[i], nz = z + flowDz[i];
      if (!inBounds(nx, y, nz)) continue;
      if (!canFlowInto(nx, y, nz)) continue;
      int c = flowCost(nx, y, nz, dist + 1, i);
      if (c < best) best = c;
    }
    return best;
  };

  int spanX = simMaxX - simMinX + 1;
  int spanY = simMaxY - simMinY + 1;
  int spanZ = simMaxZ - simMinZ + 1;
  int scanVolume = spanX * spanY * spanZ;
  if (scanVolume <= 0) return;
  if (m_waterScanCursor < 0 || m_waterScanCursor >= scanVolume) m_waterScanCursor = 0;

  for (int n = 0; n < scanVolume && !budgetReached; ++n) {
    int idx = m_waterScanCursor + n;
    if (idx >= scanVolume) idx -= scanVolume;

    int lx = idx % spanX;
    int yz = idx / spanX;
    int lz = yz % spanZ;
    int ly = yz / spanZ;

    int x = simMinX + lx;
    int z = simMinZ + lz;
    int y = simMaxY - ly;

    uint8_t id = getBlock(x, y, z);
    if (!isWaterBlock(id)) continue;
    if (++processedWaterCells > maxWaterCellsPerTick) {
      budgetReached = true;
      break;
    }

    // Skip fully enclosed still-water interior (large lakes/oceans) to reduce
    // heavy per-block work near big water bodies.
    if (id == BLOCK_WATER_STILL) {
      bool waterAbove = isWaterBlock(getBlock(x, y + 1, z));
      bool waterBelow = isWaterBlock(getBlock(x, y - 1, z));
      bool waterSides =
          isWaterBlock(getBlock(x - 1, y, z)) &&
          isWaterBlock(getBlock(x + 1, y, z)) &&
          isWaterBlock(getBlock(x, y, z - 1)) &&
          isWaterBlock(getBlock(x, y, z + 1));
      if (waterAbove && waterBelow && waterSides) continue;
    }
    uint8_t curDepth = getWaterDepth(x, y, z);
    if (curDepth == 0xFF) curDepth = (id == BLOCK_WATER_STILL) ? 0 : 1;

    if (id == BLOCK_WATER_STILL) curDepth = 0;

    // Prefer downward flow.
    int by = y - 1;
    bool flowedDown = false;
    if (by >= 0 && canFlowInto(x, by, z)) {
      queueSet(x, by, z, BLOCK_WATER_FLOW, 1);
      flowedDown = true;
    }

    // Horizontal spread and depth propagation.
    uint8_t neighborMin = 7;
    bool hasWaterNeighbor = false;
    int sourceNeighbors = 0;
    bool hasWaterAbove = isWaterBlock(getBlock(x, y + 1, z));
    for (int i = 0; i < 4; ++i) {
      int nx = x + flowDx[i], nz = z + flowDz[i];
      if (!inBounds(nx, y, nz)) continue;
      uint8_t nId = getBlock(nx, y, nz);
      if (isWaterBlock(nId)) {
        hasWaterNeighbor = true;
        uint8_t nd = getWaterDepth(nx, y, nz);
        if (nd == 0xFF) nd = (nId == BLOCK_WATER_STILL) ? 0 : 1;
        if (nd == 0) sourceNeighbors++;
        if (nd != 0xFF && nd < neighborMin) neighborMin = nd;
      }
    }
    uint8_t belowId = getBlock(x, y - 1, z);
    bool supportBelow = (y == 0) || g_blockProps[belowId].isSolid() || isWaterBlock(belowId);

    uint8_t nextDepth = curDepth;
    if (id == BLOCK_WATER_STILL) {
      nextDepth = 0;
    } else {
      if (hasWaterAbove) nextDepth = 1;
      else if (sourceNeighbors >= 2 && supportBelow) nextDepth = 0;
      else if (neighborMin < 7) nextDepth = (uint8_t)(neighborMin + 1);
      else nextDepth = 8;
    }

    if (nextDepth <= 7) {
      uint8_t spreadDepth = (id == BLOCK_WATER_STILL) ? 1 : (uint8_t)(nextDepth + 1);
      if (spreadDepth <= 7 && !flowedDown) {
        bool useShortestPathPriority = isWaterBlock(id);
        int costs[4] = {999, 999, 999, 999};
        int bestCost = 999;
        bool canSpread[4] = {false, false, false, false};
        for (int i = 0; i < 4; ++i) {
          int nx = x + flowDx[i], nz = z + flowDz[i];
          if (!inBounds(nx, y, nz)) continue;
          if (!canFlowInto(nx, y, nz)) continue;
          canSpread[i] = true;
          if (useShortestPathPriority) {
            costs[i] = hasDownwardExit(nx, y, nz) ? 0 : flowCost(nx, y, nz, 1, i);
          } else {
            // Still/source blocks spread cheaply; avoid expensive recursive path search.
            costs[i] = hasDownwardExit(nx, y, nz) ? 0 : 1;
          }
          if (costs[i] < bestCost) bestCost = costs[i];
        }
        for (int i = 0; i < 4; ++i) {
          if (!canSpread[i]) continue;
          if (bestCost != 999 && costs[i] != bestCost) continue;
          int nx = x + flowDx[i], nz = z + flowDz[i];
          uint8_t nId = getBlock(nx, y, nz);
          uint8_t nDepth = getWaterDepth(nx, y, nz);
          if (!isWaterBlock(nId) || nDepth == 0xFF || nDepth > spreadDepth) {
            queueSet(nx, y, nz, BLOCK_WATER_FLOW, spreadDepth);
          }
        }
      }
      if (id == BLOCK_WATER_FLOW) {
        if (nextDepth >= 8 || (!hasWaterAbove && !hasWaterNeighbor && !flowedDown)) {
          queueSet(x, y, z, BLOCK_AIR, 0xFF);
        } else {
          queueSet(x, y, z, (nextDepth == 0) ? BLOCK_WATER_STILL : BLOCK_WATER_FLOW, nextDepth);
        }
      } else {
        queueSet(x, y, z, BLOCK_WATER_STILL, 0);
      }
    } else {
      queueSet(x, y, z, BLOCK_AIR, 0xFF);
    }
  }

  if (scanVolume > 1) {
    int advance = maxWaterCellsPerTick;
    if (advance < 1) advance = 1;
    m_waterScanCursor += advance;
    if (m_waterScanCursor >= scanVolume) m_waterScanCursor %= scanVolume;
  } else {
    m_waterScanCursor = 0;
  }

  for (const auto &op : ops) {
    int cx = op.x >> 4;
    int cz = op.z >> 4;
    if (cx < 0 || cx >= WORLD_CHUNKS_X || cz < 0 || cz >= WORLD_CHUNKS_Z || op.y < 0 || op.y >= CHUNK_SIZE_Y) continue;
    uint8_t cur = m_chunks[cx][cz]->getBlock(op.x & 0xF, op.y, op.z & 0xF);
    if (cur != op.id) {
      m_chunks[cx][cz]->setBlock(op.x & 0xF, op.y, op.z & 0xF, op.id);
      updateLight(op.x, op.y, op.z);
      markDirty(op.x, op.y, op.z);
    }
    setWaterDepth(op.x, op.y, op.z, op.depth);
  }
  m_waterDirty = !ops.empty();
}

Level::Level() {
  memset(m_chunks, 0, sizeof(m_chunks));
  m_waterDepth.resize(WORLD_CHUNKS_X * CHUNK_SIZE_X * CHUNK_SIZE_Y * WORLD_CHUNKS_Z * CHUNK_SIZE_Z, 0xFF);
  for (int cx = 0; cx < WORLD_CHUNKS_X; cx++)
    for (int cz = 0; cz < WORLD_CHUNKS_Z; cz++)
      m_chunks[cx][cz] = new Chunk();
}

Level::~Level() {
  for (int cx = 0; cx < WORLD_CHUNKS_X; cx++)
    for (int cz = 0; cz < WORLD_CHUNKS_Z; cz++)
      delete m_chunks[cx][cz];
}

Chunk* Level::getChunk(int cx, int cz) const {
  if (cx < 0 || cx >= WORLD_CHUNKS_X || cz < 0 || cz >= WORLD_CHUNKS_Z) return nullptr;
  return m_chunks[cx][cz];
}

void Level::markDirty(int wx, int wy, int wz) {
  int cx = wx >> 4;
  int cz = wz >> 4;
  int sy = wy >> 4;
  if (cx >= 0 && cx < WORLD_CHUNKS_X && cz >= 0 && cz < WORLD_CHUNKS_Z && sy >= 0 && sy < 4) {
    m_chunks[cx][cz]->dirty[sy] = true;
    
    // Dirty neighbor subchunks
    int lx = wx & 0xF;
    int lz = wz & 0xF;
    int ly = wy & 0xF;
    
    if (lx == 0 && cx > 0) m_chunks[cx - 1][cz]->dirty[sy] = true;
    if (lx == 15 && cx < WORLD_CHUNKS_X - 1) m_chunks[cx + 1][cz]->dirty[sy] = true;
    if (lz == 0 && cz > 0) m_chunks[cx][cz - 1]->dirty[sy] = true;
    if (lz == 15 && cz < WORLD_CHUNKS_Z - 1) m_chunks[cx][cz + 1]->dirty[sy] = true;
    if (ly == 0 && sy > 0) m_chunks[cx][cz]->dirty[sy - 1] = true;
    if (ly == 15 && sy < 3) m_chunks[cx][cz]->dirty[sy + 1] = true;
  }
}

uint8_t Level::getBlock(int wx, int wy, int wz) const {
  int cx = wx >> 4;
  int cz = wz >> 4;
  if (cx < 0 || cx >= WORLD_CHUNKS_X || cz < 0 || cz >= WORLD_CHUNKS_Z || wy < 0 || wy >= CHUNK_SIZE_Y) return BLOCK_AIR;
  return m_chunks[cx][cz]->getBlock(wx & 0xF, wy, wz & 0xF);
}

int Level::waterIndex(int wx, int wy, int wz) const {
  return ((wy * (WORLD_CHUNKS_Z * CHUNK_SIZE_Z) + wz) * (WORLD_CHUNKS_X * CHUNK_SIZE_X) + wx);
}

uint8_t Level::getWaterDepth(int wx, int wy, int wz) const {
  int maxX = WORLD_CHUNKS_X * CHUNK_SIZE_X;
  int maxZ = WORLD_CHUNKS_Z * CHUNK_SIZE_Z;
  if (wx < 0 || wx >= maxX || wz < 0 || wz >= maxZ || wy < 0 || wy >= CHUNK_SIZE_Y) return 0xFF;
  return m_waterDepth[waterIndex(wx, wy, wz)];
}

void Level::setWaterDepth(int wx, int wy, int wz, uint8_t depth) {
  int maxX = WORLD_CHUNKS_X * CHUNK_SIZE_X;
  int maxZ = WORLD_CHUNKS_Z * CHUNK_SIZE_Z;
  if (wx < 0 || wx >= maxX || wz < 0 || wz >= maxZ || wy < 0 || wy >= CHUNK_SIZE_Y) return;
  m_waterDepth[waterIndex(wx, wy, wz)] = depth;
}

void Level::setBlock(int wx, int wy, int wz, uint8_t id) {
  int cx = wx >> 4;
  int cz = wz >> 4;
  if (cx < 0 || cx >= WORLD_CHUNKS_X || cz < 0 || cz >= WORLD_CHUNKS_Z || wy < 0 || wy >= CHUNK_SIZE_Y) return;
  uint8_t oldId = m_chunks[cx][cz]->getBlock(wx & 0xF, wy, wz & 0xF);
  if (oldId == id) return;
  
  m_chunks[cx][cz]->setBlock(wx & 0xF, wy, wz & 0xF, id);
  if (isWaterBlock(id)) {
    setWaterDepth(wx, wy, wz, (id == BLOCK_WATER_STILL) ? 0 : 1);
  } else {
    setWaterDepth(wx, wy, wz, 0xFF);
  }

  bool touchesWater = isWaterBlock(oldId) || isWaterBlock(id);
  if (!touchesWater) {
    static const int dx[6] = {-1, 1, 0, 0, 0, 0};
    static const int dy[6] = {0, 0, -1, 1, 0, 0};
    static const int dz[6] = {0, 0, 0, 0, -1, 1};
    for (int i = 0; i < 6; ++i) {
      int nx = wx + dx[i], ny = wy + dy[i], nz = wz + dz[i];
      if (ny < 0 || ny >= CHUNK_SIZE_Y) continue;
      if (isWaterBlock(getBlock(nx, ny, nz))) {
        touchesWater = true;
        break;
      }
    }
  }
  if (touchesWater) {
    m_waterDirty = true;
    m_waterWakeX = wx;
    m_waterWakeY = wy;
    m_waterWakeZ = wz;
    m_waterWakeTicks = 16;
  }

  updateLight(wx, wy, wz);
}

std::vector<AABB> Level::getCubes(const AABB& box) const {
  std::vector<AABB> boxes;
  int x0 = (int)floorf(box.x0);
  int x1 = (int)floorf(box.x1 + 1.0f);
  int y0 = (int)floorf(box.y0);
  int y1 = (int)floorf(box.y1 + 1.0f);
  int z0 = (int)floorf(box.z0);
  int z1 = (int)floorf(box.z1 + 1.0f);

  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (z0 < 0) z0 = 0;
  if (x1 > WORLD_CHUNKS_X * CHUNK_SIZE_X) x1 = WORLD_CHUNKS_X * CHUNK_SIZE_X;
  if (y1 > CHUNK_SIZE_Y) y1 = CHUNK_SIZE_Y;
  if (z1 > WORLD_CHUNKS_Z * CHUNK_SIZE_Z) z1 = WORLD_CHUNKS_Z * CHUNK_SIZE_Z;

  for (int x = x0; x < x1; x++) {
    for (int y = y0; y < y1; y++) {
      for (int z = z0; z < z1; z++) {
        uint8_t id = getBlock(x, y, z);
        if (id > 0 && g_blockProps[id].isSolid()) {
          // Create bounding box
          boxes.push_back(AABB((double)x, (double)y, (double)z,
                               (double)(x + 1), (double)(y + 1), (double)(z + 1)));
        }
      }
    }
  }
  return boxes;
}

uint8_t Level::getSkyLight(int wx, int wy, int wz) const {
  int cx = wx >> 4;
  int cz = wz >> 4;
  if (cx < 0 || cx >= WORLD_CHUNKS_X || cz < 0 || cz >= WORLD_CHUNKS_Z || wy < 0 || wy >= CHUNK_SIZE_Y) return 15;
  return m_chunks[cx][cz]->getSkyLight(wx & 0xF, wy, wz & 0xF);
}

uint8_t Level::getBlockLight(int wx, int wy, int wz) const {
  int cx = wx >> 4;
  int cz = wz >> 4;
  if (cx < 0 || cx >= WORLD_CHUNKS_X || cz < 0 || cz >= WORLD_CHUNKS_Z || wy < 0 || wy >= CHUNK_SIZE_Y) return 0;
  return m_chunks[cx][cz]->getBlockLight(wx & 0xF, wy, wz & 0xF);
}

void Level::setSkyLight(int wx, int wy, int wz, uint8_t val) {
  int cx = wx >> 4;
  int cz = wz >> 4;
  if (cx < 0 || cx >= WORLD_CHUNKS_X || cz < 0 || cz >= WORLD_CHUNKS_Z || wy < 0 || wy >= CHUNK_SIZE_Y) return;
  uint8_t curBlock = m_chunks[cx][cz]->getBlockLight(wx & 0xF, wy, wz & 0xF);
  m_chunks[cx][cz]->setLight(wx & 0xF, wy, wz & 0xF, val, curBlock);
}

void Level::setBlockLight(int wx, int wy, int wz, uint8_t val) {
  int cx = wx >> 4;
  int cz = wz >> 4;
  if (cx < 0 || cx >= WORLD_CHUNKS_X || cz < 0 || cz >= WORLD_CHUNKS_Z || wy < 0 || wy >= CHUNK_SIZE_Y) return;
  uint8_t curSky = m_chunks[cx][cz]->getSkyLight(wx & 0xF, wy, wz & 0xF);
  m_chunks[cx][cz]->setLight(wx & 0xF, wy, wz & 0xF, curSky, val);
}

void Level::generate(Random *rng) {
  int64_t seed = rng->nextLong();

  for (int cx = 0; cx < WORLD_CHUNKS_X; cx++) {
    for (int cz = 0; cz < WORLD_CHUNKS_Z; cz++) {
      Chunk *c = m_chunks[cx][cz];
      c->cx = cx;
      c->cz = cz;
      WorldGen::generateChunk(c->blocks, cx, cz, seed);
      for(int i=0; i<4; i++) c->dirty[i] = true;
    }
  }

  for (int cx = 0; cx < WORLD_CHUNKS_X; cx++) {
    for (int cz = 0; cz < WORLD_CHUNKS_Z; cz++) {
      Random chunkRng(seed ^ ((int64_t)cx * 341873128712LL) ^ ((int64_t)cz * 132897987541LL));
      for (int i = 0; i < 3; i++) {
        int lx = chunkRng.nextInt(CHUNK_SIZE_X);
        int lz = chunkRng.nextInt(CHUNK_SIZE_Z);
        int wx = cx * CHUNK_SIZE_X + lx;
        int wz = cz * CHUNK_SIZE_Z + lz;

        int wy = CHUNK_SIZE_Y - 1;
        while (wy > 0 && getBlock(wx, wy, wz) == BLOCK_AIR) wy--;

        if (wy > 50 && getBlock(wx, wy, wz) == BLOCK_GRASS) {
          setBlock(wx, wy, wz, BLOCK_DIRT);
          TreeFeature::place(this, wx, wy + 1, wz, chunkRng);
        }
      }
    }
  }

  for (int y = 0; y < CHUNK_SIZE_Y; ++y) {
    for (int z = 0; z < WORLD_CHUNKS_Z * CHUNK_SIZE_Z; ++z) {
      for (int x = 0; x < WORLD_CHUNKS_X * CHUNK_SIZE_X; ++x) {
        uint8_t id = getBlock(x, y, z);
        if (id == BLOCK_WATER_STILL) setWaterDepth(x, y, z, 0);
        else if (id == BLOCK_WATER_FLOW) setWaterDepth(x, y, z, 1);
        else setWaterDepth(x, y, z, 0xFF);
      }
    }
  }

  computeLighting();
}

void Level::computeLighting() {
  std::vector<LightNode> lightQ;
  lightQ.reserve(65536);

  // 1. Sunlight
  for (int x = 0; x < WORLD_CHUNKS_X * CHUNK_SIZE_X; x++) {
    for (int z = 0; z < WORLD_CHUNKS_Z * CHUNK_SIZE_Z; z++) {
      int curLight = 15;
      for (int y = CHUNK_SIZE_Y - 1; y >= 0; y--) {
        uint8_t id = getBlock(x, y, z);
        if (id != BLOCK_AIR) {
           const BlockProps &bp = g_blockProps[id];
           if (bp.isOpaque()) curLight = 0;
           else if (id == BLOCK_LEAVES) curLight = (curLight >= 2) ? curLight - 2 : 0;
           else if (bp.isLiquid()) curLight = (curLight >= 3) ? curLight - 3 : 0;
        }
        setSkyLight(x, y, z, curLight);
      }
    }
  }

  // 2. Queue borders
  for (int x = 0; x < WORLD_CHUNKS_X * CHUNK_SIZE_X; x++) {
    for (int z = 0; z < WORLD_CHUNKS_Z * CHUNK_SIZE_Z; z++) {
      for (int y = CHUNK_SIZE_Y - 1; y >= 0; y--) {
        if (getSkyLight(x, y, z) == 15) {
           bool needsSpread = false;
           const int dx[] = {-1, 1, 0, 0, 0, 0};
           const int dy[] = {0, 0, -1, 1, 0, 0};
           const int dz[] = {0, 0, 0, 0, -1, 1};
           for(int i = 0; i < 6; i++) {
             int nx = x + dx[i], ny = y + dy[i], nz = z + dz[i];
             if (ny >= 0 && ny < CHUNK_SIZE_Y && nx >= 0 && nx < WORLD_CHUNKS_X * CHUNK_SIZE_X && nz >= 0 && nz < WORLD_CHUNKS_Z * CHUNK_SIZE_Z) {
                 if (getSkyLight(nx, ny, nz) < 15 && !g_blockProps[getBlock(nx, ny, nz)].isOpaque()) {
                     needsSpread = true;
                     break;
                 }
             }
           }
           if (needsSpread) lightQ.push_back({x, y, z});
        }
      }
    }
  }

  // 3. Sky light flood fill
  int head = 0;
  while (head < (int)lightQ.size()) {
    LightNode node = lightQ[head++];
    uint8_t level = getSkyLight(node.x, node.y, node.z);
    if (level <= 1) continue;

    const int dx[] = {-1, 1, 0, 0, 0, 0};
    const int dy[] = {0, 0, -1, 1, 0, 0};
    const int dz[] = {0, 0, 0, 0, -1, 1};

    for (int i = 0; i < 6; i++) {
      int nx = node.x + dx[i];
      int ny = node.y + dy[i];
      int nz = node.z + dz[i];

      if (ny < 0 || ny >= CHUNK_SIZE_Y || nx < 0 || nz < 0 || nx >= WORLD_CHUNKS_X * CHUNK_SIZE_X || nz >= WORLD_CHUNKS_Z * CHUNK_SIZE_Z) continue;
      uint8_t neighborId = getBlock(nx, ny, nz);
      if (g_blockProps[neighborId].isOpaque()) continue;

      int attenuation = 1;
      if (neighborId == BLOCK_LEAVES) attenuation = 2;
      else if (neighborId == BLOCK_WATER_STILL || neighborId == BLOCK_WATER_FLOW || neighborId == BLOCK_LAVA_STILL || neighborId == BLOCK_LAVA_FLOW) attenuation = 3;

      int neighborLevel = getSkyLight(nx, ny, nz);
      if (level - attenuation > neighborLevel) {
        setSkyLight(nx, ny, nz, level - attenuation);
        lightQ.push_back({nx, ny, nz});
      }
    }
  }

  lightQ.clear();

  // 4. Block light sources
  for (int cx = 0; cx < WORLD_CHUNKS_X; cx++) {
    for (int cz = 0; cz < WORLD_CHUNKS_Z; cz++) {
      for (int lx = 0; lx < CHUNK_SIZE_X; lx++) {
        for (int lz = 0; lz < CHUNK_SIZE_Z; lz++) {
          for (int ly = 0; ly < CHUNK_SIZE_Y; ly++) {
            int wx = cx * CHUNK_SIZE_X + lx;
            int wz = cz * CHUNK_SIZE_Z + lz;
            uint8_t id = m_chunks[cx][cz]->blocks[lx][lz][ly];
            if (id == BLOCK_LAVA_STILL || id == BLOCK_LAVA_FLOW || id == BLOCK_GLOWSTONE) {
              setBlockLight(wx, ly, wz, 15);
              lightQ.push_back({wx, ly, wz});
            } else {
              setBlockLight(wx, ly, wz, 0);
            }
          }
        }
      }
    }
  }

  head = 0;
  while (head < (int)lightQ.size()) {
    LightNode node = lightQ[head++];
    uint8_t level = getBlockLight(node.x, node.y, node.z);
    if (level <= 1) continue;

    const int dx[] = {-1, 1, 0, 0, 0, 0};
    const int dy[] = {0, 0, -1, 1, 0, 0};
    const int dz[] = {0, 0, 0, 0, -1, 1};

    for (int i = 0; i < 6; i++) {
      int nx = node.x + dx[i];
      int ny = node.y + dy[i];
      int nz = node.z + dz[i];

      if (ny < 0 || ny >= CHUNK_SIZE_Y || nx < 0 || nz < 0 || nx >= WORLD_CHUNKS_X * CHUNK_SIZE_X || nz >= WORLD_CHUNKS_Z * CHUNK_SIZE_Z) continue;
      uint8_t neighborId = getBlock(nx, ny, nz);
      if (g_blockProps[neighborId].isOpaque()) continue;

      int attenuation = 1;
      if (neighborId == BLOCK_LEAVES) attenuation = 2;
      else if (neighborId == BLOCK_WATER_STILL || neighborId == BLOCK_WATER_FLOW || neighborId == BLOCK_LAVA_STILL || neighborId == BLOCK_LAVA_FLOW) attenuation = 3;

      int neighborLevel = getBlockLight(nx, ny, nz);
      if (level - attenuation > neighborLevel) {
        setBlockLight(nx, ny, nz, level - attenuation);
        lightQ.push_back({nx, ny, nz});
      }
    }
  }
}

struct LightRemovalNode {
    short x, y, z;
    uint8_t val;
};

void Level::updateLight(int wx, int wy, int wz) {
  uint8_t id = getBlock(wx, wy, wz);
  uint8_t oldBlockLight = getBlockLight(wx, wy, wz);
  uint8_t newBlockLight = g_blockProps[id].light_emit;
  
  const int dx[] = {-1, 1, 0, 0, 0, 0};
  const int dy[] = {0, 0, -1, 1, 0, 0};
  const int dz[] = {0, 0, 0, 0, -1, 1};
  
  uint8_t maxNeighborLight = 0;
  for(int i=0; i<6; i++) {
    int nx = wx + dx[i];
    int ny = wy + dy[i];
    int nz = wz + dz[i];
    if (ny < 0 || ny >= CHUNK_SIZE_Y || nx < 0 || nz < 0 || nx >= WORLD_CHUNKS_X * CHUNK_SIZE_X || nz >= WORLD_CHUNKS_Z * CHUNK_SIZE_Z) continue;
    uint8_t nl = getBlockLight(nx, ny, nz);
    if(nl > maxNeighborLight) maxNeighborLight = nl;
  }
  
  uint8_t blockAtten = g_blockProps[id].isOpaque() ? 15 : ((id == BLOCK_LEAVES) ? 2 : (g_blockProps[id].isLiquid() ? 3 : 1));
  uint8_t expectedBlockLight = newBlockLight;
  if (maxNeighborLight > blockAtten && (maxNeighborLight - blockAtten) > expectedBlockLight) {
      expectedBlockLight = maxNeighborLight - blockAtten;
  }
  updateBlockLight(wx, wy, wz, oldBlockLight, expectedBlockLight);

  uint8_t oldSkyLight = getSkyLight(wx, wy, wz);
  uint8_t expectedSkyLight = 0;
  if (wy == CHUNK_SIZE_Y - 1) {
      expectedSkyLight = blockAtten < 15 ? 15 : 0;
  } else if (getSkyLight(wx, wy + 1, wz) == 15 && blockAtten < 15) {
      expectedSkyLight = 15;
  } else {
      uint8_t maxNeighborSkyLight = 0;
      for(int i=0; i<6; i++) {
        int nx = wx + dx[i];
        int ny = wy + dy[i];
        int nz = wz + dz[i];
        if (ny < 0 || ny >= CHUNK_SIZE_Y || nx < 0 || nz < 0 || nx >= WORLD_CHUNKS_X * CHUNK_SIZE_X || nz >= WORLD_CHUNKS_Z * CHUNK_SIZE_Z) continue;
        uint8_t nl = getSkyLight(nx, ny, nz);
        if(nl > maxNeighborSkyLight) maxNeighborSkyLight = nl;
      }
      if (maxNeighborSkyLight > blockAtten) expectedSkyLight = maxNeighborSkyLight - blockAtten;
  }
  updateSkyLight(wx, wy, wz, oldSkyLight, expectedSkyLight);
}

void Level::updateBlockLight(int wx, int wy, int wz, uint8_t oldLight, uint8_t newLight) {
    if (oldLight == newLight) return;
    
    static LightRemovalNode darkQ[65536];
    static LightNode lightQ[65536];
    int darkHead = 0, darkTail = 0;
    int lightHead = 0, lightTail = 0;

    const int dx[] = {-1, 1, 0, 0, 0, 0};
    const int dy[] = {0, 0, -1, 1, 0, 0};
    const int dz[] = {0, 0, 0, 0, -1, 1};

    if (oldLight > newLight) {
        darkQ[darkTail++] = {(short)wx, (short)wy, (short)wz, oldLight};
        setBlockLight(wx, wy, wz, 0);
    } else {
        lightQ[lightTail++] = {(short)wx, (short)wy, (short)wz};
        setBlockLight(wx, wy, wz, newLight);
    }

    while (darkHead < darkTail) {
        LightRemovalNode node = darkQ[darkHead++];
        int x = node.x, y = node.y, z = node.z;
        uint8_t level = node.val;

        for (int i = 0; i < 6; i++) {
            int nx = x + dx[i], ny = y + dy[i], nz = z + dz[i];
            if (ny < 0 || ny >= CHUNK_SIZE_Y || nx < 0 || nz < 0 || nx >= WORLD_CHUNKS_X * CHUNK_SIZE_X || nz >= WORLD_CHUNKS_Z * CHUNK_SIZE_Z) continue;
            
            uint8_t neighborLevel = getBlockLight(nx, ny, nz);
            if (neighborLevel != 0 && neighborLevel < level) {
                setBlockLight(nx, ny, nz, 0);
                // Mask array index
            }
        }
    }

    while (lightHead < lightTail) {
        LightNode node = lightQ[lightHead++];
        int x = node.x, y = node.y, z = node.z;
        uint8_t level = getBlockLight(x, y, z);
        
        for (int i = 0; i < 6; i++) {
            int nx = x + dx[i], ny = y + dy[i], nz = z + dz[i];
            if (ny < 0 || ny >= CHUNK_SIZE_Y || nx < 0 || nz < 0 || nx >= WORLD_CHUNKS_X * CHUNK_SIZE_X || nz >= WORLD_CHUNKS_Z * CHUNK_SIZE_Z) continue;
            
            uint8_t id = getBlock(nx, ny, nz);
            const BlockProps& bp = g_blockProps[id];
            
            int attenuation = bp.isOpaque() ? 15 : ((id == BLOCK_LEAVES) ? 2 : (bp.isLiquid() ? 3 : 1));
            int neighborLevel = getBlockLight(nx, ny, nz);
            
            if (level - attenuation > neighborLevel) {
                setBlockLight(nx, ny, nz, level - attenuation);
                lightQ[lightTail++ & 0xFFFF] = {(short)nx, (short)ny, (short)nz};
            }
        }
    }
}

void Level::updateSkyLight(int wx, int wy, int wz, uint8_t oldLight, uint8_t newLight) {
    if (oldLight == newLight) return;
    
    static LightRemovalNode darkQ[65536];
    static LightNode lightQ[65536];
    int darkHead = 0, darkTail = 0;
    int lightHead = 0, lightTail = 0;

    const int dx[] = {-1, 1, 0, 0, 0, 0};
    const int dy[] = {0, 0, -1, 1, 0, 0};
    const int dz[] = {0, 0, 0, 0, -1, 1};

    if (oldLight > newLight) {
        darkQ[darkTail++] = {(short)wx, (short)wy, (short)wz, oldLight};
        setSkyLight(wx, wy, wz, 0);
    } else {
        lightQ[lightTail++] = {(short)wx, (short)wy, (short)wz};
        setSkyLight(wx, wy, wz, newLight);
    }

    while (darkHead < darkTail) {
        LightRemovalNode node = darkQ[darkHead++];
        int x = node.x, y = node.y, z = node.z;
        uint8_t level = node.val;

        for (int i = 0; i < 6; i++) {
            int nx = x + dx[i], ny = y + dy[i], nz = z + dz[i];
            if (ny < 0 || ny >= CHUNK_SIZE_Y || nx < 0 || nz < 0 || nx >= WORLD_CHUNKS_X * CHUNK_SIZE_X || nz >= WORLD_CHUNKS_Z * CHUNK_SIZE_Z) continue;
            
            uint8_t neighborLevel = getSkyLight(nx, ny, nz);
            
            if (neighborLevel != 0 && ((dy[i] == -1 && level == 15 && neighborLevel == 15) || neighborLevel < level)) {
                setSkyLight(nx, ny, nz, 0);
                darkQ[darkTail++ & 0xFFFF] = {(short)nx, (short)ny, (short)nz, neighborLevel};
            } else if (neighborLevel >= level) {
                lightQ[lightTail++ & 0xFFFF] = {(short)nx, (short)ny, (short)nz};
            }
        }
    }

    while (lightHead < lightTail) {
        LightNode node = lightQ[lightHead++];
        int x = node.x, y = node.y, z = node.z;
        uint8_t level = getSkyLight(x, y, z);
        
        for (int i = 0; i < 6; i++) {
            int nx = x + dx[i], ny = y + dy[i], nz = z + dz[i];
            if (ny < 0 || ny >= CHUNK_SIZE_Y || nx < 0 || nz < 0 || nx >= WORLD_CHUNKS_X * CHUNK_SIZE_X || nz >= WORLD_CHUNKS_Z * CHUNK_SIZE_Z) continue;
            
            uint8_t id = getBlock(nx, ny, nz);
            const BlockProps& bp = g_blockProps[id];
            
            int attenuation = bp.isOpaque() ? 15 : ((id == BLOCK_LEAVES) ? 2 : (bp.isLiquid() ? 3 : 1));
            if (dy[i] == -1 && level == 15 && attenuation < 15) attenuation = 0;
            
            int neighborLevel = getSkyLight(nx, ny, nz);
            if (level - attenuation > neighborLevel) {
                setSkyLight(nx, ny, nz, level - attenuation);
                lightQ[lightTail++ & 0xFFFF] = {(short)nx, (short)ny, (short)nz};
            }
        }
    }
}
