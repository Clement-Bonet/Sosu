/* The interface of physics2d to Box2D 3 (MIT, taken by [native.box2d] of sosu.toml): plain
   functions of numbers, so that Sosu passes no structure by value. The identifiers of Box2D are
   kept as the integers that b2Store*Id gives (0 is none). */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "box2d/box2d.h"

typedef struct {
    b2WorldId id;
} World;

void *sosu_b2_world(float gx, float gy, bool sleep) {
    World *w = malloc(sizeof *w);
    if (!w) return NULL;
    b2WorldDef def = b2DefaultWorldDef();
    def.gravity = (b2Vec2){ gx, gy };
    def.enableSleep = sleep;
    def.workerCount = 1; /* steps in the thread of the program */
    w->id = b2CreateWorld(&def);
    return w;
}

void sosu_b2_world_free(void *world) {
    World *w = world;
    if (!w) return;
    b2DestroyWorld(w->id);
    free(w);
}

void sosu_b2_step(void *world, float dt, int substeps) {
    b2World_Step(((World *)world)->id, dt, substeps);
}

void sosu_b2_set_gravity(void *world, float gx, float gy) {
    b2World_SetGravity(((World *)world)->id, (b2Vec2){ gx, gy });
}

int sosu_b2_awake_count(void *world) {
    return b2World_GetAwakeBodyCount(((World *)world)->id);
}

/* ---- bodies ---- */

uint64_t sosu_b2_body(void *world, int kind, float x, float y, float angle, float damping, float angular_damping,
                      float gravity_scale, bool bullet, bool fixed_rotation) {
    b2BodyDef def = b2DefaultBodyDef();
    def.type = kind == 2 ? b2_kinematicBody : kind == 1 ? b2_dynamicBody : b2_staticBody;
    def.position = (b2Pos){ x, y };
    def.rotation = b2MakeRot(angle);
    def.linearDamping = damping;
    def.angularDamping = angular_damping;
    def.gravityScale = gravity_scale;
    def.isBullet = bullet;
    def.motionLocks.angularZ = fixed_rotation;
    return b2StoreBodyId(b2CreateBody(((World *)world)->id, &def));
}

bool sosu_b2_body_valid(uint64_t body) {
    return body != 0 && b2Body_IsValid(b2LoadBodyId(body));
}

void sosu_b2_body_destroy(uint64_t body) {
    if (sosu_b2_body_valid(body)) b2DestroyBody(b2LoadBodyId(body));
}

/* x, y, angle, vx, vy, spin */
void sosu_b2_body_state(uint64_t body, float *out) {
    b2BodyId id = b2LoadBodyId(body);
    b2Pos p = b2Body_GetPosition(id);
    b2Vec2 v = b2Body_GetLinearVelocity(id);
    out[0] = (float)p.x;
    out[1] = (float)p.y;
    out[2] = b2Rot_GetAngle(b2Body_GetRotation(id));
    out[3] = v.x;
    out[4] = v.y;
    out[5] = b2Body_GetAngularVelocity(id);
}

float sosu_b2_body_mass(uint64_t body) {
    return b2Body_GetMass(b2LoadBodyId(body));
}

bool sosu_b2_body_awake(uint64_t body) {
    return b2Body_IsAwake(b2LoadBodyId(body));
}

void sosu_b2_body_set_awake(uint64_t body, bool awake) {
    b2Body_SetAwake(b2LoadBodyId(body), awake);
}

void sosu_b2_body_set_velocity(uint64_t body, float vx, float vy, float spin) {
    b2BodyId id = b2LoadBodyId(body);
    b2Body_SetLinearVelocity(id, (b2Vec2){ vx, vy });
    b2Body_SetAngularVelocity(id, spin);
}

void sosu_b2_body_teleport(uint64_t body, float x, float y, float angle) {
    b2Body_SetTransform(b2LoadBodyId(body), (b2Pos){ x, y }, b2MakeRot(angle));
}

void sosu_b2_body_force(uint64_t body, float fx, float fy, float torque) {
    b2BodyId id = b2LoadBodyId(body);
    b2Body_ApplyForceToCenter(id, (b2Vec2){ fx, fy }, true);
    if (torque != 0.0f) b2Body_ApplyTorque(id, torque, true);
}

