/*
Copyright (C) 1996-2001 Id Software, Inc.
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

#include "quakedef.h"
#include "q_stdinc.h"
#include "arch_def.h"
#include "net_sys.h"
#include "net_defs.h"
#include "net_dgrm.h"

// This is enables a simple IP banning mechanism
#define BAN_TEST

// these two macros are to make the code more readable
#define sfunc	net_landrivers[sock->landriver]
#define dfunc	net_landrivers[net_landriverlevel]

static int net_landriverlevel;

/* statistic counters */
static int packetsSent = 0;
static int packetsReSent = 0;
static int packetsReceived = 0;
static int receivedDuplicateCount = 0;
static int shortPacketCount = 0;
static int droppedDatagrams;

static struct
{
	unsigned int	length;
	unsigned int	sequence;
	unsigned int	ack;
	unsigned int	ack_bits;
	byte	data[MAX_DATAGRAM];
} packetBuffer;

static int myDriverLevel;

static FILE *net_debug_log;
static qboolean net_debug_log_tried;
static double net_debug_next_time;
static unsigned int net_debug_packets;
static unsigned int net_debug_bytes;
static unsigned int net_debug_max;
static unsigned int net_debug_oversize;
static unsigned int net_debug_resends;

extern qboolean m_return_onerror;
extern char m_return_reason[32];

static qboolean NET_DebugEnabled (void)
{
	if (developer.value)
		return true;
	if (sv.active && sv_mtu_debug.value > 0)
		return true;
	return false;
}

static void NET_DebugLogV (qboolean force, const char *fmt, va_list argptr);
static void NET_DebugLog (qboolean force, const char *fmt, ...) FUNCP_PRINTF(2,3);
static void NET_DebugLog (qboolean force, const char *fmt, ...)
{
	va_list argptr;

	va_start (argptr, fmt);
	NET_DebugLogV (force, fmt, argptr);
	va_end (argptr);
}

static void NET_DebugLogV (qboolean force, const char *fmt, va_list argptr)
{
	char text[2048];
	int len;

	if (!force && !NET_DebugEnabled ())
		return;
	if (!net_debug_log && !net_debug_log_tried)
	{
		char path[MAX_OSPATH];
		net_debug_log_tried = true;
		if (host_parms && host_parms->basedir)
		{
			q_snprintf (path, sizeof(path), "%s/net_debug.log", host_parms->basedir);
			net_debug_log = Sys_fopen (path, "ab");
		}
	}
	if (!net_debug_log)
		return;

	len = q_vsnprintf (text, sizeof(text), fmt, argptr);
	if (len < 0)
		return;
	if (len >= (int)sizeof(text))
		len = (int)sizeof(text) - 1;
	fwrite (text, 1, (size_t)len, net_debug_log);
	fflush (net_debug_log);
}

void NET_DebugLogEvent (qboolean force, const char *fmt, ...)
{
	va_list argptr;

	va_start (argptr, fmt);
	NET_DebugLogV (force, fmt, argptr);
	va_end (argptr);
}

static void NET_DebugRecordPacket (unsigned int packet_len)
{
	net_debug_packets++;
	net_debug_bytes += packet_len;
	if (packet_len > net_debug_max)
		net_debug_max = packet_len;
}

static void NET_DebugMaybeLogSummary (void)
{
	if (!NET_DebugEnabled ())
		return;
	if (net_time < net_debug_next_time)
		return;

	if (net_debug_packets > 0)
	{
		unsigned int avg = net_debug_bytes / net_debug_packets;
		NET_DebugLog (false,
			"NETDBG time %.3f summary packets %u avg %u max %u oversize %u resends %u\n",
			net_time, net_debug_packets, avg, net_debug_max, net_debug_oversize, net_debug_resends);
	}

	net_debug_packets = 0;
	net_debug_bytes = 0;
	net_debug_max = 0;
	net_debug_oversize = 0;
	net_debug_resends = 0;
	net_debug_next_time = net_time + 1.0;
}

static int NET_GetConfiguredMTU (const qsocket_t *sock)
{
	(void)sock;
	if (sv.active)
		return (int)sv_mtu.value;
	return (int)net_mtu.value;
}

static int NET_GetPacketPayloadLimit (const qsocket_t *sock)
{
	int mtu = NET_GetConfiguredMTU (sock);
	int payload;

	if (mtu <= 0)
		return NET_DATAGRAMSIZE;
	payload = mtu - NET_UDPIP_HEADER_BYTES;
	if (payload < NET_HEADERSIZE + 1)
		payload = NET_HEADERSIZE + 1;
	return q_min (payload, NET_DATAGRAMSIZE);
}

static int NET_GetPacketDataLimit (const qsocket_t *sock)
{
	int payload = NET_GetPacketPayloadLimit (sock);
	int data_limit = payload - NET_HEADERSIZE;

	if (data_limit < 1)
		data_limit = 1;
	return q_min (data_limit, MAX_DATAGRAM);
}

static void NET_LogOversizedSend (const qsocket_t *sock, unsigned int packet_len)
{
	static double next_log_time = 0.0;
	unsigned int mtu = (unsigned int)NET_GetConfiguredMTU (sock);
	unsigned int payload_limit = (unsigned int)NET_GetPacketPayloadLimit (sock);

	NET_DebugLog (true,
		"NETDBG time %.3f PREVENTED_OVERSHOOT addr %s mtu %u payload_cap %u packet %u\n",
		net_time, NET_QSocketGetAddressString (sock), mtu, payload_limit, packet_len);
	if (sv.active && sv_mtu_debug.value > 0 && net_time >= next_log_time)
	{
		Con_Printf ("sv_mtu %d oversize send blocked (%s size %u)\n",
			(int)sv_mtu.value,
			NET_QSocketGetAddressString (sock),
			packet_len);
		next_log_time = net_time + 1.0;
	}
}

static qboolean NET_SequenceLessThan (unsigned int a, unsigned int b)
{
	return (int)(a - b) < 0;
}

static unsigned int NET_GetAckSequence (const qsocket_t *sock)
{
	if (!sock->reliableReceiveValid)
		return 0;
	return sock->reliableReceiveSequence;
}

static unsigned int NET_GetAckBits (const qsocket_t *sock)
{
	if (!sock->reliableReceiveValid)
		return 0;
	return sock->reliableReceiveMask;
}

static void NET_SetPacketHeader (qsocket_t *sock, unsigned int packet_len, unsigned int flags, unsigned int sequence)
{
	packetBuffer.length = BigLong(packet_len | flags);
	packetBuffer.sequence = BigLong(sequence);
	packetBuffer.ack = BigLong(NET_GetAckSequence(sock));
	packetBuffer.ack_bits = BigLong(NET_GetAckBits(sock));
}

