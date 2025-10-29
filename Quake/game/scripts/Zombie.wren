// Zombie.wren
// Ports the zombie monster implementation from zombie.qc so that all behavior
// runs inside Wren without delegating to QuakeC.

import "./Engine" for Engine
import "./Globals" for SolidTypes, MoveTypes, DamageValues, Channels, Attenuations, CombatStyles
import "./AI" for AIModule
import "./Fight" for FightModule
import "./Monsters" for MonstersModule
import "./Combat" for CombatModule
import "./Subs" for SubsModule
import "./Player" for PlayerModule

var _STAND_FRAMES = []
for (i in 1..15) { _STAND_FRAMES.add("stand" + i.toString) }

var _WALK_FRAMES = []
for (i in 1..19) { _WALK_FRAMES.add("walk" + i.toString) }

var _RUN_FRAMES = []
for (i in 1..18) { _RUN_FRAMES.add("run" + i.toString) }

var _ATTACK_A_FRAMES = []
for (i in 1..13) { _ATTACK_A_FRAMES.add("atta" + i.toString) }

var _ATTACK_B_FRAMES = []
for (i in 1..14) { _ATTACK_B_FRAMES.add("attb" + i.toString) }

var _ATTACK_C_FRAMES = []
for (i in 1..12) { _ATTACK_C_FRAMES.add("attc" + i.toString) }

var _PAINA_FRAMES = []
for (i in 1..12) { _PAINA_FRAMES.add("paina" + i.toString) }

var _PAINB_FRAMES = []
for (i in 1..28) { _PAINB_FRAMES.add("painb" + i.toString) }

var _PAINC_FRAMES = []
for (i in 1..18) { _PAINC_FRAMES.add("painc" + i.toString) }

var _PAIND_FRAMES = []
for (i in 1..13) { _PAIND_FRAMES.add("paind" + i.toString) }

var _PAINE_FRAMES = []
for (i in 1..30) { _PAINE_FRAMES.add("paine" + i.toString) }

var _CRUC_FRAMES = []
for (i in 1..6) { _CRUC_FRAMES.add("cruc_" + i.toString) }

var _WALK_SPEEDS = [0, 2, 3, 2, 1, 0, 0, 0, 0, 0, 2, 2, 1, 0, 0, 0, 0, 0, 0]
var _RUN_SPEEDS = [1, 1, 0, 1, 2, 3, 4, 4, 2, 0, 0, 0, 2, 4, 6, 7, 3, 8]

