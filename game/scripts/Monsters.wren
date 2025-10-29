// Monsters.wren
// Ports the shared monster setup routines from monsters.qc, providing
// consistent initialization for walking, flying, and swimming enemies.

import "./Engine" for Engine
import "./Globals" for Teams, PlayerFlags, DamageValues, Items
import "./Subs" for SubsModule

class MonstersModule {
  static _vectorSub(a, b) {
    return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
  }

  static _vectorToYaw(vector) {
    var angles = Engine.vectorToAngles(vector)
    return angles[1]
  }

  static setFrame(globals, monster, frame, nextThink, delay) {
    if (monster == null) return
    if (frame != null) monster.set("frame", frame)

    if (nextThink != null && nextThink != "") {
      var wait = delay == null ? 0.1 : delay
      monster.set("think", nextThink)
      monster.set("nextthink", globals.time + wait)
      Engine.scheduleThink(monster, nextThink, wait)
    }
  }

  static _callStoredFunction(globals, entity, field, args) {
    if (entity == null) return
    var functionName = entity.get(field, null)
    if (functionName == null || functionName == "") return

    var previousSelf = globals.self
    var previousOther = globals.other
    globals.self = entity
    Engine.callEntityFunction(entity, functionName, args)
    globals.self = previousSelf
    globals.other = previousOther
  }

  static monster_use(globals, monster, activator) {
    if (monster == null || activator == null) return

    if (monster.get("enemy", null) != null) return
    if (monster.get("health", 0) <= 0) return

    var items = activator.get("items", 0)
    if (Engine.bitAnd(items, Items.INVISIBILITY) != 0) return

    var flags = activator.get("flags", 0)
    if (Engine.bitAnd(flags, PlayerFlags.NOTARGET) != 0) return

    if (activator.get("classname", "") != "player") return

    monster.set("enemy", activator)
    monster.set("nextthink", globals.time + 0.1)
    monster.set("think", "AIModule.foundTarget")
    Engine.scheduleThink(monster, "AIModule.foundTarget", 0.1)
  }

  static monsterDeathUse(globals, monster) {
    if (monster == null) return

    var flags = monster.get("flags", 0)
    flags = flags - Engine.bitAnd(flags, PlayerFlags.FLY)
    flags = flags - Engine.bitAnd(flags, PlayerFlags.SWIM)
    monster.set("flags", flags)

    var targetName = monster.get("target", null)
    if (targetName == null || targetName == "") return

    SubsModule.useTargets(globals, monster, monster.get("enemy", null))
  }

  static walkmonster_start_go(globals, monster) {
    if (monster == null) return

    var origin = monster.get("origin", [0, 0, 0])
    origin[2] = origin[2] + 1
    monster.set("origin", origin)
    Engine.dropToFloor(monster)

    if (!Engine.walkMove(monster, 0, 0)) {
      Engine.log("walkmonster in wall at: " + monster.get("origin", [0, 0, 0]).toString)
    }

    monster.set("takedamage", DamageValues.AIM)

    var angles = monster.get("angles", [0, 0, 0])
    monster.set("ideal_yaw", angles[1])

    if (monster.get("yaw_speed", 0.0) == 0) {
      monster.set("yaw_speed", 20.0)
    }

    monster.set("view_ofs", [0, 0, 25])
    monster.set("use", "MonstersModule.monster_use")
    monster.set("team", Teams.MONSTERS)
    monster.set("flags", Engine.bitOr(monster.get("flags", 0), PlayerFlags.MONSTER))

    var targetName = monster.get("target", null)
    if (targetName != null && targetName != "") {
      var goal = Engine.find(globals.world, "targetname", targetName)
      monster.set("goalentity", goal)
      monster.set("movetarget", goal)
      if (goal != null) {
        monster.set("ideal_yaw", MonstersModule._vectorToYaw(MonstersModule._vectorSub(goal.get("origin", [0, 0, 0]), monster.get("origin", [0, 0, 0]))))
        if (goal.get("classname", "") == "path_corner") {
          MonstersModule._callStoredFunction(globals, monster, "th_walk", [])
        } else {
          monster.set("pausetime", 99999999.0)
          MonstersModule._callStoredFunction(globals, monster, "th_stand", [])
        }
      } else {
        Engine.log("Monster can't find target")
      }
    } else {
      monster.set("pausetime", 99999999.0)
      MonstersModule._callStoredFunction(globals, monster, "th_stand", [])
    }
  }

