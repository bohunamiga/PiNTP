/*
    Copyright (C) 2024, The AROS Development Team. All rights reserved.

    Desc: SNTP (RFC 4330) time synchronization support for the Time preferences.
          Based on the PiNTP client (pintp.c).
*/

/*********************************************************************************************/

#include "global.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/select.h>

/*********************************************************************************************/

/*
 * bsdsocket.library is opened automatically by net.lib's autoinit code
 * (linked in via uselibs="net") because this file references the global
 * "SocketBase" symbol declared by <proto/bsdsocket.h>.
 */
#define NTP_PORT            123
#define NTP_PACKET_SIZE     48
#define NTP_EPOCH_DELTA     2208988800UL    /* Seconds from 1900 to 1970 */
#define AMIGA_EPOCH_DELTA   2461449600UL    /* Seconds from 1900 to 1978 */
#define UNIX_TO_AMIGA       (AMIGA_EPOCH_DELTA - NTP_EPOCH_DELTA) /* 252460800 */
#define RECV_TIMEOUT_S      5
#define MIN_NTP_REPLY       NTP_PACKET_SIZE /* need bytes 40-43 (transmit timestamp) */
#define NTP_SERVER_DEFAULT  "162.159.200.123"

/*********************************************************************************************/

/*
 * Parse a timezone offset from UTC.
 * Accepts:
 *   - seconds as a plain integer   (e.g. 3600, -18000, 19800)
 *   - a signed HH / HH:MM form     (e.g. +1, -5, +5:30)
 * An empty string means UTC (0).
 * Returns the offset in seconds, or LONG_MIN on error.
 */
LONG ParseTZOffset(CONST_STRPTR tz_str)
{
    char *end = NULL;
    LONG secs;
    int sign = 1, hours = 0, mins = 0, n;
    CONST_STRPTR p;

    if (!tz_str)
        return 0;

    while (*tz_str == ' ' || *tz_str == '\t')
        tz_str++;

    if (*tz_str == '\0')
        return 0;

    /* Separate an explicit sign so that "-5:30" means -5h30m, not -5h+30m */
    p = tz_str;
    if (*p == '+')
    {
        sign = 1;
        p++;
    }
    else if (*p == '-')
    {
        sign = -1;
        p++;
    }

    /* HH[:MM] form (with or without sign) is always HOURS */
    if (strchr((STRPTR)tz_str, ':'))
    {
        n = sscanf((STRPTR)p, "%d:%d", &hours, &mins);
        if (n < 2 || mins < 0 || mins > 59)
            return LONG_MIN;

        return (LONG)sign * (hours * 60L + mins) * 60L;
    }

    /* Plain number: small values are convenient whole HOURS ("+2", "-5"),
       large values are SECONDS ("3600", "-18000", "19800") */
    secs = (LONG)strtol((STRPTR)tz_str, &end, 10);
    if (!(end && *end == '\0' && end != (char *)tz_str))
        return LONG_MIN;

    if (secs >= -24 && secs <= 24)
        return secs * 3600L;

    return secs;
}

/*********************************************************************************************/

static void NTPError(STRPTR errbuf, ULONG errlen, ULONG msgid)
{
    if (!errbuf || errlen == 0)
        return;

    strncpy(errbuf, (STRPTR)MSG(msgid), errlen - 1);
    errbuf[errlen - 1] = '\0';
}

/*********************************************************************************************/

/*
 * Query an NTP server and return the resulting time as AROS (Amiga) seconds,
 * adjusted by tz_offset_secs. Returns TRUE on success, FALSE on failure
 * (errbuf then contains a localized error message).
 */
