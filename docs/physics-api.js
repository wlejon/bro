// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * =============================================================================
 * Physics — Jolt Physics 3D Simulation & Collision System
 * =============================================================================
 *
 * Complete 3D physics engine binding backed by Jolt Physics.
 * Includes rigid body dynamics, collision queries (raycast, shape cast, overlap),
 * character virtual controllers, vehicles, ragdolls, soft bodies, constraints,
 * and sandbox worlds.
 * @typedef {Object} PhysicsVec3
 * @property {number} [x]
 * @property {number} [y]
 * @property {number} [z]
 */

/**
 * @typedef {Object} PhysicsQuat
 * @property {number} [x]
 * @property {number} [y]
 * @property {number} [z]
 * @property {number} [w]
 */

/**
 * @typedef {Object} PhysicsTransform
 * @property {PhysicsVec3} [position]
 * @property {PhysicsQuat} [rotation]
 * @property {number} [userData]
 */

/**
 * @typedef {Object} PhysicsVelocity
 * @property {PhysicsVec3} [linear]
 * @property {PhysicsVec3} [angular]
 */

/**
 * @typedef {Object} PhysicsAreaOverride
 * @property {string} [gravityMode]
 * @property {PhysicsVec3} [gravity]
 * @property {boolean} [gravityPoint]
 * @property {number} [gravityStrength]
 * @property {number} [falloffDistance]
 * @property {number} [gravityScale]
 * @property {number} [linearDamping]
 * @property {number} [angularDamping]
 * @property {number} [priority]
 */

/**
 * @typedef {Object} PhysicsCompoundPart
 * @property {string} [shape]
 * @property {PhysicsVec3} [position]
 * @property {PhysicsQuat} [rotation]
 * @property {PhysicsVec3} [localPosition]
 * @property {PhysicsQuat} [localRotation]
 * @property {PhysicsVec3} [halfExtents]
 * @property {number} [radius]
 * @property {number} [halfHeight]
 * @property {number} [density]
 * @property {number} [friction]
 * @property {number} [restitution]
 */

/**
 * @typedef {Object} PhysicsBodyOptions
 * @property {string} [shape]
 * @property {PhysicsVec3} [position]
 * @property {PhysicsQuat} [rotation]
 * @property {PhysicsVec3} [localPosition]
 * @property {PhysicsQuat} [localRotation]
 * @property {PhysicsVec3} [halfExtents]
 * @property {number} [radius]
 * @property {number} [halfHeight]
 * @property {boolean} [static]
 * @property {boolean} [isStatic]
 * @property {boolean} [sensor]
 * @property {boolean} [isSensor]
 * @property {boolean} [ccd]
 * @property {number} [friction]
 * @property {number} [restitution]
 * @property {string} [frictionCombine]
 * @property {string} [restitutionCombine]
 * @property {number} [density]
 * @property {number} [mass]
 * @property {number} [gravityFactor]
 * @property {number} [linearDamping]
 * @property {number} [angularDamping]
 * @property {number} [maxLinearVelocity]
 * @property {number} [maxAngularVelocity]
 * @property {number} [userData]
 * @property {string} [dofs]
 * @property {number} [layer]
 * @property {Array<number>} [points]
 * @property {Array<number>} [positions]
 * @property {Array<number>} [indices]
 * @property {Array<PhysicsCompoundPart>} [parts]
 * @property {PhysicsAreaOverride} [area]
 */

/**
 * @typedef {Object} PhysicsLayersConfig
 * @property {Array<string>} [names]
 * @property {Array<boolean>} [matrix]
 */

/**
 * @typedef {Object} PhysicsRayHit
 * @property {number} [body]
 * @property {number} [bodyId]
 * @property {number} [fraction]
 * @property {number} [userData]
 * @property {PhysicsVec3} [position]
 * @property {PhysicsVec3} [normal]
 */

/**
 * @typedef {Object} PhysicsShapeCastHit
 * @property {number} [body]
 * @property {number} [bodyId]
 * @property {number} [fraction]
 * @property {number} [userData]
 * @property {PhysicsVec3} [position]
 * @property {PhysicsVec3} [normal]
 */