  static walkmonster_start(globals, monster) {
    if (monster == null) return
    monster.set("nextthink", monster.get("nextthink", globals.time) + Engine.random() * 0.5)
    monster.set("think", "MonstersModule.walkmonster_start_go")
    Engine.scheduleThink(monster, "MonstersModule.walkmonster_start_go", 0.0)
    globals.totalMonsters = globals.totalMonsters + 1
  }

  static flymonster_start_go(globals, monster) {
    if (monster == null) return

    monster.set("takedamage", DamageValues.AIM)
    var angles = monster.get("angles", [0, 0, 0])
    monster.set("ideal_yaw", angles[1])

    if (monster.get("yaw_speed", 0.0) == 0) {
      monster.set("yaw_speed", 10.0)
    }

    monster.set("view_ofs", [0, 0, 25])
    monster.set("use", "MonstersModule.monster_use")
    monster.set("team", Teams.MONSTERS)
    var flags = monster.get("flags", 0)
    flags = Engine.bitOr(flags, PlayerFlags.FLY)
    flags = Engine.bitOr(flags, PlayerFlags.MONSTER)
    monster.set("flags", flags)

    if (!Engine.walkMove(monster, 0, 0)) {
      Engine.log("flymonster in wall at: " + monster.get("origin", [0, 0, 0]).toString)
    }

    var targetName = monster.get("target", null)
    if (targetName != null && targetName != "") {
      var goal = Engine.find(globals.world, "targetname", targetName)
      monster.set("goalentity", goal)
      monster.set("movetarget", goal)
      if (goal != null) {
        if (goal.get("classname", "") == "path_corner") {
          MonstersModule._callStoredFunction(globals, monster, "th_walk", [])
        } else {
          monster.set("pausetime", 99999999.0)
          MonstersModule._callStoredFunction(globals, monster, "th_stand", [])
        }
      } else {
        Engine.log("Monster can't find target")
      }
    } else {
      monster.set("pausetime", 99999999.0)
      MonstersModule._callStoredFunction(globals, monster, "th_stand", [])
    }
  }

  static flymonster_start(globals, monster) {
    if (monster == null) return
    monster.set("nextthink", monster.get("nextthink", globals.time) + Engine.random() * 0.5)
    monster.set("think", "MonstersModule.flymonster_start_go")
    Engine.scheduleThink(monster, "MonstersModule.flymonster_start_go", 0.0)
    globals.totalMonsters = globals.totalMonsters + 1
  }

  static swimmonster_start_go(globals, monster) {
    if (monster == null) return

    if (globals.deathmatch != 0) {
      Engine.removeEntity(monster)
      return
    }

    monster.set("takedamage", DamageValues.AIM)
    var angles = monster.get("angles", [0, 0, 0])
    monster.set("ideal_yaw", angles[1])

    if (monster.get("yaw_speed", 0.0) == 0) {
      monster.set("yaw_speed", 10.0)
    }

    monster.set("view_ofs", [0, 0, 10])
    monster.set("use", "MonstersModule.monster_use")
    var flags = monster.get("flags", 0)
    flags = Engine.bitOr(flags, PlayerFlags.SWIM)
    flags = Engine.bitOr(flags, PlayerFlags.MONSTER)
    monster.set("flags", flags)

    var targetName = monster.get("target", null)
    if (targetName != null && targetName != "") {
      var goal = Engine.find(globals.world, "targetname", targetName)
      monster.set("goalentity", goal)
      monster.set("movetarget", goal)
      if (goal != null) {
        monster.set("ideal_yaw", MonstersModule._vectorToYaw(MonstersModule._vectorSub(goal.get("origin", [0, 0, 0]), monster.get("origin", [0, 0, 0]))))
        MonstersModule._callStoredFunction(globals, monster, "th_walk", [])
      } else {
        Engine.log("Monster can't find target")
      }
    } else {
      monster.set("pausetime", 99999999.0)
      MonstersModule._callStoredFunction(globals, monster, "th_stand", [])
    }

    monster.set("nextthink", monster.get("nextthink", globals.time) + Engine.random() * 0.5)
  }

  static swimmonster_start(globals, monster) {
    if (monster == null) return
    monster.set("nextthink", monster.get("nextthink", globals.time) + Engine.random() * 0.5)
    monster.set("think", "MonstersModule.swimmonster_start_go")
    Engine.scheduleThink(monster, "MonstersModule.swimmonster_start_go", 0.0)
    globals.totalMonsters = globals.totalMonsters + 1
  }

  // ------------------------------------------------------------------------
  // Compatibility wrappers -------------------------------------------------

  static monster_death_use(globals, monster) { MonstersModule.monsterDeathUse(globals, monster) }
}
