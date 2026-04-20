#ifndef RENDERER_HOST_BRIDGE_H
#define RENDERER_HOST_BRIDGE_H

#include "q_stdinc.h"
#include <stdarg.h>

struct cvar_s;
struct cache_user_s;
struct qmodel_s;
struct mleaf_s;
struct entity_s;
struct qcvm_s;
struct edict_s;
struct hull_s;
struct trace_s;
struct lumpinfo_s;
struct quakeparms_s;
struct mousecursor_s;

typedef struct cvar_s cvar_t;
typedef struct cache_user_s cache_user_t;
typedef struct qmodel_s qmodel_t;
typedef struct mleaf_s mleaf_t;
typedef struct entity_s entity_t;
typedef struct qcvm_s qcvm_t;
#ifndef GL_MODEL_H
typedef struct hull_s hull_t;
#endif
#ifndef _QUAKE_WORLD_H
typedef struct trace_s trace_t;
#endif
#ifndef _QUAKE_WAD_H
typedef struct lumpinfo_s lumpinfo_t;
#endif
#ifndef QUAKEDEFS_H
typedef struct quakeparms_s quakeparms_t;
#endif
typedef struct edict_s edict_t;

typedef struct findfile_s findfile_t;
#ifndef _Q_COMMON_H
typedef enum {cpe_none, cpe_quake, cpe_tokenonly} cpe_mode;
typedef struct stringview_s stringview_t;
#endif
#ifndef _CLIENT_H_
typedef struct client_state_s client_state_t;
typedef struct client_static_s client_static_t;
typedef struct scoreboard_s scoreboard_t;
#endif
#ifndef QUAKE_SERVER_H
typedef struct server_s server_t;
#endif
#ifndef __VID_DEFS_H
typedef struct viddef_s viddef_t;
#endif
#ifndef _QUAKE_RENDER_H
typedef struct refdef_s refdef_t;
#endif
typedef struct dlight_s dlight_t;
typedef struct particle_debug_stats_s particle_debug_stats_t;
typedef struct glcanvas_s glcanvas_t;

struct mplane_s;
struct mnode_s;
struct texture_s;
struct material_s;
struct mat_texmatrix_s;
struct mat_wave_s;
struct cmd_function_s;
struct postfx_state_s;
struct lightgrid_s;
struct gpulightbuffer_s;
struct rl_light_s;
struct gpuframedata_s;
struct r_ssao_fog_state_s;
struct bc7enc_compress_block_params_s;
typedef struct mplane_s mplane_t;
typedef struct mnode_s mnode_t;
typedef struct texture_s texture_t;
typedef struct material_s material_t;
typedef struct mat_texmatrix_s mat_texmatrix_t;
typedef struct mat_wave_s mat_wave_t;
typedef struct cmd_function_s cmd_function_t;
typedef struct postfx_state_s postfx_state_t;
typedef struct lightgrid_s lightgrid_t;
typedef struct gpulightbuffer_s gpulightbuffer_t;
typedef struct rl_light_s rl_light_t;
typedef struct gpuframedata_s gpuframedata_t;
typedef struct r_ssao_fog_state_s r_ssao_fog_state_t;
typedef struct bc7enc_compress_block_params_s bc7enc_compress_block_params_t;

#ifndef _QUAKE_KEYS_H
typedef enum {key_game, key_console, key_message, key_menu} keydest_t;
#endif

#ifndef FUNC_NORETURN
#if defined(__GNUC__) || defined(__clang__)
#define FUNC_NORETURN __attribute__((noreturn))
#elif defined(_MSC_VER)
#define FUNC_NORETURN __declspec(noreturn)
#else
#define FUNC_NORETURN
#endif
#endif

#define IW_RENDERER_HOST_BRIDGE_ABI_VERSION 1u

