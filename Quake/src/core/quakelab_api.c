#include "quakedef.h"
#include "json.h"
#include "quakelab_api.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET ql_socket_t;
#define QL_INVALID_SOCKET INVALID_SOCKET
#define QL_CLOSESOCKET closesocket
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int ql_socket_t;
#define QL_INVALID_SOCKET (-1)
#define QL_CLOSESOCKET close
#define SOCKET_ERROR (-1)
typedef int u_long;
#endif

#define QLAB_MAX_IO 8192
#define QLAB_MAX_LINES_PER_REQUEST 1000

static cvar_t qlab_api_enable = {"qlab_api_enable", "0", CVAR_ARCHIVE};
static cvar_t qlab_api_protocol = {"qlab_api_protocol", "stub", CVAR_ARCHIVE};
static cvar_t qlab_api_host = {"qlab_api_host", "127.0.0.1", CVAR_ARCHIVE};
static cvar_t qlab_api_port = {"qlab_api_port", "27955", CVAR_ARCHIVE};
static cvar_t qlab_api_allow_remote = {"qlab_api_allow_remote", "0", CVAR_ARCHIVE};
static cvar_t qlab_api_auth_token = {"qlab_api_auth_token", "", CVAR_ARCHIVE};
static cvar_t qlab_api_allow_shutdown = {"qlab_api_allow_shutdown", "0", CVAR_ARCHIVE};
static cvar_t qlab_api_command_denylist = {"qlab_api_command_denylist", "quit;exec;writeconfig;fs_", CVAR_ARCHIVE};

static ql_socket_t qlab_listen = QL_INVALID_SOCKET;
static ql_socket_t qlab_client = QL_INVALID_SOCKET;
static qboolean qlab_sub_console = false;
static qboolean qlab_ws_mode = false;
static qboolean qlab_ws_ready = false;
static int qlab_last_console_line = 0;
static char qlab_inbuf[QLAB_MAX_IO];
static size_t qlab_inlen = 0;

static void SHA1_Compute (const unsigned char *data, size_t len, unsigned char out[20]);

static void QLab_JsonEscape (char *dst, size_t dstsize, const char *src)
{
	size_t i = 0;
	if (!dst || dstsize == 0) return;
	while (src && *src && i + 1 < dstsize)
	{
		unsigned char c = (unsigned char)*src++;
		if ((c == '"' || c == '\\') && i + 2 < dstsize) { dst[i++]='\\'; dst[i++]=(char)c; }
		else if (c == '\n' && i + 2 < dstsize) { dst[i++]='\\'; dst[i++]='n'; }
		else if (c == '\r' && i + 2 < dstsize) { dst[i++]='\\'; dst[i++]='r'; }
		else if (c == '\t' && i + 2 < dstsize) { dst[i++]='\\'; dst[i++]='t'; }
		else if (c >= 32) dst[i++] = (char)c;
	}
	dst[i] = 0;
}

static int QLab_Base64 (const unsigned char *in, int inlen, char *out, int outlen)
{
	static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	int i=0,o=0;
	while (i < inlen && o + 4 < outlen)
	{
		int a = in[i++];
		int b = (i < inlen) ? in[i++] : -1;
		int c = (i < inlen) ? in[i++] : -1;
		out[o++] = t[(a >> 2) & 63];
		out[o++] = t[((a & 3) << 4) | ((b >= 0 ? b : 0) >> 4)];
		out[o++] = (b >= 0) ? t[((b & 15) << 2) | ((c >= 0 ? c : 0) >> 6)] : '=';
		out[o++] = (c >= 0) ? t[c & 63] : '=';
	}
	out[o] = 0;
	return o;
}

static qboolean QLab_ExtractHeader (const char *req, const char *name, char *out, size_t outsz)
{
	const char *p = req;
	size_t nlen = (size_t)Q_strlen(name);
	while ((p = strstr (p, name)) != NULL)
	{
		const char *v;
		if (p != req && p[-1] != '\n') { p += nlen; continue; }
		v = p + nlen;
		while (*v == ' ' || *v == ':') ++v;
		q_strlcpy (out, v, outsz);
		v = strstr (out, "\r");
		if (v) ((char*)v)[0] = 0;
		return true;
	}
	return false;
}

