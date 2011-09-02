// socket.c


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

#include "sdrproxy.h"
int 
send_all(int rsocket, char *buf, int len,long *timeout) {
	/*
	 * Sends the entire buffer, even if it takes a few tries.
	 * takes the same arguments as send, except for the timeout argument in ms.
	 * Return values:
	 *  >0 -- number of bytes sent, up to the length len
	 *   0 -- timed out with no data sent
	 *  -1 -- connection was reset
	 *  -2 -- server closed the socket.
	 */
	int bytesleft = len;
	struct timeval tv;
	fd_set writefds;
	int rv,n;
	char errorstr[101];

	/*
	 * Note that *timeout is reduced by SENDCYCLETIME each time through the 
	 *  following loop. If select times out each time through, this will
	 *  be a reasonably accurate measure of how much time has elapsed. However, if
	 *  select returns after some time has elapsed but before it times out,
	 *  we don't have a relaible way to know how much time that took. The most
	 *  we can be off on each loop is SENDCYCLETIME. The presumption here is that
	 *  it usually won't take very many iterations to send all the data, so
	 *  the timeout value set on return will be reasonably close.
	 */
    n = rsocket + 1;
	while ((bytesleft > 0) && (*timeout > 0)) {
		FD_ZERO(&writefds); // Clear the select set
		FD_SET(rsocket,&writefds);
		tv.tv_sec = SENDCYCLETIME / 1000;
		tv.tv_usec = (SENDCYCLETIME % 1000) * 1000; // SENDCYCLETIME is in ms
        writelog(7,"send_all: Waiting to send data\n");
		rv = select(n,NULL,&writefds,NULL,&tv);
		if(rv == -1) {
			perror("select");
			fail("");
		} else if(!rv) {
			// select timed out
			*timeout -= SENDCYCLETIME;
		} 
		if(FD_ISSET(rsocket,&writefds)) {
            writelog(6,"send_all: Sending\n");
			rv = send(rsocket,buf + (len - bytesleft), bytesleft, 0);
			if(rv == -1) {
				writelog(0,"send_all: Resetting the connection -- %s\n",strerror_r(errno,errorstr,100));
				return -2;
			} else if(rv == 0) {
                writelog(0,"send_all: Server closed socket while sending\n");
				return -2;
			}  else {
				// send was successful. Tally the number of bytes left
				bytesleft -= rv;
			}
		} else {
            fail("send_all: send_all: Error -- no FD_ISSET \n");
		}
	}
	return len - bytesleft;
}

int 
get_addr(char *host,struct addrinfo **res,char *port) {
    /*
	 * Gets address info, and sets res to point to it.
	 * Returns 0 on success, various other error codes on failure (from
	 *  getaddrinfo).
	 */
	struct addrinfo hints;
    int status;

	memset(&hints,0,sizeof(hints));
	hints.ai_family = AF_UNSPEC; // Use AF_INET or AF_INET6 to force the version
	hints.ai_socktype = SOCK_STREAM;

    // Look up the remote host. It needs the port number as a string
	status = getaddrinfo(host,port,&hints,res);
	if(status != 0) {
        writelog(4,"get_addr: %s\n",gai_strerror(status));
		return status;
	}
    writelog(1,"get_addr: Got addresses for host %s on port %s\n",host,port);
	return 0;
}

