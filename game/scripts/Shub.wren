// Shub.wren
// Ports Shub-Niggurath so the final boss sequence executes entirely inside the
// Wren gameplay runtime.

import "./Engine" for Engine
import "./Globals" for Channels, Attenuations, SolidTypes, MoveTypes, DamageValues
import "./Globals" for TempEntityCodes, MessageTypes, ServiceCodes, CombatStyles
import "./Monsters" for MonstersModule
import "./Subs" for SubsModule
import "./Player" for PlayerModule

var _IDLE_FRAMES = [
  "old1", "old2", "old3", "old4", "old5", "old6", "old7", "old8", "old9", "old10",
  "old11", "old12", "old13", "old14", "old15", "old16", "old17", "old18", "old19", "old20",
  "old21", "old22", "old23", "old24", "old25", "old26", "old27", "old28", "old29", "old30",
  "old31", "old32", "old33", "old34", "old35", "old36", "old37", "old38", "old39", "old40",
  "old41", "old42", "old43", "old44", "old45", "old46"
]

var _THRASH_FRAMES = [
  "shake1", "shake2", "shake3", "shake4", "shake5", "shake6", "shake7", "shake8", "shake9", "shake10",
  "shake11", "shake12", "shake13", "shake14", "shake15", "shake16", "shake17", "shake18", "shake19", "shake20"
]

var _THRASH_LIGHTS = [
  "m", "k", "k", "i", "g", "e", "c", "a", "c", "e",
  "g", "i", "k", "m", "m", "g", "c", "b", "a", null
]

class ShubModule {
  static var _shubEntity = null

  static _setFrame(globals, entity, frame, nextFunction, delay) {
    MonstersModule.setFrame(globals, entity, frame, nextFunction, delay)
  }

  static _loopSequence(globals, entity, frames, indexField, nextFunction) {
    if (entity == null) return
    if (frames == null || frames.count == 0) return

    var index = entity.get(indexField, 0)
    if (index < 0 || index >= frames.count) index = 0

    ShubModule._setFrame(globals, entity, frames[index], nextFunction, 0.1)

    index = index + 1
    if (index >= frames.count) index = 0
    entity.set(indexField, index)
  }

  static old_idle(globals, shub) {
    ShubModule._loopSequence(globals, shub, _IDLE_FRAMES, "_shubIdleIndex", "ShubModule.old_idle")
  }

  static old_thrash1(globals, shub) {
    if (shub == null) return
    shub.set("_shubThrashIndex", 0)
    shub.set("_shubThrashRepeats", 0)
    ShubModule.thrashAdvance(globals, shub)
  }

  static thrashAdvance(globals, shub) {
    if (shub == null) return

    var index = shub.get("_shubThrashIndex", 0)
    if (index < 0 || index >= _THRASH_FRAMES.count) index = 0

    var nextName = "ShubModule.thrashAdvance"
    var delay = 0.1

    if (index == 14) {
      var count = shub.get("_shubThrashRepeats", 0) + 1
      shub.set("_shubThrashRepeats", count)
      if (count < 3) {
        nextName = "ShubModule.old_thrash1"
      }
    }

    if (index == _THRASH_FRAMES.count - 1) {
      nextName = "ShubModule.finale_4"
    }

    ShubModule._setFrame(globals, shub, _THRASH_FRAMES[index], nextName, delay)

    var light = _THRASH_LIGHTS[index]
    if (light != null) {
      Engine.lightstyle(0, light)
    }

    if (index == 14 && shub.get("_shubThrashRepeats", 0) < 3) {
      shub.set("_shubThrashIndex", 0)
    } else if (index >= _THRASH_FRAMES.count - 1) {
      shub.set("_shubThrashIndex", 0)
    } else {
      shub.set("_shubThrashIndex", index + 1)
    }
  }

