// The interface of nav to Recast, Detour and DetourCrowd (zlib, taken by [native.recast] of
// sosu.toml): functions of C over numbers and pointers. A mesh is built as the RecastDemo builds a
// single tile (Sample_SoloMesh): the triangles rasterized into voxels, what an agent can stand on
// kept, cut into regions, then polygons, then the data of Detour, which the query and the crowds
// read. The data is kept as bytes too, for `nav.save`.

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "DetourCommon.h"
#include "DetourCrowd.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "DetourNavMeshQuery.h"
#include "Recast.h"

namespace {

const int MAX_POLYS = 2048;  // of a path
const unsigned short WALK = 1;

struct Mesh {
    dtNavMesh *nav = nullptr;
    dtNavMeshQuery *query = nullptr;
    dtQueryFilter filter;
    unsigned char *bytes = nullptr;  // a copy of the data, for save
    int size = 0;
};

static Mesh *open_data(const unsigned char *data, int size) {
    if (size <= 0) return nullptr;
    unsigned char *owned = (unsigned char *)dtAlloc((size_t)size, DT_ALLOC_PERM);
    if (!owned) return nullptr;
    std::memcpy(owned, data, (size_t)size);
    Mesh *m = new Mesh();
    m->nav = dtAllocNavMesh();
    if (!m->nav || dtStatusFailed(m->nav->init(owned, size, DT_TILE_FREE_DATA))) {
        if (m->nav) {
            dtFreeNavMesh(m->nav);
        } else {
            dtFree(owned);
        }
        delete m;
        return nullptr;
    }
    m->query = dtAllocNavMeshQuery();
    if (!m->query || dtStatusFailed(m->query->init(m->nav, MAX_POLYS))) {
        dtFreeNavMeshQuery(m->query);
        dtFreeNavMesh(m->nav);
        delete m;
        return nullptr;
    }
    m->filter.setIncludeFlags(WALK);
    m->filter.setExcludeFlags(0);
    m->bytes = (unsigned char *)std::malloc((size_t)size);
    if (m->bytes) std::memcpy(m->bytes, data, (size_t)size);
    m->size = m->bytes ? size : 0;
    return m;
}

struct Build {
    rcContext context{ false };
    rcHeightfield *solid = nullptr;
    unsigned char *areas = nullptr;
    rcCompactHeightfield *compact = nullptr;
    rcContourSet *contours = nullptr;
    rcPolyMesh *polys = nullptr;
    rcPolyMeshDetail *detail = nullptr;
    ~Build() {
        rcFreeHeightField(solid);
        delete[] areas;
        rcFreeCompactHeightfield(compact);
        rcFreeContourSet(contours);
        rcFreePolyMesh(polys);
        rcFreePolyMeshDetail(detail);
    }
};

} // namespace

