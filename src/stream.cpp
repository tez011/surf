#include "ffmpeg.h"
#include "http.h"
#include <fstream>
#include <regex>
#include <thread>
constexpr int OUTPUT_SAMPLE_RATE = 44100;

typedef struct {
	http_server::session* sn;
	std::FILE* cache_fp;
} tc_out_mux_t;

void surf_server::api_v1_stream(http_server::session* sn, const std::string& track_uuid, int quality)
{
	if (quality < 0 || quality > 9)
		return sn->serve_error(400, "Unexpected value for parameter 'q' (should be an integer from 0-9)\r\n");

	auto cached = mdb.get_cached_transcode(track_uuid, quality);
	if (cached.second)
		return api_v1_stream_cached(sn, cached.first);

	auto track_path = mdb.get_track_path(track_uuid);
	if (track_path)
		std::thread(&surf_server::api_v1_transcode, this, sn, track_uuid, track_path.value(), quality).detach();
	else
		sn->serve_error(404, "Not Found\r\n");
}

void surf_server::api_v1_stream_cached(http_server::session* sn, const std::string& path_to_tc)
{
	// Handle a range request, if we got one.
	auto range_hdr = sn->request_header("range");
	std::optional<std::pair<int, int>> range = std::nullopt;
	if (range_hdr) {
		std::smatch rsm;
		if (std::regex_match(range_hdr.value(), rsm, std::regex(R"(bytes=(\d*)-(\d*))"))) {
			range.emplace(strtoul(rsm[1].str().c_str(), nullptr, 10), strtoul(rsm[2].str().c_str(), nullptr, 10));
		} else {
			return sn->serve_error(400, "malformed range request\r\n");
		}
	}

	std::ifstream tcf(path_to_tc, std::ios_base::in | std::ios_base::ate | std::ios_base::binary);
	if (!tcf) {
		sn->set_status_code(500);
		sn->set_response_header("Cache-Control", "no-store");
		sn->set_response_header("Content-type", "text/plain");
		sn->set_response_header("Content-length", "26");
		return sn->write("Failed to open transcode\r\n", 26);
	}

	size_t tc_size = tcf.tellg();
	tcf.clear();
	if (range && (range->first >= tc_size || range->second >= tc_size)) {
		sn->set_status_code(416);
		sn->set_response_header("Cache-Control", "no-store");
		sn->set_response_header("Content-length", "0");
		sn->set_response_header("Content-range", "bytes */" + std::to_string(tc_size));
		return sn->write_headers();
	}

	std::vector<char> tcbuf(16384);
	sn->set_response_header("Content-type", "audio/mpeg");
	if (range) {
		std::stringstream ss;
		int remaining = range->second - range->first + 1;
		ss << "bytes " << range->first << '-' << range->second << '/' << tc_size;

		tcf.seekg(range->first);
		sn->set_status_code(206);
		sn->set_response_header("Content-range", ss.str());
		while (!tcf.eof()) {
			tcf.read(tcbuf.data(), tcbuf.size());
			sn->write(tcbuf.data(), std::min(static_cast<int>(tcf.gcount()), remaining));
		}
	} else {
		tcf.seekg(0);
		sn->set_status_code(200);
		sn->set_response_header("Content-length", std::to_string(tc_size));
		while (!tcf.eof()) {
			tcf.read(tcbuf.data(), tcbuf.size());
			sn->write(tcbuf.data(), tcf.gcount());
		}
	}
}

static int iom_write(void* opaque, uint8_t* buf, int buf_size)
{
	tc_out_mux_t* out = static_cast<tc_out_mux_t*>(opaque);
	std::fwrite(buf, 1, buf_size, out->cache_fp);

	// chunk-encode the stream for HTTP
	char cel[16];
	int celln = snprintf(cel, 16, "%X\r\n", buf_size);
	out->sn->write(cel, celln);
	out->sn->write(reinterpret_cast<const char*>(buf), buf_size);
	out->sn->write("\r\n", 2);
	return buf_size;
}

