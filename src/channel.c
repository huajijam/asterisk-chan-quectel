/*
   Copyright (C) 2009 - 2010

   Artem Makhutov <artem@makhutov.org>
   http://www.makhutov.org

   Dmitry Vagin <dmitry2004@yandex.ru>

   bg <bg_one@mail.ru>
   Alsa component
   Copyright (C) , Digium, Inc.

   By Matthew Fredrickson <creslin@digium.com>
*/

#include "ast_compat.h"
#include "ast_config.h"

#include <asterisk/causes.h>       /* AST_CAUSE_INCOMPATIBLE_DESTINATION AST_CAUSE_FACILITY_NOT_IMPLEMENTED AST_CAUSE_REQUESTED_CHAN_UNAVAIL */
#include <asterisk/lock.h>         /* AST_MUTEX_DEFINE_STATIC */
#include <asterisk/module.h>       /* ast_module_ref() ast_module_info = shit */
#include <asterisk/musiconhold.h>  /* ast_moh_start() ast_moh_stop() */
#include <asterisk/pbx.h>          /* pbx_builtin_setvar_helper() */
#include <asterisk/timing.h>       /* ast_timer_fd() ast_timer_set_rate() ast_timer_ack() */
#if ASTERISK_VERSION_NUM >= 130000 /* 13+ */
#include <asterisk/format_cache.h>
#include <asterisk/stasis_channels.h>
#endif /* ^13+ */

#include "at_command.h"
#include "at_queue.h" /* write_all() TODO: move out */
#include "chan_quectel.h"
#include "channel.h"
#include "helpers.h" /* get_at_clir_value()  */

#ifndef ESTRPIPE
#define ESTRPIPE EPIPE
#endif

static int setvar_helper(const struct pvt* const pvt, struct ast_channel* chan, const char* name, const char* value)
{
    ast_debug(1, "[%s] Setting chanvar %s = %s\n", PVT_ID(pvt), S_OR(name, "(null)"), S_OR(value, "(null)"));
    return pbx_builtin_setvar_helper(chan, name, value);
}

#/* */

static int parse_dial_string(char* dialstr, const char** number, int* opts)
{
    char* options;
    char* dest_num;
    int lopts = 0;

    options = strchr(dialstr, '/');
    if (!options) {
        ast_log(LOG_WARNING, "Can't determine destination in chan_quectel\n");
        return AST_CAUSE_INCOMPATIBLE_DESTINATION;
    }
    *options++ = '\0';

    dest_num = strchr(options, ':');
    if (!dest_num) {
        dest_num = options;
    } else {
        *dest_num++ = '\0';

        if (!strcasecmp(options, "holdother")) {
            lopts = CALL_FLAG_HOLD_OTHER;
        } else if (!strcasecmp(options, "conference")) {
            lopts = CALL_FLAG_HOLD_OTHER | CALL_FLAG_CONFERENCE;
        } else {
            ast_log(LOG_WARNING, "Invalid options in chan_quectel\n");
            return AST_CAUSE_INCOMPATIBLE_DESTINATION;
        }
    }

    if (*dest_num == '\0') {
        ast_log(LOG_WARNING, "Empty destination in chan_quectel\n");
        return AST_CAUSE_INCOMPATIBLE_DESTINATION;
    }
    if (!is_valid_phone_number(dest_num)) {
        ast_log(LOG_WARNING, "Invalid destination '%s' in chan_quectel, only 0123456789*#+ABC allowed\n", dest_num);
        return AST_CAUSE_INCOMPATIBLE_DESTINATION;
    }

    *number = dest_num;
    *opts   = lopts;
    return 0;
}

#/* */

int channels_loop(struct pvt* pvt, const struct ast_channel* requestor)
{
    /* not allow hold requester channel :) */
    /* FIXME: requestor may be just proxy/masquerade for real channel */
    //	use ast_bridged_channel(chan) ?
    //	use requestor->tech->get_base_channel() ?
    struct cpvt* tmp;
    return (requestor && ast_channel_tech(requestor) == &channel_tech && (tmp = ast_channel_tech_pvt(requestor)) && tmp->pvt == pvt) ? 1 : 0;
}

#if ASTERISK_VERSION_NUM >= 120000 /* 12+ */
static struct ast_channel* channel_request(attribute_unused const char* type, struct ast_format_cap* cap, const struct ast_assigned_ids* assignedids,
                                           const struct ast_channel* requestor, const char* data, int* cause)
#elif ASTERISK_VERSION_NUM >= 110000 /* 11+ */
static struct ast_channel* channel_request(attribute_unused const char* type, struct ast_format_cap* cap, const struct ast_channel* requestor, const char* data,
                                           int* cause)
#elif ASTERISK_VERSION_NUM >= 100000 /* 10+ */
static struct ast_channel* channel_request(attribute_unused const char* type, struct ast_format_cap* cap, const struct ast_channel* requestor, void* data,
                                           int* cause)
#elif ASTERISK_VERSION_NUM >= 10800  /* 1.8+ */
static struct ast_channel* channel_request(attribute_unused const char* type, format_t format, const struct ast_channel* requestor, void* data, int* cause)
#else                                /* 1.8- */
static struct ast_channel* channel_request(attribute_unused const char* type, int format, void* data, int* cause)
#endif                               /* ^1.8- */
{
/* TODO: simplify by moving common code to functions */
/* TODO: add check when request 'holdother' what requestor is not on same device for 1.6 */
#if ASTERISK_VERSION_NUM >= 10800 && ASTERISK_VERSION_NUM < 100000 /* 1.8+ .. 10- */
    format_t oldformat;
#elif ASTERISK_VERSION_NUM < 10800 /* 1.8- */
    int oldformat;
    const struct ast_channel* requestor = NULL;
#endif                             /* ^1.8- */
    char* dest_dev;
    const char* dest_num;
    struct ast_channel* channel = NULL;
    struct pvt* pvt;
    int opts = CALL_FLAG_NONE;
    int exists;

    if (!data) {
        ast_log(LOG_WARNING, "Channel requested with no data\n");
        *cause = AST_CAUSE_INCOMPATIBLE_DESTINATION;
        return NULL;
    }

    dest_dev = ast_strdupa(data);

    *cause = parse_dial_string(dest_dev, &dest_num, &opts);
    if (*cause) {
        return NULL;
    }

#if ASTERISK_VERSION_NUM >= 10800
    pvt = find_device_by_resource(dest_dev, opts, requestor, &exists);
#else  /* 1.8- */
    pvt                                 = find_device_by_resource(dest_dev, opts, NULL, &exists);
#endif /* ^1.8- */

    unsigned local_channel = 0;

#if ASTERISK_VERSION_NUM >= 130000 /* 13+ */
    if (pvt) {
        local_channel = (unsigned)ast_format_cap_has_type(cap, AST_MEDIA_TYPE_TEXT);
        if (!local_channel) {
            const struct ast_format* const fmt = pvt_get_audio_format(pvt);
            const enum ast_format_cmp_res res  = ast_format_cap_iscompatible_format(cap, fmt);
            if (res != AST_FORMAT_CMP_EQUAL && res != AST_FORMAT_CMP_SUBSET) {
                struct ast_str* codec_buf = ast_str_alloca(64);
                ast_log(LOG_WARNING, "Asked to get a channel of unsupported format '%s'\n", ast_format_cap_get_names(cap, &codec_buf));
#ifdef WRONG_CODEC_FAILURE
                *cause = AST_CAUSE_FACILITY_NOT_IMPLEMENTED;
                ast_mutex_unlock(&pvt->lock);
                return NULL;
#endif
            }
        }
    } else {
        if (ast_format_cap_iscompatible_format(cap, ast_format_slin) != AST_FORMAT_CMP_EQUAL &&
            ast_format_cap_iscompatible_format(cap, ast_format_slin16) != AST_FORMAT_CMP_EQUAL &&
            ast_format_cap_iscompatible_format(cap, ast_format_slin48) != AST_FORMAT_CMP_EQUAL) {
            struct ast_str* codec_buf = ast_str_alloca(64);
            ast_log(LOG_WARNING, "Asked to get a channel of unsupported format '%s'\n", ast_format_cap_get_names(cap, &codec_buf));
#ifdef WRONG_CODEC_FAILURE
            *cause = AST_CAUSE_FACILITY_NOT_IMPLEMENTED;
            return NULL;
#endif
        }
    }
#elif ASTERISK_VERSION_NUM >= 100000 /* 10-13 */
    if (!ast_format_cap_iscompatible(cap, &chan_quectel_format)) {
        char buf[255];
        ast_log(LOG_WARNING, "Asked to get a channel of unsupported format '%s'\n", ast_getformatname_multiple(buf, 255, cap));
        *cause = AST_CAUSE_FACILITY_NOT_IMPLEMENTED;
        if (pvt) {
            ast_mutex_unlock(&pvt->lock);
        }
        return NULL;
    }
#else                                /* 10- */
    oldformat  = format;
    format    &= AST_FORMAT_SLINEAR;

    if (!format) {
#if ASTERISK_VERSION_NUM >= 10800    /* 1.8+ */
        ast_log(LOG_WARNING, "Asked to get a channel of unsupported format '%s'\n", ast_getformatname(oldformat));
#else                                /* 1.8- */
        ast_log(LOG_WARNING, "Asked to get a channel of unsupported format '%d'\n", oldformat);
#endif                               /* ^1.8- */
        *cause = AST_CAUSE_FACILITY_NOT_IMPLEMENTED;
        if (pvt) {
            ast_mutex_unlock(&pvt->lock);
        }
        return NULL;
    }
#endif                               /* ^10- */

    if (pvt) {
#if ASTERISK_VERSION_NUM >= 120000 /* 12+ */
        channel = new_channel(pvt, AST_STATE_DOWN, NULL, pvt_get_pseudo_call_idx(pvt), CALL_DIR_OUTGOING, CALL_STATE_INIT, NULL, assignedids, requestor,
                              local_channel);
#else  /* 12- */
        channel = new_channel(pvt, AST_STATE_DOWN, NULL, pvt_get_pseudo_call_idx(pvt), CALL_DIR_OUTGOING, CALL_STATE_INIT, NULL, requestor, local_channel);
#endif /* ^12- */
        ast_mutex_unlock(&pvt->lock);
        if (!channel) {
            ast_log(LOG_WARNING, "Unable to allocate channel structure\n");
            *cause = AST_CAUSE_REQUESTED_CHAN_UNAVAIL;
        }
    } else {
        ast_log(LOG_WARNING, "[%s] Request to call on device %s\n", dest_dev, exists ? "which can not make call at this moment" : "not exists");
        *cause = AST_CAUSE_REQUESTED_CHAN_UNAVAIL;
    }

    return channel;
}

