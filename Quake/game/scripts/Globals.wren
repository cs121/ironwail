// Globals.wren
// Contains gameplay constants and the mutable global state structure that the
// ported QuakeC code expects to interact with.

import "./Entity" for GameEntity

class Items {
  static SHOTGUN { 1 }
  static SUPER_SHOTGUN { 2 }
  static NAILGUN { 4 }
  static SUPER_NAILGUN { 8 }
  static GRENADE_LAUNCHER { 16 }
  static ROCKET_LAUNCHER { 32 }
  static LIGHTNING { 64 }
  static EXTRA_WEAPON { 128 }
  static SHELLS { 256 }
  static NAILS { 512 }
  static ROCKETS { 1024 }
  static CELLS { 2048 }
  static AXE { 4096 }
  static ARMOR1 { 8192 }
  static ARMOR2 { 16384 }
  static ARMOR3 { 32768 }
  static SUPERHEALTH { 65536 }
  static KEY1 { 131072 }
  static KEY2 { 262144 }
  static INVISIBILITY { 524288 }
  static INVULNERABILITY { 1048576 }
  static SUIT { 2097152 }
  static QUAD { 4194304 }
}

class WorldTypes {
  static MEDIEVAL { 0 }
  static METAL { 1 }
  static BASE { 2 }
}

class Channels {
  static AUTO { 0 }
  static WEAPON { 1 }
  static VOICE { 2 }
  static ITEM { 3 }
  static BODY { 4 }
}

class Attenuations {
  static NONE { 0 }
  static NORMAL { 1 }
  static IDLE { 2 }
  static STATIC { 3 }
}

class TempEntityCodes {
  static SPIKE { 0 }
  static SUPERSPIKE { 1 }
  static GUNSHOT { 2 }
  static EXPLOSION { 3 }
  static TAREXPLOSION { 4 }
  static LIGHTNING1 { 5 }
  static LIGHTNING2 { 6 }
  static WIZSPIKE { 7 }
  static KNIGHTSPIKE { 8 }
  static LIGHTNING3 { 9 }
  static LAVASPLASH { 10 }
  static TELEPORT { 11 }
  static EXPLOSION2 { 12 }
  static BEAM { 13 }
}

class MessageTypes {
  static BROADCAST { 0 }
  static ONE { 1 }
  static ALL { 2 }
  static INIT { 3 }
}

class ServiceCodes {
  static INTERMISSION { 30 }
  static FINALE { 31 }
  static CDTRACK { 32 }
  static SELL_SCREEN { 33 }
  static ACHIEVEMENT { 52 }
  static KILLEDMONSTER { 27 }
}

class DamageValues {
  static NO { 0 }
  static YES { 1 }
  static AIM { 2 }
}

class SolidTypes {
  static NOT { 0 }
  static TRIGGER { 1 }
  static BBOX { 2 }
  static SLIDEBOX { 3 }
  static BSP { 4 }
  static CORPSE { 5 }
}

class MoveTypes {
  static NONE { 0 }
  static WALK { 3 }
  static STEP { 4 }
  static FLY { 5 }
  static TOSS { 6 }
  static PUSH { 7 }
  static NOCLIP { 8 }
  static FLYMISSILE { 9 }
  static BOUNCE { 10 }
  static GIB { 11 }
}

class Ranges {
  static MELEE { 0 }
  static NEAR { 1 }
  static MID { 2 }
  static FAR { 3 }
}

class MoverStates {
  static TOP { 0 }
  static BOTTOM { 1 }
  static UP { 2 }
  static DOWN { 3 }
}

class DeadFlags {
  static NO { 0 }
  static DYING { 1 }
  static DEAD { 2 }
  static RESPAWNABLE { 3 }
}

class AttackStates {
  static STRAIGHT { 1 }
  static SLIDING { 2 }
  static MELEE { 3 }
  static MISSILE { 4 }
}

class CombatStyles {
  static NONE { 0 }
  static RANGED { 1 }
  static MELEE { 2 }
  static MIXED { 3 }
}

class PlayerFlags {
  static FLY { 1 }
  static SWIM { 2 }
  static CLIENT { 8 }
  static INWATER { 16 }
  static MONSTER { 32 }
  static GODMODE { 64 }
  static NOTARGET { 128 }
  static ITEM { 256 }
  static ONGROUND { 512 }
  static PARTIALGROUND { 1024 }
  static WATERJUMP { 2048 }
  static JUMPRELEASED { 4096 }
  static ISBOT { 8192 }
}

class DoorSpawnFlags {
  static START_OPEN { 1 }
  static DONT_LINK { 4 }
  static GOLD_KEY { 8 }
  static SILVER_KEY { 16 }
  static TOGGLE { 32 }
}

class SecretDoorFlags {
  static OPEN_ONCE { 1 }
  static FIRST_LEFT { 2 }
  static FIRST_DOWN { 4 }
  static NO_SHOOT { 8 }
  static ALWAYS_SHOOT { 16 }
}

class PlayerExtraFlags {
  static CHANGE_ONLY_NEW { 1 }
  static CHANGE_NEVER { 2 }
}

class Effects {
  static MUZZLEFLASH { 2 }
  static BRIGHTLIGHT { 4 }
  static DIMLIGHT { 8 }
  static QUADLIGHT { 16 }
  static PENTALIGHT { 32 }
  static CANDLELIGHT { 64 }
}

class Teams {
  static NONE { -1 }
  static MONSTERS { 0 }
  static HUMANS { 1 }
}

class PathResults {
  static ERROR { 0 }
  static REACHED_GOAL { 1 }
  static REACHED_PATH_END { 2 }
  static MOVE_BLOCKED { 3 }
  static IN_PROGRESS { 4 }
}

class WorldTypes {
  static MEDIEVAL { 0 }
  static METAL { 1 }
  static BASE { 2 }
}

class HullVectors {
  static PLAYER_MIN { [-16, -16, -24] }
  static PLAYER_MAX { [16, 16, 32] }
}

class Contents {
  static WATER { -3 }
  static SLIME { -4 }
  static LAVA { -5 }
  static SKY { -6 }
}

class GameGlobals {
  construct new() {
    self = null
    other = null
    activator = null
    world = GameEntity.new()
    time = 0.0
    frameTime = 0.0
    mapName = ""
    deathmatch = 0.0
    coop = 0.0
    teamplay = 0.0
    serverFlags = 0.0
    totalSecrets = 0.0
    totalMonsters = 0.0
    foundSecrets = 0.0
    killedMonsters = 0.0
    vForward = [0, 0, 0]
    vRight = [0, 0, 0]
    vUp = [0, 0, 0]
    traceAllSolid = 0.0
    traceStartSolid = 0.0
    traceFraction = 1.0
    traceEndPos = [0, 0, 0]
    tracePlaneNormal = [0, 0, 0]
    tracePlaneDist = 0.0
    traceEnt = null
    traceInOpen = 0.0
    traceInWater = 0.0
    spawnParms = List.filled(16, 0.0)
    nextMap = null
    gameOver = false
    campaign = 0.0
    campaignValid = false
    frameCount = 0.0
    cheatsAllowed = 0.0
    skill = 0.0
    resetFlag = false
    msgEntity = null
    bodyQueueHead = null
    startingServerFlags = 0.0
    lastSpawn = null
    intermissionRunning = 0.0
    intermissionExitTime = 0.0
    modelIndexPlayer = 0
    modelIndexEyes = 0
    damageAttacker = null
    forceRetouch = 0.0
  }

  setSpawnParm(index, value) {
    spawnParms[index - 1] = value
  }

  spawnParm(index) {
    return spawnParms[index - 1]
  }
}
