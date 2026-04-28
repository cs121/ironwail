#ifndef REF_GL_BRIDGE_H
#define REF_GL_BRIDGE_H

#ifdef RENDERER_PLUGIN_BUILD

#ifdef _WIN32
#include <intrin.h>
#endif

#include "renderer_host_bridge.h"

extern const iw_renderer_host_bridge_t *g_host_bridge;
extern const iw_renderer_host_bridge_functions_t *g_bridge_fn;
extern const iw_renderer_host_bridge_data_t *g_bridge_data;

void Bridge_Init (const iw_renderer_host_bridge_t *bridge);

#define cl              (*g_bridge_data->cl)
#define cls             (*g_bridge_data->cls)
#define sv              (*g_bridge_data->sv)
#define sv_player       (*g_bridge_data->sv_player)
extern qcvm_t *qcvm;
#define v_blend         (*g_bridge_data->v_blend)
#define host_initialized (*g_bridge_data->host_initialized)
#define host_rawframetime (*g_bridge_data->host_rawframetime)
#define host_colormap   (*g_bridge_data->host_colormap)
#define com_token       (g_bridge_data->com_token)
#define com_argc        (*g_bridge_data->com_argc)
#define com_argv        (g_bridge_data->com_argv)
#define com_gamedir     (g_bridge_data->com_gamedir)
#define con_forcedup    (*g_bridge_data->con_forcedup)
#define con_chars       (*g_bridge_data->con_chars)
#define key_dest        (*g_bridge_data->key_dest)
#define sb_lines        (*g_bridge_data->sb_lines)
#define m_state         (*g_bridge_data->m_state)
#define host_parms      (g_bridge_data->host_parms)
#define realtime        (*g_bridge_data->realtime)
#define host_frametime  (*g_bridge_data->host_frametime)
#define cl_static_entities ((entity_t *)g_bridge_data->cl_static_entities)
#define cl_lightstyle ((lightstyle_t *)g_bridge_data->cl_lightstyle)
#define cl_visedicts ((entity_t **)g_bridge_data->cl_visedicts)
#define cl_numvisedicts (*g_bridge_data->cl_numvisedicts)
#define cl_entities (*g_bridge_data->cl_entities)
#define in_attack (*(kbutton_t *)g_bridge_data->in_attack)
#define com_searchpaths (*(searchpath_t **)g_bridge_data->com_searchpaths)
#define com_filesize (*(qfileofs_t *)g_bridge_data->com_filesize)
#define con_initialized (*g_bridge_data->con_initialized)
#define developer       (*g_bridge_data->developer)
#define map_checks      (*g_bridge_data->map_checks)
#define scr_scale       (*g_bridge_data->scr_scale)
#define chase_active    (*g_bridge_data->chase_active)
#define sensitivity     (*g_bridge_data->sensitivity)
#define wad_base        (*g_bridge_data->wad_base)
#define vec3_origin     (*g_bridge_data->vec3_origin)
#define vec4_origin     (*g_bridge_data->vec4_origin)
#define crosshair       (*g_bridge_data->crosshair)
#define crosshair_char  (*g_bridge_data->crosshair_char)
#define con_notifyfade  (*g_bridge_data->con_notifyfade)
#define con_notifyfadetime (*g_bridge_data->con_notifyfadetime)
#define dev_stats       (*(devstats_t *)g_bridge_data->dev_stats)
#define dev_peakstats   (*(devstats_t *)g_bridge_data->dev_peakstats)
#define devstats        (*g_bridge_data->devstats)
#define host_timescale  (*g_bridge_data->host_timescale)
#define isDedicated     (*g_bridge_data->isDedicated)
#define r_material_debug (*g_bridge_data->r_material_debug)
#define r_materials     (*g_bridge_data->r_materials)
#define r_skyvis        (*g_bridge_data->r_skyvis)
#define r_skyvis_debug  (*g_bridge_data->r_skyvis_debug)
#define r_sun_visibility (*g_bridge_data->r_sun_visibility)
#define r_tcgen_debug   (*g_bridge_data->r_tcgen_debug)
#define r_particles_material_strict (*g_bridge_data->r_particles_material_strict)
#define sv_gravity      (*g_bridge_data->sv_gravity)

#endif /* RENDERER_PLUGIN_BUILD */

void Bridge_DrawInit (void);
void Bridge_DrawFlush (void);

#endif /* REF_GL_BRIDGE_H */
