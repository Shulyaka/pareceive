#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <pulse/pulseaudio.h>

#include "libavformat/avio.h"
#include "libavformat/avformat.h"
#include "libswresample/swresample.h"

static char *indevice = NULL;
static char *outdevice = NULL;

static pa_context *context = NULL;
static pa_stream *instream = NULL;
static pa_stream *outstream = NULL;
static pa_mainloop_api *mainloop_api = NULL;

#define SILENCE_CHECK_SIZE 12288

static void *inbuffer = NULL;
static size_t inbuffer_length = 0, inbuffer_index = 0;

static void *outbuffer = NULL;
static size_t outbuffer_length = 0, outbuffer_index = 0;

static pa_io_event* stdio_event = NULL;

uint32_t tlength = 0;

static int verbose = 1;

static pa_sample_spec in_sample_spec =
{
	.format = PA_SAMPLE_S16LE,
	.rate = 48000,
	.channels = 2
};
pa_sample_spec out_sample_spec;

const char* input_device_name = "stdin";
#define PA_MAX_BUF (1024*1024*96)
#define MAX_STDIN_READ 16384
size_t stdin_fragsize = 0;

static pa_stream_flags_t inflags = PA_STREAM_FIX_RATE | PA_STREAM_FIX_FORMAT | PA_STREAM_NO_REMIX_CHANNELS | PA_STREAM_NO_REMAP_CHANNELS | PA_STREAM_VARIABLE_RATE | PA_STREAM_DONT_MOVE | PA_STREAM_START_UNMUTED | PA_STREAM_PASSTHROUGH | PA_STREAM_ADJUST_LATENCY;
static pa_stream_flags_t outflags = PA_STREAM_ADJUST_LATENCY;

enum state {NOSIGNAL, PCM, IEC61937} state=NOSIGNAL;

static AVFormatContext *avformatcontext = NULL;
static AVCodecContext *avcodeccontext = NULL;
static AVFrame *avframe;
static AVPacket *pkt;

static SwrContext *swrcontext = NULL;
enum AVSampleFormat swroutformat = AV_SAMPLE_FMT_NONE;
size_t out_bytes_per_sample;

/* A shortcut for terminating the application */
static void quit(int ret)
{
	assert(mainloop_api);
	mainloop_api->quit(mainloop_api, ret);
}

/* Connection draining complete */
static void context_drain_complete(pa_context *c, void *userdata)
{
	pa_context_disconnect(c);
}

/* Start connection draining */
static void start_context_drain(pa_context *c)
{
	pa_operation *o = NULL;

	if (verbose)
		fprintf(stderr, "Draining connection to server.\n");
	if (!(o = pa_context_drain(c, context_drain_complete, NULL)))
		pa_context_disconnect(c);
	else
	{
		pa_operation_unref(o);
	}
}

/* Stream draining complete */
static void stream_drain_complete(pa_stream *s, int success, void *userdata)
{
	if (!success)
	{
		fprintf(stderr, "Failed to drain stream: %s\n", pa_strerror(pa_context_errno(context)));
		quit(1);
	}

	if (verbose)
		fprintf(stderr, "Playback stream drained.\n");

	pa_stream_disconnect(s);
	pa_stream_unref(s);

	if(s == outstream)
	{
		outstream = NULL;

		if (!instream && !stdio_event)
			start_context_drain(context);
	}
}

/* Start draining */
static void start_drain(pa_stream *s)
{
	pa_operation *o;

	if(verbose)
		fprintf(stderr, "Draining output stream\n");

	if(!s && !outstream)
	{
		fprintf(stderr, "The output stream has not been created\n");
		if (!instream && !stdio_event)
			start_context_drain(context);
		return;
	}
	if(pa_stream_get_state(s) == PA_STREAM_CREATING)
	{
		fprintf(stderr, "The output stream is still being created\n");
		return;
	}

	pa_stream_set_write_callback(s, NULL, NULL);

	if (!(o = pa_stream_drain(s, stream_drain_complete, NULL)))
	{
		fprintf(stderr, "pa_stream_drain(): %s\n", pa_strerror(pa_context_errno(context)));
		quit(1);
		return;
	}

	pa_operation_unref(o);
}

/* Updating stream timing info complete */
static void stream_timing_complete(pa_stream *s, int success, void *userdata)
{
	if (!success)
	{
		fprintf(stderr, "Failed to update timing info for the stream: %s\n", pa_strerror(pa_context_errno(context)));
		quit(1);
	}

	if(verbose)
	{
		pa_usec_t r_usec;
		int negative;
		int r;
		if(!(r=pa_stream_get_latency(s, &r_usec, &negative)))
			fprintf(stderr, "Stream latency %s%ld usec\n", (negative?"-":""), r_usec);
		else
			fprintf(stderr, "pa_stream_get_latency=%d\n", r);
	}
}

