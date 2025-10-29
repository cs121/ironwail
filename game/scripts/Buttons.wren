// Buttons.wren
// Ports the func_button logic from buttons.qc so brush buttons behave like
// their QuakeC counterparts when running under the Wren gameplay layer.

import "./Engine" for Engine
import "./Globals" for SolidTypes, MoveTypes, DamageValues, Channels, Attenuations, MoverStates
import "./Subs" for SubsModule

class ButtonsModule {
  static _vectorAdd(a, b) {
    return [a[0] + b[0], a[1] + b[1], a[2] + b[2]]
  }

  static _vectorSub(a, b) {
    return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
  }

  static _vectorScale(v, scalar) {
    return [v[0] * scalar, v[1] * scalar, v[2] * scalar]
  }

  static _vectorAbs(v) {
    return [v[0].abs, v[1].abs, v[2].abs]
  }

  static _vectorDot(a, b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
  }

  static buttonWait(globals, button) {
    if (button == null) return

    button.set("state", MoverStates.TOP)
    button.set("frame", 1)

    var activator = button.get("enemy", null)
    SubsModule.useTargets(globals, button, activator)

    var wait = button.get("wait", 1.0)
    if (wait < 0) {
      button.set("think", null)
      button.set("nextthink", -1)
      return
    }

    var ltime = button.get("ltime", Engine.time())
    var fireTime = ltime + wait
    button.set("think", "ButtonsModule.buttonReturn")
    button.set("nextthink", fireTime)
    Engine.scheduleThink(button, "ButtonsModule.buttonReturn", wait)
  }

  static buttonDone(globals, button) {
    if (button == null) return
    button.set("state", MoverStates.BOTTOM)
  }

  static buttonReturn(globals, button) {
    if (button == null) return

    button.set("state", MoverStates.DOWN)
    var pos1 = button.get("pos1", button.get("origin", [0, 0, 0]))
    var speed = button.get("speed", 40)
    SubsModule.calcMove(globals, button, pos1, speed, "ButtonsModule.buttonDone")
    button.set("frame", 0)

    if (button.get("health", 0) > 0) {
      button.set("takedamage", DamageValues.YES)
    }
  }

  static buttonBlocked(globals, button, other) {
    // Original QuakeC intentionally left this empty to let the button stop
    // without additional side effects.
  }

  static buttonFire(globals, button) {
    if (button == null) return

    var state = button.get("state", MoverStates.BOTTOM)
    if (state == MoverStates.UP || state == MoverStates.TOP) {
      return
    }

    var noise = button.get("noise", null)
    if (noise != null && noise != "") {
      Engine.playSound(button, Channels.VOICE, noise, 1, Attenuations.NORMAL)
    }

    button.set("state", MoverStates.UP)
    var pos2 = button.get("pos2", button.get("origin", [0, 0, 0]))
    var speed = button.get("speed", 40)
    SubsModule.calcMove(globals, button, pos2, speed, "ButtonsModule.buttonWait")

    if (button.get("health", 0) > 0) {
      button.set("takedamage", DamageValues.NO)
    }
  }

  static buttonUse(globals, button, activator) {
    if (button == null) return
    button.set("enemy", activator)
    ButtonsModule.buttonFire(globals, button)
  }

  static buttonTouch(globals, button, other) {
    if (button == null) return
    if (other == null) return
    if (other.get("classname", "") != "player") return

    button.set("enemy", other)
    ButtonsModule.buttonFire(globals, button)
  }

  static buttonKilled(globals, button) {
    if (button == null) return
    button.set("enemy", globals.damageAttacker)
    button.set("health", button.get("max_health", button.get("health", 0)))
    button.set("takedamage", DamageValues.NO)
    ButtonsModule.buttonFire(globals, button)
  }

  static _configureSound(button) {
    var sounds = button.get("sounds", 0)
    if (sounds == 0) {
      Engine.precacheSound("buttons/airbut1.wav")
      button.set("noise", "buttons/airbut1.wav")
    } else if (sounds == 1) {
      Engine.precacheSound("buttons/switch21.wav")
      button.set("noise", "buttons/switch21.wav")
    } else if (sounds == 2) {
      Engine.precacheSound("buttons/switch02.wav")
      button.set("noise", "buttons/switch02.wav")
    } else if (sounds == 3) {
      Engine.precacheSound("buttons/switch04.wav")
      button.set("noise", "buttons/switch04.wav")
    }
  }

  static _computeEndPosition(button) {
    var origin = button.get("origin", [0, 0, 0])
    var movedir = button.get("movedir", [0, 0, 0])
    var mins = button.get("mins", [0, 0, 0])
    var maxs = button.get("maxs", [0, 0, 0])
    var size = ButtonsModule._vectorSub(maxs, mins)
    var travel = ButtonsModule._vectorDot(ButtonsModule._vectorAbs(movedir), size) - button.get("lip", 4)
    if (travel < 0) travel = 0
    return ButtonsModule._vectorAdd(origin, ButtonsModule._vectorScale(movedir, travel))
  }

  static funcButton(globals, button) {
    if (button == null) return

    ButtonsModule._configureSound(button)

    SubsModule.setMoveDir(globals, button)

    button.set("classname", "func_button")
    button.set("movetype", MoveTypes.PUSH)
    button.set("solid", SolidTypes.BSP)
    Engine.setModel(button, button.get("model", ""))

    button.set("blocked", "ButtonsModule.buttonBlocked")
    button.set("use", "ButtonsModule.buttonUse")

    if (button.get("health", 0) > 0) {
      button.set("max_health", button.get("health", 0))
      button.set("th_die", "ButtonsModule.buttonKilled")
      button.set("takedamage", DamageValues.YES)
    } else {
      button.set("touch", "ButtonsModule.buttonTouch")
    }

    if (button.get("speed", 0) == 0) {
      button.set("speed", 40)
    }

    if (!button.fields.containsKey("wait")) {
      button.set("wait", 1)
    }

    if (button.get("lip", 0) == 0) {
      button.set("lip", 4)
    }

    button.set("state", MoverStates.BOTTOM)
    button.set("frame", 0)
    button.set("pos1", button.get("origin", [0, 0, 0]))
    button.set("pos2", ButtonsModule._computeEndPosition(button))
  }

  // ------------------------------------------------------------------------
  // Compatibility wrappers -------------------------------------------------

  static button_wait(globals, button) { ButtonsModule.buttonWait(globals, button) }
  static button_done(globals, button) { ButtonsModule.buttonDone(globals, button) }
  static button_return(globals, button) { ButtonsModule.buttonReturn(globals, button) }
  static button_blocked(globals, button, other) {
    ButtonsModule.buttonBlocked(globals, button, other)
  }
  static button_fire(globals, button) { ButtonsModule.buttonFire(globals, button) }
  static button_use(globals, button, activator) {
    ButtonsModule.buttonUse(globals, button, activator)
  }
  static button_touch(globals, button, other) {
    ButtonsModule.buttonTouch(globals, button, other)
  }
  static button_killed(globals, button) { ButtonsModule.buttonKilled(globals, button) }
  static func_button(globals, button) { ButtonsModule.funcButton(globals, button) }
}