typedef struct iw_renderer_host_bridge_functions_s
{
	unsigned int struct_size;

	void (*con_vprintf)(const char *fmt, va_list args);
	void (*con_vdprintf)(const char *fmt, va_list args);
	void (*con_vwarning)(const char *fmt, va_list args);
	void (*con_vdwaring)(const char *fmt, va_list args);
	void (*con_vsafe_printf)(const char *fmt, va_list args);
	void (*con_vlink_printf)(const char *addr, const char *fmt, va_list args);
	void (*con_add_to_tab_list)(const char *name, const char *partial, const char *type);

	void (*cvar_register)(cvar_t *variable);
	void (*cvar_set_callback)(cvar_t *var, void (*func)(cvar_t *));
	void (*cvar_set_quick)(cvar_t *var, const char *value);
	void (*cvar_set_value_quick)(cvar_t *var, float value);
	cvar_t *(*cvar_find_var)(const char *var_name);
	void (*cvar_set_value)(const char *var_name, float value);
	void (*cvar_set)(const char *var_name, const char *value);
	float (*cvar_variable_value)(const char *var_name);

	void *(*cmd_add_command)(const char *name, void (*func)(void));
	int (*cmd_argc)(void);
	const char *(*cmd_argv)(int arg);

	const char *(*com_parse)(const char *data);
	const char *(*com_parse_ex)(const char *data, cpe_mode mode);
	int (*com_check_parm)(const char *parm);
	void (*com_strip_extension)(const char *in, char *out, size_t outsize);
	void (*com_file_base)(const char *in, char *out, size_t outsize);
	void (*com_add_extension)(char *path, const char *extension, size_t len);
	const char *(*com_file_get_extension)(const char *in);
	qboolean (*com_has_extension)(const char *path, const char *extension);
	char *(*com_tint_string)(const char *in, char *out, size_t outsize);
	unsigned (*com_hash_block)(const void *data, size_t size);
	byte *(*com_load_hunk_file)(const char *path, unsigned int *path_id);
	byte *(*com_load_malloc_file)(const char *path, unsigned int *path_id);
	int (*com_fopen_file)(const char *filename, FILE **file, unsigned int *path_id);
	qboolean (*com_file_exists)(const char *filename, unsigned int *path_id);
	const char *(*com_skip_path)(const char *pathname);
	qboolean (*com_parse_line)(const char **str, stringview_t *line);
	char *(*com_tint_substring)(const char *in, const char *substr, char *out, size_t outsize);

	char *(*va_list_fn)(const char *format, va_list args);
	int (*q_snprintf)(char *str, size_t size, const char *format, ...) FUNC_PRINTF(3,4);
	int (*q_vsnprintf)(char *str, size_t size, const char *format, va_list args);
	int (*q_atoi)(const char *str);
	float (*q_atof)(const char *str);
	size_t (*q_strlcpy)(char *dst, const char *src, size_t size);
	size_t (*q_strlcat)(char *dst, const char *src, size_t size);
	int (*q_strcasecmp)(const char *s1, const char *s2);
	int (*q_strncasecmp)(const char *s1, const char *s2, size_t n);

	void (*sys_verror)(const char *error, va_list args);
	void (*sys_vprintf)(const char *fmt, va_list args);
	double (*sys_double_time)(void);
	void (*sys_send_key_events)(void);
	void (*sys_sleep)(unsigned long msecs);
	FILE *(*sys_fopen)(const char *path, const char *mode);
	qboolean (*sys_get_file_time)(const char *path, time_t *out);
	int (*sys_file_type)(const char *path);
	long (*sys_ftell)(FILE *file);
	findfile_t *(*sys_find_first)(const char *dir, const char *ext);
	findfile_t *(*sys_find_next)(findfile_t *find);
	void (*sys_find_close)(findfile_t *find);

	void (*mod_init)(void);
	void (*mod_clear_all)(void);
	void (*mod_reset_all)(void);
	qmodel_t *(*mod_for_name)(const char *name, qboolean crash);
	void *(*mod_extradata)(qmodel_t *mod);
	void (*mod_touch_model)(const char *name);
	mleaf_t *(*mod_point_in_leaf)(vec3_t p, qmodel_t *model);
	byte *(*mod_leaf_pvs)(mleaf_t *leaf, qmodel_t *model);
	byte *(*mod_no_vis_pvs)(qmodel_t *model);
	void (*mod_set_extra_flags)(qmodel_t *mod);
	qboolean (*mod_is_known_model)(const qmodel_t *mod);

	void *(*hunk_alloc)(int size);
	void *(*hunk_alloc_name)(int size, const char *name);
	int (*hunk_low_mark)(void);
	void (*hunk_free_to_low_mark)(int mark);
	void *(*cache_check)(cache_user_t *c);
	void (*cache_free)(cache_user_t *c, qboolean freetextures);
	void *(*cache_alloc)(cache_user_t *c, int size, const char *name);

	void *(*w_get_lump_name)(const char *name, lumpinfo_t **out_info);

	void (*pr_switch_qcvm)(qcvm_t *nvm);
	void (*pr_push_qcvm)(qcvm_t *newvm, qcvm_t **oldvm);
	void (*pr_pop_qcvm)(qcvm_t *oldvm);

	byte *(*sv_fat_pvs)(vec3_t org, qmodel_t *worldmodel);
	qboolean (*sv_edict_in_pvs)(edict_t *test, byte *pvs);

	qboolean (*cl_in_cutscene)(void);
	qboolean (*cl_is_player_ent)(const entity_t *ent);

	int (*msg_read_byte)(void);
	int (*msg_read_short)(void);

	void (*m_draw)(void);
	qboolean (*m_wants_console)(float *alpha);
	qboolean (*m_forced_center_print)(float *alpha);
	qboolean (*m_forced_underwater)(void);
	void (*m_draw_text_box)(int x, int y, int width, int lines);

	void (*v_polyblend)(void);

	void (*host_verror)(const char *error, va_list args);

	qboolean (*image_write_tga)(const char *name, byte *data, int width, int height, int bpp, qboolean upsidedown);
	qboolean (*image_write_png)(const char *name, byte *data, int width, int height, int bpp, qboolean upsidedown);
	qboolean (*image_write_jpg)(const char *name, byte *data, int width, int height, int bpp, int quality, qboolean upsidedown);

	void *(*vid_get_window)(void);
	qboolean (*vid_has_mouse_focus)(void);
	qboolean (*vid_is_minimized)(void);
	void (*vid_set_window_title)(const char *title);
	void (*vid_recalc_console_size)(void);
	void (*vid_recalc_interface_size)(void);
	void (*vid_lock)(void);

	void (*scr_center_print)(const char *str);
	int (*scr_modal_message)(const char *text, float timeout);

	void *(*sys_load_library)(const char *path);
	void *(*sys_get_library_function)(void *lib, const char *name);
	void (*sys_close_library)(void *lib);
	qboolean (*sys_is_debugger_present)(void);

	void (*cvar_set_completion)(cvar_t *var, void (*func)(cvar_t *, const char *));
	void *(*hunk_alloc_no_fill)(int size);
	void *(*hunk_alloc_name_no_fill)(int size, const char *name);
	void (*cbuf_add_text)(const char *text);

	void (*vector_ma)(const vec3_t veca, float scale, const vec3_t vecb, vec3_t vecc);
	float (*vector_normalize)(vec3_t v);
	int (*vector_compare)(const vec3_t v1, const vec3_t v2);
	vec_t (*vector_length)(const vec3_t v);
	void (*vector_lerp)(const vec3_t veca, const vec3_t vecb, float frac, vec3_t dst);
	void (*cross_product)(const vec3_t v1, const vec3_t v2, vec3_t cross);
	void (*angle_vectors)(vec3_t angles, vec3_t forward, vec3_t right, vec3_t up);
	float (*distance_fn)(const vec3_t a, const vec3_t b);
	void (*vector_scale)(const vec3_t in, vec_t scale, vec3_t out);
	void (*vector_inverse)(vec3_t v);
	void (*project_vector)(const vec3_t src, const float matrix[16], vec3_t dst);
	void (*matrix_multiply)(float left[16], float right[16]);
	void (*rotation_matrix)(float matrix[16], float angle, int axis);
	void (*translation_matrix)(float matrix[16], float x, float y, float z);
	void (*matrix_transpose_4x3)(const float src[16], float dst[12]);
	qboolean (*mat4_inverse)(const float in[16], float out[16]);
	void (*r_concat_transforms)(float in1[3][4], float in2[3][4], float out[3][4]);
	void (*apply_translation)(float matrix[16], float x, float y, float z);
	void (*apply_scale)(float matrix[16], float x, float y, float z);
	uint32_t (*interleave)(uint16_t even, uint16_t odd);
	qboolean (*ray_vs_box)(const vec3_t org, const vec3_t rcpdelta, const vec3_t mins, const vec3_t maxs, float *frac);
	int (*box_on_plane_side)(vec3_t emins, vec3_t emaxs, mplane_t *plane);
	int (*q_next_pow2)(int val);
	int (*q_log2)(int val);
	float (*get_fraction)(float val, float minval, float maxval);
	unsigned short (*crc_block)(const void *start, int count);
	void (*swap_pic)(void *pic);

	int (*q_strcmp)(const char *s1, const char *s2);
	int (*q_strncmp)(const char *s1, const char *s2, int count);
	void (*q_strncpy)(char *dest, const char *src, int count);
	int (*q_strlen)(const char *str);
	char *(*q_strcasestr)(const char *haystack, const char *needle);
	void *(*q_malloc_fn)(size_t size);
	void (*q_free_fn)(void *ptr);
	void *(*q_realloc_fn)(void *ptr, size_t size);
	void *(*q_calloc_fn)(size_t count, size_t size);

	void (*r_backend_register)(const void *backend);
	void (*r_backend_init)(void);
	void (*r_backend_shutdown)(void);
	void (*r_backend_memory_barrier)(unsigned barrier_bits);
	void (*r_backend_set_dynamic_state)(const void *dynamic_state);
	void (*r_backend_draw_indexed)(int primitive, int index_type, int count, intptr_t index_offset_bytes);
	void (*r_backend_bind_pipeline)(const void *pipeline);
	void (*r_backend_set_viewport)(int x, int y, int width, int height);
	void (*r_backend_finish)(void);
	void (*r_backend_draw_fn)(int primitive, int first, int count);
	void (*r_backend_dispatch)(unsigned group_x, unsigned group_y, unsigned group_z);
	void (*r_backend_present)(void);
	void (*r_backend_begin_frame)(void);
	void (*r_backend_end_frame)(void);
	void (*r_backend_on_resize)(int width, int height);
	int (*r_backend_get_scene_sample_count)(void);
	void (*r_backend_draw_indexed_indirect)(int primitive, int index_type, intptr_t indirect_offset_bytes);
	void (*r_backend_multi_draw_indexed_indirect)(int primitive, int index_type, intptr_t indirect_offset_bytes, int draw_count, int stride_bytes);
	void (*r_backend_draw_indexed_instanced)(int primitive, int index_type, int count, intptr_t index_offset_bytes, int instance_count);
	void (*r_backend_draw_instanced)(int primitive, int first, int count, int instance_count);
	void (*r_backend_draw_packet)(const void *packet);
	void (*r_backend_bind_descriptors)(const void *bindings, unsigned count);
	void (*r_backend_set_blend_factors)(int src, int dst);
	void (*r_backend_set_depth_func)(int depth_func);
	const void *(*r_backend_get_caps)(void);
	void (*r_backend_configure_postfx_lut_texture)(unsigned texture_id);
	unsigned (*r_backend_create_postfx_lut_texture)(void);

	void (*sbar_changed)(void);
	void (*sbar_draw)(void);
	void (*sbar_load_pics)(void);
	void (*sbar_intermission_overlay)(void);
	void (*sbar_finale_overlay)(void);
	void (*con_clear_notify)(void);
	void (*con_draw_console)(int lines, qboolean drawbg, qboolean drawinput);
	void (*con_draw_notify)(void);
	void (*con_check_resize)(void);
	void (*con_vdprintf2)(const char *fmt, va_list args);
	void (*key_get_grabbed_input)(int *lastkey, int *lastchar);
	void (*key_begin_input_grab)(void);
	void (*key_end_input_grab)(void);
	void (*key_clear_states)(void);
	void (*in_deactivate_for_console)(void);
	void (*in_deactivate_for_menu)(void);
	void (*in_clear_states)(void);
	void (*m_print)(int cx, int cy, const char *str);
	void (*v_render_view)(void);
	void (*v_calc_blend)(void);
	void (*v_update_blend)(void);
	void (*v_set_contents_color)(int contents);
	void (*cl_postfx_set_contents)(int contents, qboolean underwater_active, qboolean underwater_postfx_active);
	void (*cl_postfx_get_state)(void *out_state);
	qboolean (*ed_is_relevant_field)(edict_t *ed, void *d);
	const char *(*ed_field_value_string)(edict_t *ed, void *d);
	const char *(*pr_get_string)(int num);
	int (*num_for_edict)(edict_t *);
	void (*pr_reload_pics)(qboolean purge);
	void (*w_load_wad_file)(void);
	void *(*z_malloc)(int size);
	void (*z_free_fn)(void *ptr);
	void (*sys_report_verror)(const char *error, va_list args);
	void (*host_report_verror)(const char *error, va_list args);
	void (*host_begin_asset_loading)(void);
	void (*host_end_asset_loading)(void);
	qboolean (*host_is_saving)(void);
	int (*cfg_open_config)(const char *cfg_name);
	void (*cfg_close_config)(void);
	void (*cfg_read_cvars)(const char **vars, int num_vars);
	void (*cfg_read_cvar_overrides)(const char **vars, int num_vars);
	void *(*cmd_add_command2)(const char *cmd_name, void (*function)(void), int srctype, qboolean qcinterceptable);
	void (*pl_set_window_icon)(void);
	void (*pl_vid_shutdown)(void);
	void (*vid_menu_init)(void);
	byte *(*image_load_image)(const char *name, int *width, int *height, int *fmt);
	qboolean (*steam_save_screenshot)(const void *rgb, int width, int height);
	void (*s_extra_update)(void);
	void (*s_clear_buffer)(void);
	void (*s_stop_all_sounds)(qboolean clear);
	void (*cdaudio_pause)(void);
	void (*cdaudio_resume)(void);
	void (*bgm_pause)(void);
	void (*bgm_resume)(void);
	size_t (*utf8_from_quake)(char *dst, size_t maxbytes, const char *src);
	size_t (*utf8_to_quake)(char *dst, size_t maxbytes, const char *src);
	void (*multi_string_append)(char **pvec, const char *str);
	void (*multi_string_append_n)(char **pvec, const char *str, size_t len);
	float (*msg_read_coord)(unsigned int flags);
	int (*msg_read_char)(void);
	trace_t (*sv_move)(vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end, int type, edict_t *passedict);
	qboolean (*sv_recursive_hull_check)(const hull_t *hull, int num, float p1f, float p2f, vec3_t p1, vec3_t p2, trace_t *trace);
	qboolean (*sv_box_in_pvs)(vec3_t mins, vec3_t maxs, byte *pvs, mnode_t *node);

	void (*material_init)(void);
	void (*material_shutdown)(void);
	void (*material_apply_to_texture)(texture_t *tex, const char *mapname);
	const material_t *(*material_find)(const char *name);
	const material_t *(*material_find_for_texture_name)(const char *texname, const char *mapname);
	void (*material_canonicalize)(const char *name, char *out, size_t out_size);
	int (*material_classify_particle_stage)(const void *stage, int policy, char *reason, size_t reason_size);
	qboolean (*material_stage_supports_particle_mvp)(const void *stage, char *reason, size_t reason_size);
	const mat_texmatrix_t *(*material_stage_eval_tex_matrix)(void *stage, float time);
	const char *(*material_stage_get_anim_map_path)(void *stage, float time);
	float (*material_eval_wave_value)(const mat_wave_t *wave, float time);

	void (*dlight_pool_clear_persistent)(void);
	int (*dlight_pool_collect_for_render)(double time, const vec3_t vieworg, const mleaf_t *viewleaf, dlight_t **out, int out_max);
	const dlight_t *const *(*dlight_pool_get_active_list)(int *count);
	int (*dlight_pool_get_budget)(void);
	dlight_t *(*dlight_pool_get_or_create_persistent)(int key, double time);
	void (*dlight_pool_new_frame)(double time, int framecount);

	void (*r_ppdlights_collect_frame)(void);
	int (*r_ppdlights_build_model_gpu_lights)(gpulightbuffer_t *out_buffer, dlight_t **out_sources, int max_lights);
	int (*r_ppdlights_build_world_gpu_lights)(gpulightbuffer_t *out_buffer, dlight_t **out_sources, int max_lights);
	const rl_light_t *(*r_ppdlights_get_frame_lights)(int *out_count);

	float (*r_skyvis_get_resolved_cap)(void);
	void (*r_skyvis_get_tint)(vec3_t out_tint);
	float (*r_skyvis_get_resolved_scale)(void);
	qboolean (*r_skyvis_active)(void);
	void (*r_skyvis_init)(void);
	void (*r_skyvis_new_map)(void);
	float (*r_skyvis_sample)(const vec3_t pos);

	float (*r_ssao_sanitize_value)(float value, float fallback, float minval, float maxval);
	void (*r_ssao_capture_fog_state)(const gpuframedata_t *framedata, r_ssao_fog_state_t *out_state);
	void (*r_ssao_register_cvars)(void);

	void (*r_quality_init)(void);
	void (*r_quality_update)(void);

	void (*r_framegraph_render_view)(void);
	void (*r_framegraph_get_timing_summary)(double *out_gpu_ms, double *out_cpu_ms, qboolean *out_gpu_valid);
	void (*r_framegraph_set_render_frame_plan)(const void *plan);
	unsigned (*r_framegraph_resolve_required_resource_by_slot)(const void *resources, int slot, const char *usage_tag);
	qboolean (*r_framegraph_get_render_frame_plan)(void *out_plan);
	qboolean (*r_framegraph_add_pass)(const void *pass_desc);

	float (*r_tonemap_tempered_overbright)(float overbright);

	void (*bc7enc_compress_block_init)(void);
	int (*bc7enc_compress_block_fn)(void *pBlock, const void *pPixelsRGBA, const void *pComp_params);

	void (*lightgrid_free)(lightgrid_t *lg);

	void (*vec_grow)(void **pvec, size_t element_size, size_t count);
	void (*vec_append)(void **pvec, size_t element_size, const void *data, size_t count);
	void (*vec_clear)(void **pvec);
	void (*vec_free)(void **pvec);
} iw_renderer_host_bridge_functions_t;

