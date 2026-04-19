#include <napi.h>
#include <atomic>
#include <cstring>

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
    Napi::Value EndOfStream(const Napi::CallbackInfo& info);
    Napi::Value SetVolume(const Napi::CallbackInfo& info);

    ma_pcm_rb rb_{};
    std::atomic<bool> rbInited_{false};

    std::atomic<uint64_t> underrunCount_{0};

    ma_device device_{};
    std::atomic<bool> deviceInited_{false};

    enum class TransportState : uint32_t {
        Stopped = 0,
        Buffering = 1,
        Playing = 2,
        Paused = 3
    };
    std::atomic<TransportState> transportState_{TransportState::Stopped};

    uint32_t sampleRate_ = 0;
    uint32_t channels_ = 0;

    std::atomic<ma_uint32> bufferedFrames_{0};

    std::atomic<bool> eos_{false};

    std::atomic<bool> needsDrain_{false};
    std::atomic<bool> drainQueued_{false};
    std::atomic<bool> endedQueued_{false};

    ma_uint32 ringCapacityFrames_ = 0;
    ma_uint32 drainLowWaterFrames_ = 0;
    ma_uint32 startThresholdFrames_ = 0; // TODO: implement buffering

    Napi::ThreadSafeFunction drainTsfn_;
    Napi::ThreadSafeFunction endedTsfn_;

    Napi::Value SetDrainCallback(const Napi::CallbackInfo& info);
    Napi::Value SetEndedCallback(const Napi::CallbackInfo& info);

    void QueueDrainEvent();
    void QueueEndedEvent();

    bool ShouldStartPlayback() const;
};

Napi::FunctionReference AudioPlayer::constructor;

AudioPlayer::AudioPlayer(const Napi::CallbackInfo& info) : Napi::ObjectWrap<AudioPlayer>(info) {}

AudioPlayer::~AudioPlayer() {
    if (deviceInited_.load(std::memory_order_relaxed)) {
        ma_device_stop(&device_);
        ma_device_uninit(&device_);
        deviceInited_.store(false, std::memory_order_relaxed);
    }

    if (rbInited_.load(std::memory_order_relaxed)) {
        ma_pcm_rb_uninit(&rb_);
        rbInited_.store(false, std::memory_order_relaxed);
    }

    if (drainTsfn_) {
        drainTsfn_.Abort();
        drainTsfn_.Release();
    }

    if (endedTsfn_) {
        endedTsfn_.Abort();
        endedTsfn_.Release();
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
            InstanceMethod("flush", &AudioPlayer::Flush),
            InstanceMethod("endOfStream", &AudioPlayer::EndOfStream),
            InstanceMethod("setDrainCallback", &AudioPlayer::SetDrainCallback),
            InstanceMethod("setEndedCallback", &AudioPlayer::SetEndedCallback),
            InstanceMethod("setVolume", &AudioPlayer::SetVolume)
        }
    );

    constructor = Napi::Persistent(func);
    constructor.SuppressDestruct();

    exports.Set("AudioPlayer", func);
    return exports;
}

Napi::Value AudioPlayer::SetDrainCallback(const Napi::CallbackInfo& info) {
    const Napi::Env env = info.Env();
    if (info.Length() < 1 || !info[0].IsFunction()) {
        Napi::TypeError::New(env, "Expected a function")
            .ThrowAsJavaScriptException();
        return env.Null();
    }
    if (drainTsfn_) {
        drainTsfn_.Abort();
        drainTsfn_.Release();
    }

    drainTsfn_ = Napi::ThreadSafeFunction::New(env,info[0].As<Napi::Function>(),"AudioPlayerDrain",0,1);
    drainTsfn_.Unref(env);
    return env.Undefined();
}

Napi::Value AudioPlayer::SetEndedCallback(const Napi::CallbackInfo& info) {
    const Napi::Env env = info.Env();
    if (info.Length() < 1 || !info[0].IsFunction()) {
        Napi::TypeError::New(env, "Expected a function")
            .ThrowAsJavaScriptException();
        return env.Null();
    }
    if (endedTsfn_) {
        endedTsfn_.Abort();
        endedTsfn_.Release();
    }

    endedTsfn_ = Napi::ThreadSafeFunction::New(env, info[0].As<Napi::Function>(), "AudioPlayerEnded", 0, 1);
    endedTsfn_.Unref(env);
    return env.Undefined();
}

