// Plats.wren
// Ports the moving platform and train logic from plats.qc to ensure Wren
// gameplay parity with the QuakeC implementation.

import "./Engine" for Engine
import "./Globals" for SolidTypes, MoveTypes, Channels, Attenuations, MoverStates, DamageValues
import "./Subs" for SubsModule
import "./Combat" for CombatModule

var _PLAT_LOW_TRIGGER = 1

class PlatsModule {
  static _vectorAdd(a, b) {
    return [a[0] + b[0], a[1] + b[1], a[2] + b[2]]
  }

  static _vectorSub(a, b) {
    return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
  }

  static _vectorScale(v, scalar) {
    return [v[0] * scalar, v[1] * scalar, v[2] * scalar]
  }

  static _copyVector(v) {
    return [v[0], v[1], v[2]]
  }

  static _sizeZ(entity) {
    var size = entity.get("size", null)
    if (size != null) {
      return size[2]
    }
    var mins = entity.get("mins", [0, 0, 0])
    var maxs = entity.get("maxs", [0, 0, 0])
    return maxs[2] - mins[2]
  }

  static _findTarget(globals, owner, field) {
    var name = owner.get(field, null)
    if (name == null || name == "") return null
    var matches = Engine.findByField(globals.world, "targetname", name)
    if (matches == null || matches.count == 0) return null
    return matches[0]
  }

  static platSpawnInsideTrigger(globals, plat) {
    if (plat == null) return

    var trigger = Engine.spawnEntity()
    trigger.set("classname", "plat_trigger")
    trigger.set("touch", "PlatsModule.platCenterTouch")
    Engine.setTriggerTouch(trigger, "PlatsModule.platCenterTouch")
    trigger.set("movetype", MoveTypes.NONE)
    trigger.set("solid", SolidTypes.TRIGGER)
    trigger.set("enemy", plat)

    var mins = plat.get("mins", [0, 0, 0])
    var maxs = plat.get("maxs", [0, 0, 0])
    var pos1 = plat.get("pos1", plat.get("origin", [0, 0, 0]))
    var pos2 = plat.get("pos2", plat.get("origin", [0, 0, 0]))

    var tmin = [mins[0] + 25, mins[1] + 25, mins[2]]
    var tmax = [maxs[0] - 25, maxs[1] - 25, maxs[2] + 8]
    var height = pos1[2] - pos2[2] + 8
    tmin[2] = tmax[2] - height

    if (Engine.bitAnd(plat.get("spawnflags", 0), _PLAT_LOW_TRIGGER) != 0) {
      tmax[2] = tmin[2] + 8
    }

    var size = PlatsModule._vectorSub(maxs, mins)
    if (size[0] <= 50) {
      var centerX = (mins[0] + maxs[0]) * 0.5
      tmin[0] = centerX
      tmax[0] = centerX + 1
    }
    if (size[1] <= 50) {
      var centerY = (mins[1] + maxs[1]) * 0.5
      tmin[1] = centerY
      tmax[1] = centerY + 1
    }

    Engine.setOrigin(trigger, plat.get("origin", [0, 0, 0]))
    Engine.setSize(trigger, tmin, tmax)
  }

  static platHitTop(globals, plat) {
    if (plat == null) return

    var noise = plat.get("noise1", null)
    if (noise != null && noise != "") {
      Engine.playSound(plat, Channels.VOICE, noise, 1, Attenuations.NORMAL)
    }

    plat.set("state", MoverStates.TOP)
    var ltime = plat.get("ltime", Engine.time())
    var delay = 3.0
    plat.set("think", "PlatsModule.platGoDown")
    plat.set("nextthink", ltime + delay)
    Engine.scheduleThink(plat, "PlatsModule.platGoDown", delay)
  }

  static platHitBottom(globals, plat) {
    if (plat == null) return

    var noise = plat.get("noise1", null)
    if (noise != null && noise != "") {
      Engine.playSound(plat, Channels.VOICE, noise, 1, Attenuations.NORMAL)
    }

    plat.set("state", MoverStates.BOTTOM)
  }

  static platGoDown(globals, plat) {
    if (plat == null) return

    var noise = plat.get("noise", null)
    if (noise != null && noise != "") {
      Engine.playSound(plat, Channels.VOICE, noise, 1, Attenuations.NORMAL)
    }

    plat.set("state", MoverStates.DOWN)
    var destination = plat.get("pos2", plat.get("origin", [0, 0, 0]))
    var speed = plat.get("speed", 150)
    SubsModule.calcMove(globals, plat, destination, speed, "PlatsModule.platHitBottom")
  }

