#include "Level.h"
#include "Random.h"
#include "WorldGen.h"
#include "TreeFeature.h"
#include <vector>
#include <functional>
#include <unordered_map>
#include <algorithm>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

struct LightNode {
  int x, y, z;
};

bool Level::isWaterBlock(uint8_t id) const {
  return id == BLOCK_WATER_STILL || id == BLOCK_WATER_FLOW;
}

bool Level::isLavaBlock(uint8_t id) const {
  return id == BLOCK_LAVA_STILL || id == BLOCK_LAVA_FLOW;
}

void Level::setSimulationFocus(int wx, int wy, int wz, int radius) {
  m_simFocusX = wx;
  m_simFocusY = wy;
  m_simFocusZ = wz;
  m_simFocusRadius = radius;
}

void Level::tick() {
  m_time += 1;
  // MCPE-like liquid logic is expensive; run every few world ticks.
  if (m_waterDirty && ++m_waterTickAccum >= 5) {
    m_waterTickAccum = 0;
    tickWater();
    if (m_waterWakeTicks > 0) m_waterWakeTicks--;
  }
  if (m_lavaDirty && ++m_lavaTickAccum >= 30) {
    m_lavaTickAccum = 0;
    tickLava();
    if (m_lavaWakeTicks > 0) m_lavaWakeTicks--;
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
  std::unordered_map<int, size_t> opIndex;
  opIndex.reserve(4096);
  const int maxWaterCellsPerTick = 768;
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
  // Queue-based scheduling (MCPE-style direction): consume queued liquid cells
  // and simulate only a tight bounding region around them this tick.
  bool hasQueuedFocus = false;
  int seeds = 0;
  const int maxSeedsPerTick = 256;
  while (!m_waterQueue.empty() && seeds < maxSeedsPerTick) {
    int idx = m_waterQueue.front();
    m_waterQueue.pop_front();
    if (idx < 0 || idx >= (int)m_waterQueued.size()) continue;
    m_waterQueued[idx] = 0;
    int x = idx % maxX;
    int yz = idx / maxX;
    int z = yz % maxZ;
    int y = yz / maxZ;
    if (!hasQueuedFocus) {
      simMinX = simMaxX = x;
      simMinY = simMaxY = y;
      simMinZ = simMaxZ = z;
      hasQueuedFocus = true;
    } else {
      if (x < simMinX) simMinX = x;
      if (x > simMaxX) simMaxX = x;
      if (y < simMinY) simMinY = y;
      if (y > simMaxY) simMaxY = y;
      if (z < simMinZ) simMinZ = z;
      if (z > simMaxZ) simMaxZ = z;
    }
    seeds++;
  }
  if (!hasQueuedFocus) {
    m_waterDirty = false;
    return;
  }
  const int expand = m_waterWakeRadius;
  simMinX = (simMinX - expand < 0) ? 0 : simMinX - expand;
  simMaxX = (simMaxX + expand >= maxX) ? (maxX - 1) : simMaxX + expand;
  simMinZ = (simMinZ - expand < 0) ? 0 : simMinZ - expand;
  simMaxZ = (simMaxZ + expand >= maxZ) ? (maxZ - 1) : simMaxZ + expand;
  simMinY = (simMinY - expand < 0) ? 0 : simMinY - expand;
  simMaxY = (simMaxY + expand >= CHUNK_SIZE_Y) ? (CHUNK_SIZE_Y - 1) : simMaxY + expand;

  auto inBounds = [&](int x, int y, int z) {
    return x >= 0 && x < maxX && z >= 0 && z < maxZ && y >= 0 && y < CHUNK_SIZE_Y;
  };

  auto queueSet = [&](int x, int y, int z, uint8_t id, uint8_t depth) {
    if (!inBounds(x, y, z)) return;
    int key = ((y * maxZ + z) * maxX + x);
    auto it = opIndex.find(key);
    if (it != opIndex.end()) {
      WaterOp &pending = ops[it->second];
      if (pending.id == id && pending.depth == depth) return;
      pending.id = id;
      pending.depth = depth;
      return;
    }
    uint8_t cur = getBlock(x, y, z);
    uint8_t curDepth = getWaterDepth(x, y, z);
    if (cur == id && curDepth == depth) return;
    opIndex[key] = ops.size();
    ops.push_back({x, y, z, id, depth});
  };

  auto waterReactionBlock = [&](uint8_t lavaId, uint8_t lavaDepth) -> uint8_t {
    if (lavaId == BLOCK_LAVA_STILL || lavaDepth == 0) return BLOCK_OBSIDIAN;
    return BLOCK_COBBLESTONE;
  };

  auto canFlowInto = [&](int x, int y, int z) {
    if (!inBounds(x, y, z)) return false;
    uint8_t t = getBlock(x, y, z);
    if (t == BLOCK_AIR) return true;
    if (isWaterBlock(t)) return true;
    if (isLavaBlock(t)) return false;
    return !g_blockProps[t].isSolid();
  };

  auto hasDownwardExit = [&](int x, int y, int z) {
    int by = y - 1;
    return by >= 0 && canFlowInto(x, by, z);
  };

  static const int flowDx[4] = {-1, 1, 0, 0};
  static const int flowDz[4] = {0, 0, -1, 1};
  static const int oppositeDir[4] = {1, 0, 3, 2};
  const int maxFlowSearch = 4;

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

  for (int y = simMaxY; y >= simMinY && !budgetReached; --y) {
    for (int z = simMinZ; z <= simMaxZ; ++z) {
      for (int x = simMinX; x <= simMaxX; ++x) {
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

        // Real-contact water->lava reaction (no path-check side effects):
        // Water touching lava cools lava into obsidian/cobblestone.
        // Exception: flowing lava directly above water is handled by lava tick
        // as "water below lava flow -> stone", so skip that direction here.
        static const int rx[6] = {-1, 1, 0, 0, 0, 0};
        static const int ry[6] = {0, 0, -1, 1, 0, 0};
        static const int rz[6] = {0, 0, 0, 0, -1, 1};
        for (int i = 0; i < 6; ++i) {
          int nx = x + rx[i], ny = y + ry[i], nz = z + rz[i];
          if (!inBounds(nx, ny, nz)) continue;
          uint8_t neighbor = getBlock(nx, ny, nz);
          if (!isLavaBlock(neighbor)) continue;
          if (ny == y + 1 && neighbor == BLOCK_LAVA_FLOW) continue;
          uint8_t lavaDepth = getLavaDepth(nx, ny, nz);
          if (lavaDepth == 0xFF) lavaDepth = (neighbor == BLOCK_LAVA_STILL) ? 0 : 1;
          queueSet(nx, ny, nz, waterReactionBlock(neighbor, lavaDepth), 0xFF);
        }

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
            bool useShortestPathPriority = true;
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
    }
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
    if (isLavaBlock(op.id)) setLavaDepth(op.x, op.y, op.z, (op.id == BLOCK_LAVA_STILL) ? 0 : 1);
    else setLavaDepth(op.x, op.y, op.z, 0xFF);
    if (isLavaBlock(cur) || isLavaBlock(op.id)) {
      m_lavaDirty = true;
      m_lavaWakeX = op.x;
      m_lavaWakeY = op.y;
      m_lavaWakeZ = op.z;
      m_lavaWakeTicks = 24;
      for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy)
          for (int dz = -1; dz <= 1; ++dz)
            if ((dx == 0) + (dy == 0) + (dz == 0) >= 2) enqueueLavaCell(op.x + dx, op.y + dy, op.z + dz);
    }
    for (int dx = -1; dx <= 1; ++dx)
      for (int dy = -1; dy <= 1; ++dy)
        for (int dz = -1; dz <= 1; ++dz)
          if ((dx == 0) + (dy == 0) + (dz == 0) >= 2) enqueueWaterCell(op.x + dx, op.y + dy, op.z + dz);
  }
  m_waterDirty = !m_waterQueue.empty() || !ops.empty();
}

void Level::tickLava() {
  struct LavaOp {
    int x, y, z;
    uint8_t id;
    uint8_t depth;
  };
  std::vector<LavaOp> ops;
  ops.reserve(2048);
  std::unordered_map<int, size_t> opIndex;
  opIndex.reserve(2048);
  const int maxLavaCellsPerTick = 256;
  int processedLavaCells = 0;
  bool budgetReached = false;

  const int maxX = WORLD_CHUNKS_X * CHUNK_SIZE_X;
  const int maxZ = WORLD_CHUNKS_Z * CHUNK_SIZE_Z;
  int simMinY = 0;
  int simMaxY = CHUNK_SIZE_Y - 1;
  int simMinX = 0;
  int simMaxX = maxX - 1;
  int simMinZ = 0;
  int simMaxZ = maxZ - 1;
  bool hasQueuedFocus = false;
  int seeds = 0;
  const int maxSeedsPerTick = 128;
  while (!m_lavaQueue.empty() && seeds < maxSeedsPerTick) {
    int idx = m_lavaQueue.front();
    m_lavaQueue.pop_front();
    if (idx < 0 || idx >= (int)m_lavaQueued.size()) continue;
    m_lavaQueued[idx] = 0;
    int x = idx % maxX;
    int yz = idx / maxX;
    int z = yz % maxZ;
    int y = yz / maxZ;
    if (!hasQueuedFocus) {
      simMinX = simMaxX = x;
      simMinY = simMaxY = y;
      simMinZ = simMaxZ = z;
      hasQueuedFocus = true;
    } else {
      if (x < simMinX) simMinX = x;
      if (x > simMaxX) simMaxX = x;
      if (y < simMinY) simMinY = y;
      if (y > simMaxY) simMaxY = y;
      if (z < simMinZ) simMinZ = z;
      if (z > simMaxZ) simMaxZ = z;
    }
    seeds++;
  }
  if (!hasQueuedFocus) {
    m_lavaDirty = false;
    return;
  }
  const int expand = m_lavaWakeRadius;
  simMinX = (simMinX - expand < 0) ? 0 : simMinX - expand;
  simMaxX = (simMaxX + expand >= maxX) ? (maxX - 1) : simMaxX + expand;
  simMinZ = (simMinZ - expand < 0) ? 0 : simMinZ - expand;
  simMaxZ = (simMaxZ + expand >= maxZ) ? (maxZ - 1) : simMaxZ + expand;
  simMinY = (simMinY - expand < 0) ? 0 : simMinY - expand;
  simMaxY = (simMaxY + expand >= CHUNK_SIZE_Y) ? (CHUNK_SIZE_Y - 1) : simMaxY + expand;

  auto inBounds = [&](int x, int y, int z) {
    return x >= 0 && x < maxX && z >= 0 && z < maxZ && y >= 0 && y < CHUNK_SIZE_Y;
  };

  auto queueSet = [&](int x, int y, int z, uint8_t id, uint8_t depth) {
    if (!inBounds(x, y, z)) return;
    int key = ((y * maxZ + z) * maxX + x);
    auto it = opIndex.find(key);
    if (it != opIndex.end()) {
      LavaOp &pending = ops[it->second];
      if (pending.id == id && pending.depth == depth) return;
      pending.id = id;
      pending.depth = depth;
      return;
    }
    uint8_t cur = getBlock(x, y, z);
    uint8_t curDepth = getLavaDepth(x, y, z);
    if (cur == id && curDepth == depth) return;
    opIndex[key] = ops.size();
    ops.push_back({x, y, z, id, depth});
  };

  auto waterReactionBlock = [&](uint8_t lavaId, uint8_t lavaDepth) -> uint8_t {
    if (lavaId == BLOCK_LAVA_STILL || lavaDepth == 0) return BLOCK_OBSIDIAN;
    return BLOCK_COBBLESTONE;
  };

  auto canFlowInto = [&](int x, int y, int z) {
    if (!inBounds(x, y, z)) return false;
    uint8_t t = getBlock(x, y, z);
    if (t == BLOCK_AIR) return true;
    if (isLavaBlock(t)) return true;
    if (isWaterBlock(t)) return false;
    return !g_blockProps[t].isSolid();
  };

  static const int flowDx[4] = {-1, 1, 0, 0};
  static const int flowDz[4] = {0, 0, -1, 1};

  for (int y = simMaxY; y >= simMinY && !budgetReached; --y) {
    for (int z = simMinZ; z <= simMaxZ; ++z) {
      for (int x = simMinX; x <= simMaxX; ++x) {
        uint8_t id = getBlock(x, y, z);
        if (!isLavaBlock(id)) continue;
        if (++processedLavaCells > maxLavaCellsPerTick) {
          budgetReached = true;
          break;
        }

        uint8_t curDepth = getLavaDepth(x, y, z);
        if (curDepth == 0xFF) curDepth = (id == BLOCK_LAVA_STILL) ? 0 : 1;
        if (id == BLOCK_LAVA_STILL) curDepth = 0;

        bool cooled = false;
        static const int rx[6] = {-1, 1, 0, 0, 0, 0};
        static const int ry[6] = {0, 0, -1, 1, 0, 0};
        static const int rz[6] = {0, 0, 0, 0, -1, 1};
        for (int i = 0; i < 6; ++i) {
          int nx = x + rx[i], ny = y + ry[i], nz = z + rz[i];
          if (!inBounds(nx, ny, nz)) continue;
          uint8_t neighbor = getBlock(nx, ny, nz);
          if (isWaterBlock(neighbor)) {
            // MC-like case: flowing lava directly above water turns that water
            // into stone, but the flowing lava block itself should not cool
            // into cobblestone/obsidian from this downward contact alone.
            if (id == BLOCK_LAVA_FLOW && ny == y - 1) {
              queueSet(nx, ny, nz, BLOCK_STONE, 0xFF);
              continue;
            }
            queueSet(nx, ny, nz, BLOCK_STONE, 0xFF);
            queueSet(x, y, z, waterReactionBlock(id, curDepth), 0xFF);
            cooled = true;
            break;
          }
        }
        if (cooled) continue;

        int by = y - 1;
        bool flowedDown = false;
        if (by >= 0 && canFlowInto(x, by, z)) {
          if (!isWaterBlock(getBlock(x, by, z))) {
            queueSet(x, by, z, BLOCK_LAVA_FLOW, 1);
          }
          flowedDown = true;
        }

        uint8_t neighborMin = 7;
        bool hasLavaNeighbor = false;
        bool hasLavaAbove = isLavaBlock(getBlock(x, y + 1, z));
        const uint8_t maxLavaDistance = 4;
        for (int i = 0; i < 4; ++i) {
          int nx = x + flowDx[i], nz = z + flowDz[i];
          if (!inBounds(nx, y, nz)) continue;
          uint8_t nId = getBlock(nx, y, nz);
          if (isLavaBlock(nId)) {
            hasLavaNeighbor = true;
            uint8_t nd = getLavaDepth(nx, y, nz);
            if (nd == 0xFF) nd = (nId == BLOCK_LAVA_STILL) ? 0 : 1;
            if (nd != 0xFF && nd < neighborMin) neighborMin = nd;
          } else if (isWaterBlock(nId)) {
            queueSet(nx, y, nz, BLOCK_STONE, 0xFF);
          }
        }

        uint8_t nextDepth = curDepth;
        if (id == BLOCK_LAVA_STILL) {
          nextDepth = 0;
        } else {
          if (hasLavaAbove) nextDepth = 1;
          else if (neighborMin < maxLavaDistance) nextDepth = (uint8_t)(neighborMin + 1);
          else nextDepth = 8;
        }

        if (nextDepth <= 7) {
          uint8_t spreadDepth = (id == BLOCK_LAVA_STILL) ? 1 : (uint8_t)(nextDepth + 1);
          if (spreadDepth <= maxLavaDistance && !flowedDown) {
            for (int i = 0; i < 4; ++i) {
              int nx = x + flowDx[i], nz = z + flowDz[i];
              if (!inBounds(nx, y, nz)) continue;
              if (!canFlowInto(nx, y, nz)) continue;
              uint8_t nId = getBlock(nx, y, nz);
              if (isWaterBlock(nId)) continue;
              uint8_t nDepth = getLavaDepth(nx, y, nz);
              if (!isLavaBlock(nId) || nDepth == 0xFF || nDepth > spreadDepth) {
                queueSet(nx, y, nz, BLOCK_LAVA_FLOW, spreadDepth);
              }
            }
          }
          if (id == BLOCK_LAVA_FLOW) {
            if (nextDepth >= 8 || (!hasLavaAbove && !hasLavaNeighbor && !flowedDown)) {
              queueSet(x, y, z, BLOCK_AIR, 0xFF);
            } else {
              queueSet(x, y, z, (nextDepth == 0) ? BLOCK_LAVA_STILL : BLOCK_LAVA_FLOW, nextDepth);
            }
          } else {
            queueSet(x, y, z, BLOCK_LAVA_STILL, 0);
          }
        } else {
          queueSet(x, y, z, BLOCK_AIR, 0xFF);
        }
      }
    }
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
    setLavaDepth(op.x, op.y, op.z, op.depth);
    if (isWaterBlock(op.id)) setWaterDepth(op.x, op.y, op.z, (op.id == BLOCK_WATER_STILL) ? 0 : 1);
    else setWaterDepth(op.x, op.y, op.z, 0xFF);
    if (isWaterBlock(cur) || isWaterBlock(op.id)) {
      m_waterDirty = true;
      m_waterWakeX = op.x;
      m_waterWakeY = op.y;
      m_waterWakeZ = op.z;
      m_waterWakeTicks = 16;
      for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy)
          for (int dz = -1; dz <= 1; ++dz)
            if ((dx == 0) + (dy == 0) + (dz == 0) >= 2) enqueueWaterCell(op.x + dx, op.y + dy, op.z + dz);
    }
    for (int dx = -1; dx <= 1; ++dx)
      for (int dy = -1; dy <= 1; ++dy)
        for (int dz = -1; dz <= 1; ++dz)
          if ((dx == 0) + (dy == 0) + (dz == 0) >= 2) enqueueLavaCell(op.x + dx, op.y + dy, op.z + dz);
  }
  m_lavaDirty = !m_lavaQueue.empty() || !ops.empty();
}