class ZombieModule {
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
    var length = ZombieModule._vectorLength(v)
    if (length == 0) return [0, 0, 0]
    var inv = 1 / length
    return [v[0] * inv, v[1] * inv, v[2] * inv]
  }

  static _makeVectors(angles) {
    var adjusted = [-angles[0], angles[1], angles[2]]
    var vectors = Engine.makeVectors(adjusted)
    if (vectors == null) return {"forward": [1, 0, 0], "right": [0, 1, 0], "up": [0, 0, 1]}
    if (!vectors.containsKey("forward")) vectors["forward"] = [1, 0, 0]
    if (!vectors.containsKey("right")) vectors["right"] = [0, 1, 0]
    if (!vectors.containsKey("up")) vectors["up"] = [0, 0, 1]
    return vectors
  }

  static _enemyYaw(monster) {
    var enemy = monster.get("enemy", null)
    if (enemy == null) return monster.get("angles", [0, 0, 0])[1]
    var delta = ZombieModule._vectorSub(enemy.get("origin", [0, 0, 0]), monster.get("origin", [0, 0, 0]))
    var angles = Engine.vectorToAngles(delta)
    return angles[1]
  }

  static _faceEnemy(globals, monster) {
    FightModule.ai_face(globals, monster, ZombieModule._enemyYaw(monster))
  }

  static _setFrame(globals, monster, frame, nextFunction, delay) {
    MonstersModule.setFrame(globals, monster, frame, nextFunction, delay)
  }

  static _advanceSequence(globals, monster, frames, stateField, advanceName, restartName, actionFn) {
    if (monster == null) return
    if (frames == null || frames.count == 0) return

    var index = monster.get(stateField, 0)
    if (index < 0 || index >= frames.count) index = 0

    var frame = frames[index]
    var nextIndex = index + 1
    var nextFunction = advanceName
    if (nextIndex >= frames.count) {
      nextIndex = 0
      nextFunction = restartName
    }
    monster.set(stateField, nextIndex)

    ZombieModule._setFrame(globals, monster, frame, nextFunction, 0.1)
    if (actionFn != null) actionFn.call(globals, monster, index)
  }

  static zombie_stand1(globals, monster) {
    monster.set("_zombieStandIndex", 0)
    ZombieModule._zombieStandAdvance(globals, monster)
  }

  static _zombieStandAdvance(globals, monster) {
    ZombieModule._advanceSequence(globals, monster, _STAND_FRAMES, "_zombieStandIndex", "ZombieModule._zombieStandAdvance", "ZombieModule.zombie_stand1", Fn.new { |g, m, i|
      AIModule.ai_stand(g, m)
    })
  }

  static _walkAction(globals, monster, index) {
    if (index >= 0 && index < _WALK_SPEEDS.count) {
      AIModule.ai_walk(globals, monster, _WALK_SPEEDS[index])
    } else {
      AIModule.ai_walk(globals, monster, 0)
    }
    if (index == _WALK_FRAMES.count - 1) {
      if (Engine.random() < 0.2) {
        Engine.playSound(monster, Channels.VOICE, "zombie/z_idle.wav", 1, Attenuations.IDLE)
      }
    }
  }

  static zombie_walk1(globals, monster) {
    monster.set("_zombieWalkIndex", 0)
    ZombieModule._zombieWalkAdvance(globals, monster)
  }

  static _zombieWalkAdvance(globals, monster) {
    ZombieModule._advanceSequence(globals, monster, _WALK_FRAMES, "_zombieWalkIndex", "ZombieModule._zombieWalkAdvance", "ZombieModule.zombie_walk1", Fn.new { |g, m, i|
      ZombieModule._walkAction(g, m, i)
    })
  }

  static _runAction(globals, monster, index) {
    if (index == 0) {
      monster.set("inpain", 0)
    }
    if (index >= 0 && index < _RUN_SPEEDS.count) {
      AIModule.ai_run(globals, monster, _RUN_SPEEDS[index])
    } else {
      AIModule.ai_run(globals, monster, 0)
    }
    if (index == _RUN_FRAMES.count - 1) {
      if (Engine.random() < 0.2) {
        Engine.playSound(monster, Channels.VOICE, "zombie/z_idle.wav", 1, Attenuations.IDLE)
      }
      if (Engine.random() > 0.8) {
        Engine.playSound(monster, Channels.VOICE, "zombie/z_idle1.wav", 1, Attenuations.IDLE)
      }
    }
  }

  static zombie_run1(globals, monster) {
    monster.set("_zombieRunIndex", 0)
    ZombieModule._zombieRunAdvance(globals, monster)
  }

  static _zombieRunAdvance(globals, monster) {
    ZombieModule._advanceSequence(globals, monster, _RUN_FRAMES, "_zombieRunIndex", "ZombieModule._zombieRunAdvance", "ZombieModule.zombie_run1", Fn.new { |g, m, i|
      ZombieModule._runAction(g, m, i)
    })
  }

  static _attackAction(globals, monster, index, offset) {
    ZombieModule._faceEnemy(globals, monster)
    if (index == _ATTACK_A_FRAMES.count - 1 || index == _ATTACK_B_FRAMES.count - 1 || index == _ATTACK_C_FRAMES.count - 1) {
      ZombieModule.ZombieFireGrenade(globals, monster, offset)
    }
  }

  static zombie_atta1(globals, monster) {
    monster.set("_zombieAttackAIndex", 0)
    ZombieModule._advanceSequence(globals, monster, _ATTACK_A_FRAMES, "_zombieAttackAIndex", "ZombieModule._zombieAttackAAdvance", "ZombieModule.zombie_run1", Fn.new { |g, m, i|
      ZombieModule._faceEnemy(g, m)
      if (i == _ATTACK_A_FRAMES.count - 1) {
        ZombieModule.ZombieFireGrenade(g, m, [-10, -22, 30])
      }
    })
  }

  static _zombieAttackAAdvance(globals, monster) {
    ZombieModule._advanceSequence(globals, monster, _ATTACK_A_FRAMES, "_zombieAttackAIndex", "ZombieModule._zombieAttackAAdvance", "ZombieModule.zombie_run1", Fn.new { |g, m, i|
      ZombieModule._faceEnemy(g, m)
      if (i == _ATTACK_A_FRAMES.count - 1) {
        ZombieModule.ZombieFireGrenade(g, m, [-10, -22, 30])
      }
    })
  }

  static zombie_attb1(globals, monster) {
    monster.set("_zombieAttackBIndex", 0)
    ZombieModule._advanceSequence(globals, monster, _ATTACK_B_FRAMES, "_zombieAttackBIndex", "ZombieModule._zombieAttackBAdvance", "ZombieModule.zombie_run1", Fn.new { |g, m, i|
      ZombieModule._faceEnemy(g, m)
      if (i == _ATTACK_B_FRAMES.count - 1) {
        ZombieModule.ZombieFireGrenade(g, m, [-10, -24, 29])
      }
    })
  }

  static _zombieAttackBAdvance(globals, monster) {
    ZombieModule._advanceSequence(globals, monster, _ATTACK_B_FRAMES, "_zombieAttackBIndex", "ZombieModule._zombieAttackBAdvance", "ZombieModule.zombie_run1", Fn.new { |g, m, i|
      ZombieModule._faceEnemy(g, m)
      if (i == _ATTACK_B_FRAMES.count - 1) {
        ZombieModule.ZombieFireGrenade(g, m, [-10, -24, 29])
      }
    })
  }

  static zombie_attc1(globals, monster) {
    monster.set("_zombieAttackCIndex", 0)
    ZombieModule._advanceSequence(globals, monster, _ATTACK_C_FRAMES, "_zombieAttackCIndex", "ZombieModule._zombieAttackCAdvance", "ZombieModule.zombie_run1", Fn.new { |g, m, i|
      ZombieModule._faceEnemy(g, m)
      if (i == _ATTACK_C_FRAMES.count - 1) {
        ZombieModule.ZombieFireGrenade(g, m, [-12, -19, 29])
      }
    })
  }

  static _zombieAttackCAdvance(globals, monster) {
    ZombieModule._advanceSequence(globals, monster, _ATTACK_C_FRAMES, "_zombieAttackCIndex", "ZombieModule._zombieAttackCAdvance", "ZombieModule.zombie_run1", Fn.new { |g, m, i|
      ZombieModule._faceEnemy(g, m)
      if (i == _ATTACK_C_FRAMES.count - 1) {
        ZombieModule.ZombieFireGrenade(g, m, [-12, -19, 29])
      }
    })
  }

  static zombie_missile(globals, monster) {
    var r = Engine.random()
    if (r < 0.3) {
      ZombieModule.zombie_atta1(globals, monster)
    } else if (r < 0.6) {
      ZombieModule.zombie_attb1(globals, monster)
    } else {
      ZombieModule.zombie_attc1(globals, monster)
    }
  }

  static ZombieGrenadeTouch(globals, grenade, other) {
    if (grenade == null) return
    if (other == grenade.get("owner", null)) return

    if (other != null && other.get("takedamage", DamageValues.NO) != DamageValues.NO) {
      CombatModule.tDamage(globals, other, grenade, grenade.get("owner", grenade), 10)
      Engine.playSound(grenade, Channels.WEAPON, "zombie/z_hit.wav", 1, Attenuations.NORMAL)
      Engine.removeEntity(grenade)
      return
    }

    Engine.playSound(grenade, Channels.WEAPON, "zombie/z_miss.wav", 1, Attenuations.NORMAL)
    grenade.set("velocity", [0, 0, 0])
    grenade.set("avelocity", [0, 0, 0])
    grenade.set("touch", "SubsModule.subRemove")
  }

  static ZombieFireGrenade(globals, monster, offset) {
    if (monster == null) return
    var enemy = monster.get("enemy", null)
    if (enemy == null) return

    Engine.playSound(monster, Channels.WEAPON, "zombie/z_shot1.wav", 1, Attenuations.NORMAL)

    var missile = Engine.spawnEntity()
    missile.set("classname", "zombie_grenade")
    missile.set("owner", monster)
    missile.set("movetype", MoveTypes.BOUNCE)
    missile.set("solid", SolidTypes.BBOX)

    var vectors = ZombieModule._makeVectors(monster.get("angles", [0, 0, 0]))
    var forward = vectors["forward"]
    var right = vectors["right"]
    var up = vectors["up"]

    var origin = ZombieModule._vectorAdd(monster.get("origin", [0, 0, 0]),
      ZombieModule._vectorAdd(ZombieModule._vectorAdd(ZombieModule._vectorScale(forward, offset[0]), ZombieModule._vectorScale(right, offset[1])), ZombieModule._vectorScale(up, offset[2] - 24)))

    var dir = ZombieModule._vectorNormalize(ZombieModule._vectorSub(enemy.get("origin", [0, 0, 0]), origin))
    var velocity = ZombieModule._vectorScale(dir, 600)
    velocity[2] = 200
    missile.set("velocity", velocity)
    missile.set("avelocity", [3000, 1000, 2000])

    missile.set("touch", "ZombieModule.ZombieGrenadeTouch")
    missile.set("think", "SubsModule.subRemove")
    var removeDelay = 2.5
    missile.set("nextthink", Engine.time() + removeDelay)
    Engine.scheduleThink(missile, "SubsModule.subRemove", removeDelay)

    Engine.setModel(missile, "progs/zom_gib.mdl")
    Engine.setSize(missile, [0, 0, 0], [0, 0, 0])
    Engine.setOrigin(missile, origin)
  }

  static _painaAction(globals, monster, index) {
    if (index == 0) {
      Engine.playSound(monster, Channels.VOICE, "zombie/z_pain.wav", 1, Attenuations.NORMAL)
    } else if (index == 1) {
      AIModule.ai_painforward(globals, monster, 3)
    } else if (index == 2) {
      AIModule.ai_painforward(globals, monster, 1)
    } else if (index == 3) {
      AIModule.ai_pain(globals, monster, 1)
    } else if (index == 4) {
      AIModule.ai_pain(globals, monster, 3)
    } else if (index == 5) {
      AIModule.ai_pain(globals, monster, 1)
    }
  }

  static zombie_paina1(globals, monster) {
    monster.set("_zombiePainAIndex", 0)
    ZombieModule._advanceSequence(globals, monster, _PAINA_FRAMES, "_zombiePainAIndex", "ZombieModule._zombiePainAAdvance", "ZombieModule.zombie_run1", Fn.new { |g, m, i|
      ZombieModule._painaAction(g, m, i)
    })
  }

  static _zombiePainAAdvance(globals, monster) {
    ZombieModule._advanceSequence(globals, monster, _PAINA_FRAMES, "_zombiePainAIndex", "ZombieModule._zombiePainAAdvance", "ZombieModule.zombie_run1", Fn.new { |g, m, i|
      ZombieModule._painaAction(g, m, i)
    })
  }

  static _painbAction(globals, monster, index) {
    if (index == 0) {
      Engine.playSound(monster, Channels.VOICE, "zombie/z_pain1.wav", 1, Attenuations.NORMAL)
    } else if (index == 1) {
      AIModule.ai_pain(globals, monster, 2)
    } else if (index == 2) {
      AIModule.ai_pain(globals, monster, 8)
    } else if (index == 3) {
      AIModule.ai_pain(globals, monster, 6)
    } else if (index == 4) {
      AIModule.ai_pain(globals, monster, 2)
    } else if (index == 8) {
      Engine.playSound(monster, Channels.BODY, "zombie/z_fall.wav", 1, Attenuations.NORMAL)
    } else if (index == 24) {
      AIModule.ai_painforward(globals, monster, 1)
    }
  }

  static zombie_painb1(globals, monster) {
    monster.set("_zombiePainBIndex", 0)
    ZombieModule._advanceSequence(globals, monster, _PAINB_FRAMES, "_zombiePainBIndex", "ZombieModule._zombiePainBAdvance", "ZombieModule.zombie_run1", Fn.new { |g, m, i|
      ZombieModule._painbAction(g, m, i)
    })
  }

  static _zombiePainBAdvance(globals, monster) {
    ZombieModule._advanceSequence(globals, monster, _PAINB_FRAMES, "_zombiePainBIndex", "ZombieModule._zombiePainBAdvance", "ZombieModule.zombie_run1", Fn.new { |g, m, i|
      ZombieModule._painbAction(g, m, i)
    })
  }

  static _paincAction(globals, monster, index) {
    if (index == 0) {
      Engine.playSound(monster, Channels.VOICE, "zombie/z_pain1.wav", 1, Attenuations.NORMAL)
    } else if (index == 2) {
      AIModule.ai_pain(globals, monster, 3)
    } else if (index == 3) {
      AIModule.ai_pain(globals, monster, 1)
    } else if (index == 10) {
      AIModule.ai_painforward(globals, monster, 1)
    } else if (index == 11) {
      AIModule.ai_painforward(globals, monster, 1)
    }
  }

  static zombie_painc1(globals, monster) {
    monster.set("_zombiePainCIndex", 0)
    ZombieModule._advanceSequence(globals, monster, _PAINC_FRAMES, "_zombiePainCIndex", "ZombieModule._zombiePainCAdvance", "ZombieModule.zombie_run1", Fn.new { |g, m, i|
      ZombieModule._paincAction(g, m, i)
    })
  }

  static _zombiePainCAdvance(globals, monster) {
    ZombieModule._advanceSequence(globals, monster, _PAINC_FRAMES, "_zombiePainCIndex", "ZombieModule._zombiePainCAdvance", "ZombieModule.zombie_run1", Fn.new { |g, m, i|
      ZombieModule._paincAction(g, m, i)
    })
  }

  static _paindAction(globals, monster, index) {
    if (index == 0) {
      Engine.playSound(monster, Channels.VOICE, "zombie/z_pain.wav", 1, Attenuations.NORMAL)
    } else if (index == 8) {
      AIModule.ai_pain(globals, monster, 1)
    }
  }

  static zombie_paind1(globals, monster) {
    monster.set("_zombiePainDIndex", 0)
    ZombieModule._advanceSequence(globals, monster, _PAIND_FRAMES, "_zombiePainDIndex", "ZombieModule._zombiePainDAdvance", "ZombieModule.zombie_run1", Fn.new { |g, m, i|
      ZombieModule._paindAction(g, m, i)
    })
  }

  static _zombiePainDAdvance(globals, monster) {
    ZombieModule._advanceSequence(globals, monster, _PAIND_FRAMES, "_zombiePainDIndex", "ZombieModule._zombiePainDAdvance", "ZombieModule.zombie_run1", Fn.new { |g, m, i|
      ZombieModule._paindAction(g, m, i)
    })
  }

  static _paineAction(globals, monster, index) {
    if (index == 0) {
      Engine.playSound(monster, Channels.VOICE, "zombie/z_pain.wav", 1, Attenuations.NORMAL)
      monster.set("health", 60)
    } else if (index == 1) {
      AIModule.ai_pain(globals, monster, 8)
    } else if (index == 2) {
      AIModule.ai_pain(globals, monster, 5)
    } else if (index == 3) {
      AIModule.ai_pain(globals, monster, 3)
    } else if (index == 4) {
      AIModule.ai_pain(globals, monster, 1)
    } else if (index == 5) {
      AIModule.ai_pain(globals, monster, 2)
    } else if (index == 6) {
      AIModule.ai_pain(globals, monster, 1)
    } else if (index == 7) {
      AIModule.ai_pain(globals, monster, 1)
    } else if (index == 8) {
      AIModule.ai_pain(globals, monster, 2)
    } else if (index == 9) {
      Engine.playSound(monster, Channels.BODY, "zombie/z_fall.wav", 1, Attenuations.NORMAL)
      monster.set("solid", SolidTypes.NOT)
    } else if (index == 10) {
      monster.set("nextthink", monster.get("nextthink", globals.time) + 5)
      monster.set("health", 60)
    } else if (index == 11) {
      monster.set("health", 60)
      Engine.playSound(monster, Channels.VOICE, "zombie/z_idle.wav", 1, Attenuations.IDLE)
      monster.set("solid", SolidTypes.SLIDEBOX)
      if (!Engine.walkMove(monster, 0, 0)) {
        monster.set("solid", SolidTypes.NOT)
        monster.set("_zombiePainEIndex", 10)
        var delay = 0.1
        monster.set("think", "ZombieModule.zombie_paine11")
        monster.set("nextthink", globals.time + delay)
        Engine.scheduleThink(monster, "ZombieModule.zombie_paine11", delay)
        return
      }
    } else if (index == 24) {
      AIModule.ai_painforward(globals, monster, 5)
    } else if (index == 25) {
      AIModule.ai_painforward(globals, monster, 3)
    } else if (index == 26) {
      AIModule.ai_painforward(globals, monster, 1)
    } else if (index == 27) {
      AIModule.ai_pain(globals, monster, 1)
    }
  }

  static zombie_paine1(globals, monster) {
    monster.set("_zombiePainEIndex", 0)
    ZombieModule._advanceSequence(globals, monster, _PAINE_FRAMES, "_zombiePainEIndex", "ZombieModule._zombiePainEAdvance", "ZombieModule.zombie_run1", Fn.new { |g, m, i|
      ZombieModule._paineAction(g, m, i)
    })
  }

  static _zombiePainEAdvance(globals, monster) {
    ZombieModule._advanceSequence(globals, monster, _PAINE_FRAMES, "_zombiePainEIndex", "ZombieModule._zombiePainEAdvance", "ZombieModule.zombie_run1", Fn.new { |g, m, i|
      ZombieModule._paineAction(g, m, i)
    })
  }

  static zombie_pain(globals, monster, attacker, damage) {
    if (monster == null) return
    monster.set("health", 60)

    if (damage < 9) return

    var inpain = monster.get("inpain", 0)
    if (inpain == 2) return

    if (damage >= 25) {
      monster.set("inpain", 2)
      ZombieModule.zombie_paine1(globals, monster)
      return
    }

    if (inpain != 0) {
      monster.set("pain_finished", globals.time + 3)
      return
    }

    var painFinished = monster.get("pain_finished", 0.0)
    if (painFinished > globals.time) {
      monster.set("inpain", 2)
      ZombieModule.zombie_paine1(globals, monster)
      return
    }

    monster.set("inpain", 1)
    var r = Engine.random()
    if (r < 0.25) {
      ZombieModule.zombie_paina1(globals, monster)
    } else if (r < 0.5) {
      ZombieModule.zombie_painb1(globals, monster)
    } else if (r < 0.75) {
      ZombieModule.zombie_painc1(globals, monster)
    } else {
      ZombieModule.zombie_paind1(globals, monster)
    }
  }

  static zombie_die(globals, monster) {
    Engine.playSound(monster, Channels.VOICE, "zombie/z_gib.wav", 1, Attenuations.NORMAL)
    PlayerModule.ThrowHead(globals, monster, "progs/h_zombie.mdl", monster.get("health", 0))
    PlayerModule.ThrowGib(globals, monster, "progs/gib1.mdl", monster.get("health", 0))
    PlayerModule.ThrowGib(globals, monster, "progs/gib2.mdl", monster.get("health", 0))
    PlayerModule.ThrowGib(globals, monster, "progs/gib3.mdl", monster.get("health", 0))
  }

  static _crucAction(globals, monster, index) {
    if (index == 0 && Engine.random() < 0.1) {
      Engine.playSound(monster, Channels.VOICE, "zombie/idle_w2.wav", 1, Attenuations.STATIC)
    }
    var delay = 0.1 + Engine.random() * 0.1
    monster.set("nextthink", globals.time + delay)
    monster.set("think", "ZombieModule.zombie_cruc" + (index + 2).toString)
    Engine.scheduleThink(monster, "ZombieModule.zombie_cruc" + (index + 2).toString, delay)
  }

  static zombie_cruc1(globals, monster) {
    ZombieModule._setFrame(globals, monster, _CRUC_FRAMES[0], "ZombieModule.zombie_cruc2", 0.1)
    if (Engine.random() < 0.1) {
      Engine.playSound(monster, Channels.VOICE, "zombie/idle_w2.wav", 1, Attenuations.STATIC)
    }
  }

  static zombie_cruc2(globals, monster) {
    var delay = 0.1 + Engine.random() * 0.1
    ZombieModule._setFrame(globals, monster, _CRUC_FRAMES[1], "ZombieModule.zombie_cruc3", delay)
  }

  static zombie_cruc3(globals, monster) {
    var delay = 0.1 + Engine.random() * 0.1
    ZombieModule._setFrame(globals, monster, _CRUC_FRAMES[2], "ZombieModule.zombie_cruc4", delay)
  }

  static zombie_cruc4(globals, monster) {
    var delay = 0.1 + Engine.random() * 0.1
    ZombieModule._setFrame(globals, monster, _CRUC_FRAMES[3], "ZombieModule.zombie_cruc5", delay)
  }

  static zombie_cruc5(globals, monster) {
    var delay = 0.1 + Engine.random() * 0.1
    ZombieModule._setFrame(globals, monster, _CRUC_FRAMES[4], "ZombieModule.zombie_cruc6", delay)
  }

  static zombie_cruc6(globals, monster) {
    var delay = 0.1 + Engine.random() * 0.1
    ZombieModule._setFrame(globals, monster, _CRUC_FRAMES[5], "ZombieModule.zombie_cruc1", delay)
  }

  static zombie_paine11(globals, monster) {
    ZombieModule._setFrame(globals, monster, "paine11", "ZombieModule.zombie_paine12", 0.1)
  }

  static zombie_paine12(globals, monster) {
    ZombieModule._setFrame(globals, monster, "paine12", "ZombieModule._zombiePainEAdvance", 0.1)
  }

  static zombie_painFinished(globals, monster) {
    monster.set("pain_finished", globals.time + 3.0)
  }

  static zombie_dieFinished(globals, monster) {
    monster.set("takedamage", DamageValues.NO)
  }

  static monster_zombie(globals, monster) {
    if (globals.deathmatch != 0) {
      Engine.removeEntity(monster)
      return
    }

    Engine.precacheModel("progs/zombie.mdl")
    Engine.precacheModel("progs/h_zombie.mdl")
    Engine.precacheModel("progs/zom_gib.mdl")

    Engine.precacheSound("zombie/z_idle.wav")
    Engine.precacheSound("zombie/z_idle1.wav")
    Engine.precacheSound("zombie/z_shot1.wav")
    Engine.precacheSound("zombie/z_gib.wav")
    Engine.precacheSound("zombie/z_pain.wav")
    Engine.precacheSound("zombie/z_pain1.wav")
    Engine.precacheSound("zombie/z_fall.wav")
    Engine.precacheSound("zombie/z_miss.wav")
    Engine.precacheSound("zombie/z_hit.wav")
    Engine.precacheSound("zombie/idle_w2.wav")

    monster.set("solid", SolidTypes.SLIDEBOX)
    monster.set("movetype", MoveTypes.STEP)
    Engine.setModel(monster, "progs/zombie.mdl")

    monster.set("noise", "zombie/z_idle.wav")
    monster.set("netname", "$qc_zombie")
    monster.set("killstring", "$qc_ks_zombie")

    Engine.setSize(monster, [-16, -16, -24], [16, 16, 40])
    monster.set("health", 60)
    monster.set("max_health", 60)

    monster.set("th_stand", "ZombieModule.zombie_stand1")
    monster.set("th_walk", "ZombieModule.zombie_walk1")
    monster.set("th_run", "ZombieModule.zombie_run1")
    monster.set("th_pain", "ZombieModule.zombie_pain")
    monster.set("th_die", "ZombieModule.zombie_die")
    monster.set("th_missile", "ZombieModule.zombie_missile")
    monster.set("allowPathFind", true)
    monster.set("combat_style", CombatStyles.RANGED)

    var spawnflags = monster.get("spawnflags", 0)
    if ((spawnflags & 1) != 0) {
      monster.set("movetype", MoveTypes.NONE)
      ZombieModule.zombie_cruc1(globals, monster)
    } else {
      MonstersModule.walkmonster_start(globals, monster)
    }
  }
}
