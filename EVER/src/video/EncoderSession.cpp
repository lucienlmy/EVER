#pragma warning(push)
#pragma warning(disable : 4244)
#pragma warning(disable : 26812)

#include "EncoderSession.h"
#include "logger.h"
#include "util.h"

#include <cmath>
#include <filesystem>

#pragma warning(pop)

namespace Encoder {
    void EncoderSession::videoEncodingWorkerLoop() {
        PRE();
        LOG(LL_NFO, "EncoderSession video worker started");

        while (true) {
            QueuedVideoFrame frame;
            {
                std::unique_lock<std::mutex> lock(videoQueueMutex_);
                videoQueueNotEmptyCv_.wait(lock, [this] {
                    return videoQueueStopRequested_ || !videoQueue_.empty();
                });

                if (videoQueue_.empty()) {
                    if (videoQueueStopRequested_) {
                        break;
                    }
                    continue;
                }

                frame = std::move(videoQueue_.front());
                videoQueue_.pop_front();
                videoQueueNotFullCv_.notify_one();
            }

            const HRESULT hr = encodeQueuedVideoFrame(frame);
            if (FAILED(hr)) {
                LOG(LL_ERR, "Video worker failed to encode queued frame index=", frame.frameIndex,
                    " hr=", Logger::hex(static_cast<uint32_t>(hr), 8));
                std::lock_guard<std::mutex> lock(videoQueueMutex_);
                videoWorkerFailed_ = true;
            }
        }

        {
            std::lock_guard<std::mutex> lock(videoQueueMutex_);
            videoWorkerRunning_ = false;
        }
        LOG(LL_NFO, "EncoderSession video worker stopped. encodedFrames=", encodedVideoFrames_,
            " droppedFrames=", droppedVideoFrames_);
        POST();
    }

    HRESULT EncoderSession::encodeQueuedVideoFrame(const QueuedVideoFrame& frame) {
        PRE();
        if (frame.data.empty()) {
            LOG(LL_ERR, "encodeQueuedVideoFrame called with empty frame data");
            POST();
            return E_FAIL;
        }

        const int32_t lengthBytes = static_cast<int32_t>(frame.rowPitch * frame.height);
        const HRESULT hr = writeVideoFrame(const_cast<BYTE*>(frame.data.data()),
                                           lengthBytes,
                                           frame.rowPitch,
                                           frame.frameIndex);
        if (SUCCEEDED(hr)) {
            ++encodedVideoFrames_;
        }
        POST();
        return hr;
    }

    EncoderSession::EncoderSession() 
        : videoFrameQueue_(128), 
        exrImageQueue_(16) {
        PRE();
        LOG(LL_NFO, "Opening encoding session: ", reinterpret_cast<uint64_t>(this));
        
        LOG(LL_DBG, "EncoderSession: Initializing FFmpeg encoder");
        ffmpegEncoder_ = std::make_unique<FFmpegEncoder>();
        
        videoFrame_.buffer = nullptr;
        videoFrame_.rowsize = nullptr;
        videoFrame_.planes = 0;
        videoFrame_.width = 0;
        videoFrame_.height = 0;
        videoFrame_.pass = 0;
        memset(videoFrame_.format, 0, sizeof(videoFrame_.format));
        memset(videoFrame_.colorRange, 0, sizeof(videoFrame_.colorRange));
        memset(videoFrame_.colorSpace, 0, sizeof(videoFrame_.colorSpace));
        memset(videoFrame_.colorPrimaries, 0, sizeof(videoFrame_.colorPrimaries));
        memset(videoFrame_.colorTrc, 0, sizeof(videoFrame_.colorTrc));

        audioChunk_.buffer = nullptr;
        audioChunk_.samples = 0;
        audioChunk_.blockSize = 0;
        audioChunk_.planes = 0;
        audioChunk_.sampleRate = 0;
        audioChunk_.layout = FFmpeg::ChannelLayout::Stereo;
        memset(audioChunk_.format, 0, sizeof(audioChunk_.format));

        videoQueueStopRequested_ = false;
        videoWorkerRunning_ = false;
        videoWorkerFailed_ = false;
        queuedVideoFrames_ = 0;
        encodedVideoFrames_ = 0;
        submittedAudioSamples_ = 0;
        droppedVideoFrames_ = 0;
        fpsNumerator_ = 0;
        fpsDenominator_ = 1;
        
        LOG(LL_DBG, "EncoderSession: Initialization complete");
        POST();
    }

