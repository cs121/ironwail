// Scrag.wren
// Ports the scrag (wizard) monster so that its behavior can execute entirely
// inside the Wren gameplay runtime.

import "./Engine" for Engine
import "./Globals" for Channels, Attenuations, SolidTypes, MoveTypes, Effects
import "./Globals" for CombatStyles, DamageValues, PlayerFlags, AttackStates, Ranges
import "./Monsters" for MonstersModule
import "./AI" for AIModule
import "./Fight" for FightModule
import "./Subs" for SubsModule
import "./Weapons" for WeaponsModule
import "./Player" for PlayerModule

var _HOVER_FRAMES = [
  "hover1",
  "hover2",
  "hover3",
  "hover4",
  "hover5",
  "hover6",
  "hover7",
  "hover8"
]

var _RUN_FRAMES = [
  "fly1",
  "fly2",
  "fly3",
  "fly4",
  "fly5",
  "fly6",
  "fly7",
  "fly8",
  "fly9",
  "fly10",
  "fly11",
  "fly12",
  "fly13",
  "fly14"
]

var _ATTACK_FRAMES = [
  "magatt1",
  "magatt2",
  "magatt3",
  "magatt4",
  "magatt5",
  "magatt6",
  "magatt5",
  "magatt4",
  "magatt3",
  "magatt2"
]

var _PAIN_FRAMES = ["pain1", "pain2", "pain3", "pain4"]

var _DEATH_FRAMES = [
  "death1",
  "death2",
  "death3",
  "death4",
  "death5",
  "death6",
  "death7",
  "death8"
]

