// server.c

#ifndef SERVER_C
#define SERVER_C

#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <time.h>
#include <ctype.h>
#include <pthread.h>

#include "sdrproxy.h"

#define SERVERLOOPTIME 30 // ms

#define SRVR_IDLE_TIMEOUT 150 // Number of SERVERLOOPTIME cycles. Approx 5 seconds

// Local server states, and corresponding strings
enum servr_state { SRVR_STATE_INACTIVE, SRVR_STATE_INIT, SRVR_STATE_SENDCHAN, 
                   SRVR_STATE_SENDADC, SRVR_STATE_SHUTDOWN };
char *server_state_strings[] = {"SRVR_STATE_INACTIVE","SRVR_STATE_INIT","SRVR_STATE_SENDCHAN",
							"SRVR_STATE_SENDADC","SRVR_STATE_SHUTDOWN"};

/*
 * Following is a structure containing the IP address and server port of a remote client and the 
 *  time of the last packet sent to that client. Packet IDs aren't guaranteed to 
 *  monotonically increase with time so we can't use that to determine which packet 
 *  was last sent. Since packets come once per second (nominally), we can save the 
 *  UNIX time of the last packet sent. This is based on the time structure contained
 *  within the packet as sent from the server, and not from any other time. This is 
 *  used as a linked list.
 */
struct conn_hist_data {
    time_t last_update; // Unix time the lastID was updated. To manage expriries
    PACKETTIME last_packet_time; // Time of the last packet sent to the client.
    char *ip; // IP address of the client, as a string.
    int server_port; // Server server port used for the connection
    struct conn_hist_data *next;
};

// Last packet ID database management
enum lastiddbcmd_t { ID_ADD, ID_RETR, ID_DELETE, ID_REMOVE_OLD };

// structure to hold data about each remote client connection.
// connectiondata flags are as follows:
struct connectiondata {
    int csocket; // socket fd of the connection with the client.
    enum servr_state state,prev_state;
    long newest_packet_id; // id of newest packet sent to this client.
	struct sockaddr_storage their_addr;  // address of the client
	socklen_t addrsize; // length of the above address
    struct conn_hist_data *history; // Pointer to history record for this connection.
    int idle_timer; // how many times through the 30ms loop without a reponse from the 
				   //  client to shut it down
    struct ringset rcv_ring; // ring buffer for receivin from the remote client
    struct ringset send_ring; // ring buffer for sending to the remote client
};


/* ################# function prototypes ########## */
int service_connection(struct connectiondata *connections,int conn_no,fd_set *readfds, fd_set *writefds);
void server_setup(int *listeners,int max_listeners);
int put_channel_data(struct connectiondata *conn_data);
int put_adc_data(struct connectiondata *conn_data);
int send_to_client(struct connectiondata *conn_data);
int create_connection_data(struct connectiondata *conn_data);
int check_server_incoming(struct connectiondata *conn_data);
int server_rcv_to_ring(struct connectiondata *conn_data);
int get_new_connection(int *listener,int listener_no,struct conn_hist_data **first_id_lst,
        struct connectiondata *conn_data);
void *get_in_addr(struct sockaddr *sa);
in_port_t get_in_port(struct sockaddr *sa);
struct conn_hist_data *clhistory_lookup(struct conn_hist_data **first_id_lst,char *claddr, int cl_port, 
        time_t search_time,enum lastiddbcmd_t action);


