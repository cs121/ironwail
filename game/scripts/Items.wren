// Items.wren
// Ports item spawning, pickup, and respawn behavior from items.qc so that the
// Wren gameplay layer mirrors the original QuakeC logic.

import "./Engine" for Engine
import "./Globals" for SolidTypes, MoveTypes, PlayerFlags, Items, WorldTypes
import "./Globals" for Channels, Attenuations
import "./ItemNames" for ItemNamesModule
import "./Subs" for SubsModule
import "./Weapons" for WeaponsModule

class ItemsModule {
  // ------------------------------------------------------------------------
  // Helpers ----------------------------------------------------------------

  static _vectorAdd(a, b) {
    return [a[0] + b[0], a[1] + b[1], a[2] + b[2]]
  }

  static _vectorSub(a, b) {
    return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
  }

  static _vectorScale(v, scalar) {
    return [v[0] * scalar, v[1] * scalar, v[2] * scalar]
  }

  static _hasFlag(entity, field, flag) {
    if (entity == null) return false
    var value = entity.get(field, 0).floor
    return Engine.bitAnd(value, flag) != 0
  }

  static _addFlag(entity, field, flag) {
    if (entity == null) return
    var value = entity.get(field, 0).floor
    entity.set(field, Engine.bitOr(value, flag))
  }

  static _removeFlags(entity, field, mask) {
    if (entity == null) return
    var value = entity.get(field, 0).floor
    var present = Engine.bitAnd(value, mask)
    entity.set(field, value - present)
  }

  static _setThink(globals, entity, functionName, delay) {
    if (entity == null) return
    entity.set("think", functionName)
    if (delay != null && delay > 0) {
      var next = globals.time + delay
      entity.set("nextthink", next)
      Engine.scheduleThink(entity, functionName, delay)
    } else {
      entity.set("nextthink", -1)
    }
  }

  static _clearForPickup(entity) {
    if (entity == null) return
    entity.set("solid", SolidTypes.NOT)
    entity.set("model", "")
    Engine.setModel(entity, "")
  }

  static _prepareRespawn(globals, entity, delay) {
    if (entity == null) return
    entity.set("think", "ItemsModule.SUB_regen")
    if (delay != null && delay > 0) {
      var when = globals.time + delay
      entity.set("nextthink", when)
      Engine.scheduleThink(entity, "ItemsModule.SUB_regen", delay)
    } else {
      entity.set("nextthink", -1)
    }
  }

  static _playerPrintPickup(player, messageId, arg) {
    if (player == null) return
    if (arg == null) {
      Engine.playerPrint(player, messageId, [])
    } else {
      Engine.playerPrint(player, messageId, [arg])
    }
  }

  static _stuffBf(player) {
    if (player == null) return
    Engine.stuffCommand(player, "bf\n")
  }

  static _playPickupSound(player, sample) {
    if (player == null || sample == null || sample == "") return
    Engine.playSound(player, Channels.ITEM, sample, 1, Attenuations.NORMAL)
  }

  static _ensureNetName(globals, item) {
    if (item == null) return
    var current = item.get("netname", null)
    if (current != null && current != "") return

    var itemsBits = item.get("items", 0)
    if (itemsBits != 0) {
      var name = ItemNamesModule.getNetName(globals, itemsBits)
      if (name != null && name != "") {
        item.set("netname", name)
        return
      }
    }

    var weaponBits = item.get("weapon", 0)
    if (weaponBits != 0) {
      var name = ItemNamesModule.getNetName(globals, weaponBits)
      if (name != null && name != "") {
        item.set("netname", name)
      }
    }
  }

  // ------------------------------------------------------------------------
  // Respawn helpers --------------------------------------------------------

  static subRegen(globals, item) {
    if (item == null) return

    var model = item.get("mdl", item.get("model", ""))
    if (model != null && model != "") {
      item.set("model", model)
      Engine.setModel(item, model)
    }

    item.set("solid", SolidTypes.TRIGGER)
    Engine.playSound(item, Channels.VOICE, "items/itembk2.wav", 1, Attenuations.NORMAL)
    Engine.setOrigin(item, item.get("origin", [0, 0, 0]))
    item.set("nextthink", -1)
  }

  static placeItem(globals, item) {
    if (item == null) return

    ItemsModule._ensureNetName(globals, item)
    item.set("mdl", item.get("model", ""))
    item.set("flags", PlayerFlags.ITEM)
    item.set("solid", SolidTypes.TRIGGER)
    item.set("movetype", MoveTypes.TOSS)
    item.set("velocity", [0, 0, 0])

    var origin = item.get("origin", [0, 0, 0])
    origin = [origin[0], origin[1], origin[2] + 6]
    item.set("origin", origin)

    if (!Engine.dropToFloor(item)) {
      Engine.log("Bonus item fell out of level at %(_)." % [item.get("origin", [0, 0, 0])])
      Engine.removeEntity(item)
      return
    }
  }

  static startItem(globals, item) {
    if (item == null) return

    var nextThink = globals.time + 0.2
    item.set("nextthink", nextThink)
    item.set("think", "ItemsModule.PlaceItem")
    Engine.scheduleThink(item, "ItemsModule.PlaceItem", 0.2)
  }

  static noclass(globals, entity) {
    if (entity == null) return
    var origin = entity.get("origin", [0, 0, 0])
    Engine.log("noclass spawned at %(_)." % [origin])
    Engine.removeEntity(entity)
  }

  // ------------------------------------------------------------------------
  // Health -----------------------------------------------------------------

