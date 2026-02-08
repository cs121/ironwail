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
// cl.input.c  -- builds an intended movement command to send to the server

// Quake is a trademark of Id Software, Inc., (c) 1996 Id Software, Inc. All
// rights reserved.

/* Q3MINI PLAN:
 * - Add cmd redundancy + ackmask fields to clc_move.
 * - Provide packet-size estimation for client throttling.
 */

#include "quakedef.h"
#include "arch_def.h"
#include "net_sys.h"
#include "net_defs.h"

extern cvar_t cl_maxpitch; //johnfitz -- variable pitch clamping
extern cvar_t cl_minpitch; //johnfitz -- variable pitch clamping
extern cvar_t cl_mwheelpitch;
extern cvar_t cl_netdebug_parse;
extern cvar_t cl_cmd_maxbatch;

// Q3MINI BEGIN
static qboolean CL_Q3Mini_Enabled (void)
{
	return (cl.protocolflags & PRFL_Q3MINI) != 0;
}

static int CL_Q3Mini_Redundancy (void)
{
	int redundancy = (int)cl_cmd_redundancy.value;

	if (redundancy < 0)
		redundancy = 0;
	if (redundancy > 8)
		redundancy = 8;
	return redundancy;
}
// Q3MINI END

/*
===============================================================================

KEY BUTTONS

Continuous button event tracking is complicated by the fact that two different
input sources (say, mouse button 1 and the control key) can both press the
same button, but the button should only be released when both of the
pressing key have been released.

When a key event issues a button command (+forward, +attack, etc), it appends
its key number as a parameter to the command so it can be matched up with
the release.

state bit 0 is the current state of the key
state bit 1 is edge triggered on the up to down transition
state bit 2 is edge triggered on the down to up transition

===============================================================================
*/


kbutton_t	in_mlook, in_klook;
kbutton_t	in_left, in_right, in_forward, in_back;
kbutton_t	in_lookup, in_lookdown, in_moveleft, in_moveright;
kbutton_t	in_strafe, in_speed, in_use, in_jump, in_attack;
kbutton_t	in_up, in_down;

int			in_impulse;


void KeyDown (kbutton_t *b)
{
	int		k;
	const char	*c;

	c = Cmd_Argv(1);
	if (c[0])
		k = atoi(c);
	else
		k = -1;		// typed manually at the console for continuous down

	if (k == b->down[0] || k == b->down[1])
		return;		// repeating key

	if (!b->down[0])
		b->down[0] = k;
	else if (!b->down[1])
		b->down[1] = k;
	else
	{
		Con_Printf ("Three keys down for a button!\n");
		return;
	}

	if (b->state & 1)
		return;		// still down
	b->state |= 1 + 2;	// down + impulse down
}

void KeyUp (kbutton_t *b)
{
	int		k;
	const char	*c;

	c = Cmd_Argv(1);
	if (c[0])
		k = atoi(c);
	else
	{ // typed manually at the console, assume for unsticking, so clear all
		b->down[0] = b->down[1] = 0;
		b->state = 4;	// impulse up
		return;
	}

	if (b->down[0] == k)
		b->down[0] = 0;
	else if (b->down[1] == k)
		b->down[1] = 0;
	else
		return;		// key up without coresponding down (menu pass through)
	if (b->down[0] || b->down[1])
		return;		// some other key is still holding it down

	if (!(b->state & 1))
		return;		// still up (this should not happen)
	b->state &= ~1;		// now up
	b->state |= 4; 		// impulse up
}

void IN_AccumMWheelPitch (float amt)
{
	int key;
	if (key_dest != key_game || Cmd_Argc() < 2)
		return;
	key = atoi (Cmd_Argv (1));
	if (key == K_MWHEELDOWN || key == K_MWHEELUP)
	{
		cl.wheel_pitch += amt;
		cl.wheel_pitch = CLAMP (-90.f, cl.wheel_pitch, 90.f);
	}
}

