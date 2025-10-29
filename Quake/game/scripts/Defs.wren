// Defs.wren
// Provides implementations for global utility functions historically exposed by
// QuakeC's defs.qc. These wrappers translate the QuakeC style API into the
// engine bindings made available to the Wren gameplay runtime.

import "./Engine" for Engine
import "./Globals" for MessageTypes

class DefsModule {
  static _zeroVector() {
    return [0, 0, 0]
  }

  static _applyVectors(globals, vectors) {
    if (vectors == null) {
      globals.vForward = _zeroVector()
      globals.vRight = _zeroVector()
      globals.vUp = _zeroVector()
      return
    }

    globals.vForward = vectors.containsKey("forward") ? vectors["forward"] : _zeroVector()
    globals.vRight = vectors.containsKey("right") ? vectors["right"] : _zeroVector()
    globals.vUp = vectors.containsKey("up") ? vectors["up"] : _zeroVector()
  }

  static _vectorLength(v) {
    if (v == null) return 0.0
    return (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]).sqrt
  }

  static _normalizeVector(v) {
    if (v == null) return _zeroVector()
    var length = _vectorLength(v)
    if (length == 0) return _zeroVector()
    return [v[0] / length, v[1] / length, v[2] / length]
  }

  static _messageEntity(globals, channel) {
    if (channel == MessageTypes.ONE) return globals.msgEntity
    return null
  }

  static makevectors(globals, angles) {
    var input = angles == null ? _zeroVector() : angles
    var vectors = Engine.makeVectors(input)
    _applyVectors(globals, vectors)
    return vectors
  }

  static setorigin(globals, entity, origin) {
    if (entity == null) entity = globals.self
    if (entity == null) return
    Engine.setOrigin(entity, origin == null ? _zeroVector() : origin)
  }

  static setmodel(globals, entity, model) {
    if (entity == null) entity = globals.self
    if (entity == null) return null
    return Engine.setModel(entity, model)
  }

  static setsize(globals, entity, mins, maxs) {
    if (entity == null) entity = globals.self
    if (entity == null) return
    Engine.setSize(entity, mins == null ? _zeroVector() : mins, maxs == null ? _zeroVector() : maxs)
  }

  static break(globals) {
    Engine.log("QuakeC break invoked")
    Fiber.abort("break")
  }

  static sound(globals, entity, channel, sample, volume, attenuation) {
    if (entity == null) entity = globals.self
    if (entity == null) return
    var vol = volume == null ? 1.0 : volume
    var atten = attenuation == null ? 1.0 : attenuation
    Engine.playSound(entity, channel == null ? 0 : channel, sample, vol, atten)
  }

  static normalize(globals, vector) {
    return _normalizeVector(vector)
  }

  static error(globals, message) {
    var text = message == null ? "Unknown QuakeC error" : message
    Engine.log("QuakeC error: " + text)
    Fiber.abort(text)
  }

  static objerror(globals, message) {
    Engine.objError(message == null ? "Object error" : message)
  }

  static vlen(globals, vector) {
    return _vectorLength(vector)
  }

  static vectoyaw(globals, vector) {
    if (vector == null) return 0.0
    var angles = Engine.vectorToAngles(vector)
    return angles == null ? 0.0 : angles[1]
  }

  static remove(globals, entity) {
    if (entity == null) entity = globals.self
    if (entity == null) return
    Engine.removeEntity(entity)
  }

  static traceline(globals, start, end, ignoreMonsters, forEnt) {
    var trace = Engine.traceLine(start, end, ignoreMonsters, forEnt)
    if (trace == null) {
      globals.traceAllSolid = 0.0
      globals.traceStartSolid = 0.0
      globals.traceFraction = 1.0
      globals.traceEndPos = _zeroVector()
      globals.tracePlaneNormal = _zeroVector()
      globals.tracePlaneDist = 0.0
      globals.traceEnt = null
      globals.traceInOpen = 0.0
      globals.traceInWater = 0.0
      return null
    }

    globals.traceAllSolid = trace.containsKey("allSolid") && trace["allSolid"] ? 1.0 : 0.0
    globals.traceStartSolid = trace.containsKey("startSolid") && trace["startSolid"] ? 1.0 : 0.0
    globals.traceFraction = trace.containsKey("fraction") ? trace["fraction"] : 1.0
    globals.traceEndPos = trace.containsKey("endpos") ? trace["endpos"] : _zeroVector()
    globals.tracePlaneNormal = trace.containsKey("planeNormal") ? trace["planeNormal"] : _zeroVector()
    globals.tracePlaneDist = trace.containsKey("planeDist") ? trace["planeDist"] : 0.0
    globals.traceEnt = trace.containsKey("entity") ? trace["entity"] : null
    globals.traceInOpen = trace.containsKey("inOpen") && trace["inOpen"] ? 1.0 : 0.0
    globals.traceInWater = trace.containsKey("inWater") && trace["inWater"] ? 1.0 : 0.0
    return trace
  }

  static checkclient(globals) {
    return Engine.checkClient()
  }

  static precache_sound(globals, path) { return Engine.precacheSound(path) }
  static precache_model(globals, path) { return Engine.precacheModel(path) }

  static stuffcmd(globals, client, command) {
    if (client == null) client = globals.self
    if (client == null) return
    Engine.stuffCommand(client, command)
  }

  static findradius(globals, origin, radius) {
    return Engine.findRadius(origin, radius)
  }

  static bprint(globals, messageId, args) {
    var payload = args == null ? [] : args
    Engine.broadcastPrint(messageId, payload)
  }

  static sprint(globals, client, messageId, args) {
    if (client == null) client = globals.self
    if (client == null) return
    var payload = args == null ? [] : args
    Engine.playerPrint(client, messageId, payload)
  }

  static dprint(globals, message) {
    Engine.log(message == null ? "" : message)
  }

  static ftos(globals, value) {
    if (value == null) return "0"
    return value.toString
  }

  static vtos(globals, vector) {
    if (vector == null) vector = _zeroVector()
    return vector[0].toString + " " + vector[1].toString + " " + vector[2].toString
  }

  static coredump(globals) {
    Engine.log("coredump requested")
  }

  static traceon(globals) { Engine.log("traceon") }
  static traceoff(globals) { Engine.log("traceoff") }

  static eprint(globals, entity) {
    if (entity == null) entity = globals.self
    if (entity == null) return
    var fields = entity.fields
    var lines = []
    for (key in fields.keys) {
      lines.add(key + ": " + fields[key].toString)
    }
    var output = "Entity dump:"
    for (line in lines) {
      output = output + "\n" + line
    }
    Engine.log(output)
  }

  static walkmove(globals, entity, yaw, distance) {
    if (entity == null) entity = globals.self
    if (entity == null) return false
    return Engine.walkMove(entity, yaw, distance)
  }

  static droptofloor(globals, entity) {
    if (entity == null) entity = globals.self
    if (entity == null) return 0.0
    return Engine.dropToFloor(entity)
  }

  static lightstyle(globals, style, value) {
    Engine.lightstyle(style, value)
  }

  static rint(globals, value) {
    if (value == null) return 0.0
    return value.round
  }

  static floor(globals, value) {
    if (value == null) return 0.0
    return value.floor
  }

  static ceil(globals, value) {
    if (value == null) return 0.0
    return value.ceil
  }

  static checkbottom(globals, entity) {
    if (entity == null) entity = globals.self
    if (entity == null) return false
    return Engine.checkBottom(entity)
  }

  static pointcontents(globals, origin) {
    return Engine.pointContents(origin)
  }

  static fabs(globals, value) {
    if (value == null) return 0.0
    return value.abs
  }

  static aim(globals, entity, speed) {
    if (entity == null) entity = globals.self
    if (entity == null) return _zeroVector()
    return Engine.aim(entity, speed == null ? 0.0 : speed)
  }

  static cvar(globals, name) {
    return Engine.cvar(name)
  }

  static localcmd(globals, command) {
    Engine.localCommand(command)
  }

  static find(globals, start, field, match) {
    return Engine.find(start, field, match)
  }

  static nextent(globals, entity) {
    return Engine.nextEntity(entity)
  }

  static particle(globals, origin, direction, color, count) {
    Engine.spawnParticles(origin, direction, color, count)
  }

  static ChangeYaw(globals, entity) {
    if (entity == null) entity = globals.self
    if (entity == null) return
    Engine.changeYaw(entity)
  }

  static vectoangles(globals, vector) {
    return Engine.vectorToAngles(vector)
  }

  static _write(globals, kind, channel, value) {
    var entity = _messageEntity(globals, channel)
    if (kind == "byte") {
      Engine.writeByte(channel, value, entity)
    } else if (kind == "char") {
      Engine.writeChar(channel, value, entity)
    } else if (kind == "short") {
      Engine.writeShort(channel, value, entity)
    } else if (kind == "long") {
      Engine.writeLong(channel, value, entity)
    } else if (kind == "coord") {
      Engine.writeCoord(channel, value, entity)
    } else if (kind == "angle") {
      Engine.writeAngle(channel, value, entity)
    } else if (kind == "string") {
      Engine.writeString(channel, value, entity)
    } else if (kind == "entity") {
      Engine.writeEntity(channel, value, entity)
    }
  }

  static WriteByte(globals, channel, value) { _write(globals, "byte", channel, value) }
  static WriteChar(globals, channel, value) { _write(globals, "char", channel, value) }
  static WriteShort(globals, channel, value) { _write(globals, "short", channel, value) }
  static WriteLong(globals, channel, value) { _write(globals, "long", channel, value) }
  static WriteCoord(globals, channel, value) { _write(globals, "coord", channel, value) }
  static WriteAngle(globals, channel, value) { _write(globals, "angle", channel, value) }
  static WriteString(globals, channel, value) { _write(globals, "string", channel, value) }
  static WriteEntity(globals, channel, value) { _write(globals, "entity", channel, value) }

  static movetogoal(globals, step) {
    if (globals.self == null) return
    Engine.moveToGoal(globals.self, step)
  }

  static precache_file(globals, path) { return Engine.precacheFile(path) }

  static makestatic(globals, entity) {
    if (entity == null) entity = globals.self
    if (entity == null) return
    Engine.makeStatic(entity)
  }

  static changelevel(globals, mapName) {
    Engine.changeLevel(mapName)
  }

  static cvar_set(globals, name, value) {
    Engine.cvarSet(name, value)
  }

  static centerprint(globals, client, messageId, args) {
    if (client == null) client = globals.self
    if (client == null) return
    var payload = args == null ? [] : args
    Engine.centerPrint(client, messageId, payload)
  }

  static ambientsound(globals, origin, sample, volume, attenuation) {
    Engine.ambientSound(origin, sample, volume == null ? 1.0 : volume, attenuation == null ? 1.0 : attenuation)
  }

  static precache_model2(globals, path) { return Engine.precacheModel2(path) }
  static precache_sound2(globals, path) { return Engine.precacheSound2(path) }
  static precache_file2(globals, path) { return Engine.precacheFile2(path) }

  static setspawnparms(globals, entity) {
    if (entity == null) entity = globals.self
    if (entity == null) return
    Engine.setSpawnParms(entity)
  }
}
