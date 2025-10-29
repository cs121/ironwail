// Doors.wren
// Ports the func_door and func_door_secret logic from doors.qc so that
// standard and secret doors behave identically to the QuakeC originals when
// running under the Wren gameplay layer.

import "./Engine" for Engine
import "./Globals" for SolidTypes, MoveTypes, DamageValues, Channels, Attenuations
import "./Globals" for MoverStates, Items, DoorSpawnFlags, SecretDoorFlags, WorldTypes
import "./Subs" for SubsModule
import "./Combat" for CombatModule

class DoorsModule {
  static _vectorAdd(a, b) {
    return [a[0] + b[0], a[1] + b[1], a[2] + b[2]]
  }

  static _vectorSub(a, b) {
    return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
  }

  static _vectorScale(v, scalar) {
    return [v[0] * scalar, v[1] * scalar, v[2] * scalar]
  }

  static _vectorMultiply(a, b) {
    return [a[0] * b[0], a[1] * b[1], a[2] * b[2]]
  }

  static _vectorDot(a, b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
  }

  static _vectorAbs(v) {
    return [v[0].abs, v[1].abs, v[2].abs]
  }

  static _vectorMin(a, b) {
    return [a[0] < b[0] ? a[0] : b[0], a[1] < b[1] ? a[1] : b[1], a[2] < b[2] ? a[2] : b[2]]
  }

  static _vectorMax(a, b) {
    return [a[0] > b[0] ? a[0] : b[0], a[1] > b[1] ? a[1] : b[1], a[2] > b[2] ? a[2] : b[2]]
  }