    EncoderSession::~EncoderSession() {
        PRE();
        LOG(LL_NFO, "Closing encoding session: ", reinterpret_cast<uint64_t>(this));
        
        isCapturing = false;
        LOG_CALL(LL_DBG, finishVideo());
        LOG_CALL(LL_DBG, finishAudio());
        LOG_CALL(LL_DBG, endSession());
        isBeingDeleted_ = true;
        
        POST();
    }

    HRESULT EncoderSession::createContext(const FFmpeg::FFENCODERCONFIG& config, 
                                        const std::wstring& filename, 
                                        uint32_t width,
                                        uint32_t height, 
                                        const std::string& inputPixelFormat, 
                                        uint32_t fpsNumerator, 
                                        uint32_t fpsDenominator,
                                        uint32_t inputChannels, 
                                        uint32_t inputSampleRate, 
                                        const std::string& inputSampleFormat,
                                        uint32_t inputAlign, 
                                        bool exportOpenExr, 
                                        uint32_t openExrWidth,
                                        uint32_t openExrHeight) {
        PRE();

        LOG(LL_DBG, "EncoderSession::createContext - Starting encoder context creation");
        
        width_ = static_cast<int32_t>(width);
        height_ = static_cast<int32_t>(height);
        fpsNumerator_ = static_cast<int32_t>(fpsNumerator);
        fpsDenominator_ = static_cast<int32_t>(fpsDenominator == 0 ? 1 : fpsDenominator);

        ASSERT_RUNTIME(filename.length() < 255, 
                    "Filename is too long for FFmpeg encoder");
        ASSERT_RUNTIME(inputChannels == 1 || inputChannels == 2 || inputChannels == 6,
                    "Invalid number of audio channels. Only 1 (mono), 2 (stereo), and 6 (5.1) are supported");

        LOG(LL_DBG, "EncoderSession::createContext - Setting FFmpeg configuration");
        REQUIRE(ffmpegEncoder_->SetConfig(config), "Failed to set FFmpeg configuration");

        FFmpeg::ChannelLayout channelLayout = FFmpeg::ChannelLayout::Stereo;
        switch (inputChannels) {
            case 1:
                channelLayout = FFmpeg::ChannelLayout::Mono;
                break;
            case 2:
                channelLayout = FFmpeg::ChannelLayout::Stereo;
                break;
            case 6:
                channelLayout = FFmpeg::ChannelLayout::FivePointOne;
                break;
        }

        LOG(LL_DBG, "EncoderSession::createContext - Preparing encoder info structure");
        
        FFmpeg::FFENCODERINFO encoderInfo{
            .application = L"Extended Video Export Revived",
            .video{
                .enabled = true,
                .width = static_cast<int>(width),
                .height = static_cast<int>(height),
                .timebase = {static_cast<int>(fpsDenominator), static_cast<int>(fpsNumerator)},
                .aspectratio = {1, 1},
                .fieldorder = FFmpeg::FieldOrder::Progressive,
            },
            .audio{
                .enabled = true,
                .samplerate = static_cast<int>(inputSampleRate),
                .channellayout = channelLayout,
                .numberChannels = static_cast<int>(inputChannels),
            },
        };
        
        filename.copy(encoderInfo.filename, std::size(encoderInfo.filename));

        LOG(LL_DBG, "EncoderSession::createContext - Encoder info prepared");
        LOG(LL_DBG, "EncoderSession::createContext - Video: ", encoderInfo.video.width, "x", encoderInfo.video.height, " @ ", fpsNumerator, "/", fpsDenominator);
        LOG(LL_DBG, "EncoderSession::createContext - Audio: ", encoderInfo.audio.samplerate, "Hz, ", encoderInfo.audio.numberChannels, " channels");

        exportExr_ = exportOpenExr;
        if (exportExr_) {
            LOG(LL_DBG, "EncoderSession::createContext - OpenEXR export enabled");
            std::string exrOutputPath = utf8_encode(filename) + ".OpenEXR";
            exrExporter_.initialize(exrOutputPath, openExrWidth, openExrHeight);
        }

        LOG(LL_NFO, "EncoderSession::createContext - Opening FFmpeg encoder");
        REQUIRE(ffmpegEncoder_->Open(encoderInfo), "Failed to open FFmpeg encoder");
        LOG(LL_NFO, "FFmpeg encoder opened successfully");

        LOG(LL_DBG, "EncoderSession::createContext - Initializing video frame structure");
        videoFrame_ = {
            .buffer = new byte*[1],
            .rowsize = new int[1],
            .planes = 1,
            .width = static_cast<int>(width),
            .height = static_cast<int>(height),
            .pass = 1,
        };
        inputPixelFormat.copy(videoFrame_.format, std::size(videoFrame_.format));

        LOG(LL_DBG, "EncoderSession::createContext - Initializing audio chunk structure");
        audioChunk_ = {
            .buffer = new byte*[1],
            .samples = 0,
            .blockSize = static_cast<int>(inputAlign),
            .planes = 1,
            .sampleRate = static_cast<int>(inputSampleRate),
            .layout = channelLayout,
        };
        inputSampleFormat.copy(audioChunk_.format, std::size(audioChunk_.format));

        audioBlockAlign_ = static_cast<int32_t>(inputAlign);
        inputAudioChannels_ = static_cast<int32_t>(inputChannels);
        inputAudioSampleRate_ = static_cast<int32_t>(inputSampleRate);

        isCapturing = true;

        {
            std::lock_guard<std::mutex> lock(videoQueueMutex_);
            videoQueueStopRequested_ = false;
            videoWorkerFailed_ = false;
            videoWorkerRunning_ = true;
            videoQueue_.clear();
            queuedVideoFrames_ = 0;
            encodedVideoFrames_ = 0;
            droppedVideoFrames_ = 0;
        }

        try {
            videoEncodingThread_ = std::thread(&EncoderSession::videoEncodingWorkerLoop, this);
            LOG(LL_NFO, "EncoderSession::createContext - Video worker thread started");
        } catch (const std::exception& ex) {
            LOG(LL_ERR, "EncoderSession::createContext - Failed to start video worker thread: ", ex.what());
            POST();
            return E_FAIL;
        } catch (...) {
            LOG(LL_ERR, "EncoderSession::createContext - Failed to start video worker thread");
            POST();
            return E_FAIL;
        }

        LOG(LL_NFO, "EncoderSession::createContext - Encoder context creation complete");
        POST();
        return S_OK;
    }

