// Player.wren
// Player-specific helpers that mirror functionality from player.qc.

import "./Engine" for Engine
import "./Globals" for SolidTypes, MoveTypes, DeadFlags, Items, PlayerFlags
import "./Globals" for Channels, Attenuations, DamageValues, Contents, Effects
import "./Subs" for SubsModule
import "./Weapons" for WeaponsModule

var _STAND_FRAMES = ["stand1", "stand2", "stand3", "stand4", "stand5"]
var _AXE_STAND_FRAMES = [
  "axstnd1",
  "axstnd2",
  "axstnd3",
  "axstnd4",
  "axstnd5",
  "axstnd6",
  "axstnd7",
  "axstnd8",
  "axstnd9",
  "axstnd10",
  "axstnd11",
  "axstnd12"
]
var _RUN_FRAMES = [
  "rockrun1",
  "rockrun2",
  "rockrun3",
  "rockrun4",
  "rockrun5",
  "rockrun6"
]
var _AXE_RUN_FRAMES = [
  "axrun1",
  "axrun2",
  "axrun3",
  "axrun4",
  "axrun5",
  "axrun6"
]
var _SHOT_FRAMES = [
  "shotatt1",
  "shotatt2",
  "shotatt3",
  "shotatt4",
  "shotatt5",
  "shotatt6"
]
var _ROCKET_FRAMES = [
  "rockatt1",
  "rockatt2",
  "rockatt3",
  "rockatt4",
  "rockatt5",
  "rockatt6"
]
var _NAIL_FRAMES = ["nailatt1", "nailatt2"]
var _LIGHT_FRAMES = ["light1", "light2"]
var _AXE_A_FRAMES = ["axatt1", "axatt2", "axatt3", "axatt4"]
var _AXE_B_FRAMES = ["axattb1", "axattb2", "axattb3", "axattb4"]
var _AXE_C_FRAMES = ["axattc1", "axattc2", "axattc3", "axattc4"]
var _AXE_D_FRAMES = ["axattd1", "axattd2", "axattd3", "axattd4"]
var _PAIN_FRAMES = ["pain1", "pain2", "pain3", "pain4", "pain5", "pain6"]
var _AXPAIN_FRAMES = [
  "axpain1",
  "axpain2",
  "axpain3",
  "axpain4",
  "axpain5",
  "axpain6"
]
var _DEATH_A_FRAMES = [
  "deatha1",
  "deatha2",
  "deatha3",
  "deatha4",
  "deatha5",
  "deatha6",
  "deatha7",
  "deatha8",
  "deatha9",
  "deatha10",
  "deatha11"
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
var _DEATH_C_FRAMES = [
  "deathc1",
  "deathc2",
  "deathc3",
  "deathc4",
  "deathc5",
  "deathc6",
  "deathc7",
  "deathc8",
  "deathc9",
  "deathc10",
  "deathc11",
  "deathc12",
  "deathc13",
  "deathc14",
  "deathc15"
]
var _DEATH_D_FRAMES = [
  "deathd1",
  "deathd2",
  "deathd3",
  "deathd4",
  "deathd5",
  "deathd6",
  "deathd7",
  "deathd8",
  "deathd9"
]
var _DEATH_E_FRAMES = [
  "deathe1",
  "deathe2",
  "deathe3",
  "deathe4",
  "deathe5",
  "deathe6",
  "deathe7",
  "deathe8",
  "deathe9"
]
var _DEATH_AX_FRAMES = [
  "axdeth1",
  "axdeth2",
  "axdeth3",
  "axdeth4",
  "axdeth5",
  "axdeth6",
  "axdeth7",
  "axdeth8",
  "axdeth9"
]

class PlayerModule {
  static setSuicideFrame(globals, player) {
    if (player.get("model", "") != "progs/player.mdl") {
      return
    }

    player.set("frame", "deatha11")
    player.set("solid", SolidTypes.NOT)
    player.set("movetype", MoveTypes.TOSS)
    player.set("deadflag", DeadFlags.DEAD)
    player.set("nextthink", -1)
  }

  static set_suicide_frame(globals, player) {
    PlayerModule.setSuicideFrame(globals, player)
  }

  static SetSuicideFrame(globals, player) {
    PlayerModule.setSuicideFrame(globals, player)
  }

  static playerDead(player) {
    if (player == null) return
    PlayerModule._playerDead(player)
  }

  static PlayerDead(player) {
    PlayerModule.playerDead(player)
  }

  static deathSound(globals, player) {
    PlayerModule._deathSound(globals, player)
  }

  static DeathSound(globals, player) {
    PlayerModule.deathSound(globals, player)
  }

  static painSound(globals, player) {
    PlayerModule._painSound(globals, player)
  }

  static PainSound(globals, player) {
    PlayerModule.painSound(globals, player)
  }

  static gibPlayer(globals, player) {
    PlayerModule._gibPlayer(globals, player)
  }

  static GibPlayer(globals, player) {
    PlayerModule.gibPlayer(globals, player)
  }

  static velocityForDamage(damage) {
    return PlayerModule._velocityForDamage(damage)
  }

  static VelocityForDamage(damage) {
    return PlayerModule.velocityForDamage(damage)
  }

  static throwGib(globals, player, model, damage) {
    if (player == null) return
    var previousSelf = globals.self
    globals.self = player
    PlayerModule._throwGib(globals, player, model, damage)
    globals.self = previousSelf
  }

  static ThrowGib(globals, player, model, damage) {
    PlayerModule.throwGib(globals, player, model, damage)
  }

  static throwHead(globals, player, model, damage) {
    if (player == null) return
    var previousSelf = globals.self
    globals.self = player
    PlayerModule._throwHead(globals, player, model, damage)
    globals.self = previousSelf
  }

  static ThrowHead(globals, player, model, damage) {
    PlayerModule.throwHead(globals, player, model, damage)
  }

  static player_stand1(globals, player) {
    if (player == null) return
    player.set("weaponframe", 0)
    var velocity = player.get("velocity", [0, 0, 0])
    if (velocity[0] != 0 || velocity[1] != 0) {
      player.set("walkframe", 0)
      PlayerModule.player_run(globals, player)
      return
    }

    var frames = player.get("weapon", Items.AXE) == Items.AXE ? _AXE_STAND_FRAMES : _STAND_FRAMES
    PlayerModule._loopFrames(player, frames, "walkframe", "player_stand1")
  }

  static player_run(globals, player) {
    if (player == null) return
    player.set("weaponframe", 0)
    var velocity = player.get("velocity", [0, 0, 0])
    if (velocity[0] == 0 && velocity[1] == 0) {
      player.set("walkframe", 0)
      PlayerModule.player_stand1(globals, player)
      return
    }

    var frames = player.get("weapon", Items.AXE) == Items.AXE ? _AXE_RUN_FRAMES : _RUN_FRAMES
    PlayerModule._loopFrames(player, frames, "walkframe", "player_run")
  }

  static player_shot1(globals, player) {
    if (player == null) return
    player.set("weaponframe", 1)
    PlayerModule._addMuzzleFlash(player)
    PlayerModule._setAnimationFrame(player, _SHOT_FRAMES[0], "player_shot2")
  }

  static player_shot2(globals, player) {
    if (player == null) return
    player.set("weaponframe", 2)
    PlayerModule._setAnimationFrame(player, _SHOT_FRAMES[1], "player_shot3")
  }

  static player_shot3(globals, player) {
    if (player == null) return
    player.set("weaponframe", 3)
    PlayerModule._setAnimationFrame(player, _SHOT_FRAMES[2], "player_shot4")
  }

  static player_shot4(globals, player) {
    if (player == null) return
    player.set("weaponframe", 4)
    PlayerModule._setAnimationFrame(player, _SHOT_FRAMES[3], "player_shot5")
  }

  static player_shot5(globals, player) {
    if (player == null) return
    player.set("weaponframe", 5)
    PlayerModule._setAnimationFrame(player, _SHOT_FRAMES[4], "player_shot6")
  }

  static player_shot6(globals, player) {
    if (player == null) return
    player.set("weaponframe", 6)
    PlayerModule._setAnimationFrame(player, _SHOT_FRAMES[5], "player_run")
  }

  static player_axe1(globals, player) {
    if (player == null) return
    player.set("weaponframe", 1)
    PlayerModule._setAnimationFrame(player, _AXE_A_FRAMES[0], "player_axe2")
  }

  static player_axe2(globals, player) {
    if (player == null) return
    player.set("weaponframe", 2)
    PlayerModule._setAnimationFrame(player, _AXE_A_FRAMES[1], "player_axe3")
  }

  static player_axe3(globals, player) {
    if (player == null) return
    player.set("weaponframe", 3)
    WeaponsModule.startAxeAttack(globals, player)
    PlayerModule._setAnimationFrame(player, _AXE_A_FRAMES[2], "player_axe4")
  }

  static player_axe4(globals, player) {
    if (player == null) return
    player.set("weaponframe", 4)
    PlayerModule._setAnimationFrame(player, _AXE_A_FRAMES[3], "player_run")
  }

  static player_axeb1(globals, player) {
    if (player == null) return
    player.set("weaponframe", 5)
    PlayerModule._setAnimationFrame(player, _AXE_B_FRAMES[0], "player_axeb2")
  }

  static player_axeb2(globals, player) {
    if (player == null) return
    player.set("weaponframe", 6)
    PlayerModule._setAnimationFrame(player, _AXE_B_FRAMES[1], "player_axeb3")
  }

  static player_axeb3(globals, player) {
    if (player == null) return
    player.set("weaponframe", 7)
    WeaponsModule.startAxeAttack(globals, player)
    PlayerModule._setAnimationFrame(player, _AXE_B_FRAMES[2], "player_axeb4")
  }

  static player_axeb4(globals, player) {
    if (player == null) return
    player.set("weaponframe", 8)
    PlayerModule._setAnimationFrame(player, _AXE_B_FRAMES[3], "player_run")
  }

  static player_axec1(globals, player) {
    if (player == null) return
    player.set("weaponframe", 1)
    PlayerModule._setAnimationFrame(player, _AXE_C_FRAMES[0], "player_axec2")
  }

  static player_axec2(globals, player) {
    if (player == null) return
    player.set("weaponframe", 2)
    PlayerModule._setAnimationFrame(player, _AXE_C_FRAMES[1], "player_axec3")
  }

  static player_axec3(globals, player) {
    if (player == null) return
    player.set("weaponframe", 3)
    WeaponsModule.startAxeAttack(globals, player)
    PlayerModule._setAnimationFrame(player, _AXE_C_FRAMES[2], "player_axec4")
  }

  static player_axec4(globals, player) {
    if (player == null) return
    player.set("weaponframe", 4)
    PlayerModule._setAnimationFrame(player, _AXE_C_FRAMES[3], "player_run")
  }

  static player_axed1(globals, player) {
    if (player == null) return
    player.set("weaponframe", 5)
    PlayerModule._setAnimationFrame(player, _AXE_D_FRAMES[0], "player_axed2")
  }

  static player_axed2(globals, player) {
    if (player == null) return
    player.set("weaponframe", 6)
    PlayerModule._setAnimationFrame(player, _AXE_D_FRAMES[1], "player_axed3")
  }

  static player_axed3(globals, player) {
    if (player == null) return
    player.set("weaponframe", 7)
    WeaponsModule.startAxeAttack(globals, player)
    PlayerModule._setAnimationFrame(player, _AXE_D_FRAMES[2], "player_axed4")
  }

  static player_axed4(globals, player) {
    if (player == null) return
    player.set("weaponframe", 8)
    PlayerModule._setAnimationFrame(player, _AXE_D_FRAMES[3], "player_run")
  }

  static player_nail1(globals, player) {
    if (player == null) return
    PlayerModule._addMuzzleFlash(player)
    if (!player.get("button0", false)) {
      PlayerModule.player_run(globals, player)
      return
    }

    var frame = player.get("weaponframe", 0) + 1
    if (frame >= 9) frame = 1
    player.set("weaponframe", frame)
    WeaponsModule.superDamageSound(globals, player)
    WeaponsModule._fireSpikes(globals, player)
    player.set("attack_finished", Engine.time() + 0.2)
    PlayerModule._setAnimationFrame(player, _NAIL_FRAMES[0], "player_nail2")
  }

  static player_nail2(globals, player) {
    if (player == null) return
    PlayerModule._addMuzzleFlash(player)
    if (!player.get("button0", false)) {
      PlayerModule.player_run(globals, player)
      return
    }

    var frame = player.get("weaponframe", 0) + 1
    if (frame >= 9) frame = 1
    player.set("weaponframe", frame)
    WeaponsModule.superDamageSound(globals, player)
    WeaponsModule._fireSpikes(globals, player)
    player.set("attack_finished", Engine.time() + 0.2)
    PlayerModule._setAnimationFrame(player, _NAIL_FRAMES[1], "player_nail1")
  }

  static player_light1(globals, player) {
    if (player == null) return
    PlayerModule._addMuzzleFlash(player)
    if (!player.get("button0", false)) {
      PlayerModule.player_run(globals, player)
      return
    }

    var frame = player.get("weaponframe", 0) + 1
    if (frame >= 5) frame = 1
    player.set("weaponframe", frame)
    WeaponsModule.superDamageSound(globals, player)
    WeaponsModule.startLightningAttack(globals, player)
    player.set("attack_finished", Engine.time() + 0.2)
    PlayerModule._setAnimationFrame(player, _LIGHT_FRAMES[0], "player_light2")
  }

  static player_light2(globals, player) {
    if (player == null) return
    PlayerModule._addMuzzleFlash(player)
    if (!player.get("button0", false)) {
      PlayerModule.player_run(globals, player)
      return
    }

    var frame = player.get("weaponframe", 0) + 1
    if (frame >= 5) frame = 1
    player.set("weaponframe", frame)
    WeaponsModule.superDamageSound(globals, player)
    WeaponsModule.startLightningAttack(globals, player)
    player.set("attack_finished", Engine.time() + 0.2)
    PlayerModule._setAnimationFrame(player, _LIGHT_FRAMES[1], "player_light1")
  }

  static player_rocket1(globals, player) {
    if (player == null) return
    player.set("weaponframe", 1)
    PlayerModule._addMuzzleFlash(player)
    PlayerModule._setAnimationFrame(player, _ROCKET_FRAMES[0], "player_rocket2")
  }

  static player_rocket2(globals, player) {
    if (player == null) return
    player.set("weaponframe", 2)
    PlayerModule._setAnimationFrame(player, _ROCKET_FRAMES[1], "player_rocket3")
  }

  static player_rocket3(globals, player) {
    if (player == null) return
    player.set("weaponframe", 3)
    PlayerModule._setAnimationFrame(player, _ROCKET_FRAMES[2], "player_rocket4")
  }

  static player_rocket4(globals, player) {
    if (player == null) return
    player.set("weaponframe", 4)
    PlayerModule._setAnimationFrame(player, _ROCKET_FRAMES[3], "player_rocket5")
  }

  static player_rocket5(globals, player) {
    if (player == null) return
    player.set("weaponframe", 5)
    PlayerModule._setAnimationFrame(player, _ROCKET_FRAMES[4], "player_rocket6")
  }

  static player_rocket6(globals, player) {
    if (player == null) return
    player.set("weaponframe", 6)
    PlayerModule._setAnimationFrame(player, _ROCKET_FRAMES[5], "player_run")
  }

  static player_pain1(globals, player) {
    if (player == null) return
    player.set("weaponframe", 0)
    PlayerModule._painSound(globals, player)
    PlayerModule._setAnimationFrame(player, _PAIN_FRAMES[0], "player_pain2")
  }

  static player_pain2(globals, player) {
    if (player == null) return
    PlayerModule._setAnimationFrame(player, _PAIN_FRAMES[1], "player_pain3")
  }

  static player_pain3(globals, player) {
    if (player == null) return
    PlayerModule._setAnimationFrame(player, _PAIN_FRAMES[2], "player_pain4")
  }

  static player_pain4(globals, player) {
    if (player == null) return
    PlayerModule._setAnimationFrame(player, _PAIN_FRAMES[3], "player_pain5")
  }

  static player_pain5(globals, player) {
    if (player == null) return
    PlayerModule._setAnimationFrame(player, _PAIN_FRAMES[4], "player_pain6")
  }

  static player_pain6(globals, player) {
    if (player == null) return
    PlayerModule._setAnimationFrame(player, _PAIN_FRAMES[5], "player_run")
  }

  static player_axpain1(globals, player) {
    if (player == null) return
    player.set("weaponframe", 0)
    PlayerModule._painSound(globals, player)
    PlayerModule._setAnimationFrame(player, _AXPAIN_FRAMES[0], "player_axpain2")
  }

  static player_axpain2(globals, player) {
    if (player == null) return
    PlayerModule._setAnimationFrame(player, _AXPAIN_FRAMES[1], "player_axpain3")
  }

  static player_axpain3(globals, player) {
    if (player == null) return
    PlayerModule._setAnimationFrame(player, _AXPAIN_FRAMES[2], "player_axpain4")
  }

  static player_axpain4(globals, player) {
    if (player == null) return
    PlayerModule._setAnimationFrame(player, _AXPAIN_FRAMES[3], "player_axpain5")
  }

  static player_axpain5(globals, player) {
    if (player == null) return
    PlayerModule._setAnimationFrame(player, _AXPAIN_FRAMES[4], "player_axpain6")
  }

  static player_axpain6(globals, player) {
    if (player == null) return
    PlayerModule._setAnimationFrame(player, _AXPAIN_FRAMES[5], "player_run")
  }

  static player_diea1(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_A_FRAMES, 0, "player_diea2")
  }

  static player_diea2(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_A_FRAMES, 1, "player_diea3")
  }

  static player_diea3(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_A_FRAMES, 2, "player_diea4")
  }

  static player_diea4(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_A_FRAMES, 3, "player_diea5")
  }

  static player_diea5(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_A_FRAMES, 4, "player_diea6")
  }

  static player_diea6(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_A_FRAMES, 5, "player_diea7")
  }

  static player_diea7(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_A_FRAMES, 6, "player_diea8")
  }

  static player_diea8(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_A_FRAMES, 7, "player_diea9")
  }

  static player_diea9(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_A_FRAMES, 8, "player_diea10")
  }

  static player_diea10(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_A_FRAMES, 9, "player_diea11")
  }

  static player_diea11(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_A_FRAMES, 10, null)
    PlayerModule._playerDead(player)
  }

  static player_dieb1(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_B_FRAMES, 0, "player_dieb2")
  }

  static player_dieb2(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_B_FRAMES, 1, "player_dieb3")
  }

  static player_dieb3(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_B_FRAMES, 2, "player_dieb4")
  }

  static player_dieb4(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_B_FRAMES, 3, "player_dieb5")
  }

  static player_dieb5(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_B_FRAMES, 4, "player_dieb6")
  }

  static player_dieb6(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_B_FRAMES, 5, "player_dieb7")
  }

  static player_dieb7(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_B_FRAMES, 6, "player_dieb8")
  }

  static player_dieb8(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_B_FRAMES, 7, "player_dieb9")
  }

  static player_dieb9(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_B_FRAMES, 8, null)
    PlayerModule._playerDead(player)
  }

  static player_diec1(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_C_FRAMES, 0, "player_diec2")
  }

  static player_diec2(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_C_FRAMES, 1, "player_diec3")
  }

  static player_diec3(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_C_FRAMES, 2, "player_diec4")
  }

  static player_diec4(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_C_FRAMES, 3, "player_diec5")
  }

  static player_diec5(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_C_FRAMES, 4, "player_diec6")
  }

  static player_diec6(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_C_FRAMES, 5, "player_diec7")
  }

  static player_diec7(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_C_FRAMES, 6, "player_diec8")
  }

  static player_diec8(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_C_FRAMES, 7, "player_diec9")
  }

  static player_diec9(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_C_FRAMES, 8, "player_diec10")
  }

  static player_diec10(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_C_FRAMES, 9, "player_diec11")
  }

  static player_diec11(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_C_FRAMES, 10, "player_diec12")
  }

  static player_diec12(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_C_FRAMES, 11, "player_diec13")
  }

  static player_diec13(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_C_FRAMES, 12, "player_diec14")
  }

  static player_diec14(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_C_FRAMES, 13, "player_diec15")
  }

  static player_diec15(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_C_FRAMES, 14, null)
    PlayerModule._playerDead(player)
  }

  static player_died1(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_D_FRAMES, 0, "player_died2")
  }

  static player_died2(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_D_FRAMES, 1, "player_died3")
  }

  static player_died3(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_D_FRAMES, 2, "player_died4")
  }

  static player_died4(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_D_FRAMES, 3, "player_died5")
  }

  static player_died5(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_D_FRAMES, 4, "player_died6")
  }

  static player_died6(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_D_FRAMES, 5, "player_died7")
  }

  static player_died7(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_D_FRAMES, 6, "player_died8")
  }

  static player_died8(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_D_FRAMES, 7, "player_died9")
  }

  static player_died9(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_D_FRAMES, 8, null)
    PlayerModule._playerDead(player)
  }

  static player_diee1(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_E_FRAMES, 0, "player_diee2")
  }

  static player_diee2(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_E_FRAMES, 1, "player_diee3")
  }

  static player_diee3(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_E_FRAMES, 2, "player_diee4")
  }

  static player_diee4(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_E_FRAMES, 3, "player_diee5")
  }

  static player_diee5(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_E_FRAMES, 4, "player_diee6")
  }

  static player_diee6(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_E_FRAMES, 5, "player_diee7")
  }

  static player_diee7(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_E_FRAMES, 6, "player_diee8")
  }

  static player_diee8(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_E_FRAMES, 7, "player_diee9")
  }

  static player_diee9(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_E_FRAMES, 8, null)
    PlayerModule._playerDead(player)
  }

  static player_die_ax1(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_AX_FRAMES, 0, "player_die_ax2")
  }

  static player_die_ax2(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_AX_FRAMES, 1, "player_die_ax3")
  }

  static player_die_ax3(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_AX_FRAMES, 2, "player_die_ax4")
  }

  static player_die_ax4(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_AX_FRAMES, 3, "player_die_ax5")
  }

  static player_die_ax5(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_AX_FRAMES, 4, "player_die_ax6")
  }

  static player_die_ax6(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_AX_FRAMES, 5, "player_die_ax7")
  }

  static player_die_ax7(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_AX_FRAMES, 6, "player_die_ax8")
  }

  static player_die_ax8(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_AX_FRAMES, 7, "player_die_ax9")
  }

  static player_die_ax9(globals, player) {
    PlayerModule._playSequenceFrame(player, _DEATH_AX_FRAMES, 8, null)
    PlayerModule._playerDead(player)
  }

  static playerPain(globals, player, attacker, damage) {
    if (player.get("weaponframe", 0) != 0) return
    if (player.get("invisible_finished", 0.0) > Engine.time()) return

    if (player.get("weapon", Items.AXE) == Items.AXE) {
      PlayerModule.player_axpain1(globals, player)
    } else {
      PlayerModule.player_pain1(globals, player)
    }
  }

  static player_pain(globals, player, attacker, damage) {
    PlayerModule.playerPain(globals, player, attacker, damage)
  }

  static PlayerPain(globals, player, attacker, damage) {
    PlayerModule.playerPain(globals, player, attacker, damage)
  }

  static playerDie(globals, player) {
    var removeMask = Engine.bitOrMany([
      Items.INVISIBILITY,
      Items.INVULNERABILITY,
      Items.SUIT,
      Items.QUAD
    ])

    var items = player.get("items", 0)
    items = items - Engine.bitAnd(items, removeMask)
    player.set("items", items)

    player.set("invisible_finished", 0)
    player.set("invincible_finished", 0)
    player.set("super_damage_finished", 0)
    player.set("radsuit_finished", 0)
    player.set("effects", 0)
    player.set("modelindex", globals.modelIndexPlayer)

    if (globals.deathmatch > 0 || globals.coop > 0) {
      PlayerModule._dropBackpack(globals, player)
    }

    player.set("weaponmodel", "")
    player.set("view_ofs", [0, 0, -8])
    player.set("deadflag", DeadFlags.DYING)
    player.set("solid", SolidTypes.NOT)

    var flags = player.get("flags", 0)
    flags = flags - Engine.bitAnd(flags, PlayerFlags.ONGROUND)
    player.set("flags", flags)

    player.set("movetype", MoveTypes.TOSS)

    var velocity = player.get("velocity", [0, 0, 0])
    if (velocity[2] < 10) {
      velocity = [velocity[0], velocity[1], velocity[2] + Engine.random() * 300]
    }
    player.set("velocity", velocity)

    if (player.get("health", 0) < -40) {
      PlayerModule._gibPlayer(globals, player)
      return
    }

    PlayerModule._deathSound(globals, player)

    var angles = player.get("angles", [0, 0, 0])
    player.set("angles", [0, angles[1], 0])

    if (player.get("weapon", Items.AXE) == Items.AXE) {
      PlayerModule.player_die_ax1(globals, player)
      return
    }

    var deathAnimations = [
      "player_diea1",
      "player_dieb1",
      "player_diec1",
      "player_died1",
      "player_diee1"
    ]
    var choice = PlayerModule._randomChoice(deathAnimations)
    if (choice == null) choice = "player_diea1"
    PlayerModule._startDeathAnimation(globals, player, choice)
  }

  static PlayerDie(globals, player) {
    PlayerModule.playerDie(globals, player)
  }

  static backpackTouch(globals, backpack, other) {
    if (other == null) return
    if (other.get("classname", "") != "player") return
    if (other.get("health", 0) <= 0) return

    Engine.playerPrint(other, "$qc_backpack_got", [])

    var backpackItems = backpack.get("items", 0)
    if (backpackItems != 0 && Engine.bitAnd(other.get("items", 0), backpackItems) == 0) {
      var netname = backpack.get("netname", null)
      if (netname != null && netname != "") {
        Engine.playerPrint(other, netname, [])
      }
    }

    var bestBefore = WeaponsModule.bestWeapon(globals, other)
    var oldWeapon = other.get("weapon", Items.AXE)

    other.set("ammo_shells", other.get("ammo_shells", 0) + backpack.get("ammo_shells", 0))
    other.set("ammo_nails", other.get("ammo_nails", 0) + backpack.get("ammo_nails", 0))
    other.set("ammo_rockets", other.get("ammo_rockets", 0) + backpack.get("ammo_rockets", 0))
    other.set("ammo_cells", other.get("ammo_cells", 0) + backpack.get("ammo_cells", 0))

    PlayerModule._boundAmmo(other)

    if (backpack.get("ammo_shells", 0) > 0) {
      Engine.playerPrint(other, "$qc_backpack_shells", [backpack.get("ammo_shells", 0).toString])
    }
    if (backpack.get("ammo_nails", 0) > 0) {
      Engine.playerPrint(other, "$qc_backpack_nails", [backpack.get("ammo_nails", 0).toString])
    }
    if (backpack.get("ammo_rockets", 0) > 0) {
      Engine.playerPrint(other, "$qc_backpack_rockets", [backpack.get("ammo_rockets", 0).toString])
    }
    if (backpack.get("ammo_cells", 0) > 0) {
      Engine.playerPrint(other, "$qc_backpack_cells", [backpack.get("ammo_cells", 0).toString])
    }

    var items = other.get("items", 0)
    if (backpackItems != 0) {
      items = Engine.bitOr(items, backpackItems)
      other.set("items", items)
    }

    Engine.playSound(other, Channels.ITEM, "weapons/lock4.wav", 1, Attenuations.NORMAL)
    Engine.stuffCommand(other, "bf\n")

    Engine.removeEntity(backpack)

    var newWeapon = backpackItems != 0 ? backpackItems : oldWeapon
    var bestAfter = WeaponsModule.bestWeapon(globals, other)
    if (WeaponsModule.wantsToChangeWeapon(globals, other, oldWeapon, newWeapon) && bestAfter != bestBefore) {
      other.set("weapon", bestAfter)
    } else {
      other.set("weapon", oldWeapon)
    }

    WeaponsModule.setCurrentAmmo(globals, other)
  }

  static deathBubbles(globals, player, count) {
    if (count <= 0) return

    var spawner = Engine.spawnEntity()
    var origin = player.get("origin", [0, 0, 0])
    Engine.setOrigin(spawner, origin)
    spawner.set("origin", origin)
    spawner.set("movetype", MoveTypes.NONE)
    spawner.set("solid", SolidTypes.NOT)
    spawner.set("think", "PlayerModule.deathBubblesSpawn")
    spawner.set("nextthink", Engine.time() + 0.1)
    Engine.scheduleThink(spawner, "PlayerModule.deathBubblesSpawn", 0.1)
    spawner.set("air_finished", 0)
    spawner.set("owner", player)
    spawner.set("bubble_count", count)
  }

  static deathBubblesSpawn(globals, spawner) {
    if (spawner == null) return

    var owner = spawner.get("owner", null)
    if (owner == null || owner.get("waterlevel", 0) != 3) {
      Engine.removeEntity(spawner)
      return
    }

    var bubble = Engine.spawnEntity()
    Engine.setModel(bubble, "progs/s_bubble.spr")
    var spawnOrigin = PlayerModule._vectorAdd(owner.get("origin", [0, 0, 0]), [0, 0, 24])
    Engine.setOrigin(bubble, spawnOrigin)
    bubble.set("origin", spawnOrigin)
    bubble.set("movetype", MoveTypes.NOCLIP)
    bubble.set("solid", SolidTypes.NOT)
    bubble.set("velocity", [0, 0, 15])
    bubble.set("classname", "bubble")
    bubble.set("frame", 0)
    bubble.set("cnt", 0)
    Engine.setSize(bubble, [-8, -8, -8], [8, 8, 8])
    bubble.set("think", "PlayerModule.bubble_bob")
    var nextThink = Engine.time() + 0.5
    bubble.set("nextthink", nextThink)
    Engine.scheduleThink(bubble, "PlayerModule.bubble_bob", 0.5)

    var produced = spawner.get("air_finished", 0) + 1
    spawner.set("air_finished", produced)

    if (produced >= spawner.get("bubble_count", 0)) {
      Engine.removeEntity(spawner)
      return
    }

    spawner.set("nextthink", Engine.time() + 0.1)
    Engine.scheduleThink(spawner, "PlayerModule.deathBubblesSpawn", 0.1)
  }

  static DeathBubbles(globals, player, count) {
    PlayerModule.deathBubbles(globals, player, count)
  }

  static DeathBubblesSpawn(globals, spawner) {
    PlayerModule.deathBubblesSpawn(globals, spawner)
  }

  static bubble_bob(globals, bubble) {
    if (bubble == null) return

    var count = bubble.get("cnt", 0) + 1
    bubble.set("cnt", count)

    if (count == 4) {
      PlayerModule._bubbleSplit(globals, bubble)
    }

    if (count >= 20) {
      Engine.removeEntity(bubble)
      return
    }

    var velocity = bubble.get("velocity", [0, 0, 0])
    var rnd1 = velocity[0] + (-10 + Engine.random() * 20)
    var rnd2 = velocity[1] + (-10 + Engine.random() * 20)
    var rnd3 = velocity[2] + 10 + Engine.random() * 10

    if (rnd1 > 10) rnd1 = 5
    if (rnd1 < -10) rnd1 = -5
    if (rnd2 > 10) rnd2 = 5
    if (rnd2 < -10) rnd2 = -5
    if (rnd3 < 10) rnd3 = 15
    if (rnd3 > 30) rnd3 = 25

    bubble.set("velocity", [rnd1, rnd2, rnd3])

    var nextThink = Engine.time() + 0.5
    bubble.set("nextthink", nextThink)
    Engine.scheduleThink(bubble, "PlayerModule.bubble_bob", 0.5)
  }

  static _loopFrames(player, frames, indexField, nextName) {
    if (player == null) return
    if (frames == null || frames.count == 0) return

    var index = player.get(indexField, 0)
    if (index < 0 || index >= frames.count) index = 0
    player.set("frame", frames[index])
    index = index + 1
    if (index >= frames.count) index = 0
    player.set(indexField, index)
    PlayerModule._scheduleAnimation(player, nextName, 0.1)
  }

  static _setAnimationFrame(player, frame, nextName) {
    if (player == null) return
    if (frame != null) {
      player.set("frame", frame)
    }
    PlayerModule._scheduleAnimation(player, nextName, 0.1)
  }

  static _scheduleAnimation(player, nextName, delay) {
    if (player == null) return
    if (delay == null) delay = 0.1

    if (nextName == null || nextName == "") {
      player.set("think", null)
      player.set("nextthink", -1)
      return
    }

    var functionName = PlayerModule._qualifiedAnimationName(nextName)
    player.set("think", functionName)
    var nextTime = Engine.time() + delay
    player.set("nextthink", nextTime)
    Engine.scheduleThink(player, functionName, delay)
  }

  static _qualifiedAnimationName(name) {
    if (name == null || name == "") return ""
    if (name.contains(".")) return name
    return "PlayerModule." + name
  }

  static _addMuzzleFlash(player) {
    if (player == null) return
    var effects = player.get("effects", 0)
    effects = Engine.bitOr(effects, Effects.MUZZLEFLASH)
    player.set("effects", effects)
  }

  static _playSequenceFrame(player, frames, index, nextName) {
    if (player == null) return
    if (frames == null) return
    if (index < 0 || index >= frames.count) return
    PlayerModule._setAnimationFrame(player, frames[index], nextName)
  }

  static _startDeathAnimation(globals, player, name) {
    if (name == "player_diea1") {
      PlayerModule.player_diea1(globals, player)
    } else if (name == "player_dieb1") {
      PlayerModule.player_dieb1(globals, player)
    } else if (name == "player_diec1") {
      PlayerModule.player_diec1(globals, player)
    } else if (name == "player_died1") {
      PlayerModule.player_died1(globals, player)
    } else if (name == "player_diee1") {
      PlayerModule.player_diee1(globals, player)
    } else {
      PlayerModule.player_diea1(globals, player)
    }
  }

  static _painSound(globals, player) {
    if (player.get("health", 0) < 0) return

    var attacker = globals.damageAttacker
    if (attacker != null) {
      var className = attacker.get("classname", "")
      if (className == "teledeath" || className == "teledeath2") {
        Engine.playSound(player, Channels.VOICE, "player/teledth1.wav", 1, Attenuations.NORMAL)
        return
      }
    }

    var watertype = player.get("watertype", 0)
    var waterlevel = player.get("waterlevel", 0)
    if (watertype == Contents.WATER && waterlevel == 3) {
      PlayerModule.deathBubbles(globals, player, 1)
      var sample = Engine.random() > 0.5 ? "player/drown1.wav" : "player/drown2.wav"
      Engine.playSound(player, Channels.VOICE, sample, 1, Attenuations.NORMAL)
      return
    }

    if (watertype == Contents.SLIME || watertype == Contents.LAVA) {
      var burn = Engine.random() > 0.5 ? "player/lburn1.wav" : "player/lburn2.wav"
      Engine.playSound(player, Channels.VOICE, burn, 1, Attenuations.NORMAL)
      return
    }

    var now = Engine.time()
    if (player.get("pain_finished", 0.0) > now) {
      player.set("axhitme", 0)
      return
    }

    player.set("pain_finished", now + 0.5)

    if (player.get("axhitme", 0) == 1) {
      player.set("axhitme", 0)
      Engine.playSound(player, Channels.VOICE, "player/axhit1.wav", 1, Attenuations.NORMAL)
      return
    }

    var samples = [
      "player/pain1.wav",
      "player/pain2.wav",
      "player/pain3.wav",
      "player/pain4.wav",
      "player/pain5.wav",
      "player/pain6.wav"
    ]
    var sample = PlayerModule._randomChoice(samples)
    if (sample != null) {
      Engine.playSound(player, Channels.VOICE, sample, 1, Attenuations.NORMAL)
    }
  }

  static _deathSound(globals, player) {
    if (player.get("waterlevel", 0) == 3) {
      PlayerModule.deathBubbles(globals, player, 20)
      Engine.playSound(player, Channels.VOICE, "player/h2odeath.wav", 1, Attenuations.NONE)
      return
    }

    var options = [
      "player/death1.wav",
      "player/death2.wav",
      "player/death3.wav",
      "player/death4.wav",
      "player/death5.wav"
    ]
    var sample = PlayerModule._randomChoice(options)
    if (sample != null) {
      Engine.playSound(player, Channels.VOICE, sample, 1, Attenuations.NONE)
    }
  }

  static _gibPlayer(globals, player) {
    PlayerModule._throwHead(globals, player, "progs/h_player.mdl", player.get("health", 0))
    PlayerModule._throwGib(globals, player, "progs/gib1.mdl", player.get("health", 0))
    PlayerModule._throwGib(globals, player, "progs/gib2.mdl", player.get("health", 0))
    PlayerModule._throwGib(globals, player, "progs/gib3.mdl", player.get("health", 0))

    player.set("deadflag", DeadFlags.DEAD)

    var attacker = globals.damageAttacker
    if (attacker != null) {
      var className = attacker.get("classname", "")
      if (className == "teledeath" || className == "teledeath2") {
        Engine.playSound(player, Channels.VOICE, "player/teledth1.wav", 1, Attenuations.NONE)
        return
      }
    }

    var sound = Engine.random() < 0.5 ? "player/gib.wav" : "player/udeath.wav"
    Engine.playSound(player, Channels.VOICE, sound, 1, Attenuations.NONE)
  }

  static _throwGib(globals, player, model, damage) {
    var gib = Engine.spawnEntity()
    var origin = player.get("origin", [0, 0, 0])
    Engine.setOrigin(gib, origin)
    gib.set("origin", origin)
    Engine.setModel(gib, model)
    Engine.setSize(gib, [0, 0, 0], [0, 0, 0])
    gib.set("velocity", PlayerModule._velocityForDamage(damage))
    gib.set("movetype", MoveTypes.BOUNCE)
    gib.set("solid", SolidTypes.NOT)
    gib.set("avelocity", [Engine.random() * 600, Engine.random() * 600, Engine.random() * 600])
    gib.set("think", "SubsModule.subRemove")
    var removeTime = Engine.time() + 10 + Engine.random() * 10
    gib.set("nextthink", removeTime)
    Engine.scheduleThink(gib, "SubsModule.subRemove", removeTime - Engine.time())
    gib.set("frame", 0)
    gib.set("flags", 0)
  }

  static _throwHead(globals, player, model, damage) {
    Engine.setModel(player, model)
    player.set("frame", 0)
    player.set("nextthink", -1)
    player.set("movetype", MoveTypes.BOUNCE)
    player.set("takedamage", DamageValues.NO)
    player.set("solid", SolidTypes.NOT)
    player.set("view_ofs", [0, 0, 8])
    Engine.setSize(player, [-16, -16, 0], [16, 16, 56])

    var velocity = PlayerModule._velocityForDamage(damage)
    player.set("velocity", velocity)

    var origin = player.get("origin", [0, 0, 0])
    origin = [origin[0], origin[1], origin[2] - 24]
    Engine.setOrigin(player, origin)
    player.set("origin", origin)

    var flags = player.get("flags", 0)
    flags = flags - Engine.bitAnd(flags, PlayerFlags.ONGROUND)
    player.set("flags", flags)

    player.set("avelocity", [0, (Engine.random() * 2 - 1) * 600, 0])
  }

  static _bubbleSplit(globals, bubble) {
    if (bubble == null) return

    var newBubble = Engine.spawnEntity()
    Engine.setModel(newBubble, "progs/s_bubble.spr")
    var origin = bubble.get("origin", [0, 0, 0])
    Engine.setOrigin(newBubble, origin)
    newBubble.set("origin", origin)
    newBubble.set("movetype", MoveTypes.NOCLIP)
    newBubble.set("solid", SolidTypes.NOT)
    newBubble.set("velocity", bubble.get("velocity", [0, 0, 15]))
    newBubble.set("classname", "bubble")
    newBubble.set("frame", 1)
    newBubble.set("cnt", 10)
    Engine.setSize(newBubble, [-8, -8, -8], [8, 8, 8])
    newBubble.set("think", "PlayerModule.bubble_bob")
    var delay = 0.5
    newBubble.set("nextthink", Engine.time() + delay)
    Engine.scheduleThink(newBubble, "PlayerModule.bubble_bob", delay)

    bubble.set("frame", 1)
    bubble.set("cnt", 10)

    if (bubble.get("waterlevel", 3) != 3) {
      Engine.removeEntity(bubble)
    }
  }

  static _playerDead(player) {
    player.set("nextthink", -1)
    player.set("deadflag", DeadFlags.DEAD)
  }

  static _dropBackpack(globals, player) {
    var shells = player.get("ammo_shells", 0)
    var nails = player.get("ammo_nails", 0)
    var rockets = player.get("ammo_rockets", 0)
    var cells = player.get("ammo_cells", 0)

    if (shells + nails + rockets + cells == 0) return

    var pack = Engine.spawnEntity()
    var origin = player.get("origin", [0, 0, 0])
    var packOrigin = PlayerModule._vectorAdd(origin, [0, 0, -24])
    Engine.setOrigin(pack, packOrigin)
    pack.set("origin", packOrigin)

    var weapon = player.get("weapon", Items.AXE)
    pack.set("items", weapon)
    pack.set("classname", "item_backpack")

    var netname = null
    if (weapon == Items.AXE) {
      netname = "$qc_axe"
    } else if (weapon == Items.SHOTGUN) {
      netname = "$qc_shotgun"
    } else if (weapon == Items.SUPER_SHOTGUN) {
      netname = "$qc_double_shotgun"
    } else if (weapon == Items.NAILGUN) {
      netname = "$qc_nailgun"
    } else if (weapon == Items.SUPER_NAILGUN) {
      netname = "$qc_super_nailgun"
    } else if (weapon == Items.GRENADE_LAUNCHER) {
      netname = "$qc_grenade_launcher"
    } else if (weapon == Items.ROCKET_LAUNCHER) {
      netname = "$qc_rocket_launcher"
    } else if (weapon == Items.LIGHTNING) {
      netname = "$qc_thunderbolt"
    }

    if (netname != null) {
      pack.set("netname", netname)
    }

    pack.set("ammo_shells", shells)
    pack.set("ammo_nails", nails)
    pack.set("ammo_rockets", rockets)
    pack.set("ammo_cells", cells)

    if (pack.get("ammo_shells", 0) < 5 && (weapon == Items.SHOTGUN || weapon == Items.SUPER_SHOTGUN)) {
      pack.set("ammo_shells", 5)
    }
    if (pack.get("ammo_nails", 0) < 20 && (weapon == Items.NAILGUN || weapon == Items.SUPER_NAILGUN)) {
      pack.set("ammo_nails", 20)
    }
    if (pack.get("ammo_rockets", 0) < 5 && (weapon == Items.GRENADE_LAUNCHER || weapon == Items.ROCKET_LAUNCHER)) {
      pack.set("ammo_rockets", 5)
    }
    if (pack.get("ammo_cells", 0) < 15 && weapon == Items.LIGHTNING) {
      pack.set("ammo_cells", 15)
    }

    var velocity = [
      -100 + Engine.random() * 200,
      -100 + Engine.random() * 200,
      300
    ]
    pack.set("velocity", velocity)

    pack.set("flags", PlayerFlags.ITEM)
    pack.set("solid", SolidTypes.TRIGGER)
    pack.set("movetype", MoveTypes.TOSS)
    Engine.setModel(pack, "progs/backpack.mdl")
    Engine.setSize(pack, [-16, -16, 0], [16, 16, 56])
    pack.set("touch", "PlayerModule.backpackTouch")
    pack.set("think", "SubsModule.subRemove")

    var removeTime = 120.0
    pack.set("nextthink", Engine.time() + removeTime)
    Engine.scheduleThink(pack, "SubsModule.subRemove", removeTime)
  }

  static _boundAmmo(player) {
    if (player.get("ammo_shells", 0) > 100) player.set("ammo_shells", 100)
    if (player.get("ammo_nails", 0) > 200) player.set("ammo_nails", 200)
    if (player.get("ammo_rockets", 0) > 100) player.set("ammo_rockets", 100)
    if (player.get("ammo_cells", 0) > 100) player.set("ammo_cells", 100)
  }

  static _velocityForDamage(damage) {
    var v = [
      100 * PlayerModule._crandom(),
      100 * PlayerModule._crandom(),
      200 + 100 * Engine.random()
    ]

    if (damage > -50) {
      return PlayerModule._vectorScale(v, 0.7)
    }
    if (damage > -200) {
      return PlayerModule._vectorScale(v, 2)
    }
    return PlayerModule._vectorScale(v, 10)
  }

  static _vectorAdd(a, b) {
    return [a[0] + b[0], a[1] + b[1], a[2] + b[2]]
  }

  static _vectorScale(v, scale) {
    return [v[0] * scale, v[1] * scale, v[2] * scale]
  }

  static _crandom() {
    return Engine.random() * 2 - 1
  }

  static _randomChoice(options) {
    if (options == null || options.count == 0) return null
    var index = (Engine.random() * options.count).floor
    if (index >= options.count) index = options.count - 1
    return options[index]
  }
}