class ScragModule {
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
    var length = ScragModule._vectorLength(v)
    if (length == 0) return [0, 0, 0]
    var inv = 1 / length
    return [v[0] * inv, v[1] * inv, v[2] * inv]
  }

  static _makeVectors(angles) {
    var adjusted = [-angles[0], angles[1], angles[2]]
    return Engine.makeVectors(adjusted)
  }

  static _forwardVector(monster) {
    var vectors = ScragModule._makeVectors(monster.get("angles", [0, 0, 0]))
    if (vectors == null || !vectors.containsKey("forward")) return [1, 0, 0]
    return vectors["forward"]
  }

  static _rightVector(monster) {
    var vectors = ScragModule._makeVectors(monster.get("angles", [0, 0, 0]))
    if (vectors == null || !vectors.containsKey("right")) return [0, 1, 0]
    return vectors["right"]
  }

  static _upVector(monster) {
    var vectors = ScragModule._makeVectors(monster.get("angles", [0, 0, 0]))
    if (vectors == null || !vectors.containsKey("up")) return [0, 0, 1]
    return vectors["up"]
  }

  static _enemyYaw(monster) {
    var enemy = monster.get("enemy", null)
    if (enemy == null) return monster.get("angles", [0, 0, 0])[1]
    var delta = ScragModule._vectorSub(enemy.get("origin", [0, 0, 0]), monster.get("origin", [0, 0, 0]))
    var angles = Engine.vectorToAngles(delta)
    return angles[1]
  }

  static _setFrame(globals, monster, frame, nextFunction, delay) {
    MonstersModule.setFrame(globals, monster, frame, nextFunction, delay)
  }

  static _loopSequence(globals, monster, frames, indexField, nextFunction, actionFn) {
    if (monster == null) return
    if (frames == null || frames.count == 0) return

    var index = monster.get(indexField, 0)
    if (index < 0 || index >= frames.count) index = 0

    ScragModule._setFrame(globals, monster, frames[index], nextFunction, 0.1)

    if (actionFn != null) actionFn.call(index)

    index = index + 1
    if (index >= frames.count) index = 0
    monster.set(indexField, index)
  }

  static _advanceSequence(globals, monster, frames, actions, indexField, advanceFunction, completionFunction) {
    if (monster == null) return
    if (frames == null || frames.count == 0) return

    var index = monster.get(indexField, 0)
    if (index < 0 || index >= frames.count) index = 0

    var nextName = advanceFunction
    if (index >= frames.count - 1) {
      nextName = completionFunction
    }

    var delay = nextName == null ? null : 0.1
    ScragModule._setFrame(globals, monster, frames[index], nextName, delay)

    if (actions != null && index < actions.count) {
      var action = actions[index]
      if (action != null) action.call(globals, monster)
    }

    if (index >= frames.count - 1) {
      monster.set(indexField, 0)
    } else {
      monster.set(indexField, index + 1)
    }
  }

  static _idleSound(globals, monster) {
    var nextIdle = monster.get("_wizIdleTime", 0.0)
    if (nextIdle >= globals.time) return

    monster.set("_wizIdleTime", globals.time + 2.0)
    var roll = Engine.random() * 5
    if (roll > 4.5) {
      Engine.playSound(monster, Channels.VOICE, "wizard/widle1.wav", 1, Attenuations.IDLE)
    }
    if (roll < 1.5) {
      Engine.playSound(monster, Channels.VOICE, "wizard/widle2.wav", 1, Attenuations.IDLE)
    }
  }

  static _faceEnemy(globals, monster) {
    FightModule.ai_face(globals, monster, ScragModule._enemyYaw(monster))
  }

  static wiz_stand1(globals, monster) {
    ScragModule._loopSequence(globals, monster, _HOVER_FRAMES, "_wizStandIndex", "ScragModule.wiz_stand1", Fn.new { |index|
      AIModule.ai_stand(globals, monster)
    })
  }

  static wiz_walk1(globals, monster) {
    ScragModule._loopSequence(globals, monster, _HOVER_FRAMES, "_wizWalkIndex", "ScragModule.wiz_walk1", Fn.new { |index|
      if (index == 0) ScragModule._idleSound(globals, monster)
      AIModule.ai_walk(globals, monster, 8)
    })
  }

  static wiz_side1(globals, monster) {
    ScragModule._loopSequence(globals, monster, _HOVER_FRAMES, "_wizSideIndex", "ScragModule.wiz_side1", Fn.new { |index|
      if (index == 0) ScragModule._idleSound(globals, monster)
      AIModule.ai_run(globals, monster, 8)
    })
  }

  static wiz_run1(globals, monster) {
    ScragModule._loopSequence(globals, monster, _RUN_FRAMES, "_wizRunIndex", "ScragModule.wiz_run1", Fn.new { |index|
      if (index == 0) ScragModule._idleSound(globals, monster)
      AIModule.ai_run(globals, monster, 16)
    })
  }

  static _wizAttackActions() {
    return [
      Fn.new { |globals, monster|
        ScragModule._faceEnemy(globals, monster)
        ScragModule._startFast(globals, monster)
      },
      Fn.new { |globals, monster| ScragModule._faceEnemy(globals, monster) },
      Fn.new { |globals, monster| ScragModule._faceEnemy(globals, monster) },
      Fn.new { |globals, monster| ScragModule._faceEnemy(globals, monster) },
      Fn.new { |globals, monster| ScragModule._faceEnemy(globals, monster) },
      Fn.new { |globals, monster| ScragModule._faceEnemy(globals, monster) },
      Fn.new { |globals, monster| ScragModule._faceEnemy(globals, monster) },
      Fn.new { |globals, monster| ScragModule._faceEnemy(globals, monster) },
      Fn.new { |globals, monster| ScragModule._faceEnemy(globals, monster) },
      Fn.new { |globals, monster|
        ScragModule._faceEnemy(globals, monster)
        SubsModule.SUB_AttackFinished(globals, monster, 2.0)
        ScragModule.wizardAttackFinished(globals, monster)
      }
    ]
  }

  static _wiz_attackAdvance(globals, monster) {
    ScragModule._advanceSequence(globals, monster, _ATTACK_FRAMES, ScragModule._wizAttackActions(), "_wizAttackIndex", "ScragModule._wiz_attackAdvance", null)
  }

  static wiz_fast1(globals, monster) {
    monster.set("_wizAttackIndex", 0)
    ScragModule._wiz_attackAdvance(globals, monster)
  }

  static _startFast(globals, monster) {
    if (monster == null) return

    Engine.playSound(monster, Channels.WEAPON, "wizard/wattack.wav", 1, Attenuations.NORMAL)

    var origin = monster.get("origin", [0, 0, 0])
    var base = ScragModule._vectorAdd(origin, [0, 0, 30])
    var forward = ScragModule._forwardVector(monster)
    var right = ScragModule._rightVector(monster)

    var firstOrigin = ScragModule._vectorAdd(base, ScragModule._vectorAdd(ScragModule._vectorScale(forward, 14), ScragModule._vectorScale(right, 14)))
    var secondOrigin = ScragModule._vectorAdd(base, ScragModule._vectorAdd(ScragModule._vectorScale(forward, 14), ScragModule._vectorScale(right, -14)))

    ScragModule._spawnFastTimer(globals, monster, firstOrigin, right, 0.8)
    ScragModule._spawnFastTimer(globals, monster, secondOrigin, ScragModule._vectorScale(right, -1), 0.3)
  }

  static _spawnFastTimer(globals, monster, origin, movedir, delay) {
    var missile = Engine.spawnEntity()
    missile.set("owner", monster)
    missile.set("enemy", monster.get("enemy", null))
    missile.set("movedir", movedir)
    Engine.setSize(missile, [0, 0, 0], [0, 0, 0])
    Engine.setOrigin(missile, origin)
    missile.set("think", "ScragModule.wiz_fastFire")
    missile.set("nextthink", globals.time + delay)
    Engine.scheduleThink(missile, "ScragModule.wiz_fastFire", delay)
  }

  static wiz_fastFire(globals, missile) {
    if (missile == null) return

    var owner = missile.get("owner", null)
    var enemy = missile.get("enemy", null)
    if (owner == null) {
      Engine.removeEntity(missile)
      return
    }

    if (owner.get("health", 0) > 0) {
      var effects = owner.get("effects", 0)
      effects = Engine.bitOr(effects, Effects.MUZZLEFLASH)
      owner.set("effects", effects)

      if (enemy != null) {
        var targetOrigin = enemy.get("origin", [0, 0, 0])
        var movedir = missile.get("movedir", [0, 0, 0])
        var adjustedTarget = ScragModule._vectorSub(targetOrigin, ScragModule._vectorScale(movedir, 13))
        var direction = ScragModule._vectorNormalize(ScragModule._vectorSub(adjustedTarget, missile.get("origin", [0, 0, 0])))

        var spike = WeaponsModule.launch_spike(globals, owner, missile.get("origin", [0, 0, 0]), direction)
        if (spike != null) {
          spike.set("velocity", ScragModule._vectorScale(direction, 600))
          spike.set("owner", owner)
          spike.set("classname", "wizard_spike")
          Engine.setModel(spike, "progs/w_spike.mdl")
        }

        Engine.playSound(owner, Channels.WEAPON, "wizard/wattack.wav", 1, Attenuations.NORMAL)
      }
    }

    Engine.removeEntity(missile)
  }

  static _painAdvance(globals, monster) {
    ScragModule._advanceSequence(globals, monster, _PAIN_FRAMES, null, "_wizPainIndex", "ScragModule._painAdvance", "ScragModule.wiz_run1")
  }

  static wiz_pain(globals, monster, attacker, damage) {
    if (monster == null) return

    Engine.playSound(monster, Channels.VOICE, "wizard/wpain.wav", 1, Attenuations.NORMAL)
    if (Engine.random() * 70 > damage) return

    monster.set("_wizPainIndex", 0)
    ScragModule._painAdvance(globals, monster)
  }

  static _deathActions() {
    return [
      Fn.new { |globals, monster|
        var velocity = [
          -200 + 400 * Engine.random(),
          -200 + 400 * Engine.random(),
          100 + 100 * Engine.random()
        ]
        monster.set("velocity", velocity)
        var flags = monster.get("flags", 0)
        flags = flags - Engine.bitAnd(flags, PlayerFlags.ONGROUND)
        monster.set("flags", flags)
        Engine.playSound(monster, Channels.VOICE, "wizard/wdeath.wav", 1, Attenuations.NORMAL)
      },
      null,
      Fn.new { |globals, monster| monster.set("solid", SolidTypes.NOT) },
      null,
      null,
      null,
      null,
      null
    ]
  }

  static _deathAdvance(globals, monster) {
    ScragModule._advanceSequence(globals, monster, _DEATH_FRAMES, ScragModule._deathActions(), "_wizDeathIndex", "ScragModule._deathAdvance", null)
  }

  static wiz_die(globals, monster) {
    if (monster == null) return

    var health = monster.get("health", 0)
    if (health < -40) {
      Engine.playSound(monster, Channels.VOICE, "player/udeath.wav", 1, Attenuations.NORMAL)
      PlayerModule.ThrowHead(globals, monster, "progs/h_wizard.mdl", health)
      PlayerModule.ThrowGib(globals, monster, "progs/gib2.mdl", health)
      PlayerModule.ThrowGib(globals, monster, "progs/gib2.mdl", health)
      PlayerModule.ThrowGib(globals, monster, "progs/gib2.mdl", health)
      return
    }

    monster.set("_wizDeathIndex", 0)
    ScragModule._deathAdvance(globals, monster)
  }

  static wizardAttackFinished(globals, monster) {
    if (monster == null) return

    var enemy = monster.get("enemy", null)
    var range = enemy == null ? Ranges.FAR : AIModule.range(globals, monster, enemy)
    var visible = enemy == null ? false : AIModule.visible(globals, monster, enemy)

    if (range >= Ranges.MID || !visible) {
      monster.set("attack_state", AttackStates.STRAIGHT)
      monster.set("_wizRunIndex", 0)
      monster.set("think", "ScragModule.wiz_run1")
      monster.set("nextthink", globals.time + 0.1)
      Engine.scheduleThink(monster, "ScragModule.wiz_run1", 0.1)
    } else {
      monster.set("attack_state", AttackStates.SLIDING)
      monster.set("_wizSideIndex", 0)
      monster.set("think", "ScragModule.wiz_side1")
      monster.set("nextthink", globals.time + 0.1)
      Engine.scheduleThink(monster, "ScragModule.wiz_side1", 0.1)
    }
  }

  static Wiz_Missile(globals, monster) {
    ScragModule.wiz_fast1(globals, monster)
  }

  static wizardCheckAttack(globals, monster, enemyVisible, enemyRange) {
    if (monster == null) return false

    if (globals.time < monster.get("attack_finished", 0.0)) return false
    if (!enemyVisible) return false

    var enemy = monster.get("enemy", null)
    if (enemy == null) return false

    if (enemyRange == Ranges.FAR) {
      if (monster.get("attack_state", AttackStates.STRAIGHT) != AttackStates.STRAIGHT) {
        monster.set("attack_state", AttackStates.STRAIGHT)
        monster.set("_wizRunIndex", 0)
        ScragModule.wiz_run1(globals, monster)
      }
      return false
    }

    var spot1 = ScragModule._vectorAdd(monster.get("origin", [0, 0, 0]), monster.get("view_ofs", [0, 0, 0]))
    var spot2 = ScragModule._vectorAdd(enemy.get("origin", [0, 0, 0]), enemy.get("view_ofs", [0, 0, 0]))
    var trace = Engine.traceLine(spot1, spot2, true, monster)

    var clearShot = false
    if (trace != null) {
      var inOpen = trace.containsKey("inOpen") ? trace["inOpen"] : false
      var inWater = trace.containsKey("inWater") ? trace["inWater"] : false
      if (!(inOpen && inWater)) {
        if (trace.containsKey("entity") && trace["entity"] == enemy) {
          clearShot = true
        } else if (trace.containsKey("fraction") && trace["fraction"] >= 1) {
          clearShot = true
        }
      }
    }

    if (!clearShot) {
      if (monster.get("attack_state", AttackStates.STRAIGHT) != AttackStates.STRAIGHT) {
        monster.set("attack_state", AttackStates.STRAIGHT)
        monster.set("_wizRunIndex", 0)
        ScragModule.wiz_run1(globals, monster)
      }
      return false
    }

    var chance = 0.0
    if (enemyRange == Ranges.MELEE) chance = 0.9
    else if (enemyRange == Ranges.NEAR) chance = 0.6
    else if (enemyRange == Ranges.MID) chance = 0.2

    if (Engine.random() < chance) {
      monster.set("attack_state", AttackStates.MISSILE)
      return true
    }

    if (enemyRange == Ranges.MID) {
      if (monster.get("attack_state", AttackStates.STRAIGHT) != AttackStates.STRAIGHT) {
        monster.set("attack_state", AttackStates.STRAIGHT)
        monster.set("_wizRunIndex", 0)
        ScragModule.wiz_run1(globals, monster)
      }
    } else {
      if (monster.get("attack_state", AttackStates.SLIDING) != AttackStates.SLIDING) {
        monster.set("attack_state", AttackStates.SLIDING)
        monster.set("_wizSideIndex", 0)
        ScragModule.wiz_side1(globals, monster)
      }
    }

    return false
  }

  static monster_wizard(globals, monster) {
    if (monster == null) return

    if (globals.deathmatch != 0) {
      Engine.removeEntity(monster)
      return
    }

    Engine.precacheModel("progs/wizard.mdl")
    Engine.precacheModel("progs/h_wizard.mdl")
    Engine.precacheModel("progs/w_spike.mdl")

    Engine.precacheSound("wizard/hit.wav")
    Engine.precacheSound("wizard/wattack.wav")
    Engine.precacheSound("wizard/wdeath.wav")
    Engine.precacheSound("wizard/widle1.wav")
    Engine.precacheSound("wizard/widle2.wav")
    Engine.precacheSound("wizard/wpain.wav")
    Engine.precacheSound("wizard/wsight.wav")

    monster.set("solid", SolidTypes.SLIDEBOX)
    monster.set("movetype", MoveTypes.STEP)
    Engine.setModel(monster, "progs/wizard.mdl")

    monster.set("noise", "wizard/wsight.wav")
    monster.set("netname", "$qc_scrag")
    monster.set("killstring", "$qc_ks_scrag")

    Engine.setSize(monster, [-16, -16, -24], [16, 16, 40])
    monster.set("health", 80)
    monster.set("max_health", 80)

    monster.set("th_stand", "ScragModule.wiz_stand1")
    monster.set("th_walk", "ScragModule.wiz_walk1")
    monster.set("th_run", "ScragModule.wiz_run1")
    monster.set("th_missile", "ScragModule.Wiz_Missile")
    monster.set("th_pain", "ScragModule.wiz_pain")
    monster.set("th_die", "ScragModule.wiz_die")
    monster.set("combat_style", CombatStyles.RANGED)

    monster.set("_wizStandIndex", 0)
    monster.set("_wizWalkIndex", 0)
    monster.set("_wizSideIndex", 0)
    monster.set("_wizRunIndex", 0)
    monster.set("_wizAttackIndex", 0)
    monster.set("_wizPainIndex", 0)
    monster.set("_wizDeathIndex", 0)
    monster.set("_wizIdleTime", 0.0)

    MonstersModule.flymonster_start(globals, monster)
  }
}
