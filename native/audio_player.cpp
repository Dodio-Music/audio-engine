#include <napi.h>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <thread>
#include <vector>

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

class AudioPlayer : public Napi::ObjectWrap<AudioPlayer> {
public:
    static Napi::Object Init(Napi::Env env, Napi::Object exports);
    AudioPlayer(const Napi::CallbackInfo& info);
    ~AudioPlayer();

private:
    static Napi::FunctionReference constructor;

    static void DataCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount);

    Napi::Value InitDevice(const Napi::CallbackInfo& info);
    Napi::Value Play(const Napi::CallbackInfo& info);
    Napi::Value Stop(const Napi::CallbackInfo& info);
    Napi::Value Write(const Napi::CallbackInfo& info);
    Napi::Value GetState(const Napi::CallbackInfo& info);
    Napi::Value Pause(const Napi::CallbackInfo& info);
    Napi::Value Flush(const Napi::CallbackInfo& info);

    void FeederLoop();

    ma_pcm_rb rb_{};
    bool rbInited_ = false;

    std::atomic<uint64_t> underrunCount_{0};
    ma_uint32 bufferFrameCapacity = 0;

    ma_device device_{};
    bool deviceInited_ = false;

    enum class TransportState : uint32_t {
        Stopped = 0,
        Playing = 1,
        Paused = 2
    };
    std::atomic<TransportState> transportState_{TransportState::Stopped};

    uint32_t sampleRate_ = 0;
    uint32_t channels_ = 0;

    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::deque<std::vector<uint8_t>> pcmQueue_;
    size_t queueReadOffset_ = 0;
    size_t queuedBytes_ = 0;
    size_t maxQueuedBytes_ = 0;

    std::atomic<bool> eos_{false};
    std::atomic<bool> shuttingDown_{false};
    std::thread feederThread_;
};

Napi::FunctionReference AudioPlayer::constructor;

AudioPlayer::AudioPlayer(const Napi::CallbackInfo& info) : Napi::ObjectWrap<AudioPlayer>(info) {}

AudioPlayer::~AudioPlayer() {
    shuttingDown_.store(true, std::memory_order_relaxed);
    queueCv_.notify_all();

    if (feederThread_.joinable()) {
        feederThread_.join();
    }

    if (deviceInited_) {
        ma_device_stop(&device_);
        ma_device_uninit(&device_);
        deviceInited_ = false;
    }

    if (rbInited_) {
        ma_pcm_rb_uninit(&rb_);
        rbInited_ = false;
    }
}

Napi::Object AudioPlayer::Init(Napi::Env env, Napi::Object exports) {
    Napi::Function func = DefineClass(
        env,
        "AudioPlayer",
        {
            InstanceMethod("initDevice", &AudioPlayer::InitDevice),
            InstanceMethod("play", &AudioPlayer::Play),
            InstanceMethod("stop", &AudioPlayer::Stop),
            InstanceMethod("write", &AudioPlayer::Write),
            InstanceMethod("getState", &AudioPlayer::GetState),
            InstanceMethod("pause", &AudioPlayer::Pause),
            InstanceMethod("flush", &AudioPlayer::Flush)
        }
    );

    constructor = Napi::Persistent(func);
    constructor.SuppressDestruct();

    exports.Set("AudioPlayer", func);
    return exports;
}

Napi::Value AudioPlayer::Write(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (!deviceInited_ || !rbInited_) {
        Napi::Error::New(env, "AudioPlayer is not initialized!")
        .ThrowAsJavaScriptException();
        return env.Null();
    }

    if (info.Length() < 1 || !info[0].IsBuffer()) {
        Napi::TypeError::New(env, "Expected a Buffer (float32 pcm)")
        .ThrowAsJavaScriptException();
        return env.Null();
    }

    auto buffer = info[0].As<Napi::Buffer<uint8_t>>();
    const size_t bytesPerFrame = sizeof(float) * channels_;

    if (buffer.Length() % bytesPerFrame != 0) {
        Napi::TypeError::New(env, "Buffer length must align to complete audio frames")
            .ThrowAsJavaScriptException();
        return env.Null();
    }

    size_t acceptedBytes = 0;

    {
        std::lock_guard lock(queueMutex_);

        const size_t availableBytes = (queuedBytes_ >= maxQueuedBytes_) ? 0 : (maxQueuedBytes_ - queuedBytes_);
        acceptedBytes = std::min(buffer.Length(), availableBytes);

        acceptedBytes -= (acceptedBytes % bytesPerFrame);

        if (acceptedBytes > 0) {
            std::vector<uint8_t> chunk(acceptedBytes);
            std::memcpy(chunk.data(), buffer.Data(), acceptedBytes);
            pcmQueue_.push_back(std::move(chunk));
            queuedBytes_ += acceptedBytes;
        }
    }

    if (acceptedBytes > 0) {
        queueCv_.notify_one();
    }

    return Napi::Number::New(env, acceptedBytes / bytesPerFrame);
}