static void stream_set_buffer_attr_callback(pa_stream *s, int success, void *userdata)
{
	assert(s);

	const pa_buffer_attr *a;

	if(!success)
	{
		fprintf(stderr, "Failed to set buffer_attr: %s\n", pa_strerror(pa_context_errno(pa_stream_get_context(s))));
		quit(1);
		return;
	}

	if (!(a = pa_stream_get_buffer_attr(s)))
		fprintf(stderr, "pa_stream_get_buffer_attr() failed: %s\n", pa_strerror(pa_context_errno(pa_stream_get_context(s))));
#ifdef DEBUG_LATENCY
	else
	{
		if(s==instream)
			fprintf(stderr, "New inbuffer metrics: maxlength=%u, fragsize=%u\n", a->maxlength, a->fragsize);
		else
			fprintf(stderr, "New outbuffer metrics: maxlength=%u, tlength=%u, prebuf=%u, minreq=%u\n", a->maxlength, a->tlength, a->prebuf, a->minreq);
	}
#endif
}

/* This routine is called whenever the stream state changes */
static void stream_state_callback(pa_stream *s, void *userdata)
{
	assert(s);

	const pa_buffer_attr *a;
	char cmt[PA_CHANNEL_MAP_SNPRINT_MAX], sst[PA_SAMPLE_SPEC_SNPRINT_MAX];

	switch (pa_stream_get_state(s))
	{
		case PA_STREAM_CREATING:
			break;

		case PA_STREAM_TERMINATED:
			if(verbose)
				fprintf(stderr, "Stream terminated.\n");
			break;

		case PA_STREAM_READY:
			fprintf(stderr, "Stream successfully created.\n");

			if (!(a = pa_stream_get_buffer_attr(s)))
				fprintf(stderr, "pa_stream_get_buffer_attr() failed: %s\n", pa_strerror(pa_context_errno(pa_stream_get_context(s))));
#ifdef DEBUG_LATENCY
			else
			{
				if(s==instream)
					fprintf(stderr, "Buffer metrics: maxlength=%u, fragsize=%u\n", a->maxlength, a->fragsize);
				else
					fprintf(stderr, "Buffer metrics: maxlength=%u, tlength=%u, prebuf=%u, minreq=%u\n", a->maxlength, a->tlength, a->prebuf, a->minreq);
			}
#endif

			fprintf(stderr, "Using sample spec '%s', channel map '%s'.\n",
					pa_sample_spec_snprint(sst, sizeof(sst), pa_stream_get_sample_spec(s)),
					pa_channel_map_snprint(cmt, sizeof(cmt), pa_stream_get_channel_map(s)));

			fprintf(stderr, "Connected to device %s (%u, %ssuspended).\n",
					pa_stream_get_device_name(s),
					pa_stream_get_device_index(s),
					pa_stream_is_suspended(s) ? "" : "not ");

			if(s == instream)
			{
				in_sample_spec = *pa_stream_get_sample_spec(s);
				input_device_name = pa_stream_get_device_name(s);
			}

			break;

		case PA_STREAM_FAILED:
		default:
			fprintf(stderr, "Stream error: %s\n", pa_strerror(pa_context_errno(pa_stream_get_context(s))));
			quit(1);
	}
}

static void stream_suspended_callback(pa_stream *s, void *userdata)
{
	assert(s);

	if (verbose)
	{
		if (pa_stream_is_suspended(s))
			fprintf(stderr, "Stream device suspended.\n");
		else
			fprintf(stderr, "Stream device resumed.\n");
	}
}

static void stream_underflow_callback(pa_stream *s, void *userdata)
{
	assert(s);

	if (verbose)
		fprintf(stderr, "Stream underrun.\n");
}

static void stream_overflow_callback(pa_stream *s, void *userdata)
{
	assert(s);

	if (verbose)
		fprintf(stderr, "Stream overrun.\n");
}

static void stream_started_callback(pa_stream *s, void *userdata)
{
	assert(s);

	if(s == instream)
	{
		in_sample_spec = *pa_stream_get_sample_spec(s);
		input_device_name = pa_stream_get_device_name(s);
	}

	if (verbose)
		fprintf(stderr, "Stream started.\n");

	if (!instream && !stdio_event)
		start_drain(s);
	else
	{
		pa_operation *o;

		if (!(o = pa_stream_update_timing_info(s, stream_timing_complete, NULL)))
		{
			fprintf(stderr, "pa_stream_update_timing_info(): %s\n", pa_strerror(pa_context_errno(context)));
			quit(1);
			return;
		}

		pa_operation_unref(o);
	}
}

static void stream_moved_callback(pa_stream *s, void *userdata)
{
	assert(s);

	if(s == instream)
	{
		in_sample_spec = *pa_stream_get_sample_spec(s);
		input_device_name = pa_stream_get_device_name(s);
	}

	if (verbose)
		fprintf(stderr, "Stream moved to device %s (%u, %ssuspended).\n", pa_stream_get_device_name(s), pa_stream_get_device_index(s), pa_stream_is_suspended(s) ? "" : "not ");
}

static void stream_buffer_attr_callback(pa_stream *s, void *userdata)
{
	pa_operation *o;

	assert(s);

	if (verbose)
		fprintf(stderr, "Stream buffer attributes changed.\n");

	if (!(o = pa_stream_update_timing_info(s, stream_timing_complete, NULL)))
	{
		fprintf(stderr, "pa_stream_update_timing_info(): %s\n", pa_strerror(pa_context_errno(context)));
		quit(1);
		return;
	}

	pa_operation_unref(o);
}

static void stream_event_callback(pa_stream *s, const char *name, pa_proplist *pl, void *userdata)
{
	char *t;

	assert(s);
	assert(name);
	assert(pl);

	t = pa_proplist_to_string_sep(pl, ", ");
	fprintf(stderr, "Got event '%s', properties '%s'\n", name, t);
	pa_xfree(t);
}

