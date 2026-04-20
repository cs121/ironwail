#include "quakedef.h"
#include "renderer_host_bridge.h"
#include "render.h"
#include "client.h"
#include "server.h"
#include "console.h"
#include "cmd.h"
#include "cvar.h"
#include "common.h"
#include "sys.h"
#include "zone.h"
#include "wad.h"
#include "draw.h"
#include "keys.h"
#include "view.h"
#include "screen.h"
#include "vid.h"
#include "progs.h"
#include "gl_model.h"
#include "image.h"
#include "r_backend.h"
#include "r_framegraph.h"
#include "r_dlight_pool.h"
#include "r_realtimelight.h"
#include "r_skyvis.h"
#include "r_ssao.h"
#include "r_quality.h"
#include "r_tonemap.h"
#include "gl_lightgrid.h"
#include "lightgrid.h"
#include "mat_material.h"
#include "cl_postfx.h"
#include "cfgfile.h"
#include "sbar.h"
#include "input.h"
#include "menu.h"
#include "q_sound.h"
#include "cdaudio.h"
#include "bgmusic.h"
#include "steam.h"
#include "bc7enc.h"
#include "crc.h"
#include <stdarg.h>

extern cvar_t crosshair;
extern char crosshair_char;
extern cvar_t con_notifyfade;
extern cvar_t con_notifyfadetime;
extern cvar_t host_timescale;
extern cvar_t sv_gravity;
extern void VID_Menu_Init (void);
extern qboolean SV_BoxInPVS (vec3_t mins, vec3_t maxs, byte *pvs, mnode_t *node);

extern byte *SV_FatPVS (vec3_t org, qmodel_t *worldmodel);
extern qboolean SV_EdictInPVS (edict_t *test, byte *pvs);

static void bridge_con_vprintf (const char *fmt, va_list args)
{
	Con_Printf ("%s", va (fmt, args));
}

static void bridge_con_vdprintf (const char *fmt, va_list args)
{
	Con_DPrintf ("%s", va (fmt, args));
}

static void bridge_con_vwarning (const char *fmt, va_list args)
{
	Con_Warning ("%s", va (fmt, args));
}

static void bridge_con_vdwaring (const char *fmt, va_list args)
{
	Con_DWarning ("%s", va (fmt, args));
}

static void bridge_con_vsafe_printf (const char *fmt, va_list args)
{
	Con_SafePrintf ("%s", va (fmt, args));
}

static void bridge_con_vlink_printf (const char *addr, const char *fmt, va_list args)
{
	Con_LinkPrintf (addr, "%s", va (fmt, args));
}

static void bridge_con_add_to_tab_list (const char *name, const char *partial, const char *type)
{
	Con_AddToTabList (name, partial, type);
}

static void bridge_cvar_register (cvar_t *variable)
{
	Cvar_RegisterVariable (variable);
}

static void bridge_cvar_set_callback (cvar_t *var, void (*func)(cvar_t *))
{
	Cvar_SetCallback (var, func);
}

static void bridge_cvar_set_quick (cvar_t *var, const char *value)
{
	Cvar_SetQuick (var, value);
}

static void bridge_cvar_set_value_quick (cvar_t *var, float value)
{
	Cvar_SetValueQuick (var, value);
}

static cvar_t *bridge_cvar_find_var (const char *var_name)
{
	return Cvar_FindVar (var_name);
}

static void bridge_cvar_set_value (const char *var_name, float value)
{
	Cvar_SetValue (var_name, value);
}

static void bridge_cvar_set (const char *var_name, const char *value)
{
	Cvar_Set (var_name, value);
}

static float bridge_cvar_variable_value (const char *var_name)
{
	return Cvar_VariableValue (var_name);
}

static void *bridge_cmd_add_command (const char *name, void (*func)(void))
{
	return (void *)Cmd_AddCommand (name, func);
}

static int bridge_cmd_argc (void)
{
	return Cmd_Argc ();
}

static const char *bridge_cmd_argv (int arg)
{
	return Cmd_Argv (arg);
}

static const char *bridge_com_parse (const char *data)
{
	return COM_Parse (data);
}

static const char *bridge_com_parse_ex (const char *data, cpe_mode mode)
{
	return COM_ParseEx (data, mode);
}

static int bridge_com_check_parm (const char *parm)
{
	return COM_CheckParm (parm);
}

static void bridge_com_strip_extension (const char *in, char *out, size_t outsize)
{
	COM_StripExtension (in, out, outsize);
}

static void bridge_com_file_base (const char *in, char *out, size_t outsize)
{
	COM_FileBase (in, out, outsize);
}

static void bridge_com_add_extension (char *path, const char *extension, size_t len)
{
	COM_AddExtension (path, extension, len);
}

static const char *bridge_com_file_get_extension (const char *in)
{
	return COM_FileGetExtension (in);
}

static qboolean bridge_com_has_extension (const char *path, const char *extension)
{
	return COM_HasExtension (path, extension);
}

static char *bridge_com_tint_string (const char *in, char *out, size_t outsize)
{
	return COM_TintString (in, out, outsize);
}

static unsigned bridge_com_hash_block (const void *data, size_t size)
{
	return COM_HashBlock (data, size);
}

static byte *bridge_com_load_hunk_file (const char *path, unsigned int *path_id)
{
	return COM_LoadHunkFile (path, path_id);
}

static byte *bridge_com_load_malloc_file (const char *path, unsigned int *path_id)
{
	return COM_LoadMallocFile (path, path_id);
}

static int bridge_com_fopen_file (const char *filename, FILE **file, unsigned int *path_id)
{
	return COM_FOpenFile (filename, file, path_id);
}

static qboolean bridge_com_file_exists (const char *filename, unsigned int *path_id)
{
	return COM_FileExists (filename, path_id);
}

static const char *bridge_com_skip_path (const char *pathname)
{
	return COM_SkipPath (pathname);
}

static qboolean bridge_com_parse_line (const char **str, stringview_t *line)
{
	return COM_ParseLine (str, line);
}

static char *bridge_com_tint_substring (const char *in, const char *substr, char *out, size_t outsize)
{
	return COM_TintSubstring (in, substr, out, outsize);
}

static char *bridge_va_list_fn (const char *format, va_list args)
{
	return va (format, args);
}

