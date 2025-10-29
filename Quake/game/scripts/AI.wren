// AI.wren
// Ports the monster AI support routines from ai.qc so that enemy behavior can
// be executed entirely from Wren.

import "./Engine" for Engine
import "./Globals" for Ranges, AttackStates, CombatStyles, PathResults
import "./Globals" for SolidTypes, Channels, Attenuations
import "./Subs" for SubsModule
import "./Fight" for FightModule

class AIModule {
  static var _sightEntity = null
  static var _sightEntityTime = 0.0
  static var _moveDistance = 0.0

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

  static _vectorNormalize(v) {
    var length = AIModule._vectorLength(v)
    if (length == 0) return [0, 0, 0]
    var inv = 1 / length
    return [v[0] * inv, v[1] * inv, v[2] * inv]
  }

  static _rint(value) {
    return (value < 0) ? ((value - 0.5).ceil) : ((value + 0.5).floor)
  }

  static _angleMod(value) {
    var result = value
    while (result >= 360) result = result - 360
    while (result < 0) result = result + 360
    return result
  }

  static _makeVectorsFixed(angles) {
    if (angles == null) return
    var adjusted = [-angles[0], angles[1], angles[2]]
    Engine.makeVectors(adjusted)
  }

  static _vectorToYaw(vector) {
    var angles = Engine.vectorToAngles(vector)
    return angles[1]
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

  static movetarget_f(globals, target) {
    if (target == null) return
    var targetName = target.get("targetname", null)
    if (targetName == null || targetName == "") {
      Engine.objError("monster_movetarget: no targetname")
      return
    }

    target.set("solid", SolidTypes.TRIGGER)
    target.set("touch", "AIModule.t_movetarget")
    Engine.setSize(target, [-8, -8, -8], [8, 8, 8])
  }

  static path_corner(globals, corner) {
    AIModule.movetarget_f(globals, corner)
  }

  static t_movetarget(globals, target, other) {
    if (target == null || other == null) return

    var moveTarget = other.get("movetarget", null)
    if (moveTarget != target) return

    var enemy = other.get("enemy", null)
    if (enemy != null) return

    var next = Engine.find(globals.world, "targetname", target.get("target", null))
    other.set("movetarget", next)
    other.set("goalentity", next)
    var goal = next
    if (goal != null) {
      var yaw = AIModule._vectorToYaw(AIModule._vectorSub(goal.get("origin", [0, 0, 0]), other.get("origin", [0, 0, 0])))
      other.set("ideal_yaw", yaw)
      var classname = goal.get("classname", "")
      if (classname == "path_corner") {
        AIModule._callStoredFunction(globals, other, "th_walk", [])
      } else {
        other.set("pausetime", 99999999.0)
        AIModule._callStoredFunction(globals, other, "th_stand", [])
      }
      return
    }

    other.set("pausetime", 99999999.0)
    AIModule._callStoredFunction(globals, other, "th_stand", [])
  }

  static range(globals, monster, target) {
    if (monster == null || target == null) return Ranges.FAR

    var spot1 = AIModule._vectorAdd(monster.get("origin", [0, 0, 0]), monster.get("view_ofs", [0, 0, 0]))
    var spot2 = AIModule._vectorAdd(target.get("origin", [0, 0, 0]), target.get("view_ofs", [0, 0, 0]))
    var delta = AIModule._vectorSub(spot1, spot2)
    var distance = AIModule._vectorLength(delta)

    if (distance < 120) return Ranges.MELEE
    if (distance < 500) return Ranges.NEAR
    if (distance < 1000) return Ranges.MID
    return Ranges.FAR
  }

  static visible(globals, monster, target) {
    if (monster == null || target == null) return false

    var spot1 = AIModule._vectorAdd(monster.get("origin", [0, 0, 0]), monster.get("view_ofs", [0, 0, 0]))
    var spot2 = AIModule._vectorAdd(target.get("origin", [0, 0, 0]), target.get("view_ofs", [0, 0, 0]))
    var trace = Engine.traceLine(spot1, spot2, true, monster)
    if (trace == null) return false

    var inOpen = trace.containsKey("inOpen") ? trace["inOpen"] : false
    var inWater = trace.containsKey("inWater") ? trace["inWater"] : false
    if (inOpen && inWater) return false

    if (trace.containsKey("fraction") && trace["fraction"] >= 1) return true
    if (trace.containsKey("entity") && trace["entity"] == target) return true
    return false
  }

  static infront(globals, monster, target) {
    if (monster == null || target == null) return false

    AIModule._makeVectorsFixed(monster.get("angles", [0, 0, 0]))
    var direction = AIModule._vectorSub(target.get("origin", [0, 0, 0]), monster.get("origin", [0, 0, 0]))
    direction = AIModule._vectorNormalize(direction)

    var forward = globals.vForward
    var dot = direction[0] * forward[0] + direction[1] * forward[1] + direction[2] * forward[2]
    return dot > 0.3
  }

  static _playSightSound(globals, monster) {
    if (monster == null) return

    var classname = monster.get("classname", "")
    var noise = monster.get("noise", null)
    if (classname == "monster_enforcer") {
      var noises = [monster.get("noise", ""), monster.get("noise1", ""), monster.get("noise2", ""), monster.get("noise4", "")]
      var choice = AIModule._rint(Engine.random() * 3)
      if (choice < 0) choice = 0
      if (choice >= noises.count) choice = noises.count - 1
      noise = noises[choice]
    }

    if (noise != null && noise != "") {
      Engine.playSound(monster, Channels.VOICE, noise, 1, Attenuations.NORMAL)
    }
  }

  static huntTarget(globals, monster) {
    if (monster == null) return
    var enemy = monster.get("enemy", null)
    monster.set("goalentity", enemy)

    var runThink = monster.get("th_run", null)
    if (runThink != null && runThink != "") {
      monster.set("think", runThink)
      monster.set("nextthink", globals.time + 0.1)
      Engine.scheduleThink(monster, runThink, 0.1)
    }

    if (enemy != null) {
      var yaw = AIModule._vectorToYaw(AIModule._vectorSub(enemy.get("origin", [0, 0, 0]), monster.get("origin", [0, 0, 0])))
      monster.set("ideal_yaw", yaw)
    }

    SubsModule.SUB_AttackFinished(globals, monster, 1.0)
  }

  static foundTarget(globals, monster) {
    if (monster == null) return
    var enemy = monster.get("enemy", null)
    if (enemy != null && enemy.get("classname", "") == "player") {
      AIModule._sightEntity = monster
      AIModule._sightEntityTime = globals.time
    }

    monster.set("show_hostile", globals.time + 1.0)
    AIModule._playSightSound(globals, monster)
    AIModule.huntTarget(globals, monster)
  }

  static findTarget(globals, monster) {
    if (monster == null) return false

    var sightTime = AIModule._sightEntityTime
    var client
    var spawnflags = monster.get("spawnflags", 0)

    if (sightTime >= globals.time - 0.1 && Engine.bitAnd(spawnflags, 3) == 0) {
      client = AIModule._sightEntity
      if (client != null && client.get("enemy", null) == monster.get("enemy", null)) {
        return true
      }
    } else {
      client = Engine.checkClient()
      if (client == null) return false
    }

    if (client == monster.get("enemy", null)) return false

    var distance = AIModule.range(globals, monster, client)
    if (distance == Ranges.FAR) return false

    if (!AIModule.visible(globals, monster, client)) return false

    if (distance == Ranges.NEAR) {
      if (client.get("show_hostile", 0.0) < globals.time && !AIModule.infront(globals, monster, client)) {
        return false
      }
    } else if (distance == Ranges.MID) {
      if (!AIModule.infront(globals, monster, client)) return false
    }

    monster.set("enemy", client)
    var enemy = monster.get("enemy", null)
    if (enemy != null && enemy.get("classname", "") != "player") {
      enemy = enemy.get("enemy", null)
      if (enemy == null || enemy.get("classname", "") != "player") {
        monster.set("enemy", globals.world)
        return false
      }
      monster.set("enemy", enemy)
    }

    AIModule.foundTarget(globals, monster)
    return true
  }

  static ai_forward(globals, monster, dist) {
    if (monster == null) return
    Engine.walkMove(monster, monster.get("angles", [0, 0, 0])[1], dist)
  }

  static ai_back(globals, monster, dist) {
    if (monster == null) return
    Engine.walkMove(monster, monster.get("angles", [0, 0, 0])[1] + 180, dist)
  }

  static ai_pain(globals, monster, dist) {
    AIModule.ai_back(globals, monster, dist)
  }

  static ai_painforward(globals, monster, dist) {
    if (monster == null) return
    Engine.walkMove(monster, monster.get("ideal_yaw", monster.get("angles", [0, 0, 0])[1]), dist)
  }

  static ai_walk(globals, monster, dist) {
    AIModule._moveDistance = dist
    if (AIModule.findTarget(globals, monster)) return
    Engine.moveToGoal(monster, dist)
  }

  static ai_stand(globals, monster) {
    if (AIModule.findTarget(globals, monster)) return
    var pauseTime = monster.get("pausetime", 0.0)
    if (globals.time > pauseTime) {
      AIModule._callStoredFunction(globals, monster, "th_walk", [])
    }
  }

  static _changeYaw(monster) {
    if (monster == null) return
    Engine.changeYaw(monster)
  }

  static chooseTurn(globals, monster, dest) {
    if (monster == null) return

    var target = dest == null ? monster.get("origin", [0, 0, 0]) : dest
    var direction = AIModule._vectorSub(monster.get("origin", [0, 0, 0]), target)

    var plane = (globals != null && globals.tracePlaneNormal != null) ? globals.tracePlaneNormal : [0, 0, 0]
    if (plane[0] == 0 && plane[1] == 0) {
      var flat = [direction[0], direction[1], 0]
      if (flat[0] == 0 && flat[1] == 0) return
      monster.set("ideal_yaw", AIModule._vectorToYaw(flat))
      return
    }

    var candidate = [plane[1], -plane[0], 0]
    var dot = direction[0] * candidate[0] + direction[1] * candidate[1] + direction[2] * candidate[2]

    var chosen = dot > 0 ? [-plane[1], plane[0], 0] : [plane[1], -plane[0], 0]
    if (chosen[0] == 0 && chosen[1] == 0) return

    monster.set("ideal_yaw", AIModule._vectorToYaw(chosen))
  }

  static ai_turn(globals, monster) {
    if (AIModule.findTarget(globals, monster)) return
    AIModule._changeYaw(monster)
  }

  static ai_face(globals, monster, enemyYaw) {
    monster.set("ideal_yaw", enemyYaw)
    AIModule._changeYaw(monster)
  }

  static facingIdeal(monster) {
    if (monster == null) return false
    var angles = monster.get("angles", [0, 0, 0])
    var ideal = monster.get("ideal_yaw", angles[1])
    var delta = AIModule._angleMod(angles[1] - ideal)
    return !(delta > 45 && delta < 315)
  }

  static ai_run_melee(globals, monster, enemyYaw) {
    monster.set("ideal_yaw", enemyYaw)
    AIModule._changeYaw(monster)
    if (AIModule.facingIdeal(monster)) {
      AIModule._callStoredFunction(globals, monster, "th_melee", [])
      monster.set("attack_state", AttackStates.STRAIGHT)
    }
  }

  static ai_run_missile(globals, monster, enemyYaw) {
    monster.set("ideal_yaw", enemyYaw)
    AIModule._changeYaw(monster)
    if (AIModule.facingIdeal(monster)) {
      AIModule._callStoredFunction(globals, monster, "th_missile", [])
      monster.set("attack_state", AttackStates.STRAIGHT)
    }
  }

  static ai_run_slide(globals, monster) {
    if (monster == null) return
    monster.set("ideal_yaw", monster.get("ideal_yaw", monster.get("angles", [0, 0, 0])[1]))
    AIModule._changeYaw(monster)
    var lefty = monster.get("lefty", 0.0)
    var offset = lefty != 0 ? 90.0 : -90.0
    if (Engine.walkMove(monster, monster.get("ideal_yaw", 0.0) + offset, AIModule._moveDistance)) {
      return
    }
    monster.set("lefty", 1 - lefty)
    Engine.walkMove(monster, monster.get("ideal_yaw", 0.0) - offset, AIModule._moveDistance)
  }

  static ai_pathtogoal(globals, monster, dist, enemy, enemyVisible, enemyRange) {
    var allowPath = monster.get("allowPathFind", false)
    if (!allowPath) {
      Engine.moveToGoal(monster, dist)
      return
    }

    if (enemyVisible && enemy != null) {
      var combatStyle = monster.get("combat_style", CombatStyles.NONE)
      if (combatStyle == CombatStyles.RANGED) {
        Engine.moveToGoal(monster, dist)
        return
      } else if (combatStyle == CombatStyles.MELEE) {
        if (enemyRange > Ranges.NEAR) {
          var result = Engine.walkPathToGoal(monster, dist, enemy.get("origin", [0, 0, 0]))
          if (result == PathResults.IN_PROGRESS) {
            return
          }
        }
      } else if (combatStyle == CombatStyles.MIXED) {
        if (enemyRange > Ranges.MID) {
          var res = Engine.walkPathToGoal(monster, dist, enemy.get("origin", [0, 0, 0]))
          if (res == PathResults.IN_PROGRESS) {
            return
          }
        }
      }
    } else if (enemy != null) {
      var follow = Engine.walkPathToGoal(monster, dist, enemy.get("origin", [0, 0, 0]))
      if (follow == PathResults.IN_PROGRESS) {
        return
      }
    }

    Engine.moveToGoal(monster, dist)
  }

  static ai_run(globals, monster, dist) {
    AIModule._moveDistance = dist
    if (monster == null) return

    var enemy = monster.get("enemy", null)
    if (enemy != null && enemy.get("health", 0) <= 0) {
      var oldEnemy = monster.get("oldenemy", globals.world)
      if (oldEnemy != null && oldEnemy.get("health", 0) > 0) {
        monster.set("enemy", oldEnemy)
        AIModule.huntTarget(globals, monster)
      } else {
        if (monster.get("movetarget", null) != null) {
          AIModule._callStoredFunction(globals, monster, "th_walk", [])
        } else {
          AIModule._callStoredFunction(globals, monster, "th_stand", [])
        }
        return
      }
      enemy = monster.get("enemy", null)
    }

    monster.set("show_hostile", globals.time + 1.0)

    var enemyVisible = AIModule.visible(globals, monster, enemy)
    if (enemyVisible) {
      monster.set("search_time", globals.time + 5.0)
    }

    if (globals.coop != 0 && monster.get("search_time", 0.0) < globals.time) {
      if (AIModule.findTarget(globals, monster)) return
    }

    var enemyInfront = AIModule.infront(globals, monster, enemy)
    var enemyRange = AIModule.range(globals, monster, enemy)
    var enemyYaw = 0.0
    if (enemy != null) {
      enemyYaw = AIModule._vectorToYaw(AIModule._vectorSub(enemy.get("origin", [0, 0, 0]), monster.get("origin", [0, 0, 0])))
    }

    var attackState = monster.get("attack_state", AttackStates.STRAIGHT)
    if (attackState == AttackStates.MISSILE) {
      AIModule.ai_run_missile(globals, monster, enemyYaw)
      return
    }
    if (attackState == AttackStates.MELEE) {
      AIModule.ai_run_melee(globals, monster, enemyYaw)
      return
    }

    if (FightModule.checkAnyAttack(globals, monster, enemyVisible, enemyInfront, enemyRange, enemyYaw)) {
      return
    }

    if (attackState == AttackStates.SLIDING) {
      AIModule.ai_run_slide(globals, monster)
      return
    }

    AIModule.ai_pathtogoal(globals, monster, dist, enemy, enemyVisible, enemyRange)
  }

  // ------------------------------------------------------------------------
  // Compatibility wrappers -------------------------------------------------

  static anglemod(value) { return AIModule._angleMod(value) }
  static FacingIdeal(monster) { return AIModule.facingIdeal(monster) }
  static FindTarget(globals, monster) { return AIModule.findTarget(globals, monster) }
  static FoundTarget(globals, monster) { AIModule.foundTarget(globals, monster) }
  static HuntTarget(globals, monster) { AIModule.huntTarget(globals, monster) }
  static ChooseTurn(globals, monster, dest) { AIModule.chooseTurn(globals, monster, dest) }
  static CheckAnyAttack(globals, monster, enemyVisible, enemyInfront, enemyRange, enemyYaw) {
    return FightModule.checkAnyAttack(globals, monster, enemyVisible, enemyInfront, enemyRange, enemyYaw)
  }
}
