#ifdef RENDERER_PLUGIN_BUILD

#include "quakedef.h"
#include "mat_material.h"
#include <stdarg.h>

#undef Cmd_AddCommand
#undef Sys_Error
#undef Host_Error

const iw_renderer_host_bridge_t *g_host_bridge = NULL;
const iw_renderer_host_bridge_functions_t *g_bridge_fn = NULL;
const iw_renderer_host_bridge_data_t *g_bridge_data = NULL;

qcvm_t *qcvm = NULL;

void Bridge_Init (const iw_renderer_host_bridge_t *bridge)
{
	g_host_bridge = bridge;
	if (bridge)
	{
		g_bridge_fn = bridge->functions;
		g_bridge_data = bridge->data;
		qcvm = *g_bridge_data->qcvm;
	}
}

void Con_Printf (const char *fmt, ...)
{
	va_list args;
	va_start (args, fmt);
	g_bridge_fn->con_vprintf (fmt, args);
	va_end (args);
}

void Con_DPrintf (const char *fmt, ...)
{
	va_list args;
	va_start (args, fmt);
	g_bridge_fn->con_vdprintf (fmt, args);
	va_end (args);
}

void Con_DWarning (const char *fmt, ...)
{
	va_list args;
	va_start (args, fmt);
	g_bridge_fn->con_vdwaring (fmt, args);
	va_end (args);
}

void Con_Warning (const char *fmt, ...)
{
	va_list args;
	va_start (args, fmt);
	g_bridge_fn->con_vwarning (fmt, args);
	va_end (args);
}

void Con_SafePrintf (const char *fmt, ...)
{
	va_list args;
	va_start (args, fmt);
	g_bridge_fn->con_vsafe_printf (fmt, args);
	va_end (args);
}

void Con_LinkPrintf (const char *addr, const char *fmt, ...)
{
	va_list args;
	va_start (args, fmt);
	g_bridge_fn->con_vlink_printf (addr, fmt, args);
	va_end (args);
}

void Con_AddToTabList (const char *name, const char *partial, const char *type)
{
	g_bridge_fn->con_add_to_tab_list (name, partial, type);
}

void Cvar_RegisterVariable (cvar_t *variable)
{
	g_bridge_fn->cvar_register (variable);
}

void Cvar_SetCallback (cvar_t *var, void (*func)(cvar_t *))
{
	g_bridge_fn->cvar_set_callback (var, func);
}

void Cvar_SetQuick (cvar_t *var, const char *value)
{
	g_bridge_fn->cvar_set_quick (var, value);
}

void Cvar_SetValueQuick (cvar_t *var, float value)
{
	g_bridge_fn->cvar_set_value_quick (var, value);
}

cvar_t *Cvar_FindVar (const char *var_name)
{
	return g_bridge_fn->cvar_find_var (var_name);
}

void Cvar_SetValue (const char *var_name, float value)
{
	g_bridge_fn->cvar_set_value (var_name, value);
}

void Cvar_Set (const char *var_name, const char *value)
{
	g_bridge_fn->cvar_set (var_name, value);
}

float Cvar_VariableValue (const char *var_name)
{
	return g_bridge_fn->cvar_variable_value (var_name);
}

void *Cmd_AddCommand (const char *name, void (*func)(void))
{
	return g_bridge_fn->cmd_add_command (name, func);
}

int Cmd_Argc (void)
{
	return g_bridge_fn->cmd_argc ();
}

const char *Cmd_Argv (int arg)
{
	return g_bridge_fn->cmd_argv (arg);
}

const char *COM_Parse (const char *data)
{
	return g_bridge_fn->com_parse (data);
}

const char *COM_ParseEx (const char *data, cpe_mode mode)
{
	return g_bridge_fn->com_parse_ex (data, mode);
}

int COM_CheckParm (const char *parm)
{
	return g_bridge_fn->com_check_parm (parm);
}

void COM_StripExtension (const char *in, char *out, size_t outsize)
{
	g_bridge_fn->com_strip_extension (in, out, outsize);
}

void COM_FileBase (const char *in, char *out, size_t outsize)
{
	g_bridge_fn->com_file_base (in, out, outsize);
}

void COM_AddExtension (char *path, const char *extension, size_t len)
{
	g_bridge_fn->com_add_extension (path, extension, len);
}

const char *COM_FileGetExtension (const char *in)
{
	return g_bridge_fn->com_file_get_extension (in);
}

qboolean COM_HasExtension (const char *path, const char *extension)
{
	return g_bridge_fn->com_has_extension (path, extension);
}

char *COM_TintString (const char *in, char *out, size_t outsize)
{
	return g_bridge_fn->com_tint_string (in, out, outsize);
}

unsigned COM_HashBlock (const void *data, size_t size)
{
	return g_bridge_fn->com_hash_block (data, size);
}

