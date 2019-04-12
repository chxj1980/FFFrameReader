/**
 * Copyright 2019 Matthew Oliver
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "FFFRStream.h"

using namespace std;

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/log.h>
}

namespace FfFrameReader {
Stream::Stream(FormatContextPtr& formatContext, const int32_t streamID, CodecContextPtr& codecContext,
    const uint32_t bufferLength) noexcept
    : m_bufferLength(bufferLength)
    , m_formatContext(move(formatContext))
    , m_index(streamID)
    , m_codecContext(move(codecContext))
{
    // Allocate ping and pong buffers
    m_bufferPing.reserve(m_bufferLength);
    m_bufferPong.reserve(m_bufferLength);

    // Set stream start time and numbers of frames
    m_startTimeStamp = getStreamStartTime();
    m_totalFrames = getStreamFrames();
    m_totalDuration = getStreamDuration();
}

Stream::FormatContextPtr::FormatContextPtr(AVFormatContext* formatContext) noexcept
    : m_formatContext(formatContext, [](AVFormatContext* p) { avformat_close_input(&p); })
{}

AVFormatContext* Stream::FormatContextPtr::operator->() const noexcept
{
    return m_formatContext.get();
}

AVFormatContext* Stream::FormatContextPtr::get() const noexcept
{
    return m_formatContext.get();
}

Stream::CodecContextPtr::CodecContextPtr(AVCodecContext* codecContext) noexcept
    : m_codecContext(codecContext, avcodec_close)
{}

AVCodecContext* Stream::CodecContextPtr::operator->() const noexcept
{
    return m_codecContext.get();
}

AVCodecContext* Stream::CodecContextPtr::get() const noexcept
{
    return m_codecContext.get();
}

uint32_t Stream::getWidth() const noexcept
{
    return m_formatContext->streams[m_index]->codecpar->width;
}

uint32_t Stream::getHeight() const noexcept
{
    return m_formatContext->streams[m_index]->codecpar->height;
}

double Stream::getAspectRatio() const noexcept
{
    if (m_formatContext->streams[m_index]->display_aspect_ratio.num) {
        return av_q2d(m_formatContext->streams[m_index]->display_aspect_ratio);
    }
    return static_cast<double>(getWidth()) / static_cast<double>(getHeight());
}

int64_t Stream::getTotalFrames() const noexcept
{
    return m_totalFrames;
}

int64_t Stream::getDuration() const noexcept
{
    return m_totalDuration;
}

double Stream::getFrameRate() const noexcept
{
    return av_q2d(m_formatContext->streams[m_index]->r_frame_rate);
}

int64_t Stream::getFrameTime() const noexcept
{
    return frameToTime(1);
}

variant<bool, shared_ptr<Frame>> Stream::peekNextFrame() noexcept
{
    lock_guard<recursive_mutex> lock(m_mutex);
    // Check if we actually have any frames in the current buffer
    if (m_bufferPingHead == m_bufferPing.size()) {
        // TODO: Async decode of next block, should start once reached the last couple of frames in a buffer
        // The swap buffer only should occur when ping buffer is exhausted and pong decode has completed
        if (!decodeNextBlock()) {
            return false;
        }
        // Swap ping and pong buffer
        swap(m_bufferPing, m_bufferPong);
        m_bufferPingHead = 0;
        // Reset the pong buffer
        m_bufferPong.resize(0);
        // Check if there are any new frames or we reached EOF
        if (m_bufferPing.size() == 0) {
            av_log(nullptr, AV_LOG_ERROR, "Cannot get a new frame, End of file has been reached.\n");
            return false;
        }
    }
    // Get frame from ping buffer
    return m_bufferPing[m_bufferPingHead];
}

variant<bool, shared_ptr<Frame>> Stream::getNextFrame() noexcept
{
    auto ret = peekNextFrame();
    if (ret.index() == 0) {
        return false;
    }
    // Remove the frame from list
    popFrame();
    return ret;
}

variant<bool, vector<shared_ptr<Frame>>> Stream::getNextFrameSequence(const vector<int64_t>& frameSequence) noexcept
{
    // Note: for best performance when using this the buffer size should be small enough to not waste to much memeory
    lock_guard<recursive_mutex> lock(m_mutex);
    vector<shared_ptr<Frame>> ret;
    int64_t start = 0;
    for (const auto& i : frameSequence) {
        if (i < start) {
            // Invalid sequence list
            av_log(nullptr, AV_LOG_ERROR,
                "Invalid sequence list passed to getNextFrameSequence(). Sequences in the list must be in ascending order.\n");
            return false;
        }
        // Remove all frames until first in sequence
        for (int64_t j = start; j < i; j++) {
            // Must peek to check there is actually a new frame
            auto err = peekNextFrame();
            if (err.index() == 0) {
                return false;
            }
            popFrame();
        }
        auto frame = getNextFrame();
        if (frame.index() == 0) {
            return false;
        }
        ret.push_back(get<1>(frame));
        start = i + 1;
    }
    return ret;
}

bool Stream::seek(const int64_t timeStamp) noexcept
{
    return seekInternal(timeStamp, false);
}

bool Stream::seekFrame(const int64_t frame) noexcept
{
    return seekFrameInternal(frame, false);
}

int64_t Stream::timeToTimeStamp(const int64_t time) const noexcept
{
    // Rescale a timestamp that is stored in microseconds (AV_TIME_BASE) to the stream timebase
    return m_startTimeStamp +
        av_rescale_q(time, av_make_q(1, AV_TIME_BASE), m_formatContext->streams[m_index]->time_base);
}

int64_t Stream::timeStampToTime(const int64_t timeStamp) const noexcept
{
    // Perform opposite operation to timeToTimeStamp
    return av_rescale_q(
        timeStamp - m_startTimeStamp, m_formatContext->streams[m_index]->time_base, av_make_q(1, AV_TIME_BASE));
}

int64_t Stream::frameToTimeStamp(const int64_t frame) const noexcept
{
    return m_startTimeStamp +
        av_rescale_q(frame, av_inv_q(m_formatContext->streams[m_index]->r_frame_rate),
            m_formatContext->streams[m_index]->time_base);
}

int64_t Stream::timeStampToFrame(const int64_t timeStamp) const noexcept
{
    return av_rescale_q(timeStamp - m_startTimeStamp, m_formatContext->streams[m_index]->r_frame_rate,
        av_inv_q(m_formatContext->streams[m_index]->time_base));
}

int64_t Stream::frameToTime(const int64_t frame) const noexcept
{
    return av_rescale_q(frame, av_make_q(AV_TIME_BASE, 1), m_formatContext->streams[m_index]->r_frame_rate);
}

int64_t Stream::timeToFrame(const int64_t time) const noexcept
{
    return av_rescale_q(time, av_make_q(1, AV_TIME_BASE), av_inv_q(m_formatContext->streams[m_index]->r_frame_rate));
}

bool Stream::decodeNextBlock() noexcept
{
    // TODO: If we are using async decode then this needs to just return if a decode is already running

    // Reset the pong buffer
    m_bufferPong.resize(0);

    // Decode the next buffer sequence
    AVPacket packet;
    Frame::FramePtr frame;
    av_init_packet(&packet);
    while (true) {
        // This may or may not be a keyframe, So we just start decoding packets until we receive a valid frame
        int32_t ret = av_read_frame(m_formatContext.get(), &packet);
        if (ret < 0) {
            if (ret != AVERROR_EOF) {
                char buffer[AV_ERROR_MAX_STRING_SIZE];
                av_log(nullptr, AV_LOG_ERROR, "Failed to retrieve new frame: %s\n",
                    av_make_error_string(buffer, AV_ERROR_MAX_STRING_SIZE, ret));
                return false;
            }
            return true;
        }

        if (m_index == packet.stream_index) {
            ret = avcodec_send_packet(m_codecContext.get(), &packet);
            av_packet_unref(&packet);
            if (ret < 0) {
                char buffer[AV_ERROR_MAX_STRING_SIZE];
                av_log(nullptr, AV_LOG_ERROR, "Failed to send packet to decoder: %s\n",
                    av_make_error_string(buffer, AV_ERROR_MAX_STRING_SIZE, ret));
                return false;
            }

            while (true) {
                if (*frame == nullptr) {
                    *frame = av_frame_alloc();
                    if (*frame == nullptr) {
                        av_log(nullptr, AV_LOG_ERROR, "Failed to allocate new frame\n");
                        return false;
                    }
                }

                ret = avcodec_receive_frame(m_codecContext.get(), *frame);
                if (ret < 0) {
                    av_frame_unref(*frame);
                    if ((ret == AVERROR(EAGAIN)) || (ret == AVERROR_EOF)) {
                        // We allow for more frames to be returned than requested to ensure that the decoder has been
                        // flushed
                        if (m_bufferPong.size() >= m_bufferLength) {
                            // TODO: Check all the timestamps in the buffer to make sure they are sorted correctly
                            // This may require a buffer overflow area as the last few frames from one buffer should be
                            // added to the start of the next buffer in case they need sorting.
                            return true;
                        }
                        break;
                    }
                    char buffer[AV_ERROR_MAX_STRING_SIZE];
                    av_log(nullptr, AV_LOG_ERROR, "Failed to receive decoded frame: %s\n",
                        av_make_error_string(buffer, AV_ERROR_MAX_STRING_SIZE, ret));
                    return false;
                }

                // Calculate time stamp for frame
                const auto timeStamp = timeStampToTime(frame->best_effort_timestamp);
                const auto frameNum = timeStampToFrame(frame->best_effort_timestamp);

                // Add the new frame to the pong buffer
                m_bufferPong.emplace_back(make_shared<Frame>(frame, timeStamp, frameNum));
            }
        } else {
            av_packet_unref(&packet);
        }

        // TODO: The maximum number of frames that are needed to get a valid frame is calculated using getCodecDelay().
        // If more than that are passed without a returned frame then an error has occured.
    }
}

void Stream::popFrame() noexcept
{
    if (m_bufferPingHead >= m_bufferPing.size()) {
        av_log(nullptr, AV_LOG_ERROR, "No more frames to pop\n");
        return;
    }
    // Release reference and pop frame
    m_bufferPing[m_bufferPingHead++] = make_shared<Frame>();
}

bool Stream::seekInternal(const int64_t timeStamp, const bool recursed) noexcept
{
    lock_guard<recursive_mutex> lock(m_mutex);
    // Check if we actually have any frames in the current buffer
    if (m_bufferPing.size() > 0) {
        // Check if the frame is in the current buffer
        if ((m_bufferPingHead < m_bufferPing.size()) && (timeStamp >= m_bufferPing[m_bufferPingHead]->getTimeStamp()) &&
            (timeStamp <= m_bufferPing.back()->getTimeStamp())) {
            // Dump all frames before requested one
            while (true) {
                // Get next frame
                auto ret = peekNextFrame();
                if (ret.index() == 0) {
                    return false;
                }
                // Check if we have found our requested time stamp
                const auto frame = get<1>(ret);
                if (timeStamp <= frame->getTimeStamp()) {
                    break;
                }
                // Check if the timestamp does not exactly match but is within the timestamp range of the next frame
                if ((timeStamp > frame->getTimeStamp()) && (timeStamp < (frame->getTimeStamp() + getFrameTime()))) {
                    break;
                }
                // Remove frames from ping buffer
                popFrame();
            }
            return true;
        }

        // Check if this is a forward seek within some predefined small range. If so then just continue reading
        // packets from the current position into buffer.
        if (timeStamp > m_bufferPing.back()->getTimeStamp()) {
            // Forward decode if within some predefined range of existing point
            constexpr int64_t forwardRange = 25;
            const auto timeRange = frameToTime(forwardRange);
            if (timeStamp <= m_bufferPing.back()->getTimeStamp() + timeRange) {
                // Loop through until the requested timestamp is found (or nearest timestamp rounded up if exact match
                // could not be found). Discard all frames occuring before timestamp

                // Clean out current buffer
                m_bufferPing.resize(0);
                m_bufferPingHead = 0;

                // Decode the next block of frames
                if (peekNextFrame().index() == 0) {
                    return false;
                }

                // Search through buffer until time stamp is found
                return seekInternal(timeStamp, true);
            }
        }
    }

    // If we have recursed and still havnt found the frame then we never will
    if (recursed) {
        av_log(nullptr, AV_LOG_ERROR, "Failed to seek to specified time stamp %" PRId64 "\n", timeStamp);
        return false;
    }

    // Seek to desired timestamp
    avcodec_flush_buffers(m_codecContext.get());
    const auto localTimeStamp = timeToTimeStamp(timeStamp) + m_startTimeStamp;
    const auto err = avformat_seek_file(m_formatContext.get(), m_index, INT64_MIN, localTimeStamp, localTimeStamp, 0);
    if (err < 0) {
        char buffer[AV_ERROR_MAX_STRING_SIZE];
        av_log(nullptr, AV_LOG_ERROR, "Failed seeking to specified time stamp %" PRId64 ": %s\n", timeStamp,
            av_make_error_string(buffer, AV_ERROR_MAX_STRING_SIZE, err));
        return false;
    }

    // Clean out current buffer
    m_bufferPing.resize(0);
    m_bufferPingHead = 0;

    // Decode the next block of frames
    if (peekNextFrame().index() == 0) {
        return false;
    }

    // Search through buffer until time stamp is found
    return seekInternal(timeStamp, true);
}

bool Stream::seekFrameInternal(const int64_t frame, const bool recursed) noexcept
{
    lock_guard<recursive_mutex> lock(m_mutex);
    // Check if we actually have any frames in the current buffer
    if (m_bufferPing.size() > 0) {
        // Check if the frame is in the current buffer
        if ((m_bufferPingHead < m_bufferPing.size()) && (frame >= m_bufferPing[m_bufferPingHead]->getFrameNumber()) &&
            (frame <= m_bufferPing.back()->getFrameNumber())) {
            // Dump all frames before requested one
            while (true) {
                // Get next frame
                auto ret = peekNextFrame();
                if (ret.index() == 0) {
                    return false;
                }
                // Check if we have found our requested frame
                if (frame <= get<1>(ret)->getFrameNumber()) {
                    break;
                }
                // Remove frames from ping buffer
                popFrame();
            }
            return true;
        }

        // Check if this is a forward seek within some predefined small range. If so then just continue reading
        // packets from the current position into buffer.
        if (frame > m_bufferPing.back()->getFrameNumber()) {
            // Loop through until the requested frame is found.
            // Forward decode if less than or equal to 2 buffer lengths
            const auto frameRange = m_bufferLength * 2;
            if (frame <= m_bufferPing.back()->getFrameNumber() + frameRange) {
                while (true) {
                    auto ret = peekNextFrame();
                    if (ret.index() == 0) {
                        return false;
                    }
                    // Check if we have found our requested time stamp
                    if (frame <= get<1>(ret)->getFrameNumber()) {
                        break;
                    }
                    // Remove frames from ping buffer
                    popFrame();
                }
                return true;
            }
        }
    }

    // If we have recursed and still havnt found the frame then we never will
    if (recursed || !m_frameSeekSupported) {
        if (m_frameSeekSupported) {
            m_frameSeekSupported = false;
            av_log(nullptr, AV_LOG_ERROR,
                "Failed to seek to specified frame %" PRId64 " (retrying using timestamp based seek)\n", frame);
        } else if (recursed) {
            return false;
        }

        // Try and seek just using a timestamp
        return seek(frameToTime(frame));
    }

    // Seek to desired timestamp
    avcodec_flush_buffers(m_codecContext.get());
    const auto frameInternal = frame + timeStampToFrame(m_startTimeStamp);
    const auto err =
        avformat_seek_file(m_formatContext.get(), m_index, INT64_MIN, frameInternal, frameInternal, AVSEEK_FLAG_FRAME);
    if (err < 0) {
        m_frameSeekSupported = false;
        char buffer[AV_ERROR_MAX_STRING_SIZE];
        av_log(nullptr, AV_LOG_ERROR,
            "Failed to seek to specified frame %" PRId64 ": %s (retrying using timestamp based seek)\n", frame,
            av_make_error_string(buffer, AV_ERROR_MAX_STRING_SIZE, err));

        // Try and seek just using a timestamp
        return seek(frameToTime(frame));
    }

    // Clean out current buffer
    m_bufferPing.resize(0);
    m_bufferPingHead = 0;

    // Decode the next block of frames
    if (peekNextFrame().index() == 0) {
        return false;
    }

    // Search through buffer until time stamp is found
    return seekFrameInternal(frame, true);
}

int32_t Stream::getCodecDelay() const noexcept
{
    return std::max(((m_codecContext->codec->capabilities & AV_CODEC_CAP_DELAY) ? m_codecContext->delay : 0) +
            m_codecContext->has_b_frames,
        1);
}

int64_t Stream::getStreamStartTime() const noexcept
{
    // First check if the stream has a start timeStamp
    AVStream* stream = m_formatContext->streams[m_index];
    if (stream->start_time != int64_t(AV_NOPTS_VALUE)) {
        return stream->start_time;
    }
    // Seek to the first frame in the video to get information directly from it
    avcodec_flush_buffers(m_codecContext.get());
    int64_t startDts = 0LL;
    if (stream->first_dts != int64_t(AV_NOPTS_VALUE)) {
        startDts = std::min(startDts, stream->first_dts);
    }
    if (av_seek_frame(m_formatContext.get(), m_index, startDts, AVSEEK_FLAG_BACKWARD) < 0) {
        av_log(nullptr, AV_LOG_ERROR, "Failed to determine stream start time\n");
        return 0;
    }
    AVPacket packet;
    av_init_packet(&packet);
    // Read frames until we get one for the video stream that contains a valid PTS or DTS.
    auto startTimeStamp = int64_t(AV_NOPTS_VALUE);
    const auto maxPackets = getCodecDelay();
    // Loop through multiple packets to take into account b-frame reordering issues
    for (int32_t i = 0; i < maxPackets;) {
        if (av_read_frame(m_formatContext.get(), &packet) < 0) {
            return 0;
        }
        if (packet.stream_index == m_index) {
            // Get the Presentation time stamp for the packet, if this value is not set then try the Decompression time
            // stamp
            auto pts = packet.pts;
            if (pts == int64_t(AV_NOPTS_VALUE)) {
                pts = packet.dts;
            }
            if ((pts != int64_t(AV_NOPTS_VALUE)) &&
                ((pts < startTimeStamp) || (startTimeStamp == int64_t(AV_NOPTS_VALUE)))) {
                startTimeStamp = pts;
            }
            ++i;
        }
        av_packet_unref(&packet);
    }
    // Seek back to start of file so future reads continue back at start
    av_seek_frame(m_formatContext.get(), m_index, startDts, AVSEEK_FLAG_BACKWARD);
    return (startTimeStamp != int64_t(AV_NOPTS_VALUE)) ? startTimeStamp : 0;
}

int64_t Stream::getStreamFrames() const noexcept
{
    // First try and get the format duration if specified. For some formats this durations can override the duration
    // specified within each stream which is why it should be checked first.
    AVStream* stream = m_formatContext->streams[m_index];
    if (m_formatContext->duration > 0) {
        const int64_t frames =
            av_rescale_q(m_formatContext->duration, stream->r_frame_rate, av_inv_q(av_make_q(1, AV_TIME_BASE)));
        // Since duration is stored in time base integer format there may have been some rounding performed when
        // calculating this value. We check for this by comparing to the number of frames reported by the stream and if
        // they are within 1 frame of each other then use the stream frame count value.
        if (abs(frames - stream->nb_frames) > 1) {
            return frames - timeStampToFrame(m_startTimeStamp * 2); //*2 To avoid the minus in timeStampToFrame
        }
    }

    // Check if the number of frames is specified in the stream
    if (stream->nb_frames > 0) {
        return stream->nb_frames - timeStampToFrame(m_startTimeStamp * 2);
    }

    // Attempt to calculate from stream duration, time base and fps
    if (stream->duration > 0) {
        return timeStampToFrame(int64_t(stream->duration));
    }

    // If we are at this point then the only option is to scan the entire file and check the DTS/PTS.
    int64_t foundTimeStamp = m_startTimeStamp;

    // Seek last key-frame.
    avcodec_flush_buffers(m_codecContext.get());
    if (av_seek_frame(m_formatContext.get(), m_index, frameToTimeStamp(1UL << 29UL), AVSEEK_FLAG_BACKWARD) < 0) {
        av_log(nullptr, AV_LOG_ERROR, "Failed to determine number of frames in stream\n");
        return 0;
    }

    // Read up to last frame, extending max PTS for every valid PTS value found for the video stream.
    AVPacket packet;
    av_init_packet(&packet);
    while (av_read_frame(m_formatContext.get(), &packet) >= 0) {
        if (packet.stream_index == m_index) {
            auto found = packet.dts;
            if (found != int64_t(AV_NOPTS_VALUE)) {
                found = packet.pts;
            }
            if (found > foundTimeStamp) {
                foundTimeStamp = found;
            }
        }
        av_packet_unref(&packet);
    }

    // Seek back to start of file so future reads continue back at start
    av_seek_frame(m_formatContext.get(), m_index, 0, 0);

    // The detected value is the index of the last frame so the total frames is 1 more than this.
    return 1 + timeStampToFrame(foundTimeStamp);
}

int64_t Stream::getStreamDuration() const noexcept
{
    // First try and get the format duration if specified. For some formats this durations can override the duration
    // specified within each stream which is why it should be checked first.
    AVStream* stream = m_formatContext->streams[m_index];
    if (m_formatContext->duration > 0) {
        return m_formatContext->duration -
            timeStampToTime(m_startTimeStamp * 2); //*2 To avoid the minus in timeStampToTime
    }

    // Check if the duration is specified in the stream
    if (stream->duration > 0) {
        return timeStampToTime(stream->duration);
    }

    // If we are at this point then the only option is to scan the entire file and check the DTS/PTS.
    int64_t foundTimeStamp = m_startTimeStamp;

    // Seek last key-frame.
    avcodec_flush_buffers(m_codecContext.get());
    if (av_seek_frame(m_formatContext.get(), m_index, frameToTimeStamp(1UL << 29UL), AVSEEK_FLAG_BACKWARD) < 0) {
        av_log(nullptr, AV_LOG_ERROR, "Failed to determine stream duration\n");
        return 0;
    }

    // Read up to last frame, extending max PTS for every valid PTS value found for the video stream.
    AVPacket packet;
    av_init_packet(&packet);
    while (av_read_frame(m_formatContext.get(), &packet) >= 0) {
        if (packet.stream_index == m_index) {
            auto found = packet.dts;
            if (found != int64_t(AV_NOPTS_VALUE)) {
                found = packet.pts;
            }
            if (found > foundTimeStamp) {
                foundTimeStamp = found;
            }
        }
        av_packet_unref(&packet);
    }

    // Seek back to start of file so future reads continue back at start
    av_seek_frame(m_formatContext.get(), m_index, 0, 0);

    // The detected value is timestamp of the last detected packet plus its display time.
    return timeStampToTime(foundTimeStamp) + frameToTime(1);
}
} // namespace FfFrameReader