static int open_input_file(const std::string& path, AVFormatContext** in_fmt_ctx, AVCodecContext** in_codec_ctx)
{
	AVCodecContext* avctx = nullptr;
	AVCodec* input_codec;
	int err, asid = -1;
	AVStream *audio_stream;

	if ((err = avformat_open_input(in_fmt_ctx, path.c_str(), nullptr, nullptr)) < 0) {
		std::cerr << "tc fail_open " << path << " : " << av_err2str(err) << std::endl;
		*in_fmt_ctx = nullptr;
		return err;
	}
	if ((err = avformat_find_stream_info(*in_fmt_ctx, nullptr)) < 0) {
		std::cerr << "tc fail_stream_inf " << path << " : " << av_err2str(err) << std::endl;
		avformat_close_input(in_fmt_ctx);
		*in_fmt_ctx = nullptr;
		return err;
	}

	for (size_t i = 0; i < (*in_fmt_ctx)->nb_streams && asid == -1; i++) {
		if ((*in_fmt_ctx)->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
			asid = i;
	}
	if (asid == -1) {
		std::cerr << "tc no_audio_stream " << path << std::endl;
		avformat_close_input(in_fmt_ctx);
		*in_fmt_ctx = nullptr;
		return err;
	}

	audio_stream = (*in_fmt_ctx)->streams[asid];
	if ((input_codec = avcodec_find_decoder(audio_stream->codecpar->codec_id)) == nullptr) {
		std::cerr << "tc no_audio_dec " << path << std::endl;
		avformat_close_input(in_fmt_ctx);
		*in_fmt_ctx = nullptr;
		return AVERROR_DECODER_NOT_FOUND;
	}
	if ((avctx = avcodec_alloc_context3(input_codec)) == nullptr) {
		avformat_close_input(in_fmt_ctx);
		*in_fmt_ctx = nullptr;
		return AVERROR(ENOMEM);
	}
	if ((err = avcodec_parameters_to_context(avctx, audio_stream->codecpar)) < 0) {
		std::cerr << "tc par2ctx " << path << " : " << av_err2str(err) << std::endl;
		avformat_close_input(in_fmt_ctx);
		avcodec_free_context(&avctx);
		*in_fmt_ctx = nullptr;
		return err;
	}
	if ((err = avcodec_open2(avctx, input_codec, nullptr)) < 0) {
		std::cerr << "tc open_input_codec " << path << " : " << av_err2str(err) << std::endl;
		avformat_close_input(in_fmt_ctx);
		avcodec_free_context(&avctx);
		*in_fmt_ctx = nullptr;
		return err;
	}

	*in_codec_ctx = avctx;
	return 0;
}

static int open_output(AVFormatContext **out_fmt_ctx, AVCodecContext **out_codec_ctx, tc_out_mux_t* out, int quality)
{
	AVCodecContext *avctx = nullptr;
	AVIOContext *out_io_ctx = nullptr;
	AVStream *stream = nullptr;
	AVCodec *out_codec = nullptr;
	unsigned char *iobuf = nullptr;
	int err;

	if ((iobuf = (unsigned char *) av_malloc(16384)) == nullptr)
		return AVERROR(ENOMEM);

	if ((out_io_ctx = avio_alloc_context(iobuf, 16384, 1, reinterpret_cast<void*>(out), nullptr, iom_write, nullptr)) == nullptr) {
		av_free(iobuf);
		return AVERROR_EXIT;
	}

	if ((*out_fmt_ctx = avformat_alloc_context()) == nullptr)
		return AVERROR(ENOMEM);
	(*out_fmt_ctx)->pb = out_io_ctx;

	if (((*out_fmt_ctx)->oformat = av_guess_format("mp3", nullptr, nullptr)) == nullptr) {
		std::cerr << "tc nomp3fmt" << std::endl;
		goto cleanup;
	}

	if ((out_codec = avcodec_find_encoder(AV_CODEC_ID_MP3)) == nullptr) {
		std::cerr << "tc nomp3enc" << std::endl;
		goto cleanup;
	}
	if ((stream = avformat_new_stream(*out_fmt_ctx, nullptr)) == nullptr) {
		err = AVERROR(ENOMEM);
		goto cleanup;
	}

	if ((avctx = avcodec_alloc_context3(out_codec)) == nullptr) {
		err = AVERROR(ENOMEM);
		goto cleanup;
	}

	avctx->flags |= AV_CODEC_FLAG_QSCALE;
	avctx->channels = 2;
	avctx->cutoff = 0;
	avctx->channel_layout = av_get_default_channel_layout(2);
	avctx->sample_rate = OUTPUT_SAMPLE_RATE;
	avctx->sample_fmt = AV_SAMPLE_FMT_S16P;
	avctx->global_quality = quality * FF_QP2LAMBDA;
	stream->time_base.den = OUTPUT_SAMPLE_RATE;
	stream->time_base.num = 1;

	if ((*out_fmt_ctx)->oformat->flags & AVFMT_GLOBALHEADER)
		avctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	if ((err = avcodec_open2(avctx, out_codec, nullptr)) < 0) {
		std::cerr << "tc fail_open_out : " << av_err2str(err) << std::endl;
		goto cleanup;
	}
	if ((err = avcodec_parameters_from_context(stream->codecpar, avctx)) < 0) {
		std::cerr << "tc par4ctx" << std::endl;
		goto cleanup;
	}

	*out_codec_ctx = avctx;
	return 0;

cleanup:
	avcodec_free_context(&avctx);
	av_free((*out_fmt_ctx)->pb->buffer);
	avio_context_free(&(*out_fmt_ctx)->pb);
	avformat_free_context(*out_fmt_ctx);
	*out_fmt_ctx = nullptr;
	return err < 0 ? err : AVERROR_EXIT;
}

static int init_resampler(AVCodecContext *in_codec_ctx, AVCodecContext *out_codec_ctx, SwrContext **resample_ctx)
{
	int err;
	*resample_ctx = swr_alloc_set_opts(nullptr,
					   av_get_default_channel_layout(out_codec_ctx->channels),
					   out_codec_ctx->sample_fmt, // AV_SAMPLE_FMT_S16P
					   out_codec_ctx->sample_rate,
					   av_get_default_channel_layout(in_codec_ctx->channels),
					   in_codec_ctx->sample_fmt,
					   in_codec_ctx->sample_rate,
					   0, nullptr);
	if (*resample_ctx == nullptr)
		return AVERROR(ENOMEM);
	if ((err = swr_init(*resample_ctx)) < 0) {
		std::cerr << "tc resample_init : " << av_err2str(err) << std::endl;
		swr_free(resample_ctx);
		return err;
	}
	return 0;
}

static int init_fifo(AVAudioFifo **fifo, AVCodecContext *out_codec_ctx)
{
	if ((*fifo = av_audio_fifo_alloc(out_codec_ctx->sample_fmt, out_codec_ctx->channels, 1)) == nullptr)
		return AVERROR(ENOMEM);
	else
		return 0;
}

static int decode_audio_frame(AVFrame *frame, AVFormatContext *in_fmt_ctx, AVCodecContext *in_codec_ctx, int *data_present, bool *finished)
{
	AVPacket in_pkt;
	int error;
	av_init_packet(&in_pkt);
	in_pkt.data = NULL;
	in_pkt.size = 0;

	if ((error = av_read_frame(in_fmt_ctx, &in_pkt)) < 0) {
		if (error == AVERROR_EOF)
			*finished = true;
		else {
			std::cerr << "tc read_frame : " << av_err2str(error) << std::endl;
			return error;
		}
	}
	if ((error = avcodec_send_packet(in_codec_ctx, &in_pkt)) < 0) {
		std::cerr << "tc send_packet_decode : " << av_err2str(error) << std::endl;
		return error;
	}

	error = avcodec_receive_frame(in_codec_ctx, frame);
	if (error == AVERROR(EAGAIN)) { // no data is present.
		error = 0;
	} else if (error == AVERROR_EOF) { // we're done
		*finished = 1;
		error = 0;
	} else if (error < 0) { // something bad happened
		std::cerr << "tc fail_decode : " << av_err2str(error) << std::endl;
	} else { // nothing bad happened, data is present
		*data_present = 1;
	}
	av_packet_unref(&in_pkt);
	return error;
}

static int init_converted_samples(uint8_t ***conv_in_samples, AVCodecContext *out_codec_ctx, int in_samples)
{
	int err;
	if ((*conv_in_samples = (uint8_t **) calloc(out_codec_ctx->channels, sizeof(**conv_in_samples))) == nullptr)
		return AVERROR(ENOMEM);

	if ((err = av_samples_alloc(*conv_in_samples, nullptr, out_codec_ctx->channels, in_samples, out_codec_ctx->sample_fmt, 0)) < 0) {
		av_freep(&(*conv_in_samples)[0]);
		free(*conv_in_samples);
		return err;
	}
	return 0;
}

static int convert_samples(const uint8_t **in_data, uint8_t **conv_data, const int in_samples, const int out_samples, SwrContext *resample_ctx)
{
	int rc;
	if ((rc = swr_convert(resample_ctx, conv_data, out_samples, in_data, in_samples)) < 0)
		std::cerr << "tc fail_conv_input : " << av_err2str(rc) << std::endl;

	return rc;
}

static int add_samples_to_fifo(AVAudioFifo *fifo, uint8_t **conv_samples, const int frame_size)
{
	int err;
	if ((err = av_audio_fifo_realloc(fifo, av_audio_fifo_size(fifo) + frame_size)) < 0)
		return err;

	if (av_audio_fifo_write(fifo, (void **) conv_samples, frame_size) < frame_size) {
		std::cerr << "tc write_fifo : " << std::endl;
		return AVERROR_EXIT;
	}
	return 0;
}

static int read_decode_convert_store(AVAudioFifo *fifo, AVFormatContext *in_fmt_ctx, AVCodecContext *in_codec_ctx, AVCodecContext *out_codec_ctx, SwrContext *resample_ctx, bool *finished)
{
	AVFrame *in_frame = nullptr;
	uint8_t **conv_in_samples = nullptr;
	int data_present = 0, ret = AVERROR_EXIT;

	if ((in_frame = av_frame_alloc()) == nullptr) {
		ret = AVERROR(ENOMEM);
		goto cleanup;
	}
	if (decode_audio_frame(in_frame, in_fmt_ctx, in_codec_ctx, &data_present, finished) != 0)
		goto cleanup;

	if (*finished) {
		ret = 0;
		goto cleanup;
	}

	if (data_present) {
		int max_out_samples = av_rescale_rnd(swr_get_delay(resample_ctx, in_codec_ctx->sample_rate) + in_frame->nb_samples, out_codec_ctx->sample_rate, in_codec_ctx->sample_rate, AV_ROUND_UP);
		int real_out_samples;
		if (init_converted_samples(&conv_in_samples, out_codec_ctx, max_out_samples) != 0)
			goto cleanup;
		if ((real_out_samples = convert_samples((const uint8_t **) in_frame->extended_data, conv_in_samples, in_frame->nb_samples, max_out_samples, resample_ctx)) < 0)
			goto cleanup;
		if (add_samples_to_fifo(fifo, conv_in_samples, real_out_samples))
			goto cleanup;
		ret = 0;
	}
	ret = 0;

cleanup:
	if (conv_in_samples != nullptr) {
		av_freep(&conv_in_samples[0]);
		free(conv_in_samples);
	}
	av_frame_free(&in_frame);
	return ret;
}

static int init_output_frame(AVFrame **frame, AVCodecContext *out_codec_ctx, int frame_size)
{
	int error;
	if ((*frame = av_frame_alloc()) == nullptr)
		return AVERROR(ENOMEM);

	(*frame)->nb_samples = frame_size;
	(*frame)->channel_layout = out_codec_ctx->channel_layout;
	(*frame)->format = out_codec_ctx->sample_fmt;
	(*frame)->sample_rate = out_codec_ctx->sample_rate;

	if ((error = av_frame_get_buffer(*frame, 0)) < 0) {
		av_frame_free(frame);
		return error;
	}
	return 0;
}

static int encode_audio_frame(AVFrame *frame, AVFormatContext *out_fmt_ctx, AVCodecContext *out_codec_ctx, uint64_t *pts, int *data_present)
{
	AVPacket out_pkt;
	int err;
	av_init_packet(&out_pkt);
	out_pkt.data = NULL;
	out_pkt.size = 0;

	if (frame) {
		frame->pts = *pts;
		*pts += frame->nb_samples;
	}

	err = avcodec_send_frame(out_codec_ctx, frame);
	if (err == AVERROR_EOF) {
		err = 0;
		goto cleanup;
	} else if (err < 0) {
		std::cerr << "tc send_pkt_encode : " << av_err2str(err) << std::endl;
		return err;
	}

	err = avcodec_receive_packet(out_codec_ctx, &out_pkt);
	if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) { // needs more data, OR done (no data to return)
		err = 0;
		goto cleanup;
	} else if (err < 0) {
		std::cerr << "tc encode_frame : " << av_err2str(err) << std::endl;
		goto cleanup;
	} else {
		*data_present = 1;
	}

	if (*data_present && (err = av_write_frame(out_fmt_ctx, &out_pkt)) < 0) {
		std::cerr << "tc write_frame : " << av_err2str(err) << std::endl;
		goto cleanup;
	}

cleanup:
	av_packet_unref(&out_pkt);
	return err;
}