byte *COM_LoadHunkFile (const char *path, unsigned int *path_id)
{
	return g_bridge_fn->com_load_hunk_file (path, path_id);
}

byte *COM_LoadMallocFile (const char *path, unsigned int *path_id)
{
	return g_bridge_fn->com_load_malloc_file (path, path_id);
}

int COM_FOpenFile (const char *filename, FILE **file, unsigned int *path_id)
{
	return g_bridge_fn->com_fopen_file (filename, file, path_id);
}

qboolean COM_FileExists (const char *filename, unsigned int *path_id)
{
	return g_bridge_fn->com_file_exists (filename, path_id);
}

const char *COM_SkipPath (const char *pathname)
{
	return g_bridge_fn->com_skip_path (pathname);
}

qboolean COM_ParseLine (const char **str, stringview_t *line)
{
	return g_bridge_fn->com_parse_line (str, line);
}

char *COM_TintSubstring (const char *in, const char *substr, char *out, size_t outsize)
{
	return g_bridge_fn->com_tint_substring (in, substr, out, outsize);
}

char *va (const char *format, ...)
{
	va_list args;
	va_start (args, format);
	char *result = g_bridge_fn->va_list_fn (format, args);
	va_end (args);
	return result;
}

void Sys_Error (const char *error, ...)
{
	va_list args;
	va_start (args, error);
	g_bridge_fn->sys_verror (error, args);
	va_end (args);
}

void Sys_Printf (const char *fmt, ...)
{
	va_list args;
	va_start (args, fmt);
	g_bridge_fn->sys_vprintf (fmt, args);
	va_end (args);
}

double Sys_DoubleTime (void)
{
	return g_bridge_fn->sys_double_time ();
}

void Sys_SendKeyEvents (void)
{
	g_bridge_fn->sys_send_key_events ();
}

void Sys_Sleep (unsigned long msecs)
{
	g_bridge_fn->sys_sleep (msecs);
}

FILE *Sys_fopen (const char *path, const char *mode)
{
	return g_bridge_fn->sys_fopen (path, mode);
}

qboolean Sys_GetFileTime (const char *path, time_t *out)
{
	return g_bridge_fn->sys_get_file_time (path, out);
}

int Sys_FileType (const char *path)
{
	return g_bridge_fn->sys_file_type (path);
}

qfileofs_t Sys_ftell (FILE *file)
{
	return g_bridge_fn->sys_ftell (file);
}

void *Sys_LoadLibrary (const char *path)
{
	return g_bridge_fn->sys_load_library (path);
}

void *Sys_GetLibraryFunction (void *lib, const char *name)
{
	return g_bridge_fn->sys_get_library_function (lib, name);
}

void Sys_CloseLibrary (void *lib)
{
	g_bridge_fn->sys_close_library (lib);
}

qboolean Sys_IsDebuggerPresent (void)
{
	return g_bridge_fn->sys_is_debugger_present ();
}


void *Hunk_Alloc (int size)
{
	return g_bridge_fn->hunk_alloc (size);
}

void *Hunk_AllocNoFill (int size)
{
	return g_bridge_fn->hunk_alloc_no_fill (size);
}

void *Hunk_AllocName (int size, const char *name)
{
	return g_bridge_fn->hunk_alloc_name (size, name);
}

void *Hunk_AllocNameNoFill (int size, const char *name)
{
	return g_bridge_fn->hunk_alloc_name_no_fill (size, name);
}

int Hunk_LowMark (void)
{
	return g_bridge_fn->hunk_low_mark ();
}

void Hunk_FreeToLowMark (int mark)
{
	g_bridge_fn->hunk_free_to_low_mark (mark);
}

void *Cache_Check (cache_user_t *c)
{
	return g_bridge_fn->cache_check (c);
}

void Cache_Free (cache_user_t *c, qboolean freetextures)
{
	g_bridge_fn->cache_free (c, freetextures);
}

void *Cache_Alloc (cache_user_t *c, int size, const char *name)
{
	return g_bridge_fn->cache_alloc (c, size, name);
}

void *W_GetLumpName (const char *name, lumpinfo_t **out_info)
{
	return g_bridge_fn->w_get_lump_name (name, out_info);
}

void PR_SwitchQCVM (qcvm_t *nvm)
{
	g_bridge_fn->pr_switch_qcvm (nvm);
	qcvm = nvm;
}

void PR_PushQCVM (qcvm_t *newvm, qcvm_t **oldvm)
{
	g_bridge_fn->pr_push_qcvm (newvm, oldvm);
	qcvm = newvm;
}

void PR_PopQCVM (qcvm_t *oldvm)
{
	g_bridge_fn->pr_pop_qcvm (oldvm);
	qcvm = oldvm;
}