Napi::Value AudioPlayer::SetVolume(const Napi::CallbackInfo& info) {
    const Napi::Env env = info.Env();

    if (!deviceInited_.load(std::memory_order_relaxed)) {
        Napi::Error::New(env, "AudioPlayer is not initialized")
            .ThrowAsJavaScriptException();
        return env.Null();
    }

    if (info.Length() < 1 || !info[0].IsNumber()) {
        Napi::TypeError::New(env, "Expected number")
            .ThrowAsJavaScriptException();
        return env.Null();
    }

    const float volume = info[0].As<Napi::Number>().FloatValue();
    if (volume < 0.0f || volume > 1.0f) {
        Napi::RangeError::New(env, "Volume must be float between 0 and 1")
            .ThrowAsJavaScriptException();
        return env.Null();
    }

    const ma_result result = ma_device_set_master_volume(&device_, volume);
    if (result != MA_SUCCESS) {
        Napi::Error::New(env, "Failed to set device volume")
            .ThrowAsJavaScriptException();
        return env.Null();
    }

    return env.Undefined();
}

void AudioPlayer::QueueDrainEvent() {
    if (!drainTsfn_) return;

    if (drainQueued_.exchange(true, std::memory_order_relaxed)) {
        return;
    }

    napi_status status = drainTsfn_.NonBlockingCall(
        [this](Napi::Env env, Napi::Function cb) {
            drainQueued_.store(false, std::memory_order_relaxed);
            cb.Call({});
        }
    );

    if (status != napi_ok) {
        drainQueued_.store(false, std::memory_order_relaxed);
    }
}

void AudioPlayer::QueueEndedEvent() {
    if (!endedTsfn_) return;
    if (endedQueued_.exchange(true, std::memory_order_relaxed)) {
        return;
    }

    const napi_status status = endedTsfn_.NonBlockingCall(
        [this](Napi::Env env, const Napi::Function cb) {
            endedQueued_.store(false, std::memory_order_relaxed);
            cb.Call({});
        }
    );
    if (status != napi_ok) {
        endedQueued_.store(false, std::memory_order_relaxed);
    }
}

Napi::Value AudioPlayer::EndOfStream(const Napi::CallbackInfo& info) {
    const Napi::Env env = info.Env();

    if (!deviceInited_.load(std::memory_order_relaxed) || !rbInited_.load(std::memory_order_relaxed)) {
        Napi::Error::New(env, "AudioPlayer is not initialized")
            .ThrowAsJavaScriptException();
        return env.Null();
    }

    eos_.store(true, std::memory_order_relaxed);

    if (transportState_.load(std::memory_order_relaxed) == TransportState::Buffering && ShouldStartPlayback()) {
        transportState_.store(TransportState::Playing, std::memory_order_relaxed);
    }

    return env.Undefined();
}

Napi::Value AudioPlayer::Write(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (!deviceInited_.load(std::memory_order_relaxed) || !rbInited_.load(std::memory_order_relaxed)) {
        Napi::Error::New(env, "AudioPlayer is not initialized")
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

    const ma_uint32 inputFrames = static_cast<ma_uint32>(buffer.Length() / bytesPerFrame);

    ma_uint32 totalWrittenFrames = 0;
    const uint8_t* srcBytes = buffer.Data();

    while (totalWrittenFrames < inputFrames) {
        ma_uint32 framesToWrite = inputFrames - totalWrittenFrames;
        void* dst = nullptr;

        ma_result result = ma_pcm_rb_acquire_write(&rb_, &framesToWrite, &dst);
        if (result != MA_SUCCESS || framesToWrite == 0) {
            break;
        }

        std::memcpy(
            dst,
            srcBytes + (static_cast<size_t>(totalWrittenFrames) * bytesPerFrame),
            static_cast<size_t>(framesToWrite) * bytesPerFrame
        );

        ma_pcm_rb_commit_write(&rb_, framesToWrite);
        bufferedFrames_.fetch_add(framesToWrite, std::memory_order_relaxed);

        totalWrittenFrames += framesToWrite;
    }

    if (totalWrittenFrames > 0) {
        eos_.store(false, std::memory_order_relaxed);
        endedQueued_.store(false, std::memory_order_relaxed);

        if (transportState_.load(std::memory_order_relaxed) == TransportState::Buffering && ShouldStartPlayback()) {
            transportState_.store(TransportState::Playing, std::memory_order_relaxed);
        }
    }

    if (totalWrittenFrames < inputFrames) {
        needsDrain_.store(true, std::memory_order_relaxed);
    }

    return Napi::Number::New(env, totalWrittenFrames);
}

void AudioPlayer::DataCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    auto* self = reinterpret_cast<AudioPlayer*>(pDevice->pUserData);
    if (self == nullptr || pOutput == nullptr) return;

    const ma_uint32 channels = pDevice->playback.channels;
    const size_t bytesPerFrame = sizeof(float) * channels;
    ma_uint8* outBytes = reinterpret_cast<ma_uint8*>(pOutput);

    if (!self->rbInited_.load(std::memory_order_relaxed)) {
        ma_silence_pcm_frames(pOutput, frameCount, pDevice->playback.format, channels);
        (void)pInput;
        return;
    }

    TransportState state = self->transportState_.load(std::memory_order_relaxed);
    if (state == TransportState::Stopped || state == TransportState::Paused) {
        ma_silence_pcm_frames(pOutput, frameCount, pDevice->playback.format, channels);
        (void)pInput;
        return;
    }

    if (state == TransportState::Buffering) {
        if (!self->ShouldStartPlayback()) {
            ma_silence_pcm_frames(pOutput, frameCount, pDevice->playback.format, channels);
            (void)pInput;
            return;
        }

        self->transportState_.store(TransportState::Playing, std::memory_order_relaxed);
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
        self->bufferedFrames_.fetch_sub(framesToRead, std::memory_order_relaxed);

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

        // if not EOS, buffer instead of playing repeated small gaps
        if (!self->eos_.load(std::memory_order_relaxed)) {
            self->transportState_.store(TransportState::Buffering, std::memory_order_relaxed);
        }
    }

    const ma_uint32 buffered = self->bufferedFrames_.load(std::memory_order_relaxed);

    if (self->needsDrain_.load(std::memory_order_relaxed) &&
        buffered <= self->drainLowWaterFrames_) {
        self->needsDrain_.store(false, std::memory_order_relaxed);
        self->QueueDrainEvent();
    }

    if (self->eos_.load(std::memory_order_relaxed) &&
        buffered == 0) {
        self->QueueEndedEvent();
    }

    (void)pInput;
}