/* ################# Functions ########## */
void 
*server_thread(void *thread_comm) {
    /*
     * Performs server operations for all connections. Sets up the server to listen on the desired 
     *  server port, monitors for connections from remote clients, looks for input from clients, 
     *  initiates responses, and sends out adc data stored in the data ring. Closes a 
     *  connection when the client does, and closes all connections when commanded by the 
     *  main thread (in response to a change in channel data). 
     *
     * This thread is started after a connection is made by our local client with the remote SDR 
     *  server, and channel information has been received. That information is needed in this 
     *  thread before we can send data to clients.
     *
     * The thread_comm structure is for communication with the main thread. The only communication 
     *  needed is for the main thread to send a command to shut down all remote client 
     *  connections due to a change in channel data. Changing channel data is major and can't be 
     *  done on the fly by the remote WinSDR clients. We close connections and force 
     *  the remote clients to reconnect and get new channel data.
     *
     * Data relevent to each active connection is stored in an array, CONN_DATA, and retrieved 
     *  by matching the file descriptor of the socket. Data about past connections to each 
     *  remote client is stored in a linked list, server_conn_history. 
     * Each client has its own send and receive data buffer, allocated dynamically when the 
     *  client connects and freed when the client connection is shut down.
     */
    // Communication with the main thread
    struct servercomm *main_comm = (struct servercomm *)thread_comm;

    fd_set readfds, writefds;
    int main_flags,conn_no,num_sockets,listener_no,packet_sent,server_fast_cycle;
    int fdmax; // Maximum file descriptor number
    int listenerfd[MAX_OUTGOING_PORTS]; // file descriptor for the listening socket
    struct timeval tv; // For loop timing
    struct conn_hist_data *first_id_lst; // Linked list of saved last packet ID numbers
    time_t expirery_time,prev_exp_time;
    // Pointer to the first item in the linked list.
    struct conn_hist_data *server_conn_history;
	char errorstr[101];

    // Array of connection data for each remote client connection
    struct connectiondata *conn_data;
    
    // Initialize first_id_lst to NULL for an empty list
    first_id_lst = NULL;

    // server_fast_cycle is the number of milliseconds to wait between packets to a client
    server_fast_cycle = 1000 / SVR_PKT_PER_SECOND;

	writelog(1,"server_thread: Starting server thread\n");

    /*
     * Allocate memory for the conn_data array, and initialize the array elements.
     */
	conn_data = (struct connectiondata *)calloc(MAXCLIENTCONNECTIONS,sizeof(struct connectiondata));
	if(conn_data == NULL) {
		fail("Couldn't allocate memory for the local server connection data array\n");
	}
    /*
     * Set all connections to the inactive state. The rest of the data will be initialized
     *  as connectiona are activated.
     */
    for(conn_no = 0; conn_no < MAXCLIENTCONNECTIONS; conn_no++) {
        conn_data[conn_no].state = SRVR_STATE_INACTIVE;
    }

    /*
     * Initialize the start pointer of the linked list of history records to NULL.
     */
    server_conn_history = NULL;

    // Set up the listening sockets, to listen for remote client connections.
    num_sockets = MAX_OUTGOING_PORTS;
    server_setup(listenerfd,num_sockets);

    /*
     * The main loop here polls each active connection to see what the status is, and monitors
     *  for new client connections. 
     */ 
    packet_sent = FALSE;
    prev_exp_time = 0;
    writelog(3,"server_thread: Entering main loop\n");
    for (;;) { // main loop.
        /*
         * If the main thread is requesting that all clients shut down, do it.
         *  This will close all active connections and return (terminate the server
         *  thread).
         * Note that this is process is to respond to the main thread requesting a shutdown.
         *  Another mechanism in the main server loop does essentially the same thing in response
         *  to the remote client closing a connection, or from other connection problems.
         */
        pthread_mutex_lock(&mutex_servercomm );
        main_flags = main_comm->flags;
        pthread_mutex_unlock(&mutex_servercomm );
        if(main_flags & SRV_CLOSECMD) {
            for (conn_no = 0; conn_no < MAXCLIENTCONNECTIONS; conn_no++) {
                if(conn_data[conn_no].state != SRVR_STATE_INACTIVE) {
                    close(conn_data[conn_no].csocket);
                    conn_data[conn_no].state = SRVR_STATE_INACTIVE;
                }
            }
			// Now close the listening sockets.
			for (listener_no = 0; listener_no < MAX_OUTGOING_PORTS; listener_no++) {
				if(close(listenerfd[listener_no]) < 0) {
					perror("server_thread: close");
					fail("server_thread: close listening socket failed on connection %d\n",
						listener_no);
				}
			}
            // Now reset the shutdown flag. We've done our job.
            pthread_mutex_lock(&mutex_servercomm );
            main_comm->flags &= ~SRV_CLOSECMD;
            pthread_mutex_unlock(&mutex_servercomm );
            return NULL; // This function (server_thread must return (void *)
        }

        /*
         * Check for sockets that have something interesting.
         * fdmax keeps track of the largest socket file descriptor in use, for use with socket calls.
         *  Since the listener socket never changes and it was created before any of the remote client
         *  sockets, we can repeatedly start with listener as the highest-numbered socket, then add
         *  remote client sockets from the list of active sockets.
         */
        FD_ZERO(&writefds);
        FD_ZERO(&readfds);
        for (listener_no = 0; listener_no < MAX_OUTGOING_PORTS; listener_no++) {
            FD_SET(listenerfd[listener_no],&readfds);
            fdmax = max(fdmax,listenerfd[listener_no]);
        }
        /*
         * If there are active remote client connections, go through them and add to 
         *  the read and write fds's for checking by select
         */
		for (conn_no = 0; conn_no < MAXCLIENTCONNECTIONS; conn_no++) {
			if(conn_data[conn_no].state != SRVR_STATE_INACTIVE) {
				FD_SET(conn_data[conn_no].csocket,&readfds);
				FD_SET(conn_data[conn_no].csocket,&writefds);
				fdmax = max(fdmax,conn_data[conn_no].csocket);
			}
		}

        /*
         * Set the timeout and call select. We use fast or slow loop times, depending on whether
         *  a packet was sent out last cycle. The idea is that we can rapidly send out data if we 
         *  have the connection and packets to send.
         * Select will almost certainly return right away when there are remote clients to which
         *  we can write. We usually only have something to write once per second, so we'll need
         *  to do the rest of the delay at the end of the loop.
         */
        if(packet_sent) {
            TV_SET(&tv,server_fast_cycle);
        } else {
            TV_SET(&tv,SERVERSLOWCYCLE);
        }

        if(select(fdmax+1,&readfds,&writefds,0,&tv) == -1) {
            fail("server_thread: select failed - %s\n",strerror_r(errno,errorstr,100));
        }

        /*
         * Check each connection to see if it needs attention. If any connection sends out a packet,
         *  cycle quickly to send another one. This should speed up the process of sending the initial
         *  data to the WinSDR clients.
         */
        for (conn_no = 0; conn_no < MAXCLIENTCONNECTIONS; conn_no++) {
            packet_sent |= service_connection(conn_data,conn_no,&readfds,&writefds);
        }


        // Check to see if there are any new connections on the listening sockets
        for (listener_no = 0; listener_no < MAX_OUTGOING_PORTS; listener_no++) {
            if(FD_ISSET(listenerfd[listener_no],&readfds)) {
                // New connection. Set it up if possible.
                get_new_connection(listenerfd,listener_no,&first_id_lst,conn_data);
            }
        }

        /*
         * Exipre old connection checkpoints. We don't need to save history for longer than we 
         *  have data in the data ring. For a client with older data than that, we'd just send them all
         *  the data in the data ring anyway.
         */
        expirery_time = time(NULL) - DATASIZE;
        if(expirery_time != prev_exp_time) {
            clhistory_lookup(&server_conn_history,NULL,0,expirery_time,ID_REMOVE_OLD);
            prev_exp_time = expirery_time;
        }

		// Wait for the remainder of our timer
        select(0,NULL,NULL,NULL,&tv);
	}
}

