foreign class Engine {
  foreign static time
  foreign static setSkill(value)
  foreign static getSkill
  foreign static loadMap(name)
  foreign static spawnFromMap
  foreign static randf
  foreign static randi(max)
  foreign static crandom
}

foreign class Entity {
  foreign static spawn()
  foreign remove()
  foreign link()
  foreign origin
  foreign origin=(value)
  foreign angles
  foreign angles=(value)
  foreign velocity
  foreign velocity=(value)
  foreign model
  foreign model=(value)
  foreign solid
  foreign solid=(value)
  foreign movetype
  foreign movetype=(value)
  foreign health
  foreign health=(value)
  foreign takedamage
  foreign takedamage=(value)
  foreign walkMove(yaw, dist)
  foreign moveToGoal(dist)
  foreign changeYaw()
}

class Game {
  static init() {
    System.print("Wren Game.init() ready for skill %(Engine.getSkill())")
  }

  static spawnEntities() {
    System.print("Wren spawning entities at t=%(Engine.time)")
    Engine.spawnFromMap()
  }

  static startFrame(dt) {
    // Default implementation does nothing; QuakeC StartFrame is bypassed.
  }

  static clientConnect(id) {
    Fiber.abort("NotImplemented")
  }

  static clientDisconnect(id) {
    Fiber.abort("NotImplemented")
  }

  static onSave() {
    // Hook for persisting Wren-managed state.
  }

  static onLoad() {
    // Hook for restoring Wren-managed state after savegame load.
  }
}