typedef struct iw_renderer_host_bridge_data_s
{
	unsigned int struct_size;

	quakeparms_t *host_parms;
	const double *realtime;
	const double *host_frametime;
	const double *host_rawframetime;
	const qboolean *host_initialized;
	byte **host_colormap;
	client_state_t *cl;
	client_static_t *cls;
	server_t *sv;
	edict_t **sv_player;
	qcvm_t **qcvm;
	const float (*v_blend)[4];
	char *com_token;
	const int *com_argc;
	const char **com_argv;
	char *com_gamedir;
	qboolean *con_forcedup;
	byte *const *con_chars;
	keydest_t *key_dest;
	int *sb_lines;
	int *m_state;
	void *cl_static_entities;
	void *cl_lightstyle;
	void *cl_visedicts;
	int *cl_numvisedicts;
	entity_t **cl_entities;
	void *in_attack;
	void *com_searchpaths;
	void *com_filesize;
	qboolean *con_initialized;

	cvar_t *developer;
	cvar_t *map_checks;
	cvar_t *scr_scale;
	cvar_t *chase_active;
	cvar_t *sensitivity;
	byte **wad_base;

	const vec3_t *vec3_origin;
	const vec4_t *vec4_origin;
	cvar_t *crosshair;
	char *crosshair_char;
	cvar_t *con_notifyfade;
	cvar_t *con_notifyfadetime;
	void *dev_stats;
	void *dev_peakstats;
	cvar_t *devstats;
	cvar_t *host_timescale;
	qboolean *isDedicated;
	cvar_t *r_material_debug;
	cvar_t *r_materials;
	cvar_t *r_skyvis;
	cvar_t *r_skyvis_debug;
	cvar_t *r_sun_visibility;
	cvar_t *r_tcgen_debug;
	cvar_t *r_particles_material_strict;
	cvar_t *sv_gravity;
} iw_renderer_host_bridge_data_t;

