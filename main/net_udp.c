/*
 * 
 * Routines for managing UDP-protocol network play.
 * 
 */

#ifdef NETWORK

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "strutil.h"
#include "pstypes.h"
#include "args.h"
#include "timer.h"
#include "newmenu.h"
#include "key.h"
#include "gauges.h"
#include "object.h"
#include "error.h"
#include "laser.h"
#include "gamesave.h"
#include "gamemine.h"
#include "player.h"
#include "gameseq.h"
#include "fireball.h"
#include "net_udp.h"
#include "game.h"
#include "multi.h"
#include "endlevel.h"
#include "palette.h"
#include "fuelcen.h"
#include "menu.h"
#include "sounds.h"
#include "text.h"
#include "kmatrix.h"
#include "newdemo.h"
#include "multibot.h"
#include "wall.h"
#include "bm.h"
#include "effects.h"
#include "physics.h"
#include "vers_id.h"
#include "gamefont.h"
#include "playsave.h"
#include "rbaudio.h"
#include "byteswap.h"
#include "config.h"

// Prototypes
void net_udp_init();
void net_udp_request_game_info(struct _sockaddr game_addr, int lite);
void net_udp_listen();
int net_udp_show_game_info();
int net_udp_do_join_game();
int net_udp_can_join_netgame(netgame_info *game);
void net_udp_flush();
void net_udp_update_netgame(void);
void net_udp_send_objects(void);
void net_udp_send_rejoin_sync(int player_num);
void net_udp_send_game_info(struct _sockaddr sender_addr, ubyte info_upid);
void net_udp_send_netgame_update();
void net_udp_do_refuse_stuff (UDP_sequence_packet *their);
void net_udp_read_sync_packet( ubyte * data, int data_len, struct _sockaddr sender_addr );
void net_udp_read_object_packet( ubyte *data );
void net_udp_ping_frame(fix time);
void net_udp_process_ping(ubyte *data, int data_len, struct _sockaddr sender_addr);
void net_udp_process_pong(ubyte *data, int data_len, struct _sockaddr sender_addr);
void net_udp_read_endlevel_packet( ubyte *data, int data_len, struct _sockaddr sender_addr );
void net_udp_send_mdata(int priority, fix time);
void net_udp_process_mdata (ubyte *data, int data_len, struct _sockaddr sender_addr, int priority);
void net_udp_send_pdata();
void net_udp_process_pdata ( ubyte *data, int data_len, struct _sockaddr sender_addr );
void net_udp_read_pdata_short_packet(UDP_frame_info *pd);
void net_udp_timeout_check(fix time);
void net_udp_timeout_player(int playernum);
int net_udp_get_new_player_num (UDP_sequence_packet *their);
void net_udp_noloss_add_queue_pkt(uint32_t pkt_num, fix time, ubyte *data, ushort data_size, ubyte pnum, ubyte player_ack[MAX_PLAYERS]);
int net_udp_noloss_validate_mdata(uint32_t pkt_num, ubyte sender_pnum, struct _sockaddr sender_addr);
void net_udp_noloss_got_ack(ubyte *data, int data_len);
void net_udp_noloss_init_mdata_queue(void);
void net_udp_noloss_clear_mdata_got(ubyte player_num);
void net_udp_noloss_process_queue(fix time);
extern void nm_draw_background1(char * filename);
extern void game_disable_cheats();

// Variables
UDP_mdata_info		UDP_MData;
UDP_sequence_packet UDP_Seq;
UDP_mdata_store UDP_mdata_queue[UDP_MDATA_STOR_QUEUE_SIZE];
UDP_mdata_recv UDP_mdata_got[MAX_PLAYERS];
UDP_sequence_packet UDP_sync_player; // For rejoin object syncing
UDP_netgame_info_lite Active_udp_games[UDP_MAX_NETGAMES];
int num_active_udp_games = 0;
int num_active_udp_changed = 0;
static int UDP_Socket[2] = { -1, -1 };
static char UDP_MyPort[6] = "";
extern obj_position Player_init[MAX_PLAYERS];
extern ubyte SurfingNet;

/* General UDP functions - START */
// Resolve address
int udp_dns_filladdr(char *host, int port, struct _sockaddr *sAddr)
{
	struct hostent *he;

#ifdef IPv6
	int socktype=AF_INET6;

	he = gethostbyname2 (host,socktype);

	if (!he)
	{
		socktype=AF_INET;
		he = gethostbyname2 (host,socktype);
	}

	if (!he)
	{
		con_printf(CON_URGENT,"udp_dns_filladdr (gethostbyname) failed\n");
		nm_messagebox(TXT_ERROR,1,TXT_OK,"Could not resolve address");
		return -1;
	}

	((struct _sockaddr *) sAddr)->sin6_family = socktype; // host byte order
	((struct _sockaddr *) sAddr)->sin6_port = htons (port); // short, network byte order
	((struct _sockaddr *) sAddr)->sin6_flowinfo = 0;
	((struct _sockaddr *) sAddr)->sin6_addr = *((struct in6_addr *) he->h_addr);
	((struct _sockaddr *) sAddr)->sin6_scope_id = 0;
#else
	if ((he = gethostbyname (host)) == NULL) // get the host info
	{
		con_printf(CON_URGENT,"udp_dns_filladdr (gethostbyname) failed\n");
		nm_messagebox(TXT_ERROR,1,TXT_OK,"Could not resolve address");
		return -1;
	}

	((struct _sockaddr *) sAddr)->sin_family = _af; // host byte order
	((struct _sockaddr *) sAddr)->sin_port = htons (port); // short, network byte order
	((struct _sockaddr *) sAddr)->sin_addr = *((struct in_addr *) he->h_addr);
	memset (&(((struct _sockaddr *) sAddr)->sin_zero), '\0', 8); // zero the rest of the struct
#endif

	return 0;
}

// Closes an existing udp socket
void udp_close_socket(int socknum)
{
	if (UDP_Socket[socknum] != -1)
	{
#ifdef _WIN32
		closesocket(UDP_Socket[socknum]);
#else
		close (UDP_Socket[socknum]);
#endif
	}
	UDP_Socket[socknum] = -1;
}

// Open socket
int udp_open_socket(int socknum, int port)
{
	// close stale socket
	if( UDP_Socket[socknum] != -1 )
		udp_close_socket(socknum);

	{
#ifdef _WIN32
	struct _sockaddr sAddr;   // my address information
	int reuse_on = -1;

	memset( &sAddr, '\0', sizeof( sAddr ) );

	if ((UDP_Socket[socknum] = socket (_af, SOCK_DGRAM, 0)) == -1) {
		con_printf(CON_URGENT,"udp_open_socket: socket creation failed\n");
		nm_messagebox(TXT_ERROR,1,TXT_OK,"Could not create socket");
		return -1;
	}

	// this is pretty annoying in win32. Not doing that will lead to 
	// "Could not bind name to socket" errors. It may be suitable for other
	// socket implementations, too
	(void)setsockopt( UDP_Socket[socknum], SOL_SOCKET, SO_REUSEADDR, (const char *) &reuse_on, sizeof( reuse_on ));

#ifdef IPv6
	sAddr.sin6_family = _pf; // host byte order
	sAddr.sin6_port = htons (port); // short, network byte order
	sAddr.sin6_flowinfo = 0;
	sAddr.sin6_addr = in6addr_any; // automatically fill with my IP
	sAddr.sin6_scope_id = 0;
#else
	sAddr.sin_family = _pf; // host byte order
	sAddr.sin_port = htons (port); // short, network byte order
	sAddr.sin_addr.s_addr = INADDR_ANY; // automatically fill with my IP
#endif
	
	memset (&(sAddr.sin_zero), '\0', 8); // zero the rest of the struct
	
	if (bind (UDP_Socket[socknum], (struct sockaddr *) &sAddr, sizeof (struct sockaddr)) == -1) 
	{      
		con_printf(CON_URGENT,"udp_open_socket: bind name to socket failed\n");
		nm_messagebox(TXT_ERROR,1,TXT_OK,"Could not bind name to socket");
		udp_close_socket(socknum);
		return -1;
	}
#else
	struct addrinfo hints,*res,*sres;
	int err,ai_family_;
	char cport[6];
	
	memset (&hints, '\0', sizeof (struct addrinfo));
	memset(cport,'\0',sizeof(char)*6);
	
	hints.ai_flags = AI_PASSIVE;
	hints.ai_family = _pf;
	hints.ai_socktype = SOCK_DGRAM;
	
	ai_family_ = 0;

	sprintf(cport,"%i",port);

	if ((err = getaddrinfo (NULL, cport, &hints, &res)) == 0)
	{
		sres = res;
		while ((ai_family_ == 0) && (sres))
		{
			if (sres->ai_family == _pf || _pf == PF_UNSPEC)
				ai_family_ = sres->ai_family;
			else
				sres = sres->ai_next;
		}
	
		if (sres == NULL)
			sres = res;
	
		ai_family_ = sres->ai_family;
		if (ai_family_ != _pf && _pf != PF_UNSPEC)
		{
			// ai_family is not identic
			freeaddrinfo (res);
			return -1;
		}
	
		if ((UDP_Socket[socknum] = socket (sres->ai_family, SOCK_DGRAM, 0)) < 0)
		{
			con_printf(CON_URGENT,"udp_open_socket: socket creation failed\n");
			nm_messagebox(TXT_ERROR,1,TXT_OK,"Could not create socket");
			freeaddrinfo (res);
			return -1;
		}
	
		if ((err = bind (UDP_Socket[socknum], sres->ai_addr, sres->ai_addrlen)) < 0)
		{
			con_printf(CON_URGENT,"udp_open_socket: bind name to socket failed\n");
			nm_messagebox(TXT_ERROR,1,TXT_OK,"Could not bind name to socket");
			udp_close_socket(socknum);
			freeaddrinfo (res);
			return -1;
		}
	
		freeaddrinfo (res);
	}
	else {
		UDP_Socket[socknum] = -1;
		con_printf(CON_URGENT,"udp_open_socket (getaddrinfo):%s\n", gai_strerror (err));
		nm_messagebox(TXT_ERROR,1,TXT_OK,"Could not get address information:\n%s",gai_strerror (err));
	}
#endif

	return 0;
	}
}

int udp_general_packet_ready(int socknum)
{
	fd_set set;
	struct timeval tv;

	FD_ZERO(&set);
	FD_SET(UDP_Socket[socknum], &set);
	tv.tv_sec = tv.tv_usec = 0;
	if (select(UDP_Socket[socknum] + 1, &set, NULL, NULL, &tv) > 0)
		return 1;
	else
		return 0;
}

// Gets some text. Returns 0 if nothing on there.
int udp_receive_packet(int socknum, ubyte *text, int len, struct _sockaddr *sender_addr)
{
	unsigned int clen = sizeof (struct _sockaddr), msglen = 0;

	if (UDP_Socket[socknum] == -1)
		return -1;

	if (udp_general_packet_ready(socknum))
	{
		msglen = recvfrom (UDP_Socket[socknum], text, len, 0, (struct sockaddr *)sender_addr, &clen);

		if (msglen < 0)
			return 0;

		if ((msglen >= 0) && (msglen < len))
			text[msglen] = 0;
	}

	return msglen;
}
/* General UDP functions - END */


// Connect to a game host and get full info. Eventually we join!
void net_udp_game_connect(struct _sockaddr HostAddr)
{
	fix start_time = 0, time = 0, last_time = 0;

	N_players = 0;
	change_playernum_to(1);
	start_time = timer_get_fixed_seconds();

	memcpy((struct _sockaddr *)&Netgame.players[0].protocol.udp.addr, (struct _sockaddr *)&HostAddr, sizeof(struct _sockaddr));

	// Get full game info so we can show it.
	while (Netgame.protocol.udp.valid != 1)
	{
		time = timer_get_fixed_seconds();
		
		// Cancel this with ESC
		if (key_inkey()==KEY_ESC)
			return;

		// Timeout after 10 seconds
		if (timer_get_fixed_seconds() >= start_time + (F1_0*10) || timer_get_fixed_seconds() < start_time)
		{
			nm_messagebox(TXT_ERROR,1,TXT_OK,"No response by host.\n\nPossible reasons:\n* No game on this IP (anymore)\n* Port of Host not open\n  or different\n* Host uses a game version\n  I do not understand");
			return;
		}
		
		if (Netgame.protocol.udp.valid == -1)
		{
			int hmaj = 0, hmin = 0, cmaj = 0, cmin = 0;
			
			// This calculation bases on vers_id.h
			hmaj = Netgame.protocol.udp.program_iver/10000;
			hmin = (Netgame.protocol.udp.program_iver-(hmaj*10000))/100;
			cmaj = D1X_IVER/10000;
			cmin = (D1X_IVER-(cmaj*10000))/100;
			nm_messagebox(TXT_ERROR,1,TXT_OK,"Version mismatch! Cannot join Game.\nHost game version: %i.%i\nYour game version: %i.%i",hmaj,hmin,cmaj,cmin);
			return;
		}
		
		if (time >= last_time + F1_0)
		{
			net_udp_request_game_info(HostAddr, 0);
			last_time = time;
		}
		timer_delay2(5);
		net_udp_listen();
	}

	if (!net_udp_show_game_info()) // show info menu and check if we join
		return;
		
	Netgame.protocol.udp.valid = 0;
	start_time = timer_get_fixed_seconds();

	// Get full game info again as it could have changed since we entered the info menu.
	while (Netgame.protocol.udp.valid != 1)
	{
		time = timer_get_fixed_seconds();

		// Cancel this with ESC
		if (key_inkey()==KEY_ESC)
			return;

		// Timeout after 10 seconds
		if (timer_get_fixed_seconds() >= start_time + (F1_0*10) || timer_get_fixed_seconds() < start_time)
		{
			nm_messagebox(TXT_ERROR,1,TXT_OK,"No response by host.\n\nPossible reasons:\n* No game on this IP (anymore)\n* Port of Host not open\n  or different\n* Host uses a game version\n  I do not understand");
			return;
		}
		
		if (time >= last_time + F1_0)
		{
			net_udp_request_game_info(HostAddr, 0);
			last_time = time;
		}
		timer_delay2(5);
		net_udp_listen();
	}
		
	net_udp_do_join_game();
}

void net_udp_manual_join_game()
{
	struct _sockaddr HostAddr;
	newmenu_item m[6];
	int choice = 0, nitems = 0;
	int old_game_mode;
	char addrbuf[128]="";
	char portbuf[6]="";

	setjmp(LeaveGame);

	net_udp_init();

	memset(&addrbuf,'\0', sizeof(char)*128);
	snprintf(addrbuf, sizeof(char)*(strlen(GameArg.MplUdpHostAddr)+1), "%s", GameArg.MplUdpHostAddr);

	if (GameArg.MplUdpHostPort != 0)
		snprintf(portbuf, sizeof(portbuf), "%d", GameArg.MplUdpHostPort);
	else
		snprintf(portbuf, sizeof(portbuf), "%d", UDP_PORT_DEFAULT);

	if (GameArg.MplUdpMyPort != 0)
		snprintf (UDP_MyPort, sizeof(UDP_MyPort), "%d", GameArg.MplUdpMyPort);
	else
		snprintf (UDP_MyPort, sizeof(UDP_MyPort), "%d", UDP_PORT_DEFAULT);

	do {
		old_game_mode = Game_mode;
		nitems = 0;
		
		m[nitems].type = NM_TYPE_TEXT;  m[nitems].text="GAME ADDRESS OR HOSTNAME:";     	nitems++;
		m[nitems].type = NM_TYPE_INPUT; m[nitems].text=addrbuf; m[nitems].text_len=128; 	nitems++;
		m[nitems].type = NM_TYPE_TEXT;  m[nitems].text="GAME PORT:";                    	nitems++;
		m[nitems].type = NM_TYPE_INPUT; m[nitems].text=portbuf; m[nitems].text_len=5;   	nitems++;
		m[nitems].type = NM_TYPE_TEXT;  m[nitems].text="MY PORT:";	                    	nitems++;
		m[nitems].type = NM_TYPE_INPUT; m[nitems].text=UDP_MyPort; m[nitems].text_len=5;	nitems++;

		choice = newmenu_do1( NULL, "ENTER GAME ADDRESS", nitems, m, NULL, NULL, choice );

		if ( choice > -1 )
		{
			int sockres = -1;

			if ((atoi(UDP_MyPort)) < 0 ||(atoi(UDP_MyPort)) > 65535)
			{
				snprintf (UDP_MyPort, sizeof(UDP_MyPort), "%d", UDP_PORT_DEFAULT);
				nm_messagebox(TXT_ERROR, 1, TXT_OK, "Illegal port");
				choice = 0;
				continue;
			}
			
			sockres = udp_open_socket(0, atoi(UDP_MyPort));

			if (sockres != 0)
			{
				choice = 0;
				continue;
			}
			
			// Resolve address
			if (udp_dns_filladdr(addrbuf, atoi(portbuf), &HostAddr) < 0)
			{
				nm_messagebox(TXT_ERROR, 1, TXT_OK, "Could not resolve Address!");
				choice = 0;
				continue;
			}
			else
			{
				net_udp_game_connect(HostAddr);
			}
		}

		if (old_game_mode != Game_mode)
		{
			break;		// leave menu
		}
	} while( choice > -1 );
}

void net_udp_send_sequence_packet(UDP_sequence_packet seq, struct _sockaddr recv_addr)
{
	int len = 0;
	ubyte buf[UPKT_SEQUENCE_SIZE];

	len = 0;
	memset(buf, 0, sizeof(buf));
	buf[0] = seq.type;											len++;
	memcpy(&buf[len], seq.player.callsign, CALLSIGN_LEN+1);		len += CALLSIGN_LEN+1;
	buf[len] = seq.player.version_major;						len++;
	buf[len] = seq.player.version_minor;						len++;
	buf[len] = seq.player.connected;							len++;
	buf[len] = seq.player.rank;									len++;
	
	sendto (UDP_Socket[0], buf, len, 0, (struct sockaddr *)&recv_addr, sizeof(struct _sockaddr));
}

void net_udp_receive_sequence_packet(ubyte *data, UDP_sequence_packet *seq, struct _sockaddr sender_addr)
{
	int len = 0;
	
	seq->type = data[0];										len++;
	memcpy(seq->player.callsign, &(data[len]), CALLSIGN_LEN+1);	len += CALLSIGN_LEN+1;
	seq->player.version_major = data[len];						len++;
	seq->player.version_minor = data[len];						len++;
	seq->player.connected = data[len];							len++;
	memcpy (&(seq->player.rank),&(data[len]),1);				len++;
	
	if (multi_i_am_master())
		memcpy(&seq->player.protocol.udp.addr, (struct _sockaddr *)&sender_addr, sizeof(struct _sockaddr));
}

void net_udp_init()
{
	// So you want to play a netgame, eh?  Let's a get a few things
	// straight

	int save_pnum = Player_num;

	if( UDP_Socket[0] != -1 )
		udp_close_socket(0);
	if( UDP_Socket[1] != -1 )
		udp_close_socket(1);

	game_disable_cheats();

	memset(&Netgame, 0, sizeof(netgame_info));
	memset(&UDP_Seq, 0, sizeof(UDP_sequence_packet));
	memset(&UDP_MData, 0, sizeof(UDP_mdata_info));
	net_udp_noloss_init_mdata_queue();
	UDP_Seq.type = UPID_REQUEST;
	memcpy(UDP_Seq.player.callsign, Players[Player_num].callsign, CALLSIGN_LEN+1);

	for (Player_num = 0; Player_num < MAX_NUM_NET_PLAYERS; Player_num++)
		init_player_stats_game();

	Player_num = save_pnum;		
	multi_new_game();
	Network_new_game = 1;
	Control_center_destroyed = 0;
	net_udp_flush();

	Netgame.PacketsPerSec = 10;
}

// Send PID_ENDLEVEL in regular intervals and listen for them (host also does the packets for playing clients)
int net_udp_kmatrix_poll1( newmenu *menu, d_event *event, void *userdata )
{
	// Polling loop for End-of-level menu
	if (event->type != EVENT_IDLE)
		return 0;
	
	menu = menu;
	userdata = userdata;

	net_udp_do_frame(0, 1);
	
	return 0;
}

