# CVar Review and Use-Case List

This list inventories all registered cvars found via `Cvar_RegisterVariable` and summarizes each cvar's use case by subsystem. Individual cvars inherit the subsystem use case noted for their source file.

Total registered cvars reviewed: **627**.

## Quake/bgmusic.c

**Use case:** Background music source selection.

| CVar | Default |
|---|---:|
| `bgm_extmusic` | `1` |

## Quake/chase.c

**Use case:** Chase camera offsets and enable/disable.

| CVar | Default |
|---|---:|
| `chase_active` | `0` |
| `chase_back` | `100` |
| `chase_right` | `0` |
| `chase_up` | `16` |

## Quake/cl_input.c

**Use case:** Movement speed scales and look sensitivity tuning.

| CVar | Default |
|---|---:|
| `cl_alwaysrun` | `1` |
| `cl_anglespeedkey` | `1.5` |
| `cl_backspeed` | `200` |
| `cl_forwardspeed` | `200` |
| `cl_movespeedkey` | `2.0` |
| `cl_pitchspeed` | `150` |
| `cl_sidespeed` | `350` |
| `cl_upspeed` | `200` |
| `cl_yawspeed` | `140` |

## Quake/cl_main.c

**Use case:** Client networking/session behavior and user identity defaults.

| CVar | Default |
|---|---:|
| `_cl_color` | `0` |
| `_cl_name` | `player` |
| `cfg_unbindall` | `1` |
| `cl_confirmquit` | `0` |
| `cl_maxpitch` | `90` |
| `cl_minpitch` | `-90` |
| `cl_mwheelpitch` | `5` |
| `cl_nolerp` | `0` |
| `cl_shownet` | `0` |
| `cl_startdemos` | `1` |
| `freelook` | `1` |
| `lookspring` | `0` |
| `lookstrafe` | `0` |
| `m_forward` | `1` |
| `m_pitch` | `0.022` |
| `m_side` | `0.8` |
| `m_yaw` | `0.022` |
| `sensitivity` | `3` |

## Quake/common.c

**Use case:** Core filesystem/comms behavior and compatibility checks.

| CVar | Default |
|---|---:|
| `cmdline` | `` |
| `fs_integrity_report` | `0` |
| `language` | `auto` |
| `registered` | `1` |
| `standalone` | `0` |

## Quake/console.c

**Use case:** Console output formatting, notifications, and logging behavior.

| CVar | Default |
|---|---:|
| `con_logcenterprint` | `1` |
| `con_maxcols` | `0` |
| `con_notifycenter` | `0` |
| `con_notifyfade` | `0` |
| `con_notifyfadetime` | `0.5` |
| `con_notifytime` | `3` |

## Quake/gl_draw.c

**Use case:** 2D draw path filtering/scaling controls.

| CVar | Default |
|---|---:|
| `scr_conalpha` | `0.5` |
| `scr_conbrightness` | `1.0` |

## Quake/gl_fog.c

**Use case:** Global fog visual tuning.

| CVar | Default |
|---|---:|
| `r_vfog` | `1` |

## Quake/gl_lightgrid.c

**Use case:** Light grid rendering and debug controls.

| CVar | Default |
|---|---:|
| `r_lightgrid` | `1` |
| `r_lightgrid_debug` | `0` |
| `r_lightgrid_force` | `0` |
| `r_lightgrid_octree_debug` | `0` |

## Quake/gl_model.c

**Use case:** Model loading/rendering constraints and compatibility flags.

| CVar | Default |
|---|---:|
| `external_ents` | `1` |
| `external_lits_dir` | `` |
| `external_vis` | `1` |
| `gl_loadlitfiles` | `1` |
| `load_lightgrid_octree` | `` |
| `mod_ignorelmscale` | `0` |
| `r_md5` | `1` |

## Quake/gl_rlight.c

**Use case:** Realtime lighting controls.

| CVar | Default |
|---|---:|
| `r_debug_itemlight` | `0` |
| `r_minlight_models` | `0.02` |
| `r_model_lightgrid` | `1` |

## Quake/gl_rmain.c

