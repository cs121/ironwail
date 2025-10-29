// Ogre.wren
// Ports the ogre monster (chainsaw and grenade launcher) from ogre.qc to the
// native Wren gameplay runtime.

import "./Engine" for Engine
import "./Globals" for Channels, Attenuations, SolidTypes, MoveTypes, Effects, CombatStyles, DamageValues
import "./Monsters" for MonstersModule
import "./AI" for AIModule
import "./Fight" for FightModule
import "./Weapons" for WeaponsModule
import "./Combat" for CombatModule
import "./Items" for ItemsModule
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
  "walk16"
]

var _WALK_SPEEDS = [3, 2, 2, 2, 2, 5, 3, 2, 3, 1, 2, 3, 3, 3, 3, 4]

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

var _RUN_SPEEDS = [9, 12, 8, 22, 16, 4, 13, 24]

var _SWING_FRAMES = [
  "swing1",
  "swing2",
  "swing3",
  "swing4",
  "swing5",
  "swing6",
  "swing7",
  "swing8",
  "swing9",
  "swing10",
  "swing11",
  "swing12",
  "swing13",
  "swing14"
]

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
  "smash11",
  "smash12",
  "smash13",
  "smash14"
]

var _SHOOT_FRAMES = [
  "shoot1",
  "shoot2",
  "shoot3",
  "shoot4",
  "shoot5",
  "shoot6"
]

var _PAIN_SHORT_FRAMES = ["pain1", "pain2", "pain3", "pain4", "pain5"]
var _PAIN_B_FRAMES = ["painb1", "painb2", "painb3"]
var _PAIN_C_FRAMES = ["painc1", "painc2", "painc3", "painc4", "painc5", "painc6"]
var _PAIN_D_FRAMES = [
  "paind1",
  "paind2",
  "paind3",
  "paind4",
  "paind5",
  "paind6",
  "paind7",
  "paind8",
  "paind9",
  "paind10",
  "paind11",
  "paind12",
  "paind13",
  "paind14",
  "paind15",
  "paind16"
]

var _PAIN_D_ACTIONS = [
  null,
  Fn.new { |g, m| AIModule.ai_pain(g, m, 10) },
  Fn.new { |g, m| AIModule.ai_pain(g, m, 9) },
  Fn.new { |g, m| AIModule.ai_pain(g, m, 4) },
  null,
  null,
  null,
  null,
  null,
  null,
  null,
  null,
  null,
  null,
  null,
  null
]

var _PAIN_E_FRAMES = [
  "paine1",
  "paine2",
  "paine3",
  "paine4",
  "paine5",
  "paine6",
  "paine7",
  "paine8",
  "paine9",
  "paine10",
  "paine11",
  "paine12",
  "paine13",
  "paine14",
  "paine15"
]

var _PAIN_E_ACTIONS = [
  null,
  Fn.new { |g, m| AIModule.ai_pain(g, m, 10) },
  Fn.new { |g, m| AIModule.ai_pain(g, m, 9) },
  Fn.new { |g, m| AIModule.ai_pain(g, m, 4) },
  null,
  null,
  null,
  null,
  null,
  null,
  null,
  null,
  null,
  null,
  null
]

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
  "death12",
  "death13",
  "death14"
]

var _DEATH_A_ACTIONS = [
  null,
  null,
  Fn.new { |g, m|
    m.set("solid", SolidTypes.NOT)
    m.set("ammo_rockets", 2)
    ItemsModule.DropBackpack(g, m)
  },
  null,
  null,
  null,
  null,
  null,
  null,
  null,
  null,
  null,
  null,
  null
]

var _DEATH_B_FRAMES = [
  "bdeath1",
  "bdeath2",
  "bdeath3",
  "bdeath4",
  "bdeath5",
  "bdeath6",
  "bdeath7",
  "bdeath8",
  "bdeath9",
  "bdeath10"
]