// Same as above but used when player pressed ESC during kmatrix (host also does the packets for playing clients)
extern fix StartAbortMenuTime;
int net_udp_kmatrix_poll2( newmenu *menu, d_event *event, void *userdata )
{
	int rval = 0;

	// Polling loop for End-of-level menu
	if (event->type != EVENT_IDLE)
		return 0;
	
	menu = menu;
	userdata = userdata;
	
	if (timer_get_fixed_seconds() > (StartAbortMenuTime+(F1_0*3)))
		rval = -2;

	net_udp_do_frame(0, 1);
	
	return rval;
}

int net_udp_endlevel(int *secret)
{
	// Do whatever needs to be done between levels

	int i;

	// We do not really check if a player has actually found a secret level... yeah, I am too lazy! So just go there and pretend we did!
	for (i = 0; i < N_secret_levels; i++)
	{
		if (Current_level_num == Secret_level_table[i])
		{
			*secret = 1;
			break;
		}
	}

	Network_status = NETSTAT_ENDLEVEL; // We are between levels
	net_udp_listen();
	net_udp_send_endlevel_packet();

	for (i=0; i<N_players; i++) 
	{
		Netgame.players[i].LastPacketTime = timer_get_fixed_seconds();
	}
   
	net_udp_send_endlevel_packet();
	net_udp_send_endlevel_packet();

	net_udp_update_netgame();

	return(0);
}

int 
net_udp_can_join_netgame(netgame_info *game)
{
	// Can this player rejoin a netgame in progress?

	int i, num_players;

	if (game->game_status == NETSTAT_STARTING)
		return 1;

	if (game->game_status != NETSTAT_PLAYING)
		return 0;

	// Game is in progress, figure out if this guy can re-join it

	num_players = game->numplayers;

	if (!(game->game_flags & NETGAME_FLAG_CLOSED)) {
		// Look for player that is not connected
		
		if (game->numconnected==game->max_numplayers)
			return (2);
		
		if (game->RefusePlayers)
			return (3);
		
		if (game->numplayers < game->max_numplayers)
			return 1;

		if (game->numconnected<num_players)
			return 1;
	}

	// Search to see if we were already in this closed netgame in progress

	for (i = 0; i < num_players; i++)
		if ( (!strcasecmp(Players[Player_num].callsign, game->players[i].callsign)) && game->players[i].protocol.udp.isyou )
			break;

	if (i != num_players)
		return 1;

	return 0;
}

void
net_udp_disconnect_player(int playernum)
{
	// A player has disconnected from the net game, take whatever steps are
	// necessary 

	if (playernum == Player_num) 
	{
		Int3(); // Weird, see Rob
		return;
	}

	Players[playernum].connected = CONNECT_DISCONNECTED;
	Netgame.players[playernum].connected = CONNECT_DISCONNECTED;

	if (Network_status == NETSTAT_PLAYING)
	{
		multi_make_player_ghost(playernum);
		multi_strip_robots(playernum);
	}

	if (Newdemo_state == ND_STATE_RECORDING)
		newdemo_record_multi_disconnect(playernum);

	net_udp_noloss_clear_mdata_got(playernum);

	if (playernum == 0) // Host has left - Quit game!
	{
		Function_mode = FMODE_MENU;
		nm_messagebox(NULL, 1, TXT_OK, "Game was closed by host!");
		Function_mode = FMODE_GAME;
		multi_quit_game = 1;
		multi_leave_menu = 1;
		multi_reset_stuff();
		Function_mode = FMODE_MENU;
	}
}

void
net_udp_new_player(UDP_sequence_packet *their)
{
	int objnum;
	int pnum;

	pnum = their->player.connected;

	Assert(pnum >= 0);
	Assert(pnum < MaxNumNetPlayers);	
	
	objnum = Players[pnum].objnum;

	if (Newdemo_state == ND_STATE_RECORDING) {
		int new_player;

		if (pnum == N_players)
			new_player = 1;
		else
			new_player = 0;
		newdemo_record_multi_connect(pnum, new_player, their->player.callsign);
	}

	memcpy(Players[pnum].callsign, their->player.callsign, CALLSIGN_LEN+1);
	memcpy(Netgame.players[pnum].callsign, their->player.callsign, CALLSIGN_LEN+1);
	memcpy(&Netgame.players[pnum].protocol.udp.addr, &their->player.protocol.udp.addr, sizeof(struct _sockaddr));

	Players[pnum].connected = CONNECT_PLAYING;
	Players[pnum].net_kills_total = 0;
	Players[pnum].net_killed_total = 0;
	memset(kill_matrix[pnum], 0, MAX_PLAYERS*sizeof(short)); 
	Players[pnum].score = 0;
	Players[pnum].flags = 0;

	if (pnum == N_players)
	{
		N_players++;
		Netgame.numplayers = N_players;
	}

	digi_play_sample(SOUND_HUD_MESSAGE, F1_0);

	hud_message(MSGC_MULTI_INFO, "'%s' %s",their->player.callsign, TXT_JOINING);
	
	multi_make_ghost_player(pnum);

	multi_send_score();

	net_udp_noloss_clear_mdata_got(pnum);
}

void net_udp_welcome_player(UDP_sequence_packet *their)
{
 	// Add a player to a game already in progress
	int player_num;
	int i;

	// Don't accept new players if we're ending this level.  Its safe to
	// ignore since they'll request again later

	if ((Endlevel_sequence) || (Control_center_destroyed))
	{
		net_udp_dump_player(their->player.protocol.udp.addr, DUMP_ENDLEVEL);
		return; 
	}

	if (Network_send_objects)
	{
		// Ignore silently, we're already responding to someone and we can't
		// do more than one person at a time.  If we don't dump them they will
		// re-request in a few seconds.
		return;
	}

	if (their->player.connected != Current_level_num)
	{
		net_udp_dump_player(their->player.protocol.udp.addr, DUMP_LEVEL);
		return;
	}

	player_num = -1;
	memset(&UDP_sync_player, 0, sizeof(UDP_sequence_packet));
	Network_player_added = 0;

	for (i = 0; i < N_players; i++)
	{
		if ((!strcasecmp(Players[i].callsign, their->player.callsign )) && !memcmp((struct _sockaddr *)&their->player.protocol.udp.addr, (struct _sockaddr *)&Netgame.players[i].protocol.udp.addr, sizeof(struct _sockaddr)))
		{
			player_num = i;
			break;
		}
	}

	if (player_num == -1)
	{
		// Player is new to this game

		if ( !(Netgame.game_flags & NETGAME_FLAG_CLOSED) && (N_players < MaxNumNetPlayers))
		{
			// Add player in an open slot, game not full yet

			player_num = N_players;
			Network_player_added = 1;
		}
		else if (Netgame.game_flags & NETGAME_FLAG_CLOSED)
		{
			// Slots are open but game is closed

			net_udp_dump_player(their->player.protocol.udp.addr, DUMP_CLOSED);
			return;
		}
		else
		{
			// Slots are full but game is open, see if anyone is
			// disconnected and replace the oldest player with this new one
		
			int oldest_player = -1;
			fix oldest_time = timer_get_fixed_seconds();
			int activeplayers = 0;

			Assert(N_players == MaxNumNetPlayers);

			for (i = 0; i < Netgame.numplayers; i++)
				if (Netgame.players[i].connected)
					activeplayers++;

			if (activeplayers == Netgame.max_numplayers)
			{
				// Game is full.
				net_udp_dump_player(their->player.protocol.udp.addr, DUMP_FULL);
				return;
			}

			for (i = 0; i < N_players; i++)
			{
				if ( (!Players[i].connected) && (Netgame.players[i].LastPacketTime < oldest_time))
				{
					oldest_time = Netgame.players[i].LastPacketTime;
					oldest_player = i;
				}
			}

			if (oldest_player == -1)
			{
				// Everyone is still connected 

				net_udp_dump_player(their->player.protocol.udp.addr, DUMP_FULL);
				return;
			}
			else
			{	
				// Found a slot!

				player_num = oldest_player;
				Network_player_added = 1;
			}
		}
	}
	else 
	{
		// Player is reconnecting
		
		if (Players[player_num].connected)
		{
			return;
		}

		if (Newdemo_state == ND_STATE_RECORDING)
			newdemo_record_multi_reconnect(player_num);

		Network_player_added = 0;

		digi_play_sample(SOUND_HUD_MESSAGE, F1_0);

		hud_message(MSGC_MULTI_INFO, "'%s' %s", Players[player_num].callsign, TXT_REJOIN);
	}

	// Send updated Objects data to the new/returning player

	UDP_sync_player = *their;
	UDP_sync_player.player.connected = player_num;
	Network_send_objects = 1;
	Network_send_objnum = -1;
	Netgame.players[player_num].LastPacketTime = timer_get_fixed_seconds();

	net_udp_send_objects();
}

int net_udp_objnum_is_past(int objnum)
{
	// determine whether or not a given object number has already been sent
	// to a re-joining player.
	
	int player_num = UDP_sync_player.player.connected;
	int obj_mode = !((object_owner[objnum] == -1) || (object_owner[objnum] == player_num));

	if (!Network_send_objects)
		return 0; // We're not sending objects to a new player

	if (obj_mode > Network_send_object_mode)
		return 0;
	else if (obj_mode < Network_send_object_mode)
	 	return 1;
	else if (objnum < Network_send_objnum)
		return 1;
	else
		return 0;
}

void net_udp_send_door_updates(void)
{
	// Send door status when new player joins
	
	int i;

	for (i = 0; i < Num_walls; i++)
	{
		if ((Walls[i].type == WALL_DOOR) && ((Walls[i].state == WALL_DOOR_OPENING) || (Walls[i].state == WALL_DOOR_WAITING)))
			multi_send_door_open(Walls[i].segnum, Walls[i].sidenum);
		else if ((Walls[i].type == WALL_BLASTABLE) && (Walls[i].flags & WALL_BLASTED))
			multi_send_door_open(Walls[i].segnum, Walls[i].sidenum);
		else if ((Walls[i].type == WALL_BLASTABLE) && (Walls[i].hps != WALL_HPS))
			multi_send_hostage_door_status(i);
	}

}	

void net_udp_process_monitor_vector(int vector)
{
	int i, j;
	int count = 0;
	segment *seg;
	
	for (i=0; i <= Highest_segment_index; i++)
	{
		int tm, ec, bm;
		seg = &Segments[i];
		for (j = 0; j < 6; j++)
		{
			if ( ((tm = seg->sides[j].tmap_num2) != 0) &&
				  ((ec = TmapInfo[tm&0x3fff].eclip_num) != -1) &&
 				  ((bm = Effects[ec].dest_bm_num) != -1) )
			{
				if (vector & (1 << count))
				{
					seg->sides[j].tmap_num2 = bm | (tm&0xc000);
				}
				count++;
				Assert(count < 32);
			}
		}
	}
}

int net_udp_create_monitor_vector(void)
{
	int i, j, k;
	int num_blown_bitmaps = 0;
	int monitor_num = 0;
	int blown_bitmaps[7];
	int vector = 0;
	segment *seg;

	for (i=0; i < Num_effects; i++)
	{
		if (Effects[i].dest_bm_num > 0) {
			for (j = 0; j < num_blown_bitmaps; j++)
				if (blown_bitmaps[j] == Effects[i].dest_bm_num)
					break;
			if (j == num_blown_bitmaps)
				blown_bitmaps[num_blown_bitmaps++] = Effects[i].dest_bm_num;
		}
	}		
		

	Assert(num_blown_bitmaps <= 7);

	for (i=0; i <= Highest_segment_index; i++)
	{
		int tm, ec;
		seg = &Segments[i];
		for (j = 0; j < 6; j++)
		{
			if ((tm = seg->sides[j].tmap_num2) != 0) 
			{
				if ( ((ec = TmapInfo[tm&0x3fff].eclip_num) != -1) &&
 					  (Effects[ec].dest_bm_num != -1) )
				{
					monitor_num++;
					Assert(monitor_num < 32);
				}
				else
				{
					for (k = 0; k < num_blown_bitmaps; k++)
					{
						if ((tm&0x3fff) == blown_bitmaps[k])
						{
							vector |= (1 << monitor_num);
							monitor_num++;
							Assert(monitor_num < 32);
							break;
						}
					}
				}
			}
		}
	}
	return(vector);
}

void net_udp_stop_resync(UDP_sequence_packet *their)
{
	if ( (!memcmp((struct _sockaddr *)&UDP_sync_player.player.protocol.udp.addr, (struct _sockaddr *)&their->player.protocol.udp.addr, sizeof(struct _sockaddr))) &&
		(!stricmp(UDP_sync_player.player.callsign, their->player.callsign)) )
	{
		Network_send_objects = 0;
		Network_rejoined=0;
		Network_send_objnum = -1;
	}
}

void net_udp_swap_object(object *obj)
{
// swap the short and int entries for this object
	obj->signature 			= INTEL_INT(obj->signature);
	obj->next				= INTEL_SHORT(obj->next);
	obj->prev				= INTEL_SHORT(obj->prev);
	obj->segnum				= INTEL_SHORT(obj->segnum);
	obj->attached_obj		= INTEL_SHORT(obj->attached_obj);
	obj->pos.x 				= INTEL_INT(obj->pos.x);
	obj->pos.y 				= INTEL_INT(obj->pos.y);
	obj->pos.z 				= INTEL_INT(obj->pos.z);

	obj->orient.rvec.x 		= INTEL_INT(obj->orient.rvec.x);
	obj->orient.rvec.y 		= INTEL_INT(obj->orient.rvec.y);
	obj->orient.rvec.z 		= INTEL_INT(obj->orient.rvec.z);
	obj->orient.fvec.x 		= INTEL_INT(obj->orient.fvec.x);
	obj->orient.fvec.y 		= INTEL_INT(obj->orient.fvec.y);
	obj->orient.fvec.z 		= INTEL_INT(obj->orient.fvec.z);
	obj->orient.uvec.x 		= INTEL_INT(obj->orient.uvec.x);
	obj->orient.uvec.y 		= INTEL_INT(obj->orient.uvec.y);
	obj->orient.uvec.z 		= INTEL_INT(obj->orient.uvec.z);
	
	obj->size				= INTEL_INT(obj->size);
	obj->shields			= INTEL_INT(obj->shields);
	
	obj->last_pos.x 		= INTEL_INT(obj->last_pos.x);
	obj->last_pos.y 		= INTEL_INT(obj->last_pos.y);
	obj->last_pos.z 		= INTEL_INT(obj->last_pos.z);
	
	obj->lifeleft			= INTEL_INT(obj->lifeleft);
	
	switch (obj->movement_type) {
	
	case MT_PHYSICS:
	
		obj->mtype.phys_info.velocity.x = INTEL_INT(obj->mtype.phys_info.velocity.x);
		obj->mtype.phys_info.velocity.y = INTEL_INT(obj->mtype.phys_info.velocity.y);
		obj->mtype.phys_info.velocity.z = INTEL_INT(obj->mtype.phys_info.velocity.z);
	
		obj->mtype.phys_info.thrust.x 	= INTEL_INT(obj->mtype.phys_info.thrust.x);
		obj->mtype.phys_info.thrust.y 	= INTEL_INT(obj->mtype.phys_info.thrust.y);
		obj->mtype.phys_info.thrust.z 	= INTEL_INT(obj->mtype.phys_info.thrust.z);
	
		obj->mtype.phys_info.mass		= INTEL_INT(obj->mtype.phys_info.mass);
		obj->mtype.phys_info.drag		= INTEL_INT(obj->mtype.phys_info.drag);
		obj->mtype.phys_info.brakes		= INTEL_INT(obj->mtype.phys_info.brakes);
	
		obj->mtype.phys_info.rotvel.x	= INTEL_INT(obj->mtype.phys_info.rotvel.x);
		obj->mtype.phys_info.rotvel.y	= INTEL_INT(obj->mtype.phys_info.rotvel.y);
		obj->mtype.phys_info.rotvel.z 	= INTEL_INT(obj->mtype.phys_info.rotvel.z);
	
		obj->mtype.phys_info.rotthrust.x = INTEL_INT(obj->mtype.phys_info.rotthrust.x);
		obj->mtype.phys_info.rotthrust.y = INTEL_INT(obj->mtype.phys_info.rotthrust.y);
		obj->mtype.phys_info.rotthrust.z = INTEL_INT(obj->mtype.phys_info.rotthrust.z);
	
		obj->mtype.phys_info.turnroll	= INTEL_INT(obj->mtype.phys_info.turnroll);
		obj->mtype.phys_info.flags		= INTEL_SHORT(obj->mtype.phys_info.flags);
	
		break;
	
	case MT_SPINNING:
		
		obj->mtype.spin_rate.x = INTEL_INT(obj->mtype.spin_rate.x);
		obj->mtype.spin_rate.y = INTEL_INT(obj->mtype.spin_rate.y);
		obj->mtype.spin_rate.z = INTEL_INT(obj->mtype.spin_rate.z);
		break;
	}
	
	switch (obj->control_type) {
	
	case CT_WEAPON:
		obj->ctype.laser_info.parent_type		= INTEL_SHORT(obj->ctype.laser_info.parent_type);
		obj->ctype.laser_info.parent_num		= INTEL_SHORT(obj->ctype.laser_info.parent_num);
		obj->ctype.laser_info.parent_signature	= INTEL_SHORT(obj->ctype.laser_info.parent_signature);
		obj->ctype.laser_info.creation_time		= INTEL_INT(obj->ctype.laser_info.creation_time);
		obj->ctype.laser_info.last_hitobj		= INTEL_INT(obj->ctype.laser_info.last_hitobj);
		obj->ctype.laser_info.multiplier		= INTEL_INT(obj->ctype.laser_info.multiplier);
		break;
	
	case CT_EXPLOSION:
		obj->ctype.expl_info.spawn_time		= INTEL_INT(obj->ctype.expl_info.spawn_time);
		obj->ctype.expl_info.delete_time	= INTEL_INT(obj->ctype.expl_info.delete_time);
		obj->ctype.expl_info.delete_objnum	= INTEL_SHORT(obj->ctype.expl_info.delete_objnum);
		obj->ctype.expl_info.attach_parent	= INTEL_SHORT(obj->ctype.expl_info.attach_parent);
		obj->ctype.expl_info.prev_attach	= INTEL_SHORT(obj->ctype.expl_info.prev_attach);
		obj->ctype.expl_info.next_attach	= INTEL_SHORT(obj->ctype.expl_info.next_attach);
		break;
	
	case CT_AI:
		obj->ctype.ai_info.hide_segment			= INTEL_SHORT(obj->ctype.ai_info.hide_segment);
		obj->ctype.ai_info.hide_index			= INTEL_SHORT(obj->ctype.ai_info.hide_index);
		obj->ctype.ai_info.path_length			= INTEL_SHORT(obj->ctype.ai_info.path_length);
		obj->ctype.ai_info.cur_path_index		= INTEL_SHORT(obj->ctype.ai_info.cur_path_index);
		obj->ctype.ai_info.follow_path_start_seg	= INTEL_SHORT(obj->ctype.ai_info.follow_path_start_seg);
		obj->ctype.ai_info.follow_path_end_seg		= INTEL_SHORT(obj->ctype.ai_info.follow_path_end_seg);
		obj->ctype.ai_info.danger_laser_signature = INTEL_INT(obj->ctype.ai_info.danger_laser_signature);
		obj->ctype.ai_info.danger_laser_num		= INTEL_SHORT(obj->ctype.ai_info.danger_laser_num);
		break;
	
	case CT_LIGHT:
		obj->ctype.light_info.intensity = INTEL_INT(obj->ctype.light_info.intensity);
		break;
	
	case CT_POWERUP:
		obj->ctype.powerup_info.count = INTEL_INT(obj->ctype.powerup_info.count);
		// Below commented out 5/2/96 by Matt.  I asked Allender why it was
		// here, and he didn't know, and it looks like it doesn't belong.
		// if (obj->id == POW_VULCAN_WEAPON)
		// obj->ctype.powerup_info.count = VULCAN_WEAPON_AMMO_AMOUNT;
		break;
	
	}
	
	switch (obj->render_type) {
	
	case RT_MORPH:
	case RT_POLYOBJ: {
		int i;
	
		obj->rtype.pobj_info.model_num		= INTEL_INT(obj->rtype.pobj_info.model_num);
	
		for (i=0;i<MAX_SUBMODELS;i++) {
			obj->rtype.pobj_info.anim_angles[i].p = INTEL_INT(obj->rtype.pobj_info.anim_angles[i].p);
			obj->rtype.pobj_info.anim_angles[i].b = INTEL_INT(obj->rtype.pobj_info.anim_angles[i].b);
			obj->rtype.pobj_info.anim_angles[i].h = INTEL_INT(obj->rtype.pobj_info.anim_angles[i].h);
		}
	
		obj->rtype.pobj_info.subobj_flags	= INTEL_INT(obj->rtype.pobj_info.subobj_flags);
		obj->rtype.pobj_info.tmap_override	= INTEL_INT(obj->rtype.pobj_info.tmap_override);
		obj->rtype.pobj_info.alt_textures	= INTEL_INT(obj->rtype.pobj_info.alt_textures);
		break;
	}
	
	case RT_WEAPON_VCLIP:
	case RT_HOSTAGE:
	case RT_POWERUP:
	case RT_FIREBALL:
		obj->rtype.vclip_info.vclip_num	= INTEL_INT(obj->rtype.vclip_info.vclip_num);
		obj->rtype.vclip_info.frametime	= INTEL_INT(obj->rtype.vclip_info.frametime);
		break;
	
	case RT_LASER:
		break;
	
	}
//  END OF SWAPPING OBJECT STRUCTURE

}