static void NET_UpdateReceiveAckState (qsocket_t *sock, unsigned int sequence)
{
	if (!sock->reliableReceiveValid)
	{
		sock->reliableReceiveValid = true;
		sock->reliableReceiveSequence = sequence;
		sock->reliableReceiveMask = 0;
		return;
	}

	if (sequence == sock->reliableReceiveSequence)
		return;

	if (NET_SequenceLessThan(sequence, sock->reliableReceiveSequence))
	{
		unsigned int delta = sock->reliableReceiveSequence - sequence;
		if (delta >= 1 && delta <= NET_ACK_BITS)
			sock->reliableReceiveMask |= 1u << (delta - 1);
		return;
	}
	else
	{
		unsigned int shift = sequence - sock->reliableReceiveSequence;
		if (shift >= NET_ACK_BITS + 1)
			sock->reliableReceiveMask = 0;
		else
			sock->reliableReceiveMask <<= shift;
		if (shift >= 1 && shift <= NET_ACK_BITS)
			sock->reliableReceiveMask |= 1u << (shift - 1);
		sock->reliableReceiveSequence = sequence;
	}
}

static int NET_SendReliablePacket (qsocket_t *sock, net_reliable_send_t *packet, const char *label, qboolean is_resend)
{
	unsigned int	packet_len;
	unsigned int	payload_limit = (unsigned int)NET_GetPacketPayloadLimit (sock);
	unsigned int	flags = NETFLAG_DATA;

	packet_len = NET_HEADERSIZE + (unsigned int)packet->length;
	if (packet->eom)
		flags |= NETFLAG_EOM;

	if (packet_len > payload_limit)
	{
		NET_LogOversizedSend (sock, packet_len);
		net_debug_oversize++;
		NET_DebugMaybeLogSummary ();
		return -1;
	}

	NET_SetPacketHeader(sock, packet_len, flags, packet->sequence);
	Q_memcpy (packetBuffer.data, sock->sendMessage + packet->offset, packet->length);

	if (sfunc.Write (sock->socket, (byte *)&packetBuffer, packet_len, &sock->addr) == -1)
		return -1;

	NET_DebugRecordPacket (packet_len);
	if (is_resend)
		net_debug_resends++;
	NET_DebugLog (false,
		"NETDBG time %.3f send reliable-%s addr %s seq %u len %u data %u eom %u\n",
		net_time, label, NET_QSocketGetAddressString (sock), packet->sequence,
		packet_len, (unsigned int)packet->length, packet->eom ? 1u : 0u);
	NET_DebugMaybeLogSummary ();

	packet->lastSendTime = net_time;
	sock->lastSendTime = net_time;
	if (is_resend)
		packetsReSent++;
	else
		packetsSent++;
	return 1;
}

static int NET_SendPendingReliable (qsocket_t *sock)
{
	unsigned int	max_data = (unsigned int)NET_GetPacketDataLimit (sock);

	while (sock->sendMessageOffset < sock->sendMessageLength)
	{
		unsigned int in_flight = sock->sendSequence - sock->sendReliableBase;
		net_reliable_send_t *packet;
		unsigned int sequence;
		int data_len;
		int offset;
		qboolean eom;

		if (in_flight >= NET_RELIABLE_WINDOW)
			break;

		offset = sock->sendMessageOffset;
		data_len = sock->sendMessageLength - sock->sendMessageOffset;
		if (data_len > (int)max_data)
			data_len = (int)max_data;
		eom = (sock->sendMessageOffset + data_len) >= sock->sendMessageLength;

		sequence = sock->sendSequence++;
		packet = &sock->reliableSend[sequence % NET_RELIABLE_WINDOW];
		packet->sequence = sequence;
		packet->length = data_len;
		packet->offset = offset;
		packet->eom = eom;
		packet->acked = false;
		packet->lastSendTime = 0.0;

		if (NET_SendReliablePacket (sock, packet, "window", false) == -1)
			return -1;

		sock->sendMessageOffset += data_len;
	}

	return 1;
}

static void NET_AckReliableSequence (qsocket_t *sock, unsigned int sequence)
{
	net_reliable_send_t *packet;

	if (NET_SequenceLessThan(sequence, sock->sendReliableBase))
		return;
	if (!NET_SequenceLessThan(sequence, sock->sendSequence))
		return;
	if (sequence - sock->sendReliableBase >= NET_RELIABLE_WINDOW)
		return;

	packet = &sock->reliableSend[sequence % NET_RELIABLE_WINDOW];
	if (packet->sequence != sequence)
		return;
	packet->acked = true;
}

static void NET_AdvanceSendWindow (qsocket_t *sock)
{
	while (sock->sendReliableBase != sock->sendSequence)
	{
		net_reliable_send_t *packet = &sock->reliableSend[sock->sendReliableBase % NET_RELIABLE_WINDOW];
		if (!packet->acked || packet->sequence != sock->sendReliableBase)
			break;
		memset(packet, 0, sizeof(*packet));
		sock->sendReliableBase++;
	}

	if (sock->sendReliableBase == sock->sendSequence &&
		sock->sendMessageOffset >= sock->sendMessageLength)
	{
		sock->sendMessageLength = 0;
		sock->sendMessageOffset = 0;
		sock->canSend = true;
	}
}

static void NET_ProcessAcks (qsocket_t *sock, unsigned int ack, unsigned int ack_bits)
{
	unsigned int i;

	if (ack == 0)
		return;

	NET_AckReliableSequence(sock, ack);
	for (i = 0; i < NET_ACK_BITS; ++i)
	{
		if (ack_bits & (1u << i))
			NET_AckReliableSequence(sock, ack - (i + 1));
	}

	NET_AdvanceSendWindow(sock);
	NET_SendPendingReliable(sock);
}

static void NET_ResendReliablePackets (qsocket_t *sock)
{
	unsigned int sequence;

	if (sock->sendMessageLength == 0)
		return;

	for (sequence = sock->sendReliableBase; sequence != sock->sendSequence; ++sequence)
	{
		net_reliable_send_t *packet = &sock->reliableSend[sequence % NET_RELIABLE_WINDOW];
		if (packet->sequence != sequence || packet->acked)
			continue;
		if ((net_time - packet->lastSendTime) <= 1.0)
			continue;
		NET_SendReliablePacket (sock, packet, "resend", true);
	}
}

static void NET_ClearReliableReceiveEntry (net_reliable_receive_t *entry)
{
	if (entry->data)
	{
		Z_Free(entry->data);
		entry->data = NULL;
	}
	memset(entry, 0, sizeof(*entry));
}


static char *StrAddr (struct qsockaddr *addr)
{
	static char buf[34];
	byte *p = (byte *)addr;
	int n;

	for (n = 0; n < 16; n++)
		sprintf (buf + n * 2, "%02x", *p++);
	return buf;
}


#ifdef BAN_TEST

static struct in_addr	banAddr;
static struct in_addr	banMask;

static void NET_Ban_f (void)
{
	char	addrStr [32];
	char	maskStr [32];
	void	(*print_fn)(const char *fmt, ...) FUNCP_PRINTF(1,2);

	if (cmd_source == src_command)
	{
		if (!sv.active)
		{
			Cmd_ForwardToServer ();
			return;
		}
		print_fn = Con_Printf;
	}
	else
	{
		if (pr_global_struct->deathmatch)
			return;
		print_fn = SV_ClientPrintf;
	}

	switch (Cmd_Argc ())
	{
	case 1:
		if (banAddr.s_addr != INADDR_ANY)
		{
			Q_strcpy(addrStr, inet_ntoa(banAddr));
			Q_strcpy(maskStr, inet_ntoa(banMask));
			print_fn("Banning %s [%s]\n", addrStr, maskStr);
		}
		else
			print_fn("Banning not active\n");
		break;

	case 2:
		if (q_strcasecmp(Cmd_Argv(1), "off") == 0)
			banAddr.s_addr = INADDR_ANY;
		else
			banAddr.s_addr = inet_addr(Cmd_Argv(1));
		banMask.s_addr = INADDR_NONE;
		break;

	case 3:
		banAddr.s_addr = inet_addr(Cmd_Argv(1));
		banMask.s_addr = inet_addr(Cmd_Argv(2));
		break;

	default:
		print_fn("BAN ip_address [mask]\n");
		break;
	}
}
#endif	// BAN_TEST


