#include <syslog.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/sysinfo.h>
#include "npreal2d.h"

int Gfglog_mode = 0;
int enable_ipv6 = 2;
int ttys, servers;
int polling_fd = -1;
int polling_nport_fd[2];
TTYINFO ttys_info[MAX_TTYS];
SERVINFO serv_info[MAX_TTYS];
#ifdef	SSL_ON
SSL_CTX *sslc_ctx;
#endif

void log_event(msg)
char *	msg;
{
	if (Gfglog_mode) fprintf (stderr, "%s\n", msg);
	else syslog (LOG_INFO, "%s", msg);
}

int	ipv4_str_to_ip(char *str, ulong *ip)
{
	int	i;
	unsigned long	m;

	/* if is space, I will save as 0xFFFFFFFF */
	*ip = 0xFFFFFFFFL;

	for (i = 0; i < 4; i++)
	{
		if ((*str < '0') || (*str > '9'))
			return NP_RET_ERROR;

		m = *str++ - '0';
		if ((*str >= '0') && (*str <= '9'))
		{
			m = m * 10;
			m += (*str++ - '0');
			if ((*str >= '0') && (*str <= '9'))
			{
				m = m * 10;
				m += (*str++ - '0');
				if ((*str >= '0') && (*str <= '9'))
					return NP_RET_ERROR;
			}
		}

		if (m > 255)
			return NP_RET_ERROR;

		if ((*str++ != '.') && (i < 3))
			return NP_RET_ERROR;

		m <<= (i * 8);

		if (i == 0)
			m |= 0xFFFFFF00L;
		else if ( i == 1 )
			m |= 0xFFFF00FFL;
		else if ( i == 2 )
			m |= 0xFF00FFFFL;
		else
			m |= 0x00FFFFFFL;

		*ip &= m;
	}

	return NP_RET_SUCCESS;
}

int	ipv6_str_to_ip(char *str, unsigned char *ip)
{
	int	i;
	char tmp[IP6_ADDR_LEN + 1];

	memset(ip, 0x0, 16);

	for (i = 0; i < IP6_ADDR_LEN; i++, str++)
	{
		if (((*str >= '0') && (*str <= '9')) ||
				((*str >= 'a') && (*str <= 'f')) ||
				((*str >= 'A') && (*str <= 'F')) || (*str == ':'))
			tmp[i] = *str;
		else
			break;
	}
	tmp[i] = '\0';

	if (!inet_pton(AF_INET6, tmp, ip))
		return NP_RET_ERROR;

	return NP_RET_SUCCESS;
}

/*
 * Initialize the polling Server UDP socket & server IP table.
 */
