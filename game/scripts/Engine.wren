// Engine.wren
// Provides abstraction for engine-dependent operations required by the
// ported QuakeC gameplay code. Each static method is expected to be
// implemented by the embedding host. The default implementations abort to
// signal that a binding must be supplied before execution.

class Engine {
  static var _bindings = {}

  static registerBinding(name, handler) {
    if (handler == null) {
      Engine.clearBinding(name)
      return
    }
    _bindings[name] = handler
  }

  static clearBinding(name) {
    if (_bindings.containsKey(name)) {
      _bindings.remove(name)
    }
  }

  static hasBinding(name) {
    return _bindings.containsKey(name)
  }

  static _callBinding(name, args) {
    var handler = _bindings[name]
    if (handler == null) return null
    return handler.call(args)
  }
  static precacheFile(path) {
    _requireHost("precacheFile", [path])
  }

  static precacheFile2(path) {
    _requireHost("precacheFile2", [path])
  }

  static precacheSound(path) {
    _requireHost("precacheSound", [path])
  }

  static precacheSound2(path) {
    _requireHost("precacheSound2", [path])
  }

  static precacheModel(path) {
    _requireHost("precacheModel", [path])
  }

  static precacheModel2(path) {
    _requireHost("precacheModel2", [path])
  }

  static lightstyle(index, pattern) {
    _requireHost("lightstyle", [index, pattern])
  }

  static cvar(name) {
    return _requireHost("cvar", [name])
  }

  static cvarSet(name, value) {
    _requireHost("cvarSet", [name, value])
  }

  static random() {
    return _requireHost("random", [])
  }

  static findAll(scope, className) {
    return _requireHost("findAll", [scope, className])
  }

  static findByField(scope, field, value) {
    return _requireHost("findByField", [scope, field, value])
  }

  static spawnEntity() {
    return _requireHost("spawnEntity", [])
  }

  static makeStatic(entity) {
    _requireHost("makeStatic", [entity])
  }

  static removeEntity(entity) {
    _requireHost("removeEntity", [entity])
  }

  static setSpawnParms(entity) {
    _requireHost("setSpawnParms", [entity])
  }

  static setModel(entity, path) {
    return _requireHost("setModel", [entity, path])
  }

  static executeChangeLevel(trigger) {
    _requireHost("executeChangeLevel", [trigger])
  }

  static changeLevel(mapName) {
    _requireHost("changeLevel", [mapName])
  }

  static _writeMessage(name, channel, value, entity) {
    if (entity == null) {
      _requireHost(name, [channel, value])
    } else {
      _requireHost(name, [channel, value, entity])
    }
  }

  static writeByte(channel, value, entity) {
    _writeMessage("writeByte", channel, value, entity)
  }

  static writeChar(channel, value, entity) {
    _writeMessage("writeChar", channel, value, entity)
  }

  static writeShort(channel, value, entity) {
    _writeMessage("writeShort", channel, value, entity)
  }

  static writeLong(channel, value, entity) {
    _writeMessage("writeLong", channel, value, entity)
  }

  static writeString(channel, value, entity) {
    _writeMessage("writeString", channel, value, entity)
  }

  static writeCoord(channel, value, entity) {
    _writeMessage("writeCoord", channel, value, entity)
  }

  static writeAngle(channel, value, entity) {
    _writeMessage("writeAngle", channel, value, entity)
  }

  static writeEntity(channel, value, entity) {
    _writeMessage("writeEntity", channel, value, entity)
  }

  static broadcastPrint(messageId, args) {
    _requireHost("broadcastPrint", [messageId, args])
  }

  static playSound(entity, channel, sample, volume, attenuation) {
    _requireHost("playSound", [entity, channel, sample, volume, attenuation])
  }

  static centerPrint(entity, message) {
    _requireHost("centerPrint", [entity, message])
  }

  static ambientSound(origin, sample, volume, attenuation) {
    _requireHost("ambientSound", [origin, sample, volume, attenuation])
  }

  static emitTempEntity(code, data) {
    _requireHost("emitTempEntity", [code, data])
  }

  static applyDamage(target, inflictor, attacker, amount) {
    _requireHost("applyDamage", [target, inflictor, attacker, amount])
  }

  static runWeaponFrame(player) {
    _requireHost("runWeaponFrame", [player])
  }

  static setCurrentAmmo(player) {
    _requireHost("setCurrentAmmo", [player])
  }

  static selectBestWeapon(player) {
    return _requireHost("selectBestWeapon", [player])
  }

  static useTargets(entity, activator) {
    _requireHost("useTargets", [entity, activator])
  }

  static callEntityFunction(entity, functionName, args) {
    _requireHost("callEntityFunction", [entity, functionName, args])
  }

  static callGlobalFunction(functionName, entity, args) {
    _requireHost("callGlobalFunction", [functionName, entity, args])
  }

  static initTrigger(entity) {
    _requireHost("initTrigger", [entity])
  }

  static setTriggerTouch(entity, handler) {
    _requireHost("setTriggerTouch", [entity, handler])
  }

  static clearTriggerTouch(entity) {
    _requireHost("clearTriggerTouch", [entity])
  }

  static scheduleThink(entity, handler, delay) {
    _requireHost("scheduleThink", [entity, handler, delay])
  }

  static objError(message) {
    _requireHost("objError", [message])
  }