static int bridge_q_snprintf (char *str, size_t size, const char *format, ...)
{
	va_list args;
	va_start (args, format);
	int ret = q_vsnprintf (str, size, format, args);
	va_end (args);
	return ret;
}

static int bridge_q_vsnprintf (char *str, size_t size, const char *format, va_list args)
{
	return q_vsnprintf (str, size, format, args);
}

static int bridge_q_atoi (const char *str)
{
	return Q_atoi (str);
}

static float bridge_q_atof (const char *str)
{
	return Q_atof (str);
}

static size_t bridge_q_strlcpy (char *dst, const char *src, size_t size)
{
	return q_strlcpy (dst, src, size);
}

static size_t bridge_q_strlcat (char *dst, const char *src, size_t size)
{
	return q_strlcat (dst, src, size);
}

static int bridge_q_strcasecmp (const char *s1, const char *s2)
{
	return q_strcasecmp (s1, s2);
}

static int bridge_q_strncasecmp (const char *s1, const char *s2, size_t n)
{
	return q_strncasecmp (s1, s2, n);
}

static FUNC_NORETURN void bridge_sys_verror (const char *error, va_list args)
{
	Sys_Error ("%s", va (error, args));
}

static void bridge_sys_vprintf (const char *fmt, va_list args)
{
	Sys_Printf ("%s", va (fmt, args));
}

static double bridge_sys_double_time (void)
{
	return Sys_DoubleTime ();
}

static void bridge_sys_send_key_events (void)
{
	Sys_SendKeyEvents ();
}

static void bridge_sys_sleep (unsigned long msecs)
{
	Sys_Sleep (msecs);
}

static FILE *bridge_sys_fopen (const char *path, const char *mode)
{
	return Sys_fopen (path, mode);
}

static qboolean bridge_sys_get_file_time (const char *path, time_t *out)
{
	return Sys_GetFileTime (path, out);
}

static int bridge_sys_file_type (const char *path)
{
	return Sys_FileType (path);
}

static long bridge_sys_ftell (FILE *file)
{
	return (long)Sys_ftell (file);
}

static findfile_t *bridge_sys_find_first (const char *dir, const char *ext)
{
	return Sys_FindFirst (dir, ext);
}

static findfile_t *bridge_sys_find_next (findfile_t *find)
{
	return Sys_FindNext (find);
}

static void bridge_sys_find_close (findfile_t *find)
{
	(void)find;
}

static void *bridge_sys_load_library (const char *path)
{
	return Sys_LoadLibrary (path);
}

static void *bridge_sys_get_library_function (void *lib, const char *name)
{
	return Sys_GetLibraryFunction (lib, name);
}

static void bridge_sys_close_library (void *lib)
{
	Sys_CloseLibrary (lib);
}

static qboolean bridge_sys_is_debugger_present (void)
{
	return Sys_IsDebuggerPresent ();
}

static void bridge_mod_init (void) { Mod_Init (); }
static void bridge_mod_clear_all (void) { Mod_ClearAll (); }
static void bridge_mod_reset_all (void) { Mod_ResetAll (); }
static qmodel_t *bridge_mod_for_name (const char *name, qboolean crash) { return Mod_ForName (name, crash); }
static void *bridge_mod_extradata (qmodel_t *mod) { return Mod_Extradata (mod); }
static void bridge_mod_touch_model (const char *name) { Mod_TouchModel (name); }
static mleaf_t *bridge_mod_point_in_leaf (vec3_t p, qmodel_t *model) { return Mod_PointInLeaf (p, model); }
static byte *bridge_mod_leaf_pvs (mleaf_t *leaf, qmodel_t *model) { return Mod_LeafPVS (leaf, model); }
static byte *bridge_mod_no_vis_pvs (qmodel_t *model) { return Mod_NoVisPVS (model); }
static void bridge_mod_set_extra_flags (qmodel_t *mod) { Mod_SetExtraFlags (mod); }
static qboolean bridge_mod_is_known_model (const qmodel_t *mod) { return Mod_IsKnownModel (mod); }

static void *bridge_hunk_alloc (int size) { return Hunk_Alloc (size); }
static void *bridge_hunk_alloc_name (int size, const char *name) { return Hunk_AllocName (size, name); }
static int bridge_hunk_low_mark (void) { return Hunk_LowMark (); }
static void bridge_hunk_free_to_low_mark (int mark) { Hunk_FreeToLowMark (mark); }
static void *bridge_cache_check (cache_user_t *c) { return Cache_Check (c); }
static void bridge_cache_free (cache_user_t *c, qboolean freetextures) { Cache_Free (c, freetextures); }
static void *bridge_cache_alloc (cache_user_t *c, int size, const char *name) { return Cache_Alloc (c, size, name); }
static void *bridge_hunk_alloc_no_fill (int size) { return Hunk_AllocNoFill (size); }
static void *bridge_hunk_alloc_name_no_fill (int size, const char *name) { return Hunk_AllocNameNoFill (size, name); }

static void *bridge_w_get_lump_name (const char *name, lumpinfo_t **out_info) { return W_GetLumpName (name, out_info); }

static void bridge_pr_switch_qcvm (qcvm_t *nvm) { PR_SwitchQCVM (nvm); }
static void bridge_pr_push_qcvm (qcvm_t *newvm, qcvm_t **oldvm) { PR_PushQCVM (newvm, oldvm); }
static void bridge_pr_pop_qcvm (qcvm_t *oldvm) { PR_PopQCVM (oldvm); }

static byte *bridge_sv_fat_pvs (vec3_t org, qmodel_t *worldmodel) { return SV_FatPVS (org, worldmodel); }
static qboolean bridge_sv_edict_in_pvs (edict_t *test, byte *pvs) { return SV_EdictInPVS (test, pvs); }

static qboolean bridge_cl_in_cutscene (void) { return CL_InCutscene (); }
static qboolean bridge_cl_is_player_ent (const entity_t *ent) { return CL_IsPlayerEnt (ent); }

static int bridge_msg_read_byte (void) { return MSG_ReadByte (); }
static int bridge_msg_read_short (void) { return MSG_ReadShort (); }

