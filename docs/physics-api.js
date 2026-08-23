// ── Classes & Interfaces ─────────────────────────────────────────────────────

/**
 * =============================================================================
 * Physics — Jolt Physics 3D Simulation & Collision System
 * =============================================================================
 *
 * Complete 3D physics engine binding backed by Jolt Physics.
 * Includes rigid body dynamics, collision queries (raycast, shape cast, overlap),
 * character virtual controllers, vehicles, ragdolls, soft bodies, constraints,
 * and sandbox worlds.
 * @example
 * Physics.setGravity(0, -9.81, 0);
 *   const body = Physics.createBody({
 *     shape: { type: 'box', halfExtents: [1, 1, 1] },
 *     position: [0, 10, 0],
 *     motionType: 'dynamic'
 *   });
 *   Physics.addImpulse(body, 0, 5, 0);
 */
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
  setLinearVelocity(x, y, z) {}

  /**
   * @returns {Object}
   */
  getPosition() {}

  /**
   * @returns {Object}
   */
  getLinearVelocity() {}

  /**
   * @param {number} dt
   */
  update(dt) {}

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
   * @returns {Object}
   */
  getTransform() {}

}

class PhysicsRagdoll {

  /**
   * @param {Object} pose
   * @param {number} dt
   */
  driveToPose(pose, dt) {}

  /**
   * @returns {Object}
   */
  getPose() {}

}

class PhysicsSoftBody {

  /**
   * @returns {Object}
   */
  getBounds() {}

}

// ── Namespaces ───────────────────────────────────────────────────────────────

/**
 * @param {Object} [opts]
 * @returns {PhysicsWorldHandle}
 */
Physics.createWorldHandle = function(opts) {};

/**
 * @param {Object} [opts]
 */
Physics.createWorld = function(opts) {};

/**
 * @param {number} x
 * @param {number} y
 * @param {number} z
 */
Physics.setGravity = function(x, y, z) {};

/**
 * @returns {Array<number>}
 */
Physics.getGravity = function() {};

/**
 * @param {Object} config
 */
Physics.setLayers = function(config) {};

/**
 * @param {Object} config
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
 * @returns {Object}
 */
Physics.getTransform = function(tag) {};

/**
 * @param {number} tag
 * @returns {Object}
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
 * @param {*} data
 */
Physics.setUserData = function(tag, data) {};

/**
 * @param {number} tag
 * @returns {*}
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
 * @param {(string|number)} type
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

/**
 * @param {number} ox
 * @param {number} oy
 * @param {number} oz
 * @param {number} dx
 * @param {number} dy
 * @param {number} dz
 * @param {number} [maxDist]
 * @param {number} [mask]
 * @returns {Object|null}
 */
Physics.raycast = function(ox, oy, oz, dx, dy, dz, maxDist, mask) {};

/**
 * @param {number} ox
 * @param {number} oy
 * @param {number} oz
 * @param {number} dx
 * @param {number} dy
 * @param {number} dz
 * @param {number} [maxDist]
 * @param {number} [mask]
 * @returns {Object|null}
 */
Physics.raycastClosest = function(ox, oy, oz, dx, dy, dz, maxDist, mask) {};

/**
 * @param {Object} config
 * @returns {Array<Object>}
 */
Physics.castShape = function(config) {};

/**
 * @param {Object} config
 * @returns {Object|null}
 */
Physics.castShapeClosest = function(config) {};

/**
 * @param {Object} config
 * @returns {Array<number>}
 */
Physics.overlapShape = function(config) {};

/**
 * @param {number} x
 * @param {number} y
 * @param {number} z
 * @param {number} [mask]
 * @returns {Array<number>}
 */
Physics.overlapPoint = function(x, y, z, mask) {};

/**
 * @returns {Array<Object>}
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
 * @returns {Object|null}
 */
Physics.getBodyProperties = function(tag) {};

/**
 * @param {number} tag
 * @param {Object} config
 */
Physics.setAreaOverride = function(tag, config) {};

/**
 * @param {number} dt
 */
Physics.setTimeStep = function(dt) {};

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
 * @param {Object} config
 * @returns {PhysicsCharacter}
 */
Physics.createCharacter = function(config) {};

/**
 * @param {Object} config
 * @returns {PhysicsVehicle}
 */
Physics.createVehicle = function(config) {};

/**
 * @param {Object} config
 * @returns {PhysicsRagdoll}
 */
Physics.createRagdoll = function(config) {};

/**
 * @param {Object} config
 * @returns {PhysicsSoftBody}
 */
Physics.createSoftBody = function(config) {};

/**
 * @param {Object} config
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
 * @param {number} vehicleTag
 * @param {number} wheelIndex
 * @param {number} motorTorque
 * @param {number} brakeTorque
 */
Physics.setWheelMotor = function(vehicleTag, wheelIndex, motorTorque, brakeTorque) {};

/**
 * @param {number} tag
 * @param {Object} config
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

