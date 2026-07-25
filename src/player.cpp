#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
}

#include "RmlUi/Backend.h"
#include "player.h"

namespace {

// HTSP stream-type string -> FFmpeg codec
AVCodecID CodecFromHtspType(const std::string& type)
{
	if (type == "H264") return AV_CODEC_ID_H264;
	if (type == "HEVC") return AV_CODEC_ID_HEVC;
	if (type == "MPEG2VIDEO") return AV_CODEC_ID_MPEG2VIDEO;
	if (type == "VP8") return AV_CODEC_ID_VP8;
	if (type == "VP9") return AV_CODEC_ID_VP9;
	if (type == "AV1") return AV_CODEC_ID_AV1;
	if (type == "AC3") return AV_CODEC_ID_AC3;
	if (type == "EAC3") return AV_CODEC_ID_EAC3;
	if (type == "AAC") return AV_CODEC_ID_AAC;
	if (type == "MPEG2AUDIO") return AV_CODEC_ID_MP2;
	if (type == "MP3") return AV_CODEC_ID_MP3;
	if (type == "OPUS") return AV_CODEC_ID_OPUS;
	if (type == "VORBIS") return AV_CODEC_ID_VORBIS;
	return AV_CODEC_ID_NONE;
}

bool IsVideoType(const std::string& type)
{
	const AVCodecID id = CodecFromHtspType(type);
	if (id == AV_CODEC_ID_NONE)
		return false;
	return avcodec_get_type(id) == AVMEDIA_TYPE_VIDEO;
}

// Size of a single HTSP fileRead. Big enough to keep the request rate low on
// a 1080i transport stream, small enough that a seek doesn't stall.
constexpr int kAvioBufferSize = 256 * 1024;

// Packets buffered ahead of the decoders when playing a file.
constexpr size_t kFilePacketQueueDepth = 256;

} // namespace

// ---------------------------------------------------------------------------
// Packet queues
// ---------------------------------------------------------------------------

void Player::PacketQueue::Push(HtspMuxPacket&& pkt)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (drop_until_keyframe)
	{
		if (pkt.frametype && pkt.frametype != 'I')
			return;
		drop_until_keyframe = false;
	}
	if (q.size() >= max_packets)
	{
		if (!drop_when_full)
			return; // caller (the demux thread) retries after checking Full()
		// Live TV: prefer staying live. Drop everything and resume cleanly at
		// the next keyframe.
		q.clear();
		drop_until_keyframe = true;
		return;
	}
	q.push_back(std::move(pkt));
	cv.notify_one();
}

bool Player::PacketQueue::Pop(HtspMuxPacket& out, std::atomic<bool>& running, uint32_t* epoch)
{
	std::unique_lock<std::mutex> lock(mutex);
	cv.wait(lock, [&] { return !q.empty() || !running; });
	if (!running)
		return false;
	if (epoch)
		*epoch = flush_epoch;
	out = std::move(q.front());
	q.pop_front();
	return true;
}

void Player::PacketQueue::Clear()
{
	std::lock_guard<std::mutex> lock(mutex);
	q.clear();
	drop_until_keyframe = false;
	flush_epoch++;
	cv.notify_all();
}

bool Player::PacketQueue::Full()
{
	std::lock_guard<std::mutex> lock(mutex);
	return q.size() >= max_packets;
}

// ---------------------------------------------------------------------------
// Audio resampling
// ---------------------------------------------------------------------------

AudioResampler::~AudioResampler()
{
	Reset();
}

void AudioResampler::Reset()
{
	if (swr_)
		swr_free(&swr_);
	in_fmt_ = AV_SAMPLE_FMT_NONE;
	in_rate_ = 0;
	av_channel_layout_uninit(&in_layout_);
}

bool AudioResampler::Configure(const AVFrame* frame)
{
	Reset();

	AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
	if (swr_alloc_set_opts2(&swr_, &out_layout, AV_SAMPLE_FMT_S16, kPlayerAudioRate,
		&frame->ch_layout, (AVSampleFormat)frame->format, frame->sample_rate, 0, nullptr) < 0)
	{
		// swr_alloc_set_opts2 may still have allocated the context.
		Reset();
		return false;
	}
	if (!swr_ || swr_init(swr_) < 0)
	{
		// A context that failed to initialise must not be kept: swr_convert()
		// on it returns an error at best, and the next frame would skip the
		// configure step entirely because the pointer is non-null.
		Reset();
		return false;
	}

	in_fmt_ = (AVSampleFormat)frame->format;
	in_rate_ = frame->sample_rate;
	if (av_channel_layout_copy(&in_layout_, &frame->ch_layout) < 0)
	{
		Reset();
		return false;
	}
	return true;
}