#                                  /* */
#if ASTERISK_VERSION_NUM >= 110000 /* 11+ */
static int channel_call(struct ast_channel* channel, const char* dest, attribute_unused int timeout)
#else  /* 11- */
static int channel_call(struct ast_channel* channel, char* dest, attribute_unused int timeout)
#endif /* ^11- */
{
    struct cpvt* const cpvt = ast_channel_tech_pvt(channel);
    char* dest_dev;
    const char* dest_num;
    int clir = 0;
    int opts;

    if (!cpvt || cpvt->channel != channel || !cpvt->pvt) {
        ast_log(LOG_WARNING, "call on unreferenced %s\n", ast_channel_name(channel));
        return -1;
    }
    struct pvt* const pvt = cpvt->pvt;

    dest_dev = ast_strdupa(dest);

    if (parse_dial_string(dest_dev, &dest_num, &opts)) {
        return -1;
    }

    if ((ast_channel_state(channel) != AST_STATE_DOWN) && (ast_channel_state(channel) != AST_STATE_RESERVED)) {
        ast_log(LOG_WARNING, "channel_call called on %s, neither down nor reserved\n", ast_channel_name(channel));
        return -1;
    }

    ast_mutex_lock(&pvt->lock);

    // FIXME: check if bridged on same device with CALL_FLAG_HOLD_OTHER
    if (!ready4voice_call(pvt, cpvt, opts)) {
        ast_mutex_unlock(&pvt->lock);
        ast_log(LOG_ERROR, "[%s] Error device already in use or uninitialized\n", PVT_ID(pvt));
        return -1;
    }
    CPVT_SET_FLAGS(cpvt, opts);

    ast_debug(1, "[%s] Calling %s on %s\n", PVT_ID(pvt), dest, ast_channel_name(channel));

    if (CONF_SHARED(pvt, usecallingpres)) {
        if (CONF_SHARED(pvt, callingpres) < 0) {
#if ASTERISK_VERSION_NUM >= 10800 /* 1.8+ */
            clir = ast_channel_connected(channel)->id.number.presentation;
#else  /* 1.8- */
            clir = channel->cid.cid_pres;
#endif /* ^1.8- */
        } else {
            clir = CONF_SHARED(pvt, callingpres);
        }

        clir = get_at_clir_value(pvt, clir);
    } else {
        clir = -1;
    }

    PVT_STAT(pvt, out_calls)++;
    if (at_enqueue_dial(cpvt, dest_num, clir)) {
        ast_mutex_unlock(&pvt->lock);
        ast_log(LOG_ERROR, "[%s] Error sending ATD command\n", PVT_ID(pvt));
        return -1;
    }

    ast_mutex_unlock(&pvt->lock);

    return 0;
}

#/* ARCH: move to cpvt level */

static void disactivate_call(struct cpvt* cpvt)
{
    struct pvt* pvt = cpvt->pvt;

    if (cpvt->channel && CPVT_TEST_FLAG(cpvt, CALL_FLAG_ACTIVATED)) {
        if (CONF_UNIQ(pvt, uac) > TRIBOOL_FALSE) {
            // snd_pcm_drop(pvt->icard);
            // snd_pcm_drop(pvt->ocard);
        } else {
            if (CONF_SHARED(pvt, multiparty)) {
                mixb_detach(&cpvt->pvt->a_write_mixb, &cpvt->mixstream);
            }
        }

        CPVT_RESET_FLAGS(cpvt, CALL_FLAG_ACTIVATED | CALL_FLAG_MASTER);

        ast_debug(6, "[%s] Call idx %d disactivated\n", PVT_ID(cpvt->pvt), cpvt->call_idx);
    }
}

#/* ARCH: move to cpvt level */

static void activate_call(struct cpvt* cpvt)
{
    struct cpvt* cpvt2;
    struct pvt* pvt;

    /* nothing todo, already main */
    if (CPVT_TEST_FLAG(cpvt, CALL_FLAG_MASTER)) {
        return;
    }

    /* drop any other from MASTER, any set pipe for actives */
    pvt = cpvt->pvt;
    AST_LIST_TRAVERSE(&pvt->chans, cpvt2, entry) {
        if (cpvt2 == cpvt) {
            continue;
        }

        if (CPVT_TEST_FLAG(cpvt, CALL_FLAG_MASTER)) {
            ast_debug(6, "[%s] Call idx:%d gave master\n", PVT_ID(pvt), cpvt2->call_idx);
        }

        CPVT_RESET_FLAGS(cpvt2, CALL_FLAG_MASTER);

        if (cpvt2->channel) {
            ast_channel_set_fd(cpvt2->channel, 1, -1);
            if (CPVT_TEST_FLAG(cpvt, CALL_FLAG_ACTIVATED)) {
                ast_channel_set_fd(cpvt2->channel, 0, cpvt2->rd_pipe[PIPE_READ]);
                ast_debug(6, "[%s] Call idx:%d FD:%d still active\n", PVT_ID(pvt), cpvt2->call_idx, cpvt2->rd_pipe[PIPE_READ]);
            }
        }
    }

    /* setup call local write possition */
    if (!CPVT_TEST_FLAG(cpvt, CALL_FLAG_ACTIVATED)) {
        // FIXME: reset possition?
        if (CONF_SHARED(pvt, multiparty)) {
            mixb_attach(&pvt->a_write_mixb, &cpvt->mixstream);
        }
    }

    if (pvt->audio_fd >= 0) {
        CPVT_SET_FLAGS(cpvt, CALL_FLAG_ACTIVATED | CALL_FLAG_MASTER);
        ast_debug(6, "[%s] Call idx:%d was master\n", PVT_ID(pvt), cpvt->call_idx);
    }
}

#/* we has 2 case of call this function, when local side want terminate call and when called for cleanup after remote side alreay terminate call, CEND received and cpvt destroyed */

