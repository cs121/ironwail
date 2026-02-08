/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
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

#ifndef _QUAKE_PROTOCOL_H
#define _QUAKE_PROTOCOL_H

/* Q3MINI PLAN:
 * - Add a protocol flag to gate mini-Q3 netcode fields without breaking legacy parsing.
 * - Document the extended clc_move header for ack mask support.
 */

// protocol.h -- communications protocols

#define PROTOCOL_RMQ		999

// protocol flags (single modern protocol)
#define PRFL_SHORTANGLE		(1 << 1)
#define PRFL_INT32COORD		(1 << 7)
#define PRFL_SNAPSHOT_HIRES	(1 << 8)
#define PRFL_SIGNON_CHUNKS	(1 << 9)	// signon streaming via svc_signon_chunk
// Q3MINI BEGIN
#define PRFL_Q3MINI		(1 << 10)	// Q3MINI: mini Q3-style netcode extensions
// Q3MINI END
#define PROTOCOL_RMQ_FLAGS	(PRFL_INT32COORD | PRFL_SHORTANGLE | PRFL_SNAPSHOT_HIRES | PRFL_SIGNON_CHUNKS)

// snapshot delta field bits
#define SNAP_ORIGIN1	(1u<<0)
#define SNAP_ORIGIN2	(1u<<1)
#define SNAP_ORIGIN3	(1u<<2)
#define SNAP_ANGLE1		(1u<<3)
#define SNAP_ANGLE2		(1u<<4)
#define SNAP_ANGLE3		(1u<<5)
#define SNAP_MODEL		(1u<<6)
#define SNAP_FRAME		(1u<<7)
#define SNAP_COLORMAP	(1u<<8)
#define SNAP_SKIN		(1u<<9)
#define SNAP_EFFECTS	(1u<<10)
#define SNAP_ALPHA		(1u<<11)
#define SNAP_SCALE		(1u<<12)
#define SNAP_STEP		(1u<<13)
#define SNAP_HIRES_ORIGIN	(1u<<14)
#define SNAP_HIRES_ANGLES	(1u<<15)
#define SNAP_PLAYERSTATS	(1u<<16)
#define SNAP_PLAYERMOVE		(1u<<17)
#define SNAP_PLAYEREVENTS	(1u<<18)

#define SNAPFL_HIRES_ORIGIN	(1u<<2)
#define SNAPFL_HIRES_ANGLES	(1u<<3)

#define	SU_VIEWHEIGHT	(1<<0)
#define	SU_IDEALPITCH	(1<<1)
#define	SU_PUNCH1		(1<<2)
#define	SU_PUNCH2		(1<<3)
#define	SU_PUNCH3		(1<<4)
#define	SU_VELOCITY1	(1<<5)
#define	SU_VELOCITY2	(1<<6)
#define	SU_VELOCITY3	(1<<7)
#define	SU_PREDICT		(1<<8)  // client prediction data
#define	SU_ITEMS		(1<<9)
#define	SU_ONGROUND		(1<<10)	// no data follows, the bit is it
#define	SU_INWATER		(1<<11)	// no data follows, the bit is it
#define	SU_WEAPONFRAME	(1<<12)
#define	SU_ARMOR		(1<<13)
#define	SU_WEAPON		(1<<14)
#define SU_EXTEND1		(1<<15) // another byte to follow
#define SU_WEAPON2		(1<<16) // 1 byte, this is .weaponmodel & 0xFF00 (second byte)
#define SU_ARMOR2		(1<<17) // 1 byte, this is .armorvalue & 0xFF00 (second byte)
#define SU_AMMO2		(1<<18) // 1 byte, this is .currentammo & 0xFF00 (second byte)
#define SU_SHELLS2		(1<<19) // 1 byte, this is .ammo_shells & 0xFF00 (second byte)
#define SU_NAILS2		(1<<20) // 1 byte, this is .ammo_nails & 0xFF00 (second byte)
#define SU_ROCKETS2		(1<<21) // 1 byte, this is .ammo_rockets & 0xFF00 (second byte)
#define SU_CELLS2		(1<<22) // 1 byte, this is .ammo_cells & 0xFF00 (second byte)
#define SU_EXTEND2		(1<<23) // another byte to follow
#define SU_WEAPONFRAME2	(1<<24) // 1 byte, this is .weaponframe & 0xFF00 (second byte)
#define SU_WEAPONALPHA	(1<<25) // 1 byte, this is alpha for weaponmodel, uses ENTALPHA_ENCODE, not sent if ENTALPHA_DEFAULT
#define SU_UNUSED26		(1<<26)
#define SU_UNUSED27		(1<<27)
#define SU_UNUSED28		(1<<28)
#define SU_UNUSED29		(1<<29)
#define SU_UNUSED30		(1<<30)
#define SU_EXTEND3		(1<<31) // another byte to follow, future expansion

