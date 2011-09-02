// client.c

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
#include <pthread.h>
#include <ctype.h>

#include "sdrproxy.h"


// Local client states, and matching strings
enum rcvstate_t {CLI_STATE_INVALID,CLI_STATE_GET_ADDR,CLI_STATE_GET_SOCKET,
                CLI_STATE_RECONNECT,CLI_STATE_CONNECT,
                CLI_STATE_SEND_CHANNEL_REQUEST,CLI_STATE_GET_DATA};

char *rcvstatestrings[] = {"CLI_STATE_INVALID","CLI_STATE_GET_ADDR","CLI_STATE_GET_SOCKET",
                           "CLI_STATE_RECONNECT","CLI_STATE_CONNECT",
                           "CLI_STATE_SEND_CHANNEL_REQUEST","CLI_STATE_GET_DATA"};

/* ############## FUNCTION PROTOTYPES #############*/
static int client_rcv_to_ring(int rsocket, struct ringset *rcv_ring);
static int send_channel_request(struct ringset *send_ring, char client_no);
static int get_chan_info(int index,struct channelinfo *new_channel_info,struct ringset *ring);
static int get_adc_data(int index,struct prehdr *pre_hdr, ULONG *packet_id,struct ringset *ring);
static void check_data(struct prehdr *pre_hdr, ULONG *packet_id,struct channelinfo *new_channel_info,
        int inhibit_adc,struct ringset *ring);
void print_sdr_data(char *bfr,ULONG datalen);
static void add_to_data_ring(char *packet,int pkt_len);
static int check_channel_info(int sps, int chans, int type);