/**
 * @typedef {Object} PhysicsShapeCastOptions
 * @property {string} [shape]
 * @property {PhysicsVec3} [position]
 * @property {PhysicsQuat} [rotation]
 * @property {PhysicsVec3} [halfExtents]
 * @property {number} [radius]
 * @property {number} [halfHeight]
 * @property {PhysicsVec3} [direction]
 * @property {number} [maxDistance]
 * @property {number} [ignoreBody]
 * @property {Array<number>} [ignoreBodies]
 * @property {Array<string>} [layers]
 */

/**
 * @typedef {Object} PhysicsOverlapShapeOptions
 * @property {string} [shape]
 * @property {PhysicsVec3} [position]
 * @property {PhysicsQuat} [rotation]
 * @property {PhysicsVec3} [halfExtents]
 * @property {number} [radius]
 * @property {number} [halfHeight]
 * @property {number} [ignoreBody]
 * @property {Array<number>} [ignoreBodies]
 * @property {Array<string>} [layers]
 */

/**
 * @typedef {Object} PhysicsContactPoint
 * @property {number} [x]
 * @property {number} [y]
 * @property {number} [z]
 */

/**
 * @typedef {Object} PhysicsContact
 * @property {string} [type]
 * @property {number} [body1]
 * @property {number} [body2]
 * @property {boolean} [sensor]
 * @property {PhysicsVec3} [normal]
 * @property {number} [penetration]
 * @property {number} [impulse]
 * @property {Array<PhysicsContactPoint>} [points]
 */

/**
 * @typedef {Object} PhysicsBodyProperties
 * @property {number} [mass]
 * @property {number} [friction]
 * @property {number} [restitution]
 * @property {number} [linearDamping]
 * @property {number} [angularDamping]
 * @property {number} [gravityFactor]
 * @property {string} [motionType]
 * @property {number} [layer]
 * @property {boolean} [isSensor]
 * @property {number} [userData]
 */

/**
 * @typedef {Object} PhysicsWorldOptions
 * @property {PhysicsVec3} [gravity]
 * @property {number} [maxBodies]
 */

/**
 * @typedef {Object} PhysicsCharacterOptions
 * @property {PhysicsVec3} [position]
 * @property {number} [radius]
 * @property {number} [halfHeight]
 * @property {number} [mass]
 * @property {number} [maxSlopeAngle]
 * @property {number} [maxStrength]
 * @property {PhysicsVec3} [shapeOffset]
 * @property {string} [layer]
 */

/**
 * @typedef {Object} PhysicsCharacterState
 * @property {string} [groundState]
 * @property {PhysicsVec3} [groundPosition]
 * @property {PhysicsVec3} [groundNormal]
 * @property {PhysicsVec3} [groundVelocity]
 */

/**
 * @typedef {Object} PhysicsVehicleOptions
 * @property {PhysicsVec3} [position]
 * @property {PhysicsQuat} [rotation]
 */

/**
 * @typedef {Object} PhysicsRagdollOptions
 * @property {PhysicsVec3} [position]
 * @property {PhysicsQuat} [rotation]
 */

/**
 * @typedef {Object} PhysicsClothOptions
 * @property {number} [gridX]
 * @property {number} [gridZ]
 * @property {number} [spacing]
 * @property {number} [mass]
 * @property {string} [pinned]
 */

/**
 * @typedef {Object} PhysicsSoftBodyOptions
 * @property {PhysicsClothOptions} [cloth]
 * @property {PhysicsVec3} [position]
 * @property {PhysicsQuat} [rotation]
 */

/**
 * @typedef {Object} PhysicsSoftBodyTopology
 * @property {number} [gridX]
 * @property {number} [gridZ]
 */

/**
 * @typedef {Object} PhysicsBounds
 * @property {PhysicsVec3} [min]
 * @property {PhysicsVec3} [max]
 */

/**
 * @typedef {Object} PhysicsPose
 * @property {Array<number>} [data]
 */

/**
 * @typedef {Object} PhysicsConstraintOptions
 * @property {string} [type]
 * @property {number} [body1]
 * @property {number} [body2]
 * @property {PhysicsVec3} [point1]
 * @property {PhysicsVec3} [point2]
 * @property {number} [minDistance]
 * @property {number} [maxDistance]
 * @property {PhysicsVec3} [axis1]
 * @property {PhysicsVec3} [axis2]
 * @property {number} [minAngle]
 * @property {number} [maxAngle]
 * @property {number} [minLimit]
 * @property {number} [maxLimit]
 */