// a sound with no channel is a local only sound
#define	SND_VOLUME		(1<<0)	// a byte
#define	SND_ATTENUATION		(1<<1)	// a byte
#define	SND_LOOPING		(1<<2)	// a long

#define DEFAULT_SOUND_PACKET_VOLUME		255
#define DEFAULT_SOUND_PACKET_ATTENUATION	1.0

#define	SND_LARGEENTITY	(1<<3)	// a short + byte (instead of just a short)
#define	SND_LARGESOUND	(1<<4)	// a short soundindex (instead of a byte)

// flags for entity baseline messages
#define B_LARGEMODEL	(1<<0)	// modelindex is short instead of byte
#define B_LARGEFRAME	(1<<1)	// frame is short instead of byte
#define B_ALPHA			(1<<2)	// 1 byte, uses ENTALPHA_ENCODE, not sent if ENTALPHA_DEFAULT
#define B_SCALE			(1<<3)

// alpha encoding
#define ENTALPHA_DEFAULT	0	//entity's alpha is "default" (i.e. water obeys r_wateralpha) -- must be zero so zeroed out memory works
#define ENTALPHA_ZERO		1	//entity is invisible (lowest possible alpha)
#define ENTALPHA_ONE		255 //entity is fully opaque (highest possible alpha)
#define ENTALPHA_OPAQUE(a)	((byte)((a)-1)>=254) //true if entity is opaque (alpha==0 or alpha==255)
#define ENTALPHA_ENCODE(a)	(((a)==0)?ENTALPHA_DEFAULT:Q_rint(CLAMP(1.0f,(a)*254.0f+1,255.0f))) //server convert to byte to send to client
#define ENTALPHA_DECODE(a)	(((a)==ENTALPHA_DEFAULT)?1.0f:((float)(a)-1)/(254)) //client convert to float for rendering
#define ENTALPHA_TOSAVE(a)	(((a)==ENTALPHA_DEFAULT)?0.0f:(((a)==ENTALPHA_ZERO)?-1.0f:((float)(a)-1)/(254))) //server convert to float for savegame
//johnfitz

#define ENTSCALE_DEFAULT	16 // Equivalent to float 1.0f due to byte packing.
#define ENTSCALE_ENCODE(a)	((a) ? ((a) * ENTSCALE_DEFAULT) : ENTSCALE_DEFAULT) // Convert to byte
#define ENTSCALE_DECODE(a)	((float)(a) / ENTSCALE_DEFAULT) // Convert to float for rendering

// defaults for clientinfo messages
#define	DEFAULT_VIEWHEIGHT	22

// game types sent by serverinfo
// these determine which intermission screen plays
#define	GAME_COOP			0
#define	GAME_DEATHMATCH		1

//==================
// note that there are some defs.qc that mirror to these numbers
// also related to svc_strings[] in cl_parse
//==================

