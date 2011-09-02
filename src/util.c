// util.c


#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
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


void closeLog(void) {
    // Closes the log file
    fflush(logfile);
    fclose(logfile);
}

int
warn(char *fmt, ...) {
    // Sends a warning to stderr
    va_list ap;
    int r;
    char timestr[50];

    pthread_mutex_lock(&mutex_stderr);
    va_start(ap,fmt);
	fprintf(stderr,"%s -- ",get_now_str(timestr));
	if(!strlen(fmt))
		fprintf(stderr,"\n");
    r = vfprintf(stderr,fmt,ap);
	fflush(stderr);
    va_end(ap);
    pthread_mutex_unlock(&mutex_stderr);
    return r;
}

int
fail(char *fmt, ...) {
    // Writes to stderr and exits
    va_list ap;
    int r;
    char timestr[50];

    pthread_mutex_lock(&mutex_stderr);
    va_start(ap,fmt);
	fprintf(stderr,"%s -- ",get_now_str(timestr));
	if(!strlen(fmt))
		fprintf(stderr,"\n");
    r = vfprintf(stderr,fmt,ap);
	fflush(stderr);
    closeLog();
    exit(EXIT_FAILURE);
    // We never get here.
    va_end(ap);
    pthread_mutex_unlock(&mutex_stderr);
    return r;
}

int
writelog(int level,char *fmt, ...) {
    /*
     * Writes to the log file. If the global DEBUG_LEVEL is as high or
     *  higher than LEVEL, the formatted text is written to the log and
     *  the number of characters written is returned. If not, nothing 
     *  is written and 0 is returned.
     * Text written to the log is formatted according to fmt, with data coming
     *  from the arguments following fmt.
     */
    va_list ap;
    int r;
    char timestr[50];

    if(debug_level_g >= level) {
        pthread_mutex_lock(&mutex_logfile);
        va_start(ap,fmt);
        fprintf(logfile,"%s -- ",get_now_str(timestr));
        if(!strlen(fmt))
            fprintf(logfile,"\n");
        r = vfprintf(logfile,fmt,ap);
        va_end(ap);
        fflush(logfile);
        pthread_mutex_unlock(&mutex_logfile);
        return r;
    }
    return 0;
}

void
hexdumplog(int level,char *msg,char *bfr,int len) {
    /*
     * Writes the message msg to the log, then a hexdump of the first len characters of bfr
     */
    char *output_str;
    int num_lines,addrlen,saved_len,output_len,bfrptr,outptr;
    int bytes_per_line = 32;
    char addrfmt[10],timestr[50];

    if(debug_level_g >= level) { 
        num_lines = (len + bytes_per_line - 1) / bytes_per_line;
        addrlen = 0;
        saved_len = len;
        // Calculate the size of the address field required.
        while(saved_len) {
            addrlen++;
            saved_len /= bytes_per_line;
        }
        if(addrlen < 4)
            addrlen = 4;

        // Create the address format
        sprintf(addrfmt,"%%0%dX",addrlen);

        // Line length is the address length, plus 3 characters per buffer byte, plus one newline
        output_len = (addrlen + bytes_per_line * 3 + 1) * num_lines;
        output_str = (char *)malloc(output_len);

        outptr = 0;
        bfrptr = 0;
        while (bfrptr < len) {
            if((bfrptr % bytes_per_line) == 0) {
                if(bfrptr) {
                    // Output a newline if not the first line
                    output_str[outptr++] = '\n';
                }
                sprintf(&output_str[outptr],addrfmt,bfrptr);
                outptr += addrlen;
            }
            sprintf(&output_str[outptr]," %02hhX",bfr[bfrptr]);
            outptr += 3;
            bfrptr++;
        }
        // Append a final newline
        output_str[outptr++] = '\n';

        // Now write to the log
        pthread_mutex_lock(&mutex_logfile);
        fprintf(logfile,"%s -- dump of %d bytes -- %s",get_now_str(timestr),len,msg);
        fwrite(output_str,1,outptr,logfile);
        fflush(logfile);
        pthread_mutex_unlock(&mutex_logfile);
    }
}