  static localCommand(command) {
    _requireHost("localCommand", [command])
  }

  static stuffCommand(entity, command) {
    _requireHost("stuffCommand", [entity, command])
  }

  static playerPrint(entity, messageId, args) {
    _requireHost("playerPrint", [entity, messageId, args])
  }

  static localSound(entity, sample) {
    _requireHost("localSound", [entity, sample])
  }

  static bitAnd(a, b) {
    return _requireHost("bitAnd", [a, b])
  }

  static bitOr(a, b) {
    return _requireHost("bitOr", [a, b])
  }

  static bitOrMany(values) {
    var result = 0
    for (value in values) {
      result = bitOr(result, value)
    }
    return result
  }

  static log(message) {
    _requireHost("log", [message])
  }

  static setOrigin(entity, origin) {
    _requireHost("setOrigin", [entity, origin])
  }

  static setSize(entity, mins, maxs) {
    _requireHost("setSize", [entity, mins, maxs])
  }

  static changeYaw(entity) {
    _requireHost("changeYaw", [entity])
  }

  static moveToGoal(entity, step) {
    _requireHost("moveToGoal", [entity, step])
  }

  static walkMove(entity, yaw, distance) {
    return _requireHost("walkMove", [entity, yaw, distance])
  }

  static dropToFloor(entity) {
    return _requireHost("dropToFloor", [entity])
  }

  static checkBottom(entity) {
    return _requireHost("checkBottom", [entity])
  }

  static findRadius(origin, radius) {
    return _requireHost("findRadius", [origin, radius])
  }

  static checkClient() {
    return _requireHost("checkClient", [])
  }

  static find(start, field, value) {
    return _requireHost("find", [start, field, value])
  }

  static nextEntity(entity) {
    return _requireHost("nextEntity", [entity])
  }

  static traceLine(start, end, ignoreMonsters, ignoreEntity) {
    return _requireHost("traceLine", [start, end, ignoreMonsters, ignoreEntity])
  }

  static pointContents(origin) {
    return _requireHost("pointContents", [origin])
  }

  static makeVectors(angles) {
    return _requireHost("makeVectors", [angles])
  }

  static vectorToAngles(vector) {
    return _requireHost("vectorToAngles", [vector])
  }

  static aim(shooter, distance) {
    return _requireHost("aim", [shooter, distance])
  }

  static spawnParticles(origin, velocity, color, count) {
    _requireHost("spawnParticles", [origin, velocity, color, count])
  }

  static isVisible(observer, target) {
    return _requireHost("isVisible", [observer, target])
  }

  static spawnTeleportFog(origin) {
    _requireHost("spawnTeleportFog", [origin])
  }

  static spawnTeleportDeath(origin, owner, mins, maxs) {
    _requireHost("spawnTeleportDeath", [origin, owner, mins, maxs])
  }

  static walkPathToGoal(entity, distance, goal) {
    return _requireHost("walkPathToGoal", [entity, distance, goal])
  }

  static botMoveToPoint(bot, point) {
    return _requireHost("botMoveToPoint", [bot, point])
  }

  static botFollowEntity(bot, goal) {
    return _requireHost("botFollowEntity", [bot, goal])
  }

  static checkPlayerEXFlags(player) {
    return _requireHost("checkPlayerEXFlags", [player])
  }

  static checkExtension(name) {
    return _requireHost("checkExtension", [name])
  }

  static drawPoint(point, colormap, lifetime, depthTest) {
    _requireHost("drawPoint", [point, colormap, lifetime, depthTest])
  }

  static drawLine(start, end, colormap, lifetime, depthTest) {
    _requireHost("drawLine", [start, end, colormap, lifetime, depthTest])
  }

  static drawArrow(start, end, colormap, size, lifetime, depthTest) {
    _requireHost("drawArrow", [start, end, colormap, size, lifetime, depthTest])
  }

  static drawRay(start, direction, length, colormap, size, lifetime, depthTest) {
    _requireHost("drawRay", [start, direction, length, colormap, size, lifetime, depthTest])
  }

  static drawCircle(origin, radius, colormap, lifetime, depthTest) {
    _requireHost("drawCircle", [origin, radius, colormap, lifetime, depthTest])
  }

  static drawBounds(mins, maxs, colormap, lifetime, depthTest) {
    _requireHost("drawBounds", [mins, maxs, colormap, lifetime, depthTest])
  }

  static drawWorldText(text, origin, size, lifetime, depthTest) {
    _requireHost("drawWorldText", [text, origin, size, lifetime, depthTest])
  }

  static drawSphere(origin, radius, colormap, lifetime, depthTest) {
    _requireHost("drawSphere", [origin, radius, colormap, lifetime, depthTest])
  }

  static drawCylinder(origin, halfHeight, radius, colormap, lifetime, depthTest) {
    _requireHost("drawCylinder", [origin, halfHeight, radius, colormap, lifetime, depthTest])
  }

  static finaleFinished() {
    return _requireHost("finaleFinished", [])
  }

  static time() {
    return _requireHost("time", [])
  }

  static _requireHost(name, args) {
    if (Engine.hasBinding(name)) {
      return Engine._callBinding(name, args)
    }
    Fiber.abort("Engine.%s requires a host implementation (args: %(_))." % [name, args])
  }
}