typedef struct iw_renderer_host_bridge_s
{
	unsigned int struct_size;
	unsigned int abi_version;
	const iw_renderer_host_bridge_functions_t *functions;
	const iw_renderer_host_bridge_data_t *data;
} iw_renderer_host_bridge_t;

#define IW_RENDERER_HOST_BRIDGE_FUNCTIONS_SIZE ((unsigned int)(offsetof(iw_renderer_host_bridge_functions_t, vec_free) + sizeof(void*)))

typedef struct iw_renderer_entry_points_s
{
	unsigned int struct_size;
	void (*R_Init)(void);
	void (*R_RenderView)(void);
	void (*R_NewMap)(void);
	void (*R_ClearEfrags)(void);
	void (*R_CheckEfrags)(void);
	void (*R_AddEfrags)(entity_t *ent);
	void (*R_ParseParticleEffect)(void);
	void (*R_RunParticleEffect)(vec3_t org, vec3_t dir, int color, int count);
	void (*R_RocketTrail)(vec3_t start, vec3_t end, int type);
	void (*R_EntityParticles)(entity_t *ent);
	void (*R_BlobExplosion)(vec3_t org);
	void (*R_ParticleExplosion)(vec3_t org);
	void (*R_ParticleExplosion2)(vec3_t org, int colorStart, int colorLength);
	void (*R_LavaSplash)(vec3_t org);
	void (*R_TeleportSplash)(vec3_t org);
	void (*R_SpawnImpactDecal)(const char *category, const vec3_t origin, const vec3_t normal);
	void (*R_SpawnImpactDecalEx)(const char *category, const vec3_t origin, const vec3_t normal, const vec3_t hit_dir, qboolean heavy_blood);
	void (*R_TranslatePlayerSkin)(int playernum);
	void (*R_TranslateNewPlayerSkin)(int playernum);
	void (*R_ClearBoundingBoxes)(void);
	void (*R_ClearParticles)(void);
	void (*R_ClearDecals)(void);
	void (*R_ReloadDecals)(void);
	void (*R_InitDecals)(void);
	void (*R_StorePrevFrameState)(void);
	void (*R_GetParticleDebugStats)(particle_debug_stats_t *stats);
	void (*R_SetAlphaMode)(int mode);
	int (*R_GetAlphaMode)(void);
	int (*R_GetEffectiveAlphaMode)(void);
	void (*R_AddStaticModels)(const byte *vis);
	void (*R_PushDlights)(void);
	void (*R_ParseDlightEntities)(void);
	const void *(*R_GetLightgridSample)(const vec3_t pos);
} iw_renderer_entry_points_t;

#endif
