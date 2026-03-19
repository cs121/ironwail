#include "quakedef.h"
#include "r_fogvol_internal.h"

static qboolean R_FogVol_IsSupportedShapeLocal (int shape)
{
	return shape == FOGVOL_SHAPE_BOX || shape == FOGVOL_SHAPE_SPHERE;
}

static int R_FogVol_NormalizeShapeLocal (int shape)
{
	return (shape == FOGVOL_SHAPE_SPHERE) ? FOGVOL_SHAPE_SPHERE : FOGVOL_SHAPE_BOX;
}

static void R_FogVol_ParseColorLocal (const char *value, vec3_t color)
{
	float r = 1.f, g = 1.f, b = 1.f;
	if (value && sscanf (value, "%f %f %f", &r, &g, &b) == 3)
	{
		if (r > 1.f || g > 1.f || b > 1.f)
		{
			r *= 1.f / 255.f;
			g *= 1.f / 255.f;
			b *= 1.f / 255.f;
		}
	}
	color[0] = r;
	color[1] = g;
	color[2] = b;
}

static qboolean R_FogVol_ParseVectorLocal (const char *value, vec3_t out)
{
	return value && sscanf (value, "%f %f %f", &out[0], &out[1], &out[2]) == 3;
}

typedef struct fogvol_entity_parse_state_s
{
	qboolean is_fog_volume;
	char modelname[64];
	vec3_t origin;
	qboolean has_origin;
} fogvol_entity_parse_state_t;

typedef enum fogvol_entity_key_type_e
{
	FOGVOL_ENTITY_KEY_FLOAT,
	FOGVOL_ENTITY_KEY_INT,
	FOGVOL_ENTITY_KEY_VEC3,
	FOGVOL_ENTITY_KEY_COLOR,
	FOGVOL_ENTITY_KEY_SPECIAL
} fogvol_entity_key_type_t;

typedef void (*fogvol_entity_special_setter_fn_t) (fog_volume_t *volume, fogvol_entity_parse_state_t *state, const char *value);

typedef struct fogvol_entity_key_dispatch_s
{
	const char *key;
	fogvol_entity_key_type_t type;
	size_t offset;
	fogvol_entity_special_setter_fn_t special_setter;
} fogvol_entity_key_dispatch_t;

static void R_FogVol_EntitySetFloat (fog_volume_t *volume, size_t offset, const char *value)
{
	float *field = (float *)((char *)volume + offset);
	*field = atof (value);
}

static void R_FogVol_EntitySetInt (fog_volume_t *volume, size_t offset, const char *value)
{
	int *field = (int *)((char *)volume + offset);
	*field = atoi (value);
}

static void R_FogVol_EntitySetVec3 (fog_volume_t *volume, size_t offset, const char *value)
{
	vec3_t *field = (vec3_t *)((char *)volume + offset);
	R_FogVol_ParseVectorLocal (value, *field);
}

static void R_FogVol_EntitySetColor (fog_volume_t *volume, size_t offset, const char *value)
{
	vec3_t *field = (vec3_t *)((char *)volume + offset);
	R_FogVol_ParseColorLocal (value, *field);
}

static void R_FogVol_EntitySetClassname (fog_volume_t *volume, fogvol_entity_parse_state_t *state, const char *value)
{
	(void)volume;
	if (!strcmp (value, "func_fog_volume") || !strcmp (value, "trigger_fog_volume"))
		state->is_fog_volume = true;
}

static void R_FogVol_EntitySetModel (fog_volume_t *volume, fogvol_entity_parse_state_t *state, const char *value)
{
	(void)volume;
	q_strlcpy (state->modelname, value, sizeof (state->modelname));
}

static void R_FogVol_EntitySetOrigin (fog_volume_t *volume, fogvol_entity_parse_state_t *state, const char *value)
{
	(void)volume;
	state->has_origin = R_FogVol_ParseVectorLocal (value, state->origin);
}

static void R_FogVol_EntitySetShape (fog_volume_t *volume, fogvol_entity_parse_state_t *state, const char *value)
{
	int parsed_shape;
	char *endptr = NULL;
	(void)state;

	if (!q_strcasecmp (value, "sphere"))
		parsed_shape = FOGVOL_SHAPE_SPHERE;
	else if (!q_strcasecmp (value, "box"))
		parsed_shape = FOGVOL_SHAPE_BOX;
	else
		parsed_shape = (int)strtol (value, &endptr, 10);

	if ((endptr && *endptr != '\0') || !R_FogVol_IsSupportedShapeLocal (parsed_shape))
	{
		if (developer.value > 0.f)
			Con_DPrintf ("FogVol: unsupported shape '%s' (expected %d=box or %d=sphere), defaulting to box\n",
				value, FOGVOL_SHAPE_BOX, FOGVOL_SHAPE_SPHERE);
		parsed_shape = FOGVOL_SHAPE_BOX;
	}

	volume->shape = parsed_shape;
}