int Datagram_SendMessage (qsocket_t *sock, sizebuf_t *data)
{
#ifdef DEBUG
	if (data->cursize == 0)
		Sys_Error("Datagram_SendMessage: zero length message");

	if (data->cursize > NET_MAXMESSAGE)
		Sys_Error("Datagram_SendMessage: message too big: %u", data->cursize);

	if (sock->canSend == false)
		Sys_Error("SendMessage: called with canSend == false");
#endif

	Q_memcpy(sock->sendMessage, data->data, data->cursize);
	sock->sendMessageLength = data->cursize;
	sock->sendMessageOffset = 0;
	sock->sendReliableBase = sock->sendSequence;

	sock->canSend = false;
	if (NET_SendPendingReliable(sock) == -1)
		return -1;
	return 1;
}



qboolean Datagram_CanSendMessage (qsocket_t *sock)
{
	if (sock->sendMessageLength > 0)
		NET_SendPendingReliable(sock);

	return sock->canSend;
}


qboolean Datagram_CanSendUnreliableMessage (qsocket_t *sock)
{
	return true;
}


int Datagram_SendUnreliableMessage (qsocket_t *sock, sizebuf_t *data)
{
	int	packetLen;
	int	payload_limit = NET_GetPacketPayloadLimit (sock);
	int	max_data = NET_GetPacketDataLimit (sock);

#ifdef DEBUG
	if (data->cursize == 0)
		Sys_Error("Datagram_SendUnreliableMessage: zero length message");

	if (data->cursize > MAX_DATAGRAM)
		Sys_Error("Datagram_SendUnreliableMessage: message too big: %u", data->cursize);
#endif

	if (data->cursize > max_data)
	{
		NET_LogOversizedSend (sock, (unsigned int)(NET_HEADERSIZE + data->cursize));
		net_debug_oversize++;
		NET_DebugMaybeLogSummary ();
		return 0;
	}

	packetLen = NET_HEADERSIZE + data->cursize;
	if (packetLen > payload_limit)
	{
		NET_LogOversizedSend (sock, (unsigned int)packetLen);
		net_debug_oversize++;
		NET_DebugMaybeLogSummary ();
		return 0;
	}

	NET_SetPacketHeader(sock, (unsigned int)packetLen, NETFLAG_UNRELIABLE, sock->unreliableSendSequence++);
	Q_memcpy (packetBuffer.data, data->data, data->cursize);

	if (sfunc.Write (sock->socket, (byte *)&packetBuffer, packetLen, &sock->addr) == -1)
		return -1;

	NET_DebugRecordPacket ((unsigned int)packetLen);
	NET_DebugLog (false,
		"NETDBG time %.3f send unreliable addr %s seq %u len %u data %u\n",
		net_time, NET_QSocketGetAddressString (sock), sock->unreliableSendSequence - 1,
		(unsigned int)packetLen, (unsigned int)data->cursize);
	NET_DebugMaybeLogSummary ();

	packetsSent++;
	return 1;
}


