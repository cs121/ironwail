// Shambler.wren
// Ports the shambler monster from shambler.qc so that the enemy can run
// entirely inside the Wren gameplay runtime.

import "./Engine" for Engine
import "./Globals" for Channels, Attenuations, SolidTypes, MoveTypes, Effects, CombatStyles
import "./Globals" for Items, TempEntityCodes, ServiceCodes, MessageTypes
import "./Monsters" for MonstersModule
import "./AI" for AIModule
import "./Fight" for FightModule
import "./Combat" for CombatModule
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
  "stand9",
  "stand10",
  "stand11",
  "stand12",
  "stand13",
  "stand14",
  "stand15",
  "stand16",
  "stand17"
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
  "walk12"
]

var _WALK_SPEEDS = [10, 9, 9, 5, 6, 12, 8, 3, 13, 9, 7, 7]

var _RUN_FRAMES = ["run1", "run2", "run3", "run4", "run5", "run6"]
var _RUN_SPEEDS = [20, 24, 20, 20, 24, 20]

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
  "smash12"
]

var _SWING_LEFT_FRAMES = [
  "swingl1",
  "swingl2",
  "swingl3",
  "swingl4",
  "swingl5",
  "swingl6",
  "swingl7",
  "swingl8",
  "swingl9"
]

var _SWING_RIGHT_FRAMES = [
  "swingr1",
  "swingr2",
  "swingr3",
  "swingr4",
  "swingr5",
  "swingr6",
  "swingr7",
  "swingr8",
  "swingr9"
]

var _MAGIC_FRAMES = [
  "magic1",
  "magic2",
  "magic3",
  "magic4",
  "magic5",
  "magic6",
  "magic9",
  "magic10",
  "magic11",
  "magic12"
]

var _PAIN_FRAMES = ["pain1", "pain2", "pain3", "pain4", "pain5", "pain6"]

var _DEATH_FRAMES = [
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
  "death11"
]

