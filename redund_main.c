/*
 *	Copyright (C) 2001  Moxa Inc.
 *	All rights reserved.
 *
 *	Moxa NPort/Async Server UNIX Real TTY daemon program.
 *
 *	Usage: npreal2d_redund [-t reset-time]
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
#include	"redund.h"
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

int	moxattyd_read_config(char *cfgpath);

/*
 *	MOXA TTY daemon main program
 */
int main(argc, argv)
int	argc;
char *	argv[];
{
	TTYINFO *	infop;
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
		fprintf (stderr, "Not any tty defined\n");
		return 1;
		//			usleep(1000);
		//			continue;
	}

	/*
	 * Initialize this Moxa TTYs daemon process.
	 */
	if (!Gfglog_mode) {
		openlog ("redund", LOG_PID, LOG_DAEMON);
		if (daemon (0, 0)) {
			log_event ("Failed to daemonize");
			return -1;
		}
	}

	/*
	 * Initialize polling async server function.
	 */
	while ( polling_time && (poll_async_server_init() < 0) ) { }

	/*
	 * Open PIPE, set read to O_NDELAY mode.
	 */
	//

	infop = ttys_info;
	for (i = 0;i < ttys;i++) {
		if ( pipe(infop->pipe_port) < 0 )
		{
			log_event("pipe error !");
			return -1;
		}
#ifdef	O_NDELAY
		fcntl(infop->pipe_port[0], F_SETFL, fcntl(infop->pipe_port[0], F_GETFL) | O_NDELAY);
#endif
		infop++;
	}

	sprintf(ver, "MOXA Real TTY daemon program starting (%s %s)...", NPREAL_VERSION, NPREAL_BUILD);
	log_event(ver);

	/*
	 * Handle Moxa TTYs data communication.
	 */
#ifdef SSL_ON
	ssl_init();
#endif

	while (1)
	{
		if (Gredund_mode)
			redund_handle_ttys(); /* child process ok */
	}
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
	int ret1 = NP_RET_SUCCESS;
	int ret2 = NP_RET_SUCCESS;

	do{
		if( ipv4_str_to_ip(infop->ip_addr_s, &addr_ipv4)==NP_RET_SUCCESS ){
			*(ulong*)infop->ip6_addr = addr_ipv4;
			infop->af = AF_INET;
			ret1 = NP_RET_SUCCESS;
			break;

		} else if(ipv6_str_to_ip(infop->ip_addr_s, addr_ipv6)==NP_RET_SUCCESS )	{
			memcpy(infop->ip6_addr, addr_ipv6, sizeof(infop->ip6_addr));
			infop->af = AF_INET6;
			ret1 = NP_RET_SUCCESS;
			break;
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
				ret1 = NP_RET_ERROR;
				break;
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
			ret1 = NP_RET_ERROR;
			break;
		}
	} while(0);

	while(infop->redundant_mode) {

		if( ipv4_str_to_ip(infop->redund.redund_ip, &addr_ipv4)==NP_RET_SUCCESS ){
			*(u_long*)infop->redund.ip6_addr = addr_ipv4;
			//{
			//	char ipAddress[INET_ADDRSTRLEN];
			//	inet_ntop(AF_INET, &(addr_ipv4), ipAddress, INET_ADDRSTRLEN);
			//	sprintf(msg, "ipAddress: %s @ %d, %s\n", ipAddress, __LINE__, __FUNCTION__);
			//	log_event(msg);
			//}
			ret2 = NP_RET_SUCCESS;
			break;
		} else if(ipv6_str_to_ip(infop->redund.redund_ip, addr_ipv6)==NP_RET_SUCCESS )	{
			memcpy(infop->redund.ip6_addr, addr_ipv6, sizeof(infop->redund.ip6_addr));
			ret2 = NP_RET_SUCCESS;
			break;
		}

		hints.ai_family = AF_INET;    /* Allow IPv4 or IPv6 */

		ret = getaddrinfo(infop->redund.redund_ip, NULL, &hints, &result);
		if( ret==EAI_AGAIN || ret==EAI_NONAME ){
			// Sometimes, this error occurred. It means DNS server or DNS configuration are wrong temporarily.
			sleep(1);
			res_init(); // init name resolver again!

			ret = getaddrinfo(infop->redund.redund_ip, NULL, &hints, &result);
			if (ret != 0) {
				sprintf(msg, "getaddrinfo: %s @ %d, %s\n", gai_strerror(ret), __LINE__, __FUNCTION__);
				log_event(msg);
				ret2 = NP_RET_ERROR;
			}
		}

		*(ulong*)infop->redund.redund_ip = 0;
		memset(infop->redund.ip6_addr, 0, sizeof(infop->redund.ip6_addr));

		for (rp = result; rp != NULL; rp = rp->ai_next) {

			if(rp->ai_family == AF_INET)
			{
				ipv4 = (struct sockaddr_in *)rp->ai_addr;
				*(ulong*)infop->redund.ip6_addr = ((struct in_addr *)&ipv4->sin_addr)->s_addr;
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
			ret2 = NP_RET_ERROR;
			break;
		}

		freeaddrinfo(result);           /* No longer needed */
		break;
	}

	if( ret1!=NP_RET_SUCCESS || ret2!=NP_RET_SUCCESS ){
		return NP_RET_ERROR;
	} else {
		return NP_RET_SUCCESS;
	}
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
	struct hostent *host;
	TTYINFO *	infop;
	char		buf[160];
	char		ttyname[160],tcpport[16],cmdport[16];
	char		ttyname2[160], curname[160], scope_id[10];
	int			redundant_mode;
	int32_t		server_type,disable_fifo;
#ifdef SSL_ON
	int32_t		ssl_enable;
#else
	int32_t            temp;
#endif

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
		memset(&infop->redund, 0, sizeof(struct redund_struct));
		server_type = disable_fifo = 0;
#ifdef SSL_ON
		ssl_enable = 0;
#endif
		n = sscanf(buf, "%s%s%s%s%d%d%s%s%s%d%s",
				ttyname,
				infop->ip_addr_s,
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
				&infop->redundant_mode,
				infop->redund.redund_ip);
		//		printf("[AP] [ttyname = %s],[tcpport = %s],[ip1 = %s],[ip2 = %s]\n", ttyname, tcpport,
		//								 infop->ip_addr_s,
		//								 infop->redund.redund_ip);
		if(n != 10 && n != 11)
		{
			continue;
		}

		if (ttyname[0]=='#')
			continue;
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

		//        server_type = CN2500;
		sprintf(infop->mpt_name,"/proc/npreal2/%s",ttyname);
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
		infop->server_type = server_type;
		infop->disable_fifo = disable_fifo;
		infop->tcp_wait_id = 0;
		infop->tty_used_timestamp = 0;
		infop->first_servertime = 0;
#ifdef	SSL_ON
		infop->pssl = NULL;
		infop->ssl_enable = ssl_enable;
#endif
		infop++;
		ttys++;
	} /* while ( ttys < MAX_TTYS ) */

	/*
	 * Close configuration file:
	 */
	fclose(ConfigFd);
	if ( ttys == 0 )
		log_event("Have no any TTY configured record !");
	return(ttys);
}

