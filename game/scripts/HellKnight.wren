// HellKnight.wren
// Ports the hell knight (death knight) monster from hellknight.qc to native
// Wren gameplay logic.

import "./Engine" for Engine
import "./Globals" for Channels, Attenuations, SolidTypes, MoveTypes, Effects, CombatStyles
import "./Monsters" for MonstersModule
import "./AI" for AIModule
import "./Fight" for FightModule
import "./Subs" for SubsModule
import "./Weapons" for WeaponsModule
import "./Player" for PlayerModule

var _STAND_FRAMES = [
  "stand1",
  "stand2",
  "stand3",
  "stand4",
  "stand5",
  "stand6",
  "stand7",
  "stand8",
  "stand9"
]

var _WALK_FRAMES = [
  "walk1",
  "walk2",
  "walk3",
  "walk4",
  "walk5",
  "walk6",
  "walk7",
  "walk8",
  "walk9",
  "walk10",
  "walk11",
  "walk12",
  "walk13",
  "walk14",
  "walk15",
  "walk16",
  "walk17",
  "walk18",
  "walk19",
  "walk20"
]

var _WALK_SPEEDS = [2, 5, 5, 4, 4, 2, 2, 3, 3, 4, 3, 4, 6, 2, 2, 4, 3, 3, 3, 2]

var _RUN_FRAMES = [
  "run1",
  "run2",
  "run3",
  "run4",
  "run5",
  "run6",
  "run7",
  "run8"
]

var _RUN_SPEEDS = [20, 25, 18, 16, 14, 25, 21, 13]

var _PAIN_FRAMES = ["pain1", "pain2", "pain3", "pain4", "pain5"]

var _DEATH_A_FRAMES = [
  "death1",
  "death2",
  "death3",
  "death4",
  "death5",
  "death6",
  "death7",
  "death8",
  "death9",
  "death10",
  "death11",
  "death12"
]

var _DEATH_A_ACTIONS = [
  Fn.new { |g, m| AIModule.ai_forward(g, m, 10) },
  Fn.new { |g, m| AIModule.ai_forward(g, m, 8) },
  Fn.new { |g, m|
    m.set("solid", SolidTypes.NOT)
    AIModule.ai_forward(g, m, 7)
  },
  null,
  null,
  null,
  null,
  null,
  Fn.new { |g, m| AIModule.ai_forward(g, m, 10) },
  Fn.new { |g, m| AIModule.ai_forward(g, m, 11) },
  null,
  null
]

var _DEATH_B_FRAMES = [
  "deathb1",
  "deathb2",
  "deathb3",
  "deathb4",
  "deathb5",
  "deathb6",
  "deathb7",
  "deathb8",
  "deathb9"
]

var _DEATH_B_ACTIONS = [
  null,
  null,
  Fn.new { |g, m| m.set("solid", SolidTypes.NOT) },
  null,
  null,
  null,
  null,
  null,
  null
]

var _MAGIC_A_FRAMES = [
  "magica1",
  "magica2",
  "magica3",
  "magica4",
  "magica5",
  "magica6",
  "magica7",
  "magica8",
  "magica9",
  "magica10",
  "magica11",
  "magica12",
  "magica13",
  "magica14"
]

var _MAGIC_B_FRAMES = [
  "magicb1",
  "magicb2",
  "magicb3",
  "magicb4",
  "magicb5",
  "magicb6",
  "magicb7",
  "magicb8",
  "magicb9",
  "magicb10",
  "magicb11",
  "magicb12",
  "magicb13"
]

var _MAGIC_C_FRAMES = [
  "magicc1",
  "magicc2",
  "magicc3",
  "magicc4",
  "magicc5",
  "magicc6",
  "magicc7",
  "magicc8",
  "magicc9",
  "magicc10",
  "magicc11"
]