void sosu_b2_body_impulse(uint64_t body, float ix, float iy, float angular) {
    b2BodyId id = b2LoadBodyId(body);
    b2Body_ApplyLinearImpulseToCenter(id, (b2Vec2){ ix, iy }, true);
    if (angular != 0.0f) b2Body_ApplyAngularImpulse(id, angular, true);
}

/* ---- shapes: the material and the filter of the next one ---- */

static b2ShapeDef shape_def(float density, float friction, float restitution, uint64_t layers, uint64_t mask,
                            bool sensor) {
    b2ShapeDef def = b2DefaultShapeDef();
    def.density = density;
    def.material.friction = friction;
    def.material.restitution = restitution;
    def.filter.categoryBits = layers;
    def.filter.maskBits = mask;
    def.isSensor = sensor;
    def.enableSensorEvents = true;
    def.enableContactEvents = true;
    return def;
}

uint64_t sosu_b2_circle(uint64_t body, float cx, float cy, float radius, float density, float friction,
                        float restitution, uint64_t layers, uint64_t mask, bool sensor) {
    b2ShapeDef def = shape_def(density, friction, restitution, layers, mask, sensor);
    b2Circle circle = { { cx, cy }, radius };
    return b2StoreShapeId(b2CreateCircleShape(b2LoadBodyId(body), &def, &circle));
}

uint64_t sosu_b2_capsule(uint64_t body, float x1, float y1, float x2, float y2, float radius, float density,
                         float friction, float restitution, uint64_t layers, uint64_t mask, bool sensor) {
    b2ShapeDef def = shape_def(density, friction, restitution, layers, mask, sensor);
    b2Capsule capsule = { { x1, y1 }, { x2, y2 }, radius };
    return b2StoreShapeId(b2CreateCapsuleShape(b2LoadBodyId(body), &def, &capsule));
}

uint64_t sosu_b2_box(uint64_t body, float hw, float hh, float cx, float cy, float angle, float density,
                     float friction, float restitution, uint64_t layers, uint64_t mask, bool sensor) {
    b2ShapeDef def = shape_def(density, friction, restitution, layers, mask, sensor);
    b2Polygon box = b2MakeOffsetBox(hw, hh, (b2Vec2){ cx, cy }, b2MakeRot(angle));
    return b2StoreShapeId(b2CreatePolygonShape(b2LoadBodyId(body), &def, &box));
}

/* A convex polygon of the hull of `count` points (x, y pairs), at most B2_MAX_POLYGON_VERTICES; 0
   when they make no hull (fewer than 3, all on a line, too many). */
uint64_t sosu_b2_polygon(uint64_t body, const float *xy, int count, float radius, float density, float friction,
                         float restitution, uint64_t layers, uint64_t mask, bool sensor) {
    if (count < 3 || count > B2_MAX_POLYGON_VERTICES) return 0;
    b2Vec2 points[B2_MAX_POLYGON_VERTICES];
    for (int i = 0; i < count; i++) points[i] = (b2Vec2){ xy[2 * i], xy[2 * i + 1] };
    b2Hull hull = b2ComputeHull(points, count);
    if (hull.count == 0) return 0;
    b2ShapeDef def = shape_def(density, friction, restitution, layers, mask, sensor);
    b2Polygon polygon = b2MakePolygon(&hull, radius);
    return b2StoreShapeId(b2CreatePolygonShape(b2LoadBodyId(body), &def, &polygon));
}

uint64_t sosu_b2_segment(uint64_t body, float x1, float y1, float x2, float y2, float friction, float restitution,
                         uint64_t layers, uint64_t mask) {
    b2ShapeDef def = shape_def(0.0f, friction, restitution, layers, mask, false);
    b2Segment segment = { { x1, y1 }, { x2, y2 } };
    return b2StoreShapeId(b2CreateSegmentShape(b2LoadBodyId(body), &def, &segment));
}

/* A chain of segments through `count` points (2 at least, 3 for a loop), closed when `loop`. An
   open chain ends as if its first and last segments went on straight (its ghost points). */
