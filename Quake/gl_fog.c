/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
Copyright (C) 2010-2014 QuakeSpasm developers

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/
// gl_fog.c -- global and volumetric fog (erweitert: linearer Fog mit Start/Ende + Fade)

#include "quakedef.h"

//==============================================================================
//
//  GLOBAL FOG
//
//==============================================================================

#define DEFAULT_DENSITY 0.0f
#define DEFAULT_GRAY    0.3f

static float fog_density;
static float fog_red;
static float fog_green;
static float fog_blue;

static float old_density;
static float old_red;
static float old_green;
static float old_blue;

// Linear-Range (neu)
static float fog_start = -1.0f; // <0 oder end<=start => linearer Fog aus
static float fog_end = -1.0f;
static float old_start;
static float old_end;

static float fade_time; // duration of fade
static float fade_done; // time when fade will be done

extern float skyfog;

// Optional: Falls der Renderer (r_framedata) noch keine fogparams besitzt,
// stellen wir ein globales Fallback bereit, das der Renderer abfragen kann.
// Konvention: [0]=start, [1]=end, [2]=1/(end-start) oder 0, [3]=mode (0=exp2, 1=linear)
float r_fogparams[4] = { -1.f, -1.f, 0.f, 0.f };

/*
=============
Fog_Update

update internal variables (inklusive linearer Range)
=============
*/
static void Fog_Update_Internal (float density, float red, float green, float blue,
	float time, float start, float end)
{
	// save previous settings for fade
	if (time > 0.0f)
	{
		// check for a fade in progress
		if (fade_done > cl.time && fade_time > 0.0f)
		{
			float f = (fade_done - cl.time) / fade_time;
			old_density = f * old_density + (1.0f - f) * fog_density;
			old_red = f * old_red + (1.0f - f) * fog_red;
			old_green = f * old_green + (1.0f - f) * fog_green;
			old_blue = f * old_blue + (1.0f - f) * fog_blue;
			old_start = f * old_start + (1.0f - f) * fog_start;
			old_end = f * old_end + (1.0f - f) * fog_end;
		}
		else
		{
			old_density = fog_density;
			old_red = fog_red;
			old_green = fog_green;
			old_blue = fog_blue;
			old_start = fog_start;
			old_end = fog_end;
		}
	}

	fog_density = density;
	fog_red = red;
	fog_green = green;
	fog_blue = blue;

	if (start < 0.0f || end <= start) {
		fog_start = -1.0f;
		fog_end = -1.0f;
	}
	else {
		fog_start = start;
		fog_end = end;
	}

	fade_time = time;
	fade_done = cl.time + time;
}

// Öffentliche API beibehalten (Kompatibilität)
void Fog_Update (float density, float red, float green, float blue, float time)
{
	Fog_Update_Internal (density, red, green, blue, time, fog_start, fog_end);
}

// Neue API: auch start/end direkt setzen
void Fog_UpdateEx (float density, float red, float green, float blue, float time,
	float start, float end)
{
	Fog_Update_Internal (density, red, green, blue, time, start, end);
}

/*
=============
Fog_ParseServerMessage

handle an SVC_FOG message from server
(kompatibel: liest wie gehabt density+rgb+time; optional start/end wenn verfügbar)
=============
*/
void Fog_ParseServerMessage (void)
{
	float density, red, green, blue, time;

	density = MSG_ReadByte () / 255.0f;
	red = MSG_ReadByte () / 255.0f;
	green = MSG_ReadByte () / 255.0f;
	blue = MSG_ReadByte () / 255.0f;
	time = MSG_ReadShort () / 100.0f;
	if (time < 0.0f) time = 0.0f;

	// Optional: zwei zusätzliche Shorts für start/end (Skalierung 1/100)
	// Wenn das Protokoll sie nicht sendet, bleiben sie ungültig und werden ignoriert.
	float start = -1.0f, end = -1.0f;
#ifdef MSG_MoreBytes
	if (MSG_MoreBytes ()) start = MSG_ReadShort () / 100.0f;
	if (MSG_MoreBytes ()) end = MSG_ReadShort () / 100.0f;
	Fog_UpdateEx (density, red, green, blue, time, start, end);
#else
	Fog_Update (density, red, green, blue, time);
#endif
}

