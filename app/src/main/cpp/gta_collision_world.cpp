// Spatial collision world built from bounded user-owned COL + IPL descriptors.
#include "gta_collision_world.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace samp::gta::world {
namespace {

constexpr float kEpsilon = 0.0001f;
constexpr float kCellSize = 64.0f;
constexpr int64_t kMaxCellsPerPlacement = 4096;

bool finiteVec(const col::Vec3& value) {
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

col::Vec3 subtract(const col::Vec3& left, const col::Vec3& right) {
  return {left.x - right.x, left.y - right.y, left.z - right.z};
}

col::Vec3 add(const col::Vec3& left, const col::Vec3& right) {
  return {left.x + right.x, left.y + right.y, left.z + right.z};
}

col::Vec3 multiply(const col::Vec3& value, float scalar) {
  return {value.x * scalar, value.y * scalar, value.z * scalar};
}

float dot(const col::Vec3& left, const col::Vec3& right) {
  return left.x * right.x + left.y * right.y + left.z * right.z;
}

col::Vec3 cross(const col::Vec3& left, const col::Vec3& right) {
  return {left.y * right.z - left.z * right.y,
          left.z * right.x - left.x * right.z,
          left.x * right.y - left.y * right.x};
}

bool normalizeQuaternion(map::Quaternion& quaternion) {
  if (!std::isfinite(quaternion.x) || !std::isfinite(quaternion.y) ||
      !std::isfinite(quaternion.z) || !std::isfinite(quaternion.w)) {
    return false;
  }
  const float lengthSquared = quaternion.x * quaternion.x + quaternion.y * quaternion.y +
                              quaternion.z * quaternion.z + quaternion.w * quaternion.w;
  if (!std::isfinite(lengthSquared) || lengthSquared < 0.25f || lengthSquared > 2.25f) return false;
  const float inverseLength = 1.0f / std::sqrt(lengthSquared);
  quaternion.x *= inverseLength;
  quaternion.y *= inverseLength;
  quaternion.z *= inverseLength;
  quaternion.w *= inverseLength;
  return true;
}

col::Vec3 rotate(const map::Quaternion& quaternion, const col::Vec3& vector) {
  // Unit-quaternion vector rotation without an intermediate quaternion object.
  const col::Vec3 axis{quaternion.x, quaternion.y, quaternion.z};
  const col::Vec3 twiceCross = multiply(cross(axis, vector), 2.0f);
  return add(add(vector, multiply(twiceCross, quaternion.w)), cross(axis, twiceCross));
}

col::Vec3 inverseRotate(const map::Quaternion& quaternion, const col::Vec3& vector) {
  const map::Quaternion inverse{-quaternion.x, -quaternion.y, -quaternion.z, quaternion.w};
  return rotate(inverse, vector);
}

col::Vec3 transformPoint(const map::Quaternion& quaternion, const col::Vec3& position,
                         const col::Vec3& local) {
  return add(position, rotate(quaternion, local));
}

void considerFloor(float z, col::Vec3 normal, uint8_t material, float minZ, float maxZ,
                   col::FloorHit& hit) {
  if (!std::isfinite(z) || !finiteVec(normal) || z < minZ - kEpsilon || z > maxZ + kEpsilon ||
      (hit.hit && z <= hit.z + kEpsilon)) {
    return;
  }
  const float lengthSquared = dot(normal, normal);
  if (!std::isfinite(lengthSquared) || lengthSquared <= kEpsilon * kEpsilon) return;
  const float inverseLength = 1.0f / std::sqrt(lengthSquared);
  normal = multiply(normal, inverseLength);
  if (normal.z < 0.0f) normal = multiply(normal, -1.0f);
  hit.hit = true;
  hit.z = z;
  hit.normal = normal;
  hit.material = material;
}

bool buildWorldBounds(const col::Model& model, const col::Vec3& position,
                      const map::Quaternion& rotation, col::Vec3& minimum, col::Vec3& maximum) {
  if (!finiteVec(position) || !finiteVec(model.bounds.minimum) || !finiteVec(model.bounds.maximum) ||
      model.bounds.minimum.x > model.bounds.maximum.x ||
      model.bounds.minimum.y > model.bounds.maximum.y ||
      model.bounds.minimum.z > model.bounds.maximum.z) {
    return false;
  }
  const float infinity = std::numeric_limits<float>::infinity();
  minimum = {infinity, infinity, infinity};
  maximum = {-infinity, -infinity, -infinity};
  for (int x = 0; x < 2; ++x) {
    for (int y = 0; y < 2; ++y) {
      for (int z = 0; z < 2; ++z) {
        const col::Vec3 corner{
            x == 0 ? model.bounds.minimum.x : model.bounds.maximum.x,
            y == 0 ? model.bounds.minimum.y : model.bounds.maximum.y,
            z == 0 ? model.bounds.minimum.z : model.bounds.maximum.z,
        };
        const col::Vec3 world = transformPoint(rotation, position, corner);
        if (!finiteVec(world)) return false;
        minimum.x = std::min(minimum.x, world.x);
        minimum.y = std::min(minimum.y, world.y);
        minimum.z = std::min(minimum.z, world.z);
        maximum.x = std::max(maximum.x, world.x);
        maximum.y = std::max(maximum.y, world.y);
        maximum.z = std::max(maximum.z, world.z);
      }
    }
  }
  return finiteVec(minimum) && finiteVec(maximum);
}

bool cellCoordinate(float value, int32_t& out) {
  if (!std::isfinite(value)) return false;
  const double cell = std::floor(static_cast<double>(value) / static_cast<double>(kCellSize));
  if (cell < static_cast<double>(std::numeric_limits<int32_t>::min()) ||
      cell > static_cast<double>(std::numeric_limits<int32_t>::max())) {
    return false;
  }
  out = static_cast<int32_t>(cell);
  return true;
}

uint64_t cellKey(int32_t x, int32_t y) {
  return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32u) |
         static_cast<uint32_t>(y);
}

void considerModelFloor(const col::Model& model, const col::Vec3& position,
                        const map::Quaternion& rotation, float x, float y, float minZ,
                        float maxZ, col::FloorHit& hit) {
  for (const col::Sphere& sphere : model.spheres) {
    const col::Vec3 center = transformPoint(rotation, position, sphere.center);
    const float dx = x - center.x;
    const float dy = y - center.y;
    const float horizontalSquared = dx * dx + dy * dy;
    const float radiusSquared = sphere.radius * sphere.radius;
    if (std::isfinite(horizontalSquared) && std::isfinite(radiusSquared) &&
        horizontalSquared <= radiusSquared) {
      const float dz = std::sqrt(std::max(0.0f, radiusSquared - horizontalSquared));
      considerFloor(center.z + dz, {dx, dy, dz}, sphere.material, minZ, maxZ, hit);
    }
  }

  // A vertical world ray becomes an arbitrary local ray when an IPL placement
  // is pitched/rolled. Slab intersection therefore handles every quaternion,
  // not only yaw-only placements.
  const col::Vec3 localOrigin = inverseRotate(rotation, {x - position.x, y - position.y, -position.z});
  const col::Vec3 localDirection = inverseRotate(rotation, {0.0f, 0.0f, 1.0f});
  for (const col::Box& box : model.boxes) {
    const float origin[3] = {localOrigin.x, localOrigin.y, localOrigin.z};
    const float direction[3] = {localDirection.x, localDirection.y, localDirection.z};
    const float lower[3] = {box.minimum.x, box.minimum.y, box.minimum.z};
    const float upper[3] = {box.maximum.x, box.maximum.y, box.maximum.z};
    float enter = -std::numeric_limits<float>::infinity();
    float exit = std::numeric_limits<float>::infinity();
    col::Vec3 exitNormal{};
    bool intersects = true;
    for (int axis = 0; axis < 3; ++axis) {
      if (std::fabs(direction[axis]) <= kEpsilon) {
        if (origin[axis] < lower[axis] - kEpsilon || origin[axis] > upper[axis] + kEpsilon) {
          intersects = false;
          break;
        }
        continue;
      }
      float axisEnter = 0.0f;
      float axisExit = 0.0f;
      col::Vec3 axisExitNormal{};
      if (direction[axis] > 0.0f) {
        axisEnter = (lower[axis] - origin[axis]) / direction[axis];
        axisExit = (upper[axis] - origin[axis]) / direction[axis];
        if (axis == 0) axisExitNormal.x = 1.0f;
        if (axis == 1) axisExitNormal.y = 1.0f;
        if (axis == 2) axisExitNormal.z = 1.0f;
      } else {
        axisEnter = (upper[axis] - origin[axis]) / direction[axis];
        axisExit = (lower[axis] - origin[axis]) / direction[axis];
        if (axis == 0) axisExitNormal.x = -1.0f;
        if (axis == 1) axisExitNormal.y = -1.0f;
        if (axis == 2) axisExitNormal.z = -1.0f;
      }
      enter = std::max(enter, axisEnter);
      if (axisExit < exit) {
        exit = axisExit;
        exitNormal = axisExitNormal;
      }
      if (enter > exit + kEpsilon) {
        intersects = false;
        break;
      }
    }
    if (intersects && std::isfinite(exit)) {
      considerFloor(exit, rotate(rotation, exitNormal), box.material, minZ, maxZ, hit);
    }
  }

  for (const col::Triangle& triangle : model.triangles) {
    if (triangle.a >= model.vertices.size() || triangle.b >= model.vertices.size() ||
        triangle.c >= model.vertices.size()) {
      continue;
    }
    const col::Vec3 a = transformPoint(rotation, position, model.vertices[triangle.a]);
    const col::Vec3 b = transformPoint(rotation, position, model.vertices[triangle.b]);
    const col::Vec3 c = transformPoint(rotation, position, model.vertices[triangle.c]);
    const float denominator = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
    if (!std::isfinite(denominator) || std::fabs(denominator) <= kEpsilon) continue;
    const float u = ((b.y - c.y) * (x - c.x) + (c.x - b.x) * (y - c.y)) / denominator;
    const float v = ((c.y - a.y) * (x - c.x) + (a.x - c.x) * (y - c.y)) / denominator;
    const float w = 1.0f - u - v;
    if (u < -kEpsilon || v < -kEpsilon || w < -kEpsilon) continue;
    const float z = u * a.z + v * b.z + w * c.z;
    const col::Vec3 normal = cross(subtract(b, a), subtract(c, a));
    if (std::fabs(normal.z) <= kEpsilon) continue;
    considerFloor(z, normal, triangle.material, minZ, maxZ, hit);
  }
}

float clampFloat(float value, float minimum, float maximum) {
  return std::max(minimum, std::min(value, maximum));
}

float distanceSquaredToSegment2d(float x, float y, const col::Vec3& a, const col::Vec3& b) {
  const float dx = b.x - a.x;
  const float dy = b.y - a.y;
  const float lengthSquared = dx * dx + dy * dy;
  if (!std::isfinite(lengthSquared) || lengthSquared <= kEpsilon * kEpsilon) {
    const float px = x - a.x;
    const float py = y - a.y;
    return px * px + py * py;
  }
  const float fraction = clampFloat(((x - a.x) * dx + (y - a.y) * dy) / lengthSquared, 0.0f, 1.0f);
  const float px = x - (a.x + dx * fraction);
  const float py = y - (a.y + dy * fraction);
  return px * px + py * py;
}

bool pointInsideTriangle2d(float x, float y, const col::Vec3& a, const col::Vec3& b,
                           const col::Vec3& c) {
  const float denominator = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
  if (!std::isfinite(denominator) || std::fabs(denominator) <= kEpsilon) return false;
  const float u = ((b.y - c.y) * (x - c.x) + (c.x - b.x) * (y - c.y)) / denominator;
  const float v = ((c.y - a.y) * (x - c.x) + (a.x - c.x) * (y - c.y)) / denominator;
  const float w = 1.0f - u - v;
  return std::isfinite(u) && std::isfinite(v) && std::isfinite(w) &&
         u >= -kEpsilon && v >= -kEpsilon && w >= -kEpsilon;
}

bool buildBoxWorldBounds(const col::Box& box, const col::Vec3& position,
                         const map::Quaternion& rotation, col::Vec3& minimum,
                         col::Vec3& maximum) {
  if (!finiteVec(box.minimum) || !finiteVec(box.maximum) || box.minimum.x > box.maximum.x ||
      box.minimum.y > box.maximum.y || box.minimum.z > box.maximum.z) {
    return false;
  }
  const float infinity = std::numeric_limits<float>::infinity();
  minimum = {infinity, infinity, infinity};
  maximum = {-infinity, -infinity, -infinity};
  for (int x = 0; x < 2; ++x) {
    for (int y = 0; y < 2; ++y) {
      for (int z = 0; z < 2; ++z) {
        const col::Vec3 local{
            x == 0 ? box.minimum.x : box.maximum.x,
            y == 0 ? box.minimum.y : box.maximum.y,
            z == 0 ? box.minimum.z : box.maximum.z,
        };
        const col::Vec3 world = transformPoint(rotation, position, local);
        if (!finiteVec(world)) return false;
        minimum.x = std::min(minimum.x, world.x);
        minimum.y = std::min(minimum.y, world.y);
        minimum.z = std::min(minimum.z, world.z);
        maximum.x = std::max(maximum.x, world.x);
        maximum.y = std::max(maximum.y, world.y);
        maximum.z = std::max(maximum.z, world.z);
      }
    }
  }
  return finiteVec(minimum) && finiteVec(maximum);
}

bool verticalOverlap(float lower, float upper, float minimum, float maximum) {
  return std::isfinite(lower) && std::isfinite(upper) && std::isfinite(minimum) &&
         std::isfinite(maximum) && lower <= upper && minimum <= maximum &&
         upper >= minimum - kEpsilon && lower <= maximum + kEpsilon;
}

bool cylinderBlocksModel(const col::Model& model, const col::Vec3& position,
                         const map::Quaternion& rotation, float x, float y,
                         float bodyMinimumZ, float bodyMaximumZ, float radius) {
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(bodyMinimumZ) ||
      !std::isfinite(bodyMaximumZ) || !std::isfinite(radius) || radius <= 0.0f ||
      bodyMinimumZ > bodyMaximumZ) {
    return false;
  }
  const float radiusSquared = radius * radius;
  for (const col::Sphere& sphere : model.spheres) {
    if (!finiteVec(sphere.center) || !std::isfinite(sphere.radius) || sphere.radius <= 0.0f) continue;
    const col::Vec3 center = transformPoint(rotation, position, sphere.center);
    if (!finiteVec(center) || !verticalOverlap(bodyMinimumZ, bodyMaximumZ,
                                               center.z - sphere.radius, center.z + sphere.radius)) {
      continue;
    }
    const float dx = x - center.x;
    const float dy = y - center.y;
    const float expandedRadius = sphere.radius + radius;
    if (std::isfinite(dx) && std::isfinite(dy) && std::isfinite(expandedRadius) &&
        dx * dx + dy * dy <= expandedRadius * expandedRadius) {
      return true;
    }
  }

