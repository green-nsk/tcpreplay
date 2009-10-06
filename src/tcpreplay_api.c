/* $Id$ */

/*
 * Copyright (c) 2009 Aaron Turner.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the names of the copyright owners nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "defines.h"
#include "common.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>

#include "send_packets.h"
#include "tcpreplay_api.h"
#include "replay.h"

#ifdef USE_AUTOOPTS
#ifdef TCPREPLAY_EDIT
#include "tcpreplay_edit_opts.h"
#else
#include "tcpreplay_opts.h"
#endif
#endif



/**
 * \brief Returns a string describing the last error.
 *
 * Value when the last call does not result in an error is undefined 
 * (may be NULL, may be garbage)
 */
char *
tcpreplay_geterr(tcpreplay_t *ctx)
{
    assert(ctx);
    return(ctx->errstr);
}

/**
 * \brief Returns a string describing the last warning.  
 *
 * Value when the last call does not result in an warning is undefined 
 * (may be NULL, may be garbage)
 */
char *
tcpreplay_getwarn(tcpreplay_t *ctx)
{
    assert(ctx);
    return(ctx->warnstr);
}

/**
 * \brief Initialize a new tcpreplay context
 *
 * Allocates memory and stuff like that.  Always returns a buffer or completely
 * fails by calling exit() on malloc failure.
 */
tcpreplay_t *
tcpreplay_init()
{
    tcpreplay_t *ctx;

    ctx = safe_malloc(sizeof(tcpreplay_t));
    ctx->options = safe_malloc(sizeof(tcpreplay_opt_t));

    /* replay packets only once */
    ctx->options->loop = 1;

    /* Default mode is to replay pcap once in real-time */
    ctx->options->speed.mode = speed_multiplier;
    ctx->options->speed.speed = 1.0;

    /* Set the default timing method */
#ifdef HAVE_ABSOLUTE_TIME
    /* This is always the best (if the OS supports it) */
    ctx->options->accurate = accurate_abs_time;
#else
    /* This is probably the second best solution */
    ctx->options->accurate = accurate_gtod;
#endif

    /* set the default MTU size */
    ctx->options->mtu = DEFAULT_MTU;

    /* disable limit send */
    ctx->options->limit_send = -1;

#ifdef ENABLE_VERBOSE
    /* clear out tcpdump struct */
    ctx->options->tcpdump = (tcpdump_t *)safe_malloc(sizeof(tcpdump_t));
#endif

    if (fcntl(STDERR_FILENO, F_SETFL, O_NONBLOCK) < 0)
        tcpreplay_setwarn(ctx, "Unable to set STDERR to non-blocking: %s", strerror(errno));

#ifdef ENABLE_PCAP_FINDALLDEVS
    ctx->intlist = get_interface_list();
#else
    ctx->intlist = NULL;
#endif

    ctx->abort = false;
    return ctx;
}

#ifdef USE_AUTOOPTS
/**
 * \brief Parses the GNU AutoOpts options for tcpreplay
 *
 * If you're using AutoOpts with tcpreplay_api, then just call this after
 * optionProcess() and it will parse all the options for you.  As always,
 * returns 0 on success, and -1 on error & -2 on warning.
 */