void 
*client_thread(void *thread_comm) {
    /*
     * Performs client operations. Connects to the client, sends the needed string to start the 
     *  server sending data, and retrieves the data from the server. Places the received data in 
     *  the data_ring. Assumes address lookup has been previously done and that a valid addrinfo 
     *  structure has been passed to us. Uses thread_comm for communication to the main thread.
     * Handles reconnection to the remote server in case of network dropout or failure. Does not return.
     *
     * Polling is used for all socket operations, and there is one wait timer at the end of the loop.
     */

    struct clientcomm *client_comm = (struct clientcomm *)thread_comm;
    struct timeval tv;
	struct addrinfo *cli_addr,good_addr;
    enum rcvstate_t state = CLI_STATE_GET_SOCKET;
    int rv,rsocket,failtimer,inhibit_adc,packet_received;
	struct prehdr pre_hdr;
	struct channelinfo new_chan_info,old_chan_info;
    ULONG packet_id;
	int old_state = CLI_STATE_INVALID;
    struct ringset rcv_ring;
    struct ringset send_ring;
	char *cli_host,*cli_port;
	fd_set readfds,writefds;

    // Get the client address info
	pthread_mutex_lock(&mutex_chaninfo);
	cli_host = strdup(client_comm->host);
    cli_port = strdup(client_comm->port);
	pthread_mutex_unlock(&mutex_chaninfo);
    if(get_addr(cli_host,&cli_addr,cli_port)) {
        fail("Error looking up remote host");
    }

    // Inialize the old and new channel info structures, used to test if
    //  anything has changed.
    memset(&old_chan_info,0,sizeof(struct channelinfo));
    memset(&new_chan_info,0,sizeof(struct channelinfo));

    // Initialize the receive ring buffers
    ring_ops(NULL,BFRSIZE,RING_ALLOC,&rcv_ring);
    ring_ops(NULL,BFRSIZE,RING_ALLOC,&send_ring);

    packet_received = FALSE;
	writelog(2,"client_thread: Entering client main loop\n");
    for (;;) { // main loop.
        // If our state has changed, log the previous and current STATE.
		if(print_state("client_thread",state,old_state,rcvstatestrings)) {
            old_state = state;
        }

        // Set the loop delay. This will be used for timing in each part of the loop.
        if(packet_received) {
            TV_SET(&tv,CLIENTFASTCYCLE);
        } else {
            TV_SET(&tv,CLIENTSLOWCYCLE);
        }

        // If we are communicating with the remote server, check to see if it
        //  is able to receive data and if it has something to send us.
        if(state >= CLI_STATE_SEND_CHANNEL_REQUEST) {
            FD_ZERO(&readfds);
            FD_ZERO(&writefds);
            FD_SET(rsocket,&readfds);
            FD_SET(rsocket,&writefds);
            if(select(rsocket+1,&readfds,&writefds,NULL,&tv) == -1) {
                perror("select");
                fail("client_thread: select failed\n");
            }
            // If the socket is communicating with the remote server, and the receive
            //  ring isn't full and there is data to receive, get any data from 
            //  the remote server to our receive ring.
            packet_received = FALSE;
            if((state >= CLI_STATE_SEND_CHANNEL_REQUEST) &&
                !ring_ops(NULL,0,RING_ISFULL,&send_ring) && 
                FD_ISSET(rsocket,&readfds))
            {
                rv = client_rcv_to_ring(rsocket,&rcv_ring);
                if (rv < 0) {
                    // The server has closed the connection.
                    state = CLI_STATE_RECONNECT;
                }  else if(rv == 0) {
                    // Timed out.
                    failtimer -= CLIENTCYCLETIMER;
                } else if(rv > 0) {
                    // Got data. All is normal. 
                    failtimer = POSTCONNECTIONTIMOUT;
                    packet_received = TRUE;
                }

                if(failtimer <= 0) {
                    state = CLI_STATE_SEND_CHANNEL_REQUEST;
                }
            }
        }

        switch (state) {
			case CLI_STATE_GET_SOCKET:
                rsocket = get_socket(cli_host,&good_addr,cli_addr);
				if(rsocket < 0) {
					fail("client_thread: Error while getting socket\n");
				}
				state = CLI_STATE_CONNECT;
				break;
			case CLI_STATE_RECONNECT:
				// Close the socket, then connect again.
                rv = close(rsocket);
				if(rv < 0) {
					perror("close()");
					fail("client_thread: Error while closing socket\n");
				}
                writelog(4,"client_thread: Closed old socket successfully. Starting New Socket\n");
				state = CLI_STATE_GET_SOCKET;
				break;
			case CLI_STATE_CONNECT:
                rv = sdr_connect(cli_host,rsocket,&good_addr);
				if(rv < 0) {
					// Error connecting. End the program.
					fail("client_thread: sdr_connect returned -1 (failure)\n");
				} else {
                    // We're connected. Clear the ring buffers and change the state to send
                    //  the channel request.
                    ring_ops(NULL,0,RING_INIT,&rcv_ring);
                    ring_ops(NULL,0,RING_INIT,&send_ring);
					writelog(1,"client_thread: Connected\n");
					state = CLI_STATE_SEND_CHANNEL_REQUEST;
				}
				break;

            case CLI_STATE_SEND_CHANNEL_REQUEST:
                    // If the remote server is able to receive data, add "SEND AD" to
                    //  our send ring buffer. If that succeeds, change the state to get ADC 
                    //  data. If it failed, the ring is full with outgoing traffic. In
                    //  that case, don't change the state and try again next pass.
                    if(FD_ISSET(rsocket,&writefds)) {
                        if(send_channel_request(&send_ring,client_comm->client_no) == 0) {
                            state = CLI_STATE_GET_DATA;
                        }
                    }
                    break;

			case CLI_STATE_GET_DATA:
                /*
                 * Get our status. If the flags indicate a pending channel info change, 
                 *  we aren't allowed to store ADC data, so just get channel info.
                 */
				pthread_mutex_lock(&mutex_clientcomm );
                inhibit_adc = client_comm->flags & CHANCHANGE;
				pthread_mutex_unlock(&mutex_clientcomm);

                // Set new channel info to be the same as the old, then check data from the
                //  remote server to see if it's changed.
                memcpy(&new_chan_info,&old_chan_info,sizeof(struct channelinfo));

                // Get channel and adc data
				//warn("client_thread: checking incoming data\n");
                check_data(&pre_hdr,&packet_id,&new_chan_info,inhibit_adc,&rcv_ring);

                // If channel config data has changed, notify the main thread.
                if((old_chan_info.sps != new_chan_info.sps) ||
                  (old_chan_info.num_channels != new_chan_info.num_channels)) 
                {
                    writelog(0,"client_thread: New channel config . Old sps: %d  New sps: %d  "
                        "Old num_channels: %d  New num_channels: %d\n",
                        old_chan_info.sps, new_chan_info.sps, 
                        old_chan_info.num_channels,new_chan_info.num_channels);
                    warn("New channel config . Old sps: %d  New sps: %d  "
                        "Old num_channels: %d  New num_channels: %d\n",
                        old_chan_info.sps, new_chan_info.sps, 
                        old_chan_info.num_channels,new_chan_info.num_channels);
                    // Set the channel config change flag, save the current channel info 
                    //  as the old data, and the new data as the current data
                    pthread_mutex_lock(&mutex_clientcomm );
                    client_comm->flags |= CHANCHANGE;
                    memcpy(&old_chan_info,&client_comm->chan_info,sizeof(struct channelinfo));
                    memcpy(&client_comm->chan_info,&new_chan_info,sizeof(struct channelinfo));
                    pthread_mutex_unlock(&mutex_clientcomm);
                    writelog(2,"client_thread: Notified main thread of channel info change\n");
                    //warn("client_thread: Notified main thread of channel info change\n");
                }
                break;

            default:
                fail("Unhandled state in local client main loop\n");
                break;
		}
        // If we are communicating with the remote server, and there is data to send,
        //  and the remote client is able to receive, try sending what's on the ring.
        if((state >= CLI_STATE_SEND_CHANNEL_REQUEST) &&
            !ring_ops(NULL,0,RING_ISMT,&send_ring) && 
            FD_ISSET(rsocket,&writefds))
        {
            // It's ok to send data to the server. Do so, but look for errors.
            rv = send_to_host(rsocket,&send_ring);
            if(rv < 0) {
                // Connection dropped. Need to reconnect
                state = CLI_STATE_RECONNECT;
            }
        }
		// Wait for the remainder of our timer, first used in the select above.
        select(0,NULL,NULL,NULL,&tv);
	}
}