  for (const col::Box& box : model.boxes) {
    col::Vec3 minimum;
    col::Vec3 maximum;
    if (!buildBoxWorldBounds(box, position, rotation, minimum, maximum) ||
        !verticalOverlap(bodyMinimumZ, bodyMaximumZ, minimum.z, maximum.z)) {
      continue;
    }
    const float nearestX = clampFloat(x, minimum.x, maximum.x);
    const float nearestY = clampFloat(y, minimum.y, maximum.y);
    const float dx = x - nearestX;
    const float dy = y - nearestY;
    if (std::isfinite(dx) && std::isfinite(dy) && dx * dx + dy * dy <= radiusSquared) return true;
  }

  // COL meshes commonly hold walls as triangles. Limit the per-instance work
  // for an Android input tick; floors/slopes are intentionally left to the
  // vertical grounding query rather than being treated as side walls.
  constexpr size_t kMaxWallTrianglesPerQuery = 4096;
  size_t inspectedTriangles = 0;
  for (const col::Triangle& triangle : model.triangles) {
    if (inspectedTriangles++ >= kMaxWallTrianglesPerQuery) break;
    if (triangle.a >= model.vertices.size() || triangle.b >= model.vertices.size() ||
        triangle.c >= model.vertices.size()) {
      continue;
    }
    const col::Vec3 a = transformPoint(rotation, position, model.vertices[triangle.a]);
    const col::Vec3 b = transformPoint(rotation, position, model.vertices[triangle.b]);
    const col::Vec3 c = transformPoint(rotation, position, model.vertices[triangle.c]);
    if (!finiteVec(a) || !finiteVec(b) || !finiteVec(c)) continue;
    const col::Vec3 normal = cross(subtract(b, a), subtract(c, a));
    const float horizontalNormal = std::sqrt(normal.x * normal.x + normal.y * normal.y);
    if (!std::isfinite(horizontalNormal) || horizontalNormal <= kEpsilon ||
        std::fabs(normal.z) > horizontalNormal * 0.75f) {
      continue;
    }
    const float minimumZ = std::min(a.z, std::min(b.z, c.z));
    const float maximumZ = std::max(a.z, std::max(b.z, c.z));
    if (!verticalOverlap(bodyMinimumZ, bodyMaximumZ, minimumZ, maximumZ)) continue;
    if (pointInsideTriangle2d(x, y, a, b, c) ||
        distanceSquaredToSegment2d(x, y, a, b) <= radiusSquared ||
        distanceSquaredToSegment2d(x, y, b, c) <= radiusSquared ||
        distanceSquaredToSegment2d(x, y, c, a) <= radiusSquared) {
      return true;
    }
  }
  return false;
}