static int load_encode_write(AVAudioFifo *fifo, AVFormatContext *out_fmt_ctx, AVCodecContext *out_codec_ctx, uint64_t *pts)
{
	AVFrame *out_frame;
	const int frame_size = FFMIN(av_audio_fifo_size(fifo), out_codec_ctx->frame_size);
	int err, data_written;
	if ((err = init_output_frame(&out_frame, out_codec_ctx, frame_size)) != 0)
		return err;

	if (av_audio_fifo_read(fifo, (void **) out_frame->data, frame_size) < frame_size) {
		std::cerr << "tc read_fifo" << std::endl;
		av_frame_free(&out_frame);
		return AVERROR_EXIT;
	}

	err = encode_audio_frame(out_frame, out_fmt_ctx, out_codec_ctx, pts, &data_written);
	av_frame_free(&out_frame);
	return err != 0 ? AVERROR_EXIT : 0;
}

static void cache_transcode(const std::string& tc_path, FILE* tc_src)
{
	std::vector<char> cbuf(16384);
	std::ofstream cs(tc_path, std::ios::out | std::ios::binary);
	std::rewind(tc_src);

	while (std::feof(tc_src) == false) {
		size_t res = std::fread(cbuf.data(), 1, cbuf.size(), tc_src);
		if (res < cbuf.size() && std::ferror(tc_src)) {
			perror("could not copy data from tempfile to cachefile");
			abort();
		}
		cs.write(cbuf.data(), res);
	}
	std::fclose(tc_src);
	cs.close();
}