void AudioPlayer::DataCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    auto* self = reinterpret_cast<AudioPlayer*>(pDevice->pUserData);
    if (self == nullptr || pOutput == nullptr) return;

    const ma_uint32 channels = pDevice->playback.channels;
    const size_t bytesPerFrame = sizeof(float) * channels;
    ma_uint8* outBytes = reinterpret_cast<ma_uint8*>(pOutput);

    const TransportState state = self->transportState_.load(std::memory_order_relaxed);
    if (!self->rbInited_ || state != TransportState::Playing) {
        ma_silence_pcm_frames(pOutput, frameCount, pDevice->playback.format, channels);
        (void)pInput;
        return;
    }

    ma_uint32 framesRemaining = frameCount;
    ma_uint32 totalFramesRead = 0;

    while (framesRemaining > 0) {
        void* src = nullptr;
        ma_uint32 framesToRead = framesRemaining;

        ma_result result = ma_pcm_rb_acquire_read(&self->rb_, &framesToRead, &src);
        if (result != MA_SUCCESS || framesToRead == 0) {
            break;
        }

        std::memcpy(
            outBytes + (totalFramesRead * bytesPerFrame),
            src,
            framesToRead * bytesPerFrame
        );

        ma_pcm_rb_commit_read(&self->rb_, framesToRead);

        totalFramesRead += framesToRead;
        framesRemaining -= framesToRead;
    }

    if (framesRemaining > 0) {
        ma_silence_pcm_frames(
            outBytes + (totalFramesRead * bytesPerFrame),
            framesRemaining,
            pDevice->playback.format,
            channels
        );
        self->underrunCount_.fetch_add(1, std::memory_order_relaxed);
    }

    (void)pInput;
}

Napi::Value AudioPlayer::InitDevice(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (deviceInited_ || rbInited_) {
        Napi::Error::New(env, "AudioPlayer is already initialized")
            .ThrowAsJavaScriptException();
        return env.Null();
    }

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = 0; // device native
    config.sampleRate = 0; // device native
    config.dataCallback = AudioPlayer::DataCallback;
    config.pUserData = this;

    // init the device
    ma_result result = ma_device_init(nullptr, &config, &device_);
    if (result != MA_SUCCESS) {
        Napi::Error::New(env, "Failed to initialize playback device")
        .ThrowAsJavaScriptException();
        return env.Null();
    }

    deviceInited_ = true;
    sampleRate_ = device_.sampleRate;
    channels_ = device_.playback.channels;

    bufferFrameCapacity = sampleRate_ * 2;
    ma_result rbResult = ma_pcm_rb_init(
        ma_format_f32,
        channels_,
        bufferFrameCapacity,
        nullptr,
        nullptr,
        &rb_
    );
    if (rbResult != MA_SUCCESS) {
        ma_device_uninit(&device_);
        deviceInited_ = false;

        Napi::Error::New(env, "Failed to initialize ring buffer!")
        .ThrowAsJavaScriptException();
        return env.Null();
    }
    rbInited_ = true;

    ma_result startResult = ma_device_start(&device_);
    if (startResult != MA_SUCCESS) {
        ma_pcm_rb_uninit(&rb_);
        rbInited_ = false;

        ma_device_uninit(&device_);
        deviceInited_ = false;

        Napi::Error::New(env, "Failed to start playback device")
            .ThrowAsJavaScriptException();
        return env.Null();
    }
    transportState_.store(TransportState::Stopped, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        pcmQueue_.clear();
        queueReadOffset_ = 0;
        queuedBytes_ = 0;
    }

    maxQueuedBytes_ = static_cast<size_t>(sampleRate_) * channels_ * sizeof(float) * 5; // 5 sec
    eos_.store(false, std::memory_order_relaxed);
    shuttingDown_.store(false, std::memory_order_relaxed);

    feederThread_ = std::thread([this]() {
        this->FeederLoop();
    });

    return Napi::Boolean::New(env, true);
}

Napi::Value AudioPlayer::Play(const Napi::CallbackInfo& info) {
    const Napi::Env env = info.Env();

    if (!deviceInited_ || !rbInited_) {
        Napi::Error::New(env, "AudioPlayer is not initialized")
            .ThrowAsJavaScriptException();
        return env.Null();
    }

    transportState_.store(TransportState::Playing, std::memory_order_relaxed);
    queueCv_.notify_one();
    return env.Undefined();
}

Napi::Value AudioPlayer::Pause(const Napi::CallbackInfo& info) {
    const Napi::Env env = info.Env();

    if (!deviceInited_ || !rbInited_) {
        Napi::Error::New(env, "AudioPlayer is not initialized")
        .ThrowAsJavaScriptException();
        return env.Null();
    }

    transportState_.store(TransportState::Paused, std::memory_order_relaxed);
    return env.Undefined();
}

Napi::Value AudioPlayer::Stop(const Napi::CallbackInfo& info) {
    return Flush(info);
}