// Static-world raycasts deliberately have independent bounds from movement.
// They are a future collision primitive, not an invitation to enumerate every
// loaded COL face on each Android input sample.
constexpr float kMaxRaycastDistance = 256.0f;
constexpr size_t kMaxRaycastCandidates = 512;
constexpr size_t kMaxRaycastCells = 128;
constexpr size_t kMaxRaycastPrimitiveTests = 8192;
constexpr size_t kMaxRaycastTriangleTests = 16384;

struct RayAabbIntersection {
  bool hit = false;
  float enter = 0.0f;
  float exit = 0.0f;
  col::Vec3 enterNormal;
  col::Vec3 exitNormal;
};

bool normalizedDirection(const col::Vec3& input, col::Vec3& output) {
  if (!finiteVec(input)) return false;
  const float lengthSquared = dot(input, input);
  if (!std::isfinite(lengthSquared) || lengthSquared <= kEpsilon * kEpsilon) return false;
  const float inverseLength = 1.0f / std::sqrt(lengthSquared);
  output = multiply(input, inverseLength);
  return finiteVec(output);
}

bool intersectRayAabb(const col::Vec3& origin, const col::Vec3& direction,
                      const col::Vec3& minimum, const col::Vec3& maximum,
                      float maxDistance, RayAabbIntersection& out) {
  out = {};
  if (!finiteVec(origin) || !finiteVec(direction) || !finiteVec(minimum) ||
      !finiteVec(maximum) || !std::isfinite(maxDistance) || maxDistance <= 0.0f ||
      minimum.x > maximum.x || minimum.y > maximum.y || minimum.z > maximum.z) {
    return false;
  }
  const float origins[3] = {origin.x, origin.y, origin.z};
  const float directions[3] = {direction.x, direction.y, direction.z};
  const float lowers[3] = {minimum.x, minimum.y, minimum.z};
  const float uppers[3] = {maximum.x, maximum.y, maximum.z};
  float enter = 0.0f;
  float exit = maxDistance;
  col::Vec3 enterNormal{};
  col::Vec3 exitNormal{};
  for (int axis = 0; axis < 3; ++axis) {
    const float directionComponent = directions[axis];
    if (std::fabs(directionComponent) <= kEpsilon) {
      if (origins[axis] < lowers[axis] - kEpsilon || origins[axis] > uppers[axis] + kEpsilon) {
        return false;
      }
      continue;
    }
    float axisEnter = 0.0f;
    float axisExit = 0.0f;
    col::Vec3 axisEnterNormal{};
    col::Vec3 axisExitNormal{};
    if (directionComponent > 0.0f) {
      axisEnter = (lowers[axis] - origins[axis]) / directionComponent;
      axisExit = (uppers[axis] - origins[axis]) / directionComponent;
      if (axis == 0) {
        axisEnterNormal.x = -1.0f;
        axisExitNormal.x = 1.0f;
      } else if (axis == 1) {
        axisEnterNormal.y = -1.0f;
        axisExitNormal.y = 1.0f;
      } else {
        axisEnterNormal.z = -1.0f;
        axisExitNormal.z = 1.0f;
      }
    } else {
      axisEnter = (uppers[axis] - origins[axis]) / directionComponent;
      axisExit = (lowers[axis] - origins[axis]) / directionComponent;
      if (axis == 0) {
        axisEnterNormal.x = 1.0f;
        axisExitNormal.x = -1.0f;
      } else if (axis == 1) {
        axisEnterNormal.y = 1.0f;
        axisExitNormal.y = -1.0f;
      } else {
        axisEnterNormal.z = 1.0f;
        axisExitNormal.z = -1.0f;
      }
    }
    if (axisEnter > enter) {
      enter = axisEnter;
      enterNormal = axisEnterNormal;
    }
    if (axisExit < exit) {
      exit = axisExit;
      exitNormal = axisExitNormal;
    }
    if (enter > exit + kEpsilon) return false;
  }
  if (!std::isfinite(enter) || !std::isfinite(exit) || exit < kEpsilon) return false;
  out.hit = true;
  out.enter = std::max(0.0f, enter);
  out.exit = exit;
  out.enterNormal = enterNormal;
  out.exitNormal = exitNormal;
  return true;
}