static void bridge_m_draw (void) { M_Draw (); }
static qboolean bridge_m_wants_console (float *alpha) { return M_WantsConsole (alpha); }
static qboolean bridge_m_forced_center_print (float *alpha) { return M_ForcedCenterPrint (alpha); }
static qboolean bridge_m_forced_underwater (void) { return M_ForcedUnderwater (); }
static void bridge_m_draw_text_box (int x, int y, int width, int lines) { M_DrawTextBox (x, y, width, lines); }
static void bridge_v_polyblend (void) { V_PolyBlend (); }

static FUNC_NORETURN void bridge_host_verror (const char *error, va_list args)
{
	Host_Error ("%s", va (error, args));
}

static qboolean bridge_image_write_tga (const char *name, byte *data, int width, int height, int bpp, qboolean upsidedown)
{
	return Image_WriteTGA (name, data, width, height, bpp, upsidedown);
}

static qboolean bridge_image_write_png (const char *name, byte *data, int width, int height, int bpp, qboolean upsidedown)
{
	return Image_WritePNG (name, data, width, height, bpp, upsidedown);
}

static qboolean bridge_image_write_jpg (const char *name, byte *data, int width, int height, int bpp, int quality, qboolean upsidedown)
{
	return Image_WriteJPG (name, data, width, height, bpp, quality, upsidedown);
}

static void *bridge_vid_get_window (void) { return VID_GetWindow (); }
static qboolean bridge_vid_has_mouse_focus (void) { return VID_HasMouseOrInputFocus (); }
static qboolean bridge_vid_is_minimized (void) { return VID_IsMinimized (); }
static void bridge_vid_set_window_title (const char *title) { VID_SetWindowTitle (title); }
static void bridge_vid_recalc_console_size (void) { VID_RecalcConsoleSize (); }
static void bridge_vid_recalc_interface_size (void) { VID_RecalcInterfaceSize (); }
static void bridge_vid_lock (void) { VID_Lock (); }

static void bridge_scr_center_print (const char *str) { SCR_CenterPrint (str); }
static int bridge_scr_modal_message (const char *text, float timeout) { return SCR_ModalMessage (text, timeout); }

static void bridge_cvar_set_completion (cvar_t *var, void (*func)(cvar_t *, const char *))
{
	Cvar_SetCompletion (var, (cvarcompletion_t)func);
}

static void bridge_cbuf_add_text (const char *text)
{
	Cbuf_AddText (text);
}

static void bridge_vector_ma (const vec3_t veca, float scale, const vec3_t vecb, vec3_t vecc) { VectorMA (veca, scale, vecb, vecc); }
static float bridge_vector_normalize (vec3_t v) { return VectorNormalize (v); }
static int bridge_vector_compare (const vec3_t v1, const vec3_t v2) { return VectorCompare (v1, v2); }
static vec_t bridge_vector_length (const vec3_t v) { return VectorLength (v); }
static void bridge_vector_lerp (const vec3_t veca, const vec3_t vecb, float frac, vec3_t dst) { VectorLerp (veca, vecb, frac, dst); }
static void bridge_cross_product (const vec3_t v1, const vec3_t v2, vec3_t cross) { CrossProduct (v1, v2, cross); }
static void bridge_angle_vectors (vec3_t angles, vec3_t forward, vec3_t right, vec3_t up) { AngleVectors (angles, forward, right, up); }
static float bridge_distance_fn (const vec3_t a, const vec3_t b) { return Distance (a, b); }
static void bridge_vector_scale (const vec3_t in, vec_t scale, vec3_t out) { VectorScale (in, scale, out); }
static void bridge_vector_inverse (vec3_t v) { VectorInverse (v); }
static void bridge_project_vector (const vec3_t src, const float matrix[16], vec3_t dst) { ProjectVector (src, matrix, dst); }
static void bridge_matrix_multiply (float left[16], float right[16]) { MatrixMultiply (left, right); }
static void bridge_rotation_matrix (float matrix[16], float angle, int axis) { RotationMatrix (matrix, angle, axis); }
static void bridge_translation_matrix (float matrix[16], float x, float y, float z) { TranslationMatrix (matrix, x, y, z); }
static void bridge_matrix_transpose_4x3 (const float src[16], float dst[12]) { MatrixTranspose4x3 (src, dst); }
static qboolean bridge_mat4_inverse (const float in[16], float out[16]) { return Mat4_Inverse (in, out); }
static void bridge_r_concat_transforms (float in1[3][4], float in2[3][4], float out[3][4]) { R_ConcatTransforms (in1, in2, out); }
static void bridge_apply_translation (float matrix[16], float x, float y, float z) { ApplyTranslation (matrix, x, y, z); }
static void bridge_apply_scale (float matrix[16], float x, float y, float z) { ApplyScale (matrix, x, y, z); }
static uint32_t bridge_interleave (uint16_t even, uint16_t odd) { return Interleave (even, odd); }
static qboolean bridge_ray_vs_box (const vec3_t org, const vec3_t rcpdelta, const vec3_t mins, const vec3_t maxs, float *frac) { return RayVsBox (org, rcpdelta, mins, maxs, frac); }
static int bridge_box_on_plane_side (vec3_t emins, vec3_t emaxs, mplane_t *plane) { return BoxOnPlaneSide (emins, emaxs, plane); }
static int bridge_q_next_pow2 (int val) { return Q_nextPow2 (val); }
static int bridge_q_log2 (int val) { return Q_log2 (val); }
static float bridge_get_fraction (float val, float minval, float maxval) { return GetFraction (val, minval, maxval); }
static unsigned short bridge_crc_block (const void *start, int count) { return CRC_Block (start, count); }
static void bridge_swap_pic (void *pic) { SwapPic ((qpic_t *)pic); }

static int bridge_q_strcmp (const char *s1, const char *s2) { return Q_strcmp (s1, s2); }
static int bridge_q_strncmp (const char *s1, const char *s2, int count) { return Q_strncmp (s1, s2, count); }
static void bridge_q_strncpy (char *dest, const char *src, int count) { Q_strncpy (dest, src, count); }
static int bridge_q_strlen (const char *str) { return Q_strlen (str); }
static char *bridge_q_strcasestr (const char *haystack, const char *needle) { return q_strcasestr (haystack, needle); }
static void *bridge_q_malloc_fn (size_t size) { return q_malloc (size); }
static void bridge_q_free_fn (void *ptr) { q_free (ptr); }
static void *bridge_q_realloc_fn (void *ptr, size_t size) { return q_realloc (ptr, size); }
static void *bridge_q_calloc_fn (size_t count, size_t size) { return q_calloc (count, size); }

