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

struct datastore *data_store_g[DATASIZE]; // The data storage pointer array.

// Global variables
int datar_start; // The index in the ring of the first data point.
int datar_end; // The index in the ring of the place the next data point will be stored.
int datar_cnt; // The number of elements currently stored in the ring.

// Ring buffer macros
#define DATAR_ISEMPTY (!datar_cnt)
#define DATAR_ISFULL ((datar_cnt) == DATASIZE)
#define DATAR_ELEMENTS_FREE (DATASIZE - (datar_cnt))

// Function Prototypes
int compare_index(PACKETTIME target,int index);
int search_data_bfr(PACKETTIME target, int *index);
void store_element(int index, struct packetinfo *element,char *sdr_pkt);
void retr_element(int index, struct packetinfo *element,char *sdr_pkt);
void datar_rotate_right(int start, int end);
int datar_normalize(int index);


int 
compare_index(PACKETTIME target,int index) {
	/*
	 * Compares the packet_time of the packet in the data ring buffer at the 
	 *  given index against the target packet time. Returns 1 if the target is
	 *  after the one in the ring buffer, returns -1 if the target is before
	 *  the one in the ring buffer, and returns 0 if they are equal.
	 */
    PACKETTIME elementtime;

    elementtime = data_store_g[index]->packet_info.packet_time;
	if (target > elementtime) {
		return 1;
	} else if (target < elementtime) {
		return -1;
	}
	return 0;
}

int 
search_data_bfr(PACKETTIME target, int *index) {
	/*
     * Attempts to find the location in the ring of an element with a time closest
     *  to time target. A status is returned, and *index is set to the element in the
     *  ring with a time equal to or below the target time. The following describes 
     *  the possible outcomes:
     * 1. The ring buffer contains no element.
     *     *index is set to -1 and -2 is returned.
     * 2. target time is older than the oldest element in the ring.
     *     *index is set to datar_start and +1 is returned.
     * 3. target time is newer than the newest element in the ring.
     *     *index is set to (datar_end-1) and -1 is returned.
     * 4. target time is within in the times of elements in the ring, but is not 
     *     equal to any of them.
     *       *index is set to the next element past target and +2 is returned
     * 5. target time is the same as the time of an element currently in the ring.
     *     *index is set to the element with the same time as target, and 0 is returned.
     *
     * If the ring contains no elements, *index is set to -1, and -1 is returned.
	 */
	int min,max,mid;
    char timestr[50],packettime[50];

	// Ring contains no elements
	if(!datar_cnt) {
        *index = -1;
        writelog(6,"search_data_bfr: Ring is empty. No search performed\n");
		return -2;
	}
    pkt_time_str(target,timestr);
    writelog(6,"search_data_bfr: Searching for data packet with time %s\n",timestr);

    /*
     * Test the corner cases first.
     */

	// Target time is newer than the newest time
	if(compare_index(target,datar_normalize(datar_end - 1)) > 0) {
        *index = datar_normalize(datar_end - 1);
        writelog(6,"search_data_bfr: Target time is newer than the newest packet. "
                "Returning the newest packet at index %d\n",*index);
		return -1;
	}
    // Target time is older than the oldest time.
    if(compare_index(target,datar_start) < 0) {
        *index = datar_start;
        writelog(6,"search_data_bfr: Target time is older than the oldest packet. Returning first packet\n");
        return +1;
    }

	/*
     * Now do the search. mid will end up to be an element with 
	 *  the time equal to or past the target packet time. Note that min, mid, and max are
     *  not the actual index to the ring buffer, but represent the number of elements
     *  into the buffer past its start.
     */
	min = 0;
	max = datar_cnt - 1;
	while (min < max) {
		mid = min + (max - min + 1) / 2;
		if (compare_index(target,datar_normalize(datar_start + mid)) < 0) {
			// target time is before the element at mid, so
            //  max now becomes mid - 1.
			max = mid - 1;
		} else {
			// target time is equal to or newer than target time, 
            //  so min becomes mid.
			min = mid;
		}
	}
    // Now min == max. See if we found an exact match.
    *index = datar_normalize(datar_start + min);
    pkt_time_str(data_store_g[*index]->packet_info.packet_time,packettime);
	if (compare_index(target,*index) == 0) {
		// Found an exact match
        writelog(6,"search_data_bfr: Found exact match for packet time %s at index %d\n",
                packettime,*index);
		return 0;
	} else {
        /*
         * Not an exact match. Return the next packet. We know there is a packet newer than the 
         *  requested time. If there wasn't it would have been caught earlier
         */
        *index = datar_normalize(*index + 1);
        writelog(6,"search_data_bfr: Found a packet of time %s newer than the target time at index %d \n",
                packettime,*index);
		return 2;
	}
}