ubyte object_buffer[UPKT_MAX_SIZE];

void net_udp_send_objects(void)
{
	short remote_objnum;
	sbyte owner;
	int loc, i, h;

	static int obj_count = 0;
	static int frame_num = 0;

	int obj_count_frame = 0;
	int player_num = UDP_sync_player.player.connected;

	// Send clear objects array trigger and send player num

	Assert(Network_send_objects != 0);
	Assert(player_num >= 0);
	Assert(player_num < MaxNumNetPlayers);

	if (Endlevel_sequence || Control_center_destroyed)
	{
		// Endlevel started before we finished sending the goods, we'll
		// have to stop and try again after the level.

		net_udp_dump_player(UDP_sync_player.player.protocol.udp.addr, DUMP_ENDLEVEL);
		Network_send_objects = 0; 
		return;
	}

	for (h = 0; h < UDP_OBJ_PACKETS_PER_FRAME; h++) // Do more than 1 per frame, try to speed it up without
															  // over-stressing the receiver.
	{
		obj_count_frame = 0;
		memset(object_buffer, 0, UPKT_MAX_SIZE);
		object_buffer[0] = UPID_OBJECT_DATA;
		loc = 3;
	
		if (Network_send_objnum == -1)
		{
			obj_count = 0;
			Network_send_object_mode = 0;
			*(short *)(object_buffer+loc) = INTEL_SHORT(-1);	loc += 2;
			object_buffer[loc] = player_num; 		loc += 1;
													loc += 2; // Placeholder for remote_objnum, not used here
			Network_send_objnum = 0;
			obj_count_frame = 1;
			frame_num = 0;
		}
		
		for (i = Network_send_objnum; i <= Highest_object_index; i++)
		{
			if ((Objects[i].type != OBJ_POWERUP) && (Objects[i].type != OBJ_PLAYER) &&
				 (Objects[i].type != OBJ_CNTRLCEN) && (Objects[i].type != OBJ_GHOST) &&
				 (Objects[i].type != OBJ_ROBOT) && (Objects[i].type != OBJ_HOSTAGE))
				continue;
			if ((Network_send_object_mode == 0) && ((object_owner[i] != -1) && (object_owner[i] != player_num)))
				continue;
			if ((Network_send_object_mode == 1) && ((object_owner[i] == -1) || (object_owner[i] == player_num)))
				continue;
	
			if ( ((UPKT_MAX_SIZE-1) - loc) < (sizeof(object)+5) )
				break; // Not enough room for another object

			obj_count_frame++;
			obj_count++;
	
			remote_objnum = objnum_local_to_remote((short)i, &owner);
			Assert(owner == object_owner[i]);

			*(short *)(object_buffer+loc) = INTEL_SHORT(i);								loc += 2;
			object_buffer[loc] = owner;											loc += 1;
			*(short *)(object_buffer+loc) = INTEL_SHORT(remote_objnum); 				loc += 2;
			memcpy(object_buffer+loc, &Objects[i], sizeof(object));	loc += sizeof(object);
			net_udp_swap_object((object *)&object_buffer[loc]);
		}

		if (obj_count_frame) // Send any objects we've buffered
		{
			frame_num++;
	
			Network_send_objnum = i;
			object_buffer[1] = obj_count_frame;
			object_buffer[2] = frame_num;

			Assert(loc <= UPKT_MAX_SIZE);

			sendto (UDP_Socket[0], object_buffer, loc, 0, (struct sockaddr *)&UDP_sync_player.player.protocol.udp.addr, sizeof(struct _sockaddr));
		}

		if (i > Highest_object_index)
		{
			if (Network_send_object_mode == 0)
			{
				Network_send_objnum = 0;
				Network_send_object_mode = 1; // go to next mode
			}
			else 
			{
				Assert(Network_send_object_mode == 1);

				frame_num++;
				// Send count so other side can make sure he got them all
				object_buffer[0] = UPID_OBJECT_DATA;
				object_buffer[1] = 1;
				object_buffer[2] = frame_num;
				*(short *)(object_buffer+3) = INTEL_SHORT(-2);	
				*(short *)(object_buffer+6) = INTEL_SHORT(obj_count);
				sendto (UDP_Socket[0], object_buffer, 8, 0, (struct sockaddr *)&UDP_sync_player.player.protocol.udp.addr, sizeof(struct _sockaddr));

				// Send sync packet which tells the player who he is and to start!
				net_udp_send_rejoin_sync(player_num);

				// Turn off send object mode
				Network_send_objnum = -1;
				Network_send_objects = 0;
				obj_count = 0;
				return;
			} // mode == 1;
		} // i > Highest_object_index
	} // For PACKETS_PER_FRAME
}

void net_udp_send_rejoin_sync(int player_num)
{
	int i, j;

	Players[player_num].connected = CONNECT_PLAYING; // connect the new guy
	Netgame.players[player_num].LastPacketTime = timer_get_fixed_seconds();

	if (Endlevel_sequence || Control_center_destroyed)
	{
		// Endlevel started before we finished sending the goods, we'll
		// have to stop and try again after the level.

		net_udp_dump_player(UDP_sync_player.player.protocol.udp.addr, DUMP_ENDLEVEL);
		Network_send_objects = 0; 
		return;
	}

	if (Network_player_added)
	{
		UDP_sync_player.type = UPID_ADDPLAYER;
		UDP_sync_player.player.connected = player_num;
		net_udp_new_player(&UDP_sync_player);

		for (i = 0; i < N_players; i++)
		{
			if ((i != player_num) && (i != Player_num) && (Players[i].connected))
				net_udp_send_sequence_packet( UDP_sync_player, Netgame.players[i].protocol.udp.addr);
		}
	}

	// Send sync packet to the new guy

	net_udp_update_netgame();

	// Fill in the kill list
	for (j=0; j<MAX_PLAYERS; j++)
	{
		for (i=0; i<MAX_PLAYERS;i++)
			Netgame.kills[j][i] = kill_matrix[j][i];
		Netgame.killed[j] = Players[j].net_killed_total;
		Netgame.player_kills[j] = Players[j].net_kills_total;
		Netgame.player_score[j] = Players[j].score;
	}	

	Netgame.level_time = Players[Player_num].time_level;
	Netgame.monitor_vector = net_udp_create_monitor_vector();

	net_udp_send_game_info(UDP_sync_player.player.protocol.udp.addr, UPID_SYNC);
	net_udp_send_door_updates();

	return;
}

char * net_udp_get_player_name( int objnum )
{
	if ( objnum < 0 ) return NULL; 
	if ( Objects[objnum].type != OBJ_PLAYER ) return NULL;
	if ( Objects[objnum].id >= MAX_PLAYERS ) return NULL;
	if ( Objects[objnum].id >= N_players ) return NULL;
	
	return Players[Objects[objnum].id].callsign;
}


void net_udp_add_player(UDP_sequence_packet *p)
{
	int i;
	fix time = timer_get_fixed_seconds();

	for (i=0; i<N_players; i++ )
	{
		if ( !memcmp( (struct _sockaddr *)&Netgame.players[i].protocol.udp.addr, (struct _sockaddr *)&p->player.protocol.udp.addr, sizeof(struct _sockaddr)))
		{
			Netgame.players[i].LastPacketTime = time;
			return;		// already got them
		}
	}

	if ( N_players >= MAX_PLAYERS )
	{
		return;		// too many of em
	}

	memcpy( Netgame.players[N_players].callsign, p->player.callsign, CALLSIGN_LEN+1 );
	memcpy( (struct _sockaddr *)&Netgame.players[N_players].protocol.udp.addr, (struct _sockaddr *)&p->player.protocol.udp.addr, sizeof(struct _sockaddr) );
	Netgame.players[N_players].version_major=p->player.version_major;
	Netgame.players[N_players].version_minor=p->player.version_minor;
	Netgame.players[N_players].rank=p->player.rank;
	Netgame.players[N_players].connected = CONNECT_PLAYING;
	Players[N_players].connected = CONNECT_PLAYING;
	Netgame.players[N_players].LastPacketTime = time;
	N_players++;
	Netgame.numplayers = N_players;

	net_udp_send_netgame_update();
}

// One of the players decided not to join the game

void net_udp_remove_player(UDP_sequence_packet *p)
{
	int i,pn;
	
	pn = -1;
	for (i=0; i<N_players; i++ )
	{
		if (!memcmp((struct _sockaddr *)&Netgame.players[i].protocol.udp.addr, (struct _sockaddr *)&p->player.protocol.udp.addr, sizeof(struct _sockaddr)))
		{
			pn = i;
			break;
		}
	}
	
	if (pn < 0 )
		return;

	for (i=pn; i<N_players-1; i++ )
	{
		memcpy( Netgame.players[i].callsign, Netgame.players[i+1].callsign, CALLSIGN_LEN+1 );
		memcpy( (struct _sockaddr *)&Netgame.players[i].protocol.udp.addr, (struct _sockaddr *)&Netgame.players[i+1].protocol.udp.addr, sizeof(struct _sockaddr) );
		Netgame.players[i].version_major=Netgame.players[i+1].version_major;
		Netgame.players[i].version_minor=Netgame.players[i+1].version_minor;
		Netgame.players[i].rank=Netgame.players[i+1].rank;
	}
		
	N_players--;
	Netgame.numplayers = N_players;

	net_udp_send_netgame_update();
}

void net_udp_dump_player(struct _sockaddr dump_addr, int why)
{
	// Inform player that he was not chosen for the netgame

	ubyte buf[2];
	
	buf[0] = UPID_DUMP;
	buf[1] = why;
	
	sendto (UDP_Socket[0], buf, sizeof(buf), 0, (struct sockaddr *)&dump_addr, sizeof(struct _sockaddr));
}

void net_udp_update_netgame(void)
{
	// Update the netgame struct with current game variables

	int i, j;
	
	Netgame.numconnected=0;
	for (i=0;i<N_players;i++)
		if (Players[i].connected)
			Netgame.numconnected++;
	
	if (Network_status == NETSTAT_STARTING)
		return;

	Netgame.numplayers = N_players;
	Netgame.game_status = Network_status;
	Netgame.max_numplayers = MaxNumNetPlayers;

	for (i = 0; i < MAX_NUM_NET_PLAYERS; i++) 
	{
		Netgame.players[i].connected = Players[i].connected;
		for(j = 0; j < MAX_NUM_NET_PLAYERS; j++)
			Netgame.kills[i][j] = kill_matrix[i][j];
		Netgame.killed[i] = Players[i].net_killed_total;
		Netgame.player_kills[i] = Players[i].net_kills_total;
	}

	Netgame.team_kills[0] = team_kills[0];
	Netgame.team_kills[1] = team_kills[1];
	Netgame.levelnum = Current_level_num;
}

/* Send an updated endlevel status to everyone (if we are host) or host (if we are client)  */
void net_udp_send_endlevel_packet(void)
{
	int i = 0, j = 0, len = 0;

	if (multi_i_am_master())
	{
		ubyte buf[UPKT_MAX_SIZE];

		memset(buf, 0, sizeof(buf));

		buf[len] = UPID_ENDLEVEL_H;											len++;
		buf[len] = Fuelcen_seconds_left;									len++;

		for (i = 0; i < MAX_PLAYERS; i++)
		{
			buf[len] = Players[i].connected;								len++;
			PUT_INTEL_SHORT(buf + len, Players[i].net_kills_total);			len += 2;
			PUT_INTEL_SHORT(buf + len, Players[i].net_killed_total);		len += 2;
		}

		for (i = 0; i < MAX_PLAYERS; i++)
		{
			for (j = 0; j < MAX_PLAYERS; j++)
			{
				PUT_INTEL_SHORT(buf + len, kill_matrix[i][j]);				len += 2;
			}
		}

		for (i = 1; i < MAX_PLAYERS; i++)
			sendto (UDP_Socket[0], buf, len, 0, (struct sockaddr *)&Netgame.players[i].protocol.udp.addr, sizeof(struct _sockaddr));
	}
	else
	{
		ubyte buf[UPKT_MAX_SIZE];

		memset(buf, 0,  sizeof(buf));

		buf[len] = UPID_ENDLEVEL_C;											len++;
		buf[len] = Player_num;												len++;
		buf[len] = Players[Player_num].connected;							len++;
		buf[len] = Fuelcen_seconds_left;									len++;
		PUT_INTEL_SHORT(buf + len, Players[Player_num].net_kills_total);	len += 2;
		PUT_INTEL_SHORT(buf + len, Players[Player_num].net_killed_total);	len += 2;

		for (i = 0; i < MAX_PLAYERS; i++)
		{
			PUT_INTEL_SHORT(buf + len, kill_matrix[Player_num][i]);			len += 2;
		}

		sendto (UDP_Socket[0], buf, len, 0, (struct sockaddr *)&Netgame.players[0].protocol.udp.addr, sizeof(struct _sockaddr));
	}
}

void net_udp_send_version_deny(struct _sockaddr sender_addr)
{
	ubyte buf[5];
	
	buf[0] = UPID_VERSION_DENY;
	PUT_INTEL_INT(buf + 1, D1X_IVER);
	
	sendto (UDP_Socket[0], buf, sizeof(buf), 0, (struct sockaddr *)&sender_addr, sizeof(struct _sockaddr));
}

void net_udp_process_version_deny(ubyte *data, struct _sockaddr sender_addr)
{
	Netgame.protocol.udp.program_iver = GET_INTEL_INT(&data[1]);
	Netgame.protocol.udp.valid = -1;
}

void net_udp_request_game_info(struct _sockaddr game_addr, int lite)
{
	ubyte buf[UPKT_GAME_INFO_REQ_SIZE];
	
	buf[0] = (lite?UPID_GAME_INFO_LITE:UPID_GAME_INFO_REQ);
	memcpy(&(buf[1]), UDP_REQ_ID, 4);
	PUT_INTEL_INT(buf + 5, D1X_IVER);
	
	sendto (UDP_Socket[0], buf, sizeof(buf), 0, (struct sockaddr *)&game_addr, sizeof(struct _sockaddr));
}

// Check request for game info. Return 1 if sucessful; -1 if version mismatch; 0 if wrong game or some other error - do not process
int net_udp_check_game_info_request(ubyte *data, int data_len)
{
	int sender_iver = 0;
	char sender_id[4] = "";

	if (!multi_i_am_master())
		return 0;
	
	if (data_len != UPKT_GAME_INFO_REQ_SIZE)
		return 0;
	
	memcpy(&sender_id, &(data[1]), 4);
	sender_iver = GET_INTEL_INT(&(data[5]));
	
	if (memcmp(&sender_id, UDP_REQ_ID, 4))
		return 0;
	
	if (sender_iver != D1X_IVER)
		return -1;
		
	return 1;
}

void net_udp_send_game_info(struct _sockaddr sender_addr, ubyte info_upid)
{
	// Send game info to someone who requested it

	int len = 0;
	
	net_udp_update_netgame(); // Update the values in the netgame struct
	
	if (info_upid == UPID_GAME_INFO_LITE)
	{
		ubyte buf[UPKT_MAX_SIZE];
		int tmpvar = 0;

		memset(buf, 0, sizeof(buf));
		
		buf[0] = info_upid;														len++;
		PUT_INTEL_INT(buf + len, D1X_IVER);  									len += 4;
		memcpy(&(buf[len]), Netgame.game_name, NETGAME_NAME_LEN+1);				len += (NETGAME_NAME_LEN+1);
		memcpy(&(buf[len]), Netgame.mission_title, MISSION_NAME_LEN+1);			len += (MISSION_NAME_LEN+1);
		memcpy(&(buf[len]), Netgame.mission_name, 9);		            		len += 9;
		PUT_INTEL_INT(buf + len, Netgame.levelnum);								len += 4;
		buf[len] = Netgame.gamemode;											len++;
		buf[len] = Netgame.RefusePlayers;										len++;
		buf[len] = Netgame.difficulty;											len++;
		tmpvar = Netgame.game_status;
		if (Endlevel_sequence || Control_center_destroyed)
			tmpvar = NETSTAT_ENDLEVEL;
//		if (Netgame.PlayTimeAllowed)
//		{
//			if ( (f2i((i2f (Netgame.PlayTimeAllowed*5*60))-ThisLevelTime)) < 30 )
//			{
//				tmpvar = NETSTAT_ENDLEVEL;
//			}
//		}
		buf[len] = tmpvar;														len++;
		buf[len] = Netgame.numplayers;											len++;
		buf[len] = Netgame.max_numplayers;										len++;
		buf[len] = Netgame.game_flags;											len++;
		buf[len] = Netgame.team_vector;											len++;
		
		sendto (UDP_Socket[0], buf, len, 0, (struct sockaddr *)&sender_addr, sizeof(struct _sockaddr));
	}
	else
	{
		ubyte buf[UPKT_MAX_SIZE];
		int i = 0, j = 0, tmpvar = 0;
		
		memset(buf, 0, sizeof(buf));
	
		buf[0] = info_upid;														len++;
		PUT_INTEL_INT(buf + len, D1X_IVER);  									len += 4;
		buf[len] = Netgame.version_major;										len++;
		buf[len] = Netgame.version_minor;										len++;
		for (i = 0; i < MAX_PLAYERS+4; i++)
		{
			memcpy(&buf[len], Netgame.players[i].callsign, CALLSIGN_LEN+1); 	len += CALLSIGN_LEN+1;
			buf[len] = Netgame.players[i].version_major;						len++;
			buf[len] = Netgame.players[i].version_minor;						len++;
			buf[len] = Netgame.players[i].connected;							len++;
			buf[len] = Netgame.players[i].rank;									len++;
			if (!memcmp((struct _sockaddr *)&sender_addr, (struct _sockaddr *)&Netgame.players[i].protocol.udp.addr, sizeof(struct _sockaddr)))
				buf[len] = 1;
			else
				buf[len] = 0;
																				len++;
		}
		memcpy(&(buf[len]), Netgame.game_name, NETGAME_NAME_LEN+1);				len += (NETGAME_NAME_LEN+1);
		memcpy(&(buf[len]), Netgame.mission_title, MISSION_NAME_LEN+1);			len += (MISSION_NAME_LEN+1);
		memcpy(&(buf[len]), Netgame.mission_name, 9);		            		len += 9;
		PUT_INTEL_INT(buf + len, Netgame.levelnum);								len += 4;
		buf[len] = Netgame.gamemode;											len++;
		buf[len] = Netgame.RefusePlayers;										len++;
		buf[len] = Netgame.difficulty;											len++;
		tmpvar = Netgame.game_status;
		if (Endlevel_sequence || Control_center_destroyed)
			tmpvar = NETSTAT_ENDLEVEL;
//		if (Netgame.PlayTimeAllowed)
//		{
//			if ( (f2i((i2f (Netgame.PlayTimeAllowed*5*60))-ThisLevelTime)) < 30 )
//			{
//				tmpvar = NETSTAT_ENDLEVEL;
//			}
//		}
		buf[len] = tmpvar;														len++;
		buf[len] = Netgame.numplayers;											len++;
		buf[len] = Netgame.max_numplayers;										len++;
		buf[len] = Netgame.numconnected;										len++;
		buf[len] = Netgame.game_flags;											len++;
		buf[len] = Netgame.team_vector;											len++;
		PUT_INTEL_INT(buf + len, Netgame.AllowedItems);							len += 4;
		PUT_INTEL_SHORT(buf + len, Netgame.Allow_marker_view);					len += 2;
		PUT_INTEL_SHORT(buf + len, Netgame.AlwaysLighting);						len += 2;
		PUT_INTEL_SHORT(buf + len, Netgame.ShowAllNames);						len += 2;
		PUT_INTEL_SHORT(buf + len, Netgame.BrightPlayers);						len += 2;
		PUT_INTEL_SHORT(buf + len, Netgame.InvulAppear);						len += 2;
		memcpy(&buf[len], Netgame.team_name, 2*(CALLSIGN_LEN+1));				len += 2*(CALLSIGN_LEN+1);
		for (i = 0; i < MAX_PLAYERS; i++)
		{
			PUT_INTEL_INT(buf + len, Netgame.locations[i]);					    len += 4;
		}
		for (i = 0; i < MAX_PLAYERS; i++)
		{
			for (j = 0; j < MAX_PLAYERS; j++)
			{
				PUT_INTEL_SHORT(buf + len, Netgame.kills[i][j]);				len += 2;
			}
		}
		PUT_INTEL_SHORT(buf + len, Netgame.segments_checksum);   				len += 2;
		PUT_INTEL_SHORT(buf + len, Netgame.team_kills[0]);						len += 2;
		PUT_INTEL_SHORT(buf + len, Netgame.team_kills[1]);						len += 2;
		for (i = 0; i < MAX_PLAYERS; i++)
		{
			PUT_INTEL_SHORT(buf + len, Netgame.killed[i]);						len += 2;
		}
		for (i = 0; i < MAX_PLAYERS; i++)
		{
			PUT_INTEL_SHORT(buf + len, Netgame.player_kills[i]);				len += 2;
		}
		PUT_INTEL_INT(buf + len, Netgame.KillGoal);								len += 4;
		PUT_INTEL_INT(buf + len, Netgame.PlayTimeAllowed);						len += 4;
		PUT_INTEL_INT(buf + len, Netgame.level_time);							len += 4;
		PUT_INTEL_INT(buf + len, Netgame.control_invul_time);					len += 4;
		PUT_INTEL_INT(buf + len, Netgame.monitor_vector);						len += 4;
		for (i = 0; i < MAX_PLAYERS; i++)
		{
			PUT_INTEL_INT(buf + len, Netgame.player_score[i]);					len += 4;
		}
		for (i = 0; i < MAX_PLAYERS; i++)
		{
			buf[len] = Netgame.player_flags[i];									len++;
		}
		PUT_INTEL_SHORT(buf + len, Netgame.PacketsPerSec);						len += 2;
		buf[len] = Netgame.PacketLossPrevention;								len++;

		sendto (UDP_Socket[0], buf, len, 0, (struct sockaddr *)&sender_addr, sizeof(struct _sockaddr));
	}
}

