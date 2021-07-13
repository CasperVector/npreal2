
#ifndef _REDUND_H
#define _REDUND_H

#define IPADDR2 1
#define BUF_SIZE 2048
#define REDUND_SIZE 1024 
#define CMD_REDUND_SIZE 84
#define HEADER_LEN 12
#define REDUNDANT_MARK 0xbeef
#define REDUNDANT_VERSION 0x01
#define REDUNDANT_HDRLEN 0x0c
#define REDUNDANT_PUSH 0x02
#define REDUNDANT_ACK 0x04
#define REDUNDANT_REPUSH 0x08
#define ACK_BIT 8 

struct redund_hdr
{
        uint16_t mark;
        uint8_t  version;
        uint8_t  hdr_len;
        uint8_t  flags;
        int8_t   session;
        uint16_t seq_no;
        uint16_t ack_no;
        uint16_t len;
};

struct redund_packet
{
        struct redund_hdr hdr;
        char   * data;
};

struct _redund_packet
{
        struct redund_hdr * hdr;
        char   * data;
};

struct expect_struct
{
        uint16_t ack;  /* Driver ack */
        uint16_t seq;  /* Driver seq */
        uint16_t last_seq;  /* Driver seq */
        uint16_t nport_ack; /* Nport ack */
    	uint16_t repush_seq[2]; /* repush_seq number */
};
struct redund_struct
{
	struct expect_struct data;
	struct expect_struct cmd;
	
	char redund_ip[40];
	u_char ip6_addr[16];
	int	sock_data[2];
	int	sock_cmd[2];

	int	data_open[2]; /* redund_open */
	int cmd_open[2];

	int connect[2]; /* redund_connect */
	int close[2]; /* redund_connect */

	//int	disdata[2];
	int reconnect[2]; /* redund_reconnect */

    uint16_t debug_seq;
	pthread_t thread_id[2];
	int thread[2];	

	int wlen;
	int rlen;
	int host_ack;
	int8_t session;
};

#endif /* _REDUND_H */