//
// server to client
//
#define	svc_bad					0
#define	svc_nop					1
#define	svc_disconnect			2
#define	svc_updatestat			3	// [byte] [long]
#define	svc_version				4	// [long] server version
#define	svc_setview				5	// [short] entity number
#define	svc_sound				6	// <see code>
#define	svc_time				7	// [float] server time
#define	svc_print				8	// [string] null terminated string
#define	svc_stufftext			9	// [string] stuffed into client's console buffer
									// the string should be \n terminated
#define	svc_setangle			10	// [angle3] set the view angle to this absolute value
#define	svc_serverinfo			11	// [long] version
									// [string] signon string
									// [string]..[0]model cache
									// [string]...[0]sounds cache
#define	svc_lightstyle			12	// [byte] [string]
#define	svc_updatename			13	// [byte] [string]
#define	svc_updatefrags			14	// [byte] [short]
#define	svc_clientdata			15	// <shortbits + data>
#define	svc_stopsound			16	// <see code>
#define	svc_updatecolors		17	// [byte] [byte]
#define	svc_particle			18	// [vec3] <variable>
#define	svc_damage				19
#define	svc_spawnstatic			20
//#define svc_spawnbinary		21
#define	svc_spawnbaseline		22
#define	svc_temp_entity			23
#define	svc_setpause			24	// [byte] on / off
#define	svc_signonnum			25	// [byte]  used for the signon sequence
#define	svc_centerprint			26	// [string] to put in center of the screen
#define	svc_killedmonster		27
#define	svc_foundsecret			28
#define	svc_spawnstaticsound	29	// [coord3] [byte] samp [byte] vol [byte] aten
#define	svc_intermission		30	// [string] music
#define	svc_finale				31	// [string] music [string] text
#define	svc_cdtrack				32	// [byte] track [byte] looptrack
#define svc_sellscreen			33
#define svc_cutscene			34

#define	svc_skybox				37	// [string] name
#define svc_bf					40
#define svc_fog					41	// [byte] density [byte] red [byte] green [byte] blue [float] time
#define svc_spawnbaseline2		42  // support for large modelindex, large framenum, alpha, using flags
#define svc_spawnstatic2		43	// support for large modelindex, large framenum, alpha, using flags
#define	svc_spawnstaticsound2	44	// [coord3] [short] samp [byte] vol [byte] aten

// 2021 re-release server messages - see:
// https://steamcommunity.com/sharedfiles/filedetails/?id=2679459726
#define svc_botchat		38
#define svc_setviews		45
#define svc_updateping		46
#define svc_updatesocial	47
#define svc_updateplinfo	48
#define svc_rawprint		49
#define svc_servervars		50
#define svc_seq			51
// Note: svc_achievement has same value as svcdp_effect!
#define svc_achievement		52	// [string] id
#define svc_chat		53
#define svc_levelcompleted	54
#define svc_backtolobby		55
#define svc_localsound		56
#define svc_snapshot_full	57
#define svc_snapshot_delta	58
#define svc_snapshot2		59
#define svc_signon_chunk	60	// [byte stage] [short seq] [byte flags] [short payload_len] [payload bytes]

// Signon chunk format (PRFL_SIGNON_CHUNKS):
//   svc_signon_chunk
//     stage: signon_stage_t
//     seq:   monotonically increasing sequence number
//     flags: SIGNON_CHUNK_FLAG_* (stage end, etc.)
//     payload_len: size of payload bytes that follow
//     payload: one or more signon records (SIGNON_REC_*)

#define SNAPSHOT_FLAG_FULL		(1u << 0)
#define SNAPSHOT_FLAG_DELTA		(1u << 1)
#define SNAPSHOT_FLAG_HAS_REMOVE_LIST	(1u << 2)
#define SNAPSHOT_FLAG_CONTINUE		(1u << 3)
#define SNAPSHOT_FLAG_INCOMPLETE	(1u << 4)
#define SNAPSHOT_FLAG_CHUNKINFO		(1u << 5)