void surf_server::api_v1_transcode(http_server::session* sn, const std::string& track_uuid, const std::string& track_path, int quality)
{
	AVFormatContext *in_fmt_ctx = nullptr, *out_fmt_ctx = nullptr;
	AVCodecContext *in_codec_ctx = nullptr, *out_codec_ctx = nullptr;
	AVAudioFifo *fifo = nullptr;
	SwrContext *resample_ctx = nullptr;
	uint64_t pts = 0;
	int ret = AVERROR_EXIT;
	tc_out_mux_t out;
	out.sn = sn;
	out.cache_fp = std::tmpfile();

	if ((ret = open_input_file(track_path, &in_fmt_ctx, &in_codec_ctx)) != 0) {
		sn->serve_error(500, "failed to open file for transcoding\r\n");
		goto end;
	}
	if ((ret = open_output(&out_fmt_ctx, &out_codec_ctx, &out, quality)) != 0) {
		sn->serve_error(500, "failed to open output\r\n");
		goto end;
	}
	if ((ret = init_resampler(in_codec_ctx, out_codec_ctx, &resample_ctx)) != 0) {
		sn->serve_error(500, "failed to open resampler\r\n");
		goto end;
	}
	if ((ret = init_fifo(&fifo, out_codec_ctx)) != 0) {
		sn->serve_error(500, "failed to allocate buffer\r\n");
		goto end;
	}

	sn->set_status_code(200);
	sn->set_response_header("Accept-Ranges", "bytes");
	sn->set_response_header("Content-type", "audio/mpeg");
	sn->set_response_header("Cache-Control", "public; max-age=31536000");
	sn->set_response_header("Transfer-Encoding", "chunked");
	if ((ret = avformat_write_header(out_fmt_ctx, nullptr)) < 0) {
		sn->serve_error(500, "failed to write to output\r\n");
		goto end;
	}

	while (true) {
		const int out_frame_size = out_codec_ctx->frame_size;
		bool finished = false;

		// Accumulate enough samples for the encoder.
		while (av_audio_fifo_size(fifo) < out_frame_size) {
			if (read_decode_convert_store(fifo, in_fmt_ctx, in_codec_ctx, out_codec_ctx, resample_ctx, &finished) != 0) {
				sn->serve_error(500, "failed to accumulate samples\r\n");
				goto end;
			}
			if (finished)
				break;
		}

		// Encode samples.
		while (av_audio_fifo_size(fifo) >= out_frame_size || (finished && av_audio_fifo_size(fifo) > 0)) {
			if (load_encode_write(fifo, out_fmt_ctx, out_codec_ctx, &pts) != 0) {
				sn->serve_error(500, "failed to encode samples\r\n");
				goto end;
			}
		}

		if (finished) {
			// Flush the encoder.
			int data_written;
			do {
				data_written = 0;
				if (encode_audio_frame(nullptr, out_fmt_ctx, out_codec_ctx, &pts, &data_written) != 0) {
					sn->serve_error(500, "failed to flush encoder\r\n");
					goto end;
				}
			} while (data_written);
			break;
		}
	}
	if ((ret = av_write_trailer(out_fmt_ctx)) < 0) {
		sn->serve_error(500, "failed to write trailer\r\n");
		goto end;
	}
	sn->write("0\r\n\r\n", 5); // Send a terminal chunk.

	cache_transcode(mdb.get_cached_transcode(track_uuid, quality).first, out.cache_fp);

end:
	if (fifo != nullptr)
		av_audio_fifo_free(fifo);
	swr_free(&resample_ctx);
	if (out_codec_ctx != nullptr)
		avcodec_free_context(&out_codec_ctx);
	if (out_fmt_ctx != nullptr) {
		av_free(out_fmt_ctx->pb->buffer);
		avio_context_free(&out_fmt_ctx->pb);
		avformat_free_context(out_fmt_ctx);
	}
	if (in_codec_ctx != nullptr)
		avcodec_free_context(&in_codec_ctx);
	if (in_fmt_ctx != nullptr)
		avformat_close_input(&in_fmt_ctx);
}