extern "C" {

// The mesh of the triangles (`indices`, three by triangle, into `xyz`) for an agent of `radius`,
// `height`, that climbs `climb` and walks up `slope` degrees, on voxels of `cell` by `cell_height`;
// null (and *stage set) when a stage of Recast fails or nothing can be walked on: 1 the sizes,
// 2 voxels, 3 regions, 4 contours, 5 polygons, 6 the data of Detour, 7 nothing walkable.
void *sosu_nav_build(const float *xyz, int vertices, const int *indices, int index_count, float radius, float height,
                     float climb, float slope, float cell, float cell_height, int *stage) {
    *stage = 1;
    if (vertices < 3 || index_count < 3 || index_count % 3 != 0 || !(cell > 0.0f) || !(cell_height > 0.0f) ||
        !(height > 0.0f) || radius < 0.0f || climb < 0.0f) {
        return nullptr;
    }
    for (int i = 0; i < index_count; i++) {
        if (indices[i] < 0 || indices[i] >= vertices) return nullptr;
    }
    int triangles = index_count / 3;
    rcConfig cfg;
    std::memset(&cfg, 0, sizeof cfg);
    cfg.cs = cell;
    cfg.ch = cell_height;
    cfg.walkableSlopeAngle = slope;
    cfg.walkableHeight = (int)std::ceil(height / cell_height);
    cfg.walkableClimb = (int)std::floor(climb / cell_height);
    cfg.walkableRadius = (int)std::ceil(radius / cell);
    cfg.maxEdgeLen = (int)(12.0f / cell);
    cfg.maxSimplificationError = 1.3f;
    cfg.minRegionArea = 8 * 8;
    cfg.mergeRegionArea = 20 * 20;
    cfg.maxVertsPerPoly = 6;
    cfg.detailSampleDist = cell * 6.0f;
    cfg.detailSampleMaxError = cell_height;
    rcCalcBounds(xyz, vertices, cfg.bmin, cfg.bmax);
    rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);
    if (cfg.width <= 0 || cfg.height <= 0 || (long long)cfg.width * cfg.height > 4096LL * 4096LL) return nullptr;

    Build b;
    *stage = 2;
    b.solid = rcAllocHeightfield();
    if (!b.solid || !rcCreateHeightfield(&b.context, *b.solid, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch)) {
        return nullptr;
    }
    b.areas = new unsigned char[(size_t)triangles];
    std::memset(b.areas, 0, (size_t)triangles);
    rcMarkWalkableTriangles(&b.context, cfg.walkableSlopeAngle, xyz, vertices, indices, triangles, b.areas);
    if (!rcRasterizeTriangles(&b.context, xyz, vertices, indices, b.areas, triangles, *b.solid, cfg.walkableClimb)) {
        return nullptr;
    }
    rcFilterLowHangingWalkableObstacles(&b.context, cfg.walkableClimb, *b.solid);
    rcFilterLedgeSpans(&b.context, cfg.walkableHeight, cfg.walkableClimb, *b.solid);
    rcFilterWalkableLowHeightSpans(&b.context, cfg.walkableHeight, *b.solid);
    b.compact = rcAllocCompactHeightfield();
    if (!b.compact || !rcBuildCompactHeightfield(&b.context, cfg.walkableHeight, cfg.walkableClimb, *b.solid, *b.compact) ||
        !rcErodeWalkableArea(&b.context, cfg.walkableRadius, *b.compact)) {
        return nullptr;
    }
    *stage = 3;
    if (!rcBuildDistanceField(&b.context, *b.compact) ||
        !rcBuildRegions(&b.context, *b.compact, 0, cfg.minRegionArea, cfg.mergeRegionArea)) {
        return nullptr;
    }
    *stage = 4;
    b.contours = rcAllocContourSet();
    if (!b.contours || !rcBuildContours(&b.context, *b.compact, cfg.maxSimplificationError, cfg.maxEdgeLen, *b.contours)) {
        return nullptr;
    }
    *stage = 5;
    b.polys = rcAllocPolyMesh();
    b.detail = rcAllocPolyMeshDetail();
    if (!b.polys || !rcBuildPolyMesh(&b.context, *b.contours, cfg.maxVertsPerPoly, *b.polys) || !b.detail ||
        !rcBuildPolyMeshDetail(&b.context, *b.polys, *b.compact, cfg.detailSampleDist, cfg.detailSampleMaxError, *b.detail)) {
        return nullptr;
    }
    *stage = 7;
    if (b.polys->npolys == 0) return nullptr;
    if (b.polys->nverts >= 0xffff) {
        *stage = 6;
        return nullptr;
    }
    for (int i = 0; i < b.polys->npolys; i++) {
        b.polys->flags[i] = b.polys->areas[i] == RC_WALKABLE_AREA ? WALK : 0;
    }
    *stage = 6;
    dtNavMeshCreateParams params;
    std::memset(&params, 0, sizeof params);
    params.verts = b.polys->verts;
    params.vertCount = b.polys->nverts;
    params.polys = b.polys->polys;
    params.polyAreas = b.polys->areas;
    params.polyFlags = b.polys->flags;
    params.polyCount = b.polys->npolys;
    params.nvp = b.polys->nvp;
    params.detailMeshes = b.detail->meshes;
    params.detailVerts = b.detail->verts;
    params.detailVertsCount = b.detail->nverts;
    params.detailTris = b.detail->tris;
    params.detailTriCount = b.detail->ntris;
    params.walkableHeight = height;
    params.walkableRadius = radius;
    params.walkableClimb = climb;
    rcVcopy(params.bmin, b.polys->bmin);
    rcVcopy(params.bmax, b.polys->bmax);
    params.cs = cfg.cs;
    params.ch = cfg.ch;
    params.buildBvTree = true;
    unsigned char *data = nullptr;
    int size = 0;
    if (!dtCreateNavMeshData(&params, &data, &size)) return nullptr;
    Mesh *m = open_data(data, size);
    dtFree(data);
    if (m) *stage = 0;
    return m;
}

// A mesh from the bytes of `sosu_nav_data`; null when they are not the data of Detour. Only the
// header is checked: the bytes must come from `nav.save` of this version, not from someone else.
void *sosu_nav_load(const unsigned char *data, int size) {
    if (size < (int)sizeof(dtMeshHeader)) return nullptr;
    dtMeshHeader header;
    std::memcpy(&header, data, sizeof header);
    if (header.magic != DT_NAVMESH_MAGIC || header.version != DT_NAVMESH_VERSION) return nullptr;
    return open_data(data, size);
}

int sosu_nav_size(void *mesh) {
    return ((Mesh *)mesh)->size;
}

const unsigned char *sosu_nav_data(void *mesh) {
    return ((Mesh *)mesh)->bytes;
}

int sosu_nav_polygons(void *mesh) {
    const dtNavMesh *nav = ((const Mesh *)mesh)->nav;
    const dtMeshTile *tile = nav->getTile(0);
    return tile && tile->header ? tile->header->polyCount : 0;
}

void sosu_nav_free(void *mesh) {
    Mesh *m = (Mesh *)mesh;
    if (!m) return;
    dtFreeNavMeshQuery(m->query);
    dtFreeNavMesh(m->nav);
    std::free(m->bytes);
    delete m;
}

