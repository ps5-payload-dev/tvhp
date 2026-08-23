#ifndef PLAYER_H
#define PLAYER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <SDL.h>

#include "htsp.h"

// Audio format handed to SDL.
inline constexpr int kPlayerAudioRate = 48000;
inline constexpr int kPlayerAudioChannels = 2;
inline constexpr int kPlayerAudioBytesPerSec = kPlayerAudioRate * kPlayerAudioChannels * 2; // S16

// Converts decoded audio frames to the format the SDL device expects
// (S16, stereo, 48 kHz).
//
// The resampler is rebuilt whenever the input format changes. That is not a
// theoretical case: recordings change it mid-stream - a 5.1 AC3 programme
// followed by a stereo ad break is the common one - and a seek can land in a
// differently configured part of the file. Handing swr_convert() a frame with
// fewer planes than the context was configured for makes it read past the end
// of the frame's plane array and segfault, so the format is re-checked on
// every frame.
class AudioResampler {
public:
  ~AudioResampler();

  // Converts one frame. Returns the number of samples per channel written to
  // 'out', 0 if the frame produced nothing, or -1 if it could not be
  // converted (in which case the frame should be skipped, not retried).
  int Convert(const AVFrame* frame, std::vector<uint8_t>& out);

  // Drops the resampler and anything it has buffered. Used after a seek, so
  // samples from before the jump are not emitted afterwards.
  void Reset();

private:
  bool Configure(const AVFrame* frame);

  SwrContext* swr_ = nullptr;
  AVSampleFormat in_fmt_ = AV_SAMPLE_FMT_NONE;
  int in_rate_ = 0;
  AVChannelLayout in_layout_ = {};
};

// Plays tvheadend content by decoding it directly with libavcodec. This is
// the merge of what used to be FfmpegPlayer (decode + present) and the thin
// Player wrapper around it (HTSP subscription plumbing): one class owns the
// whole path from HTSP message to pixels.
//
// Two sources feed the same decode/present pipeline:
//
//   Live TV      subscribe() -> subscriptionStart gives the elementary stream
//                list, muxpkt payloads are pushed straight into the packet
//                queues. No container, no demuxer.
//
//   Recordings   fileOpen("/dvrfile/<id>") -> a libavformat AVIOContext whose
//                read/seek callbacks are HTSP fileRead/fileStat (the seek is
//                resolved client-side), demuxed by
//                libavformat on a private thread. This is what makes seeking
//                and playback of still-recording files work.
//
// Threads:
//   HTSP reader  -> Enqueue()                (live only, bounded queues)
//   demux thread -> read + queue packets     (recordings only)
//   video thread -> decode -> frame queue    (yuv420p, converted if needed)
//   audio thread -> decode -> swresample -> SDL audio queue (S16 stereo 48k)
//   main thread  -> RenderVideo(): pick the frame that is due, upload it to a
//                   streaming IYUV SDL_Texture and copy it letterboxed. The
//                   YUV->RGB conversion is done by SDL (in software with the
//                   software renderer); no OpenGL is used.
class Player {
public:
  Player();
  ~Player();

  // Main thread. Grabs the backend's SDL renderer, which must outlive this
  // object (or Shutdown() must be called before it is destroyed).
  bool Initialize(std::string& error);
  void Shutdown();

  // Starts live playback of a channel, replacing whatever is playing.
  bool PlayChannel(HtspClient& client, uint32_t channel_id, std::string& error);

  // Starts playback of a recording over the HTSP file API, replacing
  // whatever is playing. Works for finished and in-progress recordings.
  bool PlayRecording(HtspClient& client, uint32_t dvr_id, std::string& error);

  // Stops playback, unsubscribing or closing the file as appropriate.
  void Stop(HtspClient& client);

  bool IsPlaying() const { return active_; }
  // True while playing a recording rather than live TV.
  bool IsRecordingPlayback() const { return source_ == Source::Recording; }

  // Called every frame after the backbuffer has been cleared and before the
  // UI is rendered; the document body is transparent while playing, so the
  // UI composites on top.
  void RenderVideo(int width, int height);

  // Short human-readable status ("H264 1280x720 + AC3", "Tuning...", the
  // last subscriptionStatus, ...) for display in the UI. Empty = nothing.
  std::string StatusText() const;

  // --- Transport, recordings only ----------------------------------------
  bool CanSeek() const { return source_ == Source::Recording && seekable_; }
  // Position and duration in microseconds; -1 when unknown.
  int64_t PositionUs() const;
  int64_t DurationUs() const { return duration_us_.load(); }
  // Seeks by a relative amount; clamped to the file. No-op for live TV.
  void SeekRelative(int64_t delta_us);
  void SetPaused(bool paused);
  // Returns the new paused state.
  bool TogglePause();
  bool IsPaused() const { return paused_; }

private:
  enum class Source { None, Live, Recording };