  static platGoUp(globals, plat) {
    if (plat == null) return

    var noise = plat.get("noise", null)
    if (noise != null && noise != "") {
      Engine.playSound(plat, Channels.VOICE, noise, 1, Attenuations.NORMAL)
    }

    plat.set("state", MoverStates.UP)
    var destination = plat.get("pos1", plat.get("origin", [0, 0, 0]))
    var speed = plat.get("speed", 150)
    SubsModule.calcMove(globals, plat, destination, speed, "PlatsModule.platHitTop")
  }

  static platCenterTouch(globals, trigger, other) {
    if (trigger == null) return
    if (other == null) return
    if (other.get("classname", "") != "player") return
    if (other.get("health", 0) <= 0) return

    var plat = trigger.get("enemy", null)
    if (plat == null) return

    var state = plat.get("state", MoverStates.BOTTOM)
    if (state == MoverStates.BOTTOM) {
      PlatsModule.platGoUp(globals, plat)
    } else if (state == MoverStates.TOP) {
      var ltime = plat.get("ltime", Engine.time())
      var targetTime = ltime + 1.0
      var delay = targetTime - Engine.time()
      if (delay < 0) delay = 0
      plat.set("nextthink", targetTime)
      plat.set("think", "PlatsModule.platGoDown")
      Engine.scheduleThink(plat, "PlatsModule.platGoDown", delay)
    }
  }

  static platOutsideTouch(globals, plat, other) {
    if (plat == null) return
    if (other == null) return
    if (other.get("classname", "") != "player") return
    if (other.get("health", 0) <= 0) return

    var state = plat.get("state", MoverStates.BOTTOM)
    if (state == MoverStates.TOP) {
      PlatsModule.platGoDown(globals, plat)
    }
  }

  static platTriggerUse(globals, plat, activator) {
    if (plat == null) return
    var think = plat.get("think", null)
    if (think != null && think != "") return
    PlatsModule.platGoDown(globals, plat)
  }

  static platCrush(globals, plat, other) {
    if (plat == null) return
    if (other != null) {
      CombatModule.tDamage(globals, other, plat, plat, 1)
    }

    var state = plat.get("state", MoverStates.BOTTOM)
    if (state == MoverStates.UP) {
      PlatsModule.platGoDown(globals, plat)
    } else if (state == MoverStates.DOWN) {
      PlatsModule.platGoUp(globals, plat)
    } else {
      Engine.objError("plat_crush: bad state")
    }
  }

  static platUse(globals, plat, activator) {
    if (plat == null) return
    plat.set("use", "SubsModule.subNull")
    if (plat.get("state", MoverStates.TOP) != MoverStates.TOP) {
      Engine.objError("plat_use: not in up state")
      return
    }
    PlatsModule.platGoDown(globals, plat)
  }

  static funcPlat(globals, plat) {
    if (plat == null) return

    if (plat.get("t_length", 0) == 0) {
      plat.set("t_length", 80)
    }
    if (plat.get("t_width", 0) == 0) {
      plat.set("t_width", 10)
    }

    var sounds = plat.get("sounds", 0)
    if (sounds == 0) {
      sounds = 2
    }

    if (sounds == 1) {
      Engine.precacheSound("plats/plat1.wav")
      Engine.precacheSound("plats/plat2.wav")
      plat.set("noise", "plats/plat1.wav")
      plat.set("noise1", "plats/plat2.wav")
    } else if (sounds == 2) {
      Engine.precacheSound("plats/medplat1.wav")
      Engine.precacheSound("plats/medplat2.wav")
      plat.set("noise", "plats/medplat1.wav")
      plat.set("noise1", "plats/medplat2.wav")
    }

    var angles = plat.get("angles", [0, 0, 0])
    plat.set("mangle", PlatsModule._copyVector(angles))
    plat.set("angles", [0, 0, 0])

    plat.set("classname", "func_plat")
    plat.set("solid", SolidTypes.BSP)
    plat.set("movetype", MoveTypes.PUSH)
    Engine.setOrigin(plat, plat.get("origin", [0, 0, 0]))
    Engine.setModel(plat, plat.get("model", ""))
    Engine.setSize(plat, plat.get("mins", [0, 0, 0]), plat.get("maxs", [0, 0, 0]))

    plat.set("touch", "PlatsModule.platOutsideTouch")
    plat.set("blocked", "PlatsModule.platCrush")

    if (plat.get("speed", 0) == 0) {
      plat.set("speed", 150)
    }

    var origin = plat.get("origin", [0, 0, 0])
    var pos1 = PlatsModule._copyVector(origin)
    var pos2 = PlatsModule._copyVector(origin)
    var height = plat.get("height", 0)
    if (height != 0) {
      pos2[2] = origin[2] - height
    } else {
      pos2[2] = origin[2] - PlatsModule._sizeZ(plat) + 8
    }

    plat.set("pos1", pos1)
    plat.set("pos2", pos2)

    plat.set("use", "PlatsModule.platTriggerUse")
    PlatsModule.platSpawnInsideTrigger(globals, plat)

    if (plat.get("targetname", "") != "") {
      plat.set("state", MoverStates.TOP)
      plat.set("use", "PlatsModule.platUse")
    } else {
      Engine.setOrigin(plat, pos2)
      plat.set("state", MoverStates.BOTTOM)
    }
  }