byte *SV_FatPVS (vec3_t org, qmodel_t *worldmodel)
{
	return g_bridge_fn->sv_fat_pvs (org, worldmodel);
}

qboolean SV_EdictInPVS (edict_t *test, byte *pvs)
{
	return g_bridge_fn->sv_edict_in_pvs (test, pvs);
}

qboolean CL_InCutscene (void)
{
	return g_bridge_fn->cl_in_cutscene ();
}

qboolean CL_IsPlayerEnt (const entity_t *ent)
{
	return g_bridge_fn->cl_is_player_ent (ent);
}

int MSG_ReadByte (void)
{
	return g_bridge_fn->msg_read_byte ();
}

int MSG_ReadShort (void)
{
	return g_bridge_fn->msg_read_short ();
}

void M_Draw (void)
{
	g_bridge_fn->m_draw ();
}

qboolean M_WantsConsole (float *alpha)
{
	return g_bridge_fn->m_wants_console (alpha);
}

qboolean M_ForcedCenterPrint (float *alpha)
{
	return g_bridge_fn->m_forced_center_print (alpha);
}

qboolean M_ForcedUnderwater (void)
{
	return g_bridge_fn->m_forced_underwater ();
}

void M_DrawTextBox (int x, int y, int width, int lines)
{
	g_bridge_fn->m_draw_text_box (x, y, width, lines);
}

void V_PolyBlend (void)
{
	g_bridge_fn->v_polyblend ();
}

void Host_Error (const char *error, ...)
{
	va_list args;
	va_start (args, error);
	g_bridge_fn->host_verror (error, args);
	va_end (args);
}

void Cbuf_AddText (const char *text)
{
	g_bridge_fn->cbuf_add_text (text);
}

void VectorMA (const vec3_t veca, float scale, const vec3_t vecb, vec3_t vecc)
{
	g_bridge_fn->vector_ma (veca, scale, vecb, vecc);
}

float VectorNormalize (vec3_t v)
{
	return g_bridge_fn->vector_normalize (v);
}

int VectorCompare (const vec3_t v1, const vec3_t v2)
{
	return g_bridge_fn->vector_compare (v1, v2);
}

vec_t VectorLength (const vec3_t v)
{
	return g_bridge_fn->vector_length (v);
}

void VectorLerp (const vec3_t veca, const vec3_t vecb, float frac, vec3_t dst)
{
	g_bridge_fn->vector_lerp (veca, vecb, frac, dst);
}

void CrossProduct (const vec3_t v1, const vec3_t v2, vec3_t cross)
{
	g_bridge_fn->cross_product (v1, v2, cross);
}

void AngleVectors (vec3_t angles, vec3_t forward, vec3_t right, vec3_t up)
{
	g_bridge_fn->angle_vectors (angles, forward, right, up);
}

float Distance (const vec3_t a, const vec3_t b)
{
	return g_bridge_fn->distance_fn (a, b);
}

void VectorScale (const vec3_t in, vec_t scale, vec3_t out)
{
	g_bridge_fn->vector_scale (in, scale, out);
}

void VectorInverse (vec3_t v)
{
	g_bridge_fn->vector_inverse (v);
}

void ProjectVector (const vec3_t src, const float matrix[16], vec3_t dst)
{
	g_bridge_fn->project_vector (src, matrix, dst);
}

void MatrixMultiply (float left[16], float right[16])
{
	g_bridge_fn->matrix_multiply (left, right);
}

void RotationMatrix (float matrix[16], float angle, int axis)
{
	g_bridge_fn->rotation_matrix (matrix, angle, axis);
}

void TranslationMatrix (float matrix[16], float x, float y, float z)
{
	g_bridge_fn->translation_matrix (matrix, x, y, z);
}

void MatrixTranspose4x3 (const float src[16], float dst[12])
{
	g_bridge_fn->matrix_transpose_4x3 (src, dst);
}

qboolean Mat4_Inverse (const float in[16], float out[16])
{
	return g_bridge_fn->mat4_inverse (in, out);
}

void R_ConcatTransforms (float in1[3][4], float in2[3][4], float out[3][4])
{
	g_bridge_fn->r_concat_transforms (in1, in2, out);
}

void ApplyTranslation (float matrix[16], float x, float y, float z)
{
	g_bridge_fn->apply_translation (matrix, x, y, z);
}

void ApplyScale (float matrix[16], float x, float y, float z)
{
	g_bridge_fn->apply_scale (matrix, x, y, z);
}

uint32_t Interleave (uint16_t even, uint16_t odd)
{
	return g_bridge_fn->interleave (even, odd);
}

qboolean RayVsBox (const vec3_t org, const vec3_t rcpdelta, const vec3_t mins, const vec3_t maxs, float *frac)
{
	return g_bridge_fn->ray_vs_box (org, rcpdelta, mins, maxs, frac);
}

