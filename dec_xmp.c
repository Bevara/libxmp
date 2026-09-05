/*
 *			GPAC - Multimedia Framework C SDK
 *
 *  This file is part of GPAC / tracker module decoder filter, based on libxmp
 *  (https://github.com/libxmp/libxmp), which plays roughly ninety module
 *  formats - Protracker MOD, Fast Tracker XM, Scream Tracker S3M, Impulse
 *  Tracker IT and many others.
 *
 *  A module is a score plus its instruments, not a sample stream: it is
 *  rendered here to 16-bit stereo PCM at 44.1 kHz, in one pass, and sent as a
 *  single packet.
 */

#include <gpac/filters.h>
#include <gpac/constants.h>
#include <string.h>
#include <stdlib.h>

#include <xmp.h>

#define XMPDEC_SAMPLE_RATE 44100
#define XMPDEC_CHANNELS 2
/* Rendering is capped so a module that never ends cannot grow the output
 * packet without bound: 180 s of 16-bit stereo at 44.1 kHz is about 30 MB. */
#define XMPDEC_MAX_SECONDS 180
#define XMPDEC_CHUNK_FRAMES 4096

typedef struct
{
	GF_FilterPid *ipid, *opid;
	Bool is_playing;
} GF_XMPDecCtx;

static GF_Err xmpdec_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	GF_XMPDecCtx *ctx = (GF_XMPDecCtx *)gf_filter_get_udta(filter);

	if (is_remove)
	{
		if (ctx->opid)
		{
			gf_filter_pid_remove(ctx->opid);
			ctx->opid = NULL;
		}
		ctx->ipid = NULL;
		return GF_OK;
	}
	if (!gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	ctx->ipid = pid;
	gf_filter_pid_set_framing_mode(pid, GF_TRUE);

	if (!ctx->opid)
		ctx->opid = gf_filter_pid_new(filter);

	/* All of these have to be on the pid before any data flows: the output
	 * link is resolved from them alone (same constraint as the image filters
	 * in this repo, see dec_qoi.c). */
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STREAM_TYPE, &PROP_UINT(GF_STREAM_AUDIO));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, &PROP_UINT(GF_CODECID_RAW));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_AUDIO_FORMAT, &PROP_UINT(GF_AUDIO_FMT_S16));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_SAMPLE_RATE, &PROP_UINT(XMPDEC_SAMPLE_RATE));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_TIMESCALE, &PROP_UINT(XMPDEC_SAMPLE_RATE));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_NUM_CHANNELS, &PROP_UINT(XMPDEC_CHANNELS));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CHANNEL_LAYOUT, &PROP_LONGUINT(GF_AUDIO_CH_FRONT_LEFT | GF_AUDIO_CH_FRONT_RIGHT));

	return GF_OK;
}

static Bool xmpdec_process_event(GF_Filter *filter, const GF_FilterEvent *evt)
{
	GF_XMPDecCtx *ctx = (GF_XMPDecCtx *)gf_filter_get_udta(filter);
	switch (evt->base.type)
	{
	case GF_FEVT_PLAY:
		ctx->is_playing = GF_TRUE;
		return GF_FALSE;
	case GF_FEVT_STOP:
		ctx->is_playing = GF_FALSE;
		return GF_FALSE;
	default:
		return GF_FALSE;
	}
}

