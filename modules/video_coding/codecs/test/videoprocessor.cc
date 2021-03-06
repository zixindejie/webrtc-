/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/video_coding/codecs/test/videoprocessor.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "api/video/i420_buffer.h"
#include "common_types.h"  // NOLINT(build/include)
#include "common_video/h264/h264_common.h"
#include "modules/rtp_rtcp/include/rtp_rtcp_defines.h"
#include "modules/video_coding/codecs/vp8/simulcast_rate_allocator.h"
#include "modules/video_coding/include/video_codec_initializer.h"
#include "modules/video_coding/utility/default_video_bitrate_allocator.h"
#include "rtc_base/checks.h"
#include "rtc_base/timeutils.h"
#include "test/gtest.h"

namespace webrtc {
namespace test {

namespace {
const int kMsToRtpTimestamp = kVideoPayloadTypeFrequency / 1000;

std::unique_ptr<VideoBitrateAllocator> CreateBitrateAllocator(
    TestConfig* config) {
  std::unique_ptr<TemporalLayersFactory> tl_factory;
  if (config->codec_settings.codecType == VideoCodecType::kVideoCodecVP8) {
    tl_factory.reset(new TemporalLayersFactory());
    config->codec_settings.VP8()->tl_factory = tl_factory.get();
  }
  return std::unique_ptr<VideoBitrateAllocator>(
      VideoCodecInitializer::CreateBitrateAllocator(config->codec_settings,
                                                    std::move(tl_factory)));
}

size_t GetMaxNaluSizeBytes(const EncodedImage& encoded_frame,
                           const TestConfig& config) {
  if (config.codec_settings.codecType != kVideoCodecH264)
    return 0;

  std::vector<webrtc::H264::NaluIndex> nalu_indices =
      webrtc::H264::FindNaluIndices(encoded_frame._buffer,
                                    encoded_frame._length);

  RTC_CHECK(!nalu_indices.empty());

  size_t max_size = 0;
  for (const webrtc::H264::NaluIndex& index : nalu_indices)
    max_size = std::max(max_size, index.payload_size);

  return max_size;
}

int GetElapsedTimeMicroseconds(int64_t start_ns, int64_t stop_ns) {
  int64_t diff_us = (stop_ns - start_ns) / rtc::kNumNanosecsPerMicrosec;
  RTC_DCHECK_GE(diff_us, std::numeric_limits<int>::min());
  RTC_DCHECK_LE(diff_us, std::numeric_limits<int>::max());
  return static_cast<int>(diff_us);
}

void ExtractBufferWithSize(const VideoFrame& image,
                           int width,
                           int height,
                           rtc::Buffer* buffer) {
  if (image.width() != width || image.height() != height) {
    EXPECT_DOUBLE_EQ(static_cast<double>(width) / height,
                     static_cast<double>(image.width()) / image.height());
    // Same aspect ratio, no cropping needed.
    rtc::scoped_refptr<I420Buffer> scaled(I420Buffer::Create(width, height));
    scaled->ScaleFrom(*image.video_frame_buffer()->ToI420());

    size_t length =
        CalcBufferSize(VideoType::kI420, scaled->width(), scaled->height());
    buffer->SetSize(length);
    RTC_CHECK_NE(ExtractBuffer(scaled, length, buffer->data()), -1);
    return;
  }

  // No resize.
  size_t length =
      CalcBufferSize(VideoType::kI420, image.width(), image.height());
  buffer->SetSize(length);
  RTC_CHECK_NE(ExtractBuffer(image, length, buffer->data()), -1);
}

}  // namespace

VideoProcessor::VideoProcessor(webrtc::VideoEncoder* encoder,
                               webrtc::VideoDecoder* decoder,
                               FrameReader* analysis_frame_reader,
                               const TestConfig& config,
                               Stats* stats,
                               IvfFileWriter* encoded_frame_writer,
                               FrameWriter* decoded_frame_writer)
    : config_(config),
      encoder_(encoder),
      decoder_(decoder),
      bitrate_allocator_(CreateBitrateAllocator(&config_)),
      encode_callback_(this),
      decode_callback_(this),
      analysis_frame_reader_(analysis_frame_reader),
      encoded_frame_writer_(encoded_frame_writer),
      decoded_frame_writer_(decoded_frame_writer),
      last_inputed_frame_num_(0),
      last_encoded_frame_num_(0),
      last_decoded_frame_num_(0),
      num_encoded_frames_(0),
      num_decoded_frames_(0),
      last_decoded_frame_buffer_(analysis_frame_reader->FrameLength()),
      stats_(stats) {
  RTC_DCHECK(encoder);
  RTC_DCHECK(decoder);
  RTC_DCHECK(analysis_frame_reader);
  RTC_DCHECK(stats);

  // Setup required callbacks for the encoder and decoder.
  RTC_CHECK_EQ(encoder_->RegisterEncodeCompleteCallback(&encode_callback_),
               WEBRTC_VIDEO_CODEC_OK);
  RTC_CHECK_EQ(decoder_->RegisterDecodeCompleteCallback(&decode_callback_),
               WEBRTC_VIDEO_CODEC_OK);

  // Initialize the encoder and decoder.
  RTC_CHECK_EQ(encoder_->InitEncode(&config_.codec_settings,
                                    static_cast<int>(config_.NumberOfCores()),
                                    config_.max_payload_size_bytes),
               WEBRTC_VIDEO_CODEC_OK);
  RTC_CHECK_EQ(decoder_->InitDecode(&config_.codec_settings,
                                    static_cast<int>(config_.NumberOfCores())),
               WEBRTC_VIDEO_CODEC_OK);
}

VideoProcessor::~VideoProcessor() {
  RTC_DCHECK_CALLED_SEQUENTIALLY(&sequence_checker_);

  RTC_CHECK_EQ(encoder_->Release(), WEBRTC_VIDEO_CODEC_OK);
  RTC_CHECK_EQ(decoder_->Release(), WEBRTC_VIDEO_CODEC_OK);

  encoder_->RegisterEncodeCompleteCallback(nullptr);
  decoder_->RegisterDecodeCompleteCallback(nullptr);
}

void VideoProcessor::ProcessFrame() {
  RTC_DCHECK_CALLED_SEQUENTIALLY(&sequence_checker_);
  const size_t frame_number = last_inputed_frame_num_++;

  // Get frame from file.
  rtc::scoped_refptr<I420BufferInterface> buffer(
      analysis_frame_reader_->ReadFrame());
  RTC_CHECK(buffer) << "Tried to read too many frames from the file.";

  size_t rtp_timestamp =
      (frame_number > 0) ? input_frames_[frame_number - 1]->timestamp() : 0;
  rtp_timestamp +=
      kVideoPayloadTypeFrequency / config_.codec_settings.maxFramerate;

  input_frames_[frame_number] = rtc::MakeUnique<VideoFrame>(
      buffer, static_cast<uint32_t>(rtp_timestamp),
      static_cast<int64_t>(rtp_timestamp / kMsToRtpTimestamp),
      webrtc::kVideoRotation_0);

  std::vector<FrameType> frame_types = config_.FrameTypeForFrame(frame_number);

  // Create frame statistics object used for aggregation at end of test run.
  FrameStatistic* frame_stat = stats_->AddFrame(rtp_timestamp);

  // For the highest measurement accuracy of the encode time, the start/stop
  // time recordings should wrap the Encode call as tightly as possible.
  frame_stat->encode_start_ns = rtc::TimeNanos();
  frame_stat->encode_return_code =
      encoder_->Encode(*input_frames_[frame_number], nullptr, &frame_types);
}

void VideoProcessor::SetRates(size_t bitrate_kbps, size_t framerate_fps) {
  RTC_DCHECK_CALLED_SEQUENTIALLY(&sequence_checker_);
  config_.codec_settings.maxFramerate = static_cast<uint32_t>(framerate_fps);
  bitrate_allocation_ = bitrate_allocator_->GetAllocation(
      static_cast<uint32_t>(bitrate_kbps * 1000),
      static_cast<uint32_t>(framerate_fps));
  const int set_rates_result = encoder_->SetRateAllocation(
      bitrate_allocation_, static_cast<uint32_t>(framerate_fps));
  RTC_DCHECK_GE(set_rates_result, 0)
      << "Failed to update encoder with new rate " << bitrate_kbps << ".";
}

void VideoProcessor::FrameEncoded(webrtc::VideoCodecType codec,
                                  const EncodedImage& encoded_image) {
  RTC_DCHECK_CALLED_SEQUENTIALLY(&sequence_checker_);

  // For the highest measurement accuracy of the encode time, the start/stop
  // time recordings should wrap the Encode call as tightly as possible.
  int64_t encode_stop_ns = rtc::TimeNanos();

  if (config_.encoded_frame_checker) {
    config_.encoded_frame_checker->CheckEncodedFrame(codec, encoded_image);
  }

  FrameStatistic* frame_stat =
      stats_->GetFrameWithTimestamp(encoded_image._timeStamp);

  // Ensure strict monotonicity.
  const size_t frame_number = frame_stat->frame_number;
  if (num_encoded_frames_ > 0) {
    RTC_CHECK_GT(frame_number, last_encoded_frame_num_);
  }

  last_encoded_frame_num_ = frame_number;

  // Update frame statistics.
  frame_stat->encode_time_us =
      GetElapsedTimeMicroseconds(frame_stat->encode_start_ns, encode_stop_ns);
  frame_stat->encoding_successful = true;
  frame_stat->encoded_frame_size_bytes = encoded_image._length;
  frame_stat->frame_type = encoded_image._frameType;
  frame_stat->temporal_layer_idx = config_.TemporalLayerForFrame(frame_number);
  frame_stat->qp = encoded_image.qp_;
  frame_stat->target_bitrate_kbps =
      bitrate_allocation_.GetSpatialLayerSum(0) / 1000;
  frame_stat->max_nalu_size_bytes = GetMaxNaluSizeBytes(encoded_image, config_);

  // For the highest measurement accuracy of the decode time, the start/stop
  // time recordings should wrap the Decode call as tightly as possible.
  frame_stat->decode_start_ns = rtc::TimeNanos();
  frame_stat->decode_return_code =
      decoder_->Decode(encoded_image, false, nullptr);

  if (encoded_frame_writer_) {
    RTC_CHECK(encoded_frame_writer_->WriteFrame(encoded_image, codec));
  }

  ++num_encoded_frames_;
}

void VideoProcessor::FrameDecoded(const VideoFrame& decoded_frame) {
  RTC_DCHECK_CALLED_SEQUENTIALLY(&sequence_checker_);

  // For the highest measurement accuracy of the decode time, the start/stop
  // time recordings should wrap the Decode call as tightly as possible.
  int64_t decode_stop_ns = rtc::TimeNanos();

  // Update frame statistics.
  FrameStatistic* frame_stat =
      stats_->GetFrameWithTimestamp(decoded_frame.timestamp());
  frame_stat->decoded_width = decoded_frame.width();
  frame_stat->decoded_height = decoded_frame.height();
  frame_stat->decode_time_us =
      GetElapsedTimeMicroseconds(frame_stat->decode_start_ns, decode_stop_ns);
  frame_stat->decoding_successful = true;

  // Ensure strict monotonicity.
  const size_t frame_number = frame_stat->frame_number;
  if (num_decoded_frames_ > 0) {
    RTC_CHECK_GT(frame_number, last_decoded_frame_num_);
  }

  // Check if the codecs have resized the frame since previously decoded frame.
  if (frame_number > 0) {
    if (decoded_frame_writer_ && num_decoded_frames_ > 0) {
      // For dropped/lost frames, write out the last decoded frame to make it
      // look like a freeze at playback.
      const size_t num_dropped_frames =
          frame_number - last_decoded_frame_num_ - 1;
      for (size_t i = 0; i < num_dropped_frames; i++) {
        WriteDecodedFrameToFile(&last_decoded_frame_buffer_);
      }
    }
  }
  last_decoded_frame_num_ = frame_number;

  // Skip quality metrics calculation to not affect CPU usage.
  if (!config_.measure_cpu) {
    frame_stat->psnr =
        I420PSNR(input_frames_[frame_number].get(), &decoded_frame);
    frame_stat->ssim =
        I420SSIM(input_frames_[frame_number].get(), &decoded_frame);
  }

  // Delay erasing of input frames by one frame. The current frame might
  // still be needed for other simulcast stream or spatial layer.
  if (frame_number > 0) {
    auto input_frame_erase_to = input_frames_.lower_bound(frame_number - 1);
    input_frames_.erase(input_frames_.begin(), input_frame_erase_to);
  }

  if (decoded_frame_writer_) {
    ExtractBufferWithSize(decoded_frame, config_.codec_settings.width,
                          config_.codec_settings.height,
                          &last_decoded_frame_buffer_);
    WriteDecodedFrameToFile(&last_decoded_frame_buffer_);
  }

  ++num_decoded_frames_;
}

void VideoProcessor::WriteDecodedFrameToFile(rtc::Buffer* buffer) {
  RTC_DCHECK_EQ(buffer->size(), decoded_frame_writer_->FrameLength());
  RTC_CHECK(decoded_frame_writer_->WriteFrame(buffer->data()));
}

}  // namespace test
}  // namespace webrtc