#define SNAP_VALID_MASK		(SNAP_ORIGIN1 | SNAP_ORIGIN2 | SNAP_ORIGIN3 | SNAP_ANGLE1 | \
				 SNAP_ANGLE2 | SNAP_ANGLE3 | SNAP_MODEL | SNAP_FRAME | SNAP_COLORMAP | \
				 SNAP_SKIN | SNAP_EFFECTS | SNAP_ALPHA | SNAP_SCALE | SNAP_STEP | \
				 SNAP_HIRES_ORIGIN | SNAP_HIRES_ANGLES | SNAP_PLAYERSTATS | SNAP_PLAYERMOVE | \
				 SNAP_PLAYEREVENTS)

#define SNAP_PM_ONGROUND	(1u<<0)
#define SNAP_PM_INWATER		(1u<<1)

// signon chunk protocol extension
#define SIGNON_CHUNK_FLAG_STAGE_END	(1u << 0)

// signon record payload types
#define SIGNON_REC_RAW		0	// [short rec_id] [short len] [len bytes raw svc command]
#define SIGNON_REC_RAW_FRAG	1	// [short rec_id] [short total_len] [short offset] [short len] [len bytes]

typedef enum
{
	SIGNON_STAGE_PRECACHES = 0,
	SIGNON_STAGE_BASELINES,
	SIGNON_STAGE_STATIC_ENTS,
	SIGNON_STAGE_LIGHTSTYLES,
	SIGNON_STAGE_CUSTOM_EXT,
	SIGNON_STAGE_FINAL,
	SIGNON_STAGE_COUNT
} signon_stage_t;

#define PACKEDENT_MASK_EXTEND	0x8000u
#define PACKEDENT_MASK_MODEL	(1u << 0)
#define PACKEDENT_MASK_FRAME	(1u << 1)
#define PACKEDENT_MASK_COLORMAP	(1u << 2)
#define PACKEDENT_MASK_SKIN	(1u << 3)
#define PACKEDENT_MASK_EFFECTS	(1u << 4)
#define PACKEDENT_MASK_ORIGIN_X	(1u << 5)
#define PACKEDENT_MASK_ORIGIN_Y	(1u << 6)
#define PACKEDENT_MASK_ORIGIN_Z	(1u << 7)
#define PACKEDENT_MASK_ANGLE_PITCH	(1u << 8)
#define PACKEDENT_MASK_ANGLE_YAW	(1u << 9)
#define PACKEDENT_MASK_ANGLE_ROLL	(1u << 10)
#define PACKEDENT_MASK_VEL_X	(1u << 11)
#define PACKEDENT_MASK_VEL_Y	(1u << 12)
#define PACKEDENT_MASK_VEL_Z	(1u << 13)
#define PACKEDENT_MASK_ALPHA	(1u << 14)
#define PACKEDENT_MASK_SCALE	(1u << 16)
#define PACKEDENT_MASK_STEP	(1u << 17)
#define PACKEDENT_MASK_LERPFINISH	(1u << 18)
#define PACKEDENT_MASK_POS_FULL	(1u << 19)
#define PACKEDENT_MASK_VALID	(PACKEDENT_MASK_MODEL | PACKEDENT_MASK_FRAME | PACKEDENT_MASK_COLORMAP | \
				 PACKEDENT_MASK_SKIN | PACKEDENT_MASK_EFFECTS | PACKEDENT_MASK_ORIGIN_X | \
				 PACKEDENT_MASK_ORIGIN_Y | PACKEDENT_MASK_ORIGIN_Z | PACKEDENT_MASK_ANGLE_PITCH | \
				 PACKEDENT_MASK_ANGLE_YAW | PACKEDENT_MASK_ANGLE_ROLL | PACKEDENT_MASK_VEL_X | \
				 PACKEDENT_MASK_VEL_Y | PACKEDENT_MASK_VEL_Z | PACKEDENT_MASK_ALPHA | \
				 PACKEDENT_MASK_SCALE | PACKEDENT_MASK_STEP | PACKEDENT_MASK_LERPFINISH | \
				 PACKEDENT_MASK_POS_FULL)