  static trainBlocked(globals, train, other) {
    if (train == null) return
    var attackFinished = train.get("attack_finished", 0.0)
    var now = Engine.time()
    if (now < attackFinished) return

    train.set("attack_finished", now + 0.5)
    if (other != null) {
      CombatModule.tDamage(globals, other, train, train, train.get("dmg", 2))
    }
  }

  static trainUse(globals, train, activator) {
    if (train == null) return
    if (train.get("think", null) != "PlatsModule.funcTrainFind") return
    PlatsModule.trainNext(globals, train)
  }

  static trainWait(globals, train) {
    if (train == null) return
    var wait = train.get("wait", 0.0)
    var ltime = train.get("ltime", Engine.time())
    if (wait != 0) {
      train.set("nextthink", ltime + wait)
      var noise = train.get("noise", null)
      if (noise != null && noise != "") {
        Engine.playSound(train, Channels.VOICE, noise, 1, Attenuations.NORMAL)
      }
      Engine.scheduleThink(train, "PlatsModule.trainNext", wait)
    } else {
      var delay = 0.1
      train.set("nextthink", ltime + delay)
      Engine.scheduleThink(train, "PlatsModule.trainNext", delay)
    }
    train.set("think", "PlatsModule.trainNext")
  }

  static trainNext(globals, train) {
    if (train == null) return

    var target = PlatsModule._findTarget(globals, train, "target")
    if (target == null) {
      Engine.objError("train_next: no next target")
      return
    }

    train.set("target", target.get("target", null))

    var wait = target.get("wait", 0.0)
    train.set("wait", wait)

    var noise = train.get("noise1", null)
    if (noise != null && noise != "") {
      Engine.playSound(train, Channels.VOICE, noise, 1, Attenuations.NORMAL)
    }

    var mins = train.get("mins", [0, 0, 0])
    var destination = PlatsModule._vectorSub(target.get("origin", [0, 0, 0]), mins)
    var speed = train.get("speed", 100)
    SubsModule.calcMove(globals, train, destination, speed, "PlatsModule.trainWait")
  }

  static funcTrainFind(globals, train) {
    if (train == null) return

    var target = PlatsModule._findTarget(globals, train, "target")
    if (target == null) {
      Engine.objError("func_train: no target")
      return
    }

    train.set("target", target.get("target", null))
    var mins = train.get("mins", [0, 0, 0])
    var origin = PlatsModule._vectorSub(target.get("origin", [0, 0, 0]), mins)
    Engine.setOrigin(train, origin)

    if (train.get("targetname", "") == "") {
      var delay = 0.1
      train.set("nextthink", train.get("ltime", Engine.time()) + delay)
      train.set("think", "PlatsModule.trainNext")
      Engine.scheduleThink(train, "PlatsModule.trainNext", delay)
    }
  }