**Use case:** Primary renderer tuning (lighting, shadows, post-processing, water/sky, model rendering, materials, and performance).

| CVar | Default |
|---|---:|
| `gl_clear` | `1` |
| `gl_farclip` | `65536` |
| `gl_finish` | `0` |
| `gl_fullbrights` | `1` |
| `gl_nocolors` | `0` |
| `gl_overbright_models` | `0` |
| `gl_playermip` | `0` |
| `gl_polyblend` | `1` |
| `gl_zfix` | `1` |
| `r_ae_max_exposure` | `8.0` |
| `r_ae_min_exposure` | `0.25` |
| `r_ae_min_scene_luma` | `0.02` |
| `r_alphasort` | `1` |
| `r_ao_applymode` | `0` |
| `r_ao_bentnormals` | `0` |
| `r_ao_debug` | `0` |
| `r_ao_halfres` | `1` |
| `r_ao_intensity` | `1.0` |
| `r_ao_method` | `1` |
| `r_ao_power` | `1.5` |
| `r_ao_quality` | `1` |
| `r_ao_radius` | `24` |
| `r_ao_temporal` | `0` |
| `r_ao_timing` | `0` |
| `r_atmos_debug` | `0` |
| `r_atmos_froxel` | `0` |
| `r_atmos_froxel_res` | `1` |
| `r_atmos_historyweight` | `0.9` |
| `r_atmos_log` | `0` |
| `r_atmos_mode` | `1` |
| `r_atmos_zslices` | `64` |
| `r_autoexposure` | `1` |
| `r_bloom` | `3.00` |
| `r_bloom_threshold` | `1.0` |
| `r_clearcolor` | `2` |
| `r_clustered_debug` | `0` |
| `r_clustered_lighting` | `0` |
| `r_clustered_log` | `1` |
| `r_clustered_maxindices` | `262144` |
| `r_clustered_tilesize` | `16` |
| `r_clustered_zslices` | `24` |
| `r_color_contrast` | `1.0` |
| `r_color_midtone` | `1.0` |
| `r_color_saturation` | `1.05` |
| `r_debug_colorspace` | `0` |
| `r_dither` | `1.0` |
| `r_dlight_bloom` | `1` |
| `r_dlight_bloom_radius` | `1.0` |
| `r_dlight_bloom_scale` | `0.15` |
| `r_dlight_bloom_threshold` | `0.1` |
| `r_dlight_buffer` | `1` |
| `r_dlight_core_boost` | `0.75` |
| `r_dlight_core_exp` | `6.0` |
| `r_dlight_debug` | `0` |
| `r_dlight_enable` | `1` |
| `r_dlight_entities` | `1` |
| `r_dlight_exp` | `2.2` |
| `r_dlight_falloff` | `3` |
| `r_dlight_max` | `64` |
| `r_dlight_mode` | `0` |
| `r_dlight_ndotl` | `0.2` |
| `r_dlight_preset` | `2` |
| `r_dlight_quality` | `2` |
| `r_dlight_radius_scale` | `1.0` |
| `r_dlight_satchop` | `0.1` |
| `r_dlight_scale` | `1.0` |
| `r_dlight_shadows` | `0` |
| `r_dlight_softknee` | `1.5` |
| `r_dlight_style` | `0` |
| `r_dof` | `1` |
| `r_dof_autofocus` | `1` |
| `r_dof_focus` | `64` |
| `r_dof_range` | `255` |
| `r_dof_strength` | `3` |
| `r_drawentities` | `1` |
| `r_drawviewmodel` | `1` |
| `r_drawworld` | `1` |
| `r_dynamic` | `1` |
| `r_exposure_bias` | `1.0` |
| `r_exposure_debug` | `0` |
| `r_exposure_lock` | `0` |
| `r_exposure_max` | `1.15` |
| `r_exposure_min` | `0.85` |
| `r_exposure_speed_down` | `0.3` |
| `r_exposure_speed_up` | `0.6` |
| `r_facenormals_enable` | `1` |
| `r_filmgrain` | `0` |
| `r_filmgrain_affect_ui` | `0` |
| `r_filmgrain_amount` | `0.08` |
| `r_filmgrain_blend` | `0.6` |
| `r_filmgrain_color` | `0` |
| `r_filmgrain_debug` | `0` |
| `r_filmgrain_luma_weight` | `0.6` |
| `r_filmgrain_seed` | `0` |
| `r_filmgrain_size` | `1.0` |
| `r_filmgrain_speed` | `1.0` |
| `r_flatlightstyles` | `0` |
| `r_fullbright` | `0` |
| `r_godrays` | `0` |
| `r_godrays_quality` | `1` |
| `r_godrays_debug` | `0` |
| `r_godrays_sky` *(deprecated)* | `0` |
| `r_godrays_light` *(deprecated)* | `0` |
| `r_godrays_world` *(deprecated)* | `0` |
| `r_lerplightstyles` | `1` |
| `r_lerpmodels` | `1` |
| `r_lerpmove` | `1` |
| `r_lightgrid_directional` | `1` |
| `r_lighting_debug` | `0` |
| `r_lightingdir` | `0` |
| `r_lightmap` | `0` |
| `r_lightmap16f` | `1` |
| `r_lightmap_colorspace` | `srgb` |
| `r_lightmap_colorspace_debug` | `0` |
| `r_lightmap_linear` | `1` |
| `r_lightmap_mipmaps` | `1` |
| `r_litwater` | `1` |
| `r_model_halflambert` | `0` |
| `r_motionblur` | `0` |
| `r_motionblur_depththreshold` | `0.1` |
| `r_motionblur_maxradiuspixels` | `32` |
| `r_motionblur_maxsamples` | `16` |
| `r_motionblur_minvelocity` | `0.0` |
| `r_motionblur_shutter` | `0.75` |
| `r_nolerp_list` | `progs/flame.mdl,progs/flame2.mdl,progs/braztall.mdl,progs/brazshrt.mdl,progs/longtrch.mdl,progs/flame_pyre.mdl,progs/v_saw.mdl,progs/v_xfist.mdl,progs/h2stuff/newfire.mdl` |
| `r_norefresh` | `0` |
| `r_noshadow_list` | `progs/flame2.mdl,progs/flame.mdl,progs/bolt1.mdl,progs/bolt2.mdl,progs/bolt3.mdl,progs/laser.mdl` |
| `r_novis` | `0` |
| `r_oit` | `1` |
| `r_oldskyleaf` | `0` |
| `r_overbrightbits` | `2` |
| `r_polyblend_legacy` | `0` |
| `r_pos` | `0` |
| `r_post_damage_doublevision` | `1` |
| `r_post_damage_dv_debug` | `0` |
| `r_post_damage_dv_freq` | `12.0` |
| `r_post_damage_dv_px` | `2.0` |
| `r_post_damage_dv_quality` | `1` |
| `r_post_damage_dv_strength` | `0.9` |
| `r_post_damage_trauma_decay` | `6.0` |
| `r_post_damage_trauma_scale` | `0.02` |
| `r_postfx` | `1` |
| `r_postfx_bloom_mode` | `0` |
| `r_postfx_damage` | `1` |
| `r_postfx_damage_accum_scale` | `0.5` |
| `r_postfx_damage_accum_window` | `0.1` |
| `r_postfx_damage_desat` | `0.35` |
| `r_postfx_damage_duration` | `0.6` |
| `r_postfx_damage_exposure` | `-0.35` |
| `r_postfx_damage_vignette` | `0.45` |
| `r_postfx_damage_vignette_softness` | `0.6` |
| `r_postfx_debug` | `0` |
| `r_postfx_lut` | `1` |
| `r_postfx_lut_debug_id` | `0` |
| `r_postfx_lut_strength_powerup` | `0.6` |
| `r_postfx_lut_strength_underwater` | `0.5` |
| `r_postfx_pickup` | `1` |
| `r_postfx_pickup_bloom` | `0.6` |
| `r_postfx_pickup_duration` | `0.35` |
| `r_postfx_pickup_exposure` | `0.4` |
| `r_postfx_powerup` | `1` |
| `r_postfx_powerup_lut_strength` | `0.6` |
| `r_postfx_powerup_ramp_in` | `0.2` |
| `r_postfx_powerup_ramp_out` | `0.3` |
| `r_postfx_quad` | `1` |
| `r_postfx_quad_bloom_boost` | `0.4` |
| `r_postfx_quad_emissive_boost` | `0.5` |
| `r_postfx_quad_pulse_intensity` | `0.1` |
| `r_postfx_quad_pulse_speed` | `2.0` |
| `r_postfx_underwater` | `1` |
| `r_postfx_underwater_fog_lava_b` | `0.05` |
| `r_postfx_underwater_fog_lava_g` | `0.2` |
| `r_postfx_underwater_fog_lava_r` | `0.6` |
| `r_postfx_underwater_fog_slime_b` | `0.1` |
| `r_postfx_underwater_fog_slime_g` | `0.25` |
| `r_postfx_underwater_fog_slime_r` | `0.1` |
| `r_postfx_underwater_fog_strength` | `0.4` |
| `r_postfx_underwater_fog_water_b` | `0.5` |
| `r_postfx_underwater_fog_water_g` | `0.35` |
| `r_postfx_underwater_fog_water_r` | `0.2` |
| `r_postfx_underwater_grade_strength` | `0.5` |
| `r_postfx_underwater_ramp_in` | `0.2` |
| `r_postfx_underwater_ramp_out` | `0.2` |
| `r_reflection_probe_debug` | `0` |
| `r_reflection_probes` | `0` |
| `r_rgblighting_enable` | `1` |
| `r_rim` | `1` |
| `r_rim_ambScale` | `0.15` |
| `r_rim_clampAmb` | `0.2` |
| `r_rim_clampDirect` | `1.0` |
| `r_rim_colorScale` | `1.0` |
| `r_rim_debug` | `0` |
| `r_rim_dynScale` | `1.0` |
| `r_rim_gateBias` | `0.0` |
| `r_rim_gateK` | `1.0` |
| `r_rim_power` | `4.0` |
| `r_rim_staticScale` | `1.0` |
| `r_rim_strength` | `0.15` |
| `r_scale` | `1` |
| `r_screendarken` | `0` |
| `r_screendarken_depth` | `0.4` |
| `r_shadow_bias` | `0.001` |
| `r_shadow_bias_mdl` | `0.001` |
| `r_shadow_debug` | `0` |
| `r_shadow_dlight_bias` | `0.0025` |
| `r_shadow_dlight_distance` | `1024` |
| `r_shadow_dlight_max` | `2` |
| `r_shadow_dlight_pcf_taps` | `4` |
| `r_shadow_dlight_size` | `256` |
| `r_shadow_dlights` | `0` |
| `r_shadow_lightgrid` | `0` |
| `r_shadow_lightgrid_mode` | `1` |
| `r_shadow_log` | `0` |
| `r_shadow_log_dump` | `0` |
| `r_shadow_log_file` | `0` |
| `r_shadow_log_gl` | `0` |
| `r_shadow_log_rate` | `60` |
| `r_shadow_normalbias` | `1.0` |
| `r_shadow_normalbias_mdl` | `1.0` |
| `r_shadow_pcf` | `1` |
| `r_shadow_pcf_taps` | `4` |
| `r_shadow_sun` | `1` |
| `r_shadow_sun_dir` | `0.3 0.5 -1.0` |
| `r_shadow_twosided_mdl` | `0` |
| `r_shadow_validate` | `0` |
| `r_shadowmap_size` | `2048` |
| `r_shadows` | `0` |
| `r_showbboxes` | `0` |
| `r_showbboxes_health` | `0` |
| `r_showbboxes_links` | `3` |
| `r_showbboxes_targets` | `1` |
| `r_showbboxes_think` | `0` |
| `r_showfields` | `0` |
| `r_showfields_align` | `1` |
| `r_showtris` | `0` |
| `r_simd` | `1` |
| `r_slimealpha` | `0` |
| `r_speeds` | `0` |
| `r_srgb_framebuffer` | `1` |
| `r_srgb_textures` | `1` |
| `r_ssao` | `1` |
| `r_ssao_bias` | `0.02` |
| `r_ssao_blur` | `1` |
| `r_ssao_blur_bilateral` | `1` |
| `r_ssao_blur_radius` | `2` |
| `r_ssao_blur_sigma` | `2.0` |
| `r_ssao_debug` | `0` |
| `r_ssao_debug_far` | `4096` |
| `r_ssao_fog_power` | `1.5` |
| `r_ssao_fog_strength` | `1.0` |
| `r_ssao_force_fullres` | `0` |
| `r_ssao_format` | `1` |
| `r_ssao_freeze_noise` | `0` |
| `r_ssao_halfres` | `1` |
| `r_ssao_intensity` | `1.5` |
| `r_ssao_max_distance` | `1024` |
| `r_ssao_min` | `0.55` |
| `r_ssao_noise` | `1` |
| `r_ssao_noise_mode` | `1` |
| `r_ssao_noise_scale` | `1.0` |
| `r_ssao_normalsource` | `0` |
| `r_ssao_power` | `1.5` |
| `r_ssao_radius` | `24` |
| `r_ssao_reversedz_mode` | `0` |
| `r_ssao_samples` | `12` |
| `r_ssao_upscale_nearest` | `0` |
| `r_telealpha` | `0` |
| `r_teleportfx` | `1` |
| `r_teleportfx_time` | `0.35` |
| `r_tonemap` | `2` |
| `r_tonemap_black_lift` | `0.0` |
| `r_tonemap_black_lift_strength` | `1.0` |
| `r_tonemap_exposure` | `1.0` |
| `r_vignette` | `0.15` |
| `r_vignette_blend_mode` | `0` |
| `r_vignette_color_b` | `0.0` |
| `r_vignette_color_g` | `0.0` |
| `r_vignette_color_r` | `0.0` |
| `r_vignette_falloff` | `2.0` |
| `r_vignette_noise` | `0.0` |
| `r_vignette_radius_inner` | `0.8` |
| `r_vignette_radius_outer` | `2.0` |
| `r_wateralpha` | `1` |