int	Datagram_GetMessage (qsocket_t *sock)
{
	unsigned int	length;
	unsigned int	flags;
	int				ret = 0;
	struct qsockaddr readaddr;
	unsigned int	sequence;
	unsigned int	count;
	int				packetLengthRead;

	NET_ResendReliablePackets (sock);

	while (1)
	{
		packetLengthRead = (int) sfunc.Read(sock->socket, (byte *)&packetBuffer,
							NET_DATAGRAMSIZE, &readaddr);

	//	if ((rand() & 255) > 220)
	//		continue;

		if (packetLengthRead == 0)
			break;

		if (packetLengthRead == -1)
		{
			Con_Printf("Read error\n");
			return -1;
		}

		if (net_maxpacket.value > 0 && packetLengthRead > (int)net_maxpacket.value)
		{
			Con_DPrintf("NET_GetMessage: oversized packet (%d > %d)\n",
				packetLengthRead, (int)net_maxpacket.value);
			continue;
		}

		if (sfunc.AddrCompare(&readaddr, &sock->addr) != 0)
		{
			Con_Printf("Forged packet received\n");
			Con_Printf("Expected: %s\n", StrAddr (&sock->addr));
			Con_Printf("Received: %s\n", StrAddr (&readaddr));
			continue;
		}

		if (packetLengthRead < NET_HEADERSIZE)
		{
			shortPacketCount++;
			continue;
		}

		length = BigLong(packetBuffer.length);
		flags = length & (~NETFLAG_LENGTH_MASK);
		length &= NETFLAG_LENGTH_MASK;

		if (flags & NETFLAG_CTL)
			continue;

		if (length < NET_HEADERSIZE || length > NET_DATAGRAMSIZE)
		{
			Con_DPrintf("NET_GetMessage: invalid packet length %u\n", length);
			continue;
		}

		if (net_maxpacket.value > 0 && length > (unsigned int)net_maxpacket.value)
		{
			Con_DPrintf("NET_GetMessage: oversized packet length %u (cap %d)\n",
				length, (int)net_maxpacket.value);
			continue;
		}

		if ((int)length > packetLengthRead)
		{
			Con_DPrintf("NET_GetMessage: truncated packet length %u (read %d)\n",
				length, packetLengthRead);
			continue;
		}

		sequence = BigLong(packetBuffer.sequence);
		{
			unsigned int ack = BigLong(packetBuffer.ack);
			unsigned int ack_bits = BigLong(packetBuffer.ack_bits);

			packetsReceived++;
			NET_ProcessAcks(sock, ack, ack_bits);
		}

		if (flags & NETFLAG_UNRELIABLE)
		{
			if (sequence < sock->unreliableReceiveSequence)
			{
				Con_DPrintf("Got a stale datagram\n");
				ret = 0;
				break;
			}
			if (sequence != sock->unreliableReceiveSequence)
			{
				count = sequence - sock->unreliableReceiveSequence;
				droppedDatagrams += count;
				Con_DPrintf("Dropped %u datagram(s)\n", count);
			}
			sock->unreliableReceiveSequence = sequence + 1;

			length -= NET_HEADERSIZE;

			if (length > MAX_DATAGRAM)
			{
				Con_DPrintf("NET_GetMessage: unreliable packet too large (%u)\n", length);
				continue;
			}

			SZ_Clear (&net_message);
			SZ_Write (&net_message, packetBuffer.data, length);

			net_last_incoming.sequence = sequence;
			net_last_incoming.flags = flags;
			net_last_incoming.packet_length = (unsigned int)packetLengthRead;
			net_last_incoming.payload_length = (unsigned int)length;
			net_last_incoming.unreliable = true;
			net_last_incoming.valid = true;

			ret = 2;
			break;
		}

		if (flags & NETFLAG_ACK)
		{
			if (!(flags & (NETFLAG_DATA | NETFLAG_UNRELIABLE)))
				continue;
		}

		if (flags & NETFLAG_DATA)
		{
			net_reliable_receive_t *entry;
			unsigned int window_offset;

			if (NET_SequenceLessThan(sequence, sock->receiveSequence))
			{
				NET_UpdateReceiveAckState(sock, sequence);
				receivedDuplicateCount++;
				NET_SetPacketHeader(sock, NET_HEADERSIZE, NETFLAG_ACK, 0);
				sfunc.Write (sock->socket, (byte *)&packetBuffer, NET_HEADERSIZE, &readaddr);
				continue;
			}

			window_offset = sequence - sock->receiveSequence;
			if (window_offset >= NET_RELIABLE_WINDOW)
				continue;

			NET_UpdateReceiveAckState(sock, sequence);

			length -= NET_HEADERSIZE;
			if (length > MAX_DATAGRAM)
			{
				Con_DPrintf("NET_GetMessage: reliable packet too large (%u)\n", length);
				continue;
			}

			entry = &sock->reliableReceive[sequence % NET_RELIABLE_WINDOW];
			if (entry->received && entry->sequence == sequence)
			{
				receivedDuplicateCount++;
			}
			else
			{
				NET_ClearReliableReceiveEntry(entry);
				entry->sequence = sequence;
				entry->length = (int)length;
				entry->eom = (flags & NETFLAG_EOM) != 0;
				entry->received = true;
				entry->data = (byte *)Z_Malloc(length);
				Q_memcpy(entry->data, packetBuffer.data, length);
			}

			NET_SetPacketHeader(sock, NET_HEADERSIZE, NETFLAG_ACK, 0);
			sfunc.Write (sock->socket, (byte *)&packetBuffer, NET_HEADERSIZE, &readaddr);

			while (1)
			{
				unsigned int message_sequence;
				entry = &sock->reliableReceive[sock->receiveSequence % NET_RELIABLE_WINDOW];
				if (!entry->received || entry->sequence != sock->receiveSequence)
					break;

				if (sock->receiveMessageLength + entry->length > NET_MAXMESSAGE)
				{
					Con_DPrintf("NET_GetMessage: reliable message overflow (%u + %u)\n",
						sock->receiveMessageLength, entry->length);
					sock->receiveMessageLength = 0;
					message_sequence = entry->sequence;
					NET_ClearReliableReceiveEntry(entry);
					sock->receiveSequence++;
					continue;
				}

				Q_memcpy(sock->receiveMessage + sock->receiveMessageLength, entry->data, entry->length);
				sock->receiveMessageLength += entry->length;
				message_sequence = entry->sequence;

				if (entry->eom)
				{
					SZ_Clear(&net_message);
					SZ_Write(&net_message, sock->receiveMessage, sock->receiveMessageLength);
					sock->receiveMessageLength = 0;

					net_last_incoming.sequence = message_sequence;
					net_last_incoming.flags = NETFLAG_DATA | NETFLAG_EOM;
					net_last_incoming.packet_length = NET_HEADERSIZE + (unsigned int)entry->length;
					net_last_incoming.payload_length = (unsigned int)(net_message.cursize);
					net_last_incoming.unreliable = false;
					net_last_incoming.valid = true;

					NET_ClearReliableReceiveEntry(entry);
					sock->receiveSequence++;
					ret = 1;
					break;
				}

				NET_ClearReliableReceiveEntry(entry);
				sock->receiveSequence++;
			}

			if (ret == 1)
				break;
			continue;
		}
	}

	return ret;
}


static void PrintStats(qsocket_t *s)
{
	Con_Printf("canSend = %4u   \n", s->canSend);
	Con_Printf("sendSeq = %4u   ", s->sendSequence);
	Con_Printf("recvSeq = %4u   \n", s->receiveSequence);
	Con_Printf("\n");
}

static void NET_Stats_f (void)
{
	qsocket_t	*s;

	if (Cmd_Argc () == 1)
	{
		Con_Printf("unreliable messages sent   = %i\n", unreliableMessagesSent);
		Con_Printf("unreliable messages recv   = %i\n", unreliableMessagesReceived);
		Con_Printf("reliable messages sent     = %i\n", messagesSent);
		Con_Printf("reliable messages received = %i\n", messagesReceived);
		Con_Printf("packetsSent                = %i\n", packetsSent);
		Con_Printf("packetsReSent              = %i\n", packetsReSent);
		Con_Printf("packetsReceived            = %i\n", packetsReceived);
		Con_Printf("receivedDuplicateCount     = %i\n", receivedDuplicateCount);
		Con_Printf("shortPacketCount           = %i\n", shortPacketCount);
		Con_Printf("droppedDatagrams           = %i\n", droppedDatagrams);
	}
	else if (Q_strcmp(Cmd_Argv(1), "*") == 0)
	{
		for (s = net_activeSockets; s; s = s->next)
			PrintStats(s);
		for (s = net_freeSockets; s; s = s->next)
			PrintStats(s);
	}
	else
	{
		for (s = net_activeSockets; s; s = s->next)
		{
			if (q_strcasecmp(Cmd_Argv(1), s->address) == 0)
				break;
		}

		if (s == NULL)
		{
			for (s = net_freeSockets; s; s = s->next)
			{
				if (q_strcasecmp(Cmd_Argv(1), s->address) == 0)
					break;
			}
		}

		if (s == NULL)
			return;

		PrintStats(s);
	}
}


// recognize ip:port (based on ProQuake)
static const char *Strip_Port (const char *host)
{
	static char	noport[MAX_QPATH];
			/* array size as in Host_Connect_f() */
	char		*p;
	int		port;

	if (!host || !*host)
		return host;
	q_strlcpy (noport, host, sizeof(noport));
	if ((p = Q_strrchr(noport, ':')) == NULL)
		return host;
	*p++ = '\0';
	port = Q_atoi (p);
	if (port > 0 && port < 65536 && port != net_hostport)
	{
		net_hostport = port;
		Con_Printf("Port set to %d\n", net_hostport);
	}
	return noport;
}


static qboolean testInProgress = false;
static int		testPollCount;
static int		testDriver;
static sys_socket_t	testSocket;

static void Test_Poll (void *);
static PollProcedure	testPollProcedure = {NULL, 0.0, Test_Poll};