static int channel_hangup(struct ast_channel* channel)
{
    struct cpvt* const cpvt = ast_channel_tech_pvt(channel);

    /* its possible call with channel w/o tech_pvt */
    if (cpvt && cpvt->channel == channel && cpvt->pvt) {
        struct pvt* const pvt = cpvt->pvt;

        ast_mutex_lock(&pvt->lock);

        const int need_hangup  = CPVT_TEST_FLAG(cpvt, CALL_FLAG_NEED_HANGUP) ? 1 : 0;
        const int hangup_cause = ast_channel_hangupcause(channel);

        ast_debug(1, "[%s] Hanging up call - idx:%d cause:%d needed:%d\n", PVT_ID(pvt), cpvt->call_idx, hangup_cause, need_hangup);

        if (need_hangup) {
            if (at_enqueue_hangup(cpvt, cpvt->call_idx, hangup_cause)) {
                ast_log(LOG_ERROR, "[%s] Error adding AT+CHUP command to queue, call not terminated!\n", PVT_ID(pvt));
            } else {
                CPVT_RESET_FLAGS(cpvt, CALL_FLAG_NEED_HANGUP);
            }
        }

        disactivate_call(cpvt);

        /* drop cpvt->channel reference */
        cpvt->channel = NULL;
        ast_mutex_unlock(&pvt->lock);
    }

    /* drop channel -> cpvt reference */
    ast_channel_tech_pvt_set(channel, NULL);

    ast_module_unref(self_module());
    ast_setstate(channel, AST_STATE_DOWN);

    return 0;
}

#/* */

static int channel_answer(struct ast_channel* channel)
{
    struct cpvt* const cpvt = ast_channel_tech_pvt(channel);

    if (!cpvt || cpvt->channel != channel || !cpvt->pvt) {
        ast_log(LOG_WARNING, "call on unreferenced %s\n", ast_channel_name(channel));
        return 0;
    }
    struct pvt* const pvt = cpvt->pvt;

    ast_mutex_lock(&pvt->lock);

    if (cpvt->dir == CALL_DIR_INCOMING) {
        if (at_enqueue_answer(cpvt)) {
            ast_log(LOG_ERROR, "[%s] Error sending answer commands\n", PVT_ID(pvt));
        }
    }

    ast_mutex_unlock(&pvt->lock);

    return 0;
}

#/* */

static int channel_digit_begin(struct ast_channel* channel, char digit)
{
    struct cpvt* const cpvt = ast_channel_tech_pvt(channel);

    if (!cpvt || cpvt->channel != channel || !cpvt->pvt) {
        ast_log(LOG_WARNING, "call on unreferenced %s\n", ast_channel_name(channel));
        return -1;
    }
    struct pvt* const pvt = cpvt->pvt;

    ast_mutex_lock(&pvt->lock);

    const int rv = at_enqueue_dtmf(cpvt, digit);
    if (rv) {
        if (rv == -1974) {
            ast_log(LOG_WARNING, "[%s] Sending DTMF %c not supported. Tell Asterisk to generate inband\n", PVT_ID(pvt), digit);
        } else {
            ast_log(LOG_ERROR, "[%s] Error adding DTMF %c command to queue\n", PVT_ID(pvt), digit);
        }
        ast_mutex_unlock(&pvt->lock);
        return -1;
    }

    ast_verb(1, "[%s] Send DTMF: %c\n", PVT_ID(pvt), digit);
    ast_mutex_unlock(&pvt->lock);
    return 0;
}

#/* */

static int channel_digit_end(attribute_unused struct ast_channel* channel, attribute_unused char digit, attribute_unused unsigned int duration) { return 0; }

static ssize_t get_iov_total_len(const struct iovec* const iov, int iovcnt)
{
    ssize_t len = 0;
    for (int i = 0; i < iovcnt; ++i) {
        len += iov[i].iov_len;
    }
    return len;
}

#/* ARCH: move to cpvt level */

static ssize_t iov_write(struct pvt* pvt, int fd, const struct iovec* const iov, int iovcnt)
{
    const ssize_t len = get_iov_total_len(iov, iovcnt);
    const ssize_t w   = writev(fd, iov, iovcnt);

    if (w < 0) {
        const int err = errno;
        if (err == EINTR || err == EAGAIN) {
            ast_debug(3, "[%s][TTY] Write error: %s\n", PVT_ID(pvt), strerror(err));
        } else {
            ast_log(LOG_WARNING, "[%s][TTY] Write error: %s\n", PVT_ID(pvt), strerror(err));
        }
        return -err;
    } else if (w && w != len) {
        ast_log(LOG_WARNING, "[%s][TTY] Incomplete frame written: %ld/%ld\n", PVT_ID(pvt), (long)w, (long)len);
    }

    return w;
}

static inline void change_audio_endianness_to_le(attribute_unused struct iovec* iov, attribute_unused int iovcnt)
{
#if __BYTE_ORDER == __BIG_ENDIAN
    for (; iovcnt-- > 0; ++iov) {
        ast_swapcopy_samples(iov->iov_base, iov->iov_base, iov->iov_len / 2);
    }
#endif
}

#/* */

static void timing_write_tty(struct pvt* pvt, size_t frame_size)
{
    size_t used;
    int iovcnt;
    struct iovec iov[3];
    const char* msg = NULL;
    //	char			buffer[FRAME_SIZE];
    //	struct cpvt*		cpvt;

    //	ast_debug (6, "[%s] tm write |\n", PVT_ID(pvt));

    //	memset(buffer, 0, sizeof(buffer));

    //	AST_LIST_TRAVERSE(&pvt->chans, cpvt, entry) {

    //		if(!CPVT_IS_ACTIVE(cpvt))
    //			continue;

    used = mixb_used(&pvt->a_write_mixb);
    //		used = rb_used (&cpvt->a_write_rb);

    if (used >= frame_size) {
        iovcnt = mixb_read_n_iov(&pvt->a_write_mixb, iov, frame_size);
        mixb_read_n_iov(&pvt->a_write_mixb, iov, frame_size);
        mixb_read_upd(&pvt->a_write_mixb, frame_size);
        change_audio_endianness_to_le(iov, iovcnt);
    } else if (used > 0) {
        PVT_STAT(pvt, write_tframes)++;
        msg = "[%s] write truncated frame\n";

        iovcnt = mixb_read_all_iov(&pvt->a_write_mixb, iov);
        mixb_read_all_iov(&pvt->a_write_mixb, iov);
        mixb_read_upd(&pvt->a_write_mixb, used);

        iov[iovcnt].iov_base = pvt_get_silence_buffer(pvt);
        iov[iovcnt].iov_len  = frame_size - used;
        iovcnt++;
        change_audio_endianness_to_le(iov, iovcnt);
    } else {
        PVT_STAT(pvt, write_sframes)++;
        msg = "[%s] write silence\n";

        iov[0].iov_base = pvt_get_silence_buffer(pvt);
        iov[0].iov_len  = frame_size;
        iovcnt          = 1;
        // no need to change_audio_endianness_to_le for zeroes
        //			continue;
    }

    //		iov_add(buffer, sizeof(buffer), iov);
    if (msg) {
        ast_debug(7, msg, PVT_ID(pvt));
    }

    //	}


    if (iov_write(pvt, pvt->audio_fd, iov, iovcnt) >= 0) {
        PVT_STAT(pvt, write_frames)++;
    }
    // if(write_all(pvt->audio_fd, buffer, sizeof(buffer)) != sizeof(buffer))
    //   ast_debug (1, "[%s] Write error!\n", PVT_ID(pvt));
}

#/* copy voice data from device to each channel in conference */

static void write_conference(struct pvt* pvt, const char* const buffer, size_t length)
{
    struct cpvt* cpvt;

    AST_LIST_TRAVERSE(&pvt->chans, cpvt, entry) {
        if (CPVT_IS_ACTIVE(cpvt) && !CPVT_IS_MASTER(cpvt) && CPVT_TEST_FLAG(cpvt, CALL_FLAG_MULTIPARTY) && cpvt->rd_pipe[PIPE_WRITE] >= 0) {
            const size_t wr = write_all(cpvt->rd_pipe[PIPE_WRITE], buffer, length);
            //			ast_debug (6, "[%s] write2 | call idx %d pipe fd %d wrote %d bytes\n", PVT_ID(pvt),
            // cpvt->call_idx, cpvt->rd_pipe[PIPE_WRITE], wr);
            if (wr != length) {
                ast_debug(1, "[%s][PIPE] Write error: %s\n", PVT_ID(pvt), strerror(errno));
            }
        }
    }
}

static struct ast_frame* prepare_voice_frame(struct cpvt* const cpvt, void* const buf, int samples, const struct ast_format* const fmt)
{
    struct ast_frame* const f = &cpvt->a_read_frame;

    memset(f, 0, sizeof(struct ast_frame));