static int
client_rcv_to_ring(int rsocket, struct ringset *rcv_ring) {
    /*
     * This fucntion is called when select indicates there is something interesting from
     * the remote server. We check to see if there is data, and if so put it in the receive 
     *  ring for the local client. Or it may be that the server closed the connection.
     * Return values:
     *  -2 -- Peer has closed the socket
     *  -1 -- Connection error
     *   0 -- Got no bytes, but ok to try again
     *   >0 -- Number of bytes placed in the ring.
     */
    int rv,free_bytes;
    char rcv_bfr[BFRSIZE];
	char errorstr[101],timestr[50];

    writelog(7,"client_rcv_to_ring: Getting bytes from client into ring buffer\n");

    // See how many bytes are free in the receive ring, but not more than our local buffer.
    free_bytes = min(BFRSIZE,ring_ops(rcv_bfr,0,RING_ELEMENTS_FREE,rcv_ring));
    if(free_bytes == 0) {
        return 0;
    }
    rv = recv(rsocket,rcv_bfr,free_bytes,0);
    if(rv == -1) {
        // An error condition on recv. Do something
        if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
            // Try again
            return 0;
        }
        // Other error
		fprintf(stderr,"%s -- ",get_now_str(timestr));
        perror("client_rcv_to_ring: Error in recv");
        writelog(0,"client_rcv_to_ring: Error in recv: %s\n",strerror_r(errno,errorstr,100));
        return -1;
    } else {
        if (rv > 0) {
            // Got some bytes. Store them in the ring and return.
            writelog(7,"Client got %d bytes from server to ring, which previously had %d bytes\n",
                    rv,ring_ops(NULL,0,RING_ELEMENTS_STORED,rcv_ring));
            ring_ops(rcv_bfr,rv,RING_STORE,rcv_ring);
            //hexdumplog(7,"Stored the following bytes into ring: \n",rcv_bfr,rv);
            return rv;
        } else { // rv == 0. Connection closed by peer.
            writelog(2,"client_rcv_to_ring: Connection closed by peer\n");
            return -2;
        }
    }
}