  static tHeal(globals, target, healAmount, ignoreMax) {
    if (target == null) return false
    if (target.get("health", 0) <= 0) return false

    var maxHealth = target.get("max_health", target.get("health", 0))
    if (!ignoreMax && target.get("health", 0) >= maxHealth) return false

    var amount = healAmount.ceil
    var newHealth = target.get("health", 0) + amount
    target.set("health", newHealth)

    if (!ignoreMax && target.get("health", 0) >= maxHealth) {
      target.set("health", maxHealth)
    }

    if (target.get("health", 0) > 250) {
      target.set("health", 250)
    }

    return true
  }

  static itemHealth(globals, item) {
    if (item == null) return

    item.set("touch", "ItemsModule.healthTouch")

    var spawnflags = item.get("spawnflags", 0)
    if (Engine.bitAnd(spawnflags, 1) != 0) { // Rotten
      Engine.precacheModel("maps/b_bh10.bsp")
      Engine.precacheSound("items/r_item1.wav")
      Engine.setModel(item, "maps/b_bh10.bsp")
      item.set("noise", "items/r_item1.wav")
      item.set("healamount", 15)
      item.set("healtype", 0)
    } else if (Engine.bitAnd(spawnflags, 2) != 0) { // Mega
      Engine.precacheModel("maps/b_bh100.bsp")
      Engine.precacheSound("items/r_item2.wav")
      Engine.setModel(item, "maps/b_bh100.bsp")
      item.set("noise", "items/r_item2.wav")
      item.set("healamount", 100)
      item.set("healtype", 2)
    } else {
      Engine.precacheModel("maps/b_bh25.bsp")
      Engine.precacheSound("items/health1.wav")
      Engine.setModel(item, "maps/b_bh25.bsp")
      item.set("noise", "items/health1.wav")
      item.set("healamount", 25)
      item.set("healtype", 1)
    }

    Engine.setSize(item, [0, 0, 0], [32, 32, 56])
    ItemsModule.startItem(globals, item)
  }

  static healthTouch(globals, item, other) {
    if (item == null || other == null) return
    if (other.get("classname", "") != "player") return

    var healType = item.get("healtype", 0)
    var healAmount = item.get("healamount", 0)

    if (healType == 2) {
      if (other.get("health", 0) >= 250) return
      if (!ItemsModule.tHeal(globals, other, healAmount, true)) return
      ItemsModule._addFlag(other, "items", Items.SUPERHEALTH)
      other.set("healthrot_nextcheck", globals.time + 5)
    } else {
      if (!ItemsModule.tHeal(globals, other, healAmount, false)) return
    }

    ItemsModule._playerPrintPickup(other, "$qc_item_health", healAmount.toString)
    ItemsModule._playPickupSound(other, item.get("noise", ""))
    ItemsModule._stuffBf(other)

    ItemsModule._clearForPickup(item)

    var deathmatch = globals.deathmatch
    if (deathmatch > 0 && deathmatch != 2) {
      var delay = (healType == 2) ? 120.0 : 20.0
      ItemsModule._prepareRespawn(globals, item, delay)
    } else {
      ItemsModule._prepareRespawn(globals, item, 0)
    }

    SubsModule.useTargets(globals, item, other)
  }

  // ------------------------------------------------------------------------
  // Armor ------------------------------------------------------------------

  static boundOtherAmmo(globals, player) {
    if (player == null) return
    if (player.get("ammo_shells", 0) > 100) player.set("ammo_shells", 100)
    if (player.get("ammo_nails", 0) > 200) player.set("ammo_nails", 200)
    if (player.get("ammo_rockets", 0) > 100) player.set("ammo_rockets", 100)
    if (player.get("ammo_cells", 0) > 100) player.set("ammo_cells", 100)
  }

  static armorTouch(globals, item, other) {
    if (item == null || other == null) return
    if (other.get("classname", "") != "player") return
    if (other.get("health", 0) <= 0) return

    var type = item.get("armortype", 0.3)
    var value = item.get("armorvalue", 100)
    var bit = item.get("armorbit", Items.ARMOR1)

    var currentType = other.get("armortype", 0.0)
    var currentValue = other.get("armorvalue", 0.0)
    if (currentType * currentValue >= type * value) return

    other.set("armortype", type)
    other.set("armorvalue", value)

    var armorMask = Engine.bitOrMany([Items.ARMOR1, Items.ARMOR2, Items.ARMOR3])
    ItemsModule._removeFlags(other, "items", armorMask)
    ItemsModule._addFlag(other, "items", bit)

    ItemsModule._clearForPickup(item)

    var deathmatch = globals.deathmatch
    if (deathmatch > 0 && deathmatch != 2) {
      ItemsModule._prepareRespawn(globals, item, 20.0)
    } else {
      ItemsModule._prepareRespawn(globals, item, 0)
    }

    ItemsModule._playerPrintPickup(other, "$qc_item_armor", null)
    ItemsModule._playPickupSound(other, "items/armor1.wav")
    ItemsModule._stuffBf(other)

    SubsModule.useTargets(globals, item, other)
  }

  static itemArmor(globals, item, armortype, armorvalue, skin, bit) {
    if (item == null) return
    item.set("touch", "ItemsModule.armorTouch")
    item.set("armortype", armortype)
    item.set("armorvalue", armorvalue)
    item.set("armorbit", bit)
    Engine.precacheModel("progs/armor.mdl")
    Engine.setModel(item, "progs/armor.mdl")
    item.set("skin", skin)
    Engine.setSize(item, [-16, -16, 0], [16, 16, 56])
    ItemsModule.startItem(globals, item)
  }

  static itemArmor1(globals, item) {
    ItemsModule.itemArmor(globals, item, 0.3, 100, 0, Items.ARMOR1)
  }

  static itemArmor2(globals, item) {
    ItemsModule.itemArmor(globals, item, 0.6, 150, 1, Items.ARMOR2)
  }