void 
store_element(int index, struct packetinfo *element,char *sdr_pkt) {
    /*
     * Stores *element into the data ring at the indicated index, with the ADC data coming from *sdr_pkt
     *  This function is not thread-safe by itself. The mutex_datastore must be locked by the
     *  calling function.
     * The data_store_g array contains pointers to allocated memory, and each of those contains a pointer
     *  to allocated memory for the sdr_packet. We can't just copy the whole datastore structure because we'd
     *  loose the pointers to the allocated memory for the sdr_packet. So we copy the packetinfo structure to
     *  that part of the memory for the packet, then copy the sdr_packet into the allocated memory for it.
     */
    struct datastore *target = data_store_g[index]; // pointer to the element in the global data_store_g
    
    // copy the datastore's packetinfo structure, then copy the data. The valid flag should already have been set.
    memcpy(&target->packet_info,element,sizeof(struct packetinfo));
    memcpy(target->sdr_packet,sdr_pkt,element->packet_len);
}

void 
retr_element(int index, struct packetinfo *element,char *sdr_pkt) {
    /*
     * Retrieves *element from the data ring at the indicated index, and places the ADC data in *data.
     *  This function is not thread-safe by itself. The mutex_datastore must be locked by the
     *  calling function.
     */
    struct datastore *target = data_store_g[index]; // pointer to the element in the global data_store_g
    
    // Copy the datastore structure, and the data
    memcpy(element,&target->packet_info,sizeof(struct packetinfo));
    memcpy(sdr_pkt,target->sdr_packet,target->packet_info.packet_len);
}

void datar_rotate_right(int start, int end) {
    /*
     * Rotates the datastore pointers, from start toward end, with the one at
     *  end being rotated to the start. If the ring is full, start == end, so we have
     *  to accommodate that by starting the loop at end-1, the move the last pointer
     *  outside the loop.
     */
    struct datastore *temp_ptr;
    int dest;

    temp_ptr = data_store_g[end];
    for (dest = datar_normalize(end-1); dest != start; dest = datar_normalize(dest - 1)) {
        data_store_g[datar_normalize(dest+1)] = data_store_g[dest];
    }
    data_store_g[datar_normalize(start+1)] = data_store_g[start];
    data_store_g[start] = temp_ptr;
}

int 
datar_normalize(int offset) {
	/*
	 * Normalizes the data ring offset to within the ring bounds.
	 */
	return (offset + DATASIZE) % DATASIZE;
}

int
data_ring_ops(struct packetinfo *element, char *pkt_img, PACKETTIME target_time, 
        enum ringbfract_t action) 
{
    /*
     * Manages the data ring. The data ring is composed of a ring buffer of pointers to type
     *  datastore, with each datastore structure containing a pointer to a buffer holding the 
	 *  actual packet. This function manages the ring buffer, and is designed to be 
	 *  thread safe with the mutex mutex_datastore. Elements are sorted by PKT_TIME, 
	 *  which is the time of the data in the packet being stored. 
	 *
     * The ring can contain up to DATASIZE number of elements, and the array is allocated to 
	 *  this many elements. There are two indexes to the array, DATAR_START and DATAR_END, and
     *  DATAR_CNT to keep track of how many elements are currently in the buffer.
	 *
	 * The data stored in the buffer will never occupy every possible time stamp. Time stamps
	 *  have a granualarity of 1ms and packets normally come about once per second, so there will
	 *  normally be many time stamps available between adjacent stored elements in the buffer. 
     *  The time stamp of a new element to be stored is checked against what's currently in the 
	 *  buffer. If the new element is before the oldest element currently in the buffer, the new
     *  one is prepended to the buffer if it is not full. If the new element is newer than the
     *  newest element in the buffer, the new element is appended to the buffer. If the buffer 
     *  was full before this, the oldest element in the buffer is discarded. If the new element
     *  has a timestamp between two existing elements in the buffer, it is inserted at the proper
     *  place and the oldest element in the buffer discarded if the buffer was full. If the new
     *  element has the same timestamp of an existing element, it replaces that element.
     *
     * Arguments are ELEMENT, for passing the datastore structure, PKT_IMG for passing the 
     *  packet image, and ACTION to tell what action to perform.
	 *
     * When a new element is presented for storage or one is being retrieved, ELEMENT points 
     *  to the element, and PKT_IMG points to the packet image.
     *
     * ACTION can be one of the following:
     *  RING_ALLOC -- Allocates the data buffers for each element in the ring.
     *  RING_DEALLOC -- De-allocates the data buffers for each element in the ring.
     *  RING_STORE -- Stores the given element at PKT_TIME.
     *  RING_RETR -- Retrieves the next packet after the given PKT_TIME
	 *  RING_ELEMENTS_STORED -- Returns the number of elments currently stored on the buffer
	 *  RING_ELEMENTS_FREE -- Returns the number of elements still free in the buffer.
	 *
	 * Return values for various actions:
	 *  RING_ALLOC -- Returns 0 on success. Program is closed on failure.
	 *  RING_DEALLOC -- Returns 0 on success. Program is closued on failure.
	 *	RING_STORE -- 0 - packet stored successfully. 
     *	              1 - packet not stored (too old and the buffer is full)
	 *	RING_RETR -- 0 - packet returned is next one past the given time stamp
	 *				 1 - No packets available after the given time stamp
	 *				 2 - No packets are stored in ring buffer.
     */
	char time_str[50];
    int i,ret, saved_end, result, location;