static void bridge_r_backend_register (const void *backend) { R_Backend_Register ((const IRenderBackend *)backend); }
static void bridge_r_backend_init (void) { R_Backend_Init (); }
static void bridge_r_backend_shutdown (void) { R_Backend_Shutdown (); }
static void bridge_r_backend_memory_barrier (unsigned barrier_bits) { R_Backend_MemoryBarrier (barrier_bits); }
static void bridge_r_backend_set_dynamic_state (const void *ds) { R_Backend_SetDynamicState ((const RenderBackendDynamicState *)ds); }
static void bridge_r_backend_draw_indexed (int primitive, int index_type, int count, intptr_t offset) { R_Backend_DrawIndexed ((render_backend_primitive_t)primitive, (render_backend_index_type_t)index_type, count, offset); }
static void bridge_r_backend_bind_pipeline (const void *pipeline) { R_Backend_BindPipeline ((const RenderBackendPipelineDesc *)pipeline); }
static void bridge_r_backend_set_viewport (int x, int y, int w, int h) { R_Backend_SetViewport (x, y, w, h); }
static void bridge_r_backend_finish (void) { R_Backend_Finish (); }
static void bridge_r_backend_draw_fn (int primitive, int first, int count) { R_Backend_Draw ((render_backend_primitive_t)primitive, first, count); }
static void bridge_r_backend_dispatch (unsigned gx, unsigned gy, unsigned gz) { R_Backend_Dispatch (gx, gy, gz); }
static void bridge_r_backend_present (void) { R_Backend_Present (); }
static void bridge_r_backend_begin_frame (void) { R_Backend_BeginFrame (); }
static void bridge_r_backend_end_frame (void) { R_Backend_EndFrame (); }
static void bridge_r_backend_on_resize (int w, int h) { R_Backend_OnResize (w, h); }
static int bridge_r_backend_get_scene_sample_count (void) { return R_Backend_GetSceneSampleCount (); }
static void bridge_r_backend_draw_indexed_indirect (int prim, int idx, intptr_t off) { R_Backend_DrawIndexedIndirect ((render_backend_primitive_t)prim, (render_backend_index_type_t)idx, off); }
static void bridge_r_backend_multi_draw_indexed_indirect (int prim, int idx, intptr_t off, int dc, int stride) { R_Backend_MultiDrawIndexedIndirect ((render_backend_primitive_t)prim, (render_backend_index_type_t)idx, off, dc, stride); }
static void bridge_r_backend_draw_indexed_instanced (int prim, int idx, int count, intptr_t off, int ic) { R_Backend_DrawIndexedInstanced ((render_backend_primitive_t)prim, (render_backend_index_type_t)idx, count, off, ic); }
static void bridge_r_backend_draw_instanced (int prim, int first, int count, int ic) { R_Backend_DrawInstanced ((render_backend_primitive_t)prim, first, count, ic); }
static void bridge_r_backend_draw_packet (const void *packet) { R_Backend_DrawPacket ((const RenderBackendDrawPacket *)packet); }
static void bridge_r_backend_bind_descriptors (const void *bindings, unsigned count) { R_Backend_BindDescriptors ((const RenderBackendDescriptorBinding *)bindings, count); }
static void bridge_r_backend_set_blend_factors (int src, int dst) { R_Backend_SetBlendFactors ((render_blend_factor_t)src, (render_blend_factor_t)dst); }
static void bridge_r_backend_set_depth_func (int df) { R_Backend_SetDepthFunc ((render_backend_depth_func_t)df); }
static const void *bridge_r_backend_get_caps (void) { return R_Backend_GetCaps (); }
static void bridge_r_backend_configure_postfx_lut_texture (unsigned tid) { R_Backend_ConfigurePostFXLUTTexture (tid); }
static unsigned bridge_r_backend_create_postfx_lut_texture (void) { return R_Backend_CreatePostFXLUTTexture (); }

