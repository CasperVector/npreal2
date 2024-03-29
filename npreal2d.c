/*
 *	Copyright (C) 2001  Moxa Inc.
 *	All rights reserved.
 *
 *	Moxa NPort/Async Server UNIX Real TTY daemon program.
 *
 *	Usage: npreal2d [-t reset-time]
 *
 *	Compilation instructions:
 *		LINUX:	cc -O -o npreal2d npreal2d.c
 *
 */

#include	"np_ver.h"
#include	<sys/types.h>
#include	<sys/stat.h>
#include	<sys/socket.h>
#include	<sys/time.h>
#include	<sys/param.h>
#include	<netinet/in.h>
#include	<netdb.h>
#include	<stdlib.h>
#include	<stdio.h>
#include	<errno.h>
#include	<time.h>
#include	<string.h>
#include	<unistd.h>
#include	<fcntl.h>
#include	<signal.h>
#include	<syslog.h>
#include	<sys/ioctl.h>
#include	<sys/sysinfo.h>
#ifdef	STREAM
#include	<sys/ptms.h>
#endif
#ifdef	SSL_ON
#include	<openssl/ssl.h>
#endif

#include	<resolv.h> // for res_init()
#include	<arpa/inet.h>
#include	"npreal2d.h"

/* The mode which daemon will be waken up */
int		Graw_mode = 0;
int		Gredund_mode = 0;

int		pipefd[2];
int		maxfd;
int     timeout_time = 0;
int		polling_time=0; 	/* default disable polling function */
#ifdef	STREAM
extern	char	*ptsname();
#endif

int	g_tcp_wait_id = 0;
//static char mm[128];

int	moxattyd_read_config(char *cfgpath);
void ConnectCheck();
void CloseTcp(TTYINFO *infop);
void ConnectTcp(TTYINFO *infop);
void OpenTcpSocket(TTYINFO *infop);
void OpenTty(TTYINFO *infop);
void moxattyd_handle_ttys();
void poll_nport_send(SERVINFO *servp);
void poll_async_server_recv();
void poll_async_server_send(SERVINFO *servp);
int CheckConnecting();

#ifdef SSL_ON
void ConnectSSL( TTYINFO *infop );
#endif

/*
 *	MOXA TTY daemon main program
 */
int main(argc, argv)
int	argc;
char *	argv[];
{
	char ver[100];
	char *cfgpath = "/etc/npreal2d.cf";
	int		i;

	for(i=0; i<2; i++)
		polling_nport_fd[i] = -1;

	/*
	 * Read the poll time & the pesudo TTYs configuation file.
	 */
	for (i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "-t")) {
			if ((++i) >= argc) { fprintf (stderr, "Usage error\n"); return 1; }
			timeout_time = 60 * atoi(argv[i]);
			polling_time = timeout_time;
			if (polling_time >= 60) polling_time = (polling_time - 20) / 4;
		} else if (!strcmp(argv[i], "-c")) {
			if ((++i) >= argc) { fprintf (stderr, "Usage error\n"); return 1; }
			cfgpath = argv[i];
		} else if (!strcmp(argv[i], "-f")) {
			Gfglog_mode = 1;
		} else { fprintf (stderr, "Usage error\n"); return 1; }
	}

	if (moxattyd_read_config(cfgpath) <= 0)
	{
		//sprintf(mm, "logger \"CFD>(%d, %s) Read CFG Error.\"", __LINE__, __FUNCTION__);
		//system(mm);

		fprintf (stderr, "Not any tty defined\n");
		return 1;
		//			usleep(1000);
		//			continue;
	}

	/*
	 * Initialize this Moxa TTYs daemon process.
	 */
	if (!Gfglog_mode) {
		openlog ("npreal2d", LOG_PID, LOG_DAEMON);
		if (daemon (0, 0)) {
			log_event ("Failed to daemonize");
			return -1;
		}
	}

	/*
	 * Initialize polling NPort and Async Server function.
	 */
	while ( polling_time && (poll_async_server_init() < 0) ) { }

	/*
	 * Open PIPE, set read to O_NDELAY mode.
	 */
	if ( pipe(pipefd) < 0 )
	{
		log_event("pipe error !");
		return -1;
	}
#ifdef	O_NDELAY
	fcntl(pipefd[0], F_SETFL, fcntl(pipefd[0], F_GETFL) | O_NDELAY);
#endif
	signal(SIGCLD, SIG_IGN);

	sprintf(ver, "MOXA Real TTY daemon program starting (%s %s)...", NPREAL_VERSION, NPREAL_BUILD);
	log_event(ver);

#ifdef SSL_ON
	ssl_init();
#endif

	// Main loop
	while (1)
	{
		/*
		 * Handle Moxa TTYs data communication.
		 */
		if (Graw_mode)
			moxattyd_handle_ttys(); /* child process ok */
		else
			return -1;
	} /* while (1) */
}

int resolve_dns_host_name(infop)
TTYINFO *infop;
{
	int ret;
	struct addrinfo *result = NULL, *rp;
	struct addrinfo hints;
	struct sockaddr_in *ipv4;
	struct sockaddr_in6 *ipv6;
	char msg[255]={0};
	ulong addr_ipv4;
	u_char addr_ipv6[16];

	if( ipv4_str_to_ip(infop->ip_addr_s, &addr_ipv4)==NP_RET_SUCCESS ){
		*(ulong*)infop->ip6_addr = addr_ipv4;
		infop->af = AF_INET;
		return NP_RET_SUCCESS;
	} else if(ipv6_str_to_ip(infop->ip_addr_s, addr_ipv6)==NP_RET_SUCCESS )	{
		memcpy(infop->ip6_addr, addr_ipv6, sizeof(infop->ip6_addr));
		infop->af = AF_INET6;
		return NP_RET_SUCCESS;
	}

	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC;    /* Allow IPv4 or IPv6 */
	hints.ai_socktype = SOCK_STREAM; /* Datagram socket */
	hints.ai_flags = AI_PASSIVE;    /* For wildcard IP address */

	ret = getaddrinfo(infop->ip_addr_s, NULL, &hints, &result);
	if( ret==EAI_AGAIN || ret==EAI_NONAME ){
		// Sometimes, this error occurred. It means DNS server or DNS configuration are wrong temporarily.
		sleep(1);
		res_init(); // init name resolver again!

		ret = getaddrinfo(infop->ip_addr_s, NULL, &hints, &result);
		if (ret != 0) {
			sprintf(msg, "getaddrinfo: %s @ %d, %s\n", gai_strerror(ret), __LINE__, __FUNCTION__);
			log_event(msg);
			return NP_RET_ERROR;
		}
	}

	memset(infop->ip6_addr, 0, sizeof(infop->ip6_addr));

	for (rp = result; rp != NULL; rp = rp->ai_next) {

		if(rp->ai_family == AF_INET)
		{
			ipv4 = (struct sockaddr_in *)rp->ai_addr;
			*(ulong*)infop->ip6_addr = ((struct in_addr *)&ipv4->sin_addr)->s_addr;
			infop->af = AF_INET;
			// IPv4 address is translated.

			//{
			//	char ipAddress[INET_ADDRSTRLEN];
			//	inet_ntop(AF_INET, &(ipv4->sin_addr), ipAddress, INET_ADDRSTRLEN);
			//	sprintf(msg, "ipAddress: %s @ %d, %s\n", ipAddress, __LINE__, __FUNCTION__);
			//	log_event(msg);
			//}
			break;

		} else if(rp->ai_family == AF_INET6)
		{
			ipv6 = (struct sockaddr_in6 *)rp->ai_addr;
			memcpy(infop->ip6_addr, ((struct in6_addr *)&ipv6->sin6_addr)->s6_addr, 16);
			infop->af = AF_INET6;

			//{
			//	char ipAddress[INET6_ADDRSTRLEN];
			//	inet_ntop(AF_INET6, &(ipv6->sin6_addr), ipAddress, INET6_ADDRSTRLEN);
			//	sprintf(msg, "ipAddress: %s @ %d, %s\n", ipAddress, __LINE__, __FUNCTION__);
			//	log_event(msg);
			//}
			break;
		}
	}
	freeaddrinfo(result);           /* No longer needed */

	if (rp == NULL) {               /* No address succeeded */
		sprintf(msg, "No available host is found. @ %d, %s\n", __LINE__, __FUNCTION__);
		log_event(msg);
		return -1;
	}

	if (infop->redundant_mode) {

		if( ipv4_str_to_ip(infop->ip_addr_s, &addr_ipv4)==NP_RET_SUCCESS ){
			*(ulong*)infop->redund.redund_ip = addr_ipv4;
			return NP_RET_SUCCESS;
		} else if(ipv6_str_to_ip(infop->ip_addr_s, addr_ipv6)==NP_RET_SUCCESS )	{
			memcpy(infop->redund.ip6_addr, addr_ipv6, sizeof(infop->redund.ip6_addr));
			return NP_RET_SUCCESS;
		}

		hints.ai_family = AF_INET;    /* Allow IPv4 or IPv6 */

		ret = getaddrinfo(infop->ip_addr_s, NULL, &hints, &result);
		if( ret==EAI_AGAIN || ret==EAI_NONAME ){
			// Sometimes, this error occurred. It means DNS server or DNS configuration are wrong temporarily.
			sleep(1);
			res_init(); // init name resolver again!

			ret = getaddrinfo(infop->ip_addr_s, NULL, &hints, &result);
			if (ret != 0) {
				sprintf(msg, "getaddrinfo: %s @ %d, %s\n", gai_strerror(ret), __LINE__, __FUNCTION__);
				log_event(msg);
				return NP_RET_ERROR;
			}
		}

		*(ulong*)infop->redund.redund_ip = 0;
		memset(infop->redund.ip6_addr, 0, sizeof(infop->redund.ip6_addr));

		for (rp = result; rp != NULL; rp = rp->ai_next) {

			if(rp->ai_family == AF_INET)
			{
				ipv4 = (struct sockaddr_in *)rp->ai_addr;
				*(ulong*)infop->redund.redund_ip = ((struct in_addr *)&ipv4->sin_addr)->s_addr;
				infop->af = AF_INET;

				// IPv4 address is translated.

				//{
				//	char ipAddress[INET_ADDRSTRLEN];
				//	inet_ntop(AF_INET, &(ipv4->sin_addr), ipAddress, INET_ADDRSTRLEN);
				//	sprintf(msg, "ipAddress: %s @ %d, %s\n", ipAddress, __LINE__, __FUNCTION__);
				//	log_event(msg);
				//}
				break;
			} else if(rp->ai_family == AF_INET6)
			{
				ipv6 = (struct sockaddr_in6 *)rp->ai_addr;
				memcpy(infop->redund.ip6_addr, ((struct in6_addr *)&ipv6->sin6_addr)->s6_addr, 16);
				infop->af = AF_INET6;

				//{
				//	char ipAddress[INET6_ADDRSTRLEN];
				//	inet_ntop(AF_INET6, &(ipv6->sin6_addr), ipAddress, INET6_ADDRSTRLEN);
				//	sprintf(msg, "ipAddress: %s @ %d, %s\n", ipAddress, __LINE__, __FUNCTION__);
				//	log_event(msg);
				//}
				break;
			}
		}

		if (rp == NULL) {               /* No address succeeded */
			sprintf(msg, "No available host is found. @ %d, %s\n", __LINE__, __FUNCTION__);
			log_event(msg);
			return -1;
		}

		freeaddrinfo(result);           /* No longer needed */
	}

	return 0;
}

