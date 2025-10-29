// Fight.wren
// Ports the combat decision making from fight.qc, including attack selection
// heuristics and the melee helper utilities used by monsters.

import "./Engine" for Engine
import "./Globals" for Ranges, AttackStates
import "./Subs" for SubsModule
import "./Combat" for CombatModule
import "./Knight" for KnightModule
import "./Fiend" for FiendModule
import "./Scrag" for ScragModule

class FightModule {
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
    var length = FightModule._vectorLength(v)
    if (length == 0) return [0, 0, 0]
    var inv = 1 / length
    return [v[0] * inv, v[1] * inv, v[2] * inv]
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

  static _canDamage(globals, target, inflictor) {
    return CombatModule.CanDamage(globals, target, inflictor)
  }

  static _traceLine(start, end, ignoreMonsters, ignoreEntity) {
    return Engine.traceLine(start, end, ignoreMonsters, ignoreEntity)
  }

  static _random() {
    return Engine.random()
  }

  static _calcShotTrace(globals, shooter, target) {
    var start = FightModule._vectorAdd(shooter.get("origin", [0, 0, 0]), shooter.get("view_ofs", [0, 0, 0]))
    var end = FightModule._vectorAdd(target.get("origin", [0, 0, 0]), target.get("view_ofs", [0, 0, 0]))
    return FightModule._traceLine(start, end, false, shooter)
  }

  static _traceHitsTarget(trace, target) {
    if (trace == null) return false
    var inOpen = trace.containsKey("inOpen") ? trace["inOpen"] : false
    var inWater = trace.containsKey("inWater") ? trace["inWater"] : false
    if (inOpen && inWater) return false

    if (trace.containsKey("entity") && trace["entity"] == target) return true
    if (trace.containsKey("fraction") && trace["fraction"] >= 1) return true
    return false
  }

  static knightAttack(globals, monster) {
    if (monster == null) return

    var enemy = monster.get("enemy", null)
    if (enemy == null) return

    var enemyEye = FightModule._vectorAdd(enemy.get("origin", [0, 0, 0]), enemy.get("view_ofs", [0, 0, 0]))
    var selfEye = FightModule._vectorAdd(monster.get("origin", [0, 0, 0]), monster.get("view_ofs", [0, 0, 0]))
    var delta = FightModule._vectorSub(enemyEye, selfEye)
    var distance = FightModule._vectorLength(delta)

    if (distance < 80) {
      KnightModule.knight_atk1(globals, monster)
    } else {
      KnightModule.knight_runatk1(globals, monster)
    }
  }

  static checkAttack(globals, monster, enemyVisible, enemyInfront, enemyRange, enemyYaw) {
    var enemy = monster.get("enemy", null)
    if (enemy == null) return false

    var trace = FightModule._calcShotTrace(globals, monster, enemy)
    if (!FightModule._traceHitsTarget(trace, enemy)) {
      return false
    }

    if (enemyRange == Ranges.MELEE) {
      var melee = monster.get("th_melee", null)
      if (melee != null && melee != "") {
        FightModule._callStoredFunction(globals, monster, "th_melee", [])
        return true
      }
    }

    var missile = monster.get("th_missile", null)
    if (missile == null || missile == "") return false

    if (globals.time < monster.get("attack_finished", 0.0)) return false

    if (enemyRange == Ranges.FAR) return false

    var chance = 0.0
    if (enemyRange == Ranges.MELEE) {
      chance = 0.9
      monster.set("attack_finished", 0.0)
    } else if (enemyRange == Ranges.NEAR) {
      chance = monster.get("th_melee", null) != null ? 0.2 : 0.4
    } else if (enemyRange == Ranges.MID) {
      chance = monster.get("th_melee", null) != null ? 0.05 : 0.1
    }

    if (FightModule._random() < chance) {
      FightModule._callStoredFunction(globals, monster, "th_missile", [])
      SubsModule.SUB_AttackFinished(globals, monster, 2 * FightModule._random())
      return true
    }

    return false
  }

  static ai_face(globals, monster, enemyYaw) {
    monster.set("ideal_yaw", enemyYaw)
    Engine.changeYaw(monster)
  }

  static _enemyYaw(monster) {
    var enemy = monster.get("enemy", null)
    if (enemy == null) return monster.get("angles", [0, 0, 0])[1]
    return FightModule._vectorToYaw(FightModule._vectorSub(enemy.get("origin", [0, 0, 0]), monster.get("origin", [0, 0, 0])))
  }

  static ai_charge(globals, monster, dist) {
    var enemyYaw = FightModule._enemyYaw(monster)
    FightModule.ai_face(globals, monster, enemyYaw)
    Engine.moveToGoal(monster, dist)
  }