void IN_KLookDown (void) {KeyDown(&in_klook);}
void IN_KLookUp (void) {KeyUp(&in_klook);}
void IN_MLookDown (void) {KeyDown(&in_mlook);}
void IN_MLookUp (void) {
	KeyUp(&in_mlook);
	if ( !((in_mlook.state & 1) || freelook.value) && lookspring.value)
		V_StartPitchDrift();
}
void IN_UpDown(void) {KeyDown(&in_up);}
void IN_UpUp(void) {KeyUp(&in_up);}
void IN_DownDown(void) {KeyDown(&in_down);}
void IN_DownUp(void) {KeyUp(&in_down);}
void IN_LeftDown(void) {KeyDown(&in_left);}
void IN_LeftUp(void) {KeyUp(&in_left);}
void IN_RightDown(void) {KeyDown(&in_right);}
void IN_RightUp(void) {KeyUp(&in_right);}
void IN_ForwardDown(void) {KeyDown(&in_forward);}
void IN_ForwardUp(void) {KeyUp(&in_forward);}
void IN_BackDown(void) {KeyDown(&in_back);}
void IN_BackUp(void) {KeyUp(&in_back);}
void IN_LookupDown(void) {KeyDown(&in_lookup);}
void IN_LookupUp(void) {KeyUp(&in_lookup); IN_AccumMWheelPitch(-cl_mwheelpitch.value);}
void IN_LookdownDown(void) {KeyDown(&in_lookdown);}
void IN_LookdownUp(void) {KeyUp(&in_lookdown); IN_AccumMWheelPitch(cl_mwheelpitch.value);}
void IN_MoveleftDown(void) {KeyDown(&in_moveleft);}
void IN_MoveleftUp(void) {KeyUp(&in_moveleft);}
void IN_MoverightDown(void) {KeyDown(&in_moveright);}
void IN_MoverightUp(void) {KeyUp(&in_moveright);}

void IN_SpeedDown(void) {KeyDown(&in_speed);}
void IN_SpeedUp(void) {KeyUp(&in_speed);}
void IN_StrafeDown(void) {KeyDown(&in_strafe);}
void IN_StrafeUp(void) {KeyUp(&in_strafe);}

void IN_AttackDown(void) {KeyDown(&in_attack);}
void IN_AttackUp(void) {KeyUp(&in_attack);}

void IN_UseDown (void) {KeyDown(&in_use);}
void IN_UseUp (void) {KeyUp(&in_use);}
void IN_JumpDown (void) {KeyDown(&in_jump);}
void IN_JumpUp (void) {KeyUp(&in_jump);}

void IN_Impulse (void) {in_impulse=Q_atoi(Cmd_Argv(1));}

/*
===============
CL_KeyState

Returns 0.25 if a key was pressed and released during the frame,
0.5 if it was pressed and held
0 if held then released, and
1.0 if held for the entire time
===============
*/
float CL_KeyState (kbutton_t *key)
{
	float		val;
	qboolean	impulsedown, impulseup, down;

	impulsedown = key->state & 2;
	impulseup = key->state & 4;
	down = key->state & 1;
	val = 0;

	if (impulsedown && !impulseup)
	{
		if (down)
			val = 0.5;	// pressed and held this frame
		else
			val = 0;	//	I_Error ();
	}
	if (impulseup && !impulsedown)
	{
		if (down)
			val = 0;	//	I_Error ();
		else
			val = 0;	// released this frame
	}
	if (!impulsedown && !impulseup)
	{
		if (down)
			val = 1.0;	// held the entire frame
		else
			val = 0;	// up the entire frame
	}
	if (impulsedown && impulseup)
	{
		if (down)
			val = 0.75;	// released and re-pressed this frame
		else
			val = 0.25;	// pressed and released this frame
	}

	key->state &= 1;		// clear impulses

	return val;
}


//==========================================================================

cvar_t	cl_upspeed = {"cl_upspeed","200",CVAR_NONE};
cvar_t	cl_forwardspeed = {"cl_forwardspeed","200", CVAR_ARCHIVE};
cvar_t	cl_backspeed = {"cl_backspeed","200", CVAR_ARCHIVE};
cvar_t	cl_sidespeed = {"cl_sidespeed","350",CVAR_NONE};

cvar_t	cl_movespeedkey = {"cl_movespeedkey","2.0",CVAR_NONE};

cvar_t	cl_yawspeed = {"cl_yawspeed","140",CVAR_NONE};
cvar_t	cl_pitchspeed = {"cl_pitchspeed","150",CVAR_NONE};

cvar_t	cl_anglespeedkey = {"cl_anglespeedkey","1.5",CVAR_NONE};

cvar_t	cl_alwaysrun = {"cl_alwaysrun","1",CVAR_ARCHIVE}; // QuakeSpasm -- new always run

/*
================
CL_InCutscene
================
*/
qboolean CL_InCutscene (void)
{
	return cl.fixangle && !cl.viewent.model;
}

