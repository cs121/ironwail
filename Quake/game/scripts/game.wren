import "./Engine" for Engine

class Game {
  static init() {
    // Initialize default state when the server boots.
  }

  static spawnEntities() {
    // Use the legacy QuakeC entity loader until the Wren port is complete.
    Engine.spawnFromMap()
  }

  static startFrame(dt) {
    Fiber.abort("NotImplemented")
  }

  static clientConnect(id) {
    Fiber.abort("NotImplemented")
  }

  static clientDisconnect(id) {
    Fiber.abort("NotImplemented")
  }

  static onSave() {
    Fiber.abort("NotImplemented")
  }

  static onLoad() {
    Fiber.abort("NotImplemented")
  }
}
