// Simple multiplayer bot for Ironwail / Quake
//
// This script is designed to be appended to the stock id1 QuakeC sources. Compile
// a new progs.dat that includes this file, then run `botspawn` from the console
// (engine-side command) to drop a roaming bot into the current game.

.float bot_nextmove;
.entity enemy;

// Pick a random yaw to keep idle bots moving.
void() bot_pick_direction =
{
    self.ideal_yaw = random() * 360;
    self.bot_nextmove = time + 0.75;
};

entity() bot_find_enemy =
{
    local entity candidate;

    candidate = find(world, classname, "player");
    while (candidate)
    {
        if (candidate.health > 0 && candidate != self)
            return candidate;

        candidate = find(candidate, classname, "player");
    }

    return world;
};

void() bot_think =
{
    if (self.enemy && self.enemy.health <= 0)
        self.enemy = world;

    if (!self.enemy)
        self.enemy = bot_find_enemy();

    if (self.enemy && self.enemy != world)
        self.ideal_yaw = vectoyaw(self.enemy.origin - self.origin);
    else if (time > self.bot_nextmove)
        bot_pick_direction();

    ChangeYaw();
    walkmove(self.ideal_yaw, 200 * frametime);

    self.nextthink = time + 0.1;
    self.think = bot_think;
};

void() botspawn =
{
    local entity bot;

    bot = spawn();
    bot.classname = "bot";
    bot.movetype = MOVETYPE_STEP;
    bot.solid = SOLID_SLIDEBOX;
    bot.takedamage = DAMAGE_AIM;
    bot.health = 100;
    bot.flags = FL_CLIENT;

    setmodel(bot, "progs/player.mdl");
    setsize(bot, VEC_HULL_MIN, VEC_HULL_MAX);
    bot.view_ofs = '0 0 22';

    if (self)
        bot.origin = self.origin + '0 0 48';
    else
        bot.origin = world.origin + '0 0 48';

    bot.think = bot_think;
    bot.nextthink = time + 0.2;
};