int 
tcpreplay_post_args(tcpreplay_t *ctx)
{
    char *temp, *intname;
    char ebuf[SENDPACKET_ERRBUF_SIZE];
    int int1dlt, int2dlt;
    tcpreplay_opt_t *options;
    int warn = 0;

    options = ctx->options;

#ifdef DEBUG
    if (HAVE_OPT(DBUG))
        debug = OPT_VALUE_DBUG;
#else
    if (HAVE_OPT(DBUG)) {
        warn ++;
        tcpreplay_setwarn(ctx, "%s", "not configured with --enable-debug.  Debugging disabled.");
    }
#endif

    options->loop = OPT_VALUE_LOOP;
    options->sleep_accel = OPT_VALUE_SLEEP_ACCEL;

    if (HAVE_OPT(LIMIT))
        options->limit_send = OPT_VALUE_LIMIT;

    if (HAVE_OPT(TOPSPEED)) {
        options->speed.mode = speed_topspeed;
        options->speed.speed = 0.0;
    } else if (HAVE_OPT(PPS)) {
        options->speed.mode = speed_packetrate;
        options->speed.speed = (float)OPT_VALUE_PPS;
        options->speed.pps_multi = OPT_VALUE_PPS_MULTI;
    } else if (HAVE_OPT(ONEATATIME)) {
        options->speed.mode = speed_oneatatime;
        options->speed.speed = 0.0;
    } else if (HAVE_OPT(MBPS)) {
        options->speed.mode = speed_mbpsrate;
        options->speed.speed = atof(OPT_ARG(MBPS));
    } else if (HAVE_OPT(MULTIPLIER)) {
        options->speed.mode = speed_multiplier;
        options->speed.speed = atof(OPT_ARG(MULTIPLIER));
    }

#ifdef ENABLE_VERBOSE
    if (HAVE_OPT(VERBOSE))
        options->verbose = 1;

    if (HAVE_OPT(DECODE))
        options->tcpdump->args = safe_strdup(OPT_ARG(DECODE));
#endif

    /*
     * Check if the file cache should be enabled - if we're looping more than
     * once and the command line option has been spec'd
     */
    if (HAVE_OPT(ENABLE_FILE_CACHE) && (options->loop != 1)) {
        options->enable_file_cache = true;
    }

    if (HAVE_OPT(TIMER)) {
        if (strcmp(OPT_ARG(TIMER), "select") == 0) {
#ifdef HAVE_SELECT
            options->accurate = accurate_select;
#else
            tcpreplay_seterr(ctx, "%s", "tcpreplay_api not compiled with select support");
            return -1;
#endif
        } else if (strcmp(OPT_ARG(TIMER), "rdtsc") == 0) {
#ifdef HAVE_RDTSC
            options->accurate = accurate_rdtsc;
#else
            tcpreplay_seterr(ctx, "%s", "tcpreplay_api not compiled with rdtsc support");
            return -1;
#endif
        } else if (strcmp(OPT_ARG(TIMER), "ioport") == 0) {
#if defined HAVE_IOPERM && defined(__i386__)
            options->accurate = accurate_ioport;
            ioport_sleep_init();
#else
            tcpreplay_seterr(ctx, "%s", "tcpreplay_api not compiled with IO Port 0x80 support");
            return -1;
#endif
        } else if (strcmp(OPT_ARG(TIMER), "gtod") == 0) {
            options->accurate = accurate_gtod;
        } else if (strcmp(OPT_ARG(TIMER), "nano") == 0) {
            options->accurate = accurate_nanosleep;
        } else if (strcmp(OPT_ARG(TIMER), "abstime") == 0) {
#ifdef HAVE_ABSOLUTE_TIME
            options->accurate = accurate_abs_time;
            if  (!MPLibraryIsLoaded()) {
                tcpreplay_seterr(ctx, "%s", "The MP library did not load.\n");
                return -1;
            }
#else
            tcpreplay_seterr(ctx, "%s", "tcpreplay_api only supports absolute time on Apple OS X");
            return -1;
#endif
        } else {
            tcpreplay_seterr(ctx, "Unsupported timer mode: %s", OPT_ARG(TIMER));
            return -1;
        }
    }

#ifdef HAVE_RDTSC
    if (HAVE_OPT(RDTSC_CLICKS)) {
        rdtsc_calibrate(OPT_VALUE_RDTSC_CLICKS);
    }
#endif

    if (HAVE_OPT(PKTLEN)) {
        options->use_pkthdr_len = true;
        warn ++;
        tcpreplay_setwarn(ctx, "%s", "--pktlen may cause problems.  Use with caution.");
    }

    if ((intname = get_interface(ctx->intlist, OPT_ARG(INTF1))) == NULL) {
        tcpreplay_seterr(ctx, "Invalid interface name/alias: %s", OPT_ARG(INTF1));
        return -1;
    }

    options->intf1_name = safe_strdup(intname);

    /* open interfaces for writing */
    if ((ctx->intf1 = sendpacket_open(options->intf1_name, ebuf, TCPR_DIR_C2S)) == NULL) {
        tcpreplay_seterr(ctx, "Can't open %s: %s", options->intf1_name, ebuf);
        return -1;
    }

    int1dlt = sendpacket_get_dlt(ctx->intf1);

    if (HAVE_OPT(INTF2)) {
        if ((intname = get_interface(ctx->intlist, OPT_ARG(INTF2))) == NULL) {
            tcpreplay_seterr(ctx, "Invalid interface name/alias: %s", OPT_ARG(INTF2));
            return -1;
        }

        options->intf2_name = safe_strdup(intname);

        /* open interface for writing */
        if ((ctx->intf2 = sendpacket_open(options->intf2_name, ebuf, TCPR_DIR_S2C)) == NULL) {
            tcpreplay_seterr(ctx, "Can't open %s: %s", options->intf2_name, ebuf);
        }

        int2dlt = sendpacket_get_dlt(ctx->intf2);
        if (int2dlt != int1dlt) {
            tcpreplay_seterr(ctx, "DLT type missmatch for %s (%s) and %s (%s)", 
                options->intf1_name, pcap_datalink_val_to_name(int1dlt), 
                options->intf2_name, pcap_datalink_val_to_name(int2dlt));
            return -1;
        }
    }

    if (HAVE_OPT(CACHEFILE)) {
        temp = safe_strdup(OPT_ARG(CACHEFILE));
        options->cache_packets = read_cache(&options->cachedata, temp,
            &options->comment);
        safe_free(temp);
    }

    /* return -2 on warnings */
    if (warn > 0)
        return -2;

    return 0;
}
#endif /* USE_AUTOOPTS */

