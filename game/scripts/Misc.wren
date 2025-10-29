// Misc.wren
// Ports the miscellaneous utility entity implementations from misc.qc so that
// environmental props, ambient sounds, and scripted traps behave like the
// original QuakeC gameplay code when running in Wren.

import "./Engine" for Engine
import "./Globals" for SolidTypes, MoveTypes, DamageValues, Channels, Attenuations
import "./Globals" for Effects, Contents
import "./Subs" for SubsModule
import "./Combat" for CombatModule
import "./Weapons" for WeaponsModule

class MiscModule {
  static _vectorAdd(a, b) {
    return [a[0] + b[0], a[1] + b[1], a[2] + b[2]]
  }

  static _vectorSub(a, b) {
    return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
  }

  static _vectorScale(v, scalar) {
    return [v[0] * scalar, v[1] * scalar, v[2] * scalar]
  }

  static _vectorNormalize(v) {
    var length = (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]).sqrt
    if (length == 0) return [0, 0, 0]
    return MiscModule._vectorScale(v, 1 / length)
  }

  static _scheduleThink(entity, functionName, delay) {
    entity.set("think", functionName)
    entity.set("nextthink", Engine.time() + delay)
    Engine.scheduleThink(entity, functionName, delay)
  }

  static infoNull(globals, entity) {
    if (entity == null) return
    Engine.removeEntity(entity)
  }

  static infoNotNull(globals, entity) {
    // Intentionally empty – serves only as a positional helper in maps.
  }

  static _toggleLight(globals, entity, onStyle, offStyle) {
    var style = entity.get("style", 0).floor
    if (style < 32) return

    var spawnflags = entity.get("spawnflags", 0)
    var startOff = 1
    if (Engine.bitAnd(spawnflags, startOff) != 0) {
      Engine.lightstyle(style, offStyle)
      entity.set("spawnflags", spawnflags - startOff)
    } else {
      Engine.lightstyle(style, onStyle)
      entity.set("spawnflags", spawnflags + startOff)
    }
  }

  static lightUse(globals, entity) {
    if (entity == null) return
    MiscModule._toggleLight(globals, entity, "a", "m")
  }

  static light(globals, entity) {
    if (entity == null) return

    var targetName = entity.get("targetname", null)
    if (targetName == null || targetName == "") {
      Engine.removeEntity(entity)
      return
    }

    var style = entity.get("style", 0).floor
    if (style < 32) return

    entity.set("use", "MiscModule.lightUse")
    if (Engine.bitAnd(entity.get("spawnflags", 0), 1) != 0) {
      Engine.lightstyle(style, "a")
    } else {
      Engine.lightstyle(style, "m")
    }
  }

  static lightFluoro(globals, entity) {
    if (entity == null) return

    var style = entity.get("style", 0).floor
    if (style >= 32) {
      entity.set("use", "MiscModule.lightUse")
      if (Engine.bitAnd(entity.get("spawnflags", 0), 1) != 0) {
        Engine.lightstyle(style, "a")
      } else {
        Engine.lightstyle(style, "m")
      }
    }

    Engine.precacheSound("ambience/fl_hum1.wav")
    Engine.ambientSound(entity.get("origin", [0, 0, 0]), "ambience/fl_hum1.wav", 0.5, Attenuations.STATIC)
  }

  static lightFluoroSpark(globals, entity) {
    if (entity == null) return
    if (entity.get("style", 0) == 0) {
      entity.set("style", 10)
    }
    Engine.precacheSound("ambience/buzz1.wav")
    Engine.ambientSound(entity.get("origin", [0, 0, 0]), "ambience/buzz1.wav", 0.5, Attenuations.STATIC)
  }

  static lightGlobe(globals, entity) {
    if (entity == null) return
    Engine.precacheModel("progs/s_light.spr")
    Engine.setModel(entity, "progs/s_light.spr")
    Engine.makeStatic(entity)
  }

  static fireAmbient(globals, entity) {
    Engine.precacheSound("ambience/fire1.wav")
    Engine.ambientSound(entity.get("origin", [0, 0, 0]), "ambience/fire1.wav", 0.5, Attenuations.STATIC)
  }

  static lightTorchSmallWalltorch(globals, entity) {
    if (entity == null) return
    Engine.precacheModel("progs/flame.mdl")
    Engine.setModel(entity, "progs/flame.mdl")
    MiscModule.fireAmbient(globals, entity)
    Engine.makeStatic(entity)
  }

  static lightFlameLargeYellow(globals, entity) {
    if (entity == null) return
    Engine.precacheModel("progs/flame2.mdl")
    Engine.setModel(entity, "progs/flame2.mdl")
    entity.set("frame", 1)
    MiscModule.fireAmbient(globals, entity)
    Engine.makeStatic(entity)
  }

  static lightFlameSmallYellow(globals, entity) {
    if (entity == null) return
    Engine.precacheModel("progs/flame2.mdl")
    Engine.setModel(entity, "progs/flame2.mdl")
    MiscModule.fireAmbient(globals, entity)
    Engine.makeStatic(entity)
  }

  static lightFlameSmallWhite(globals, entity) {
    if (entity == null) return
    Engine.precacheModel("progs/flame2.mdl")
    Engine.setModel(entity, "progs/flame2.mdl")
    MiscModule.fireAmbient(globals, entity)
    Engine.makeStatic(entity)
  }

  static miscFireball(globals, entity) {
    if (entity == null) return
    Engine.precacheModel("progs/lavaball.mdl")
    entity.set("classname", "fireball")
    entity.set("netname", "$qc_lava_ball")
    entity.set("killstring", "$qc_ks_lavaball")
    var delay = Engine.random() * 5.0
    MiscModule._scheduleThink(entity, "MiscModule.fireFly", delay)
  }

  static fireFly(globals, entity) {
    if (entity == null) return

    var fireball = Engine.spawnEntity()
    fireball.set("classname", "fireball")
    fireball.set("movetype", MoveTypes.TOSS)
    fireball.set("solid", SolidTypes.TRIGGER)

    var velocity = [
      Engine.random() * 100 - 50,
      Engine.random() * 100 - 50,
      entity.get("speed", 0.0) + Engine.random() * 200
    ]
    fireball.set("velocity", velocity)

    Engine.setModel(fireball, "progs/lavaball.mdl")
    Engine.setSize(fireball, [0, 0, 0], [0, 0, 0])
    Engine.setOrigin(fireball, entity.get("origin", [0, 0, 0]))

    fireball.set("think", "SubsModule.subRemove")
    fireball.set("touch", "MiscModule.fireTouch")
    var removeDelay = 5.0
    fireball.set("nextthink", Engine.time() + removeDelay)
    Engine.scheduleThink(fireball, "SubsModule.subRemove", removeDelay)

    var nextDelay = Engine.random() * 5.0 + 3.0
    MiscModule._scheduleThink(entity, "MiscModule.fireFly", nextDelay)
  }

  static fireTouch(globals, fireball, other) {
    if (fireball == null) return
    if (other == null) return
    CombatModule.tDamage(globals, other, fireball, fireball, 20)
    Engine.removeEntity(fireball)
  }

  static barrelExplode(globals, entity) {
    if (entity == null) return
    var attacker = entity.get("enemy", null)
    CombatModule.tRadiusDamage(globals, entity, attacker, 160, globals.world)
    Engine.playSound(entity, Channels.VOICE, "weapons/r_exp3.wav", 1, Attenuations.NORMAL)
    Engine.spawnParticles(entity.get("origin", [0, 0, 0]), [0, 0, 0], 75, 255)

    var origin = entity.get("origin", [0, 0, 0])
    origin[2] = origin[2] + 32
    entity.set("origin", origin)
    Engine.setOrigin(entity, origin)

    WeaponsModule.becomeExplosion(globals, entity)
  }

  static barrelDetonate(globals, entity) {
    if (entity == null) return
    entity.set("classname", "explo_box")
    entity.set("takedamage", DamageValues.NO)
    entity.set("think", "MiscModule.barrelExplode")
    var delay = 0.3
    entity.set("nextthink", entity.get("ltime", Engine.time()) + delay)
    Engine.scheduleThink(entity, "MiscModule.barrelExplode", delay)
  }

  static miscExplobox(globals, entity) {
    if (entity == null) return

    var modelPath = entity.get("mdl", null)
    if (modelPath == null || modelPath == "") {
      modelPath = "maps/b_explob.bsp"
      entity.set("mdl", modelPath)
    }

    entity.set("solid", SolidTypes.BSP)
    entity.set("movetype", MoveTypes.PUSH)
    Engine.precacheModel(modelPath)
    Engine.setModel(entity, modelPath)
    Engine.precacheSound("weapons/r_exp3.wav")

    entity.set("health", 20)
    entity.set("th_die", "MiscModule.barrelDetonate")
    entity.set("takedamage", DamageValues.AIM)
    entity.set("netname", "$qc_exploding_barrel")
    entity.set("killstring", "$qc_ks_blew_up")

    var origin = entity.get("origin", [0, 0, 0])
    var oldZ = origin[2] + 2
    origin[2] = oldZ
    entity.set("origin", origin)
    Engine.setOrigin(entity, origin)

    Engine.dropToFloor(entity)
    var newOrigin = entity.get("origin", origin)
    if ((oldZ - newOrigin[2]).abs > 250) {
      Engine.log("explobox fell out of level at %(_)." % [newOrigin])
      Engine.removeEntity(entity)
    }
  }

  static miscExplobox2(globals, entity) {
    if (entity == null) return
    entity.set("mdl", "maps/b_exbox2.bsp")
    MiscModule.miscExplobox(globals, entity)
  }

  static _spawnLaser(globals, owner, origin, direction) {
    var normalized = MiscModule._vectorNormalize(direction)
    var laser = Engine.spawnEntity()
    laser.set("classname", "enforcer_laser")
    laser.set("owner", owner)
    laser.set("movetype", MoveTypes.FLY)
    laser.set("solid", SolidTypes.BBOX)
    laser.set("effects", Engine.bitOr(laser.get("effects", 0), Effects.DIMLIGHT))

    Engine.setModel(laser, "progs/laser.mdl")
    Engine.setSize(laser, [0, 0, 0], [0, 0, 0])
    Engine.setOrigin(laser, origin)

    var velocity = MiscModule._vectorScale(normalized, 600)
    laser.set("velocity", velocity)
    laser.set("angles", Engine.vectorToAngles(velocity))

    var removeDelay = 5.0
    laser.set("think", "SubsModule.subRemove")
    laser.set("nextthink", Engine.time() + removeDelay)
    Engine.scheduleThink(laser, "SubsModule.subRemove", removeDelay)

    laser.set("touch", "MiscModule.laserTouch")
    return laser
  }

  static launchLaser(globals, owner, origin, direction) {
    if (owner != null && owner.get("classname", "") == "monster_enforcer") {
      Engine.playSound(owner, Channels.WEAPON, "enforcer/enfire.wav", 1, Attenuations.NORMAL)
    }
    return MiscModule._spawnLaser(globals, owner, origin, direction)
  }

  static laserTouch(globals, laser, other) {
    if (laser == null) return
    if (other == null) return
    var owner = laser.get("owner", null)
    if (owner != null && other == owner) return

    if (Engine.pointContents(laser.get("origin", [0, 0, 0])) == Contents.SKY) {
      Engine.removeEntity(laser)
      return
    }

    Engine.playSound(laser, Channels.WEAPON, "enforcer/enfstop.wav", 1, Attenuations.STATIC)

    var velocity = laser.get("velocity", [0, 0, 0])
    var backOffset = MiscModule._vectorScale(MiscModule._vectorNormalize(velocity), 8)
    var impact = MiscModule._vectorSub(laser.get("origin", [0, 0, 0]), backOffset)

    if (other.get("health", 0) > 0) {
      WeaponsModule.spawnBlood(impact, MiscModule._vectorScale(velocity, 0.2), 15)
      CombatModule.tDamage(globals, other, laser, owner == null ? laser : owner, 15)
    } else {
      WeaponsModule._emitGunshot(impact)
    }

    Engine.removeEntity(laser)
  }

  static spikeshooterUse(globals, shooter, activator) {
    if (shooter == null) return null
    var origin = shooter.get("origin", [0, 0, 0])
    var movedir = shooter.get("movedir", [0, 0, 0])
    var spawnflags = shooter.get("spawnflags", 0)

    if (Engine.bitAnd(spawnflags, 2) != 0) {
      Engine.playSound(shooter, Channels.VOICE, "enforcer/enfire.wav", 1, Attenuations.NORMAL)
      return MiscModule.launchLaser(globals, shooter, origin, movedir)
    }

    Engine.playSound(shooter, Channels.VOICE, "weapons/spike2.wav", 1, Attenuations.NORMAL)
    var missile = WeaponsModule.launch_spike(globals, shooter, origin, movedir)
    if (missile != null) {
      missile.set("velocity", MiscModule._vectorScale(movedir, 500))
      if (Engine.bitAnd(spawnflags, 1) != 0) {
        missile.set("touch", "WeaponsModule.superSpikeTouch")
      }
    }
    return missile
  }

  static shooterThink(globals, shooter) {
    if (shooter == null) return
    var missile = MiscModule.spikeshooterUse(globals, shooter, shooter.get("enemy", null))
    if (missile != null && Engine.bitAnd(shooter.get("spawnflags", 0), 2) == 0) {
      missile.set("velocity", MiscModule._vectorScale(shooter.get("movedir", [0, 0, 0]), 500))
    }
    var wait = shooter.get("wait", 1.0)
    MiscModule._scheduleThink(shooter, "MiscModule.shooterThink", wait)
  }

  static trapSpikeshooter(globals, shooter) {
    if (shooter == null) return
    SubsModule.setMoveDir(globals, shooter)
    shooter.set("use", "MiscModule.spikeshooterUse")
    shooter.set("netname", "$qc_spike_trap")
    shooter.set("killstring", "$qc_ks_spiked")

    if (Engine.bitAnd(shooter.get("spawnflags", 0), 2) != 0) {
      Engine.precacheModel2("progs/laser.mdl")
      Engine.precacheSound2("enforcer/enfire.wav")
      Engine.precacheSound2("enforcer/enfstop.wav")
    } else {
      Engine.precacheSound("weapons/spike2.wav")
    }
  }

  static trapShooter(globals, shooter) {
    if (shooter == null) return
    MiscModule.trapSpikeshooter(globals, shooter)

    var wait = shooter.get("wait", 0.0)
    if (wait == 0) {
      wait = 1.0
      shooter.set("wait", wait)
    }

    var ltime = shooter.get("ltime", Engine.time())
    var initialDelay = shooter.get("nextthink", 0.0) + wait
    if (initialDelay < 0) initialDelay = wait
    shooter.set("think", "MiscModule.shooterThink")
    shooter.set("nextthink", ltime + initialDelay)
    Engine.scheduleThink(shooter, "MiscModule.shooterThink", initialDelay)
  }

  static airBubbles(globals, entity) {
    if (entity == null) return
    if (globals.deathmatch != 0) {
      Engine.removeEntity(entity)
      return
    }

    Engine.precacheModel("progs/s_bubble.spr")
    MiscModule._scheduleThink(entity, "MiscModule.makeBubbles", 1.0)
  }

  static makeBubbles(globals, spawner) {
    if (spawner == null) return
    var bubble = Engine.spawnEntity()
    Engine.setModel(bubble, "progs/s_bubble.spr")
    Engine.setOrigin(bubble, spawner.get("origin", [0, 0, 0]))
    bubble.set("movetype", MoveTypes.NOCLIP)
    bubble.set("solid", SolidTypes.NOT)
    bubble.set("velocity", [0, 0, 15])
    bubble.set("think", "MiscModule.bubbleBob")
    bubble.set("touch", "MiscModule.bubbleRemove")
    bubble.set("classname", "bubble")
    bubble.set("frame", 0)
    bubble.set("cnt", 0)
    Engine.setSize(bubble, [-8, -8, -8], [8, 8, 8])
    bubble.set("nextthink", Engine.time() + 0.5)
    Engine.scheduleThink(bubble, "MiscModule.bubbleBob", 0.5)

    var delay = Engine.random() + 0.5
    MiscModule._scheduleThink(spawner, "MiscModule.makeBubbles", delay)
  }

  static bubbleSplit(globals, bubble) {
    if (bubble == null) return
    var newBubble = Engine.spawnEntity()
    Engine.setModel(newBubble, "progs/s_bubble.spr")
    Engine.setOrigin(newBubble, bubble.get("origin", [0, 0, 0]))
    newBubble.set("movetype", MoveTypes.NOCLIP)
    newBubble.set("solid", SolidTypes.NOT)
    newBubble.set("velocity", bubble.get("velocity", [0, 0, 0]))
    newBubble.set("think", "MiscModule.bubbleBob")
    newBubble.set("touch", "MiscModule.bubbleRemove")
    newBubble.set("classname", "bubble")
    newBubble.set("frame", 1)
    newBubble.set("cnt", 10)
    Engine.setSize(newBubble, [-8, -8, -8], [8, 8, 8])
    newBubble.set("nextthink", Engine.time() + 0.5)
    Engine.scheduleThink(newBubble, "MiscModule.bubbleBob", 0.5)

    bubble.set("frame", 1)
    bubble.set("cnt", 10)
    if (bubble.get("waterlevel", 0) != 3) {
      Engine.removeEntity(bubble)
    }
  }

  static bubbleRemove(globals, bubble, other) {
    if (bubble == null) return
    if (other != null && other.get("classname", "") == bubble.get("classname", "")) return
    Engine.removeEntity(bubble)
  }

  static bubbleBob(globals, bubble) {
    if (bubble == null) return
    var count = bubble.get("cnt", 0) + 1
    bubble.set("cnt", count)

    if (count == 4) {
      MiscModule.bubbleSplit(globals, bubble)
    }
    if (count == 20) {
      Engine.removeEntity(bubble)
      return
    }

    var velocity = bubble.get("velocity", [0, 0, 0])
    var adjust = {
      "x": Engine.random() * 20 - 10,
      "y": Engine.random() * 20 - 10,
      "z": 10 + Engine.random() * 10
    }

    var vx = velocity[0] + adjust["x"]
    var vy = velocity[1] + adjust["y"]
    var vz = velocity[2] + adjust["z"]

    if (vx > 10) vx = 5
    if (vx < -10) vx = -5
    if (vy > 10) vy = 5
    if (vy < -10) vy = -5
    if (vz < 10) vz = 15
    if (vz > 30) vz = 25

    bubble.set("velocity", [vx, vy, vz])
    MiscModule._scheduleThink(bubble, "MiscModule.bubbleBob", 0.5)
  }

  static viewthing(globals, entity) {
    if (entity == null) return
    entity.set("movetype", MoveTypes.NONE)
    entity.set("solid", SolidTypes.NOT)
    Engine.precacheModel("progs/player.mdl")
    Engine.setModel(entity, "progs/player.mdl")
  }

  static funcWallUse(globals, entity, activator) {
    if (entity == null) return
    var frame = entity.get("frame", 0)
    entity.set("frame", frame == 0 ? 1 : 0)
  }

  static funcWall(globals, entity) {
    if (entity == null) return
    entity.set("angles", [0, 0, 0])
    entity.set("classname", "func_wall")
    entity.set("movetype", MoveTypes.PUSH)
    entity.set("solid", SolidTypes.BSP)
    entity.set("use", "MiscModule.funcWallUse")
    Engine.setModel(entity, entity.get("model", ""))
  }

  static funcIllusionary(globals, entity) {
    if (entity == null) return
    entity.set("angles", [0, 0, 0])
    entity.set("movetype", MoveTypes.NONE)
    entity.set("solid", SolidTypes.NOT)
    Engine.setModel(entity, entity.get("model", ""))
    Engine.makeStatic(entity)
  }

  static funcEpisodeGate(globals, entity) {
    if (entity == null) return
    var spawnflags = entity.get("spawnflags", 0)
    if (Engine.bitAnd(globals.serverFlags.floor, spawnflags.floor) == 0) {
      return
    }
    entity.set("angles", [0, 0, 0])
    entity.set("movetype", MoveTypes.PUSH)
    entity.set("solid", SolidTypes.BSP)
    entity.set("use", "MiscModule.funcWallUse")
    Engine.setModel(entity, entity.get("model", ""))
  }

  static funcBossGate(globals, entity) {
    if (entity == null) return
    if (Engine.bitAnd(globals.serverFlags.floor, 15) == 15) {
      return
    }
    entity.set("angles", [0, 0, 0])
    entity.set("movetype", MoveTypes.PUSH)
    entity.set("solid", SolidTypes.BSP)
    entity.set("use", "MiscModule.funcWallUse")
    Engine.setModel(entity, entity.get("model", ""))
  }

  static ambientSound(globals, entity, sample, volume) {
    Engine.precacheSound(sample)
    Engine.ambientSound(entity.get("origin", [0, 0, 0]), sample, volume, Attenuations.STATIC)
  }

  static ambientSuckWind(globals, entity) {
    MiscModule.ambientSound(globals, entity, "ambience/suck1.wav", 1)
  }

  static ambientDrone(globals, entity) {
    MiscModule.ambientSound(globals, entity, "ambience/drone6.wav", 0.5)
  }

  static ambientFlouroBuzz(globals, entity) {
    MiscModule.ambientSound(globals, entity, "ambience/buzz1.wav", 1)
  }

  static ambientDrip(globals, entity) {
    MiscModule.ambientSound(globals, entity, "ambience/drip1.wav", 0.5)
  }

  static ambientCompHum(globals, entity) {
    MiscModule.ambientSound(globals, entity, "ambience/comp1.wav", 1)
  }

  static ambientThunder(globals, entity) {
    MiscModule.ambientSound(globals, entity, "ambience/thunder1.wav", 0.5)
  }

  static ambientLightBuzz(globals, entity) {
    MiscModule.ambientSound(globals, entity, "ambience/fl_hum1.wav", 0.5)
  }

  static ambientSwamp1(globals, entity) {
    MiscModule.ambientSound(globals, entity, "ambience/swamp1.wav", 0.5)
  }

  static ambientSwamp2(globals, entity) {
    MiscModule.ambientSound(globals, entity, "ambience/swamp2.wav", 0.5)
  }

  static noiseThink(globals, entity) {
    if (entity == null) return
    var samples = [
      "enforcer/enfire.wav",
      "enforcer/enfstop.wav",
      "enforcer/sight1.wav",
      "enforcer/sight2.wav",
      "enforcer/sight3.wav",
      "enforcer/sight4.wav",
      "enforcer/pain1.wav"
    ]
    for (sample in samples) {
      Engine.playSound(entity, Channels.WEAPON, sample, 1, Attenuations.NORMAL)
    }
    MiscModule._scheduleThink(entity, "MiscModule.noiseThink", 0.5)
  }

  static miscNoisemaker(globals, entity) {
    if (entity == null) return
    Engine.precacheSound2("enforcer/enfire.wav")
    Engine.precacheSound2("enforcer/enfstop.wav")
    Engine.precacheSound2("enforcer/sight1.wav")
    Engine.precacheSound2("enforcer/sight2.wav")
    Engine.precacheSound2("enforcer/sight3.wav")
    Engine.precacheSound2("enforcer/sight4.wav")
    Engine.precacheSound2("enforcer/pain1.wav")
    Engine.precacheSound2("enforcer/pain2.wav")
    Engine.precacheSound2("enforcer/death1.wav")
    Engine.precacheSound2("enforcer/idle1.wav")

    var delay = 0.1 + Engine.random()
    MiscModule._scheduleThink(entity, "MiscModule.noiseThink", delay)
  }

  // --------------------------------------------------------------------------
  // Compatibility wrappers ---------------------------------------------------

  static info_null(globals, entity) { MiscModule.infoNull(globals, entity) }
  static info_notnull(globals, entity) { MiscModule.infoNotNull(globals, entity) }
  static light_use(globals, entity) { MiscModule.lightUse(globals, entity) }
  static light(globals, entity) { MiscModule.light(globals, entity) }
  static light_fluoro(globals, entity) { MiscModule.lightFluoro(globals, entity) }
  static light_fluorospark(globals, entity) { MiscModule.lightFluoroSpark(globals, entity) }
  static light_globe(globals, entity) { MiscModule.lightGlobe(globals, entity) }
  static FireAmbient(globals, entity) { MiscModule.fireAmbient(globals, entity) }
  static light_torch_small_walltorch(globals, entity) { MiscModule.lightTorchSmallWalltorch(globals, entity) }
  static light_flame_large_yellow(globals, entity) { MiscModule.lightFlameLargeYellow(globals, entity) }
  static light_flame_small_yellow(globals, entity) { MiscModule.lightFlameSmallYellow(globals, entity) }
  static light_flame_small_white(globals, entity) { MiscModule.lightFlameSmallWhite(globals, entity) }
  static misc_fireball(globals, entity) { MiscModule.miscFireball(globals, entity) }
  static fire_fly(globals, entity) { MiscModule.fireFly(globals, entity) }
  static fire_touch(globals, entity, other) { MiscModule.fireTouch(globals, entity, other) }
  static barrel_explode(globals, entity) { MiscModule.barrelExplode(globals, entity) }
  static barrel_detonate(globals, entity) { MiscModule.barrelDetonate(globals, entity) }
  static misc_explobox(globals, entity) { MiscModule.miscExplobox(globals, entity) }
  static misc_explobox2(globals, entity) { MiscModule.miscExplobox2(globals, entity) }
  static spikeshooter_use(globals, entity, activator) { return MiscModule.spikeshooterUse(globals, entity, activator) }
  static shooter_think(globals, entity) { MiscModule.shooterThink(globals, entity) }
  static trap_spikeshooter(globals, entity) { MiscModule.trapSpikeshooter(globals, entity) }
  static trap_shooter(globals, entity) { MiscModule.trapShooter(globals, entity) }
  static LaunchLaser(globals, entity, origin, direction) {
    return MiscModule.launchLaser(globals, entity, origin, direction)
  }
  static air_bubbles(globals, entity) { MiscModule.airBubbles(globals, entity) }
  static make_bubbles(globals, entity) { MiscModule.makeBubbles(globals, entity) }
  static bubble_split(globals, entity) { MiscModule.bubbleSplit(globals, entity) }
  static bubble_remove(globals, entity, other) { MiscModule.bubbleRemove(globals, entity, other) }
  static bubble_bob(globals, entity) { MiscModule.bubbleBob(globals, entity) }
  static viewthing(globals, entity) { MiscModule.viewthing(globals, entity) }
  static func_wall_use(globals, entity, activator) { MiscModule.funcWallUse(globals, entity, activator) }
  static func_wall(globals, entity) { MiscModule.funcWall(globals, entity) }
  static func_illusionary(globals, entity) { MiscModule.funcIllusionary(globals, entity) }
  static func_episodegate(globals, entity) { MiscModule.funcEpisodeGate(globals, entity) }
  static func_bossgate(globals, entity) { MiscModule.funcBossGate(globals, entity) }
  static ambient_suck_wind(globals, entity) { MiscModule.ambientSuckWind(globals, entity) }
  static ambient_drone(globals, entity) { MiscModule.ambientDrone(globals, entity) }
  static ambient_flouro_buzz(globals, entity) { MiscModule.ambientFlouroBuzz(globals, entity) }
  static ambient_drip(globals, entity) { MiscModule.ambientDrip(globals, entity) }
  static ambient_comp_hum(globals, entity) { MiscModule.ambientCompHum(globals, entity) }
  static ambient_thunder(globals, entity) { MiscModule.ambientThunder(globals, entity) }
  static ambient_light_buzz(globals, entity) { MiscModule.ambientLightBuzz(globals, entity) }
  static ambient_swamp1(globals, entity) { MiscModule.ambientSwamp1(globals, entity) }
  static ambient_swamp2(globals, entity) { MiscModule.ambientSwamp2(globals, entity) }
  static noise_think(globals, entity) { MiscModule.noiseThink(globals, entity) }
  static misc_noisemaker(globals, entity) { MiscModule.miscNoisemaker(globals, entity) }
}