/* Send game info to all players in this game */
void net_udp_send_netgame_update()
{
	int i = 0;
	
	for (i=1; i<N_players; i++ )
	{
		net_udp_send_game_info(Netgame.players[i].protocol.udp.addr, UPID_GAME_INFO);
	}
}

int net_udp_send_request(void)
{
	// Send a request to join a game 'Netgame'.  Returns 0 if we can join this
	// game, non-zero if there is some problem.
	int i;

	if (Netgame.numplayers < 1)
	 return 1;

	for (i = 0; i < MAX_NUM_NET_PLAYERS; i++)
		if (Netgame.players[i].connected)
			break;

	Assert(i < MAX_NUM_NET_PLAYERS);

	UDP_Seq.type = UPID_REQUEST;
	UDP_Seq.player.connected = Current_level_num;

	net_udp_send_sequence_packet(UDP_Seq, Netgame.players[0].protocol.udp.addr);

	return i;
}
	
void net_udp_process_game_info(ubyte *data, int data_len, struct _sockaddr game_addr, int lite_info)
{
	int len = 0, i = 0, j = 0;
	
	if (lite_info)
	{
		UDP_netgame_info_lite recv_game;
		
		if (data_len > UPKT_MAX_SIZE)
			return;
			
		memcpy(&recv_game, &game_addr, sizeof(struct _sockaddr));
																				len++; // skip UPID byte
		Netgame.protocol.udp.program_iver = GET_INTEL_INT(&(data[len]));		len += 4;
		
		if (recv_game.program_iver != D1X_IVER)
			return;

		memcpy(&recv_game.game_name, &(data[len]), NETGAME_NAME_LEN+1);			len += (NETGAME_NAME_LEN+1);
		memcpy(&recv_game.mission_title, &(data[len]), MISSION_NAME_LEN+1);		len += (MISSION_NAME_LEN+1);
		memcpy(&recv_game.mission_name, &(data[len]), 9);						len += 9;
		recv_game.levelnum = GET_INTEL_INT(&(data[len]));						len += 4;
		recv_game.gamemode = data[len];											len++;
		recv_game.RefusePlayers = data[len];									len++;
		recv_game.difficulty = data[len];										len++;
		recv_game.game_status = data[len];										len++;
		recv_game.numplayers = data[len];										len++;
		recv_game.max_numplayers = data[len];									len++;
		recv_game.game_flags = data[len];										len++;
		recv_game.team_vector = data[len];										len++;
	
		num_active_udp_changed = 1;
		
		for (i = 0; i < num_active_udp_games; i++)
			if (!strcasecmp(Active_udp_games[i].game_name, recv_game.game_name) && !memcmp((struct _sockaddr *)&Active_udp_games[i].game_addr, (struct _sockaddr *)&recv_game.game_addr, sizeof(struct _sockaddr)))
				break;

		if (i == UDP_MAX_NETGAMES)
		{
			return;
		}
		
		memcpy(&Active_udp_games[i], &recv_game, sizeof(UDP_netgame_info_lite));
		
		if (i == num_active_udp_games)
			num_active_udp_games++;

		if (Active_udp_games[i].numplayers == 0)
		{
			// Delete this game
			for (j = i; j < num_active_udp_games-1; j++)
				memcpy(&Active_udp_games[j], &Active_udp_games[j+1], sizeof(UDP_netgame_info_lite));
			num_active_udp_games--;
		}
	}
	else
	{
		if (data_len > UPKT_MAX_SIZE)
			return;

		memcpy((struct _sockaddr *)&Netgame.players[0].protocol.udp.addr, (struct _sockaddr *)&game_addr, sizeof(struct _sockaddr));

																				len++; // skip UPID byte
		Netgame.protocol.udp.program_iver = GET_INTEL_INT(&(data[len]));		len += 4;
		Netgame.version_major = data[len];										len++;
		Netgame.version_minor = data[len];										len++;
		for (i = 0; i < MAX_PLAYERS+4; i++)
		{
			memcpy(&Netgame.players[i].callsign, &(data[len]), CALLSIGN_LEN+1);	len += CALLSIGN_LEN+1;
			Netgame.players[i].version_major = data[len];						len++;
			Netgame.players[i].version_minor = data[len];						len++;
			Netgame.players[i].connected = data[len];							len++;
			Netgame.players[i].rank = data[len];								len++;
			Netgame.players[i].protocol.udp.isyou = data[len];					len++;
		}
		memcpy(&Netgame.game_name, &(data[len]), NETGAME_NAME_LEN+1);			len += (NETGAME_NAME_LEN+1);
		memcpy(&Netgame.mission_title, &(data[len]), MISSION_NAME_LEN+1);		len += (MISSION_NAME_LEN+1);
		memcpy(&Netgame.mission_name, &(data[len]), 9);							len += 9;		
		Netgame.levelnum = GET_INTEL_INT(&(data[len]));							len += 4;
		Netgame.gamemode = data[len];											len++;
		Netgame.RefusePlayers = data[len];										len++;
		Netgame.difficulty = data[len];											len++;
		Netgame.game_status = data[len];										len++;
		Netgame.numplayers = data[len];											len++;
		Netgame.max_numplayers = data[len];										len++;
		Netgame.numconnected = data[len];										len++;
		Netgame.game_flags = data[len];											len++;
		Netgame.team_vector = data[len];										len++;
		Netgame.AllowedItems = GET_INTEL_INT(&(data[len]));						len += 4;		
		Netgame.Allow_marker_view = GET_INTEL_SHORT(&(data[len]));				len += 2;
		Netgame.AlwaysLighting = GET_INTEL_SHORT(&(data[len]));					len += 2;		
		Netgame.ShowAllNames = GET_INTEL_SHORT(&(data[len]));					len += 2;		
		Netgame.BrightPlayers = GET_INTEL_SHORT(&(data[len]));					len += 2;
		Netgame.InvulAppear = GET_INTEL_SHORT(&(data[len]));					len += 2;
		memcpy(Netgame.team_name, &(data[len]), 2*(CALLSIGN_LEN+1));			len += 2*(CALLSIGN_LEN+1);
		for (i = 0; i < MAX_PLAYERS; i++)
		{
			Netgame.locations[i] = GET_INTEL_INT(&(data[len]));				    len += 4;
		}
		for (i = 0; i < MAX_PLAYERS; i++)
		{
			for (j = 0; j < MAX_PLAYERS; j++)
			{
				Netgame.kills[i][j] = GET_INTEL_SHORT(&(data[len]));		    len += 2;
			}
		}
		Netgame.segments_checksum = GET_INTEL_SHORT(&(data[len]));				len += 2;
		Netgame.team_kills[0] = GET_INTEL_SHORT(&(data[len]));					len += 2;	
		Netgame.team_kills[1] = GET_INTEL_SHORT(&(data[len]));					len += 2;
		for (i = 0; i < MAX_PLAYERS; i++)
		{
			Netgame.killed[i] = GET_INTEL_SHORT(&(data[len]));				    len += 2;
		}
		for (i = 0; i < MAX_PLAYERS; i++)
		{
			Netgame.player_kills[i] = GET_INTEL_SHORT(&(data[len]));		    len += 2;
		}
		Netgame.KillGoal = GET_INTEL_INT(&(data[len]));							len += 4;
		Netgame.PlayTimeAllowed = GET_INTEL_INT(&(data[len]));					len += 4;
		Netgame.level_time = GET_INTEL_INT(&(data[len]));						len += 4;
		Netgame.control_invul_time = GET_INTEL_INT(&(data[len]));				len += 4;
		Netgame.monitor_vector = GET_INTEL_INT(&(data[len]));					len += 4;
		for (i = 0; i < MAX_PLAYERS; i++)
		{
			Netgame.player_score[i] = GET_INTEL_INT(&(data[len]));		 	   len += 4;
		}
		for (i = 0; i < MAX_PLAYERS; i++)
		{
			Netgame.player_flags[i] = data[len];								len++;
		}
		Netgame.PacketsPerSec = GET_INTEL_SHORT(&(data[len]));					len += 2;
		Netgame.PacketLossPrevention = data[len];								len++;

		Netgame.protocol.udp.valid = 1; // This game is valid! YAY!
	}
}

void net_udp_process_dump(ubyte *data, int len, struct _sockaddr sender_addr)
{
	// Our request for join was denied.  Tell the user why.
	if (memcmp((struct _sockaddr *)&sender_addr,(struct _sockaddr *)&Netgame.players[0].protocol.udp.addr,sizeof(struct _sockaddr)))
		return;

	if (data[1]==DUMP_KICKED)
	{
		if (Network_status==NETSTAT_PLAYING)
			multi_leave_game();
		Function_mode = FMODE_MENU;
		nm_messagebox(NULL, 1, TXT_OK, "%s has kicked you out!",Players[0].callsign);
		Function_mode = FMODE_GAME;
		multi_quit_game = 1;
		multi_leave_menu = 1;
		multi_reset_stuff();
		Function_mode = FMODE_MENU;
	}
	else
	{
		nm_messagebox(NULL, 1, TXT_OK, NET_DUMP_STRINGS(data[1]));
		Network_status = NETSTAT_MENU;
 	}
}
	
void net_udp_process_request(UDP_sequence_packet *their)
{
	// Player is ready to receieve a sync packet
	int i;

	for (i = 0; i < N_players; i++)
		if (!memcmp((struct _sockaddr *)&their->player.protocol.udp.addr, (struct _sockaddr *)&Netgame.players[i].protocol.udp.addr, sizeof(struct _sockaddr)) && (!strcasecmp(their->player.callsign, Netgame.players[i].callsign)))
		{
			Players[i].connected = CONNECT_PLAYING;
			Netgame.players[i].LastPacketTime = timer_get_fixed_seconds();
			break;
		}
}

void net_udp_process_packet(ubyte *data, struct _sockaddr sender_addr, int length )
{
	UDP_sequence_packet their;
	memset(&their, 0, sizeof(UDP_sequence_packet));

	switch (data[0])
	{
		case UPID_VERSION_DENY:
			net_udp_process_version_deny(data, sender_addr);
			break;
		case UPID_GAME_INFO_REQ:
		{
			int result = 0;
			result = net_udp_check_game_info_request(data, length);
			if (result == -1)
				net_udp_send_version_deny(sender_addr);
			else if (result == 1)
				net_udp_send_game_info(sender_addr, UPID_GAME_INFO);
			break;
		}
		case UPID_GAME_INFO:
			net_udp_process_game_info(data, length, sender_addr, 0);
			break;
		case UPID_GAME_INFO_LITE_REQ:
			if (net_udp_check_game_info_request(data, length) == 1)
				net_udp_send_game_info(sender_addr, UPID_GAME_INFO_LITE);
			break;
		case UPID_GAME_INFO_LITE:
			net_udp_process_game_info(data, length, sender_addr, 1);
			break;
		case UPID_DUMP:
			if ((Network_status == NETSTAT_WAITING) || (Network_status == NETSTAT_PLAYING))
				net_udp_process_dump(data, length, sender_addr);
			break;
		case UPID_ADDPLAYER:
			net_udp_receive_sequence_packet(data, &their, sender_addr);
			net_udp_new_player(&their);
			break;
		case UPID_REQUEST:
			net_udp_receive_sequence_packet(data, &their, sender_addr);
			if (Network_status == NETSTAT_STARTING) 
			{
				// Someone wants to join our game!
				net_udp_add_player(&their);
			}
			else if (Network_status == NETSTAT_WAITING)
			{
				// Someone is ready to recieve a sync packet
				net_udp_process_request(&their);
			}
			else if (Network_status == NETSTAT_PLAYING)
			{
				// Someone wants to join a game in progress!
				if (Netgame.RefusePlayers)
					net_udp_do_refuse_stuff (&their);
				else
					net_udp_welcome_player(&their);
			}
			break;
		case UPID_QUIT_JOINING:
			net_udp_receive_sequence_packet(data, &their, sender_addr);
			if (Network_status == NETSTAT_STARTING)
				net_udp_remove_player( &their );
			else if ((Network_status == NETSTAT_PLAYING) && (Network_send_objects))
				net_udp_stop_resync( &their );
			break;
		case UPID_SYNC:
			if (Network_status == NETSTAT_WAITING)
				net_udp_read_sync_packet(data, length, sender_addr);
			break;
		case UPID_OBJECT_DATA:
			if (Network_status == NETSTAT_WAITING) 
				net_udp_read_object_packet(data);
			break;
		case UPID_PING:
			if (!multi_i_am_master())
				net_udp_process_ping(data, length, sender_addr);
			break;
		case UPID_PONG:
			if (multi_i_am_master())
				net_udp_process_pong(data, length, sender_addr);
			break;
		case UPID_ENDLEVEL_H:
			if ((!multi_i_am_master()) && ((Network_status == NETSTAT_ENDLEVEL) || (Network_status == NETSTAT_PLAYING)))
				net_udp_read_endlevel_packet( data, length, sender_addr );
			break;
		case UPID_ENDLEVEL_C:
			if ((multi_i_am_master()) && ((Network_status == NETSTAT_ENDLEVEL) || (Network_status == NETSTAT_PLAYING)))
				net_udp_read_endlevel_packet( data, length, sender_addr );
			break;
		case UPID_PDATA_H:
			if (!multi_i_am_master())
				net_udp_process_pdata( data, length, sender_addr );
			break;
		case UPID_PDATA_C:
			if (multi_i_am_master())
				net_udp_process_pdata( data, length, sender_addr );
			break;
		case UPID_MDATA_P0:
			net_udp_process_mdata( data, length, sender_addr, 0 );
			break;
		case UPID_MDATA_P1:
			net_udp_process_mdata( data, length, sender_addr, 1 );
			break;
		case UPID_MDATA_ACK:
			net_udp_noloss_got_ack(data, length);
			break;
		default:
			con_printf(CON_DEBUG, "unknown packet type received - type %i\n", data[0]);
			break;
	}
}

// Packet for end of level syncing
void net_udp_read_endlevel_packet( ubyte *data, int data_len, struct _sockaddr sender_addr )
{
	int len = 0, i = 0, j = 0;
	ubyte tmpvar = 0;
	fix time = timer_get_fixed_seconds();
	
	if (multi_i_am_master())
	{
		ubyte pnum = data[1];

		if (memcmp((struct _sockaddr *)&Netgame.players[pnum].protocol.udp.addr, (struct _sockaddr *)&sender_addr, sizeof(struct _sockaddr)))
			return;

		len += 2;

		Players[pnum].connected = data[len];								len++;
		tmpvar = data[len];													len++;
		if ((Network_status != NETSTAT_PLAYING) && (Players[pnum].connected == CONNECT_PLAYING) && (tmpvar < Fuelcen_seconds_left))
			Fuelcen_seconds_left = tmpvar;
		Players[pnum].net_kills_total = GET_INTEL_SHORT(&(data[len]));		len += 2;
		Players[pnum].net_killed_total = GET_INTEL_SHORT(&(data[len]));		len += 2;

		for (i = 0; i < MAX_PLAYERS; i++)
		{
			kill_matrix[pnum][i] = GET_INTEL_SHORT(&(data[len]));			len += 2;
		}

		Netgame.players[pnum].LastPacketTime = time;
	}
	else
	{
		if (memcmp((struct _sockaddr *)&Netgame.players[0].protocol.udp.addr, (struct _sockaddr *)&sender_addr, sizeof(struct _sockaddr)))
			return;

		len++;

		tmpvar = data[len];													len++;
		if ((Network_status != NETSTAT_PLAYING) && (tmpvar < Fuelcen_seconds_left))
			Fuelcen_seconds_left = tmpvar;

		for (i = 0; i < MAX_PLAYERS; i++)
		{
			if (i == Player_num)
			{
				len += 5;
				continue;
			}
				
			Players[i].connected = data[len];								len++;
			Players[i].net_kills_total = GET_INTEL_SHORT(&(data[len]));		len += 2;
			Players[i].net_killed_total = GET_INTEL_SHORT(&(data[len]));	len += 2;

			if (Players[i].connected)
				Netgame.players[i].LastPacketTime = time;
		}

		for (i = 0; i < MAX_PLAYERS; i++)
		{
			for (j = 0; j < MAX_PLAYERS; j++)
			{
				if (i != Player_num)
				{
					kill_matrix[i][j] = GET_INTEL_SHORT(&(data[len]));
				}
																			len += 2;
			}
		}
	}
}

void
net_udp_pack_objects(void)
{
	// Switching modes, pack the object array

	special_reset_objects();
}				

int
net_udp_verify_objects(int remote, int local)
{
	int i;
	int nplayers, got_controlcen=0;

	if ((remote-local) > 10)
		return(-1);

	if (Game_mode & GM_MULTI_ROBOTS)
		got_controlcen = 1;

	nplayers = 0;

	for (i = 0; i <= Highest_object_index; i++)
	{
		if ((Objects[i].type == OBJ_PLAYER) || (Objects[i].type == OBJ_GHOST))
			nplayers++;
		if (Objects[i].type == OBJ_CNTRLCEN)
			got_controlcen=1;
	}

	if (got_controlcen && (MaxNumNetPlayers<=nplayers))
		return(0);

	return(1);
}
	
