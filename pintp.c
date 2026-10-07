/*
 * PiNTP - Lightweight SNTP time synchronization client for AROS (aarch64)
 * Designed specifically for Raspberry Pi 3B+ (which lacks a hardware RTC).
 *
 * Compile: aarch64-aros-gcc -o PiNTP pintp.c -lnet
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/select.h>

#include <exec/types.h>
#include <libraries/locale.h>
#include <proto/exec.h>
#include <proto/locale.h>

#define NTP_PORT 123
#define NTP_PACKET_SIZE 48
#define NTP_EPOCH_DELTA 2208988800ULL   /* Seconds from 1900 to 1970 */
#define AMIGA_EPOCH_DELTA 2461449600ULL /* Seconds from 1900 to 1978 (Amiga/AROS Epoch) */
#define RECV_TIMEOUT_S 5
#define MIN_NTP_REPLY 4                  /* LI+Version+Mode, Stratum, Poll, Precision */

static void print_usage(void) {
    printf("PiNTP - SNTP Time Client for AROS aarch64 (Raspberry Pi 3B+)\n");
    printf("Perfect for setting the software clock on boards without RTC.\n\n");
    printf("USAGE:\n");
    printf("  PiNTP [options] [server]\n\n");
    printf("OPTIONS:\n");
    printf("  -s            Set system clock from NTP response\n");
    printf("  -z <offset>   Timezone offset from UTC (seconds, e.g. 3600, -18000, 19800)\n");
    printf("  -v            Verbose output\n");
    printf("  -h            Show this help\n\n");
    printf("Default server: pool.ntp.org\n");
}

/*
 * Parse timezone offset from UTC.
 * Accepts:
 *   - seconds as plain integer       (e.g. 3600, -18000)
 *   - comfortable HH / HH:MM forms  (e.g. +1, -5, +5:30) as in docs
 * Returns offset in SECONDS, or INT_MIN on error.
 */
static int parse_tz(const char *tz_str) {
    long secs = 0;
    char *end = NULL;

    if (!tz_str || tz_str[0] == '\0')
        return INT_MIN;

    /* Pure integer -> interpret as seconds (most explicit form) */
    secs = strtol(tz_str, &end, 10);
    if (end && *end == '\0' && end != tz_str)
        return (int)secs;

    /* Otherwise fall back to the +/-HH[:MM] form given in usage text */
    int hours = 0, mins = 0, consumed = 0;
    int n = sscanf(tz_str, "%d:%d%n", &hours, &mins, &consumed);
    if (n < 1)
        return INT_MIN;

    long total = (long)hours * 60L + (long)mins; /* minutes */
    return (int)(total * 60L);
}