  static _vectorEquals(a, b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2]
  }

  static _vectorSubScalar(v, scalar) {
    return [v[0] - scalar, v[1] - scalar, v[2] - scalar]
  }

  static _forEachDoorInChain(start, callback) {
    if (start == null || callback == null) return

    var current = start
    var safety = 0
    while (current != null && safety < 128) {
      callback(current)
      current = current.get("enemy", null)
      if (current == start) break
      safety = safety + 1
    }
  }

  static doorBlocked(globals, door, other) {
    if (door == null || other == null) return

    var damage = door.get("dmg", 2)
    CombatModule.tDamage(globals, other, door, door, damage)

    if (door.get("wait", 0.0) < 0) {
      return
    }

    var state = door.get("state", MoverStates.BOTTOM)
    if (state == MoverStates.DOWN) {
      DoorsModule.doorGoUp(globals, door, globals.activator)
    } else {
      DoorsModule.doorGoDown(globals, door)
    }
  }

  static doorHitTop(globals, door) {
    if (door == null) return

    var noise = door.get("noise1", null)
    if (noise != null && noise != "") {
      Engine.playSound(door, Channels.VOICE, noise, 1, Attenuations.NORMAL)
    }

    door.set("state", MoverStates.TOP)

    var spawnflags = door.get("spawnflags", 0)
    if (Engine.bitAnd(spawnflags, DoorSpawnFlags.TOGGLE) != 0) {
      return
    }

    var wait = door.get("wait", 3.0)
    var ltime = door.get("ltime", Engine.time())
    var delay = wait
    door.set("think", "DoorsModule.doorGoDown")
    door.set("nextthink", ltime + delay)
    Engine.scheduleThink(door, "DoorsModule.doorGoDown", delay)
  }

  static doorHitBottom(globals, door) {
    if (door == null) return

    var noise = door.get("noise1", null)
    if (noise != null && noise != "") {
      Engine.playSound(door, Channels.VOICE, noise, 1, Attenuations.NORMAL)
    }

    door.set("state", MoverStates.BOTTOM)
  }

  static doorGoDown(globals, door) {
    if (door == null) return

    var noise = door.get("noise2", null)
    if (noise != null && noise != "") {
      Engine.playSound(door, Channels.VOICE, noise, 1, Attenuations.NORMAL)
    }

    if (door.get("max_health", 0) != 0) {
      door.set("takedamage", DamageValues.YES)
      door.set("health", door.get("max_health", door.get("health", 0)))
    }

    door.set("state", MoverStates.DOWN)
    SubsModule.calcMove(globals, door, door.get("pos1", door.get("origin", [0, 0, 0])), door.get("speed", 100), "DoorsModule.doorHitBottom")
  }

  static doorGoUp(globals, door, activator) {
    if (door == null) return

    var state = door.get("state", MoverStates.BOTTOM)
    if (state == MoverStates.UP) {
      return
    }

    if (state == MoverStates.TOP) {
      var wait = door.get("wait", 3.0)
      var ltime = door.get("ltime", Engine.time())
      door.set("think", "DoorsModule.doorGoDown")
      door.set("nextthink", ltime + wait)
      Engine.scheduleThink(door, "DoorsModule.doorGoDown", wait)
      return
    }

    var noise = door.get("noise2", null)
    if (noise != null && noise != "") {
      Engine.playSound(door, Channels.VOICE, noise, 1, Attenuations.NORMAL)
    }

    door.set("state", MoverStates.UP)
    SubsModule.calcMove(globals, door, door.get("pos2", door.get("origin", [0, 0, 0])), door.get("speed", 100), "DoorsModule.doorHitTop")

    var previousActivator = globals.activator
    if (activator != null) {
      globals.activator = activator
    }
    SubsModule.useTargets(globals, door, activator)
    globals.activator = previousActivator
  }

  static doorFire(globals, door, activator) {
    if (door == null) return

    var owner = door.get("owner", door)
    if (owner != door) {
      Engine.objError("door_fire: door.owner != door")
      return
    }

    if (door.get("items", 0) != 0) {
      var unlockSound = door.get("noise4", null)
      if (unlockSound != null && unlockSound != "") {
        Engine.playSound(door, Channels.ITEM, unlockSound, 1, Attenuations.NORMAL)
      }
    }

    door.set("message", null)

    var spawnflags = door.get("spawnflags", 0)
    if (Engine.bitAnd(spawnflags, DoorSpawnFlags.TOGGLE) != 0) {
      var state = door.get("state", MoverStates.BOTTOM)
      if (state == MoverStates.UP || state == MoverStates.TOP) {
        DoorsModule._forEachDoorInChain(door, (d) { DoorsModule.doorGoDown(globals, d) })
        return
      }
    }

    DoorsModule._forEachDoorInChain(door, (d) {
      DoorsModule.doorGoUp(globals, d, activator)
    })
  }

  static doorUse(globals, door) {
    if (door == null) return

    door.set("message", null)

    var master = door.get("owner", door)
    if (master == null) master = door

    master.set("message", null)

    var firstEnemy = master.get("enemy", null)
    if (firstEnemy != null) {
      firstEnemy.set("message", null)
    }

    DoorsModule.doorFire(globals, master, globals.activator)
  }

  static doorTriggerTouch(globals, trigger, other) {
    if (trigger == null || other == null) return
    if (other.get("health", 0) <= 0) return

    var nextAllowed = trigger.get("attack_finished", 0.0)
    if (Engine.time() < nextAllowed) {
      return
    }

    trigger.set("attack_finished", Engine.time() + 1)

    var owner = trigger.get("owner", null)
    if (owner == null) return

    var previousActivator = globals.activator
    globals.activator = other
    DoorsModule.doorUse(globals, owner)
    globals.activator = previousActivator
  }

  static doorKilled(globals, door) {
    if (door == null) return

    var owner = door.get("owner", door)
    if (owner == null) return

    owner.set("health", owner.get("max_health", owner.get("health", 0)))
    owner.set("takedamage", DamageValues.NO)

    var previousActivator = globals.activator
    globals.activator = globals.damageAttacker
    DoorsModule.doorUse(globals, owner)
    globals.activator = previousActivator
  }

  static _worldMessage(globals, keyType) {
    var worldType = globals.world.get("worldtype", WorldTypes.MEDIEVAL)
    if (keyType == Items.KEY1) {
      if (worldType == WorldTypes.BASE) return "$qc_need_silver_keycard"
      if (worldType == WorldTypes.METAL) return "$qc_need_silver_runekey"
      return "$qc_need_silver_key"
    }

    if (worldType == WorldTypes.BASE) return "$qc_need_gold_keycard"
    if (worldType == WorldTypes.METAL) return "$qc_need_gold_runekey"
    return "$qc_need_gold_key"
  }

  static doorTouch(globals, door, other) {
    if (door == null || other == null) return
    if (other.get("classname", "") != "player") return

    var owner = door.get("owner", door)
    if (owner == null) owner = door

    var nextAllowed = owner.get("attack_finished", 0.0)
    if (nextAllowed > Engine.time()) {
      return
    }

    owner.set("attack_finished", Engine.time() + 2)

    var message = owner.get("message", null)
    if (message != null && message != "") {
      Engine.centerPrint(other, message)
      Engine.playSound(other, Channels.VOICE, "misc/talk.wav", 1, Attenuations.NORMAL)
    }

    var requiredItems = door.get("items", 0)
    if (requiredItems == 0) {
      return
    }

    var playerItems = other.get("items", 0)
    if (Engine.bitAnd(playerItems, requiredItems) != requiredItems) {
      var keyType = owner.get("items", requiredItems)
      var failMessage = DoorsModule._worldMessage(globals, keyType)
      Engine.centerPrint(other, failMessage)
      var denySound = door.get("noise3", null)
      if (denySound != null && denySound != "") {
        Engine.playSound(door, Channels.VOICE, denySound, 1, Attenuations.NORMAL)
      }
      return
    }

    other.set("items", playerItems - requiredItems)
    door.set("touch", "SubsModule.subNull")

    var partner = door.get("enemy", null)
    if (partner != null) {
      partner.set("touch", "SubsModule.subNull")
    }

    var previousActivator = globals.activator
    globals.activator = other
    DoorsModule.doorUse(globals, owner)
    globals.activator = previousActivator
  }

  static _spawnField(globals, owner, mins, maxs) {
    var trigger = Engine.spawnEntity()
    trigger.set("movetype", MoveTypes.NONE)
    trigger.set("solid", SolidTypes.TRIGGER)
    trigger.set("owner", owner)
    trigger.set("classname", "door_trigger")
    trigger.set("touch", "DoorsModule.doorTriggerTouch")

    var expandedMins = DoorsModule._vectorSub(mins, [60, 60, 8])
    var expandedMaxs = DoorsModule._vectorAdd(maxs, [60, 60, 8])
    Engine.setSize(trigger, expandedMins, expandedMaxs)
    Engine.setTriggerTouch(trigger, "DoorsModule.doorTriggerTouch")

    return trigger
  }

  static spawnField(globals, owner, mins, maxs) {
    return DoorsModule._spawnField(globals, owner, mins, maxs)
  }

  static _entitiesTouching(a, b) {
    if (a == null || b == null) return false

    var aMins = a.get("mins", [0, 0, 0])
    var aMaxs = a.get("maxs", [0, 0, 0])
    var bMins = b.get("mins", [0, 0, 0])
    var bMaxs = b.get("maxs", [0, 0, 0])

    if (aMins[0] > bMaxs[0]) return false
    if (aMins[1] > bMaxs[1]) return false
    if (aMins[2] > bMaxs[2]) return false
    if (aMaxs[0] < bMins[0]) return false
    if (aMaxs[1] < bMins[1]) return false
    if (aMaxs[2] < bMins[2]) return false

    return true
  }

  static entitiesTouching(a, b) { return DoorsModule._entitiesTouching(a, b) }

  static linkDoors(globals, door) {
    if (door == null) return
    if (door.get("enemy", null) != null) {
      door.set("think", null)
      door.set("nextthink", -1)
      return
    }

    var spawnflags = door.get("spawnflags", 0)
    if (Engine.bitAnd(spawnflags, DoorSpawnFlags.DONT_LINK) != 0) {
      door.set("owner", door)
      door.set("enemy", door)
      door.set("think", null)
      door.set("nextthink", -1)
      return
    }

    var className = door.get("classname", "func_door")
    var candidates = Engine.findAll(globals.world, className)
    if (candidates == null) candidates = []

    var master = door
    var combinedMins = door.get("mins", [0, 0, 0])
    var combinedMaxs = door.get("maxs", [0, 0, 0])

    var cluster = []
    var queue = [door]
    var seen = {}

    while (queue.count > 0) {
      var current = queue.removeAt(0)
      if (current == null) continue
      if (seen.containsKey(current)) continue
      seen[current] = true
      cluster.add(current)

      combinedMins = DoorsModule._vectorMin(combinedMins, current.get("mins", combinedMins))
      combinedMaxs = DoorsModule._vectorMax(combinedMaxs, current.get("maxs", combinedMaxs))

      if (current.get("health", 0) != 0) {
        master.set("health", current.get("health", 0))
        master.set("max_health", current.get("health", 0))
      }

      var currentTarget = current.get("targetname", null)
      if (currentTarget != null && currentTarget != "") {
        master.set("targetname", currentTarget)
      }

      var currentMessage = current.get("message", null)
      if (currentMessage != null && currentMessage != "") {
        master.set("message", currentMessage)
      }

      if (current.get("items", 0) != 0) {
        master.set("items", current.get("items", 0))
      }

      for (candidate in candidates) {
        if (candidate == null) continue
        if (candidate == current) continue
        if (seen.containsKey(candidate)) continue
        if (DoorsModule._entitiesTouching(current, candidate)) {
          queue.add(candidate)
        }
      }
    }

    if (cluster.count == 0) {
      door.set("owner", door)
      door.set("enemy", door)
      door.set("think", null)
      door.set("nextthink", -1)
      return
    }

    for (i in 0...cluster.count) {
      var current = cluster[i]
      current.set("owner", master)
      var next = cluster[(i + 1) % cluster.count]
      current.set("enemy", next)
    }

    if (master.get("health", 0) != 0) {
      door.set("think", null)
      door.set("nextthink", -1)
      return
    }
    if (master.get("targetname", null) != null && master.get("targetname", null) != "") {
      door.set("think", null)
      door.set("nextthink", -1)
      return
    }
    if (master.get("items", 0) != 0) {
      door.set("think", null)
      door.set("nextthink", -1)
      return
    }

    var field = DoorsModule._spawnField(globals, master, combinedMins, combinedMaxs)
    master.set("trigger_field", field)
    door.set("think", null)
    door.set("nextthink", -1)
  }

  static funcDoor(globals, door) {
    if (door == null) return

    var worldType = globals.world.get("worldtype", WorldTypes.MEDIEVAL)
    if (worldType == WorldTypes.MEDIEVAL) {
      Engine.precacheSound("doors/medtry.wav")
      Engine.precacheSound("doors/meduse.wav")
      door.set("noise3", "doors/medtry.wav")
      door.set("noise4", "doors/meduse.wav")
    } else if (worldType == WorldTypes.METAL) {
      Engine.precacheSound("doors/runetry.wav")
      Engine.precacheSound("doors/runeuse.wav")
      door.set("noise3", "doors/runetry.wav")
      door.set("noise4", "doors/runeuse.wav")
    } else if (worldType == WorldTypes.BASE) {
      Engine.precacheSound("doors/basetry.wav")
      Engine.precacheSound("doors/baseuse.wav")
      door.set("noise3", "doors/basetry.wav")
      door.set("noise4", "doors/baseuse.wav")
    } else {
      Engine.log("World type not set for door precache")
    }

    var sounds = door.get("sounds", 0)
    if (sounds == 0) {
      Engine.precacheSound("misc/null.wav")
      Engine.precacheSound("misc/null.wav")
      door.set("noise1", "misc/null.wav")
      door.set("noise2", "misc/null.wav")
    } else if (sounds == 1) {
      Engine.precacheSound("doors/drclos4.wav")
      Engine.precacheSound("doors/doormv1.wav")
      door.set("noise1", "doors/drclos4.wav")
      door.set("noise2", "doors/doormv1.wav")
    } else if (sounds == 2) {
      Engine.precacheSound("doors/hydro1.wav")
      Engine.precacheSound("doors/hydro2.wav")
      door.set("noise2", "doors/hydro1.wav")
      door.set("noise1", "doors/hydro2.wav")
    } else if (sounds == 3) {
      Engine.precacheSound("doors/stndr1.wav")
      Engine.precacheSound("doors/stndr2.wav")
      door.set("noise2", "doors/stndr1.wav")
      door.set("noise1", "doors/stndr2.wav")
    } else if (sounds == 4) {
      Engine.precacheSound("doors/ddoor1.wav")
      Engine.precacheSound("doors/ddoor2.wav")
      door.set("noise1", "doors/ddoor2.wav")
      door.set("noise2", "doors/ddoor1.wav")
    }

    SubsModule.setMoveDir(globals, door)

    door.set("max_health", door.get("health", 0))
    door.set("solid", SolidTypes.BSP)
    door.set("movetype", MoveTypes.PUSH)
    Engine.setOrigin(door, door.get("origin", [0, 0, 0]))
    Engine.setModel(door, door.get("model", ""))
    door.set("classname", "func_door")

    door.set("blocked", "DoorsModule.doorBlocked")
    door.set("use", "DoorsModule.doorUse")

    var spawnflags = door.get("spawnflags", 0)
    if (Engine.bitAnd(spawnflags, DoorSpawnFlags.SILVER_KEY) != 0) {
      door.set("items", Items.KEY1)
    }
    if (Engine.bitAnd(spawnflags, DoorSpawnFlags.GOLD_KEY) != 0) {
      door.set("items", Items.KEY2)
    }

    if (door.get("speed", 0) == 0) {
      door.set("speed", 100)
    }

    if (door.get("wait", 0) == 0) {
      door.set("wait", 3)
    }

    if (door.get("lip", 0) == 0) {
      door.set("lip", 8)
    }

    if (door.get("dmg", 0) == 0) {
      door.set("dmg", 2)
    }

    var pos1 = door.get("origin", [0, 0, 0])
    door.set("pos1", pos1)

    var mins = door.get("mins", [0, 0, 0])
    var maxs = door.get("maxs", [0, 0, 0])
    var size = DoorsModule._vectorSub(maxs, mins)
    var movedir = door.get("movedir", [0, 0, 0])
    var travelDistance = DoorsModule._vectorDot(DoorsModule._vectorAbs(movedir), size) - door.get("lip", 8)
    if (travelDistance < 0) travelDistance = 0
    var pos2 = DoorsModule._vectorAdd(pos1, DoorsModule._vectorScale(movedir, travelDistance))
    door.set("pos2", pos2)

    if (Engine.bitAnd(spawnflags, DoorSpawnFlags.START_OPEN) != 0) {
      Engine.setOrigin(door, pos2)
      door.set("origin", pos2)
      door.set("pos2", pos1)
      door.set("pos1", door.get("origin", pos2))
    }

    door.set("state", MoverStates.BOTTOM)

    if (door.get("health", 0) != 0) {
      door.set("takedamage", DamageValues.YES)
      door.set("th_die", "DoorsModule.doorKilled")
    }

    if (door.get("items", 0) != 0) {
      door.set("wait", -1)
    }

    door.set("touch", "DoorsModule.doorTouch")

    door.set("think", "DoorsModule.linkDoors")
    var delay = 0.1
    var ltime = door.get("ltime", Engine.time())
    door.set("nextthink", ltime + delay)
    Engine.scheduleThink(door, "DoorsModule.linkDoors", delay)
  }

  static fdSecretUse(globals, door, activator) {
    if (door == null) return

    door.set("health", 10000)

    if (!DoorsModule._vectorEquals(door.get("origin", [0, 0, 0]), door.get("oldorigin", door.get("origin", [0, 0, 0])))) {
      return
    }

    door.set("message", null)

    var previousActivator = globals.activator
    if (activator != null) {
      globals.activator = activator
    }
    SubsModule.useTargets(globals, door, activator)
    globals.activator = previousActivator

    var spawnflags = door.get("spawnflags", 0)
    if (Engine.bitAnd(spawnflags, SecretDoorFlags.NO_SHOOT) == 0) {
      door.set("th_pain", "SubsModule.subNull")
      door.set("takedamage", DamageValues.NO)
    }

    door.set("velocity", [0, 0, 0])

    var latchSound = door.get("noise1", null)
    if (latchSound != null && latchSound != "") {
      Engine.playSound(door, Channels.VOICE, latchSound, 1, Attenuations.NORMAL)
    }

    var ltime = door.get("ltime", Engine.time())
    door.set("nextthink", ltime + 0.1)

    var temp = Engine.bitAnd(spawnflags, SecretDoorFlags.FIRST_LEFT) != 0 ? -1 : 1
    var vectors = Engine.makeVectors(door.get("mangle", door.get("angles", [0, 0, 0])))
    var forward = vectors != null && vectors.containsKey("forward") ? vectors["forward"] : [1, 0, 0]
    var right = vectors != null && vectors.containsKey("right") ? vectors["right"] : [0, 1, 0]
    var up = vectors != null && vectors.containsKey("up") ? vectors["up"] : [0, 0, 1]

    var tWidth = door.get("t_width", 0.0)
    if (tWidth == 0) {
      if (Engine.bitAnd(spawnflags, SecretDoorFlags.FIRST_DOWN) != 0) {
        tWidth = DoorsModule._vectorDot(up, DoorsModule._vectorSub(door.get("maxs", [0, 0, 0]), door.get("mins", [0, 0, 0]))).abs
      } else {
        tWidth = DoorsModule._vectorDot(right, DoorsModule._vectorSub(door.get("maxs", [0, 0, 0]), door.get("mins", [0, 0, 0]))).abs
      }
      door.set("t_width", tWidth)
    }

    var tLength = door.get("t_length", 0.0)
    if (tLength == 0) {
      var size = DoorsModule._vectorSub(door.get("maxs", [0, 0, 0]), door.get("mins", [0, 0, 0]))
      tLength = DoorsModule._vectorDot(forward, size).abs
      door.set("t_length", tLength)
    }

    var dest1
    if (Engine.bitAnd(spawnflags, SecretDoorFlags.FIRST_DOWN) != 0) {
      dest1 = DoorsModule._vectorSub(door.get("origin", [0, 0, 0]), DoorsModule._vectorScale(up, tWidth))
    } else {
      dest1 = DoorsModule._vectorAdd(door.get("origin", [0, 0, 0]), DoorsModule._vectorScale(right, tWidth * temp))
    }
    door.set("dest1", dest1)

    var dest2 = DoorsModule._vectorAdd(dest1, DoorsModule._vectorScale(forward, tLength))
    door.set("dest2", dest2)

    SubsModule.calcMove(globals, door, dest1, door.get("speed", 50), "DoorsModule.fdSecretMove1")

    var moveSound = door.get("noise2", null)
    if (moveSound != null && moveSound != "") {
      Engine.playSound(door, Channels.VOICE, moveSound, 1, Attenuations.NORMAL)
    }
  }

  static fdSecretPain(globals, door, attacker, damage) {
    DoorsModule.fdSecretUse(globals, door, attacker)
  }

  static fdSecretMove1(globals, door) {
    if (door == null) return
    door.set("think", "DoorsModule.fdSecretMove2")
    var delay = 1.0
    var ltime = door.get("ltime", Engine.time())
    door.set("nextthink", ltime + delay)
    Engine.scheduleThink(door, "DoorsModule.fdSecretMove2", delay)

    var sound = door.get("noise3", null)
    if (sound != null && sound != "") {
      Engine.playSound(door, Channels.VOICE, sound, 1, Attenuations.NORMAL)
    }
  }

  static fdSecretMove2(globals, door) {
    if (door == null) return
    var sound = door.get("noise2", null)
    if (sound != null && sound != "") {
      Engine.playSound(door, Channels.VOICE, sound, 1, Attenuations.NORMAL)
    }
    SubsModule.calcMove(globals, door, door.get("dest2", door.get("origin", [0, 0, 0])), door.get("speed", 50), "DoorsModule.fdSecretMove3")
  }

  static fdSecretMove3(globals, door) {
    if (door == null) return
    var sound = door.get("noise3", null)
    if (sound != null && sound != "") {
      Engine.playSound(door, Channels.VOICE, sound, 1, Attenuations.NORMAL)
    }

    if (Engine.bitAnd(door.get("spawnflags", 0), SecretDoorFlags.OPEN_ONCE) == 0) {
      var wait = door.get("wait", 5)
      var ltime = door.get("ltime", Engine.time())
      door.set("think", "DoorsModule.fdSecretMove4")
      door.set("nextthink", ltime + wait)
      Engine.scheduleThink(door, "DoorsModule.fdSecretMove4", wait)
    }
  }

  static fdSecretMove4(globals, door) {
    if (door == null) return
    var sound = door.get("noise2", null)
    if (sound != null && sound != "") {
      Engine.playSound(door, Channels.VOICE, sound, 1, Attenuations.NORMAL)
    }
    SubsModule.calcMove(globals, door, door.get("dest1", door.get("origin", [0, 0, 0])), door.get("speed", 50), "DoorsModule.fdSecretMove5")
  }

  static fdSecretMove5(globals, door) {
    if (door == null) return
    var delay = 1.0
    var ltime = door.get("ltime", Engine.time())
    door.set("think", "DoorsModule.fdSecretMove6")
    door.set("nextthink", ltime + delay)
    Engine.scheduleThink(door, "DoorsModule.fdSecretMove6", delay)

    var sound = door.get("noise3", null)
    if (sound != null && sound != "") {
      Engine.playSound(door, Channels.VOICE, sound, 1, Attenuations.NORMAL)
    }
  }

  static fdSecretMove6(globals, door) {
    if (door == null) return
    var sound = door.get("noise2", null)
    if (sound != null && sound != "") {
      Engine.playSound(door, Channels.VOICE, sound, 1, Attenuations.NORMAL)
    }
    SubsModule.calcMove(globals, door, door.get("oldorigin", door.get("origin", [0, 0, 0])), door.get("speed", 50), "DoorsModule.fdSecretDone")
  }

  static fdSecretDone(globals, door) {
    if (door == null) return

    var spawnflags = door.get("spawnflags", 0)
    if (door.get("targetname", null) == null || door.get("targetname", null) == "" || Engine.bitAnd(spawnflags, SecretDoorFlags.ALWAYS_SHOOT) != 0) {
      door.set("health", 10000)
      door.set("takedamage", DamageValues.YES)
      door.set("th_pain", "DoorsModule.fdSecretPain")
      door.set("th_die", "DoorsModule.fdSecretUse")
    }

    var sound = door.get("noise3", null)
    if (sound != null && sound != "") {
      Engine.playSound(door, Channels.VOICE, sound, 1, Attenuations.NORMAL)
    }
  }

  static secretBlocked(globals, door, other) {
    if (door == null || other == null) return

    var nextAllowed = door.get("attack_finished", 0.0)
    if (Engine.time() < nextAllowed) {
      return
    }

    door.set("attack_finished", Engine.time() + 0.5)
    CombatModule.tDamage(globals, other, door, door, door.get("dmg", 2))
  }

  static secretTouch(globals, door, other) {
    if (door == null || other == null) return
    if (other.get("classname", "") != "player") return

    var nextAllowed = door.get("attack_finished", 0.0)
    if (nextAllowed > Engine.time()) {
      return
    }

    door.set("attack_finished", Engine.time() + 2)

    var message = door.get("message", null)
    if (message != null && message != "") {
      Engine.centerPrint(other, message)
      Engine.playSound(other, Channels.BODY, "misc/talk.wav", 1, Attenuations.NORMAL)
    }
  }

  static funcDoorSecret(globals, door) {
    if (door == null) return

    if (door.get("sounds", 0) == 0) {
      door.set("sounds", 3)
    }

    var sounds = door.get("sounds", 3)
    if (sounds == 1) {
      Engine.precacheSound("doors/latch2.wav")
      Engine.precacheSound("doors/winch2.wav")
      Engine.precacheSound("doors/drclos4.wav")
      door.set("noise1", "doors/latch2.wav")
      door.set("noise2", "doors/winch2.wav")
      door.set("noise3", "doors/drclos4.wav")
    } else if (sounds == 2) {
      Engine.precacheSound("doors/airdoor1.wav")
      Engine.precacheSound("doors/airdoor2.wav")
      door.set("noise2", "doors/airdoor1.wav")
      door.set("noise1", "doors/airdoor2.wav")
      door.set("noise3", "doors/airdoor2.wav")
    } else {
      Engine.precacheSound("doors/basesec1.wav")
      Engine.precacheSound("doors/basesec2.wav")
      door.set("noise2", "doors/basesec1.wav")
      door.set("noise1", "doors/basesec2.wav")
      door.set("noise3", "doors/basesec2.wav")
    }

    if (door.get("dmg", 0) == 0) {
      door.set("dmg", 2)
    }

    door.set("mangle", door.get("angles", [0, 0, 0]))
    door.set("angles", [0, 0, 0])
    door.set("solid", SolidTypes.BSP)
    door.set("movetype", MoveTypes.PUSH)
    door.set("classname", "func_door_secret")
    Engine.setModel(door, door.get("model", ""))
    Engine.setOrigin(door, door.get("origin", [0, 0, 0]))

    door.set("touch", "DoorsModule.secretTouch")
    door.set("blocked", "DoorsModule.secretBlocked")
    door.set("speed", 50)
    door.set("use", "DoorsModule.fdSecretUse")

    var spawnflags = door.get("spawnflags", 0)
    if (door.get("targetname", null) == null || door.get("targetname", null) == "" || Engine.bitAnd(spawnflags, SecretDoorFlags.ALWAYS_SHOOT) != 0) {
      door.set("health", 10000)
      door.set("takedamage", DamageValues.YES)
      door.set("th_pain", "DoorsModule.fdSecretPain")
      door.set("th_die", "DoorsModule.fdSecretUse")
    }

    door.set("oldorigin", door.get("origin", [0, 0, 0]))

    if (door.get("wait", 0) == 0) {
      door.set("wait", 5)
    }
  }

  // ------------------------------------------------------------------------
  // Compatibility wrappers -------------------------------------------------

  static door_blocked(globals, door, other) {
    DoorsModule.doorBlocked(globals, door, other)
  }
  static door_hit_top(globals, door) { DoorsModule.doorHitTop(globals, door) }
  static door_hit_bottom(globals, door) { DoorsModule.doorHitBottom(globals, door) }
  static door_go_down(globals, door) { DoorsModule.doorGoDown(globals, door) }
  static door_go_up(globals, door, activator) {
    DoorsModule.doorGoUp(globals, door, activator)
  }
  static door_fire(globals, door, activator) {
    DoorsModule.doorFire(globals, door, activator)
  }
  static door_use(globals, door) { DoorsModule.doorUse(globals, door) }
  static door_trigger_touch(globals, trigger, other) {
    DoorsModule.doorTriggerTouch(globals, trigger, other)
  }
  static door_killed(globals, door) { DoorsModule.doorKilled(globals, door) }
  static door_touch(globals, door, other) { DoorsModule.doorTouch(globals, door, other) }
  static LinkDoors(globals, door) { DoorsModule.linkDoors(globals, door) }
  static func_door(globals, door) { DoorsModule.funcDoor(globals, door) }
  static fd_secret_use(globals, door, activator) {
    DoorsModule.fdSecretUse(globals, door, activator)
  }
  static fd_secret_pain(globals, door, attacker, damage) {
    DoorsModule.fdSecretPain(globals, door, attacker, damage)
  }
  static fd_secret_move1(globals, door) { DoorsModule.fdSecretMove1(globals, door) }
  static fd_secret_move2(globals, door) { DoorsModule.fdSecretMove2(globals, door) }
  static fd_secret_move3(globals, door) { DoorsModule.fdSecretMove3(globals, door) }
  static fd_secret_move4(globals, door) { DoorsModule.fdSecretMove4(globals, door) }
  static fd_secret_move5(globals, door) { DoorsModule.fdSecretMove5(globals, door) }
  static fd_secret_move6(globals, door) { DoorsModule.fdSecretMove6(globals, door) }
  static fd_secret_done(globals, door) { DoorsModule.fdSecretDone(globals, door) }
  static secret_blocked(globals, door, other) {
    DoorsModule.secretBlocked(globals, door, other)
  }
  static secret_touch(globals, door, other) {
    DoorsModule.secretTouch(globals, door, other)
  }
  static func_door_secret(globals, door) { DoorsModule.funcDoorSecret(globals, door) }
  static spawn_field(globals, owner, mins, maxs) {
    return DoorsModule._spawnField(globals, owner, mins, maxs)
  }
  static EntitiesTouching(globals, a, b) { return DoorsModule._entitiesTouching(a, b) }
  static entities_touching(globals, a, b) { return DoorsModule._entitiesTouching(a, b) }
}