    f->frametype = AST_FRAME_VOICE;
#if ASTERISK_VERSION_NUM >= 130000 /* 13+ */
    f->subclass.format = (struct ast_format*)fmt;
#elif ASTERISK_VERSION_NUM >= 100000 /* 10-13 */
    ast_format_copy(&f->subclass.format, &chan_quectel_format);
#else                                /* 10- */
    f->subclass_codec = AST_FORMAT_SLINEAR;
#endif                               /* ^10- */
    f->samples  = samples;
    f->datalen  = samples * sizeof(short);
    f->data.ptr = buf;
    f->offset   = AST_FRIENDLY_OFFSET;
    f->src      = AST_MODULE;

    return f;
}

static struct ast_frame* channel_read_tty(struct cpvt* cpvt, struct pvt* pvt, size_t frame_size, const struct ast_format* const fmt)
{
    char* const buf = cpvt->a_read_buf + AST_FRIENDLY_OFFSET;
    const int fd    = CPVT_IS_MASTER(cpvt) ? pvt->audio_fd : cpvt->rd_pipe[PIPE_READ];

    if (fd < 0) {
        return NULL;
    }

    const int res = read(fd, buf, frame_size);
    if (res <= 0) {
        if (errno && errno != EAGAIN && errno != EINTR) {
            ast_debug(1, "[%s][TTY] Read error: %s\n", PVT_ID(pvt), strerror(errno));
        }

        return NULL;
    }

    // ast_debug(7, "[%s] call idx %d read %u\n", PVT_ID(pvt), cpvt->call_idx, (unsigned)res);
    // ast_debug(6, "[%s] read | call idx %d fd %d read %d bytes\n", PVT_ID(pvt), cpvt->call_idx, pvt->audio_fd, res);

    if (CPVT_IS_MASTER(cpvt)) {
        if (CPVT_TEST_FLAG(cpvt, CALL_FLAG_MULTIPARTY)) {
            write_conference(pvt, buf, res);
        }

        PVT_STAT(pvt, a_read_bytes) += res;
        PVT_STAT(pvt, read_frames)++;
        if (res < frame_size) {
            PVT_STAT(pvt, read_sframes)++;
        }
    }

    struct ast_frame* const f = prepare_voice_frame(cpvt, buf, res / 2, fmt);
    ast_frame_byteswap_le(f);
    return f;
}

static void _show_alsa_state(int a, const char* file, int line, const char* function, const char* const pcm_desc, struct pvt* const pvt, snd_pcm_t* const pcm)
{
    const size_t ss                    = snd_pcm_status_sizeof();
    snd_pcm_status_t* const pcm_status = (snd_pcm_status_t*)ast_alloca(ss);

    int res = snd_pcm_status(pcm, pcm_status);
    if (res < 0) {
        ast_log(__LOG_ERROR, file, line, function, "[%s][ALSA][%s] Unable to get device status: %s", PVT_ID(pvt), pcm_desc, snd_strerror(res));
        return;
    }

    const snd_pcm_state_t pcm_state = snd_pcm_status_get_state(pcm_status);
    snd_pcm_sframes_t delay         = snd_pcm_status_get_delay(pcm_status);
    snd_pcm_uframes_t avail         = snd_pcm_status_get_avail(pcm_status);
    snd_pcm_uframes_t avail_max     = snd_pcm_status_get_avail_max(pcm_status);

    snd_pcm_uframes_t buffer_size;
    snd_pcm_uframes_t period_size;

    res = snd_pcm_get_params(pcm, &buffer_size, &period_size);
    if (res < 0) {
        ast_log(__LOG_ERROR, file, line, function, "[%s][ALSA][%s] Unable to get buffer sizes: %s", PVT_ID(pvt), pcm_desc, snd_strerror(res));
    }

    int sdelay = 0;
    if (res >= 0) {
        avail     %= buffer_size;
        avail_max %= buffer_size;
        delay     %= buffer_size;

        if (delay < 0l) {
            sdelay = -1l;
        } else if (delay > buffer_size) {
            sdelay = 1l;
        }
    }

    ast_log(__LOG_DEBUG, file, line, function, "[%s][ALSA][%s] Status - state:%s delay:%ld avail:%lu avail_max:%lu sdelay:%d buf:%lu|%lu\n", PVT_ID(pvt),
            pcm_desc, snd_pcm_state_name(pcm_state), delay, avail, avail_max, sdelay, period_size, buffer_size);
}

#define show_alsa_state(level, ...)                       \
    do {                                                  \
        if (DEBUG_ATLEAST(level)) {                       \
            _show_alsa_state(AST_LOG_DEBUG, __VA_ARGS__); \
        }                                                 \
    } while (0)

static struct ast_frame* channel_read_uac(struct cpvt* cpvt, struct pvt* pvt, size_t frames, const struct ast_format* const fmt)
{
    show_alsa_state(7, "CAPTURE", pvt, pvt->icard);

    const snd_pcm_state_t state = snd_pcm_state(pvt->icard);
    switch (state) {
        case SND_PCM_STATE_XRUN:
            ast_log(LOG_WARNING, "[%s][ALSA][CAPTURE] Device state: %s", PVT_ID(pvt), snd_pcm_state_name(state));

        case SND_PCM_STATE_SETUP: {
            const int res = snd_pcm_prepare(pvt->icard);
            if (res) {
                ast_log(LOG_ERROR, "[%s][ALSA][CAPTURE] Prepare failed - state:%s err:'%s'\n", PVT_ID(pvt), snd_pcm_state_name(state), snd_strerror(res));
                return NULL;
            }
            break;
        }

        case SND_PCM_STATE_PREPARED:
        case SND_PCM_STATE_RUNNING:
            break;

        default:
            ast_log(LOG_ERROR, "[%s][ALSA][CAPTURE] Device state: %s\n", PVT_ID(pvt), snd_pcm_state_name(state));
            return NULL;
    }

    char* const buf = cpvt->a_read_buf + AST_FRIENDLY_OFFSET;
    const int res   = snd_pcm_mmap_readi(pvt->icard, buf, frames);

    switch (res) {
        case -EAGAIN:
            ast_log(LOG_WARNING, "[%s][ALSA][CAPTURE] Error - try again later\n", PVT_ID(pvt));
            break;

        case -EPIPE:
        case -ESTRPIPE:
            break;

        default:
            if (res > 0) {
                if (CPVT_IS_MASTER(cpvt)) {
                    if (CPVT_TEST_FLAG(cpvt, CALL_FLAG_MULTIPARTY)) {
                        write_conference(pvt, buf, res);
                    }

                    PVT_STAT(pvt, a_read_bytes) += res * sizeof(short);
                    PVT_STAT(pvt, read_frames)++;
                    if (res < frames) {
                        PVT_STAT(pvt, read_sframes)++;
                    }
                }

                if (res < frames) {
                    ast_log(LOG_WARNING, "[%s][ALSA][CAPTURE] Short frame: %d/%d\n", PVT_ID(pvt), res, (int)frames);
                }

                return prepare_voice_frame(cpvt, buf, res, fmt);
            } else if (res < 0) {
                ast_log(LOG_ERROR, "[%s][ALSA][CAPTURE] Read error: %s\n", PVT_ID(pvt), snd_strerror(res));
            }
            break;
    }

    return NULL;
}

#if ASTERISK_VERSION_NUM >= 100000 /* 10+ */
#define subclass_integer subclass.integer
#elif ASTERISK_VERSION_NUM >= 10800 /* 1.8+ */
#define subclass_codec subclass.codec
#define subclass_integer subclass.integer
#else /* 1.8- */
#define subclass_codec subclass
#define subclass_integer subclass
#endif /* ^1.8- */

#/* */

static struct ast_frame* channel_read(struct ast_channel* channel)
{
    struct cpvt* const cpvt = ast_channel_tech_pvt(channel);
    struct ast_frame* f     = NULL;

    if (!cpvt || cpvt->channel != channel || !cpvt->pvt || cpvt->local_channel) {
        return &ast_null_frame;
    }

    struct pvt* const pvt = cpvt->pvt;

    while (ast_mutex_trylock(&pvt->lock)) {
        CHANNEL_DEADLOCK_AVOIDANCE(channel);
    }

    ast_debug(8, "[%s] Read - idx:%d state:%d audio_fd:%d\n", PVT_ID(pvt), cpvt->call_idx, cpvt->state, pvt->audio_fd);

    /* FIXME: move down for enable timing_write() to device ? */
    if (CONF_UNIQ(pvt, uac) == TRIBOOL_FALSE && (!CPVT_IS_SOUND_SOURCE(cpvt) || pvt->audio_fd < 0)) {
        goto m_unlock;
    }

    const int fdno                     = ast_channel_fdno(channel);
    const struct ast_format* const fmt = pvt_get_audio_format(pvt);
    const size_t frame_size            = pvt_get_audio_frame_size(pvt, 1, fmt);