int AudioResampler::Convert(const AVFrame* frame, std::vector<uint8_t>& out)
{
	if (!frame || frame->nb_samples <= 0 || frame->sample_rate <= 0 ||
	    frame->ch_layout.nb_channels <= 0 || frame->format < 0)
		return -1;

	// Rebuild whenever the input format differs from what the context was
	// configured for. Comparing the layout matters most: a mismatch there is
	// what reads past the end of frame->extended_data.
	const bool stale = !swr_ ||
		in_fmt_ != (AVSampleFormat)frame->format ||
		in_rate_ != frame->sample_rate ||
		av_channel_layout_compare(&in_layout_, &frame->ch_layout) != 0;
	if (stale && !Configure(frame))
		return -1;

	const int max_out = swr_get_out_samples(swr_, frame->nb_samples);
	if (max_out <= 0)
		return max_out == 0 ? 0 : -1;

	out.resize((size_t)max_out * kPlayerAudioChannels * 2);
	uint8_t* planes[1] = {out.data()};
	const int got = swr_convert(swr_, planes, max_out,
		(const uint8_t**)frame->extended_data, frame->nb_samples);
	if (got < 0)
		return -1;
	return got;
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

Player::Player() = default;

Player::~Player()
{
	StopPipeline();
	CloseRecording();
}

bool Player::Initialize(std::string& error)
{
	renderer_ = Backend::GetSDLRenderer();
	if (!renderer_)
	{
		error = "no SDL renderer";
		return false;
	}
	return true; // the texture is created lazily, once the frame size is known
}

void Player::Shutdown()
{
	active_ = false;
	StopPipeline();
	CloseRecording();

	if (current_.frame)
	{
		av_frame_free(&current_.frame);
		current_ = {};
	}
	if (texture_)
	{
		SDL_DestroyTexture(texture_);
		texture_ = nullptr;
	}
	tex_w_ = tex_h_ = 0;
	renderer_ = nullptr;
}

std::string Player::StatusText() const
{
	std::lock_guard<std::mutex> lock(status_mutex_);
	return status_;
}

void Player::SetStatus(const std::string& text)
{
	std::lock_guard<std::mutex> lock(status_mutex_);
	status_ = text;
}

// ---------------------------------------------------------------------------
// Live TV
// ---------------------------------------------------------------------------

bool Player::PlayChannel(HtspClient& client, uint32_t channel_id, std::string& error)
{
	Stop(client);

	// Callbacks fire on the HTSP reader thread.
	HtspClient::StreamCallbacks cbs;
	cbs.on_start = [this](std::vector<HtspStreamInfo> streams) {
		const std::string desc = StartLiveStreams(streams);
		SetStatus(desc.empty() ? "No playable streams" : desc);
	};
	cbs.on_packet = [this](HtspMuxPacket&& pkt) { Enqueue(std::move(pkt)); };
	cbs.on_status = [this](std::string status) {
		if (!status.empty())
			SetStatus(status);
	};
	client.SetStreamCallbacks(std::move(cbs));

	SetStatus("Tuning...");
	if (!client.Subscribe(channel_id, error))
	{
		client.SetStreamCallbacks({});
		SetStatus({});
		return false;
	}

	source_ = Source::Live;
	active_ = true;
	return true;
}

bool Player::OpenLiveDecoder(const HtspStreamInfo& info, AVCodecContext** out_ctx)
{
	const AVCodecID id = CodecFromHtspType(info.type);
	const AVCodec* codec = avcodec_find_decoder(id);
	if (!codec)
		return false;

	AVCodecContext* ctx = avcodec_alloc_context3(codec);
	if (!ctx)
		return false;

	// HTSP timestamps are microseconds.
	ctx->pkt_timebase = AVRational{1, 1000000};
	if (avcodec_get_type(id) == AVMEDIA_TYPE_VIDEO)
	{
		ctx->width = (int)info.width;
		ctx->height = (int)info.height;
	}

	if (avcodec_open2(ctx, codec, nullptr) < 0)
	{
		avcodec_free_context(&ctx);
		return false;
	}
	*out_ctx = ctx;
	return true;
}

std::string Player::StartLiveStreams(const std::vector<HtspStreamInfo>& streams)
{
	StopPipeline();

	std::string desc;
	for (const HtspStreamInfo& s : streams)
	{
		if (video_stream_ < 0 && IsVideoType(s.type) && OpenLiveDecoder(s, &vctx_))
		{
			video_stream_ = (int)s.index;
			desc += s.type;
			if (s.width && s.height)
				desc += " " + std::to_string(s.width) + "x" + std::to_string(s.height);
		}
	}
	for (const HtspStreamInfo& s : streams)
	{
		if (audio_stream_ < 0 && !IsVideoType(s.type) && CodecFromHtspType(s.type) != AV_CODEC_ID_NONE &&
		    OpenLiveDecoder(s, &actx_))
		{
			audio_stream_ = (int)s.index;
			desc += (desc.empty() ? "" : " + ") + s.type;
		}
	}

	if (video_stream_ < 0 && audio_stream_ < 0)
		return {};

	// Live TV stays live: queues drop rather than block.
	vqueue_.drop_when_full = true;
	aqueue_.drop_when_full = true;
	vqueue_.max_packets = aqueue_.max_packets = 512;

	if (!StartPipeline())
		return {};
	return desc;
}

void Player::Enqueue(HtspMuxPacket&& pkt)
{
	if (!threads_running_)
		return;
	if ((int)pkt.stream == video_stream_)
		vqueue_.Push(std::move(pkt));
	else if ((int)pkt.stream == audio_stream_)
		aqueue_.Push(std::move(pkt));
}

// ---------------------------------------------------------------------------
// Recording playback (HTSP file API + libavformat)
// ---------------------------------------------------------------------------

int Player::AvioRead(void* opaque, uint8_t* buf, int buf_size)
{
	HtspFile* f = static_cast<HtspFile*>(opaque);
	if (!f || !f->open || !f->client)
		return AVERROR_EOF;

	std::string error;
	const int64_t got = f->client->FileRead(f->handle, buf, buf_size, f->pos, error);
	if (got < 0)
	{
		std::fprintf(stderr, "htsp fileRead failed: %s\n", error.c_str());
		return AVERROR(EIO);
	}
	if (got == 0)
	{
		// The file may still be growing (recording in progress): refresh the
		// size so seeking keeps working, and report EOF for this read.
		int64_t size = -1;
		if (f->client->FileStat(f->handle, &size, error) && size > 0)
			f->size = size;
		return AVERROR_EOF;
	}
	f->pos += got;
	return (int)got;
}

int64_t Player::AvioSeek(void* opaque, int64_t offset, int whence)
{
	HtspFile* f = static_cast<HtspFile*>(opaque);
	if (!f || !f->open || !f->client)
		return AVERROR(EIO);

	std::string error;
	if (whence & AVSEEK_SIZE)
	{
		if (f->size < 0)
		{
			int64_t size = -1;
			if (f->client->FileStat(f->handle, &size, error))
				f->size = size;
		}
		return f->size >= 0 ? f->size : AVERROR(ENOSYS);
	}

	int64_t target = f->pos;
	switch (whence & ~AVSEEK_FORCE)
	{
	case SEEK_SET: target = offset; break;
	case SEEK_CUR: target = f->pos + offset; break;
	case SEEK_END:
		if (f->size < 0)
		{
			int64_t size = -1;
			if (!f->client->FileStat(f->handle, &size, error) || size < 0)
				return AVERROR(ENOSYS);
			f->size = size;
		}
		target = f->size + offset;
		break;
	default:
		return AVERROR(EINVAL);
	}
	if (target < 0)
		return AVERROR(EINVAL);

	// Reads pass an explicit offset, so the server-side position does not
	// have to be moved; tracking it locally saves a round trip per seek.
	f->pos = target;
	return target;
}

bool Player::OpenRecording(HtspClient& client, uint32_t dvr_id, std::string& error)
{
	CloseRecording();

	file_.client = &client;
	file_.pos = 0;
	file_.size = -1;
	if (!client.FileOpen("/dvrfile/" + std::to_string(dvr_id), &file_.handle, &file_.size, error))
		return false;
	file_.open = true;

	unsigned char* buffer = (unsigned char*)av_malloc(kAvioBufferSize);
	if (!buffer)
	{
		error = "out of memory";
		CloseRecording();
		return false;
	}

	avio_ = avio_alloc_context(buffer, kAvioBufferSize, 0, &file_, &Player::AvioRead, nullptr, &Player::AvioSeek);
	if (!avio_)
	{
		av_free(buffer);
		error = "failed to create the AVIO context";
		CloseRecording();
		return false;
	}
	avio_->seekable = file_.size >= 0 ? AVIO_SEEKABLE_NORMAL : 0;

	fmt_ = avformat_alloc_context();
	if (!fmt_)
	{
		error = "out of memory";
		CloseRecording();
		return false;
	}
	fmt_->pb = avio_;
	fmt_->flags |= AVFMT_FLAG_CUSTOM_IO;

	if (avformat_open_input(&fmt_, nullptr, nullptr, nullptr) < 0)
	{
		// avformat_open_input frees fmt_ on failure.
		fmt_ = nullptr;
		error = "unable to read the recording";
		CloseRecording();
		return false;
	}
	if (avformat_find_stream_info(fmt_, nullptr) < 0)
	{
		error = "unable to identify the streams in the recording";
		CloseRecording();
		return false;
	}

	file_video_stream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	file_audio_stream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
	if (file_video_stream_ < 0 && file_audio_stream_ < 0)
	{
		error = "the recording has no playable streams";
		CloseRecording();
		return false;
	}

	// Decoders are opened straight from the container's stream parameters,
	// which also carries the extradata the live path gets from HTSP.
	std::string desc;
	auto open_stream = [&](int index, AVCodecContext** out) -> bool {
		if (index < 0)
			return false;
		AVStream* st = fmt_->streams[index];
		const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
		if (!codec)
			return false;
		AVCodecContext* ctx = avcodec_alloc_context3(codec);
		if (!ctx)
			return false;
		if (avcodec_parameters_to_context(ctx, st->codecpar) < 0 || avcodec_open2(ctx, codec, nullptr) < 0)
		{
			avcodec_free_context(&ctx);
			return false;
		}
		ctx->pkt_timebase = st->time_base;
		*out = ctx;
		return true;
	};

	if (open_stream(file_video_stream_, &vctx_))
	{
		video_stream_ = file_video_stream_;
		desc = avcodec_get_name(vctx_->codec_id);
		if (vctx_->width && vctx_->height)
			desc += " " + std::to_string(vctx_->width) + "x" + std::to_string(vctx_->height);
	}
	else
	{
		file_video_stream_ = -1;
	}
	if (open_stream(file_audio_stream_, &actx_))
	{
		audio_stream_ = file_audio_stream_;
		desc += (desc.empty() ? "" : " + ") + std::string(avcodec_get_name(actx_->codec_id));
	}
	else
	{
		file_audio_stream_ = -1;
	}

	if (video_stream_ < 0 && audio_stream_ < 0)
	{
		error = "no decoder available for the recording";
		CloseRecording();
		return false;
	}

	file_start_us_ = (fmt_->start_time != AV_NOPTS_VALUE) ? fmt_->start_time : 0;
	duration_us_ = (fmt_->duration != AV_NOPTS_VALUE && fmt_->duration > 0) ? fmt_->duration : -1;
	seekable_ = file_.size >= 0 && duration_us_ > 0;
	SetStatus(desc);
	return true;
}

void Player::CloseRecording()
{
	if (fmt_)
	{
		avformat_close_input(&fmt_); // also frees fmt_
		fmt_ = nullptr;
	}
	if (avio_)
	{
		if (avio_->buffer)
			av_freep(&avio_->buffer);
		avio_context_free(&avio_);
	}
	if (file_.open && file_.client)
		file_.client->FileClose(file_.handle);
	file_ = {};
	file_video_stream_ = file_audio_stream_ = -1;
	file_start_us_ = 0;
	duration_us_ = -1;
	seek_target_us_ = -1;
	last_position_us_ = 0;
	seekable_ = false;
	eof_ = false;
}

bool Player::PlayRecording(HtspClient& client, uint32_t dvr_id, std::string& error)
{
	Stop(client);

	if (!client.SupportsFileApi())
	{
		error = "this server is too old to stream recordings over HTSP";
		return false;
	}

	SetStatus("Opening recording...");
	if (!OpenRecording(client, dvr_id, error))
	{
		SetStatus({});
		return false;
	}

	// A file is not live: the demux thread waits when the queues are full
	// rather than dropping data.
	vqueue_.drop_when_full = false;
	aqueue_.drop_when_full = false;
	vqueue_.max_packets = aqueue_.max_packets = kFilePacketQueueDepth;

	if (!StartPipeline())
	{
		CloseRecording();
		SetStatus({});
		error = "failed to start the decoders";
		return false;
	}

	dthread_ = std::thread(&Player::DemuxThread, this);
	source_ = Source::Recording;
	active_ = true;
	return true;
}

void Player::DemuxThread()
{
	AVPacket* pkt = av_packet_alloc();

	while (threads_running_)
	{
		// Pending seek: reposition, then flush the queues so the decoders
		// throw away everything from before the jump.
		const int64_t seek_us = seek_target_us_.exchange(-1);
		if (seek_us >= 0)
		{
			if (avformat_seek_file(fmt_, -1, INT64_MIN, seek_us + file_start_us_, INT64_MAX, 0) >= 0)
			{
				vqueue_.Clear();
				aqueue_.Clear();
				DropDecodedFrames();
				if (audio_dev_)
					SDL_ClearQueuedAudio(audio_dev_);
				audio_pts_end_ = INT64_MIN;
				wall_anchor_pts_ = INT64_MIN;
				last_position_us_ = seek_us;
				eof_ = false;
			}
			continue;
		}

		if (paused_ || vqueue_.Full() || aqueue_.Full())
		{
			// Nothing to do right now; sleep briefly so a seek or a resume
			// is picked up quickly.
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			continue;
		}

		const int rc = av_read_frame(fmt_, pkt);
		if (rc < 0)
		{
			if (rc == AVERROR_EOF)
			{
				// End of what the server has written so far. For a recording
				// that is still running this is temporary, so clear the EOF
				// flag and retry; the next fileRead re-stats the file.
				eof_ = true;
				std::this_thread::sleep_for(std::chrono::milliseconds(500));
				if (avio_)
					avio_->eof_reached = 0;
				eof_ = false;
			}
			else
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			}
			continue;
		}

		if (pkt->stream_index == file_video_stream_ || pkt->stream_index == file_audio_stream_)
		{
			const AVRational tb = fmt_->streams[pkt->stream_index]->time_base;
			HtspMuxPacket out;
			out.stream = (uint32_t)pkt->stream_index;
			out.pts = (pkt->pts != AV_NOPTS_VALUE)
				? av_rescale_q(pkt->pts, tb, AVRational{1, 1000000}) - file_start_us_ : INT64_MIN;
			out.dts = (pkt->dts != AV_NOPTS_VALUE)
				? av_rescale_q(pkt->dts, tb, AVRational{1, 1000000}) - file_start_us_ : INT64_MIN;
			out.frametype = (pkt->flags & AV_PKT_FLAG_KEY) ? 'I' : 0;
			out.payload.assign(pkt->data, pkt->data + pkt->size);
			if (pkt->stream_index == file_video_stream_)
				vqueue_.Push(std::move(out));
			else
				aqueue_.Push(std::move(out));
		}
		av_packet_unref(pkt);
	}

	av_packet_free(&pkt);
}

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