static const fogvol_entity_key_dispatch_t fogvol_entity_key_dispatch[] = {
	{"classname", FOGVOL_ENTITY_KEY_SPECIAL, 0, R_FogVol_EntitySetClassname},
	{"model", FOGVOL_ENTITY_KEY_SPECIAL, 0, R_FogVol_EntitySetModel},
	{"origin", FOGVOL_ENTITY_KEY_SPECIAL, 0, R_FogVol_EntitySetOrigin},
	{"color", FOGVOL_ENTITY_KEY_COLOR, offsetof (fog_volume_t, color), NULL},
	{"density", FOGVOL_ENTITY_KEY_FLOAT, offsetof (fog_volume_t, density), NULL},
	{"falloff", FOGVOL_ENTITY_KEY_FLOAT, offsetof (fog_volume_t, falloff), NULL},
	{"maxdist", FOGVOL_ENTITY_KEY_FLOAT, offsetof (fog_volume_t, maxDistance), NULL},
	{"priority", FOGVOL_ENTITY_KEY_INT, offsetof (fog_volume_t, priority), NULL},
	{"noise_scale", FOGVOL_ENTITY_KEY_FLOAT, offsetof (fog_volume_t, noiseScale), NULL},
	{"noise_amount", FOGVOL_ENTITY_KEY_FLOAT, offsetof (fog_volume_t, noiseAmount), NULL},
	{"noise_bias", FOGVOL_ENTITY_KEY_FLOAT, offsetof (fog_volume_t, noiseBias), NULL},
	{"velocity", FOGVOL_ENTITY_KEY_VEC3, offsetof (fog_volume_t, velocity), NULL},
	{"mode", FOGVOL_ENTITY_KEY_INT, offsetof (fog_volume_t, mode), NULL},
	{"shape", FOGVOL_ENTITY_KEY_SPECIAL, 0, R_FogVol_EntitySetShape},
	{"radius", FOGVOL_ENTITY_KEY_FLOAT, offsetof (fog_volume_t, sphereRadius), NULL},
	{"center", FOGVOL_ENTITY_KEY_VEC3, offsetof (fog_volume_t, sphereCenter), NULL},
	{"blendmode", FOGVOL_ENTITY_KEY_INT, offsetof (fog_volume_t, blendMode), NULL},
	{"emissive", FOGVOL_ENTITY_KEY_FLOAT, offsetof (fog_volume_t, emissiveStrength), NULL},
	{"wind_dir", FOGVOL_ENTITY_KEY_VEC3, offsetof (fog_volume_t, windDir), NULL},
	{"wind_speed", FOGVOL_ENTITY_KEY_FLOAT, offsetof (fog_volume_t, windSpeed), NULL},
	{"turbulence", FOGVOL_ENTITY_KEY_FLOAT, offsetof (fog_volume_t, turbulence), NULL},
	{"height", FOGVOL_ENTITY_KEY_FLOAT, offsetof (fog_volume_t, height), NULL},
	{"height_scale", FOGVOL_ENTITY_KEY_FLOAT, offsetof (fog_volume_t, heightScale), NULL},
	{"edge_softness", FOGVOL_ENTITY_KEY_FLOAT, offsetof (fog_volume_t, edgeSoftness), NULL},
};

static const fogvol_entity_key_dispatch_t *R_FogVol_FindEntityKeyDispatch (const char *key)
{
	for (int i = 0; i < (int)countof (fogvol_entity_key_dispatch); ++i)
	{
		if (!strcmp (fogvol_entity_key_dispatch[i].key, key))
			return &fogvol_entity_key_dispatch[i];
	}
	return NULL;
}

