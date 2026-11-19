#include "yt-remux.h"

#include <obs-module.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>

struct remux_input {
	AVFormatContext *fmt;
	int in_index;
	int out_index;
	AVPacket *pkt;
	bool has_pkt;
	bool eof;
};

static bool open_input(struct remux_input *in, const char *path, enum AVMediaType type, struct dstr *error)
{
	int ret = avformat_open_input(&in->fmt, path, NULL, NULL);
	if (ret < 0) {
		dstr_printf(error, "open '%s': %s", path, av_err2str(ret));
		return false;
	}
	ret = avformat_find_stream_info(in->fmt, NULL);
	if (ret < 0) {
		dstr_printf(error, "stream info '%s': %s", path, av_err2str(ret));
		return false;
	}
	in->in_index = av_find_best_stream(in->fmt, type, -1, -1, NULL, 0);
	if (in->in_index < 0) {
		dstr_printf(error, "no %s stream in '%s'", type == AVMEDIA_TYPE_VIDEO ? "video" : "audio", path);
		return false;
	}
	in->pkt = av_packet_alloc();
	return true;
}

static void close_input(struct remux_input *in)
{
	av_packet_free(&in->pkt);
	avformat_close_input(&in->fmt);
}

/* read the next packet of the selected stream into in->pkt */
static void fill(struct remux_input *in)
{
	while (!in->has_pkt && !in->eof) {
		int ret = av_read_frame(in->fmt, in->pkt);
		if (ret < 0) {
			in->eof = true;
			break;
		}
		if (in->pkt->stream_index == in->in_index)
			in->has_pkt = true;
		else
			av_packet_unref(in->pkt);
	}
}

static int64_t pkt_ts(const AVPacket *pkt)
{
	return pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
}

bool yt_remux(const char *video_path, const char *audio_path, const char *out_path, volatile bool *cancel,
	      struct dstr *error)
{
	struct remux_input in[2] = {0};
	int n_in = 2;
	AVFormatContext *out = NULL;
	bool success = false;
	int ret;

	if (!open_input(&in[0], video_path, AVMEDIA_TYPE_VIDEO, error))
		goto done;
	if (!open_input(&in[1], audio_path ? audio_path : video_path, AVMEDIA_TYPE_AUDIO, error)) {
		/* video without audio is still playable */
		if (in[1].fmt && in[1].in_index < 0) {
			n_in = 1;
			dstr_free(error);
		} else {
			goto done;
		}
	}

	ret = avformat_alloc_output_context2(&out, NULL, "matroska", out_path);
	if (ret < 0 || !out) {
		dstr_printf(error, "alloc output: %s", av_err2str(ret));
		goto done;
	}

	for (int i = 0; i < n_in; i++) {
		AVStream *src = in[i].fmt->streams[in[i].in_index];
		AVStream *dst = avformat_new_stream(out, NULL);
		if (!dst) {
			dstr_copy(error, "new stream failed");
			goto done;
		}
		ret = avcodec_parameters_copy(dst->codecpar, src->codecpar);
		if (ret < 0) {
			dstr_printf(error, "copy codec params: %s", av_err2str(ret));
			goto done;
		}
		dst->codecpar->codec_tag = 0;
		dst->time_base = src->time_base;
		in[i].out_index = dst->index;
	}

	ret = avio_open(&out->pb, out_path, AVIO_FLAG_WRITE);
	if (ret < 0) {
		dstr_printf(error, "open output '%s': %s", out_path, av_err2str(ret));
		goto done;
	}
	ret = avformat_write_header(out, NULL);
	if (ret < 0) {
		dstr_printf(error, "write header: %s", av_err2str(ret));
		goto done;
	}

	for (;;) {
		if (cancel && *cancel) {
			dstr_copy(error, "cancelled");
			goto done;
		}

		for (int i = 0; i < n_in; i++)
			fill(&in[i]);

		/* pick the input whose next packet has the earliest timestamp */
		int pick = -1;
		for (int i = 0; i < n_in; i++) {
			if (!in[i].has_pkt)
				continue;
			if (pick < 0) {
				pick = i;
				continue;
			}
			AVRational tb_i = in[i].fmt->streams[in[i].in_index]->time_base;
			AVRational tb_p = in[pick].fmt->streams[in[pick].in_index]->time_base;
			if (av_compare_ts(pkt_ts(in[i].pkt), tb_i, pkt_ts(in[pick].pkt), tb_p) < 0)
				pick = i;
		}
		if (pick < 0)
			break; /* all inputs exhausted */

		struct remux_input *p = &in[pick];
		AVStream *src = p->fmt->streams[p->in_index];
		AVStream *dst = out->streams[p->out_index];

		av_packet_rescale_ts(p->pkt, src->time_base, dst->time_base);
		p->pkt->stream_index = p->out_index;
		p->pkt->pos = -1;

		ret = av_interleaved_write_frame(out, p->pkt);
		p->has_pkt = false; /* write_frame takes ownership / unrefs */
		if (ret < 0) {
			dstr_printf(error, "write packet: %s", av_err2str(ret));
			goto done;
		}
	}

	ret = av_write_trailer(out);
	if (ret < 0) {
		dstr_printf(error, "write trailer: %s", av_err2str(ret));
		goto done;
	}
	success = true;

done:
	if (out) {
		if (out->pb)
			avio_closep(&out->pb);
		avformat_free_context(out);
	}
	for (int i = 0; i < 2; i++)
		close_input(&in[i]);
	return success;
}