void considerRayHit(float distance, const col::Vec3& origin, const col::Vec3& direction,
                    col::Vec3 normal, uint8_t material, float maxDistance, RaycastHit& out) {
  if (!std::isfinite(distance) || distance <= kEpsilon || distance > maxDistance + kEpsilon ||
      !finiteVec(normal)) {
    return;
  }
  const float normalLengthSquared = dot(normal, normal);
  if (!std::isfinite(normalLengthSquared) || normalLengthSquared <= kEpsilon * kEpsilon) return;
  normal = multiply(normal, 1.0f / std::sqrt(normalLengthSquared));
  if (dot(normal, direction) > 0.0f) normal = multiply(normal, -1.0f);
  const col::Vec3 position = add(origin, multiply(direction, distance));
  if (!finiteVec(position)) return;
  if (out.hit && distance >= out.distance - kEpsilon) return;
  out.hit = true;
  out.distance = distance;
  out.position = position;
  out.normal = normal;
  out.material = material;
}

struct RaycastBudget {
  size_t primitiveTests = 0;
  size_t triangleTests = 0;
  bool exhausted = false;

  bool consumePrimitive() {
    if (primitiveTests >= kMaxRaycastPrimitiveTests) {
      exhausted = true;
      return false;
    }
    ++primitiveTests;
    return true;
  }