uint64_t sosu_b2_chain(uint64_t body, const float *xy, int count, bool loop, float friction, float restitution,
                       uint64_t layers, uint64_t mask) {
    if (count < (loop ? 3 : 2)) return 0;
    b2Vec2 *points = malloc((size_t)count * sizeof *points);
    if (!points) return 0;
    for (int i = 0; i < count; i++) points[i] = (b2Vec2){ xy[2 * i], xy[2 * i + 1] };
    b2SurfaceMaterial material = b2DefaultShapeDef().material;
    material.friction = friction;
    material.restitution = restitution;
    b2ChainDef def = b2DefaultChainDef();
    def.points = points;
    def.pointCount = count;
    def.isLoop = loop;
    def.ghost1 = (b2Vec2){ 2.0f * points[0].x - points[1].x, 2.0f * points[0].y - points[1].y };
    def.ghost2 = (b2Vec2){ 2.0f * points[count - 1].x - points[count - 2].x,
                           2.0f * points[count - 1].y - points[count - 2].y };
    def.materials = &material;
    def.materialCount = 1;
    def.filter.categoryBits = layers;
    def.filter.maskBits = mask;
    b2ChainId chain = b2CreateChain(b2LoadBodyId(body), &def);
    free(points);
    return b2StoreChainId(chain);
}

uint64_t sosu_b2_shape_body(uint64_t shape) {
    return b2StoreBodyId(b2Shape_GetBody(b2LoadShapeId(shape)));
}

/* ---- queries ---- */

/* The closest shape that the ray from (ox, oy) along (tx, ty) hits, or 0; out: x, y, nx, ny, the
   fraction of the ray. */
uint64_t sosu_b2_ray(void *world, float ox, float oy, float tx, float ty, uint64_t mask, float *out) {
    b2QueryFilter filter = b2DefaultQueryFilter();
    filter.maskBits = mask;
    b2RayResult r = b2World_CastRayClosest(((World *)world)->id, (b2Pos){ ox, oy }, (b2Vec2){ tx, ty }, filter);
    if (!r.hit) return 0;
    out[0] = (float)r.point.x;
    out[1] = (float)r.point.y;
    out[2] = r.normal.x;
    out[3] = r.normal.y;
    out[4] = r.fraction;
    return b2StoreShapeId(r.shapeId);
}

typedef struct {
    uint64_t *out;
    int capacity;
    int count;
} Found;

static bool found_shape(b2ShapeId shape, void *context) {
    Found *found = context;
    if (found->count < found->capacity) found->out[found->count] = b2StoreShapeId(shape);
    found->count++;
    return true;
}

/* The shapes whose bounds overlap the box, up to `capacity` written; the count of all. */
int sosu_b2_overlap_box(void *world, float x0, float y0, float x1, float y1, uint64_t mask, uint64_t *out,
                        int capacity) {
    b2QueryFilter filter = b2DefaultQueryFilter();
    filter.maskBits = mask;
    Found found = { out, capacity, 0 };
    b2AABB box = { { 0.0f, 0.0f }, { x1 - x0, y1 - y0 } };
    b2World_OverlapAABB(((World *)world)->id, (b2Pos){ x0, y0 }, box, filter, found_shape, &found);
    return found.count;
}

/* ---- contacts of the last step ---- */

int sosu_b2_contacts(void *world, int ended, uint64_t *shapes, int capacity) {
    b2ContactEvents events = b2World_GetContactEvents(((World *)world)->id);
    int count = ended ? events.endCount : events.beginCount;
    for (int i = 0; i < count && i < capacity / 2; i++) {
        b2ShapeId a = ended ? events.endEvents[i].shapeIdA : events.beginEvents[i].shapeIdA;
        b2ShapeId b = ended ? events.endEvents[i].shapeIdB : events.beginEvents[i].shapeIdB;
        shapes[2 * i] = b2StoreShapeId(a);
        shapes[2 * i + 1] = b2StoreShapeId(b);
    }
    return count;
}

int sosu_b2_sensor_events(void *world, int ended, uint64_t *shapes, int capacity) {
    b2SensorEvents events = b2World_GetSensorEvents(((World *)world)->id);
    int count = ended ? events.endCount : events.beginCount;
    for (int i = 0; i < count && i < capacity / 2; i++) {
        b2ShapeId sensor = ended ? events.endEvents[i].sensorShapeId : events.beginEvents[i].sensorShapeId;
        b2ShapeId visitor = ended ? events.endEvents[i].visitorShapeId : events.beginEvents[i].visitorShapeId;
        shapes[2 * i] = b2StoreShapeId(sensor);
        shapes[2 * i + 1] = b2StoreShapeId(visitor);
    }
    return count;
}

/* ---- joints, at an anchor given in the world ---- */

