#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "protocol.h"
#include "datalink.h"

#define MAX_SEQ 15 /* should be 2ˆn − 1 */
#define NR_BUFS ((MAX_SEQ + 1)/2)
#define DATA_TIMER 3000
#define ACK_TIMER 300

typedef enum { false, true } boolean; /* boolean type */

typedef unsigned char seq_nr; /* sequence or ack numbers */

typedef struct {
    unsigned char data[PKT_LEN];
}packet; /* packet definition */

typedef struct { /* frames are transported in this layer */
    unsigned char kind; /* what kind of frame is it? */
    seq_nr ack; /* acknowledgement number */
    seq_nr seq; /* sequence number */
    packet info; /* the network layer packet */
    unsigned int  padding;
} frame;

typedef struct node{
    seq_nr seq;
    struct node* next;
} node;


boolean no_nak = true; /* no nak has been sent yet */
static int phl_ready = 0;
node* frame_head = NULL;
node* frame_tail = NULL;

static inline void inc(seq_nr* k) {
    if (*k < MAX_SEQ)
        (*k)++;
    else  *k = 0;
}

static void put_frame(unsigned char* frame, int len)
{
    *(unsigned int*)(frame + len) = crc32(frame, len);
    send_frame(frame, len + 4);
    phl_ready = 0;
}

static boolean between(seq_nr a, seq_nr b, seq_nr c)
{
    /* Same as between in protocol 5, but shorter and more obscure. */
    return ((a <= b) && (b < c)) || ((c < a) && (a <= b)) || ((b < c) && (c < a));
}

static void add_node(unsigned int nr) {
    //lprintf("add timer %d\n", nr);
    node* new_node = (node*)malloc(sizeof(node));
    new_node->seq = nr;
    new_node->next = NULL;
    if (frame_head == NULL) {
        frame_head = new_node;
        frame_tail = new_node;
    }
    else {
        frame_tail->next = new_node;
        frame_tail = new_node;
    }
}

static void del_node(unsigned int nr) {
	//lprintf("delete timer %d\n", nr);
	node* cur = frame_head;
	node* pre = NULL;
	while (cur != NULL) {
		if (cur->seq == nr) {
			if (pre == NULL) {
				frame_head = cur->next;
			}
			else {
				pre->next = cur->next;
			}
			free(cur);
			return;
		}
		pre = cur;
		cur = cur->next;
	}
}

static void to_send_frame(unsigned char fk, seq_nr frame_nr, seq_nr frame_expected, packet buffer[])
{
    /* Construct and send a data, ack, or nak frame. */
    frame s; /* scratch variable */
    s.kind = fk; /* kind == data, ack, or nak */
    s.ack = (frame_expected + MAX_SEQ) % (MAX_SEQ + 1);
    if (fk == FRAME_DATA) {
        memcpy(s.info.data, buffer[frame_nr % NR_BUFS].data, PKT_LEN); /* fetch the packet */
        s.seq = frame_nr;
        put_frame((unsigned char*)&s, 3 + PKT_LEN);
        start_timer(frame_nr % NR_BUFS, DATA_TIMER);
		add_node(frame_nr);
        dbg_frame("Send DATA %d %d, ID %d\n", s.seq, s.ack, *(short*)s.info.data);
    }
    else if (fk == FRAME_ACK) {
        put_frame((unsigned char*)&s, 2);
        dbg_frame("Send ACK  %d\n", s.ack);
    }
    else {
        put_frame((unsigned char*)&s, 2);
        dbg_frame("Send NAK  %d\n", s.ack);
        no_nak = false; /* one nak per frame, please */
    }
    stop_ack_timer(); /* no need for separate ack frame */
}