static void bridge_sbar_changed (void) { Sbar_Changed (); }
static void bridge_sbar_draw (void) { Sbar_Draw (); }
static void bridge_sbar_load_pics (void) { Sbar_LoadPics (); }
static void bridge_sbar_intermission_overlay (void) { Sbar_IntermissionOverlay (); }
static void bridge_sbar_finale_overlay (void) { Sbar_FinaleOverlay (); }
static void bridge_con_clear_notify (void) { Con_ClearNotify (); }
static void bridge_con_draw_console (int lines, qboolean drawbg, qboolean drawinput) { Con_DrawConsole (lines, drawbg, drawinput); }
static void bridge_con_draw_notify (void) { Con_DrawNotify (); }
static void bridge_con_check_resize (void) { Con_CheckResize (); }
static void bridge_con_vdprintf2 (const char *fmt, va_list args) { Con_DPrintf2 ("%s", va (fmt, args)); }
static void bridge_key_get_grabbed_input (int *lastkey, int *lastchar) { Key_GetGrabbedInput (lastkey, lastchar); }
static void bridge_key_begin_input_grab (void) { Key_BeginInputGrab (); }
static void bridge_key_end_input_grab (void) { Key_EndInputGrab (); }
static void bridge_key_clear_states (void) { Key_ClearStates (); }
static void bridge_in_deactivate_for_console (void) { IN_DeactivateForConsole (); }
static void bridge_in_deactivate_for_menu (void) { IN_DeactivateForMenu (); }
static void bridge_in_clear_states (void) { IN_ClearStates (); }
static void bridge_m_print (int cx, int cy, const char *str) { M_Print (cx, cy, str); }
static void bridge_v_render_view (void) { V_RenderView (); }
static void bridge_v_calc_blend (void) { V_CalcBlend (); }
static void bridge_v_update_blend (void) { V_UpdateBlend (); }
static void bridge_v_set_contents_color (int contents) { V_SetContentsColor (contents); }
static void bridge_cl_postfx_set_contents (int contents, qboolean ua, qboolean upa) { CL_PostFX_SetContents (contents, ua, upa); }
static void bridge_cl_postfx_get_state (void *out) { CL_PostFX_GetState ((postfx_state_t *)out); }
static qboolean bridge_ed_is_relevant_field (edict_t *ed, void *d) { return ED_IsRelevantField (ed, (ddef_t *)d); }
static const char *bridge_ed_field_value_string (edict_t *ed, void *d) { return ED_FieldValueString (ed, (ddef_t *)d); }
static const char *bridge_pr_get_string (int num) { return PR_GetString (num); }
static int bridge_num_for_edict (edict_t *e) { return NUM_FOR_EDICT (e); }
static void bridge_pr_reload_pics (qboolean purge) { PR_ReloadPics (purge); }
static void bridge_w_load_wad_file (void) { W_LoadWadFile (); }
static void *bridge_z_malloc (int size) { return Z_Malloc (size); }
static void bridge_z_free_fn (void *ptr) { Z_Free (ptr); }
static FUNC_NORETURN void bridge_sys_report_verror (const char *error, va_list args) { Sys_ReportError ("%s", va (error, args)); }
static FUNC_NORETURN void bridge_host_report_verror (const char *error, va_list args) { Host_ReportError ("%s", va (error, args)); }
static void bridge_host_begin_asset_loading (void) { Host_BeginAssetLoading (); }
static void bridge_host_end_asset_loading (void) { Host_EndAssetLoading (); }
static qboolean bridge_host_is_saving (void) { return Host_IsSaving (); }
static int bridge_cfg_open_config (const char *name) { return CFG_OpenConfig (name); }
static void bridge_cfg_close_config (void) { CFG_CloseConfig (); }
static void bridge_cfg_read_cvars (const char **vars, int n) { CFG_ReadCvars (vars, n); }
static void bridge_cfg_read_cvar_overrides (const char **vars, int n) { CFG_ReadCvarOverrides (vars, n); }
static void *bridge_cmd_add_command2 (const char *name, void (*fn)(void), int src, qboolean qi) { return (void *)Cmd_AddCommand2 (name, (xcommand_t)fn, (cmd_source_t)src, qi); }
static void bridge_pl_set_window_icon (void) { PL_SetWindowIcon (); }
static void bridge_pl_vid_shutdown (void) { PL_VID_Shutdown (); }
static void bridge_vid_menu_init (void) { VID_Menu_Init (); }
static byte *bridge_image_load_image (const char *name, int *w, int *h, int *fmt) { return Image_LoadImage (name, w, h, (enum srcformat *)fmt); }
static qboolean bridge_steam_save_screenshot (const void *rgb, int w, int h) { return Steam_SaveScreenshot (rgb, w, h); }
static void bridge_s_extra_update (void) { S_ExtraUpdate (); }
static void bridge_s_clear_buffer (void) { S_ClearBuffer (); }
static void bridge_s_stop_all_sounds (qboolean clear) { S_StopAllSounds (clear); }
static void bridge_cdaudio_pause (void) { CDAudio_Pause (); }
static void bridge_cdaudio_resume (void) { CDAudio_Resume (); }
static void bridge_bgm_pause (void) { BGM_Pause (); }
static void bridge_bgm_resume (void) { BGM_Resume (); }
static size_t bridge_utf8_from_quake (char *dst, size_t mb, const char *src) { return UTF8_FromQuake (dst, mb, src); }
static size_t bridge_utf8_to_quake (char *dst, size_t mb, const char *src) { return UTF8_ToQuake (dst, mb, src); }
static void bridge_multi_string_append (char **pv, const char *s) { MultiString_Append (pv, s); }
static void bridge_multi_string_append_n (char **pv, const char *s, size_t len) { MultiString_AppendN (pv, s, len); }
static float bridge_msg_read_coord (unsigned int flags) { return MSG_ReadCoord (flags); }
static int bridge_msg_read_char (void) { return MSG_ReadChar (); }
static trace_t bridge_sv_move (vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end, int type, edict_t *pe) { return SV_Move (start, mins, maxs, end, type, pe); }
static qboolean bridge_sv_recursive_hull_check (const hull_t *hull, int num, float p1f, float p2f, vec3_t p1, vec3_t p2, trace_t *trace) { return SV_RecursiveHullCheck (hull, num, p1f, p2f, p1, p2, trace); }
static qboolean bridge_sv_box_in_pvs (vec3_t mins, vec3_t maxs, byte *pvs, mnode_t *node) { return SV_BoxInPVS (mins, maxs, pvs, node); }

static void bridge_material_init (void) { Material_Init (); }
static void bridge_material_shutdown (void) { Material_Shutdown (); }
static void bridge_material_apply_to_texture (texture_t *tex, const char *mapname) { Material_ApplyToTexture (tex, mapname); }
static const material_t *bridge_material_find (const char *name) { return Material_Find (name); }
static const material_t *bridge_material_find_for_texture_name (const char *tn, const char *mn) { return Material_FindForTextureName (tn, mn); }
static void bridge_material_canonicalize (const char *name, char *out, size_t sz) { Material_Canonicalize (name, out, sz); }
static int bridge_material_classify_particle_stage (const void *s, int policy, char *reason, size_t rsz) { return (int)Material_ClassifyParticleStage ((const material_stage_t *)s, (mat_particle_policy_t)policy, reason, rsz); }
static qboolean bridge_material_stage_supports_particle_mvp (const void *s, char *reason, size_t rsz) { return Material_StageSupportsParticleMVP ((const material_stage_t *)s, reason, rsz); }
static const mat_texmatrix_t *bridge_material_stage_eval_tex_matrix (void *s, float time) { return MaterialStage_EvalTexMatrix ((material_stage_t *)s, time); }
static const char *bridge_material_stage_get_anim_map_path (void *s, float time) { return MaterialStage_GetAnimMapPath ((material_stage_t *)s, time); }
static float bridge_material_eval_wave_value (const mat_wave_t *w, float time) { return Material_EvalWaveValue (w, time); }

static void bridge_dlight_pool_clear_persistent (void) { DLightPool_ClearPersistent (); }
static int bridge_dlight_pool_collect_for_render (double time, const vec3_t vo, const mleaf_t *vl, dlight_t **out, int mx) { return DLightPool_CollectForRender (time, vo, vl, out, mx); }
static const dlight_t *const *bridge_dlight_pool_get_active_list (int *count) { return DLightPool_GetActiveList (count); }
static int bridge_dlight_pool_get_budget (void) { return DLightPool_GetBudget (); }
static dlight_t *bridge_dlight_pool_get_or_create_persistent (int key, double time) { return DLightPool_GetOrCreatePersistent (key, time); }
static void bridge_dlight_pool_new_frame (double time, int fc) { DLightPool_NewFrame (time, fc); }