/**
 * Closes & free's all memory related to a tcpreplay context
 */
void
tcpreplay_close(tcpreplay_t *ctx)
{
    tcpreplay_opt_t *options;
    interface_list_t *intlist, *intlistnext;
    packet_cache_t *packet_cache, *next;

    assert(ctx);
    assert(ctx->options);
    options = ctx->options;

    safe_free(options->intf1_name);
    safe_free(options->intf2_name);
    sendpacket_close(ctx->intf1);
    if (ctx->intf2 != NULL)
        sendpacket_close(ctx->intf2);
    safe_free(options->cachedata);
    safe_free(options->comment);

#ifdef ENABLE_VERBOSE
    safe_free(options->tcpdump_args);
    tcpdump_close(options->tcpdump);
#endif

    /* free the file cache */
    if (options->file_cache != NULL) {
        packet_cache = options->file_cache->packet_cache;
        while (packet_cache != NULL) {
            next = packet_cache->next;
            safe_free(packet_cache->pktdata);
            safe_free(packet_cache);
            packet_cache = next;
        }
        safe_free(options->file_cache);
    }

    /* free our interface list */
    if (ctx->intlist != NULL) {
        intlist = ctx->intlist;
        while (intlist != NULL) {
            intlistnext = intlist->next;
            safe_free(intlist);
            intlist = intlistnext;
        }
    }
}

/**
 * \brief Specifies an interface to use for sending.
 *
 * You may call this up to two (2) times with different interfaces
 * when using a tcpprep cache file.  Note, both interfaces must use
 * the same DLT type
 */
int
tcpreplay_set_interface(tcpreplay_t *ctx, tcpreplay_intf intf, char *value)
{
    static int int1dlt = -1, int2dlt = -1;
    char *intname;
    char ebuf[SENDPACKET_ERRBUF_SIZE];

    assert(ctx);
    assert(value);

    if (intf == intf1) {
        if ((intname = get_interface(ctx->intlist, value)) == NULL) {
            tcpreplay_seterr(ctx, "Invalid interface name/alias: %s", value);
            return -1;
        }

        ctx->options->intf1_name = safe_strdup(intname);

        /* open interfaces for writing */
        if ((ctx->intf1 = sendpacket_open(ctx->options->intf1_name, ebuf, TCPR_DIR_C2S)) == NULL) {
            tcpreplay_seterr(ctx, "Can't open %s: %s", ctx->options->intf1_name, ebuf);
            return -1;
        }

        int1dlt = sendpacket_get_dlt(ctx->intf1);
    } else if (intf == intf2) {
        if ((intname = get_interface(ctx->intlist, value)) == NULL) {
            tcpreplay_seterr(ctx, "Invalid interface name/alias: %s", ctx->options->intf2_name);
            return -1;
        }

        ctx->options->intf2_name = safe_strdup(intname);

        /* open interface for writing */
        if ((ctx->intf2 = sendpacket_open(ctx->options->intf2_name, ebuf, TCPR_DIR_S2C)) == NULL) {
            tcpreplay_seterr(ctx, "Can't open %s: %s", ctx->options->intf2_name, ebuf);
            return -1;
        }
        int2dlt = sendpacket_get_dlt(ctx->intf2);
    }

    /*
     * If both interfaces are selected, then make sure both interfaces use
     * the same DLT type
     */
    if (int1dlt != -1 && int2dlt != -1) {
        if (int1dlt != int2dlt) {
            tcpreplay_seterr(ctx, "DLT type missmatch for %s (%s) and %s (%s)", 
                ctx->options->intf1_name, pcap_datalink_val_to_name(int1dlt), 
                ctx->options->intf2_name, pcap_datalink_val_to_name(int2dlt));
            return -1;
        }
    }

    return 0;
}