int BoxOnPlaneSide (vec3_t emins, vec3_t emaxs, mplane_t *plane)
{
	return g_bridge_fn->box_on_plane_side (emins, emaxs, plane);
}

int Q_nextPow2 (int val)
{
	return g_bridge_fn->q_next_pow2 (val);
}

int Q_log2 (int val)
{
	return g_bridge_fn->q_log2 (val);
}

float GetFraction (float val, float minval, float maxval)
{
	return g_bridge_fn->get_fraction (val, minval, maxval);
}

unsigned short CRC_Block (const void *start, int count)
{
	return g_bridge_fn->crc_block (start, count);
}

void SwapPic (qpic_t *pic)
{
	g_bridge_fn->swap_pic ((void *)pic);
}

int Q_strcmp (const char *s1, const char *s2)
{
	return g_bridge_fn->q_strcmp (s1, s2);
}

int Q_strncmp (const char *s1, const char *s2, int count)
{
	return g_bridge_fn->q_strncmp (s1, s2, count);
}

void Q_strncpy (char *dest, const char *src, int count)
{
	g_bridge_fn->q_strncpy (dest, src, count);
}

int Q_strlen (const char *str)
{
	return g_bridge_fn->q_strlen (str);
}

char *q_strcasestr (const char *haystack, const char *needle)
{
	return g_bridge_fn->q_strcasestr (haystack, needle);
}

void *q_malloc (size_t size)
{
	return g_bridge_fn->q_malloc_fn (size);
}

void q_free (void *ptr)
{
	g_bridge_fn->q_free_fn (ptr);
}

void *q_realloc (void *ptr, size_t size)
{
	return g_bridge_fn->q_realloc_fn (ptr, size);
}

void *q_calloc (size_t count, size_t size)
{
	return g_bridge_fn->q_calloc_fn (count, size);
}

int q_snprintf (char *str, size_t size, const char *format, ...)
{
	va_list args;
	va_start (args, format);
	int ret = g_bridge_fn->q_vsnprintf (str, size, format, args);
	va_end (args);
	return ret;
}

int q_vsnprintf (char *str, size_t size, const char *format, va_list args)
{
	return g_bridge_fn->q_vsnprintf (str, size, format, args);
}

int Q_atoi (const char *str)
{
	return g_bridge_fn->q_atoi (str);
}

float Q_atof (const char *str)
{
	return g_bridge_fn->q_atof (str);
}

size_t q_strlcpy (char *dst, const char *src, size_t size)
{
	return g_bridge_fn->q_strlcpy (dst, src, size);
}

size_t q_strlcat (char *dst, const char *src, size_t size)
{
	return g_bridge_fn->q_strlcat (dst, src, size);
}

int q_strcasecmp (const char *s1, const char *s2)
{
	return g_bridge_fn->q_strcasecmp (s1, s2);
}

int q_strncasecmp (const char *s1, const char *s2, size_t n)
{
	return g_bridge_fn->q_strncasecmp (s1, s2, n);
}

findfile_t *Sys_FindFirst (const char *dir, const char *ext)
{
	return g_bridge_fn->sys_find_first (dir, ext);
}

findfile_t *Sys_FindNext (findfile_t *find)
{
	return g_bridge_fn->sys_find_next (find);
}

qboolean Image_WriteTGA (const char *name, byte *data, int width, int height, int bpp, qboolean upsidedown)
{
	return g_bridge_fn->image_write_tga (name, data, width, height, bpp, upsidedown);
}

qboolean Image_WritePNG (const char *name, byte *data, int width, int height, int bpp, qboolean upsidedown)
{
	return g_bridge_fn->image_write_png (name, data, width, height, bpp, upsidedown);
}

qboolean Image_WriteJPG (const char *name, byte *data, int width, int height, int bpp, int quality, qboolean upsidedown)
{
	return g_bridge_fn->image_write_jpg (name, data, width, height, bpp, quality, upsidedown);
}

void Cvar_SetCompletion (cvar_t *var, void (*func)(cvar_t *, const char *))
{
	g_bridge_fn->cvar_set_completion (var, func);
}

void R_Backend_Register (const void *backend)
{
	g_bridge_fn->r_backend_register (backend);
}

void R_Backend_Init (void)
{
	g_bridge_fn->r_backend_init ();
}

void R_Backend_Shutdown (void)
{
	g_bridge_fn->r_backend_shutdown ();
}

void R_Backend_MemoryBarrier (unsigned barrier_bits)
{
	g_bridge_fn->r_backend_memory_barrier (barrier_bits);
}

void R_Backend_SetDynamicState (const RenderBackendDynamicState *dynamic_state)
{
	g_bridge_fn->r_backend_set_dynamic_state (dynamic_state);
}