var _DEATH_B_ACTIONS = [
  null,
  Fn.new { |g, m| AIModule.ai_forward(g, m, 5) },
  Fn.new { |g, m|
    m.set("solid", SolidTypes.NOT)
    m.set("ammo_rockets", 2)
    ItemsModule.DropBackpack(g, m)
  },
  Fn.new { |g, m| AIModule.ai_forward(g, m, 1) },
  Fn.new { |g, m| AIModule.ai_forward(g, m, 3) },
  Fn.new { |g, m| AIModule.ai_forward(g, m, 7) },
  Fn.new { |g, m| AIModule.ai_forward(g, m, 25) },
  null,
  null,
  null
]

class OgreModule {
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
    var length = OgreModule._vectorLength(v)
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
    var delta = OgreModule._vectorSub(enemy.get("origin", [0, 0, 0]), monster.get("origin", [0, 0, 0]))
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

    OgreModule._setFrame(globals, monster, frames[index], nextFunction, 0.1)
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
    OgreModule._setFrame(globals, monster, frames[index], nextName, delay)

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

  static _playWalkSound(globals, monster, index) {
    if (index == 2 && Engine.random() < 0.2) {
      Engine.playSound(monster, Channels.VOICE, "ogre/ogidle.wav", 1, Attenuations.IDLE)
    }
    if (index == 5 && Engine.random() < 0.1) {
      Engine.playSound(monster, Channels.VOICE, "ogre/ogdrag.wav", 1, Attenuations.IDLE)
    }
  }

  static _walkAction(globals, monster, index) {
    OgreModule._playWalkSound(globals, monster, index)
    if (index < 0 || index >= _WALK_SPEEDS.count) return
    AIModule.ai_walk(globals, monster, _WALK_SPEEDS[index])
  }

  static _runAction(globals, monster, index) {
    if (index == 0 && Engine.random() < 0.2) {
      Engine.playSound(monster, Channels.VOICE, "ogre/ogidle2.wav", 1, Attenuations.IDLE)
    }
    if (index < 0 || index >= _RUN_SPEEDS.count) return
    AIModule.ai_run(globals, monster, _RUN_SPEEDS[index])
  }

  static _faceEnemy(globals, monster) {
    FightModule.ai_face(globals, monster, OgreModule._enemyYaw(monster))
  }

  static _adjustYaw(monster, amount) {
    var angles = monster.get("angles", [0, 0, 0])
    angles[1] = angles[1] + amount
    monster.set("angles", angles)
  }

  static _extendThink(globals, monster, extra) {
    if (monster == null) return
    var current = monster.get("nextthink", globals.time + 0.1)
    var thinkName = monster.get("think", null)
    monster.set("nextthink", current + extra)
    if (thinkName != null && thinkName != "") {
      var delay = current + extra - globals.time
      if (delay < 0) delay = 0
      Engine.scheduleThink(monster, thinkName, delay)
    }
  }

  static _chainsaw(globals, monster, side) {
    var enemy = monster.get("enemy", null)
    if (enemy == null) return
    if (!CombatModule.CanDamage(globals, enemy, monster)) return

    FightModule.ai_charge(globals, monster, 10)

    var delta = OgreModule._vectorSub(enemy.get("origin", [0, 0, 0]), monster.get("origin", [0, 0, 0]))
    if (OgreModule._vectorLength(delta) > 100) return

    var damage = (Engine.random() + Engine.random() + Engine.random()) * 4
    CombatModule.tDamage(globals, enemy, monster, monster, damage)

    if (side == 0) return

    var angles = monster.get("angles", [0, 0, 0])
    var vectors = OgreModule._makeVectors(angles)
    if (vectors == null || !vectors.containsKey("forward")) return
    var forward = vectors["forward"]
    var right = vectors.containsKey("right") ? vectors["right"] : [1, 0, 0]
    var origin = OgreModule._vectorAdd(monster.get("origin", [0, 0, 0]), OgreModule._vectorScale(forward, 16))
    var velocity
    if (side == 1) {
      velocity = OgreModule._vectorScale(right, (Engine.random() * 2 - 1) * 100)
    } else {
      velocity = OgreModule._vectorScale(right, side)
    }
    WeaponsModule.SpawnMeatSpray(globals, origin, velocity)
  }