/* Write some data to the stream */
static void do_stream_write(pa_stream *s, size_t length)
{
	size_t l;

	assert(s);

	size_t out_frame_size = pa_frame_size(&out_sample_spec);

	if (!outbuffer || !outbuffer_length || !length || !(l = ((length < outbuffer_length ? length : outbuffer_length) / out_frame_size) * out_frame_size))
		return;

	if (pa_stream_write(s, (uint8_t*) outbuffer + outbuffer_index, l, NULL, 0, PA_SEEK_RELATIVE) < 0)
	{
		fprintf(stderr, "pa_stream_write() failed: %s\n", pa_strerror(pa_context_errno(context)));
		quit(1);
		return;
	}

	outbuffer_length -= l;
	outbuffer_index += l;

	if (!outbuffer_length)
	{
		pa_xfree(outbuffer);
		outbuffer = NULL;
		outbuffer_index = outbuffer_length = 0;
	}
	else if(outbuffer_index > 4*1024)
	{
		memmove((uint8_t*) outbuffer, (uint8_t*) outbuffer + outbuffer_index, outbuffer_length);
		outbuffer = pa_xrealloc(outbuffer, outbuffer_length);
		outbuffer_index = 0;
	}
}

/* This is called whenever new data may be written to the stream */
static void stream_write_callback(pa_stream *s, size_t length, void *userdata)
{
	assert(s);

	if (!length || !outbuffer)
		return;

	if(tlength && outbuffer_length > tlength*2)
	{
#ifdef DEBUG_LATENCY
		printf("Outbuffer is too long (%zu > %u*2). Flushing it to reduce latency. Sorry for that!\n", outbuffer_length, tlength);
#endif
		outbuffer_index += outbuffer_length - tlength;
		outbuffer_length = tlength;
#ifdef DEBUG_LATENCY
		printf("outbuffer_length = %zu\n", outbuffer_length);
#endif
	}

	do_stream_write(s, length);
}

/* Maps FFMpeg sample format to PA sample format */
enum pa_sample_format map_sample_format(enum AVSampleFormat format)
{
	const int isbe = (*(uint16_t *)"\0\xff" < 0x100) ? 1 : 0;
	switch(av_get_packed_sample_fmt(format))
	{
		case AV_SAMPLE_FMT_U8:
			return PA_SAMPLE_U8;
		case AV_SAMPLE_FMT_S16:
			return PA_SAMPLE_S16LE + isbe;
		case AV_SAMPLE_FMT_S32:
			return PA_SAMPLE_S32LE + isbe;
		case AV_SAMPLE_FMT_FLT:
			return PA_SAMPLE_FLOAT32LE + isbe;
		case AV_SAMPLE_FMT_DBL:
			fprintf(stderr, "PulseAudio does not support double float sample formats\n");
		default:
			fprintf(stderr, "Unexpected sample format %s\n", av_get_sample_fmt_name(format));
	}
	return PA_SAMPLE_INVALID;
}

