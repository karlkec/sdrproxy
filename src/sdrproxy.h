/*
 * sdrproxy.h
 */


/*
 * Following is a description of the capabilities of the various PSN A/D boards. This is from
 *  http://psn.quake.net/PSNADBoardDLL.html
 *  
 * V1 board support the following sample rates: 5, 10, 20, 25, 50, 100 and 200 SPS. At 200 SPS
 * this board can record up to 4 channels. Below this sample rate the board can record all 8
 * channels.
 *
 * V2 board support the following sample rates: 10, 20, 50, 100 and 200 SPS. This board can
 * record data from all 8 channels independent of the sample rate.
 *
 * V3 board support the following sample rates: 10, 20, 50, 100, 200, 250 and 500 SPS. This
 * board can record up to 4 channels at 500 SPS and up to 8 channels for all other sample
 * rates.
 *  
 * VM board supports the following sample rates: 5, 10, 20, 25, 40, 50 and 80 SPS. This board
 * can record one or two channels of 24-bit data (sent as 4-byte data)..
 */ 

// Type passed during initial client/server connection 
#define ADC_TYPE_UNKNOWN 0
#define ADC_TYPE1 1
#define ADC_TYPE2 2
#define ADC_TYPE_VM 3
  
// Data header flag values for various version of A/D boards
#define V1_DFLAGS 0
#define V2_DFLAGS 0x80
#define V3_DFLAGS 0x81
#define VM_DFLAGS 0x40


#define SERVERPORTNO 16065 // Our local server TCP port number.
#define MAX_OUTGOING_PORTS 2 // Maximum number of concurrent outgoing ports supported

// Number of timeouts to go through trying to send data before giving up.
#define SENDTRIES 3 

#define CHANREQUEST "SEND AD"
#define CHANREQUESTLEN strlen(CHANREQUEST)
#define ADCTOKEN "\xAA\x55\x88\x44"
#define ADCTOKENLEN 4
#define CHANTOKEN "SPS: "
#define CHANTOKENLEN 5
#define CHANINFOLEN 25
// sscanf format string for the channel data.
#define CHANINFOSCANFMT "SPS: %d Chans: %d Type: %d "
#define MAXCHANNELS 8
#define ULONG unsigned long
#define BYTE unsigned char
#define ADCDATASIZE 4 // Bytes per adc data point, for volksmeter data.
#define VMBYTES 4
#define MAXDATAPERSEC 1600 // 200sps * 8 channels

// Length of the WinSDR preheader and dataheader structures in the TCP packets.
#define PREHDRLEN 8
#define DATAHDRLEN 22
#define PKTCRCSTART 4 // Starting byte to calculate the CRC.
#define CRCLEN 1

#define SERVERRCVBFRSIZE 300 // Bytes max to receive from remote client 
#define BFRSIZE 5000
// Maximum packet size is 200sps * 8 channels * 4bytes/sample + prehdr + datahdr + CRC
#define MAXPACKETSIZE MAXDATAPERSEC * VMBYTES + PREHDRLEN + DATAHDRLEN + CRCLEN

#define MAXCLIENTCONNECTIONS 10 // Maximum number of clients who can connect at the same time.
#define MAXIDTRACKING 30 // Maximum number of lastPacketIDs tracked by client IP address

#define PRECONNECTIONTIMOUT 30000 // ms
#define POSTCONNECTIONTIMOUT 15000 // ms
#define CHANREQUESTTIMEOUT 15000 // ms

// Loop timers
#define SENDCYCLETIME 10  // ms to spend loading bytes into the txqueue
#define SVR_PKT_PER_SECOND 100 // Packets per second to send to WinSDR
#define SERVERSLOWCYCLE 50 // ms
#define SERVERFASTCYCLE 20 // ms
#define CLIENTSLOWCYCLE 50 // ms
#define CLIENTFASTCYCLE 10 // ms
// CLIENTCYCLETIMER is the number of times through the CLIENTSLOWCYCLE before sending a 
//  channel request (about 5 seconds)
#define CLIENTCYCLETIMER 100 

#define LOGFILENAME "sdrproxy.log"
#define RINGDUMPFILENAME "ringdump.bin"

#define DATASIZE 14400 // second to save. 4 hours

// Set the value of a timeval structure. Accepts a pointer to tv, and the time in ms.
#define TV_SET(tm,ms) ((tm)->tv_sec = ((ms) / 1000), (tm)->tv_usec = (((ms) % 1000) * 1000))

