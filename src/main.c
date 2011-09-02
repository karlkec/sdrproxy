// main.c

/* Sets upa proxy for WinSDR client/server operation */

/* Written by Karl Cunningham  at domain keckec.com with address karlc */

/*
 * This program was written with lots of help from:
 * http://beej.us/guide/bgnet/output/html/singlepage/bgnet.html
 * and this about threads:
 * http://www.yolinux.com/TUTORIALS/LinuxTutorialPosixThreads.html 
 * and this very good (and no-nonsense) one about sockets and threads:
 * http://www.ibm.com/developerworks/library/l-pthred.html
 */

#ifndef MAIN_C
#define MAIN_C

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
#include <stdint.h>

#include "sdrproxy.h"


#define MAINLOOPTIME 30 // ms

/*
 * ####################$$$$   Program Description   $$$$####################
 * This program operates a caching data proxy between a WinSDR server
 *  and one or more WinSDR clients, hereafter known as the remote server
 *  and remote clients.
 *
 * Data is cached in a form ready to be sent to the clients, as conversion to 
 *  this format only has to be done once for any number of client connections. 
 *
 * The amount of storage required for the data cache depends on the number A/D channels,
 *  the sample rate (sps) and the bit width of each reading. The number of channels
 *  can range from 1 to 8, the sample rate from 10 to 500, and the A/D width can be 
 *  either 16 or 24 bits. This program can only accept 16-bit A/D data. The capability
 *  of WinSDR is limited to 4 channels when the sample rate is 500sps. This limits the
 *  A/D data to 4Kbytes per second, plus overhead for metadata and CRC. Data storage 
 *  is allocated dynamically only after the details of the data coming from the 
 *  WinSDR server are known, and are reallocated in the event that any of those 
 *  values change. Note that this is a very rare occurrance, and would be very 
 *  unlikely to happen to an exsiting connection. However, this program attempts 
 *  to be very robust in its connection to the remote WinSDR server, and the 
 *  remote server can be restarted and we will pick up the connection again.
 *
 * The local client and server operate independently except both access the same 
 *  data cache. 
 *
 * The local client places data in the cache and the local server retrieves it from 
 *  the cache. The depth of the cache is normally one hour (3600 seconds) but can 
 *  be specified differently on the command line. When the cache is full and new 
 *  data is received, the oldest data is lost.
 *
 * In operation, the remote WinSDR server sends out data each second, which includes 
 *  metadata (including timestamp) and the A/D data for each channel. When a remote 
 *  client connects to the local server for the first time, the server attempts
 *  to send the oldest data in the cache. It continues to send packets in order
 *  until it is "caught up" with data coming from the remote WinSDR server. 
 *  Whenever a remote client disconnects or the connection is interrupted such 
 *  that it can't continue, the timestamp of the last data sent is saved, indexed 
 *  by the client's IP address. Whenver a remote client connects, a check is made 
 *  to see if a saved timestamp exists for that IP address, and if so data is sent 
 *  starting from the saved time for that IP address. Saved timestamps are removed 
 *  when data for those times are no longer available in the cache. Thus when a 
 *  remote client connects and a connection from its IP address was last made
 *  more than one hour ago (or whatever the specified cache depth is), new data 
 *  is sent starting with the oldest data currently in the cache.
 *
 * Separate threads are used for the local client, local server, and main loops. These
 *  are each described below.
 *
 * Main thread --
 *  Monitors the client thread for changes in channel information. When this 
 *   occurs, it issues a shutdown command to the local server thread and waits 
 *   for that thread to shut down. When this has occurred, it reallocates the 
 *   data ring buffer and issues notification to the client thread that it can 
 *   start receiving data from the remote server and place it in the data ring.
 *
 * Client thread --
 *  Allocates receive and send buffers, opens a connection to the remote client,
 *   and sends it the "SEND AD" command. It then starts receiving data from the
 *   remote client.
 *
 *  If it gets a channel information packet from the remote client in which the 
 *   data differs from the current channel information, it issues a notice
 *   to the main thread that the channel information has changed. The client 
 *   thread doesn't store any more data in the data ring buffer but monitors
 *   the socket for more data from the remote server, and polls for a notice from
 *   the main thread that a new data ring buffer has been allocated. When the main
 *   thread sends notification that the data ring buffer has been allocated (or
 *   reallocated in the case of this happening sometime after data was received),
 *   the client thread starts placing received ADC data into the data ring buffer.
 *
 *  While waiting for the main thread to (re)allocate the data ring buffer, 
 *   the client thread continues to monitor for additional channel data from
 *   the remote WinSDR server. 
 *
 *  The communication with the main thread regarding change in channel data
 *   and (re)allocation of data ring buffers goes as follows: 
 *  At startup, CHANCHANGE and DATAALLOC are both false. When the client thread 
 *   sees a change in channel data (or when channel data is established for 
 *   the first time), the client thread sets CHANCHANGE true. When it detects 
 *   this, the main thread sets CHANCHANGE false and (re)allocates the 
 *   buffers. When the buffers are allocated, the main thread sets 
 *   DATAALLOC true.
 *
 *   When the client thread sees CHANCHANGE go false, it polls for DATAALLOC to 
 *   go true and checks for another possible channel data change from the
 *   server. When DATALLOC goes true, the client thread starts collecing ADC 
 *   data from the remote server. If there is another channel data change, the
 *   client thread again sets CHANCHANGE true and the process repeats. These
 *   events can happen with arbitrary timing. 
 *     
 *
 *  The client thread continues to receive data from the remote server and 
 *   placing it in the data ring buffer. If the connection is lost with the 
 *   remote server, the client thread attempts to reconnect. Out-of-order data
 *   packets are placed in the correct position in the data ring buffer, and 
 *   missing packets are specially-requested from the remote server.
 *
 * Server Thread--
 *  Sets up a listening socket on the TCP ports specified to accept connections from remote
 *   clients. When a client connects, send and receive ring buffers are allocated and
 *   and a check is made to see if data has been sent to the remote IP address before.
 *   If it has, the time stamp of the last packet sent to that IP address is used
 *   to determine from which time to start sending A/D information. It then waits 
 *   for the "SEND AD" command from the remote client, and sends a channel information 
 *   packet followed by A/D data packets. Each time a "SEND AD" command is receieved,
 *   the channel data infomation is sent to the remote client.
 *
 *  Operation is similar for each remote client connection made. When a connection 
 *   times out or closes, the time stamp of the packet last sent is saved in the 
 *   database of previous connections, to be used if/when that client connects again.
 *
 *  If a shutdown request is received from the main thread, the server thread
 *   immediately closes all client sockets and the server thread ends.
 *
 * The local client (connected to the remote WinSDR server) maintains a circular (ring)
 *  buffer for received data. This buffer is allocated on startup to be sufficient for
 *  the largest expected packets from the remote server (maximum number of channels times
 *  the maximum samples per second times the ADC data depth). Its size is never
 *  changed. 
 *
 */