## Quake/gl_screen.c

**Use case:** HUD/view presentation and screenshot behavior.

| CVar | Default |
|---|---:|
| `cl_gun_fovscale` | `1` |
| `cl_gun_x` | `0` |
| `cl_gun_y` | `0` |
| `cl_gun_z` | `0` |
| `cl_screenshotname` | `screenshots/%map%_%date%_%time%` |
| `fov` | `90` |
| `fov_adapt` | `1` |
| `gl_triplebuffer` | `1` |
| `hudstyle` | `2` |
| `scr_centerprintbg` | `0` |
| `scr_centertime` | `2` |
| `scr_clock` | `0` |
| `scr_conscale` | `1` |
| `scr_conspeed` | `2000` |
| `scr_conwidth` | `0` |
| `scr_crosshairscale` | `1` |
| `scr_demobar_timeout` | `1` |
| `scr_menubgalpha` | `0.7` |
| `scr_menubgstyle` | `-1` |
| `scr_menuscale` | `1` |
| `scr_pixelaspect` | `1` |
| `scr_printspeed` | `8` |
| `scr_sbaralpha` | `0.75` |
| `scr_sbarscale` | `1` |
| `scr_showfps` | `0` |
| `scr_showspeed` | `0` |
| `scr_showspeed_ofs` | `0` |
| `scr_usekfont` | `0` |
| `showpause` | `1` |
| `showturtle` | `0` |
| `viewsize` | `100` |
| `zoom_fov` | `30` |
| `zoom_speed` | `8` |