/*
================
CL_AdjustAngles

Moves the local angle positions
================
*/
void CL_AdjustAngles (void)
{
	float	speed;
	float	up, down;

	if (CL_InCutscene ())
		return;

	if ((in_speed.state & 1) ^ (cl_alwaysrun.value != 0.0))
		speed = host_frametime * cl_anglespeedkey.value;
	else
		speed = host_frametime;

	if (!(in_strafe.state & 1))
	{
		cl.viewangles[YAW] -= speed*cl_yawspeed.value*CL_KeyState (&in_right);
		cl.viewangles[YAW] += speed*cl_yawspeed.value*CL_KeyState (&in_left);
		cl.viewangles[YAW] = NormalizeAngle180 (cl.viewangles[YAW]);
	}
	if (in_klook.state & 1)
	{
		V_StopPitchDrift ();
		cl.viewangles[PITCH] -= speed*cl_pitchspeed.value * CL_KeyState (&in_forward);
		cl.viewangles[PITCH] += speed*cl_pitchspeed.value * CL_KeyState (&in_back);
	}

	up = CL_KeyState (&in_lookup);
	down = CL_KeyState(&in_lookdown);

	cl.viewangles[PITCH] -= speed*cl_pitchspeed.value * up;
	cl.viewangles[PITCH] += speed*cl_pitchspeed.value * down;

	if (up || down || cl.wheel_pitch)
		V_StopPitchDrift ();

	if (cl.wheel_pitch)
	{
		float delta = speed*cl_pitchspeed.value;
		if (cl.wheel_pitch > 0.f)
		{
			cl.viewangles[PITCH] += q_min (delta, cl.wheel_pitch);
			cl.wheel_pitch -= delta;
			cl.wheel_pitch = q_max (0.f, cl.wheel_pitch);
		}
		else
		{
			cl.viewangles[PITCH] -= q_min (delta, -cl.wheel_pitch);
			cl.wheel_pitch += delta;
			cl.wheel_pitch = q_min (0.f, cl.wheel_pitch);
		}
	}

	//johnfitz -- variable pitch clamping
	if (cl.viewangles[PITCH] > cl_maxpitch.value)
		cl.viewangles[PITCH] = cl_maxpitch.value;
	if (cl.viewangles[PITCH] < cl_minpitch.value)
		cl.viewangles[PITCH] = cl_minpitch.value;
	//johnfitz

	if (cl.viewangles[ROLL] > 50)
		cl.viewangles[ROLL] = 50;
	if (cl.viewangles[ROLL] < -50)
		cl.viewangles[ROLL] = -50;
}

/*
================
CL_BaseMove

Send the intended movement message to the server
================
*/
void CL_BaseMove (usercmd_t *cmd)
{
	if (cls.signon != SIGNONS)
		return;

	Q_memset (cmd, 0, sizeof(*cmd));

	if (in_strafe.state & 1)
	{
		cmd->sidemove += cl_sidespeed.value * CL_KeyState (&in_right);
		cmd->sidemove -= cl_sidespeed.value * CL_KeyState (&in_left);
	}

	cmd->sidemove += cl_sidespeed.value * CL_KeyState (&in_moveright);
	cmd->sidemove -= cl_sidespeed.value * CL_KeyState (&in_moveleft);

	cmd->upmove += cl_upspeed.value * CL_KeyState (&in_up);
	cmd->upmove -= cl_upspeed.value * CL_KeyState (&in_down);

	if (! (in_klook.state & 1) )
	{
		cmd->forwardmove += cl_forwardspeed.value * CL_KeyState (&in_forward);
		cmd->forwardmove -= cl_backspeed.value * CL_KeyState (&in_back);
	}

//
// adjust for speed key
//
	if ((in_speed.state & 1) ^ (cl_alwaysrun.value != 0.0))
	{
		cmd->forwardmove *= cl_movespeedkey.value;
		cmd->sidemove *= cl_movespeedkey.value;
		cmd->upmove *= cl_movespeedkey.value;
	}
}


