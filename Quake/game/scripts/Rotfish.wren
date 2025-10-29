// Rotfish.wren
// Ports the rotfish monster from rotfish.qc to run entirely inside the Wren
// gameplay runtime.

import "./Engine" for Engine
import "./Globals" for Channels, Attenuations, SolidTypes, MoveTypes, CombatStyles
import "./Monsters" for MonstersModule
import "./AI" for AIModule
import "./Fight" for FightModule
import "./Combat" for CombatModule

var _SWIM_FRAMES = [
  "swim1",
  "swim2",
  "swim3",
  "swim4",
  "swim5",
  "swim6",
  "swim7",
  "swim8",
  "swim9",
  "swim10",
  "swim11",
  "swim12",
  "swim13",
  "swim14",
  "swim15",
  "swim16",
  "swim17",
  "swim18"
]

var _RUN_FRAMES = ["swim1", "swim3", "swim5", "swim7", "swim9", "swim11", "swim13", "swim15", "swim17"]
var _ATTACK_FRAMES = [
  "attack1",
  "attack2",
  "attack3",
  "attack4",
  "attack5",
  "attack6",
  "attack7",
  "attack8",
  "attack9",
  "attack10",
  "attack11",
  "attack12",
  "attack13",
  "attack14",
  "attack15",
  "attack16",
  "attack17",
  "attack18"
]

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
  "death11",
  "death12",
  "death13",
  "death14",
  "death15",
  "death16",
  "death17",
  "death18",
  "death19",
  "death20",
  "death21"
]

var _PAIN_FRAMES = [
  "pain1",
  "pain2",
  "pain3",
  "pain4",
  "pain5",
  "pain6",
  "pain7",
  "pain8",
  "pain9"
]

class RotfishModule {
  static _vectorSub(a, b) {
    return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
  }