// The closest point of the mesh to `p`, within `reach` across and twice that up and down: 1 and
// `out` set, or 0.
int sosu_nav_nearest(void *mesh, const float *p, float reach, float *out) {
    Mesh *m = (Mesh *)mesh;
    float extents[3] = { reach, reach * 2.0f, reach };
    dtPolyRef ref = 0;
    if (dtStatusFailed(m->query->findNearestPoly(p, extents, &m->filter, &ref, out)) || ref == 0) return 0;
    return 1;
}

// The corners of the shortest path from `from` to `to` (each brought to the mesh within `reach`),
// up to `capacity`: their count, 0 when either point is off the mesh, -1 when the two are not
// connected (Detour finds only a partial path).
int sosu_nav_path(void *mesh, const float *from, const float *to, float reach, float *corners, int capacity) {
    Mesh *m = (Mesh *)mesh;
    float extents[3] = { reach, reach * 2.0f, reach };
    dtPolyRef start = 0, end = 0;
    float a[3], b[3];
    if (dtStatusFailed(m->query->findNearestPoly(from, extents, &m->filter, &start, a)) || start == 0) return 0;
    if (dtStatusFailed(m->query->findNearestPoly(to, extents, &m->filter, &end, b)) || end == 0) return 0;
    dtPolyRef *path = new dtPolyRef[MAX_POLYS];
    int count = 0;
    dtStatus status = m->query->findPath(start, end, a, b, &m->filter, path, &count, MAX_POLYS);
    if (dtStatusFailed(status) || count == 0 || path[count - 1] != end || capacity <= 0) {
        bool connected = !dtStatusFailed(status) && count > 0 && path[count - 1] == end;
        delete[] path;
        return connected ? 0 : -1;
    }
    int corner_count = 0;
    unsigned char *flags = new unsigned char[(size_t)capacity];
    dtPolyRef *refs = new dtPolyRef[(size_t)capacity];
    status = m->query->findStraightPath(a, b, path, count, corners, flags, refs, &corner_count, capacity, 0);
    delete[] flags;
    delete[] refs;
    delete[] path;
    if (dtStatusFailed(status)) return -1;
    return corner_count;
}

// ---- crowds ----

void *sosu_nav_crowd(void *mesh, int max_agents, float max_radius) {
    dtCrowd *crowd = dtAllocCrowd();
    if (!crowd || !crowd->init(max_agents, max_radius, ((Mesh *)mesh)->nav)) {
        dtFreeCrowd(crowd);
        return nullptr;
    }
    crowd->getEditableFilter(0)->setIncludeFlags(WALK);
    return crowd;
}

void sosu_nav_crowd_free(void *crowd) {
    dtFreeCrowd((dtCrowd *)crowd);
}

// An agent at `p` (brought to the mesh); its index, or -1 (the crowd is full, off the mesh).
int sosu_nav_agent(void *crowd, const float *p, float radius, float height, float speed, float acceleration) {
    dtCrowdAgentParams params;
    std::memset(&params, 0, sizeof params);
    params.radius = radius;
    params.height = height;
    params.maxAcceleration = acceleration;
    params.maxSpeed = speed;
    params.collisionQueryRange = radius * 12.0f;
    params.pathOptimizationRange = radius * 30.0f;
    params.updateFlags = DT_CROWD_ANTICIPATE_TURNS | DT_CROWD_OPTIMIZE_VIS | DT_CROWD_OPTIMIZE_TOPO |
                         DT_CROWD_OBSTACLE_AVOIDANCE | DT_CROWD_SEPARATION;
    params.obstacleAvoidanceType = 3;
    params.separationWeight = 2.0f;
    return ((dtCrowd *)crowd)->addAgent(p, &params);
}

void sosu_nav_agent_remove(void *crowd, int agent) {
    ((dtCrowd *)crowd)->removeAgent(agent);
}

// The agent heads for `p` (brought to the mesh): 1, or 0 when it is off the mesh.
int sosu_nav_agent_target(void *crowd, int agent, const float *p) {
    dtCrowd *c = (dtCrowd *)crowd;
    dtPolyRef ref = 0;
    float nearest[3];
    if (dtStatusFailed(c->getNavMeshQuery()->findNearestPoly(p, c->getQueryExtents(), c->getFilter(0), &ref, nearest)) ||
        ref == 0) {
        return 0;
    }
    return c->requestMoveTarget(agent, ref, nearest) ? 1 : 0;
}

void sosu_nav_crowd_update(void *crowd, float dt) {
    ((dtCrowd *)crowd)->update(dt, nullptr);
}

// position, velocity; 1 when the agent is active, else 0
int sosu_nav_agent_state(void *crowd, int agent, float *out) {
    const dtCrowdAgent *a = ((dtCrowd *)crowd)->getAgent(agent);
    if (!a || !a->active) return 0;
    dtVcopy(out, a->npos);
    dtVcopy(out + 3, a->vel);
    return 1;
}

} // extern "C"
