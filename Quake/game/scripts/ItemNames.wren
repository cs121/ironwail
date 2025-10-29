// ItemNames.wren
// Provides localized item name lookup mirroring itemnames.qc.

import "./Globals" for Items, WorldTypes

class ItemNamesModule {
  static getNetName(globals, itemNumber) {
    switch (itemNumber) {
      case Items.AXE:
        return "Axe"
      case Items.SHOTGUN:
        return "Shotgun"
      case Items.SUPER_SHOTGUN:
        return "Super Shotgun"
      case Items.NAILGUN:
        return "Nailgun"
      case Items.SUPER_NAILGUN:
        return "Perforator"
      case Items.GRENADE_LAUNCHER:
        return "Grenade Launcher"
      case Items.ROCKET_LAUNCHER:
        return "Rocket Launcher"
      case Items.LIGHTNING:
        return "Lightning Gun"
      case Items.EXTRA_WEAPON:
        return "Extra Weapon"
      case Items.SHELLS:
        return "Shells"
      case Items.NAILS:
        return "Nails"
      case Items.ROCKETS:
        return "Rockets"
      case Items.CELLS:
        return "Cells"
      case Items.ARMOR1:
        return "Green Armor"
      case Items.ARMOR2:
        return "Yellow Armor"
      case Items.ARMOR3:
        return "Red Armor"
      case Items.SUPERHEALTH:
        return "Mega Health"
      case Items.KEY1:
        return ItemNamesModule._keyName(globals, true)
      case Items.KEY2:
        return ItemNamesModule._keyName(globals, false)
      case Items.INVISIBILITY:
        return "Ring of Shadows"
      case Items.INVULNERABILITY:
        return "Pentagram of Protection"
      case Items.SUIT:
        return "Biohazard Suit"
      case Items.QUAD:
        return "Quad Damage"
      default:
        return null
    }
  }

  static GetNetName(globals, itemNumber) {
    return ItemNamesModule.getNetName(globals, itemNumber)
  }

  static _keyName(globals, isSilver) {
    var world = globals.world
    var worldType = world == null ? WorldTypes.MEDIEVAL : world.get("worldtype", WorldTypes.MEDIEVAL)

    if (worldType == WorldTypes.MEDIEVAL) {
      return isSilver ? "Silver key" : "Gold key"
    } else if (worldType == WorldTypes.METAL) {
      return isSilver ? "Silver runkey" : "Gold runkey"
    } else if (worldType == WorldTypes.BASE) {
      return isSilver ? "Silver keycard" : "Gold keycard"
    }

    return null
  }
}
