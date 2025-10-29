// Client.wren
// Port of the essential client management and rules logic from client.qc.

import "./Engine" for Engine
import "./Globals" for GameGlobals, Items, MessageTypes, ServiceCodes, DamageValues
import "./Globals" for SolidTypes, MoveTypes, Channels, Attenuations, Contents
import "./Globals" for PlayerFlags, Effects, Teams, HullVectors
import "./Entity" for GameEntity
import "./Player" for PlayerModule
import "./World" for WorldModule
import "./Subs" for SubsModule
import "./Weapons" for WeaponsModule

var _IDEAL_DM_SPAWN_DIST = 384
var _MIN_DM_SPAWN_DIST = 84

class ClientModule {
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
    var length = ClientModule._vectorLength(v)
    if (length == 0) return [0, 0, 0]
    var inv = 1 / length
    return [v[0] * inv, v[1] * inv, v[2] * inv]
  }

  static _vectorEquals(a, b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2]
  }

  static _makeVectorsFixed(angles) {
    var fixed = [-angles[0], angles[1], angles[2]]
    return Engine.makeVectors(fixed)
  }

  static _hasFlag(player, flag) {
    return Engine.bitAnd(player.get("flags", 0), flag) != 0
  }

  static _setFlag(player, flag) {
    var flags = Engine.bitOr(player.get("flags", 0), flag)
    player.set("flags", flags)
  }

  static _clearFlag(player, flag) {
    var flags = player.get("flags", 0)
    flags = flags - Engine.bitAnd(flags, flag)
    player.set("flags", flags)
  }

  static setChangeParms(globals, player) {
    if (globals.resetFlag) {
      Engine.setSpawnParms(player)
      return
    }

    var health = player.get("health", 0)
    if (health <= 0 || globals.deathmatch > 0) {
      setNewParms(globals, player)
      return
    }

    var items = player.get("items", 0)
    var removable = Engine.bitOrMany([
      Items.KEY1,
      Items.KEY2,
      Items.INVISIBILITY,
      Items.INVULNERABILITY,
      Items.SUIT,
      Items.QUAD
    ])
    items = items - Engine.bitAnd(items, removable)
    player.set("items", items)

    var maxHealth = player.get("max_health", health)
    if (health > maxHealth) {
      health = maxHealth
    }
    if (health < maxHealth / 2) {
      health = maxHealth / 2
    }
    player.set("health", health)

    globals.setSpawnParm(1, player.get("items", 0))
    globals.setSpawnParm(2, player.get("health", 0))
    globals.setSpawnParm(3, player.get("armorvalue", 0))

    var shells = player.get("ammo_shells", 0)
    if (shells < 25) shells = 25
    globals.setSpawnParm(4, shells)

    globals.setSpawnParm(5, player.get("ammo_nails", 0))
    globals.setSpawnParm(6, player.get("ammo_rockets", 0))
    globals.setSpawnParm(7, player.get("ammo_cells", 0))
    globals.setSpawnParm(8, player.get("weapon", 0))
    globals.setSpawnParm(9, player.get("armortype", 0) * 100)
  }

  static setNewParms(globals, player) {
    globals.setSpawnParm(1, Engine.bitOr(Items.SHOTGUN, Items.AXE))
    var baseHealth = (globals.skill == 3 && globals.deathmatch == 0) ? 50 : 100
    globals.setSpawnParm(2, baseHealth)
    globals.setSpawnParm(3, 0)
    globals.setSpawnParm(4, 25)
    globals.setSpawnParm(5, 0)
    globals.setSpawnParm(6, 0)
    globals.setSpawnParm(7, 0)
    globals.setSpawnParm(8, 1)
    globals.setSpawnParm(9, 0)
  }

  static decodeLevelParms(globals, player) {
    if (globals.serverFlags != 0) {
      var worldModel = globals.world.get("model", "")
      if (worldModel == "maps/start.bsp") {
        setNewParms(globals, player)
      }
    }

    player.set("items", globals.spawnParm(1))
    player.set("health", globals.spawnParm(2))
    player.set("armorvalue", globals.spawnParm(3))
    player.set("ammo_shells", globals.spawnParm(4))
    player.set("ammo_nails", globals.spawnParm(5))
    player.set("ammo_rockets", globals.spawnParm(6))
    player.set("ammo_cells", globals.spawnParm(7))
    player.set("weapon", globals.spawnParm(8))
    player.set("armortype", globals.spawnParm(9) * 0.01)
  }

  static clientConnect(globals, player) {
    var name = player.get("netname", "")
    Engine.broadcastPrint("$qc_entered", [name])

    if (globals.intermissionRunning > 0) {
      exitIntermission(globals)
    }
  }

  static clientDisconnect(globals, player) {
    if (globals.gameOver) return

    var name = player.get("netname", "")
    var frags = player.get("frags", 0).toString
    Engine.broadcastPrint("$qc_left_game", [name, frags])

    Engine.playSound(
      player,
      Channels.BODY,
      "player/tornoff2.wav",
      1,
      Attenuations.NONE
    )

    player.set("effects", 0)
    PlayerModule.setSuicideFrame(globals, player)
  }

  static findIntermission(globals) {
    var spots = Engine.findAll(globals.world, "info_intermission")
    if (spots != null && spots.count > 0) {
      var cyc = Engine.random() * 4
      var index = cyc.floor % spots.count
      return spots[index]
    }

    spots = Engine.findAll(globals.world, "info_player_start")
    if (spots != null && spots.count > 0) {
      return spots[0]
    }

    spots = Engine.findAll(globals.world, "testplayerstart")
    if (spots != null && spots.count > 0) {
      return spots[0]
    }

    Engine.log("FindIntermission: no spot")
    return globals.world
  }

  static nextLevel(globals) {
    if (globals.nextMap != null) return

    var currentMap = globals.mapName
    var trigger = null

    if (currentMap == "start") {
      if (Engine.cvar("registered") == 0) {
        currentMap = "e1m1"
      } else if (Engine.bitAnd(globals.serverFlags, 1) == 0) {
        currentMap = "e1m1"
        globals.serverFlags = Engine.bitOr(globals.serverFlags, 1)
      } else if (Engine.bitAnd(globals.serverFlags, 2) == 0) {
        currentMap = "e2m1"
        globals.serverFlags = Engine.bitOr(globals.serverFlags, 2)
      } else if (Engine.bitAnd(globals.serverFlags, 4) == 0) {
        currentMap = "e3m1"
        globals.serverFlags = Engine.bitOr(globals.serverFlags, 4)
      } else if (Engine.bitAnd(globals.serverFlags, 8) == 0) {
        currentMap = "e4m1"
        globals.serverFlags = globals.serverFlags - 7
      }

      trigger = Engine.spawnEntity()
      trigger.set("map", currentMap)
    } else {
      var matches = Engine.findAll(globals.world, "trigger_changelevel")
      if (matches != null && matches.count > 0) {
        trigger = matches[0]
      }

      if (trigger == null || currentMap == "start") {
        trigger = Engine.spawnEntity()
        trigger.set("map", currentMap)
      }
    }

    globals.mapName = currentMap
    globals.nextMap = trigger.get("map", currentMap)
    globals.gameOver = true

    var now = Engine.time()
    var nextThink = trigger.get("nextthink", 0.0)
    if (nextThink < now) {
      trigger.set("nextthink", now + 0.1)
      executeChangeLevel(globals)
    }
  }

  static checkRules(globals, player) {
    if (globals.gameOver) return

    var timeLimit = Engine.cvar("timelimit") * 60
    var fragLimit = Engine.cvar("fraglimit")
    var now = Engine.time()

    if (timeLimit != 0 && now >= timeLimit) {
      nextLevel(globals)
      return
    }

    if (fragLimit != 0 && player.get("frags", 0) >= fragLimit) {
      nextLevel(globals)
      return
    }
  }

  static gotoNextMap(globals) {
    if (Engine.cvar("samelevel") != 0) {
      Engine.changeLevel(globals.mapName)
    } else if (globals.nextMap != null) {
      Engine.changeLevel(globals.nextMap)
    }
  }

  static changeLevelTouch(globals, trigger, toucher) {
    if (toucher == null) return
    if (toucher.get("classname", "") != "player") return

    var noExit = Engine.cvar("noexit")
    if (noExit == 1 || (noExit == 2 && globals.mapName != "start")) {
      Engine.applyDamage(toucher, trigger, trigger, 50000)
      return
    }

    if (globals.coop > 0 || globals.deathmatch > 0) {
      var name = toucher.get("netname", "")
      Engine.broadcastPrint("$qc_exited", [name])
    }

    globals.nextMap = trigger.get("map", null)
    SubsModule.useTargets(globals, trigger, toucher)

    var spawnFlags = trigger.get("spawnflags", 0)
    if (Engine.bitAnd(spawnFlags, 1) != 0 && globals.deathmatch == 0) {
      gotoNextMap(globals)
      return
    }

    Engine.clearTriggerTouch(trigger)
    Engine.scheduleThink(trigger, "Client.executeChangeLevel", 0.1)
  }

  static changelevel_touch(globals, trigger, toucher) {
    ClientModule.changeLevelTouch(globals, trigger, toucher)
  }

  static exitIntermission(globals) {
    if (globals.deathmatch > 0) {
      gotoNextMap(globals)
      return
    }

    globals.intermissionExitTime = Engine.time() + 1
    globals.intermissionRunning = globals.intermissionRunning + 1

    var worldModel = globals.world.get("model", "")

    if (globals.intermissionRunning == 2) {
      Engine.writeByte(MessageTypes.ALL, ServiceCodes.CDTRACK, null)
      Engine.writeByte(MessageTypes.ALL, 2, null)
      Engine.writeByte(MessageTypes.ALL, 3, null)

      if (worldModel == "maps/e1m7.bsp") {
        if (Engine.cvar("registered") == 0) {
          Engine.writeByte(MessageTypes.ALL, ServiceCodes.FINALE, null)
          Engine.writeString(MessageTypes.ALL, "$qc_finale_e1_shareware", null)
        } else {
          Engine.writeByte(MessageTypes.ALL, ServiceCodes.FINALE, null)
          Engine.writeString(MessageTypes.ALL, "$qc_finale_e1", null)
        }
        return
      } else if (worldModel == "maps/e2m6.bsp") {
        Engine.writeByte(MessageTypes.ALL, ServiceCodes.FINALE, null)
        Engine.writeString(MessageTypes.ALL, "$qc_finale_e2", null)
        return
      } else if (worldModel == "maps/e3m6.bsp") {
        Engine.writeByte(MessageTypes.ALL, ServiceCodes.FINALE, null)
        Engine.writeString(MessageTypes.ALL, "$qc_finale_e3", null)
        return
      } else if (worldModel == "maps/e4m7.bsp") {
        Engine.writeByte(MessageTypes.ALL, ServiceCodes.FINALE, null)
        Engine.writeString(MessageTypes.ALL, "$qc_finale_e4", null)
        return
      }

      gotoNextMap(globals)
      return
    }

    if (globals.intermissionRunning == 3) {
      if (Engine.cvar("registered") == 0) {
        Engine.writeByte(MessageTypes.ALL, ServiceCodes.SELL_SCREEN, null)
        return
      }

      if (Engine.bitAnd(globals.serverFlags, 15) == 15) {
        Engine.writeByte(MessageTypes.ALL, ServiceCodes.FINALE, null)
        Engine.writeString(MessageTypes.ALL, "$qc_finale_all_runes", null)
        return
      }
    }

    gotoNextMap(globals)
  }

  static intermissionThink(globals, player) {
    if (Engine.time() < globals.intermissionExitTime) return

    var pressed = player.get("button0", false) ||
      player.get("button1", false) ||
      player.get("button2", false)

    if (!pressed) return

    exitIntermission(globals)
  }

  static executeChangeLevel(globals) {
    globals.intermissionRunning = 1

    var waitTime = (globals.deathmatch > 0) ? 5 : 2
    globals.intermissionExitTime = Engine.time() + waitTime

    Engine.writeByte(MessageTypes.ALL, ServiceCodes.CDTRACK, null)
    Engine.writeByte(MessageTypes.ALL, 3, null)
    Engine.writeByte(MessageTypes.ALL, 3, null)

    var pos = findIntermission(globals)
    var players = Engine.findAll(globals.world, "player")

    if (players != null) {
      for (other in players) {
        other.set("view_ofs", [0, 0, 0])

        var mangle = pos.get("mangle", [0, 0, 0])
        other.set("angles", mangle)
        other.set("v_angle", mangle)
        other.set("fixangle", true)
        other.set("nextthink", Engine.time() + 0.5)
        other.set("takedamage", DamageValues.NO)
        other.set("solid", SolidTypes.NOT)
        other.set("movetype", MoveTypes.NONE)
        other.set("modelindex", 0)
        Engine.setOrigin(other, pos.get("origin", [0, 0, 0]))

        if (globals.skill == 3) {
          var worldModel = globals.world.get("model", "")
          if (other.get("fired_weapon", 0) == 0 && worldModel == "maps/e1m1.bsp") {
            Engine.writeByte(MessageTypes.ONE, ServiceCodes.ACHIEVEMENT, other)
            Engine.writeString(MessageTypes.ONE, "ACH_PACIFIST", other)
          }
          if (other.get("took_damage", 0) == 0 && worldModel == "maps/e4m6.bsp") {
            Engine.writeByte(MessageTypes.ONE, ServiceCodes.ACHIEVEMENT, other)
            Engine.writeString(MessageTypes.ONE, "ACH_PAINLESS_MAZE", other)
          }
        }
      }
    }

    Engine.writeByte(MessageTypes.ALL, ServiceCodes.INTERMISSION, null)

    var worldModel = globals.world.get("model", "")
    if (globals.campaign != 0 && worldModel == "maps/e1m7.bsp") {
      Engine.writeByte(MessageTypes.ALL, ServiceCodes.ACHIEVEMENT, null)
      Engine.writeString(MessageTypes.ALL, "ACH_COMPLETE_E1M7", null)
    } else if (globals.campaign != 0 && worldModel == "maps/e2m6.bsp") {
      Engine.writeByte(MessageTypes.ALL, ServiceCodes.ACHIEVEMENT, null)
      Engine.writeString(MessageTypes.ALL, "ACH_COMPLETE_E2M6", null)
    } else if (globals.campaign != 0 && worldModel == "maps/e3m6.bsp") {
      Engine.writeByte(MessageTypes.ALL, ServiceCodes.ACHIEVEMENT, null)
      Engine.writeString(MessageTypes.ALL, "ACH_COMPLETE_E3M6", null)
    } else if (globals.campaign != 0 && worldModel == "maps/e4m7.bsp") {
      Engine.writeByte(MessageTypes.ALL, ServiceCodes.ACHIEVEMENT, null)
      Engine.writeString(MessageTypes.ALL, "ACH_COMPLETE_E4M7", null)
    }

    var nextMap = globals.nextMap
    if (worldModel == "maps/e1m4.bsp" && nextMap == "e1m8") {
      Engine.writeByte(MessageTypes.ALL, ServiceCodes.ACHIEVEMENT, null)
      Engine.writeString(MessageTypes.ALL, "ACH_FIND_E1M8", null)
    } else if (worldModel == "maps/e2m3.bsp" && nextMap == "e2m7") {
      Engine.writeByte(MessageTypes.ALL, ServiceCodes.ACHIEVEMENT, null)
      Engine.writeString(MessageTypes.ALL, "ACH_FIND_E2M7", null)
    } else if (worldModel == "maps/e3m4.bsp" && nextMap == "e3m7") {
      Engine.writeByte(MessageTypes.ALL, ServiceCodes.ACHIEVEMENT, null)
      Engine.writeString(MessageTypes.ALL, "ACH_FIND_E3M7", null)
    } else if (worldModel == "maps/e4m5.bsp" && nextMap == "e4m8") {
      Engine.writeByte(MessageTypes.ALL, ServiceCodes.ACHIEVEMENT, null)
      Engine.writeString(MessageTypes.ALL, "ACH_FIND_E4M8", null)
    }
  }

  static execute_changelevel(globals) {
    ClientModule.executeChangeLevel(globals)
  }

  static triggerChangeLevel(globals, trigger) {
    var mapName = trigger.get("map", null)
    if (mapName == null || mapName == "") {
      Engine.objError("changelevel trigger doesn't have map")
      return
    }

    trigger.set("netname", "changelevel")
    trigger.set("killstring", "$qc_ks_tried_leave")

    Engine.initTrigger(trigger)
    Engine.setTriggerTouch(trigger, "Client.changeLevelTouch")
  }

  static trigger_changelevel(globals, trigger) {
    ClientModule.triggerChangeLevel(globals, trigger)
  }

  static infoIntermission(globals, entity) {
    // Marker entity used purely for camera placement during intermissions.
    // QuakeC definition is intentionally empty, so we mirror that behavior.
  }

  static info_intermission(globals, entity) {
    ClientModule.infoIntermission(globals, entity)
  }

  static infoPlayerStart(globals, entity) {
    // Regular single-player start position. No setup needed beyond the map
    // data, matching the QuakeC stub definition.
  }

  static info_player_start(globals, entity) {
    ClientModule.infoPlayerStart(globals, entity)
  }

  static infoPlayerStart2(globals, entity) {
    // Alternate start location used when returning to the start map between
    // episodes. Provided for parity with the QuakeC stub implementation.
  }

  static info_player_start2(globals, entity) {
    ClientModule.infoPlayerStart2(globals, entity)
  }

  static testPlayerStart(globals, entity) {
    // Region-based start marker created by level editors. Left intentionally
    // empty in QuakeC, so no additional logic is required here.
  }

  static testplayerstart(globals, entity) {
    ClientModule.testPlayerStart(globals, entity)
  }

  static infoPlayerDeathmatch(globals, entity) {
    // Multiplayer spawn point. The QuakeC version is a stub, so we simply
    // expose the entry point for completeness.
  }

  static info_player_deathmatch(globals, entity) {
    ClientModule.infoPlayerDeathmatch(globals, entity)
  }

  static infoPlayerCoop(globals, entity) {
    // Cooperative spawn point. QuakeC treats this as a no-op definition.
  }

  static info_player_coop(globals, entity) {
    ClientModule.infoPlayerCoop(globals, entity)
  }

  static respawn(globals, player) {
    if (globals.coop > 0) {
      WorldModule.copyToBodyQueue(globals, player)
      Engine.setSpawnParms(player)
      ClientModule.putClientInServer(globals, player)
      return
    }

    if (globals.deathmatch > 0) {
      WorldModule.copyToBodyQueue(globals, player)
      ClientModule.setNewParms(globals, player)
      ClientModule.putClientInServer(globals, player)
      return
    }

    globals.serverFlags = globals.startingServerFlags
    globals.resetFlag = true
    Engine.localCommand("changelevel %s\n" % globals.mapName)
  }

  static clientKill(globals, player) {
    Engine.broadcastPrint("$qc_suicides", [player.get("netname", "")])
    PlayerModule.setSuicideFrame(globals, player)
    player.set("modelindex", globals.modelIndexPlayer)
    player.set("frags", player.get("frags", 0) - 2)
    ClientModule.respawn(globals, player)
  }

  static playerVisibleToSpawnPoint(globals, point) {
    var players = Engine.findAll(globals.world, "player")
    if (players == null) return false

    var pointOrigin = point.get("origin", [0, 0, 0])
    for (other in players) {
      if (other.get("health", 0) <= 0) continue

      var viewOfs = other.get("view_ofs", [0, 0, 0])
      var start = ClientModule._vectorAdd(pointOrigin, viewOfs)
      var end = ClientModule._vectorAdd(other.get("origin", [0, 0, 0]), viewOfs)
      var trace = Engine.traceLine(start, end, true, point)
      if (trace != null && trace.containsKey("fraction") && trace["fraction"] >= 1) {
        return true
      }
    }

    return false
  }

  static selectSpawnPoint(globals, forceSpawn) {
    var testStarts = Engine.findAll(globals.world, "testplayerstart")
    if (testStarts != null && testStarts.count > 0) {
      return testStarts[0]
    }

    if (globals.coop > 0) {
      var coopSpots = Engine.findAll(globals.world, "info_player_coop")
      if (coopSpots == null || coopSpots.count == 0) {
        coopSpots = Engine.findAll(globals.world, "info_player_start")
      }

      if (coopSpots != null && coopSpots.count > 0) {
        var index = 0
        if (globals.lastSpawn != null) {
          var lastIndex = coopSpots.indexOf(globals.lastSpawn)
          if (lastIndex == -1) lastIndex = -1
          index = (lastIndex + 1) % coopSpots.count
        }

        var spot = coopSpots[index]
        globals.lastSpawn = spot
        return spot
      }
    } else if (globals.deathmatch > 0) {
      var dmSpots = Engine.findAll(globals.world, "info_player_deathmatch")
      if (dmSpots == null || dmSpots.count == 0) {
        return globals.world
      }

      var goodSpots = []

      for (spot in dmSpots) {
        var origin = spot.get("origin", [0, 0, 0])
        var nearby = Engine.findRadius(origin, _IDEAL_DM_SPAWN_DIST)
        var count = 0
        if (nearby != null) {
          for (thing in nearby) {
            if (thing.get("classname", "") == "player" && thing.get("health", 0) > 0) {
              count = count + 1
            }
          }
        }

        if (count == 0 && ClientModule.playerVisibleToSpawnPoint(globals, spot)) {
          count = count + 1
        }

        if (count == 0) {
          goodSpots.add(spot)
        }
      }

      if (goodSpots.count == 0) {
        for (spot in dmSpots) {
          var origin = spot.get("origin", [0, 0, 0])
          var nearby = Engine.findRadius(origin, _MIN_DM_SPAWN_DIST)
          var count = 0
          if (nearby != null) {
            for (thing in nearby) {
              if (thing.get("classname", "") == "player" && thing.get("health", 0) > 0) {
                count = count + 1
              }
            }
          }

          if (count == 0) {
            goodSpots.add(spot)
          }
        }
      }

      if (goodSpots.count == 0) {
        if (!forceSpawn) {
          return globals.world
        }

        var randomIndex = (Engine.random() * dmSpots.count).floor
        if (randomIndex >= dmSpots.count) randomIndex = dmSpots.count - 1
        return dmSpots[randomIndex]
      }

      var choice = (Engine.random() * goodSpots.count).floor
      if (choice >= goodSpots.count) choice = goodSpots.count - 1
      return goodSpots[choice]
    }

    if (globals.serverFlags != 0) {
      var start2 = Engine.findAll(globals.world, "info_player_start2")
      if (start2 != null && start2.count > 0) {
        return start2[0]
      }
    }

    var starts = Engine.findAll(globals.world, "info_player_start")
    if (starts == null || starts.count == 0) {
      Engine.objError("PutClientInServer: no info_player_start on level")
      return globals.world
    }

    return starts[0]
  }

  static putClientInServer(globals, player) {
    player.set("classname", "player")

    var nightmare = globals.skill == 3 && globals.deathmatch == 0
    var baseHealth = nightmare ? 50 : 100
    player.set("health", baseHealth)
    player.set("takedamage", DamageValues.AIM)
    player.set("solid", SolidTypes.SLIDEBOX)
    player.set("movetype", MoveTypes.WALK)
    player.set("show_hostile", 0)
    player.set("max_health", baseHealth)
    player.set("flags", PlayerFlags.CLIENT)
    player.set("air_finished", Engine.time() + 12)
    player.set("dmg", 2)
    player.set("super_damage_finished", 0)
    player.set("radsuit_finished", 0)
    player.set("invisible_finished", 0)
    player.set("invincible_finished", 0)
    player.set("effects", 0)
    player.set("invincible_time", 0)
    player.set("healthrot_nextcheck", 0)
    player.set("fired_weapon", 0)
    player.set("took_damage", 0)
    player.set("team", Teams.NONE)
    if (globals.coop > 0) {
      player.set("team", Teams.HUMANS)
    }

    ClientModule.decodeLevelParms(globals, player)
    WeaponsModule.setCurrentAmmo(globals, player)

    player.set("attack_finished", Engine.time())
    player.set("th_pain", "PlayerModule.playerPain")
    player.set("th_die", "PlayerModule.playerDie")
    player.set("deadflag", DeadFlags.NO)
    player.set("pausetime", 0)

    var spawnDeferred = player.get("spawn_deferred", 0.0)
    var shouldTelefrag = spawnDeferred > 0 && Engine.time() >= spawnDeferred

    var spot = ClientModule.selectSpawnPoint(globals, shouldTelefrag)
    if (spot == globals.world) {
      player.set("takedamage", DamageValues.NO)
      player.set("solid", SolidTypes.NOT)
      player.set("movetype", MoveTypes.NONE)
      player.set("deadflag", DeadFlags.DEAD)
      Engine.setModel(player, "")
      player.set("view_ofs", [0, 0, 1])
      player.set("velocity", [0, 0, 0])

      if (spawnDeferred == 0) {
        player.set("spawn_deferred", Engine.time() + 5)
      }

      var intermission = ClientModule.findIntermission(globals)
      var mangle = intermission.get("mangle", [0, 0, 0])
      player.set("angles", mangle)
      player.set("v_angle", mangle)
      player.set("fixangle", true)
      var origin = intermission.get("origin", [0, 0, 0])
      Engine.setOrigin(player, origin)
      player.set("origin", origin)
      player.set("weaponmodel", "")
      player.set("weaponframe", 0)
      player.set("weapon", 0)
      return
    }

    player.set("spawn_deferred", 0)

    var spawnOrigin = ClientModule._vectorAdd(spot.get("origin", [0, 0, 0]), [0, 0, 1])
    Engine.setOrigin(player, spawnOrigin)
    player.set("origin", spawnOrigin)

    var spotAngles = spot.get("angles", [0, 0, 0])
    player.set("angles", spotAngles)
    player.set("v_angle", spotAngles)
    player.set("fixangle", true)

    var eyesIndex = Engine.setModel(player, "progs/eyes.mdl")
    globals.modelIndexEyes = eyesIndex
    var playerIndex = Engine.setModel(player, "progs/player.mdl")
    globals.modelIndexPlayer = playerIndex

    Engine.setSize(player, HullVectors.PLAYER_MIN, HullVectors.PLAYER_MAX)
    player.set("mins", HullVectors.PLAYER_MIN)
    player.set("maxs", HullVectors.PLAYER_MAX)

    player.set("view_ofs", [0, 0, 22])
    player.set("velocity", [0, 0, 0])
    player.set("frame", "stand1")
    player.set("weaponframe", 0)

    var vectors = ClientModule._makeVectorsFixed(player.get("angles", [0, 0, 0]))
    if (vectors != null && vectors.containsKey("forward")) {
      var forward = vectors["forward"]
      var effectOrigin = ClientModule._vectorAdd(spawnOrigin, ClientModule._vectorScale(forward, 20))
      Engine.spawnTeleportFog(effectOrigin)
    }

    Engine.spawnTeleportDeath(
      spawnOrigin,
      player,
      player.get("mins", HullVectors.PLAYER_MIN),
      player.get("maxs", HullVectors.PLAYER_MAX)
    )

    Engine.stuffCommand(player, "-attack\n")
  }

  static playerDeathThink(globals, player) {
    if (ClientModule._hasFlag(player, PlayerFlags.ONGROUND)) {
      var velocity = player.get("velocity", [0, 0, 0])
      var speed = ClientModule._vectorLength(velocity) - 20
      if (speed <= 0) {
        player.set("velocity", [0, 0, 0])
      } else {
        var normalized = ClientModule._vectorNormalize(velocity)
        player.set("velocity", ClientModule._vectorScale(normalized, speed))
      }
    }

    var spawnDeferred = player.get("spawn_deferred", 0.0)
    if (spawnDeferred != 0) {
      var spot = ClientModule.selectSpawnPoint(globals, false)
      if (spot != globals.world || Engine.time() >= spawnDeferred) {
        ClientModule.respawn(globals, player)
      }
      return
    }

    if (player.get("deadflag", DeadFlags.NO) == DeadFlags.DEAD) {
      if (player.get("button0", false) || player.get("button1", false) || player.get("button2", false)) {
        return
      }
      player.set("deadflag", DeadFlags.RESPAWNABLE)
      return
    }

    if (!player.get("button0", false) && !player.get("button1", false) && !player.get("button2", false)) {
      return
    }

    player.set("button0", false)
    player.set("button1", false)
    player.set("button2", false)
    ClientModule.respawn(globals, player)
  }

  static playerJump(globals, player) {
    if (ClientModule._hasFlag(player, PlayerFlags.WATERJUMP)) return

    var waterLevel = player.get("waterlevel", 0)
    if (waterLevel >= 2) {
      var velocity = player.get("velocity", [0, 0, 0])
      var waterType = player.get("watertype", 0)
      if (waterType == Contents.WATER) {
        velocity = [velocity[0], velocity[1], 100]
      } else if (waterType == Contents.SLIME) {
        velocity = [velocity[0], velocity[1], 80]
      } else {
        velocity = [velocity[0], velocity[1], 50]
      }
      player.set("velocity", velocity)

      var now = Engine.time()
      if (player.get("swim_flag", 0) < now) {
        player.set("swim_flag", now + 1)
        var sample = Engine.random() < 0.5 ? "misc/water1.wav" : "misc/water2.wav"
        Engine.playSound(player, Channels.BODY, sample, 1, Attenuations.NORMAL)
      }
      return
    }

    if (!ClientModule._hasFlag(player, PlayerFlags.ONGROUND)) return
    if (!ClientModule._hasFlag(player, PlayerFlags.JUMPRELEASED)) return

    ClientModule._clearFlag(player, PlayerFlags.JUMPRELEASED)
    ClientModule._clearFlag(player, PlayerFlags.ONGROUND)
    player.set("button2", false)
    Engine.playSound(player, Channels.BODY, "player/plyrjmp8.wav", 1, Attenuations.NORMAL)

    var velocity = player.get("velocity", [0, 0, 0])
    velocity = [velocity[0], velocity[1], velocity[2] + 270]
    player.set("velocity", velocity)
  }

  static waterMove(globals, player) {
    if (player.get("movetype", MoveTypes.NONE) == MoveTypes.NOCLIP) return
    if (player.get("health", 0) < 0) return

    var now = Engine.time()
    var waterLevel = player.get("waterlevel", 0)

    if (waterLevel != 3) {
      if (player.get("air_finished", 0) < now) {
        Engine.playSound(player, Channels.VOICE, "player/gasp2.wav", 1, Attenuations.NORMAL)
      } else if (player.get("air_finished", 0) < now + 9) {
        Engine.playSound(player, Channels.VOICE, "player/gasp1.wav", 1, Attenuations.NORMAL)
      }
      player.set("air_finished", now + 12)
      player.set("dmg", 2)
    } else if (player.get("air_finished", 0) < now) {
      if (player.get("pain_finished", 0) < now) {
        var dmg = player.get("dmg", 0) + 2
        if (dmg > 15) dmg = 10
        player.set("dmg", dmg)
        Engine.applyDamage(player, globals.world, globals.world, dmg)
        player.set("pain_finished", now + 1)
      }
    }

    if (waterLevel == 0) {
      if (ClientModule._hasFlag(player, PlayerFlags.INWATER)) {
        Engine.playSound(player, Channels.BODY, "misc/outwater.wav", 1, Attenuations.NORMAL)
        ClientModule._clearFlag(player, PlayerFlags.INWATER)
      }
      return
    }

    var waterType = player.get("watertype", 0)
    if (waterType == Contents.LAVA) {
      if (player.get("dmgtime", 0) < now) {
        if (player.get("radsuit_finished", 0) > now) {
          player.set("dmgtime", now + 1)
        } else {
          player.set("dmgtime", now + 0.2)
        }
        Engine.applyDamage(player, globals.world, globals.world, 10 * waterLevel)
      }
    } else if (waterType == Contents.SLIME) {
      if (player.get("dmgtime", 0) < now && player.get("radsuit_finished", 0) < now) {
        player.set("dmgtime", now + 1)
        Engine.applyDamage(player, globals.world, globals.world, 4 * waterLevel)
      }
    }

    if (!ClientModule._hasFlag(player, PlayerFlags.INWATER)) {
      if (waterType == Contents.LAVA) {
        Engine.playSound(player, Channels.BODY, "player/inlava.wav", 1, Attenuations.NORMAL)
      } else if (waterType == Contents.WATER) {
        Engine.playSound(player, Channels.BODY, "player/inh2o.wav", 1, Attenuations.NORMAL)
      } else if (waterType == Contents.SLIME) {
        Engine.playSound(player, Channels.BODY, "player/slimbrn2.wav", 1, Attenuations.NORMAL)
      }
      ClientModule._setFlag(player, PlayerFlags.INWATER)
      player.set("dmgtime", 0)
    }

    if (!ClientModule._hasFlag(player, PlayerFlags.WATERJUMP)) {
      var velocity = player.get("velocity", [0, 0, 0])
      var damp = 0.8 * waterLevel * globals.frameTime
      velocity = [
        velocity[0] - velocity[0] * damp,
        velocity[1] - velocity[1] * damp,
        velocity[2] - velocity[2] * damp
      ]
      player.set("velocity", velocity)
    }
  }

  static checkWaterJump(globals, player) {
    var start = ClientModule._vectorAdd(player.get("origin", [0, 0, 0]), [0, 0, 8])
    var vectors = ClientModule._makeVectorsFixed(player.get("angles", [0, 0, 0]))
    var forward = vectors == null ? [0, 0, 0] : vectors["forward"]
    forward = [forward[0], forward[1], 0]
    forward = ClientModule._vectorNormalize(forward)
    var end = ClientModule._vectorAdd(start, ClientModule._vectorScale(forward, 24))
    var trace = Engine.traceLine(start, end, true, player)
    if (trace == null || trace["fraction"] == null || trace["fraction"] >= 1) return

    var maxs = player.get("maxs", HullVectors.PLAYER_MAX)
    start = ClientModule._vectorAdd(start, [0, 0, maxs[2] - 8])
    end = ClientModule._vectorAdd(start, ClientModule._vectorScale(forward, 24))
    if (trace != null && trace.containsKey("planeNormal")) {
      player.set("movedir", ClientModule._vectorScale(trace["planeNormal"], -50))
    }
    trace = Engine.traceLine(start, end, true, player)
    if (trace != null && trace["fraction"] == 1) {
      ClientModule._setFlag(player, PlayerFlags.WATERJUMP)
      var velocity = player.get("velocity", [0, 0, 0])
      velocity = [velocity[0], velocity[1], 225]
      player.set("velocity", velocity)
      ClientModule._clearFlag(player, PlayerFlags.JUMPRELEASED)
      player.set("teleport_time", Engine.time() + 2)
    }
  }

  static playerPreThink(globals, player) {
    if (globals.intermissionRunning > 0) {
      ClientModule.intermissionThink(globals, player)
      return
    }

    if (ClientModule._vectorEquals(player.get("view_ofs", [0, 0, 0]), [0, 0, 0])) return

    Engine.makeVectors(player.get("v_angle", [0, 0, 0]))

    if (globals.deathmatch > 0 || globals.coop > 0) {
      ClientModule.checkRules(globals, player)
    }

    ClientModule.waterMove(globals, player)

    if (player.get("waterlevel", 0) == 2) {
      ClientModule.checkWaterJump(globals, player)
    }

    var deadflag = player.get("deadflag", DeadFlags.NO)
    if (deadflag >= DeadFlags.DEAD) {
      ClientModule.playerDeathThink(globals, player)
      return
    }

    if (deadflag == DeadFlags.DYING) return

    if (player.get("button2", false)) {
      ClientModule.playerJump(globals, player)
    } else {
      ClientModule._setFlag(player, PlayerFlags.JUMPRELEASED)
    }

    if (Engine.time() < player.get("pausetime", 0)) {
      player.set("velocity", [0, 0, 0])
    }

    if (Engine.time() > player.get("attack_finished", 0) &&
        player.get("currentammo", 0) == 0 &&
        player.get("weapon", Items.AXE) != Items.AXE) {
      var best = WeaponsModule.bestWeapon(globals, player)
      if (best != null) {
        player.set("weapon", best)
        WeaponsModule.setCurrentAmmo(globals, player)
      }
    }
  }

  static checkPowerups(globals, player) {
    if (player.get("health", 0) <= 0) return

    var now = Engine.time()

    if (player.get("invisible_finished", 0) > 0) {
      if (player.get("invisible_sound", 0) < now) {
        Engine.playSound(player, Channels.AUTO, "items/inv3.wav", 0.5, Attenuations.IDLE)
        player.set("invisible_sound", now + (Engine.random() * 3 + 1))
      }

      if (player.get("invisible_finished", 0) < now + 3) {
        if (player.get("invisible_time", 0) == 1) {
          Engine.playerPrint(player, "$qc_ring_fade", [])
          Engine.stuffCommand(player, "bf\n")
          Engine.playSound(player, Channels.AUTO, "items/inv2.wav", 1, Attenuations.NORMAL)
          player.set("invisible_time", now + 1)
        }
        if (player.get("invisible_time", 0) < now) {
          player.set("invisible_time", now + 1)
          Engine.stuffCommand(player, "bf\n")
        }
      }

      if (player.get("invisible_finished", 0) < now) {
        var items = player.get("items", 0)
        items = items - Engine.bitAnd(items, Items.INVISIBILITY)
        player.set("items", items)
        player.set("invisible_finished", 0)
        player.set("invisible_time", 0)
      }

      player.set("frame", 0)
      player.set("modelindex", globals.modelIndexEyes)
    } else {
      player.set("modelindex", globals.modelIndexPlayer)
    }

    if (player.get("invincible_finished", 0) > 0) {
      if (player.get("invincible_finished", 0) < now + 3) {
        if (player.get("invincible_time", 0) == 1) {
          Engine.playerPrint(player, "$qc_protection_fade", [])
          Engine.stuffCommand(player, "bf\n")
          Engine.playSound(player, Channels.AUTO, "items/protect2.wav", 1, Attenuations.NORMAL)
          player.set("invincible_time", now + 1)
        }
        if (player.get("invincible_time", 0) < now) {
          player.set("invincible_time", now + 1)
          Engine.stuffCommand(player, "bf\n")
        }
      }

      if (player.get("invincible_finished", 0) < now) {
        var items = player.get("items", 0)
        items = items - Engine.bitAnd(items, Items.INVULNERABILITY)
        player.set("items", items)
        player.set("invincible_time", 0)
        player.set("invincible_finished", 0)
      }

      if (player.get("invincible_finished", 0) > now) {
        player.set("effects", Engine.bitOr(player.get("effects", 0), Effects.PENTALIGHT))
      } else {
        var effects = player.get("effects", 0)
        effects = effects - Engine.bitAnd(effects, Effects.PENTALIGHT)
        player.set("effects", effects)
      }
    }

    if (player.get("super_damage_finished", 0) > 0) {
      if (player.get("super_damage_finished", 0) < now + 3) {
        if (player.get("super_time", 0) == 1) {
          Engine.playerPrint(player, "$qc_quad_fade", [])
          Engine.stuffCommand(player, "bf\n")
          Engine.playSound(player, Channels.AUTO, "items/damage2.wav", 1, Attenuations.NORMAL)
          player.set("super_time", now + 1)
        }
        if (player.get("super_time", 0) < now) {
          player.set("super_time", now + 1)
          Engine.stuffCommand(player, "bf\n")
        }
      }

      if (player.get("super_damage_finished", 0) < now) {
        var items = player.get("items", 0)
        items = items - Engine.bitAnd(items, Items.QUAD)
        player.set("items", items)
        player.set("super_damage_finished", 0)
        player.set("super_time", 0)
      }

      if (player.get("super_damage_finished", 0) > now) {
        player.set("effects", Engine.bitOr(player.get("effects", 0), Effects.QUADLIGHT))
      } else {
        var effects = player.get("effects", 0)
        effects = effects - Engine.bitAnd(effects, Effects.QUADLIGHT)
        player.set("effects", effects)
      }
    }

    if (player.get("radsuit_finished", 0) > 0) {
      player.set("air_finished", now + 12)

      if (player.get("radsuit_finished", 0) < now + 3) {
        if (player.get("rad_time", 0) == 1) {
          Engine.playerPrint(player, "$qc_biosuit_fade", [])
          Engine.stuffCommand(player, "bf\n")
          Engine.playSound(player, Channels.AUTO, "items/suit2.wav", 1, Attenuations.NORMAL)
          player.set("rad_time", now + 1)
        }
        if (player.get("rad_time", 0) < now) {
          player.set("rad_time", now + 1)
          Engine.stuffCommand(player, "bf\n")
        }
      }

      if (player.get("radsuit_finished", 0) < now) {
        var items = player.get("items", 0)
        items = items - Engine.bitAnd(items, Items.SUIT)
        player.set("items", items)
        player.set("rad_time", 0)
        player.set("radsuit_finished", 0)
      }
    }
  }

  static checkHealthRot(globals, player) {
    if (Engine.bitAnd(player.get("items", 0), Items.SUPERHEALTH) == 0) return

    var now = Engine.time()
    if (player.get("healthrot_nextcheck", 0) > now) return

    if (player.get("health", 0) > player.get("max_health", player.get("health", 0))) {
      player.set("health", player.get("health", 0) - 1)
      player.set("healthrot_nextcheck", now + 1)
      return
    }

    var items = player.get("items", 0)
    items = items - Engine.bitAnd(items, Items.SUPERHEALTH)
    player.set("items", items)
    player.set("healthrot_nextcheck", 0)
  }

  static playerPostThink(globals, player) {
    if (ClientModule._vectorEquals(player.get("view_ofs", [0, 0, 0]), [0, 0, 0])) return
    if (player.get("deadflag", 0) != DeadFlags.NO) return

    WeaponsModule.weaponFrame(globals, player)

    if (player.get("jump_flag", 0) < -300 && ClientModule._hasFlag(player, PlayerFlags.ONGROUND) && player.get("health", 0) > 0) {
      var waterType = player.get("watertype", 0)
      if (waterType == Contents.WATER) {
        Engine.playSound(player, Channels.BODY, "player/h2ojump.wav", 1, Attenuations.NORMAL)
      } else if (player.get("jump_flag", 0) < -650) {
        Engine.applyDamage(player, globals.world, globals.world, 5)
        Engine.playSound(player, Channels.VOICE, "player/land2.wav", 1, Attenuations.NORMAL)
        if (player.get("health", 0) <= 5) {
          player.set("deathtype", "falling")
        }
      } else {
        Engine.playSound(player, Channels.VOICE, "player/land.wav", 1, Attenuations.NORMAL)
      }
      player.set("jump_flag", 0)
    }

    if (!ClientModule._hasFlag(player, PlayerFlags.ONGROUND)) {
      player.set("jump_flag", player.get("velocity", [0, 0, 0])[2])
    }

    ClientModule.checkPowerups(globals, player)
    ClientModule.checkHealthRot(globals, player)
  }

  static clientObituary(globals, target, attacker) {
    var targetClass = target.get("classname", "")
    if (targetClass != "player") return

    var attackerClass = attacker.get("classname", "")
    var targetName = target.get("netname", "")
    var attackerName = attacker.get("netname", "")
    var attackerTeam = attacker.get("team", 0)
    var targetTeam = target.get("team", 0)

    var randomValue = Engine.random()

    if (attackerClass == "teledeath") {
      var owner = attacker.get("owner", null)
      var ownerName = owner == null ? "" : owner.get("netname", "")
      Engine.broadcastPrint("$qc_telefragged", [targetName, ownerName])
      if (owner != null) {
        owner.set("frags", owner.get("frags", 0) + 1)
      }
      return
    }

    if (attackerClass == "teledeath2") {
      Engine.broadcastPrint("$qc_satans_power", [targetName])
      target.set("frags", target.get("frags", 0) - 1)
      return
    }

    if (attackerClass == "player") {
      if (target == attacker) {
        attacker.set("frags", attacker.get("frags", 0) - 1)

        var targetWeapon = target.get("weapon", 0)
        if (targetWeapon == Items.LIGHTNING && target.get("waterlevel", 0) > 1) {
          var waterType = target.get("watertype", 0)
          if (waterType == Contents.SLIME) {
            Engine.broadcastPrint("$qc_discharge_slime", [targetName])
          } else if (waterType == Contents.LAVA) {
            Engine.broadcastPrint("$qc_discharge_lava", [targetName])
          } else {
            Engine.broadcastPrint("$qc_discharge_water", [targetName])
          }
          return
        }

        if (targetWeapon == Items.GRENADE_LAUNCHER) {
          Engine.broadcastPrint("$qc_suicide_pin", [targetName])
          return
        }

        if (randomValue >= 0.5) {
          Engine.broadcastPrint("$qc_suicide_bored", [targetName])
        } else {
          Engine.broadcastPrint("$qc_suicide_loaded", [targetName])
        }
        return
      }

      if (globals.teamplay == 2 && targetTeam == attackerTeam && attackerTeam != 0) {
        if (randomValue < 0.25) {
          Engine.broadcastPrint("$qc_ff_teammate", [attackerName])
        } else if (randomValue < 0.50) {
          Engine.broadcastPrint("$qc_ff_glasses", [attackerName])
        } else if (randomValue < 0.75) {
          Engine.broadcastPrint("$qc_ff_otherteam", [attackerName])
        } else {
          Engine.broadcastPrint("$qc_ff_friend", [attackerName])
        }

        attacker.set("frags", attacker.get("frags", 0) - 1)
        return
      }

      attacker.set("frags", attacker.get("frags", 0) + 1)

      var weapon = attacker.get("weapon", 0)
      if (weapon == Items.AXE) {
        Engine.broadcastPrint("$qc_death_ax", [targetName, attackerName])
        return
      }

      if (weapon == Items.SHOTGUN) {
        Engine.broadcastPrint("$qc_death_sg", [targetName, attackerName])
        return
      }

      if (weapon == Items.SUPER_SHOTGUN) {
        Engine.broadcastPrint("$qc_death_dbl", [targetName, attackerName])
        return
      }

      if (weapon == Items.NAILGUN) {
        Engine.broadcastPrint("$qc_death_nail", [targetName, attackerName])
        return
      }

      if (weapon == Items.SUPER_NAILGUN) {
        Engine.broadcastPrint("$qc_death_sng", [targetName, attackerName])
        return
      }

      if (weapon == Items.GRENADE_LAUNCHER) {
        if (target.get("health", 0) < -40) {
          Engine.broadcastPrint("$qc_death_gl1", [targetName, attackerName])
        } else {
          Engine.broadcastPrint("$qc_death_gl2", [targetName, attackerName])
        }
        return
      }

      if (weapon == Items.ROCKET_LAUNCHER) {
        var quadActive = attacker.get("super_damage_finished", 0) > 0
        if (quadActive && target.get("health", 0) < -40) {
          var quadRand = Engine.random()
          if (quadRand < 0.3) {
            Engine.broadcastPrint("$qc_death_rl_quad1", [targetName, attackerName])
          } else if (quadRand < 0.6) {
            Engine.broadcastPrint("$qc_death_rl_quad2", [targetName, attackerName])
          } else {
            Engine.broadcastPrint("$qc_death_rl1", [targetName, attackerName])
          }
          return
        }

        if (target.get("health", 0) < -40) {
          Engine.broadcastPrint("$qc_death_rl2", [targetName, attackerName])
        } else {
          Engine.broadcastPrint("$qc_death_rl3", [targetName, attackerName])
        }
        return
      }

      if (weapon == Items.LIGHTNING) {
        if (attacker.get("waterlevel", 0) > 1) {
          Engine.broadcastPrint("$qc_death_lg1", [targetName, attackerName])

          if (attacker.get("invincible_finished", 0) > 0) {
            globals.msgEntity = attacker
            Engine.writeByte(MessageTypes.ONE, ServiceCodes.ACHIEVEMENT, attacker)
            Engine.writeString(MessageTypes.ONE, "ACH_SURVIVE_DISCHARGE", attacker)
          }
        } else {
          Engine.broadcastPrint("$qc_death_lg2", [targetName, attackerName])
        }
        return
      }

      return
    }

    target.set("frags", target.get("frags", 0) - 1)
    var waterType = target.get("watertype", 0)
    if (waterType == Contents.WATER) {
      if (Engine.random() < 0.5) {
        Engine.broadcastPrint("$qc_death_drown1", [targetName])
      } else {
        Engine.broadcastPrint("$qc_death_drown2", [targetName])
      }
      return
    }

    if (waterType == Contents.SLIME) {
      if (Engine.random() < 0.5) {
        Engine.broadcastPrint("$qc_death_slime1", [targetName])
      } else {
        Engine.broadcastPrint("$qc_death_slime2", [targetName])
      }
      return
    }

    if (waterType == Contents.LAVA) {
      if (target.get("health", 0) < -15) {
        Engine.broadcastPrint("$qc_death_lava1", [targetName])
        return
      }

      if (Engine.random() < 0.5) {
        Engine.broadcastPrint("$qc_death_lava2", [targetName])
      } else {
        Engine.broadcastPrint("$qc_death_lava3", [targetName])
      }
      return
    }

    if (attacker.get("solid", SolidTypes.NOT) == SolidTypes.BSP && attacker != globals.world) {
      Engine.broadcastPrint("$qc_death_squish", [targetName])
      return
    }

    var killString = attacker.get("killstring", null)
    if (killString != null && killString != "") {
      Engine.broadcastPrint(killString, [targetName])
      return
    }

    var deathType = target.get("deathtype", "")
    if (deathType == "falling") {
      target.set("deathtype", "")
      Engine.broadcastPrint("$qc_death_fall", [targetName])
      return
    }

    Engine.broadcastPrint("$qc_death_died", [targetName])
  }

  static CheckPowerups(globals, player) { ClientModule.checkPowerups(globals, player) }
  static CheckRules(globals, player) { ClientModule.checkRules(globals, player) }
  static CheckWaterJump(globals, player) { ClientModule.checkWaterJump(globals, player) }
  static ClientConnect(globals, player) { ClientModule.clientConnect(globals, player) }
  static ClientDisconnect(globals, player) { ClientModule.clientDisconnect(globals, player) }
  static ClientKill(globals, player) { ClientModule.clientKill(globals, player) }
  static ClientObituary(globals, attacker, target) { ClientModule.clientObituary(globals, attacker, target) }
  static DecodeLevelParms(globals, player) { ClientModule.decodeLevelParms(globals, player) }
  static ExitIntermission(globals) { ClientModule.exitIntermission(globals) }
  static FindIntermission(globals) { return ClientModule.findIntermission(globals) }
  static GotoNextMap(globals) { ClientModule.gotoNextMap(globals) }
  static IntermissionThink(globals, player) { ClientModule.intermissionThink(globals, player) }
  static NextLevel(globals) { ClientModule.nextLevel(globals) }
  static PlayerDeathThink(globals, player) { ClientModule.playerDeathThink(globals, player) }
  static PlayerJump(globals, player) { ClientModule.playerJump(globals, player) }
  static PlayerPostThink(globals, player) { ClientModule.playerPostThink(globals, player) }
  static PlayerPreThink(globals, player) { ClientModule.playerPreThink(globals, player) }
  static PutClientInServer(globals, player) { ClientModule.putClientInServer(globals, player) }
  static SetChangeParms(globals, player) { ClientModule.setChangeParms(globals, player) }
  static SetNewParms(globals, player) { ClientModule.setNewParms(globals, player) }
  static WaterMove(globals, player) { ClientModule.waterMove(globals, player) }
}
