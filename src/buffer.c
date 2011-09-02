// buffer.c


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

// Function prototype
int ring_normalize(int index,struct ringset *ring);
int ring_bytes_free(struct ringset *ring);
int ring_bytes_stored(struct ringset *ring);
int ring_isfull(struct ringset *ring);

int 
find_chan_info(struct ringset *ring) {
    /*
     * Searches the given ring buffer for a channel info string. If found, returns
     *  the position in the ring buffer of the string. If not found, returns -1.
     */
    int chanidx;
    char temp_bfr[BFRSIZE+1]; // +1 since we're going to null-terminate what we get.
    int dummy_sps, dummy_numch, dummy_type;

    // Get the index of the channel info token. Bail if not found.
    chanidx = search_ring(CHANTOKEN,ring);
    if(chanidx < 0) {
        return chanidx;
    }
    // If there's not enough on the ring, it can't be a complete channel info packet.
    if(ring_ops(NULL,0,RING_ELEMENTS_STORED,ring) < (chanidx + CHANINFOLEN)) {
        return -1;
    }
    // Get the data from the ring into our buffer. We have to get all the stuff on the ring
    //  before the channel info token, since there is no way to ask the ring functions for
    //  data in the middle of the ring buffer. After getting the data, we null-terminate the buffer
    //  and see if we successfully scan the data from the string. We don't keep the data,
    //  as we're just testing.
    ring_ops(temp_bfr,chanidx + CHANINFOLEN,RING_VIEW,ring);
    temp_bfr[chanidx + CHANINFOLEN] = '\0';
    if(sscanf(&temp_bfr[chanidx],CHANINFOSCANFMT,&dummy_sps,&dummy_numch,&dummy_type) < 3) {
        return -1;
    }
    return chanidx;
}

int 
search_ring(char *target,struct ringset *ring) {
    // Searches the given ring buffer for the null-terminated target string. 
    //  Returns the index posiiton in the ring of the target,
    //  or -1 if not found.
    char testbfr[BFRSIZE];
    int size;
    char *pos;

    writelog(8,"search_ring: Searching ring for target string of length %d\n",strlen(target));
    size = ring_ops(NULL,0,RING_ELEMENTS_STORED,ring);
    // Don't do anything if the ring contains less than the target, or if they're both zero bytes.
    if(size && (size >= strlen(target))) {
        ring_ops(testbfr,size,RING_VIEW,ring);
        pos = bytes_in_bytes(testbfr,size,target,strlen(target));
        if(pos == NULL) {
            writelog(7,"search_ring: Did not find target string\n");
            return -1;
        } else {
            writelog(5,"search_ring: Found target string\n");
            return (int)(pos - testbfr);
        }
    } else {
        writelog(8,"search_ring: Ring doesn't contain enough characters (%d). Search string "
                "contains %d. Search aborted\n",size,strlen(target));
        return -1;
    }
}

inline int 
ring_normalize(int index,struct ringset *ring) {
    /*
     * Normlizes the given index, by adding BFRSIZE and returning the result modulo BFRSIZE
     */
    return (index + ring->bfrsize) % ring->bfrsize;
}

inline int 
ring_bytes_free(struct ringset *ring) {
    /*
     * REturnes the number of bytes stored on the ring buffer.
     */
    writelog(7,"ring_bytes_free: readptr: %d  writeptr: %d  bfrsize: %d  free: %d\n",
            ring->readptr,ring->writeptr,ring->bfrsize,
        (ring->writeptr - ring->readptr + ring->bfrsize - 1) % ring->bfrsize);
    return (ring->readptr - ring->writeptr + ring->bfrsize - 1) % ring->bfrsize;
}

inline int 
ring_bytes_stored(struct ringset *ring) {
    /*
     * Returns the number of free bytes in the ring
     */
    writelog(7,"ring_bytes_stored: readptr: %d  writeptr: %d  bfrsize: %d  stored: %d\n",
            ring->readptr,ring->writeptr,ring->bfrsize,
        (ring->writeptr - ring->readptr + ring->bfrsize) % ring->bfrsize);
    return (ring->writeptr - ring->readptr + ring->bfrsize) % ring->bfrsize;
}

inline int
ring_isfull(struct ringset *ring) {
    /*
     * Returns boolean whether the ring is full
     */
    return (((ring->writeptr + 1) % ring->bfrsize) == ring->readptr);
}

inline int
ring_isempty(struct ringset *ring) {
    /*
     * Returns a boolean whether the ring is empty.
     */
    return (ring->readptr == ring->writeptr);
}