void R_Backend_DrawIndexed (int primitive, int index_type, int count, intptr_t index_offset_bytes)
{
	g_bridge_fn->r_backend_draw_indexed (primitive, index_type, count, index_offset_bytes);
}

void R_Backend_BindPipeline (const RenderBackendPipelineDesc *pipeline)
{
	g_bridge_fn->r_backend_bind_pipeline (pipeline);
}

void R_Backend_SetViewport (int x, int y, int width, int height)
{
	g_bridge_fn->r_backend_set_viewport (x, y, width, height);
}

void R_Backend_Finish (void)
{
	g_bridge_fn->r_backend_finish ();
}

void R_Backend_Draw (int primitive, int first, int count)
{
	g_bridge_fn->r_backend_draw_fn (primitive, first, count);
}

void R_Backend_Dispatch (unsigned group_x, unsigned group_y, unsigned group_z)
{
	g_bridge_fn->r_backend_dispatch (group_x, group_y, group_z);
}

void R_Backend_Present (void)
{
	g_bridge_fn->r_backend_present ();
}

void R_Backend_BeginFrame (void)
{
	g_bridge_fn->r_backend_begin_frame ();
}

void R_Backend_EndFrame (void)
{
	g_bridge_fn->r_backend_end_frame ();
}

void R_Backend_OnResize (int width, int height)
{
	g_bridge_fn->r_backend_on_resize (width, height);
}

int R_Backend_GetSceneSampleCount (void)
{
	return g_bridge_fn->r_backend_get_scene_sample_count ();
}

void R_Backend_DrawIndexedIndirect (int primitive, int index_type, intptr_t indirect_offset_bytes)
{
	g_bridge_fn->r_backend_draw_indexed_indirect (primitive, index_type, indirect_offset_bytes);
}

void R_Backend_MultiDrawIndexedIndirect (int primitive, int index_type, intptr_t indirect_offset_bytes, int draw_count, int stride_bytes)
{
	g_bridge_fn->r_backend_multi_draw_indexed_indirect (primitive, index_type, indirect_offset_bytes, draw_count, stride_bytes);
}

void R_Backend_DrawIndexedInstanced (int primitive, int index_type, int count, intptr_t index_offset_bytes, int instance_count)
{
	g_bridge_fn->r_backend_draw_indexed_instanced (primitive, index_type, count, index_offset_bytes, instance_count);
}

void R_Backend_DrawInstanced (int primitive, int first, int count, int instance_count)
{
	g_bridge_fn->r_backend_draw_instanced (primitive, first, count, instance_count);
}

void R_Backend_DrawPacket (const void *packet)
{
	g_bridge_fn->r_backend_draw_packet (packet);
}

void R_Backend_BindDescriptors (const void *bindings, unsigned count)
{
	g_bridge_fn->r_backend_bind_descriptors (bindings, count);
}

void R_Backend_SetBlendFactors (int src, int dst)
{
	g_bridge_fn->r_backend_set_blend_factors (src, dst);
}

void R_Backend_SetDepthFunc (int depth_func)
{
	g_bridge_fn->r_backend_set_depth_func (depth_func);
}

const void *R_Backend_GetCaps (void)
{
	return g_bridge_fn->r_backend_get_caps ();
}

void R_Backend_ConfigurePostFXLUTTexture (unsigned texture_id)
{
	g_bridge_fn->r_backend_configure_postfx_lut_texture (texture_id);
}

unsigned R_Backend_CreatePostFXLUTTexture (void)
{
	return g_bridge_fn->r_backend_create_postfx_lut_texture ();
}

void Sbar_Changed (void)
{
	g_bridge_fn->sbar_changed ();
}

void Sbar_Draw (void)
{
	g_bridge_fn->sbar_draw ();
}

void Sbar_LoadPics (void)
{
	g_bridge_fn->sbar_load_pics ();
}

void Sbar_IntermissionOverlay (void)
{
	g_bridge_fn->sbar_intermission_overlay ();
}

void Sbar_FinaleOverlay (void)
{
	g_bridge_fn->sbar_finale_overlay ();
}

void Con_ClearNotify (void)
{
	g_bridge_fn->con_clear_notify ();
}

void Con_DrawConsole (int lines, qboolean drawbg, qboolean drawinput)
{
	g_bridge_fn->con_draw_console (lines, drawbg, drawinput);
}

void Con_DrawNotify (void)
{
	g_bridge_fn->con_draw_notify ();
}

void Con_CheckResize (void)
{
	g_bridge_fn->con_check_resize ();
}

void Con_DPrintf2 (const char *fmt, ...)
{
	va_list args;
	va_start (args, fmt);
	g_bridge_fn->con_vdprintf2 (fmt, args);
	va_end (args);
}

void Key_GetGrabbedInput (int *lastkey, int *lastchar)
{
	g_bridge_fn->key_get_grabbed_input (lastkey, lastchar);
}