/*
=============
Fog_FogCommand_f

handle the 'fog' console command
Erweiterung: fog <density> <r> <g> <b> <start> <end> [time]
=============
*/
void Fog_FogCommand_f (void)
{
	float d = fog_density, r = fog_red, g = fog_green, b = fog_blue;
	float t = 0.0f;
	float s = fog_start, e = fog_end;

	switch (Cmd_Argc ())
	{
	default:
	case 1:
		Con_Printf ("usage:\n");
		Con_Printf ("   fog <density>\n");
		Con_Printf ("   fog <red> <green> <blue>\n");
		Con_Printf ("   fog <density> <red> <green> <blue>\n");
		Con_Printf ("   fog <density> <red> <green> <blue> <start> <end> [time]\n");
		Con_Printf ("current values:\n");
		Con_Printf ("   density=%g\n", fog_density);
		Con_Printf ("   color=(%g %g %g)\n", fog_red, fog_green, fog_blue);
		Con_Printf ("   start=%g end=%g\n", fog_start, fog_end);
		return;

	case 2: // density
		d = Q_atof (Cmd_Argv (1));
		break;

	case 4: // rgb
		r = Q_atof (Cmd_Argv (1));
		g = Q_atof (Cmd_Argv (2));
		b = Q_atof (Cmd_Argv (3));
		break;

	case 5: // density + rgb
		d = Q_atof (Cmd_Argv (1));
		r = Q_atof (Cmd_Argv (2));
		g = Q_atof (Cmd_Argv (3));
		b = Q_atof (Cmd_Argv (4));
		break;

	case 7: // density + rgb + start end
		d = Q_atof (Cmd_Argv (1));
		r = Q_atof (Cmd_Argv (2));
		g = Q_atof (Cmd_Argv (3));
		b = Q_atof (Cmd_Argv (4));
		s = Q_atof (Cmd_Argv (5));
		e = Q_atof (Cmd_Argv (6));
		break;

	case 8: // density + rgb + start end + time
		d = Q_atof (Cmd_Argv (1));
		r = Q_atof (Cmd_Argv (2));
		g = Q_atof (Cmd_Argv (3));
		b = Q_atof (Cmd_Argv (4));
		s = Q_atof (Cmd_Argv (5));
		e = Q_atof (Cmd_Argv (6));
		t = Q_atof (Cmd_Argv (7));
		break;
	}

	// clamp & prüfen
	if (d < 0.0f) d = 0.0f;
	r = CLAMP (0.0f, r, 1.0f);
	g = CLAMP (0.0f, g, 1.0f);
	b = CLAMP (0.0f, b, 1.0f);

	if (!(e > s && s >= 0.0f)) { s = -1.0f; e = -1.0f; }

	Fog_UpdateEx (d, r, g, b, t, s, e);
}

/*
=============
Fog_ParseWorldspawn

called at map load (akzeptiert: "fog" = d r g b [start end])
=============
*/
void Fog_ParseWorldspawn (void)
{
	char key[128], value[4096];
	const char* data;

	// defaults
	fog_density = DEFAULT_DENSITY;
	fog_red = DEFAULT_GRAY;
	fog_green = DEFAULT_GRAY;
	fog_blue = DEFAULT_GRAY;

	old_density = DEFAULT_DENSITY;
	old_red = DEFAULT_GRAY;
	old_green = DEFAULT_GRAY;
	old_blue = DEFAULT_GRAY;

	fog_start = old_start = -1.0f;
	fog_end = old_end = -1.0f;

	fade_time = 0.0f;
	fade_done = 0.0f;

	data = COM_Parse (cl.worldmodel->entities);
	if (!data) return; // error
	if (com_token[0] != '{') return; // error
	while (1)
	{
		data = COM_Parse (data);
		if (!data) return; // error
		if (com_token[0] == '}') break; // end of worldspawn
		if (com_token[0] == '_') q_strlcpy (key, com_token + 1, sizeof (key));
		else                     q_strlcpy (key, com_token, sizeof (key));
		while (key[0] && key[strlen (key) - 1] == ' ') // remove trailing spaces
			key[strlen (key) - 1] = 0;
		data = COM_ParseEx (data, CPE_ALLOWTRUNC);
		if (!data) return; // error
		q_strlcpy (value, com_token, sizeof (value));

		if (!strcmp ("fog", key))
		{
			// akzeptiert: d r g b [start end]
			float d, r, g, b, s = -1.0f, e = -1.0f;
			int n = sscanf (value, "%f %f %f %f %f %f", &d, &r, &g, &b, &s, &e);
			if (n >= 4) {
				fog_density = (d < 0.0f) ? 0.0f : d;
				fog_red = CLAMP (0.0f, r, 1.0f);
				fog_green = CLAMP (0.0f, g, 1.0f);
				fog_blue = CLAMP (0.0f, b, 1.0f);
			}
			if (n >= 6 && e > s && s >= 0.0f) {
				fog_start = s; fog_end = e;
			}
			else {
				fog_start = -1.0f; fog_end = -1.0f;
			}
		}
	}
}

/*
=============
Fog_GetColor

calculates fog color for this frame, taking into account fade times
=============
*/
float* Fog_GetColor (void)
{
	static float c[4];
	float f;
	int i;

	if (fade_done > cl.time && fade_time > 0.0f)
	{
		f = (fade_done - cl.time) / fade_time;
		c[0] = f * old_red + (1.0f - f) * fog_red;
		c[1] = f * old_green + (1.0f - f) * fog_green;
		c[2] = f * old_blue + (1.0f - f) * fog_blue;
		c[3] = 1.0f;
	}
	else
	{
		c[0] = fog_red;
		c[1] = fog_green;
		c[2] = fog_blue;
		c[3] = 1.0f;
	}

	for (i = 0; i < 3; i++)
		c[i] = CLAMP (0.0f, c[i], 1.0f);

	// find closest 24-bit RGB value, so solid-colored sky can match the fog perfectly
	for (i = 0; i < 3; i++)
		c[i] = (float)(Q_rint (c[i] * 255)) / 255.0f;

	return c;
}