  bool consumeTriangle() {
    if (triangleTests >= kMaxRaycastTriangleTests) {
      exhausted = true;
      return false;
    }
    ++triangleTests;
    return true;
  }
};

bool considerModelRay(const col::Model& model, const col::Vec3& position,
                      const map::Quaternion& rotation, const col::Vec3& origin,
                      const col::Vec3& direction, float maxDistance, RaycastBudget& budget,
                      RaycastHit& out) {
  const col::Vec3 localOrigin = inverseRotate(rotation, subtract(origin, position));
  const col::Vec3 localDirection = inverseRotate(rotation, direction);
  RayAabbIntersection modelBounds;
  if (!intersectRayAabb(localOrigin, localDirection, model.bounds.minimum, model.bounds.maximum,
                        maxDistance, modelBounds)) {
    return true;
  }

  for (const col::Sphere& sphere : model.spheres) {
    if (!budget.consumePrimitive()) return false;
    if (!finiteVec(sphere.center) || !std::isfinite(sphere.radius) || sphere.radius <= 0.0f) continue;
    const col::Vec3 originOffset = subtract(localOrigin, sphere.center);
    const float b = dot(originOffset, localDirection);
    const float c = dot(originOffset, originOffset) - sphere.radius * sphere.radius;
    const float discriminant = b * b - c;
    if (!std::isfinite(discriminant) || discriminant < 0.0f) continue;
    const float root = std::sqrt(std::max(0.0f, discriminant));
    float distance = -b - root;
    if (distance <= kEpsilon) distance = -b + root;
    if (!std::isfinite(distance) || distance <= kEpsilon || distance > maxDistance + kEpsilon) continue;
    const col::Vec3 localPoint = add(localOrigin, multiply(localDirection, distance));
    considerRayHit(distance, origin, direction, rotate(rotation, subtract(localPoint, sphere.center)),
                   sphere.material, maxDistance, out);
  }

  for (const col::Box& box : model.boxes) {
    if (!budget.consumePrimitive()) return false;
    RayAabbIntersection boxHit;
    if (!intersectRayAabb(localOrigin, localDirection, box.minimum, box.maximum, maxDistance,
                          boxHit)) {
      continue;
    }
    const bool entering = boxHit.enter > kEpsilon;
    const float distance = entering ? boxHit.enter : boxHit.exit;
    const col::Vec3 localNormal = entering ? boxHit.enterNormal : boxHit.exitNormal;
    considerRayHit(distance, origin, direction, rotate(rotation, localNormal), box.material,
                   maxDistance, out);
  }

  for (const col::Triangle& triangle : model.triangles) {
    if (!budget.consumeTriangle()) return false;
    if (triangle.a >= model.vertices.size() || triangle.b >= model.vertices.size() ||
        triangle.c >= model.vertices.size()) {
      continue;
    }
    const col::Vec3& a = model.vertices[triangle.a];
    const col::Vec3& b = model.vertices[triangle.b];
    const col::Vec3& c = model.vertices[triangle.c];
    if (!finiteVec(a) || !finiteVec(b) || !finiteVec(c)) continue;
    const col::Vec3 edgeOne = subtract(b, a);
    const col::Vec3 edgeTwo = subtract(c, a);
    const col::Vec3 perpendicular = cross(localDirection, edgeTwo);
    const float determinant = dot(edgeOne, perpendicular);
    if (!std::isfinite(determinant) || std::fabs(determinant) <= kEpsilon) continue;
    const float inverseDeterminant = 1.0f / determinant;
    const col::Vec3 originToA = subtract(localOrigin, a);
    const float u = dot(originToA, perpendicular) * inverseDeterminant;
    if (!std::isfinite(u) || u < -kEpsilon || u > 1.0f + kEpsilon) continue;
    const col::Vec3 q = cross(originToA, edgeOne);
    const float v = dot(localDirection, q) * inverseDeterminant;
    if (!std::isfinite(v) || v < -kEpsilon || u + v > 1.0f + kEpsilon) continue;
    const float distance = dot(edgeTwo, q) * inverseDeterminant;
    if (!std::isfinite(distance) || distance <= kEpsilon || distance > maxDistance + kEpsilon) {
      continue;
    }
    considerRayHit(distance, origin, direction, rotate(rotation, cross(edgeOne, edgeTwo)),
                   triangle.material, maxDistance, out);
  }
  return !budget.exhausted;
}

}  // namespace