  static ai_charge_side(globals, monster) {
    if (monster == null) return
    var enemy = monster.get("enemy", null)
    if (enemy == null) return

    var angles = monster.get("angles", [0, 0, 0])
    monster.set("ideal_yaw", FightModule._vectorToYaw(FightModule._vectorSub(enemy.get("origin", [0, 0, 0]), monster.get("origin", [0, 0, 0]))))
    Engine.changeYaw(monster)

    FightModule._makeVectorsFixed(monster.get("angles", angles))
    var right = globals.vRight
    var offset = FightModule._vectorSub(enemy.get("origin", [0, 0, 0]), FightModule._vectorScale(right, 30))
    var heading = FightModule._vectorToYaw(FightModule._vectorSub(offset, monster.get("origin", [0, 0, 0])))
    Engine.walkMove(monster, heading, 20)
  }

  static ai_melee(globals, monster) {
    if (monster == null) return
    var enemy = monster.get("enemy", null)
    if (enemy == null) return

    var delta = FightModule._vectorSub(enemy.get("origin", [0, 0, 0]), monster.get("origin", [0, 0, 0]))
    if (FightModule._vectorLength(delta) > 60) return

    var damage = (FightModule._random() + FightModule._random() + FightModule._random()) * 3
    CombatModule.tDamage(globals, enemy, monster, monster, damage)
  }

  static ai_melee_side(globals, monster) {
    if (monster == null) return
    var enemy = monster.get("enemy", null)
    if (enemy == null) return

    FightModule.ai_charge_side(globals, monster)

    if (!FightModule._canDamage(globals, enemy, monster)) return

    var delta = FightModule._vectorSub(enemy.get("origin", [0, 0, 0]), monster.get("origin", [0, 0, 0]))
    if (FightModule._vectorLength(delta) > 60) return

    var damage = (FightModule._random() + FightModule._random() + FightModule._random()) * 3
    CombatModule.tDamage(globals, enemy, monster, monster, damage)
  }

  static _checkShot(globals, monster, enemy) {
    var trace = FightModule._calcShotTrace(globals, monster, enemy)
    return FightModule._traceHitsTarget(trace, enemy)
  }

  static soldierCheckAttack(globals, monster, enemyRange) {
    var enemy = monster.get("enemy", null)
    if (enemy == null) return false

    var trace = FightModule._calcShotTrace(globals, monster, enemy)
    if (!FightModule._traceHitsTarget(trace, enemy)) return false

    if (globals.time < monster.get("attack_finished", 0.0)) return false
    if (enemyRange == Ranges.FAR) return false

    var chance = 0.0
    if (enemyRange == Ranges.MELEE) chance = 0.9
    else if (enemyRange == Ranges.NEAR) chance = 0.4
    else if (enemyRange == Ranges.MID) chance = 0.05

    if (FightModule._random() < chance) {
      FightModule._callStoredFunction(globals, monster, "th_missile", [])
      SubsModule.SUB_AttackFinished(globals, monster, 1 + FightModule._random())
      if (FightModule._random() < 0.3) {
        var lefty = monster.get("lefty", 0.0)
        monster.set("lefty", lefty == 0 ? 1.0 : 0.0)
      }
      return true
    }

    return false
  }

  static shamCheckAttack(globals, monster, enemyVisible, enemyRange) {
    var enemy = monster.get("enemy", null)
    if (enemy == null) return false

    if (enemyRange == Ranges.MELEE && FightModule._canDamage(globals, enemy, monster)) {
      monster.set("attack_state", AttackStates.MELEE)
      return true
    }

    if (globals.time < monster.get("attack_finished", 0.0)) return false
    if (!enemyVisible) return false

    var start = FightModule._vectorAdd(monster.get("origin", [0, 0, 0]), monster.get("view_ofs", [0, 0, 0]))
    var end = FightModule._vectorAdd(enemy.get("origin", [0, 0, 0]), enemy.get("view_ofs", [0, 0, 0]))
    if (FightModule._vectorLength(FightModule._vectorSub(start, end)) > 600) return false

    var trace = FightModule._calcShotTrace(globals, monster, enemy)
    if (!FightModule._traceHitsTarget(trace, enemy)) return false

    if (enemyRange == Ranges.FAR) return false

    monster.set("attack_state", AttackStates.MISSILE)
    SubsModule.SUB_AttackFinished(globals, monster, 2 + 2 * FightModule._random())
    return true
  }