  static itemArmorInv(globals, item) {
    ItemsModule.itemArmor(globals, item, 0.8, 200, 2, Items.ARMOR3)
  }

  // ------------------------------------------------------------------------
  // Weapons ----------------------------------------------------------------

  static rankForWeapon(globals, weapon) {
    if (weapon == Items.LIGHTNING) return 1
    if (weapon == Items.ROCKET_LAUNCHER) return 2
    if (weapon == Items.SUPER_NAILGUN) return 3
    if (weapon == Items.GRENADE_LAUNCHER) return 4
    if (weapon == Items.SUPER_SHOTGUN) return 5
    if (weapon == Items.NAILGUN) return 6
    return 7
  }

  static weaponCode(globals, weapon) {
    if (weapon == Items.SUPER_SHOTGUN) return 3
    if (weapon == Items.NAILGUN) return 4
    if (weapon == Items.SUPER_NAILGUN) return 5
    if (weapon == Items.GRENADE_LAUNCHER) return 6
    if (weapon == Items.ROCKET_LAUNCHER) return 7
    if (weapon == Items.LIGHTNING) return 8
    return 1
  }

  static deathmatchWeapon(globals, player, oldWeapon, newWeapon) {
    if (player == null) return
    if (ItemsModule._hasFlag(player, "flags", PlayerFlags.ISBOT)) return

    var currentRank = ItemsModule.rankForWeapon(globals, player.get("weapon", Items.SHOTGUN))
    var newRank = ItemsModule.rankForWeapon(globals, newWeapon)
    if (newRank < currentRank) {
      player.set("weapon", newWeapon)
    }
  }

  static _giveWeaponAmmo(player, ammoField, amount) {
    if (player == null) return
    var current = player.get(ammoField, 0)
    player.set(ammoField, current + amount)
  }

  static weaponTouch(globals, item, other) {
    if (item == null || other == null) return
    if (!ItemsModule._hasFlag(other, "flags", PlayerFlags.CLIENT)) return
    if (other.get("health", 0) <= 0) return

    var best = WeaponsModule.W_BestWeapon(globals, other)
    var leave = globals.coop > 0 || globals.deathmatch == 2 || globals.deathmatch == 3 || globals.deathmatch == 5

    var className = item.get("classname", "")
    var newWeapon = item.get("weapon", 0)

    if (className == "weapon_nailgun") {
      if (leave && Engine.bitAnd(other.get("items", 0).floor, Items.NAILGUN) != 0) return
      newWeapon = Items.NAILGUN
      ItemsModule._giveWeaponAmmo(other, "ammo_nails", 30)
    } else if (className == "weapon_supernailgun") {
      if (leave && Engine.bitAnd(other.get("items", 0).floor, Items.SUPER_NAILGUN) != 0) return
      newWeapon = Items.SUPER_NAILGUN
      ItemsModule._giveWeaponAmmo(other, "ammo_nails", 30)
    } else if (className == "weapon_supershotgun") {
      if (leave && Engine.bitAnd(other.get("items", 0).floor, Items.SUPER_SHOTGUN) != 0) return
      newWeapon = Items.SUPER_SHOTGUN
      ItemsModule._giveWeaponAmmo(other, "ammo_shells", 5)
    } else if (className == "weapon_rocketlauncher") {
      if (leave && Engine.bitAnd(other.get("items", 0).floor, Items.ROCKET_LAUNCHER) != 0) return
      newWeapon = Items.ROCKET_LAUNCHER
      ItemsModule._giveWeaponAmmo(other, "ammo_rockets", 5)
    } else if (className == "weapon_grenadelauncher") {
      if (leave && Engine.bitAnd(other.get("items", 0).floor, Items.GRENADE_LAUNCHER) != 0) return
      newWeapon = Items.GRENADE_LAUNCHER
      ItemsModule._giveWeaponAmmo(other, "ammo_rockets", 5)
    } else if (className == "weapon_lightning") {
      if (leave && Engine.bitAnd(other.get("items", 0).floor, Items.LIGHTNING) != 0) return
      newWeapon = Items.LIGHTNING
      ItemsModule._giveWeaponAmmo(other, "ammo_cells", 15)
    } else {
      Engine.objError("weapon_touch: unknown classname")
      return
    }

    ItemsModule.boundOtherAmmo(globals, other)

    ItemsModule._playerPrintPickup(other, "$qc_got_item", item.get("netname", ""))
    ItemsModule._playPickupSound(other, "weapons/pkup.wav")
    ItemsModule._stuffBf(other)

    var oldItems = other.get("items", 0).floor
    other.set("items", Engine.bitOr(oldItems, newWeapon))

    if (WeaponsModule.W_WantsToChangeWeapon(globals, other, oldItems, other.get("items", 0))) {
      if (globals.deathmatch == 0) {
        other.set("weapon", newWeapon)
      } else {
        ItemsModule.deathmatchWeapon(globals, other, oldItems, newWeapon)
      }
    }

    WeaponsModule.W_SetCurrentAmmo(globals, other)

    if (leave) return

    ItemsModule._clearForPickup(item)

    var deathmatch = globals.deathmatch
    if (deathmatch > 0) {
      if (deathmatch == 3 || deathmatch == 5) {
        ItemsModule._prepareRespawn(globals, item, 15.0)
      } else if (deathmatch != 2) {
        ItemsModule._prepareRespawn(globals, item, 30.0)
      } else {
        ItemsModule._prepareRespawn(globals, item, 0)
      }
    } else {
      ItemsModule._prepareRespawn(globals, item, 0)
    }

    SubsModule.useTargets(globals, item, other)
  }