bool buildCollisionWorld(const CollisionCatalog& catalog, const std::vector<map::Instance>& placements,
                         CollisionWorld& out, CollisionWorldBuildReport& report, std::string& error,
                         size_t maxInstances, size_t maxSpatialReferences) {
  report = {};
  error.clear();
  if (maxInstances == 0 || maxInstances > kMaxCollisionWorldInstances) {
    maxInstances = kMaxCollisionWorldInstances;
  }
  if (maxSpatialReferences == 0 || maxSpatialReferences > kMaxSpatialReferences) {
    maxSpatialReferences = kMaxSpatialReferences;
  }

  CollisionWorld candidate;
  CollisionWorldBuildReport candidateReport;
  candidate.instances_.reserve(std::min(placements.size(), maxInstances));
  for (const map::Instance& placement : placements) {
    ++candidateReport.consideredPlacements;
    const col::Model* model = catalog.find(placement.modelName);
    if (!model) {
      ++candidateReport.missingCollisionModels;
      continue;
    }
    if (candidate.instances_.size() >= maxInstances) {
      error = "jumlah placement collision melebihi batas aman";
      return false;
    }
    map::Quaternion rotation = placement.rotation;
    col::Vec3 worldMinimum;
    col::Vec3 worldMaximum;
    if (!normalizeQuaternion(rotation) ||
        !buildWorldBounds(*model, placement.position, rotation, worldMinimum, worldMaximum)) {
      error = "transform placement collision tidak valid";
      return false;
    }
    candidate.instances_.push_back({model, placement.position, rotation, placement.interior,
                                    worldMinimum, worldMaximum});
    ++candidateReport.attachedPlacements;
  }

  size_t spatialReferences = 0;
  for (size_t index = 0; index < candidate.instances_.size(); ++index) {
    const CollisionWorld::Instance& instance = candidate.instances_[index];
    int32_t minX = 0;
    int32_t maxX = 0;
    int32_t minY = 0;
    int32_t maxY = 0;
    if (!cellCoordinate(instance.worldMinimum.x, minX) || !cellCoordinate(instance.worldMaximum.x, maxX) ||
        !cellCoordinate(instance.worldMinimum.y, minY) || !cellCoordinate(instance.worldMaximum.y, maxY) ||
        minX > maxX || minY > maxY) {
      error = "bounds placement collision tidak dapat diindeks";
      return false;
    }
    const int64_t width = static_cast<int64_t>(maxX) - static_cast<int64_t>(minX) + 1;
    const int64_t height = static_cast<int64_t>(maxY) - static_cast<int64_t>(minY) + 1;
    if (width <= 0 || height <= 0 || width > kMaxCellsPerPlacement ||
        height > kMaxCellsPerPlacement || width > kMaxCellsPerPlacement / height) {
      candidate.largeInstances_.push_back(static_cast<uint32_t>(index));
      ++candidateReport.largePlacements;
      continue;
    }
    const size_t references = static_cast<size_t>(width * height);
    if (references > maxSpatialReferences - spatialReferences) {
      error = "spatial index collision melebihi batas aman";
      return false;
    }
    for (int64_t cellX = minX; cellX <= maxX; ++cellX) {
      for (int64_t cellY = minY; cellY <= maxY; ++cellY) {
        candidate.cells_[cellKey(static_cast<int32_t>(cellX), static_cast<int32_t>(cellY))]
            .push_back(static_cast<uint32_t>(index));
      }
    }
    spatialReferences += references;
  }

  out = std::move(candidate);
  report = candidateReport;
  return true;
}

