# Multiplayer bot support

Ironwail ships with a lightweight server-side bot controller that can be used in any multiplayer session.

## Spawning bots

* Open the server console and set `sv_bot_spawn` to the number of bots you would like to add. For example, `sv_bot_spawn 3` queues three bot spawns.
* Bots will take the first free client slots, automatically spawn into the current map, and begin roaming/fighting other clients.

## Removing bots

* Set `sv_bot_remove` to the number of bots you want to kick, e.g. `sv_bot_remove 2`.
* Removal happens immediately, starting from the highest-numbered bot slots.

## Behaviour and limitations

* Bots are entirely server-side: they do not consume network slots and stay active across level changes until removed.
* Each bot is spawned from a profile in `botprofiles.cfg`, giving it a unique name, colours, skill setting and a pool of chat lines.
* When under-equipped they prioritise finding better weapons, then health, then armour; they only fight defensively with the axe or shotgun while searching for gear.
* Bots fire only when a line of sight trace to their target succeeds and will switch goals if they cannot make progress, preventing them from shooting walls or getting stuck on unreachable pickups.
* They wander and engage nearby targets with simple tactics, but they still do not execute map objectives or advanced teamwork.

## Customising bot profiles

Profiles live in `botprofiles.cfg` inside the game directory (for the stock build this file is shipped as `Quake/botprofiles.cfg`). Each profile block follows this structure:

```
bot
{
    name "Ranger"
    topcolor 4
    bottomcolor 12
    skin 0
    skill 1.3
    chat "Ready to frag."
}
```

Add or modify entries to introduce new personalities. During spawning the server cycles through available profiles; if it runs out, it falls back to the built-in defaults.

Use these controls to quickly populate a listen or dedicated server for testing multiplayer maps or practicing movement and combat.