    HRESULT EncoderSession::enqueueExrImage(const Microsoft::WRL::ComPtr<ID3D11DeviceContext>& deviceContext,
                                        const Microsoft::WRL::ComPtr<ID3D11Texture2D>& colorTexture,
                                        const Microsoft::WRL::ComPtr<ID3D11Texture2D>& depthTexture) {
        PRE();

        if (isBeingDeleted_) {
            POST();
            return E_FAIL;
        }

        if (!exportExr_) {
            POST();
            return S_OK;
        }

        REQUIRE(exrExporter_.exportFrame(deviceContext, colorTexture, depthTexture, exrFrameNumber_++),
                "Failed to export OpenEXR frame");

        POST();
        return S_OK;
    }

    HRESULT EncoderSession::enqueueVideoFrame(const D3D11_MAPPED_SUBRESOURCE& subresource) {
        PRE();

        if (isBeingDeleted_) {
            POST();
            return E_FAIL;
        }

        if (subresource.pData == nullptr || subresource.RowPitch <= 0 || height_ <= 0) {
            LOG(LL_ERR, "enqueueVideoFrame received invalid mapped resource");
            POST();
            return E_FAIL;
        }

        QueuedVideoFrame frame;
        frame.rowPitch = static_cast<int>(subresource.RowPitch);
        frame.height = height_;
        frame.frameIndex = videoPts_++;

        const size_t frameBytes = static_cast<size_t>(frame.rowPitch) * static_cast<size_t>(frame.height);
        frame.data.resize(frameBytes);
        std::memcpy(frame.data.data(), subresource.pData, frameBytes);

        std::unique_lock<std::mutex> lock(videoQueueMutex_);
        if (videoQueue_.size() >= kMaxQueuedVideoFrames) {
            LOG(LL_DBG, "Video queue full (", videoQueue_.size(), "/", kMaxQueuedVideoFrames,
                ") waiting for encoder worker...");
        }

        videoQueueNotFullCv_.wait(lock, [this] {
            return videoQueueStopRequested_ || videoQueue_.size() < kMaxQueuedVideoFrames;
        });

        if (videoQueueStopRequested_) {
            LOG(LL_WRN, "Video queue stop requested; dropping frame ", frame.frameIndex);
            ++droppedVideoFrames_;
            POST();
            return E_FAIL;
        }

        videoQueue_.push_back(std::move(frame));
        ++queuedVideoFrames_;
        const size_t queueDepth = videoQueue_.size();
        lock.unlock();
        videoQueueNotEmptyCv_.notify_one();

        LOG(LL_TRC, "Queued video frame index=", (videoPts_ - 1), " depth=", queueDepth,
            " maxDepth=", kMaxQueuedVideoFrames);

        POST();
        return S_OK;
    }