Napi::Value AudioPlayer::Flush(const Napi::CallbackInfo& info) {
    const Napi::Env env = info.Env();

    if (!deviceInited_ || !rbInited_) {
        Napi::Error::New(env, "AudioPlayer is not initialized")
            .ThrowAsJavaScriptException();
        return env.Null();
    }

    transportState_.store(TransportState::Stopped, std::memory_order_relaxed);
    eos_.store(false, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        pcmQueue_.clear();
        queueReadOffset_ = 0;
        queuedBytes_ = 0;
    }

    for (;;) {
        ma_uint32 framesAvailable = ma_pcm_rb_available_read(&rb_);
        if (framesAvailable == 0) {
            break;
        }

        void* src = nullptr;
        ma_uint32 framesToDiscard = framesAvailable;

        ma_result result = ma_pcm_rb_acquire_read(&rb_, &framesToDiscard, &src);
        if (result != MA_SUCCESS || framesToDiscard == 0) {
            break;
        }

        ma_pcm_rb_commit_read(&rb_, framesToDiscard);
    }

    queueCv_.notify_one();
    return env.Undefined();
}

void AudioPlayer::FeederLoop() {
    const size_t bytesPerFrame = sizeof(float) * channels_;

    for (;;) {
        std::unique_lock<std::mutex> lock(queueMutex_);

        queueCv_.wait(lock, [this]() {
            return shuttingDown_.load(std::memory_order_relaxed) || !pcmQueue_.empty();
        });

        if (shuttingDown_.load(std::memory_order_relaxed)) {
            break;
        }

        while (!pcmQueue_.empty()) {
            ma_uint32 availableWriteFrames = ma_pcm_rb_available_write(&rb_);
            if (availableWriteFrames == 0) {
                break;
            }

            std::vector<uint8_t>& front = pcmQueue_.front();

            const size_t remainingBytes = front.size() - queueReadOffset_;
            const ma_uint32 availableChunkFrames = static_cast<ma_uint32>(remainingBytes / bytesPerFrame);

            if (availableChunkFrames == 0) {
                pcmQueue_.pop_front();
                queueReadOffset_ = 0;
                continue;
            }

            ma_uint32 framesToWrite = std::min(availableWriteFrames, availableChunkFrames);

            void* dst = nullptr;
            ma_uint32 acquiredFrames = framesToWrite;

            ma_result result = ma_pcm_rb_acquire_write(&rb_, &acquiredFrames, &dst);
            if (result != MA_SUCCESS || acquiredFrames == 0) {
                break;
            }

            const size_t bytesToWrite = static_cast<size_t>(acquiredFrames) * bytesPerFrame;

            std::memcpy(
                dst,
                front.data() + queueReadOffset_,
                bytesToWrite
            );

            ma_pcm_rb_commit_write(&rb_, acquiredFrames);

            queueReadOffset_ += bytesToWrite;
            queuedBytes_ -= bytesToWrite;

            if (queueReadOffset_ >= front.size()) {
                pcmQueue_.pop_front();
                queueReadOffset_ = 0;
            }
        }

        lock.unlock();

        // fixme: notify when ringbuffer space opens up (so i don't have to retry in 2ms)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

Napi::Value AudioPlayer::GetState(const Napi::CallbackInfo& info) {
    const Napi::Env env = info.Env();

    size_t queuedBytesSnapshot = 0;
    {
        std::lock_guard lock(queueMutex_);
        queuedBytesSnapshot = queuedBytes_;
    }

    const Napi::Object state = Napi::Object::New(env);
    state.Set("sampleRate", Napi::Number::New(env, sampleRate_));
    state.Set("channels", Napi::Number::New(env, channels_));
    state.Set("bufferedFrames", Napi::Number::New(env, rbInited_ ? ma_pcm_rb_available_read(&rb_) : 0));
    state.Set("underrunCount", Napi::Number::New(env, underrunCount_.load()));
    state.Set("queuedBytes", Napi::Number::New(env, queuedBytesSnapshot));
    state.Set("queueCapacityBytes", Napi::Number::New(env, maxQueuedBytes_));
    state.Set("queuedFrames", Napi::Number::New(env, channels_ > 0 ? queuedBytesSnapshot / (sizeof(float) * channels_) : 0));
    state.Set("eos", Napi::Boolean::New(env, eos_.load(std::memory_order_relaxed)));

    const TransportState ts = transportState_.load(std::memory_order_relaxed);

    auto transportStateStr = "stopped";
    if (ts == TransportState::Playing) {
        transportStateStr = "playing";
    } else if (ts == TransportState::Paused) {
        transportStateStr = "paused";
    }
    state.Set("transportState", Napi::String::New(env, transportStateStr));

    return state;
}

Napi::Object InitAll(Napi::Env env, Napi::Object exports) {
    return AudioPlayer::Init(env, exports);
}

NODE_API_MODULE(dodio_audio, InitAll)