    pthread_mutex_lock(&mutex_datastore);
    switch (action) {
		case RING_ELEMENTS_STORED:
			return datar_cnt;
			break;
		case RING_ELEMENTS_FREE:
			return DATAR_ELEMENTS_FREE;
			break;
		case RING_ALLOC:	
			// Allocate new datastore structure and packet storage buffers.
            writelog(2,"data_ring_ops: Allocating memory for the data ring\n");
			for (i = 0; i < DATASIZE; i++) {
				data_store_g[i] = (struct datastore *)malloc(sizeof(struct datastore));
				if(data_store_g[i] == NULL) {
					fail("data_ring_ops: Failed to allocate memory for data_store_g array\n");
				}
				data_store_g[i]->sdr_packet = (char *)malloc(sdr_pkt_size());
				if(data_store_g[i]->sdr_packet == NULL) {
					fail("data_ring_ops: Failed to allocate memory for data_store_g sdr_packet "
							"buffer\n");
				}
			}
            // Initialize the indexes.
			datar_end = datar_start = datar_cnt = 0;
            writelog(5,"data_ring_ops: Data ring successfully allocated\n");
            ret = 0;
			break;

		case RING_DEALLOC:
			// frees the memory taken by the data buffers. Does not free data_store_g. This is
			//  done when we need to reallocate due to a change in WinSDR config, such as 
			//  different number of channels, sps, etc.
            writelog(2,"data_ring_ops: Deallocating the data ring memory\n");
            for (i = 0; i < DATASIZE; i++) {
                free(data_store_g[i]->sdr_packet);
                free(data_store_g[i]);
            }
			datar_end = datar_start = datar_cnt = 0;
            writelog(5,"data_ring_ops: Data ring memory successfully deallocated\n");
            ret = 0;
			break;

        case RING_STORE:
            /*
			 * Stores the given element in the ring.
             */
            if(DATAR_ISEMPTY) {
                /*
                 * The ring is empty. Store this element at datar_end, and increment datar_end
                 *  and datar_cnt.
                 */
                store_element(datar_end,element,pkt_img);
				pkt_time_str(element->packet_time,time_str);
                saved_end = datar_end;
				datar_end = datar_normalize(datar_end + 1);
				datar_cnt++;
                writelog(4,"data_ring_ops: Stored first element, with ID %ld, length %d and time %s "
                        "into the ring, at index %d, which now contains %d elements\n",
                        element->packet_id,element->packet_len,time_str,saved_end,datar_cnt);
				ret = 0;
            } else {
                result = search_data_bfr(element->packet_time, &location);
                /* 
                 * Buffer empty -- location is set to -1 and -2 is returned.
                 * Nothing that old -- location is set to the first packet and +1 is returned.
                 * Nothing that new -- location is set to the newest packet and -1 is returned.
                 * Not an exact match -- location is set to next newer packet and +2 is returned
                 * An exact match -- location is set to the matching packet and 0 is returned.
                 *
                 * The ring is not empty. Search the data in the ring to see where the new element
                 *  should go. There will be four possibilities:
                 *   The new element is older than any others and should go at the start of 
                 *    the ring.
                 *   The new element is newer than any others and should go at the end of 
                 *    the ring.
                 *   The new element fits between two existing elements.
                 *   The new element has the same time stamp as an existing element.
                 */

                if(result == 1) {
                    /*
                     * The new element is older than any of the data currently in the ring. If
                     *  the ring is full we do not accept the data. If the ring is not full,
                     *  move all the element pointers up one slot and move the next unused
                     *  element pointer to the beginning of the ring.
                     */
                    if(DATAR_ISFULL) {
                        ret = 1;
                    } else { // Ring is not full
                        /*
                         * We're putting the new element at the start of the ring, so we rotate
                         *  the entire ring to the right, using the one at datar_end
                         *  (the oldest) for the new element. Then update the pointers.
                         */
                        datar_rotate_right(datar_start,datar_end);
                        store_element(datar_start,element,pkt_img);
                        pkt_time_str(element->packet_time,time_str);
                        saved_end = datar_end;
                        datar_end = datar_normalize(datar_end + 1);
                        datar_cnt++;
                        writelog(4,"data_ring_ops: Stored oldest element, with ID %ld, length %d and time %s "
                                "into the ring, at start of ring index %d. Ring contains %d elements\n",
                                element->packet_id,element->packet_len,time_str,saved_end,datar_cnt);
                        ret = 0;
                    }
                } else if(result == -1) {
                    /*
                     * The new element is newer than the latest element in the ring. It goes 
                     *  at the end of the ring. It will utilize the pointer at datar_end. 
                     */
                    store_element(datar_end,element,pkt_img);
                    pkt_time_str(element->packet_time,time_str);
                    saved_end = datar_end;
                    datar_end = datar_normalize(datar_end + 1);
                    // If the ring is full, update the start pointer, otherwise increment 
                    //  the count
                    if(DATAR_ISFULL) {
                        datar_start = datar_normalize(datar_start + 1);
                    } else {
                        datar_cnt++;
                    }
                    writelog(4,"data_ring_ops: Stored newest element, with ID %ld, length %d and time %s "
                            "into the ring, at end of ring at index %d. Ring contains %d elements\n",
                            element->packet_id,element->packet_len,time_str,saved_end,datar_cnt);
                    ret = 0;
                } else if(result == 2) {
                    /*
                     * The new element goes inbetween two existing elements, and location 
                     *  points to the element just after where it should go. Insert it there.
                     * We utilize the pointer at datar_end, move it to the ring just after the
                     * element at LOCATION, and move all ements after that up by one slot.
                     */
                    datar_rotate_right(location,datar_end);
                    store_element(location,element,pkt_img);
                    pkt_time_str(element->packet_time,time_str);
                    saved_end = datar_end;
                    datar_end = datar_normalize(datar_end + 1);
                    if(DATAR_ISFULL) {
                        datar_start = datar_normalize(datar_start + 1);
                    } else {
                        datar_cnt++;
                    }
                    writelog(4,"data_ring_ops: Stored element between others, with ID %ld, length %d and time %s "
                            "into the ring, at index %d. Ring contains %d elements\n",
                            element->packet_id,element->packet_len,time_str,saved_end,datar_cnt);
                    ret = 0;
                } else if(result == 0) {
                    /*
                     * The new element overwrites the element at location. no pointers 
                     *  get changed.
                     */
                    store_element(location,element,pkt_img);
                    pkt_time_str(element->packet_time,time_str);
                    writelog(4,"data_ring_ops: Overwrote element, with ID %ld, length %d and time %s "
                            "into the ring, at index %d. Ring contains %d elements\n",
                            element->packet_id,element->packet_len,time_str,location,datar_cnt);
                    ret = 0;
                } else {
                    /*
                     * Error in logic. Should not have gotten here.
                     */
                    fail("data_ring_ops: OOps...  Logic error in RING_STORE");
                    ret = 0;
                }
            }
            break;
        case RING_RETR:
            /*
             * Retrieve the next element from the ring with a time after given target time.
             * If the request can be met. If the requested time is earlier 
             *  than the oldest_time or newer than the newest time, we return 1.
			 *
			 *   search_data_bfr returns the following:
			 * Buffer empty -- location is set to -1 and -2 is returned.
			 * Nothing that old -- location is set to the first packet and +1 is returned.
			 * Nothing that new -- location is set to the newest packet and -1 is returned.
			 * Not an exact match -- location is set to next newer packet and +2 is returned
			 * An exact match -- location is set to the matching packet and 0 is returned.
			 *
			 * We are interested in the next packet after the requested time, if there is any. We
			 *  increment the search time by one millisecond (the granularity of WinSDR data) and
			 *  do the search. That way we can accept anything matching or after the search time.
			 */
			target_time += 0.001;
            pkt_time_str(target_time,time_str);
            writelog(6,"data_ring_ops: Request for packet after time %s\n",time_str);
            result = search_data_bfr(target_time, &location);
            if(result >= 0) {
				retr_element(location,element,pkt_img);
				pkt_time_str(element->packet_time,time_str);
				writelog(5,"data_ring_ops: Retrieved element from the data ring with time "
					"%s, at index %d\n", time_str,location);
				ret = 0;
            } else { // Buffer is empty or the requested time is after the last element.
                element = NULL;
                ret = 1;
            }
            break;
        default:
            fail("data_ring_ops: Called with an invalid action: %d\n",action);
            ret = 1; // for completeness
            break;
    }
    pthread_mutex_unlock(&mutex_datastore);
    return ret;
}