static qboolean QLab_WSHandshake (const char *req)
{
	char key[256], combo[512], accept[128], resp[512];
	unsigned char dig[20];
	if (!QLab_ExtractHeader (req, "Sec-WebSocket-Key", key, sizeof key)) return false;
	q_snprintf (combo, sizeof combo, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
	SHA1_Compute ((const unsigned char*)combo, (size_t)Q_strlen(combo), dig);
	QLab_Base64 (dig, 20, accept, (int)sizeof accept);
	q_snprintf (resp, sizeof resp,
		"HTTP/1.1 101 Switching Protocols\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Accept: %s\r\n\r\n", accept);
	send (qlab_client, resp, Q_strlen(resp), 0);
	return true;
}

static void QLab_SendRaw (const char *s)
{
	if (qlab_client == QL_INVALID_SOCKET || !s) return;
	send (qlab_client, s, Q_strlen(s), 0);
}

static void QLab_SendText (const char *json)
{
	if (!qlab_ws_mode) { QLab_SendRaw(json); QLab_SendRaw("\n"); return; }
	{
		unsigned char hdr[4];
		int len = Q_strlen(json);
		if (len > 125) len = 125;
		hdr[0] = 0x81;
		hdr[1] = (unsigned char)len;
		send (qlab_client, (const char*)hdr, 2, 0);
		send (qlab_client, json, len, 0);
	}
}

static void QLab_SendResponseOk (const char *request_id, const char *result_json)
{
	char id[128], out[QLAB_MAX_IO];
	QLab_JsonEscape (id, sizeof id, request_id ? request_id : "");
	q_snprintf (out, sizeof out, "{\"type\":\"response\",\"request_id\":\"%s\",\"ok\":true,\"result\":%s}", id, result_json ? result_json : "{}");
	QLab_SendText (out);
}

static void QLab_SendResponseErr (const char *request_id, const char *code, const char *message)
{
	char id[128], c[64], m[256], out[QLAB_MAX_IO];
	QLab_JsonEscape (id, sizeof id, request_id ? request_id : "");
	QLab_JsonEscape (c, sizeof c, code ? code : "INTERNAL_ERROR");
	QLab_JsonEscape (m, sizeof m, message ? message : "error");
	q_snprintf (out, sizeof out, "{\"type\":\"response\",\"request_id\":\"%s\",\"ok\":false,\"error\":{\"code\":\"%s\",\"message\":\"%s\",\"details\":{}}}", id, c, m);
	QLab_SendText (out);
}

/* existing command handler from previous version, shortened */
static void QLab_HandleRequest (const char *line)
{
	json_t *json = JSON_Parse (line);
	const jsonentry_t *root, *params; const char *type, *request_id, *command; char out[QLAB_MAX_IO], esc[512];
	if (!json || !json->root) { QLab_SendResponseErr ("", "INVALID_PARAMS", "Malformed JSON"); goto done; }
	root = json->root; type = JSON_FindString(root,"type"); request_id = JSON_FindString(root,"request_id"); command = JSON_FindString(root,"command"); params = JSON_Find(root,"params",JSON_OBJECT);
	if (!type || Q_strcmp(type,"request")) { QLab_SendResponseErr(request_id,"INVALID_PARAMS","type must be request"); goto done; }
	if (!command) { QLab_SendResponseErr(request_id,"INVALID_PARAMS","Missing command"); goto done; }
	if (!Q_strcmp(command,"ping")) QLab_SendResponseOk(request_id,"{\"pong\":true}");
	else if (!Q_strcmp(command,"handshake")) QLab_SendResponseOk(request_id,"{\"protocol_version\":\"1.0\",\"server\":\"ironwail\"}");
	else if (!Q_strcmp(command,"get_engine_info")) { q_snprintf(out,sizeof out,"{\"name\":\"Ironwail\",\"protocol_version\":\"1.0\",\"version\":\"%s\",\"map\":\"%s\"}",IRONWAIL_VER_STRING,cls.state==ca_connected?cl.mapname:""); QLab_SendResponseOk(request_id,out); }
	else if (!Q_strcmp(command,"get_capabilities")) QLab_SendResponseOk(request_id,"{\"protocol_version\":\"1.0\",\"capabilities\":[\"ping\",\"handshake\",\"get_engine_info\",\"get_capabilities\",\"execute_console_command\",\"get_cvar\",\"set_cvar\",\"list_cvars\",\"read_console_log\",\"subscribe_console_output\",\"start_map\",\"reload_map\",\"get_current_map\"],\"events\":[\"console_output\",\"map_changed\"]}");
	else if (!Q_strcmp(command,"execute_console_command")) { const char *cmd=params?JSON_FindString(params,"command"):NULL; if(!cmd||!cmd[0]) QLab_SendResponseErr(request_id,"INVALID_PARAMS","Missing command"); else { Cmd_ExecuteString(cmd,src_command); QLab_SendResponseOk(request_id,"{\"executed\":true}"); }}
	else if (!Q_strcmp(command,"get_current_map")) { QLab_JsonEscape(esc,sizeof esc,cls.state==ca_connected?cl.mapname:""); q_snprintf(out,sizeof out,"{\"map\":\"%s\"}",esc); QLab_SendResponseOk(request_id,out); }
	else if (!Q_strcmp(command,"start_map")) { const char *m=params?JSON_FindString(params,"map"):NULL; if(!m||!m[0]) QLab_SendResponseErr(request_id,"INVALID_PARAMS","Missing map"); else { Cbuf_AddText(va("map %s\n",m)); QLab_SendResponseOk(request_id,"{\"queued\":true}"); }}
	else if (!Q_strcmp(command,"reload_map")) { if(cls.state==ca_connected&&cl.mapname[0]) { Cbuf_AddText(va("map %s\n",cl.mapname)); QLab_SendResponseOk(request_id,"{\"queued\":true}"); } else QLab_SendResponseErr(request_id,"NOT_CONNECTED","No active map"); }
	else if (!Q_strcmp(command,"subscribe_console_output")) { qlab_sub_console=true; qlab_last_console_line=Con_GetCurrentLine(); QLab_SendResponseOk(request_id,"{\"subscribed\":true}"); }
	else QLab_SendResponseErr(request_id,"UNKNOWN_COMMAND","Unknown command");
done:
	if (json) JSON_Free(json);
}

static void QLab_CloseClient (void){ if(qlab_client!=QL_INVALID_SOCKET) QL_CLOSESOCKET(qlab_client); qlab_client=QL_INVALID_SOCKET; qlab_inlen=0; qlab_sub_console=false; qlab_ws_ready=false; }
static void QLab_CloseListen (void){ if(qlab_listen!=QL_INVALID_SOCKET) QL_CLOSESOCKET(qlab_listen); qlab_listen=QL_INVALID_SOCKET; QLab_CloseClient(); }

static void QLab_OpenListen (void)
{
	struct sockaddr_in a; u_long nb=1; int port=Q_atoi(qlab_api_port.string); const char *ip = qlab_api_allow_remote.value?qlab_api_host.string:"127.0.0.1";
	QLab_CloseListen(); qlab_listen = socket(PF_INET,SOCK_STREAM,IPPROTO_TCP); if(qlab_listen==QL_INVALID_SOCKET) return;
	ioctlsocket(qlab_listen,FIONBIO,&nb); memset(&a,0,sizeof a); a.sin_family=AF_INET; a.sin_port=htons((unsigned short)port); a.sin_addr.s_addr=inet_addr(ip);
	if (bind(qlab_listen,(struct sockaddr*)&a,sizeof a)==SOCKET_ERROR||listen(qlab_listen,1)==SOCKET_ERROR){ QLab_CloseListen(); return; }
	Con_Printf("QuakeLab API listening on %s:%d (%s)\n",ip,port,qlab_api_protocol.string);
}

static void QLab_ProcessTextLineBuffer (void)
{
	char *nl;
	while ((nl = strchr(qlab_inbuf,'\n'))!=NULL)
	{
		size_t linelen=(size_t)(nl-qlab_inbuf);
		if(linelen>0&&qlab_inbuf[linelen-1]=='\r') qlab_inbuf[linelen-1]=0; else qlab_inbuf[linelen]=0;
		QLab_HandleRequest(qlab_inbuf);
		memmove(qlab_inbuf,nl+1,qlab_inlen-(linelen+1)); qlab_inlen -= linelen+1; qlab_inbuf[qlab_inlen]=0;
	}
}

static void QLab_ProcessWSFrames (void)
{
	while (qlab_inlen >= 2)
	{
		unsigned char *b=(unsigned char*)qlab_inbuf; int fin=b[0]&0x80, opcode=b[0]&0x0f; int masked=b[1]&0x80; size_t len=b[1]&0x7f; size_t off=2,i;
		unsigned char mask[4];
		if(!fin||len>125||!masked) { QLab_CloseClient(); return; }
		if (qlab_inlen < off+4+len) return;
		memcpy(mask,qlab_inbuf+off,4); off+=4;
		for(i=0;i<len;++i) qlab_inbuf[i]=(char)(qlab_inbuf[off+i]^mask[i&3]);
		qlab_inbuf[len]=0;
		if(opcode==0x8){ QLab_CloseClient(); return; }
		if(opcode==0x9){ unsigned char pong[2]={0x8A,0}; send(qlab_client,(const char*)pong,2,0); }
		else if(opcode==0x1){ QLab_HandleRequest(qlab_inbuf); }
		memmove(qlab_inbuf,qlab_inbuf+off+len,qlab_inlen-(off+len)); qlab_inlen -= off+len;
	}
}

void QuakeLabAPI_Init (void)
{
	Cvar_RegisterVariable(&qlab_api_enable); Cvar_RegisterVariable(&qlab_api_protocol); Cvar_RegisterVariable(&qlab_api_host);
	Cvar_RegisterVariable(&qlab_api_port); Cvar_RegisterVariable(&qlab_api_allow_remote); Cvar_RegisterVariable(&qlab_api_auth_token);
	Cvar_RegisterVariable(&qlab_api_allow_shutdown); Cvar_RegisterVariable(&qlab_api_command_denylist);
}

void QuakeLabAPI_Frame (void)
{
	char rx[2048]; int n;
	qlab_ws_mode = !q_strcasecmp(qlab_api_protocol.string,"ws") || !q_strcasecmp(qlab_api_protocol.string,"http+ws");
	if (!qlab_api_enable.value || (q_strcasecmp(qlab_api_protocol.string,"stub") && !qlab_ws_mode)) { QLab_CloseListen(); return; }
	if (qlab_listen==QL_INVALID_SOCKET) QLab_OpenListen(); if (qlab_listen==QL_INVALID_SOCKET) return;
	if (qlab_client==QL_INVALID_SOCKET)
	{
		struct sockaddr_in c; int cl=sizeof c; ql_socket_t s=accept(qlab_listen,(struct sockaddr*)&c,&cl);
		if(s!=QL_INVALID_SOCKET){ u_long nb=1; ioctlsocket(s,FIONBIO,&nb); if(!qlab_api_allow_remote.value&&ntohl(c.sin_addr.s_addr)!=INADDR_LOOPBACK) QL_CLOSESOCKET(s); else { qlab_client=s; qlab_last_console_line=Con_GetCurrentLine(); qlab_ws_ready=!qlab_ws_mode; }}
	}
	if (qlab_client==QL_INVALID_SOCKET) return;
	for(;;){ n=recv(qlab_client,rx,sizeof(rx),0); if(n<=0) break; if(qlab_inlen+(size_t)n>=sizeof qlab_inbuf){ qlab_inlen=0; continue; } memcpy(qlab_inbuf+qlab_inlen,rx,(size_t)n); qlab_inlen+=(size_t)n; if(!qlab_ws_mode) { qlab_inbuf[qlab_inlen]=0; QLab_ProcessTextLineBuffer(); }
		else if(!qlab_ws_ready){ char *e; qlab_inbuf[qlab_inlen]=0; e=strstr(qlab_inbuf,"\r\n\r\n"); if(e){ if(!QLab_WSHandshake(qlab_inbuf)){ QLab_CloseClient(); return; } qlab_ws_ready=true; qlab_inlen=0; }} else QLab_ProcessWSFrames(); }
	if(n==0){ QLab_CloseClient(); return; }
	if(qlab_sub_console&&qlab_client!=QL_INVALID_SOCKET){ int cur=Con_GetCurrentLine(),st=qlab_last_console_line+1,i,min=cur-Con_GetTotalLines()+1; char l[1024],e[2048],o[QLAB_MAX_IO]; if(st<min) st=min; for(i=st;i<=cur;++i){ Con_CopyLine(i,l,sizeof l); QLab_JsonEscape(e,sizeof e,l); q_snprintf(o,sizeof o,"{\"type\":\"event\",\"event\":\"console_output\",\"payload\":{\"line\":\"%s\"}}",e); QLab_SendText(o);} qlab_last_console_line=cur; }
}

void QuakeLabAPI_Shutdown (void){ QLab_CloseListen(); }

/* tiny SHA1 */
static unsigned rol32(unsigned x, unsigned n){ return (x<<n)|(x>>(32-n)); }
static void SHA1_Compute (const unsigned char *data, size_t len, unsigned char out[20])
{
	unsigned h0=0x67452301,h1=0xEFCDAB89,h2=0x98BADCFE,h3=0x10325476,h4=0xC3D2E1F0;
	size_t ml=len*8, i, j, blocks=((len+9+63)/64); unsigned char buf[64];
	for(i=0;i<blocks;++i){ unsigned w[80],a,b,c,d,e,f,k,temp; size_t off=i*64;
		memset(buf,0,64);
		for(j=0;j<64 && off+j<len;++j) buf[j]=data[off+j];
		if(off<=len && len<off+64){ buf[len-off]=0x80; }
		if(i==blocks-1){ buf[56]=(unsigned char)(ml>>56); buf[57]=(unsigned char)(ml>>48); buf[58]=(unsigned char)(ml>>40); buf[59]=(unsigned char)(ml>>32); buf[60]=(unsigned char)(ml>>24); buf[61]=(unsigned char)(ml>>16); buf[62]=(unsigned char)(ml>>8); buf[63]=(unsigned char)ml; }
		for(j=0;j<16;++j) w[j]=((unsigned)buf[j*4]<<24)|((unsigned)buf[j*4+1]<<16)|((unsigned)buf[j*4+2]<<8)|buf[j*4+3];
		for(j=16;j<80;++j) w[j]=rol32(w[j-3]^w[j-8]^w[j-14]^w[j-16],1);
		a=h0;b=h1;c=h2;d=h3;e=h4;
		for(j=0;j<80;++j){ if(j<20){f=(b&c)|((~b)&d);k=0x5A827999;} else if(j<40){f=b^c^d;k=0x6ED9EBA1;} else if(j<60){f=(b&c)|(b&d)|(c&d);k=0x8F1BBCDC;} else {f=b^c^d;k=0xCA62C1D6;} temp=rol32(a,5)+f+e+k+w[j]; e=d;d=c;c=rol32(b,30);b=a;a=temp; }
		h0+=a;h1+=b;h2+=c;h3+=d;h4+=e;
	}
	out[0]=h0>>24;out[1]=h0>>16;out[2]=h0>>8;out[3]=h0; out[4]=h1>>24;out[5]=h1>>16;out[6]=h1>>8;out[7]=h1; out[8]=h2>>24;out[9]=h2>>16;out[10]=h2>>8;out[11]=h2; out[12]=h3>>24;out[13]=h3>>16;out[14]=h3>>8;out[15]=h3; out[16]=h4>>24;out[17]=h4>>16;out[18]=h4>>8;out[19]=h4;
}