static int 
get_chan_info(int index,struct channelinfo *new_channel_info,struct ringset *ring) {
    /*
     * Retrieves the channel info from the receive ring. index is the location of the token starting 
     *  channel info. On success returns 0. If the target string was not found on the ring, returns -1.
     *  Removes all before index1 from the ring, and if successful, removes the channel info from
     *  the ring as well. REturns 0 on success, -1 on failure.
     */
    char rcv_bfr[CHANINFOLEN+1];
    int tmp_sps, tmp_chans, tmp_type;

    // Remove all from the ring before the index
    if(index > 0) 
        ring_ops(NULL,index,RING_DELETE,ring);
    // If there's enough data on the ring to get channel info, grab it. Otherwise
	//  return so we can get more from the server.
    if(ring_ops(NULL,0,RING_ELEMENTS_STORED,ring) >= CHANINFOLEN) {
        ring_ops(rcv_bfr,CHANINFOLEN,RING_RETR,ring);
        rcv_bfr[CHANINFOLEN] = '\0';
        // Get the channel info from the buffer.
        if(sscanf(rcv_bfr,CHANINFOSCANFMT,&tmp_sps,&tmp_chans,&tmp_type) == 3) {
            writelog(0,"get_chan_info: Got Channel Data -- SPS = %d  Channels = %d  Type = %d\n",
                tmp_sps,tmp_chans,tmp_type);
            //warn("get_chan_info: Got Channel Data -- SPS = %d  Channels = %d  Type = %d\n",
              //  tmp_sps,tmp_chans,tmp_type);
            // Check that the data is valid, and return accordingly.
            if(!check_channel_info(tmp_sps,tmp_chans,tmp_type)) {
				new_channel_info->sps = tmp_sps;
				new_channel_info->num_channels = tmp_chans;
				new_channel_info->type = tmp_type;
                return 0;
            } else {
                // Invalid channel info 
				writelog(0,"get_chan_info:   ERROR -- Invalid channel config data from WinSDR");
				//warn("get_chan_info:   ERROR -- Invalid channel config data from WinSDR");
                return -1;
			}
        } else {
            // Channel info data token seen but the data didn't decode correctly.
            writelog(0,"get_chan_info:   ERROR -- Invalid channel config data from WinSDR");
            //warn("get_chan_info:   ERROR -- Invalid channel config data from WinSDR");
            return -1;
        }
    }
    return -1;
}

static int
check_channel_info(int sps, int chans, int type) {
    // Checsk sps, chans, and type against known valid values
    //  returns 0 if good, 1 if bad

    int valid_sps[] = {10,20,25,50,100,200,500,0};
    int i;

    if((chans < 1) || (chans > MAXCHANNELS)) {
        return 1;
    }
    // Limit type to 1 digit.
    if((type < 1) || (type > 9)) {
        return 1;
    }
    for (i = 0; valid_sps[i] > 0; i++) {
        if(sps == valid_sps[i]) {
            return 0;
        }
    }
    return 1;
}

