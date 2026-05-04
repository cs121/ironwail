#ifndef _QUAKE_DEVSTATS_H
#define _QUAKE_DEVSTATS_H

#include "q_stdinc.h"

struct cvar_s;
typedef struct cvar_s cvar_t;

typedef struct devstats_s
{
	int packetsize;
	int edicts;
	int visedicts;
	int efrags;
	int tempents;
	int beams;
	int dlights;
	int gpu_upload;
} devstats_t;

typedef struct overflowtimes_s
{
	double packetsize;
	double efrags;
	double beams;
	double varstring;
} overflowtimes_t;

#ifndef RENDERER_PLUGIN_BUILD
extern cvar_t devstats;
extern devstats_t dev_stats, dev_peakstats;
#endif

extern overflowtimes_t dev_overflows;

#define CONSOLE_RESPAM_TIME 3

#endif /* _QUAKE_DEVSTATS_H */
