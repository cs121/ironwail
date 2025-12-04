# QuakeC additions

This folder contains a lightweight QuakeC script that introduces a simple multiplayer bot.

Add `Misc/qc/multiplayer_bot.qc` to your `progs.src` (ideally after the other gameplay scripts) and rebuild `progs.dat` with your preferred QuakeC compiler. The engine-side `botspawn` console command will invoke the `botspawn` function defined in that file, so it can be used from the server console or by connected players once their modded `progs.dat` is loaded.