int main(int argc, char** argv)
{
    protocol_init(argc, argv);
    lprintf("Designed by Sun Minghao, build: " __DATE__"  "__TIME__"\n");

    seq_nr ack_expected = 0; /* lower edge of sender’s window */
    seq_nr next_frame_to_send = 0; /* upper edge of sender’s window + 1 */
    seq_nr frame_expected = 0; /* lower edge of receiver’s window */
    seq_nr too_far = NR_BUFS; /* upper edge of receiver’s window + 1 */

    frame r; /* scratch variable */

    packet out_buf[NR_BUFS]; /* buffers for the outbound stream */
    packet in_buf[NR_BUFS]; /* buffers for the inbound stream */
    boolean arrived[NR_BUFS] = { false }; /* inbound bit map */
    seq_nr nbuffered = 0; /* how many output buffers currently used */

    int event, arg, len;

    disable_network_layer(); /* initialize */

    for (;;) {
        event = wait_for_event(&arg);

        /* five possibilities: see event type above */
        switch (event) {

            case NETWORK_LAYER_READY: /* accept, save, and transmit a new frame */
                nbuffered++; /* expand the window */
                get_packet(out_buf[next_frame_to_send % NR_BUFS].data); /* fetch new packet */
                to_send_frame(FRAME_DATA, next_frame_to_send, frame_expected, out_buf);/* transmit the frame */
                inc(&next_frame_to_send); /* advance upper window edge */
                break;

            case PHYSICAL_LAYER_READY:
                phl_ready = 1;
                break;

            case FRAME_RECEIVED: /* a data or control frame has arrived */
                len = recv_frame((unsigned char*)&r, sizeof r);
                if (len < 6 || crc32((unsigned char*)&r, len) != 0) {
                    dbg_event("**** Receiver Error, Bad CRC Checksum\n");
                    if (no_nak)
                        to_send_frame(FRAME_NAK, 0, frame_expected, out_buf);
				    break;
                }
                if (r.kind == FRAME_ACK)
                    dbg_frame("Recv ACK  %d\n", r.ack);

                if (r.kind == FRAME_DATA) {
                    dbg_frame("Recv DATA %d %d, ID %d\n", r.seq, r.ack, *(short*)r.info.data);
                    if ((r.seq != frame_expected) && no_nak)
                        to_send_frame(FRAME_NAK, 0, frame_expected, out_buf);
                    else start_ack_timer(ACK_TIMER);

                    if (between(frame_expected, r.seq, too_far) && (arrived[r.seq % NR_BUFS] == false)) {
                        /* Frames may be accepted in any order. */
                        arrived[r.seq % NR_BUFS] = true; /* mark buffer as full */
                        in_buf[r.seq % NR_BUFS] = r.info; /* insert data into buffer */

                        while (arrived[frame_expected % NR_BUFS]) {
                            /* Pass frames and advance window. */
                            put_packet(in_buf[frame_expected % NR_BUFS].data, PKT_LEN); /* pass packet to network layer */
                            no_nak = true;
                            arrived[frame_expected % NR_BUFS] = false;
                            inc(&frame_expected); /* advance lower edge of receiver’s window */
                            inc(&too_far); /* advance upper edge of receiver’s window */
                            start_ack_timer(ACK_TIMER); /* to see if a separate ack is needed */
                        }
                    }
                }

                if ((r.kind == FRAME_NAK) && between(ack_expected, (r.ack + 1) % (MAX_SEQ + 1), next_frame_to_send)) {
                    dbg_frame("Recv NAK  %d\n", r.ack);
                    del_node((r.ack + 1) % (MAX_SEQ + 1));
                    to_send_frame(FRAME_DATA, (r.ack + 1) % (MAX_SEQ + 1), frame_expected, out_buf);
                }
                while (between(ack_expected, r.ack, next_frame_to_send)) {
                    nbuffered--; /* handle piggybacked ack */
                    stop_timer(ack_expected % NR_BUFS); /* frame arrived intact */
				    del_node(ack_expected);
                    inc(&ack_expected); /* advance lower edge of sender’s window */
                }
                break;

            case DATA_TIMEOUT:
                dbg_event("---- DATA %d timeout\n", frame_head->seq);
                to_send_frame(FRAME_DATA, frame_head->seq, frame_expected, out_buf); /* we timed out */
                del_node(frame_head->seq);
                break;

            case ACK_TIMEOUT:
                dbg_event("---- ACK %d timeout\n", (frame_expected + MAX_SEQ) % (MAX_SEQ + 1));
                to_send_frame(FRAME_ACK, 0, frame_expected, out_buf); /* ack timer expired; send ack */
			    break;
        }

        if (nbuffered < NR_BUFS && phl_ready)
            enable_network_layer();
        else disable_network_layer();
    }
}