  static _ogreGrenadeExplode(globals, grenade) {
    if (grenade == null) return
    var owner = grenade.get("owner", null)
    CombatModule.tRadiusDamage(globals, grenade, owner, 40, globals.world)
    Engine.playSound(grenade, Channels.VOICE, "weapons/r_exp3.wav", 1, Attenuations.NORMAL)
    WeaponsModule._emitExplosion(grenade.get("origin", [0, 0, 0]))
    grenade.set("velocity", [0, 0, 0])
    grenade.set("touch", null)
    WeaponsModule.becomeExplosion(globals, grenade)
  }

  static _ogreGrenadeTouch(globals, grenade, other) {
    if (grenade == null || other == null) return
    if (other == grenade.get("owner", null)) return

    if (other.get("takedamage", DamageValues.NO) == DamageValues.AIM) {
      OgreModule._ogreGrenadeExplode(globals, grenade)
      return
    }

    Engine.playSound(grenade, Channels.VOICE, "weapons/bounce.wav", 1, Attenuations.NORMAL)
    var velocity = grenade.get("velocity", [0, 0, 0])
    if (velocity[0] == 0 && velocity[1] == 0 && velocity[2] == 0) {
      grenade.set("avelocity", [0, 0, 0])
    }
  }

  static _ogreFireGrenade(globals, monster) {
    var enemy = monster.get("enemy", null)
    if (enemy == null) return

    var effects = monster.get("effects", 0)
    effects = Engine.bitOr(effects, Effects.MUZZLEFLASH)
    monster.set("effects", effects)

    Engine.playSound(monster, Channels.WEAPON, "weapons/grenade.wav", 1, Attenuations.NORMAL)

    var missile = Engine.spawnEntity()
    missile.set("classname", "ogre_grenade")
    missile.set("owner", monster)
    missile.set("movetype", MoveTypes.BOUNCE)
    missile.set("solid", SolidTypes.BBOX)

    var direction = OgreModule._vectorNormalize(OgreModule._vectorSub(enemy.get("origin", [0, 0, 0]), monster.get("origin", [0, 0, 0])))
    var velocity = OgreModule._vectorScale(direction, 600)
    velocity[2] = 200

    missile.set("velocity", velocity)
    missile.set("avelocity", [300, 300, 300])
    missile.set("angles", Engine.vectorToAngles(velocity))

    missile.set("touch", "OgreModule.ogreGrenadeTouch")

    if (Engine.cvar("pr_checkextension") != 0 && Engine.checkExtension("EX_EXTENDED_EF")) {
      missile.set("effects", Engine.bitOr(missile.get("effects", 0), Effects.CANDLELIGHT))
    }

    missile.set("think", "OgreModule.ogreGrenadeExplode")
    missile.set("nextthink", globals.time + 2.5)
    Engine.scheduleThink(missile, "OgreModule.ogreGrenadeExplode", 2.5)

    Engine.setModel(missile, "progs/grenade.mdl")
    Engine.setSize(missile, [0, 0, 0], [0, 0, 0])
    Engine.setOrigin(missile, monster.get("origin", [0, 0, 0]))
  }

  static ogreGrenadeExplode(globals, grenade) {
    OgreModule._ogreGrenadeExplode(globals, grenade)
  }

  static ogreGrenadeTouch(globals, grenade, other) {
    OgreModule._ogreGrenadeTouch(globals, grenade, other)
  }