## Quake/gl_sky.c

**Use case:** Sky rendering quality and cloud layers.

| CVar | Default |
|---|---:|
| `r_fastsky` | `0` |
| `r_skyalpha` | `1` |
| `r_skyfog` | `0.5` |
| `r_skywind` | `1` |

## Quake/gl_texmgr.c

**Use case:** Texture manager filtering, anisotropy, and upload behavior.

| CVar | Default |
|---|---:|
| `gl_compress_textures` | `0` |
| `gl_legacy_palettes` | `0` |
| `gl_lodbias` | `auto` |
| `gl_max_size` | `0` |
| `gl_picmip` | `0` |
| `gl_texture_anisotropy` | `8` |
| `gl_texturemode` | `` |
| `r_bc7_compress` | `0` |
| `r_softemu` | `0` |
| `r_softemu_dither_screen` | `1.0` |
| `r_softemu_dither_texture` | `1.0` |
| `r_softemu_lightmap_banding` | `-1` |
| `r_softemu_mdl_warp` | `-1` |
| `r_softemu_metric` | `-1` |

## Quake/gl_vidsdl.c

**Use case:** Video mode/window and display properties.

| CVar | Default |
|---|---:|
| `contrast` | `1` |
| `gamma` | `1` |
| `vid_borderless` | `0` |
| `vid_desktopfullscreen` | `0` |
| `vid_fsaa` | `0` |
| `vid_fsaamode` | `0` |
| `vid_fullscreen` | `0` |
| `vid_height` | `600` |
| `vid_refreshrate` | `60` |
| `vid_saveresize` | `0` |
| `vid_vsync` | `0` |
| `vid_width` | `800` |