void map_channel_layout(pa_channel_map* channel_map, uint64_t channel_layout)
{
	pa_channel_map_init(channel_map);
	channel_map->channels = av_get_channel_layout_nb_channels(channel_layout);

	int i;
	for(i = 0; i < channel_map->channels; i++)
	{
		switch(av_channel_layout_extract_channel(channel_layout, i))
		{
			case AV_CH_FRONT_LEFT:
				channel_map->map[i] = PA_CHANNEL_POSITION_FRONT_LEFT;
				break;
			case AV_CH_FRONT_RIGHT:
				channel_map->map[i] = PA_CHANNEL_POSITION_FRONT_RIGHT;
				break;
			case AV_CH_FRONT_CENTER:
				channel_map->map[i] = PA_CHANNEL_POSITION_FRONT_CENTER;
				break;
			case AV_CH_LOW_FREQUENCY:
				channel_map->map[i] = PA_CHANNEL_POSITION_LFE;
				break;
			case AV_CH_BACK_LEFT:
				channel_map->map[i] = PA_CHANNEL_POSITION_REAR_LEFT;
				break;
			case AV_CH_BACK_RIGHT:
				channel_map->map[i] = PA_CHANNEL_POSITION_REAR_RIGHT;
				break;
			case AV_CH_FRONT_LEFT_OF_CENTER:
				channel_map->map[i] = PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER;
				break;
			case AV_CH_FRONT_RIGHT_OF_CENTER:
				channel_map->map[i] = PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER;
				break;
			case AV_CH_BACK_CENTER:
				channel_map->map[i] = PA_CHANNEL_POSITION_REAR_CENTER;
				break;
			case AV_CH_SIDE_LEFT:
				channel_map->map[i] = PA_CHANNEL_POSITION_SIDE_LEFT;
				break;
			case AV_CH_SIDE_RIGHT:
				channel_map->map[i] = PA_CHANNEL_POSITION_SIDE_RIGHT;
				break;
			case AV_CH_TOP_CENTER:
				channel_map->map[i] = PA_CHANNEL_POSITION_TOP_CENTER;
				break;
			case AV_CH_TOP_FRONT_LEFT:
				channel_map->map[i] = PA_CHANNEL_POSITION_TOP_FRONT_LEFT;
				break;
			case AV_CH_TOP_FRONT_CENTER:
				channel_map->map[i] = PA_CHANNEL_POSITION_TOP_FRONT_CENTER;
				break;
			case AV_CH_TOP_FRONT_RIGHT:
				channel_map->map[i] = PA_CHANNEL_POSITION_TOP_FRONT_RIGHT;
				break;
			case AV_CH_TOP_BACK_LEFT:
				channel_map->map[i] = PA_CHANNEL_POSITION_TOP_REAR_LEFT;
				break;
			case AV_CH_TOP_BACK_CENTER:
				channel_map->map[i] = PA_CHANNEL_POSITION_TOP_REAR_CENTER;
				break;
			case AV_CH_TOP_BACK_RIGHT:
				channel_map->map[i] = PA_CHANNEL_POSITION_TOP_REAR_RIGHT;
				break;
			case AV_CH_STEREO_LEFT: //Stereo downmix
				channel_map->map[i] = PA_CHANNEL_POSITION_FRONT_LEFT;
				break;
			case AV_CH_STEREO_RIGHT: //See AV_CH_STEREO_LEFT
				channel_map->map[i] = PA_CHANNEL_POSITION_FRONT_RIGHT;
				break;
			case AV_CH_WIDE_LEFT: // PA does not have wide speakers, so map them to side instead. If both of your setup and source have both wide and side speakers, you may want to change this to PA_CHANNEL_POSITION_AUX*. And if this is the case, please also send me a postcard.
				channel_map->map[i] = PA_CHANNEL_POSITION_SIDE_LEFT;
				break;
			case AV_CH_WIDE_RIGHT:
				channel_map->map[i] = PA_CHANNEL_POSITION_SIDE_RIGHT;
				break;
			case AV_CH_SURROUND_DIRECT_LEFT:
				channel_map->map[i] = PA_CHANNEL_POSITION_SIDE_LEFT;
				break;
			case AV_CH_SURROUND_DIRECT_RIGHT:
				channel_map->map[i] = PA_CHANNEL_POSITION_SIDE_RIGHT;
				break;
			case AV_CH_LOW_FREQUENCY_2:
				channel_map->map[i] = PA_CHANNEL_POSITION_LFE;
				break;

			default:
				fprintf(stderr, "Unexpected channel position %s\n", av_get_channel_description(av_channel_layout_extract_channel(channel_layout, i)));
				channel_map->map[i] = PA_CHANNEL_POSITION_INVALID;
		}
	}
}

void open_output_stream(void)
{
	pa_channel_map out_channel_map;
	pa_buffer_attr buffer_attr;

	assert(context);
	assert(!outstream);

	if(state == IEC61937)
	{
		out_sample_spec.format = map_sample_format(swroutformat);
		out_sample_spec.rate = avcodeccontext->sample_rate;
		out_sample_spec.channels = avcodeccontext->channels;
		map_channel_layout(&out_channel_map, avcodeccontext->channel_layout);
		tlength = avformatcontext->pb->buffer_size / 4 * out_bytes_per_sample * 2;
	}
	else
	{
		out_sample_spec.format = in_sample_spec.format;
		out_sample_spec.rate = in_sample_spec.rate;
		out_sample_spec.channels = in_sample_spec.channels;
		if (instream)
		{
			memcpy(&out_channel_map, pa_stream_get_channel_map(instream), sizeof(pa_channel_map));
			tlength = pa_stream_get_buffer_attr(instream)->fragsize;
		}
		else
		{
			pa_channel_map_init(&out_channel_map);
			out_channel_map.channels = 2;
			out_channel_map.map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
			out_channel_map.map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
			tlength = 16384;
		}
	}

	buffer_attr.fragsize = (uint32_t) -1;
	buffer_attr.maxlength = (uint32_t) -1;
	buffer_attr.minreq = (uint32_t) -1;
	buffer_attr.prebuf = (uint32_t) -1;
	buffer_attr.tlength = tlength;

	if (!(outstream = pa_stream_new(context, "pareceive output stream", &out_sample_spec, &out_channel_map)))
	{
		fprintf(stderr, "pa_stream_new() failed: %s\n", pa_strerror(pa_context_errno(context)));
		quit(1);
		return;
	}

	pa_stream_set_state_callback(outstream, stream_state_callback, NULL);
	pa_stream_set_write_callback(outstream, stream_write_callback, NULL);
	pa_stream_set_suspended_callback(outstream, stream_suspended_callback, NULL);
	pa_stream_set_moved_callback(outstream, stream_moved_callback, NULL);
	pa_stream_set_underflow_callback(outstream, stream_underflow_callback, NULL);
	pa_stream_set_overflow_callback(outstream, stream_overflow_callback, NULL);
	pa_stream_set_started_callback(outstream, stream_started_callback, NULL);
	pa_stream_set_event_callback(outstream, stream_event_callback, NULL);
	pa_stream_set_buffer_attr_callback(outstream, stream_buffer_attr_callback, NULL);

	if (pa_stream_connect_playback(outstream, outdevice, &buffer_attr, outflags, NULL, NULL) < 0)
	{
		fprintf(stderr, "pa_stream_connect_playback() failed: %s\n", pa_strerror(pa_context_errno(context)));
		quit(1);
	}

	return;
}