    HRESULT EncoderSession::writeVideoFrame(BYTE* data, int32_t length, int rowPitch, LONGLONG presentationTime) {
        PRE();
        
        if (isBeingDeleted_) {
            POST();
            return E_FAIL;
        }

        LOG(LL_TRC, "EncoderSession::writeVideoFrame - Writing video frame, length: ", length, ", rowPitch: ", rowPitch, ", PTS: ", presentationTime);
        
        videoFrame_.buffer[0] = data;
        videoFrame_.rowsize[0] = rowPitch;

        LOG(LL_TRC, "EncoderSession::writeVideoFrame - Sending frame to FFmpeg encoder");
        REQUIRE(ffmpegEncoder_->SendVideoFrame(videoFrame_), "Failed to send video frame to FFmpeg");

        POST();
        return S_OK;
    }

    HRESULT EncoderSession::writeAudioFrame(BYTE* data, int32_t lengthBytes, LONGLONG presentationTime) {
        PRE();

        if (isBeingDeleted_) {
            POST();
            return E_FAIL;
        }

        LOG(LL_TRC, "EncoderSession::writeAudioFrame - Writing audio frame, bytes: ", lengthBytes, ", PTS: ", presentationTime);

        if (audioBlockAlign_ <= 0) {
            LOG(LL_ERR, "EncoderSession::writeAudioFrame - Invalid audio block alignment");
            POST();
            return E_FAIL;
        }

        const int32_t samples = lengthBytes / audioBlockAlign_;
        if (samples <= 0) {
            LOG(LL_ERR, "EncoderSession::writeAudioFrame - Computed non-positive sample count, bytes: ", lengthBytes, " blockAlign: ", audioBlockAlign_);
            POST();
            return E_FAIL;
        }

        audioChunk_.buffer[0] = data;
        audioChunk_.samples = samples;
        submittedAudioSamples_ += samples;

        LOG(LL_TRC, "EncoderSession::writeAudioFrame - Sending audio chunk to FFmpeg encoder");
        REQUIRE(ffmpegEncoder_->SendAudioSampleChunk(audioChunk_), "Failed to send audio chunk to FFmpeg");

        POST();
        return S_OK;
    }