Napi::Value AudioPlayer::InitDevice(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    double bufferCapacityMs = 5000.0;
    double drainLowWaterMs = 500.0;
    double startThresholdMs = 0.0;

    if (info.Length() >= 1 && info[0].IsObject()) {
        const Napi::Object opts = info[0].As<Napi::Object>();
        if (opts.Has("bufferCapacityMs")) bufferCapacityMs = opts.Get("bufferCapacityMs").As<Napi::Number>().DoubleValue();
        if (opts.Has("drainLowWaterMs")) drainLowWaterMs = opts.Get("drainLowWaterMs").As<Napi::Number>().DoubleValue();
        if (opts.Has("startThresholdMs")) startThresholdMs = opts.Get("startThresholdMs").As<Napi::Number>().DoubleValue();
    }

    if (deviceInited_.load(std::memory_order_relaxed) || rbInited_.load(std::memory_order_relaxed)) {
        Napi::Error::New(env, "AudioPlayer is already initialized")
            .ThrowAsJavaScriptException();
        return env.Null();
    }

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = 0; // device native
    config.sampleRate = 0; // device native
    config.dataCallback = DataCallback;
    config.pUserData = this;

    // init the device
    ma_result result = ma_device_init(nullptr, &config, &device_);
    if (result != MA_SUCCESS) {
        Napi::Error::New(env, "Failed to initialize playback device")
        .ThrowAsJavaScriptException();
        return env.Null();
    }

    deviceInited_.store(true, std::memory_order_relaxed);
    sampleRate_ = device_.sampleRate;
    channels_ = device_.playback.channels;

    ringCapacityFrames_ = static_cast<ma_uint32>((sampleRate_ * bufferCapacityMs) / 1000.0);
    drainLowWaterFrames_ = static_cast<ma_uint32>((sampleRate_ * drainLowWaterMs) / 1000.0);
    startThresholdFrames_ = static_cast<ma_uint32>((sampleRate_ * startThresholdMs) / 1000.0);

    // safety cases
    if (ringCapacityFrames_ == 0) ringCapacityFrames_ = sampleRate_ * 5;
    if (drainLowWaterFrames_ > ringCapacityFrames_) drainLowWaterFrames_ = ringCapacityFrames_;
    if (startThresholdFrames_ > ringCapacityFrames_) startThresholdFrames_ = ringCapacityFrames_;

    // init the ring buffer
    ma_result rbResult = ma_pcm_rb_init(
        ma_format_f32,
        channels_,
        ringCapacityFrames_,
        nullptr,
        nullptr,
        &rb_
    );
    if (rbResult != MA_SUCCESS) {
        ma_device_uninit(&device_);
        deviceInited_.store(false, std::memory_order_relaxed);

        Napi::Error::New(env, "Failed to initialize ring buffer!")
        .ThrowAsJavaScriptException();
        return env.Null();
    }
    rbInited_.store(true, std::memory_order_relaxed);

    // start the device
    ma_result startResult = ma_device_start(&device_);
    if (startResult != MA_SUCCESS) {
        ma_pcm_rb_uninit(&rb_);
        rbInited_.store(false, std::memory_order_relaxed);

        ma_device_uninit(&device_);
        deviceInited_.store(false, std::memory_order_relaxed);

        Napi::Error::New(env, "Failed to start playback device")
            .ThrowAsJavaScriptException();
        return env.Null();
    }
    transportState_.store(TransportState::Stopped, std::memory_order_relaxed);
    eos_.store(false, std::memory_order_relaxed);

    return Napi::Boolean::New(env, true);
}