## Quake/gl_warp.c

**Use case:** Warp/liquid visual effect intensity.

| CVar | Default |
|---|---:|
| `r_waterwarp` | `1` |

## Quake/host.c

**Use case:** Engine session behavior, developer/debug toggles, and high-level gameplay options.

| CVar | Default |
|---|---:|
| `bloodhound` | `0` |
| `campaign` | `0` |
| `cl_nocsqc` | `0` |
| `cl_titlestats` | `1` |
| `coop` | `0` |
| `deathmatch` | `0` |
| `developer` | `0` |
| `devstats` | `0` |
| `fraglimit` | `0` |
| `horde` | `0` |
| `host_framerate` | `0` |
| `host_maxfps` | `250` |
| `host_speeds` | `0` |
| `host_timescale` | `0` |
| `map_checks` | `0` |
| `max_edicts` | `16384` |
| `noexit` | `0` |
| `pausable` | `1` |
| `samelevel` | `0` |
| `serverprofile` | `0` |
| `skill` | `1` |
| `sv_autosave` | `1` |
| `sv_autosave_interval` | `30` |
| `sv_cheats` | `0` |
| `sys_ticrate` | `0.05` |
| `teamplay` | `0` |
| `temp1` | `0` |
| `timelimit` | `0` |

## Quake/host_cmd.c