    if (fdno == 1) {
        ast_timer_ack(pvt->a_timer, 1);
        if (CPVT_IS_MASTER(cpvt)) {
            if (CONF_UNIQ(pvt, uac) > TRIBOOL_FALSE) {
                // TODO: implement timing_write_uac
                ast_log(LOG_WARNING, "[%s] Multiparty calls not supported in UAC mode\n", PVT_ID(pvt));
            } else {
                timing_write_tty(pvt, frame_size);
            }
            ast_debug(7, "[%s] *** timing ***\n", PVT_ID(pvt));
        }
        goto m_unlock;
    }

    if (CONF_UNIQ(pvt, uac) > TRIBOOL_FALSE && CPVT_IS_MASTER(cpvt)) {
        f = channel_read_uac(cpvt, pvt, frame_size / 2, fmt);
    } else {
        f = channel_read_tty(cpvt, pvt, frame_size, fmt);
    }

m_unlock:
    ast_mutex_unlock(&pvt->lock);

    return (f == NULL || f->frametype == AST_FRAME_NULL) ? &ast_null_frame : f;
}

static int channel_write_tty(struct ast_channel* channel, struct ast_frame* f, struct cpvt* cpvt, struct pvt* pvt, size_t frame_size)
{
    if (CPVT_TEST_FLAG(cpvt, CALL_FLAG_MULTIPARTY) && !CPVT_TEST_FLAG(cpvt, CALL_FLAG_BRIDGE_CHECK)) {
#if ASTERISK_VERSION_NUM >= 120000 /* 12+ */
        RAII_VAR(struct ast_channel*, bridged, ast_channel_bridge_peer(channel), ast_channel_cleanup);
#else  /* 12- */
        struct ast_channel* bridged = ast_bridged_channel(channel);
#endif /* ^12- */
        struct cpvt* tmp_cpvt;

        CPVT_SET_FLAGS(cpvt, CALL_FLAG_BRIDGE_CHECK);

        if (bridged && ast_channel_tech(bridged) == &channel_tech && (tmp_cpvt = ast_channel_tech_pvt(bridged)) && tmp_cpvt->pvt == pvt) {
            CPVT_SET_FLAGS(cpvt, CALL_FLAG_BRIDGE_LOOP);
            CPVT_SET_FLAGS((struct cpvt*)ast_channel_tech_pvt(bridged), CALL_FLAG_BRIDGE_LOOP);
            ast_log(LOG_WARNING, "[%s] Bridged channels %s and %s working on same device, discard writes to avoid voice loop\n", PVT_ID(pvt),
                    ast_channel_name(channel), ast_channel_name(bridged));
            return 0;
        }
    }

    if (CONF_SHARED(pvt, multiparty) && f->datalen) {
        /** try to minimize of ast_frame_adjust_volume() calls:
         *  one hand we must obey txgain but with other divide gain to
         *  number of mixed channels. In some cases one call of ast_frame_adjust_volume() enough
         */

        int gains[2];

        gains[1] = mixb_streams(&pvt->a_write_mixb);
        if (gains[1] < 1 || pvt->a_timer == NULL) {
            gains[1] = 1;
        }

        gains[0] = 0;

        for (size_t count = 0; count < ITEMS_OF(gains); ++count) {
            if (gains[count] > 1 || gains[count] < -1) {
                if (ast_frame_adjust_volume(f, gains[count]) == -1) {
                    ast_debug(1, "[%s] Volume could not be adjusted!\n", PVT_ID(pvt));
                }
            }
        }
    }

    if (CONF_SHARED(pvt, multiparty)) {  // use mix buffer
        const size_t count = mixb_free(&pvt->a_write_mixb, &cpvt->mixstream);

        if (count < (size_t)f->datalen) {
            mixb_read_upd(&pvt->a_write_mixb, f->datalen - count);

            PVT_STAT(pvt, write_rb_overflow_bytes) += f->datalen - count;
            PVT_STAT(pvt, write_rb_overflow)++;
        }

        mixb_write(&pvt->a_write_mixb, &cpvt->mixstream, f->data.ptr, f->datalen);

        /*
                ast_debug (6, "[%s] write | call idx %d, %d bytes lwrite %d lused %d write %d used %d\n", PVT_ID(pvt),
           cpvt->call_idx, f->datalen, cpvt->write, cpvt->used, pvt->a_write_rb.write, pvt->a_write_rb.used);
                rb_tetris(&pvt->a_write_rb, f->data.ptr, f->datalen, &cpvt->write, &cpvt->used);
                ast_debug (6, "[%s] write | lwrite %d lused %d write %d used %d\n", PVT_ID(pvt), cpvt->write,
           cpvt->used, pvt->a_write_rb.write, pvt->a_write_rb.used);
        */
    } else if (CPVT_IS_ACTIVE(cpvt)) {  // direct write
        struct iovec iov;

        ast_frame_byteswap_le(f);
        iov.iov_base = f->data.ptr;
        iov.iov_len  = f->datalen;

        if (iov_write(pvt, pvt->audio_fd, &iov, 1) >= 0) {
            PVT_STAT(pvt, write_frames)++;
        }
    }

    ast_debug(7, "[%s] Write frame - samples:%d bytes:%d\n", PVT_ID(pvt), f->samples, f->datalen);
    return 0;
}

static int channel_write_uac(struct ast_channel* a, struct ast_frame* f, struct cpvt* b, struct pvt* pvt, size_t frame_size)
{
    const int samples = f->samples;
    int res           = 0;

    snd_pcm_state_t state = snd_pcm_state(pvt->ocard);
    switch (state) {
        case SND_PCM_STATE_XRUN:
            ast_log(LOG_WARNING, "[%s][ALSA][PLAYBACK] Device state: %s\n", PVT_ID(pvt), snd_pcm_state_name(state));

        case SND_PCM_STATE_SETUP:
            res = snd_pcm_prepare(pvt->ocard);
            if (res) {
                ast_log(LOG_ERROR, "[%s][ALSA][PLAYBACK] Prepare failed - state:%s err:'%s'\n", PVT_ID(pvt), snd_pcm_state_name(state), snd_strerror(res));
                goto w_finish;
            }
            break;

        case SND_PCM_STATE_PREPARED:
        case SND_PCM_STATE_RUNNING:
            break;

        default:
            ast_log(LOG_ERROR, "[%s][ALSA][PLAYBACK] Device state: %s\n", PVT_ID(pvt), snd_pcm_state_name(state));
            res = -1;
            goto w_finish;
    }

    if (pvt->ocard_channels == 1u) {
        res = snd_pcm_mmap_writei(pvt->ocard, f->data.ptr, samples);
    } else {
        void* d[pvt->ocard_channels];
        for (unsigned int i = 0; i < pvt->ocard_channels; ++i) {
            d[i] = f->data.ptr;
        }
        res = snd_pcm_mmap_writen(pvt->ocard, (void**)&d, samples);
    }

    switch (res) {
        case -EAGAIN:
            ast_log(LOG_WARNING, "[%s][ALSA][PLAYBACK] Error - try again later\n", PVT_ID(pvt));
            res = 0;
            break;

        case -EPIPE:
        case -ESTRPIPE:
            break;

        default:
            if (res != samples) {
                PVT_STAT(pvt, write_frames)++;
                PVT_STAT(pvt, write_sframes)++;
                ast_log(LOG_WARNING, "[%s][ALSA][PLAYBACK] %d/%d samples\n", PVT_ID(pvt), res, samples);
                res = 0;
            } else if (res < 0) {
                state = snd_pcm_state(pvt->ocard);
                ast_log(LOG_WARNING, "[%s][ALSA][PLAYBACK] state:%d err:'%s'\n", PVT_ID(pvt), (int)state, snd_strerror(res));
            } else {
                PVT_STAT(pvt, write_frames)++;
            }
            show_alsa_state(6, "PLAYBACK", pvt, pvt->ocard);
            break;
    }

w_finish:

    return res >= 0 ? 0 : res;
}

#/* */