Napi::Value AudioPlayer::Play(const Napi::CallbackInfo& info) {
    const Napi::Env env = info.Env();

    if (!deviceInited_.load(std::memory_order_relaxed) || !rbInited_.load(std::memory_order_relaxed)) {
        Napi::Error::New(env, "AudioPlayer is not initialized")
            .ThrowAsJavaScriptException();
        return env.Null();
    }

    if (ShouldStartPlayback()) {
        transportState_.store(TransportState::Playing, std::memory_order_relaxed);
    } else {
        transportState_.store(TransportState::Buffering, std::memory_order_relaxed);
    }
    return env.Undefined();
}

Napi::Value AudioPlayer::Pause(const Napi::CallbackInfo& info) {
    const Napi::Env env = info.Env();

    if (!deviceInited_.load(std::memory_order_relaxed) || !rbInited_.load(std::memory_order_relaxed)) {
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

    if (!deviceInited_.load(std::memory_order_relaxed) || !rbInited_.load(std::memory_order_relaxed)) {
        Napi::Error::New(env, "AudioPlayer is not initialized")
            .ThrowAsJavaScriptException();
        return env.Null();
    }

    transportState_.store(TransportState::Stopped, std::memory_order_relaxed);
    eos_.store(false, std::memory_order_relaxed);
    needsDrain_.store(false, std::memory_order_relaxed);
    drainQueued_.store(false, std::memory_order_relaxed);
    endedQueued_.store(false, std::memory_order_relaxed);

    if (ma_device_stop(&device_) != MA_SUCCESS) {
        Napi::Error::New(env, "Failed to stop playback device")
            .ThrowAsJavaScriptException();
        return env.Null();
    }

    ma_pcm_rb_reset(&rb_);
    bufferedFrames_.store(0, std::memory_order_relaxed);

    if (ma_device_start(&device_) != MA_SUCCESS) {
        Napi::Error::New(env, "Failed to restart playback device")
            .ThrowAsJavaScriptException();
        return env.Null();
    }

    return env.Undefined();
}

Napi::Value AudioPlayer::GetState(const Napi::CallbackInfo& info) {
    const Napi::Env env = info.Env();

    const Napi::Object state = Napi::Object::New(env);
    state.Set("sampleRate", Napi::Number::New(env, sampleRate_));
    state.Set("channels", Napi::Number::New(env, channels_));
    state.Set("bufferedFrames", Napi::Number::New(env,bufferedFrames_.load(std::memory_order_relaxed)));
    state.Set("bufferCapacityFrames", Napi::Number::New(env, ringCapacityFrames_));
    state.Set("underrunCount", Napi::Number::New(env, underrunCount_.load()));
    state.Set("eos", Napi::Boolean::New(env, eos_.load(std::memory_order_relaxed)));

    float volume = 1.0f;
    if (deviceInited_.load(std::memory_order_relaxed)) ma_device_get_master_volume(&device_, &volume);
    state.Set("volume", Napi::Number::New(env, volume));

    const TransportState ts = transportState_.load(std::memory_order_relaxed);


    std::string transportStateStr;
    switch (ts) {
        case TransportState::Stopped:
            transportStateStr = "stopped";
            break;
        case TransportState::Buffering:
            transportStateStr = "buffering";
            break;
        case TransportState::Playing:
            transportStateStr = "playing";
            break;
        case TransportState::Paused:
            transportStateStr = "paused";
            break;
    }
    state.Set("transportState", Napi::String::New(env, transportStateStr));

    return state;
}

Napi::Object InitAll(Napi::Env env, Napi::Object exports) {
    return AudioPlayer::Init(env, exports);
}

bool AudioPlayer::ShouldStartPlayback() const {
    const ma_uint32 buffered = bufferedFrames_.load(std::memory_order_relaxed);
    if (buffered >= startThresholdFrames_) {
        return true;
    }
    if (eos_.load(std::memory_order_relaxed) && buffered > 0) {
        return true;
    }

    return false;
}

NODE_API_MODULE(dodio_audio, InitAll)