Level::Level() {
  memset(m_chunks, 0, sizeof(m_chunks));
  m_waterDepth.resize(WORLD_CHUNKS_X * CHUNK_SIZE_X * CHUNK_SIZE_Y * WORLD_CHUNKS_Z * CHUNK_SIZE_Z, 0xFF);
  m_lavaDepth.resize(WORLD_CHUNKS_X * CHUNK_SIZE_X * CHUNK_SIZE_Y * WORLD_CHUNKS_Z * CHUNK_SIZE_Z, 0xFF);
  m_waterQueued.resize(m_waterDepth.size(), 0);
  m_lavaQueued.resize(m_lavaDepth.size(), 0);
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

int Level::lavaIndex(int wx, int wy, int wz) const {
  return ((wy * (WORLD_CHUNKS_Z * CHUNK_SIZE_Z) + wz) * (WORLD_CHUNKS_X * CHUNK_SIZE_X) + wx);
}

void Level::enqueueWaterCell(int wx, int wy, int wz) {
  int maxX = WORLD_CHUNKS_X * CHUNK_SIZE_X;
  int maxZ = WORLD_CHUNKS_Z * CHUNK_SIZE_Z;
  if (wx < 0 || wx >= maxX || wz < 0 || wz >= maxZ || wy < 0 || wy >= CHUNK_SIZE_Y) return;
  int idx = waterIndex(wx, wy, wz);
  if (idx < 0 || idx >= (int)m_waterQueued.size()) return;
  if (m_waterQueued[idx]) return;
  m_waterQueued[idx] = 1;
  m_waterQueue.push_back(idx);
  m_waterDirty = true;
}

void Level::enqueueLavaCell(int wx, int wy, int wz) {
  int maxX = WORLD_CHUNKS_X * CHUNK_SIZE_X;
  int maxZ = WORLD_CHUNKS_Z * CHUNK_SIZE_Z;
  if (wx < 0 || wx >= maxX || wz < 0 || wz >= maxZ || wy < 0 || wy >= CHUNK_SIZE_Y) return;
  int idx = lavaIndex(wx, wy, wz);
  if (idx < 0 || idx >= (int)m_lavaQueued.size()) return;
  if (m_lavaQueued[idx]) return;
  m_lavaQueued[idx] = 1;
  m_lavaQueue.push_back(idx);
  m_lavaDirty = true;
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

uint8_t Level::getLavaDepth(int wx, int wy, int wz) const {
  int maxX = WORLD_CHUNKS_X * CHUNK_SIZE_X;
  int maxZ = WORLD_CHUNKS_Z * CHUNK_SIZE_Z;
  if (wx < 0 || wx >= maxX || wz < 0 || wz >= maxZ || wy < 0 || wy >= CHUNK_SIZE_Y) return 0xFF;
  return m_lavaDepth[lavaIndex(wx, wy, wz)];
}

void Level::setLavaDepth(int wx, int wy, int wz, uint8_t depth) {
  int maxX = WORLD_CHUNKS_X * CHUNK_SIZE_X;
  int maxZ = WORLD_CHUNKS_Z * CHUNK_SIZE_Z;
  if (wx < 0 || wx >= maxX || wz < 0 || wz >= maxZ || wy < 0 || wy >= CHUNK_SIZE_Y) return;
  m_lavaDepth[lavaIndex(wx, wy, wz)] = depth;
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
  if (isLavaBlock(id)) {
    setLavaDepth(wx, wy, wz, (id == BLOCK_LAVA_STILL) ? 0 : 1);
  } else {
    setLavaDepth(wx, wy, wz, 0xFF);
  }

  bool touchesWater = isWaterBlock(oldId) || isWaterBlock(id);
  bool touchesLava = isLavaBlock(oldId) || isLavaBlock(id);
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
  if (!touchesLava) {
    static const int dx[6] = {-1, 1, 0, 0, 0, 0};
    static const int dy[6] = {0, 0, -1, 1, 0, 0};
    static const int dz[6] = {0, 0, 0, 0, -1, 1};
    for (int i = 0; i < 6; ++i) {
      int nx = wx + dx[i], ny = wy + dy[i], nz = wz + dz[i];
      if (ny < 0 || ny >= CHUNK_SIZE_Y) continue;
      if (isLavaBlock(getBlock(nx, ny, nz))) {
        touchesLava = true;
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
    for (int dx = -1; dx <= 1; ++dx)
      for (int dy = -1; dy <= 1; ++dy)
        for (int dz = -1; dz <= 1; ++dz)
          if ((dx == 0) + (dy == 0) + (dz == 0) >= 2) enqueueWaterCell(wx + dx, wy + dy, wz + dz);
  }
  if (touchesLava) {
    m_lavaDirty = true;
    m_lavaWakeX = wx;
    m_lavaWakeY = wy;
    m_lavaWakeZ = wz;
    m_lavaWakeTicks = 24;
    for (int dx = -1; dx <= 1; ++dx)
      for (int dy = -1; dy <= 1; ++dy)
        for (int dz = -1; dz <= 1; ++dz)
          if ((dx == 0) + (dy == 0) + (dz == 0) >= 2) enqueueLavaCell(wx + dx, wy + dy, wz + dz);
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

bool Level::saveToFile(const char *path) const {
  if (!path) return false;
  FILE *f = fopen(path, "wb");
  if (!f) return false;

  struct SaveHeader {
    char magic[8];
    uint32_t version;
    uint32_t chunksX;
    uint32_t chunksZ;
    uint32_t chunkY;
    int64_t time;
    uint32_t waterSize;
    uint32_t lavaSize;
  } hdr;

  memcpy(hdr.magic, "MCPSPWLD", 8);
  hdr.version = 3;
  hdr.chunksX = WORLD_CHUNKS_X;
  hdr.chunksZ = WORLD_CHUNKS_Z;
  hdr.chunkY = CHUNK_SIZE_Y;
  hdr.time = m_time;
  hdr.waterSize = (uint32_t)m_waterDepth.size();
  hdr.lavaSize = (uint32_t)m_lavaDepth.size();
  if (fwrite(&hdr, sizeof(hdr), 1, f) != 1) { fclose(f); return false; }

  for (int cx = 0; cx < WORLD_CHUNKS_X; ++cx) {
    for (int cz = 0; cz < WORLD_CHUNKS_Z; ++cz) {
      const Chunk *ch = m_chunks[cx][cz];
      if (fwrite(ch->blocks, sizeof(ch->blocks), 1, f) != 1) { fclose(f); return false; }
      if (fwrite(ch->light, sizeof(ch->light), 1, f) != 1) { fclose(f); return false; }
    }
  }

  if (!m_waterDepth.empty()) {
    if (fwrite(m_waterDepth.data(), 1, m_waterDepth.size(), f) != m_waterDepth.size()) {
      fclose(f);
      return false;
    }
  }
  if (!m_lavaDepth.empty()) {
    if (fwrite(m_lavaDepth.data(), 1, m_lavaDepth.size(), f) != m_lavaDepth.size()) {
      fclose(f);
      return false;
    }
  }

  fflush(f);
  fclose(f);
  return true;
}

bool Level::loadFromFile(const char *path) {
  if (!path) return false;
  FILE *f = fopen(path, "rb");
  if (!f) return false;

  struct SaveHeaderV2 {
    char magic[8];
    uint32_t version;
    uint32_t chunksX;
    uint32_t chunksZ;
    uint32_t chunkY;
    int64_t time;
    uint32_t waterSize;
  };
  struct SaveHeaderV3 {
    char magic[8];
    uint32_t version;
    uint32_t chunksX;
    uint32_t chunksZ;
    uint32_t chunkY;
    int64_t time;
    uint32_t waterSize;
    uint32_t lavaSize;
  };
  SaveHeaderV3 hdr;
  memset(&hdr, 0, sizeof(hdr));

  uint32_t version = 0;
  if (fseek(f, 8, SEEK_SET) != 0) { fclose(f); return false; }
  if (fread(&version, sizeof(version), 1, f) != 1) { fclose(f); return false; }
  if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return false; }

  if (version >= 3) {
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) { fclose(f); return false; }
  } else {
    SaveHeaderV2 oldHdr;
    if (fread(&oldHdr, sizeof(oldHdr), 1, f) != 1) { fclose(f); return false; }
    memcpy(hdr.magic, oldHdr.magic, sizeof(hdr.magic));
    hdr.version = oldHdr.version;
    hdr.chunksX = oldHdr.chunksX;
    hdr.chunksZ = oldHdr.chunksZ;
    hdr.chunkY = oldHdr.chunkY;
    hdr.time = oldHdr.time;
    hdr.waterSize = oldHdr.waterSize;
    hdr.lavaSize = (uint32_t)m_lavaDepth.size();
  }
  if (memcmp(hdr.magic, "MCPSPWLD", 8) != 0 || (hdr.version != 1 && hdr.version != 2 && hdr.version != 3)) { fclose(f); return false; }
  if (hdr.chunksX != WORLD_CHUNKS_X || hdr.chunksZ != WORLD_CHUNKS_Z || hdr.chunkY != CHUNK_SIZE_Y) { fclose(f); return false; }
  if (hdr.waterSize != m_waterDepth.size()) { fclose(f); return false; }
  if (hdr.version >= 3 && hdr.lavaSize != m_lavaDepth.size()) { fclose(f); return false; }

  for (int cx = 0; cx < WORLD_CHUNKS_X; ++cx) {
    for (int cz = 0; cz < WORLD_CHUNKS_Z; ++cz) {
      Chunk *ch = m_chunks[cx][cz];
      ch->cx = cx;
      ch->cz = cz;
      if (fread(ch->blocks, sizeof(ch->blocks), 1, f) != 1) { fclose(f); return false; }
      if (fread(ch->light, sizeof(ch->light), 1, f) != 1) { fclose(f); return false; }
      for (int sy = 0; sy < 4; ++sy) ch->dirty[sy] = true;
    }
  }

  if (!m_waterDepth.empty()) {
    if (fread(m_waterDepth.data(), 1, m_waterDepth.size(), f) != m_waterDepth.size()) {
      fclose(f);
      return false;
    }
  }
  if (hdr.version >= 3 && !m_lavaDepth.empty()) {
    if (fread(m_lavaDepth.data(), 1, m_lavaDepth.size(), f) != m_lavaDepth.size()) {
      fclose(f);
      return false;
    }
  } else {
    for (int y = 0; y < CHUNK_SIZE_Y; ++y) {
      for (int z = 0; z < WORLD_CHUNKS_Z * CHUNK_SIZE_Z; ++z) {
        for (int x = 0; x < WORLD_CHUNKS_X * CHUNK_SIZE_X; ++x) {
          uint8_t id = getBlock(x, y, z);
          if (id == BLOCK_LAVA_STILL) setLavaDepth(x, y, z, 0);
          else if (id == BLOCK_LAVA_FLOW) setLavaDepth(x, y, z, 1);
          else setLavaDepth(x, y, z, 0xFF);
        }
      }
    }
  }

  fclose(f);
  m_time = hdr.time;
  std::fill(m_waterQueued.begin(), m_waterQueued.end(), 0);
  std::fill(m_lavaQueued.begin(), m_lavaQueued.end(), 0);
  m_waterQueue.clear();
  m_lavaQueue.clear();
  for (int y = 0; y < CHUNK_SIZE_Y; ++y) {
    for (int z = 0; z < WORLD_CHUNKS_Z * CHUNK_SIZE_Z; ++z) {
      for (int x = 0; x < WORLD_CHUNKS_X * CHUNK_SIZE_X; ++x) {
        uint8_t id = getBlock(x, y, z);
        if (isWaterBlock(id)) enqueueWaterCell(x, y, z);
        if (isLavaBlock(id)) enqueueLavaCell(x, y, z);
      }
    }
  }
  m_waterDirty = true;
  m_lavaDirty = true;
  return true;
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
        if (id == BLOCK_LAVA_STILL) setLavaDepth(x, y, z, 0);
        else if (id == BLOCK_LAVA_FLOW) setLavaDepth(x, y, z, 1);
        else setLavaDepth(x, y, z, 0xFF);
      }
    }
  }
  std::fill(m_waterQueued.begin(), m_waterQueued.end(), 0);
  std::fill(m_lavaQueued.begin(), m_lavaQueued.end(), 0);
  m_waterQueue.clear();
  m_lavaQueue.clear();
  for (int y = 0; y < CHUNK_SIZE_Y; ++y) {
    for (int z = 0; z < WORLD_CHUNKS_Z * CHUNK_SIZE_Z; ++z) {
      for (int x = 0; x < WORLD_CHUNKS_X * CHUNK_SIZE_X; ++x) {
        uint8_t id = getBlock(x, y, z);
        if (isWaterBlock(id)) enqueueWaterCell(x, y, z);
        if (isLavaBlock(id)) enqueueLavaCell(x, y, z);
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