bool CollisionWorld::findHighestFloor(float x, float y, float minZ, float maxZ, int32_t interior,
                                      col::FloorHit& out) const {
  out = {};
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(minZ) || !std::isfinite(maxZ) ||
      minZ > maxZ) {
    return false;
  }
  int32_t cellX = 0;
  int32_t cellY = 0;
  if (!cellCoordinate(x, cellX) || !cellCoordinate(y, cellY)) return false;

  const auto considerInstance = [&](uint32_t index) {
    if (index >= instances_.size()) return;
    const Instance& instance = instances_[index];
    if (!instance.model || instance.interior != interior ||
        x < instance.worldMinimum.x - kEpsilon || x > instance.worldMaximum.x + kEpsilon ||
        y < instance.worldMinimum.y - kEpsilon || y > instance.worldMaximum.y + kEpsilon ||
        maxZ < instance.worldMinimum.z - kEpsilon || minZ > instance.worldMaximum.z + kEpsilon) {
      return;
    }
    considerModelFloor(*instance.model, instance.position, instance.rotation, x, y, minZ, maxZ, out);
  };

  const auto bucket = cells_.find(cellKey(cellX, cellY));
  if (bucket != cells_.end()) {
    for (const uint32_t index : bucket->second) considerInstance(index);
  }
  for (const uint32_t index : largeInstances_) considerInstance(index);
  return out.hit;
}

HorizontalMoveResult CollisionWorld::resolveHorizontalMove(float startX, float startY, float targetX,
                                                            float targetY, float feetZ, float radius,
                                                            float height, int32_t interior) const {
  HorizontalMoveResult result;
  result.position = {startX, startY, feetZ};
  if (!std::isfinite(startX) || !std::isfinite(startY) || !std::isfinite(targetX) ||
      !std::isfinite(targetY) || !std::isfinite(feetZ) || !std::isfinite(radius) ||
      !std::isfinite(height) || radius <= 0.05f || radius > 2.0f || height <= 0.10f ||
      height > 4.0f) {
    return result;
  }
  const float bodyMinimumZ = feetZ + 0.08f;
  const float bodyMaximumZ = feetZ + height;
  bool queryValid = true;
  const auto occupied = [&](float x, float y) {
    int32_t minimumCellX = 0;
    int32_t maximumCellX = 0;
    int32_t minimumCellY = 0;
    int32_t maximumCellY = 0;
    if (!cellCoordinate(x - radius, minimumCellX) || !cellCoordinate(x + radius, maximumCellX) ||
        !cellCoordinate(y - radius, minimumCellY) || !cellCoordinate(y + radius, maximumCellY) ||
        minimumCellX > maximumCellX || minimumCellY > maximumCellY) {
      queryValid = false;
      return true;
    }
    const auto occupiedBy = [&](uint32_t index) {
      if (index >= instances_.size()) return false;
      const Instance& instance = instances_[index];
      if (!instance.model || instance.interior != interior ||
          x + radius < instance.worldMinimum.x - kEpsilon ||
          x - radius > instance.worldMaximum.x + kEpsilon ||
          y + radius < instance.worldMinimum.y - kEpsilon ||
          y - radius > instance.worldMaximum.y + kEpsilon ||
          !verticalOverlap(bodyMinimumZ, bodyMaximumZ, instance.worldMinimum.z,
                           instance.worldMaximum.z)) {
        return false;
      }
      return cylinderBlocksModel(*instance.model, instance.position, instance.rotation, x, y,
                                 bodyMinimumZ, bodyMaximumZ, radius);
    };
    for (int64_t cellX = minimumCellX; cellX <= maximumCellX; ++cellX) {
      for (int64_t cellY = minimumCellY; cellY <= maximumCellY; ++cellY) {
        const auto bucket = cells_.find(cellKey(static_cast<int32_t>(cellX),
                                                 static_cast<int32_t>(cellY)));
        if (bucket == cells_.end()) continue;
        for (const uint32_t index : bucket->second) {
          if (occupiedBy(index)) return true;
        }
      }
    }
    for (const uint32_t index : largeInstances_) {
      if (occupiedBy(index)) return true;
    }
    return false;
  };

  const bool targetBlocked = occupied(targetX, targetY);
  if (!queryValid) return result;
  result.valid = true;
  if (!targetBlocked) {
    result.position = {targetX, targetY, feetZ};
    return result;
  }

  // Resolve one short input step with deterministic axis sliding. Local
  // movement caps each step below 0.3 GTA units, while the player radius keeps
  // thin-wall crossing conservative without a long unbounded ray cast.
  result.blocked = true;
  float resolvedX = startX;
  float resolvedY = startY;
  if (!occupied(targetX, startY)) resolvedX = targetX;
  if (!queryValid) {
    result.valid = false;
    return result;
  }
  if (!occupied(resolvedX, targetY)) resolvedY = targetY;
  if (!queryValid) {
    result.valid = false;
    return result;
  }
  result.position = {resolvedX, resolvedY, feetZ};
  return result;
}