static void bridge_r_ppdlights_collect_frame (void) { R_PPdlights_CollectFrame (); }
static int bridge_r_ppdlights_build_model_gpu_lights (gpulightbuffer_t *ob, dlight_t **os, int ml) { return R_PPdlights_BuildModelGpuLights (ob, os, ml); }
static int bridge_r_ppdlights_build_world_gpu_lights (gpulightbuffer_t *ob, dlight_t **os, int ml) { return R_PPdlights_BuildWorldGpuLights (ob, os, ml); }
static const rl_light_t *bridge_r_ppdlights_get_frame_lights (int *oc) { return R_PPdlights_GetFrameLights (oc); }

static float bridge_r_skyvis_get_resolved_cap (void) { return R_SkyVis_GetResolvedCap (); }
static void bridge_r_skyvis_get_tint (vec3_t out) { R_SkyVis_GetTint (out); }
static float bridge_r_skyvis_get_resolved_scale (void) { return R_SkyVis_GetResolvedScale (); }
static qboolean bridge_r_skyvis_active (void) { return R_SkyVis_Active (); }
static void bridge_r_skyvis_init (void) { R_SkyVis_Init (); }
static void bridge_r_skyvis_new_map (void) { R_SkyVis_NewMap (); }
static float bridge_r_skyvis_sample (const vec3_t pos) { return R_SkyVis_Sample (pos); }

static float bridge_r_ssao_sanitize_value (float v, float fb, float mn, float mx) { return R_SSAO_SanitizeValue (v, fb, mn, mx); }
static void bridge_r_ssao_capture_fog_state (const gpuframedata_t *fd, r_ssao_fog_state_t *os) { R_SSAO_CaptureFogState (fd, os); }
static void bridge_r_ssao_register_cvars (void) { R_SSAO_RegisterCvars (); }

static void bridge_r_quality_init (void) { R_Quality_Init (); }
static void bridge_r_quality_update (void) { R_Quality_Update (); }

static void bridge_r_framegraph_render_view (void) { R_FrameGraph_RenderView (); }
static void bridge_r_framegraph_get_timing_summary (double *og, double *oc, qboolean *ov) { R_FrameGraph_GetTimingSummary (og, oc, ov); }
static void bridge_r_framegraph_set_render_frame_plan (const void *plan) { R_FrameGraph_SetRenderFramePlan ((const RenderFramePlan *)plan); }
static unsigned bridge_r_framegraph_resolve_required_resource_by_slot (const void *res, int slot, const char *tag) { return R_FrameGraph_ResolveRequiredResourceBySlot ((const RenderGraphResourceHandle *)res, (render_backend_resource_slot_t)slot, tag); }
static qboolean bridge_r_framegraph_get_render_frame_plan (void *out) { return R_FrameGraph_GetRenderFramePlan ((RenderFramePlan *)out); }
static qboolean bridge_r_framegraph_add_pass (const void *pd) { return R_FrameGraph_AddPass ((const RenderPassDesc *)pd); }

static float bridge_r_tonemap_tempered_overbright (float ob) { return R_Tonemap_TemperedOverbright (ob); }

static void bridge_bc7enc_compress_block_init (void) { bc7enc_compress_block_init (); }
static int bridge_bc7enc_compress_block_fn (void *pb, const void *pp, const void *pcp) { return (int)bc7enc_compress_block (pb, pp, (const bc7enc_compress_block_params *)pcp); }

static void bridge_lightgrid_free (lightgrid_t *lg) { Lightgrid_Free (lg); }

static void bridge_vec_grow (void **pv, size_t es, size_t c) { Vec_Grow (pv, es, c); }
static void bridge_vec_append (void **pv, size_t es, const void *d, size_t c) { Vec_Append (pv, es, d, c); }
static void bridge_vec_clear (void **pv) { Vec_Clear (pv); }
static void bridge_vec_free (void **pv) { Vec_Free (pv); }