static void Test_Poll (void *unused)
{
	struct qsockaddr clientaddr;
	int		control;
	int		len;
	char	name[32];
	char	address[64];
	int		colors;
	int		frags;
	int		connectTime;

	net_landriverlevel = testDriver;

	while (1)
	{
		len = dfunc.Read (testSocket, net_message.data, net_message.maxsize, &clientaddr);
		if (len < (int) sizeof(int))
			break;

		net_message.cursize = len;

		MSG_BeginReading ();
		control = BigLong(*((int *)net_message.data));
		MSG_ReadLong();
		if (control == -1)
			break;
		if ((control & (~NETFLAG_LENGTH_MASK)) != (int)NETFLAG_CTL)
			break;
		if ((control & NETFLAG_LENGTH_MASK) != len)
			break;

		if (MSG_ReadByte() != CCREP_PLAYER_INFO)
			Sys_Error("Unexpected response to Player Info request\n");

		MSG_ReadByte(); /* playerNumber */
		Q_strcpy(name, MSG_ReadString());
		colors = MSG_ReadLong();
		frags = MSG_ReadLong();
		connectTime = MSG_ReadLong();
		Q_strcpy(address, MSG_ReadString());

		Con_Printf("%s\n  frags:%3i  colors:%d %d  time:%d\n  %s\n", name, frags, colors >> 4, colors & 0x0f, connectTime / 60, address);
	}

	testPollCount--;
	if (testPollCount)
	{
		SchedulePollProcedure(&testPollProcedure, 0.1);
	}
	else
	{
		dfunc.Close_Socket(testSocket);
		testInProgress = false;
	}
}

static void Test_f (void)
{
	const char	*host;
	int		n;
	int		maxusers = MAX_SCOREBOARD;
	struct qsockaddr sendaddr;

	if (testInProgress)
		return;

	host = Strip_Port (Cmd_Argv(1));

	if (host && hostCacheCount)
	{
		for (n = 0; n < hostCacheCount; n++)
		{
			if (q_strcasecmp (host, hostcache[n].name) == 0)
			{
				if (hostcache[n].driver != myDriverLevel)
					continue;
				net_landriverlevel = hostcache[n].ldriver;
				maxusers = hostcache[n].maxusers;
				Q_memcpy(&sendaddr, &hostcache[n].addr, sizeof(struct qsockaddr));
				break;
			}
		}

		if (n < hostCacheCount)
			goto JustDoIt;
	}

	for (net_landriverlevel = 0; net_landriverlevel < net_numlandrivers; net_landriverlevel++)
	{
		if (!net_landrivers[net_landriverlevel].initialized)
			continue;

		// see if we can resolve the host name
		if (dfunc.GetAddrFromName(host, &sendaddr) != -1)
			break;
	}

	if (net_landriverlevel == net_numlandrivers)
	{
		Con_Printf("Could not resolve %s\n", host);
		return;
	}

JustDoIt:
	testSocket = dfunc.Open_Socket(0);
	if (testSocket == INVALID_SOCKET)
		return;

	testInProgress = true;
	testPollCount = 20;
	testDriver = net_landriverlevel;

	for (n = 0; n < maxusers; n++)
	{
		SZ_Clear(&net_message);
		// save space for the header, filled in later
		MSG_WriteLong(&net_message, 0);
		MSG_WriteByte(&net_message, CCREQ_PLAYER_INFO);
		MSG_WriteByte(&net_message, n);
		*((int *)net_message.data) = BigLong(NETFLAG_CTL | (net_message.cursize & NETFLAG_LENGTH_MASK));
		dfunc.Write (testSocket, net_message.data, net_message.cursize, &sendaddr);
	}
	SZ_Clear(&net_message);
	SchedulePollProcedure(&testPollProcedure, 0.1);
}


static qboolean test2InProgress = false;
static int		test2Driver;
static sys_socket_t	test2Socket;

static void Test2_Poll (void *);
static PollProcedure	test2PollProcedure = {NULL, 0.0, Test2_Poll};

static void Test2_Poll (void *unused)
{
	struct qsockaddr clientaddr;
	int		control;
	int		len;
	char	name[256];
	char	value[256];

	net_landriverlevel = test2Driver;
	name[0] = 0;

	len = dfunc.Read (test2Socket, net_message.data, net_message.maxsize, &clientaddr);
	if (len < (int) sizeof(int))
		goto Reschedule;

	net_message.cursize = len;

	MSG_BeginReading ();
	control = BigLong(*((int *)net_message.data));
	MSG_ReadLong();
	if (control == -1)
		goto Error;
	if ((control & (~NETFLAG_LENGTH_MASK)) != (int)NETFLAG_CTL)
		goto Error;
	if ((control & NETFLAG_LENGTH_MASK) != len)
		goto Error;

	if (MSG_ReadByte() != CCREP_RULE_INFO)
		goto Error;

	Q_strcpy(name, MSG_ReadString());
	if (name[0] == 0)
		goto Done;
	Q_strcpy(value, MSG_ReadString());

	Con_Printf("%-16.16s  %-16.16s\n", name, value);

	SZ_Clear(&net_message);
	// save space for the header, filled in later
	MSG_WriteLong(&net_message, 0);
	MSG_WriteByte(&net_message, CCREQ_RULE_INFO);
	MSG_WriteString(&net_message, name);
	*((int *)net_message.data) = BigLong(NETFLAG_CTL | (net_message.cursize & NETFLAG_LENGTH_MASK));
	dfunc.Write (test2Socket, net_message.data, net_message.cursize, &clientaddr);
	SZ_Clear(&net_message);

Reschedule:
	SchedulePollProcedure(&test2PollProcedure, 0.05);
	return;

Error:
	Con_Printf("Unexpected response to Rule Info request\n");
Done:
	dfunc.Close_Socket(test2Socket);
	test2InProgress = false;
	return;
}

static void Test2_f (void)
{
	const char	*host;
	int		n;
	struct qsockaddr sendaddr;

	if (test2InProgress)
		return;

	host = Strip_Port (Cmd_Argv(1));

	if (host && hostCacheCount)
	{
		for (n = 0; n < hostCacheCount; n++)
		{
			if (q_strcasecmp (host, hostcache[n].name) == 0)
			{
				if (hostcache[n].driver != myDriverLevel)
					continue;
				net_landriverlevel = hostcache[n].ldriver;
				Q_memcpy(&sendaddr, &hostcache[n].addr, sizeof(struct qsockaddr));
				break;
			}
		}

		if (n < hostCacheCount)
			goto JustDoIt;
	}

	for (net_landriverlevel = 0; net_landriverlevel < net_numlandrivers; net_landriverlevel++)
	{
		if (!net_landrivers[net_landriverlevel].initialized)
			continue;

		// see if we can resolve the host name
		if (dfunc.GetAddrFromName(host, &sendaddr) != -1)
			break;
	}

	if (net_landriverlevel == net_numlandrivers)
	{
		Con_Printf("Could not resolve %s\n", host);
		return;
	}

JustDoIt:
	test2Socket = dfunc.Open_Socket(0);
	if (test2Socket == INVALID_SOCKET)
		return;

	test2InProgress = true;
	test2Driver = net_landriverlevel;

	SZ_Clear(&net_message);
	// save space for the header, filled in later
	MSG_WriteLong(&net_message, 0);
	MSG_WriteByte(&net_message, CCREQ_RULE_INFO);
	MSG_WriteString(&net_message, "");
	*((int *)net_message.data) = BigLong(NETFLAG_CTL | (net_message.cursize & NETFLAG_LENGTH_MASK));
	dfunc.Write (test2Socket, net_message.data, net_message.cursize, &sendaddr);
	SZ_Clear(&net_message);
	SchedulePollProcedure(&test2PollProcedure, 0.05);
}