// Global variables
int debug_level_g;

// #############   Globals #############
// nowStr -- String to hold the current time of data (UTC).
char nowstr[50]; 
pthread_mutex_t mutex_nowstr = PTHREAD_MUTEX_INITIALIZER;

// The data_store_g. Pointer to dynamically allocated ring of pointers to sdr data.
pthread_mutex_t mutex_datastore = PTHREAD_MUTEX_INITIALIZER;

// Channel info data structure and mutex
struct channelinfo chan_info_g;
pthread_mutex_t mutex_chaninfo = PTHREAD_MUTEX_INITIALIZER;

// stdout
pthread_mutex_t mutex_stderr = PTHREAD_MUTEX_INITIALIZER;

// The log file
FILE *logfile; // Log file
pthread_mutex_t mutex_logfile = PTHREAD_MUTEX_INITIALIZER;

// The server communication structure
pthread_mutex_t mutex_servercomm = PTHREAD_MUTEX_INITIALIZER;

//  The client communication structure.
pthread_mutex_t mutex_clientcomm = PTHREAD_MUTEX_INITIALIZER;

int main(int argc, char *argv[]) {

	struct clientcomm client_comm; // Communication with the client thread
	struct servercomm server_comm; // Communication with the server thread
    int server_running = FALSE;
	int chan_info_change;
    debug_level_g = DEFAULTDEBUGLEVEL;
	pthread_t pt_server,pt_client;

	if(argc < 3) {
        warn("Must give target host as first argument and port number as the second\n");
		return 1;
	}

    // Open the log file
    logfile = fopen(LOGFILENAME,"a");
    if(logfile == NULL) {
        fail("main: Error opening logfile\n");
    }

	writelog(0,"main: debug_level set to %d\n",debug_level_g);

	// Allocate the datastore structure array
	data_ring_ops(NULL,0,0,RING_ALLOC);

	// Clear the client and server communication structures, and set the 
	//  remote WinSDR server's hostname for the local client. Mutexes locked here for 
    //  completeness, as the other threads aren't running yet.
    pthread_mutex_lock(&mutex_clientcomm );
	memset(&client_comm,0,sizeof(struct clientcomm));
	client_comm.host = argv[1];
    client_comm.port = argv[2];
	if(argc > 3) {
		if(!sscanf(argv[3],"%hhd",&client_comm.client_no) || 
		   (client_comm.client_no < 0) || 
		   (client_comm.client_no > 9)) 
		{
			warn("The third argument, Client Number, must be between 0 and 9\n");
			return 1;
		} 
	} else {
		client_comm.client_no = -1;
	}

    pthread_mutex_unlock(&mutex_clientcomm );
    pthread_mutex_lock(&mutex_servercomm);
	memset(&server_comm,0,sizeof(struct servercomm));
    pthread_mutex_unlock(&mutex_servercomm);

	// Launch the client thread and start the main loop.
    writelog(2,"main: Starting the client thread\n");
	pthread_create(&pt_client, NULL, client_thread, (void *)&client_comm);

    // Main loop
    for (;;) {
		/*
		 * Check to see if the local client has indicated a change in channel data.
		 */
		pthread_mutex_lock(&mutex_clientcomm);
		chan_info_change = client_comm.flags & CHANCHANGE;
		pthread_mutex_unlock(&mutex_clientcomm);
		if(chan_info_change) {
            /*
             * Our client has gotten a channel config change from the remote WinSDR server. This
             *  can be a change in the number of channels or sample rate, and can happen
             *  if the remote server configuration has changed. It will always happen when
             *  the local client first connects to the remote WinSDR server.
             * Winsdr doesn't handle channel configuration changes on the fly, and in fact 
             *  it dumps the current daily record file when this happens. Thus it will rarely 
             *  occur, but we handle the initial channel config discovery as if it did.
             * When a channel config change occurs:
             *  1. If the server thread is running, tell it to stop. Clients will be 
             *     disconnected and should reconnect automatically when the server thread is restarted.
             *  2. Wait for the local server thread to end.
             *  3. (Re)allocate the data structures, with sizes consistent with the (new) channel configuration.
             *  4. Clear the channel data change flag. This will tell the client it is ok 
             *     to start saving data.
             *  5. Start a new server thread.
             */
            pthread_mutex_lock(&mutex_chaninfo);
            pthread_mutex_lock(&mutex_clientcomm);
            chan_info_g.sps = client_comm.chan_info.sps;
            chan_info_g.num_channels = client_comm.chan_info.num_channels;
            chan_info_g.type = client_comm.chan_info.type;
            pthread_mutex_unlock(&mutex_clientcomm);
			writelog(0,"main: Channel data change new sps: %d  new num_channels: %d\n",
                    chan_info_g.sps,chan_info_g.num_channels);
            pthread_mutex_unlock(&mutex_chaninfo);
            // If the server thread is running, tell it to stop the clients and wait for it to do so.
            if(server_running) {
                writelog(1,"main: Telling server thread to shut down the connections\n");
				pthread_mutex_lock(&mutex_servercomm);
                server_comm.flags |= SRV_CLOSECMD;
				pthread_mutex_unlock(&mutex_servercomm);
                writelog(2,"main: Waiting for the server to shut down\n");
                pthread_join(pt_server,NULL);
                writelog(2,"main: The server has successfully shut down and closed its thread\n");
				server_running = FALSE;
            }
			/*
			 * (Re)allocate the data ring buffer, copy the channel data from the client_comm structure 
             *  to the global chan_info_g structure, and set the client_comm flags to 
			 *  indicate that we've accepted the change and have allocated new buffers for it. This will be
             *  the signal to it to start receiving data from the remote server.
			 */
			writelog(2,"main: Allocating data buffers\n");
			data_ring_ops(NULL,0,0,RING_DEALLOC);
			data_ring_ops(NULL,0,0,RING_ALLOC);
			// Copy the new channel data to the global channel data structure
            pthread_mutex_lock(&mutex_clientcomm );
			pthread_mutex_lock(&mutex_chaninfo);
			memcpy(&chan_info_g,&client_comm.chan_info,sizeof(struct channelinfo));
			pthread_mutex_unlock(&mutex_chaninfo);
			client_comm.flags &= ~CHANCHANGE;
            pthread_mutex_unlock(&mutex_clientcomm );
            // Clear the server flags and start the server thread.
			pthread_mutex_lock(&mutex_servercomm); // Really no need -- server isn't running
            server_comm.flags = 0;
			pthread_mutex_unlock(&mutex_servercomm);
            writelog(2,"main: Starting the server_thread\n");
            pthread_create(&pt_server,NULL, server_thread, (void *)&server_comm);
			server_running = TRUE;
		}

		// Our loop delay
		select_delay(MAINLOOPTIME);
    }
    // Should never get here, but the compiler complains if we don't have this.
    return 0;
} 

#endif // #ifndef MAIN_C