void
net_udp_read_object_packet( ubyte *data )
{
	// Object from another net player we need to sync with

	short objnum, remote_objnum;
	sbyte obj_owner;
	int segnum, i;
	object *obj;

	static int my_pnum = 0;
	static int mode = 0;
	static int object_count = 0;
	static int frame_num = 0;
	int nobj = data[1];
	int loc = 3;
	int remote_frame_num = data[2];
	
	frame_num++;

	for (i = 0; i < nobj; i++)
	{
		objnum = INTEL_SHORT(*(short *)(data+loc));			loc += 2;
		obj_owner = data[loc];								loc += 1;
		remote_objnum = INTEL_SHORT(*(short *)(data+loc));	loc += 2;

		if (objnum == -1) 
		{
			// Clear object array
			init_objects();
			Network_rejoined = 1;
			my_pnum = obj_owner;
			change_playernum_to(my_pnum);
			mode = 1;
			object_count = 0;
			frame_num = 1;
		}
		else if (objnum == -2)
		{
			// Special debug checksum marker for entire send
			if (mode == 1)
			{
				net_udp_pack_objects();
				mode = 0;
			}
			if (remote_objnum != object_count) {
				Int3();
			}
			if (net_udp_verify_objects(remote_objnum, object_count))
			{
				// Failed to sync up 
				nm_messagebox(NULL, 1, TXT_OK, TXT_NET_SYNC_FAILED);
				Network_status = NETSTAT_MENU;				
				return;
			}
			frame_num = 0;
		}
		else 
		{
			if (frame_num != remote_frame_num)
				Int3();

			object_count++;
			if ((obj_owner == my_pnum) || (obj_owner == -1)) 
			{
				if (mode != 1)
					Int3(); // SEE ROB
				objnum = remote_objnum;
				//if (objnum > Highest_object_index)
				//{
				//	Highest_object_index = objnum;
				//	num_objects = Highest_object_index+1;
				//}
			}
			else {
				if (mode == 1)
				{
					net_udp_pack_objects();
					mode = 0;
				}
				objnum = obj_allocate();
			}
			if (objnum != -1) {
				obj = &Objects[objnum];
				if (obj->segnum != -1)
					obj_unlink(objnum);
				Assert(obj->segnum == -1);
				Assert(objnum < MAX_OBJECTS);
				memcpy(obj, &data[loc], sizeof(object));
				net_udp_swap_object(obj);
				loc += sizeof(object);
				segnum = obj->segnum;
				obj->next = obj->prev = obj->segnum = -1;
				obj->attached_obj = -1;
				if (segnum > -1)
					obj_link(obj-Objects,segnum);
				if (obj_owner == my_pnum) 
					map_objnum_local_to_local(objnum);
				else if (obj_owner != -1)
					map_objnum_local_to_remote(objnum, remote_objnum, obj_owner);
				else
					object_owner[objnum] = -1;
			}
		} // For a standard onbject
	} // For each object in packet
}

/*
 * Polling loop waiting for sync packet to start game after having sent request
 */
int net_udp_sync_poll( newmenu *menu, d_event *event, void *userdata )
{
	static fix t1 = 0;
	int rval = 0;

	if (event->type != EVENT_IDLE)
		return 0;
	
	menu = menu;
	userdata = userdata;
	
	net_udp_listen();

	// Leave if Host disconnects
	if (Netgame.players[0].connected == CONNECT_DISCONNECTED)
		rval = -2;

	if (Network_status != NETSTAT_WAITING)	// Status changed to playing, exit the menu
		rval = -2;

	if (Network_status != NETSTAT_MENU && !Network_rejoined && (timer_get_fixed_seconds() > t1+F1_0*2))
	{
		int i;

		// Poll time expired, re-send request
		
		t1 = timer_get_fixed_seconds();

		i = net_udp_send_request();
		if (i < 0)
			rval = -2;
	}
	
	return rval;
}

int net_udp_start_poll( newmenu *menu, d_event *event, void *userdata )
{
	newmenu_item *menus = newmenu_get_items(menu);
	int nitems = newmenu_get_nitems(menu);
	int i,n,nm;

	if (event->type != EVENT_IDLE)
		return 0;
	
	userdata = userdata;
	
	Assert(Network_status == NETSTAT_STARTING);

	if (!menus[0].value) {
			menus[0].value = 1;
	}

	for (i=1; i<nitems; i++ )	{
		if ( (i>= N_players) && (menus[i].value) )	{
			menus[i].value = 0;
		}
	}

	nm = 0;
	for (i=0; i<nitems; i++ )	{
		if ( menus[i].value )	{
			nm++;
			if ( nm > N_players )	{
				menus[i].value = 0;
			}
		}
	}

	if ( nm > MaxNumNetPlayers )	{
		nm_messagebox( TXT_ERROR, 1, TXT_OK, "%s %d %s", TXT_SORRY_ONLY, MaxNumNetPlayers, TXT_NETPLAYERS_IN );
		// Turn off the last player highlighted
		for (i = N_players; i > 0; i--)
			if (menus[i].value == 1) 
			{
				menus[i].value = 0;
				break;
			}
	}

   //added/killed by Victor Rachels to eventually add msging
           //since nitems should not be changing, anyway
//        if (nitems > MAX_PLAYERS ) return;
   //end this section kill - VR
	
	n = Netgame.numplayers;
	net_udp_listen();

	if (n < Netgame.numplayers ) 	
	{
		sprintf( menus[N_players-1].text, "%d. %-16s", N_players, Netgame.players[N_players-1].callsign );

		//Begin addition by GF
		digi_play_sample(SOUND_HUD_MESSAGE, F1_0);  //A noise to alert you when someone joins a starting game...
		//End addition by GF

		if (N_players <= MaxNumNetPlayers)
		{
			menus[N_players-1].value = 1;
		}
	} 
	else if ( n > Netgame.numplayers )	
	{
		// One got removed...

		//Begin addition by GF
		// <Taken out for now due to lack of testing> digi_play_sample(SOUND_HUD_KILL, F1_0);  //A noise to alert you when someone leaves a starting game...
		//End addition by GF

		for (i=0; i<N_players; i++ )	
		{
			sprintf( menus[i].text, "%d. %-16s", i+1, Netgame.players[i].callsign );
			if (i < MaxNumNetPlayers)
				menus[i].value = 1;
			else
				menus[i].value = 0;
		}
		for (i=N_players; i<n; i++ )	
		{
			sprintf( menus[i].text, "%d. ", i+1 );		// Clear out the deleted entries...
			menus[i].value = 0;
		}
   }
	
	return 0;
}

static int opt_cinvul, opt_team_anarchy, opt_coop, opt_show_on_map, opt_closed, opt_refuse, opt_maxnet;
static int opt_show_on_map, opt_difficulty, opt_setpower, opt_port, opt_packets, opt_plp;
static int last_maxnet, last_cinvul=0;

void net_udp_set_power (void)
{
	newmenu_item m[MULTI_ALLOW_POWERUP_MAX];
	int i;

	for (i = 0; i < MULTI_ALLOW_POWERUP_MAX; i++)
	{
		m[i].type = NM_TYPE_CHECK; m[i].text = multi_allow_powerup_text[i]; m[i].value = (Netgame.AllowedItems >> i) & 1;
	}

	i = newmenu_do1( NULL, "Objects to allow", MULTI_ALLOW_POWERUP_MAX, m, NULL, NULL, 0 );

	if (i > -1)
	{
		Netgame.AllowedItems &= ~NETFLAG_DOPOWERUP;
		for (i = 0; i < MULTI_ALLOW_POWERUP_MAX; i++)
			if (m[i].value)
				Netgame.AllowedItems |= (1 << i);
	}
}

int net_udp_more_options_poll( newmenu *menu, d_event *event, void *userdata );

void net_udp_more_game_options ()
{
	int opt=0,i=0;
	char srinvul[50],packstring[5];
	newmenu_item m[9];

	snprintf(packstring,sizeof(char)*4,"%d",Netgame.PacketsPerSec);
	
	opt_difficulty = opt;
	m[opt].type = NM_TYPE_SLIDER; m[opt].value=Netgame.difficulty; m[opt].text=TXT_DIFFICULTY; m[opt].min_value=0; m[opt].max_value=(NDL-1); opt++;

	opt_cinvul = opt;
	sprintf( srinvul, "%s: %d %s", TXT_REACTOR_LIFE, Netgame.control_invul_time/F1_0/60, TXT_MINUTES_ABBREV );
	m[opt].type = NM_TYPE_SLIDER; m[opt].value=Netgame.control_invul_time/5/F1_0/60; m[opt].text= srinvul; m[opt].min_value=0; m[opt].max_value=10; opt++;

	opt_show_on_map=opt;
	m[opt].type = NM_TYPE_CHECK; m[opt].text = TXT_SHOW_ON_MAP; m[opt].value=(Netgame.game_flags & NETGAME_FLAG_SHOW_MAP); opt_show_on_map=opt; opt++;

	opt_setpower = opt;
	m[opt].type = NM_TYPE_MENU;  m[opt].text = "Set Objects allowed..."; opt++;

	m[opt].type = NM_TYPE_TEXT; m[opt].text = "Packets per second (2 - 20)"; opt++;
	opt_packets=opt;
	m[opt].type = NM_TYPE_INPUT; m[opt].text=packstring; m[opt].text_len=2; opt++;

	m[opt].type = NM_TYPE_TEXT; m[opt].text = "Network port"; opt++;
	opt_port = opt;
	m[opt].type = NM_TYPE_INPUT; m[opt].text = UDP_MyPort; m[opt].text_len=5; opt++;
	opt_plp = opt;
	m[opt].type = NM_TYPE_CHECK; m[opt].text = "Packet Loss Prevention"; m[opt].value = Netgame.PacketLossPrevention; opt++;

menu:
	i = newmenu_do1( NULL, "Advanced netgame options", opt, m, net_udp_more_options_poll, NULL, 0 );

	Netgame.control_invul_time = m[opt_cinvul].value*5*F1_0*60;

	if (i==opt_setpower)
	{
		net_udp_set_power ();
		goto menu;
	}

	Netgame.PacketsPerSec=atoi(packstring);
	
	if (Netgame.PacketsPerSec>20)
	{
		Netgame.PacketsPerSec=20;
		nm_messagebox(TXT_ERROR, 1, TXT_OK, "Packet value out of range\nSetting value to 20");
	}

	if (Netgame.PacketsPerSec<2)
	{
		nm_messagebox(TXT_ERROR, 1, TXT_OK, "Packet value out of range\nSetting value to 2");
		Netgame.PacketsPerSec=2;
	}

	if ((atoi(UDP_MyPort)) < 0 ||(atoi(UDP_MyPort)) > 65535)
	{
		snprintf (UDP_MyPort, sizeof(UDP_MyPort), "%d", UDP_PORT_DEFAULT);
		nm_messagebox(TXT_ERROR, 1, TXT_OK, "Illegal port");
	}
	
	Netgame.difficulty=Difficulty_level = m[opt_difficulty].value;
	if (m[opt_show_on_map].value)
		Netgame.game_flags |= NETGAME_FLAG_SHOW_MAP;
	else
		Netgame.game_flags &= ~NETGAME_FLAG_SHOW_MAP;
	Netgame.PacketLossPrevention = m[opt_plp].value;
}

int net_udp_more_options_poll( newmenu *menu, d_event *event, void *userdata )
{
	newmenu_item *menus = newmenu_get_items(menu);

	if (event->type != EVENT_IDLE)	// FIXME: Should become EVENT_ITEM_CHANGED when available
		return 0;
	
	userdata = userdata;
	
	if ( last_cinvul != menus[opt_cinvul].value )   {
		sprintf( menus[opt_cinvul].text, "%s: %d %s", TXT_REACTOR_LIFE, menus[opt_cinvul].value*5, TXT_MINUTES_ABBREV );
		last_cinvul = menus[opt_cinvul].value;
	}
	
	return 0;
}

int net_udp_game_param_poll( newmenu *menu, d_event *event, void *userdata )
{
	newmenu_item *menus = newmenu_get_items(menu);
	static int oldmaxnet=0;

	if (event->type != EVENT_IDLE)	// FIXME: Should become EVENT_ITEM_CHANGED when available
		return 0;

	if (menus[opt_team_anarchy].value) 
	{
		menus[opt_closed].value = 1;
		menus[opt_closed-1].value = 0;
		menus[opt_closed+1].value = 0;
	}

	if (menus[opt_coop].value)
	{
		oldmaxnet=1;

		if (menus[opt_maxnet].value>2) 
		{
			menus[opt_maxnet].value=2;
		}

		if (menus[opt_maxnet].max_value>2)
		{
			menus[opt_maxnet].max_value=2;
		}

		if (!(Netgame.game_flags & NETGAME_FLAG_SHOW_MAP))
			Netgame.game_flags |= NETGAME_FLAG_SHOW_MAP;

	}
	else // if !Coop game
	{
		if (oldmaxnet)
		{
			oldmaxnet=0;
			menus[opt_maxnet].value=6;
			menus[opt_maxnet].max_value=6;
		}
	}

	if ( last_maxnet != menus[opt_maxnet].value )   
	{
		sprintf( menus[opt_maxnet].text, "Maximum players: %d", menus[opt_maxnet].value+2 );
		last_maxnet = menus[opt_maxnet].value;
	}
	
	return 0;
}

int net_udp_get_game_params()
{
	int i;
	int opt, opt_name, opt_level, opt_mode,opt_moreopts;
	newmenu_item m[20];
	char slevel[5];
	char level_text[32];
	char srmaxnet[50];

	for (i=0;i<MAX_PLAYERS;i++)
		if (i!=Player_num)
			Players[i].callsign[0]=0;

	MaxNumNetPlayers = MAX_NUM_NET_PLAYERS;
	Netgame.RefusePlayers=0;
	sprintf( Netgame.game_name, "%s%s", Players[Player_num].callsign, TXT_S_GAME );
	Netgame.difficulty=PlayerCfg.DefaultDifficulty;
	Netgame.max_numplayers=MaxNumNetPlayers;
	if (GameArg.MplUdpMyPort != 0)
		snprintf (UDP_MyPort, sizeof(UDP_MyPort), "%d", GameArg.MplUdpMyPort);
	else
		snprintf (UDP_MyPort, sizeof(UDP_MyPort), "%d", UDP_PORT_DEFAULT);
	last_cinvul = 0;   //default to zero
	Netgame.AllowedItems=0;
	Netgame.AllowedItems |= NETFLAG_DOPOWERUP;
	Netgame.PacketLossPrevention = 1;


	if (!select_mission(1, TXT_MULTI_MISSION))
		return -1;

	strcpy(Netgame.mission_name, Current_mission_filename);
	strcpy(Netgame.mission_title, Current_mission_longname);

	sprintf( slevel, "1" );

	opt = 0;
	m[opt].type = NM_TYPE_TEXT; m[opt].text = TXT_DESCRIPTION; opt++;

	opt_name = opt;
	m[opt].type = NM_TYPE_INPUT; m[opt].text = Netgame.game_name; m[opt].text_len = NETGAME_NAME_LEN; opt++;

	sprintf(level_text, "%s (1-%d)", TXT_LEVEL_, Last_level);
	if (Last_secret_level < -1)
		sprintf(level_text+strlen(level_text)-1, ", S1-S%d)", -Last_secret_level);
	else if (Last_secret_level == -1)
		sprintf(level_text+strlen(level_text)-1, ", S1)");

	Assert(strlen(level_text) < 32);

	m[opt].type = NM_TYPE_TEXT; m[opt].text = level_text; opt++;

	opt_level = opt;
	m[opt].type = NM_TYPE_INPUT; m[opt].text = slevel; m[opt].text_len=4; opt++;
	m[opt].type = NM_TYPE_TEXT; m[opt].text = TXT_OPTIONS; opt++;

	opt_mode = opt;
	m[opt].type = NM_TYPE_RADIO; m[opt].text = TXT_ANARCHY; m[opt].value=(Netgame.gamemode == NETGAME_ANARCHY); m[opt].group=0; opt++;
	m[opt].type = NM_TYPE_RADIO; m[opt].text = TXT_TEAM_ANARCHY; m[opt].value=(Netgame.gamemode == NETGAME_TEAM_ANARCHY); m[opt].group=0; opt_team_anarchy=opt; opt++;
	m[opt].type = NM_TYPE_RADIO; m[opt].text = TXT_ANARCHY_W_ROBOTS; m[opt].value=(Netgame.gamemode == NETGAME_ROBOT_ANARCHY); m[opt].group=0; opt++;
	m[opt].type = NM_TYPE_RADIO; m[opt].text = TXT_COOPERATIVE; m[opt].value=(Netgame.gamemode == NETGAME_COOPERATIVE); m[opt].group=0; opt_coop=opt; opt++;

	m[opt].type = NM_TYPE_TEXT; m[opt].text = ""; opt++;

	m[opt].type = NM_TYPE_RADIO; m[opt].text = "Open game"; m[opt].group=1; m[opt].value=(!Netgame.RefusePlayers && !Netgame.game_flags & NETGAME_FLAG_CLOSED); opt++;
	opt_closed = opt;
	m[opt].type = NM_TYPE_RADIO; m[opt].text = TXT_CLOSED_GAME; m[opt].group=1; m[opt].value=Netgame.game_flags & NETGAME_FLAG_CLOSED; opt++;
	opt_refuse = opt;
	m[opt].type = NM_TYPE_RADIO; m[opt].text = "Restricted Game              "; m[opt].group=1; m[opt].value=Netgame.RefusePlayers; opt++;

	opt_maxnet = opt;
	sprintf( srmaxnet, "Maximum players: %d", MaxNumNetPlayers);
	m[opt].type = NM_TYPE_SLIDER; m[opt].value=Netgame.max_numplayers-2; m[opt].text= srmaxnet; m[opt].min_value=0; 
	m[opt].max_value=MaxNumNetPlayers-2; opt++;
	last_maxnet=MaxNumNetPlayers-2;
	
	opt_moreopts=opt;
	m[opt].type = NM_TYPE_MENU;  m[opt].text = "Advanced options"; opt++;

	Assert(opt <= 20);

menu:
	i = newmenu_do1( NULL, NULL, opt, m, net_udp_game_param_poll, NULL, 1 );

	if (i==opt_moreopts)
	{
		if ( m[opt_mode+3].value )
			Game_mode=GM_MULTI_COOP;
		net_udp_more_game_options();
		Game_mode=0;
		goto menu;
	}
	Netgame.RefusePlayers=m[opt_refuse].value;

	if ( i > -1 )   {
		MaxNumNetPlayers = m[opt_maxnet].value+2;
		Netgame.max_numplayers=MaxNumNetPlayers;

		Netgame.levelnum = atoi(slevel);

		if (!strnicmp(slevel, "s", 1))
			Netgame.levelnum = -atoi(slevel+1);
		else
			Netgame.levelnum = atoi(slevel);

		if ((Netgame.levelnum < Last_secret_level) || (Netgame.levelnum > Last_level) || (Netgame.levelnum == 0))
		{
			nm_messagebox(TXT_ERROR, 1, TXT_OK, TXT_LEVEL_OUT_RANGE );
			sprintf(slevel, "1");
			goto menu;
		}

		if ( m[opt_mode].value )
			Netgame.gamemode = NETGAME_ANARCHY;

		else if (m[opt_mode+1].value) {
			Netgame.gamemode = NETGAME_TEAM_ANARCHY;
		}
// 		else if (ANARCHY_ONLY_MISSION) {
// 			nm_messagebox(NULL, 1, TXT_OK, TXT_ANARCHY_ONLY_MISSION);
// 			m[opt_mode+2].value = 0;
// 			m[opt_mode+3].value = 0;
// 			m[opt_mode].value = 1;
// 			goto menu;
// 		}
		else if ( m[opt_mode+2].value ) 
			Netgame.gamemode = NETGAME_ROBOT_ANARCHY;
		else if ( m[opt_mode+3].value ) 
			Netgame.gamemode = NETGAME_COOPERATIVE;
		else Int3(); // Invalid mode -- see Rob
		if (m[opt_closed].value)
			Netgame.game_flags |= NETGAME_FLAG_CLOSED;
		else
			Netgame.game_flags &= ~NETGAME_FLAG_CLOSED;
	}
	return i;
}