/*
=============
Fog_GetDensity

returns current density of fog
=============
*/
float Fog_GetDensity (void)
{
	float f;

	if (fade_done > cl.time && fade_time > 0.0f)
	{
		f = (fade_done - cl.time) / fade_time;
		return f * old_density + (1.0f - f) * fog_density;
	}
	else
	{
		return fog_density;
	}
}

/*
=============
Fog_GetLinearRange

liefert die (ggf. interpolierten) Start/Ende-Werte
=============
*/
static void Fog_GetLinearRange (float* start, float* end)
{
	if (fade_done > cl.time && fade_time > 0.0f)
	{
		float f = (fade_done - cl.time) / fade_time;
		*start = f * old_start + (1.0f - f) * fog_start;
		*end = f * old_end + (1.0f - f) * fog_end;
	}
	else
	{
		*start = fog_start;
		*end = fog_end;
	}
}

/*
=============
Fog_SetupFrame

called at the beginning of each frame
=============
*/
void Fog_SetupFrame (void)
{
	const float ExpAdjustment = 1.20112241f; // sqrt(log2(e))
	const float SphericalCorrection = 0.85f; // compensate higher perceived density with spherical fog
	const float DensityScale = ExpAdjustment * SphericalCorrection / 64.0f;

	float density = Fog_GetDensity () * DensityScale;
	memcpy (r_framedata.fogdata, Fog_GetColor (), 3 * sizeof (float));
	memcpy (r_framedata.skyfogdata, r_framedata.fogdata, 3 * sizeof (float));
	r_framedata.fogdata[3] = density * density; // EXP2
	r_framedata.skyfogdata[3] = density > 0.0f ? CLAMP (0.0f, skyfog, 1.0f) : 0.0f;

	// Linear-Parameter berechnen
	float s, e;
	Fog_GetLinearRange (&s, &e);
	if (e > s && s >= 0.0f)
	{
		r_fogparams[0] = s;
		r_fogparams[1] = e;
		r_fogparams[2] = 1.0f / (e - s);
		r_fogparams[3] = 1.0f; // mode linear
#ifdef R_FRAME_HAS_FOGPARAMS
		memcpy (r_framedata.fogparams, r_fogparams, 4 * sizeof (float));
#endif
	}
	else
	{
		r_fogparams[0] = -1.0f;
		r_fogparams[1] = -1.0f;
		r_fogparams[2] = 0.0f;
		r_fogparams[3] = 0.0f; // mode exp2
#ifdef R_FRAME_HAS_FOGPARAMS
		memcpy (r_framedata.fogparams, r_fogparams, 4 * sizeof (float));
#endif
	}
}

/*
=============
Fog_EnableGFog

called before drawing stuff that should be fogged
=============
*/
void Fog_EnableGFog (void)
{
	Fog_SetupFrame ();
}

/*
=============
Fog_DisableGFog

called after drawing stuff that should be fogged
=============
*/
void Fog_DisableGFog (void)
{
	memset (r_framedata.fogdata, 0, sizeof (r_framedata.fogdata));
	// r_fogparams bleibt erhalten, damit der Renderer bei Bedarf den Modus kennt
}

//==============================================================================
//
//  VOLUMETRIC FOG
//
//==============================================================================

cvar_t r_vfog = { "r_vfog", "1", CVAR_NONE };

void Fog_DrawVFog (void) {}
void Fog_MarkModels (void) {}

//==============================================================================
//
//  INIT
//
//==============================================================================

/*
=============
Fog_NewMap

called whenever a map is loaded
=============
*/
void Fog_NewMap (void)
{
	Fog_ParseWorldspawn (); // for global fog
	Fog_MarkModels ();     // for volumetric fog
}

/*
=============
Fog_Init

called when quake initializes
=============
*/
void Fog_Init (void)
{
	Cmd_AddCommand ("fog", Fog_FogCommand_f);

	//Cvar_RegisterVariable (&r_vfog);

	// set up global fog defaults
	fog_density = DEFAULT_DENSITY;
	fog_red = DEFAULT_GRAY;
	fog_green = DEFAULT_GRAY;
	fog_blue = DEFAULT_GRAY;

	old_density = DEFAULT_DENSITY;
	old_red = DEFAULT_GRAY;
	old_green = DEFAULT_GRAY;
	old_blue = DEFAULT_GRAY;

	fog_start = old_start = -1.0f;
	fog_end = old_end = -1.0f;

	fade_time = 0.0f;
	fade_done = 0.0f;

	r_fogparams[0] = -1.0f;
	r_fogparams[1] = -1.0f;
	r_fogparams[2] = 0.0f;
	r_fogparams[3] = 0.0f; // exp2 default
}
