// Subs.wren
// Ports utility routines from subs.qc that are shared across many gameplay
// systems. The implementation mirrors the original QuakeC behavior so that
// other modules can rely on identical side effects when manipulating movers,
// triggers, and attack timing state.

import "./Engine" for Engine
import "./Globals" for SolidTypes, MoveTypes, Channels, Attenuations

class SubsModule {
  static _vectorAdd(a, b) {
    return [a[0] + b[0], a[1] + b[1], a[2] + b[2]]
  }

  static _vectorSub(a, b) {
    return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
  }

  static _vectorScale(v, scalar) {
    return [v[0] * scalar, v[1] * scalar, v[2] * scalar]
  }

  static _vectorLength(v) {
    return (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]).sqrt
  }

  static _vectorEquals(a, b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2]
  }

  static _vectorIsZero(v) {
    return v[0] == 0 && v[1] == 0 && v[2] == 0
  }

  static _callEntityFunction(globals, entity, functionName, other, args) {
    if (functionName == null || functionName == "" || functionName == "SUB_Null") {
      return
    }

    var previousSelf = globals.self
    var previousOther = globals.other
    globals.self = entity
    globals.other = other
    Engine.callEntityFunction(entity, functionName, args)
    globals.self = previousSelf
    globals.other = previousOther
  }

  static _useTargetsImmediate(globals, entity, activator) {
    if (entity == null) return

    var delay = entity.get("delay", 0.0)
    if (delay != 0) {
      var delayed = Engine.spawnEntity()
      delayed.set("classname", "DelayedUse")
      delayed.set("enemy", activator)
      delayed.set("message", entity.get("message", null))
      delayed.set("killtarget", entity.get("killtarget", null))
      delayed.set("target", entity.get("target", null))
      delayed.set("noise", entity.get("noise", null))
      delayed.set("delay", 0.0)
      Engine.scheduleThink(delayed, "SubsModule.delayThink", delay)
      return
    }

    var message = entity.get("message", null)
    if (activator != null && activator.get("classname", "") == "player") {
      if (message != null && message != "") {
        Engine.centerPrint(activator, message)
        var noise = entity.get("noise", null)
        if (noise == null || noise == "") {
          Engine.playSound(activator, Channels.VOICE, "misc/talk.wav", 1.0, Attenuations.NORMAL)
        } else {
          Engine.playSound(activator, Channels.VOICE, noise, 1.0, Attenuations.NORMAL)
        }
      }
    }

    var killTarget = entity.get("killtarget", null)
    if (killTarget != null && killTarget != "") {
      var victims = Engine.findByField(globals.world, "targetname", killTarget)
      if (victims != null) {
        for (victim in victims) {
          Engine.removeEntity(victim)
        }
      }
    }

    var targetName = entity.get("target", null)
    if (targetName == null || targetName == "") {
      return
    }

    var targets = Engine.findByField(globals.world, "targetname", targetName)
    if (targets == null) return

    var previousActivator = globals.activator
    globals.activator = activator

    for (target in targets) {
      var useFunction = target.get("use", null)
      SubsModule._callEntityFunction(globals, target, useFunction, entity, [])
      globals.activator = activator
    }

    globals.activator = previousActivator
  }

  static useTargets(globals, entity, activator) {
    var previousActivator = globals.activator
    if (activator == null) {
      activator = previousActivator
    }
    globals.activator = activator
    SubsModule._useTargetsImmediate(globals, entity, activator)
    globals.activator = previousActivator
  }

  static delayThink(globals, delayed) {
    var activator = delayed.get("enemy", null)
    var previousActivator = globals.activator
    globals.activator = activator
    SubsModule._useTargetsImmediate(globals, delayed, activator)
    globals.activator = previousActivator
    Engine.removeEntity(delayed)
  }

  static setMoveDir(globals, entity) {
    var angles = entity.get("angles", [0, 0, 0])
    var movedir = [0, 0, 0]

    if (SubsModule._vectorEquals(angles, [0, -1, 0])) {
      movedir = [0, 0, 1]
    } else if (SubsModule._vectorEquals(angles, [0, -2, 0])) {
      movedir = [0, 0, -1]
    } else {
      var vectors = Engine.makeVectors(angles)
      if (vectors != null && vectors.containsKey("forward")) {
        movedir = vectors["forward"]
      }
    }

    entity.set("movedir", movedir)
    entity.set("angles", [0, 0, 0])
  }

  static initTrigger(globals, entity) {
    var angles = entity.get("angles", [0, 0, 0])
    if (!SubsModule._vectorIsZero(angles)) {
      SubsModule.setMoveDir(globals, entity)
    }

    entity.set("solid", SolidTypes.TRIGGER)
    Engine.setModel(entity, entity.get("model", ""))
    entity.set("movetype", MoveTypes.NONE)
    entity.set("modelindex", 0)
    entity.set("model", "")
  }

  static calcMove(globals, entity, destination, speed, callback) {
    if (entity == null) return
    if (speed == null || speed == 0) {
      Engine.objError("No speed is defined!")
      return
    }

    entity.set("think1", callback)
    entity.set("finaldest", destination)
    entity.set("think", "SubsModule.calcMoveDone")

    var origin = entity.get("origin", [0, 0, 0])
    if (SubsModule._vectorEquals(destination, origin)) {
      entity.set("velocity", [0, 0, 0])
      var delay = 0.1
      var ltime = entity.get("ltime", Engine.time())
      entity.set("nextthink", ltime + delay)
      Engine.scheduleThink(entity, "SubsModule.calcMoveDone", delay)
      return
    }

    var delta = SubsModule._vectorSub(destination, origin)
    var length = SubsModule._vectorLength(delta)
    var travelTime = length / speed

    if (travelTime < 0.1) {
      entity.set("velocity", [0, 0, 0])
      var delay = 0.1
      var ltime = entity.get("ltime", Engine.time())
      entity.set("nextthink", ltime + delay)
      Engine.scheduleThink(entity, "SubsModule.calcMoveDone", delay)
      return
    }

    var velocity = SubsModule._vectorScale(delta, 1 / travelTime)
    entity.set("velocity", velocity)

    var ltime = entity.get("ltime", Engine.time())
    entity.set("nextthink", ltime + travelTime)
    Engine.scheduleThink(entity, "SubsModule.calcMoveDone", travelTime)
  }

  static calcMoveEnt(globals, ent, destination, speed, callback) {
    SubsModule.calcMove(globals, ent, destination, speed, callback)
  }

  static calcMoveDone(globals, entity) {
    var finalDest = entity.get("finaldest", entity.get("origin", [0, 0, 0]))
    Engine.setOrigin(entity, finalDest)
    entity.set("velocity", [0, 0, 0])
    entity.set("nextthink", -1)

    var callback = entity.get("think1", null)
    SubsModule._callEntityFunction(globals, entity, callback, entity.get("enemy", null), [])
  }

  static calcAngleMove(globals, entity, destination, speed, callback) {
    if (entity == null) return
    if (speed == null || speed == 0) {
      Engine.objError("No speed is defined!")
      return
    }

    var angles = entity.get("angles", [0, 0, 0])
    var delta = SubsModule._vectorSub(destination, angles)
    var length = SubsModule._vectorLength(delta)
    var travelTime = length / speed

    entity.set("think1", callback)
    entity.set("finalangle", destination)
    entity.set("think", "SubsModule.calcAngleMoveDone")

    var ltime = entity.get("ltime", Engine.time())

    if (travelTime < 0.1) {
      entity.set("avelocity", [0, 0, 0])
      var delay = 0.1
      entity.set("nextthink", ltime + delay)
      Engine.scheduleThink(entity, "SubsModule.calcAngleMoveDone", delay)
      return
    }

    entity.set("avelocity", SubsModule._vectorScale(delta, 1 / travelTime))
    entity.set("nextthink", ltime + travelTime)
    Engine.scheduleThink(entity, "SubsModule.calcAngleMoveDone", travelTime)
  }

  static calcAngleMoveEnt(globals, ent, destination, speed, callback) {
    SubsModule.calcAngleMove(globals, ent, destination, speed, callback)
  }

  static calcAngleMoveDone(globals, entity) {
    entity.set("angles", entity.get("finalangle", entity.get("angles", [0, 0, 0])))
    entity.set("avelocity", [0, 0, 0])
    entity.set("nextthink", -1)

    var callback = entity.get("think1", null)
    SubsModule._callEntityFunction(globals, entity, callback, entity.get("enemy", null), [])
  }

  static attackFinished(globals, entity, normalTime) {
    if (entity == null) return
    entity.set("cnt", 0)
    entity.set("attack_finished", Engine.time() + normalTime)
  }

  static checkRefire(globals, entity, thinkFunction) {
    if (entity == null) return
    if (globals.skill != 3) return
    if (entity.get("cnt", 0) == 1) return

    var enemy = entity.get("enemy", null)
    if (enemy == null) return
    if (!Engine.isVisible(entity, enemy)) return

    entity.set("cnt", 1)
    entity.set("think", thinkFunction)
  }

  // --------------------------------------------------------------------------
  // Compatibility aliases ----------------------------------------------------
  //
  // Some gameplay scripts still reference the original QuakeC helper names
  // ("SUB_*" variants).  Provide thin wrappers so callers can continue using
  // those identifiers while sharing the core implementations above.

  static subCalcMove(globals, entity, destination, speed, callback) {
    SubsModule.calcMove(globals, entity, destination, speed, callback)
  }

  static subCalcMoveEnt(globals, entity, destination, speed, callback) {
    SubsModule.calcMoveEnt(globals, entity, destination, speed, callback)
  }

  static subCalcMoveDone(globals, entity) {
    SubsModule.calcMoveDone(globals, entity)
  }

  static subCalcAngleMove(globals, entity, destination, speed, callback) {
    SubsModule.calcAngleMove(globals, entity, destination, speed, callback)
  }

  static subCalcAngleMoveEnt(globals, entity, destination, speed, callback) {
    SubsModule.calcAngleMoveEnt(globals, entity, destination, speed, callback)
  }

  static subCalcAngleMoveDone(globals, entity) {
    SubsModule.calcAngleMoveDone(globals, entity)
  }

  static subUseTargets(globals, entity, activator) {
    SubsModule.useTargets(globals, entity, activator)
  }

  static subAttackFinished(globals, entity, normalTime) {
    SubsModule.attackFinished(globals, entity, normalTime)
  }

  static subCheckRefire(globals, entity, thinkFunction) {
    SubsModule.checkRefire(globals, entity, thinkFunction)
  }

  static subRemove(globals, entity) {
    if (entity == null) return
    Engine.removeEntity(entity)
  }

  static subNull(globals, entity) {
    // Intentionally empty; mirrors SUB_Null in QuakeC.
  }

  static SetMovedir(globals, entity) { SubsModule.setMoveDir(globals, entity) }
  static InitTrigger(globals, entity) { SubsModule.initTrigger(globals, entity) }
  static SUB_CalcMove(globals, entity, destination, speed, callback) {
    SubsModule.calcMove(globals, entity, destination, speed, callback)
  }
  static SUB_CalcMoveEnt(globals, entity, destination, speed, callback) {
    SubsModule.calcMoveEnt(globals, entity, destination, speed, callback)
  }
  static SUB_CalcMoveDone(globals, entity) { SubsModule.calcMoveDone(globals, entity) }
  static SUB_CalcAngleMove(globals, entity, destination, speed, callback) {
    SubsModule.calcAngleMove(globals, entity, destination, speed, callback)
  }
  static SUB_CalcAngleMoveEnt(globals, entity, destination, speed, callback) {
    SubsModule.calcAngleMoveEnt(globals, entity, destination, speed, callback)
  }
  static SUB_CalcAngleMoveDone(globals, entity) {
    SubsModule.calcAngleMoveDone(globals, entity)
  }
  static DelayThink(globals, entity) { SubsModule.delayThink(globals, entity) }
  static SUB_UseTargets(globals, entity, activator) {
    SubsModule.useTargets(globals, entity, activator)
  }
  static SUB_AttackFinished(globals, entity, normalTime) {
    SubsModule.attackFinished(globals, entity, normalTime)
  }
  static SUB_CheckRefire(globals, entity, thinkFunction) {
    SubsModule.checkRefire(globals, entity, thinkFunction)
  }
  static SUB_Remove(globals, entity) { SubsModule.subRemove(globals, entity) }
  static SUB_Null(globals, entity) { SubsModule.subNull(globals, entity) }
}