void Key_BeginInputGrab (void)
{
	g_bridge_fn->key_begin_input_grab ();
}

void Key_EndInputGrab (void)
{
	g_bridge_fn->key_end_input_grab ();
}

void Key_ClearStates (void)
{
	g_bridge_fn->key_clear_states ();
}

void IN_DeactivateForConsole (void)
{
	g_bridge_fn->in_deactivate_for_console ();
}

void IN_DeactivateForMenu (void)
{
	g_bridge_fn->in_deactivate_for_menu ();
}

void IN_ClearStates (void)
{
	g_bridge_fn->in_clear_states ();
}

void M_Print (int cx, int cy, const char *str)
{
	g_bridge_fn->m_print (cx, cy, str);
}

void V_RenderView (void)
{
	g_bridge_fn->v_render_view ();
}

void V_CalcBlend (void)
{
	g_bridge_fn->v_calc_blend ();
}

void V_UpdateBlend (void)
{
	g_bridge_fn->v_update_blend ();
}

void V_SetContentsColor (int contents)
{
	g_bridge_fn->v_set_contents_color (contents);
}

void CL_PostFX_SetContents (int contents, qboolean underwater_active, qboolean underwater_postfx_active)
{
	g_bridge_fn->cl_postfx_set_contents (contents, underwater_active, underwater_postfx_active);
}

void CL_PostFX_GetState (void *out_state)
{
	g_bridge_fn->cl_postfx_get_state (out_state);
}

qboolean ED_IsRelevantField (edict_t *ed, ddef_t *d)
{
	return g_bridge_fn->ed_is_relevant_field (ed, (void *)d);
}

const char *ED_FieldValueString (edict_t *ed, ddef_t *d)
{
	return g_bridge_fn->ed_field_value_string (ed, (void *)d);
}

const char *PR_GetString (int num)
{
	return g_bridge_fn->pr_get_string (num);
}

int NUM_FOR_EDICT (edict_t *e)
{
	return g_bridge_fn->num_for_edict (e);
}

void PR_ReloadPics (qboolean purge)
{
	g_bridge_fn->pr_reload_pics (purge);
}

void W_LoadWadFile (void)
{
	g_bridge_fn->w_load_wad_file ();
}

void *Z_Malloc (int size)
{
	return g_bridge_fn->z_malloc (size);
}

void Z_Free (void *ptr)
{
	g_bridge_fn->z_free_fn (ptr);
}

void Sys_ReportError (const char *error, ...)
{
	va_list args;
	va_start (args, error);
	g_bridge_fn->sys_report_verror (error, args);
	va_end (args);
}

void Host_ReportError (const char *error, ...)
{
	va_list args;
	va_start (args, error);
	g_bridge_fn->host_report_verror (error, args);
	va_end (args);
}

void Host_BeginAssetLoading (void)
{
	g_bridge_fn->host_begin_asset_loading ();
}

void Host_EndAssetLoading (void)
{
	g_bridge_fn->host_end_asset_loading ();
}

qboolean Host_IsSaving (void)
{
	return g_bridge_fn->host_is_saving ();
}

int CFG_OpenConfig (const char *cfg_name)
{
	return g_bridge_fn->cfg_open_config (cfg_name);
}

void CFG_CloseConfig (void)
{
	g_bridge_fn->cfg_close_config ();
}

void CFG_ReadCvars (const char **vars, int num_vars)
{
	g_bridge_fn->cfg_read_cvars (vars, num_vars);
}

void CFG_ReadCvarOverrides (const char **vars, int num_vars)
{
	g_bridge_fn->cfg_read_cvar_overrides (vars, num_vars);
}

cmd_function_t *Cmd_AddCommand2 (const char *cmd_name, xcommand_t function, cmd_source_t srctype, qboolean qcinterceptable)
{
	return (cmd_function_t *)g_bridge_fn->cmd_add_command2 (cmd_name, (void (*)(void))function, (int)srctype, qcinterceptable);
}

void PL_SetWindowIcon (void)
{
	g_bridge_fn->pl_set_window_icon ();
}

void PL_VID_Shutdown (void)
{
	g_bridge_fn->pl_vid_shutdown ();
}

void VID_Menu_Init (void)
{
	g_bridge_fn->vid_menu_init ();
}

byte *Image_LoadImage (const char *name, int *width, int *height, enum srcformat *fmt)
{
	return g_bridge_fn->image_load_image (name, width, height, (int *)fmt);
}

qboolean Steam_SaveScreenshot (const void *rgb, int width, int height)
{
	return g_bridge_fn->steam_save_screenshot (rgb, width, height);
}

void S_ExtraUpdate (void)
{
	g_bridge_fn->s_extra_update ();
}

