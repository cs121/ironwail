#include "quakedef.h"
#include "sv_coalesce.h"

#define COALESCE_FLUSH_BUDGET 2
#define COALESCE_DRIFT_CLAMP 0.25

cvar_t sv_tickrate = {"sv_tickrate", "60", CVAR_NONE};
cvar_t sv_netrate = {"sv_netrate", "30", CVAR_NONE};
cvar_t sv_netburst = {"sv_netburst", "1", CVAR_NONE};
cvar_t sv_pktmax = {"sv_pktmax", "1200", CVAR_NONE};
cvar_t sv_netcoalesce = {"sv_netcoalesce", "1", CVAR_NONE};
cvar_t sv_net_debug = {"sv_net_debug", "0", CVAR_NONE};

static unsigned int sv_net_packets_sent;
static unsigned int sv_net_bytes_sent;
static unsigned int sv_net_coalesced_msgs;
static unsigned int sv_net_split_packets;
static unsigned int sv_net_dropped_msgs;
static double sv_net_debug_next_time;

void SV_Coalesce_FlushClient(client_t *client);

static double SV_Coalesce_Interval(void)
{
	double rate = sv_netrate.value;

	if (rate <= 0.0)
		return 0.0;

	if (rate < 1.0)
		rate = 1.0;

	return 1.0 / rate;
}

static int SV_Coalesce_PacketCap(client_t *client)
{
	int cap = MAX_DATAGRAM;

	if (Q_strcmp(NET_QSocketGetAddressString(client->netconnection), "LOCAL") != 0)
		cap = DATAGRAM_MTU;

	if (sv_pktmax.value > 0)
		cap = q_min(cap, (int)sv_pktmax.value);

	return cap;
}

static int SV_Coalesce_CalcSendBytes(const sizebuf_t *buf, int cap)
{
	int pos = 0;
	int total = 0;

	if (buf->cursize < 2)
		return 0;

	if (cap <= 0)
		cap = buf->maxsize;

	while (pos + 2 <= buf->cursize)
	{
		int len = buf->data[pos] | (buf->data[pos + 1] << 8);

		if (len <= 0)
			return 0;

		if (pos + 2 + len > buf->cursize)
			return 0;

		if (total == 0 && 2 + len > cap)
			return 2 + len;

		if (total + 2 + len > cap)
			break;

		total += 2 + len;
		pos += 2 + len;
	}

	return total;
}

void SV_Coalesce_RegisterCvars(void)
{
	Cvar_RegisterVariable(&sv_tickrate);
	Cvar_RegisterVariable(&sv_netrate);
	Cvar_RegisterVariable(&sv_netburst);
	Cvar_RegisterVariable(&sv_pktmax);
	Cvar_RegisterVariable(&sv_netcoalesce);
	Cvar_RegisterVariable(&sv_net_debug);
}

void SV_Coalesce_UpdateProtocolFlags(void)
{
	if (sv.protocol == PROTOCOL_RMQ && sv_netcoalesce.value)
		sv.protocolflags |= PRFL_NET_COALESCE;
}

void SV_Coalesce_InitClient(client_t *client)
{
	client->next_send_time = realtime;
	client->coalesce_buf.data = client->coalesce_storage;
	client->coalesce_buf.maxsize = sizeof(client->coalesce_storage);
	client->coalesce_buf.cursize = 0;
	client->coalesce_buf.allowoverflow = false;
	client->coalesce_framing = (sv.protocol == PROTOCOL_RMQ) && (sv.protocolflags & PRFL_NET_COALESCE);
}

void SV_Coalesce_Enqueue(client_t *client, const byte *data, int len)
{
	sizebuf_t *buf;

	if (!sv_netcoalesce.value)
		return;

	if (len <= 0)
		return;

	buf = &client->coalesce_buf;

	if (!client->coalesce_framing)
	{
		if (len > buf->maxsize)
			return;

		if (buf->cursize)
		{
			sv_net_dropped_msgs++;
			buf->cursize = 0;
		}

		memcpy(buf->data, data, len);
		buf->cursize = len;
		sv_net_coalesced_msgs++;
		return;
	}

	if (len > 0xFFFF)
		return;

	if (len + 2 > buf->maxsize)
		return;

	if (buf->cursize + len + 2 > buf->maxsize)
	{
		while (buf->cursize && buf->cursize + len + 2 > buf->maxsize)
		{
			int before = buf->cursize;

			SV_Coalesce_FlushClient(client);

			if (buf->cursize >= before)
				break;
		}

		if (buf->cursize + len + 2 > buf->maxsize)
		{
			if (buf->cursize)
				sv_net_dropped_msgs++;
			buf->cursize = 0;
		}
	}

	buf->data[buf->cursize] = len & 0xff;
	buf->data[buf->cursize + 1] = (len >> 8) & 0xff;
	memcpy(buf->data + buf->cursize + 2, data, len);
	buf->cursize += len + 2;
	sv_net_coalesced_msgs++;
}