void
net_udp_set_game_mode(int gamemode)
{
	Show_kill_list = 1;

	if ( gamemode == NETGAME_ANARCHY )
		Game_mode = GM_NETWORK;
	else if ( gamemode == NETGAME_ROBOT_ANARCHY )
		Game_mode = GM_NETWORK | GM_MULTI_ROBOTS;
	else if ( gamemode == NETGAME_COOPERATIVE ) 
		Game_mode = GM_NETWORK | GM_MULTI_COOP | GM_MULTI_ROBOTS;
	else if ( gamemode == NETGAME_TEAM_ANARCHY )
	{
		Game_mode = GM_NETWORK | GM_TEAM;
                Show_kill_list = 3;
	}
	else
		Int3();
}

void net_udp_read_sync_packet( ubyte * data, int data_len, struct _sockaddr sender_addr )
{	
	int i, j;

	char temp_callsign[CALLSIGN_LEN+1];
	
	// This function is now called by all people entering the netgame.

	if (data)
	{
		net_udp_process_game_info(data, data_len, sender_addr, 0);
	}

	N_players = Netgame.numplayers;
	Difficulty_level = Netgame.difficulty;
	Network_status = Netgame.game_status;

	Assert(Function_mode != FMODE_GAME);

	// New code, 11/27

	if (Netgame.segments_checksum != my_segments_checksum)
	{
		Network_status = NETSTAT_MENU;
		nm_messagebox(TXT_ERROR, 1, TXT_OK, TXT_NETLEVEL_NMATCH);
#ifdef NDEBUG
		return;
#endif
	}

	// Discover my player number

	memcpy(temp_callsign, Players[Player_num].callsign, CALLSIGN_LEN+1);
	
	Player_num = -1;

	for (i=0; i<MAX_NUM_NET_PLAYERS; i++ )
	{
		Players[i].net_kills_total = 0;
		Players[i].net_killed_total = 0;
	}


	for (i=0; i<N_players; i++ )
	{
		if ( Netgame.players[i].protocol.udp.isyou == 1 && (!strcasecmp( Netgame.players[i].callsign, temp_callsign)) )
		{
			Assert(Player_num == -1); // Make sure we don't find ourselves twice!  Looking for interplay reported bug
			change_playernum_to(i);
		}
		memcpy( Players[i].callsign, Netgame.players[i].callsign, CALLSIGN_LEN+1 );

		Players[i].connected = Netgame.players[i].connected;
		Players[i].net_kills_total = Netgame.player_kills[i];
		Players[i].net_killed_total = Netgame.killed[i];
		if ((Network_rejoined) || (i != Player_num))
			Players[i].score = Netgame.player_score[i];
		for (j = 0; j < MAX_NUM_NET_PLAYERS; j++)
		{
			kill_matrix[i][j] = Netgame.kills[i][j];
		}
	}

	if ( Player_num < 0 )	{
		Network_status = NETSTAT_MENU;
		return;
	}

	if (Network_rejoined)
		for (i=0; i<N_players;i++)
			Players[i].net_killed_total = Netgame.killed[i];

	PlayerCfg.NetlifeKills -= Players[Player_num].net_kills_total;
	PlayerCfg.NetlifeKilled -= Players[Player_num].net_killed_total;

	if (Network_rejoined)
	{
		net_udp_process_monitor_vector(Netgame.monitor_vector);
		Players[Player_num].time_level = Netgame.level_time;
	}

	team_kills[0] = Netgame.team_kills[0];
	team_kills[1] = Netgame.team_kills[1];
	
	Players[Player_num].connected = CONNECT_PLAYING;
	Netgame.players[Player_num].connected = CONNECT_PLAYING;

	if (!Network_rejoined)
	{
		for (i=0; i<NumNetPlayerPositions; i++)
		{
			Objects[Players[i].objnum].pos = Player_init[Netgame.locations[i]].pos;
			Objects[Players[i].objnum].orient = Player_init[Netgame.locations[i]].orient;
		 	obj_relink(Players[i].objnum,Player_init[Netgame.locations[i]].segnum);
		}
	}
	
	Objects[Players[Player_num].objnum].type = OBJ_PLAYER;

	multi_allow_powerup = NETFLAG_DOPOWERUP;

	Network_status = NETSTAT_PLAYING;
	Function_mode = FMODE_GAME;
	multi_sort_kill_list();
}

int net_udp_send_sync(void)
{
	int i, j, np;

	// Check if there are enough starting positions
	if (NumNetPlayerPositions < MaxNumNetPlayers)
	{
		nm_messagebox(TXT_ERROR, 1, TXT_OK, "Not enough start positions\n(set %d got %d)\nNetgame aborted", MaxNumNetPlayers, NumNetPlayerPositions);

		for (i=1; i<N_players; i++)
		{
			net_udp_dump_player(Netgame.players[i].protocol.udp.addr, DUMP_ABORTED);
			Netgame.numplayers = 0;
			net_udp_send_game_info(Netgame.players[i].protocol.udp.addr, UPID_GAME_INFO); // Tell everyone we're bailing
		}
		longjmp(LeaveGame, 0);
	}

	// Randomize their starting locations...
	d_srand( timer_get_fixed_seconds() );
	for (i=0; i<NumNetPlayerPositions; i++ )        
	{
		if (Players[i].connected)
			Players[i].connected = CONNECT_PLAYING; // Get rid of endlevel connect statuses

		if (Game_mode & GM_MULTI_COOP)
			Netgame.locations[i] = i;
		else {
			do 
			{
				np = d_rand() % NumNetPlayerPositions;
				for (j=0; j<i; j++ )    
				{
					if (Netgame.locations[j]==np)   
					{
						np =-1;
						break;
					}
				}
			} while (np<0);
			// np is a location that is not used anywhere else..
			Netgame.locations[i]=np;
		}
	}

	// Push current data into the sync packet

	net_udp_update_netgame();
	Netgame.game_status = NETSTAT_PLAYING;
	Netgame.segments_checksum = my_segments_checksum;

	for (i=0; i<N_players; i++ )
	{
		if ((!Players[i].connected) || (i == Player_num))
			continue;

		net_udp_send_game_info(Netgame.players[i].protocol.udp.addr, UPID_SYNC);
	}

	net_udp_read_sync_packet(NULL, 0, Netgame.players[0].protocol.udp.addr); // Read it myself, as if I had sent it
	return 0;
}

int
net_udp_select_teams(void)
{
	newmenu_item m[MAX_PLAYERS+4];
	int choice, opt, opt_team_b;
	ubyte team_vector = 0;
	char team_names[2][CALLSIGN_LEN+1];
	int i;
	int pnums[MAX_PLAYERS+2];

	// One-time initialization

	for (i = N_players/2; i < N_players; i++) // Put first half of players on team A
	{
		team_vector |= (1 << i);
	}

	sprintf(team_names[0], "%s", TXT_BLUE);
	sprintf(team_names[1], "%s", TXT_RED);

	// Here comes da menu
menu:
	m[0].type = NM_TYPE_INPUT; m[0].text = team_names[0]; m[0].text_len = CALLSIGN_LEN; 

	opt = 1;
	for (i = 0; i < N_players; i++)
	{
		if (!(team_vector & (1 << i)))
		{
			m[opt].type = NM_TYPE_MENU; m[opt].text = Netgame.players[i].callsign; pnums[opt] = i; opt++;
		}
	}
	opt_team_b = opt;
	m[opt].type = NM_TYPE_INPUT; m[opt].text = team_names[1]; m[opt].text_len = CALLSIGN_LEN; opt++;
	for (i = 0; i < N_players; i++)
	{
		if (team_vector & (1 << i))
		{
			m[opt].type = NM_TYPE_MENU; m[opt].text = Netgame.players[i].callsign; pnums[opt] = i; opt++;
		}
	}
	m[opt].type = NM_TYPE_TEXT; m[opt].text = ""; opt++;
	m[opt].type = NM_TYPE_MENU; m[opt].text = TXT_ACCEPT; opt++;

	Assert(opt <= MAX_PLAYERS+4);
	
	choice = newmenu_do(NULL, TXT_TEAM_SELECTION, opt, m, NULL, NULL);

	if (choice == opt-1)
	{
		if ((opt-2-opt_team_b < 2) || (opt_team_b == 1)) 
		{
			nm_messagebox(NULL, 1, TXT_OK, TXT_TEAM_MUST_ONE);
			goto menu;
		}
		
		Netgame.team_vector = team_vector;
		strcpy(Netgame.team_name[0], team_names[0]);
		strcpy(Netgame.team_name[1], team_names[1]);
		return 1;
	}

	else if ((choice > 0) && (choice < opt_team_b)) {
		team_vector |= (1 << pnums[choice]);
	}
	else if ((choice > opt_team_b) && (choice < opt-2)) {
		team_vector &= ~(1 << pnums[choice]);
	}
	else if (choice == -1)
		return 0;
	goto menu;
}

int
net_udp_select_players(void)
{
        int i, j, opts, opt_msg;
        newmenu_item m[MAX_PLAYERS+1];
	char text[MAX_PLAYERS][25];
	char title[50];
	int save_nplayers;

	net_udp_add_player( &UDP_Seq );
		
	for (i=0; i< MAX_PLAYERS; i++ )	{
		sprintf( text[i], "%d.  %-16s", i+1, "" );
		m[i].type = NM_TYPE_CHECK; m[i].text = text[i]; m[i].value = 0;
	}
//added/edited on 11/7/98 by Victor Rachels in an attempt to get msgs going.
        opts=MAX_PLAYERS;
        opt_msg = opts;
//killed for now to not raise people's hopes - 11/10/98 - VR
//        m[opts].type = NM_TYPE_MENU; m[opts].text = "Send message..."; opts++;

	m[0].value = 1;				// Assume server will play...

	sprintf( text[0], "%d. %-16s", 1, Players[Player_num].callsign );
	sprintf( title, "%s %d %s", TXT_TEAM_SELECT, MaxNumNetPlayers, TXT_TEAM_PRESS_ENTER );

GetPlayersAgain:

        j=opt_msg;
         while(j==opt_msg)
          {
            j=newmenu_do1( NULL, title, opts, m, net_udp_start_poll, NULL, 1 );

            if(j==opt_msg)
             {
              multi_send_message_dialog();
               if (Network_message_reciever != -1)
                multi_send_message();
             }
          }
//end this section addition
	save_nplayers = N_players;

	if (j<0) 
	{
		// Aborted!					 
		// Dump all players and go back to menu mode

abort:
		for (i=1; i<save_nplayers; i++) {
			net_udp_dump_player(Netgame.players[i].protocol.udp.addr, DUMP_ABORTED);
			Netgame.numplayers = 0;
			net_udp_send_game_info(Netgame.players[i].protocol.udp.addr, UPID_GAME_INFO); // Tell everyone we're bailing
		}

		Network_status = NETSTAT_MENU;
		return(0);
	}

	// Count number of players chosen

	N_players = 0;
	for (i=0; i<save_nplayers; i++ )
	{
		if (m[i].value)	
			N_players++;
	}
	
	if ( N_players > MaxNumNetPlayers) {
		nm_messagebox( TXT_ERROR, 1, TXT_OK, "%s %d %s", TXT_SORRY_ONLY, MaxNumNetPlayers, TXT_NETPLAYERS_IN );
		N_players = save_nplayers;
		goto GetPlayersAgain;
	}

// Let host join without Client available. Let's see if our players like that
#if 0 //def RELEASE
        if ( N_players < 2 )    {
                nm_messagebox( TXT_ERROR, 1, TXT_OK, TXT_TEAM_ATLEAST_TWO );
		N_players = save_nplayers;
		goto GetPlayersAgain;
        }
#endif

// Let host join without Client available. Let's see if our players like that
#if 0 //def RELEASE
	if ( (Netgame.gamemode == NETGAME_TEAM_ANARCHY) && (N_players < 3) ) {
		nm_messagebox(TXT_ERROR, 1, TXT_OK, TXT_TEAM_ATLEAST_THREE );
		N_players = save_nplayers;
		goto GetPlayersAgain;
	}
#endif

	// Remove players that aren't marked.
	N_players = 0;
	for (i=0; i<save_nplayers; i++ )	{
		if (m[i].value)	
		{
			if (i > N_players)
			{
				memcpy(Netgame.players[N_players].callsign, Netgame.players[i].callsign, CALLSIGN_LEN+1);
			}
			Players[N_players].connected = CONNECT_PLAYING;
			N_players++;
		}
		else
		{
			net_udp_dump_player(Netgame.players[i].protocol.udp.addr, DUMP_DORK);
		}
	}

	for (i = N_players; i < MAX_NUM_NET_PLAYERS; i++) {
		memset(Netgame.players[i].callsign, 0, CALLSIGN_LEN+1);
	}

	if (Netgame.gamemode == NETGAME_TEAM_ANARCHY)
		if (!net_udp_select_teams())
			goto abort;

	return(1); 
}

void net_udp_start_game(void)
{
	int i;

	if (setjmp(LeaveGame))
	{
		Game_mode = GM_GAME_OVER;
		return;
	}

	net_udp_init();
	change_playernum_to(0);

	i = net_udp_get_game_params();

	if (i<0) return;

	i = udp_open_socket(0, atoi(UDP_MyPort));

	if (i != 0)
		return;
	
	if (atoi(UDP_MyPort) != UDP_PORT_DEFAULT)
		i = udp_open_socket(1, UDP_PORT_DEFAULT); // Default port open for Broadcasts

	if (i != 0)
		return;

	N_players = 0;

    Netgame.game_status = NETSTAT_STARTING;
	Netgame.numplayers = 0;
	net_udp_set_game_mode(Netgame.gamemode);
	MaxNumNetPlayers = Netgame.max_numplayers;
	Netgame.players[0].protocol.udp.isyou = 1; // I am Host. I need to know that y'know? For syncing later.

	Network_status = NETSTAT_STARTING;

	if(net_udp_select_players())
	{
		StartNewLevel(Netgame.levelnum);
        }
	else
		Game_mode = GM_GAME_OVER;
	
}

int
net_udp_wait_for_sync(void)
{
	char text[60];
	newmenu_item m[2];
	int i, choice=0;
	
	Network_status = NETSTAT_WAITING;
	m[0].type=NM_TYPE_TEXT; m[0].text = text;
	m[1].type=NM_TYPE_TEXT; m[1].text = TXT_NET_LEAVE;
	
	i = net_udp_send_request();

	if (i < 0)
		return(-1);

	sprintf( m[0].text, "%s\n'%s' %s", TXT_NET_WAITING, Netgame.players[i].callsign, TXT_NET_TO_ENTER );

	while (choice > -1)
		choice=newmenu_do( NULL, TXT_WAIT, 2, m, net_udp_sync_poll, NULL );


	if (Network_status != NETSTAT_PLAYING)	
	{
		UDP_sequence_packet me;

		memset(&me, 0, sizeof(UDP_sequence_packet));
		me.type = UPID_QUIT_JOINING;
		memcpy( me.player.callsign, Players[Player_num].callsign, CALLSIGN_LEN+1 );
		net_udp_send_sequence_packet( me, Netgame.players[0].protocol.udp.addr );
		N_players = 0;
		Function_mode = FMODE_MENU;
		Game_mode = GM_GAME_OVER;
		return(-1);	// they cancelled		
	}
	return(0);
}

int net_udp_request_poll( newmenu *menu, d_event *event, void *userdata )
{
	// Polling loop for waiting-for-requests menu

	int i = 0;
	int num_ready = 0;

	if (event->type != EVENT_IDLE)
		return 0;
	
	menu = menu;
	userdata = userdata;
	
	// Send our endlevel packet at regular intervals

//	if (timer_get_fixed_seconds() > t1+ENDLEVEL_SEND_INTERVAL)
//	{
//		net_udp_send_endlevel_packet();
//		t1 = timer_get_fixed_seconds();
//	}

	net_udp_listen();
	net_udp_timeout_check(timer_get_fixed_seconds());

	for (i = 0; i < N_players; i++)
	{
		if ((Players[i].connected == CONNECT_PLAYING) || (Players[i].connected == CONNECT_DISCONNECTED))
			num_ready++;
	}

	if (num_ready == N_players) // All players have checked in or are disconnected
	{
		return -2;
	}
	
	return 0;
}

void
net_udp_wait_for_requests(void)
{
	// Wait for other players to load the level before we send the sync
	int choice, i;
	newmenu_item m[1];
	
	Network_status = NETSTAT_WAITING;

	m[0].type=NM_TYPE_TEXT; m[0].text = TXT_NET_LEAVE;

	Network_status = NETSTAT_WAITING;
	net_udp_flush();

	Players[Player_num].connected = CONNECT_PLAYING;

menu:
	choice = newmenu_do(NULL, TXT_WAIT, 1, m, net_udp_request_poll, NULL);	

	if (choice == -1)
	{
		// User aborted
		choice = nm_messagebox(NULL, 3, TXT_YES, TXT_NO, TXT_START_NOWAIT, TXT_QUITTING_NOW);
		if (choice == 2)
			return;
		if (choice != 0)
			goto menu;
		
		// User confirmed abort
		
		for (i=0; i < N_players; i++)
			if ((Players[i].connected != CONNECT_DISCONNECTED) && (i != Player_num))
				net_udp_dump_player(Netgame.players[i].protocol.udp.addr, DUMP_ABORTED);

		longjmp(LeaveGame, 0);
	}
	else if (choice != -2)
		goto menu;
}

int
net_udp_level_sync(void)
{
 	// Do required syncing between (before) levels

	int result;

	memset(&UDP_MData, 0, sizeof(UDP_mdata_info));
	net_udp_noloss_init_mdata_queue();

//	my_segments_checksum = netmisc_calc_checksum(Segments, sizeof(segment)*(Highest_segment_index+1));

	net_udp_flush(); // Flush any old packets

	if (N_players == 0)
		result = net_udp_wait_for_sync();
	else if (multi_i_am_master())
	{
		net_udp_wait_for_requests();
		result = net_udp_send_sync();
		if (result)
			return -1;
	}
	else
		result = net_udp_wait_for_sync();

	if (result)
	{
		Players[Player_num].connected = CONNECT_DISCONNECTED;
		net_udp_send_endlevel_packet();
		longjmp(LeaveGame, 0);
	}
	return(0);
}

int net_udp_do_join_game()
{
	if (Netgame.game_status == NETSTAT_ENDLEVEL)
	{
		nm_messagebox(TXT_SORRY, 1, TXT_OK, TXT_NET_GAME_BETWEEN2);
		return 0;
	}

	if (!load_mission_by_name(Netgame.mission_name))
	{
		nm_messagebox(NULL, 1, TXT_OK, TXT_MISSION_NOT_FOUND);
		return 0;
	}

	if (!net_udp_can_join_netgame(&Netgame))
	{
		if (Netgame.numplayers == Netgame.max_numplayers)
			nm_messagebox(TXT_SORRY, 1, TXT_OK, TXT_GAME_FULL);
		else
			nm_messagebox(TXT_SORRY, 1, TXT_OK, TXT_IN_PROGRESS);
		return 0;
	}

	// Choice is valid, prepare to join in
	Difficulty_level = Netgame.difficulty;
	MaxNumNetPlayers = Netgame.max_numplayers;
	change_playernum_to(1);

	net_udp_set_game_mode(Netgame.gamemode);
	
	StartNewLevel(Netgame.levelnum);

	return 1;     // look ma, we're in a game!!!
}

void net_udp_leave_game()
{
	int nsave, i;

	net_udp_do_frame(1, 1);

	if ((multi_i_am_master()))
	{
		Netgame.numplayers = 0;
		nsave=N_players;
		N_players=0;
		for (i=1; i<nsave; i++ )
		{
			net_udp_send_game_info(Netgame.players[i].protocol.udp.addr, UPID_GAME_INFO);
		}
		N_players=nsave;
	
	}

	Players[Player_num].connected = CONNECT_DISCONNECTED;
	net_udp_send_endlevel_packet();
	change_playernum_to(0);
	Game_mode = GM_GAME_OVER;
	net_udp_flush();

	if( UDP_Socket[0] != -1 )
		udp_close_socket(0);
	if( UDP_Socket[1] != -1 )
		udp_close_socket(1);
}

void net_udp_flush()
{
	ubyte packet[UPKT_MAX_SIZE];
	struct _sockaddr sender_addr; 

	if (UDP_Socket[0] != -1)
		while (udp_receive_packet( 0, packet, UPKT_MAX_SIZE, &sender_addr) > 0);

	if (UDP_Socket[1] != -1)
		while (udp_receive_packet( 1, packet, UPKT_MAX_SIZE, &sender_addr) > 0);
}