BOOL NTPGetTime(CONST_STRPTR server, LONG tz_offset_secs,
                ULONG *amiga_secs, STRPTR errbuf, ULONG errlen)
{
    unsigned char msg[NTP_PACKET_SIZE];
    unsigned long resolved_ip;
    struct hostent host_storage;
    struct hostent *host = NULL;
    unsigned long h_addr_4;
    char *h_addr_list_4[2];
    struct sockaddr_in serv_addr;
    struct timeval timeout;
    fd_set readfds;
    int sockfd;
    ssize_t nrecv;
    socklen_t serv_len;
    uint32_t ntp_seconds;
    uint8_t stratum;
    time_t unix_time;

    if (errbuf && errlen)
        errbuf[0] = '\0';

    if (!server || !*server)
        server = (CONST_STRPTR)NTP_SERVER_DEFAULT;

    /* Build the NTP request packet: LI = 0, VN = 4, Mode = 3 (Client) */
    memset(msg, 0, NTP_PACKET_SIZE);
    msg[0] = 0x23;

    /* Resolve the server (a dotted-quad is used directly) */
    resolved_ip = inet_addr((STRPTR)server);
    if ((long)resolved_ip != -1)
    {
        h_addr_4 = resolved_ip;
        memset(&host_storage, 0, sizeof(host_storage));
        h_addr_list_4[0] = (char *)&h_addr_4;
        h_addr_list_4[1] = NULL;
        host_storage.h_addr_list = h_addr_list_4;
        host_storage.h_length = 4;
        host_storage.h_addrtype = AF_INET;
        host = &host_storage;
    }
    else
    {
        host = gethostbyname((STRPTR)server);
    }

    if (!host || host->h_addrtype != AF_INET || host->h_length != 4)
    {
        NTPError(errbuf, errlen, MSG_ERR_NTP_RESOLVE);
        return FALSE;
    }

    /* AROSTCP marks unresolvable names with a broadcast/zero address. Sending
       to it fails with EACCES (it is treated as a broadcast). */
    resolved_ip = *(unsigned long *)host->h_addr;
    if (resolved_ip == 0xFFFFFFFFUL || resolved_ip == 0)
    {
        NTPError(errbuf, errlen, MSG_ERR_NTP_BADDNS);
        return FALSE;
    }

    /* Open a UDP socket */
    sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd < 0)
    {
        NTPError(errbuf, errlen, MSG_ERR_NTP_SOCKET);
        return FALSE;
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(NTP_PORT);
    memcpy(&serv_addr.sin_addr.s_addr, host->h_addr, host->h_length);

    if (sendto(sockfd, msg, NTP_PACKET_SIZE, 0,
               (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        CloseSocket(sockfd);
        NTPError(errbuf, errlen, MSG_ERR_NTP_SEND);
        return FALSE;
    }

    /* Wait for the reply with a timeout */
    FD_ZERO(&readfds);
    FD_SET(sockfd, &readfds);
    timeout.tv_secs = RECV_TIMEOUT_S;
    timeout.tv_micro = 0;

    if (select(sockfd + 1, &readfds, NULL, NULL, &timeout) <= 0)
    {
        CloseSocket(sockfd);
        NTPError(errbuf, errlen, MSG_ERR_NTP_TIMEOUT);
        return FALSE;
    }

    serv_len = sizeof(serv_addr);
    nrecv = recvfrom(sockfd, msg, NTP_PACKET_SIZE, 0,
                     (struct sockaddr *)&serv_addr, &serv_len);
    CloseSocket(sockfd);

    if (nrecv < MIN_NTP_REPLY)
    {
        NTPError(errbuf, errlen, MSG_ERR_NTP_RECV);
        return FALSE;
    }

    /* Mode must be 4 (Server) */
    if ((msg[0] & 0x07) != 4)
    {
        NTPError(errbuf, errlen, MSG_ERR_NTP_BADREPLY);
        return FALSE;
    }

    /* LI is bits 6-7; value 3 means "clock not synchronized" */
    if ((msg[0] >> 6) == 3)
    {
        NTPError(errbuf, errlen, MSG_ERR_NTP_BADREPLY);
        return FALSE;
    }

    /* Extract the transmit timestamp (bytes 40-43) */
    ntp_seconds = ((uint32_t)msg[40] << 24) |
                  ((uint32_t)msg[41] << 16) |
                  ((uint32_t)msg[42] << 8)  |
                  (uint32_t)msg[43];
    stratum = msg[1];

    if (ntp_seconds == 0 || stratum == 0)
    {
        NTPError(errbuf, errlen, MSG_ERR_NTP_BADREPLY);
        return FALSE;
    }

    /* Convert to Unix time (fractional seconds are ignored for SNTP) and
       apply the fixed timezone offset (AROS locale has no DST rules). */
    unix_time = (time_t)ntp_seconds - (time_t)NTP_EPOCH_DELTA;
    unix_time += (time_t)tz_offset_secs;

    *amiga_secs = (ULONG)((int64_t)unix_time - (int64_t)UNIX_TO_AMIGA);
    return TRUE;
}

/*********************************************************************************************/