/*
 *	Prepare LOG file and read the config TTY records.
 *
 */
int	moxattyd_read_config(cfgpath)
char *	cfgpath;
{
	int		n, data, cmd;
	FILE *		ConfigFd;
	TTYINFO *	infop;
	char		buf[160];
	char		ttyname[160],tcpport[16],cmdport[16];
	char		ttyname2[160], curname[160], scope_id[10];
	int			redundant_mode;
	int32_t		server_type,disable_fifo;
#ifdef SSL_ON
	int32_t		ssl_enable;
#else
	int32_t		temp;
#endif
	char		tmpstr[256];
	char		ip_addr[40];
	char		redund_ip[40];

	redundant_mode = 0;

	/*
	 * Prepare the full-path file names of LOG/Configuration.
	 */
	sprintf(buf, cfgpath);        /* Config file name */

	/*
	 * Open configuration file:
	 */
	ConfigFd = fopen(buf, "r");
	if ( ConfigFd == NULL )
	{
		log_event("Can't open configuration file (npreal2d.cf) !");
		return(-1);			/* Can't open file ! */
	}

	/*
	 * old configuration file format.
	 *"Device Id" "Server IP addr/Name" "data_port" "cmd_port" "Server Type"
	 * ex:
	 *  0	       192.168.1.1	950	966 2500
	 *	1	       tty_server	951	967 303
	 *	2	       192.168.1.1	950	966 311
	 *
	 *
	 * Read configuration & the data format of every data line is :
	 * [Minor] [ServerIP]	   [Data] [Cmd] [FIFO] [ttyName] [coutName]
	 *  0      192.168.1.1     950    966   1      ttyr00    cur00
	 *  1      192.168.1.1     951    967   1      ttyr01    cur01
	 *  2      192.168.1.1     952    968   1      ttyr02    cur02
	 *
	 * Security data format
	 * [Minor] [ServerIP]	             [Data] [Cmd] [FIFO] [SSL] [ttyName] [coutName] [interface]
	 *  0      192.168.1.1               950    966   1      0     ttyr00    cur00
	 *  1      192.168.1.1               951    967   1      0     ttyr01    cur01
	 *  2      192.168.1.1               952    968   1      1     ttyr02    cur02
	 *  3      fe80::216:d4ff:fe80:63e6  950    966   1      0     ttyr03    cur03       eth0
	 */
	ttys = 0;
	infop = ttys_info;
	while ( ttys < MAX_TTYS )
	{
		if ( fgets(buf, sizeof(buf), ConfigFd) == NULL )
			break;				/* end of file */

		server_type = disable_fifo = 0;

#ifdef SSL_ON
		ssl_enable = 0;
#endif

		n = sscanf(buf, "%s%s%s%s%d%d%s%s%s%d%s",
				ttyname,
				ip_addr,
				tcpport,
				cmdport,
				&disable_fifo,
#ifdef SSL_ON
				&ssl_enable,
#else
				&temp,
#endif
				ttyname2,
				curname,
				scope_id,
				&redundant_mode,
				redund_ip);

		if(n != 10 && n != 11)
		{
			continue;
		}

#if 0
		n = sscanf(buf, "%s%s%s%s%d%s%s%s",
				ttyname,
				infop->ip_addr_s,
				tcpport,
				cmdport,
				&disable_fifo,
				ttyname2,
				curname,
				scope_id);

		if(n != 7 && n != 8)
		{
			continue;
		}
#endif

		if (ttyname[0]=='#')
			continue;

		// Ignore to update data from npreal2d.cf if static_param is set.
		sprintf(tmpstr,"/proc/npreal2/%s",ttyname);
		while( infop->static_param ){
			if( strcmp(infop->mpt_name, tmpstr)==0 &&
				strcmp(infop->ip_addr_s, ip_addr)==0 &&
				infop->tcp_port == atoi(tcpport) )
			{
				//sprintf(mm, "logger \"CFD>(%d, %s) Same CFG is read(%s).\"", __LINE__, __FUNCTION__, infop->mpt_name);
				//system(mm);
				break;;
			}

			infop++;
			ttys++;
			if( ttys>=MAX_TTYS ){
				log_event("Out of memory for ttys!");
				fclose(ConfigFd);
				return (-1);
			}
		}

		if( infop->static_param ){
			//This configuration line is duplicate and should be ignored.
			infop++;
			ttys++;
			continue;
		}

		//sprintf(mm, "logger \"CFD>(%d, %s) New CFG is read(%s).\"", __LINE__, __FUNCTION__, infop->mpt_name);
		//system(mm);

		strcpy( infop->ip_addr_s, ip_addr );
		infop->redundant_mode = redundant_mode;
		strcpy( infop->redund.redund_ip, redund_ip );

		memset(&infop->redund, 0, sizeof(struct redund_struct));

		Graw_mode = 1;
		Gredund_mode = 1;

		/* in npreal2d.cf, [FIFO] is set to 1 if user is tend to */
		/* enable fifo, so the value of disable_fifo must be set to 0*/
		/* vice versa */
		if (disable_fifo == 1)
		{
			disable_fifo = 0;
		}
		else
		{
			disable_fifo = 1;
		}

		//server_type = CN2500;

		sprintf(tmpstr, "/proc/npreal2/%s", ttyname);
		memset(infop->mpt_name, 0, sizeof(infop->mpt_name));
		memcpy(infop->mpt_name, tmpstr, sizeof(infop->mpt_name)-1); 

		resolve_dns_host_name(infop);

		if ( (data = atoi(tcpport)) <= 0 || data >= 10000 )
			continue;
		if ( (cmd = atoi(cmdport)) <= 0 || cmd >= 10000 )
			continue;

		if((strncmp(infop->ip_addr_s, "fe80", 4) == 0) || (strncmp(infop->ip_addr_s, "FE80", 4) == 0))
		{
			if(strlen(scope_id) == 0)
			{
				break;
			}
			strcpy(infop->scope_id, scope_id);
		}
		else
		{
			memset(infop->scope_id, 0, 10);
		}
		infop->tcp_port = data;
		infop->cmd_port = cmd;
		infop->mpt_fd = -1;
		infop->sock_fd = -1;
		infop->sock_cmd_fd = -1;
		infop->state = STATE_INIT;
		infop->mpt_bufptr = (char *)malloc(BUFFER_SIZE * 2);
		if ( infop->mpt_bufptr == (char *)NULL )
		{
			log_event("Alocate memory fail !");
			break;
		}
		infop->sock_bufptr = infop->mpt_bufptr + BUFFER_SIZE;
		infop->alive_check_cnt = 0;
		infop->mpt_datakeep = 0;
		infop->mpt_dataofs = 0;
		infop->mpt_cmdkeep = 0;
		infop->sock_datakeep = 0;
		infop->sock_dataofs = 0;
		infop->sock_cmdkeep = 0;
		infop->error_flags = 0;
		strcpy(infop->ttyname, ttyname);
		strcpy(infop->ttyname2, ttyname2);
		strcpy(infop->curname, curname);
		infop->static_param = 0;
		infop->server_type = server_type;
		infop->disable_fifo = disable_fifo;
		infop->tcp_wait_id = g_tcp_wait_id;
		infop->tty_used_timestamp = 0;
		infop->first_servertime = 0;
#ifdef	SSL_ON
		infop->pssl = NULL;
		infop->ssl_enable = ssl_enable;
#endif
		infop++;
		ttys++;
	}

	/*
	 * Close configuration file:
	 */
	fclose(ConfigFd);
	if ( ttys == 0 )
		log_event("Have no any TTY configured record !");
	return(ttys);
}

void poll_async_server_send(servp)
SERVINFO	*servp;
{
	struct sockaddr_in	to;
	int			len;
	unsigned char		msg[32];
	struct sysinfo		sys_info;
	//printf("[AP] poll_async_server_send\n");
	if (servp->ap_id)
		return;
#ifndef	STREAM
	bzero(msg, 28);
#endif
#ifdef	STREAM
	memset (msg, 0, 28);
#endif
	sysinfo(&sys_info);
	if ( servp->dev_type == 0 )
	{
		servp->next_sendtime = (time_t)((int32_t)(sys_info.uptime + 5 ));
		msg[0] = 1;
		msg[3] = 6;
		len = 6;
	}
	else
	{
		servp->next_sendtime = (time_t)((int32_t)(sys_info.uptime + polling_time));
		msg[0] = 0x71;
		msg[3] = 30;
		*(u_short *)&msg[22] = servp->dev_type;
		*(u_short *)&msg[24] = servp->serial_no;
		*(uint32_t *)&msg[26] = htonl((uint32_t)sys_info.uptime);
		len = 30;
	}
	to.sin_family = AF_INET;
	to.sin_port = htons(0x405);
	to.sin_addr.s_addr = *(u_long*)servp->ip6_addr;
	sendto(polling_fd, msg, len, 0, (struct sockaddr *)&to, sizeof(to));
}