static int 
get_adc_data(int index,struct prehdr *pre_hdr, ULONG *packet_id,struct ringset *ring) {
	/*
	 * Gets the SDR data from the ring, starting at index. 
     *  Removes all before index from the ring, checks for the token, grabs the 
     *  preheader, examines it for the data length, then gets the data from the ring.
     *  If there wasn't enough on the ring to get the full data packet, leaves the packet 
	 *  on the ring and returns.
	 * After grabbing the entire packet, checks the CRC and the packet type. If the type
	 *  is 0, 1 or 2, log ascii data contained in the packet, remove the packet from the ring, and 
	 *  return. If the packet type is 3, process it as A/D data.
	 * Return values:
	 *   0 -- Success.
	 *   -1 -- Failure, didn't remove anything from the ring.
	 */
	char rcv_bfr[BFRSIZE];
	unsigned short datalen;
	int i;

    writelog(4,"get_adc_data: Getting data packet starting at index %d\n",index);
    // Clear the ring up to the index.
    if(index > 0) {
        writelog(4,"get_adc_data: Clearing %d bytes from the beginning of the ring\n",index);
        ring_ops(NULL,index,RING_DELETE,ring);
    }

    // If there is enough bytes on the ring for the preheader, copy it to our 
	//  buffer. Otherwise, return to get more.
    if(ring_ops(NULL,0,RING_ELEMENTS_STORED,ring) > PREHDRLEN) {
		ring_ops(rcv_bfr,PREHDRLEN,RING_VIEW,ring);
	} else {
		return -1;
	}
	// Check that the ID string is first.
    if(!strcmp(rcv_bfr,ADCTOKEN)) {
        fail("getAdcData: ERROR. Didn't find the ADCTOKEN in buffer loaded from ring "
                "when it should have been there\n");
    }

    datalen = *((unsigned short *)(&rcv_bfr[PREHDR_POS_DATALEN]));
    pre_hdr->datalen = datalen;
    pre_hdr->type = *((BYTE *)&rcv_bfr[PREHDR_POS_TYPE]);
    pre_hdr->flags = *((BYTE *)&rcv_bfr[PREHDR_POS_FLAGS]);
    writelog(4,"get_adc_data: Got preheader data -- datalen: %d  type: %d  flags: %02Xh\n",
        datalen, pre_hdr->type, pre_hdr->flags);
	
    // If the ring constains enough data for the data, get it. Otherwise return to get more 
	//  data from the server.
    if(ring_ops(NULL,0,RING_ELEMENTS_STORED,ring) >= (PREHDRLEN + datalen)) {
        // Enough on ring. Get the whole thing, preheader and all, to simplify things.
        ring_ops(rcv_bfr,PREHDRLEN + datalen,RING_RETR,ring);
        writelog(4,"get_adc_data: Got %d bytes of SDR data from the ring, which now contains %d bytes\n",
            PREHDRLEN + datalen,ring_ops(NULL,0,RING_ELEMENTS_STORED,ring));

        // The CRC is computed on the data starting with the datalen and going to the 
        //  byte just before the last byte in the data. THe crc is stored in the last byte 
        //  of the data.
        if(do_crc(&rcv_bfr[PREHDR_POS_DATALEN],datalen + PREHDRLEN - 4 - 1) == 
                rcv_bfr[PREHDRLEN + datalen - 1]) 
        {
            // CRC ok. Process the data.
            writelog(5,"get_adc_data: CRC checked ok\n");
            /*
             * The preheader type determines what we'll do with it.
             *  Type 0 -- ADC Message
             *  Type 1 -- ADC ERROR
             *  Type 2 -- ADC A/D Message
             *  Type 3 -- ADC A/D Data
             */
			if(pre_hdr->type == ADC_AD_DATA) {
				// ADC Data packet type
				*packet_id = *((ULONG *)&rcv_bfr[DATAHDR_POS_PACKETID]);
                writelog(4,"get_adc_data: Got PacketID: %lu\n",*packet_id);
				add_to_data_ring(rcv_bfr,PREHDRLEN + datalen);
				return 0;
			} else { // Not adc packet type
				/*
                 * Probably an ascii string. Dump the ascii to the log, but replace 
                 * non-printable characters with periods. And for debugging, log
                 * it as hex characters.
                 */
                //hexdumplog(6,"Msg from remote server in hex:\n",rcv_bfr,datalen + PREHDRLEN);
				for(i = PREHDRLEN; i < (datalen); i++) {
					if(!isprint(rcv_bfr[i])) {
						rcv_bfr[i] = '.';
					}
				}
				// Null-terminate the string and log it
				rcv_bfr[i] = '\0';
				writelog(0,"Type %hhu message received with %hhu flags from remote server: %s\n",
					pre_hdr->type,pre_hdr->flags,&rcv_bfr[PREHDRLEN]);
			}
			return 0;
        } else  { // Bad CRC
            writelog(0,"get_adc_data: CRC Mismatch\n");
            return 0;
        }
    } // Enough data for processing.
    return -1;
}