var _CHAR_A_FRAMES = [
  "char_a1",
  "char_a2",
  "char_a3",
  "char_a4",
  "char_a5",
  "char_a6",
  "char_a7",
  "char_a8",
  "char_a9",
  "char_a10",
  "char_a11",
  "char_a12",
  "char_a13",
  "char_a14",
  "char_a15",
  "char_a16"
]

var _CHAR_A_SPEEDS = [20, 25, 18, 16, 14, 20, 21, 13, 20, 20, 18, 16, 14, 25, 21, 13]

var _CHAR_B_FRAMES = [
  "char_b1",
  "char_b2",
  "char_b3",
  "char_b4",
  "char_b5",
  "char_b6"
]

var _CHAR_B_SPEEDS = [23, 17, 12, 22, 18, 8]

var _SLICE_FRAMES = [
  "slice1",
  "slice2",
  "slice3",
  "slice4",
  "slice5",
  "slice6",
  "slice7",
  "slice8",
  "slice9",
  "slice10"
]

var _SLICE_SPEEDS = [9, 6, 13, 4, 7, 15, 8, 2, 0, 3]

var _SMASH_FRAMES = [
  "smash1",
  "smash2",
  "smash3",
  "smash4",
  "smash5",
  "smash6",
  "smash7",
  "smash8",
  "smash9",
  "smash10",
  "smash11"
]

var _SMASH_SPEEDS = [1, 13, 9, 11, 10, 7, 12, 2, 3, 0, 0]

var _WIDE_ATTACK_FRAMES = [
  "w_attack1",
  "w_attack2",
  "w_attack3",
  "w_attack4",
  "w_attack5",
  "w_attack6",
  "w_attack7",
  "w_attack8",
  "w_attack9",
  "w_attack10",
  "w_attack11",
  "w_attack12",
  "w_attack13",
  "w_attack14",
  "w_attack15",
  "w_attack16",
  "w_attack17",
  "w_attack18",
  "w_attack19",
  "w_attack20",
  "w_attack21",
  "w_attack22"
]

var _WIDE_ATTACK_SPEEDS = [2, 0, 0, 0, 0, 0, 1, 4, 5, 3, 2, 2, 0, 0, 0, 1, 1, 3, 4, 6, 7, 3]