**Use case:** Host command safety/permissions behavior.

| CVar | Default |
|---|---:|
| `sv_autoload` | `2` |

## Quake/in_sdl.c

**Use case:** Input/controller and mouse behavior.

| CVar | Default |
|---|---:|
| `gyro_calibration_x` | `0` |
| `gyro_calibration_y` | `0` |
| `gyro_calibration_z` | `0` |
| `gyro_enable` | `1` |
| `gyro_mode` | `2` |
| `gyro_noise_thresh` | `1.5` |
| `gyro_pitchsensitivity` | `2.5` |
| `gyro_turning_axis` | `0` |
| `gyro_yawsensitivity` | `2.5` |
| `in_debugkeys` | `0` |
| `in_disablemacosxmouseaccel` | `1` |
| `joy_always_active` | `0` |
| `joy_deadzone_look` | `0.175` |
| `joy_deadzone_move` | `0.175` |
| `joy_deadzone_trigger` | `0.2` |
| `joy_device` | `0` |
| `joy_exponent` | `2` |
| `joy_exponent_move` | `2` |
| `joy_flick` | `0` |
| `joy_flick_adjust_speed` | `30.0` |
| `joy_flick_deadzone` | `0.9` |
| `joy_flick_noise_thresh` | `2.0` |
| `joy_flick_recenter` | `0.0` |
| `joy_flick_time` | `0.125` |
| `joy_invert` | `0` |
| `joy_outer_threshold_look` | `0.02` |
| `joy_outer_threshold_move` | `0.02` |
| `joy_rumble` | `0.3` |
| `joy_sensitivity_pitch` | `130` |
| `joy_sensitivity_yaw` | `240` |
| `joy_swapmovelook` | `0` |