  static _setupWeaponItem(globals, item, model, weaponBit, netname) {
    if (item == null) return
    Engine.precacheModel(model)
    Engine.setModel(item, model)
    item.set("weapon", weaponBit)
    item.set("netname", netname)
    item.set("touch", "ItemsModule.weaponTouch")
    Engine.setSize(item, [-16, -16, 0], [16, 16, 56])
    ItemsModule.startItem(globals, item)
  }

  static weaponSupershotgun(globals, item) {
    ItemsModule._setupWeaponItem(globals, item, "progs/g_shot.mdl", Items.SUPER_SHOTGUN, "$qc_double_shotgun")
  }

  static weaponNailgun(globals, item) {
    ItemsModule._setupWeaponItem(globals, item, "progs/g_nail.mdl", Items.NAILGUN, "$qc_nailgun")
  }

  static weaponSupernailgun(globals, item) {
    ItemsModule._setupWeaponItem(globals, item, "progs/g_nail2.mdl", Items.SUPER_NAILGUN, "$qc_super_nailgun")
  }

  static weaponGrenadeLauncher(globals, item) {
    ItemsModule._setupWeaponItem(globals, item, "progs/g_rock.mdl", Items.GRENADE_LAUNCHER, "$qc_grenade_launcher")
  }

  static weaponRocketLauncher(globals, item) {
    ItemsModule._setupWeaponItem(globals, item, "progs/g_rock2.mdl", Items.ROCKET_LAUNCHER, "$qc_rocket_launcher")
  }

  static weaponLightning(globals, item) {
    ItemsModule._setupWeaponItem(globals, item, "progs/g_light.mdl", Items.LIGHTNING, "$qc_thunderbolt")
  }

  // ------------------------------------------------------------------------
  // Ammo -------------------------------------------------------------------

  static ammoTouch(globals, item, other) {
    if (item == null || other == null) return
    if (other.get("classname", "") != "player") return
    if (other.get("health", 0) <= 0) return

    var best = WeaponsModule.W_BestWeapon(globals, other)
    var ammoType = item.get("weapon", 0)
    var amount = item.get("aflag", 0)

    if (ammoType == 1) {
      if (other.get("ammo_shells", 0) >= 100) return
      ItemsModule._giveWeaponAmmo(other, "ammo_shells", amount)
    } else if (ammoType == 2) {
      if (other.get("ammo_nails", 0) >= 200) return
      ItemsModule._giveWeaponAmmo(other, "ammo_nails", amount)
    } else if (ammoType == 3) {
      if (other.get("ammo_rockets", 0) >= 100) return
      ItemsModule._giveWeaponAmmo(other, "ammo_rockets", amount)
    } else if (ammoType == 4) {
      if (other.get("ammo_cells", 0) >= 100) return
      ItemsModule._giveWeaponAmmo(other, "ammo_cells", amount)
    }

    ItemsModule.boundOtherAmmo(globals, other)

    ItemsModule._playerPrintPickup(other, "$qc_got_item", item.get("netname", ""))
    ItemsModule._playPickupSound(other, "weapons/lock4.wav")
    ItemsModule._stuffBf(other)

    if (other.get("weapon", Items.AXE) == best && WeaponsModule.W_WantsToChangeWeapon(globals, other, 0, 1)) {
      other.set("weapon", WeaponsModule.W_BestWeapon(globals, other))
      WeaponsModule.W_SetCurrentAmmo(globals, other)
    } else {
      WeaponsModule.W_SetCurrentAmmo(globals, other)
    }

    ItemsModule._clearForPickup(item)

    var deathmatch = globals.deathmatch
    if (deathmatch > 0) {
      if (deathmatch == 3 || deathmatch == 5) {
        ItemsModule._prepareRespawn(globals, item, 15.0)
      } else if (deathmatch != 2) {
        ItemsModule._prepareRespawn(globals, item, 30.0)
      } else {
        ItemsModule._prepareRespawn(globals, item, 0)
      }
    } else {
      ItemsModule._prepareRespawn(globals, item, 0)
    }

    SubsModule.useTargets(globals, item, other)
  }

  static _setupAmmo(globals, item, spawnflag, bigModel, smallModel, bigAmount, smallAmount, weaponType, netname) {
    if (item == null) return
    item.set("touch", "ItemsModule.ammoTouch")
    if (Engine.bitAnd(item.get("spawnflags", 0), spawnflag) != 0) {
      Engine.precacheModel(bigModel)
      Engine.setModel(item, bigModel)
      item.set("aflag", bigAmount)
    } else {
      Engine.precacheModel(smallModel)
      Engine.setModel(item, smallModel)
      item.set("aflag", smallAmount)
    }

    item.set("weapon", weaponType)
    item.set("netname", netname)
    Engine.setSize(item, [0, 0, 0], [32, 32, 56])
    ItemsModule.startItem(globals, item)
  }

  static itemShells(globals, item) {
    ItemsModule._setupAmmo(globals, item, 1, "maps/b_shell1.bsp", "maps/b_shell0.bsp", 40, 20, 1, "$qc_shells")
  }

  static itemSpikes(globals, item) {
    ItemsModule._setupAmmo(globals, item, 1, "maps/b_nail1.bsp", "maps/b_nail0.bsp", 50, 25, 2, "$qc_nails")
  }

  static itemRockets(globals, item) {
    ItemsModule._setupAmmo(globals, item, 1, "maps/b_rock1.bsp", "maps/b_rock0.bsp", 10, 5, 3, "$qc_rockets")
  }

  static itemCells(globals, item) {
    ItemsModule._setupAmmo(globals, item, 1, "maps/b_batt1.bsp", "maps/b_batt0.bsp", 12, 6, 4, "$qc_cells")
  }

