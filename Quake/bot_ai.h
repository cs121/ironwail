#ifndef BOT_AI_H
#define BOT_AI_H

#include "quakedef.h"
#include "bot_main.h"

void BotAI_OnMapSpawn (void);
void BotAI_ResetState (bot_state_t *bot);
void BotAI_BuildCommand (bot_state_t *bot, client_t *client, usercmd_t *outcmd, vec3_t out_vangle, qboolean *out_attack, int *out_impulse);
const char *BotAI_StateName (bot_ai_state_t state);

#endif /* BOT_AI_H */