int Datagram_Init (void)
{
	int	i, num_inited;
	sys_socket_t	csock;

#ifdef BAN_TEST
	banAddr.s_addr = INADDR_ANY;
	banMask.s_addr = INADDR_NONE;
#endif
	myDriverLevel = net_driverlevel;

	Cmd_AddCommand ("net_stats", NET_Stats_f);

	if (safemode || COM_CheckParm("-nolan"))
		return -1;

	num_inited = 0;
	for (i = 0; i < net_numlandrivers; i++)
	{
		csock = net_landrivers[i].Init ();
		if (csock == INVALID_SOCKET)
			continue;
		net_landrivers[i].initialized = true;
		net_landrivers[i].controlSock = csock;
		num_inited++;
	}

	if (num_inited == 0)
		return -1;

#ifdef BAN_TEST
	Cmd_AddCommand ("ban", NET_Ban_f);
#endif
	Cmd_AddCommand ("test", Test_f);
	Cmd_AddCommand ("test2", Test2_f);

	return 0;
}


void Datagram_Shutdown (void)
{
	int i;

//
// shutdown the lan drivers
//
	for (i = 0; i < net_numlandrivers; i++)
	{
		if (net_landrivers[i].initialized)
		{
			net_landrivers[i].Shutdown ();
			net_landrivers[i].initialized = false;
		}
	}
}


void Datagram_Close (qsocket_t *sock)
{
	sfunc.Close_Socket(sock->socket);
}


void Datagram_Listen (qboolean state)
{
	int i;

	for (i = 0; i < net_numlandrivers; i++)
	{
		if (net_landrivers[i].initialized)
			net_landrivers[i].Listen (state);
	}
}


static qsocket_t *_Datagram_CheckNewConnections (void)
{
	struct qsockaddr clientaddr;
	struct qsockaddr newaddr;
	sys_socket_t		newsock;
	sys_socket_t		acceptsock;
	qsocket_t	*sock;
	qsocket_t	*s;
	int			len;
	int			command;
	int			control;
	int			ret;

	acceptsock = dfunc.CheckNewConnections();
	if (acceptsock == INVALID_SOCKET)
		return NULL;

	SZ_Clear(&net_message);

	len = dfunc.Read (acceptsock, net_message.data, net_message.maxsize, &clientaddr);
	if (len < (int) sizeof(int))
		return NULL;
	net_message.cursize = len;

	MSG_BeginReading ();
	control = BigLong(*((int *)net_message.data));
	MSG_ReadLong();
	if (control == -1)
	{
		const char *request = MSG_ReadString();

		if (!q_strncasecmp(request, "status", 6) ||
			!q_strncasecmp(request, "info", 4) ||
			!q_strncasecmp(request, "A2A_INFO", 8))
		{
			char reply[MAX_MSGLEN];

			q_snprintf (reply, sizeof (reply),
					"\\hostname\\%s\\map\\%s\\players\\%d\\max\\%d",
					hostname.string, sv.name, net_activeconnections, svs.maxclients);

			SZ_Clear(&net_message);
			MSG_WriteLong(&net_message, -1);
			MSG_WriteString(&net_message, "statusResponse");
			MSG_WriteString(&net_message, reply);
			dfunc.Write (acceptsock, net_message.data, net_message.cursize, &clientaddr);
			SZ_Clear(&net_message);
		}
		return NULL;
	}
	if ((control & (~NETFLAG_LENGTH_MASK)) != (int)NETFLAG_CTL)
		return NULL;
	if ((control & NETFLAG_LENGTH_MASK) != len)
		return NULL;

	command = MSG_ReadByte();
	if (command == CCREQ_SERVER_INFO)
	{
		if (Q_strcmp(MSG_ReadString(), "QUAKE") != 0)
			return NULL;

		SZ_Clear(&net_message);
		// save space for the header, filled in later
		MSG_WriteLong(&net_message, 0);
		MSG_WriteByte(&net_message, CCREP_SERVER_INFO);
		dfunc.GetSocketAddr(acceptsock, &newaddr);
		MSG_WriteString(&net_message, dfunc.AddrToString(&newaddr));
		MSG_WriteString(&net_message, hostname.string);
		MSG_WriteString(&net_message, sv.name);
		MSG_WriteByte(&net_message, net_activeconnections);
		MSG_WriteByte(&net_message, svs.maxclients);
		MSG_WriteByte(&net_message, NET_PROTOCOL_VERSION);
		*((int *)net_message.data) = BigLong(NETFLAG_CTL | (net_message.cursize & NETFLAG_LENGTH_MASK));
		dfunc.Write (acceptsock, net_message.data, net_message.cursize, &clientaddr);
		SZ_Clear(&net_message);
		return NULL;
	}

	if (command == CCREQ_PLAYER_INFO)
	{
		int			playerNumber;
		int			activeNumber;
		int			clientNumber;
		client_t	*client;

		playerNumber = MSG_ReadByte();
		activeNumber = -1;

		for (clientNumber = 0, client = svs.clients; clientNumber < svs.maxclients; clientNumber++, client++)
		{
			if (client->active)
			{
				activeNumber++;
				if (activeNumber == playerNumber)
					break;
			}
		}

		if (clientNumber == svs.maxclients)
			return NULL;

		SZ_Clear(&net_message);
		// save space for the header, filled in later
		MSG_WriteLong(&net_message, 0);
		MSG_WriteByte(&net_message, CCREP_PLAYER_INFO);
		MSG_WriteByte(&net_message, playerNumber);
		MSG_WriteString(&net_message, client->name);
		MSG_WriteLong(&net_message, client->colors);
		MSG_WriteLong(&net_message, (int)client->edict->v.frags);
		MSG_WriteLong(&net_message, (int)(net_time - client->netconnection->connecttime));
		MSG_WriteString(&net_message, client->netconnection->address);
		*((int *)net_message.data) = BigLong(NETFLAG_CTL | (net_message.cursize & NETFLAG_LENGTH_MASK));
		dfunc.Write (acceptsock, net_message.data, net_message.cursize, &clientaddr);
		SZ_Clear(&net_message);

		return NULL;
	}

	if (command == CCREQ_RULE_INFO)
	{
		const char	*prevCvarName;
		cvar_t			*var;

		// find the search start location
		prevCvarName = MSG_ReadString();
		var = Cvar_FindVarAfter (prevCvarName, CVAR_SERVERINFO);

		// send the response
		SZ_Clear(&net_message);
		// save space for the header, filled in later
		MSG_WriteLong(&net_message, 0);
		MSG_WriteByte(&net_message, CCREP_RULE_INFO);
		if (var)
		{
			MSG_WriteString(&net_message, var->name);
			MSG_WriteString(&net_message, var->string);
		}
		*((int *)net_message.data) = BigLong(NETFLAG_CTL | (net_message.cursize & NETFLAG_LENGTH_MASK));
		dfunc.Write (acceptsock, net_message.data, net_message.cursize, &clientaddr);
		SZ_Clear(&net_message);

		return NULL;
	}

	if (command != CCREQ_CONNECT)
		return NULL;

	if (Q_strcmp(MSG_ReadString(), "QUAKE") != 0)
		return NULL;

	if (MSG_ReadByte() != NET_PROTOCOL_VERSION)
	{
		SZ_Clear(&net_message);
		// save space for the header, filled in later
		MSG_WriteLong(&net_message, 0);
		MSG_WriteByte(&net_message, CCREP_REJECT);
		MSG_WriteString(&net_message, "Incompatible version.\n");
		*((int *)net_message.data) = BigLong(NETFLAG_CTL | (net_message.cursize & NETFLAG_LENGTH_MASK));
		dfunc.Write (acceptsock, net_message.data, net_message.cursize, &clientaddr);
		SZ_Clear(&net_message);
		return NULL;
	}

#ifdef BAN_TEST
	// check for a ban
	if (clientaddr.qsa_family == AF_INET)
	{
		in_addr_t	testAddr;
		testAddr = ((struct sockaddr_in *)&clientaddr)->sin_addr.s_addr;
		if ((testAddr & banMask.s_addr) == banAddr.s_addr)
		{
			SZ_Clear(&net_message);
			// save space for the header, filled in later
			MSG_WriteLong(&net_message, 0);
			MSG_WriteByte(&net_message, CCREP_REJECT);
			MSG_WriteString(&net_message, "You have been banned.\n");
			*((int *)net_message.data) = BigLong(NETFLAG_CTL | (net_message.cursize & NETFLAG_LENGTH_MASK));
			dfunc.Write (acceptsock, net_message.data, net_message.cursize, &clientaddr);
			SZ_Clear(&net_message);
			return NULL;
		}
	}
#endif

	// see if this guy is already connected
	for (s = net_activeSockets; s; s = s->next)
	{
		if (s->driver != net_driverlevel)
			continue;
		ret = dfunc.AddrCompare(&clientaddr, &s->addr);
		if (ret >= 0)
		{
			// is this a duplicate connection reqeust?
			if (ret == 0 && net_time - s->connecttime < 2.0)
			{
				// yes, so send a duplicate reply
				SZ_Clear(&net_message);
				// save space for the header, filled in later
				MSG_WriteLong(&net_message, 0);
				MSG_WriteByte(&net_message, CCREP_ACCEPT);
				dfunc.GetSocketAddr(s->socket, &newaddr);
				MSG_WriteLong(&net_message, dfunc.GetSocketPort(&newaddr));
				*((int *)net_message.data) = BigLong(NETFLAG_CTL | (net_message.cursize & NETFLAG_LENGTH_MASK));
				dfunc.Write (acceptsock, net_message.data, net_message.cursize, &clientaddr);
				SZ_Clear(&net_message);
				return NULL;
			}
			// it's somebody coming back in from a crash/disconnect
			// so close the old qsocket and let their retry get them back in
			NET_Close(s);
			return NULL;
		}
	}

	// allocate a QSocket
	sock = NET_NewQSocket ();
	if (sock == NULL)	// no room; try to let him know
	{
		SZ_Clear(&net_message);
		// save space for the header, filled in later
		MSG_WriteLong(&net_message, 0);
		MSG_WriteByte(&net_message, CCREP_REJECT);
		MSG_WriteString(&net_message, "Server is full.\n");
		*((int *)net_message.data) = BigLong(NETFLAG_CTL | (net_message.cursize & NETFLAG_LENGTH_MASK));
		dfunc.Write (acceptsock, net_message.data, net_message.cursize, &clientaddr);
		SZ_Clear(&net_message);
		return NULL;
	}

	// allocate a network socket
	newsock = dfunc.Open_Socket(0);
	if (newsock == INVALID_SOCKET)
	{
		NET_FreeQSocket(sock);
		return NULL;
	}

	// connect to the client
	if (dfunc.Connect (newsock, &clientaddr) == -1)
	{
		dfunc.Close_Socket(newsock);
		NET_FreeQSocket(sock);
		return NULL;
	}

	// everything is allocated, just fill in the details
	sock->socket = newsock;
	sock->landriver = net_landriverlevel;
	sock->addr = clientaddr;
	Q_strcpy(sock->address, dfunc.AddrToString(&clientaddr));

	// send him back the info about the server connection he has been allocated
	SZ_Clear(&net_message);
	// save space for the header, filled in later
	MSG_WriteLong(&net_message, 0);
	MSG_WriteByte(&net_message, CCREP_ACCEPT);
	dfunc.GetSocketAddr(newsock, &newaddr);
	MSG_WriteLong(&net_message, dfunc.GetSocketPort(&newaddr));
//	MSG_WriteString(&net_message, dfunc.AddrToString(&newaddr));
	*((int *)net_message.data) = BigLong(NETFLAG_CTL | (net_message.cursize & NETFLAG_LENGTH_MASK));
	dfunc.Write (acceptsock, net_message.data, net_message.cursize, &clientaddr);
	SZ_Clear(&net_message);

	return sock;
}