int main(int argc, char **argv) {
    char *server = "pool.ntp.org";
    int set_clock = 0;
    int verbose = 0;
    int tz_offset_secs = 0;
    int tz_provided = 0; /* 0 = none, 1 = from -z / locale, 2 = from TZ env */

    /* Parse command line arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0) {
            set_clock = 1;
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "-h") == 0) {
            print_usage();
            return 0;
        } else if (strcmp(argv[i], "-z") == 0 && i + 1 < argc) {
            tz_offset_secs = parse_tz(argv[++i]);
            if (tz_offset_secs == INT_MIN) {
                printf("Invalid timezone offset: %s\n", argv[i]);
                print_usage();
                return 1;
            }
            tz_provided = 1;
        } else if (argv[i][0] != '-') {
            server = argv[i];
        }
    }

    /* Auto-detect timezone */
    if (!tz_provided) {
        char *env_tz = getenv("TZ");
        if (env_tz && *env_tz) {
            if (verbose) printf("[INFO] Using timezone from ENV:TZ\n");
            tz_provided = 2;
        } else {
            /* Try reading the AROS locale.library */
            struct Library *LocaleBase = OpenLibrary((CONST_STRPTR)"locale.library", 38);
            if (LocaleBase) {
                struct Locale *loc = OpenLocale(NULL);
                if (loc) {
                    /* loc_GMTOffset is "offset from local to GMT, positive for zones
                       west of Greenwich", i.e. (GMT - local). So local = GMT - gmtOffset. */
                    tz_offset_secs = -(int)loc->loc_GMTOffset * 60;
                    tz_provided = 1;
                    if (verbose)
                        printf("[INFO] Auto-detected Timezone offset from Locale: %d seconds\n",
                               tz_offset_secs);
                    CloseLocale(loc);
                }
                CloseLibrary(LocaleBase);
            }
        }
    }

    if (!tz_provided && set_clock) {
        printf("WARNING: Setting clock with no timezone source (-z / ENV:TZ / locale). UTC will be used.\n");
    }

    /* Build the NTP request packet */
    unsigned char msg[NTP_PACKET_SIZE];
    memset(msg, 0, NTP_PACKET_SIZE);
    msg[0] = 0x23; /* LI = 0, VN = 4, Mode = 3 (Client) */

    /* Resolve server (support dotted quad directly too) */
    unsigned long inet_addr_ip = inet_addr(server);
    struct hostent host_storage;
    unsigned long h_addr_4;
    struct hostent *host = NULL;

    if ((long)inet_addr_ip != -1) {
        /* Already an IPv4 address */
        h_addr_4 = inet_addr_ip;
        host_storage.h_addr = (char *)&h_addr_4;
        host_storage.h_length = 4;
        host_storage.h_addrtype = AF_INET;
        host = &host_storage;
    } else {
        host = gethostbyname(server);
    }

    if (!host || host->h_addrtype != AF_INET || host->h_length != 4) {
        printf("Failed to resolve hostname: %s\n", server);
        return 1;
    }

    /* AROSTCP resolver marks unresolvable names with a bogus broadcast/zero
     * address (255.255.255.255 or 0.0.0.0). Sending to it would fail with
     * EACCES in AROSTCP (it is treated as a broadcast), so fail early with
     * a clear message instead. */
    {
        unsigned long bad_ip = *(unsigned long *)host->h_addr;
        if (bad_ip == 0xFFFFFFFFUL || bad_ip == 0) {
            printf("Failed to resolve hostname: %s\n", server);
            printf("  (DNS returned a broadcast/zero address - the AROSTCP resolver is\n");
            printf("   not reaching its nameserver. Configure NAMESERVER in\n");
            printf("   'System:Network/AROSTCP/db/netdb-myhost' (use your router/gateway IP),\n");
            printf("   restart the stack, or pass an NTP server IP directly, e.g.\n");
            printf("   'PiNTP -s 162.159.200.123')\n");
            return 1;
        }
    }

    /* Open UDP socket */
    int sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd < 0) {
        perror("Failed to create socket");
        return 1;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(NTP_PORT);
    memcpy(&serv_addr.sin_addr.s_addr, host->h_addr, host->h_length);

    if (verbose) {
        unsigned long resolved = *(unsigned long *)host->h_addr;
        printf("[INFO] Sending SNTP query to %s (Port %d) as %ld.%ld.%ld.%ld...\n",
               server, NTP_PORT,
               (resolved >> 24) & 0xff, (resolved >> 16) & 0xff,
               (resolved >> 8) & 0xff, resolved & 0xff);
    }

    if (sendto(sockfd, msg, NTP_PACKET_SIZE, 0,
               (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Failed to send data");
        printf("  (errno=%d)\n", errno);
        if (errno == EACCES)
            printf("Hint: sendto() got EACCES. This can happen if the destination address\n"
                   "      is 0.0.0.0 or a broadcast address (a DNS failure may resolve to 0.0.0.0).\n"
                   "      Verify with 'PiNTP -v <server>' what IP was resolved, or test directly:\n"
                   "      'PiNTP 162.159.200.123'. Also check the AROSTCP interface/route:\n"
                   "      'netstat -r' and 'ifconfig'.\n");
        close(sockfd);
        return 1;
    }

    /* Wait for reply with timeout */
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(sockfd, &readfds);

    struct timeval timeout;
    timeout.tv_sec = RECV_TIMEOUT_S;
    timeout.tv_usec = 0;

    if (select(sockfd + 1, &readfds, NULL, NULL, &timeout) <= 0) {
        printf("Timeout waiting for NTP response.\n");
        close(sockfd);
        return 1;
    }

    /* Receive and validate packet size */
    socklen_t serv_len = sizeof(serv_addr);
    ssize_t nrecv = recvfrom(sockfd, msg, NTP_PACKET_SIZE, 0,
                             (struct sockaddr *)&serv_addr, &serv_len);
    close(sockfd);

    if (nrecv < MIN_NTP_REPLY) {
        printf("Invalid (too short) NTP response received.\n");
        return 1;
    }

    /* Mode must be 4 (Server) */
    if ((msg[0] & 0x07) != 4) {
        printf("NTP reply has unexpected mode (not a server response).\n");
        return 1;
    }

    /* LI is bits 6-7; value 3 means "clock not synchronized" -> discard */
    if ((msg[0] >> 6) == 3) {
        printf("NTP server reports its clock as not synchronized.\n");
        return 1;
    }

    /* Extract transmit timestamp (bytes 40-43) */
    uint32_t ntp_seconds = ((uint32_t)msg[40] << 24) |
                           ((uint32_t)msg[41] << 16) |
                           ((uint32_t)msg[42] << 8) |
                           (uint32_t)msg[43];
    uint8_t stratum = msg[1];

    if (verbose) {
        printf("[INFO] NTP Stratum: %d\n", stratum);
        printf("[INFO] Raw NTP timestamp (since 1900): %u\n", ntp_seconds);
    }

    if (ntp_seconds == 0) {
        printf("Invalid NTP response received (zero timestamp).\n");
        return 1;
    }

    if (stratum == 0) {
        printf("NTP Kiss-of-Death (stratum 0): server is rate-limiting or unreachable.\n");
        return 1;
    }

    /* Convert to Unix time (fractional seconds are ignored for SNTP) */
    time_t unix_time = (time_t)ntp_seconds - (time_t)NTP_EPOCH_DELTA;

    if (verbose) {
        uint32_t aros_time = ntp_seconds - AMIGA_EPOCH_DELTA;
        printf("[INFO] AROS Epoch timestamp (since 1978): %u\n", aros_time);
    }

    /* Compute local time:
       - tz_provided == 2 -> respect TZ env via localtime()
       - else apply explicit offset manually          */
    struct tm *tm_info;
    if (tz_provided == 2) {
        tm_info = localtime(&unix_time);
    } else {
        time_t local_unix_time = unix_time + tz_offset_secs;
        tm_info = gmtime(&local_unix_time);
    }

    /* Nightmare: DST. AROS locale knows only a fixed offset and no DST rules,
       so we cannot apply DST automatically. Only -z / ENV:TZ provide it. */
    if (tz_provided && set_clock && verbose) {
        printf("[INFO] Note: AROS locale offset has no DST rules; verify vs. your location.\n");
    }

    const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    if (tm_info == NULL || tm_info->tm_mon < 0 || tm_info->tm_mon > 11) {
        printf("Failed to convert time.\n");
        return 1;
    }

    char date_str[64];
    snprintf(date_str, sizeof(date_str), "%02d-%s-%02d",
             tm_info->tm_mday, months[tm_info->tm_mon], (tm_info->tm_year % 100));

    char time_str[16];
    snprintf(time_str, sizeof(time_str), "%02d:%02d:%02d",
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);

    printf("Time from %s: %s %s\n", server, date_str, time_str);

    /* Update the AROS software clock. AROS's settimeofday() is not implemented,
       so we go through the CLI 'Date' command, which sets both date and time
       via timer.device (TR_SETSYSTIME). Lowercased months are conventional. */
    if (set_clock) {
        for (char *p = date_str + 3; *p; p++)
            if (*p >= 'A' && *p <= 'Z') *p = *p - 'A' + 'a';

        char cmd[128];
        snprintf(cmd, sizeof(cmd), "Date %s %s", date_str, time_str);
        if (verbose) printf("[INFO] Executing: %s\n", cmd);

        int res = system(cmd);
        if (res == 0) {
            printf("AROS software clock successfully updated.\n");
        } else {
            printf("Failed to set system clock.\n");
        }
    }

    return 0;
}