int 
get_socket(char *host,struct addrinfo *good_addr, struct addrinfo *paddr) {
    // Tries to get a working socket. Returns the socket fd and sets *good_addr to the valid
	// address info on success.  Returns -1 on failure.
    struct addrinfo *p;
    void *addr;
    char *ipver;
	char ipstr[INET6_ADDRSTRLEN];
	int sockfd;

    writelog(2,"get_socket: Getting IP addresses for %s\n",host);
	for(p = paddr;p != NULL; p = p->ai_next) {
		// Get the pointer to an address that works for us
		if(p->ai_family == AF_INET) { // IPv4 only
			struct sockaddr_in 	*ipv4 = (struct sockaddr_in *)p->ai_addr;
			addr = &ipv4->sin_addr;
			ipver = "IPv4";
			
			// Convert to IP string
			inet_ntop(p->ai_family,addr,ipstr,sizeof(ipstr));
            writelog(2,"get_socket:  %s: %s\n",ipver,ipstr);

			// Now open a socket for this one.
            sockfd = socket(p->ai_family,p->ai_socktype,p->ai_protocol);
            if(sockfd < 0) {
                perror("socket");
                return -1;
            }
            writelog(1,"get_socket:Acquiring Socket successful\n");
            // If we've gotten this far, this must be  a good one.
			//  Copy the structure to save the good address info.
            memcpy(good_addr,p,sizeof(struct addrinfo));
			// We could fre the linked list of paddr info, but if we do we couldn't use it again
			//  to reconnect. So leave it alone.
            return sockfd;
		}
	}
	// Didn't find any. 
	writelog(3,"No suitable addresses found\n");
    return -1;
}

int 
sdr_connect(char *host,int socketfd, struct addrinfo *good_addr) {
    // Tries to connect to the given host. Returns 0 on success, 
    // -1 on failure.
	int done = FALSE;

	while (!done) {
		if(connect(socketfd,good_addr->ai_addr,good_addr->ai_addrlen) < 0) {
			if(errno == ECONNREFUSED) {
				// Connection refused by server
				writelog(0,"sdr_connect: Connecting refused. Trying again in 20 seconds\n");
				sleep(20);
			} else if (errno == ENETUNREACH) { 
                // Network is unreachable. Wait 20 seconds and try again
				writelog(0,"sdr_connect: Network is unreachable. Trying again in 20 seconds\n");
				sleep(20);
			} else if (errno == ETIMEDOUT) { 
                // Connection timed out. Try again right away. The OS will provide the 
                //  delay.
				writelog(0,"sdr_connect: Attempt to connect to WinSDR server timed out. "
                        "Trying again\n");
			} else {
				perror("connect");
				return -1;
			}
		}
        writelog(0,"sdr_connect: Connected to %s!\n",host);
		done = TRUE;
	}
    return 0;
}

void select_delay(int timems) {
		/*
		 * Delays the specified number of milliseconds using select
		 */
		struct timeval tv;

		tv.tv_sec = timems / 1000;
		tv.tv_usec = timems * 1000;
		if(select(0,NULL,NULL,NULL,&tv) < 0) {
			perror("select_delay()");
			fail("");
		}
}

int 
send_to_host(int rsocket, struct ringset *ring) {
    /*
     * Sends as much as possible from the ring buffer to the remote host using the given
     *  rsocket. Only tries once and does no delays. If successful or partially successful, 
     *  removes the sent bytes from the ring buffer. If not, leaves the ring buffer alone.
     * Assumes a select has already been done and that the socket is able to receive data.
     * Returns the following:
     *  >0 -- number of bytes sent and removed from the ring buffer
     *   0 -- Nothing sent, but ok to try again.
     *  -1 -- Connection was reset.
     *  -2 -- The remote host closed the socket.
     *  Fails on fatal errors.
     */
    int rv,bytes_to_send;
    char outbfr[BFRSIZE];

    bytes_to_send = ring_ops(NULL,0,RING_ELEMENTS_STORED,ring);
    ring_ops(outbfr,bytes_to_send,RING_VIEW,ring);
    rv = send(rsocket,outbfr,bytes_to_send,0);
    if(rv < 0) {
        if(errno == EWOULDBLOCK) {
            writelog(7,"send_to_host: Sending would block");
            return 0;
        } else if(errno == EAGAIN) {
            writelog(7,"send_to_host: send issued EAGAIN");
            return 0;
        } else if(errno == ECONNRESET) {
            writelog(2,"send_to_host: Connection reset by peer");
            return -1;
        }
    } else if(rv == 0) {
        writelog(0,"send_to_host: Remote host closed socket while sending\n");
        return -2;
    }
    // No error occurred. Remove the bytes sent from the ring and return.
    writelog(2,"send_to_host: Sent %d bytes to the remote host\n",rv);
    ring_ops(NULL,rv,RING_DELETE,ring);
    return rv;
}