/*
 * The following two constants specify how much time overlap there should be between the data sent
 *  before a client disconnects and the data sent out when that client reconnects later on. WinSDR
 *  seems to not save the last block (up to one minute) of data when terminated. Blocks start on the 
 *  minute mark and the data saved to disk some seconds later. The current block of data is never saved,
 *  and if terminated close enough to a minute boundary, the previous minute's data is not saved
 *  either. So when a client reconnects, we send data starting MINUTES_TO_BACKUP number of minutes 
 *  prior to the start of the last minute in which data was last sent. If the last data was sent 
 *  within SECONDS_OVERLAP of the start of a minute, data is sent starting one minute earlier.
 */
#define MINUTES_TO_BACKUP 1
#define SECONDS_OVERLAP 4

#define TRUE 1
#define FALSE 0

#define DEFAULTDEBUGLEVEL 1

#define max(x,y) (((x) > (y)) ? (x) : (y))
#define min(x,y) (((x) < (y)) ? (x) : (y))

// These are packet types for data packets. The type is from the preheader. 
// These still contain an ADCTOKEN at the beginning
#define ADC_MSG     0
#define ADC_ERROR   1
#define ADC_AD_MSG  2
#define ADC_AD_DATA 3

// SDR data packet structures
struct prehdr {
    char id[4];
    unsigned short datalen;
    BYTE type;
    BYTE flags;
};

struct dataHdr {
    short year,mon,dow,dom,hour,min,sec,msec;
    ULONG packet_id;
    BYTE  time_ref_stat;
    BYTE flags;
};

/*
 * SDR packet field positions. We define fixed positions within the TCP data for 
 *  portability, since the packing method used by various platforms is unknown and we
 *  don't have control of both ends.
 *  These offsets are derived from inspection of the TCP data from WinSDR. 
 */
// Preheader byte positions, relative to the start of the packet.
#define PREHDR_POS_ID 0
#define PREHDR_POS_DATALEN 4
#define PREHDR_POS_TYPE 6
#define PREHDR_POS_FLAGS 7
// Dataheader byte positions, relative to the start of the packet.
#define DATAHDR_POS_TIME 8
#define DATAHDR_POS_YEAR 8
#define DATAHDR_POS_MON 10
#define DATAHDR_POS_DOW 12
#define DATAHDR_POS_DOM 14
#define DATAHDR_POS_HOUR 16
#define DATAHDR_POS_MIN 18
#define DATAHDR_POS_SEC 20
#define DATAHDR_POS_MSEC 22
#define DATAHDR_POS_PACKETID 24
#define DATAHDR_POS_TIMEREFSTAT 28
#define DATADHR_POS_FLAGS 29

// Dataheader byte positions, relative to the start of the data header.
#define DATAHDR_POS_TIME_REL 0
#define DATAHDR_POS_YEAR_REL 0
#define DATAHDR_POS_MON_REL 2
#define DATAHDR_POS_DOW_REL 4
#define DATAHDR_POS_DOM_REL 6
#define DATAHDR_POS_HOUR_REL 8
#define DATAHDR_POS_MIN_REL 10
#define DATAHDR_POS_SEC_REL 12
#define DATAHDR_POS_MSEC_REL 14
#define DATAHDR_POS_PACKETID_REL 16
#define DATAHDR_POS_TIMEREFSTAT_REL 20
#define DATADHR_POS_FLAGS_REL 21

// Volksmeter decision
#define IS_VM (chan_info_g.type == 4)

// Time structure with ms granularity
struct mstime {
    long sec; // Unix seconds
    int msec;
};

struct channelinfo {
    int sps;
    int num_channels;
	int type;
};

typedef double PACKETTIME ;

struct packetinfo {
    int packet_len; // Length of the entire packet in bytes including the preheader and CRC.
	PACKETTIME packet_time; // Unix time of this packet.
    long packet_id;
    int valid; // TRUE if valid data here. FALSE if zero (filled) data
};

// We access the pre-header and dataheader by individual elements, not as a structure. Too 
//  scary to overlay a compiled structure over (potentially-unknown) stuff to/from WinSDR. 
//  We essentially start over if the number of samples per second or the number of channels 
//  change in midstream.  This structure is used for passing data back and forth to the data 
//  ring management function. The actual ADC data is passed as a separate pointer.
struct datastore {
	struct packetinfo packet_info; // Holds the rest of the info about the packet.
    char *sdr_packet; // pointer to the data packet, ready to send to WinSDR
};

/*
 * Local client communication flag masks
 *   CHANCHANGE  --  Change in channel data detected. Main thread must reallocate memory and 
 *                    restart the server thread.
 *   DATAALLOC   --  Data storage allocated and OK for use. OK for Local client to put
 *                    data in memory
 */