static int channel_write(struct ast_channel* channel, struct ast_frame* f)
{
    struct cpvt* const cpvt = ast_channel_tech_pvt(channel);
    int res                 = -1;

    if (!cpvt || cpvt->channel != channel || !cpvt->pvt || cpvt->local_channel) {
        return 0;
    }

    /* TODO: write silence better ? */
    /* TODO: check end of bridge loop condition */
    /* never write to same device from other channel its possible for call hold or conference */
    if (CPVT_TEST_FLAG(cpvt, CALL_FLAG_BRIDGE_LOOP)) {
        return 0;
    }

    struct pvt* const pvt = cpvt->pvt;

    while (ast_mutex_trylock(&pvt->lock)) {
        CHANNEL_DEADLOCK_AVOIDANCE(channel);
    }

    const struct ast_format* const fmt = pvt_get_audio_format(pvt);
    const size_t frame_size            = pvt_get_audio_frame_size(pvt, 0, fmt);

#if ASTERISK_VERSION_NUM >= 130000 /* 13+ */
    if (f->frametype != AST_FRAME_VOICE || ast_format_cmp(f->subclass.format, fmt) != AST_FORMAT_CMP_EQUAL)
#elif ASTERISK_VERSION_NUM >= 100000 /* 10-13 */
    if (f->frametype != AST_FRAME_VOICE || f->subclass.format.id != AST_FORMAT_SLINEAR)
#else                                /* 10- */
    if (f->frametype != AST_FRAME_VOICE || f->subclass_codec != AST_FORMAT_SLINEAR)
#endif                               /* ^10- */
    {
        ast_debug(1, "[%s] Unsupported audio codec: %s\n", PVT_ID(pvt), ast_format_get_name(f->subclass.format));
        ast_mutex_unlock(&pvt->lock);
        return 0;
    }

    ast_debug(8, "[%s] Write - idx:%d state:%d\n", PVT_ID(pvt), cpvt->call_idx, cpvt->state);

    if (f->datalen < frame_size) {
        ast_debug(8, "[%s] Short voice frame: %d/%d\n", PVT_ID(pvt), f->datalen, (int)frame_size);
        PVT_STAT(pvt, write_tframes)++;
    } else if (f->datalen > frame_size) {
        ast_debug(8, "[%s] Large voice frame: %d/%d\n", PVT_ID(pvt), f->datalen, (int)frame_size);
    }

    if (CONF_UNIQ(pvt, uac) > TRIBOOL_FALSE && CPVT_IS_MASTER(cpvt)) {
        res = channel_write_uac(channel, f, cpvt, pvt, frame_size);
    } else {
        res = channel_write_tty(channel, f, cpvt, pvt, frame_size);
    }

    ast_mutex_unlock(&pvt->lock);
    return res;
}

#undef subclass_integer
#undef subclass_codec

#/* */

static int channel_fixup(struct ast_channel* oldchannel, struct ast_channel* newchannel)
{
    struct cpvt* const cpvt = ast_channel_tech_pvt(newchannel);

    if (!cpvt || !cpvt->pvt) {
        ast_log(LOG_WARNING, "Call on unreferenced %s\n", ast_channel_name(newchannel));
        return -1;
    }
    struct pvt* const pvt = cpvt->pvt;

    ast_mutex_lock(&pvt->lock);
    if (cpvt->channel == oldchannel) {
        cpvt->channel = newchannel;
    }
    ast_mutex_unlock(&pvt->lock);

    return 0;
}

/* FIXME: must modify in conjuction with state on call not whole device? */
#if ASTERISK_VERSION_NUM >= 110000 /* 11+ */
static int channel_devicestate(const char* data)
#else  /* 11- */
static int channel_devicestate(void* data)
#endif /* ^11- */
{
    int res = AST_DEVICE_INVALID;

    const char* const device = ast_strdupa(data ? data : "");
    ast_debug(1, "[%s] Checking device state\n", device);

    struct pvt* const pvt = find_device_ext(device);
    if (!pvt) {
        return res;
    }

    if (pvt->connected) {
        if (is_dial_possible(pvt, CALL_FLAG_NONE)) {
            res = AST_DEVICE_NOT_INUSE;
        } else {
            res = AST_DEVICE_INUSE;
        }
    } else {
        res = AST_DEVICE_UNAVAILABLE;
    }
    ast_mutex_unlock(&pvt->lock);
    return res;
}

#/* */

static int channel_indicate(struct ast_channel* channel, int condition, const void* data, attribute_unused size_t datalen)
{
    ast_debug(1, "[%s] Requested indication %d\n", ast_channel_name(channel), condition);

    struct cpvt* const cpvt = ast_channel_tech_pvt(channel);
    struct pvt* pvt         = NULL;
    int res                 = 0;

    if (!cpvt || cpvt->channel != channel || !cpvt->pvt) {
        ast_log(LOG_WARNING, "Call on unreferenced %s\n", ast_channel_name(channel));
    } else {
        pvt = cpvt->pvt;
    }

    switch (condition) {
        case AST_CONTROL_BUSY:
        case AST_CONTROL_CONGESTION:
        case AST_CONTROL_RINGING:
        case AST_CONTROL_MASQUERADE_NOTIFY:
#if ASTERISK_VERSION_NUM >= 110000 /* 11+ */
        case AST_CONTROL_PVT_CAUSE_CODE:
#endif
        case -1:
            res = -1;
            break;

/* appears in r295843 */
#ifdef HAVE_AST_CONTROL_SRCCHANGE
        case AST_CONTROL_SRCCHANGE:
#endif
        case AST_CONTROL_PROGRESS:
        case AST_CONTROL_PROCEEDING:
        case AST_CONTROL_VIDUPDATE:
        case AST_CONTROL_SRCUPDATE:
            break;

        case AST_CONTROL_HOLD:
            if (!pvt || CONF_SHARED(pvt, moh)) {
                ast_moh_start(channel, data, NULL);
            } else {
                ast_mutex_lock(&pvt->lock);
                at_enqueue_mute(cpvt, 1);
                ast_mutex_unlock(&pvt->lock);
            }
            break;

        case AST_CONTROL_UNHOLD:
            if (!pvt || CONF_SHARED(pvt, moh)) {
                ast_moh_stop(channel);
            } else {
                ast_mutex_lock(&pvt->lock);
                at_enqueue_mute(cpvt, 0);
                ast_mutex_unlock(&pvt->lock);
            }
            break;

        case AST_CONTROL_CONNECTED_LINE: {
            struct ast_party_connected_line* const cnncd = ast_channel_connected(channel);
            ast_mutex_lock(&pvt->lock);
            ast_log(LOG_NOTICE, "[%s] Connected party is now %s <%s>\n", PVT_ID(pvt), S_COR(cnncd->id.name.valid, cnncd->id.name.str, ""),
                    S_COR(cnncd->id.number.valid, cnncd->id.number.str, ""));
            ast_mutex_unlock(&pvt->lock);
            break;
        }

        default:
            ast_log(LOG_WARNING, "[%s] Don't know how to indicate condition %d\n", ast_channel_name(channel), condition);
            res = -1;
            break;
    }

    return res;
}

/* ARCH: move to cpvt */
/* FIXME: protection for cpvt->channel if exists */
#/* NOTE: called from device level with locked pvt */

