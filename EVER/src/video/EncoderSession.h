#pragma once

#include "SafeQueue.h"
#include "VideoFrameTypes.h"
#include "OpenEXRExporter.h"
#include "FFmpegEncoder.h"
#include "FFmpegTypes.h"

#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <mfidl.h>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <valarray>
#include <vector>
#include <wrl.h>

namespace Encoder {
    class EncoderSession {
    public:
        EncoderSession();
        ~EncoderSession();

        EncoderSession(const EncoderSession&) = delete;
        EncoderSession& operator=(const EncoderSession&) = delete;

        HRESULT createContext(const FFmpeg::FFENCODERCONFIG& config, 
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
                            uint32_t openExrHeight);

        HRESULT enqueueVideoFrame(const D3D11_MAPPED_SUBRESOURCE& subresource);

        HRESULT enqueueExrImage(const Microsoft::WRL::ComPtr<ID3D11DeviceContext>& deviceContext,
                            const Microsoft::WRL::ComPtr<ID3D11Texture2D>& colorTexture,
                            const Microsoft::WRL::ComPtr<ID3D11Texture2D>& depthTexture);

        HRESULT writeVideoFrame(BYTE* data, int32_t length, int rowPitch, LONGLONG presentationTime);

        HRESULT writeAudioFrame(BYTE* data, int32_t length, LONGLONG presentationTime);

        HRESULT finishVideo();

        HRESULT finishAudio();

        HRESULT endSession();

        bool isCapturing = false;

    private:
        struct QueuedVideoFrame {
            std::vector<uint8_t> data;
            int rowPitch = 0;
            int32_t height = 0;
            int64_t frameIndex = 0;
        };

        void videoEncodingWorkerLoop();
        HRESULT encodeQueuedVideoFrame(const QueuedVideoFrame& frame);

        std::unique_ptr<FFmpegEncoder> ffmpegEncoder_;
        FFmpeg::FFVIDEOFRAME videoFrame_;
        FFmpeg::FFAUDIOCHUNK audioChunk_;

        int64_t videoPts_ = 0;
        int64_t audioPts_ = 0;

        bool isVideoFinished_ = false;
        bool isAudioFinished_ = false;
        bool isSessionFinished_ = false;
        bool isBeingDeleted_ = false;

        std::mutex finishMutex_;
        std::mutex endSessionMutex_;
        std::condition_variable endSessionCondition_;

        std::mutex videoQueueMutex_;
        std::condition_variable videoQueueNotEmptyCv_;
        std::condition_variable videoQueueNotFullCv_;
        std::deque<QueuedVideoFrame> videoQueue_;
        std::thread videoEncodingThread_;
        bool videoQueueStopRequested_ = false;
        bool videoWorkerRunning_ = false;
        bool videoWorkerFailed_ = false;
        static constexpr size_t kMaxQueuedVideoFrames = 8;
        int64_t queuedVideoFrames_ = 0;
        int64_t encodedVideoFrames_ = 0;
        int64_t submittedAudioSamples_ = 0;
        int64_t droppedVideoFrames_ = 0;
        int32_t fpsNumerator_ = 0;
        int32_t fpsDenominator_ = 1;

        SafeQueue<FrameQueueItem> videoFrameQueue_;
        SafeQueue<ExrQueueItem> exrImageQueue_;

        std::valarray<uint16_t> motionBlurAccBuffer_;
        std::valarray<uint16_t> motionBlurTempBuffer_;
        std::valarray<uint8_t> motionBlurDestBuffer_;

        bool exportExr_ = false;
        uint64_t exrFrameNumber_ = 0;
        OpenEXRExporter exrExporter_;
        bool isExrEncodingThreadFinished_ = false;
        std::condition_variable exrEncodingThreadFinishedCondition_;
        std::mutex exrEncodingThreadMutex_;
        std::thread exrEncodingThread_;

        int32_t width_ = 0;
        int32_t height_ = 0;
        int32_t framerate_ = 0;
        uint8_t motionBlurSamples_ = 0;
        int32_t audioBlockAlign_ = 0;
        int32_t inputAudioSampleRate_ = 0;
        int32_t inputAudioChannels_ = 0;
        int32_t outputAudioSampleRate_ = 0;
        int32_t outputAudioChannels_ = 0;
        std::string filename_;
        float shutterPosition_ = 0.0f;
    };
}