class HellKnightModule {
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
    var length = HellKnightModule._vectorLength(v)
    if (length == 0) return [0, 0, 0]
    var inv = 1 / length
    return [v[0] * inv, v[1] * inv, v[2] * inv]
  }

  static _makeVectors(angles) {
    if (angles == null) return null
    var adjusted = [-angles[0], angles[1], angles[2]]
    return Engine.makeVectors(adjusted)
  }

  static _enemyYaw(monster) {
    var enemy = monster.get("enemy", null)
    if (enemy == null) return monster.get("angles", [0, 0, 0])[1]
    var delta = HellKnightModule._vectorSub(enemy.get("origin", [0, 0, 0]), monster.get("origin", [0, 0, 0]))
    return Engine.vectorToAngles(delta)[1]
  }

  static _setFrame(globals, monster, frame, nextThink, delay) {
    MonstersModule.setFrame(globals, monster, frame, nextThink, delay)
  }

  static _loopSequence(globals, monster, frames, indexField, nextFunction, actionFn) {
    if (monster == null) return
    if (frames == null || frames.count == 0) return

    var index = monster.get(indexField, 0)
    if (index < 0 || index >= frames.count) index = 0

    HellKnightModule._setFrame(globals, monster, frames[index], nextFunction, 0.1)
    if (actionFn != null) actionFn.call(index)

    if (index >= frames.count - 1) {
      monster.set(indexField, 0)
    } else {
      monster.set(indexField, index + 1)
    }
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
    HellKnightModule._setFrame(globals, monster, frames[index], nextName, delay)

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

  static _playIdle(globals, monster, index) {
    if (index == 0 && Engine.random() < 0.2) {
      Engine.playSound(monster, Channels.VOICE, "hknight/idle.wav", 1, Attenuations.IDLE)
    }
  }

  static _walkAction(globals, monster, index) {
    HellKnightModule._playIdle(globals, monster, index)
    if (index < 0 || index >= _WALK_SPEEDS.count) return
    AIModule.ai_walk(globals, monster, _WALK_SPEEDS[index])
  }

  static _runAction(globals, monster, index) {
    HellKnightModule._playIdle(globals, monster, index)
    if (index < 0 || index >= _RUN_SPEEDS.count) return
    AIModule.ai_run(globals, monster, _RUN_SPEEDS[index])
    if (index == 0) {
      HellKnightModule._checkForCharge(globals, monster)
    }
  }

  static _faceEnemy(globals, monster) {
    FightModule.ai_face(globals, monster, HellKnightModule._enemyYaw(monster))
  }

  static _enemyVisible(globals, monster) {
    var enemy = monster.get("enemy", null)
    if (enemy == null) return false
    return AIModule.visible(globals, monster, enemy)
  }

  static _checkForCharge(globals, monster) {
    if (!HellKnightModule._enemyVisible(globals, monster)) return
    if (globals.time < monster.get("attack_finished", 0.0)) return

    var enemy = monster.get("enemy", null)
    if (enemy == null) return

    var origin = monster.get("origin", [0, 0, 0])
    var enemyOrigin = enemy.get("origin", [0, 0, 0])
    if ((enemyOrigin[2] - origin[2]).abs > 20) return

    var delta = HellKnightModule._vectorSub(enemyOrigin, origin)
    if (HellKnightModule._vectorLength(delta) < 80) return

    SubsModule.SUB_AttackFinished(globals, monster, 2.0)
    HellKnightModule.hknight_char_a1(globals, monster)
  }

  static _checkContinueCharge(globals, monster) {
    if (globals.time > monster.get("attack_finished", 0.0)) {
      SubsModule.SUB_AttackFinished(globals, monster, 3.0)
      HellKnightModule.hknight_run1(globals, monster)
      return
    }

    if (Engine.random() > 0.5) {
      Engine.playSound(monster, Channels.WEAPON, "knight/sword2.wav", 1, Attenuations.NORMAL)
    } else {
      Engine.playSound(monster, Channels.WEAPON, "knight/sword1.wav", 1, Attenuations.NORMAL)
    }
  }

  static _hellknightShot(globals, monster, offset) {
    var enemy = monster.get("enemy", null)
    if (enemy == null) return

    var enemyOrigin = enemy.get("origin", [0, 0, 0])
    var origin = monster.get("origin", [0, 0, 0])
    var delta = HellKnightModule._vectorSub(enemyOrigin, origin)
    var angles = Engine.vectorToAngles(delta)
    angles[1] = angles[1] + offset * 6

    var vectors = HellKnightModule._makeVectors(angles)
    if (vectors == null || !vectors.containsKey("forward")) return
    var forward = vectors["forward"]

    var mins = monster.get("mins", [0, 0, 0])
    var maxs = monster.get("maxs", [0, 0, 0])
    var size = HellKnightModule._vectorSub(maxs, mins)
    var center = HellKnightModule._vectorAdd(origin, HellKnightModule._vectorAdd(mins, HellKnightModule._vectorScale(size, 0.5)))
    var launchOrigin = HellKnightModule._vectorAdd(center, HellKnightModule._vectorScale(forward, 20))

    var dir = HellKnightModule._vectorNormalize(forward)
    dir[2] = -dir[2] + (Engine.random() - 0.5) * 0.1
    dir = HellKnightModule._vectorNormalize(dir)

    var missile = WeaponsModule.launch_spike(globals, monster, launchOrigin, dir)
    if (missile == null) return

    missile.set("classname", "knight_spike")
    Engine.setModel(missile, "progs/k_spike.mdl")
    Engine.setSize(missile, [0, 0, 0], [0, 0, 0])
    missile.set("velocity", HellKnightModule._vectorScale(dir, 300))
    missile.set("angles", Engine.vectorToAngles(missile.get("velocity", [0, 0, 0])))

    if (Engine.cvar("pr_checkextension") != 0 && Engine.checkExtension("EX_EXTENDED_EF")) {
      var effects = missile.get("effects", 0)
      effects = Engine.bitOr(effects, Effects.CANDLELIGHT)
      missile.set("effects", effects)
    }

    Engine.playSound(monster, Channels.WEAPON, "hknight/attack1.wav", 1, Attenuations.NORMAL)
  }

  static _applyCharge(globals, monster, speed, doMelee) {
    FightModule.ai_charge(globals, monster, speed)
    if (doMelee) {
      FightModule.ai_melee(globals, monster)
    }
  }

  static hknight_stand1(globals, monster) {
    HellKnightModule._loopSequence(globals, monster, _STAND_FRAMES, "_hellKnightStandIndex", "HellKnightModule.hknight_stand1", Fn.new { |i|
      AIModule.ai_stand(globals, monster)
    })
  }

  static hknight_walk1(globals, monster) {
    HellKnightModule._loopSequence(globals, monster, _WALK_FRAMES, "_hellKnightWalkIndex", "HellKnightModule.hknight_walk1", Fn.new { |i|
      HellKnightModule._walkAction(globals, monster, i)
    })
  }

  static hknight_run1(globals, monster) {
    HellKnightModule._loopSequence(globals, monster, _RUN_FRAMES, "_hellKnightRunIndex", "HellKnightModule.hknight_run1", Fn.new { |i|
      HellKnightModule._runAction(globals, monster, i)
    })
  }

  static _magicAOffsets() {
    return {
      6: -2,
      7: -1,
      8: 0,
      9: 1,
      10: 2,
      11: 3
    }
  }

  static _magicBOffsets() {
    return {
      6: -2,
      7: -1,
      8: 0,
      9: 1,
      10: 2,
      11: 3
    }
  }

  static _magicCOffsets() {
    return {
      5: -2,
      6: -1,
      7: 0,
      8: 1,
      9: 2,
      10: 3
    }
  }

  static hknight_magica1(globals, monster) {
    monster.set("_hellKnightMagicAIndex", 0)
    var offsets = HellKnightModule._magicAOffsets()
    var actions = []
    for (i in 0..._MAGIC_A_FRAMES.count) {
      if (offsets.containsKey(i)) {
        var off = offsets[i]
        actions.add(Fn.new { |g, m| HellKnightModule._hellknightShot(g, m, off) })
      } else {
        actions.add(Fn.new { |g, m| HellKnightModule._faceEnemy(g, m) })
      }
    }
    HellKnightModule._advanceSequence(globals, monster, _MAGIC_A_FRAMES, actions, "_hellKnightMagicAIndex", "HellKnightModule.hknight_magicaAdvance", "HellKnightModule.hknight_run1")
  }

  static hknight_magicaAdvance(globals, monster) {
    HellKnightModule.hknight_magica1(globals, monster)
  }

  static hknight_magicb1(globals, monster) {
    monster.set("_hellKnightMagicBIndex", 0)
    var offsets = HellKnightModule._magicBOffsets()
    var actions = []
    for (i in 0..._MAGIC_B_FRAMES.count) {
      if (offsets.containsKey(i)) {
        var off = offsets[i]
        actions.add(Fn.new { |g, m| HellKnightModule._hellknightShot(g, m, off) })
      } else {
        actions.add(Fn.new { |g, m| HellKnightModule._faceEnemy(g, m) })
      }
    }
    HellKnightModule._advanceSequence(globals, monster, _MAGIC_B_FRAMES, actions, "_hellKnightMagicBIndex", "HellKnightModule.hknight_magicbAdvance", "HellKnightModule.hknight_run1")
  }

  static hknight_magicbAdvance(globals, monster) {
    HellKnightModule.hknight_magicb1(globals, monster)
  }

  static hknight_magicc1(globals, monster) {
    monster.set("_hellKnightMagicCIndex", 0)
    var offsets = HellKnightModule._magicCOffsets()
    var actions = []
    for (i in 0..._MAGIC_C_FRAMES.count) {
      if (offsets.containsKey(i)) {
        var off = offsets[i]
        actions.add(Fn.new { |g, m| HellKnightModule._hellknightShot(g, m, off) })
      } else {
        actions.add(Fn.new { |g, m| HellKnightModule._faceEnemy(g, m) })
      }
    }
    HellKnightModule._advanceSequence(globals, monster, _MAGIC_C_FRAMES, actions, "_hellKnightMagicCIndex", "HellKnightModule.hknight_magiccAdvance", "HellKnightModule.hknight_run1")
  }

  static hknight_magiccAdvance(globals, monster) {
    HellKnightModule.hknight_magicc1(globals, monster)
  }

  static hknight_char_a1(globals, monster) {
    monster.set("_hellKnightCharAIndex", 0)
    HellKnightModule._hknight_charAAdvance(globals, monster)
  }

  static _hknight_charAAdvance(globals, monster) {
    var actions = []
    for (i in 0..._CHAR_A_FRAMES.count) {
      var speed = _CHAR_A_SPEEDS[i]
      var doMelee = (i >= 5 && i <= 10)
      actions.add(Fn.new { |g, m| HellKnightModule._applyCharge(g, m, speed, doMelee) })
    }
    HellKnightModule._advanceSequence(globals, monster, _CHAR_A_FRAMES, actions, "_hellKnightCharAIndex", "HellKnightModule._hknight_charAAdvance", "HellKnightModule.hknight_run1")
  }

  static hknight_char_b1(globals, monster) {
    monster.set("_hellKnightCharBIndex", 0)
    HellKnightModule._hknight_charBAdvance(globals, monster)
  }

  static _hknight_charBAdvance(globals, monster) {
    var actions = []
    for (i in 0..._CHAR_B_FRAMES.count) {
      var speed = _CHAR_B_SPEEDS[i]
      var action = Fn.new { |g, m|
        if (i == 0) HellKnightModule._checkContinueCharge(g, m)
        HellKnightModule._applyCharge(g, m, speed, true)
      }
      actions.add(action)
    }
    HellKnightModule._advanceSequence(globals, monster, _CHAR_B_FRAMES, actions, "_hellKnightCharBIndex", "HellKnightModule._hknight_charBAdvance", "HellKnightModule.hknight_char_b1")
  }

  static hknight_slice1(globals, monster) {
    monster.set("_hellKnightSliceIndex", 0)
    HellKnightModule._hknight_sliceAdvance(globals, monster)
  }

  static _hknight_sliceAdvance(globals, monster) {
    var actions = []
    for (i in 0..._SLICE_FRAMES.count) {
      var speed = _SLICE_SPEEDS[i]
      var doMelee = (i >= 4 && i <= 8)
      actions.add(Fn.new { |g, m| HellKnightModule._applyCharge(g, m, speed, doMelee) })
    }
    HellKnightModule._advanceSequence(globals, monster, _SLICE_FRAMES, actions, "_hellKnightSliceIndex", "HellKnightModule._hknight_sliceAdvance", "HellKnightModule.hknight_run1")
  }

  static hknight_smash1(globals, monster) {
    monster.set("_hellKnightSmashIndex", 0)
    HellKnightModule._hknight_smashAdvance(globals, monster)
  }

  static _hknight_smashAdvance(globals, monster) {
    var actions = []
    for (i in 0..._SMASH_FRAMES.count) {
      var speed = _SMASH_SPEEDS[i]
      var doMelee = (i >= 4 && i <= 8)
      actions.add(Fn.new { |g, m| HellKnightModule._applyCharge(g, m, speed, doMelee) })
    }
    HellKnightModule._advanceSequence(globals, monster, _SMASH_FRAMES, actions, "_hellKnightSmashIndex", "HellKnightModule._hknight_smashAdvance", "HellKnightModule.hknight_run1")
  }

  static hknight_watk1(globals, monster) {
    monster.set("_hellKnightWideIndex", 0)
    HellKnightModule._hknight_wideAdvance(globals, monster)
  }

  static _hknight_wideAdvance(globals, monster) {
    var actions = []
    for (i in 0..._WIDE_ATTACK_FRAMES.count) {
      var speed = _WIDE_ATTACK_SPEEDS[i]
      var doMelee = (i >= 3 && i <= 5) || (i >= 10 && i <= 12) || (i >= 16 && i <= 18)
      actions.add(Fn.new { |g, m| HellKnightModule._applyCharge(g, m, speed, doMelee) })
    }
    HellKnightModule._advanceSequence(globals, monster, _WIDE_ATTACK_FRAMES, actions, "_hellKnightWideIndex", "HellKnightModule._hknight_wideAdvance", "HellKnightModule.hknight_run1")
  }

  static hknight_pain(globals, monster, attacker, damage) {
    if (monster.get("pain_finished", 0.0) > globals.time) return

    Engine.playSound(monster, Channels.VOICE, "hknight/pain1.wav", 1, Attenuations.NORMAL)

    if (globals.time - monster.get("pain_finished", 0.0) > 5.0) {
      monster.set("pain_finished", globals.time + 1.0)
      HellKnightModule.hknight_pain1(globals, monster)
      return
    }

    if (Engine.random() * 30 > damage) return

    monster.set("pain_finished", globals.time + 1.0)
    HellKnightModule.hknight_pain1(globals, monster)
  }

  static hknight_pain1(globals, monster) {
    monster.set("_hellKnightPainIndex", 0)
    HellKnightModule._hknight_painAdvance(globals, monster)
  }

  static _hknight_painAdvance(globals, monster) {
    var actions = [
      null,
      null,
      null,
      null,
      Fn.new { |g, m| HellKnightModule.hknight_run1(g, m) }
    ]
    HellKnightModule._advanceSequence(globals, monster, _PAIN_FRAMES, actions, "_hellKnightPainIndex", "HellKnightModule._hknight_painAdvance", "HellKnightModule.hknight_run1")
  }

  static hknight_die(globals, monster) {
    var health = monster.get("health", 0)
    if (health < -40) {
      Engine.playSound(monster, Channels.VOICE, "player/udeath.wav", 1, Attenuations.NORMAL)
      PlayerModule.ThrowHead(globals, monster, "progs/h_hellkn.mdl", health)
      PlayerModule.ThrowGib(globals, monster, "progs/gib1.mdl", health)
      PlayerModule.ThrowGib(globals, monster, "progs/gib2.mdl", health)
      PlayerModule.ThrowGib(globals, monster, "progs/gib3.mdl", health)
      return
    }

    Engine.playSound(monster, Channels.VOICE, "hknight/death1.wav", 1, Attenuations.NORMAL)
    if (Engine.random() > 0.5) {
      monster.set("_hellKnightDeathAIndex", 0)
      HellKnightModule._hknight_deathAAdvance(globals, monster)
    } else {
      monster.set("_hellKnightDeathBIndex", 0)
      HellKnightModule._hknight_deathBAdvance(globals, monster)
    }
  }

  static _hknight_deathAAdvance(globals, monster) {
    HellKnightModule._advanceSequence(globals, monster, _DEATH_A_FRAMES, _DEATH_A_ACTIONS, "_hellKnightDeathAIndex", "HellKnightModule._hknight_deathAAdvance", null)
  }

  static _hknight_deathBAdvance(globals, monster) {
    HellKnightModule._advanceSequence(globals, monster, _DEATH_B_FRAMES, _DEATH_B_ACTIONS, "_hellKnightDeathBIndex", "HellKnightModule._hknight_deathBAdvance", null)
  }

  static hknight_melee(globals, monster) {
    var cycle = monster.get("_hellKnightMeleeCycle", 0) + 1
    Engine.playSound(monster, Channels.WEAPON, "hknight/slash1.wav", 1, Attenuations.NORMAL)
    if (cycle == 1) {
      HellKnightModule.hknight_slice1(globals, monster)
    } else if (cycle == 2) {
      HellKnightModule.hknight_smash1(globals, monster)
    } else {
      HellKnightModule.hknight_watk1(globals, monster)
      cycle = 0
    }
    monster.set("_hellKnightMeleeCycle", cycle)
  }

  static monster_hell_knight(globals, monster) {
    if (globals.deathmatch != 0) {
      Engine.removeEntity(monster)
      return
    }

    Engine.precacheModel("progs/hknight.mdl")
    Engine.precacheModel("progs/k_spike.mdl")
    Engine.precacheModel("progs/h_hellkn.mdl")

    Engine.precacheSound("hknight/attack1.wav")
    Engine.precacheSound("hknight/death1.wav")
    Engine.precacheSound("hknight/pain1.wav")
    Engine.precacheSound("hknight/sight1.wav")
    Engine.precacheSound("hknight/hit.wav")
    Engine.precacheSound("hknight/slash1.wav")
    Engine.precacheSound("hknight/idle.wav")
    Engine.precacheSound("hknight/grunt.wav")
    Engine.precacheSound("knight/sword1.wav")
    Engine.precacheSound("knight/sword2.wav")
    Engine.precacheSound("player/udeath.wav")

    monster.set("solid", SolidTypes.SLIDEBOX)
    monster.set("movetype", MoveTypes.STEP)
    Engine.setModel(monster, "progs/hknight.mdl")

    monster.set("noise", "hknight/sight1.wav")
    monster.set("netname", "$qc_death_knight")
    monster.set("killstring", "$qc_ks_deathknight")

    Engine.setSize(monster, [-16, -16, -24], [16, 16, 40])
    monster.set("health", 250)
    monster.set("max_health", 250)

    monster.set("th_stand", "HellKnightModule.hknight_stand1")
    monster.set("th_walk", "HellKnightModule.hknight_walk1")
    monster.set("th_run", "HellKnightModule.hknight_run1")
    monster.set("th_melee", "HellKnightModule.hknight_melee")
    monster.set("th_missile", "HellKnightModule.hknight_magicc1")
    monster.set("th_pain", "HellKnightModule.hknight_pain")
    monster.set("th_die", "HellKnightModule.hknight_die")

    monster.set("allowPathFind", true)
    monster.set("combat_style", CombatStyles.MIXED)

    monster.set("_hellKnightStandIndex", 0)
    monster.set("_hellKnightWalkIndex", 0)
    monster.set("_hellKnightRunIndex", 0)
    monster.set("_hellKnightMagicAIndex", 0)
    monster.set("_hellKnightMagicBIndex", 0)
    monster.set("_hellKnightMagicCIndex", 0)
    monster.set("_hellKnightCharAIndex", 0)
    monster.set("_hellKnightCharBIndex", 0)
    monster.set("_hellKnightSliceIndex", 0)
    monster.set("_hellKnightSmashIndex", 0)
    monster.set("_hellKnightWideIndex", 0)
    monster.set("_hellKnightPainIndex", 0)
    monster.set("_hellKnightDeathAIndex", 0)
    monster.set("_hellKnightDeathBIndex", 0)
    monster.set("_hellKnightMeleeCycle", 0)

    MonstersModule.walkmonster_start(globals, monster)
  }
}