static const iw_renderer_host_bridge_functions_t s_bridge_functions = {
	sizeof (iw_renderer_host_bridge_functions_t),
	bridge_con_vprintf,
	bridge_con_vdprintf,
	bridge_con_vwarning,
	bridge_con_vdwaring,
	bridge_con_vsafe_printf,
	bridge_con_vlink_printf,
	bridge_con_add_to_tab_list,
	bridge_cvar_register,
	bridge_cvar_set_callback,
	bridge_cvar_set_quick,
	bridge_cvar_set_value_quick,
	bridge_cvar_find_var,
	bridge_cvar_set_value,
	bridge_cvar_set,
	bridge_cvar_variable_value,
	bridge_cmd_add_command,
	bridge_cmd_argc,
	bridge_cmd_argv,
	bridge_com_parse,
	bridge_com_parse_ex,
	bridge_com_check_parm,
	bridge_com_strip_extension,
	bridge_com_file_base,
	bridge_com_add_extension,
	bridge_com_file_get_extension,
	bridge_com_has_extension,
	bridge_com_tint_string,
	bridge_com_hash_block,
	bridge_com_load_hunk_file,
	bridge_com_load_malloc_file,
	bridge_com_fopen_file,
	bridge_com_file_exists,
	bridge_com_skip_path,
	bridge_com_parse_line,
	bridge_com_tint_substring,
	bridge_va_list_fn,
	bridge_q_snprintf,
	bridge_q_vsnprintf,
	bridge_q_atoi,
	bridge_q_atof,
	bridge_q_strlcpy,
	bridge_q_strlcat,
	bridge_q_strcasecmp,
	bridge_q_strncasecmp,
	bridge_sys_verror,
	bridge_sys_vprintf,
	bridge_sys_double_time,
	bridge_sys_send_key_events,
	bridge_sys_sleep,
	bridge_sys_fopen,
	bridge_sys_get_file_time,
	bridge_sys_file_type,
	bridge_sys_ftell,
	bridge_sys_find_first,
	bridge_sys_find_next,
	bridge_sys_find_close,
	bridge_mod_init,
	bridge_mod_clear_all,
	bridge_mod_reset_all,
	bridge_mod_for_name,
	bridge_mod_extradata,
	bridge_mod_touch_model,
	bridge_mod_point_in_leaf,
	bridge_mod_leaf_pvs,
	bridge_mod_no_vis_pvs,
	bridge_mod_set_extra_flags,
	bridge_mod_is_known_model,
	bridge_hunk_alloc,
	bridge_hunk_alloc_name,
	bridge_hunk_low_mark,
	bridge_hunk_free_to_low_mark,
	bridge_cache_check,
	bridge_cache_free,
	bridge_cache_alloc,
	bridge_w_get_lump_name,
	bridge_pr_switch_qcvm,
	bridge_pr_push_qcvm,
	bridge_pr_pop_qcvm,
	bridge_sv_fat_pvs,
	bridge_sv_edict_in_pvs,
	bridge_cl_in_cutscene,
	bridge_cl_is_player_ent,
	bridge_msg_read_byte,
	bridge_msg_read_short,
	bridge_m_draw,
	bridge_m_wants_console,
	bridge_m_forced_center_print,
	bridge_m_forced_underwater,
	bridge_m_draw_text_box,
	bridge_v_polyblend,
	bridge_host_verror,
	bridge_image_write_tga,
	bridge_image_write_png,
	bridge_image_write_jpg,
	bridge_vid_get_window,
	bridge_vid_has_mouse_focus,
	bridge_vid_is_minimized,
	bridge_vid_set_window_title,
	bridge_vid_recalc_console_size,
	bridge_vid_recalc_interface_size,
	bridge_vid_lock,
	bridge_scr_center_print,
	bridge_scr_modal_message,
	bridge_sys_load_library,
	bridge_sys_get_library_function,
	bridge_sys_close_library,
	bridge_sys_is_debugger_present,
	bridge_cvar_set_completion,
	bridge_hunk_alloc_no_fill,
	bridge_hunk_alloc_name_no_fill,
	bridge_cbuf_add_text,
	bridge_vector_ma,
	bridge_vector_normalize,
	bridge_vector_compare,
	bridge_vector_length,
	bridge_vector_lerp,
	bridge_cross_product,
	bridge_angle_vectors,
	bridge_distance_fn,
	bridge_vector_scale,
	bridge_vector_inverse,
	bridge_project_vector,
	bridge_matrix_multiply,
	bridge_rotation_matrix,
	bridge_translation_matrix,
	bridge_matrix_transpose_4x3,
	bridge_mat4_inverse,
	bridge_r_concat_transforms,
	bridge_apply_translation,
	bridge_apply_scale,
	bridge_interleave,
	bridge_ray_vs_box,
	bridge_box_on_plane_side,
	bridge_q_next_pow2,
	bridge_q_log2,
	bridge_get_fraction,
	bridge_crc_block,
	bridge_swap_pic,
	bridge_q_strcmp,
	bridge_q_strncmp,
	bridge_q_strncpy,
	bridge_q_strlen,
	bridge_q_strcasestr,
	bridge_q_malloc_fn,
	bridge_q_free_fn,
	bridge_q_realloc_fn,
	bridge_q_calloc_fn,
	bridge_r_backend_register,
	bridge_r_backend_init,
	bridge_r_backend_shutdown,
	bridge_r_backend_memory_barrier,
	bridge_r_backend_set_dynamic_state,
	bridge_r_backend_draw_indexed,
	bridge_r_backend_bind_pipeline,
	bridge_r_backend_set_viewport,
	bridge_r_backend_finish,
	bridge_r_backend_draw_fn,
	bridge_r_backend_dispatch,
	bridge_r_backend_present,
	bridge_r_backend_begin_frame,
	bridge_r_backend_end_frame,
	bridge_r_backend_on_resize,
	bridge_r_backend_get_scene_sample_count,
	bridge_r_backend_draw_indexed_indirect,
	bridge_r_backend_multi_draw_indexed_indirect,
	bridge_r_backend_draw_indexed_instanced,
	bridge_r_backend_draw_instanced,
	bridge_r_backend_draw_packet,
	bridge_r_backend_bind_descriptors,
	bridge_r_backend_set_blend_factors,
	bridge_r_backend_set_depth_func,
	bridge_r_backend_get_caps,
	bridge_r_backend_configure_postfx_lut_texture,
	bridge_r_backend_create_postfx_lut_texture,
	bridge_sbar_changed,
	bridge_sbar_draw,
	bridge_sbar_load_pics,
	bridge_sbar_intermission_overlay,
	bridge_sbar_finale_overlay,
	bridge_con_clear_notify,
	bridge_con_draw_console,
	bridge_con_draw_notify,
	bridge_con_check_resize,
	bridge_con_vdprintf2,
	bridge_key_get_grabbed_input,
	bridge_key_begin_input_grab,
	bridge_key_end_input_grab,
	bridge_key_clear_states,
	bridge_in_deactivate_for_console,
	bridge_in_deactivate_for_menu,
	bridge_in_clear_states,
	bridge_m_print,
	bridge_v_render_view,
	bridge_v_calc_blend,
	bridge_v_update_blend,
	bridge_v_set_contents_color,
	bridge_cl_postfx_set_contents,
	bridge_cl_postfx_get_state,
	bridge_ed_is_relevant_field,
	bridge_ed_field_value_string,
	bridge_pr_get_string,
	bridge_num_for_edict,
	bridge_pr_reload_pics,
	bridge_w_load_wad_file,
	bridge_z_malloc,
	bridge_z_free_fn,
	bridge_sys_report_verror,
	bridge_host_report_verror,
	bridge_host_begin_asset_loading,
	bridge_host_end_asset_loading,
	bridge_host_is_saving,
	bridge_cfg_open_config,
	bridge_cfg_close_config,
	bridge_cfg_read_cvars,
	bridge_cfg_read_cvar_overrides,
	bridge_cmd_add_command2,
	bridge_pl_set_window_icon,
	bridge_pl_vid_shutdown,
	bridge_vid_menu_init,
	bridge_image_load_image,
	bridge_steam_save_screenshot,
	bridge_s_extra_update,
	bridge_s_clear_buffer,
	bridge_s_stop_all_sounds,
	bridge_cdaudio_pause,
	bridge_cdaudio_resume,
	bridge_bgm_pause,
	bridge_bgm_resume,
	bridge_utf8_from_quake,
	bridge_utf8_to_quake,
	bridge_multi_string_append,
	bridge_multi_string_append_n,
	bridge_msg_read_coord,
	bridge_msg_read_char,
	bridge_sv_move,
	bridge_sv_recursive_hull_check,
	bridge_sv_box_in_pvs,
	bridge_material_init,
	bridge_material_shutdown,
	bridge_material_apply_to_texture,
	bridge_material_find,
	bridge_material_find_for_texture_name,
	bridge_material_canonicalize,
	bridge_material_classify_particle_stage,
	bridge_material_stage_supports_particle_mvp,
	bridge_material_stage_eval_tex_matrix,
	bridge_material_stage_get_anim_map_path,
	bridge_material_eval_wave_value,
	bridge_dlight_pool_clear_persistent,
	bridge_dlight_pool_collect_for_render,
	bridge_dlight_pool_get_active_list,
	bridge_dlight_pool_get_budget,
	bridge_dlight_pool_get_or_create_persistent,
	bridge_dlight_pool_new_frame,
	bridge_r_ppdlights_collect_frame,
	bridge_r_ppdlights_build_model_gpu_lights,
	bridge_r_ppdlights_build_world_gpu_lights,
	bridge_r_ppdlights_get_frame_lights,
	bridge_r_skyvis_get_resolved_cap,
	bridge_r_skyvis_get_tint,
	bridge_r_skyvis_get_resolved_scale,
	bridge_r_skyvis_active,
	bridge_r_skyvis_init,
	bridge_r_skyvis_new_map,
	bridge_r_skyvis_sample,
	bridge_r_ssao_sanitize_value,
	bridge_r_ssao_capture_fog_state,
	bridge_r_ssao_register_cvars,
	bridge_r_quality_init,
	bridge_r_quality_update,
	bridge_r_framegraph_render_view,
	bridge_r_framegraph_get_timing_summary,
	bridge_r_framegraph_set_render_frame_plan,
	bridge_r_framegraph_resolve_required_resource_by_slot,
	bridge_r_framegraph_get_render_frame_plan,
	bridge_r_framegraph_add_pass,
	bridge_r_tonemap_tempered_overbright,
	bridge_bc7enc_compress_block_init,
	bridge_bc7enc_compress_block_fn,
	bridge_lightgrid_free,
	bridge_vec_grow,
	bridge_vec_append,
	bridge_vec_clear,
	bridge_vec_free,
};