  static finale_1(globals, shub) {
    if (shub == null) return

    globals.intermissionExitTime = globals.time + 10000000.0
    globals.intermissionRunning = 1.0

    var spots = Engine.findAll(globals.world, "info_intermission")
    if (spots == null || spots.count == 0) {
      Engine.objError("no info_intermission")
      return
    }
    var pos = spots[0]

    var trains = Engine.findAll(globals.world, "misc_teleporttrain")
    if (trains != null && trains.count > 0) {
      Engine.removeEntity(trains[0])
    }

    Engine.writeByte(MessageTypes.ALL, ServiceCodes.FINALE, null)
    Engine.writeString(MessageTypes.ALL, "", null)

    var players = Engine.findAll(globals.world, "player")
    var targetOrigin = pos.get("origin", [0, 0, 0])
    var targetAngles = pos.get("mangle", [0, 0, 0])

    for (player in players) {
      if (player == null) continue
      player.set("view_ofs", [0, 0, 0])
      player.set("angles", targetAngles)
      player.set("v_angle", targetAngles)
      player.set("fixangle", true)
      player.set("map", shub.get("map", ""))
      player.set("nextthink", globals.time + 0.5)
      player.set("takedamage", DamageValues.NO)
      player.set("solid", SolidTypes.NOT)
      player.set("movetype", MoveTypes.NONE)
      player.set("modelindex", 0)
      Engine.setOrigin(player, targetOrigin)
    }

    var timer = Engine.spawnEntity()
    timer.set("think", "ShubModule.finale_2")
    timer.set("nextthink", globals.time + 1.0)
    Engine.scheduleThink(timer, "ShubModule.finale_2", 1.0)

    if (globals.campaign != 0 && globals.world.get("model", "") == "maps/end.bsp") {
      Engine.writeByte(MessageTypes.ALL, ServiceCodes.ACHIEVEMENT, null)
      Engine.writeString(MessageTypes.ALL, "ACH_DEFEAT_SHUB", null)
      if (globals.skill == 3) {
        Engine.writeByte(MessageTypes.ALL, ServiceCodes.ACHIEVEMENT, null)
        Engine.writeString(MessageTypes.ALL, "ACH_DEFEAT_SHUB_NIGHTMARE", null)
      }
    }
  }

  static finale_2(globals, timer) {
    var shub = ShubModule._shubEntity
    if (shub == null) return

    var origin = shub.get("origin", [0, 0, 0])
    var effectOrigin = [origin[0], origin[1] - 100, origin[2]]
    Engine.emitTempEntity(TempEntityCodes.TELEPORT, {"origin": effectOrigin})
    Engine.playSound(shub, Channels.VOICE, "misc/r_tele1.wav", 1, Attenuations.NORMAL)

    if (timer != null) {
      timer.set("think", "ShubModule.finale_3")
      timer.set("nextthink", globals.time + 2.0)
      Engine.scheduleThink(timer, "ShubModule.finale_3", 2.0)
    }
  }

  static finale_3(globals, timer) {
    var shub = ShubModule._shubEntity
    if (shub == null) return

    shub.set("think", "ShubModule.old_thrash1")
    shub.set("nextthink", globals.time + 0.1)
    Engine.scheduleThink(shub, "ShubModule.old_thrash1", 0.1)
    Engine.playSound(shub, Channels.VOICE, "boss2/death.wav", 1, Attenuations.NORMAL)
    Engine.lightstyle(0, "abcdefghijklmlkjihgfedcb")
  }

