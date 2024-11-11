#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include "utils.h"
#include "deque.h"
#include "common.h"

/* Initializes the parameters needed by the client or server. */
void p_init(params *p, int q_capacity, int argc, char *argv[], void (*construct_addr)(struct sockaddr_in*, int, char*[])) {
    p->sockfd = make_nonblock_socket();
    memset(&p->pkt_send, 0, sizeof(packet));
    memset(&p->pkt_recv, 0, sizeof(packet));
    p->recv_seq = 0;
    p->send_seq = htonl(rand() & RANDMASK);
    p->recv_q = q_init(q_capacity);
    p->send_q = q_init(q_capacity);
    if (p->recv_q == NULL || p->send_q == NULL)
        die("queue initialization malloc failed");
    p->recv_ack = -1;
    p->ack_count = 0;
    p->before = clock();
    if (construct_addr != NULL)
        construct_addr(&p->addr, argc, argv);
}

/* Checks for a 1 second timeout since timer was last rest, and sends first packet in the send buffer, if any. */
void p_retransmit_on_timeout(params *p) {
    clock_t now = clock();
    // Packet retransmission
    if (now - p->before > CLOCKS_PER_SEC) { // 1 second timer
        p->before = now;
        // send the packet with lowest seq number in sending buffer, which is probably the top packet
        packet* send = q_front(p->send_q);
        // fprintf(stderr, "q size: %ld\n", q_size(send_q));
        if (send != NULL)
            send_packet(p->sockfd, &p->addr, send, "RTOS");
    }
}

bool p_send_packet_from_stdin(params *p) {
    if (q_full(p->send_q))
        return false;
    int bytes = read_stdin_to_pkt(&p->pkt_send);
    if (bytes <= 0)
        return false;
    p->pkt_send.ack = p->recv_seq;
    p->pkt_send.seq = p->send_seq;
    p->pkt_send.flags = PKT_ACK;
    q_push_back(p->send_q, &p->pkt_send);
    q_print(p->send_q, "SBUF");
    p->send_seq = htonl(ntohl(p->send_seq)+bytes);
    send_packet(p->sockfd, &p->addr, &p->pkt_send, "SEND");
    return true;
}

void p_retransmit_on_duplicate_ack(params *p) {
    // retransmit if 3 same acks in a row
    if (p->pkt_recv.ack == p->recv_ack) {
        p->ack_count++;
        if (p->ack_count == 3) {
            p->ack_count = 0;
            packet* send = q_front(p->send_q);
            if (send != NULL)
                send_packet(p->sockfd, &p->addr, send, "DUPS");
        }
    } else {
        p->recv_ack = p->pkt_recv.ack;
    }
}

/* Handles the incoming data packet. Returns true if printed out, and false if buffered. */
bool p_handle_data_packet(params *p) {
    if (p->pkt_recv.seq == p->recv_seq) { // write contents of packet if expected
        write_pkt_to_stdout(&p->pkt_recv);
        p->recv_seq = htonl(ntohl(p->recv_seq)+ntohs(p->pkt_recv.length)); // next packet

        // loop through sorted packet buffer and pop off next packets

        for (packet *pkt = q_front(p->recv_q);
                pkt != NULL && pkt->seq == p->recv_seq;
                pkt = q_pop_front_get_next(p->recv_q)) {
            write_pkt_to_stdout(pkt);
            p->recv_seq = htonl(ntohl(p->recv_seq)+ntohs(pkt->length));
        }
        return true;
    } else if (ntohl(p->pkt_recv.seq) > ntohl(p->recv_seq)) { // unexpected packet, insert into buffer
        q_try_insert_keep_sorted(p->recv_q, &p->pkt_recv);
        return false;
    }
}

void p_clear_acked_packets_from_sbuf(params *p) {
    packet *pkt = q_front(p->send_q);
    while (pkt != NULL && ntohl(pkt->seq) < ntohl(p->pkt_recv.ack))
        pkt = q_pop_front_get_next(p->send_q);
}

void p_send_empty_ack(params *p) {
    p->pkt_send.flags = PKT_ACK;
    p->pkt_send.ack = p->recv_seq;
    p->pkt_send.seq = 0;
    p->pkt_send.length = 0;
    send_packet(p->sockfd, &p->addr, &p->pkt_send, "SEND");
}

void p_listen(params *p) {
    p_retransmit_on_timeout(p);
    if (recv_packet(p->sockfd, &p->addr, &p->pkt_recv) <= 0) {
        p_send_packet_from_stdin(p);
    } else { // packet received
        // reset the timer
        p->before = clock();

        p_retransmit_on_duplicate_ack(p);

        p_clear_acked_packets_from_sbuf(p);

        if (p->pkt_recv.length == 0) return; // only payload packets need to be acknowledged

        p_handle_data_packet(p);

        // If the send queue isn't full yet and we have data to send, read data into payload
        // Or if we're responding to a syn packet
        if (!p_send_packet_from_stdin(p))
            p_send_empty_ack(p);
    }
}