void net_udp_listen()
{
	int size;
	ubyte packet[UPKT_MAX_SIZE];
	struct _sockaddr sender_addr;

	if (UDP_Socket[0] != -1)
	{
		size = udp_receive_packet( 0, packet, UPKT_MAX_SIZE, &sender_addr );
		while ( size > 0 )	{
			net_udp_process_packet( packet, sender_addr, size );
			size = udp_receive_packet( 0, packet, UPKT_MAX_SIZE, &sender_addr );
		}
	}

	if (UDP_Socket[1] != -1)
	{
		size = udp_receive_packet( 1, packet, UPKT_MAX_SIZE, &sender_addr );
		while ( size > 0 )	{
			net_udp_process_packet( packet, sender_addr, size );
			size = udp_receive_packet( 1, packet, UPKT_MAX_SIZE, &sender_addr );
		}
	}
}

void net_udp_send_data( ubyte * ptr, int len, int priority )
{
	char check;

	if (Endlevel_sequence)
		return;

	if (priority)
		PacketUrgent = 1;

	if ((UDP_MData.mbuf_size+len) > UPKT_MBUF_SIZE )
	{
		check = ptr[0];
		net_udp_do_frame(1, 0);
		if (UDP_MData.mbuf_size != 0)
			Int3();
		Assert(check == ptr[0]);
	}

	Assert(UDP_MData.mbuf_size+len <= UPKT_MBUF_SIZE);

	memcpy( &UDP_MData.mbuf[UDP_MData.mbuf_size], ptr, len );
	UDP_MData.mbuf_size += len;
}

void net_udp_timeout_check(fix time)
{
	int i = 0;
	static fix last_timeout_time = 0;
	
	if ((time>=last_timeout_time+F1_0) || (time < last_timeout_time))
	{
		// Check for player timeouts
		for (i = 0; i < N_players; i++)
		{
			if ((i != Player_num) && (Players[i].connected != CONNECT_DISCONNECTED))
			{
				if ((Netgame.players[i].LastPacketTime == 0) || (Netgame.players[i].LastPacketTime > time))
				{
					Netgame.players[i].LastPacketTime = time;
				}
				else if ((time - Netgame.players[i].LastPacketTime) > UDP_TIMEOUT)
				{
					net_udp_timeout_player(i);
				}
			}
			if (Players[i].connected == CONNECT_DISCONNECTED && Objects[Players[i].objnum].type != OBJ_GHOST)
			{
				HUD_init_message( "'%s' has left.", Players[i].callsign );
				multi_make_player_ghost(i);
			}
		}
		last_timeout_time = time;
	}
}

void net_udp_timeout_player(int playernum)
{
	// Remove a player from the game if we haven't heard from them in 
	// a long time.
	int i, n = 0;

	Assert(playernum < N_players);
	Assert(playernum > -1);

	net_udp_disconnect_player(playernum);

	if (Network_status == NETSTAT_PLAYING)
	{
		create_player_appearance_effect(&Objects[Players[playernum].objnum]);
		digi_play_sample(SOUND_HUD_MESSAGE, F1_0);
		hud_message(MSGC_MULTI_INFO, "%s %s", Players[playernum].callsign, TXT_DISCONNECTING);
	}

	for (i = 0; i < N_players; i++)
		if (Players[i].connected) 
			n++;

	if (n == 1 && Network_status == NETSTAT_PLAYING)
	{
		hud_message(MSGC_GAME_FEEDBACK, "You are the only person remaining in this netgame");
	}
}

void net_udp_do_frame(int force, int listen)
{
	int send_mdata = (Network_laser_fired || force || PacketUrgent);
	fix time = 0;
	static fix last_send_time = 0, last_endlevel_time = 0;

	if (!(Game_mode&GM_NETWORK))
		return;

	time = timer_get_fixed_seconds();

	net_udp_ping_frame(time);

	if (WaitForRefuseAnswer && time>(RefuseTimeLimit+(F1_0*12)))
		WaitForRefuseAnswer=0;

	// Send player position packet (and endlevel if needed)
	if ((time >= (last_send_time+(F1_0/Netgame.PacketsPerSec))) || (time < last_send_time))
	{
		multi_send_robot_frame(0);
		multi_send_fire(); // Do firing if needed..
		
		last_send_time = time;
		
		net_udp_send_pdata();

		send_mdata = 1;
	}

	if (send_mdata)
		net_udp_send_mdata(PacketUrgent, time);

	net_udp_noloss_process_queue(time);

	if (((time>=last_endlevel_time+F1_0) || (time < last_endlevel_time)) && Control_center_destroyed)
	{
		last_endlevel_time = time;
		net_udp_send_endlevel_packet();
	}

	if (listen)
	{
		net_udp_timeout_check(time);
		net_udp_listen();
		if (Network_send_objects)
			net_udp_send_objects();
	}

	PacketUrgent = 0;
}

/* CODE FOR PACKET LOSS PREVENTION - START */
/*
 * Adds a packet to our queue. Should be called when an IMPORTANT mdata packet is created.
 * player_ack is an array which should contain 0 for each player that needs to send an ACK signal.
 */
void net_udp_noloss_add_queue_pkt(uint32_t pkt_num, fix time, ubyte *data, ushort data_size, ubyte pnum, ubyte player_ack[MAX_PLAYERS])
{
	int i;
	
	if (!Netgame.PacketLossPrevention)
		return;

	for (i = 0; i < UDP_MDATA_STOR_QUEUE_SIZE; i++)
	{
		int j;
		
		if (UDP_mdata_queue[i].used)
			continue;

		con_printf(CON_VERBOSE, "P#%i: Adding MData pkt_num %i from %i to MData store list\n", Player_num, pkt_num, pnum);

		UDP_mdata_queue[i].used = 1;
		UDP_mdata_queue[i].pkt_initial_timestamp = time;
		for (j = 0; j < MAX_PLAYERS; j++)
			UDP_mdata_queue[i].pkt_timestamp[j] = time;
		UDP_mdata_queue[i].pkt_num = pkt_num;
		UDP_mdata_queue[i].Player_num = pnum;
		memcpy( &UDP_mdata_queue[i].player_ack, player_ack, sizeof(ubyte)*MAX_PLAYERS);
		memcpy( &UDP_mdata_queue[i].data, data, sizeof(char)*data_size );
		UDP_mdata_queue[i].data_size = data_size;
		
		return;
	}
	con_printf(CON_VERBOSE, "P#%i: MData store list is full!\n", Player_num);
}

/*
 * We have received a MDATA packet. Send ACK response to sender!
 * Also check in our UDP_mdata_got list, if we got this packet already. If yes, return 0 so do not process it!
 */
int net_udp_noloss_validate_mdata(uint32_t pkt_num, ubyte sender_pnum, struct _sockaddr sender_addr)
{
	ubyte buf[7];
	int i = 0, len = 0;

	// Check if this comes from a valid IP
	if (multi_i_am_master())
	{
		if (memcmp((struct _sockaddr *)&sender_addr, (struct _sockaddr *)&Netgame.players[sender_pnum].protocol.udp.addr, sizeof(struct _sockaddr)))
			return 0;
	}
	else
		{
		if (memcmp((struct _sockaddr *)&sender_addr, (struct _sockaddr *)&Netgame.players[0].protocol.udp.addr, sizeof(struct _sockaddr)))
			return 0;
	}
	
	con_printf(CON_VERBOSE, "P#%i: Sending MData ACK for pkt %i - pnum %i\n",Player_num, pkt_num, sender_pnum);
	memset(&buf,0,sizeof(buf));
	buf[len] = UPID_MDATA_ACK;													len++;
	buf[len] = Player_num;														len++;
	buf[len] = sender_pnum;														len++;
	PUT_INTEL_INT(buf + len, pkt_num);											len += 4;
	sendto (UDP_Socket[0], buf, len, 0, (struct sockaddr *)&sender_addr, sizeof(struct _sockaddr));
	
	for (i = 0; i < UDP_MDATA_STOR_QUEUE_SIZE; i++)
	{
		if (pkt_num == UDP_mdata_got[sender_pnum].pkt_num[i])
			return 0; // we got this packet already
	}
	UDP_mdata_got[sender_pnum].cur_slot++;
	if (UDP_mdata_got[sender_pnum].cur_slot >= UDP_MDATA_STOR_QUEUE_SIZE)
		UDP_mdata_got[sender_pnum].cur_slot = 0;
	UDP_mdata_got[sender_pnum].pkt_num[UDP_mdata_got[sender_pnum].cur_slot] = pkt_num;
	return 1;
}

/* We got an ACK by a player. Set this player slot to positive! */
void net_udp_noloss_got_ack(ubyte *data, int data_len)
{
	int i = 0, len = 0;
	uint32_t pkt_num = 0;
	ubyte sender_pnum = 0, dest_pnum = 0;

	if (data_len != 7)
		return;

																				len++;
	sender_pnum = data[len];													len++;
	dest_pnum = data[len];														len++;
	pkt_num = GET_INTEL_INT(&data[len]);										len += 4;

	for (i = 0; i < UDP_MDATA_STOR_QUEUE_SIZE; i++)
	{
		if ((pkt_num == UDP_mdata_queue[i].pkt_num) && (dest_pnum == UDP_mdata_queue[i].Player_num))
		{
			con_printf(CON_VERBOSE, "P#%i: Got MData ACK for pkt_num %i from pnum %i for pnum %i\n",Player_num, pkt_num, sender_pnum, dest_pnum);
			UDP_mdata_queue[i].player_ack[sender_pnum] = 1;
			break;
		}
	}
}

/* Init/Free the queue. Call at start and end of a game or level. */
void net_udp_noloss_init_mdata_queue(void)
{
	con_printf(CON_VERBOSE, "P#%i: Clearing MData store/GOT list\n",Player_num);
	memset(&UDP_mdata_queue,0,sizeof(UDP_mdata_store)*UDP_MDATA_STOR_QUEUE_SIZE);
	memset(&UDP_mdata_got,0,sizeof(UDP_mdata_recv)*MAX_PLAYERS);
}

/* Reset the trace list for given player when (dis)connect happens */
void net_udp_noloss_clear_mdata_got(ubyte player_num)
{
	con_printf(CON_VERBOSE, "P#%i: Clearing GOT list for %i\n",Player_num, player_num);
	memset(&UDP_mdata_got[player_num].pkt_num,0,sizeof(uint32_t)*UDP_MDATA_STOR_QUEUE_SIZE);
	UDP_mdata_got[player_num].cur_slot = 0;
}

/*
 * The main queue-process function.
 * Check if we can remove a packet from queue, and check if there are packets in queue which we need to re-send
 */
void net_udp_noloss_process_queue(fix time)
{
	int queuec = 0, plc = 0, count = 0;

	if (!Netgame.PacketLossPrevention)
		return;

	for (queuec = 0; queuec < UDP_MDATA_STOR_QUEUE_SIZE; queuec++)
	{
		fix resend_delay = (F1_0/2);
		int resend = 0;
		
		if (!UDP_mdata_queue[queuec].used)
			continue;

		// Check if at least one connected player has not ACK'd the packet
		for (plc = 0; plc < MAX_PLAYERS; plc++)
		{
			// If player is not playing anymore, we can remove him from list. Also remove *me* (even if that should have been done already). Also make sure Clients do not send to anyone else than Host
			if ((Players[plc].connected != CONNECT_PLAYING || plc == Player_num) || (!multi_i_am_master() && plc > 0))
				UDP_mdata_queue[queuec].player_ack[plc] = 1;

			if (!UDP_mdata_queue[queuec].player_ack[plc])
			{
				// Set resend interval
				if (Netgame.players[plc].ping < 20)
					resend_delay = (F1_0/20);
				else if (Netgame.players[plc].ping < 500)
					resend_delay = i2f(Netgame.players[plc].ping + (Netgame.players[plc].ping/10));
				else
					resend_delay = (F1_0/2);

				// Resend if enough time has passed.
				if ((UDP_mdata_queue[queuec].pkt_timestamp[plc] + resend_delay <= time) || (time < UDP_mdata_queue[queuec].pkt_timestamp[plc]))
				{
					ubyte buf[sizeof(UDP_mdata_info)];
					int len = 0;
					
					con_printf(CON_VERBOSE, "P#%i: Resending pkt_num %i from pnum %i to pnum %i\n",Player_num, UDP_mdata_queue[queuec].pkt_num, UDP_mdata_queue[queuec].Player_num, plc);
					
					UDP_mdata_queue[queuec].pkt_timestamp[plc] = time;
					memset(&buf, 0, sizeof(UDP_mdata_info));
					
					// Prepare the packet and send it
					buf[len] = UPID_MDATA_P1;													len++;
					buf[len] = UDP_mdata_queue[queuec].Player_num;								len++;
					PUT_INTEL_INT(buf + len, UDP_mdata_queue[queuec].pkt_num);					len += 4;
					memcpy(&buf[len], UDP_mdata_queue[queuec].data, sizeof(char)*UDP_mdata_queue[queuec].data_size);
																								len += UDP_mdata_queue[queuec].data_size;
					sendto (UDP_Socket[0], buf, len, 0, (struct sockaddr *)&Netgame.players[plc].protocol.udp.addr, sizeof(struct _sockaddr));
				}
				resend++;
			}
		}

		// Check if we can remove that packet due to to it had no resend's or Timeout
		if (!resend || ((UDP_mdata_queue[queuec].pkt_initial_timestamp + UDP_TIMEOUT <= time) || (time < UDP_mdata_queue[queuec].pkt_initial_timestamp)))
		{
			con_printf(CON_VERBOSE, "P#%i: Removing stored pkt_num %i - All ACK: %i\n",Player_num, UDP_mdata_queue[queuec].pkt_num, !resend);
			memset(&UDP_mdata_queue[queuec],0,sizeof(UDP_mdata_store));
		}

		if (resend)
			count++;
		
		// Only send 5 packets from the queue by each time the queue process is called
		if (count >= 5)
			break;
	}
}
/* CODE FOR PACKET LOSS PREVENTION - END */

void net_udp_send_mdata(int priority, fix time)
{
	ubyte buf[sizeof(UDP_mdata_info)];
	ubyte pack[MAX_PLAYERS];
	int len = 0, i = 0;
	
	if (!(Game_mode&GM_NETWORK))
		return;

	if (!(UDP_MData.mbuf_size > 0))
		return;

	if (!Netgame.PacketLossPrevention)
		priority = 0;

	memset(&buf, 0, sizeof(UDP_mdata_info));
	memset(&pack, 1, sizeof(ubyte)*MAX_PLAYERS);

	if (priority)
		buf[len] = UPID_MDATA_P1;
	else
		buf[len] = UPID_MDATA_P0;
																				len++;
	buf[len] = Player_num;														len++;
	if (priority)
	{
		UDP_MData.pkt_num++;
		PUT_INTEL_INT(buf + len, UDP_MData.pkt_num);							len += 4;
	}
	memcpy(&buf[len], UDP_MData.mbuf, sizeof(char)*UDP_MData.mbuf_size);		len += UDP_MData.mbuf_size;

	if (multi_i_am_master())
	{
		for (i = 1; i < MAX_PLAYERS; i++)
		{
			if (Players[i].connected == CONNECT_PLAYING)
			{
				sendto (UDP_Socket[0], buf, len, 0, (struct sockaddr *)&Netgame.players[i].protocol.udp.addr, sizeof(struct _sockaddr));
				pack[i] = 0;
			}
		}
	}
	else
	{
		sendto (UDP_Socket[0], buf, len, 0, (struct sockaddr *)&Netgame.players[0].protocol.udp.addr, sizeof(struct _sockaddr));
		pack[0] = 0;
	}
	
	if (priority)
		net_udp_noloss_add_queue_pkt(UDP_MData.pkt_num, time, UDP_MData.mbuf, UDP_MData.mbuf_size, Player_num, pack);

	// Clear UDP_MData except pkt_num. That one must not be deleted so we can clearly keep track of important packets.
	UDP_MData.type = 0;
	UDP_MData.Player_num = 0;
	UDP_MData.mbuf_size = 0;
	memset(&UDP_MData.mbuf, 0, sizeof(ubyte)*UPKT_MBUF_SIZE);
}

void net_udp_process_mdata (ubyte *data, int data_len, struct _sockaddr sender_addr, int priority)
{
	int pnum = data[1], dataoffset = (priority?6:2);

	// Check if packet might be bogus
	if ((pnum < 0) || (data_len > sizeof(UDP_mdata_info)))
		return;

	// Check if it came from valid IP
	if (multi_i_am_master())
	{
		if (memcmp((struct _sockaddr *)&sender_addr, (struct _sockaddr *)&Netgame.players[pnum].protocol.udp.addr, sizeof(struct _sockaddr)))
		{
			return;
		}
	}
	else
	{
		if (memcmp((struct _sockaddr *)&sender_addr, (struct _sockaddr *)&Netgame.players[0].protocol.udp.addr, sizeof(struct _sockaddr)))
		{
			return;
		}
	}

	// Add priority packet and check for possible redundancy
	if (priority)
	{
		if (!net_udp_noloss_validate_mdata(GET_INTEL_SHORT(&data[2]), pnum, sender_addr))
			return;
	}

	// send this to everyone else (if master)
	if (multi_i_am_master())
	{
		ubyte pack[MAX_PLAYERS];
		int i = 0;

		memset(&pack, 1, sizeof(ubyte)*MAX_PLAYERS);
		
		for (i = 1; i < MAX_PLAYERS; i++)
		{
			if ((i != pnum) && Players[i].connected == CONNECT_PLAYING)
			{
				sendto (UDP_Socket[0], data, data_len, 0, (struct sockaddr *)&Netgame.players[i].protocol.udp.addr, sizeof(struct _sockaddr));
				pack[i] = 0;
			}
		}

		if (priority && N_players > 2)
		{
			net_udp_noloss_add_queue_pkt(GET_INTEL_SHORT(&data[2]), timer_get_fixed_seconds(), data+dataoffset, data_len-dataoffset, pnum, pack);
		}
	}

	// Check if we are in correct state to process the packet
	if (!((Network_status == NETSTAT_PLAYING)||(Network_status == NETSTAT_ENDLEVEL) || Network_status==NETSTAT_WAITING))
		return;

	// Process
	if (Endlevel_sequence || (Network_status == NETSTAT_ENDLEVEL))
	{
		int old_Endlevel_sequence = Endlevel_sequence;
		Endlevel_sequence = 1;
		multi_process_bigdata( (char*)data+dataoffset, data_len-dataoffset);
		Endlevel_sequence = old_Endlevel_sequence;
		return;
	}

	multi_process_bigdata( (char*)data+dataoffset, data_len-dataoffset );
}