static int readFunction(void* opaque, uint8_t* buf, int buf_size)
{
	size_t l = buf_size;

	if (!inbuffer)
		return AVERROR_EOF;

	if (inbuffer_length < l)
		return AVERROR_EOF;

	memcpy(buf, (uint8_t*) inbuffer + inbuffer_index, l);

	inbuffer_length -= (uint32_t) l;
	inbuffer_index += (uint32_t) l;

	if (!inbuffer_length)
	{
		pa_xfree(inbuffer);
		inbuffer = NULL;
		inbuffer_length = inbuffer_index = 0;
	}
	else if(inbuffer_index > 4*1024)
	{
		memmove((uint8_t*) inbuffer, (uint8_t*) inbuffer + inbuffer_index, inbuffer_length);
		inbuffer = pa_xrealloc(inbuffer, inbuffer_length);
		inbuffer_index = 0;
	}

	return l;
}

void print_averror(const char *str, int err)
{
	char errbuf[128];
	const char *errbuf_ptr = errbuf;

	if (av_strerror(err, errbuf, sizeof(errbuf)) < 0)
		errbuf_ptr = strerror(AVUNERROR(err));

	fprintf(stderr, "%s: %s\n", str, errbuf_ptr);
}

void set_instream_fragsize(uint32_t fragsize)
{
	if(instream)
	{
		pa_buffer_attr buffer_attr;
		memcpy(&buffer_attr, pa_stream_get_buffer_attr(instream), sizeof(pa_buffer_attr));
		buffer_attr.fragsize = fragsize;
		pa_operation_unref(pa_stream_set_buffer_attr(instream, &buffer_attr, stream_set_buffer_attr_callback, NULL));
	}
	else
	{
		stdin_fragsize = fragsize == (uint32_t) -1 ? MAX_STDIN_READ : fragsize;
		if(stdin_fragsize > MAX_STDIN_READ)
			stdin_fragsize = MAX_STDIN_READ;
	}
}

void set_state(enum state newstate)
{
	enum state oldstate = state;

	if(oldstate == newstate)
		return;

	if (outstream)
	{
		if(pa_stream_get_state(outstream) == PA_STREAM_READY)
			stream_write_callback(outstream, pa_stream_writable_size(outstream), NULL);
		pa_stream_set_write_callback(outstream, NULL, NULL);
		start_drain(outstream);
		outstream = NULL;
		out_sample_spec = in_sample_spec;
		fprintf(stderr, "Closed output stream\n");
	}

	if (outbuffer)
	{
		pa_xfree(outbuffer);
		outbuffer = NULL;
		outbuffer_index = outbuffer_length = 0;
	}

	switch(oldstate)
	{
		case NOSIGNAL:
			break;
		case PCM:
			break;
		case IEC61937:
			if(avformatcontext)
			{
				av_free(avformatcontext->pb->buffer);
				av_free(avformatcontext->pb);
				avformat_close_input(&avformatcontext);
				avcodec_free_context(&avcodeccontext);
				swr_free(&swrcontext);
				avformatcontext = NULL;
			}

			if(inbuffer)
			{
					if(newstate == PCM)
					{
				   		outbuffer = pa_xrealloc(outbuffer, outbuffer_index + outbuffer_length + inbuffer_length);
				   		memcpy((uint8_t*) outbuffer + outbuffer_index + outbuffer_length, (uint8_t*) inbuffer + inbuffer_index, inbuffer_length);
				   		outbuffer_length += inbuffer_length;
					}

		  			pa_xfree(inbuffer);
	  				inbuffer = NULL;
	  				inbuffer_index = inbuffer_length = 0;
			}
			break;
	}

	state = newstate;

	switch(newstate)
	{
		case NOSIGNAL:
			set_instream_fragsize(SILENCE_CHECK_SIZE);
			break;
		case PCM:
			fprintf(stderr, "Playing PCM\n");
			open_output_stream();
			break;
		case IEC61937:
			break;
	}
}

#define SPDIF_MAX_OFFSET 16384*10

// returns 0 if data is too small for examination, 1 if validation fails and a block size (aka offset) if validation is successful
size_t iec61937_validate(const uint8_t* data, size_t length)
{
	static const uint32_t magic = 0x4E1FF872;
	size_t firstmagic, secondmagic;

	for(firstmagic = 0; firstmagic < length-sizeof(uint32_t)+1; firstmagic++)
		if(*(uint32_t*)(data+firstmagic) == magic)
			break;

	if(firstmagic == length-sizeof(uint32_t)+1)
	{
		if(length < SPDIF_MAX_OFFSET)
			return 0;
		else
			return 1;
	}

	for(secondmagic = firstmagic + ((*(uint16_t*)(data+firstmagic+6))>>3) + 8; secondmagic < length-sizeof(uint32_t)+1; secondmagic++)
		if(*(uint32_t*)(data+secondmagic) == magic)
			break;

	if(secondmagic == length-sizeof(uint32_t)+1)
	{
		if(length < SPDIF_MAX_OFFSET * 2)
			return 0;
		else
			return 1;
	}

	secondmagic -= firstmagic;

	if(secondmagic > SPDIF_MAX_OFFSET)
		return 1;

	if(length < secondmagic * 2)
		return 0;

	return secondmagic;
}