int 
service_connection(struct connectiondata *connections,int conn_no,fd_set *readfds, fd_set *writefds) {
    /*
     * Handles processing of the indicated conenction. See if it needs attention.
     *  This can be because select has identified one as having data that can be read, or one to 
     *  which data can be written. Returns 1 if a data packet was sent to the client, 0 otherwise.
     */
    int rv,bytes_to_send,bytes_sent,packet_sent;

    // If the STATE for this client has changed, log it and update the PREV_STATE.
    if(print_state("server_thread",connections[conn_no].state,connections[conn_no].prev_state,
        server_state_strings)) 
    {
            connections[conn_no].prev_state = connections[conn_no].state;
    }
    packet_sent = FALSE;
    if(connections[conn_no].state != SRVR_STATE_INACTIVE) {
        // Check if the remote client has something to say
        if(FD_ISSET(connections[conn_no].csocket,readfds)) {
            /*
             * The remote client has data to be read. If there is an error, 
             *  server_rcv_to_ring will return either -1 (reset) or -2 
             *  (socket closed). In either case we shut down the socket and 
             *  force the client to reconnect.
             */
            rv = server_rcv_to_ring(&connections[conn_no]);
            if(rv < 0) {
                // Problem with the connection. Set the shutdown flag.
                connections[conn_no].state = SRVR_STATE_SHUTDOWN;
            } else if(rv > 0) {
                // The remote client sent us something. Check to see what it was
                if(check_server_incoming(&connections[conn_no]) == 1) {
                    connections[conn_no].state = SRVR_STATE_SENDCHAN;
                }
            }
        }

        switch (connections[conn_no].state) {
            case SRVR_STATE_INACTIVE: // Do nothing
                break;
            case SRVR_STATE_SHUTDOWN: 
                /*
                 * The client connection needs to be shut down. Close the socket, 
                 *  save the current status in the database of last send packet IDs, 
                 *   and free the allocated buffers for the client connection
                 */
                close(connections[conn_no].csocket);
                connections[conn_no].state = SRVR_STATE_INACTIVE;
                warn("Closed connection %d to client at %s\n",
                        conn_no,connections[conn_no].history->ip);
                writelog(2,"Closed connection %d to client %s\n",
                        conn_no,connections[conn_no].history->ip);
                break;
            case SRVR_STATE_SENDCHAN:
                /*
                 * Send the channel information to the remote client. All we do here
                 *  is put it in the outgoing ring buffer. It will be written later.
                 */
                if(put_channel_data(&connections[conn_no]) == 0) {
                    connections[conn_no].state = SRVR_STATE_SENDADC;
                }
                break;
            case SRVR_STATE_SENDADC:
                // Put any new ADC data on the outgoing ring buffer.
                packet_sent = put_adc_data(&connections[conn_no]);
                break;
            case SRVR_STATE_INIT:
                // If we've been in the SRVR_STATE_INIT state for too long waiting for the 
                // "SEND AD" instruction from the client, shut it down. Otherwise just 
                // decrement the timer.
                if(connections[conn_no].idle_timer <= 0) {
					writelog(4,"Waited too long for SEND AD from client on connection %d. "
						"from %s on port %d. Sutting it down\n",
						conn_no,connections[conn_no].history->ip,
						connections[conn_no].history->server_port);
                    connections[conn_no].state = SRVR_STATE_SHUTDOWN;
                } else {
                    connections[conn_no].idle_timer--;
				}
                break;
            default:
                fail("server_thread: conn_data state invalid: %s\n",
                        server_state_strings[connections[conn_no].state]);
                break;
        }

        // If there is data to send, the state of this connection is to send channel or adc data,
		//  and the remote client is able to receive, try sending
        bytes_to_send = ring_ops(NULL,0,RING_ELEMENTS_STORED,&connections[conn_no].send_ring);
        if(bytes_to_send && FD_ISSET(connections[conn_no].csocket,writefds) &&
			((connections[conn_no].state == SRVR_STATE_SENDCHAN) || 
			(connections[conn_no].state == SRVR_STATE_SENDADC))) 
		{
            bytes_sent = send_to_client(&connections[conn_no]);
            if(bytes_sent >= 0) {
                writelog(4,"service_connection: Sent %d bytes on connection %d to client "
                        "at %s on server port %d. %d bytes left to send\n",bytes_sent,conn_no,
                        connections[conn_no].history->ip,
                        connections[conn_no].history->server_port,bytes_to_send - bytes_sent);
            } else {
                writelog(4,"service_connection: Did not send any data. %d bytes left to send\n",
                        bytes_to_send);
            }
        }
    }
    return packet_sent;
}