/**
 * Set the replay speed mode.
 */
int
tcpreplay_set_speed_mode(tcpreplay_t *ctx, tcpreplay_speed_mode value)
{
    assert(ctx);

    ctx->options->speed.mode = value;
    return 0;
}

/**
 * Set the approprate speed value.  Value is interpreted based on 
 * how tcpreplay_set_speed_mode() value
 */
int
tcpreplay_set_speed_speed(tcpreplay_t *ctx, float value)
{
    assert(ctx);
    ctx->options->speed.speed = value;
    return 0;
}


/**
 * Sending under packets/sec requires an integer value, not float.
 * you must first call tcpreplay_set_speed_mode(ctx, speed_packetrate)
 */
int
tcpreplay_set_speed_pps_multi(tcpreplay_t *ctx, int value)
{
    assert(ctx);
    ctx->options->speed.pps_multi = value;
    return 0;
}

/**
 * How many times should we loop through all the pcap files?
 */
int
tcpreplay_set_loop(tcpreplay_t *ctx, u_int32_t value)
{
    assert(ctx);
    ctx->options->loop = value;
    return 0;
}

/**
 * Set the sleep accellerator fudge factor
 */
int
tcpreplay_set_sleep_accel(tcpreplay_t *ctx, int value)
{
    assert(ctx);
    ctx->options->sleep_accel = value;
    return 0;
}

/**
 * Tell tcpreplay to ignore the snaplen (default) and use the "actual"
 * packet len instead
 */
int
tcpreplay_set_use_pkthdr_len(tcpreplay_t *ctx, bool value)
{
    assert(ctx);
    ctx->options->use_pkthdr_len = value;
    return 0;
}

/**
 * Override the outbound MTU
 */
int
tcpreplay_set_mtu(tcpreplay_t *ctx, int value)
{
    assert(ctx);
    ctx->options->mtu = value;
    return 0;
}

/**
 * Sets the accurate timing mode
 */
int
tcpreplay_set_accurate(tcpreplay_t *ctx, tcpreplay_accurate value)
{
    assert(ctx);
    ctx->options->accurate = value;
    return 0;
}

/**
 * \brief Enable or disable file caching
 *
 * Note: This is a global option and turns on/off file caching
 * for ALL files in this context
 */
int
tcpreplay_set_file_cache(tcpreplay_t *ctx, bool value)
{
    assert(ctx);
    ctx->options->enable_file_cache = value;
    return 0;
}

/**
 * \brief Add a pcap file to be sent via tcpreplay
 *
 * One or more pcap files can be added.  Each file will be replayed
 * in order
 */
int
tcpreplay_add_pcapfile(tcpreplay_t *ctx, char *pcap_file)
{
    assert(ctx);
    assert(pcap_file);

    if (ctx->options->source_cnt < MAX_FILES) {
        ctx->options->sources[ctx->options->source_cnt].filename = safe_strdup(pcap_file);

        /*
         * prepare the cache info data struct.  This doesn't actually enable
         * file caching for this pcap (that is controlled globally via
         * tcpreplay_set_file_cache())
         */
        ctx->options->file_cache[ctx->options->source_cnt].index = ctx->options->source_cnt;
        ctx->options->file_cache[ctx->options->source_cnt].cached = false;
        ctx->options->file_cache[ctx->options->source_cnt].packet_cache = NULL;

        ctx->options->source_cnt += 1;


    } else {
        tcpreplay_seterr(ctx, "Unable to add more then %u files", MAX_FILES);
        return -1;
    }
    return 0;
}