int64_t Player::PositionUs() const
{
	if (source_ != Source::Recording)
		return -1;
	return last_position_us_.load();
}

void Player::SeekRelative(int64_t delta_us)
{
	if (!CanSeek())
		return;

	const int64_t duration = duration_us_.load();
	int64_t target = last_position_us_.load() + delta_us;
	if (target < 0)
		target = 0;
	if (duration > 0 && target > duration - 1000000)
		target = std::max<int64_t>(0, duration - 1000000);

	last_position_us_ = target; // so repeated presses accumulate
	seek_target_us_ = target;
}

void Player::SetPaused(bool paused)
{
	if (source_ != Source::Recording)
		return; // pausing live TV would just desync the stream
	if (paused_ == paused)
		return;

	paused_ = paused;
	if (audio_dev_)
		SDL_PauseAudioDevice(audio_dev_, paused ? 1 : 0);

	if (!paused)
	{
		// Re-anchor the fallback wall clock so it doesn't jump by the length
		// of the pause on resume.
		if (wall_anchor_pts_ != INT64_MIN)
			wall_anchor_time_ = av_gettime_relative();
	}
}

bool Player::TogglePause()
{
	SetPaused(!paused_);
	return paused_;
}

// ---------------------------------------------------------------------------
// Pipeline start / stop
// ---------------------------------------------------------------------------