static b2JointDef joint_base(uint64_t a, uint64_t b, float ax, float ay, float bx, float by, bool collide) {
    b2JointDef base = { 0 };
    base.bodyIdA = b2LoadBodyId(a);
    base.bodyIdB = b2LoadBodyId(b);
    base.localFrameA.p = b2Body_GetLocalPoint(base.bodyIdA, (b2Pos){ ax, ay });
    base.localFrameA.q = b2Rot_identity;
    base.localFrameB.p = b2Body_GetLocalPoint(base.bodyIdB, (b2Pos){ bx, by });
    base.localFrameB.q = b2Rot_identity;
    base.collideConnected = collide;
    return base;
}

/* kind: 0 hinge (revolute), 1 distance, 2 weld, 3 slider (prismatic, along (dx, dy)). */
uint64_t sosu_b2_joint(void *world, int kind, uint64_t a, uint64_t b, float ax, float ay, float bx, float by,
                       float dx, float dy, bool collide, bool limit, float lower, float upper, bool motor, float speed,
                       float max_force) {
    b2WorldId w = ((World *)world)->id;
    b2JointId id;
    if (kind == 1) {
        b2DistanceJointDef def = b2DefaultDistanceJointDef();
        b2JointDef base = joint_base(a, b, ax, ay, bx, by, collide);
        def.base.bodyIdA = base.bodyIdA;
        def.base.bodyIdB = base.bodyIdB;
        def.base.localFrameA = base.localFrameA;
        def.base.localFrameB = base.localFrameB;
        def.base.collideConnected = collide;
        float ex = bx - ax, ey = by - ay;
        def.length = b2Length((b2Vec2){ ex, ey });
        def.enableLimit = limit;
        def.minLength = lower;
        def.maxLength = upper;
        def.enableMotor = motor;
        def.motorSpeed = speed;
        def.maxMotorForce = max_force;
        id = b2CreateDistanceJoint(w, &def);
    } else if (kind == 2) {
        b2WeldJointDef def = b2DefaultWeldJointDef();
        b2JointDef base = joint_base(a, b, ax, ay, ax, ay, collide);
        def.base.bodyIdA = base.bodyIdA;
        def.base.bodyIdB = base.bodyIdB;
        def.base.localFrameA = base.localFrameA;
        def.base.localFrameB = base.localFrameB;
        def.base.collideConnected = collide;
        id = b2CreateWeldJoint(w, &def);
    } else if (kind == 3) {
        b2PrismaticJointDef def = b2DefaultPrismaticJointDef();
        b2JointDef base = joint_base(a, b, ax, ay, ax, ay, collide);
        /* the axis is the x axis of the frames: turned to (dx, dy) in each body */
        b2Rot axis = b2MakeRot(b2Atan2(dy, dx));
        b2Rot ra = b2Body_GetRotation(base.bodyIdA), rb = b2Body_GetRotation(base.bodyIdB);
        def.base.bodyIdA = base.bodyIdA;
        def.base.bodyIdB = base.bodyIdB;
        def.base.localFrameA = (b2Transform){ base.localFrameA.p, b2InvMulRot(ra, axis) };
        def.base.localFrameB = (b2Transform){ base.localFrameB.p, b2InvMulRot(rb, axis) };
        def.base.collideConnected = collide;
        def.enableLimit = limit;
        def.lowerTranslation = lower;
        def.upperTranslation = upper;
        def.enableMotor = motor;
        def.motorSpeed = speed;
        def.maxMotorForce = max_force;
        id = b2CreatePrismaticJoint(w, &def);
    } else {
        b2RevoluteJointDef def = b2DefaultRevoluteJointDef();
        b2JointDef base = joint_base(a, b, ax, ay, ax, ay, collide);
        def.base.bodyIdA = base.bodyIdA;
        def.base.bodyIdB = base.bodyIdB;
        def.base.localFrameA = base.localFrameA;
        def.base.localFrameB = base.localFrameB;
        def.base.collideConnected = collide;
        def.enableLimit = limit;
        def.lowerAngle = lower;
        def.upperAngle = upper;
        def.enableMotor = motor;
        def.motorSpeed = speed;
        def.maxMotorTorque = max_force;
        id = b2CreateRevoluteJoint(w, &def);
    }
    return b2StoreJointId(id);
}

void sosu_b2_joint_destroy(uint64_t joint) {
    if (joint != 0 && b2Joint_IsValid(b2LoadJointId(joint))) b2DestroyJoint(b2LoadJointId(joint));
}