//returns 1 if magic found, 0 if not
int iec61937_suspect(const uint8_t* data, size_t length)
{
	static const uint32_t magic = 0x4E1FF872;
	size_t firstmagic;

	for(firstmagic = 0; firstmagic < length-sizeof(uint32_t)+1; firstmagic++)
		if(*(uint32_t*)(data+firstmagic) == magic)
			break;

	return (firstmagic == length-sizeof(uint32_t)+1) ? 0 : 1;
}

/* Process new data */
static void decode_data(const void *data, size_t length, void *userdata)
{
	int i=0;
	static size_t prevextralength = 0;

	assert(data);
	assert(length > 0);

	if(state==NOSIGNAL)
	{
		for(i=0; i<length/sizeof(uint32_t); i++)
			if(((uint32_t*) data)[i])
				break;
		if(i<length/sizeof(uint32_t))
			set_state(IEC61937);
	}
	else
	{
		static pa_usec_t silence=0;
		for(i=0; i<length/sizeof(uint32_t); i++)
			if(((uint32_t*) data)[i])
				break;
		if(i<length/sizeof(uint32_t))
		{
			silence = 0;
		}
		else
		{
			silence+=pa_bytes_to_usec(length, &in_sample_spec);
			if(silence > 100000)
			{
				fprintf(stderr, "Playing silence\n");
				set_state(NOSIGNAL);
				silence=0;
				return;
			}
		}
		i=0;
	}

	if(state==IEC61937)
	{
   		inbuffer = pa_xrealloc(inbuffer, inbuffer_index + inbuffer_length + length - i*sizeof(uint32_t));
   		memcpy((uint8_t*) inbuffer + inbuffer_index + inbuffer_length, (uint32_t*) data + i, length - i*sizeof(uint32_t));
   		inbuffer_length += length - i*sizeof(uint32_t);

		if(!avformatcontext)
		{
			size_t block_size = iec61937_validate((uint8_t*) inbuffer + inbuffer_index, inbuffer_length);
			if (block_size == 0)
			{
#ifdef DEBUG_LATENCY
				fprintf(stderr, "Buffer is too small, waiting for more data\n");
#endif
				return;
			}
			else if(block_size == 1)
			{
				fprintf(stderr, "IEC61937 validation failed\n");
				set_state(PCM);
				return;
			}

#ifdef DEBUG_LATENCY
			fprintf(stderr, "block_size=%zu\n", block_size);
#endif
	
			if(inbuffer_length < block_size * 3)
			{
#ifdef DEBUG_LATENCY
				fprintf(stderr, "Buffer is too small, waiting for more data\n");
#endif
				return;
			}

			prevextralength = inbuffer_length;

			avformatcontext = avformat_alloc_context();
			avformatcontext->pb = avio_alloc_context(av_malloc(block_size), block_size, 0, NULL, readFunction, NULL, NULL);
			if( (i = avformat_open_input(&avformatcontext, input_device_name, av_find_input_format("spdif"), NULL)) < 0)
			{
				print_averror("avformat_open_input", i);
				fprintf(stderr, "Playing silence\n");
				set_state(NOSIGNAL);
				return;
			}

			if( (i=avformat_find_stream_info(avformatcontext, NULL)) < 0)
			{
				print_averror("avformat_find_stream_info", i);
				fprintf(stderr, "Playing silence\n");
				set_state(NOSIGNAL);
				return;
			}

			//av_dump_format(avformatcontext, 0, input_device_name, 0);

			AVCodec *dec = NULL;
			int stream_index = av_find_best_stream(avformatcontext, AVMEDIA_TYPE_AUDIO, -1, -1, &dec, 0);

			if(stream_index < 0)
			{
				print_averror("av_find_best_stream", stream_index);
				fprintf(stderr, "Playing silence\n");
				set_state(NOSIGNAL);
				return;
			}

			avcodeccontext = avcodec_alloc_context3(dec); 	

			avcodec_parameters_to_context(avcodeccontext,avformatcontext->streams[stream_index]->codecpar);

			if ((i = avcodec_open2(avcodeccontext, dec, NULL)) < 0)
			{
				print_averror("avcodec_open2", i);
				fprintf(stderr, "Playing silence\n");
				set_state(NOSIGNAL);
				return;
			}

			swroutformat = av_get_packed_sample_fmt(avcodeccontext->sample_fmt);
			if(swroutformat == AV_SAMPLE_FMT_DBL)
				swroutformat = AV_SAMPLE_FMT_FLT;
			swrcontext = swr_alloc_set_opts(swrcontext,
											avcodeccontext->channel_layout,
											swroutformat,
											avcodeccontext->sample_rate,
											avcodeccontext->channel_layout,
											avcodeccontext->sample_fmt,
											avcodeccontext->sample_rate,
											0, NULL);
			swr_init(swrcontext);

			out_bytes_per_sample = av_get_bytes_per_sample(swroutformat) * (size_t)avcodeccontext->channels;

			pkt->data = NULL;
			pkt->size = 0;

			set_instream_fragsize(block_size * 2);

			open_output_stream();

			char buf[256];
			avcodec_string(buf, sizeof(buf), avcodeccontext, 0);
			fprintf(stderr, "Playing IEC61937: %s\n", buf);
		}
	}

	if(state==IEC61937 && inbuffer_length > avformatcontext->pb->buffer_size * 2)
	{
		int pcount=0, fcount=0;
		while ( inbuffer_length > avformatcontext->pb->buffer_size * 2 && (i = av_read_frame(avformatcontext, pkt)) >= 0)
		{
			pcount++;
			int ret = avcodec_send_packet(avcodeccontext, pkt);
			if(ret<0)
			{
				print_averror("avcodec_send_packet", ret);
				fprintf(stderr, "Playing silence\n");
				set_state(NOSIGNAL);
				return;
			}
			while ( (ret = avcodec_receive_frame(avcodeccontext, avframe)) >=0)
			{
				size_t addlen = swr_get_out_samples(swrcontext, avframe->nb_samples) * out_bytes_per_sample;
   				outbuffer = pa_xrealloc(outbuffer, outbuffer_index + outbuffer_length + addlen);
				uint8_t *outptr = (uint8_t*) outbuffer + outbuffer_length;
				outbuffer_length += swr_convert(swrcontext, &outptr, addlen, (const uint8_t **) avframe->extended_data, avframe->nb_samples) * out_bytes_per_sample;

				fcount++;
				av_frame_unref(avframe);
			}
			if(ret != AVERROR(EAGAIN))
			{
				print_averror("avcodec_receive_frame", ret);
				fprintf(stderr, "Playing silence\n");
				set_state(NOSIGNAL);
				return;
			}
		}

		static int total_missed_frames=0;

		if((!pcount || i != 0) && i != AVERROR_EOF)
		{
			print_averror("av_read_frame", i);
			total_missed_frames = 0;
			prevextralength = 0;
			fprintf(stderr, "Playing silence\n");
			set_state(NOSIGNAL);
			return;
		}

		int missed_frames = (length+prevextralength) / avformatcontext->pb->buffer_size;
		prevextralength += length - (unsigned long)missed_frames * avformatcontext->pb->buffer_size;
		missed_frames -= fcount;
		if (missed_frames < 0)
		{
			missed_frames = 0;
			prevextralength = 0;
		}

		total_missed_frames += missed_frames;
		if(!missed_frames)
			total_missed_frames = 0;

		if(total_missed_frames > 32)
		{
			fprintf(stderr, "Too many missed frames\n");
			total_missed_frames = 0;
			prevextralength = 0;
			fprintf(stderr, "Playing silence\n");
			set_state(NOSIGNAL);
			return;
		}
	}
#ifdef DEBUG_LATENCY
	else if(state==IEC61937)
	{
		printf("Inbuffer %zu is too low, skipping decode step\n", inbuffer_length);
	}
#endif

	if(state==PCM)
	{
		if(iec61937_suspect(data, length))
		{
			printf("Suspected IEC61937\n");
			set_state(IEC61937);
			return;
		}

   		outbuffer = pa_xrealloc(outbuffer, outbuffer_index + outbuffer_length + length);
   		memcpy((uint8_t*) outbuffer + outbuffer_index + outbuffer_length, data, length);
   		outbuffer_length += length;
	}

	if(outstream && pa_stream_get_state(outstream) == PA_STREAM_READY)
		stream_write_callback(outstream, pa_stream_writable_size(outstream), NULL);
}

