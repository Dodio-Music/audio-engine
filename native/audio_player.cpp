#include <napi.h>
#include <atomic>
#include <cmath>

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
    std::atomic<bool> initialized_{false};

    uint32_t sampleRate_ = 0;
    uint32_t channels_ = 0;
};

Napi::FunctionReference AudioPlayer::constructor;

AudioPlayer::AudioPlayer(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<AudioPlayer>(info) {}

AudioPlayer::~AudioPlayer() {
    if (deviceInited_) {
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
        }
    );

    constructor = Napi::Persistent(func);
    constructor.SuppressDestruct();

    exports.Set("AudioPlayer", func);
    return exports;
}

Napi::Value AudioPlayer::Write(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (!initialized_.load(std::memory_order_relaxed) || !rbInited_) {
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

    const ma_uint8* srcBytes = buffer.Data();
    ma_uint32 framesRemaining = static_cast<ma_uint32>(buffer.Length() / bytesPerFrame);
    ma_uint32 totalFramesWritten = 0;

    while (framesRemaining > 0) {
        void* dst = nullptr;
        ma_uint32 framesToWrite = framesRemaining;

        ma_result result = ma_pcm_rb_acquire_write(&rb_, &framesToWrite, &dst);
        if (result != MA_SUCCESS ||framesToWrite == 0) {
            break;
        }

        std::memcpy(dst, srcBytes + (totalFramesWritten * bytesPerFrame),
            framesToWrite * bytesPerFrame);

        ma_pcm_rb_commit_write(&rb_, framesToWrite);

        totalFramesWritten += framesToWrite;
        framesRemaining -= framesToWrite;
    }

    return Napi::Number::New(env, totalFramesWritten);
}

void AudioPlayer::DataCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    auto* self = reinterpret_cast<AudioPlayer*>(pDevice->pUserData);
    if (self == nullptr || pOutput == nullptr) return;

    float* out = reinterpret_cast<float*>(pOutput);
    const ma_uint32 channels = pDevice->playback.channels;
    const size_t bytesPerFrame = sizeof(float) * channels;

    const TransportState state = self->transportState_.load(std::memory_order_relaxed);
    if (!self->rbInited_ || state != TransportState::Playing) {
        ma_silence_pcm_frames(out, frameCount, pDevice->playback.format, channels);
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
            reinterpret_cast<ma_uint8*>(out) + (totalFramesRead * bytesPerFrame),
            src,
            framesToRead * bytesPerFrame
        );

        ma_pcm_rb_commit_read(&self->rb_, framesToRead);

        totalFramesRead += framesToRead;
        framesRemaining -= framesToRead;
    }

    if (framesRemaining > 0) {
        ma_silence_pcm_frames(
            reinterpret_cast<ma_uint8*>(out) + (totalFramesRead * bytesPerFrame),
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

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = 0; // device native
    config.sampleRate = 0; // device native
    config.dataCallback = AudioPlayer::DataCallback;
    config.pUserData = this;

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
        Napi::Error::New(env, "Failed to initialize ring buffer!")
        .ThrowAsJavaScriptException();
        return env.Null();
    }
    rbInited_ = true;

    ma_result startResult = ma_device_start(&device_);
    if (startResult != MA_SUCCESS) {
        ma_device_uninit(&device_);
        deviceInited_ = false;

        Napi::Error::New(env, "Failed to start playback device")
            .ThrowAsJavaScriptException();
        return env.Null();
    }
    initialized_.store(true, std::memory_order_relaxed);
    transportState_.store(TransportState::Stopped, std::memory_order_relaxed);

    return Napi::Boolean::New(env, true);
}

Napi::Value AudioPlayer::Play(const Napi::CallbackInfo& info) {
    const Napi::Env env = info.Env();

    if (!initialized_.load(std::memory_order_relaxed) || !deviceInited_) {
        Napi::Error::New(env, "AudioPlayer is not initialized")
            .ThrowAsJavaScriptException();
        return env.Null();
    }

    transportState_.store(TransportState::Playing, std::memory_order_relaxed);
    return env.Undefined();
}

Napi::Value AudioPlayer::Pause(const Napi::CallbackInfo& info) {
    const Napi::Env env = info.Env();

    if (!initialized_.load(std::memory_order_relaxed) || !deviceInited_) {
        Napi::Error::New(env, "AudioPlayer is not initialized")
        .ThrowAsJavaScriptException();
        return env.Null();
    }

    transportState_.store(TransportState::Paused, std::memory_order_relaxed);
    return env.Undefined();
}

Napi::Value AudioPlayer::Stop(const Napi::CallbackInfo& info) {
    const Napi::Env env = info.Env();

    if (!initialized_.load(std::memory_order_relaxed) || !deviceInited_) {
        Napi::Error::New(env, "AudioPlayer is not initialized")
            .ThrowAsJavaScriptException();
        return env.Null();
    }

    transportState_.store(TransportState::Stopped, std::memory_order_relaxed);
    return env.Undefined();
}

Napi::Value AudioPlayer::GetState(const Napi::CallbackInfo& info) {
    const Napi::Env env = info.Env();

    const Napi::Object state = Napi::Object::New(env);
    state.Set("initialized", Napi::Boolean::New(env, initialized_.load()));
    state.Set("sampleRate", Napi::Number::New(env, sampleRate_));
    state.Set("channels", Napi::Number::New(env, channels_));
    state.Set("bufferedFrames", Napi::Number::New(env, rbInited_ ? ma_pcm_rb_available_read(&rb_) : 0));
    state.Set("underrunCount", Napi::Number::New(env, underrunCount_.load()));

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