void poll_async_server_recv()
{
	struct sockaddr_in	from;
	int			len, n, m, i, connected, listening;
	int32_t			t;
	SERVINFO *		servp;
	TTYINFO *		infop;
	unsigned char	msg[100];
	struct sysinfo		sys_info;

	//printf("[AP] poll_async_server_recv\n");
#ifdef	AIX
	if ( recvfrom(polling_fd, msg, 86, 0, (struct sockaddr *)&from, (socklen_t *)&len)
#else
#ifdef	SCO
			if ( recvfrom(polling_fd, msg, 86, 0, (struct sockaddr *)&from, &len)
#endif
#ifndef	SCO
					len = sizeof(from);
	if ( recvfrom(polling_fd, msg, 86, 0, (struct sockaddr *)&from, (socklen_t *)&len)
			//if ( recvfrom(polling_fd, msg, 86, 0, (struct sockaddr *)&from, (size_t  *)&len)
#endif
#endif
			!= 86 )
		return;
	if ( ((msg[0] != 0x81) && (msg[0] != 0xF1)) || (msg[3] != 86) )
		return;
	if ( msg[1] || msg[2] || msg[4] || msg[5] ||
			(from.sin_port != ntohs(0x405)) )
		return;
	for ( n=0, servp=serv_info; n<servers; n++, servp++ )
	{
		if ( from.sin_addr.s_addr == *(u_long*)servp->ip6_addr )
			break;
	}
	if ( n == servers )
		return;

	if ( msg[0] == 0x81 )
	{
		sysinfo(&sys_info);
		n = 0;
		if ( (msg[10]==0x08 && msg[11]==0x21) || (msg[10]==0x16 && msg[11]==0x21) )
		{
			if ( (msg[25] > 1) || (msg[24] > (unsigned char)0x25) )
			{
				servp->dev_type = *(u_short *)&msg[10];
				servp->serial_no = *(u_short *)&msg[12];
				servp->last_servertime = (time_t)((int32_t)(sys_info.uptime - 1));
				n = 1;
			}
		}
		else
		{
			servp->dev_type = *(u_short *)&msg[10];
			servp->serial_no = *(u_short *)&msg[12];
			servp->last_servertime = (time_t)((int32_t)(sys_info.uptime - 1));
			n = 1;
		}
		if ( (servp->serial_no == 0) || (n == 1) )
		{
			servp->serial_no = *(u_short *)&msg[12];
			*(uint32_t *)(&msg[96]) = *(u_long*)servp->ip6_addr;
			msg[93] = msg[23];
			msg[94] = msg[24];
			msg[95] = msg[25];

			char *log_msg;
			log_msg = (char *)malloc(100);

			if (msg[93]) /* x.x.[x] */
				sprintf(log_msg,
						"IP=%d.%d.%d.%d, Ver=%x.%x.%x[0x%02x%02x%02x] is alive.",
						(int)(msg[96]), (int)(msg[97]), (int)(msg[98]),
						(int)(msg[99]), (int)(msg[95]), (int)(msg[94]),
						(int)(msg[93]), (int)(msg[95]), (int)(msg[94]),
						(int)(msg[93]));
			else
				sprintf(log_msg,
						"IP=%d.%d.%d.%d, Ver=%x.%x(0x%02x%02x) is alive.",
						(int)(msg[96]), (int)(msg[97]), (int)(msg[98]),
						(int)(msg[99]), (int)(msg[95]), (int)(msg[94]),
						(int)(msg[95]), (int)(msg[94]));
			/*
					if (msg[94] < 0x10)
						sprintf((char *)msg, "NPort(Async) Server (%d.%d.%d.%d) firmware version is %d.%02x .",
						(int)(msg[96]), (int)(msg[97]), (int)(msg[98]),
						(int)(msg[99]), (int)(msg[95]), (int)(msg[94]));
					else
						sprintf((char *)msg, "NPort(Async) Server (%d.%d.%d.%d) firmware version is %d.%2x .",
						(int)(msg[96]), (int)(msg[97]), (int)(msg[98]),
						(int)(msg[99]), (int)(msg[95]), (int)(msg[94]));
			 */
			log_event(log_msg);
			free(log_msg);
		}
		return;
	}
	t = ntohl(*(int32_t *)&msg[18]);
	if (  t - servp->last_servertime  <= 0 )
		return;
	if ( (servp->dev_type != *(u_short *)&msg[10]) ||
			(servp->serial_no != *(u_short *)&msg[12]) )
	{
		servp->dev_type = 0;
		sysinfo(&sys_info);
		servp->next_sendtime = (time_t)((int32_t)(sys_info.uptime - 1 ));
		return;
	}
	m = 0;
	servp->last_servertime = t;
	for ( n=0, infop=ttys_info; n<ttys; n++, infop++ )
	{
		if ( *(u_long*)infop->ip6_addr != *(u_long*)servp->ip6_addr )
			continue;
		for (i=0, connected=0, listening=0; i<MAX_PORTS; i++)
		{
			if ( infop->tcp_port != ntohs(*(u_short *)&msg[22+i*2]) )
				continue;

			if ( msg[54+i] == TCP_CONNECTED )
			{
				connected = 1;
				break;
			}
			if ( msg[54+i] == TCP_LISTEN )
			{
				if ( infop->state == STATE_RW_DATA )
					listening = 1;
				/* 1-30-02 by William
							else if ( infop->state == STATE_REMOTE_LISTEN ) {
								infop->state = STATE_RW_DATA;
							}
				 */
			}
		}
		if ( !connected && listening == 1 )
		{
			m++;
			infop->state = STATE_REMOTE_LISTEN;
			sysinfo(&sys_info);
			infop->time_out = sys_info.uptime;
		}
	}
	if ( m )
	{
		char *log_msg;
		log_msg = (char *)malloc(100);
		*(uint32_t *)(&msg[96]) = *(u_long*)servp->ip6_addr;
		sprintf(log_msg, "Ports reset of NPort(Async) Server %d.%d.%d.%d !",
				(int)(msg[96]), (int)(msg[97]), (int)(msg[98]),
				(int)(msg[99]));
		log_event(log_msg);
		free(log_msg);
	}
}

void poll_nport_send(servp)
SERVINFO	*servp;
{
	union	sock_addr to;
	int			    len;
	unsigned char	msg[32];
	DSCI_HEADER     *dsci_headerp;
	DSCI_DA_DATA    *dscidata_p;
	EX_HEADER		*exheader;
	struct sysinfo		sys_info;
	int af = servp->af;

	if (servp->dev_type)
		return;
	if(af == AF_INET6 && enable_ipv6 == DIS_IPV6)
		return;
#ifndef	STREAM
	bzero(msg, 28);
#endif
#ifdef	STREAM
	memset (msg, 0, 28);
#endif
	sysinfo(&sys_info);
	if ( servp->ap_id == 0 )
	{   // send dsc_search
		servp->next_sendtime = (time_t)((int32_t)(sys_info.uptime + 5 ));
		servp->dsci_ver = 0xFFFF;
		dsci_headerp=(DSCI_HEADER*)&msg[0];
		dsci_headerp->opcode = 0x01; // dsc_search
		dsci_headerp->result = 0;
		dsci_headerp->length = htons(8);
		dsci_headerp->id = 0;
		len = 8;
		if(af == AF_INET6)
		{
			dsci_headerp->opcode = DSCI_IPV6;
			dsci_headerp->length = htons(28);
			exheader = (EX_HEADER*)&msg[20];
			exheader->ex_vision = EX_VERSION;
			memset(exheader->reservd, 0, 3);
			exheader->catalog = htons(KERNEL_FUN);
			exheader->subcode = htons(DSC_ENUMSEARCH);
			len = 28;
		}
	}
	else if (servp->dsci_ver == 0xFFFF)
	{	// send getkernelinfo
		servp->next_sendtime = (time_t)((int32_t)(sys_info.uptime + 5 ));
		dsci_headerp=(DSCI_HEADER*)&msg[0];
		dsci_headerp->opcode = 0x16; // getkernelinfo
		dsci_headerp->result = 0;
		dsci_headerp->length = htons(20);
		dsci_headerp->id = 0;

		dscidata_p=(DSCI_DA_DATA*)&msg[8];
		dscidata_p->ap_id = htonl(servp->ap_id);
		dscidata_p->hw_id = htons(servp->hw_id);
		memcpy((void*)dscidata_p->mac, (void*)servp->mac, 6);
		len = 20;
	}
	else if (servp->dsci_ver == 0)
	{        // send dsc_GetNetstat
		servp->next_sendtime = (time_t)((int32_t)(sys_info.uptime + polling_time ));
		dsci_headerp=(DSCI_HEADER*)&msg[0];
		dsci_headerp->opcode = 0x14; // dsc_GetNetstat
		dsci_headerp->result = 0;
		dsci_headerp->length = htons(22);
		dsci_headerp->id = htonl((uint32_t)sys_info.uptime);

		dscidata_p=(DSCI_DA_DATA*)&msg[8];
		dscidata_p->ap_id = htonl(servp->ap_id);
		dscidata_p->hw_id = htons(servp->hw_id);
		memcpy((void*)dscidata_p->mac, (void*)servp->mac, 6);
		msg[20] = 128;   // max number of sockets
		msg[21] = 0;   // max number of sockets
		len = 22;
	}
	else
	{	// send dsc_GetNetstat_ex
		int addr;
		servp->next_sendtime = (time_t)((int32_t)(sys_info.uptime + polling_time ));
		dsci_headerp=(DSCI_HEADER*)&msg[0];
		dsci_headerp->opcode = (af == AF_INET) ? 0x1D : DSCI_IPV6; // dsc_GetNetstat_ex : DSCI IPv6
		dsci_headerp->result = 0;
		dsci_headerp->length = (af == AF_INET) ? htons(24) : htons(32);
		dsci_headerp->id = htonl((uint32_t)sys_info.uptime);

		dscidata_p=(DSCI_DA_DATA*)&msg[8];
		dscidata_p->ap_id = htonl(servp->ap_id);
		dscidata_p->hw_id = htons(servp->hw_id);
		memcpy((void*)dscidata_p->mac, (void*)servp->mac, 6);
		if(af == AF_INET6)
		{
			exheader = (EX_HEADER*)&msg[20];
			exheader->ex_vision = EX_VERSION;
			memset(exheader->reservd, 0, 3);
			exheader->catalog = htons(NETWORK_CONFIG);
			exheader->subcode = htons(DSC_GETNETSTAT_V6);
		}
		addr = (af == AF_INET) ? 20 : 28;  //max socket address for ipv4 or ipv6

		msg[addr] = 0x00;   // max number of sockets
		msg[addr+1] = (af == AF_INET) ? 0xFF : MAX_SOCK_V6;   // max number of sockets

		if(servp->af == AF_INET)
		{
			msg[addr+2] = (unsigned char)servp->start_item;	// start item
			msg[addr+3] = 0;   					// start item
		}
		else
		{
			msg[addr+3] = (unsigned char)servp->start_item;	// start item
			msg[addr+2] = 0;   					// start item
		}

		len = (af == AF_INET) ? 24 : 32;
	}
	memset(&to, 0, sizeof(to));
	if(af == AF_INET)
	{
		to.sin.sin_family = AF_INET;
		to.sin.sin_port = htons(4800);
		to.sin.sin_addr.s_addr = *(u_long*)servp->ip6_addr;
	}
	else
	{
		to.sin6.sin6_family = AF_INET6;
		to.sin6.sin6_port = htons(4800);
		memcpy(to.sin6.sin6_addr.s6_addr, servp->ip6_addr, 16);
	}
	sendto(polling_nport_fd[((af == AF_INET) ? 0 : 1)], msg, len, 0, (struct sockaddr *)&to, sizeof(to));
}

void poll_nport_recv(af_type)
int af_type;
{
	union sock_addr from;
	int             retlen, len, n, m, i, nstat, connected_tcp, connected_cmd, listening_tcp, listening_cmd;
	int32_t         t;
	SERVINFO *      servp;
	TTYINFO *       infop;
	char   msg[2100];
	DSCI_HEADER     *dsci_headerp;
	DSCI_RET_HEADER *dsci_retp;
	DSCI_NET_STAT   *desc_netstatp;
	DSCI_NET_STAT_IPV6 *desc_netstatp_ipv6;
	struct sysinfo  sys_info;
	u_short			next_item = 0;
	int             addr;

#ifdef	AIX
	if ( (retlen=recvfrom(polling_nport_fd[af_type], msg, sizeof(msg), 0, (struct sockaddr *)&from, (socklen_t *)&len))
#else
#ifdef	SCO
			if ( (retlen=recvfrom(polling_nport_fd[af_type], msg, sizeof(msg), 0, (struct sockaddr *)&from, &len))
#endif
#ifndef	SCO
					len = sizeof(from);
	if ( (retlen=recvfrom(polling_nport_fd[af_type], msg, sizeof(msg), 0, (struct sockaddr *)&from, (socklen_t *)&len))
#endif
#endif
			!= 24 && ((retlen-24)%16) && retlen != 36 && ((retlen - 44)%16) && ((retlen-32)%40) )
		return;
	dsci_headerp = (DSCI_HEADER*)&msg[0];
	if ( (dsci_headerp->opcode == 0x81 &&
			( (ntohs(dsci_headerp->length) != 24 ) && (ntohs(dsci_headerp->length) != 40) )) ||
			(dsci_headerp->opcode == 0x94 && ((ntohs(dsci_headerp->length)-24)%16) != 0) ||
			(dsci_headerp->opcode == 0x96 && ntohs(dsci_headerp->length) != 36) ||
			(dsci_headerp->opcode == 0x9d && ((ntohs(dsci_headerp->length)-24)%16) != 0) ||
			(dsci_headerp->opcode == DSCI_IPV6_RESPONS &&
					((((ntohs(dsci_headerp->length)-44)%16) != 0) &&  //dsci ipv6 enum search return
							(((ntohs(dsci_headerp->length)-32)%40) != 0))) ) //dsci ipv6 GetNetstat_V6 return
		return;
	if ( dsci_headerp->result!=0 ||
			( (from.sin.sin_port != ntohs(4800)) && (from.sin6.sin6_port != htons(4800)) ) )
		return;

	for ( n=0, servp=serv_info; n<servers; n++, servp++ )
	{
		if(af_type == 0)
		{
			if ( from.sin.sin_addr.s_addr == *(u_long*)servp->ip6_addr )
				break;
		}
		else
		{
			if(memcmp(from.sin6.sin6_addr.s6_addr, servp->ip6_addr, 16) == 0)
				break;
		}
	}
	if ( n == servers )
		return;

	sysinfo(&sys_info);
	dsci_retp=(DSCI_RET_HEADER*)&msg[8];
	if ( dsci_headerp->opcode == 0x81 ||
			((dsci_headerp->opcode == DSCI_IPV6_RESPONS) && ((htons(dsci_headerp->length)-44)%16 == 0)))
	{     // dsc_search respons
		char tmpbuf[4096];
		servp->ap_id = ntohl(dsci_retp->ap_id);
		servp->hw_id = ntohs(dsci_retp->hw_id);
		memcpy((void*)servp->mac, (void*)dsci_retp->mac, 6);
		servp->last_servertime = (time_t)((int32_t)(sys_info.uptime - 1));

		inet_ntop(servp->af, servp->ip6_addr, (char *)&msg[96], 50);
		sprintf(tmpbuf, "%s is alive", &msg[96]);
		log_event(tmpbuf);
		return;
	}
	else if ( dsci_headerp->opcode == 0x96 )
	{     // getkernelinfo respons
		servp->last_servertime = (time_t)((int32_t)(sys_info.uptime - 1));
		servp->dsci_ver = *(u_short *)(&msg[34]);
		return;
	}

	if (dsci_headerp->opcode == 0x9D
			|| (dsci_headerp->opcode == DSCI_IPV6_RESPONS && ((ntohs(dsci_headerp->length)-32)%40) == 0) )
	{
		if(servp->af == AF_INET)
		{
			next_item = (int)msg[23];	/* for big&little endian machine */
			next_item = (next_item << 8) | ((int)msg[22] & 0xff);
		}
		else
		{
			next_item = msg[30];
			next_item = (next_item << 8) | ((int)msg[31] & 0xff);
		}

		if (next_item)
			servp->start_item = next_item;
		else
			servp->start_item = 0;
	}

	t = ntohl(dsci_headerp->id);
	if (  t - servp->last_servertime  <= 0 )
		return;
	if ( (servp->ap_id != ntohl(dsci_retp->ap_id)) ||
			(servp->hw_id != ntohs(dsci_retp->hw_id)) )
	{
		servp->ap_id = 0;
		sysinfo(&sys_info);
		servp->next_sendtime = (time_t)((int32_t)(sys_info.uptime - 1 ));
		return;
	}
	m = 0;
	servp->last_servertime = t;
	if (servp->af == AF_INET) {
		addr = 21;
		nstat = (int)msg[addr]; /* for big & little endian machine */
		nstat = (nstat << 8) | ((int)msg[addr - 1] & 0xff);
	} else {
		addr = 29;
		nstat = (int)msg[addr - 1];
		nstat = (nstat << 8) | ((int)msg[addr] & 0xff);
	}
#if 0
	addr = (servp->af == AF_INET)? 21 : 29;
	nstat = (int)msg[addr];	/* for big&little endian machine */
	nstat = (nstat << 8) | ((int)msg[addr-1] & 0xff);
#endif
	addr = (servp->af == AF_INET) ? 128 : 35;
	if(nstat > addr){            /* the value can not over 128 */
		nstat = addr;            /*for ipv6, the value can not over 35*/
	}
	for ( n=0, infop=ttys_info; n<ttys; n++, infop++ )
	{
		u_short local_port = 0, remote_port = 0;
		unsigned char status = 0;
		if(servp->af == AF_INET)
		{
			if ( *(u_long*)infop->ip6_addr != *(u_long*)servp->ip6_addr )
				continue;
		}
		else if(servp->af == AF_INET6)
		{
			if(memcmp(infop->ip6_addr, servp->ip6_addr, 16) != 0)
				continue;
		}

		for (i=0, connected_tcp = connected_cmd = listening_tcp = listening_cmd = 0; i<nstat; i++)
		{
			if(servp->af == AF_INET)
			{
				desc_netstatp=(DSCI_NET_STAT*)&msg[24+i*16];
				local_port = desc_netstatp->local_port;
				remote_port = desc_netstatp->remote_port;
				status = desc_netstatp->status;
			}
			else if(servp->af == AF_INET6)
			{
				unsigned char *buf;
				desc_netstatp_ipv6 = (DSCI_NET_STAT_IPV6*)&msg[32+i*40];
				buf = (unsigned char *)&desc_netstatp_ipv6->local_port;
				local_port = buf[0]*0x100 + buf[1];
				buf = (unsigned char *)&desc_netstatp_ipv6->remote_port;
				remote_port = buf[0]*0x100 + buf[1];
				status = desc_netstatp_ipv6->status;
			}

			// Scott: 2005-09-19
			// If either the command port or data port is back to listen,
			//  info state of the port must be set the STATE_REMOTE_LISTEN,
			//  so that the port can be re-opened by the user application.
			// if ( infop->tcp_port != desc_netstatp->local_port )
			if ( !(infop->local_tcp_port && infop->tcp_port == local_port) &&
					!(infop->local_cmd_port && infop->cmd_port == local_port))
				continue;

#if 0
			if (infop->local_tcp_port && infop->tcp_port == desc_netstatp->local_port)
				printf("hit data port (%d, %d)\n", infop->tcp_port, infop->local_tcp_port);
			else if (infop->local_cmd_port && infop->cmd_port == desc_netstatp->local_port)
				printf("hit command port (%d, %d)\n", infop->cmd_port, infop->local_cmd_port);
#endif

			if (infop->tcp_port == local_port && status == TCP_LISTEN && infop->state == STATE_RW_DATA)
				listening_tcp = 1;
			else if (infop->cmd_port == local_port && status == TCP_LISTEN && infop->state == STATE_RW_DATA)
				listening_cmd = 1;
			else if (infop->local_tcp_port == remote_port && status == TCP_CONNECTED)
				connected_tcp = 1;
			else if (infop->local_cmd_port == remote_port && status == TCP_CONNECTED)
				connected_cmd = 1;
		}

		if ( (listening_tcp == 1 || listening_cmd == 1) && (!connected_tcp || !connected_cmd))
		{
			if (servp->dsci_ver == 0 || (servp->dsci_ver != 0 && next_item == 0))
			{
				m++;
				infop->alive_check_cnt++;
				if (infop->alive_check_cnt > 1) {
					infop->state = STATE_REMOTE_LISTEN;
					infop->alive_check_cnt = 0;
				}
			}
			sysinfo(&sys_info);
			infop->time_out = sys_info.uptime;
		} else {
			infop->alive_check_cnt = 0;
		}
	}
	if ( m )
	{
		char ip_buf[50];
		int size;
		size = sizeof(ip_buf);
		sprintf(msg, "Ports reset of NPort(Async) Server %s !",
				inet_ntop(servp->af, servp->ip6_addr, ip_buf, size));
		log_event(msg);
	}
}

/*
 *	The major function of Moxa pseudo TTYs daemon:
 *	maintain the TCP/IP connection to Async-Server and exchange those
 *	data to/from TCP sockets and master pseudo tty file descriptors.
 */
void moxattyd_handle_ttys()
{
	int		i, n, m, maxfd, sndx,len,len1,j;
	TTYINFO *	infop;
	SERVINFO *	servp;
	fd_set		rfd, wfd, efd;
	struct timeval	tm;
	char		cmd_buf[CMD_BUFFER_SIZE];
	int		tcp_wait_count;
	struct sysinfo	sys_info;

	signal(SIGPIPE, SIG_IGN);	/* add for "broken pipe" error */

	while ( 1 )
	{
		tm.tv_sec = 3;
		tm.tv_usec = 0;
		FD_ZERO(&rfd);
		FD_ZERO(&wfd);
		FD_ZERO(&efd);
		maxfd = -1;
		sndx = -1;
		tcp_wait_count = 0;
		for ( i=0, infop=ttys_info; i<ttys; i+=1, infop+=1 )
		{
			if(infop->static_param ){
				//sprintf(mm, "logger \"CFD>(%d, %s) static_param cleared(%s).\"", __LINE__, __FUNCTION__, infop->mpt_name);
				//system(mm);
				infop->static_param = 0;
			}

			if (infop->redundant_mode)
				continue;

			//This is a test code to generate many logs
			//{
			//	char msg[256];
			//	sprintf(msg, "RED> %d, %s, %s, %s", __LINE__, __FUNCTION__, __FUNCTION__, __FUNCTION__);
			//	log_event(msg);
			//}

			//sprintf(mm, "logger \"CFD>(%d) STATE=0x%X\"", infop->tcp_port, infop->state);
			//system(mm);

			if ( infop->state == STATE_INIT ||
					infop->state == STATE_MPT_OPEN ||
					infop->state == STATE_MPT_REOPEN )
			{
				//sprintf(mm, "logger \"CFD>(%d, %s) Opening(%s).\"", __LINE__, __FUNCTION__, infop->mpt_name);
				//system(mm);

				OpenTty(infop);
			}

			if ( infop->state == STATE_CONN_FAIL )
			{
				sysinfo(&sys_info);
				if ( (sys_info.uptime - infop->time_out) >= 1 ){
					//sprintf(mm, "logger \"CFD>(%d) Set TCP_OPEN @ %d, %s\"", infop->tcp_port, __LINE__, __FUNCTION__);
					//system(mm);
					infop->state = STATE_TCP_OPEN;
				}
			}

			if ( infop->state == STATE_TCP_OPEN )
				OpenTcpSocket(infop);

			if ( infop->state == STATE_TCP_CONN )
				ConnectTcp(infop);

#ifdef SSL_ON
			if ( infop->ssl_enable )
			{
				if ( infop->state == STATE_SSL_CONN )
					ConnectSSL(infop);
			}
#endif

			if ( infop->state == STATE_TCP_CLOSE )
				CloseTcp(infop);

			if ( infop->state == STATE_TCP_WAIT )
			{
				ConnectCheck();
				if ( infop->state == STATE_TCP_WAIT )
					tcp_wait_count++;
			}

			if ( infop->state < STATE_TTY_WAIT )
			{
				tm.tv_sec = 1;
			}
			else if ( infop->state == STATE_REMOTE_LISTEN)
			{
				CloseTcp(infop);
				continue;
			}

			if (infop->mpt_fd >= 0)
				FD_SET(infop->mpt_fd, &efd);
			if ( infop->mpt_fd > maxfd )
				maxfd = infop->mpt_fd;

			servp = &serv_info[infop->serv_index];
#ifndef OFFLINE_POLLING			
			if ( (infop->state >= STATE_RW_DATA)&&polling_time )
#else
				if ( polling_time )
#endif
				{
					if (!infop->first_servertime)
					{
						sysinfo(&sys_info);
#ifndef OFFLINE_POLLING					
						infop->first_servertime = sys_info.uptime - 1;
						servp->last_servertime = (time_t)((int32_t)(sys_info.uptime - 1));
#else
						infop->first_servertime = sys_info.uptime - polling_time;
						servp->last_servertime = (time_t)((int32_t)(sys_info.uptime - polling_time));
#endif
						//printf("[AP3] last_servertime = %d, sys_info.uptime = %d\n", servp->last_servertime, sys_info.uptime);
					}

					if ( sndx < 0 )
					{
						sysinfo(&sys_info);
						if ( ((time_t)((int32_t)sys_info.uptime) - servp->next_sendtime) > 0 )
						{
							sndx = infop->serv_index;
							FD_SET(polling_fd, &wfd);
						}
						//		printf("[AP3] last_servertime = %d, sys_info.uptime = %d\n", servp->last_servertime, sys_info.uptime);
#if 1
#ifndef OFFLINE_POLLING
						if (((time_t)((int32_t)sys_info.uptime)-servp->last_servertime)>timeout_time)
#else
							if (((time_t)((int32_t)sys_info.uptime)-servp->last_servertime)>timeout_time &&
									(infop->state >= STATE_RW_DATA))
#endif
							{
								infop->first_servertime = 0;
								infop->state = STATE_REMOTE_LISTEN;
								infop->time_out = sys_info.uptime;
								servp->start_item = 0;
							}
#endif
					}

					FD_SET(polling_fd, &rfd);
					if ( polling_fd > maxfd )
						maxfd = polling_fd;
					for(n=0; n<enable_ipv6; n++)
					{
						FD_SET(polling_nport_fd[n], &rfd);
						if ( polling_nport_fd[n] > maxfd )
							maxfd = polling_nport_fd[n];
					}
				}

			if ( infop->state >= STATE_RW_DATA )
			{
				if ( infop->mpt_fd > maxfd )
					maxfd = infop->mpt_fd;
				if ( infop->sock_fd > maxfd )
					maxfd = infop->sock_fd;
				if ( infop->sock_cmd_fd > maxfd )
					maxfd = infop->sock_cmd_fd;

				if ( infop->mpt_datakeep )
				{
					FD_SET(infop->sock_fd, &wfd);
				}
				else
				{
					FD_SET(infop->mpt_fd, &rfd);
				}

				if ( infop->sock_datakeep )
				{
					FD_SET(infop->mpt_fd, &wfd);
				}
				else
				{
					FD_SET(infop->sock_fd, &rfd);
				}

				FD_SET(infop->sock_cmd_fd, &rfd);
			}
		} /* for ( i=0, infop=ttys_info; i<ttys; i+=1, infop+=1 ) */

		if (tcp_wait_count)
		{
			tm.tv_sec = 0;
			tm.tv_usec = 20000;
		}

		if ((j= select(maxfd+1, &rfd, &wfd, &efd, &tm)) <= 0 )
		{
			continue;
		}

		for ( i=0, infop=ttys_info; i<ttys; i+=1, infop+=1 )
		{
			if (infop->redundant_mode)
				continue;
			if ( infop->mpt_fd < 0)
				continue;
			if ( (infop->mpt_fd)&&FD_ISSET(infop->mpt_fd, &efd) )
			{ //cmd ready
				if ((n=ioctl(infop->mpt_fd,
						_IOC(_IOC_READ|_IOC_WRITE,'m',CMD_RETRIEVE,CMD_BUFFER_SIZE),
						infop->mpt_cmdbuf)) > 0)
				{
					if (infop->mpt_cmdbuf[0] == NPREAL_ASPP_COMMAND_SET)
					{
						//sprintf(mm, "logger \"CFD>(%d) ASPP CMD=0x%02X @ %d <==\"", infop->tcp_port, infop->mpt_cmdbuf[1], __LINE__);
						//system(mm);
						write (infop->sock_cmd_fd,
								infop->mpt_cmdbuf+1,n-1);
					}
					else if (infop->mpt_cmdbuf[0] == NPREAL_LOCAL_COMMAND_SET)
					{
						switch (infop->mpt_cmdbuf[1])
						{
						case LOCAL_CMD_TTY_USED:
#ifdef SSL_ON
							//sprintf(mm, "logger \"CFD>(%d) CMD_TTY_USED, state=0x%X, (pssl=0x%x)\"", infop->tcp_port, infop->state, infop->pssl);
#else
							//sprintf(mm, "logger \"CFD>(%d) CMD_TTY_USED, state=0x%X\"", infop->tcp_port, infop->state);
#endif
							//system(mm);

//system("logger 'TTY USED'");
							if (infop->state != STATE_TTY_WAIT)
							{
#ifdef SSL_ON
								if (infop->ssl_enable)
								{
									SSL_shutdown(infop->pssl);
									SSL_free(infop->pssl);
									infop->pssl = NULL;
								}
#endif
								shutdown(infop->sock_fd, 2);
								shutdown(infop->sock_cmd_fd, 2);
								close(infop->sock_fd);
								close(infop->sock_cmd_fd);
								infop->sock_fd = -1;
								infop->sock_cmd_fd = -1;
								infop->local_tcp_port = 0;
								infop->local_cmd_port = 0;
								sprintf(cmd_buf, "Repeat connection!, %d, %s\n", infop->tcp_port, infop->ip_addr_s);
								log_event(cmd_buf);
								sleep(1);
							}
							//sprintf(mm, "logger \"CFD>(%d) Set TCP_OPEN @ %d, %s\"", infop->tcp_port, __LINE__, __FUNCTION__);
							//system(mm);
							infop->state = STATE_TCP_OPEN;
							sysinfo(&sys_info);
							infop->tty_used_timestamp = sys_info.uptime;
							continue;

						case LOCAL_CMD_TTY_UNUSED:
#ifdef SSL_ON
							//sprintf(mm, "logger \"CFD>(%d) CMD_TTY_UNUSED, (pssl=0x%x)\"", infop->tcp_port, infop->pssl);
#else
							//sprintf(mm, "logger \"CFD>(%d) CMD_TTY_UNUSED\"", infop->tcp_port);
#endif
							//system(mm);
//system("logger 'TTY UNUSED'");
#ifdef SSL_ON
							if (infop->ssl_enable)
							{
								//SSL_shutdown(infop->pssl);
								SSL_free(infop->pssl);
								infop->pssl = NULL;
							}
#endif
							shutdown(infop->sock_fd, 2);
							shutdown(infop->sock_cmd_fd, 2);
							close(infop->sock_fd);
							close(infop->sock_cmd_fd);
							infop->sock_fd = -1;
							infop->sock_cmd_fd = -1;
							infop->local_tcp_port = 0;
							infop->local_cmd_port = 0;
							cmd_buf[0] = NPREAL_LOCAL_COMMAND_SET;
							cmd_buf[1] = LOCAL_CMD_TTY_UNUSED;
							ioctl(infop->mpt_fd,
									_IOC(_IOC_READ|_IOC_WRITE,'m',CMD_RESPONSE,2),
									cmd_buf);
							infop->sock_datakeep = 0;
							infop->sock_dataofs = 0;
							infop->mpt_datakeep = 0;
							infop->mpt_dataofs = 0;
							if ((infop->state < STATE_RW_DATA) && !(infop->error_flags & ERROR_TCP_CONN))
							{
								sprintf(cmd_buf, "Socket connect fail (%s,TCP port %d) !",
										infop->ip_addr_s,
										infop->tcp_port);
								log_event(cmd_buf);
							}
							infop->state = STATE_TTY_WAIT;
							infop->tty_used_timestamp = 0;
							/* We not reset first polling time in offline polling mode */
#ifndef OFFLINE_POLLING								
							infop->first_servertime = 0;
#endif
							continue;
						}
					}
				}
			}

			if ( infop->state < STATE_RW_DATA )
				continue;

			if ( FD_ISSET(infop->sock_cmd_fd, &rfd) )
			{ //cmd resp
				if ((len = read(infop->sock_cmd_fd,
						infop->sock_cmdbuf,
						CMD_BUFFER_SIZE)) <= 0)
				{
#ifdef SSL_ON

					//sprintf(mm, "logger \"CFD>(%d) len=%d, errno: %d, %s @ %d <==\"", infop->tcp_port, len, errno, strerror(errno), __LINE__);
					//system(mm);

					if( CheckConnecting() ){
						infop->state = STATE_TCP_CLOSE;
						infop->reconn_flag = 1;
						continue;
					}

					if (infop->ssl_enable)
					{
						SSL_shutdown(infop->pssl);
						SSL_free(infop->pssl);
						infop->pssl = NULL;
					}
#endif
					close(infop->sock_fd);
					close(infop->sock_cmd_fd);
					infop->sock_fd = -1;
					infop->sock_cmd_fd = -1;
					infop->local_tcp_port = 0;
					infop->local_cmd_port = 0;
					infop->state = STATE_TCP_OPEN;
					//sprintf(mm, "logger \"CFD>(%d) Set TCP_OPEN @ %d, %s\"", infop->tcp_port, __LINE__, __FUNCTION__);
					//system(mm);
					ioctl(infop->mpt_fd,
							_IOC(_IOC_READ|_IOC_WRITE,'m',CMD_DISCONNECTED,0),
							0);
					continue;
				}
				//sprintf(mm, "logger \"CFD>(%d) ASPP GOT=0x%02X @ %d <==\"", infop->tcp_port, infop->sock_cmdbuf[0], __LINE__);
				//system(mm);

				n = 0;
				while (len > 0)
				{
					if (infop->sock_cmdbuf[n]
										   == ASPP_CMD_POLLING)
					{
						if (len < 3)
						{
							len = 0;
							continue;
						}
						cmd_buf[0] = ASPP_CMD_ALIVE;
						cmd_buf[1] = 1;
						cmd_buf[2] = infop->sock_cmdbuf[n+2];
						len1 = 3;
						write(infop->sock_cmd_fd,cmd_buf,len1);
						n += len1;
						len -= len1;
						continue;
					}
					switch (infop->sock_cmdbuf[n])
					{
					case ASPP_CMD_NOTIFY :
					case ASPP_CMD_WAIT_OQUEUE :
					case ASPP_CMD_OQUEUE :
					case ASPP_CMD_IQUEUE :
						len1 = 4;
						break;
					case ASPP_CMD_LSTATUS :
					case ASPP_CMD_PORT_INIT :
						len1 = 5;
						break;
					case ASPP_CMD_FLOWCTRL:
					case ASPP_CMD_IOCTL:
					case ASPP_CMD_SETBAUD:
					case ASPP_CMD_LINECTRL:
					case ASPP_CMD_START_BREAK:
					case ASPP_CMD_STOP_BREAK:
					case ASPP_CMD_START_NOTIFY:
					case ASPP_CMD_STOP_NOTIFY:
					case ASPP_CMD_FLUSH:
					case ASPP_CMD_HOST:
					case ASPP_CMD_TX_FIFO:
					case ASPP_CMD_XONXOFF:
					case ASPP_CMD_SETXON:
					case ASPP_CMD_SETXOFF:
						len1 = 3;
						break;
					default :
						len1 = len;
						break;
					}

					if ((len1 > 0)&&((n+len1) < CMD_BUFFER_SIZE))
					{
						cmd_buf[0] = NPREAL_ASPP_COMMAND_SET;
						memcpy(&cmd_buf[1],&infop->sock_cmdbuf[n],len1);
						ioctl(infop->mpt_fd,
								_IOC(_IOC_READ|_IOC_WRITE,'m',CMD_RESPONSE,len1+1),
								cmd_buf);
					}
					n += len1;
					len -= len1;
				}

			}

			if ( FD_ISSET(infop->mpt_fd, &rfd) )
			{
				m = infop->mpt_datakeep + infop->mpt_dataofs;
				n = read(infop->mpt_fd,
						infop->mpt_bufptr + m,
						BUFFER_SIZE - m);
				if ( n > 0 )
					infop->mpt_datakeep += n;
			}
			if ( FD_ISSET(infop->sock_fd, &rfd) )
			{
				m = infop->sock_datakeep + infop->sock_dataofs;
#ifdef	SSL_ON
				if (infop->ssl_enable)
				{
					n = SSL_read(infop->pssl,
							infop->sock_bufptr + m,
							BUFFER_SIZE - m);
				}
				else
				{
					n = read(infop->sock_fd,
							infop->sock_bufptr + m,
							BUFFER_SIZE - m);
				}
#else
				n = read(infop->sock_fd,
						infop->sock_bufptr + m,
						BUFFER_SIZE - m);
#endif
				if ( n > 0 )
				{
					infop->sock_datakeep += n;
					infop->state = STATE_RW_DATA;
					sysinfo(&sys_info);
					servp = &serv_info[infop->serv_index];
					servp->last_servertime = (time_t)((int32_t)(sys_info.uptime - 1));
					//printf("[AP4] last_servertime = %d, sys_info.uptime = %d\n", servp->last_servertime, sys_info.uptime);
				}
				else if (n <= 0)
				{
#ifdef SSL_ON

					//n=SSL_get_error(infop->pssl, n);
					//sprintf(mm, "logger \"CFD>(%d) SSL_get_error=%d @ %d, %s\"", infop->tcp_port, n, __LINE__, __FUNCTION__);
					//system(mm);

					if( CheckConnecting() ){
						infop->state = STATE_TCP_CLOSE;
						infop->reconn_flag = 1;
						continue;
					}

					if (infop->ssl_enable)
					{
						SSL_shutdown(infop->pssl);
						SSL_free(infop->pssl);
						infop->pssl = NULL;
					}
#endif
					close(infop->sock_fd);
					close(infop->sock_cmd_fd);
					infop->sock_fd = -1;
					infop->sock_cmd_fd = -1;
					infop->local_tcp_port = 0;
					infop->local_cmd_port = 0;
					infop->state = STATE_TCP_OPEN;
					//sprintf(mm, "logger \"CFD>(%d) Set TCP_OPEN @ %d, %s\"", infop->tcp_port, __LINE__, __FUNCTION__);
					//system(mm);
					ioctl(infop->mpt_fd,
							_IOC(_IOC_READ|_IOC_WRITE,'m',CMD_DISCONNECTED,0),
							0);
					continue;
				}
			}
			if ( FD_ISSET(infop->mpt_fd, &wfd) )
			{
				n = write(infop->mpt_fd,
						infop->sock_bufptr+infop->sock_dataofs,
						infop->sock_datakeep);
				if ( n > 0 )
				{
					infop->sock_datakeep -= n;
					if ( infop->sock_datakeep )
						infop->sock_dataofs += n;
					else
						infop->sock_dataofs = 0;
				}
			}
			if ( FD_ISSET(infop->sock_fd, &wfd) )
			{
#ifdef	SSL_ON
				if (infop->ssl_enable)
				{
					n = SSL_write(infop->pssl,
							infop->mpt_bufptr+infop->mpt_dataofs,
							infop->mpt_datakeep);
				}
				else
				{
					n = write(infop->sock_fd,
							infop->mpt_bufptr+infop->mpt_dataofs,
							infop->mpt_datakeep);
				}
#else
				n = write(infop->sock_fd,
						infop->mpt_bufptr+infop->mpt_dataofs,
						infop->mpt_datakeep);
#endif
				if ( n > 0 )
				{
					sysinfo(&sys_info);
					servp = &serv_info[infop->serv_index];
					servp->last_servertime = (time_t)((int32_t)(sys_info.uptime - 1));
					//printf("[AP5] last_servertime = %d, sys_info.uptime = %d\n", servp->last_servertime, sys_info.uptime);
					infop->mpt_datakeep -= n;
					if ( infop->mpt_datakeep )
						infop->mpt_dataofs += n;
					else
						infop->mpt_dataofs = 0;
				}
				else if (n < 0)
				{
					log_event("Can not write data");
				}
			}
		}

		if ( polling_time == 0 )
			continue;
#if 1

		if ( (sndx >= 0) && FD_ISSET(polling_fd, &wfd))
		{
			poll_async_server_send(&serv_info[sndx]);
			poll_nport_send(&serv_info[sndx]);
		}
		if ( FD_ISSET(polling_fd, &rfd) )
		{
			poll_async_server_recv();
		}
		for(n=0; n<enable_ipv6; n++)
		{

			if ( FD_ISSET(polling_nport_fd[n], &rfd) )
			{
				poll_nport_recv(n);
			}
		}
#endif
	} /* while ( 1 ) */
}

void OpenTty(infop)
TTYINFO *	infop;
{
	char	buf[80];

	if ( infop->mpt_fd >= 0 )
	{
		struct	net_node_setting nd_settings;
		int	tty_status = 0;
		infop->reconn_flag = 1;
		nd_settings.server_type = infop->server_type; /* TODO: Remove server_type which is useless */
		nd_settings.disable_fifo = infop->disable_fifo;
		ioctl(infop->mpt_fd,
				_IOC(_IOC_READ|_IOC_WRITE,'m',CMD_NET_SETTING,
						sizeof(struct net_node_setting)),
						&nd_settings); /* pass fifo to kernel */
		ioctl(infop->mpt_fd,
				_IOC(_IOC_READ|_IOC_WRITE,'m',CMD_GET_TTY_STATUS,
						sizeof(int)),&tty_status); /* get whether the port is opened? */

		if (infop->tty_used_timestamp == 0)
		{
			if (!tty_status)
			{
				infop->state = STATE_TTY_WAIT;
			}
			else
			{
				ioctl(infop->mpt_fd,
						_IOC(_IOC_READ|_IOC_WRITE,'m',CMD_DISCONNECTED,0),
						0);
				infop->state = STATE_TCP_OPEN;
			}
		}
		else
			infop->state = STATE_TCP_OPEN; /* If TCP is opened, tty_used_timestamp is given. */

	} else {

		infop->mpt_fd = open(infop->mpt_name, O_RDWR);

		if ( infop->mpt_fd < 0 )
		{
			sprintf(buf, "Master tty open fail (%s) !",
					infop->mpt_name);
			log_event(buf);
			infop->error_flags |= ERROR_MPT_OPEN;
		}
	}
}
#define SOCK_BUF 1048
void OpenTcpSocket(infop)
TTYINFO *	infop;
{
	char	buf[256];
	int	on=1;
	int af;
	af = infop->af;

	infop->sock_fd = socket(af, SOCK_STREAM, 0);
	if ( infop->sock_fd >= 0 )
	{
		if ( setsockopt(infop->sock_fd, SOL_SOCKET, SO_KEEPALIVE, (char *)&on, sizeof(on)) < 0 )
			log_event("Set TCP keep alive fail !");
		if(strlen(infop->scope_id) > 0)
		{
			if ( setsockopt(infop->sock_fd, SOL_SOCKET, SO_BINDTODEVICE, infop->scope_id, strlen(infop->scope_id)) < 0)
				log_event("Set TCP bind to device fail !");
		}
		infop->state = STATE_TCP_CONN;
	}
	infop->sock_cmd_fd = socket(af, SOCK_STREAM, 0);
	if ( infop->sock_cmd_fd >= 0 )
	{
		if ( setsockopt(infop->sock_cmd_fd, SOL_SOCKET, SO_KEEPALIVE, (char *)&on, sizeof(on)) < 0 )
			log_event("Set TCP keep alive fail !");
		if(strlen(infop->scope_id) > 0)
		{
			if ( setsockopt(infop->sock_cmd_fd, SOL_SOCKET, SO_BINDTODEVICE, infop->scope_id, strlen(infop->scope_id)) < 0)
				log_event("Set TCP bind to device fail !");
		}
		infop->state = STATE_TCP_CONN;
	}

	if ((infop->sock_fd < 0) || (infop->sock_cmd_fd < 0))
	{
		close(infop->sock_fd);
		close(infop->sock_cmd_fd);
		if ( !(infop->error_flags & ERROR_TCP_OPEN) )
		{
			if (infop->sock_fd < 0)
			{
				sprintf(buf, "Socket open fail (%s, TCP port %d) !",
						infop->ip_addr_s,
						infop->tcp_port);
				log_event(buf);
			}
			if (infop->sock_cmd_fd < 0)
			{
				sprintf(buf, "Socket open fail (%s, TCP port %d) !",
						infop->ip_addr_s,
						infop->cmd_port);
				log_event(buf);
			}
			infop->error_flags |= ERROR_TCP_OPEN;
		}
		infop->sock_fd = -1;
		infop->sock_cmd_fd = -1;
		infop->state = STATE_TCP_OPEN; // Scott: 2005-09-20
	}
}

void ConnectTcp(infop)
TTYINFO *	infop;
{
	int			childpid;
	ConnMsg 		msg;
	union sock_addr sock;

#ifdef OFFLINE_POLLING
	SERVINFO *	servp;
	struct sysinfo	sys_info;
	servp = &serv_info[infop->serv_index];
#endif

	if(infop->af == AF_INET6 && enable_ipv6 == DIS_IPV6)
		return;

	resolve_dns_host_name(infop);

	infop->state = STATE_TCP_WAIT;
	infop->tcp_wait_id = (++g_tcp_wait_id);
	if ( (childpid = fork()) == 0 )
	{	/* child process */
		msg.tcp_wait_id = infop->tcp_wait_id;
		close(pipefd[0]);
		msg.status = CONNECT_FAIL;

#ifdef OFFLINE_POLLING
		sysinfo(&sys_info);
		/* Check out the last server response time, if it is over 30 second not response,
		 *  we treat this serve as non-exist, the TCP connect fail directly*/
		if((!infop->first_servertime) || /* If the poll not start yet, we indicate device not exist */
				(infop->first_servertime == servp->last_servertime ) || /* If this two are the same means first polling not ack back */
				(sys_info.uptime - servp->last_servertime >= POLLING_ALIVE_TIME))
		{
			msg.infop = infop;
			write(pipefd[1], (char *)&msg, sizeof(ConnMsg));
			close(pipefd[1]);
			exit(0);
		}
#endif

		if(infop->af == AF_INET)
		{
			sock.sin.sin_family = AF_INET;
			sock.sin.sin_addr.s_addr = *(u_long*)infop->ip6_addr;
			sock.sin.sin_port = htons(infop->cmd_port);
		}
		else
		{
			memset(&sock.sin6, 0, sizeof(sock));
			sock.sin6.sin6_family = AF_INET6;
			sock.sin6.sin6_port = htons(infop->cmd_port);
			memcpy(sock.sin6.sin6_addr.s6_addr, infop->ip6_addr, 16);
		}
		if ( connect(infop->sock_cmd_fd, (struct sockaddr*)&sock, sizeof(sock)) >= 0 )
		{
			if(infop->af == AF_INET)
			{
				sock.sin.sin_family = AF_INET;
				sock.sin.sin_addr.s_addr = *(u_long*)infop->ip6_addr;
				sock.sin.sin_port = htons(infop->tcp_port);
			}
			else
			{
				sock.sin6.sin6_family = AF_INET6;
				sock.sin6.sin6_port = htons(infop->tcp_port);
				memcpy(sock.sin6.sin6_addr.s6_addr, infop->ip6_addr, 16);
			}
			if ( connect(infop->sock_fd, (struct sockaddr*)&sock, sizeof(sock)) >= 0)
			{
				if(infop->af == AF_INET6)
				{
					int rand[16];
					if ( write(infop->sock_cmd_fd, rand, 16) >= 0)
					{
						if(read(infop->sock_cmd_fd, rand, 16) != 16)
						{
							msg.infop = infop;
							//write(pipefd[1], (char *)&msg, sizeof(ConnMsg));
							write(pipefd[1], (char *)&msg, sizeof(ConnMsg));
							close(pipefd[1]);
							//close(pipefd[1]);
							exit(0);
						}
					}
				}
				msg.status = CONNECT_OK;
			}
		}
		msg.infop = infop;
		write(pipefd[1], (char *)&msg, sizeof(ConnMsg));
		close(pipefd[1]);
		/*		close(infop->sock_fd);
			close(infop->sock_cmd_fd);
		 */
		exit(0);
		/*
		msg.tcp_wait_id = infop->tcp_wait_id;
		close(pipefd[0]);
		msg.status = CONNECT_FAIL;
		sin.sin_family = AF_INET;
		sin.sin_addr.s_addr = infop->ip_addr;
		sin.sin_port = htons(infop->tcp_port);
		if ( connect(infop->sock_fd, (struct sockaddr *)&sin,
			sizeof(sin)) >= 0 ) {
			sin.sin_family = AF_INET;
			sin.sin_addr.s_addr = infop->ip_addr;
			sin.sin_port = htons(infop->cmd_port);
			if ( connect(infop->sock_cmd_fd,
				 (struct sockaddr *)&sin,
				 sizeof(sin)) >= 0 ) {
				msg.status = CONNECT_OK;
			}
		}
		msg.infop = infop;
		write(pipefd[1], (char *)&msg, sizeof(ConnMsg));
		close(pipefd[1]);
		close(infop->sock_fd);
		close(infop->sock_cmd_fd);
		exit(0);
		 */
	}
	else if ( childpid < 0 )
	{
		infop->state = STATE_TCP_CONN;
		if ( !(infop->error_flags & ERROR_FORK) )
		{
			log_event("Can't fork child process !");
			infop->error_flags |= ERROR_FORK;
		}
	}
}

#ifdef SSL_ON
void ConnectSSL(infop)
TTYINFO *	infop;
{
	fd_set		rfd, wfd;
	struct timeval	tm;
	int	fd, flags, ret;
	struct sysinfo	sys_info;
	char buf[100];
	TTYINFO * chk_infop;
	int i;

	if( infop->ssl_time==0 ){
		for ( i=0, chk_infop=ttys_info; i<ttys; i+=1, chk_infop+=1 )
		{
			// Scan all ports and check if there is also ports in STATE_SSL_CONN state.
			// We should avoid multiple ports to call SSL_connect together.
			// Otherwise SSL core migth got Alert (Decrypt_Error 51) and reset previous connection.
			// Here we let connection call SSL_connect by ttys_info sequence to avoid this issue.
			// Windows driver has similar behavior as it.
			
			if( (chk_infop->state==STATE_SSL_CONN) && (chk_infop->ssl_time!=0)  ){
				//sprintf(mm, "logger \"CFD>(%d) Wait (%d) for SSL..\"", infop->tcp_port, chk_infop->tcp_port);
				//system(mm);
				return;
			}       
		}
	}

	sysinfo(&sys_info);
	if( infop->ssl_time==0 ){
		infop->ssl_time = sys_info.uptime;
	} else {
		//if ((sys_info.uptime - infop->ssl_time) < 3 ){
		//infop->ssl_time = sys_info.uptime;
		//      return;
		//}
	}

	//infop->ssl_time = sys_info.uptime;
	if (infop->pssl==NULL)
	{
		infop->pssl = SSL_new(sslc_ctx);
		//sprintf(mm, "logger \"CFD>(%d) pssl=0x%X..\"", infop->tcp_port, infop->pssl);
		//system(mm);
		
		if (infop->pssl != NULL)
		{
			if (SSL_set_fd(infop->pssl, infop->sock_fd))
			{
				//sprintf(mm, "logger \"CFD>(%d) set_connect_state..\"", infop->tcp_port);
				//system(mm);
				SSL_set_connect_state(infop->pssl);
			}
			else
			{
				//sprintf(mm, "logger \"CFD>(%d) SSL_set_fd() error..\"", infop->tcp_port);
				//system(mm);
				log_event("SSL_set_fd() error!");
			}
		}
		else
		{
			//sprintf(mm, "logger \"CFD>(%d) SSL_new() error..\"", infop->tcp_port);
			//system(mm);
			log_event("SSL_new() error!");
		}
	}


	fd = infop->sock_fd;

	tm.tv_sec = 0;
	tm.tv_usec = 1000;

	FD_ZERO(&rfd);
	FD_ZERO(&wfd);

	FD_SET(fd, &wfd);
	FD_SET(fd, &rfd);

	if ((flags = fcntl(infop->sock_fd, F_GETFL, 0)) < 0)
		log_event("fcntl F_GETFL fail!");
	if (fcntl(infop->sock_fd, F_SETFL, flags | O_NONBLOCK) < 0)
		log_event("fcntl F_SETFL fail!");

	if ( select(fd+1, &rfd, &wfd, 0, &tm) > 0 )
	{
		if ( FD_ISSET(fd, &wfd) || FD_ISSET(fd, &rfd))
		{
			if ((ret = SSL_connect(infop->pssl)) > 0)
			{
				infop->ssl_time = 0;
				infop->state = STATE_RW_DATA;
			}
			else
			{
				ret = SSL_get_error(infop->pssl, ret);
				switch (ret)
				{
				case SSL_ERROR_WANT_READ:
				case SSL_ERROR_WANT_WRITE:
					infop->state = STATE_SSL_CONN;
					log_event("SSL_ERROR_WANT_WRITE");
					break;
				case SSL_ERROR_SYSCALL:
					//sprintf(mm, "logger \"CFD>(%d) errno: %d, %s @ %d <==\"", infop->tcp_port, errno, strerror(errno), __LINE__);
					//system(mm);
					
					//SSL_shutdown(infop->pssl);
					//SSL_free(infop->pssl);
					//infop->pssl = NULL;
					infop->state = STATE_TCP_CLOSE;
					infop->reconn_flag = 1;
					break;
				case SSL_ERROR_ZERO_RETURN:
				case SSL_ERROR_WANT_CONNECT:
				case SSL_ERROR_WANT_X509_LOOKUP:
				case SSL_ERROR_SSL:
					infop->state = STATE_TCP_CLOSE;
					infop->reconn_flag = 0;
					sprintf(buf, "SSL_connect Other Error %d", ret);
					log_event(buf);
					break;
				}
			}
		}
	}
	fcntl(fd, F_SETFL, flags);

#if 0
	sysinfo(&sys_info);
	if ((sys_info.uptime - infop->ssl_time) > 5 )
	{
		infop->state = STATE_TCP_CLOSE;
		infop->reconn_flag = 0;
		log_event("Your target machine might not be set secure mode.");
	}
#endif
}
#endif

void CloseTcp(infop)
TTYINFO *	infop;
{
	int			childpid;
	ConnMsg 		msg;

	infop->state = STATE_TCP_WAIT;
	infop->tcp_wait_id = (++g_tcp_wait_id);
	if ( (childpid = fork()) == 0 )
	{	/* child process */
		msg.tcp_wait_id = infop->tcp_wait_id;
		close(pipefd[0]);
#ifdef SSL_ON
		if (infop->ssl_enable)
		{
			SSL_shutdown(infop->pssl);
			SSL_free(infop->pssl);
			infop->pssl = NULL;
		}
#endif
		close(infop->sock_fd);
		close(infop->sock_cmd_fd);
		sleep(1);
		msg.status = CLOSE_OK;
		msg.infop = infop;
		write(pipefd[1], (char *)&msg, sizeof(ConnMsg));
		close(pipefd[1]);
		exit(0);
	}
	else if ( childpid < 0 )
	{
		infop->state = STATE_TCP_CLOSE;
		if ( !(infop->error_flags & ERROR_FORK) )
		{
			log_event("Can't fork child process !");
			infop->error_flags |= ERROR_FORK;
		}
	}

	if ( infop->state != STATE_TCP_CLOSE )
	{
#ifdef SSL_ON
		if (infop->ssl_enable)
		{
			SSL_shutdown(infop->pssl);
			SSL_free(infop->pssl);
			infop->pssl = NULL;
		}
#endif
		close(infop->sock_fd);
		close(infop->sock_cmd_fd);
		infop->local_tcp_port = 0;
		infop->local_cmd_port = 0;
	}
}

void ConnectCheck()
{
	ConnMsg 	msg;
	TTYINFO *	infop;
	char		buf[256];
	int ret;
	struct sysinfo	sys_info;
	struct sockaddr_in	local_sin;
	struct sockaddr_in6	local_sin6;
	socklen_t		socklen = sizeof(local_sin);
	struct sockaddr * ptr;

	if ((ret=read(pipefd[0], (char *)&msg, sizeof(ConnMsg))) == sizeof(ConnMsg))
	{
		infop = msg.infop;
		if ( (infop->state == STATE_TCP_WAIT)&&(infop->tcp_wait_id == msg.tcp_wait_id) )
		{
			ptr = (infop->af == AF_INET) ? (struct sockaddr*)&local_sin : (struct sockaddr*)&local_sin6;
			socklen = (infop->af == AF_INET) ? sizeof(local_sin) : sizeof(local_sin6);
			if ( msg.status == CONNECT_OK )
			{
				infop->alive_check_cnt = 0;
				getsockname(infop->sock_fd, ptr, &socklen);
				if(infop->af == AF_INET)
					infop->local_tcp_port = ntohs(local_sin.sin_port);
				else
					infop->local_tcp_port = ntohs(local_sin6.sin6_port);
				getsockname(infop->sock_cmd_fd, ptr, &socklen);
				if(infop->af == AF_INET)
					infop->local_cmd_port = ntohs(local_sin.sin_port);
				else
					infop->local_cmd_port = ntohs(local_sin6.sin6_port);

				infop->state = STATE_RW_DATA;
				infop->error_flags = 0;
				buf[0] = NPREAL_LOCAL_COMMAND_SET;
				buf[1] = LOCAL_CMD_TTY_USED;
#ifdef OFFLINE_POLLING
				buf[2] = 1; /* Indicate connection ok */
#endif
				ioctl(infop->mpt_fd,
						_IOC(_IOC_READ|_IOC_WRITE,'m',CMD_CONNECTED,0),
						0);
#ifndef OFFLINE_POLLING
				ioctl(infop->mpt_fd,
						_IOC(_IOC_READ|_IOC_WRITE,'m',CMD_RESPONSE,2),
						buf);
#else
				ioctl(infop->mpt_fd,
						_IOC(_IOC_READ|_IOC_WRITE,'m',CMD_RESPONSE,3),
						buf);
#endif
#ifdef SSL_ON
				if (infop->ssl_enable)
				{
					//sysinfo(&sys_info);
					//infop->ssl_time = sys_info.uptime;
					infop->ssl_time = 0;
					infop->state = STATE_SSL_CONN;
					// Create new SSL in ConnectSSL() after previous SSL connected
				}
#endif

#if 0
				if (infop->ssl_enable)
				{
					infop->pssl = SSL_new(sslc_ctx);
					if (infop->pssl != NULL)
					{
						if (SSL_set_fd(infop->pssl, infop->sock_fd))
						{
							SSL_set_connect_state(infop->pssl);
						}
						else
						{
							log_event("SSL_set_fd() error!");
						}
					}
					else
					{
						log_event("SSL_new() error!");
					}
					sysinfo(&sys_info);
					infop->ssl_time = sys_info.uptime;
					infop->state = STATE_SSL_CONN;
					/*if (SSL_connect(infop->pssl) < 0){
								printf("SSL_connect() error.\n");
					SSL_free(infop->pssl);
					}*/
				}
#endif
			}
			else if ( msg.status == CLOSE_OK )
			{
				infop->error_flags = 0;
				infop->sock_fd = -1;
				infop->sock_cmd_fd = -1;
				if(infop->reconn_flag == 1)       /*reconnect or not*/
					infop->state = STATE_TCP_OPEN;
				else if(infop->reconn_flag == 0)
					infop->state = STATE_TTY_WAIT;
				ioctl(infop->mpt_fd,
						_IOC(_IOC_READ|_IOC_WRITE,'m',CMD_DISCONNECTED,0),
						0);
			}
			else
			{
				close(infop->sock_fd);
				close(infop->sock_cmd_fd);
				infop->sock_fd = -1;
				infop->sock_cmd_fd = -1;
				infop->local_tcp_port = 0;
				infop->local_cmd_port = 0;
				infop->state = STATE_CONN_FAIL;
				sysinfo(&sys_info);
				infop->time_out = sys_info.uptime;
#ifdef OFFLINE_POLLING
				buf[0] = NPREAL_LOCAL_COMMAND_SET;
				buf[1] = LOCAL_CMD_TTY_USED;
				buf[2] = 0; /* Indicate connection fail */
				ioctl(infop->mpt_fd,
						_IOC(_IOC_READ|_IOC_WRITE,'m',CMD_RESPONSE,3),
						buf);
#endif				
				ioctl(infop->mpt_fd,
						_IOC(_IOC_READ|_IOC_WRITE,'m',CMD_DISCONNECTED,0),
						0);
				if ( !(infop->error_flags & ERROR_TCP_CONN) && ((sys_info.uptime - infop->tty_used_timestamp) > 60))
				{
					sprintf(buf, "ConnectCheck> Socket connect fail (%s,TCP port %d) !",
							infop->ip_addr_s,
							infop->tcp_port);
					log_event(buf);
					infop->error_flags |= ERROR_TCP_CONN;
				}
			}
		}
	}
}

int CheckConnecting()
{
#ifdef SSL_ON
	TTYINFO * chk_infop;
	int i;
	for ( i=0, chk_infop=ttys_info; i<ttys; i+=1, chk_infop+=1 )
	{
		if( (chk_infop->state==STATE_SSL_CONN) ){
			return 1; 
		}       
	}
#endif
	return 0;
}