/**
 * @typedef {Object} PhysicsConstraintMotorOptions
 * @property {string} [type]
 * @property {number} [target]
 * @property {number} [maxForce]
 * @property {number} [maxTorque]
 * @property {number} [frequency]
 * @property {number} [damping]
 * @property {string} [axis]
 */

// ── Classes & Interfaces ─────────────────────────────────────────────────────

class PhysicsWorldHandle {

  destroy() {}

  /**
   * @param {number} dt
   */
  step(dt) {}

}

class PhysicsCharacter {

  /**
   * @param {number} x
   * @param {number} y
   * @param {number} z
   */
  setPosition(x, y, z) {}

  /**
   * @param {number} x
   * @param {number} y
   * @param {number} z
   */
  setVelocity(x, y, z) {}

  /**
   * @param {number} x
   * @param {number} y
   * @param {number} z
   */
  setLinearVelocity(x, y, z) {}

  /**
   * @returns {PhysicsVec3}
   */
  getPosition() {}

  /**
   * @returns {PhysicsVec3}
   */
  getVelocity() {}

  /**
   * @returns {PhysicsVec3}
   */
  getLinearVelocity() {}

  /**
   * @returns {PhysicsCharacterState}
   */
  getState() {}

  /**
   * @param {number} dt
   */
  update(dt) {}

  destroy() {}

}

class PhysicsVehicle {

  /**
   * @param {number} forward
   * @param {number} steer
   * @param {number} brake
   * @param {number} handBrake
   */
  setDriverInput(forward, steer, brake, handBrake) {}

  /**
   * @returns {PhysicsTransform}
   */
  getTransform() {}

  destroy() {}

}

class PhysicsRagdoll {

  /**
   * @param {PhysicsPose} pose
   * @param {number} dt
   */
  driveToPose(pose, dt) {}

  /**
   * @returns {PhysicsPose}
   */
  getPose() {}

  destroy() {}

}

class PhysicsSoftBody {

  /**
   * @readonly
   * @type {number}
   */
  vertexCount;

  /**
   * @returns {PhysicsSoftBodyTopology}
   */
  topology() {}

  /**
   * @returns {Float32Array}
   */
  vertices() {}

  /**
   * @param {number} index
   * @param {boolean} [pinned=true]
   * @returns {boolean}
   */
  pin(index, pinned) {}

  /**
   * @param {number} index
   * @param {number} x
   * @param {number} y
   * @param {number} z
   * @returns {boolean}
   */
  setVertex(index, x, y, z) {}

  /**
   * @param {number} index
   * @param {number} x
   * @param {number} y
   * @param {number} z
   * @returns {boolean}
   */
  setVertexVelocity(index, x, y, z) {}

  /**
   * @returns {PhysicsBounds}
   */
  getBounds() {}

  destroy() {}

}

// ── Namespaces ───────────────────────────────────────────────────────────────

/**
 * @param {PhysicsWorldOptions} [opts]
 * @returns {PhysicsWorldHandle}
 */
Physics.createWorldHandle = function(opts) {};

/**
 * @param {PhysicsWorldOptions} [opts]
 */
Physics.createWorld = function(opts) {};

/**
 * @param {number} x
 * @param {number} y
 * @param {number} z
 */
Physics.setGravity = function(x, y, z) {};

/**
 * @returns {PhysicsVec3}
 */
Physics.getGravity = function() {};

/**
 * @param {PhysicsLayersConfig} config
 * @returns {boolean}
 */
Physics.setLayers = function(config) {};

/**
 * @param {PhysicsBodyOptions} config
 * @returns {number}
 */
Physics.createBody = function(config) {};

/**
 * @param {number} tag
 */
Physics.destroyBody = function(tag) {};

Physics.destroyAll = function() {};

/**
 * @param {number} tag
 * @returns {PhysicsTransform}
 */
Physics.getTransform = function(tag) {};

/**
 * @param {number} tag
 * @returns {PhysicsVelocity}
 */