/* This is called whenever new data may is available */
static void stream_read_callback(pa_stream *s, size_t length, void *userdata)
{
	const void *data;

	assert(s);
	assert(length > 0);

	if (pa_stream_peek(s, &data, &length) < 0)
	{
		fprintf(stderr, "pa_stream_peek() failed: %s\n", pa_strerror(pa_context_errno(context)));
		quit(1);
		return;
	}

	decode_data(data, length, userdata);

	pa_stream_drop(s);
}

/* New data on STDIN **/
static void stdin_callback(pa_mainloop_api *a, pa_io_event *e, int fd, pa_io_event_flags_t f, void *userdata)
{
	uint8_t buf[MAX_STDIN_READ];
	ssize_t r=1;

	assert(a == mainloop_api);
	assert(e);
	assert(stdio_event == e);

	if(!stdin_fragsize)
		return;

	while(outbuffer_index + outbuffer_length + stdin_fragsize < PA_MAX_BUF && (r = read(fd, &buf, stdin_fragsize)) > 0)
	{
		decode_data(buf, r, userdata);
	}

	if (r == 0)
	{
		if (verbose)
			fprintf(stderr, "Got EOF.\n");

		mainloop_api->io_free(stdio_event);
		stdio_event = NULL;
		start_drain(outstream);
		return;
	}
	else if (r < 0 && errno != EWOULDBLOCK)
	{
		fprintf(stderr, "read() failed: %s\n", strerror(errno));
		quit(1);
	}
}