## Quake/mat_shader.c

**Use case:** Material shader parsing/runtime feature controls.

| CVar | Default |
|---|---:|
| `r_matshader_debug_parse` | `0` |
| `r_matshader_fuzz` | `0` |
| `r_matshader_report` | `0` |
| `r_reloadshaders` | `0` |
| `r_shader_debug` | `0` |
| `r_shader_verbose` | `0` |
| `r_shaders` | `1` |
| `r_tcgen_debug` | `0` |

## Quake/menu.c

**Use case:** Menu UX behavior and accessibility preferences.

| CVar | Default |
|---|---:|
| `ui_live_preview` | `1` |
| `ui_mouse` | `1` |
| `ui_mouse_sound` | `0` |
| `ui_search_timeout` | `1` |
| `ui_sound_throttle` | `0.1` |

## Quake/net_main.c

**Use case:** Network transport fallback and diagnostics.

| CVar | Default |
|---|---:|
| `hostname` | `UNNAMED` |
| `net_messagetimeout` | `300` |

## Quake/pr_cmds.c

**Use case:** QC command behavior compatibility toggles.

| CVar | Default |
|---|---:|
| `pr_checkextension` | `1` |
| `sv_aim` | `1` |
| `sv_gameplayfix_random` | `1` |

## Quake/pr_edict.c

**Use case:** QC VM entity/edict limits and diagnostics.

| CVar | Default |
|---|---:|
| `gamecfg` | `0` |
| `nomonsters` | `0` |
| `saved1` | `0` |
| `saved2` | `0` |
| `saved3` | `0` |
| `saved4` | `0` |
| `savedgamecfg` | `0` |
| `scratch1` | `0` |
| `scratch2` | `0` |
| `scratch3` | `0` |
| `scratch4` | `0` |

## Quake/r_brush.c

**Use case:** Brush model rendering workarounds.

| CVar | Default |
|---|---:|
| `gl_lightmap_atlas_size` | `1024` |

## Quake/r_decals.c

**Use case:** Decal rendering limits and fade behavior.

| CVar | Default |
|---|---:|
| `r_decals` | `1` |
| `r_decals_bias` | `0.6` |
| `r_decals_debug` | `0` |
| `r_decals_drawdist` | `1536` |
| `r_decals_fade` | `6` |
| `r_decals_lifetime` | `30` |
| `r_decals_max` | `512` |
| `r_decals_on_liquids` | `0` |
| `r_decals_reload` | `0` |
| `r_decals_render_budget_decals` | `256` |
| `r_decals_spawn_budget` | `8` |

## Quake/r_dlight_pool.c

**Use case:** Dynamic light pool allocation/debug controls.

| CVar | Default |
|---|---:|
| `r_dlight_budget` | `64` |
| `r_dlight_cull_distance` | `0` |
| `r_dlight_hysteresis` | `6` |
| `r_dlight_min_brightness` | `0.02` |
| `r_dlight_min_radius` | `8` |
| `r_dlight_pool_max` | `512` |
| `r_dlight_smooth` | `0.25` |

## Quake/r_fogvol.c

**Use case:** Volumetric fog system quality and look.

| CVar | Default |
|---|---:|
| `r_fogvol` | `0` |
| `r_fogvol_halfres` | `0` |
| `r_fogvol_jitter` | `1` |
| `r_fogvol_noise` | `1` |
| `r_fogvol_noisemode` | `0` |
| `r_fogvol_physblend` | `1` |
| `r_fogvol_steps` | `32` |
| `r_fogvol_steps_scale_halfres` | `0.5` |
| `r_fogvol_temporal_alpha` | `0.9` |
| `r_fogvol_temporal_depth_reject` | `0.01` |
| `r_fogvol_testvolumes` | `0` |
| `r_fogvol_testvolumes_dumpstate` | `0` |
| `r_fogvol_upsample` | `1` |
| `r_fogvol_upsample_k` | `100` |
| `r_fogvol_upsample_taps` | `4` |