void change_channel_state(struct cpvt* cpvt, unsigned newstate, int cause)
{
    const call_state_t oldstate = cpvt->state;
    if (newstate == oldstate) {
        return;
    }

    struct pvt* const pvt             = cpvt->pvt;
    struct ast_channel* const channel = cpvt->channel;
    const short call_idx              = cpvt->call_idx;

    cpvt->state = newstate;
    PVT_STATE(pvt, chan_count[oldstate])--;
    PVT_STATE(pvt, chan_count[newstate])++;

    // U+2192 : Rightwards Arrow : 0xE2 0x86 0x92
    if (CONF_SHARED(pvt, multiparty)) {
        ast_debug(1, "[%s] Call - idx:%d channel:%s mpty:%d [%s] \xE2\x86\x92 [%s]\n", PVT_ID(pvt), call_idx, channel ? "attached" : "detached",
                  CPVT_TEST_FLAG(cpvt, CALL_FLAG_MULTIPARTY) ? 1 : 0, call_state2str(oldstate), call_state2str(newstate));
    } else {
        ast_debug(1, "[%s] Call - idx:%d channel:%s [%s] \xE2\x86\x92 [%s]\n", PVT_ID(pvt), call_idx, channel ? "attached" : "detached",
                  call_state2str(oldstate), call_state2str(newstate));
    }

    /* update bits of devstate cache */
    switch (newstate) {
        case CALL_STATE_ACTIVE:
        case CALL_STATE_RELEASED:
            /* no split to incoming/outgoing because these states not intersect */
            switch (oldstate) {
                case CALL_STATE_INIT:
                case CALL_STATE_DIALING:
                case CALL_STATE_ALERTING:
                    pvt->dialing = 0;
                    break;
                case CALL_STATE_INCOMING:
                    pvt->ring = 0;
                    break;
                case CALL_STATE_WAITING:
                    pvt->cwaiting = 0;
                    break;
                default:
                    break;
            }
            break;

        default:
            break;
    }

    /* check channel is dead */
    if (!channel) {
        /* channel already dead */
        if (newstate == CALL_STATE_RELEASED) {
            cpvt_free(cpvt);
        }
    } else {
        /* for live channel */
        switch (newstate) {
            case CALL_STATE_DIALING:
                /* from ^ORIG:idx,y */
                activate_call(cpvt);
                queue_control_channel(cpvt, AST_CONTROL_PROGRESS);
                ast_setstate(channel, AST_STATE_DIALING);
                break;

            case CALL_STATE_ALERTING:
                activate_call(cpvt);
                queue_control_channel(cpvt, AST_CONTROL_RINGING);
                ast_setstate(channel, AST_STATE_RINGING);
                break;

            case CALL_STATE_INCOMING:
                activate_call(cpvt);
                break;

            case CALL_STATE_ACTIVE:
                activate_call(cpvt);
                if (oldstate == CALL_STATE_ONHOLD) {
                    ast_debug(1, "[%s] Unhold call idx:%d\n", PVT_ID(pvt), call_idx);
                    queue_control_channel(cpvt, AST_CONTROL_UNHOLD);
                } else if (cpvt->dir == CALL_DIR_OUTGOING) {
                    ast_debug(1, "[%s] Remote end answered on call idx:%d\n", PVT_ID(pvt), call_idx);
                    queue_control_channel(cpvt, AST_CONTROL_ANSWER);
                } else { /* if (cpvt->answered) */
                    ast_debug(1, "[%s] Call idx:%d answer\n", PVT_ID(pvt), call_idx);
                    ast_setstate(channel, AST_STATE_UP);
                }
                break;

            case CALL_STATE_ONHOLD:
                disactivate_call(cpvt);
                ast_debug(1, "[%s] Hold call idx:%d\n", PVT_ID(pvt), call_idx);
                queue_control_channel(cpvt, AST_CONTROL_HOLD);
                break;

            case CALL_STATE_RELEASED:
                disactivate_call(cpvt);
                /* from +CEND, restart or disconnect */


                /* drop channel -> cpvt reference */
                ast_channel_tech_pvt_set(channel, NULL);
                cpvt_free(cpvt);
                if (queue_hangup(channel, cause)) {
                    ast_log(LOG_ERROR, "[%s] Error queueing hangup...\n", PVT_ID(pvt));
                }
                break;
        }
    }
}

#/* */

static void set_channel_vars(struct pvt* pvt, struct ast_channel* channel)
{
    char plmn[20];  // public land mobile network
    snprintf(plmn, ITEMS_OF(plmn), "%d", pvt->operator);

    char mcc[20];  // mobile country code
    snprintf(mcc, ITEMS_OF(mcc), "%d", pvt->operator/ 100);

    char mnc[20];  // mobile network code
    snprintf(mnc, ITEMS_OF(mnc), "%02d", pvt->operator% 100);

    const channel_var_t dev_vars[] = {
        {"QUECTELNAME", PVT_ID(pvt)},
        {"QUECTELNETWORKNAME", pvt->network_name},
        {"QUECTELSHORTNETWORKNAME", pvt->short_network_name},
        {"QUECTELPROVIDER", pvt->provider_name},
        {"QUECTELPLMN", plmn},
        {"QUECTELMCC", mcc},
        {"QUECTELMNC", mnc},
        {"QUECTELIMEI", pvt->imei},
        {"QUECTELIMSI", pvt->imsi},
        {"QUECTELICCID", pvt->iccid},
        {"QUECTELNUMBER", S_OR(pvt->subscriber_number, "Unknown")},
    };

#if ASTERISK_VERSION_NUM >= 110000 /* 11+  */
    ast_channel_language_set(channel, CONF_SHARED(pvt, language));
#else  /* 11- */
    // TODO uncomment and fix
    // ast_string_field_set (channel, language, CONF_SHARED(pvt, language);
#endif /* ^11- */

    for (size_t i = 0; i < ITEMS_OF(dev_vars); ++i) {
        setvar_helper(pvt, channel, dev_vars[i].name, dev_vars[i].value);
    }
}

/* NOTE: called from device and current levels with locked pvt */
#if ASTERISK_VERSION_NUM >= 120000 /* 12+ */
struct ast_channel* new_channel(struct pvt* pvt, int ast_state, const char* cid_num, int call_idx, unsigned dir, call_state_t state, const char* dnid,
                                const struct ast_assigned_ids* assignedids, attribute_unused const struct ast_channel* requestor, unsigned local_channel)
#else  /* 13- */
struct ast_channel* new_channel(struct pvt* pvt, int ast_state, const char* cid_num, int call_idx, unsigned dir, call_state_t state, const char* dnid,
                                attribute_unused const struct ast_channel* requestor, unsigned local_channel)
#endif /* ^13- */
{
    struct ast_channel* channel;
    struct cpvt* cpvt;

    cpvt = cpvt_alloc(pvt, call_idx, dir, CALL_STATE_INIT, local_channel);
    if (cpvt) {
#if ASTERISK_VERSION_NUM >= 120000 /* 12+ */
        channel = ast_channel_alloc(1, ast_state, cid_num, PVT_ID(pvt), NULL, dnid, CONF_SHARED(pvt, context), assignedids, requestor, 0, "%s/%s-%02u%08lx",
                                    channel_tech.type, PVT_ID(pvt), call_idx, pvt->channel_instance);
#elif ASTERISK_VERSION_NUM >= 10800 /* 1.8+ */
        channel =
            ast_channel_alloc(1, ast_state, cid_num, PVT_ID(pvt), NULL, dnid, CONF_SHARED(pvt, context), requestor ? ast_channel_linkedid(requestor) : NULL, 0,
                              "%s/%s-%02u%08lx", channel_tech.type, PVT_ID(pvt), call_idx, pvt->channel_instance);
#else                               /* 1.8- */
        channel = ast_channel_alloc(1, ast_state, cid_num, PVT_ID(pvt), NULL, dnid, CONF_SHARED(pvt, context), 0, "%s/%s-%02u%08lx", channel_tech.type,
                                    PVT_ID(pvt), call_idx, pvt->channel_instance);
#endif                              /* ^1.8- */
        if (channel) {
            cpvt->channel = channel;
            pvt->channel_instance++;

            ast_channel_tech_pvt_set(channel, cpvt);
            ast_channel_tech_set(channel, &channel_tech);

#if ASTERISK_VERSION_NUM >= 130000 /* 13+ */
            if (local_channel) {
                struct ast_format_cap* const cap = ast_format_cap_alloc(AST_FORMAT_CAP_FLAG_DEFAULT);
                ast_format_cap_append_by_type(cap, AST_MEDIA_TYPE_TEXT);
                ast_channel_nativeformats_set(channel, cap);
            } else {
                struct ast_format* const fmt     = (struct ast_format*)pvt_get_audio_format(pvt);
                struct ast_format_cap* const cap = ast_format_cap_alloc(AST_FORMAT_CAP_FLAG_DEFAULT);
                ast_format_cap_append(cap, fmt, 40);
                ast_format_cap_set_framing(cap, 40);
                ast_channel_nativeformats_set(channel, cap);

                ast_channel_set_rawreadformat(channel, fmt);
                ast_channel_set_rawwriteformat(channel, fmt);
                ast_channel_set_writeformat(channel, fmt);
                ast_channel_set_readformat(channel, fmt);
            }
#elif ASTERISK_VERSION_NUM >= 110000 /* 11+ */
            ast_format_cap_add(ast_channel_nativeformats(channel), &chan_quectel_format);
            ast_format_copy(ast_channel_rawreadformat(channel), &chan_quectel_format);
            ast_format_copy(ast_channel_rawwriteformat(channel), &chan_quectel_format);
            ast_format_copy(ast_channel_writeformat(channel), &chan_quectel_format);
            ast_format_copy(ast_channel_readformat(channel), &chan_quectel_format);
#elif ASTERISK_VERSION_NUM >= 100000 /* 10+ */
            ast_format_cap_add(channel->nativeformats, &chan_quectel_format);
            ast_format_copy(&channel->rawreadformat, &chan_quectel_format);
            ast_format_copy(&channel->rawwriteformat, &chan_quectel_format);
            ast_format_copy(&channel->writeformat, &chan_quectel_format);
            ast_format_copy(&channel->readformat, &chan_quectel_format);
#else                                /* 10- */
            channel->nativeformats  = AST_FORMAT_SLINEAR;
            channel->rawreadformat  = AST_FORMAT_SLINEAR;
            channel->rawwriteformat = AST_FORMAT_SLINEAR;
            channel->readformat     = AST_FORMAT_SLINEAR;
            channel->writeformat    = AST_FORMAT_SLINEAR;
#endif                               /* ^10- */

            ast_channel_set_fd(channel, 0, pvt->audio_fd);
            if (pvt->a_timer) {
                ast_channel_set_fd(channel, 1, ast_timer_fd(pvt->a_timer));
                ast_timer_set_rate(pvt->a_timer, 50);
            }

            set_channel_vars(pvt, channel);

            if (!ast_strlen_zero(dnid)) {
                setvar_helper(pvt, channel, "CALLERID(dnid)", dnid);
            }

            ast_jb_configure(channel, &CONF_GLOBAL(jbconf));

            if (state != CALL_STATE_INIT) {
                change_channel_state(cpvt, state, AST_CAUSE_NORMAL_UNSPECIFIED);
            }

            ast_module_ref(self_module());

#if ASTERISK_VERSION_NUM >= 120000 /* 12+ */
            /* commit e2630fcd516b8f794bf342d9fd267b0c905e79ce
             * Date:   Wed Dec 18 19:28:05 2013 +0000a
             * ast_channel_alloc() returns allocated channels locked. */
            ast_channel_unlock(channel);
#endif /* ^12+ */

            return channel;
        }
        cpvt_free(cpvt);
    }
    return NULL;
}