qsocket_t *Datagram_CheckNewConnections (void)
{
	qsocket_t *ret = NULL;

	for (net_landriverlevel = 0; net_landriverlevel < net_numlandrivers; net_landriverlevel++)
	{
		if (net_landrivers[net_landriverlevel].initialized)
		{
			if ((ret = _Datagram_CheckNewConnections ()) != NULL)
				break;
		}
	}
	return ret;
}


static void _Datagram_SearchForHosts (qboolean xmit)
{
	int		ret;
	int		n;
	int		i;
	struct qsockaddr readaddr;
	struct qsockaddr myaddr;
	int		control;

	dfunc.GetSocketAddr (dfunc.controlSock, &myaddr);
	if (xmit)
	{
		SZ_Clear(&net_message);
		// save space for the header, filled in later
		MSG_WriteLong(&net_message, 0);
		MSG_WriteByte(&net_message, CCREQ_SERVER_INFO);
		MSG_WriteString(&net_message, "QUAKE");
		MSG_WriteByte(&net_message, NET_PROTOCOL_VERSION);
		*((int *)net_message.data) = BigLong(NETFLAG_CTL | (net_message.cursize & NETFLAG_LENGTH_MASK));
		dfunc.Broadcast(dfunc.controlSock, net_message.data, net_message.cursize);
		SZ_Clear(&net_message);
	}

	while ((ret = dfunc.Read (dfunc.controlSock, net_message.data, net_message.maxsize, &readaddr)) > 0)
	{
		if (ret < (int) sizeof(int))
			continue;
		net_message.cursize = ret;

		// don't answer our own query
		if (dfunc.AddrCompare(&readaddr, &myaddr) >= 0)
			continue;

		// is the cache full?
		if (hostCacheCount == HOSTCACHESIZE)
			continue;

		MSG_BeginReading ();
		control = BigLong(*((int *)net_message.data));
		MSG_ReadLong();
		if (control == -1)
			continue;
		if ((control & (~NETFLAG_LENGTH_MASK)) != (int)NETFLAG_CTL)
			continue;
		if ((control & NETFLAG_LENGTH_MASK) != ret)
			continue;

		if (MSG_ReadByte() != CCREP_SERVER_INFO)
			continue;

		dfunc.GetAddrFromName(MSG_ReadString(), &readaddr);
		// search the cache for this server
		for (n = 0; n < hostCacheCount; n++)
		{
			if (dfunc.AddrCompare(&readaddr, &hostcache[n].addr) == 0)
				break;
		}

		// is it already there?
		if (n < hostCacheCount)
			continue;

		// add it
		hostCacheCount++;
		Q_strcpy(hostcache[n].name, MSG_ReadString());
		Q_strcpy(hostcache[n].map, MSG_ReadString());
		hostcache[n].users = MSG_ReadByte();
		hostcache[n].maxusers = MSG_ReadByte();
		if (MSG_ReadByte() != NET_PROTOCOL_VERSION)
		{
			Q_strcpy(hostcache[n].cname, hostcache[n].name);
			hostcache[n].cname[14] = 0;
			Q_strcpy(hostcache[n].name, "*");
			Q_strcat(hostcache[n].name, hostcache[n].cname);
		}
		Q_memcpy(&hostcache[n].addr, &readaddr, sizeof(struct qsockaddr));
		hostcache[n].driver = net_driverlevel;
		hostcache[n].ldriver = net_landriverlevel;
		Q_strcpy(hostcache[n].cname, dfunc.AddrToString(&readaddr));

		// check for a name conflict
		for (i = 0; i < hostCacheCount; i++)
		{
			if (i == n)
				continue;
			if (q_strcasecmp (hostcache[n].name, hostcache[i].name) == 0)
			{
				i = Q_strlen(hostcache[n].name);
				if (i < 15 && hostcache[n].name[i-1] > '8')
				{
					hostcache[n].name[i] = '0';
					hostcache[n].name[i+1] = 0;
				}
				else
					hostcache[n].name[i-1]++;

				i = -1;
			}
		}
	}
}

