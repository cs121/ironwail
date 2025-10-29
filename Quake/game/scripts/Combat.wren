// Combat.wren
// Ports critical combat-related helpers from combat.qc to Wren.

import "./Engine" for Engine
import "./AI" for AIModule
import "./Globals" for MoveTypes, PlayerFlags, Items, ServiceCodes, MessageTypes
import "./Globals" for DamageValues, Channels, Attenuations
import "./Client" for ClientModule
import "./Subs" for SubsModule

class CombatModule {
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
    var length = CombatModule._vectorLength(v)
    if (length == 0) return [0, 0, 0]
    var inv = 1 / length
    return [v[0] * inv, v[1] * inv, v[2] * inv]
  }

  static _vectorMidpoint(a, b) {
    return [(a[0] + b[0]) * 0.5, (a[1] + b[1]) * 0.5, (a[2] + b[2]) * 0.5]
  }

  static _entityCenter(entity) {
    var absMin = entity.get("absmin", null)
    var absMax = entity.get("absmax", null)
    if (absMin != null && absMax != null) {
      return CombatModule._vectorMidpoint(absMin, absMax)
    }

    var mins = entity.get("mins", [0, 0, 0])
    var maxs = entity.get("maxs", [0, 0, 0])
    var origin = entity.get("origin", [0, 0, 0])
    var offset = CombatModule._vectorScale(CombatModule._vectorAdd(mins, maxs), 0.5)
    return CombatModule._vectorAdd(origin, offset)
  }

  static _callEntityFunction(globals, entity, field, args) {
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

  static canDamage(globals, target, inflictor) {
    var targetMove = target.get("movetype", MoveTypes.WALK)
    var inflictorOrigin = inflictor.get("origin", [0, 0, 0])
    var ignore = globals.self

    if (targetMove == MoveTypes.PUSH) {
      var center = CombatModule._entityCenter(target)
      var trace = Engine.traceLine(inflictorOrigin, center, true, ignore)
      if (trace != null && trace.containsKey("fraction") && trace["fraction"] >= 1) {
        return true
      }
      if (trace != null && trace.containsKey("entity") && trace["entity"] == target) {
        return true
      }
      return false
    }

    var targetOrigin = target.get("origin", [0, 0, 0])
    var offsets = [
      [0, 0, 0],
      [15, 15, 0],
      [-15, -15, 0],
      [-15, 15, 0],
      [15, -15, 0]
    ]

    for (offset in offsets) {
      var end = CombatModule._vectorAdd(targetOrigin, offset)
      var trace = Engine.traceLine(inflictorOrigin, end, true, ignore)
      if (trace != null && trace.containsKey("fraction") && trace["fraction"] >= 1) {
        return true
      }
    }

    return false
  }

  static monsterDeathUse(globals, monster) {
    var flags = monster.get("flags", 0)
    if (Engine.bitAnd(flags, PlayerFlags.FLY) != 0) {
      flags = flags - PlayerFlags.FLY
    }
    if (Engine.bitAnd(flags, PlayerFlags.SWIM) != 0) {
      flags = flags - PlayerFlags.SWIM
    }
    monster.set("flags", flags)

    var targetName = monster.get("target", null)
    if (targetName == null || targetName == "") return

    globals.activator = monster.get("enemy", null)
    SubsModule.useTargets(globals, monster, globals.activator)
  }

  static killed(globals, target, attacker) {
    if (target.get("health", 0) < -99) {
      target.set("health", -99)
    }

    var moveType = target.get("movetype", MoveTypes.NONE)
    if (moveType == MoveTypes.PUSH || moveType == MoveTypes.NONE) {
      CombatModule._callEntityFunction(globals, target, "th_die", [])
      return
    }

    target.set("enemy", attacker)

    var flags = target.get("flags", 0)
    if (Engine.bitAnd(flags, PlayerFlags.MONSTER) != 0) {
      globals.killedMonsters = globals.killedMonsters + 1
      Engine.writeByte(MessageTypes.ALL, ServiceCodes.KILLEDMONSTER, null)

      if (attacker != null && attacker.get("classname", "") == "player") {
        attacker.set("frags", attacker.get("frags", 0) + 1)
      }

      if (attacker != null && attacker != target) {
        var attackerFlags = attacker.get("flags", 0)
        if (Engine.bitAnd(attackerFlags, PlayerFlags.MONSTER) != 0) {
          Engine.writeByte(MessageTypes.ALL, ServiceCodes.ACHIEVEMENT, null)
          Engine.writeString(MessageTypes.ALL, "ACH_FRIENDLY_FIRE", null)
        }
      }
    }

    ClientModule.clientObituary(globals, target, attacker)

    target.set("takedamage", DamageValues.NO)
    target.set("touch", "SUB_Null")

    CombatModule.monsterDeathUse(globals, target)
    CombatModule._callEntityFunction(globals, target, "th_die", [])
  }

  static tDamage(globals, target, inflictor, attacker, damage) {
    if (target == null) return
    if (target.get("takedamage", DamageValues.NO) == DamageValues.NO) return

    if (globals.coop > 0 && target != attacker) {
      if (target.get("classname", "") == "player" && attacker != null && attacker.get("classname", "") == "player") {
        var attackerFlags = attacker.get("flags", 0)
        var targetFlags = target.get("flags", 0)
        if (Engine.bitAnd(attackerFlags, PlayerFlags.ISBOT) != 0 && Engine.bitAnd(targetFlags, PlayerFlags.ISBOT) == 0) {
          return
        }
      }
    }

    if (target.get("classname", "") == "monster_oldone" && damage < 9999) {
      return
    }

    globals.damageAttacker = attacker

    if (attacker != null) {
      var quadTime = attacker.get("super_damage_finished", 0)
      if (quadTime > Engine.time()) {
        damage = damage * 4
      }
    }

    var armorType = target.get("armortype", 0.0)
    var armorValue = target.get("armorvalue", 0.0)
    var save = (armorType * damage).ceil

    if (save >= armorValue) {
      save = armorValue
      target.set("armortype", 0.0)
      var items = target.get("items", 0)
      var armorBits = Engine.bitOrMany([Items.ARMOR1, Items.ARMOR2, Items.ARMOR3])
      items = items - Engine.bitAnd(items, armorBits)
      target.set("items", items)
    }

    target.set("armorvalue", armorValue - save)
    var take = (damage - save).ceil

    var targetFlags = target.get("flags", 0)
    if (Engine.bitAnd(targetFlags, PlayerFlags.CLIENT) != 0) {
      target.set("dmg_take", target.get("dmg_take", 0) + take)
      target.set("dmg_save", target.get("dmg_save", 0) + save)
      target.set("dmg_inflictor", inflictor)
    }

    if (inflictor != globals.world && target.get("movetype", MoveTypes.NONE) == MoveTypes.WALK) {
      var center = CombatModule._entityCenter(inflictor)
      var dir = CombatModule._vectorNormalize(CombatModule._vectorSub(target.get("origin", [0, 0, 0]), center))
      var velocity = target.get("velocity", [0, 0, 0])
      velocity = CombatModule._vectorAdd(velocity, CombatModule._vectorScale(dir, damage * 8))
      target.set("velocity", velocity)
    }

    if (Engine.bitAnd(targetFlags, PlayerFlags.GODMODE) != 0) {
      return
    }

    var invincibleUntil = target.get("invincible_finished", 0)
    var now = Engine.time()
    if (invincibleUntil >= now) {
      var nextSound = target.get("invincible_sound", 0)
      if (nextSound < now) {
        Engine.playSound(target, Channels.ITEM, "items/protect3.wav", 1, Attenuations.NORMAL)
        target.set("invincible_sound", now + 2)
      }
      return
    }

    if (globals.teamplay == 1 && target != attacker) {
      var targetTeam = target.get("team", 0)
      var attackerTeam = attacker == null ? 0 : attacker.get("team", 0)
      if (targetTeam > 0 && targetTeam == attackerTeam) {
        return
      }
    }

    if (target.get("classname", "") == "player" && take != 0) {
      target.set("took_damage", 1)
    }

    target.set("health", target.get("health", 0) - take)
    if (target.get("health", 0) <= 0) {
      CombatModule.killed(globals, target, attacker)
      return
    }

    if (Engine.bitAnd(targetFlags, PlayerFlags.MONSTER) != 0 && attacker != globals.world && attacker != null) {
      if (target != attacker) {
        var currentEnemy = target.get("enemy", null)
        if (attacker != currentEnemy) {
          var targetClass = target.get("classname", "")
          var attackerClass = attacker.get("classname", "")
          if (targetClass != attackerClass || attackerClass == "monster_army") {
            if (currentEnemy != null && currentEnemy.get("classname", "") == "player") {
              target.set("oldenemy", currentEnemy)
            }
            target.set("enemy", attacker)
            AIModule.foundTarget(globals, target)
          }
        }
      }
    }

    var painFunc = target.get("th_pain", null)
    if (painFunc != null && painFunc != "") {
      CombatModule._callEntityFunction(globals, target, "th_pain", [attacker, take])
      if (globals.skill == 3) {
        target.set("pain_finished", now + 5)
      }
    }
  }

  static tRadiusDamage(globals, inflictor, attacker, damage, ignore) {
    var origin = inflictor.get("origin", [0, 0, 0])
    var entities = Engine.findRadius(origin, damage + 40)
    if (entities == null) return

    for (other in entities) {
      if (other == ignore) continue
      if (other.get("takedamage", DamageValues.NO) == DamageValues.NO) continue

      var otherCenter = CombatModule._entityCenter(other)
      var delta = CombatModule._vectorSub(origin, otherCenter)
      var points = 0.5 * CombatModule._vectorLength(delta)
      if (points < 0) points = 0
      points = damage - points
      if (other == attacker) points = points * 0.5
      if (points <= 0) continue

      if (!CombatModule.canDamage(globals, other, inflictor)) continue

      if (other.get("classname", "") == "monster_shambler") {
        CombatModule.tDamage(globals, other, inflictor, attacker, points * 0.5)
      } else {
        CombatModule.tDamage(globals, other, inflictor, attacker, points)
      }
    }
  }

  static tBeamDamage(globals, attacker, damage) {
    var origin = attacker.get("origin", [0, 0, 0])
    var entities = Engine.findRadius(origin, damage + 40)
    if (entities == null) return

    for (other in entities) {
      if (other.get("takedamage", DamageValues.NO) == DamageValues.NO) continue

      var delta = CombatModule._vectorSub(origin, other.get("origin", [0, 0, 0]))
      var points = 0.5 * CombatModule._vectorLength(delta)
      if (points < 0) points = 0
      points = damage - points
      if (other == attacker) points = points * 0.5
      if (points <= 0) continue

      if (!CombatModule.canDamage(globals, other, attacker)) continue

      if (other.get("classname", "") == "monster_shambler") {
        CombatModule.tDamage(globals, other, attacker, attacker, points * 0.5)
      } else {
        CombatModule.tDamage(globals, other, attacker, attacker, points)
      }
    }
  }

  static CanDamage(globals, target, inflictor) {
    return CombatModule.canDamage(globals, target, inflictor)
  }

  static monster_death_use(globals, monster) {
    CombatModule.monsterDeathUse(globals, monster)
  }

  static Killed(globals, target, attacker) {
    CombatModule.killed(globals, target, attacker)
  }

  static T_Damage(globals, target, inflictor, attacker, damage) {
    CombatModule.tDamage(globals, target, inflictor, attacker, damage)
  }

  static T_RadiusDamage(globals, inflictor, attacker, damage, ignore) {
    CombatModule.tRadiusDamage(globals, inflictor, attacker, damage, ignore)
  }

  static T_BeamDamage(globals, attacker, damage) {
    CombatModule.tBeamDamage(globals, attacker, damage)
  }
}