/**
 * Limit the total number of packets to send
 */
int
tcpreplay_set_limit_send(tcpreplay_t *ctx, COUNTER value)
{
    assert(ctx);
    ctx->options->limit_send = value;
    return 0;
}

/**
 * \brief Specify the tcpprep cache file to use for replaying with two NICs
 *
 * Note: this only works if you have a single pcap file
 * returns -1 on error
 */
int
tcpreplay_set_tcpprep_cache(tcpreplay_t *ctx, char *file)
{
    assert(ctx);
    char *tcpprep_file;

    if (ctx->options->source_cnt > 1) {
        tcpreplay_seterr(ctx, "%s", "Unable to use tcpprep cache file with a single pcap file");
        return -1;
    }

    tcpprep_file = safe_strdup(file);
    ctx->options->cache_packets = read_cache(&ctx->options->cachedata, 
        tcpprep_file, &ctx->options->comment);

    free(tcpprep_file);

    return 0;
}



/*
 * Verbose mode requires fork() and tcpdump binary, hence won't work
 * under Win32 without Cygwin
 */
#ifdef ENABLE_VERBOSE

/**
 * Enable verbose mode
 */
int
tcpreplay_set_verbose(tcpreplay_t *ctx, bool value)
{
    assert(ctx);
    ctx->options->verbose = value;
    return 0;
}

/**
 * \brief Set the arguments to be passed to tcpdump
 *
 * Specify the additional argument to be passed to tcpdump when enabling
 * verbose mode.  See TCPDUMP_ARGS in tcpdump.h for the default options
 */
int
tcpreplay_set_tcpdump_args(tcpreplay_t *ctx, char *value)
{
    assert(ctx);
    assert(value);
    ctx->options->tcpdump_args = safe_strdup(value);
    return 0;
}

/**
 * \brief Set the path to the tcpdump binary
 *
 * In order to support the verbose feature, tcpreplay needs to know where
 * tcpdump lives
 */
int
tcpreplay_set_tcpdump(tcpreplay_t *ctx, tcpdump_t *value)
{
    assert(ctx);
    assert(value);
    ctx->options->verbose = true;
    ctx->options->tcpdump = value;
    return 0;
}

#endif /* ENABLE_VERBOSE */


/**
 * \brief Set the callback function for handing manual iteration
 *
 * Obviously for this to work, you need to first set speed_mode = speed_oneatatime
 * returns 0 on success, < 0 on error
 */
int
tcpreplay_set_manual_callback(tcpreplay_t *ctx, tcpreplay_manual_callback callback)
{
    assert(ctx);
    assert(callback);

    if (ctx->options->speed.mode != speed_oneatatime) {
        tcpreplay_seterr(ctx, "%s", 
                "Unable to set manual callback because speed mode is not 'speed_oneatatime'");
        return -1;
    }

    ctx->options->speed.manual_callback = callback;
    return 0;
}

/**
 * \brief return the number of packets sent so far
 */
COUNTER
tcpreplay_get_pkts_sent(tcpreplay_t *ctx)
{
    assert(ctx);

    ctx->static_stats.pkts_sent = ctx->stats.pkts_sent;
    return ctx->static_stats.pkts_sent;
}

/**
 * \brief return the number of bytes sent so far
 */
COUNTER
tcpreplay_get_bytes_sent(tcpreplay_t *ctx)
{
    assert(ctx);
    ctx->static_stats.bytes_sent = ctx->stats.bytes_sent;
    return ctx->static_stats.bytes_sent;
}

/**
 * \brief return the number of failed attempts to send a packet
 */
COUNTER
tcpreplay_get_failed(tcpreplay_t *ctx)
{
    assert(ctx);
    ctx->static_stats.failed = ctx->stats.failed;
    return ctx->static_stats.failed;
}