  static _vectorLength(v) {
    return (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]).sqrt
  }

  static _setFrame(globals, monster, frame, nextFunction, delay) {
    MonstersModule.setFrame(globals, monster, frame, nextFunction, delay)
  }

  static _loopSequence(globals, monster, frames, indexField, nextFunction, actionFn) {
    if (monster == null) return
    if (frames == null || frames.count == 0) return

    var index = monster.get(indexField, 0)
    if (index < 0 || index >= frames.count) index = 0

    RotfishModule._setFrame(globals, monster, frames[index], nextFunction, 0.1)

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
    RotfishModule._setFrame(globals, monster, frames[index], nextName, delay)

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

  static _fishBite(globals, monster) {
    if (monster == null) return
    var enemy = monster.get("enemy", null)
    if (enemy == null) return

    var delta = RotfishModule._vectorSub(enemy.get("origin", [0, 0, 0]), monster.get("origin", [0, 0, 0]))
    if (RotfishModule._vectorLength(delta) > 60) return

    Engine.playSound(monster, Channels.VOICE, "fish/bite.wav", 1, Attenuations.NORMAL)
    var damage = (Engine.random() + Engine.random()) * 3
    CombatModule.tDamage(globals, enemy, monster, monster, damage)
  }

  static fish_stand1(globals, monster) {
    RotfishModule._loopSequence(globals, monster, _SWIM_FRAMES, "_fishStandIndex", "RotfishModule.fish_stand1", Fn.new { |_|
      AIModule.ai_stand(globals, monster)
    })
  }

  static fish_walk1(globals, monster) {
    RotfishModule._loopSequence(globals, monster, _SWIM_FRAMES, "_fishWalkIndex", "RotfishModule.fish_walk1", Fn.new { |_|
      AIModule.ai_walk(globals, monster, 8)
    })
  }

  static fish_run1(globals, monster) {
    RotfishModule._loopSequence(globals, monster, _RUN_FRAMES, "_fishRunIndex", "RotfishModule.fish_run1", Fn.new { |index|
      if (index == 0 && Engine.random() < 0.5) {
        Engine.playSound(monster, Channels.VOICE, "fish/idle.wav", 1, Attenuations.NORMAL)
      }
      AIModule.ai_run(globals, monster, 12)
    })
  }

  static _fish_attackAdvance(globals, monster) {
    var actions = [
      Fn.new { |g, m| FightModule.ai_charge(g, m, 10) },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 10) },
      Fn.new { |g, m| RotfishModule._fishBite(g, m) },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 10) },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 10) },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 10) },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 10) },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 10) },
      Fn.new { |g, m| RotfishModule._fishBite(g, m) },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 10) },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 10) },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 10) },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 10) },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 10) },
      Fn.new { |g, m| RotfishModule._fishBite(g, m) },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 10) },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 10) },
      Fn.new { |g, m| FightModule.ai_charge(g, m, 10) }
    ]

    RotfishModule._advanceSequence(globals, monster, _ATTACK_FRAMES, actions, "_fishAttackIndex", "RotfishModule._fish_attackAdvance", "RotfishModule.fish_run1")
  }

  static fish_attack1(globals, monster) {
    monster.set("_fishAttackIndex", 0)
    RotfishModule._fish_attackAdvance(globals, monster)
  }

  static _fish_painAdvance(globals, monster) {
    var actions = [
      null,
      Fn.new { |g, m| AIModule.ai_pain(g, m, 6) },
      Fn.new { |g, m| AIModule.ai_pain(g, m, 6) },
      Fn.new { |g, m| AIModule.ai_pain(g, m, 6) },
      Fn.new { |g, m| AIModule.ai_pain(g, m, 6) },
      Fn.new { |g, m| AIModule.ai_pain(g, m, 6) },
      Fn.new { |g, m| AIModule.ai_pain(g, m, 6) },
      Fn.new { |g, m| AIModule.ai_pain(g, m, 6) },
      Fn.new { |g, m| AIModule.ai_pain(g, m, 6) }
    ]

    RotfishModule._advanceSequence(globals, monster, _PAIN_FRAMES, actions, "_fishPainIndex", "RotfishModule._fish_painAdvance", "RotfishModule.fish_run1")
  }

  static fish_pain(globals, monster, attacker, damage) {
    monster.set("_fishPainIndex", 0)
    RotfishModule._fish_painAdvance(globals, monster)
  }

  static _fish_deathAdvance(globals, monster) {
    var actions = [
      Fn.new { |g, m| Engine.playSound(m, Channels.VOICE, "fish/death.wav", 1, Attenuations.NORMAL) },
      Fn.new { |g, m| m.set("solid", SolidTypes.NOT) }
    ]

    RotfishModule._advanceSequence(globals, monster, _DEATH_FRAMES, actions, "_fishDeathIndex", "RotfishModule._fish_deathAdvance", null)
  }

  static fish_die(globals, monster) {
    if (monster == null) return
    monster.set("_fishDeathIndex", 0)
    RotfishModule._fish_deathAdvance(globals, monster)
  }

  static monster_fish(globals, monster) {
    if (monster == null) return

    if (globals.deathmatch != 0) {
      Engine.removeEntity(monster)
      return
    }

    Engine.precacheModel2("progs/fish.mdl")
    Engine.precacheSound2("fish/death.wav")
    Engine.precacheSound2("fish/bite.wav")
    Engine.precacheSound2("fish/idle.wav")

    monster.set("solid", SolidTypes.SLIDEBOX)
    monster.set("movetype", MoveTypes.STEP)
    Engine.setModel(monster, "progs/fish.mdl")

    monster.set("noise", "fish/idle.wav")
    monster.set("netname", "$qc_rotfish")
    monster.set("killstring", "$qc_ks_rotfish")

    Engine.setSize(monster, [-16, -16, -24], [16, 16, 24])
    monster.set("health", 25)
    monster.set("max_health", 25)

    monster.set("th_stand", "RotfishModule.fish_stand1")
    monster.set("th_walk", "RotfishModule.fish_walk1")
    monster.set("th_run", "RotfishModule.fish_run1")
    monster.set("th_melee", "RotfishModule.fish_attack1")
    monster.set("th_die", "RotfishModule.fish_die")
    monster.set("th_pain", "RotfishModule.fish_pain")

    monster.set("combat_style", CombatStyles.MELEE)

    monster.set("_fishStandIndex", 0)
    monster.set("_fishWalkIndex", 0)
    monster.set("_fishRunIndex", 0)
    monster.set("_fishAttackIndex", 0)
    monster.set("_fishPainIndex", 0)
    monster.set("_fishDeathIndex", 0)

    MonstersModule.swimmonster_start(globals, monster)
  }
}