/* NOTE: bg: hmm ast_queue_control() say no need channel lock, trylock got deadlock up to 30 seconds here */
/* NOTE: called from device and current levels with pvt locked */
int queue_control_channel(struct cpvt* cpvt, enum ast_control_frame_type control)
{
    /*
        for (;;)
        {
    */
    if (cpvt->channel) {
        /*
                    if (ast_channel_trylock (cpvt->channel))
                    {
                        DEADLOCK_AVOIDANCE (&cpvt->pvt->lock);
                    }
                    else
                    {
        */
        ast_queue_control(cpvt->channel, control);
        /*
                        ast_channel_unlock (cpvt->channel);
                        break;
                    }
        */
    }
    /*
            else
            {
                break;
            }
        }
    */
    return 0;
}

/* NOTE: bg: hmm ast_queue_hangup() say no need channel lock before call, trylock got deadlock up to 30 seconds here */
/* NOTE: bg: called from device level and change_channel_state() with pvt locked */
int queue_hangup(struct ast_channel* channel, int hangupcause)
{
    int rv = 0;
    if (channel) {
        if (hangupcause) {
            ast_channel_hangupcause_set(channel, hangupcause);
        }

        rv = ast_queue_hangup(channel);
    }
    return rv;
}

void start_local_report_channel(struct pvt* pvt, const char* number, const struct ast_str* const payload, const char* ts, const char* dt, int success,
                                const char report_type, const struct ast_str* const report)
{
    const char report_type_str[2] = {report_type, '\000'};
    const channel_var_t vars[]    = {
        {"SMS_REPORT_PAYLOAD", ast_str_buffer(payload)},
        {"SMS_REPORT_TS", ts},
        {"SMS_REPORT_DT", dt},
        {"SMS_REPORT_SUCCESS", S_COR(success, "1", "0")},
        {"SMS_REPORT_TYPE", report_type_str},
        {"SMS_REPORT", ast_str_buffer(report)},
        {NULL, NULL},
    };
    start_local_channel(pvt, "report", number, vars);
}

#/* NOTE: bg: called from device level with pvt locked */

void start_local_channel(struct pvt* pvt, const char* exten, const char* number, const channel_var_t* vars)
{
    struct ast_channel* channel;
    int cause = 0;
    char channel_name[1024];

    snprintf(channel_name, sizeof(channel_name), "%s@%s", exten, CONF_SHARED(pvt, context));

#if ASTERISK_VERSION_NUM >= 120000 /* 12+ */
    channel = ast_request("Local", pvt->local_format_cap ? pvt->local_format_cap : channel_tech.capabilities, NULL, NULL, channel_name, &cause);
#elif ASTERISK_VERSION_NUM >= 100000 /* 10-12 */
    channel = ast_request("Local", chan_quectel_format_cap, NULL, channel_name, &cause);
#elif ASTERISK_VERSION_NUM >= 10800  /* 1.8+ */
    channel = ast_request("Local", AST_FORMAT_AUDIO_MASK, NULL, channel_name, &cause);
#else                                /* 1.8- */
    channel = ast_request("Local", AST_FORMAT_AUDIO_MASK, channel_name, &cause);
#endif                               /* ^1.8- */
    if (channel) {
        set_channel_vars(pvt, channel);
        ast_set_callerid(channel, number, PVT_ID(pvt), number);

        for (; !ast_strlen_zero(vars->name); ++vars) {
            setvar_helper(pvt, channel, vars->name, vars->value);
        }

        cause = ast_pbx_start(channel);
        if (cause) {
            ast_hangup(channel);
            ast_log(LOG_ERROR, "[%s] Unable to start pbx on channel Local/%s\n", PVT_ID(pvt), channel_name);
        }
    } else {
        ast_log(LOG_ERROR, "[%s] Unable to request channel Local/%s\n", PVT_ID(pvt), channel_name);
    }
}

#/* */

static int channel_func_read(struct ast_channel* channel, attribute_unused const char* function, char* data, char* buf, size_t len)
{
    struct cpvt* const cpvt = ast_channel_tech_pvt(channel);
    int ret                 = 0;

    if (!cpvt || !cpvt->pvt) {
        ast_log(LOG_WARNING, "call on unreferenced %s\n", ast_channel_name(channel));
        return -1;
    }
    struct pvt* const pvt = cpvt->pvt;

    if (!strcasecmp(data, "callstate")) {
        while (ast_mutex_trylock(&pvt->lock)) {
            CHANNEL_DEADLOCK_AVOIDANCE(channel);
        }
        call_state_t state = cpvt->state;
        ast_mutex_unlock(&pvt->lock);

        ast_copy_string(buf, call_state2str(state), len);
    } else {
        ret = -1;
    }

    return ret;
}

#/* */

static int channel_func_write(struct ast_channel* channel, const char* function, char* data, const char* value)
{
    struct cpvt* const cpvt = ast_channel_tech_pvt(channel);
    call_state_t newstate, oldstate;
    int ret = 0;

    if (!cpvt || !cpvt->pvt) {
        ast_log(LOG_WARNING, "call on unreferenced %s\n", ast_channel_name(channel));
        return -1;
    }

    if (!strcasecmp(data, "callstate")) {
        if (!strcasecmp(value, "active")) {
            newstate = CALL_STATE_ACTIVE;
        } else {
            ast_log(LOG_WARNING, "Invalid value for %s(callstate).\n", function);
            return -1;
        }

        while (ast_mutex_trylock(&cpvt->pvt->lock)) {
            CHANNEL_DEADLOCK_AVOIDANCE(channel);
        }
        oldstate = cpvt->state;

        if (oldstate == newstate)
            ;
        else if (oldstate == CALL_STATE_ONHOLD) {
            if (at_enqueue_activate(cpvt)) {
                /* TODO: handle error */
                ast_log(LOG_ERROR, "Error state to active for call idx %d in %s(callstate).\n", cpvt->call_idx, function);
            }
        } else {
            ast_log(LOG_WARNING, "allow change state to 'active' only from 'held' in %s(callstate).\n", function);
            ret = -1;
        }
        ast_mutex_unlock(&cpvt->pvt->lock);
    } else {
        ret = -1;
    }

    return ret;
}

struct ast_channel_tech channel_tech = {.type        = "Quectel",
                                        .description = MODULE_DESCRIPTION,
#if ASTERISK_VERSION_NUM < 100000 /* 10- */
                                        .capabilities = AST_FORMAT_SLINEAR,
#endif /* ^10- */
                                        .requester          = channel_request,
                                        .call               = channel_call,
                                        .hangup             = channel_hangup,
                                        .answer             = channel_answer,
                                        .send_digit_begin   = channel_digit_begin,
                                        .send_digit_end     = channel_digit_end,
                                        .read               = channel_read,
                                        .write              = channel_write,
                                        .exception          = channel_read,
                                        .fixup              = channel_fixup,
                                        .devicestate        = channel_devicestate,
                                        .indicate           = channel_indicate,
                                        .func_channel_read  = channel_func_read,
                                        .func_channel_write = channel_func_write};