  static funcTrain(globals, train) {
    if (train == null) return

    if (train.get("speed", 0) == 0) {
      train.set("speed", 100)
    }
    if (train.get("target", "") == "") {
      Engine.objError("func_train without a target")
      return
    }
    if (train.get("dmg", 0) == 0) {
      train.set("dmg", 2)
    }

    var sounds = train.get("sounds", 0)
    if (sounds == 0) {
      Engine.precacheSound("misc/null.wav")
      train.set("noise", "misc/null.wav")
      train.set("noise1", "misc/null.wav")
    } else if (sounds == 1) {
      Engine.precacheSound("plats/train2.wav")
      Engine.precacheSound("plats/train1.wav")
      train.set("noise", "plats/train2.wav")
      train.set("noise1", "plats/train1.wav")
    }

    train.set("cnt", 1)
    train.set("solid", SolidTypes.BSP)
    train.set("movetype", MoveTypes.PUSH)
    train.set("blocked", "PlatsModule.trainBlocked")
    train.set("use", "PlatsModule.trainUse")
    train.set("classname", "train")

    Engine.setModel(train, train.get("model", ""))
    Engine.setSize(train, train.get("mins", [0, 0, 0]), train.get("maxs", [0, 0, 0]))
    Engine.setOrigin(train, train.get("origin", [0, 0, 0]))

    var delay = 0.1
    train.set("nextthink", train.get("ltime", Engine.time()) + delay)
    train.set("think", "PlatsModule.funcTrainFind")
    Engine.scheduleThink(train, "PlatsModule.funcTrainFind", delay)
  }

  static miscTeleporttrain(globals, train) {
    if (train == null) return

    if (train.get("speed", 0) == 0) {
      train.set("speed", 100)
    }
    if (train.get("target", "") == "") {
      Engine.objError("func_train without a target")
      return
    }

    train.set("cnt", 1)
    train.set("solid", SolidTypes.NOT)
    train.set("movetype", MoveTypes.PUSH)
    train.set("blocked", "PlatsModule.trainBlocked")
    train.set("use", "PlatsModule.trainUse")
    train.set("avelocity", [100, 200, 300])

    Engine.precacheSound("misc/null.wav")
    train.set("noise", "misc/null.wav")
    train.set("noise1", "misc/null.wav")

    Engine.precacheModel("progs/teleport.mdl")
    Engine.setModel(train, "progs/teleport.mdl")
    Engine.setSize(train, train.get("mins", [0, 0, 0]), train.get("maxs", [0, 0, 0]))
    Engine.setOrigin(train, train.get("origin", [0, 0, 0]))

    var delay = 0.1
    train.set("nextthink", train.get("ltime", Engine.time()) + delay)
    train.set("think", "PlatsModule.funcTrainFind")
    Engine.scheduleThink(train, "PlatsModule.funcTrainFind", delay)
  }

  // ------------------------------------------------------------------------
  // Compatibility wrappers -------------------------------------------------

  static plat_spawn_inside_trigger(globals, plat) {
    PlatsModule.platSpawnInsideTrigger(globals, plat)
  }
  static plat_hit_top(globals, plat) { PlatsModule.platHitTop(globals, plat) }
  static plat_hit_bottom(globals, plat) { PlatsModule.platHitBottom(globals, plat) }
  static plat_go_down(globals, plat) { PlatsModule.platGoDown(globals, plat) }
  static plat_go_up(globals, plat) { PlatsModule.platGoUp(globals, plat) }
  static plat_center_touch(globals, trigger, other) {
    PlatsModule.platCenterTouch(globals, trigger, other)
  }
  static plat_outside_touch(globals, plat, other) {
    PlatsModule.platOutsideTouch(globals, plat, other)
  }
  static plat_trigger_use(globals, plat, activator) {
    PlatsModule.platTriggerUse(globals, plat, activator)
  }
  static plat_crush(globals, plat, other) { PlatsModule.platCrush(globals, plat, other) }
  static plat_use(globals, plat, activator) {
    PlatsModule.platUse(globals, plat, activator)
  }
  static func_plat(globals, plat) { PlatsModule.funcPlat(globals, plat) }
  static train_blocked(globals, train, other) {
    PlatsModule.trainBlocked(globals, train, other)
  }
  static train_use(globals, train, activator) {
    PlatsModule.trainUse(globals, train, activator)
  }
  static train_wait(globals, train) { PlatsModule.trainWait(globals, train) }
  static train_next(globals, train) { PlatsModule.trainNext(globals, train) }
  static func_train_find(globals, train) {
    PlatsModule.funcTrainFind(globals, train)
  }
  static func_train(globals, train) { PlatsModule.funcTrain(globals, train) }
  static misc_teleporttrain(globals, train) {
    PlatsModule.miscTeleporttrain(globals, train)
  }
}