static GF_Err xmpdec_process(GF_Filter *filter)
{
	GF_FilterPacket *pck, *dst_pck;
	u8 *data, *output;
	u32 size;
	xmp_context xc;
	s16 *pcm = NULL;
	u32 pcm_alloc, pcm_used = 0, chunk_bytes;
	GF_XMPDecCtx *ctx = (GF_XMPDecCtx *)gf_filter_get_udta(filter);

	pck = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck)
	{
		if (gf_filter_pid_is_eos(ctx->ipid))
		{
			gf_filter_pid_set_eos(ctx->opid);
			return GF_EOS;
		}
		return GF_OK;
	}
	data = (u8 *)gf_filter_pck_get_data(pck, &size);
	if (!data)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_IO_ERR;
	}

	xc = xmp_create_context();
	if (!xc)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_OUT_OF_MEM;
	}
	if (xmp_load_module_from_memory(xc, data, (long)size) != 0)
	{
		xmp_free_context(xc);
		gf_filter_pid_drop_packet(ctx->ipid);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[XMPDec] Not a module libxmp can load\n"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}
	gf_filter_pid_drop_packet(ctx->ipid);

	if (xmp_start_player(xc, XMPDEC_SAMPLE_RATE, 0) != 0)
	{
		xmp_release_module(xc);
		xmp_free_context(xc);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[XMPDec] Failed to start the player\n"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	pcm_alloc = XMPDEC_SAMPLE_RATE * XMPDEC_CHANNELS; /* one second to start with */
	pcm = (s16 *)gf_malloc(pcm_alloc * sizeof(s16));
	chunk_bytes = XMPDEC_CHUNK_FRAMES * XMPDEC_CHANNELS * (u32)sizeof(s16);
	if (pcm)
	{
		u32 max_samples = (u32)XMPDEC_MAX_SECONDS * XMPDEC_SAMPLE_RATE * XMPDEC_CHANNELS;
		/* loop count 1: xmp_play_buffer returns -XMP_END once the module has
		 * played through once, instead of looping forever. */
		while (pcm_used < max_samples)
		{
			if (pcm_used + chunk_bytes / sizeof(s16) > pcm_alloc)
			{
				s16 *bigger;
				pcm_alloc *= 2;
				bigger = (s16 *)gf_realloc(pcm, pcm_alloc * sizeof(s16));
				if (!bigger)
					break;
				pcm = bigger;
			}
			if (xmp_play_buffer(xc, pcm + pcm_used, (int)chunk_bytes, 1) != 0)
				break;
			pcm_used += chunk_bytes / (u32)sizeof(s16);
		}
	}

	xmp_end_player(xc);
	xmp_release_module(xc);
	xmp_free_context(xc);

	if (!pcm || !pcm_used)
	{
		if (pcm)
			gf_free(pcm);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[XMPDec] Module rendered no audio\n"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	dst_pck = gf_filter_pck_new_alloc(ctx->opid, pcm_used * (u32)sizeof(s16), &output);
	if (!dst_pck)
	{
		gf_free(pcm);
		return GF_OUT_OF_MEM;
	}
	memcpy(output, pcm, pcm_used * sizeof(s16));
	gf_free(pcm);

	gf_filter_pck_set_cts(dst_pck, 0);
	gf_filter_pck_set_sap(dst_pck, GF_FILTER_SAP_1);
	gf_filter_pck_set_duration(dst_pck, pcm_used / XMPDEC_CHANNELS);
	gf_filter_pck_send(dst_pck);

	gf_filter_pid_set_eos(ctx->opid);
	return GF_EOS;
}

static void xmpdec_finalize(GF_Filter *filter)
{
}

static const GF_FilterCapability XMPDecCaps[] =
	{
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_FILE_EXT, "mod|xm|s3m|it|mtm|669|far|okt|ptm|stm|ult|amf|med|dbm|umx"),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_MIME, "audio/x-mod|audio/x-xm|audio/x-s3m|audio/x-it|audio/mod"),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_AUDIO),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_RAW),
};

GF_FilterRegister XMPDecoderRegister = {
	.name = "xmpdec",
	GF_FS_SET_DESCRIPTION("Tracker module decoder (MOD, XM, S3M, IT, ...)")
		GF_FS_SET_HELP("This filter renders tracker modules (Protracker MOD, Fast Tracker XM, Scream Tracker S3M, Impulse Tracker IT and about ninety other formats) to PCM using libxmp.")
			.private_size = sizeof(GF_XMPDecCtx),
	SETCAPS(XMPDecCaps),
	.configure_pid = xmpdec_configure_pid,
	.process = xmpdec_process,
	.process_event = xmpdec_process_event,
	.finalize = xmpdec_finalize,
};

const GF_FilterRegister *EMSCRIPTEN_KEEPALIVE xmpdec_register(GF_FilterSession *session)
{
	return &XMPDecoderRegister;
}

#include "filter_register.h"
__attribute__((constructor))
void register_xmpdec(void) {
    gf_filter_auto_register("xmpdec", xmpdec_register);
}