void net_udp_send_pdata()
{
	int i = 0, j = 0;

	if (multi_i_am_master())
	{
		ubyte buf[sizeof(UDP_frame_info)*MAX_PLAYERS];
		shortpos pos[MAX_PLAYERS];
		int len = 0;
		
		memset(&buf, 0, sizeof(UDP_frame_info)*MAX_PLAYERS);
		memset(&pos, 0, sizeof(shortpos)*MAX_PLAYERS);

		for (i = 0; i < MAX_PLAYERS; i++)
			if (Players[i].connected == CONNECT_PLAYING)
				create_shortpos(&pos[i], Objects+Players[i].objnum, 1);

		for (i = 1; i < MAX_PLAYERS; i++)
		{
			if (Players[i].connected != CONNECT_PLAYING)
				continue;
			
			len = 0;
			buf[len] = UPID_PDATA_H;										len++;
			for (j = 0; j < MAX_PLAYERS; j++)
			{
				if ((i == j) || (Players[j].connected != CONNECT_PLAYING))
				{
					buf[len] = j;											len++;
					buf[len] = Players[j].connected;						len++;
				}
				else
				{
					buf[len] = j;											len++;
					buf[len] = Players[j].connected;						len++;
					buf[len] = Objects[Players[j].objnum].render_type;		len++;
					memcpy(buf + len, &pos[j].bytemat, 9);					len += 9;
					PUT_INTEL_SHORT(&buf[len], pos[j].xo);					len += 2;
					PUT_INTEL_SHORT(&buf[len], pos[j].yo);					len += 2;
					PUT_INTEL_SHORT(&buf[len], pos[j].zo);					len += 2;
					PUT_INTEL_SHORT(&buf[len], pos[j].segment);				len += 2;
					PUT_INTEL_SHORT(&buf[len], pos[j].velx);				len += 2;
					PUT_INTEL_SHORT(&buf[len], pos[j].vely);				len += 2;
					PUT_INTEL_SHORT(&buf[len], pos[j].velz);				len += 2;
				}
			}

			sendto (UDP_Socket[0], buf, len, 0, (struct sockaddr *)&Netgame.players[i].protocol.udp.addr, sizeof(struct _sockaddr));
		}
	}
	else if (Players[Player_num].connected == CONNECT_PLAYING)
	{
		ubyte buf[sizeof(UDP_frame_info)];
		shortpos pos;
		int len = 0;
		
		memset(&buf, 0, sizeof(UDP_frame_info));
		
		buf[len] = UPID_PDATA_C;											len++;
		buf[len] = Player_num;												len++;
		buf[len] = Players[Player_num].connected;							len++;
		buf[len] = Objects[Players[j].objnum].render_type;					len++;
		memset(&pos, 0, sizeof(shortpos));
		create_shortpos(&pos, Objects+Players[Player_num].objnum, 1);
		memcpy(buf + len, &pos.bytemat, 9);									len += 9;
		PUT_INTEL_SHORT(&buf[len], pos.xo);									len += 2;
		PUT_INTEL_SHORT(&buf[len], pos.yo);									len += 2;
		PUT_INTEL_SHORT(&buf[len], pos.zo);									len += 2;
		PUT_INTEL_SHORT(&buf[len], pos.segment);							len += 2;
		PUT_INTEL_SHORT(&buf[len], pos.velx);								len += 2;
		PUT_INTEL_SHORT(&buf[len], pos.vely);								len += 2;
		PUT_INTEL_SHORT(&buf[len], pos.velz);								len += 2;

		sendto (UDP_Socket[0], buf, len, 0, (struct sockaddr *)&Netgame.players[0].protocol.udp.addr, sizeof(struct _sockaddr));
	}
}

void net_udp_process_pdata ( ubyte *data, int data_len, struct _sockaddr sender_addr )
{
	int len = 0, i = 0;

	if (!(Game_mode & GM_NETWORK))
		return;

	len++;

	if (multi_i_am_master())
	{
		UDP_frame_info pd;

		memset(&pd, 0, sizeof(UDP_frame_info));

		if (data_len > sizeof(UDP_frame_info))
			return;

		if (memcmp((struct _sockaddr *)&sender_addr, (struct _sockaddr *)&Netgame.players[data[len]].protocol.udp.addr, sizeof(struct _sockaddr)))
			return;

		pd.Player_num = data[len];												len++;
		pd.connected = data[len];												len++;
		pd.obj_render_type = data[len];											len++;
		memcpy(&pd.pos.bytemat, &(data[len]), 9);								len += 9;
		pd.pos.xo = GET_INTEL_SHORT(&data[len]);								len += 2;
		pd.pos.yo = GET_INTEL_SHORT(&data[len]);								len += 2;
		pd.pos.zo = GET_INTEL_SHORT(&data[len]);								len += 2;
		pd.pos.segment = GET_INTEL_SHORT(&data[len]);							len += 2;
		pd.pos.velx = GET_INTEL_SHORT(&data[len]);								len += 2;
		pd.pos.vely = GET_INTEL_SHORT(&data[len]);								len += 2;
		pd.pos.velz = GET_INTEL_SHORT(&data[len]);								len += 2;

		if ((Game_mode&GM_NETWORK) && ((Network_status == NETSTAT_PLAYING)||(Network_status == NETSTAT_ENDLEVEL) || Network_status==NETSTAT_WAITING))
			net_udp_read_pdata_short_packet (&pd);
	}
	else
	{
		fix time = timer_get_fixed_seconds();
		
		if (data_len > (sizeof(UDP_frame_info)*(MAX_PLAYERS-1)))
			return;

		if (memcmp((struct _sockaddr *)&sender_addr, (struct _sockaddr *)&Netgame.players[0].protocol.udp.addr, sizeof(struct _sockaddr)))
			return;

		for (i = 0; i < MAX_PLAYERS; i++)
		{
			UDP_frame_info pd;

			memset(&pd, 0, sizeof(UDP_frame_info));

			pd.Player_num = i;													len++;
			pd.connected = data[len];											len++;
			
			if ((i == Player_num) || (pd.connected != CONNECT_PLAYING))
			{
				if (pd.connected == CONNECT_DISCONNECTED)
				{
					Netgame.players[i].LastPacketTime = time - UDP_TIMEOUT;
					net_udp_timeout_check(time);
				}
				continue;
			}

			pd.obj_render_type = data[len];										len++;
			memcpy(&pd.pos.bytemat, &(data[len]), 9);							len += 9;
			pd.pos.xo = GET_INTEL_SHORT(&data[len]);							len += 2;
			pd.pos.yo = GET_INTEL_SHORT(&data[len]);							len += 2;
			pd.pos.zo = GET_INTEL_SHORT(&data[len]);							len += 2;
			pd.pos.segment = GET_INTEL_SHORT(&data[len]);						len += 2;
			pd.pos.velx = GET_INTEL_SHORT(&data[len]);							len += 2;
			pd.pos.vely = GET_INTEL_SHORT(&data[len]);							len += 2;
			pd.pos.velz = GET_INTEL_SHORT(&data[len]);							len += 2;

			if ((Game_mode&GM_NETWORK) && ((Network_status == NETSTAT_PLAYING)||(Network_status == NETSTAT_ENDLEVEL) || Network_status==NETSTAT_WAITING))
				net_udp_read_pdata_short_packet (&pd);
		}
	}
}

void net_udp_read_pdata_short_packet(UDP_frame_info *pd)
{
	int TheirPlayernum;
	int TheirObjnum;
	object * TheirObj = NULL;

	TheirPlayernum = pd->Player_num;
	TheirObjnum = Players[pd->Player_num].objnum;

	if (!multi_quit_game && (TheirPlayernum >= N_players))
	{
		if (Network_status!=NETSTAT_WAITING)
		{
			Int3(); // We missed an important packet!
			multi_consistency_error(0);
			return;
		}
		else
			return;
	}

	TheirObj = &Objects[TheirObjnum];
	Netgame.players[TheirPlayernum].LastPacketTime = timer_get_fixed_seconds();

	//------------ Read the player's ship's object info ----------------------

	extract_shortpos(TheirObj, &pd->pos, 0);

	if (TheirObj->movement_type == MT_PHYSICS)
		set_thrust_from_velocity(TheirObj);

	//------------ Welcome them back if reconnecting --------------
	if (!Players[TheirPlayernum].connected) {
		Players[TheirPlayernum].connected = CONNECT_PLAYING;

		if (Newdemo_state == ND_STATE_RECORDING)
			newdemo_record_multi_reconnect(TheirPlayernum);

		digi_play_sample( SOUND_HUD_MESSAGE, F1_0);
		
		HUD_init_message( "'%s' %s", Players[TheirPlayernum].callsign, TXT_REJOIN );

		multi_send_score();

		net_udp_noloss_clear_mdata_got(TheirPlayernum);
	}
}

// Send the ping list in regular intervals
void net_udp_ping_frame(fix time)
{
	static fix PingTime = 0;
	
	if (!multi_i_am_master())
		return;
	
	if (((PingTime + F1_0) < time) || (PingTime > time))
	{
		ubyte buf[UPKT_PING_SIZE];
		int len = 0, i = 0;
		
		memset(&buf, 0, sizeof(ubyte)*UPKT_PING_SIZE);
		buf[len] = UPID_PING;										len++;
		PUT_INTEL_INT(buf + len, time);								len += 4;
		for (i = 1; i < MAX_PLAYERS; i++)
		{
			PUT_INTEL_INT(buf + len, Netgame.players[i].ping);		len += 4;
		}
		
		for (i = 1; i < MAX_PLAYERS; i++)
		{
			if (Players[i].connected == CONNECT_DISCONNECTED)
				continue;
			sendto (UDP_Socket[0], buf, sizeof(buf), 0, (struct sockaddr *)&Netgame.players[i].protocol.udp.addr, sizeof(struct _sockaddr));
		}
		PingTime = time;
	}
}

// Got a PING from host. Apply the pings to our players and respond to host.
void net_udp_process_ping(ubyte *data, int data_len, struct _sockaddr sender_addr)
{
	fix host_ping_time = 0;
	ubyte buf[UPKT_PONG_SIZE];
	int i, len = 0;
	
	if (data_len != UPKT_PING_SIZE)
		return;
	
	if (memcmp((struct _sockaddr *)&sender_addr, (struct _sockaddr *)&Netgame.players[0].protocol.udp.addr, sizeof(struct _sockaddr)))
		return;
		
																	len++; // Skip UPID byte;
	host_ping_time = GET_INTEL_INT(&(data[len]));					len += 4;
	for (i = 1; i < MAX_PLAYERS; i++)
	{
		Netgame.players[i].ping = GET_INTEL_INT(&(data[len]));		len += 4;
	}
	
	buf[0] = UPID_PONG;
	buf[1] = Player_num;
	PUT_INTEL_INT(buf + 2, host_ping_time);

	sendto (UDP_Socket[0], buf, sizeof(buf), 0, (struct sockaddr *)&sender_addr, sizeof(struct _sockaddr));
}

// Got a PONG from a client. Check the time and add it to our players.
void net_udp_process_pong(ubyte *data, int data_len, struct _sockaddr sender_addr)
{
	fix client_pong_time = 0;
	int i = 0;
	
	if (data_len != UPKT_PONG_SIZE)
		return;

	if (data[1] >= MAX_PLAYERS || data[1] < 1)
		return;
	
	if (memcmp((struct _sockaddr *)&sender_addr, (struct _sockaddr *)&Netgame.players[data[1]].protocol.udp.addr, sizeof(struct _sockaddr)))
		return;
	
	if (i == MAX_PLAYERS)
		return;

	memcpy(&client_pong_time, &(data[2]), 4);
	Netgame.players[data[1]].ping = f2i(fixmul(timer_get_fixed_seconds() - INTEL_INT(client_pong_time),i2f(1000)));

	if (Netgame.players[data[1]].ping < 0)
		Netgame.players[data[1]].ping = 0;
		
	if (Netgame.players[data[1]].ping > 9999)
		Netgame.players[data[1]].ping = 9999;
}

void net_udp_do_refuse_stuff (UDP_sequence_packet *their)
{
	int i,new_player_num;
	
	for (i=0;i<MAX_PLAYERS;i++)
	{
		if (!strcmp (their->player.callsign,Players[i].callsign))
		{
			net_udp_welcome_player(their);
			return;
		}
	}

	if (!WaitForRefuseAnswer)
	{
		for (i=0;i<MAX_PLAYERS;i++)
		{
			if (!strcmp (their->player.callsign,Players[i].callsign))
			{
				net_udp_welcome_player(their);
				return;
			}
		}
	
		digi_play_sample (SOUND_CONTROL_CENTER_WARNING_SIREN,F1_0*2);
	
		if (Game_mode & GM_TEAM)
		{
			HUD_init_message ("%s wants to join",their->player.callsign);
			HUD_init_message ("Alt-1 assigns to team %s. Alt-2 to team %s",their->player.callsign,Netgame.team_name[0],Netgame.team_name[1]);
		}
		else
		{
			HUD_init_message ("%s wants to join (accept: F6)",their->player.callsign);
		}
	
		strcpy (RefusePlayerName,their->player.callsign);
		RefuseTimeLimit=timer_get_fixed_seconds();
		RefuseThisPlayer=0;
		WaitForRefuseAnswer=1;
	}
	else
	{
		for (i=0;i<MAX_PLAYERS;i++)
		{
			if (!strcmp (their->player.callsign,Players[i].callsign))
			{
				net_udp_welcome_player(their);
				return;
			}
		}
	
		if (strcmp(their->player.callsign,RefusePlayerName))
			return;
	
		if (RefuseThisPlayer)
		{
			RefuseTimeLimit=0;
			RefuseThisPlayer=0;
			WaitForRefuseAnswer=0;
			if (Game_mode & GM_TEAM)
			{
				new_player_num=net_udp_get_new_player_num (their);
	
				Assert (RefuseTeam==1 || RefuseTeam==2);        
			
				if (RefuseTeam==1)      
					Netgame.team_vector &=(~(1<<new_player_num));
				else
					Netgame.team_vector |=(1<<new_player_num);
				net_udp_welcome_player(their);
				net_udp_send_netgame_update();
			}
			else
			{
				net_udp_welcome_player(their);
			}
			return;
		}

		if ((timer_get_fixed_seconds()) > RefuseTimeLimit+REFUSE_INTERVAL)
		{
			RefuseTimeLimit=0;
			RefuseThisPlayer=0;
			WaitForRefuseAnswer=0;
			if (!strcmp (their->player.callsign,RefusePlayerName))
			{
				net_udp_dump_player(their->player.protocol.udp.addr, DUMP_DORK);
			}
			return;
		}
	}
}

int net_udp_get_new_player_num (UDP_sequence_packet *their)
  {
	 int i;
	
	 their=their;
	
		if ( N_players < MaxNumNetPlayers)
			return (N_players);
		
		else
		{
			// Slots are full but game is open, see if anyone is
			// disconnected and replace the oldest player with this new one
		
			int oldest_player = -1;
			fix oldest_time = timer_get_fixed_seconds();

			Assert(N_players == MaxNumNetPlayers);

			for (i = 0; i < N_players; i++)
			{
				if ( (!Players[i].connected) && (Netgame.players[i].LastPacketTime < oldest_time))
				{
					oldest_time = Netgame.players[i].LastPacketTime;
					oldest_player = i;
				}
			}
	    return (oldest_player);
	  }
  }

void net_udp_show_game_rules()
{
	int done,k;
	grs_canvas canvas;
	int w = FSPACX(280), h = FSPACY(130);

	gr_set_current_canvas(NULL);

	gr_init_sub_canvas(&canvas, &grd_curscreen->sc_canvas, (SWIDTH - FSPACX(320))/2, (SHEIGHT - FSPACY(200))/2, FSPACX(320), FSPACY(200));

	game_flush_inputs();

	done = 0;

	while(!done)	{
		timer_delay2(50);
		gr_set_current_canvas(NULL);
#ifdef OGL
		gr_flip();
		nm_draw_background1(NULL);
#endif
		nm_draw_background(((SWIDTH-w)/2)-BORDERX,((SHEIGHT-h)/2)-BORDERY,((SWIDTH-w)/2)+w+BORDERX,((SHEIGHT-h)/2)+h+BORDERY);

		gr_set_current_canvas(&canvas);

		gr_set_fontcolor(gr_find_closest_color_current(29,29,47),-1);
		grd_curcanv->cv_font = MEDIUM3_FONT;
	
		gr_string( 0x8000, FSPACY(35), "NETGAME INFO" );
	
		grd_curcanv->cv_font = GAME_FONT;
	

		gr_printf( FSPACX( 25),FSPACY( 55), "Reactor Life:");
		gr_printf( FSPACX( 25),FSPACY( 67), "Pakets per second:");
		gr_printf( FSPACX( 25),FSPACY( 73), "Show All Players On Automap:");

		gr_printf( FSPACX( 25),FSPACY(100), "Allowed Objects");
		gr_printf( FSPACX( 25),FSPACY(110), "Laser Upgrade:");
		gr_printf( FSPACX( 25),FSPACY(116), "Quad Laser:");
		gr_printf( FSPACX( 25),FSPACY(122), "Vulcan Cannon:");
		gr_printf( FSPACX( 25),FSPACY(128), "Spreadfire Cannon:");
		gr_printf( FSPACX( 25),FSPACY(134), "Plasma Cannon:");
		gr_printf( FSPACX( 25),FSPACY(140), "Fusion Cannon:");
		gr_printf( FSPACX(170),FSPACY(110), "Homing Missile:");
		gr_printf( FSPACX(170),FSPACY(116), "Proximity Bomb:");
		gr_printf( FSPACX(170),FSPACY(122), "Smart Missile:");
		gr_printf( FSPACX(170),FSPACY(128), "Mega Missile:");
		gr_printf( FSPACX( 25),FSPACY(150), "Invulnerability:");
		gr_printf( FSPACX( 25),FSPACY(156), "Cloak:");

		gr_set_fontcolor(gr_find_closest_color_current(255,255,255),-1);
		gr_printf( FSPACX(170),FSPACY( 55), "%i Min", Netgame.control_invul_time/F1_0/60);
		gr_printf( FSPACX(170),FSPACY( 67), "%i", Netgame.PacketsPerSec);
		gr_printf( FSPACX(170),FSPACY( 73), Netgame.game_flags&NETGAME_FLAG_SHOW_MAP?"ON":"OFF");


		gr_printf( FSPACX(130),FSPACY(110), Netgame.AllowedItems&NETFLAG_DOLASER?"YES":"NO");
		gr_printf( FSPACX(130),FSPACY(116), Netgame.AllowedItems&NETFLAG_DOQUAD?"YES":"NO");
		gr_printf( FSPACX(130),FSPACY(122), Netgame.AllowedItems&NETFLAG_DOVULCAN?"YES":"NO");
		gr_printf( FSPACX(130),FSPACY(128), Netgame.AllowedItems&NETFLAG_DOSPREAD?"YES":"NO");
		gr_printf( FSPACX(130),FSPACY(134), Netgame.AllowedItems&NETFLAG_DOPLASMA?"YES":"NO");
		gr_printf( FSPACX(130),FSPACY(140), Netgame.AllowedItems&NETFLAG_DOFUSION?"YES":"NO");
		gr_printf( FSPACX(275),FSPACY(110), Netgame.AllowedItems&NETFLAG_DOHOMING?"YES":"NO");
		gr_printf( FSPACX(275),FSPACY(116), Netgame.AllowedItems&NETFLAG_DOPROXIM?"YES":"NO");
		gr_printf( FSPACX(275),FSPACY(122), Netgame.AllowedItems&NETFLAG_DOSMART?"YES":"NO");
		gr_printf( FSPACX(275),FSPACY(128), Netgame.AllowedItems&NETFLAG_DOMEGA?"YES":"NO");
		gr_printf( FSPACX(130),FSPACY(150), Netgame.AllowedItems&NETFLAG_DOINVUL?"YES":"NO");
		gr_printf( FSPACX(130),FSPACY(156), Netgame.AllowedItems&NETFLAG_DOCLOAK?"YES":"NO");

		//see if redbook song needs to be restarted
		RBACheckFinishedHook();
		
		k = key_inkey();
		switch( k )	{
			case KEY_PRINT_SCREEN:
				save_screen_shot(0); k = 0;
				break;
			case KEY_ENTER:
			case KEY_SPACEBAR:
			case KEY_ESC:
				done=1;
				break;
		}
	}

	gr_set_current_canvas(NULL);
	game_flush_inputs();
}

int net_udp_show_game_info()
{
	char rinfo[512],*info=rinfo;
	char *NetworkModeNames[]={"Anarchy","Team Anarchy","Robo Anarchy","Cooperative","Unknown"};
	int c;

	memset(info,0,sizeof(char)*256);

	info+=sprintf(info,"\nConnected to\n\"%s\"\n",Netgame.game_name);

	if(!Netgame.mission_title)
		info+=sprintf(info,"Descent: First Strike");
	else
		info+=sprintf(info,Netgame.mission_title);

   if( Netgame.levelnum >= 0 )
   {
	   info+=sprintf (info," - Lvl %i",Netgame.levelnum);
   }
   else
   {
      info+=sprintf (info," - Lvl S%i",(Netgame.levelnum*-1));
   }

	info+=sprintf (info,"\n\nDifficulty: %s",MENU_DIFFICULTY_TEXT(Netgame.difficulty));
	info+=sprintf (info,"\nGame Mode: %s",NetworkModeNames[Netgame.gamemode]);
	info+=sprintf (info,"\nPlayers: %i/%i",Netgame.numplayers,Netgame.max_numplayers);

	while (1){
		c=nm_messagebox("WELCOME", 2, "JOIN GAME", "GAME INFO", rinfo);
		if (c==0)
			return 1;
		else if (c==1)
			net_udp_show_game_rules();
		else
			return 0;
	}
}
#endif