static void
add_to_data_ring(char *packet,int pkt_len) {
	// Adds the packet at the location given to the data ring.
	//  Decode the packet only as far as is necessary.
	struct packetinfo this_pkt;
    char *data_hdr = &packet[PREHDRLEN];
    char pkt_time[50];

	this_pkt.packet_len = pkt_len;
	this_pkt.packet_time = decode_pkt_time(data_hdr);
	this_pkt.packet_id = *((ULONG *)&packet[DATAHDR_POS_PACKETID]);
	this_pkt.valid = TRUE;

	// Store the data on the ring
	data_ring_ops(&this_pkt, packet, 0, RING_STORE);
    pkt_time_str(this_pkt.packet_time,pkt_time);
	writelog(3,"add_to_data_ring: added packet id %lu  time %s, to data ring\n",
            this_pkt.packet_id,pkt_time);
}
	
static void 
check_data(struct prehdr *pre_hdr, ULONG *packet_id,struct channelinfo *new_channel_info,
        int inhibit_adc,struct ringset *ring) {
    /*
     * Looks for Channel info or ADC data (unless inhibit_adc is set) in the receive ring. Processes 
	 *  whatever's there. Searches the ring for either the signature of channel info or 
	 *  adc data. Whichever is found first in the ring is processed, and then the 
	 *  second (if there). Removes the data from the ring after it was used.
     */
    int chanidx,adcidx,rv = 0;
	int ring_bytes;

    // Loop while there is either channel or adc data on the ring. If neither is there, return 
    //  so we can get more data from the server. 
	do {
		// first check for the tokens in the ring. If we're inhibited from receiving ADC
		//  data, set adcidx to -1 so we don't get any adc data.
        chanidx = find_chan_info(ring);
        if(chanidx >= 0) {
            writelog(1,"check_data: Found channel ID token in buffer at offset %d\n",
                    chanidx);
            //warn("check_data: Found channel ID token in buffer at offset %d\n",
              //      chanidx);
        }
		if(inhibit_adc) {
            writelog(6,"check_data: Inhibited from receiving ADC data pending ok "
                    "from main thread\n");
            //warn("check_data: Inhibited from receiving ADC data pending ok "
              //      "from main thread\n");
			adcidx = -1;
		} else {
			adcidx = search_ring(ADCTOKEN,ring);
            if(adcidx >= 0) {
                writelog(4,"check_data: Found adc ID token in buffer at offset %d\n",
                        adcidx);
                //warn("check_data: Found adc ID token in buffer at offset %d\n",
                  //      adcidx);
                // Dump the first 25 bytes of the ring 
                char tmp[100];
                ring_ops(tmp,25,RING_VIEW,ring);
                //hexdumplog(7,"check_data: Dump of 1st 25 ring bytes:",tmp,25);
            }
		}
        // If channel id or adc data are on the ring, process them. Do whichever one comes first.
        if((chanidx >= 0) && (chanidx < adcidx)) {
            // Both channel and adc data are on the ring, and Channel data comes before ADC data
            // Get channel info first.
            rv = get_chan_info(chanidx,new_channel_info,ring);
        } else if(adcidx >= 0) {
            // ADC data by itself, or before a channel info packet.
            rv = get_adc_data(adcidx,pre_hdr,packet_id,ring);
            if((rv == -2) && (chanidx >= 0)) {
                /*
                 * There is an incomplete data packet on the ring followed by channel info
                 *  packet. Either the data packet is corrput (or truncated) or it is
                 *  an incomplete data packet with some data that looks like a channel info 
                 *  packet. We assume the former, since the probability of the latter is 
                 *  almost nil. And, incorrectly choosing the latter could cause us to 
                 *  lose some data. So we will discard the data packet and get the 
                 *  channel info.
                 */
                ring_ops(NULL,chanidx,RING_DELETE,ring);
                writelog(2,"check_data: Channel ID token follows incomplete ADC packet. Removed %d "
                        "bytes. Ring now has %d bytes",
                        chanidx,ring_ops(NULL,0,RING_ELEMENTS_STORED,ring));
                //warn("check_data: Channel ID token follows incomplete ADC packet. Removed %d "
                  //      "bytes. Ring now has %d bytes",
                   //     chanidx,ring_ops(NULL,0,RING_ELEMENTS_STORED,ring));
            }
        } else if(chanidx >= 0) {
            // Channel data by itself
			//warn("check_data: getting channel info\n");
            rv = get_chan_info(chanidx,new_channel_info,ring);
        }
    // Keep looping while there is more data, and no errors.
    } while (((chanidx >= 0) || (adcidx >= 0)) && !rv);
	/*
     * There is no more channel info or adc data on the ring. If inhibit_adc is TRUE,
	 *  remove all from the ring but leave the length of the CHANTOKEN in case
	 *  there is the start of a channel config packet at the end of the ring data.
     */
	if(inhibit_adc) {
        ring_bytes = ring_ops(NULL,0,RING_ELEMENTS_STORED,ring);
        if(ring_bytes > CHANTOKENLEN) {
            ring_ops(NULL,ring_bytes,RING_DELETE,ring);
        }
    }
}