void 
server_setup(int *listeners,int max_listeners) {
    // Initializes the server socket interface and returns the file descriptor for 
    //  the listening socket
    struct addrinfo hints, *res, *p;
    int listener_no;
	int yes;
    char listenerport[50];
	char our_addr[INET6_ADDRSTRLEN],*ipver;
	char errorstr[101];
	void *addr;

    for (listener_no = 0; listener_no < max_listeners; listener_no++) {
		writelog(5,"server_setup: Setting up listener %d on port %d\n",
			listener_no,SERVERPORTNO + listener_no);
        sprintf(listenerport,"%d",SERVERPORTNO + listener_no);
        memset(&hints,0,sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;
        if(getaddrinfo(NULL,listenerport,&hints, &res) != 0) {
            fail("server_setup: getaddrinfo failed with %s\n", gai_strerror(errno));
        }
        // Search the list for a valid socket.
        for (p = res; p != NULL; p = p->ai_next) {
            listeners[listener_no] = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            if(listeners[listener_no] == -1) {
                fail("server_setup: socket failed -- %s\n",strerror_r(errno,errorstr,100));
            }
			
            // From Beej's networking guide: Supposed to lose the pesky "address already in use" error message
			//  But doesn't do anything
			yes = 1;
            if(setsockopt(listeners[listener_no], SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
				writelog(2,"server_setup: Setting SO_REUSEADDR in setsockopt failed\n");
			}

			// Kludge to conver IP address to a string
			if(p->ai_family == AF_INET) { // IPv4
				struct sockaddr_in *ipv4 = (struct sockaddr_in *)p->ai_addr;
				addr = &(ipv4->sin_addr);
				ipver = "IPv4";
			} else { // IPv6
				struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)p->ai_addr;
				addr = &(ipv6->sin6_addr);
				ipver = "IPv6";
			}
			// Convert the IP address to a string
			inet_ntop(p->ai_family, addr, our_addr, sizeof our_addr);
			writelog(5,"server_setup: Attempting to bind to port %d on local address %s\n",
				SERVERPORTNO + listener_no,our_addr);
            if(bind(listeners[listener_no], p->ai_addr, p->ai_addrlen) < 0) {
                // Couldn't bind to this one. Try another.
				writelog(5,"server_setup: bind failed on address %s due to error %d: %s. Trying another\n",
					our_addr,errno,strerror_r(errno,errorstr,100));
				perror("server_setup: bind");
                close(listeners[listener_no]);
                continue;
            }
            // Now we've should have a good one.
            break;
        }
        // See if we got bound
        if(p == NULL) {
            fail("server_setup: Failed to bind to a local address on port %d\n",
				SERVERPORTNO + listener_no);
        }
        // All done with the local address info
        freeaddrinfo(res);

        if(listen(listeners[listener_no],MAXCLIENTCONNECTIONS) == -1) {
            fail("server_setup: listen failed %s\n",strerror_r(errno,errorstr,100));
        }
        writelog(0,"server_setup: Server number %d now listening on server port %s\n",
                listener_no,listenerport);
    }
}

int 
put_channel_data(struct connectiondata *conn_data) {
	/*
	 * If there is room on the ring, puts the channel data on the ring buffer
	 * Returns -1 if there was no room on the ring. Returns 0 on success.
	 */
	char bfr[CHANINFOLEN+1];
	if(ring_ops(NULL,0,RING_ELEMENTS_FREE,&conn_data->send_ring) >= CHANINFOLEN) {
		sprintf(bfr,"SPS: %3d Chans: %1d Type: %1d",chan_info_g.sps,chan_info_g.num_channels,
			chan_info_g.type);
        writelog(4,"put_channel_data: Loaded outgoing ring with %d bytes of channel data\n",
                strlen(bfr));
		ring_ops(bfr,CHANINFOLEN,RING_STORE,&conn_data->send_ring);
		return 0;
	}
	return -1;
}

int 
put_adc_data(struct connectiondata *conn_data) {
	/*
     * Searches for a packet with a time after the time of the last packet sent. If one is
     *  available and if there is room on the send ring, puts it on the send ring.
	 * Return 0 if there was none available or if there was no room on the ring. Return
	 *  1 on success.
	 */
    int ring_ret,send_avail;
	char bfr[MAXPACKETSIZE];
	struct packetinfo element;
    char packet_time[50];

	// First see if a packet is available for the time we need, one second past the last time.
	ring_ret = data_ring_ops(&element,bfr,conn_data->history->last_packet_time,RING_RETR);
	if(ring_ret) {
		// Nothing available to meet our time requirements
        pkt_time_str(conn_data->history->last_packet_time,packet_time);
        writelog(7,"put_adc_data: No data packet to send to %s on server port %d after time %s\n",
                conn_data->history->ip,conn_data->history->server_port,packet_time);
		return 0;
	}
	// Now we have the element we want. So see if it will fit on the outgoing ring.
    pkt_time_str(element.packet_time,packet_time);
    writelog(5,"put_adc_data: Found packet id %ld length %d at time %s to send to %s on server port %d\n",
            element.packet_id,element.packet_len,packet_time,conn_data->history->ip,
            conn_data->history->server_port);
    send_avail = ring_ops(NULL,0,RING_ELEMENTS_FREE,&conn_data->send_ring);
	if(element.packet_len > send_avail) {
		// Won't fit. Return to allow some data to be sent to the client.
        writelog(5,"put_adc_data: Packet of length %d won't fit on send ring, which only has "
                "%d bytes available\n",element.packet_len,send_avail);
		return 0;
	}
	// Put the packet on the send ring, save the time of the last packet, 
    // and set the last_update time to the current unix time.
	ring_ops(bfr,element.packet_len,RING_STORE,&conn_data->send_ring);
    pkt_time_str(element.packet_time,packet_time);
    writelog(3,"put_adc_data: Placed packet of time %s on send ring for %s on server port %d, and updated history\n",
            packet_time,conn_data->history->ip,conn_data->history->server_port);
    conn_data->history->last_packet_time = element.packet_time;
    conn_data->history->last_update = time(NULL);
	return 1;
}

int
send_to_client(struct connectiondata *conn_data) {
	/*
	 * Sends the channel information to the remote client. The socket has already 
     *  been checked and is able to receive data and the send ring has data to be 
     *  sent. Send as much data as we can to the remote client and return.
	 * Return Values:
	 *  -1 -- Got SIGPIPE from the socket. Must close it.
	 *  -2 -- Some other error on sending. Also must close it.
	 *   0 -- No bytes sent but there was no problem. Can try again.
	 *  >0 -- Number of bytes sent.
	 */
	char sendbfr[MAXPACKETSIZE];
	int bytes_to_send,rv;
	char errorstr[101];

	bytes_to_send = ring_ops(NULL,0,RING_ELEMENTS_STORED,&conn_data->send_ring);
    ring_ops(sendbfr,bytes_to_send,RING_VIEW,&conn_data->send_ring);
	rv = send(conn_data->csocket,sendbfr,bytes_to_send,MSG_DONTWAIT | MSG_NOSIGNAL);
	if(rv <= 0) {
		// Error on send. See what it was.
		if((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
			// Non-fatal error. Return to try again.
			return 0;
		} else if (errno == EPIPE) {
			// Would have been SIGPIPE but we aren't set up to receive signals. Return -1.
			return -1;
		} else {
			// Some other (fatal) error.
			fail("send_to_client: Error on sending -- %s\n",strerror_r(errno,errorstr,100));
			return -2; // For completeness
		}
	}
	// Sent at least some of the bytes. Remove them from the ring and return.
	ring_ops(NULL,rv,RING_DELETE,&conn_data->send_ring);
	return rv;
}

int
create_connection_data(struct connectiondata *conn_data) {
    /*
     * Finds an unused connectiondata element, if one exists, allocates the buffers
	 *  and returns the index of the element. If there isn't one available, returns -1.
     */
    int conn_no, send_bfr_size;

	// Find the first inactive connection. If we didn't find one, return NULL.
    conn_no = 0;
    while((conn_no < MAXCLIENTCONNECTIONS) && 
		(conn_data[conn_no].state != SRVR_STATE_INACTIVE)) 
	{
        conn_no++;
    }
	if(conn_no >= MAXCLIENTCONNECTIONS) {
		writelog(0,"create_connection_data: Connection failed. No more connections available\n");
		return -1;
	}
	
	/*
     * Allocate for the send and receive rings. We calculate the size of the send 
	 *  buffer based on the channel data (sample rate and number of channels), and 
	 *  assume 4 bytes per sample. 
     *  We use a fixed length receive buffer. 
     * Allocate the buffer for the ring one longer than the amount of data to be stored.
	 */
    ring_ops(NULL,SERVERRCVBFRSIZE,RING_ALLOC,&conn_data[conn_no].rcv_ring);

    // The send_bfr_size is the size of the preheader plus the size of the data header 
	//  plus the data size (number of channels times the number of samples per second) 
	//  plus one for the checksum.
    send_bfr_size = PREHDRLEN + DATAHDRLEN + chan_info_g.sps * 
        chan_info_g.num_channels * ADCDATASIZE + 1;
    ring_ops(NULL,send_bfr_size,RING_ALLOC,&conn_data[conn_no].send_ring);

    writelog(3,"create_connection_data: Successfully created new connection and "
            "allocated send and receive rings\n");
    return conn_no;
}

int 
check_server_incoming(struct connectiondata *conn_data) {
    /*
     * Checks for known messages from the client stored on our recv ring. If there's something
     *  else, log it. Returns 1 if SEND AD was received, 0 otherwise. More return codes
     *  should be used for other things which are detected.
     */
    int rpos,ind; 
    char inbfr[SERVERRCVBFRSIZE+1];
    int found = 0;

	/*
	 * Search for either the normal WinSDR channel request ("SEND AD", or the 
	 *  sdrproxy request: "SENDXAD ", followed by a number representing the unix time of 
	 *  the last packet received.
	 */
    rpos = search_ring(CHANREQUEST,&conn_data->rcv_ring);

    if (rpos >= 0) {
        // Found a channel request from the client. 
        // If there was anything on the ring prior to the channel request,
        //  send it to the log and then dump it.
        if(rpos > 0) {
            // There was stuff on the ring before the channel request string. Dump it to the 
            //  log and remove it from the ring. We get the stuff from the ring, test to see if
            //  it's printable, then dump it to the log.
            ring_ops(inbfr,rpos,RING_RETR,&conn_data->rcv_ring);

            for (ind = 0; ind < rpos;ind++) {
                if(!isprint(inbfr[ind])) {
                    // Change non-printables to dots
                    inbfr[ind] = '.';
                }
            }
            // Null-terminate the buffer and write it to the log.
            inbfr[rpos] = '\0';
            writelog(4,"check_server_incoming: Found unknown stuff from the client at " \
                    "%s on server port %d -- before a channel request string %s",
                    conn_data->history->ip,conn_data->history->server_port,inbfr);
        }
        // Remove the channel request string from the ring.
        ring_ops(NULL,CHANREQUESTLEN,RING_DELETE,&conn_data->rcv_ring);
        found = 1;
    } else {
        // There was no channel request on the ring. If the receive ring has more 
        //  than the number of bytes in a channel request, dump the first part of the 
        //  buffer to the log, but leave enough on the ring for a channel request.
        // We want to examine this stuff to see what the clients are sending.
        rpos = ring_ops(NULL,0,RING_ELEMENTS_STORED,&conn_data->rcv_ring);
        if(rpos > CHANREQUESTLEN) {
            // More than CHANREQUESTLEN bytes on the ring
            ring_ops(inbfr,rpos - CHANREQUESTLEN,RING_RETR,&conn_data->rcv_ring);
            for (ind = 0; ind < (rpos - CHANREQUESTLEN); ind++) {
                // Make all characters printable by replacing bad ones with a period.
                if(!isprint(inbfr[ind])) {
                    inbfr[ind] = '.';
                }
            }
            // Null-terminate the buffer and write it to the log.
            inbfr[ind] = '\0';
            writelog(4,"check_server_incoming: Found unknown stuff from the client at %s on server port %d -> %s",
                    conn_data->history->ip,conn_data->history->server_port,inbfr);
        }
    }
    return found;
}

int
server_rcv_to_ring(struct connectiondata *conn_data) {
    /*
     * This fucntion is called when select indicates there is something interesting from
     * the remote client. We check to see if there is data, and if so put it in the receive 
     *  ring for this client. Or it may be that the client closed the connection.
     * Return values:
     *  -2 -- Remote client has closed the socket
     *  -1 -- Connection error
     *   0 -- Got no bytes, but ok to try again
     *   >0 -- Number of bytes placed in the ring.
     */
    int rv,free_bytes;
    char bfr[SERVERRCVBFRSIZE];
	char errorstr[101];

    writelog(7,"server_rcv_to_ring: Getting bytes from client into ring buffer\n");

    // See how many bytes are free in the receive ring, but not more than our local buffer.
    free_bytes = min(SERVERRCVBFRSIZE,ring_ops(NULL,0,RING_ELEMENTS_FREE,&conn_data->rcv_ring));
    if(free_bytes == 0) {
        return 0;
    }
    rv = recv(conn_data->csocket,bfr,free_bytes,0);
    if(rv < 0) {
        // Error condition on recv.
        if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
            // Try again
            return 0;
        }
        // Other error
        writelog(0,"server_rcv_to_ring: Error in recv: %s\n",strerror_r(errno,errorstr,100));
        return -1;
    } else { // !(rv < 0)
        if (rv > 0) {
            // Got some bytes. Store them in the ring and return.
            writelog(2,"server_rcv_to_ring: Got %d bytes from remote client at %s, on server port %d "
				"to ring buffer\n",
                    rv,conn_data->history->ip,conn_data->history->server_port);
            ring_ops(bfr,rv,RING_STORE,&conn_data->rcv_ring);
            return rv;
        } else { // rv == 0. Connection closed by peer.
            writelog(2,"server_rcv_to_ring: Connection closed by peer at %s on server port %d\n",
				conn_data->history->ip,conn_data->history->server_port);
            return -2;
        }
    }
}

int
get_new_connection(int *listeners,int listener_no,struct conn_hist_data **first_id_lst, 
					struct connectiondata *conn_data) 
{
    /*
     * Opens a new connection to a remote client. This is called after a connection request has 
     * been detected by the listening socket with select. This function accepts the connection and,
     *  if that is successful, sets up the client connection data record for this connection,
     *  and allocates the send and receive buffers.
     * It also checks for the presence of a history record for this client and if found links it to the 
     *  connection data record. If not found, creates a new one.
     * Returns 0 if the connection is successful,
     *  Closes the client connection and returns -1 if the maximum number of clients has 
	 *  already been reached. Also returns -1 if the connection fails. No buffer or data 
	 *  space is allocated if the connection fails.
     */
    int newfd;
    struct sockaddr_storage remote_addr;
    socklen_t remote_addr_len;
    char remote_ip[INET6_ADDRSTRLEN];
    int server_port;
    int new_conn; // Index of the record to use for the new connection
    struct conn_hist_data *client_hist_record;
    char packet_time_str[50];

    // First accept the connection
    remote_addr_len = sizeof(remote_addr);
    newfd = accept(listeners[listener_no], (struct sockaddr *)&remote_addr, &remote_addr_len);
    if(newfd == -1) {
        writelog(2,"get_new_connection: Accept of new connection failed\n");
        return -1;
    }
	// Decode the remote IP address.
    inet_ntop(remote_addr.ss_family,get_in_addr((struct sockaddr *)&remote_addr),
            remote_ip,INET6_ADDRSTRLEN);
    server_port = SERVERPORTNO + listener_no;

	/*
	 * Call create_connection_data to find an inactive connectiondata record to use. It allocates 
     *  the send and receive buffers and returns the index of the connectiondata record. We'll 
     *  have to do the rest.
	 */
    new_conn = create_connection_data(conn_data);
    if(new_conn < 0) {
		/*
         * Unable to complete the connection record, probably due to too many connections already.
         * Close the socket with the remote client and return -1.
         */
		close(newfd);
		return -1;
	}

    writelog(0,"get_new_connection: Accepted connection %d from %s, on server port %d, "
            "on socket %d\n",new_conn,remote_ip,server_port,newfd);
    warn("Accepted connection %d from %s, on server port %d, on socket %d\n",
            new_conn,remote_ip,server_port,newfd);

    // Look up the last packet sent in the database of last packet IDs
    client_hist_record = clhistory_lookup(first_id_lst,remote_ip,server_port,0, ID_RETR);
    if(client_hist_record == NULL) {
        // Not found in the database. Create a new record in the database and start at zero.
        client_hist_record = clhistory_lookup(first_id_lst,remote_ip,server_port,0,ID_ADD);
    } else { // Packet time was found in previous record for this client.
        pkt_time_str(client_hist_record->last_packet_time,packet_time_str);
        writelog(4,"get_new_connection: Found existing data for this client at %s on server port %d. "
                "Last packet sent was at %s\n",remote_ip,server_port,packet_time_str);
        /*
         * Set the last packet time back to the start of the minute MINUTES_TO_BACKUP minutes prior 
		 *  to its actual time, but go back one more minute if it's within SECONDS_OVERLAP seconds 
		 *  of the even minute.. This is so that when a client re-connects WinSDR can start at an 
		 *  even minute mark. When WinSDR terminates it seems that it doesn't save anything within 
		 *  the last minute of its termination, and a bit into the next minute. This seems to fix 
		 *  the problem most of the time.
         */
        client_hist_record->last_packet_time =
            ((client_hist_record->last_packet_time - SECONDS_OVERLAP) / (MINUTES_TO_BACKUP * 60) *
			(MINUTES_TO_BACKUP * 60));
        pkt_time_str(client_hist_record->last_packet_time,packet_time_str);
        writelog(4,"get_new_connection: Setting last packet time back to an even %d minutes: %s\n",
			MINUTES_TO_BACKUP,packet_time_str);
    }

    // Set the last_update time to current time.
    client_hist_record->last_update = time(NULL); // Assign current time.

    /*
     * Set up the needed fields in the connection record.
     */
    conn_data[new_conn].csocket = newfd;
    conn_data[new_conn].state = SRVR_STATE_INIT;
    conn_data[new_conn].prev_state = SRVR_STATE_INACTIVE;
    conn_data[new_conn].newest_packet_id = 0L;
    conn_data[new_conn].idle_timer = SRVR_IDLE_TIMEOUT;
    memcpy(&conn_data[new_conn].their_addr,&remote_addr,remote_addr_len);
    conn_data[new_conn].addrsize = remote_addr_len;
    conn_data[new_conn].history = client_hist_record; // Link the history record and connection record.

    // Success
    return 0;
}

void 
*get_in_addr(struct sockaddr *sa) {
    // Returns a pointer to the address, considering IPv4 vs IPv6
    //  Copied from Beej's network guide.
    if(sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in *)sa)->sin_addr);
    } else {
        return &(((struct sockaddr_in6 *)sa)->sin6_addr);
    }
}