class ShamblerModule {
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
    var length = ShamblerModule._vectorLength(v)
    if (length == 0) return [0, 0, 0]
    var inv = 1 / length
    return [v[0] * inv, v[1] * inv, v[2] * inv]
  }

  static _vectorToYaw(vector) {
    var angles = Engine.vectorToAngles(vector)
    return angles[1]
  }

  static _makeVectors(angles) {
    var adjusted = [-angles[0], angles[1], angles[2]]
    return Engine.makeVectors(adjusted)
  }

  static _enemyYaw(monster) {
    var enemy = monster.get("enemy", null)
    if (enemy == null) return monster.get("angles", [0, 0, 0])[1]
    return ShamblerModule._vectorToYaw(ShamblerModule._vectorSub(enemy.get("origin", [0, 0, 0]), monster.get("origin", [0, 0, 0])))
  }

  static _faceEnemy(globals, monster) {
    FightModule.ai_face(globals, monster, ShamblerModule._enemyYaw(monster))
  }

  static _setFrame(globals, monster, frame, nextFunction, delay) {
    MonstersModule.setFrame(globals, monster, frame, nextFunction, delay)
  }

  static _loopSequence(globals, monster, frames, indexField, nextFunction, actionFn) {
    if (monster == null) return
    if (frames == null || frames.count == 0) return

    var index = monster.get(indexField, 0)
    if (index < 0 || index >= frames.count) index = 0

    ShamblerModule._setFrame(globals, monster, frames[index], nextFunction, 0.1)

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
    ShamblerModule._setFrame(globals, monster, frames[index], nextName, delay)

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

  static _extendThink(globals, monster, extra) {
    if (monster == null) return
    var current = monster.get("nextthink", globals.time + 0.1)
    var newTime = current + extra
    monster.set("nextthink", newTime)
    var thinkName = monster.get("think", null)
    if (thinkName == null || thinkName == "") return
    var delay = newTime - globals.time
    if (delay < 0) delay = 0
    Engine.scheduleThink(monster, thinkName, delay)
  }

  static _maybeIdle(globals, monster) {
    if (Engine.random() > 0.8) {
      Engine.playSound(monster, Channels.VOICE, "shambler/sidle.wav", 1, Attenuations.IDLE)
    }
  }

  static sham_stand1(globals, monster) {
    ShamblerModule._loopSequence(globals, monster, _STAND_FRAMES, "_shamblerStandIndex", "ShamblerModule.sham_stand1", Fn.new { |_|
      AIModule.ai_stand(globals, monster)
    })
  }

  static sham_walk1(globals, monster) {
    ShamblerModule._loopSequence(globals, monster, _WALK_FRAMES, "_shamblerWalkIndex", "ShamblerModule.sham_walk1", Fn.new { |index|
      AIModule.ai_walk(globals, monster, _WALK_SPEEDS[index])
      if (index == _WALK_FRAMES.count - 1) {
        ShamblerModule._maybeIdle(globals, monster)
      }
    })
  }

  static sham_run1(globals, monster) {
    ShamblerModule._loopSequence(globals, monster, _RUN_FRAMES, "_shamblerRunIndex", "ShamblerModule.sham_run1", Fn.new { |index|
      AIModule.ai_run(globals, monster, _RUN_SPEEDS[index])
      if (index == _RUN_FRAMES.count - 1) {
        ShamblerModule._maybeIdle(globals, monster)
      }
    })
  }

  static _shamSmashDamage(globals, monster) {
    var enemy = monster.get("enemy", null)
    if (enemy == null) return

    FightModule.ai_charge(globals, monster, 0)

    var delta = ShamblerModule._vectorSub(enemy.get("origin", [0, 0, 0]), monster.get("origin", [0, 0, 0]))
    if (ShamblerModule._vectorLength(delta) > 100) return
    if (!CombatModule.canDamage(globals, enemy, monster)) return

    var damage = (Engine.random() + Engine.random() + Engine.random()) * 40
    CombatModule.tDamage(globals, enemy, monster, monster, damage)
    Engine.playSound(monster, Channels.VOICE, "shambler/smack.wav", 1, Attenuations.NORMAL)

    var vectors = ShamblerModule._makeVectors(monster.get("angles", [0, 0, 0]))
    if (vectors == null || !vectors.containsKey("forward")) return
    var forward = vectors["forward"]
    var right = vectors.containsKey("right") ? vectors["right"] : [1, 0, 0]
    var origin = ShamblerModule._vectorAdd(monster.get("origin", [0, 0, 0]), ShamblerModule._vectorScale(forward, 16))

    for (i in 0..1) {
      var velocity = ShamblerModule._vectorScale(right, (Engine.random() * 2 - 1) * 100)
      WeaponsModule.SpawnMeatSpray(globals, origin, velocity)
    }
  }

  static _shamClaw(globals, monster, side) {
    var enemy = monster.get("enemy", null)
    if (enemy == null) return

    FightModule.ai_charge(globals, monster, 10)

    var delta = ShamblerModule._vectorSub(enemy.get("origin", [0, 0, 0]), monster.get("origin", [0, 0, 0]))
    if (ShamblerModule._vectorLength(delta) > 100) return

    var damage = (Engine.random() + Engine.random() + Engine.random()) * 20
    CombatModule.tDamage(globals, enemy, monster, monster, damage)
    Engine.playSound(monster, Channels.VOICE, "shambler/smack.wav", 1, Attenuations.NORMAL)

    if (side == 0) return

    var vectors = ShamblerModule._makeVectors(monster.get("angles", [0, 0, 0]))
    if (vectors == null || !vectors.containsKey("forward")) return
    var forward = vectors["forward"]
    var right = vectors.containsKey("right") ? vectors["right"] : [1, 0, 0]
    var origin = ShamblerModule._vectorAdd(monster.get("origin", [0, 0, 0]), ShamblerModule._vectorScale(forward, 16))
    var velocity = ShamblerModule._vectorScale(right, side)
    WeaponsModule.SpawnMeatSpray(globals, origin, velocity)
  }

  static _sham_smashAdvance(globals, monster) {
    var actions = [
      Fn.new { |g, m|
        Engine.playSound(m, Channels.VOICE, "shambler/melee1.wav", 1, Attenuations.NORMAL)
        FightModule.ai_charge(g, m, 2)
      },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 6) },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 6) },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 5) },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 4) },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 1) },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 0) },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 0) },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 0) },
      Fn.new { |g, m| ShamblerModule._shamSmashDamage(g, m) },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 5) },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 4) }
    ]

    ShamblerModule._advanceSequence(globals, monster, _SMASH_FRAMES, actions, "_shamblerSmashIndex", "ShamblerModule._sham_smashAdvance", "ShamblerModule.sham_run1")
  }

  static sham_smash1(globals, monster) {
    monster.set("_shamblerSmashIndex", 0)
    ShamblerModule._sham_smashAdvance(globals, monster)
  }

  static _overrideThink(globals, monster, thinkName) {
    if (monster == null) return
    monster.set("think", thinkName)
    monster.set("nextthink", globals.time + 0.1)
    Engine.scheduleThink(monster, thinkName, 0.1)
  }

  static _sham_swingLeftAdvance(globals, monster) {
    var actions = [
      Fn.new { |g, m|
        Engine.playSound(m, Channels.VOICE, "shambler/melee2.wav", 1, Attenuations.NORMAL)
        FightModule.ai_charge(g, m, 5)
      },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 3) },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 7) },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 3) },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 7) },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 9) },
      Fn.new { |g, m|
        FightModule.ai_charge(g, m, 5)
        ShamblerModule._shamClaw(g, m, 250)
      },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 4) },
      Fn.new { |g, m|
        FightModule.ai_charge(g, m, 8)
        if (Engine.random() < 0.5) {
          ShamblerModule._overrideThink(g, m, "ShamblerModule.sham_swingr1")
        }
      }
    ]

    ShamblerModule._advanceSequence(globals, monster, _SWING_LEFT_FRAMES, actions, "_shamblerSwingLeftIndex", "ShamblerModule._sham_swingLeftAdvance", "ShamblerModule.sham_run1")
  }

  static sham_swingl1(globals, monster) {
    monster.set("_shamblerSwingLeftIndex", 0)
    ShamblerModule._sham_swingLeftAdvance(globals, monster)
  }

  static _sham_swingRightAdvance(globals, monster) {
    var actions = [
      Fn.new { |g, m|
        Engine.playSound(m, Channels.VOICE, "shambler/melee1.wav", 1, Attenuations.NORMAL)
        FightModule.ai_charge(g, m, 1)
      },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 8) },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 14) },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 7) },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 3) },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 6) },
      Fn.new { |g, m|
        FightModule.ai_charge(g, m, 6)
        ShamblerModule._shamClaw(g, m, -250)
      },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 3) },
      Fn.new { |g, m|
        FightModule.ai_charge(g, m, 1)
        FightModule.ai_charge(g, m, 10)
        if (Engine.random() < 0.5) {
          ShamblerModule._overrideThink(g, m, "ShamblerModule.sham_swingl1")
        }
      }
    ]

    ShamblerModule._advanceSequence(globals, monster, _SWING_RIGHT_FRAMES, actions, "_shamblerSwingRightIndex", "ShamblerModule._sham_swingRightAdvance", "ShamblerModule.sham_run1")
  }

  static sham_swingr1(globals, monster) {
    monster.set("_shamblerSwingRightIndex", 0)
    ShamblerModule._sham_swingRightAdvance(globals, monster)
  }

  static sham_melee(globals, monster) {
    var chance = Engine.random()
    if (chance > 0.6 || monster.get("health", 0) == 600) {
      ShamblerModule.sham_smash1(globals, monster)
    } else if (chance > 0.3) {
      ShamblerModule.sham_swingr1(globals, monster)
    } else {
      ShamblerModule.sham_swingl1(globals, monster)
    }
  }

  static _castLightning(globals, monster) {
    var enemy = monster.get("enemy", null)
    if (enemy == null) return

    var effects = monster.get("effects", 0)
    effects = Engine.bitOr(effects, Effects.MUZZLEFLASH)
    monster.set("effects", effects)

    ShamblerModule._faceEnemy(globals, monster)

    var origin = ShamblerModule._vectorAdd(monster.get("origin", [0, 0, 0]), [0, 0, 40])
    var target = ShamblerModule._vectorAdd(enemy.get("origin", [0, 0, 0]), [0, 0, 16])
    var direction = ShamblerModule._vectorNormalize(ShamblerModule._vectorSub(target, origin))
    var end = ShamblerModule._vectorAdd(origin, ShamblerModule._vectorScale(direction, 600))

    var trace = Engine.traceLine(origin, end, true, monster)
    var impact = trace != null && trace.containsKey("endpos") ? trace["endpos"] : end

    Engine.emitTempEntity(TempEntityCodes.LIGHTNING1, {
      "owner": monster,
      "start": origin,
      "end": impact
    })

    monster.set("frags", monster.get("frags", 0) + 1)
    WeaponsModule.LightningDamage(globals, monster, origin, impact, 10)
  }

  static _sham_magicAdvance(globals, monster) {
    var actions = [
      Fn.new { |g, m|
        ShamblerModule._faceEnemy(g, m)
        Engine.playSound(m, Channels.WEAPON, "shambler/sattck1.wav", 1, Attenuations.NORMAL)
      },
      Fn.new { |g, m| ShamblerModule._faceEnemy(g, m) },
      Fn.new { |g, m|
        ShamblerModule._faceEnemy(g, m)
        ShamblerModule._extendThink(g, m, 0.2)
        var owner = Engine.spawnEntity()
        m.set("owner", owner)
        Engine.setModel(owner, "progs/s_light.mdl")
        Engine.setOrigin(owner, m.get("origin", [0, 0, 0]))
        owner.set("angles", m.get("angles", [0, 0, 0]))
        owner.set("think", "SubsModule.subRemove")
        owner.set("nextthink", globals.time + 0.7)
        Engine.scheduleThink(owner, "SubsModule.subRemove", 0.7)
      },
      Fn.new { |g, m|
        var owner = m.get("owner", null)
        if (owner != null) {
          owner.set("frame", 1)
        }
        m.set("effects", Engine.bitOr(m.get("effects", 0), Effects.MUZZLEFLASH))
      },
      Fn.new { |g, m|
        var owner = m.get("owner", null)
        if (owner != null) {
          owner.set("frame", 2)
        }
        m.set("effects", Engine.bitOr(m.get("effects", 0), Effects.MUZZLEFLASH))
      },
      Fn.new { |g, m|
        var owner = m.get("owner", null)
        if (owner != null) {
          Engine.removeEntity(owner)
          m.set("owner", null)
        }
        ShamblerModule._castLightning(g, m)
        Engine.playSound(m, Channels.WEAPON, "shambler/sboom.wav", 1, Attenuations.NORMAL)
      },
      Fn.new { |g, m| ShamblerModule._castLightning(g, m) },
      Fn.new { |g, m| ShamblerModule._castLightning(g, m) },
      null,
      null
    ]

    ShamblerModule._advanceSequence(globals, monster, _MAGIC_FRAMES, actions, "_shamblerMagicIndex", "ShamblerModule._sham_magicAdvance", "ShamblerModule.sham_run1")
  }

  static sham_magic1(globals, monster) {
    monster.set("_shamblerMagicIndex", 0)
    ShamblerModule._sham_magicAdvance(globals, monster)
  }

  static _sham_painAdvance(globals, monster) {
    ShamblerModule._advanceSequence(globals, monster, _PAIN_FRAMES, null, "_shamblerPainIndex", "ShamblerModule._sham_painAdvance", "ShamblerModule.sham_run1")
  }

  static sham_pain(globals, monster, attacker, damage) {
    Engine.playSound(monster, Channels.VOICE, "shambler/shurt2.wav", 1, Attenuations.NORMAL)

    if (attacker != null && attacker.get("classname", "") == "player" && damage >= monster.get("health", 0)) {
      if (attacker.get("weapon", Items.AXE) == Items.AXE) {
        Engine.writeByte(MessageTypes.ONE, ServiceCodes.ACHIEVEMENT, attacker)
        Engine.writeString(MessageTypes.ONE, "ACH_CLOSE_SHAVE", attacker)
      }
      if (monster.get("frags", 0) == 0) {
        Engine.writeByte(MessageTypes.ONE, ServiceCodes.ACHIEVEMENT, attacker)
        Engine.writeString(MessageTypes.ONE, "ACH_SHAMBLER_DANCE", attacker)
      }
    }

    if (monster.get("health", 0) <= 0) return
    if (Engine.random() * 400 > damage) return
    if (monster.get("pain_finished", 0.0) > globals.time) return

    monster.set("pain_finished", globals.time + 2)
    monster.set("_shamblerPainIndex", 0)
    ShamblerModule._sham_painAdvance(globals, monster)
  }

  static _sham_deathAdvance(globals, monster) {
    var actions = [
      null,
      null,
      Fn.new { |g, m| m.set("solid", SolidTypes.NOT) }
    ]

    ShamblerModule._advanceSequence(globals, monster, _DEATH_FRAMES, actions, "_shamblerDeathIndex", "ShamblerModule._sham_deathAdvance", null)
  }

  static sham_die(globals, monster) {
    if (monster == null) return

    var health = monster.get("health", 0)
    if (health < -60) {
      Engine.playSound(monster, Channels.VOICE, "player/udeath.wav", 1, Attenuations.NORMAL)
      PlayerModule.ThrowHead(globals, monster, "progs/h_shams.mdl", health)
      PlayerModule.ThrowGib(globals, monster, "progs/gib1.mdl", health)
      PlayerModule.ThrowGib(globals, monster, "progs/gib2.mdl", health)
      PlayerModule.ThrowGib(globals, monster, "progs/gib3.mdl", health)
      return
    }

    Engine.playSound(monster, Channels.VOICE, "shambler/sdeath.wav", 1, Attenuations.NORMAL)
    monster.set("_shamblerDeathIndex", 0)
    ShamblerModule._sham_deathAdvance(globals, monster)
  }

  static monster_shambler(globals, monster) {
    if (monster == null) return

    if (globals.deathmatch != 0) {
      Engine.removeEntity(monster)
      return
    }

    Engine.precacheModel("progs/shambler.mdl")
    Engine.precacheModel("progs/s_light.mdl")
    Engine.precacheModel("progs/h_shams.mdl")
    Engine.precacheModel("progs/bolt.mdl")

    Engine.precacheSound("shambler/sattck1.wav")
    Engine.precacheSound("shambler/sboom.wav")
    Engine.precacheSound("shambler/sdeath.wav")
    Engine.precacheSound("shambler/shurt2.wav")
    Engine.precacheSound("shambler/sidle.wav")
    Engine.precacheSound("shambler/ssight.wav")
    Engine.precacheSound("shambler/melee1.wav")
    Engine.precacheSound("shambler/melee2.wav")
    Engine.precacheSound("shambler/smack.wav")

    monster.set("solid", SolidTypes.SLIDEBOX)
    monster.set("movetype", MoveTypes.STEP)
    Engine.setModel(monster, "progs/shambler.mdl")

    monster.set("noise", "shambler/ssight.wav")
    monster.set("netname", "$qc_shambler")
    monster.set("killstring", "$qc_ks_shambler")

    Engine.setSize(monster, [-32, -32, -24], [32, 32, 64])
    monster.set("health", 600)
    monster.set("max_health", 600)

    monster.set("th_stand", "ShamblerModule.sham_stand1")
    monster.set("th_walk", "ShamblerModule.sham_walk1")
    monster.set("th_run", "ShamblerModule.sham_run1")
    monster.set("th_die", "ShamblerModule.sham_die")
    monster.set("th_melee", "ShamblerModule.sham_melee")
    monster.set("th_missile", "ShamblerModule.sham_magic1")
    monster.set("th_pain", "ShamblerModule.sham_pain")

    monster.set("allowPathFind", true)
    monster.set("combat_style", CombatStyles.MIXED)
    monster.set("frags", 0)

    monster.set("_shamblerStandIndex", 0)
    monster.set("_shamblerWalkIndex", 0)
    monster.set("_shamblerRunIndex", 0)
    monster.set("_shamblerSmashIndex", 0)
    monster.set("_shamblerSwingLeftIndex", 0)
    monster.set("_shamblerSwingRightIndex", 0)
    monster.set("_shamblerMagicIndex", 0)
    monster.set("_shamblerPainIndex", 0)
    monster.set("_shamblerDeathIndex", 0)

    MonstersModule.walkmonster_start(globals, monster)
  }
}