  static _swingActions() {
    return [
      Fn.new { |g, m|
        FightModule.ai_charge(g, m, 11)
        Engine.playSound(m, Channels.WEAPON, "ogre/ogsawatk.wav", 1, Attenuations.NORMAL)
      },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 1) },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 4) },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 13) },
      Fn.new { |g, m|
        FightModule.ai_charge(g, m, 9)
        OgreModule._chainsaw(g, m, 0)
        OgreModule._adjustYaw(m, Engine.random() * 25)
      },
      Fn.new { |g, m|
        OgreModule._chainsaw(g, m, 200)
        OgreModule._adjustYaw(m, Engine.random() * 25)
      },
      Fn.new { |g, m|
        OgreModule._chainsaw(g, m, 0)
        OgreModule._adjustYaw(m, Engine.random() * 25)
      },
      Fn.new { |g, m|
        OgreModule._chainsaw(g, m, 0)
        OgreModule._adjustYaw(m, Engine.random() * 25)
      },
      Fn.new { |g, m|
        OgreModule._chainsaw(g, m, 0)
        OgreModule._adjustYaw(m, Engine.random() * 25)
      },
      Fn.new { |g, m|
        OgreModule._chainsaw(g, m, -200)
        OgreModule._adjustYaw(m, Engine.random() * 25)
      },
      Fn.new { |g, m|
        OgreModule._chainsaw(g, m, 0)
        OgreModule._adjustYaw(m, Engine.random() * 25)
      },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 3) },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 8) },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 9) }
    ]
  }

  static _smashActions() {
    return [
      Fn.new { |g, m|
        FightModule.ai_charge(g, m, 6)
        Engine.playSound(m, Channels.WEAPON, "ogre/ogsawatk.wav", 1, Attenuations.NORMAL)
      },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 0) },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 0) },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 1) },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 4) },
      Fn.new { |g, m|
        FightModule.ai_charge(g, m, 4)
        OgreModule._chainsaw(g, m, 0)
      },
      Fn.new { |g, m|
        FightModule.ai_charge(g, m, 4)
        OgreModule._chainsaw(g, m, 0)
      },
      Fn.new { |g, m|
        FightModule.ai_charge(g, m, 10)
        OgreModule._chainsaw(g, m, 0)
      },
      Fn.new { |g, m|
        FightModule.ai_charge(g, m, 13)
        OgreModule._chainsaw(g, m, 0)
      },
      Fn.new { |g, m| OgreModule._chainsaw(g, m, 1) },
      Fn.new { |g, m|
        FightModule.ai_charge(g, m, 2)
        OgreModule._chainsaw(g, m, 0)
        OgreModule._extendThink(g, m, Engine.random() * 0.2)
      },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 0) },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 4) },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 12) }
    ]
  }

  static _shootActions() {
    return [
      Fn.new { |g, m| OgreModule._faceEnemy(g, m) },
      Fn.new { |g, m| OgreModule._faceEnemy(g, m) },
      Fn.new { |g, m| OgreModule._faceEnemy(g, m) },
      Fn.new { |g, m|
        OgreModule._faceEnemy(g, m)
        OgreModule._ogreFireGrenade(g, m)
      },
      Fn.new { |g, m| OgreModule._faceEnemy(g, m) },
      Fn.new { |g, m| OgreModule._faceEnemy(g, m) }
    ]
  }

  static ogre_stand1(globals, monster) {
    OgreModule._loopSequence(globals, monster, _STAND_FRAMES, "_ogreStandIndex", "OgreModule.ogre_stand1", Fn.new { |i|
      AIModule.ai_stand(globals, monster)
    })
  }

  static ogre_walk1(globals, monster) {
    OgreModule._loopSequence(globals, monster, _WALK_FRAMES, "_ogreWalkIndex", "OgreModule.ogre_walk1", Fn.new { |i|
      OgreModule._walkAction(globals, monster, i)
    })
  }

  static ogre_run1(globals, monster) {
    OgreModule._loopSequence(globals, monster, _RUN_FRAMES, "_ogreRunIndex", "OgreModule.ogre_run1", Fn.new { |i|
      OgreModule._runAction(globals, monster, i)
    })
  }

  static ogre_swing1(globals, monster) {
    monster.set("_ogreSwingIndex", 0)
    OgreModule._ogre_swingAdvance(globals, monster)
  }

  static _ogre_swingAdvance(globals, monster) {
    OgreModule._advanceSequence(globals, monster, _SWING_FRAMES, OgreModule._swingActions(), "_ogreSwingIndex", "OgreModule._ogre_swingAdvance", "OgreModule.ogre_run1")
  }

  static ogre_smash1(globals, monster) {
    monster.set("_ogreSmashIndex", 0)
    OgreModule._ogre_smashAdvance(globals, monster)
  }

  static _ogre_smashAdvance(globals, monster) {
    OgreModule._advanceSequence(globals, monster, _SMASH_FRAMES, OgreModule._smashActions(), "_ogreSmashIndex", "OgreModule._ogre_smashAdvance", "OgreModule.ogre_run1")
  }

  static ogre_nail1(globals, monster) {
    monster.set("_ogreShootIndex", 0)
    OgreModule._ogre_shootAdvance(globals, monster)
  }

  static _ogre_shootAdvance(globals, monster) {
    OgreModule._advanceSequence(globals, monster, _SHOOT_FRAMES, OgreModule._shootActions(), "_ogreShootIndex", "OgreModule._ogre_shootAdvance", "OgreModule.ogre_run1")
  }

  static _resumeRun(globals, monster) {
    OgreModule.ogre_run1(globals, monster)
  }

  static ogre_pain(globals, monster, attacker, damage) {
    if (monster.get("pain_finished", 0.0) > globals.time) return

    Engine.playSound(monster, Channels.VOICE, "ogre/ogpain1.wav", 1, Attenuations.NORMAL)

    var choice = Engine.random()
    if (choice < 0.25) {
      monster.set("_ogrePainIndex", 0)
      monster.set("pain_finished", globals.time + 1.0)
      OgreModule._advanceSequence(globals, monster, _PAIN_SHORT_FRAMES, null, "_ogrePainIndex", "OgreModule._advancePainA", "OgreModule.ogre_run1")
    } else if (choice < 0.5) {
      monster.set("_ogrePainBIndex", 0)
      monster.set("pain_finished", globals.time + 1.0)
      OgreModule._advanceSequence(globals, monster, _PAIN_B_FRAMES, null, "_ogrePainBIndex", "OgreModule._advancePainB", "OgreModule.ogre_run1")
    } else if (choice < 0.75) {
      monster.set("_ogrePainCIndex", 0)
      monster.set("pain_finished", globals.time + 1.0)
      OgreModule._advanceSequence(globals, monster, _PAIN_C_FRAMES, null, "_ogrePainCIndex", "OgreModule._advancePainC", "OgreModule.ogre_run1")
    } else if (choice < 0.88) {
      monster.set("_ogrePainDIndex", 0)
      monster.set("pain_finished", globals.time + 2.0)
      OgreModule._advanceSequence(globals, monster, _PAIN_D_FRAMES, _PAIN_D_ACTIONS, "_ogrePainDIndex", "OgreModule._advancePainD", "OgreModule.ogre_run1")
    } else {
      monster.set("_ogrePainEIndex", 0)
      monster.set("pain_finished", globals.time + 2.0)
      OgreModule._advanceSequence(globals, monster, _PAIN_E_FRAMES, _PAIN_E_ACTIONS, "_ogrePainEIndex", "OgreModule._advancePainE", "OgreModule.ogre_run1")
    }
  }

  static _advancePainA(globals, monster) { OgreModule._advanceSequence(globals, monster, _PAIN_SHORT_FRAMES, null, "_ogrePainIndex", "OgreModule._advancePainA", "OgreModule.ogre_run1") }
  static _advancePainB(globals, monster) { OgreModule._advanceSequence(globals, monster, _PAIN_B_FRAMES, null, "_ogrePainBIndex", "OgreModule._advancePainB", "OgreModule.ogre_run1") }
  static _advancePainC(globals, monster) { OgreModule._advanceSequence(globals, monster, _PAIN_C_FRAMES, null, "_ogrePainCIndex", "OgreModule._advancePainC", "OgreModule.ogre_run1") }
  static _advancePainD(globals, monster) { OgreModule._advanceSequence(globals, monster, _PAIN_D_FRAMES, _PAIN_D_ACTIONS, "_ogrePainDIndex", "OgreModule._advancePainD", "OgreModule.ogre_run1") }
  static _advancePainE(globals, monster) { OgreModule._advanceSequence(globals, monster, _PAIN_E_FRAMES, _PAIN_E_ACTIONS, "_ogrePainEIndex", "OgreModule._advancePainE", "OgreModule.ogre_run1") }

  static ogre_die(globals, monster) {
    var health = monster.get("health", 0)
    if (health < -80) {
      Engine.playSound(monster, Channels.VOICE, "player/udeath.wav", 1, Attenuations.NORMAL)
      PlayerModule.ThrowHead(globals, monster, "progs/h_ogre.mdl", health)
      PlayerModule.ThrowGib(globals, monster, "progs/gib3.mdl", health)
      PlayerModule.ThrowGib(globals, monster, "progs/gib3.mdl", health)
      PlayerModule.ThrowGib(globals, monster, "progs/gib3.mdl", health)
      return
    }

    Engine.playSound(monster, Channels.VOICE, "ogre/ogdth.wav", 1, Attenuations.NORMAL)
    if (Engine.random() < 0.5) {
      monster.set("_ogreDeathAIndex", 0)
      OgreModule._ogre_deathAAdvance(globals, monster)
    } else {
      monster.set("_ogreDeathBIndex", 0)
      OgreModule._ogre_deathBAdvance(globals, monster)
    }
  }

  static _ogre_deathAAdvance(globals, monster) {
    OgreModule._advanceSequence(globals, monster, _DEATH_A_FRAMES, _DEATH_A_ACTIONS, "_ogreDeathAIndex", "OgreModule._ogre_deathAAdvance", null)
  }

  static _ogre_deathBAdvance(globals, monster) {
    OgreModule._advanceSequence(globals, monster, _DEATH_B_FRAMES, _DEATH_B_ACTIONS, "_ogreDeathBIndex", "OgreModule._ogre_deathBAdvance", null)
  }

  static ogre_melee(globals, monster) {
    if (Engine.random() > 0.5) {
      OgreModule.ogre_smash1(globals, monster)
    } else {
      OgreModule.ogre_swing1(globals, monster)
    }
  }

  static monster_ogre(globals, monster) {
    if (globals.deathmatch != 0) {
      Engine.removeEntity(monster)
      return
    }

    Engine.precacheModel("progs/ogre.mdl")
    Engine.precacheModel("progs/h_ogre.mdl")
    Engine.precacheModel("progs/grenade.mdl")
    Engine.precacheModel("progs/s_explod.spr")

    Engine.precacheSound("ogre/ogdrag.wav")
    Engine.precacheSound("ogre/ogdth.wav")
    Engine.precacheSound("ogre/ogidle.wav")
    Engine.precacheSound("ogre/ogidle2.wav")
    Engine.precacheSound("ogre/ogpain1.wav")
    Engine.precacheSound("ogre/ogsawatk.wav")
    Engine.precacheSound("ogre/ogwake.wav")
    Engine.precacheSound("weapons/grenade.wav")
    Engine.precacheSound("weapons/r_exp3.wav")
    Engine.precacheSound("weapons/bounce.wav")

    monster.set("solid", SolidTypes.SLIDEBOX)
    monster.set("movetype", MoveTypes.STEP)
    Engine.setModel(monster, "progs/ogre.mdl")

    monster.set("noise", "ogre/ogwake.wav")
    monster.set("netname", "$qc_ogre")
    monster.set("killstring", "$qc_ks_ogre")

    Engine.setSize(monster, [-32, -32, -24], [32, 32, 64])
    monster.set("health", 200)
    monster.set("max_health", 200)

    monster.set("th_stand", "OgreModule.ogre_stand1")
    monster.set("th_walk", "OgreModule.ogre_walk1")
    monster.set("th_run", "OgreModule.ogre_run1")
    monster.set("th_die", "OgreModule.ogre_die")
    monster.set("th_melee", "OgreModule.ogre_melee")
    monster.set("th_missile", "OgreModule.ogre_nail1")
    monster.set("th_pain", "OgreModule.ogre_pain")

    monster.set("allowPathFind", true)
    monster.set("combat_style", CombatStyles.MIXED)

    monster.set("_ogreStandIndex", 0)
    monster.set("_ogreWalkIndex", 0)
    monster.set("_ogreRunIndex", 0)
    monster.set("_ogreSwingIndex", 0)
    monster.set("_ogreSmashIndex", 0)
    monster.set("_ogreShootIndex", 0)
    monster.set("_ogrePainIndex", 0)
    monster.set("_ogrePainBIndex", 0)
    monster.set("_ogrePainCIndex", 0)
    monster.set("_ogrePainDIndex", 0)
    monster.set("_ogrePainEIndex", 0)
    monster.set("_ogreDeathAIndex", 0)
    monster.set("_ogreDeathBIndex", 0)

    MonstersModule.walkmonster_start(globals, monster)
  }

  static monster_ogre_marksman(globals, monster) {
    OgreModule.monster_ogre(globals, monster)
  }
}