void S_ClearBuffer (void)
{
	g_bridge_fn->s_clear_buffer ();
}

void S_StopAllSounds (qboolean clear)
{
	g_bridge_fn->s_stop_all_sounds (clear);
}

void CDAudio_Pause (void)
{
	g_bridge_fn->cdaudio_pause ();
}

void CDAudio_Resume (void)
{
	g_bridge_fn->cdaudio_resume ();
}

void BGM_Pause (void)
{
	g_bridge_fn->bgm_pause ();
}

void BGM_Resume (void)
{
	g_bridge_fn->bgm_resume ();
}

size_t UTF8_FromQuake (char *dst, size_t maxbytes, const char *src)
{
	return g_bridge_fn->utf8_from_quake (dst, maxbytes, src);
}

size_t UTF8_ToQuake (char *dst, size_t maxbytes, const char *src)
{
	return g_bridge_fn->utf8_to_quake (dst, maxbytes, src);
}

void MultiString_Append (char **pvec, const char *str)
{
	g_bridge_fn->multi_string_append (pvec, str);
}

void MultiString_AppendN (char **pvec, const char *str, size_t len)
{
	g_bridge_fn->multi_string_append_n (pvec, str, len);
}

float MSG_ReadCoord (unsigned int flags)
{
	return g_bridge_fn->msg_read_coord (flags);
}

int MSG_ReadChar (void)
{
	return g_bridge_fn->msg_read_char ();
}

trace_t SV_Move (vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end, int type, edict_t *passedict)
{
	return g_bridge_fn->sv_move (start, mins, maxs, end, type, passedict);
}

qboolean SV_RecursiveHullCheck (const hull_t *hull, int num, float p1f, float p2f, vec3_t p1, vec3_t p2, trace_t *trace)
{
	return g_bridge_fn->sv_recursive_hull_check (hull, num, p1f, p2f, p1, p2, trace);
}

qboolean SV_BoxInPVS (vec3_t mins, vec3_t maxs, byte *pvs, mnode_t *node)
{
	return g_bridge_fn->sv_box_in_pvs (mins, maxs, pvs, node);
}

void Material_Init (void)
{
	g_bridge_fn->material_init ();
}

void Material_Shutdown (void)
{
	g_bridge_fn->material_shutdown ();
}

void Material_ApplyToTexture (texture_t *tex, const char *mapname)
{
	g_bridge_fn->material_apply_to_texture (tex, mapname);
}

const material_t *Material_Find (const char *name)
{
	return g_bridge_fn->material_find (name);
}

const material_t *Material_FindForTextureName (const char *texname, const char *mapname)
{
	return g_bridge_fn->material_find_for_texture_name (texname, mapname);
}

void Material_Canonicalize (const char *name, char *out, size_t out_size)
{
	g_bridge_fn->material_canonicalize (name, out, out_size);
}

mat_particle_stage_support_t Material_ClassifyParticleStage (const material_stage_t *stage, mat_particle_policy_t policy, char *reason, size_t reason_size)
{
	return (mat_particle_stage_support_t)g_bridge_fn->material_classify_particle_stage ((const void *)stage, (int)policy, reason, reason_size);
}

qboolean Material_StageSupportsParticleMVP (const material_stage_t *stage, char *reason, size_t reason_size)
{
	return g_bridge_fn->material_stage_supports_particle_mvp ((const void *)stage, reason, reason_size);
}

const mat_texmatrix_t *MaterialStage_EvalTexMatrix (material_stage_t *stage, float time)
{
	return g_bridge_fn->material_stage_eval_tex_matrix ((void *)stage, time);
}

const char *MaterialStage_GetAnimMapPath (material_stage_t *stage, float time)
{
	return g_bridge_fn->material_stage_get_anim_map_path ((void *)stage, time);
}

float Material_EvalWaveValue (const mat_wave_t *wave, float time)
{
	return g_bridge_fn->material_eval_wave_value (wave, time);
}

void DLightPool_ClearPersistent (void)
{
	g_bridge_fn->dlight_pool_clear_persistent ();
}

int DLightPool_CollectForRender (double time, const vec3_t vieworg, const mleaf_t *viewleaf, dlight_t **out, int out_max)
{
	return g_bridge_fn->dlight_pool_collect_for_render (time, vieworg, viewleaf, out, out_max);
}

const dlight_t *const *DLightPool_GetActiveList (int *count)
{
	return g_bridge_fn->dlight_pool_get_active_list (count);
}

int DLightPool_GetBudget (void)
{
	return g_bridge_fn->dlight_pool_get_budget ();
}

dlight_t *DLightPool_GetOrCreatePersistent (int key, double time)
{
	return g_bridge_fn->dlight_pool_get_or_create_persistent (key, time);
}

void DLightPool_NewFrame (double time, int framecount)
{
	g_bridge_fn->dlight_pool_new_frame (time, framecount);
}