/*
==============
CL_SendMove
==============
*/
void CL_SendMove (const usercmd_t *cmd)
{
	int		i;
	int		cmd_count;
	int		j;
	sizebuf_t	buf;
	byte	data[128];
	usercmd_t	cmd_buf[CMD_BACKUP + 1];

	buf.maxsize = 128;
	buf.cursize = 0;
	buf.data = data;
	buf.allowoverflow = false;
	buf.overflowed = false;
	buf.overflowed_once = false;
	buf.write_blocked = false;
	buf.write_locked = false;
	buf.blocked_file = NULL;
	buf.blocked_line = 0;
	buf.dbg_name = "cl_move";
	buf.dbg_file = NULL;
	buf.dbg_line = 0;
	buf.dbg_msgkind = 0;
	buf.dbg_id = 0;
	buf.dbg_aux = 0;
	buf.bitpos = 0;

	if (cmd) 
	{
		cl.cmd = *cmd;

	//
	// send the movement message
	//
		MSG_WriteByte (&buf, clc_move);

		MSG_WriteFloat (&buf, cl.mtime[0]);	// so server can get ping times
		// RMQ wire format: cmd_seq/cmd_ack are 32-bit unsigned (wrap-safe via NETSEQ_GT).
		MSG_WriteLong (&buf, (int)cmd->sequence);
		MSG_WriteLong (&buf, (int)cl.last_cmd_ack);
		// Q3MINI BEGIN
		if (CL_Q3Mini_Enabled () && net_ackmask.value > 0.0f)
		{
			unsigned int srv_ack = cl.q3mini_srv_ack_valid ? cl.q3mini_srv_ack : 0u;
			unsigned int srv_ack_mask = cl.q3mini_srv_ack_valid ? cl.q3mini_srv_ack_mask : 0u;

			MSG_WriteLong (&buf, (int)srv_ack);
			MSG_WriteLong (&buf, (int)srv_ack_mask);
		}
		// Q3MINI END

		cmd_count = 0;
		{
			int maxbatch = (int)cl_cmd_maxbatch.value;
			int redundancy = CL_Q3Mini_Redundancy ();
			int desired = redundancy + 1;

			if (maxbatch < 1)
				maxbatch = 1;
			if (maxbatch > MAX_CMDS_PER_PACKET)
				maxbatch = MAX_CMDS_PER_PACKET;
			if (maxbatch > desired)
				maxbatch = desired;

			for (i = redundancy; i >= 0 && cmd_count < maxbatch; i--)
			{
				unsigned int seq = cmd->sequence - (unsigned int)i;

				if (CL_Predict_GetCmd (seq, &cmd_buf[cmd_count]))
					cmd_count++;
			}
		}

		MSG_WriteByte (&buf, cmd_count);
		for (i = 0; i < cmd_count; i++)
		{
			const usercmd_t *out = &cmd_buf[i];

			MSG_WriteLong (&buf, (int)out->sequence);
			for (j=0 ; j<3 ; j++)
				MSG_WriteAngle16 (&buf, out->viewangles[j], cl.protocolflags);

			MSG_WriteShort (&buf, out->forwardmove);
			MSG_WriteShort (&buf, out->sidemove);
			MSG_WriteShort (&buf, out->upmove);
			MSG_WriteByte (&buf, out->buttons);
			MSG_WriteByte (&buf, out->impulse);
		}

		if (cl_netdebug_parse.value)
		{
			unsigned int reliable_seq = cls.netcon ? cls.netcon->sendSequence : 0u;
			unsigned int reliable_base = cls.netcon ? cls.netcon->sendReliableBase : 0u;
			Con_Printf ("NETDBG time %.3f cmd_move_write msg %d type %d flags 0x%x cmd_seq %u "
				"cmd_ack %u snap_ack %u rel_seq %u rel_base %u widths[mtime=float cmd_seq=long "
				"cmd_ack=long cmd_count=byte ucmd_seq=long angles=short moves=short buttons=byte impulse=byte]\n",
				realtime, buf.cursize, clc_move, cl.protocolflags, cmd->sequence,
				cl.last_cmd_ack, cl.last_snapshot_ack_sent, reliable_seq, reliable_base);
			JITTER_LOG ("NETDBG time %.3f cmd_move_write msg %d type %d flags 0x%x cmd_seq %u "
				"cmd_ack %u snap_ack %u rel_seq %u rel_base %u widths[mtime=float cmd_seq=long "
				"cmd_ack=long cmd_count=byte ucmd_seq=long angles=short moves=short buttons=byte impulse=byte]\n",
				realtime, buf.cursize, clc_move, cl.protocolflags, cmd->sequence,
				cl.last_cmd_ack, cl.last_snapshot_ack_sent, reliable_seq, reliable_base);
		}
		// Q3MINI BEGIN
		if (net_dbg_q3mini.value > 0.0f)
		{
			unsigned int srv_ack = cl.q3mini_srv_ack_valid ? cl.q3mini_srv_ack : 0u;
			unsigned int srv_ack_mask = cl.q3mini_srv_ack_valid ? cl.q3mini_srv_ack_mask : 0u;
			Con_Printf ("NETDBG q3mini send cmd_seq %u cmd_ack %u srv_ack %u mask 0x%08x cmds %d\n",
				cmd->sequence, cl.last_cmd_ack, srv_ack, srv_ack_mask, cmd_count);
			JITTER_LOG ("NETDBG q3mini send cmd_seq %u cmd_ack %u srv_ack %u mask 0x%08x cmds %d\n",
				cmd->sequence, cl.last_cmd_ack, srv_ack, srv_ack_mask, cmd_count);
		}
		// Q3MINI END
	}

//
// deliver the message
//
	if (cls.demoplayback)
		return;

//
// allways dump the first two message, because it may contain leftover inputs
// from the last level
//
	if (++cl.movemessages <= 2)
		return;

	if (NET_SendUnreliableMessage (cls.netcon, &buf) == -1)
	{
		Con_Printf ("CL_SendMove: lost server connection\n");
		CL_Disconnect ();
	}
}

