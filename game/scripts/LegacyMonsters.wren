// LegacyMonsters.wren
// Provides temporary adapters that allow existing QuakeC monster definitions
// to be invoked from the Wren runtime while native ports are still underway.

import "./Engine" for Engine

class LegacyMonstersModule {
  static var _announced = {}

  static _announceOnce(name) {
    if (_announced.containsKey(name)) return
    Engine.log("Legacy fallback: delegating monster behavior to QuakeC function '" + name + "'.")
    _announced[name] = true
  }

  static spawn(globals, monster, qcFunction) {
    if (qcFunction == null || qcFunction == "") return
    LegacyMonstersModule._announceOnce(qcFunction)

    var previousSelf = globals.self
    var previousOther = globals.other
    globals.self = monster
    Engine.callGlobalFunction(qcFunction, monster, [])
    globals.self = previousSelf
    globals.other = previousOther
  }
}