bool Player::OpenAudioDevice()
{
	// SDL queue mode (no callback). The amount of queued but not yet played
	// audio is what drives the master clock.
	SDL_AudioSpec want = {}, have = {};
	want.freq = kAudioRate;
	want.format = AUDIO_S16SYS;
	want.channels = kAudioChannels;
	want.samples = 1024;
	audio_dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
	if (!audio_dev_)
	{
		std::fprintf(stderr, "audio device unavailable (%s); using wall clock\n", SDL_GetError());
		return false;
	}
	SDL_PauseAudioDevice(audio_dev_, 0);
	return true;
}

bool Player::StartPipeline()
{
	if (audio_stream_ >= 0 && !OpenAudioDevice())
	{
		avcodec_free_context(&actx_);
		audio_stream_ = -1;
	}

	audio_pts_end_ = INT64_MIN;
	wall_anchor_pts_ = INT64_MIN;
	paused_ = false;
	eof_ = false;

	threads_running_ = true;
	if (video_stream_ >= 0)
		vthread_ = std::thread(&Player::VideoThread, this);
	if (audio_stream_ >= 0)
		athread_ = std::thread(&Player::AudioThread, this);
	return video_stream_ >= 0 || audio_stream_ >= 0;
}

void Player::StopPipeline()
{
	threads_running_ = false;
	paused_ = false;
	vqueue_.cv.notify_all();
	aqueue_.cv.notify_all();
	frames_cv_.notify_all();

	if (dthread_.joinable())
		dthread_.join();
	if (vthread_.joinable())
		vthread_.join();
	if (athread_.joinable())
		athread_.join();

	vqueue_.Clear();
	aqueue_.Clear();
	DropDecodedFrames();

	if (audio_dev_)
	{
		SDL_CloseAudioDevice(audio_dev_);
		audio_dev_ = 0;
	}
	if (vctx_) avcodec_free_context(&vctx_);
	if (actx_) avcodec_free_context(&actx_);
	if (sws_) { sws_freeContext(sws_); sws_ = nullptr; }
	resampler_.Reset();
	video_stream_ = audio_stream_ = -1;
}