  struct PacketQueue {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<HtspMuxPacket> q;
    size_t max_packets = 512;
    bool drop_until_keyframe = false;
    // Live TV drops packets to stay live; a recording is a file, so the
    // producer waits instead of throwing data away.
    bool drop_when_full = true;
    // Bumped on flush; the decode thread resets its codec state when it sees
    // the counter change, so a seek doesn't smear old frames into new ones.
    uint32_t flush_epoch = 0;

    void Push(HtspMuxPacket&& pkt);
    bool Pop(HtspMuxPacket& out, std::atomic<bool>& running, uint32_t* epoch);
    void Clear();
    bool Full();
  };

  struct TimedFrame {
    AVFrame* frame = nullptr;
    int64_t pts = INT64_MIN; // us
  };

  // The AVIOContext callbacks read through these.
  struct HtspFile {
    HtspClient* client = nullptr;
    uint32_t handle = 0;
    int64_t pos = 0;
    int64_t size = -1; // -1 = unknown / still growing
    bool open = false;
  };

  // Decode / present pipeline, shared by both sources.
  bool StartPipeline();
  void StopPipeline();
  void VideoThread();
  void AudioThread();
  void DemuxThread();
  int64_t MasterClock(); // us, or INT64_MIN if not started
  bool OpenAudioDevice();
  void PushVideoFrame(AVFrame* frame);
  void DropDecodedFrames();
  void SetStatus(const std::string& text);

  // Live: opens decoders from the HTSP stream list. Returns a description
  // like "H264 1280x720 + AC3", or empty when nothing is playable. Called
  // from the HTSP reader thread.
  std::string StartLiveStreams(const std::vector<HtspStreamInfo>& streams);
  // Live: route a demuxed packet to its decoder (HTSP reader thread).
  void Enqueue(HtspMuxPacket&& pkt);

  // Recording: opens the container and its decoders.
  bool OpenRecording(HtspClient& client, uint32_t dvr_id, std::string& error);
  void CloseRecording();

  static int AvioRead(void* opaque, uint8_t* buf, int buf_size);
  static int64_t AvioSeek(void* opaque, int64_t offset, int whence);

  bool OpenLiveDecoder(const HtspStreamInfo& info, AVCodecContext** out_ctx);

  std::atomic<bool> active_{false};        // subscription/file open
  std::atomic<bool> threads_running_{false};
  std::atomic<Source> source_{Source::None};

  // --- video ---
  int video_stream_ = -1;
  AVCodecContext* vctx_ = nullptr;
  SwsContext* sws_ = nullptr; // lazy, only if decoder output isn't yuv420p
  std::thread vthread_;
  PacketQueue vqueue_;

  std::mutex frames_mutex_;
  std::condition_variable frames_cv_;
  std::deque<TimedFrame> frames_; // decoded, in presentation order
  static constexpr size_t kMaxFrames = 4;

  // --- audio ---
  int audio_stream_ = -1;
  AVCodecContext* actx_ = nullptr;
  AudioResampler resampler_;
  std::thread athread_;
  PacketQueue aqueue_;
  SDL_AudioDeviceID audio_dev_ = 0;
  static constexpr int kAudioRate = kPlayerAudioRate;
  static constexpr int kAudioChannels = kPlayerAudioChannels;
  static constexpr int kAudioBytesPerSec = kPlayerAudioBytesPerSec;

  // --- clock (us) ---
  std::atomic<int64_t> audio_pts_end_{INT64_MIN}; // pts at the end of queued audio
  std::atomic<int64_t> wall_anchor_pts_{INT64_MIN};
  std::atomic<int64_t> wall_anchor_time_{0};
  std::atomic<bool> paused_{false};

  // --- recordings ---
  std::thread dthread_;
  AVFormatContext* fmt_ = nullptr;
  AVIOContext* avio_ = nullptr;
  HtspFile file_;
  int file_video_stream_ = -1; // index within fmt_->streams
  int file_audio_stream_ = -1;
  std::atomic<int64_t> duration_us_{-1};
  // Recordings often start at a non-zero container timestamp; everything
  // downstream works in file-relative microseconds, so this is subtracted on
  // ingest and added back when seeking.
  int64_t file_start_us_ = 0;
  std::atomic<int64_t> seek_target_us_{-1}; // >= 0 = seek pending
  std::atomic<int64_t> last_position_us_{0};
  std::atomic<bool> seekable_{false};
  std::atomic<bool> eof_{false};

  // --- presentation (SDL) ---
  SDL_Renderer* renderer_ = nullptr;
  SDL_Texture* texture_ = nullptr; // IYUV streaming texture
  int tex_w_ = 0, tex_h_ = 0;
  TimedFrame current_; // frame currently on screen (owned)

  mutable std::mutex status_mutex_;
  std::string status_;
};

#endif
