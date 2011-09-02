// time.c


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


char *get_now_str(char *dest) {
    /*
     * Puts the current time into a string, including that date and time to microseconds.
     * If dest is NULL, returns a dynamically-allocated string containing the time value.
     *  This should be cleared by the caller to avoid memory leaks.
     * If dest is not NULL, places the time string in *dest, and returns dest. Dest should be 
     *  at least 27 bytes long, including the final null byte.
     */
    time_t now;
    struct timeval tv;
    struct tm *ts;

    gettimeofday(&tv,NULL);
    now = time(NULL);
    ts = gmtime(&tv.tv_sec);
    if(dest != NULL) {
        sprintf(dest,"%02d-%02d-%04d %02d:%02d:%02d.%06lu",
            ts->tm_mon+1,ts->tm_mday,ts->tm_year+1900,ts->tm_hour,ts->tm_min,
			ts->tm_sec,tv.tv_usec);
        return dest;
    } else {
        char nowstr[50];
        sprintf(nowstr,"%02d-%02d-%04d %02d:%02d:%02d.%06lu",
            ts->tm_mon+1,ts->tm_mday,ts->tm_year+1900,ts->tm_hour,ts->tm_min,
			ts->tm_sec,tv.tv_usec);
        return strdup(nowstr);
    }
}

char 
*mktimestr(struct tm *tm) {
    /*
     * Creates a string to display the current time. Not thread safe.
     */
    static char timestr[50];
    sprintf(timestr,"%02d/%02d/%02d %02d:%02d:%02d",tm->tm_mon+1,tm->tm_mday,
            tm->tm_year+1900,tm->tm_hour,tm->tm_min,tm->tm_sec);
    return timestr;
}

void
time_to_str(struct tm *tmst,char *dest) {
    /*
     * Puts the time given into the given string to display the current time.
     */
    sprintf(dest,"%02d/%02d/%02d %02d:%02d:%02d",tmst->tm_mon+1,tmst->tm_mday,
            tmst->tm_year+1900,tmst->tm_hour,tmst->tm_min,tmst->tm_sec);
}

void unix_time_to_str(time_t utime, char *dest) {
    /*
     * Puts the time given into the given string to display the current time.
     */
    struct tm tm;
	gmtime_r(&utime,&tm);
    time_to_str(&tm,dest);
}


void 
data_hdr_to_timeval(char *bfr,struct mstime *data_time) {
    // Converts fields in the data header to a mstime structure

    struct tm ptime;
    short *sbuf = (short *)bfr;

    ptime.tm_year = sbuf[0] - 1900;
    ptime.tm_mon = sbuf[1] - 1;
    ptime.tm_mday = sbuf[3];
    ptime.tm_hour = sbuf[4];
    ptime.tm_min = sbuf[5];
    ptime.tm_sec = sbuf[6];
    data_time->sec = timegm(&ptime);
    data_time->msec = sbuf[7];
}

void 
timeval_to_data_hdr(struct mstime *data_time, char *bfr) {
    // Places fields from the mstime into the data header at *bfr
    struct tm ptime;
    short *sbuf = (short *)bfr;

    gmtime_r(&data_time->sec,&ptime);
    sbuf[0] = ptime.tm_year + 1900;
    sbuf[1] = ptime.tm_mon + 1;
    sbuf[2] = 0;
    sbuf[3] = ptime.tm_mday;
    sbuf[4] = ptime.tm_hour;
    sbuf[5] = ptime.tm_min;
    sbuf[6] = ptime.tm_sec;
    sbuf[7] = data_time->msec;
}

time_t
get_packet_time(char *data_hdr) {
    /*
     * REturns the Unix time of the packet, given the buffer starting with the data header
     */
    struct tm ptime;

    ptime.tm_year = *((short *)&data_hdr[DATAHDR_POS_YEAR_REL]) - 1900;
    ptime.tm_mon = *((short *)&data_hdr[DATAHDR_POS_MON_REL]) - 1;
    ptime.tm_mday = *((short *)&data_hdr[DATAHDR_POS_DOM_REL]);
    ptime.tm_hour = *((short *)&data_hdr[DATAHDR_POS_HOUR_REL]);
    ptime.tm_min = *((short *)&data_hdr[DATAHDR_POS_MIN_REL]);
    ptime.tm_sec = *((short *)&data_hdr[DATAHDR_POS_SEC_REL]);
    return timegm(&ptime);
}

PACKETTIME decode_pkt_time(char *data_hdr) {
    /*
     * Decodes the time in the data header of the packet, into a packettime structure
     */
	PACKETTIME temp;

    temp = get_packet_time(data_hdr);
    // Milliseconds is a short int starting at byte 14 of the data header.
    temp +=  (double)*((short *)&data_hdr[DATAHDR_POS_MSEC_REL]) / 1000.0;
	return temp;
}

void pkt_time_str(PACKETTIME pkt_time,char *time_str) {
	/*
	 * Places an ascii rendition of the given PKT_TIME into TIME_STR
	 */
	struct tm pktm;
	char msecstr[10];
	time_t ptime;

	ptime = (time_t)pkt_time;
	gmtime_r(&ptime,&pktm);
	time_to_str(&pktm,time_str);
	sprintf(msecstr,".%03d",(int)((pkt_time - ptime) * 1000));
	strcat(time_str,msecstr);
}

int cmp_pkt_time(PACKETTIME a, PACKETTIME b) {
    /*
     * Compares two packettime structures. If returns 1 if a>b, 0 if a==b, -1 if a<b
     */
    if(a > b) {
        return 1;
    } else if(a < b) {
        return -1;
	}
    return 0;
}