/* This is called whenever the context status changes */
static void context_state_callback(pa_context *c, void *userdata)
{
	assert(c);

	switch (pa_context_get_state(c))
	{
		case PA_CONTEXT_CONNECTING:
		case PA_CONTEXT_AUTHORIZING:
		case PA_CONTEXT_SETTING_NAME:
			break;

		case PA_CONTEXT_READY:
		{
			fprintf(stderr, "Connection established.\n");

			if (stdio_event)
			{
				stdin_fragsize = MAX_STDIN_READ;
				break;
			}

			int r;
			pa_buffer_attr buffer_attr;

			assert(c);
			assert(!instream);

			if (!(instream = pa_stream_new(c, "pareceive input stream", &in_sample_spec, NULL)))
			{
				fprintf(stderr, "pa_stream_new() failed: %s\n", pa_strerror(pa_context_errno(c)));
				goto fail;
			}

			pa_stream_set_state_callback(instream, stream_state_callback, NULL);
			pa_stream_set_read_callback(instream, stream_read_callback, NULL);
			pa_stream_set_suspended_callback(instream, stream_suspended_callback, NULL);
			pa_stream_set_moved_callback(instream, stream_moved_callback, NULL);
			pa_stream_set_underflow_callback(instream, stream_underflow_callback, NULL);
			pa_stream_set_overflow_callback(instream, stream_overflow_callback, NULL);
			pa_stream_set_started_callback(instream, stream_started_callback, NULL);
			pa_stream_set_event_callback(instream, stream_event_callback, NULL);
			pa_stream_set_buffer_attr_callback(instream, stream_buffer_attr_callback, NULL);

			buffer_attr.fragsize = SILENCE_CHECK_SIZE;
			buffer_attr.maxlength = (uint32_t) -1;
			buffer_attr.minreq = (uint32_t) -1;
			buffer_attr.prebuf = (uint32_t) -1;
			buffer_attr.tlength = (uint32_t) -1;

			if ((r = pa_stream_connect_record(instream, indevice, &buffer_attr, inflags)) < 0)
			{
				fprintf(stderr, "pa_stream_connect_record() failed: %s\n", pa_strerror(pa_context_errno(c)));
				goto fail;
			}

			break;
		}

		case PA_CONTEXT_TERMINATED:
			quit(0);
			break;

		case PA_CONTEXT_FAILED:
		default:
			fprintf(stderr, "Connection failure: %s\n", pa_strerror(pa_context_errno(c)));
			goto fail;
	}

	return;

fail:
	quit(1);

}

/* UNIX signal to quit recieved */
static void exit_signal_callback(pa_mainloop_api *m, pa_signal_event *e, int sig, void *userdata)
{
	fprintf(stderr, "Got signal, exiting.\n");
	quit(0);
}

static void sigusr1_signal_callback(pa_mainloop_api *m, pa_signal_event *e, int sig, void *userdata)
{
	return;
}

int main(int argc, char *argv[])
{
	pa_mainloop* m = NULL;
	int ret = 1, r;
	char *server = NULL;
	unsigned long type = 0;

	if(argc > 1 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")))
	{
		printf("Usage: %s [indevice [outdevice [server]]]\nTo use stdin as input, use - as indevice\n", argv[0]);
		return 0;
	}

	if(argc > 1)
		indevice = argv[1];

	if(argc > 2)
		outdevice = argv[2];

	if(argc > 3)
		server = argv[3];

	avframe = av_frame_alloc();
	pkt = av_packet_alloc();

	/* Set up a new main loop */
	if (!(m = pa_mainloop_new()))
	{
		fprintf(stderr, "pa_mainloop_new() failed.\n");
		goto quit;
	}

	mainloop_api = pa_mainloop_get_api(m);

	r = pa_signal_init(mainloop_api);
	assert(r == 0);
	pa_signal_new(SIGINT, exit_signal_callback, NULL);
	pa_signal_new(SIGTERM, exit_signal_callback, NULL);
#ifdef SIGUSR1
	pa_signal_new(SIGUSR1, sigusr1_signal_callback, NULL);
#endif
#ifdef SIGPIPE
	signal(SIGPIPE, SIG_IGN);
#endif

	if (indevice && !strcmp(indevice, "-"))
	{
		if(fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK) < 0)
		{
			fprintf(stderr, "fcntl: %s\n", strerror(errno));
			goto quit;
		}
		if (!(stdio_event = mainloop_api->io_new(mainloop_api, STDIN_FILENO, PA_IO_EVENT_INPUT, stdin_callback, &type)))
		{
			fprintf(stderr, "io_new() failed.\n");
			goto quit;
		}
	}

	/* Create a new connection context */
	if (!(context = pa_context_new(mainloop_api, "pareceive")))
	{
		fprintf(stderr, "pa_context_new() failed.\n");
		goto quit;
	}

	pa_context_set_state_callback(context, context_state_callback, NULL);

	/* Connect the context */
	if (pa_context_connect(context, server, PA_CONTEXT_NOFLAGS, NULL) < 0)
	{
		fprintf(stderr, "pa_context_connect() failed: %s\n", pa_strerror(pa_context_errno(context)));
		goto quit;
	}

	/* Run the main loop */
	if (pa_mainloop_run(m, &ret) < 0)
	{
		fprintf(stderr, "pa_mainloop_run() failed.\n");
		goto quit;
	}

quit:
	av_packet_free(&pkt);
	av_frame_free(&avframe);

	set_state(NOSIGNAL);

	if (instream)
	{
		pa_stream_disconnect(instream);
		pa_stream_unref(instream);
	}

	if (context)
	{
		pa_context_disconnect(context);
		pa_context_unref(context);
	}

	if (stdio_event)
	{
		assert(mainloop_api);
		mainloop_api->io_free(stdio_event);
	}

	if (m)
	{
		pa_signal_done();
		pa_mainloop_free(m);
	}

	pa_xfree(outbuffer);

	return ret;
}