## Quake/r_maptex_export.c

**Use case:** Map texture export pipeline settings.

| CVar | Default |
|---|---:|
| `r_maptex_export` | `0` |
| `r_maptex_export_format` | `ktx2` |
| `r_maptex_export_mipmaps` | `1` |
| `r_maptex_export_overwrite` | `0` |
| `r_maptex_export_uastc` | `0` |
| `r_maptex_export_verbose` | `0` |

## Quake/r_part.c

**Use case:** Particle system rendering behavior.

| CVar | Default |
|---|---:|
| `r_particles` | `2` |

## Quake/simd_caps.c

**Use case:** SIMD capability override/debug toggles.

| CVar | Default |
|---|---:|
| `simd_enable` | `1` |
| `simd_force` | `0` |

## Quake/snd_dma.c

**Use case:** Sound mixer levels and quality settings.

| CVar | Default |
|---|---:|
| `_snd_mixahead` | `0.1` |
| `ambient_fade` | `100` |
| `ambient_level` | `0.3` |
| `bgmvolume` | `1` |
| `loadas8bit` | `0` |
| `nosound` | `0` |
| `precache` | `1` |
| `snd_filterquality` | `5` |
| `snd_mixspeed` | `44100` |
| `snd_noextraupdate` | `0` |
| `snd_show` | `0` |
| `snd_waterfx` | `1` |
| `sndspeed` | `11025` |
| `volume` | `0.7` |

## Quake/sv_main.c

**Use case:** Server visibility and message-rate behavior.

| CVar | Default |
|---|---:|
| `sv_netsort` | `1` |

## Quake/sv_phys.c

**Use case:** Server-side physics and movement behavior.

| CVar | Default |
|---|---:|
| `sv_freezenonclients` | `0` |
| `sv_friction` | `4` |
| `sv_gameplayfix_elevators` | `2` |
| `sv_gravity` | `800` |
| `sv_maxvelocity` | `2000` |
| `sv_nostep` | `0` |
| `sv_stopspeed` | `100` |

## Quake/sv_user.c

**Use case:** Server player movement command handling.

| CVar | Default |
|---|---:|
| `edgefriction` | `2` |
| `sv_accelerate` | `10` |
| `sv_altnoclip` | `1` |
| `sv_idealpitchscale` | `0.8` |
| `sv_maxspeed` | `320` |

## Quake/view.c

**Use case:** First-person camera, bob, kick, idle sway, and view effects.

| CVar | Default |
|---|---:|
| `cl_bob` | `0.02` |
| `cl_bobcycle` | `0.6` |
| `cl_bobup` | `0.5` |
| `cl_rollangle` | `2.0` |
| `cl_rollspeed` | `200` |
| `crosshair` | `0` |
| `gl_cshiftpercent` | `100` |
| `gl_cshiftpercent_bonus` | `100` |
| `gl_cshiftpercent_contents` | `100` |
| `gl_cshiftpercent_damage` | `100` |
| `gl_cshiftpercent_powerup` | `100` |
| `r_viewmodel_quake` | `0` |
| `scr_ofsx` | `0` |
| `scr_ofsy` | `0` |
| `scr_ofsz` | `0` |
| `v_centermove` | `0.15` |
| `v_centerspeed` | `500` |
| `v_explosionvibration` | `1` |
| `v_gunkick` | `2` |
| `v_gunsway` | `0` |
| `v_idlescale` | `0` |
| `v_ipitch_cycle` | `1` |
| `v_ipitch_level` | `0.3` |
| `v_iroll_cycle` | `0.5` |
| `v_iroll_level` | `0.1` |
| `v_iyaw_cycle` | `2` |
| `v_iyaw_level` | `0.3` |
| `v_kickpitch` | `0.6` |
| `v_kickroll` | `0.6` |
| `v_kicktime` | `0.5` |
| `v_weaponwhip` | `1` |
