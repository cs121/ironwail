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
* They perform basic wandering, target the closest enemy (favouring human players), and retreat when wounded.
* Bots respect water movement and fire when in range, but they do not execute map objectives or advanced tactics.

Use these controls to quickly populate a listen or dedicated server for testing multiplayer maps or practicing movement and combat.
