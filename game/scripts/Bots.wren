// Bots.wren
// Provides empty hook implementations matching bots/bot.qc so hosts can
// override bot behavior from Wren when desired.

class BotsModule {
  static botPreThink(globals, bot) {
    // Intentionally left blank for modders to extend in Wren.
  }

  static Bot_PreThink(globals, bot) { BotsModule.botPreThink(globals, bot) }

  static botPostThink(globals, bot) {
    // Intentionally left blank for modders to extend in Wren.
  }

  static Bot_PostThink(globals, bot) { BotsModule.botPostThink(globals, bot) }
}