Physics.getVelocity = function(tag) {};

/**
 * @param {number} tag
 * @param {number} x
 * @param {number} y
 * @param {number} z
 */
Physics.setPosition = function(tag, x, y, z) {};

/**
 * @param {number} tag
 * @param {number} x
 * @param {number} y
 * @param {number} z
 * @param {number} w
 */
Physics.setRotation = function(tag, x, y, z, w) {};

/**
 * @param {number} tag
 * @param {number} x
 * @param {number} y
 * @param {number} z
 */
Physics.setLinearVelocity = function(tag, x, y, z) {};

/**
 * @param {number} tag
 * @param {number} x
 * @param {number} y
 * @param {number} z
 */
Physics.setAngularVelocity = function(tag, x, y, z) {};

/**
 * @param {number} tag
 * @param {number} x
 * @param {number} y
 * @param {number} z
 */
Physics.addForce = function(tag, x, y, z) {};

/**
 * @param {number} tag
 * @param {number} x
 * @param {number} y
 * @param {number} z
 */
Physics.addImpulse = function(tag, x, y, z) {};

/**
 * @param {number} tag
 * @param {number} x
 * @param {number} y
 * @param {number} z
 */
Physics.addTorque = function(tag, x, y, z) {};

/**
 * @param {number} tag
 * @param {number} data
 */
Physics.setUserData = function(tag, data) {};

/**
 * @param {number} tag
 * @returns {number}
 */
Physics.getUserData = function(tag) {};

/**
 * @param {number} tag
 * @param {number} layer
 */
Physics.setLayer = function(tag, layer) {};

/**
 * @param {number} tag
 */
Physics.setKinematic = function(tag) {};

/**
 * @param {number} tag
 * @param {*} type
 */
Physics.setMotionType = function(tag, type) {};

/**
 * @param {number} tag
 * @param {number} x
 * @param {number} y
 * @param {number} z
 * @param {number} dt
 */
Physics.moveKinematic = function(tag, x, y, z, dt) {};

Physics.raycast = function() {};

Physics.raycastClosest = function() {};

Physics.castShape = function() {};

Physics.castShapeClosest = function() {};

Physics.overlapShape = function() {};

Physics.overlapSphere = function() {};

Physics.overlapBox = function() {};

Physics.overlapPoint = function() {};

Physics.onContact = function() {};

Physics.addEventListener = function() {};

Physics.removeEventListener = function() {};

/**
 * @param {number} ox
 * @param {number} oy
 * @param {number} oz
 * @param {number} dx
 * @param {number} dy
 * @param {number} dz
 * @param {number} maxDist
 * @param {number} [mask=0]
 * @returns {PhysicsRayHit}
 */
Physics.raycastClosestRaw = function(ox, oy, oz, dx, dy, dz, maxDist, mask) {};

/**
 * @param {number} ox
 * @param {number} oy
 * @param {number} oz
 * @param {number} dx
 * @param {number} dy
 * @param {number} dz
 * @param {number} maxDist
 * @param {number} [mask=0]
 * @returns {Array<PhysicsRayHit>}
 */
Physics.raycastRaw = function(ox, oy, oz, dx, dy, dz, maxDist, mask) {};

/**
 * @param {PhysicsShapeCastOptions} config
 * @returns {Array<PhysicsShapeCastHit>}
 */
Physics.castShapeRaw = function(config) {};

/**
 * @param {PhysicsShapeCastOptions} config
 * @returns {PhysicsShapeCastHit}
 */
Physics.castShapeClosestRaw = function(config) {};

/**
 * @param {PhysicsOverlapShapeOptions} config
 * @returns {Array<number>}
 */
Physics.overlapShapeRaw = function(config) {};

/**
 * @param {number} x
 * @param {number} y
 * @param {number} z
 * @param {number} radius
 * @returns {Array<number>}
 */
Physics.overlapSphereRaw = function(x, y, z, radius) {};

/**
 * @param {number} cx
 * @param {number} cy
 * @param {number} cz
 * @param {number} hx
 * @param {number} hy
 * @param {number} hz
 * @returns {Array<number>}
 */
Physics.overlapBoxRaw = function(cx, cy, cz, hx, hy, hz) {};

