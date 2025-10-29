// World.wren
// Port of the critical world management routines from world.qc.

import "./Engine" for Engine
import "./Globals" for GameGlobals
import "./Entity" for GameEntity
import "./Weapons" for WeaponsModule

var _CORE_FILES = [
  "progs.dat",
  "gfx.wad",
  "quake.rc",
  "default.cfg",
  "end1.bin",
  "demo1.dem",
  "demo2.dem",
  "demo3.dem",
  "gfx/palette.lmp",
  "gfx/colormap.lmp",
  "gfx/complete.lmp",
  "gfx/inter.lmp",
  "gfx/ranking.lmp",
  "gfx/vidmodes.lmp",
  "gfx/finale.lmp",
  "gfx/conback.lmp",
  "gfx/qplaque.lmp",
  "gfx/menudot1.lmp",
  "gfx/menudot2.lmp",
  "gfx/menudot3.lmp",
  "gfx/menudot4.lmp",
  "gfx/menudot5.lmp",
  "gfx/menudot6.lmp",
  "gfx/menuplyr.lmp",
  "gfx/bigbox.lmp",
  "gfx/dim_modm.lmp",
  "gfx/dim_drct.lmp",
  "gfx/dim_ipx.lmp",
  "gfx/dim_tcp.lmp",
  "gfx/dim_mult.lmp",
  "gfx/mainmenu.lmp",
  "gfx/box_tl.lmp",
  "gfx/box_tm.lmp",
  "gfx/box_tr.lmp",
  "gfx/box_ml.lmp",
  "gfx/box_mm.lmp",
  "gfx/box_mm2.lmp",
  "gfx/box_mr.lmp",
  "gfx/box_bl.lmp",
  "gfx/box_bm.lmp",
  "gfx/box_br.lmp",
  "gfx/sp_menu.lmp",
  "gfx/ttl_sgl.lmp",
  "gfx/ttl_main.lmp",
  "gfx/ttl_cstm.lmp",
  "gfx/mp_menu.lmp",
  "gfx/netmen1.lmp",
  "gfx/netmen2.lmp",
  "gfx/netmen3.lmp",
  "gfx/netmen4.lmp",
  "gfx/netmen5.lmp",
  "gfx/sell.lmp",
  "gfx/help0.lmp",
  "gfx/help1.lmp",
  "gfx/help2.lmp",
  "gfx/help3.lmp",
  "gfx/help4.lmp",
  "gfx/help5.lmp",
  "gfx/pause.lmp",
  "gfx/loading.lmp",
  "gfx/p_option.lmp",
  "gfx/p_load.lmp",
  "gfx/p_save.lmp",
  "gfx/p_multi.lmp",
  "maps/start.bsp",
  "maps/e1m1.bsp",
  "maps/e1m2.bsp",
  "maps/e1m3.bsp",
  "maps/e1m4.bsp",
  "maps/e1m5.bsp",
  "maps/e1m6.bsp",
  "maps/e1m7.bsp",
  "maps/e1m8.bsp"
]

var _SECONDARY_FILES = [
  "end2.bin",
  "gfx/pop.lmp",
  "maps/e2m1.bsp",
  "maps/e2m2.bsp",
  "maps/e2m3.bsp",
  "maps/e2m4.bsp",
  "maps/e2m5.bsp",
  "maps/e2m6.bsp",
  "maps/e2m7.bsp",
  "maps/e3m1.bsp",
  "maps/e3m2.bsp",
  "maps/e3m3.bsp",
  "maps/e3m4.bsp",
  "maps/e3m5.bsp",
  "maps/e3m6.bsp",
  "maps/e3m7.bsp",
  "maps/e4m1.bsp",
  "maps/e4m2.bsp",
  "maps/e4m3.bsp",
  "maps/e4m4.bsp",
  "maps/e4m5.bsp",
  "maps/e4m6.bsp",
  "maps/e4m7.bsp",
  "maps/e4m8.bsp",
  "maps/end.bsp",
  "maps/dm1.bsp",
  "maps/dm2.bsp",
  "maps/dm3.bsp",
  "maps/dm4.bsp",
  "maps/dm5.bsp",
  "maps/dm6.bsp"
]

var _CORE_SOUNDS = [
  "misc/menu1.wav",
  "misc/menu2.wav",
  "misc/menu3.wav",
  "ambience/water1.wav",
  "ambience/wind2.wav"
]

var _WORLD_SOUNDS = [
  "demon/dland2.wav",
  "misc/h2ohit1.wav",
  "items/itembk2.wav",
  "player/plyrjmp8.wav",
  "player/land.wav",
  "player/land2.wav",
  "player/drown1.wav",
  "player/drown2.wav",
  "player/gasp1.wav",
  "player/gasp2.wav",
  "player/h2odeath.wav",
  "misc/talk.wav",
  "player/teledth1.wav",
  "misc/r_tele1.wav",
  "misc/r_tele2.wav",
  "misc/r_tele3.wav",
  "misc/r_tele4.wav",
  "misc/r_tele5.wav",
  "weapons/lock4.wav",
  "weapons/pkup.wav",
  "items/armor1.wav",
  "weapons/lhit.wav",
  "weapons/lstart.wav",
  "items/damage3.wav",
  "misc/power.wav",
  "player/gib.wav",
  "player/udeath.wav",
  "player/tornoff2.wav",
  "player/pain1.wav",
  "player/pain2.wav",
  "player/pain3.wav",
  "player/pain4.wav",
  "player/pain5.wav",
  "player/pain6.wav",
  "player/death1.wav",
  "player/death2.wav",
  "player/death3.wav",
  "player/death4.wav",
  "player/death5.wav",
  "weapons/ax1.wav",
  "player/axhit1.wav",
  "player/axhit2.wav",
  "player/h2ojump.wav",
  "player/slimbrn2.wav",
  "player/inh2o.wav",
  "player/inlava.wav",
  "misc/outwater.wav",
  "player/lburn1.wav",
  "player/lburn2.wav",
  "misc/water1.wav",
  "misc/water2.wav"
]