  static itemWeapon(globals, item) {
    if (item == null) return
    item.set("touch", "ItemsModule.ammoTouch")
    var spawnflags = item.get("spawnflags", 0)

    if (Engine.bitAnd(spawnflags, 1) != 0) {
      if (Engine.bitAnd(spawnflags, 8) != 0) {
        Engine.precacheModel("maps/b_shell1.bsp")
        Engine.setModel(item, "maps/b_shell1.bsp")
        item.set("aflag", 40)
      } else {
        Engine.precacheModel("maps/b_shell0.bsp")
        Engine.setModel(item, "maps/b_shell0.bsp")
        item.set("aflag", 20)
      }
      item.set("weapon", 1)
      item.set("netname", "$qc_shells")
    }

    if (Engine.bitAnd(spawnflags, 4) != 0) {
      if (Engine.bitAnd(spawnflags, 8) != 0) {
        Engine.precacheModel("maps/b_nail1.bsp")
        Engine.setModel(item, "maps/b_nail1.bsp")
        item.set("aflag", 40)
      } else {
        Engine.precacheModel("maps/b_nail0.bsp")
        Engine.setModel(item, "maps/b_nail0.bsp")
        item.set("aflag", 20)
      }
      item.set("weapon", 2)
      item.set("netname", "$qc_spikes")
    }

    if (Engine.bitAnd(spawnflags, 2) != 0) {
      if (Engine.bitAnd(spawnflags, 8) != 0) {
        Engine.precacheModel("maps/b_rock1.bsp")
        Engine.setModel(item, "maps/b_rock1.bsp")
        item.set("aflag", 10)
      } else {
        Engine.precacheModel("maps/b_rock0.bsp")
        Engine.setModel(item, "maps/b_rock0.bsp")
        item.set("aflag", 5)
      }
      item.set("weapon", 3)
      item.set("netname", "$qc_rockets")
    }

    Engine.setSize(item, [0, 0, 0], [32, 32, 56])
    ItemsModule.startItem(globals, item)
  }

  // ------------------------------------------------------------------------
  // Keys -------------------------------------------------------------------

  static keySetSounds(globals, item) {
    if (item == null) return
    var worldType = globals.world == null ? WorldTypes.MEDIEVAL : globals.world.get("worldtype", WorldTypes.MEDIEVAL)
    if (worldType == WorldTypes.MEDIEVAL) {
      Engine.precacheSound("misc/medkey.wav")
      item.set("noise", "misc/medkey.wav")
    } else if (worldType == WorldTypes.METAL) {
      Engine.precacheSound("misc/runekey.wav")
      item.set("noise", "misc/runekey.wav")
    } else if (worldType == WorldTypes.BASE) {
      Engine.precacheSound2("misc/basekey.wav")
      item.set("noise", "misc/basekey.wav")
    }
  }

  static keyTouch(globals, item, other) {
    if (item == null || other == null) return
    if (other.get("classname", "") != "player") return
    if (other.get("health", 0) <= 0) return

    var itemsBits = other.get("items", 0)
    if (Engine.bitAnd(itemsBits, item.get("items", 0)) != 0) return

    ItemsModule._playerPrintPickup(other, "$qc_got_item", item.get("netname", ""))
    ItemsModule._playPickupSound(other, item.get("noise", ""))
    ItemsModule._stuffBf(other)

    other.set("items", Engine.bitOr(itemsBits, item.get("items", 0)))

    if (globals.coop == 0) {
      ItemsModule._clearForPickup(item)
    }

    SubsModule.useTargets(globals, item, other)
  }

  static itemKey1(globals, item) {
    if (item == null) return
    var worldType = globals.world == null ? WorldTypes.MEDIEVAL : globals.world.get("worldtype", WorldTypes.MEDIEVAL)
    if (worldType == WorldTypes.MEDIEVAL) {
      Engine.precacheModel("progs/w_s_key.mdl")
      Engine.setModel(item, "progs/w_s_key.mdl")
      item.set("netname", "$qc_silver_key")
    } else if (worldType == WorldTypes.METAL) {
      Engine.precacheModel("progs/m_s_key.mdl")
      Engine.setModel(item, "progs/m_s_key.mdl")
      item.set("netname", "$qc_silver_runekey")
    } else if (worldType == WorldTypes.BASE) {
      Engine.precacheModel2("progs/b_s_key.mdl")
      Engine.setModel(item, "progs/b_s_key.mdl")
      item.set("netname", "$qc_silver_keycard")
    }

    ItemsModule.keySetSounds(globals, item)
    item.set("touch", "ItemsModule.keyTouch")
    item.set("items", Items.KEY1)
    Engine.setSize(item, [-16, -16, -24], [16, 16, 32])
    ItemsModule.startItem(globals, item)
  }

  static itemKey2(globals, item) {
    if (item == null) return
    var worldType = globals.world == null ? WorldTypes.MEDIEVAL : globals.world.get("worldtype", WorldTypes.MEDIEVAL)
    if (worldType == WorldTypes.MEDIEVAL) {
      Engine.precacheModel("progs/w_g_key.mdl")
      Engine.setModel(item, "progs/w_g_key.mdl")
      item.set("netname", "$qc_gold_key")
    } else if (worldType == WorldTypes.METAL) {
      Engine.precacheModel("progs/m_g_key.mdl")
      Engine.setModel(item, "progs/m_g_key.mdl")
      item.set("netname", "$qc_gold_runekey")
    } else if (worldType == WorldTypes.BASE) {
      Engine.precacheModel2("progs/b_g_key.mdl")
      Engine.setModel(item, "progs/b_g_key.mdl")
      item.set("netname", "$qc_gold_keycard")
    }

    ItemsModule.keySetSounds(globals, item)
    item.set("touch", "ItemsModule.keyTouch")
    item.set("items", Items.KEY2)
    Engine.setSize(item, [-16, -16, -24], [16, 16, 32])
    ItemsModule.startItem(globals, item)
  }