in_port_t get_in_port(struct sockaddr *sa) {
    // Returns the port number in internet byte order, considering IPv4 vs IPv6
    //  Similar to what Beej's network guide did for IP addresses.
    if(sa->sa_family == AF_INET) {
        return ((struct sockaddr_in *)sa)->sin_port;
    } else {
        return ((struct sockaddr_in6 *)sa)->sin6_port;
    }
}

struct conn_hist_data
*clhistory_lookup(struct conn_hist_data **server_conn_history,char *claddr, int svr_port, 
        time_t search_time,enum lastiddbcmd_t action) 
{
    /*
     * Manages a linked list of server history. These are accessed by client IP 
	 *  address and port for addition, retrieval, and deletion, or by date of last update for 
     *  removing old ones.. Does the following, based on the action:
     *   ID_ADD -- Checks that the given client address isn't in the database, and adds it to 
     *     the end if not. In either case, it returns a pointer to the item with the 
     *     given client address and port.
     *   ID_RETR -- Checks the database for the given client address and port, and returns a pointer 
     *     to the item. If not found, returns NULL.
     *   ID_DELETE -- Removes all items in the database with the given client address and port. 
	 *     Returns NULL.
     *   ID_REMOVE_OLD -- Removes all items in the database with accessDate older than the 
     *     given search_time. Returns NULL.
     */
    struct conn_hist_data *cur,*prev;
    char tmpdate[50];
    int cnt;

    switch (action) {
        case ID_ADD: // Add new element if not already there. 
            writelog(4,"clhistory_lookup: Adding %s port %d\n",claddr,svr_port);
            // Check for the presence of the given element. If found,
            //  returns a pointer to it. If not found, adds the element and returns a 
			//  pointer to it.
            cur = *server_conn_history;
            cnt = 0;
            while ((cur != NULL) && (strcmp(cur->ip,claddr) || (cur->server_port != svr_port))) {
                prev = cur;
                cur = cur->next;
                cnt++;
            }
            if(cur != NULL) { 
                // Already in the database
                writelog(4,"clhistory_lookup: Asked to ADD, but record was already in the "
                        "database. Num %d in list.\n",cnt);
                return cur;
            } else { // cur == NULL
                // Not found. Add it.
                cur = (struct conn_hist_data *)malloc(sizeof(struct conn_hist_data));
                cur->ip = strdup(claddr);
                cur->server_port = svr_port;
                cur->last_packet_time = 0.0; // Initialize the packet time.
                cur->next = NULL; // Positioned at end of list.
                if(*server_conn_history == NULL) {
                    // First one
                    writelog(4,"clhistory_lookup: Adding first in database\n");
                    *server_conn_history = cur;
                } else {
                    // Not the first one
                    writelog(4,"  Adding to database at the end. No %d in the list\n",cnt+1);
                    prev->next = cur;
                }
                return cur;
            }
            break;
        case ID_RETR: // Recall first one with client address matching claddr and svr_port
            writelog(4,"clhistory_lookup: Retrieving %s port %d\n",claddr,svr_port);
            cur = *server_conn_history;
            while ((cur != NULL) && (strcmp(cur->ip,claddr) || (cur->server_port != svr_port))) {
                cur = cur->next;
            }
            return cur;
            break;
        case ID_DELETE: // Remove any with client address matching claddr and svr_port
            writelog(4,"clhistory_lookup: Removing any with client address %s and server port %d\n",
                    claddr,svr_port);
            cur = *server_conn_history;
            while(cur != NULL) {
                if (!strcmp(cur->ip,claddr) && (cur->server_port == svr_port)) {
                    // Found one, remove it.
                    if(*server_conn_history == cur) {
                        // First one
                        writelog(4,"clhistory_lookup: Removing the first one\n");
                        *server_conn_history = cur->next;
                        free(cur->ip);
                        free(cur);
                        cur = *server_conn_history;
                    } else {
                        // Not the first one
                        writelog(4,"clhistory_lookup: Removing other than the first one\n");
                        prev->next = cur->next;
                        free(cur->ip);
                        free(cur);
                        cur = prev->next;
                    }
                } else {
                    prev = cur;
                    cur = cur->next;
                }
            }
            break;
        case ID_REMOVE_OLD: // Remove any with last_update older than search_time
            unix_time_to_str(search_time,tmpdate);
            writelog(6,"clhistory_lookup: Removing any with times before %s\n",tmpdate);
            cur = *server_conn_history;
            while(cur != NULL) {
                if (cur->last_update < search_time) {
                    // Found one, remove it.
                    unix_time_to_str(cur->last_update,tmpdate);
                    if(*server_conn_history == cur) {
                        // First one
                        writelog(4,"clhistory_lookup: Removing the first one, which has "
                                "date %s ip %s and server port %d\n",
                                tmpdate,cur->ip,cur->server_port);
                        *server_conn_history = cur->next;
                        free(cur->ip);
                        free(cur);
                        cur = *server_conn_history;
                    } else {
                        // Not the first one
                        writelog(4,"clhistory_lookup: Removing other than the first one, "
                                "which has date %s ip %s, and server port %d\n",
                                tmpdate,cur->ip,cur->server_port);
                        prev->next = cur->next;
                        free(cur->ip);
                        free(cur);
                        cur = prev->next;
                    }
					warn("Expired history for time %s for client at %s on server port %d\n",tmpdate,
						cur->ip,cur->server_port);
                } else {
                    prev = cur;
                    cur = cur->next;
                }
            }
            break;
    }
    return NULL;
}
#endif