var _WORLD_MODELS = [
  "progs/player.mdl",
  "progs/eyes.mdl",
  "progs/h_player.mdl",
  "progs/gib1.mdl",
  "progs/gib2.mdl",
  "progs/gib3.mdl",
  "progs/s_bubble.spr",
  "progs/s_explod.spr",
  "progs/v_axe.mdl",
  "progs/v_shot.mdl",
  "progs/v_nail.mdl",
  "progs/v_rock.mdl",
  "progs/v_shot2.mdl",
  "progs/v_nail2.mdl",
  "progs/v_rock2.mdl",
  "progs/bolt.mdl",
  "progs/bolt2.mdl",
  "progs/bolt3.mdl",
  "progs/lavaball.mdl",
  "progs/missile.mdl",
  "progs/grenade.mdl",
  "progs/spike.mdl",
  "progs/s_spike.mdl",
  "progs/backpack.mdl",
  "progs/zom_gib.mdl",
  "progs/v_light.mdl"
]

var _LIGHT_STYLES = [
  [0, "m"],
  [1, "mmnmmommommnonmmonqnmmo"],
  [2, "abcdefghijklmnopqrstuvwxyzyxwvutsrqponmlkjihgfedcba"],
  [3, "mmmmmaaaaammmmmaaaaaabcdefgabcdefg"],
  [4, "mamamamamama"],
  [5, "jklmnopqrstuvwxyzyxwvutsrqponmlkj"],
  [6, "nmonqnmomnmomomno"],
  [7, "mmmaaaabcdefgmmmmaaaammmaamm"],
  [8, "mmmaaammmaaammmabcdefaaaammmmabcdefmmmaaaa"],
  [9, "aaaaaaaazzzzzzzz"],
  [10, "mmamammmmammamamaaamammma"],
  [11, "abcdefghijklmnopqrrqponmlkjihgfedcba"],
  [63, "a"]
]

class WorldModule {
  static main(globals) {
    Engine.log("main function")

    for (path in _CORE_FILES) {
      Engine.precacheFile(path)
    }

    for (path in _SECONDARY_FILES) {
      Engine.precacheFile2(path)
    }

    for (path in _CORE_SOUNDS) {
      Engine.precacheSound(path)
    }
  }

  static startFrame(globals) {
    globals.teamplay = Engine.cvar("teamplay")
    globals.skill = Engine.cvar("skill")
    globals.cheatsAllowed = Engine.cvar("sv_cheats")

    if (!globals.campaignValid) {
      globals.campaignValid = true
      globals.campaign = Engine.cvar("campaign")
    } else {
      Engine.cvarSet("campaign", globals.campaign.toString)
    }

    globals.frameCount = globals.frameCount + 1
  }

  static worldSpawn(globals) {
    globals.startingServerFlags = globals.serverFlags
    globals.lastSpawn = globals.world

    WorldModule.initBodyQueue(globals)

    var worldModel = globals.world.get("model", "")
    var gravity = (worldModel == "maps/e1m8.bsp") ? "100" : "800"
    Engine.cvarSet("sv_gravity", gravity)

    WeaponsModule.precache(globals)

    for (path in _WORLD_SOUNDS) {
      Engine.precacheSound(path)
    }

    for (path in _WORLD_MODELS) {
      Engine.precacheModel(path)
    }

    for (style in _LIGHT_STYLES) {
      Engine.lightstyle(style[0], style[1])
    }
  }

  static initBodyQueue(globals) {
    var head = Engine.spawnEntity()
    head.set("classname", "bodyqueue")

    var owner1 = Engine.spawnEntity()
    owner1.set("classname", "bodyqueue")
    head.set("owner", owner1)

    var owner2 = Engine.spawnEntity()
    owner2.set("classname", "bodyqueue")
    owner1.set("owner", owner2)

    var owner3 = Engine.spawnEntity()
    owner3.set("classname", "bodyqueue")
    owner2.set("owner", owner3)

    owner3.set("owner", head)

    globals.bodyQueueHead = head
  }

  static bodyQueue(globals, entity) {
    // Placeholder to mirror the no-op bodyqueue() QuakeC definition.
  }

  static copyToBodyQueue(globals, ent) {
    if (globals.bodyQueueHead == null) {
      WorldModule.initBodyQueue(globals)
    }

    var head = globals.bodyQueueHead
    head.set("angles", ent.get("angles", null))
    head.set("model", ent.get("model", null))
    head.set("modelindex", ent.get("modelindex", 0))
    head.set("frame", ent.get("frame", 0))
    head.set("colormap", ent.get("colormap", 0))
    head.set("movetype", ent.get("movetype", 0))
    head.set("velocity", ent.get("velocity", [0, 0, 0]))
    head.set("flags", 0)

    Engine.setOrigin(head, ent.get("origin", [0, 0, 0]))
    Engine.setSize(
      head,
      ent.get("mins", [0, 0, 0]),
      ent.get("maxs", [0, 0, 0])
    )

    globals.bodyQueueHead = head.get("owner", head)
  }

  static worldspawn(globals) { WorldModule.worldSpawn(globals) }
  static bodyqueue(globals, entity) { WorldModule.bodyQueue(globals, entity) }
  static InitBodyQueue(globals) { WorldModule.initBodyQueue(globals) }
  static CopyToBodyQueue(globals, entity) { WorldModule.copyToBodyQueue(globals, entity) }
}