  // ------------------------------------------------------------------------
  // Runes ------------------------------------------------------------------

  static sigilTouch(globals, item, other) {
    if (item == null || other == null) return
    if (other.get("classname", "") != "player") return
    if (other.get("health", 0) <= 0) return

    Engine.centerPrint(other, "$qc_got_rune")
    ItemsModule._playPickupSound(other, item.get("noise", ""))
    ItemsModule._stuffBf(other)

    ItemsModule._clearForPickup(item)
    item.set("classname", "")

    var spawnflags = Engine.bitAnd(item.get("spawnflags", 0).floor, 15)
    globals.serverFlags = Engine.bitOr(globals.serverFlags.floor, spawnflags)

    SubsModule.useTargets(globals, item, other)
  }

  static itemSigil(globals, item) {
    if (item == null) return
    if (item.get("spawnflags", 0) == 0) {
      Engine.objError("no spawnflags")
      return
    }

    Engine.precacheSound("misc/runekey.wav")
    item.set("noise", "misc/runekey.wav")

    if (Engine.bitAnd(item.get("spawnflags", 0), 1) != 0) {
      Engine.precacheModel("progs/end1.mdl")
      Engine.setModel(item, "progs/end1.mdl")
    }
    if (Engine.bitAnd(item.get("spawnflags", 0), 2) != 0) {
      Engine.precacheModel2("progs/end2.mdl")
      Engine.setModel(item, "progs/end2.mdl")
    }
    if (Engine.bitAnd(item.get("spawnflags", 0), 4) != 0) {
      Engine.precacheModel2("progs/end3.mdl")
      Engine.setModel(item, "progs/end3.mdl")
    }
    if (Engine.bitAnd(item.get("spawnflags", 0), 8) != 0) {
      Engine.precacheModel2("progs/end4.mdl")
      Engine.setModel(item, "progs/end4.mdl")
    }

    item.set("touch", "ItemsModule.sigilTouch")
    Engine.setSize(item, [-16, -16, -24], [16, 16, 32])
    ItemsModule.startItem(globals, item)
  }

  // ------------------------------------------------------------------------
  // Powerups ---------------------------------------------------------------

  static powerupTouch(globals, item, other) {
    if (item == null || other == null) return
    if (other.get("classname", "") != "player") return
    if (other.get("health", 0) <= 0) return

    ItemsModule._playerPrintPickup(other, "$qc_got_item", item.get("netname", ""))

    if (globals.deathmatch > 0) {
      item.set("mdl", item.get("model", ""))
      var delay = 60.0
      var className = item.get("classname", "")
      if (className == "item_artifact_invulnerability" || className == "item_artifact_invisibility") {
        delay = 300.0
      }
      ItemsModule._prepareRespawn(globals, item, delay)
    }

    Engine.playSound(other, Channels.VOICE, item.get("noise", ""), 1, Attenuations.NORMAL)
    ItemsModule._stuffBf(other)

    ItemsModule._clearForPickup(item)
    ItemsModule._addFlag(other, "items", item.get("items", 0))

    var className = item.get("classname", "")
    if (className == "item_artifact_envirosuit") {
      other.set("rad_time", 1)
      other.set("radsuit_finished", globals.time + 30)
    } else if (className == "item_artifact_invulnerability") {
      other.set("invincible_time", 1)
      other.set("invincible_finished", globals.time + 30)
    } else if (className == "item_artifact_invisibility") {
      other.set("invisible_time", 1)
      other.set("invisible_finished", globals.time + 30)
    } else if (className == "item_artifact_super_damage") {
      other.set("super_time", 1)
      other.set("super_damage_finished", globals.time + 30)
    }

    SubsModule.useTargets(globals, item, other)
  }

  static itemArtifactInvulnerability(globals, item) {
    if (item == null) return
    item.set("touch", "ItemsModule.powerupTouch")
    Engine.precacheModel("progs/invulner.mdl")
    Engine.precacheSound("items/protect.wav")
    Engine.precacheSound("items/protect2.wav")
    Engine.precacheSound("items/protect3.wav")
    item.set("noise", "items/protect.wav")
    Engine.setModel(item, "progs/invulner.mdl")
    item.set("netname", "$qc_pentagram_of_protection")
    item.set("items", Items.INVULNERABILITY)
    Engine.setSize(item, [-16, -16, -24], [16, 16, 32])
    ItemsModule.startItem(globals, item)
  }

  static itemArtifactEnvirosuit(globals, item) {
    if (item == null) return
    item.set("touch", "ItemsModule.powerupTouch")
    Engine.precacheModel("progs/suit.mdl")
    Engine.precacheSound("items/suit.wav")
    Engine.precacheSound("items/suit2.wav")
    item.set("noise", "items/suit.wav")
    Engine.setModel(item, "progs/suit.mdl")
    item.set("netname", "$qc_biosuit")
    item.set("items", Items.SUIT)
    Engine.setSize(item, [-16, -16, -24], [16, 16, 32])
    ItemsModule.startItem(globals, item)
  }

  static itemArtifactInvisibility(globals, item) {
    if (item == null) return
    item.set("touch", "ItemsModule.powerupTouch")
    Engine.precacheModel("progs/invisibl.mdl")
    Engine.precacheSound("items/inv1.wav")
    Engine.precacheSound("items/inv2.wav")
    Engine.precacheSound("items/inv3.wav")
    item.set("noise", "items/inv1.wav")
    Engine.setModel(item, "progs/invisibl.mdl")
    item.set("netname", "$qc_ring_of_shadows")
    item.set("items", Items.INVISIBILITY)
    Engine.setSize(item, [-16, -16, -24], [16, 16, 32])
    ItemsModule.startItem(globals, item)
  }