char 
do_crc(char *bfr,int cnt) {
	// REturns an 8-bit CRC (exclusive or) of the indicated number of bytes in *bfr
	int i;
	char crc = 0;

    writelog(6,"do_crc: Computing CRC\n");
	for (i = 0; i < cnt; i++) {
		crc ^= bfr[i];
	}
    writelog(6,"do_crc: CRC is 0x%02hhX\n",crc);
	return crc;
}

size_t 
data_bfr_size(void) {
    /*
     * Computes the size of the buffer needed to hold one second's worth of A/D data.
     *  Uses the global chan_info_g for  the sps, num_channels, and type
     */
    size_t elementsize;
    if (chan_info_g.type == 4) {
        elementsize = sizeof(int32_t);
    } else {
        elementsize = sizeof(int16_t);
    }
    return chan_info_g.sps * chan_info_g.num_channels * elementsize;
}

int 
sdr_pkt_size(void) {
    /*
     * Computes the size of the WinSDR packet needed to hold one second's worth of A/D data,
     *  Uses the global chan_info_g for  the sps, num_channels, and type
     */
    return (PREHDRLEN + DATAHDRLEN + data_bfr_size() + CRCLEN);
}


char 
*bytes_in_bytes(char *haystack, size_t haysize, char *needle, size_t nsize) {
    /*
     * Searches in character array haystack for the character array needle. Neither has to be 
     * null-termianted, and the search will stop after the limits stated. If needle is found 
     * in haystack, returns a pointer to the position in haystack. Otherwise, returns NULL.
	 * The search is done using the longest possible single-operation comparison (4 bytes) 
     * for arrays that are long enough. When there is not enough left in either the haystack 
     * or the needle, a shift is made to shorter (2 bytes) or single-byte comparisons.
     */
    int inmatch,increment;
	char *testneed,*savedhay;
	unsigned long val1,val2;

	int lsize = sizeof(unsigned long);
	int ssize = sizeof(unsigned short);
	char *endhay = &haystack[haysize]; // One past the end of the haystack
	char *endneed = &needle[nsize]; // One past the end of the needle.

	testneed = needle;
	inmatch = FALSE;
    // Search until we're at the end of the haystack or the end of the needle.
	while((haystack < endhay) && (testneed < endneed)) {
		if(((haystack + lsize) <= endhay) && ((testneed + lsize) <= endneed)) {
			// OK to do unsigned long tests
			val1 = *((unsigned long *)haystack);
			val2 = *((unsigned long *)testneed);
			increment = lsize;
		} else if(((haystack + ssize) <= endhay) && ((testneed + ssize) <= endneed)) {
			// Do short tests
			val1 = *((unsigned short *)haystack);
			val2 = *((unsigned short *)testneed);
			increment = ssize;
		} else {
			// character testing.
			val1 = *((unsigned char *)haystack);
			val2 = *((unsigned char *)testneed);
			increment = 1;
		}
		if(val1 == val2) {
			// Matched
			if(!inmatch) {
				// This is the start of a match. Set the flag and save the pointer.
				inmatch = TRUE;
				savedhay = haystack;
			}
			haystack += increment;
			testneed += increment;
		} else {
			// Doesn't match.
			if(inmatch) {
				// We were matching, but no longer are. Reset the pointers to start at the 
                //   place we left off when we first matched
				inmatch = FALSE;
				haystack = savedhay + 1;
				testneed = needle;
			} else {
				// We weren't in the middle of a match, so just increment the haystack 
                //   pointer by one character.
				haystack++;
			}
            /*
             * If our search is close enough to the end of the haystack such that the needle
             *  can't be there, it's not there so return NULL.
             */
            if((endhay - haystack) < nsize) {
                return NULL;
            }
		}
	}
	if (inmatch) {
		// We ended in the middle of a match. Return the saved pointer
		return savedhay;
	} else {
		return NULL;
	}
}

int 
print_state(char *func, int state, int prev_state,char *state_strings[]) {
    // Prints to the log the PREV_STATE and current STATE, using the given STATE_STRINGS
    // Returns 1 if the state has changed, 0 otherwise.
    if(state != prev_state) {
        writelog(4,"%s: State change from %s to %s\n",
            func,state_strings[prev_state],state_strings[state]);
        return 1;
    }
    return 0;
}

