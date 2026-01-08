#ifndef SV_COALESCE_H
#define SV_COALESCE_H

#include "quakedef.h"

extern cvar_t sv_tickrate;
extern cvar_t sv_netrate;
extern cvar_t sv_netburst;
extern cvar_t sv_net_mtu;
extern cvar_t sv_pktmax;
extern cvar_t sv_netcoalesce;
extern cvar_t sv_net_debug;

void SV_Coalesce_RegisterCvars(void);
void SV_Coalesce_UpdateProtocolFlags(void);
void SV_Coalesce_InitClient(client_t *client);
void SV_Coalesce_Enqueue(client_t *client, const byte *data, int len);
void SV_Coalesce_FlushClient(client_t *client);
void SV_Coalesce_MaybeFlushClient(client_t *client, qboolean force);
void SV_Coalesce_DebugUpdate(void);

#endif