  static finale_4(globals, shub) {
    if (shub == null) return

    Engine.playSound(shub, Channels.VOICE, "boss2/pop2.wav", 1, Attenuations.NORMAL)

    var baseOrigin = shub.get("origin", [0, 0, 0])
    var spawnOrigin = [baseOrigin[0], baseOrigin[1], baseOrigin[2]]

    var previousSelf = globals.self
    globals.self = shub

    var z = 16
    while (z <= 144) {
      var x = -64
      while (x <= 64) {
        var y = -64
        while (y <= 64) {
          spawnOrigin[0] = baseOrigin[0] + x
          spawnOrigin[1] = baseOrigin[1] + y
          spawnOrigin[2] = baseOrigin[2] + z
          Engine.setOrigin(shub, [spawnOrigin[0], spawnOrigin[1], spawnOrigin[2]])

          var r = Engine.random()
          if (r < 0.3) {
            PlayerModule.ThrowGib(globals, shub, "progs/gib1.mdl", -999)
          } else if (r < 0.6) {
            PlayerModule.ThrowGib(globals, shub, "progs/gib2.mdl", -999)
          } else {
            PlayerModule.ThrowGib(globals, shub, "progs/gib3.mdl", -999)
          }

          y = y + 32
        }
        x = x + 32
      }
      z = z + 96
    }

    globals.self = previousSelf

    Engine.writeByte(MessageTypes.ALL, ServiceCodes.FINALE, null)
    Engine.writeString(MessageTypes.ALL, "$qc_finale_end", null)

    var playerModel = Engine.spawnEntity()
    Engine.setModel(playerModel, "progs/player.mdl")
    var modelOrigin = [baseOrigin[0] - 32, baseOrigin[1] - 264, baseOrigin[2]]
    Engine.setOrigin(playerModel, modelOrigin)
    playerModel.set("angles", [0, 290, 0])
    playerModel.set("frame", 1)

    Engine.removeEntity(shub)

    Engine.writeByte(MessageTypes.ALL, ServiceCodes.CDTRACK, null)
    Engine.writeByte(MessageTypes.ALL, 3, null)
    Engine.writeByte(MessageTypes.ALL, 3, null)
    Engine.lightstyle(0, "m")

    ShubModule._shubEntity = null

    var timer = Engine.spawnEntity()
    timer.set("think", "ShubModule.finale_5")
    timer.set("nextthink", globals.time + 1.0)
    Engine.scheduleThink(timer, "ShubModule.finale_5", 1.0)
  }

  static finale_5(globals, timer) {
    if (Engine.finaleFinished()) {
      if (timer != null) {
        timer.set("think", "ShubModule.finale_6")
        timer.set("nextthink", globals.time + 5.0)
        Engine.scheduleThink(timer, "ShubModule.finale_6", 5.0)
      }
    } else {
      if (timer != null) {
        timer.set("think", "ShubModule.finale_5")
        timer.set("nextthink", globals.time + 0.1)
        Engine.scheduleThink(timer, "ShubModule.finale_5", 0.1)
      }
    }
  }

  static finale_6(globals, timer) {
    if (globals.coop == 0) {
      Engine.localCommand("menu_credits\n")
      Engine.localCommand("disconnect\n")
    } else {
      Engine.changeLevel("start")
    }
  }

  static monster_oldone(globals, monster) {
    if (monster == null) return

    if (globals.deathmatch != 0) {
      Engine.removeEntity(monster)
      return
    }

    Engine.precacheModel2("progs/oldone.mdl")
    Engine.precacheSound2("boss2/death.wav")
    Engine.precacheSound2("boss2/idle.wav")
    Engine.precacheSound2("boss2/sight.wav")
    Engine.precacheSound2("boss2/pop2.wav")
    Engine.precacheSound2("misc/r_tele1.wav")

    monster.set("solid", SolidTypes.SLIDEBOX)
    monster.set("movetype", MoveTypes.STEP)
    Engine.setModel(monster, "progs/oldone.mdl")

    monster.set("netname", "$qc_shub")
    monster.set("killstring", "$qc_ks_shub")
    monster.set("noise", "boss2/sight.wav")

    Engine.setSize(monster, [-160, -128, -24], [160, 128, 256])
    monster.set("health", 40000)
    monster.set("max_health", 40000)

    monster.set("think", "ShubModule.old_idle")
    monster.set("nextthink", globals.time + 0.1)
    Engine.scheduleThink(monster, "ShubModule.old_idle", 0.1)

    monster.set("takedamage", DamageValues.YES)
    monster.set("th_pain", "SubsModule.subNull")
    monster.set("th_die", "ShubModule.finale_1")
    monster.set("combat_style", CombatStyles.MELEE)

    monster.set("_shubIdleIndex", 0)
    monster.set("_shubThrashIndex", 0)
    monster.set("_shubThrashRepeats", 0)

    globals.totalMonsters = globals.totalMonsters + 1

    ShubModule._shubEntity = monster
  }
}