int 
ring_ops(char *bfr,int bytes,enum ringbfract_t action,struct ringset *ring) {
	/*
     * Does ring buffer management. Actions can include 
     *   RING_ALLOC, RING_DEALLOC, RING_INIT, RING_STORE, RING_RETR, RING_ISMT, 
     *     RING_ISFULL, RING_ELEMENTS_FREE, RING_ELEMENTS_STORED,RING_VIEW, RING_DELETE
     * RING_ALLOC uses bytes to indicate the nuber of bytes to be stored in the ring. 
     *   The buffer will be allocated to one more than this
     * RING_DEALLOC deallocates the ring buffer
     * RING_STORE, RING_VIEW, and RING_RETR both use the *bfr and bytes. Others do not and 
     *   they can be set to zero.
	 * RING_ISMT and RING_ISFULL, return 1 if true, 0 otherwise.
     * RING_RETR, RING_VIEW, and RING_STORE return -1 if the requested number of bytes is invalid
	 * RING_ELEMENTS_FREE and RING_ELEMENTS_STORED return the number of bytes free and stored, repsectively.
     *
     * This should be called with the appropriate struct ringset *ring pointer to point to 
     *  data from the appropriate thread.
	*/
    int partial;

    switch (action) {
        case RING_ALLOC: // Allocate memory for the ring
            ring->ring = (char *)malloc(bytes+1);
            if(ring->ring == NULL)
                fail("ring_ops: Failed to allocate memory for ring buffer\n");
            ring->bfrsize = bytes + 1;
            ring->readptr = ring->writeptr = 0;
            writelog(2,"ring_ops: Successfully allocated ring buffer to hold %d bytes\n",bytes);
            break;
        case RING_DEALLOC: // Free the buffer
            writelog(2,"ring_ops: De-allocating ring buffer of %d bytes\n",ring->bfrsize);
            free(ring->ring);
            break;
        case RING_INIT: // Initialize
            writelog(5,"ring_ops: Initializing ring buffer of %d bytes\n",ring->bfrsize);
            ring->readptr = ring->writeptr = 0;
            break;
        case RING_STORE: // Store bytes from bfr to ring
            if(bytes > ring_bytes_free(ring)) {
                return -1;
            } else {
                // Copy in two parts if it will wrap.
                partial = ring->bfrsize - ring->writeptr;
                if(bytes > partial) {
                    memcpy(&ring->ring[ring->writeptr],bfr,partial);
                    memcpy(ring->ring,&bfr[partial],bytes - partial);
                } else {
                    memcpy(&ring->ring[ring->writeptr],bfr,bytes);
                }
                ring->writeptr = ring_normalize(ring->writeptr + bytes,ring);
                writelog(7,"ring_ops: Stored %d bytes into ring. new readptr: %d  new writeptr: %d. "
                        "Ring now contains %d bytes\n",
                        bytes, ring->readptr,ring->writeptr, ring_bytes_stored(ring));
            }
            break;
        case RING_RETR: // Retrieve bytes from the ring to bfr
        case RING_VIEW: // Retrieve but don't update the ring->readptr pointer
            if((bytes > 0) && (bytes > ring_bytes_stored(ring))) {
				return -1;
            } else {
                partial = ring->bfrsize - ring->readptr;
                if(bytes > partial) { 
                    // Copy in two parts if it will wrap
                    memcpy(bfr,&(ring->ring[ring->readptr]),partial);
                    memcpy(&bfr[partial],ring->ring,bytes - partial);
                } else {
                    memcpy(bfr,&(ring->ring[ring->readptr]),bytes);
                }
                // Update the readptring pointer if action == retrieve
                if(action == RING_RETR) {
                    ring->readptr = ring_normalize(ring->readptr + bytes,ring);
                    writelog(7,"ring_ops: Retrieved %d bytes from ring. new readptr: %d  new writeptr: %d. "
                            "Ring now contains %d bytes\n", bytes, ring->readptr,ring->writeptr,ring_bytes_stored(ring));
                }
            }
            break;
        case RING_DELETE: // Delete the given number of bytes from the readptr of the ring (update the ring->readptr).
                     //  If bytes is out of range, do nothing but return -1.
            if((bytes > 0) && (bytes <= ring_bytes_stored(ring))) {
                ring->readptr = ring_normalize(ring->readptr + bytes,ring);
                writelog(7,"ring_ops: Deleted %d bytes from ring. new readptr: %d  new writeptr: %d. "
                        "Ring now contains %d bytes\n",
                        bytes, ring->readptr,ring->writeptr,ring_bytes_stored(ring));
            } else {
                return -1;
            }
            break;
        case RING_ISMT: // Is empty?
            return ring_isempty(ring);
            break;
        case RING_ISFULL: // Is full?
            return ring_isfull(ring);
            break;
        case RING_ELEMENTS_FREE: // REturns the number of free bytes
            return ring_bytes_free(ring);
            break;
        case RING_ELEMENTS_STORED: // REturns the number of bytes currently stored.
            return ring_bytes_stored(ring);
            break;
        default:
            fail("ring_ops: Illegal action passed to ring_ops: %d\n",action);
            break;
    }
    return 0;
}

