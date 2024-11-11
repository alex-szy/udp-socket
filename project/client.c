#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <fcntl.h>
#include <time.h>
#include "utils.h"
#include "deque.h"
#include "common.h"


static void construct_serveraddr(struct sockaddr_in *serveraddr, int argc, char *argv[]) {
	// Construct server address
	serveraddr->sin_family = AF_INET; // use IPv4
	if (argc > 1 && strcmp(argv[1], "localhost"))
		serveraddr->sin_addr.s_addr = inet_addr(argv[1]);
	else
		serveraddr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	// Set sending port
	int PORT;
	if (argc > 2)
		PORT = atoi(argv[2]);
	else
		PORT = 8080;
	serveraddr->sin_port = htons(PORT);
}


int main(int argc, char *argv[]) {
	// Seed the random number generator
	srand(0);

	params p;
	p_init(&p, 20, argc, argv, construct_serveraddr);

	stdin_nonblock();

	// Create the SYN packet
	p.pkt_send.length = 0;
	p.pkt_send.seq = p.send_seq;
	p.pkt_send.flags = PKT_SYN;

	// Push the syn packet onto the queue and send it
	q_push_back(p.send_q, &p.pkt_send);
	send_packet(p.sockfd, &p.addr, &p.pkt_send, "SEND");
	p.send_seq = htonl(ntohl(p.send_seq)+1);

	for (;;) { // wait for syn ack
		p_retransmit_on_timeout(&p);
		if (recv_packet(p.sockfd, &p.addr, &p.pkt_recv) <= 0)
			continue;
		p.before = clock();
		if (p.pkt_recv.flags & PKT_ACK && p.pkt_recv.flags & PKT_SYN) { // syn ack packet
			p_clear_acked_packets_from_sbuf(&p);
			p.recv_seq = htonl(ntohl(p.pkt_recv.seq)+1);
			if (!p_send_packet_from_stdin(&p))
				p_send_empty_ack(&p);
			break;
		} else {
			send_packet(p.sockfd, &p.addr, q_front(p.send_q), "SEND"); // ack, not retransmit
		}
	}

	for (;;)
		p_listen(&p);
}