int poll_async_server_init()
{
	int			i, n, udp_port;
	struct sockaddr_in	sin;
	struct sockaddr_in6	sin6;
	struct sysinfo		sys_info;

	int family[] = {AF_INET, AF_INET6};
	struct sockaddr * ptr;
	int len;

	servers = 0;

	// This loop group ttys with a given sequence server id by IP address and update uptime for each ttys_info[].
	for ( i=0; i<ttys; i++ )
	{
		for ( n=0; n<servers; n++ )
		{
			// These loop will store IP for last tty port. It also align variable 'n' for later usage.
			if ( *(u_long*)serv_info[n].ip6_addr == *(u_long*)ttys_info[i].ip6_addr )
				break;
			else if(memcmp(serv_info[n].ip6_addr, ttys_info[i].ip6_addr, 16) == 0)
				break;
		}
		if ( n == servers )
		{
			// For each tty, it initiate server parameters here.
			sysinfo(&sys_info);
			ttys_info[i].serv_index = servers;
			if(ttys_info[i].af == AF_INET)
				*(u_long*)serv_info[servers].ip6_addr = *(u_long*)ttys_info[i].ip6_addr;
			else
				memcpy(serv_info[servers].ip6_addr, ttys_info[i].ip6_addr, 16);
			serv_info[servers].af = ttys_info[i].af;
			serv_info[servers].dev_type = 0;
			serv_info[servers].serial_no = 0;
			serv_info[servers].last_servertime = (time_t)((int32_t)(sys_info.uptime - 2));
			serv_info[servers].next_sendtime = (time_t)((int32_t)(sys_info.uptime - 1));
			serv_info[servers].ap_id = 0;
			serv_info[servers].hw_id = 0;
			serv_info[servers].dsci_ver= 0xFFFF;
			serv_info[servers].start_item= 0;
			servers++;
		}
		else
			ttys_info[i].serv_index = n; // Scott added: 2005-03-02
	}

	// Bind socket for polling NPort net status DSCI.
	for(i=0; i<2; i++)
	{
		ptr = (i == IS_IPV4)? (struct sockaddr*)&sin : (struct sockaddr*)&sin6;
		len = (i == IS_IPV4)? sizeof(sin) : sizeof(sin6);

		if ( (polling_nport_fd[i] = socket(family[i], SOCK_DGRAM, 0)) < 0 )
		{
			log_event("Can not open the polling_nport_fd socket !");
			if(i == IS_IPV6)
			{
				polling_nport_fd[1] = -1;
				enable_ipv6 = DIS_IPV6;
				break;
			}
			return(-1);
		}
		if(i == IS_IPV4)
		{
			sin.sin_family = AF_INET;
			sin.sin_port = 0;
			sin.sin_addr.s_addr = INADDR_ANY;
		}
		else
		{
			memset(&sin6, 0, sizeof(sin6));
			sin6.sin6_family = AF_INET6;
			sin6.sin6_port = 0;
		}
		if (bind(polling_nport_fd[i], ptr, len) == 0)
		{
#ifdef	FIONBIO
			fcntl(polling_nport_fd[i], FIONBIO);
#endif
		}
		else
		{
			for(n=0; n<=i; n++)
			{
				close(polling_nport_fd[n]);
				polling_nport_fd[n] = -1;
			}
			log_event("Can not bind the polling NPort UDP port !");
			return(-1);
		}
	}

	// Bind socket for polling Async Server.
	if ( (polling_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0 )
	{
		log_event("Can not open the polling UDP socket !");
		return(-1);
	}

	sin.sin_family = AF_INET;
	sin.sin_port = 0;
	sin.sin_addr.s_addr = INADDR_ANY;

	if (bind(polling_fd, (struct sockaddr*)&sin, sizeof(sin)) == 0)
	{
#ifdef	FIONBIO
		fcntl(polling_fd, FIONBIO);
#endif
	}
	else
	{
		close(polling_fd);
		polling_fd = -1; /* Add by Ying */
		for(i=0; i<2; i++)
		{
			close(polling_nport_fd[i]);
			polling_nport_fd[i] = -1;
		}
		log_event("Can not bind the polling UDP port !");
		return(-1);
	}
	return 0;
}

#ifdef	SSL_ON
#define CIPHER_LIST "ALL:@STRENGTH"
void ssl_init(void)
{
	SSLeay_add_ssl_algorithms();

#ifdef SSL_VER2
	sslc_ctx = SSL_CTX_new(SSLv2_client_method());
#else
#ifdef SSL_VER3
	sslc_ctx = SSL_CTX_new(SSLv3_client_method());
	;
#else
	sslc_ctx = SSL_CTX_new(SSLv23_client_method());
#endif
#endif

	SSL_CTX_set_options(sslc_ctx, SSL_OP_ALL|SSL_OP_NO_SSLv2);
	if( SSL_CTX_set_cipher_list(sslc_ctx, CIPHER_LIST) != 1 ){
		//sprintf(mm, "logger \"CFD> set_cipher_error %d, %s\"", __LINE__, __FUNCTION__);
		//system(mm);
	}

	/* For blocking mode: cause read/write operations to only return after the handshake and successful completion. */
	SSL_CTX_set_mode(sslc_ctx, SSL_MODE_AUTO_RETRY);
}
#endif