/**
 * @param {number} x
 * @param {number} y
 * @param {number} z
 * @param {number} [mask=0]
 * @returns {Array<number>}
 */
Physics.overlapPointRaw = function(x, y, z, mask) {};

/**
 * @returns {Array<PhysicsContact>}
 */
Physics.getContacts = function() {};

/**
 * @param {number} tag
 * @param {string} mode
 */
Physics.setFrictionCombine = function(tag, mode) {};

/**
 * @param {number} tag
 * @param {string} mode
 */
Physics.setRestitutionCombine = function(tag, mode) {};

/**
 * @param {number} tag
 * @returns {number}
 */
Physics.getMass = function(tag) {};

/**
 * @param {number} tag
 * @param {number} mass
 */
Physics.setMass = function(tag, mass) {};

/**
 * @param {number} tag
 * @param {number} damping
 */
Physics.setLinearDamping = function(tag, damping) {};

/**
 * @param {number} tag
 * @param {number} damping
 */
Physics.setAngularDamping = function(tag, damping) {};

/**
 * @param {number} tag
 * @param {number} factor
 */
Physics.setGravityFactor = function(tag, factor) {};

/**
 * @param {number} tag
 * @param {number} friction
 */
Physics.setFriction = function(tag, friction) {};

/**
 * @param {number} tag
 * @param {number} restitution
 */
Physics.setRestitution = function(tag, restitution) {};

/**
 * @param {number} tag
 * @returns {PhysicsBodyProperties}
 */
Physics.getBodyProperties = function(tag) {};

/**
 * @param {number} tag
 * @param {PhysicsAreaOverride} config
 */
Physics.setAreaOverride = function(tag, config) {};

/**
 * @param {number} dt
 */
Physics.setTimeStep = function(dt) {};

/**
 * @param {number} dt
 */
Physics.step = function(dt) {};

/**
 * @param {boolean} enabled
 */
Physics.setInterpolation = function(enabled) {};

/**
 * @returns {boolean}
 */
Physics.getInterpolation = function() {};

/**
 * @param {number} tag
 * @returns {boolean}
 */
Physics.isActive = function(tag) {};

/**
 * @param {number} tag
 */
Physics.activate = function(tag) {};

/**
 * @param {number} [worldHandle]
 * @returns {Float32Array}
 */
Physics.getAllTransforms = function(worldHandle) {};

/**
 * @param {PhysicsCharacterOptions} config
 * @returns {PhysicsCharacter}
 */
Physics.createCharacter = function(config) {};

/**
 * @param {PhysicsVehicleOptions} config
 * @returns {PhysicsVehicle}
 */
Physics.createVehicle = function(config) {};

/**
 * @param {PhysicsRagdollOptions} config
 * @returns {PhysicsRagdoll}
 */
Physics.createRagdoll = function(config) {};

/**
 * @param {PhysicsSoftBodyOptions} config
 * @returns {PhysicsSoftBody}
 */
Physics.createSoftBody = function(config) {};

/**
 * @param {PhysicsConstraintOptions} config
 * @returns {number}
 */
Physics.createConstraint = function(config) {};

/**
 * @param {number} tag
 */
Physics.destroyConstraint = function(tag) {};

/**
 * @param {number} tag
 * @param {boolean} enabled
 */
Physics.setConstraintEnabled = function(tag, enabled) {};

/**
 * @param {number} tag
 * @returns {boolean}
 */
Physics.isConstraintEnabled = function(tag) {};

/**
 * @param {number} vehicleTag
 * @param {number} wheelIndex
 * @param {number} motorTorque
 * @param {number} brakeTorque
 */
Physics.setWheelMotor = function(vehicleTag, wheelIndex, motorTorque, brakeTorque) {};

/**
 * @param {number} tag
 * @param {PhysicsConstraintMotorOptions} config
 */
Physics.setConstraintMotor = function(tag, config) {};

/**
 * @param {number} tag
 * @param {number} impulse
 */
Physics.setConstraintBreakingImpulse = function(tag, impulse) {};

/**
 * @param {number} tag
 * @returns {number}
 */
Physics.getConstraintBreakingImpulse = function(tag) {};

/**
 * @returns {Array<number>}
 */
Physics.getBrokenConstraints = function() {};