// Q3MINI BEGIN
int CL_CalcMovePacketBytes (const usercmd_t *cmd, int *out_cmd_count)
{
	int cmd_count = 0;
	int i;
	int header_bytes = 1 + 4 + 4 + 4; // clc_move + mtime + cmd_seq + cmd_ack
	int cmd_bytes = 4 + (3 * 2) + (3 * 2) + 1 + 1;

	if (!cmd)
	{
		if (out_cmd_count)
			*out_cmd_count = 0;
		return 0;
	}

	if (CL_Q3Mini_Enabled () && net_ackmask.value > 0.0f)
		header_bytes += 8;

	{
		int maxbatch = (int)cl_cmd_maxbatch.value;
		int redundancy = CL_Q3Mini_Redundancy ();
		int desired = redundancy + 1;

		if (maxbatch < 1)
			maxbatch = 1;
		if (maxbatch > MAX_CMDS_PER_PACKET)
			maxbatch = MAX_CMDS_PER_PACKET;
		if (maxbatch > desired)
			maxbatch = desired;

		for (i = redundancy; i >= 0 && cmd_count < maxbatch; i--)
		{
			unsigned int seq = cmd->sequence - (unsigned int)i;

			if (CL_Predict_GetCmd (seq, NULL))
				cmd_count++;
		}
	}

	if (out_cmd_count)
		*out_cmd_count = cmd_count;

	return header_bytes + 1 + (cmd_count * cmd_bytes);
}
// Q3MINI END

/*
============
CL_InitInput
============
*/
void CL_InitInput (void)
{
	Cmd_AddCommand ("+moveup",IN_UpDown);
	Cmd_AddCommand ("-moveup",IN_UpUp);
	Cmd_AddCommand ("+movedown",IN_DownDown);
	Cmd_AddCommand ("-movedown",IN_DownUp);
	Cmd_AddCommand ("+left",IN_LeftDown);
	Cmd_AddCommand ("-left",IN_LeftUp);
	Cmd_AddCommand ("+right",IN_RightDown);
	Cmd_AddCommand ("-right",IN_RightUp);
	Cmd_AddCommand ("+forward",IN_ForwardDown);
	Cmd_AddCommand ("-forward",IN_ForwardUp);
	Cmd_AddCommand ("+back",IN_BackDown);
	Cmd_AddCommand ("-back",IN_BackUp);
	Cmd_AddCommand ("+lookup", IN_LookupDown);
	Cmd_AddCommand ("-lookup", IN_LookupUp);
	Cmd_AddCommand ("+lookdown", IN_LookdownDown);
	Cmd_AddCommand ("-lookdown", IN_LookdownUp);
	Cmd_AddCommand ("+strafe", IN_StrafeDown);
	Cmd_AddCommand ("-strafe", IN_StrafeUp);
	Cmd_AddCommand ("+moveleft", IN_MoveleftDown);
	Cmd_AddCommand ("-moveleft", IN_MoveleftUp);
	Cmd_AddCommand ("+moveright", IN_MoverightDown);
	Cmd_AddCommand ("-moveright", IN_MoverightUp);
	Cmd_AddCommand ("+speed", IN_SpeedDown);
	Cmd_AddCommand ("-speed", IN_SpeedUp);
	Cmd_AddCommand ("+attack", IN_AttackDown);
	Cmd_AddCommand ("-attack", IN_AttackUp);
	Cmd_AddCommand ("+use", IN_UseDown);
	Cmd_AddCommand ("-use", IN_UseUp);
	Cmd_AddCommand ("+jump", IN_JumpDown);
	Cmd_AddCommand ("-jump", IN_JumpUp);
	Cmd_AddCommand ("impulse", IN_Impulse);
	Cmd_AddCommand ("+klook", IN_KLookDown);
	Cmd_AddCommand ("-klook", IN_KLookUp);
	Cmd_AddCommand ("+mlook", IN_MLookDown);
	Cmd_AddCommand ("-mlook", IN_MLookUp);

}