  static itemArtifactSuperDamage(globals, item) {
    if (item == null) return
    item.set("touch", "ItemsModule.powerupTouch")
    Engine.precacheModel("progs/quaddama.mdl")
    Engine.precacheSound("items/damage.wav")
    Engine.precacheSound("items/damage2.wav")
    Engine.precacheSound("items/damage3.wav")
    item.set("noise", "items/damage.wav")
    Engine.setModel(item, "progs/quaddama.mdl")
    item.set("netname", "$qc_quad_damage")
    item.set("items", Items.QUAD)
    Engine.setSize(item, [-16, -16, -24], [16, 16, 32])
    ItemsModule.startItem(globals, item)
  }

  // ------------------------------------------------------------------------
  // Backpacks --------------------------------------------------------------

  static backpackTouch(globals, item, other) {
    if (item == null || other == null) return
    if (other.get("classname", "") != "player") return
    if (other.get("health", 0) <= 0) return

    ItemsModule._playerPrintPickup(other, "$qc_backpack_got", null)

    var itemBits = item.get("items", 0)
    if (itemBits != 0 && Engine.bitAnd(other.get("items", 0), itemBits) == 0) {
      ItemsModule._playerPrintPickup(other, item.get("netname", ""), null)
    }

    other.set("ammo_shells", other.get("ammo_shells", 0) + item.get("ammo_shells", 0))
    other.set("ammo_nails", other.get("ammo_nails", 0) + item.get("ammo_nails", 0))
    other.set("ammo_rockets", other.get("ammo_rockets", 0) + item.get("ammo_rockets", 0))
    other.set("ammo_cells", other.get("ammo_cells", 0) + item.get("ammo_cells", 0))

    var newWeapon = itemBits
    if (newWeapon == 0) {
      newWeapon = other.get("weapon", Items.AXE)
    }

    var oldItems = other.get("items", 0)
    other.set("items", Engine.bitOr(oldItems, itemBits))

    ItemsModule.boundOtherAmmo(globals, other)

    if (item.get("ammo_shells", 0) != 0) {
      ItemsModule._playerPrintPickup(other, "$qc_backpack_shells", item.get("ammo_shells", 0).toString)
    }
    if (item.get("ammo_nails", 0) != 0) {
      ItemsModule._playerPrintPickup(other, "$qc_backpack_nails", item.get("ammo_nails", 0).toString)
    }
    if (item.get("ammo_rockets", 0) != 0) {
      ItemsModule._playerPrintPickup(other, "$qc_backpack_rockets", item.get("ammo_rockets", 0).toString)
    }
    if (item.get("ammo_cells", 0) != 0) {
      ItemsModule._playerPrintPickup(other, "$qc_backpack_cells", item.get("ammo_cells", 0).toString)
    }

    ItemsModule._playPickupSound(other, "weapons/lock4.wav")
    ItemsModule._stuffBf(other)

    Engine.removeEntity(item)

    if (WeaponsModule.W_WantsToChangeWeapon(globals, other, oldItems, other.get("items", 0))) {
      if (ItemsModule._hasFlag(other, "flags", PlayerFlags.INWATER)) {
        if (newWeapon != Items.LIGHTNING) {
          ItemsModule.deathmatchWeapon(globals, other, oldItems, newWeapon)
        }
      } else {
        ItemsModule.deathmatchWeapon(globals, other, oldItems, newWeapon)
      }
    }

    WeaponsModule.W_SetCurrentAmmo(globals, other)
  }

  static dropBackpack(globals, player) {
    if (player == null) return
    var totalAmmo = player.get("ammo_shells", 0) + player.get("ammo_nails", 0) + player.get("ammo_rockets", 0) + player.get("ammo_cells", 0)
    if (totalAmmo == 0) return

    var backpack = Engine.spawnEntity()
    var origin = player.get("origin", [0, 0, 0])
    var dropOrigin = ItemsModule._vectorSub(origin, [0, 0, 24])
    backpack.set("origin", dropOrigin)
    Engine.setOrigin(backpack, dropOrigin)

    backpack.set("items", player.get("weapon", Items.AXE))
    backpack.set("classname", "item_backpack")

    var weapon = backpack.get("items", 0)
    if (weapon == Items.AXE) backpack.set("netname", "$qc_axe")
    else if (weapon == Items.SHOTGUN) backpack.set("netname", "$qc_shotgun")
    else if (weapon == Items.SUPER_SHOTGUN) backpack.set("netname", "$qc_double_shotgun")
    else if (weapon == Items.NAILGUN) backpack.set("netname", "$qc_nailgun")
    else if (weapon == Items.SUPER_NAILGUN) backpack.set("netname", "$qc_super_nailgun")
    else if (weapon == Items.GRENADE_LAUNCHER) backpack.set("netname", "$qc_grenade_launcher")
    else if (weapon == Items.ROCKET_LAUNCHER) backpack.set("netname", "$qc_rocket_launcher")
    else if (weapon == Items.LIGHTNING) backpack.set("netname", "$qc_thunderbolt")

    backpack.set("ammo_shells", player.get("ammo_shells", 0))
    backpack.set("ammo_nails", player.get("ammo_nails", 0))
    backpack.set("ammo_rockets", player.get("ammo_rockets", 0))
    backpack.set("ammo_cells", player.get("ammo_cells", 0))

    if (backpack.get("ammo_shells", 0) < 5 && (weapon == Items.SHOTGUN || weapon == Items.SUPER_SHOTGUN)) {
      backpack.set("ammo_shells", 5)
    }
    if (backpack.get("ammo_nails", 0) < 20 && (weapon == Items.NAILGUN || weapon == Items.SUPER_NAILGUN)) {
      backpack.set("ammo_nails", 20)
    }
    if (backpack.get("ammo_rockets", 0) < 5 && (weapon == Items.GRENADE_LAUNCHER || weapon == Items.ROCKET_LAUNCHER)) {
      backpack.set("ammo_rockets", 5)
    }
    if (backpack.get("ammo_cells", 0) < 15 && weapon == Items.LIGHTNING) {
      backpack.set("ammo_cells", 15)
    }

    var velocity = [-100 + Engine.random() * 200, -100 + Engine.random() * 200, 300]
    backpack.set("velocity", velocity)

    backpack.set("flags", PlayerFlags.ITEM)
    backpack.set("solid", SolidTypes.TRIGGER)
    backpack.set("movetype", MoveTypes.TOSS)
    Engine.precacheModel("progs/backpack.mdl")
    Engine.setModel(backpack, "progs/backpack.mdl")
    Engine.setSize(backpack, [-16, -16, 0], [16, 16, 56])
    backpack.set("touch", "ItemsModule.BackpackTouch")

    var delay = 120.0
    backpack.set("think", "SubsModule.SUB_Remove")
    backpack.set("nextthink", globals.time + delay)
    Engine.scheduleThink(backpack, "SubsModule.SUB_Remove", delay)
  }