static int 
send_channel_request(struct ringset *send_ring, char client_no) {
    /*
     * Puts "SENDnAD" on outgoing ring buffer, where n is the client_no (range: 0-9). If 
	 *  client_no < 0, sends the default string.
     * returns 0 is successful, -1 if no room on ring buffer
     */
	char send_str[50];

	if(client_no >= 0) {
		sprintf(send_str,"SEND%dAD",client_no);
	} else {
		strcpy(send_str,CHANREQUEST);
	}
    writelog(4,"send_channel_request: Putting %s on send_ring\n",send_str);
    if(ring_ops(NULL,0,RING_ELEMENTS_FREE,send_ring) >= CHANREQUESTLEN) {
        ring_ops(send_str,CHANREQUESTLEN,RING_STORE,send_ring);
        writelog(4,"send_channel_request: %s successfully placed on send ring, "
                "which contains %d bytes\n",send_str,ring_ops(NULL,0,RING_ELEMENTS_STORED,send_ring));
        return 0;
    }
    writelog(4,"send_channel_request: failed to place %s on send ring, which contains %d bytes\n",
            CHANREQUEST, ring_ops(NULL,0,RING_ELEMENTS_STORED,send_ring));
    return -1;
}

#if(0)
static void 
print_sdr_data(char *data_bfr,ULONG datalen) {
    /*
     * Prints the packet time and the first sample for each channel. data_bfr is assumed to start
     *  at the beginning of a packet.
     */
    int chan,sample,num_samples;
    int16_t value,*tbuf,*dbuf; 
    int overflow = 0;

    // tbuf is an array of shorts, for the time values
    tbuf = (int16_t *)&data_bfr[DATAHDR_POS_TIME];
    writelog(0,"print_sdr_data: Channel Data for %02d-%02d-%02d %02d:%02d:%02d.%03d - ",
            tbuf[1],tbuf[3],tbuf[0]-2000,tbuf[4],tbuf[5],tbuf[6],tbuf[7]);

    // dbuf points to the data buffer as and array of shorts
    dbuf = (int16_t *)&data_bfr[PREHDRLEN + DATAHDRLEN];
    num_samples = chan_info_g.sps * chan_info_g.num_channels;
    for(chan = 0; chan < chan_info_g.num_channels; chan++) {
        writelog(0,"ch%d: %-6hd  ",chan+1,dbuf[chan]);
    }
    
    for(sample = 0; sample < num_samples; sample++) {
        value = dbuf[sample];
        overflow |= (value == 32767);
        overflow |= ((value == -32768) << 1);
    }
    if(overflow & 1) {
        warn("print_sdr_data: Max data value detected - Positive Overflow likely\n");
    } else if(overflow & 2) {
        warn("print_sdr_data: Min data value detected - Negative Overflow likely\n");
    }

    writelog(0,"\n");
    // Board temperature sensor output. This is from channel 2. We grab the first sample
    //  from channel 2.
    writelog(0,"Temperature: %.2f deg C\n",(float)dbuf[1] / 666.22);
}
#endif