#define PACKEDENT_POS_SCALE	16.0f	// 12.4 int16 range ~+-2048; PACKEDENT_MASK_POS_FULL falls back to MSG_WriteCoord.
#define PACKEDENT_VEL_SCALE	8.0f
#define PACKEDENT_ANGLE_SCALE	(65536.0f / 360.0f)

#define MAX_SNAPSHOT_ENTITIES	MAX_EDICTS
#define MAX_REMOVE_ENTITIES	MAX_EDICTS
#define MAX_PACKED_FIELDS	32
#define MAX_CMDS_PER_PACKET	64
#define MAX_SNAPSHOT_PARSE_ERRORS	5

//
// client to server
//
#define	clc_bad			0
#define	clc_nop 		1
#define	clc_disconnect	2
#define	clc_move		3		// [float mtime] [long cmd_seq] [long cmd_ack] [byte count] [usercmd_t]
						// RMQ: cmd_seq/cmd_ack are uint32 on wire; compare with NETSEQ_GT for wrap safety.
						// Q3MINI: [long srv_ack] [long srv_ack_mask] when PRFL_Q3MINI && net_ackmask.
#define	clc_stringcmd	4		// [string] message
#define	clc_snapshot_ack	5	// [long] seq
#define	clc_snapshot_nak	6	// [long] expected base [long] received base
#define	clc_signon_ack	7	// [byte stage] [short next_seq_expected]

//
// temp entity events
//
#define	TE_SPIKE			0
#define	TE_SUPERSPIKE		1
#define	TE_GUNSHOT			2
#define	TE_EXPLOSION		3
#define	TE_TAREXPLOSION		4
#define	TE_LIGHTNING1		5
#define	TE_LIGHTNING2		6
#define	TE_WIZSPIKE			7
#define	TE_KNIGHTSPIKE		8
#define	TE_LIGHTNING3		9
#define	TE_LAVASPLASH		10
#define	TE_TELEPORT			11
#define TE_EXPLOSION2		12

// PGM 01/21/97
#define TE_BEAM				13
// PGM 01/21/97

typedef struct
{
	vec3_t		origin;
	vec3_t		angles;
	unsigned short 	modelindex;	//johnfitz -- was int
	unsigned short 	frame;		//johnfitz -- was int
	unsigned char 	colormap;	//johnfitz -- was int
	unsigned char 	skin;		//johnfitz -- was int
	unsigned char	alpha;		//johnfitz -- added
	unsigned char	scale;		//Quakespasm: for model scale support.
	int		effects;
} entity_state_t;

typedef struct
{
	entity_state_t	state;
	byte		step;
	short		health;
	short		armor;
	short		ammo;
	unsigned short	weaponmodel;
	unsigned short	weaponframe;
	unsigned int	items;
	unsigned int	activeweapon;
	byte		ammo_shells;
	byte		ammo_nails;
	byte		ammo_rockets;
	byte		ammo_cells;
	char		viewheight;
	char		idealpitch;
	char		punchangle[3];
	char		velocity[3];
	byte		movetype;
	byte		pm_flags;
	byte		waterlevel;
	byte		watertype;
	byte		event;
	byte		event_param;
	unsigned short	event_seq;
} snapshot_state_t;

typedef struct
{
	unsigned int	sequence;
	vec3_t	viewangles;

// intended velocities
	float	forwardmove;
	float	sidemove;
	float	upmove;
	byte	buttons;
	byte	impulse;
} usercmd_t;

#define CMD_BACKUP	2
#define CMD_RING	64

#endif	/* _QUAKE_PROTOCOL_H */