bool CollisionWorld::raycastStaticWorld(const col::Vec3& origin, const col::Vec3& direction,
                                        float maxDistance, int32_t interior,
                                        RaycastHit& out) const {
  out = {};
  col::Vec3 normalized;
  if (!finiteVec(origin) || !normalizedDirection(direction, normalized) ||
      !std::isfinite(maxDistance) || maxDistance <= kEpsilon ||
      maxDistance > kMaxRaycastDistance) {
    return false;
  }
  const col::Vec3 end = add(origin, multiply(normalized, maxDistance));
  if (!finiteVec(end)) return false;
  int32_t minimumCellX = 0;
  int32_t maximumCellX = 0;
  int32_t minimumCellY = 0;
  int32_t maximumCellY = 0;
  if (!cellCoordinate(std::min(origin.x, end.x), minimumCellX) ||
      !cellCoordinate(std::max(origin.x, end.x), maximumCellX) ||
      !cellCoordinate(std::min(origin.y, end.y), minimumCellY) ||
      !cellCoordinate(std::max(origin.y, end.y), maximumCellY) ||
      minimumCellX > maximumCellX || minimumCellY > maximumCellY) {
    return false;
  }
  const int64_t width = static_cast<int64_t>(maximumCellX) - minimumCellX + 1;
  const int64_t height = static_cast<int64_t>(maximumCellY) - minimumCellY + 1;
  const int64_t maxRaycastCells = static_cast<int64_t>(kMaxRaycastCells);
  if (width <= 0 || height <= 0 || width > maxRaycastCells || height > maxRaycastCells ||
      width > maxRaycastCells / height) {
    return false;
  }

  std::array<uint32_t, kMaxRaycastCandidates> candidates{};
  size_t candidateCount = 0;
  const auto addCandidate = [&](uint32_t index) {
    if (index >= instances_.size()) return true;
    const Instance& instance = instances_[index];
    if (!instance.model || instance.interior != interior) return true;
    RayAabbIntersection boundsHit;
    if (!intersectRayAabb(origin, normalized, instance.worldMinimum, instance.worldMaximum,
                          maxDistance, boundsHit)) {
      return true;
    }
    for (size_t previous = 0; previous < candidateCount; ++previous) {
      if (candidates[previous] == index) return true;
    }
    if (candidateCount >= candidates.size()) return false;
    candidates[candidateCount++] = index;
    return true;
  };

  for (int64_t cellX = minimumCellX; cellX <= maximumCellX; ++cellX) {
    for (int64_t cellY = minimumCellY; cellY <= maximumCellY; ++cellY) {
      const auto bucket = cells_.find(
          cellKey(static_cast<int32_t>(cellX), static_cast<int32_t>(cellY)));
      if (bucket == cells_.end()) continue;
      for (const uint32_t index : bucket->second) {
        if (!addCandidate(index)) return false;
      }
    }
  }
  for (const uint32_t index : largeInstances_) {
    if (!addCandidate(index)) return false;
  }

  RaycastBudget budget;
  for (size_t candidate = 0; candidate < candidateCount; ++candidate) {
    const Instance& instance = instances_[candidates[candidate]];
    if (!considerModelRay(*instance.model, instance.position, instance.rotation, origin, normalized,
                          maxDistance, budget, out)) {
      out = {};
      return false;
    }
  }
  out.valid = true;
  return true;
}

}  // namespace samp::gta::world
