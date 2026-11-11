// The interface of physics to Jolt Physics (MIT, taken by [native.jolt] of sosu.toml): functions of
// C over numbers and pointers, so that Sosu passes no structure by value and never sees C++.
// Bodies are named by their BodyID (index and sequence, 32 bits; 0xFFFFFFFF is none), shapes by a
// pointer that holds one reference. The world steps in the thread that calls it.
//
// Layers: Jolt's object layer is an index into a table of the combinations that bodies use (32
// layers, 32 bits of mask, and whether the body moves), so a body may be on any of 32 layers and
// collide with any of them, while static bodies keep a tree of their own in the broad phase.

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <cstdarg>
#include <cstdint>

using namespace JPH;

namespace {

static void trace(const char *, ...) {}

// Jolt's types and allocator, once for the program: kept until it ends.
static void start() {
    static bool started = false;
    if (started) return;
    started = true;
    RegisterDefaultAllocator();
    Trace = trace;
    Factory::sInstance = new Factory();
    RegisterTypes();
}

struct LayerKind {
    uint32_t layers;
    uint32_t mask;
    bool moving;
};

class BroadPhaseLayers final : public BroadPhaseLayerInterface {
public:
    const Array<LayerKind> *kinds = nullptr;
    uint GetNumBroadPhaseLayers() const override { return 2; }
    BroadPhaseLayer GetBroadPhaseLayer(ObjectLayer layer) const override {
        return BroadPhaseLayer((*kinds)[layer].moving ? 1 : 0);
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char *GetBroadPhaseLayerName(BroadPhaseLayer layer) const override {
        return layer.GetValue() ? "moving" : "static";
    }
#endif
};

class ObjectVsBroadPhase final : public ObjectVsBroadPhaseLayerFilter {
public:
    const Array<LayerKind> *kinds = nullptr;
    bool ShouldCollide(ObjectLayer layer, BroadPhaseLayer tree) const override {
        return (*kinds)[layer].moving || tree.GetValue() == 1;
    }
};

class LayerPairs final : public ObjectLayerPairFilter {
public:
    const Array<LayerKind> *kinds = nullptr;
    bool ShouldCollide(ObjectLayer a, ObjectLayer b) const override {
        const LayerKind &x = (*kinds)[a], &y = (*kinds)[b];
        return (x.moving || y.moving) && (x.layers & y.mask) != 0 && (y.layers & x.mask) != 0;
    }
};

// What a query sees: the bodies on a layer of `mask`.
class QueryLayers final : public ObjectLayerFilter {
public:
    const Array<LayerKind> *kinds;
    uint32_t mask;
    QueryLayers(const Array<LayerKind> *k, uint32_t m) : kinds(k), mask(m) {}
    bool ShouldCollide(ObjectLayer layer) const override { return ((*kinds)[layer].layers & mask) != 0; }
};

struct Event {
    uint32_t a, b;
    int began;
    float point[3];
    float normal[3];
};

class Contacts final : public ContactListener {
public:
    Array<Event> *events = nullptr;
    void OnContactAdded(const Body &a, const Body &b, const ContactManifold &m, ContactSettings &) override {
        Event e{ a.GetID().GetIndexAndSequenceNumber(), b.GetID().GetIndexAndSequenceNumber(), 1, {}, {} };
        RVec3 p = m.GetWorldSpaceContactPointOn1(0);
        e.point[0] = (float)p.GetX();
        e.point[1] = (float)p.GetY();
        e.point[2] = (float)p.GetZ();
        e.normal[0] = m.mWorldSpaceNormal.GetX();
        e.normal[1] = m.mWorldSpaceNormal.GetY();
        e.normal[2] = m.mWorldSpaceNormal.GetZ();
        events->push_back(e);
    }
    void OnContactRemoved(const SubShapeIDPair &pair) override {
        events->push_back(Event{ pair.GetBody1ID().GetIndexAndSequenceNumber(), pair.GetBody2ID().GetIndexAndSequenceNumber(), 0,
                                 {}, {} });
    }
};

struct World {
    JPH_OVERRIDE_NEW_DELETE
    Array<LayerKind> kinds;
    BroadPhaseLayers broad;
    ObjectVsBroadPhase object_vs_broad;
    LayerPairs pairs;
    Contacts listener;
    Array<Event> events;
    PhysicsSystem system;
    TempAllocatorImpl temp{ 16 * 1024 * 1024 };
    JobSystemSingleThreaded jobs{ cMaxPhysicsJobs };
};

static ObjectLayer layer_of(World *w, uint32_t layers, uint32_t mask, bool moving) {
    for (size_t i = 0; i < w->kinds.size(); i++) {
        const LayerKind &k = w->kinds[i];
        if (k.layers == layers && k.mask == mask && k.moving == moving) return (ObjectLayer)i;
    }
    if (w->kinds.size() >= 0xFFFF) return 0; /* the table is full: the first combination */
    w->kinds.push_back(LayerKind{ layers, mask, moving });
    return (ObjectLayer)(w->kinds.size() - 1);
}

static Shape *keep(const ShapeSettings::ShapeResult &result) {
    if (result.HasError()) return nullptr;
    Shape *shape = result.Get().GetPtr();
    shape->AddRef();
    return shape;
}

static void write3(float *out, Vec3 v) {
    out[0] = v.GetX();
    out[1] = v.GetY();
    out[2] = v.GetZ();
}

static void write_r3(float *out, RVec3 v) {
    out[0] = (float)v.GetX();
    out[1] = (float)v.GetY();
    out[2] = (float)v.GetZ();
}

} // namespace

extern "C" {

// ---- the world ----

void *sosu_jolt_world(float gx, float gy, float gz, uint32_t max_bodies) {
    start();
    World *w = new World();
    w->broad.kinds = &w->kinds;
    w->object_vs_broad.kinds = &w->kinds;
    w->pairs.kinds = &w->kinds;
    w->listener.events = &w->events;
    w->kinds.push_back(LayerKind{ 1, 0xFFFFFFFFu, false }); /* layer 0: the default */
    w->system.Init(max_bodies, 0, max_bodies * 2, max_bodies * 2, w->broad, w->object_vs_broad, w->pairs);
    w->system.SetGravity(Vec3(gx, gy, gz));
    w->system.SetContactListener(&w->listener);
    return w;
}

void sosu_jolt_world_free(void *world) {
    World *w = (World *)world;
    if (!w) return;
    BodyInterface &bodies = w->system.GetBodyInterface();
    BodyIDVector ids;
    w->system.GetBodies(ids);
    if (!ids.empty()) {
        bodies.RemoveBodies(ids.data(), (int)ids.size());
        bodies.DestroyBodies(ids.data(), (int)ids.size());
    }
    delete w;
}

void sosu_jolt_set_gravity(void *world, float gx, float gy, float gz) {
    ((World *)world)->system.SetGravity(Vec3(gx, gy, gz));
}

// One step of `dt`, with `collision_steps` passes of collision; the contacts of this step replace
// those of the last one. 0 on success, else Jolt's error (EPhysicsUpdateError: too many bodies in
// contact, pairs, constraints).
int sosu_jolt_step(void *world, float dt, int collision_steps) {
    World *w = (World *)world;
    w->events.clear();
    return (int)w->system.Update(dt, collision_steps, &w->temp, &w->jobs);
}

void sosu_jolt_optimize(void *world) {
    ((World *)world)->system.OptimizeBroadPhase();
}

int sosu_jolt_active_count(void *world) {
    return (int)((World *)world)->system.GetNumActiveBodies(EBodyType::RigidBody);
}

int sosu_jolt_events(void *world) {
    return (int)((World *)world)->events.size();
}

// The event `i` of the last step: its bodies, whether it began, and where (began only).
int sosu_jolt_event(void *world, int i, uint32_t *a, uint32_t *b, float *point_normal) {
    const Event &e = ((World *)world)->events[(size_t)i];
    *a = e.a;
    *b = e.b;
    for (int k = 0; k < 3; k++) {
        point_normal[k] = e.point[k];
        point_normal[3 + k] = e.normal[k];
    }
    return e.began;
}

// ---- shapes: each pointer holds one reference, released by sosu_jolt_shape_release ----

void *sosu_jolt_box(float hx, float hy, float hz, float density) {
    start();
    float smallest = hx < hy ? (hx < hz ? hx : hz) : (hy < hz ? hy : hz);
    if (!(smallest > 0.0f)) return nullptr;
    BoxShapeSettings settings(Vec3(hx, hy, hz), smallest < cDefaultConvexRadius * 2 ? smallest / 2 : cDefaultConvexRadius);
    settings.SetDensity(density);
    return keep(settings.Create());
}

void *sosu_jolt_sphere(float radius, float density) {
    start();
    if (!(radius > 0.0f)) return nullptr;
    SphereShapeSettings settings(radius);
    settings.SetDensity(density);
    return keep(settings.Create());
}

void *sosu_jolt_capsule(float half_height, float radius, float density) {
    start();
    if (!(radius > 0.0f) || !(half_height > 0.0f)) return nullptr;
    CapsuleShapeSettings settings(half_height, radius);
    settings.SetDensity(density);
    return keep(settings.Create());
}

void *sosu_jolt_cylinder(float half_height, float radius, float density) {
    start();
    if (!(radius > 0.0f) || !(half_height > 0.0f)) return nullptr;
    float r = radius < half_height ? radius : half_height;
    CylinderShapeSettings settings(half_height, radius, r < cDefaultConvexRadius * 2 ? r / 2 : cDefaultConvexRadius);
    settings.SetDensity(density);
    return keep(settings.Create());
}

// The convex hull of `count` points (x, y, z); null when they make none (fewer than 4, flat).
void *sosu_jolt_convex(const float *xyz, int count, float density) {
    start();
    if (count < 4) return nullptr;
    Array<Vec3> points;
    for (int i = 0; i < count; i++) points.push_back(Vec3(xyz[3 * i], xyz[3 * i + 1], xyz[3 * i + 2]));
    ConvexHullShapeSettings settings(points);
    settings.SetDensity(density);
    return keep(settings.Create());
}

// Triangles of `vertices` (x, y, z) named by `indices` (three by triangle): for static bodies.
void *sosu_jolt_mesh(const float *xyz, int vertices, const uint32_t *indices, int index_count) {
    start();
    if (vertices < 3 || index_count < 3 || index_count % 3 != 0) return nullptr;
    VertexList list;
    for (int i = 0; i < vertices; i++) list.push_back(Float3(xyz[3 * i], xyz[3 * i + 1], xyz[3 * i + 2]));
    IndexedTriangleList triangles;
    for (int i = 0; i < index_count; i += 3) {
        if (indices[i] >= (uint32_t)vertices || indices[i + 1] >= (uint32_t)vertices || indices[i + 2] >= (uint32_t)vertices) {
            return nullptr;
        }
        triangles.push_back(IndexedTriangle(indices[i], indices[i + 1], indices[i + 2]));
    }
    MeshShapeSettings settings(list, triangles);
    return keep(settings.Create());
}

// A grid of `size` by `size` heights (rows along z), `cell` meters apart, its corner at `offset`.
void *sosu_jolt_heightfield(const float *heights, uint32_t size, float cell, float ox, float oy, float oz) {
    start();
    if (size < 4 || size % 2 != 0 || !(cell > 0.0f)) return nullptr;
    HeightFieldShapeSettings settings(heights, Vec3(ox, oy, oz), Vec3(cell, 1.0f, cell), size);
    return keep(settings.Create());
}

void *sosu_jolt_compound_new(void) {
    start();
    return new StaticCompoundShapeSettings();
}

void sosu_jolt_compound_add(void *compound, void *shape, float px, float py, float pz, float qx, float qy, float qz,
                            float qw) {
    ((StaticCompoundShapeSettings *)compound)->AddShape(Vec3(px, py, pz), Quat(qx, qy, qz, qw).Normalized(), (const Shape *)shape);
}

// The compound of what was added (its settings released); null when it holds nothing.
void *sosu_jolt_compound_build(void *compound) {
    StaticCompoundShapeSettings *settings = (StaticCompoundShapeSettings *)compound;
    Shape *shape = settings->mSubShapes.empty() ? nullptr : keep(settings->Create());
    delete settings;
    return shape;
}

void sosu_jolt_shape_release(void *shape) {
    if (shape) ((Shape *)shape)->Release();
}

// ---- bodies ----

// kind: 0 static, 1 dynamic, 2 kinematic. `mass` 0: from the density of the shape.
uint32_t sosu_jolt_body(void *world, void *shape, int kind, const float *pose, float mass, float friction,
                        float restitution, float damping, float angular_damping, float gravity_factor, uint32_t layers,
                        uint32_t mask, bool sensor, bool bullet) {
    World *w = (World *)world;
    if (!shape) return BodyID::cInvalidBodyID;
    EMotionType motion = kind == 1 ? EMotionType::Dynamic : kind == 2 ? EMotionType::Kinematic : EMotionType::Static;
    if (kind == 1 && ((Shape *)shape)->GetSubType() == EShapeSubType::Mesh) return BodyID::cInvalidBodyID;
    ObjectLayer layer = layer_of(w, layers, mask, kind != 0);
    BodyCreationSettings s((const Shape *)shape, RVec3(pose[0], pose[1], pose[2]),
                           Quat(pose[3], pose[4], pose[5], pose[6]).Normalized(), motion, layer);
    s.mFriction = friction;
    s.mRestitution = restitution;
    s.mLinearDamping = damping;
    s.mAngularDamping = angular_damping;
    s.mGravityFactor = gravity_factor;
    s.mIsSensor = sensor;
    if (bullet) s.mMotionQuality = EMotionQuality::LinearCast;
    if (sensor && kind != 1) s.mCollideKinematicVsNonDynamic = true; /* a sensor sees the kinematic bodies */
    if (mass > 0.0f && kind != 0) {
        s.mOverrideMassProperties = EOverrideMassProperties::CalculateInertia;
        s.mMassPropertiesOverride.mMass = mass;
    }
    BodyID id = w->system.GetBodyInterface().CreateAndAddBody(s, kind == 0 ? EActivation::DontActivate : EActivation::Activate);
    return id.GetIndexAndSequenceNumber();
}

bool sosu_jolt_body_valid(void *world, uint32_t body) {
    if (body == BodyID::cInvalidBodyID) return false;
    return ((World *)world)->system.GetBodyInterface().IsAdded(BodyID(body));
}

void sosu_jolt_body_remove(void *world, uint32_t body) {
    if (!sosu_jolt_body_valid(world, body)) return;
    BodyInterface &bodies = ((World *)world)->system.GetBodyInterface();
    bodies.RemoveBody(BodyID(body));
    bodies.DestroyBody(BodyID(body));
}

// x, y, z, rotation (x, y, z, w), velocity (x, y, z), angular velocity (x, y, z)
void sosu_jolt_body_state(void *world, uint32_t body, float *out) {
    BodyInterface &bodies = ((World *)world)->system.GetBodyInterface();
    RVec3 p;
    Quat q;
    bodies.GetPositionAndRotation(BodyID(body), p, q);
    write_r3(out, p);
    out[3] = q.GetX();
    out[4] = q.GetY();
    out[5] = q.GetZ();
    out[6] = q.GetW();
    write3(out + 7, bodies.GetLinearVelocity(BodyID(body)));
    write3(out + 10, bodies.GetAngularVelocity(BodyID(body)));
}

float sosu_jolt_body_mass(void *world, uint32_t body) {
    BodyLockRead lock(((World *)world)->system.GetBodyLockInterface(), BodyID(body));
    if (!lock.Succeeded()) return 0.0f;
    const Body &b = lock.GetBody();
    if (!b.IsDynamic()) return 0.0f;
    float inverse = b.GetMotionProperties()->GetInverseMass();
    return inverse > 0.0f ? 1.0f / inverse : 0.0f;
}

bool sosu_jolt_body_active(void *world, uint32_t body) {
    return ((World *)world)->system.GetBodyInterface().IsActive(BodyID(body));
}

void sosu_jolt_body_set_active(void *world, uint32_t body, bool active) {
    BodyInterface &bodies = ((World *)world)->system.GetBodyInterface();
    if (active) {
        bodies.ActivateBody(BodyID(body));
    } else {
        bodies.DeactivateBody(BodyID(body));
    }
}

void sosu_jolt_body_set_velocity(void *world, uint32_t body, float vx, float vy, float vz, float ax, float ay, float az) {
    ((World *)world)->system.GetBodyInterface().SetLinearAndAngularVelocity(BodyID(body), Vec3(vx, vy, vz), Vec3(ax, ay, az));
}

void sosu_jolt_body_force(void *world, uint32_t body, float fx, float fy, float fz, float tx, float ty, float tz) {
    BodyInterface &bodies = ((World *)world)->system.GetBodyInterface();
    bodies.AddForceAndTorque(BodyID(body), Vec3(fx, fy, fz), Vec3(tx, ty, tz));
}

void sosu_jolt_body_impulse(void *world, uint32_t body, float ix, float iy, float iz, float ax, float ay, float az) {
    BodyInterface &bodies = ((World *)world)->system.GetBodyInterface();
    bodies.AddImpulse(BodyID(body), Vec3(ix, iy, iz));
    if (ax != 0.0f || ay != 0.0f || az != 0.0f) bodies.AddAngularImpulse(BodyID(body), Vec3(ax, ay, az));
}

void sosu_jolt_body_teleport(void *world, uint32_t body, const float *pose) {
    ((World *)world)->system.GetBodyInterface().SetPositionAndRotation(
        BodyID(body), RVec3(pose[0], pose[1], pose[2]), Quat(pose[3], pose[4], pose[5], pose[6]).Normalized(),
        EActivation::Activate);
}

// A kinematic body moved to `pose` over the next step of `dt`: what it meets is pushed.
void sosu_jolt_body_move(void *world, uint32_t body, const float *pose, float dt) {
    ((World *)world)->system.GetBodyInterface().MoveKinematic(
        BodyID(body), RVec3(pose[0], pose[1], pose[2]), Quat(pose[3], pose[4], pose[5], pose[6]).Normalized(), dt);
}

// ---- queries ----

// The first body on the ray from `o` along `d` (its length is the distance), or none; out: point,
// normal, fraction of `d`.
uint32_t sosu_jolt_ray(void *world, float ox, float oy, float oz, float dx, float dy, float dz, uint32_t mask, float *out) {
    World *w = (World *)world;
    RRayCast ray{ RVec3(ox, oy, oz), Vec3(dx, dy, dz) };
    RayCastResult hit;
    QueryLayers filter(&w->kinds, mask);
    if (!w->system.GetNarrowPhaseQuery().CastRay(ray, hit, BroadPhaseLayerFilter(), filter)) return BodyID::cInvalidBodyID;
    RVec3 point = ray.GetPointOnRay(hit.mFraction);
    write_r3(out, point);
    BodyLockRead lock(w->system.GetBodyLockInterface(), hit.mBodyID);
    write3(out + 3, lock.Succeeded() ? lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, point) : Vec3::sZero());
    out[6] = hit.mFraction;
    return hit.mBodyID.GetIndexAndSequenceNumber();
}

// The bodies on the ray, nearest first, up to `capacity` (their fractions in `fractions`); the
// count of all.
int sosu_jolt_ray_all(void *world, float ox, float oy, float oz, float dx, float dy, float dz, uint32_t mask, uint32_t *bodies,
                      float *fractions, int capacity) {
    World *w = (World *)world;
    RRayCast ray{ RVec3(ox, oy, oz), Vec3(dx, dy, dz) };
    AllHitCollisionCollector<CastRayCollector> collector;
    QueryLayers filter(&w->kinds, mask);
    RayCastSettings settings;
    w->system.GetNarrowPhaseQuery().CastRay(ray, settings, collector, BroadPhaseLayerFilter(), filter);
    collector.Sort();
    int count = (int)collector.mHits.size();
    for (int i = 0; i < count && i < capacity; i++) {
        bodies[i] = collector.mHits[(size_t)i].mBodyID.GetIndexAndSequenceNumber();
        fractions[i] = collector.mHits[(size_t)i].mFraction;
    }
    return count;
}

// The bodies that `shape` at `pose` overlaps, up to `capacity`, each once; the count of all.
int sosu_jolt_overlap(void *world, void *shape, const float *pose, uint32_t mask, uint32_t *bodies, int capacity) {
    World *w = (World *)world;
    AllHitCollisionCollector<CollideShapeCollector> collector;
    QueryLayers filter(&w->kinds, mask);
    CollideShapeSettings settings;
    RMat44 at = RMat44::sRotationTranslation(Quat(pose[3], pose[4], pose[5], pose[6]).Normalized(), RVec3(pose[0], pose[1], pose[2]));
    w->system.GetNarrowPhaseQuery().CollideShape((const Shape *)shape, Vec3::sOne(), at, settings, RVec3::sZero(), collector,
                                                 BroadPhaseLayerFilter(), filter);
    int count = 0;
    for (const CollideShapeResult &hit : collector.mHits) {
        uint32_t id = hit.mBodyID2.GetIndexAndSequenceNumber();
        bool seen = false;
        for (int k = 0; k < count && k < capacity; k++) seen |= bodies[k] == id;
        if (seen) continue;
        if (count < capacity) bodies[count] = id;
        count++;
    }
    return count;
}

// A sphere of `radius` swept from `o` along `d`: the first body it touches, or none; out: the
// point, the normal, the fraction of `d`.
uint32_t sosu_jolt_sphere_cast(void *world, float radius, float ox, float oy, float oz, float dx, float dy, float dz, uint32_t mask,
                               float *out) {
    World *w = (World *)world;
    if (!(radius > 0.0f)) return BodyID::cInvalidBodyID;
    SphereShape sphere(radius);
    sphere.SetEmbedded();
    RShapeCast cast = RShapeCast::sFromWorldTransform(&sphere, Vec3::sOne(), RMat44::sTranslation(RVec3(ox, oy, oz)), Vec3(dx, dy, dz));
    ShapeCastSettings settings;
    ClosestHitCollisionCollector<CastShapeCollector> collector;
    QueryLayers filter(&w->kinds, mask);
    w->system.GetNarrowPhaseQuery().CastShape(cast, settings, RVec3::sZero(), collector, BroadPhaseLayerFilter(), filter);
    if (!collector.HadHit()) return BodyID::cInvalidBodyID;
    const ShapeCastResult &hit = collector.mHit;
    write3(out, hit.mContactPointOn2);
    Vec3 axis = hit.mPenetrationAxis;
    write3(out + 3, axis.LengthSq() > 0.0f ? -axis.Normalized() : Vec3::sZero());
    out[6] = hit.mFraction;
    return hit.mBodyID2.GetIndexAndSequenceNumber();
}

// ---- the virtual character: a capsule standing on its feet, moved by the program ----

struct Character {
    JPH_OVERRIDE_NEW_DELETE
    Ref<CharacterVirtual> body;
    ObjectLayer layer;
    float step_up;
};

void *sosu_jolt_character(void *world, float height, float radius, float px, float py, float pz, float max_slope, float step_up,
                          uint32_t layers, uint32_t mask) {
    World *w = (World *)world;
    float half = height / 2.0f - radius;
    if (!(radius > 0.0f) || !(half > 0.0f)) return nullptr;
    Ref<CharacterVirtualSettings> settings = new CharacterVirtualSettings();
    settings->mShape = RotatedTranslatedShapeSettings(Vec3(0, height / 2.0f, 0), Quat::sIdentity(), new CapsuleShape(half, radius))
                           .Create()
                           .Get();
    settings->mMaxSlopeAngle = max_slope;
    settings->mSupportingVolume = Plane(Vec3::sAxisY(), -radius); /* the lower half sphere stands */
    Character *c = new Character();
    c->body = new CharacterVirtual(settings, RVec3(px, py, pz), Quat::sIdentity(), 0, &w->system);
    c->layer = layer_of(w, layers, mask, true);
    c->step_up = step_up;
    return c;
}

void sosu_jolt_character_free(void *character) {
    delete (Character *)character;
}

// One frame of `dt`: the character walks at `velocity` (x and z; y for a jump, else it keeps
// falling), climbs steps up to its `step_up`, sticks to the ground going down, slides along walls.
void sosu_jolt_character_update(void *world, void *character, float dt, float vx, float vy, float vz) {
    World *w = (World *)world;
    Character *c = (Character *)character;
    CharacterVirtual &body = *c->body;
    Vec3 gravity = w->system.GetGravity();
    body.UpdateGroundVelocity();
    Vec3 current = body.GetLinearVelocity();
    bool grounded = body.GetGroundState() == CharacterBase::EGroundState::OnGround;
    float vertical = current.GetY();
    if (grounded && vertical <= body.GetGroundVelocity().GetY() + 0.0001f) vertical = body.GetGroundVelocity().GetY();
    if (vy > 0.0f && grounded) vertical = vy; /* a jump starts from the ground only */
    vertical += gravity.GetY() * dt;
    body.SetLinearVelocity(Vec3(vx, vertical, vz));
    CharacterVirtual::ExtendedUpdateSettings settings;
    settings.mWalkStairsStepUp = Vec3(0, c->step_up, 0);
    settings.mStickToFloorStepDown = Vec3(0, -c->step_up, 0);
    QueryLayers filter(&w->kinds, w->kinds[c->layer].mask);
    body.ExtendedUpdate(dt, gravity, settings, BroadPhaseLayerFilter(), filter, BodyFilter(), ShapeFilter(), w->temp);
}

// x, y, z of the feet, velocity, on the ground (1) or not (0)
int sosu_jolt_character_state(void *character, float *out) {
    CharacterVirtual &body = *((Character *)character)->body;
    write_r3(out, body.GetPosition());
    write3(out + 3, body.GetLinearVelocity());
    return body.GetGroundState() == CharacterBase::EGroundState::OnGround ? 1 : 0;
}

void sosu_jolt_character_teleport(void *character, float px, float py, float pz) {
    CharacterVirtual &body = *((Character *)character)->body;
    body.SetPosition(RVec3(px, py, pz));
    body.SetLinearVelocity(Vec3::sZero());
}

} // extern "C"