/**
 * \brief returns a pointer to the timeval structure of when replay first started
 */
const struct timeval *
tcpreplay_get_start_time(tcpreplay_t *ctx)
{
    assert(ctx);
    memcpy(&ctx->static_stats.start_time, &ctx->stats.end_time, sizeof(ctx->stats.end_time));
    return &ctx->static_stats.start_time;
}

/**
 * \brief returns a pointer to the timeval structure of when replay finished
 */
const struct timeval *
tcpreplay_get_end_time(tcpreplay_t *ctx)
{
    assert(ctx);
    memcpy(&ctx->static_stats.end_time, &ctx->stats.end_time, sizeof(ctx->stats.end_time));
    return &ctx->static_stats.end_time;
}


/**
 * \brief Internal function to set the tcpreplay error string
 *
 * Used to set the error string when there is an error, result is retrieved
 * using tcpedit_geterr().  You shouldn't ever actually call this, but use
 * tcpreplay_seterr() which is a macro wrapping this instead.
 */
void
__tcpreplay_seterr(tcpreplay_t *ctx, const char *func, const int line, 
    const char *file, const char *fmt, ...)
{
    va_list ap;
    char errormsg[TCPREPLAY_ERRSTR_LEN];

    assert(ctx);

    va_start(ap, fmt);
    if (fmt != NULL) {
        (void)vsnprintf(errormsg,
              (TCPREPLAY_ERRSTR_LEN - 1), fmt, ap);
    }

    va_end(ap);

    snprintf(ctx->errstr, (TCPREPLAY_ERRSTR_LEN -1), "From %s:%s() line %d:\n%s",
        file, func, line, errormsg);
}

/**
 * \brief Internal function to set the tcpedit warning string
 *
 * Used to set the warning string when there is an non-fatal issue, result is retrieved
 * using tcpedit_getwarn().
 */
void
tcpreplay_setwarn(tcpreplay_t *ctx, const char *fmt, ...)
{
    va_list ap;
    assert(ctx);

    va_start(ap, fmt);
    if (fmt != NULL)
        (void)vsnprintf(ctx->warnstr, (TCPREPLAY_ERRSTR_LEN - 1), fmt, ap);

    va_end(ap);
}


/**
 * \brief sends the traffic out the interfaces
 *
 * Designed to be called in a separate thread if you need to.  Blocks until
 * the replay is complete or you call tcpreplay_abort() in another thread.
 * Pass the index of the pcap you want to replay, or -1 for all pcaps
 */
int
tcpreplay_replay(tcpreplay_t *ctx, int idx)
{
    int i, rcode;

    assert(ctx);

    if (idx < 0 || idx > ctx->options->source_cnt) {
        tcpreplay_seterr(ctx, "invalid source index value: %d", idx);
        return -1;
    }

    /*
     * Setup up the file cache, if required
     */
    if (ctx->options->enable_file_cache && ctx->options->file_cache == NULL) {
        /* Initialise each of the file cache structures */
        for (i = 0; i < ctx->options->source_cnt; i++) {
            ctx->options->file_cache[i].index = i;
            ctx->options->file_cache[i].cached = FALSE;
            ctx->options->file_cache[i].packet_cache = NULL;
        }
    }

    if (gettimeofday(&ctx->stats.start_time, NULL) < 0) {
        tcpreplay_seterr(ctx, "gettimeofday() failed: %s",  strerror(errno));
        return -1;
    }

    ctx->running = true;

    /* main loop, when not looping forever */
    if (ctx->options->loop > 0) {
        while (ctx->options->loop--) {  /* limited loop */
            /* process each pcap file in order */
            for (ctx->current_source = 0;
                    ctx->current_source < ctx->options->source_cnt;
                    ctx->current_source++) {
                /* reset cache markers for each iteration */
                ctx->cache_byte = 0;
                ctx->cache_bit = 0;
                switch(ctx->options->sources[ctx->current_source].type) {
                    case source_filename:
                        rcode = replay_file(ctx, ctx->current_source);
                        break;
                    case source_fd:
                        rcode = replay_fd(ctx, ctx->current_source);
                        break;
                    case source_cache:
                        rcode = replay_cache(ctx, ctx->current_source);
                        break;
                    default:
                        tcpreplay_seterr(ctx, "Invalid source type: %d", ctx->options->sources[ctx->current_source].type);
                        rcode = -1;
                }
                if (rcode < 0) {
                    ctx->running = false;
                    return -1;
                }

            }
        }
    }
    else {
        /* loop forever */
        while (1) {
            for (ctx->current_source = 0;
                    ctx->current_source < ctx->options->source_cnt;
                    ctx->current_source++) {
                /* reset cache markers for each iteration */
                ctx->cache_byte = 0;
                ctx->cache_bit = 0;
                switch(ctx->options->sources[ctx->current_source].type) {
                    case source_filename:
                        rcode = replay_file(ctx, ctx->current_source);
                        break;
                    case source_fd:
                        rcode = replay_fd(ctx, ctx->current_source);
                        break;
                    case source_cache:
                        rcode = replay_cache(ctx, ctx->current_source);
                        break;
                    default:
                        tcpreplay_seterr(ctx, "Invalid source type: %d", ctx->options->sources[ctx->current_source].type);
                        rcode = -1;
                }
                if (rcode < 0) {
                    ctx->running = false;
                    return -1;
                }
            }
        }
    }

    ctx->running = false;
    return 0;
}