#define CHANCHANGE 1
// Structure for communication from the main loop to the local client thread.
struct clientcomm {
    int flags; // Assume 16 bits
    char *host; // the hostname of the server we are to connect to.
    char *port; // The port number of the remote server
	char client_no; // Number of our client, to connect with other sdrproxy servers.
    // Channel data. Initialized to zeros by the main thread, changed by client thread 
    //   when the remote server changes the channel data.
	struct channelinfo chan_info; 
};

/*
 * Local server thread communication flags masks
 *   SRV_CLOSING  -- This server is closing the connection. Main thread should 
 *                   expect this server connection to close. Not currently used
 *   SRV_CLOSECMD -- Main thread needs server to shut down, due to channel data change.
 */
#define SRV_CLOSING 1 
#define SRV_CLOSECMD 2 
struct servercomm {
    int flags;
};

// Structure to pass particulars about a specific ring buffer to the ring functions.
struct ringset {
    int readptr, writeptr; // Pointers to start and end. These must be stored between calls.
    int bfrsize; // Maximum number of bytes that can be stored. *ring must be allocated one more than this value.
    char *ring; // Data store
};

#define CLI_IDLE_TIMEOUT 150 // Approx 5 seconds

// Ring buffer management
enum ringbfract_t { RING_ALLOC, RING_DEALLOC, RING_INIT, RING_STORE, RING_RETR, RING_ISMT, 
				RING_ISFULL, RING_ELEMENTS_FREE, RING_ELEMENTS_STORED, RING_VIEW, RING_DELETE };

// Globals defined in main
#ifndef main_c
extern char r_ring[BFRSIZE+1]; // receive Ring buffer
extern int debug_level_g;
extern char *state_strings[];
extern char *rcvstatestrings[];
extern char *remclistatestr[];

// #############   Globals requiring thread protection, and their mutexes  #############
// nowStr -- String to hold the current time of data (UTC).
extern char nowstr[50]; 
extern pthread_mutex_t mutex_nowstr;

// The data_store_g. Pointer to dynamically allocated ring of sdr data
extern struct datastore *data_store_g[DATASIZE];
extern pthread_mutex_t mutex_datastore;

// Channel data
extern struct channelinfo chan_info_g;
extern pthread_mutex_t mutex_chaninfo;

// stderr
extern pthread_mutex_t mutex_stderr;

// The log file
extern FILE *logfile; // Log file
extern pthread_mutex_t mutex_logfile;

// Server communications
extern pthread_mutex_t mutex_servercomm;

// Client communications
extern pthread_mutex_t mutex_clientcomm;
#endif


// Function Prototypes
// main.c

// util.c
char do_crc(char *bfr,int cnt);
void closeLog(void);
int sdr_pkt_size(void);
char *bytes_in_bytes(char *haystack, size_t haysize, char *needle, size_t nsize);
int warn(char *fmt, ...);
int fail(char *fmt, ...);
int writelog(int level,char *fmt, ...);
void hexdumplog(int loglevel,char *msg,char *bfr,int len);
int print_state(char *func, int state, int prev_state,char *state_strings[]);

// time.c
char *get_now_str(char *dest);
char *mktimestr(struct tm *tm);
void time_to_str(struct tm *tm,char *dest);
void unix_time_to_str(time_t utime,char *dest);
time_t get_packet_time(char *bfr);
PACKETTIME decode_pkt_time(char *data_hdr);
void pkt_time_str(PACKETTIME pkt_time,char *time_str);
int cmp_pkt_time(PACKETTIME a, PACKETTIME b);

// socket.c
int send_all(int rsocket, char *buf, int len,long *timeout); // Sends the entire buffer, even if it takes several tries.
int send_to_host(int rsocket, struct ringset *ring);
int get_addr(char *host,struct addrinfo **res,char *port);
int get_socket(char *host,struct addrinfo *good_addr, struct addrinfo *paddr);
int sdr_connect(char *host,int socketfd, struct addrinfo *good_addr);
void select_delay(int timems);

// buffer.c
int find_chan_info(struct ringset *ring);
int search_ring(char *target,struct ringset *ring);
int ring_ops(char *bfr,int bytes,enum ringbfract_t action,struct ringset *ring);

// databfr.c
int data_ring_ops(struct packetinfo *element, char *pkt_img, PACKETTIME target_time, 
        enum ringbfract_t action);

// client.c
void *client_thread(void *);

// server.c
void *server_thread(void *thread_comm);