  static ogreCheckAttack(globals, monster, enemyVisible, enemyRange) {
    var enemy = monster.get("enemy", null)
    if (enemy == null) return false

    if (enemyRange == Ranges.MELEE && FightModule._canDamage(globals, enemy, monster)) {
      monster.set("attack_state", AttackStates.MELEE)
      return true
    }

    if (globals.time < monster.get("attack_finished", 0.0)) return false
    if (!enemyVisible) return false

    var trace = FightModule._calcShotTrace(globals, monster, enemy)
    if (!FightModule._traceHitsTarget(trace, enemy)) return false

    if (globals.time < monster.get("attack_finished", 0.0)) return false
    if (enemyRange == Ranges.FAR) return false

    var chance = 0.0
    if (enemyRange == Ranges.NEAR) chance = 0.10
    else if (enemyRange == Ranges.MID) chance = 0.05

    if (FightModule._random() >= chance) return false

    monster.set("attack_state", AttackStates.MISSILE)
    SubsModule.SUB_AttackFinished(globals, monster, 1 + 2 * FightModule._random())
    return true
  }

  static _checkDogMelee(globals, monster, enemyRange) {
    if (enemyRange != Ranges.MELEE) return false
    monster.set("attack_state", AttackStates.MELEE)
    return true
  }

  static _checkDogJump(globals, monster) {
    var enemy = monster.get("enemy", null)
    if (enemy == null) return false

    var origin = monster.get("origin", [0, 0, 0])
    var mins = monster.get("mins", [0, 0, 0])
    var maxs = monster.get("maxs", [0, 0, 0])
    var enemyOrigin = enemy.get("origin", [0, 0, 0])
    var enemyMins = enemy.get("mins", [0, 0, 0])
    var enemyMaxs = enemy.get("maxs", [0, 0, 0])
    var enemySizeZ = enemyMaxs[2] - enemyMins[2]

    if (origin[2] + mins[2] > enemyOrigin[2] + enemyMins[2] + 0.75 * enemySizeZ) return false
    if (origin[2] + maxs[2] < enemyOrigin[2] + enemyMins[2] + 0.25 * enemySizeZ) return false

    var dist = FightModule._vectorSub(enemyOrigin, origin)
    dist[2] = 0
    var distance = FightModule._vectorLength(dist)

    if (distance < 80) return false
    if (distance > 150) return false
    return true
  }

  static dogCheckAttack(globals, monster, enemyRange) {
    if (FightModule._checkDogMelee(globals, monster, enemyRange)) return true
    if (!FightModule._checkDogJump(globals, monster)) return false
    monster.set("attack_state", AttackStates.MISSILE)
    return true
  }

  static checkAnyAttack(globals, monster, enemyVisible, enemyInfront, enemyRange, enemyYaw) {
    var classname = monster.get("classname", "")
    if (!enemyVisible) return false

    if (classname == "monster_army") {
      return FightModule.soldierCheckAttack(globals, monster, enemyRange)
    }
    if (classname == "monster_ogre") {
      return FightModule.ogreCheckAttack(globals, monster, enemyVisible, enemyRange)
    }
    if (classname == "monster_shambler") {
      return FightModule.shamCheckAttack(globals, monster, enemyVisible, enemyRange)
    }
    if (classname == "monster_demon1") {
      return FiendModule.demonCheckAttack(globals, monster, enemyRange)
    }
    if (classname == "monster_dog") {
      return FightModule.dogCheckAttack(globals, monster, enemyRange)
    }
    if (classname == "monster_wizard") {
      return ScragModule.wizardCheckAttack(globals, monster, enemyVisible, enemyRange)
    }

    return FightModule.checkAttack(globals, monster, enemyVisible, enemyInfront, enemyRange, enemyYaw)
  }

  // ------------------------------------------------------------------------
  // Compatibility wrappers -------------------------------------------------

  static knight_attack(globals, monster) { FightModule.knightAttack(globals, monster) }
  static CheckAttack(globals, monster, enemyVisible, enemyInfront, enemyRange, enemyYaw) {
    return FightModule.checkAttack(globals, monster, enemyVisible, enemyInfront, enemyRange, enemyYaw)
  }
  static SoldierCheckAttack(globals, monster, enemyRange) {
    return FightModule.soldierCheckAttack(globals, monster, enemyRange)
  }
  static ShamCheckAttack(globals, monster, enemyVisible, enemyRange) {
    return FightModule.shamCheckAttack(globals, monster, enemyVisible, enemyRange)
  }
  static OgreCheckAttack(globals, monster, enemyVisible, enemyRange) {
    return FightModule.ogreCheckAttack(globals, monster, enemyVisible, enemyRange)
  }
  static DogCheckAttack(globals, monster, enemyRange) {
    return FightModule.dogCheckAttack(globals, monster, enemyRange)
  }
  static CheckAnyAttack(globals, monster, enemyVisible, enemyInfront, enemyRange, enemyYaw) {
    return FightModule.checkAnyAttack(globals, monster, enemyVisible, enemyInfront, enemyRange, enemyYaw)
  }
}