static iw_renderer_host_bridge_data_t s_bridge_data;

void R_Backend_FillHostBridge (iw_renderer_host_bridge_t *out)
{
	s_bridge_data.struct_size = sizeof (iw_renderer_host_bridge_data_t);
	s_bridge_data.host_parms = host_parms;
	s_bridge_data.realtime = &realtime;
	s_bridge_data.host_frametime = &host_frametime;
	s_bridge_data.host_rawframetime = &host_rawframetime;
	s_bridge_data.host_initialized = &host_initialized;
	s_bridge_data.host_colormap = &host_colormap;
	s_bridge_data.cl = &cl;
	s_bridge_data.cls = &cls;
	s_bridge_data.sv = &sv;
	s_bridge_data.sv_player = &sv_player;
	s_bridge_data.qcvm = &qcvm;
	s_bridge_data.v_blend = &v_blend;
	s_bridge_data.com_token = com_token;
	s_bridge_data.com_argc = &com_argc;
	s_bridge_data.com_argv = com_argv;
	s_bridge_data.com_gamedir = com_gamedir;
	s_bridge_data.con_forcedup = &con_forcedup;
	s_bridge_data.con_chars = NULL;
	s_bridge_data.key_dest = &key_dest;
	s_bridge_data.sb_lines = &sb_lines;
	s_bridge_data.m_state = NULL;
	s_bridge_data.cl_static_entities = cl_static_entities;
	s_bridge_data.cl_lightstyle = cl_lightstyle;
	s_bridge_data.cl_visedicts = cl_visedicts;
	s_bridge_data.cl_numvisedicts = &cl_numvisedicts;
	s_bridge_data.cl_entities = &cl_entities;
	s_bridge_data.in_attack = &in_attack;
	s_bridge_data.com_searchpaths = COM_GetSearchPathsPointer ();
	s_bridge_data.com_filesize = &com_filesize;
	s_bridge_data.con_initialized = &con_initialized;
	s_bridge_data.developer = &developer;
	s_bridge_data.map_checks = &map_checks;
	s_bridge_data.scr_scale = NULL;
	s_bridge_data.chase_active = &chase_active;
	s_bridge_data.sensitivity = &sensitivity;
	s_bridge_data.wad_base = &wad_base;
	s_bridge_data.vec3_origin = &vec3_origin;
	s_bridge_data.vec4_origin = &vec4_origin;
	s_bridge_data.crosshair = &crosshair;
	s_bridge_data.crosshair_char = &crosshair_char;
	s_bridge_data.con_notifyfade = &con_notifyfade;
	s_bridge_data.con_notifyfadetime = &con_notifyfadetime;
	s_bridge_data.dev_stats = (void *)&dev_stats;
	s_bridge_data.dev_peakstats = (void *)&dev_peakstats;
	s_bridge_data.devstats = &devstats;
	s_bridge_data.host_timescale = &host_timescale;
	s_bridge_data.isDedicated = &isDedicated;
	s_bridge_data.r_material_debug = &r_material_debug;
	s_bridge_data.r_materials = &r_materials;
	s_bridge_data.r_skyvis = &r_skyvis;
	s_bridge_data.r_skyvis_debug = &r_skyvis_debug;
	s_bridge_data.r_sun_visibility = &r_sun_visibility;
	s_bridge_data.r_tcgen_debug = &r_tcgen_debug;
	s_bridge_data.r_particles_material_strict = &r_particles_material_strict;
	s_bridge_data.sv_gravity = &sv_gravity;

	out->struct_size = sizeof (iw_renderer_host_bridge_t);
	out->abi_version = IW_RENDERER_HOST_BRIDGE_ABI_VERSION;
	out->functions = &s_bridge_functions;
	out->data = &s_bridge_data;
}