void Player::DropDecodedFrames()
{
	std::lock_guard<std::mutex> lock(frames_mutex_);
	for (TimedFrame& tf : frames_)
		av_frame_free(&tf.frame);
	frames_.clear();
	frames_cv_.notify_all();
}

void Player::Stop(HtspClient& client)
{
	const Source was = source_.load();
	active_ = false;
	source_ = Source::None;

	if (was == Source::Live)
	{
		client.Unsubscribe();
		client.SetStreamCallbacks({});
	}

	StopPipeline();
	if (was == Source::Recording)
		CloseRecording();

	SetStatus({});
}

// ---------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------

int64_t Player::MasterClock()
{
	const int64_t aend = audio_pts_end_;
	if (audio_dev_ && aend != INT64_MIN)
	{
		const int64_t queued_us = (int64_t)SDL_GetQueuedAudioSize(audio_dev_) * 1000000 / kAudioBytesPerSec;
		return aend - queued_us;
	}
	// No audio: wall clock anchored at the first video frame.
	const int64_t anchor = wall_anchor_pts_;
	if (anchor == INT64_MIN)
		return INT64_MIN;
	if (paused_)
		return anchor;
	return anchor + (av_gettime_relative() - wall_anchor_time_);
}

// ---------------------------------------------------------------------------
// Decode threads
// ---------------------------------------------------------------------------