    HRESULT EncoderSession::finishVideo() {
        PRE();
        std::lock_guard<std::mutex> guard(finishMutex_);

        if (!isVideoFinished_) {
            {
                std::lock_guard<std::mutex> lock(videoQueueMutex_);
                videoQueueStopRequested_ = true;
            }
            videoQueueNotEmptyCv_.notify_all();
            videoQueueNotFullCv_.notify_all();

            if (videoEncodingThread_.joinable()) {
                LOG(LL_NFO, "Waiting for video worker to drain queued frames...");
                videoEncodingThread_.join();
            }

            if (exrEncodingThread_.joinable()) {
                exrImageQueue_.enqueue(ExrQueueItem());
                
                std::unique_lock<std::mutex> lock(exrEncodingThreadMutex_);
                while (!isExrEncodingThreadFinished_) {
                    exrEncodingThreadFinishedCondition_.wait(lock);
                }

                exrEncodingThread_.join();
            }

            if (videoFrame_.buffer != nullptr) {
                delete[] videoFrame_.buffer;
                videoFrame_.buffer = nullptr;
            }
            
            if (videoFrame_.rowsize != nullptr) {
                delete[] videoFrame_.rowsize;
                videoFrame_.rowsize = nullptr;
            }

            isVideoFinished_ = true;

            if (videoWorkerFailed_) {
                LOG(LL_ERR, "finishVideo detected video worker encoding failure");
                POST();
                return E_FAIL;
            }
        }

        POST();
        return S_OK;
    }

    HRESULT EncoderSession::finishAudio() {
        PRE();
        std::lock_guard<std::mutex> guard(finishMutex_);

        if (!isAudioFinished_) {
            if (audioChunk_.buffer != nullptr) {
                delete[] audioChunk_.buffer;
                audioChunk_.buffer = nullptr;
            }
            
            isAudioFinished_ = true;
        }

        POST();
        return S_OK;
    }

    HRESULT EncoderSession::endSession() {
        PRE();
        std::lock_guard<std::mutex> lock(endSessionMutex_);

        if (isSessionFinished_ || isBeingDeleted_) {
            POST();
            return S_OK;
        }

        while (true) {
            std::lock_guard<std::mutex> guard(finishMutex_);
            if (isVideoFinished_ && isAudioFinished_) {
                break;
            }
        }

        isCapturing = false;
        LOG(LL_NFO, "Ending encoding session...");

        if (fpsNumerator_ > 0 && fpsDenominator_ > 0 && inputAudioSampleRate_ > 0) {
            const double videoDurationSec = static_cast<double>(encodedVideoFrames_) *
                                            static_cast<double>(fpsDenominator_) /
                                            static_cast<double>(fpsNumerator_);
            const double audioDurationSec = static_cast<double>(submittedAudioSamples_) /
                                            static_cast<double>(inputAudioSampleRate_);
            const double deltaSec = std::fabs(videoDurationSec - audioDurationSec);

            LOG(LL_NFO, "A/V sync telemetry: encodedVideoFrames=", encodedVideoFrames_,
                " submittedAudioSamples=", submittedAudioSamples_,
                " videoDurationSec=", videoDurationSec,
                " audioDurationSec=", audioDurationSec,
                " deltaSec=", deltaSec);

            if (deltaSec > 0.250) {
                LOG(LL_WRN, "A/V sync delta is high (>", 0.250, "s). Check capture cadence and audio replay chunking.");
            }
        }

        if (ffmpegEncoder_) {
            LOG(LL_DBG, "EncoderSession::endSession - Closing FFmpeg encoder");
            LOG_IF_FAILED(ffmpegEncoder_->Close(true), "Failed to close FFmpeg encoder");
        } else {
            LOG(LL_DBG, "FFmpeg encoder instance was never created (audio-only mode)");
        }

        isSessionFinished_ = true;
        endSessionCondition_.notify_all();
        
        LOG(LL_NFO, "Encoding session ended successfully");

        POST();
        return S_OK;
    }
}