/**
 * \brief Abort the tcpreplay_replay execution.
 *
 * This might take a little while since tcpreplay_replay() only checks this
 * once per packet (sleeping between packets can cause delays), however, 
 * this function returns once the signal has been sent and does not block
 */
int
tcpreplay_abort(tcpreplay_t *ctx)
{
    assert(ctx);
    ctx->abort = true;

    if (ctx->intf1 != NULL)
        sendpacket_abort(ctx->intf1);

    if (ctx->intf2 != NULL)
        sendpacket_abort(ctx->intf2);

    return 0;
}

/**
 * \brief Temporarily suspend tcpreplay_replay()
 *
 * This might take a little while since tcpreplay_replay() only checks this
 * once per packet (sleeping between packets can cause delays), however, 
 * this function returns once the signal has been sent and does not block 
 *
 * Note that suspending a running context can create odd timing 
 */
int
tcpreplay_suspend(tcpreplay_t *ctx)
{
    assert(ctx);
    ctx->suspend = true;
    return 0;
}

/**
 * \brief Restart tcpreplay_replay() after suspend
 *
 * Causes the worker thread to restart sending packets
 */
int
tcpreplay_restart(tcpreplay_t *ctx)
{
    assert(ctx);
    ctx->suspend = false;
    return 0;
}

/**
 * \brief Tells you if the given tcpreplay context is currently suspended
 *
 * Suspended == running, but not sending packets
 */
bool
tcpreplay_is_suspended(tcpreplay_t *ctx)
{
    assert(ctx);
    return ctx->suspend;
}

/**
 * \brief Tells you if the tcpreplay context is running (not yet finished)
 *
 * Returns true even if it is suspended
 */
bool 
tcpreplay_is_running(tcpreplay_t *ctx)
{
    assert(ctx);
    return ctx->running;
}

/**
 * \brief returns the current statistics during or after a replay
 *
 * For performance reasons, I don't bother to put a mutex around this and you
 * don't need to either.  Just realize that your values may be off by one until
 * tcreplay_replay() returns.
 */
const tcpreplay_stats_t *
tcpreplay_get_stats(tcpreplay_t *ctx)
{
    const tcpreplay_stats_t *ptr;

    assert(ctx);

    /* copy stats over so they don't change while caller is using the buffer */
    memcpy(&ctx->static_stats, &ctx->stats, sizeof(tcpreplay_stats_t));
    ptr = &ctx->static_stats;
    return ptr;
}


/**
 * \brief returns the current number of sources/files to be sent
 */
int
tcpreplay_get_source_count(tcpreplay_t *ctx)
{
    assert(ctx);
    return ctx->options->source_cnt;
}

/**
 * \brief Returns the current source id being replayed
 */
int
tcpreplay_get_current_source(tcpreplay_t *ctx)
{
    assert(ctx);
    return ctx->current_source;
}

/* vim: set tabstop=8 expandtab shiftwidth=4 softtabstop=4: */

