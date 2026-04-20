// keep in sync with Misc/qs_pak/default.cfg

static const char default_cfg[] =
"unbindall\n"

"bind ALT +strafe\n"

"bind , +moveleft\n"
"bind a +moveleft\n"
"bind . +moveright\n"
"bind d +moveright\n"
"bind DEL +lookdown\n"
"bind PGDN +lookup\n"
"bind END centerview\n"

"bind e +moveup\n"
"bind c +movedown\n"
"bind SHIFT +speed\n"
"bind CTRL +attack\n"
"bind UPARROW +forward\n"
"bind w +forward\n"
"bind DOWNARROW +back\n"
"bind s +back\n"
"bind LEFTARROW +left\n"
"bind RIGHTARROW +right\n"

"bind SPACE +jump\n"

"bind TAB +showscores\n"

"bind 1 \"impulse 1\"\n"
"bind 2 \"impulse 2\"\n"
"bind 3 \"impulse 3\"\n"
"bind 4 \"impulse 4\"\n"
"bind 5 \"impulse 5\"\n"
"bind 6 \"impulse 6\"\n"
"bind 7 \"impulse 7\"\n"
"bind 8 \"impulse 8\"\n"

"bind 0 \"impulse 0\"\n"

"bind / \"impulse 10\"\n"
"bind MWHEELDOWN \"impulse 10\"\n"
"bind MWHEELUP \"impulse 12\"\n"

"alias zoom_in \"togglezoom\"\n"
"alias zoom_out \"togglezoom\"\n"
"bind F11 zoom_in\n"

"bind F1 \"help\"\n"
"bind F2 \"menu_save\"\n"
"bind F3 \"menu_load\"\n"
"bind F4 \"menu_options\"\n"
"bind F5 \"menu_multiplayer\"\n"
"bind F6 \"echo Quicksaving...; wait; save quick\"\n"
"bind F7 \"spec_prev\"\n"
"bind F8 \"spec_next\"\n"
"bind F9 \"echo Quickloading...; wait; load quick\"\n"
"bind F10 \"quit\"\n"
"bind F12 \"screenshot\"\n"

"bind PRINTSCREEN \"screenshot\"\n"

"bind \\ +mlook\n"

"bind PAUSE \"pause\"\n"
"bind ESCAPE \"togglemenu\"\n"
"bind ~ \"toggleconsole\"\n"
"bind ` \"toggleconsole\"\n"

"bind t \"messagemode\"\n"

"bind + \"sizeup\"\n"
"bind = \"sizeup\"\n"
"bind - \"sizedown\"\n"

"bind INS +klook\n"

"bind MOUSE1 +attack\n"
"bind MOUSE2 +jump\n"

"bind LSHOULDER \"impulse 12\"\n"
"bind RSHOULDER \"impulse 10\"\n"
"bind LTRIGGER +jump\n"
"bind RTRIGGER +attack\n"

"alias drs_resp_low \"r_drs_step_up 1; r_drs_step_down 1; r_drs_cooldown_after_down 12; r_drs_cooldown_after_up 4; r_drs_hysteresis_ms 0.7; echo DRS responsiveness: low (stable, slower reactions)\"\n"
"alias drs_resp_medium \"r_drs_step_up 1; r_drs_step_down 1; r_drs_cooldown_after_down 8; r_drs_cooldown_after_up 2; r_drs_hysteresis_ms 0.5; echo DRS responsiveness: medium (balanced)\"\n"
"alias drs_resp_high \"r_drs_step_up 1; r_drs_step_down 2; r_drs_cooldown_after_down 4; r_drs_cooldown_after_up 1; r_drs_hysteresis_ms 0.25; echo DRS responsiveness: high (fast reactions)\"\n"
"alias drs_profile_60_moderate \"r_drs 1; r_drs_target_fps 60; r_drs_target_ms 0; r_drs_min_scale 1; r_drs_max_scale 3; r_drs_guard_mode 1; drs_resp_medium; echo DRS profile: 60 FPS moderate stability\"\n"

"gamma 0.95\n"
"contrast 1.2\n"
"volume 0.7\n"
"sensitivity 3\n"

"viewsize 110\n"
"scr_autoscale\n"

"+mlook\n";