void Datagram_SearchForHosts (qboolean xmit)
{
	for (net_landriverlevel = 0; net_landriverlevel < net_numlandrivers; net_landriverlevel++)
	{
		if (hostCacheCount == HOSTCACHESIZE)
			break;
		if (net_landrivers[net_landriverlevel].initialized)
			_Datagram_SearchForHosts (xmit);
	}
}


static qsocket_t *_Datagram_Connect (const char *host)
{
	struct qsockaddr sendaddr;
	struct qsockaddr readaddr;
	qsocket_t	*sock;
	sys_socket_t		newsock;
	int			ret;
	int			reps;
	double		start_time;
	int			control;
	const char		*reason;

	// see if we can resolve the host name
	if (dfunc.GetAddrFromName(host, &sendaddr) == -1)
	{
		Con_Printf("Could not resolve %s\n", host);
		return NULL;
	}

	newsock = dfunc.Open_Socket (0);
	if (newsock == INVALID_SOCKET)
		return NULL;

	sock = NET_NewQSocket ();
	if (sock == NULL)
		goto ErrorReturn2;
	sock->socket = newsock;
	sock->landriver = net_landriverlevel;

	// connect to the host
	if (dfunc.Connect (newsock, &sendaddr) == -1)
		goto ErrorReturn;

	// send the connection request
	Con_Printf("trying...\n");
	SCR_UpdateScreen ();
	start_time = net_time;

	for (reps = 0; reps < 3; reps++)
	{
		SZ_Clear(&net_message);
		// save space for the header, filled in later
		MSG_WriteLong(&net_message, 0);
		MSG_WriteByte(&net_message, CCREQ_CONNECT);
		MSG_WriteString(&net_message, "QUAKE");
		MSG_WriteByte(&net_message, NET_PROTOCOL_VERSION);
		*((int *)net_message.data) = BigLong(NETFLAG_CTL | (net_message.cursize & NETFLAG_LENGTH_MASK));
		dfunc.Write (newsock, net_message.data, net_message.cursize, &sendaddr);
		SZ_Clear(&net_message);
		do
		{
			ret = dfunc.Read (newsock, net_message.data, net_message.maxsize, &readaddr);
			// if we got something, validate it
			if (ret > 0)
			{
				// is it from the right place?
				if (sfunc.AddrCompare(&readaddr, &sendaddr) != 0)
				{
					Con_Printf("wrong reply address\n");
					Con_Printf("Expected: %s | %s\n", dfunc.AddrToString (&sendaddr), StrAddr(&sendaddr));
					Con_Printf("Received: %s | %s\n", dfunc.AddrToString (&readaddr), StrAddr(&readaddr));
					SCR_UpdateScreen ();
					ret = 0;
					continue;
				}

				if (ret < (int) sizeof(int))
				{
					ret = 0;
					continue;
				}

				net_message.cursize = ret;
				MSG_BeginReading ();

				control = BigLong(*((int *)net_message.data));
				MSG_ReadLong();
				if (control == -1)
				{
					ret = 0;
					continue;
				}
				if ((control & (~NETFLAG_LENGTH_MASK)) != (int)NETFLAG_CTL)
				{
					ret = 0;
					continue;
				}
				if ((control & NETFLAG_LENGTH_MASK) != ret)
				{
					ret = 0;
					continue;
				}
			}
		}
		while (ret == 0 && (SetNetTime() - start_time) < 2.5);

		if (ret)
			break;

		Con_Printf("still trying...\n");
		SCR_UpdateScreen ();
		start_time = SetNetTime();
	}

	if (ret == 0)
	{
		reason = "No Response";
		Con_Printf("%s\n", reason);
		Q_strcpy(m_return_reason, reason);
		goto ErrorReturn;
	}

	if (ret == -1)
	{
		reason = "Network Error";
		Con_Printf("%s\n", reason);
		Q_strcpy(m_return_reason, reason);
		goto ErrorReturn;
	}

	ret = MSG_ReadByte();
	if (ret == CCREP_REJECT)
	{
		reason = MSG_ReadString();
		Con_Printf("%s\n", reason);
		q_strlcpy(m_return_reason, reason, sizeof(m_return_reason));
		goto ErrorReturn;
	}

	if (ret == CCREP_ACCEPT)
	{
		Q_memcpy(&sock->addr, &sendaddr, sizeof(struct qsockaddr));
		dfunc.SetSocketPort (&sock->addr, MSG_ReadLong());
	}
	else
	{
		reason = "Bad Response";
		Con_Printf("%s\n", reason);
		Q_strcpy(m_return_reason, reason);
		goto ErrorReturn;
	}

	dfunc.GetNameFromAddr (&sendaddr, sock->address);

	Con_Printf ("Connection accepted\n");
	sock->lastMessageTime = SetNetTime();

	// switch the connection to the specified address
	if (dfunc.Connect (newsock, &sock->addr) == -1)
	{
		reason = "Connect to Game failed";
		Con_Printf("%s\n", reason);
		Q_strcpy(m_return_reason, reason);
		goto ErrorReturn;
	}

	m_return_onerror = false;
	return sock;

ErrorReturn:
	NET_FreeQSocket(sock);
ErrorReturn2:
	dfunc.Close_Socket(newsock);
	if (m_return_onerror)
	{
		IN_DeactivateForMenu();
		key_dest = key_menu;
		m_state = m_return_state;
		m_return_onerror = false;
	}
	return NULL;
}

qsocket_t *Datagram_Connect (const char *host)
{
	qsocket_t *ret = NULL;

	host = Strip_Port (host);
	for (net_landriverlevel = 0; net_landriverlevel < net_numlandrivers; net_landriverlevel++)
	{
		if (net_landrivers[net_landriverlevel].initialized)
		{
			if ((ret = _Datagram_Connect (host)) != NULL)
				break;
		}
	}
	return ret;
}