void Player::PushVideoFrame(AVFrame* frame)
{
	if (frame->width <= 0 || frame->height <= 0 || frame->format < 0)
		return;

	// Normalize to yuv420p (= SDL IYUV) if the decoder emits anything else.
	// sws_getCachedContext rebuilds itself when the size or pixel format
	// changes, which a recording can do mid-stream.
	if (frame->format != AV_PIX_FMT_YUV420P)
	{
		AVFrame* conv = av_frame_alloc();
		if (!conv)
			return;
		conv->format = AV_PIX_FMT_YUV420P;
		conv->width = frame->width;
		conv->height = frame->height;
		sws_ = sws_getCachedContext(sws_, frame->width, frame->height, (AVPixelFormat)frame->format,
			frame->width, frame->height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
		if (!sws_ || av_frame_get_buffer(conv, 0) < 0)
		{
			av_frame_free(&conv);
			return;
		}
		sws_scale(sws_, frame->data, frame->linesize, 0, frame->height, conv->data, conv->linesize);
		conv->pts = frame->pts;
		conv->best_effort_timestamp = frame->best_effort_timestamp;
		conv->sample_aspect_ratio = frame->sample_aspect_ratio;
		av_frame_unref(frame);
		av_frame_move_ref(frame, conv);
		av_frame_free(&conv);
	}

	TimedFrame tf;
	tf.frame = av_frame_alloc();
	if (!tf.frame)
		return;
	av_frame_move_ref(tf.frame, frame);
	tf.pts = (tf.frame->best_effort_timestamp != AV_NOPTS_VALUE) ? tf.frame->best_effort_timestamp : INT64_MIN;

	std::unique_lock<std::mutex> lock(frames_mutex_);
	frames_cv_.wait(lock, [&] { return frames_.size() < kMaxFrames || !threads_running_; });
	if (!threads_running_)
	{
		av_frame_free(&tf.frame);
		return;
	}
	frames_.push_back(tf);
}

void Player::VideoThread()
{
	AVPacket* pkt = av_packet_alloc();
	AVFrame* frame = av_frame_alloc();
	uint32_t epoch = 0;
	bool have_epoch = false;

	HtspMuxPacket in;
	uint32_t pkt_epoch = 0;
	while (vqueue_.Pop(in, threads_running_, &pkt_epoch))
	{
		// A flush happened (seek): drop whatever the decoder is holding.
		if (!have_epoch)
		{
			epoch = pkt_epoch;
			have_epoch = true;
		}
		else if (pkt_epoch != epoch)
		{
			epoch = pkt_epoch;
			avcodec_flush_buffers(vctx_);
		}

		av_new_packet(pkt, (int)in.payload.size());
		std::memcpy(pkt->data, in.payload.data(), in.payload.size());
		pkt->pts = (in.pts != INT64_MIN) ? in.pts : AV_NOPTS_VALUE;
		pkt->dts = (in.dts != INT64_MIN) ? in.dts : AV_NOPTS_VALUE;
		if (in.frametype == 'I')
			pkt->flags |= AV_PKT_FLAG_KEY;

		if (avcodec_send_packet(vctx_, pkt) == 0)
		{
			while (avcodec_receive_frame(vctx_, frame) == 0)
			{
				// No audio stream: anchor the wall clock at the first frame.
				if (audio_stream_ < 0 && wall_anchor_pts_ == INT64_MIN &&
				    frame->best_effort_timestamp != AV_NOPTS_VALUE)
				{
					wall_anchor_pts_ = frame->best_effort_timestamp;
					wall_anchor_time_ = av_gettime_relative();
				}
				PushVideoFrame(frame);
				av_frame_unref(frame);
			}
		}
		av_packet_unref(pkt);
	}

	av_frame_free(&frame);
	av_packet_free(&pkt);
}

void Player::AudioThread()
{
	AVPacket* pkt = av_packet_alloc();
	AVFrame* frame = av_frame_alloc();
	std::vector<uint8_t> out;
	int64_t pts_accum = INT64_MIN;
	uint32_t epoch = 0;
	bool have_epoch = false;

	HtspMuxPacket in;
	uint32_t pkt_epoch = 0;
	while (aqueue_.Pop(in, threads_running_, &pkt_epoch))
	{
		if (!have_epoch)
		{
			epoch = pkt_epoch;
			have_epoch = true;
		}
		else if (pkt_epoch != epoch)
		{
			// Seek: drop the decoder's and the resampler's buffered tail so
			// nothing from before the jump is played afterwards.
			epoch = pkt_epoch;
			pts_accum = INT64_MIN;
			avcodec_flush_buffers(actx_);
			resampler_.Reset();
		}

		av_new_packet(pkt, (int)in.payload.size());
		std::memcpy(pkt->data, in.payload.data(), in.payload.size());
		pkt->pts = (in.pts != INT64_MIN) ? in.pts : AV_NOPTS_VALUE;
		pkt->dts = (in.dts != INT64_MIN) ? in.dts : AV_NOPTS_VALUE;

		if (avcodec_send_packet(actx_, pkt) == 0)
		{
			while (avcodec_receive_frame(actx_, frame) == 0)
			{
				const int got = resampler_.Convert(frame, out);
				if (got < 0)
				{
					// Unconvertible frame (bad format, or the resampler could
					// not be built for it): skip it rather than stopping.
					av_frame_unref(frame);
					continue;
				}
				if (got > 0 && audio_dev_)
				{
					SDL_QueueAudio(audio_dev_, out.data(), (Uint32)got * kAudioChannels * 2);

					const int64_t dur = (int64_t)frame->nb_samples * 1000000 / frame->sample_rate;
					if (frame->best_effort_timestamp != AV_NOPTS_VALUE)
						pts_accum = frame->best_effort_timestamp + dur;
					else if (pts_accum != INT64_MIN)
						pts_accum += dur;
					if (pts_accum != INT64_MIN)
						audio_pts_end_ = pts_accum;
				}
				av_frame_unref(frame);
			}
		}
		av_packet_unref(pkt);
	}

	av_frame_free(&frame);
	av_packet_free(&pkt);
}

// ---------------------------------------------------------------------------
// Presentation
// ---------------------------------------------------------------------------

void Player::RenderVideo(int win_w, int win_h)
{
	if (!renderer_ || (!threads_running_ && !current_.frame))
		return;

	// Advance to the newest frame that is due (drop older late ones).
	const int64_t clock = MasterClock();
	{
		std::lock_guard<std::mutex> lock(frames_mutex_);
		while (!frames_.empty())
		{
			const TimedFrame& head = frames_.front();
			const bool due = (head.pts == INT64_MIN) || (clock != INT64_MIN && head.pts <= clock);
			if (!due)
				break;
			if (current_.frame)
				av_frame_free(&current_.frame);
			current_ = frames_.front();
			frames_.pop_front();
			frames_cv_.notify_one();
		}
	}

	if (current_.pts != INT64_MIN && source_ == Source::Recording)
		last_position_us_ = current_.pts;

	if (!current_.frame)
		return;
	AVFrame* f = current_.frame;

	// (Re)create the streaming texture when the frame size changes. IYUV is
	// exactly yuv420p, which PushVideoFrame() normalizes to.
	if (!texture_ || tex_w_ != f->width || tex_h_ != f->height)
	{
		if (texture_)
			SDL_DestroyTexture(texture_);
		texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, f->width, f->height);
		if (!texture_)
		{
			std::fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
			return;
		}
		SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_NONE);
		tex_w_ = f->width;
		tex_h_ = f->height;
	}

	SDL_UpdateYUVTexture(texture_, nullptr,
		f->data[0], f->linesize[0],
		f->data[1], f->linesize[1],
		f->data[2], f->linesize[2]);

	// Letterbox: fit the (SAR-corrected) frame into the window.
	double dar = (double)f->width / f->height;
	if (f->sample_aspect_ratio.num > 0 && f->sample_aspect_ratio.den > 0)
		dar *= av_q2d(f->sample_aspect_ratio);
	const double win_ar = (double)win_w / win_h;
	SDL_Rect dst;
	if (win_ar > dar)
	{
		dst.h = win_h;
		dst.w = (int)(win_h * dar + 0.5);
	}
	else
	{
		dst.w = win_w;
		dst.h = (int)(win_w / dar + 0.5);
	}
	dst.x = (win_w - dst.w) / 2;
	dst.y = (win_h - dst.h) / 2;

	SDL_RenderCopy(renderer_, texture_, nullptr, &dst);
}