  // ------------------------------------------------------------------------
  // Compatibility wrappers -------------------------------------------------

  static SUB_regen(globals, item) { ItemsModule.subRegen(globals, item) }
  static PlaceItem(globals, item) { ItemsModule.placeItem(globals, item) }
  static StartItem(globals, item) { ItemsModule.startItem(globals, item) }
  static T_Heal(globals, target, healAmount, ignoreMax) {
    return ItemsModule.tHeal(globals, target, healAmount, ignoreMax)
  }
  static item_health(globals, item) { ItemsModule.itemHealth(globals, item) }
  static health_touch(globals, item, other) { ItemsModule.healthTouch(globals, item, other) }
  static armor_touch(globals, item, other) { ItemsModule.armorTouch(globals, item, other) }
  static item_armor1(globals, item) { ItemsModule.itemArmor1(globals, item) }
  static item_armor2(globals, item) { ItemsModule.itemArmor2(globals, item) }
  static item_armorInv(globals, item) { ItemsModule.itemArmorInv(globals, item) }
  static bound_other_ammo(globals, player) { ItemsModule.boundOtherAmmo(globals, player) }
  static RankForWeapon(globals, weapon) { return ItemsModule.rankForWeapon(globals, weapon) }
  static WeaponCode(globals, weapon) { return ItemsModule.weaponCode(globals, weapon) }
  static Deathmatch_Weapon(globals, player, oldWeapon, newWeapon) {
    ItemsModule.deathmatchWeapon(globals, player, oldWeapon, newWeapon)
  }
  static weapon_touch(globals, item, other) { ItemsModule.weaponTouch(globals, item, other) }
  static weapon_supershotgun(globals, item) { ItemsModule.weaponSupershotgun(globals, item) }
  static weapon_nailgun(globals, item) { ItemsModule.weaponNailgun(globals, item) }
  static weapon_supernailgun(globals, item) { ItemsModule.weaponSupernailgun(globals, item) }
  static weapon_grenadelauncher(globals, item) { ItemsModule.weaponGrenadeLauncher(globals, item) }
  static weapon_rocketlauncher(globals, item) { ItemsModule.weaponRocketLauncher(globals, item) }
  static weapon_lightning(globals, item) { ItemsModule.weaponLightning(globals, item) }
  static ammo_touch(globals, item, other) { ItemsModule.ammoTouch(globals, item, other) }
  static item_shells(globals, item) { ItemsModule.itemShells(globals, item) }
  static item_spikes(globals, item) { ItemsModule.itemSpikes(globals, item) }
  static item_rockets(globals, item) { ItemsModule.itemRockets(globals, item) }
  static item_cells(globals, item) { ItemsModule.itemCells(globals, item) }
  static item_weapon(globals, item) { ItemsModule.itemWeapon(globals, item) }
  static key_touch(globals, item, other) { ItemsModule.keyTouch(globals, item, other) }
  static key_setsounds(globals, item) { ItemsModule.keySetSounds(globals, item) }
  static item_key1(globals, item) { ItemsModule.itemKey1(globals, item) }
  static item_key2(globals, item) { ItemsModule.itemKey2(globals, item) }
  static sigil_touch(globals, item, other) { ItemsModule.sigilTouch(globals, item, other) }
  static item_sigil(globals, item) { ItemsModule.itemSigil(globals, item) }
  static powerup_touch(globals, item, other) { ItemsModule.powerupTouch(globals, item, other) }
  static item_artifact_invulnerability(globals, item) { ItemsModule.itemArtifactInvulnerability(globals, item) }
  static item_artifact_envirosuit(globals, item) { ItemsModule.itemArtifactEnvirosuit(globals, item) }
  static item_artifact_invisibility(globals, item) { ItemsModule.itemArtifactInvisibility(globals, item) }
  static item_artifact_super_damage(globals, item) { ItemsModule.itemArtifactSuperDamage(globals, item) }
  static BackpackTouch(globals, item, other) { ItemsModule.backpackTouch(globals, item, other) }
  static DropBackpack(globals, player) { ItemsModule.dropBackpack(globals, player) }
}