void SV_Coalesce_FlushClient(client_t *client)
{
	byte packetbuf[MAX_DATAGRAM];
	sizebuf_t *buf;
	int cap;
	int budget;
	int sent_packets = 0;

	buf = &client->coalesce_buf;
	if (!buf->cursize)
		return;

	if (!client->coalesce_framing)
	{
		sizebuf_t msg;

		msg.data = packetbuf;
		msg.maxsize = sizeof(packetbuf);
		msg.cursize = buf->cursize;
		memcpy(msg.data, buf->data, buf->cursize);

		if (sv_pktmax.value > 0 && msg.cursize > SV_Coalesce_PacketCap(client) && sv_net_debug.value)
			Con_DWarning("sv_netcoalesce: oversized legacy datagram (%d bytes)\n", msg.cursize);

		if (NET_SendUnreliableMessage(client->netconnection, &msg) == -1)
		{
			SV_DropClient(true);
			return;
		}

		sv_net_packets_sent++;
		sv_net_bytes_sent += msg.cursize;
		buf->cursize = 0;
		return;
	}

	cap = SV_Coalesce_PacketCap(client);
	budget = COALESCE_FLUSH_BUDGET;

	while (budget-- > 0 && buf->cursize > 0)
	{
		int send_bytes = SV_Coalesce_CalcSendBytes(buf, cap);
		sizebuf_t msg;

		if (send_bytes <= 0)
		{
			sv_net_dropped_msgs++;
			buf->cursize = 0;
			break;
		}

		msg.data = packetbuf;
		msg.maxsize = sizeof(packetbuf);
		msg.cursize = send_bytes;
		memcpy(msg.data, buf->data, send_bytes);

		if (cap > 0 && send_bytes > cap && sv_net_debug.value)
			Con_DWarning("sv_netcoalesce: oversized packet (%d bytes)\n", send_bytes);

		if (NET_SendUnreliableMessage(client->netconnection, &msg) == -1)
		{
			SV_DropClient(true);
			return;
		}

		sv_net_packets_sent++;
		sv_net_bytes_sent += msg.cursize;
		sent_packets++;

		if (buf->cursize > send_bytes)
			memmove(buf->data, buf->data + send_bytes, buf->cursize - send_bytes);
		buf->cursize -= send_bytes;
	}

	if (sent_packets > 1)
		sv_net_split_packets += sent_packets - 1;
}

void SV_Coalesce_MaybeFlushClient(client_t *client, qboolean force)
{
	double interval;

	if (!sv_netcoalesce.value)
		return;

	interval = SV_Coalesce_Interval();
	if (client->next_send_time <= 0.0)
		client->next_send_time = realtime;

	if (!force && realtime < client->next_send_time)
		return;

	if (client->coalesce_buf.cursize)
		SV_Coalesce_FlushClient(client);
	else if (!force)
		return;

	if (interval <= 0.0)
	{
		client->next_send_time = realtime;
		return;
	}

	if (force && realtime < client->next_send_time)
		client->next_send_time = realtime + interval;
	else
		client->next_send_time += interval;

	if (realtime - client->next_send_time > COALESCE_DRIFT_CLAMP)
		client->next_send_time = realtime + interval;
}

void SV_Coalesce_DebugUpdate(void)
{
	if (!sv_net_debug.value)
		return;

	if (realtime < sv_net_debug_next_time)
		return;

	Con_Printf("sv_net: pkts=%u bytes=%u coalesced=%u split=%u dropped=%u\n",
		sv_net_packets_sent,
		sv_net_bytes_sent,
		sv_net_coalesced_msgs,
		sv_net_split_packets,
		sv_net_dropped_msgs);

	sv_net_debug_next_time = realtime + 5.0;
}