void R_PPdlights_CollectFrame (void)
{
	g_bridge_fn->r_ppdlights_collect_frame ();
}

int R_PPdlights_BuildModelGpuLights (gpulightbuffer_t *out_buffer, dlight_t **out_sources, int max_lights)
{
	return g_bridge_fn->r_ppdlights_build_model_gpu_lights (out_buffer, out_sources, max_lights);
}

int R_PPdlights_BuildWorldGpuLights (gpulightbuffer_t *out_buffer, dlight_t **out_sources, int max_lights)
{
	return g_bridge_fn->r_ppdlights_build_world_gpu_lights (out_buffer, out_sources, max_lights);
}

const rl_light_t *R_PPdlights_GetFrameLights (int *out_count)
{
	return g_bridge_fn->r_ppdlights_get_frame_lights (out_count);
}

float R_SkyVis_GetResolvedCap (void)
{
	return g_bridge_fn->r_skyvis_get_resolved_cap ();
}

void R_SkyVis_GetTint (vec3_t out_tint)
{
	g_bridge_fn->r_skyvis_get_tint (out_tint);
}

float R_SkyVis_GetResolvedScale (void)
{
	return g_bridge_fn->r_skyvis_get_resolved_scale ();
}

qboolean R_SkyVis_Active (void)
{
	return g_bridge_fn->r_skyvis_active ();
}

void R_SkyVis_Init (void)
{
	g_bridge_fn->r_skyvis_init ();
}

void R_SkyVis_NewMap (void)
{
	g_bridge_fn->r_skyvis_new_map ();
}

float R_SkyVis_Sample (const vec3_t pos)
{
	return g_bridge_fn->r_skyvis_sample (pos);
}

float R_SSAO_SanitizeValue (float value, float fallback, float minval, float maxval)
{
	return g_bridge_fn->r_ssao_sanitize_value (value, fallback, minval, maxval);
}

void R_SSAO_CaptureFogState (const gpuframedata_t *framedata, r_ssao_fog_state_t *out_state)
{
	g_bridge_fn->r_ssao_capture_fog_state (framedata, out_state);
}

void R_SSAO_RegisterCvars (void)
{
	g_bridge_fn->r_ssao_register_cvars ();
}

void R_Quality_Init (void)
{
	g_bridge_fn->r_quality_init ();
}

void R_Quality_Update (void)
{
	g_bridge_fn->r_quality_update ();
}

void R_FrameGraph_RenderView (void)
{
	g_bridge_fn->r_framegraph_render_view ();
}

void R_FrameGraph_GetTimingSummary (double *out_gpu_ms, double *out_cpu_ms, qboolean *out_gpu_valid)
{
	g_bridge_fn->r_framegraph_get_timing_summary (out_gpu_ms, out_cpu_ms, out_gpu_valid);
}

void R_FrameGraph_SetRenderFramePlan (const void *plan)
{
	g_bridge_fn->r_framegraph_set_render_frame_plan (plan);
}

unsigned R_FrameGraph_ResolveRequiredResourceBySlot (const void *resources, int slot, const char *usage_tag)
{
	return g_bridge_fn->r_framegraph_resolve_required_resource_by_slot (resources, slot, usage_tag);
}

qboolean R_FrameGraph_GetRenderFramePlan (void *out_plan)
{
	return g_bridge_fn->r_framegraph_get_render_frame_plan (out_plan);
}

qboolean R_FrameGraph_AddPass (const void *pass_desc)
{
	return g_bridge_fn->r_framegraph_add_pass (pass_desc);
}

float R_Tonemap_TemperedOverbright (float overbright)
{
	return g_bridge_fn->r_tonemap_tempered_overbright (overbright);
}

void bc7enc_compress_block_init (void)
{
	g_bridge_fn->bc7enc_compress_block_init ();
}

int bc7enc_compress_block (void *pBlock, const void *pPixelsRGBA, const void *pComp_params)
{
	return g_bridge_fn->bc7enc_compress_block_fn (pBlock, pPixelsRGBA, pComp_params);
}

void Lightgrid_Free (lightgrid_t *lg)
{
	g_bridge_fn->lightgrid_free (lg);
}

void Vec_Grow (void **pvec, size_t element_size, size_t count)
{
	g_bridge_fn->vec_grow (pvec, element_size, count);
}

void Vec_Append (void **pvec, size_t element_size, const void *data, size_t count)
{
	g_bridge_fn->vec_append (pvec, element_size, data, count);
}

void Vec_Clear (void **pvec)
{
	g_bridge_fn->vec_clear (pvec);
}

void Vec_Free (void **pvec)
{
	g_bridge_fn->vec_free (pvec);
}

#endif /* RENDERER_PLUGIN_BUILD */