static void R_FogVol_ApplyEntityKey (const fogvol_entity_key_dispatch_t *dispatch, fog_volume_t *volume, fogvol_entity_parse_state_t *state, const char *value)
{
	if (!dispatch)
		return;

	switch (dispatch->type)
	{
	case FOGVOL_ENTITY_KEY_FLOAT:
		R_FogVol_EntitySetFloat (volume, dispatch->offset, value);
		break;
	case FOGVOL_ENTITY_KEY_INT:
		R_FogVol_EntitySetInt (volume, dispatch->offset, value);
		break;
	case FOGVOL_ENTITY_KEY_VEC3:
		R_FogVol_EntitySetVec3 (volume, dispatch->offset, value);
		break;
	case FOGVOL_ENTITY_KEY_COLOR:
		R_FogVol_EntitySetColor (volume, dispatch->offset, value);
		break;
	case FOGVOL_ENTITY_KEY_SPECIAL:
		if (dispatch->special_setter)
			dispatch->special_setter (volume, state, value);
		break;
	default:
		break;
	}
}

void R_FogVol_ParseEntities (void)
{
	const char *data;

	R_FogVol_ClearEntities ();
	R_FogVol_ResetStaticLights ();
	R_FogVol_FreeStaticField ();

	if (!cl.worldmodel || !cl.worldmodel->entities)
	{
		R_FogVol_CommitStaticBuildConfig ();
		return;
	}

	data = cl.worldmodel->entities;
	data = COM_Parse (data);
	while (data && com_token[0])
	{
		fog_volume_t volume;
		fogvol_entity_parse_state_t parse_state;

		if (com_token[0] != '{')
			break;

		memset (&volume, 0, sizeof (volume));
		VectorSet (volume.color, 1.f, 1.f, 1.f);
		/* Default to disabled density. Many maps contain trigger/brush entities
		 * without explicit fog tuning; a non-zero default creates large white
		 * volume artifacts from unintended boxes. */
		volume.density = 0.0f;
		volume.falloff = 16.f;
		volume.mode = 0;
		volume.shape = FOGVOL_SHAPE_BOX;
		volume.blendMode = -1;
		volume.emissiveStrength = 0.f;
		volume.noiseScale = 0.05f;
		volume.noiseAmount = 0.5f;
		volume.noiseBias = 0.f;
		volume.turbulence = 0.f;
		VectorSet (volume.velocity, 0.f, 0.f, 0.f);
		volume.windSpeed = 0.f;
		VectorSet (volume.windDir, 0.f, 0.f, 0.f);
		volume.maxDistance = 2048.f;
		volume.priority = 0;
		volume.enabled = 1;
		volume.height = 0.f;
		volume.heightScale = 0.f;
		volume.edgeSoftness = 0.f;
		memset (&parse_state, 0, sizeof (parse_state));

		while (1)
		{
			char key[64], value[1024];
			data = COM_Parse (data);
			if (!data || !com_token[0])
				return;
			if (com_token[0] == '}')
				break;
			q_strlcpy (key, com_token, sizeof (key));
			if (key[0] == '_')
				memmove (key, key + 1, strlen (key) + 1);
			data = COM_ParseEx (data, CPE_ALLOWTRUNC);
			if (!data)
				return;
			q_strlcpy (value, com_token, sizeof (value));
			R_FogVol_ApplyEntityKey (R_FogVol_FindEntityKeyDispatch (key), &volume, &parse_state, value);
		}

		if (parse_state.is_fog_volume && parse_state.modelname[0])
		{
			qmodel_t *model = Mod_ForName (parse_state.modelname, false);
			if (model && model->type == mod_brush)
			{
				vec3_t mins;
				vec3_t maxs;
				VectorCopy (model->mins, mins);
				VectorCopy (model->maxs, maxs);
				if (parse_state.has_origin)
				{
					VectorAdd (mins, parse_state.origin, mins);
					VectorAdd (maxs, parse_state.origin, maxs);
				}
				VectorCopy (mins, volume.mins);
				VectorCopy (maxs, volume.maxs);
				if (R_FogVol_NormalizeShapeLocal (volume.shape) == FOGVOL_SHAPE_SPHERE)
				{
					for (int a = 0; a < 3; ++a)
						volume.sphereCenter[a] = 0.5f * (volume.mins[a] + volume.maxs[a]);
					if (volume.sphereRadius <= 0.f)
					{
						float ex = 0.5f * (volume.maxs[0] - volume.mins[0]);
						float ey = 0.5f * (volume.maxs[1] - volume.mins[1]);
						float ez = 0.5f * (volume.maxs[2] - volume.mins[2]);
						volume.sphereRadius = q_max (ex, q_max (ey, ez));
					}
				}
				R_FogVol_AddEntityVolume (&volume);
			}
		}

		data = COM_Parse (data);
	}

	R_FogVol_BuildStaticLightInjection ();
}
