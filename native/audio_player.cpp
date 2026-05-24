#include <napi.h>
#include <cstring>
#include <atomic>

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

class AudioPlayer : public Napi::ObjectWrap<AudioPlayer> {
public:
    static Napi::Object Init(Napi::Env env, Napi::Object exports);
    AudioPlayer(const Napi::CallbackInfo& info);
    ~AudioPlayer();

private:
    static Napi::FunctionReference constructor;
    Napi::Value Load(const Napi::CallbackInfo& info);
    Napi::Value Pause(const Napi::CallbackInfo& info);
    Napi::Value Resume(const Napi::CallbackInfo& info);
    Napi::Value TogglePlayPause(const Napi::CallbackInfo& info);
    Napi::Value Seek(const Napi::CallbackInfo& info);
    Napi::Value SetVolume(const Napi::CallbackInfo& info);
    Napi::Value GetState(const Napi::CallbackInfo& info);

    void Cleanup();

    static void DataCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount);

    ma_decoder decoder{};
    bool decoderInitialized = false;

    ma_device device{};
    bool deviceInitialized = false;

    bool playing = false;
    float volume = 1.0f;

    std::atomic<bool> seekRequested = false;
    std::atomic<ma_uint64> seekTargetFrame = 0;

    std::atomic<ma_uint64> songFramesPlayed = 0;
    std::atomic<bool> ended = false;


};

Napi::FunctionReference AudioPlayer::constructor;

AudioPlayer::AudioPlayer(const Napi::CallbackInfo& info) : Napi::ObjectWrap<AudioPlayer>(info) {}

AudioPlayer::~AudioPlayer() {
    if (deviceInitialized) {
        ma_device_uninit(&device);
    }

    if (decoderInitialized) {
        ma_decoder_uninit(&decoder);
    }
}

Napi::Object AudioPlayer::Init(Napi::Env env, Napi::Object exports) {
    Napi::Function func = DefineClass(
        env,
        "AudioPlayer",
        {
            InstanceMethod("load", &AudioPlayer::Load),
            InstanceMethod("pause", &AudioPlayer::Pause),
            InstanceMethod("resume", &AudioPlayer::Resume),
            InstanceMethod("togglePlayPause", &AudioPlayer::TogglePlayPause),
            InstanceMethod("seek", &AudioPlayer::Seek),
            InstanceMethod("setVolume", &AudioPlayer::SetVolume),
            InstanceMethod("getState", &AudioPlayer::GetState)
        }
    );

    constructor = Napi::Persistent(func);
    constructor.SuppressDestruct();

    exports.Set("AudioPlayer", func);
    return exports;
}

Napi::Object InitAll(Napi::Env env, Napi::Object exports) {
    return AudioPlayer::Init(env, exports);
}

NODE_API_MODULE(dodio_audio, InitAll)

void AudioPlayer::DataCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    (void)pInput;

    auto* player = static_cast<AudioPlayer*>(pDevice->pUserData);

    // if decoder isn't initialized, don't process buffer
    if (player == nullptr || !player->decoderInitialized) {
        return;
    }

    if (player->seekRequested.exchange(false, std::memory_order_acquire)) {
        const ma_uint64 targetFrame = player->seekTargetFrame.load(std::memory_order_relaxed);

        if (ma_decoder_seek_to_pcm_frame(&player->decoder, targetFrame) == MA_SUCCESS) {
            player->songFramesPlayed.store(targetFrame, std::memory_order_relaxed);
            player->ended.store(false, std::memory_order_relaxed);
        } else {
            player->ended.store(true, std::memory_order_relaxed);
            return;
        }
    }

    // if song has ended, don't process buffer
    if (player->ended.load(std::memory_order_relaxed)) {
        return;
    }

    ma_uint64 framesRead = 0;
    const ma_result result = ma_decoder_read_pcm_frames(
        &player->decoder,
        pOutput,
        frameCount,
        &framesRead
    );

    player->songFramesPlayed.fetch_add(framesRead, std::memory_order_relaxed);

    if (result != MA_SUCCESS || framesRead < frameCount) {
        player->ended.store(true, std::memory_order_relaxed);
    }
}

Napi::Value AudioPlayer::Seek(const Napi::CallbackInfo &info) {
    const auto env = info.Env();
    const double seconds = info[0].As<Napi::Number>().DoubleValue();
    const auto frame = static_cast<ma_uint64>(seconds * decoder.outputSampleRate);

    seekTargetFrame.store(frame, std::memory_order_relaxed);
    songFramesPlayed.store(frame, std::memory_order_relaxed);
    ended.store(false, std::memory_order_relaxed);
    seekRequested.store(true, std::memory_order_relaxed);

    return env.Undefined();
}

Napi::Value AudioPlayer::SetVolume(const Napi::CallbackInfo& info) {
    const auto env = info.Env();
    if (info.Length() < 1 || !info[0].IsNumber()) {
        Napi::TypeError::New(env, "Expected volume number.").ThrowAsJavaScriptException();
        return env.Null();
    }
    float value = info[0].As<Napi::Number>().FloatValue();
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;

    volume = value;

    if (deviceInitialized) {
        ma_device_set_master_volume(&device, value);
    }

    return env.Undefined();
}

void AudioPlayer::Cleanup() {
    if (deviceInitialized) {
        ma_device_stop(&device);
        ma_device_uninit(&device);
        deviceInitialized = false;
    }

    if (decoderInitialized) {
        ma_decoder_uninit(&decoder);
        decoderInitialized = false;
    }

    playing = false;
}

Napi::Value AudioPlayer::Pause(const Napi::CallbackInfo& info) {
    const Napi::Env env = info.Env();
    if (deviceInitialized) {
        ma_device_stop(&device);
        playing = false;
    }

    return env.Undefined();
}

Napi::Value AudioPlayer::Resume(const Napi::CallbackInfo& info) {
    const Napi::Env env = info.Env();
    if (deviceInitialized) {
        ma_result result = ma_device_start(&device);
        if (result != MA_SUCCESS) {
            Napi::Error::New(env, "Failed to resume playback!")
                .ThrowAsJavaScriptException();
            return env.Null();
        }

        playing = true;
    }

    return env.Undefined();
}

Napi::Value AudioPlayer::TogglePlayPause(const Napi::CallbackInfo &info) {
    const Napi::Env env = info.Env();
    if (playing) {
        Pause(info);
    } else {
        Resume(info);
    }

    return Napi::Boolean::New(env, playing);
}

Napi::Value AudioPlayer::Load(const Napi::CallbackInfo& info) {
    const Napi::Env env = info.Env();
    if (info.Length() < 1 || !info[0].IsString()) {
        Napi::TypeError::New(env, "Expected a path (string).")
        .ThrowAsJavaScriptException();
        return env.Null();
    }

    Cleanup();

    const std::string utf8Path = info[0].As<Napi::String>().Utf8Value();
    ma_result result;
#ifdef _WIN32
    std::u16string path16 = info[0].As<Napi::String>().Utf16Value();
    std::wstring path(path16.begin(), path16.end());

    result = ma_decoder_init_file_w(path.c_str(), nullptr, &decoder);
#else
    result = ma_decoder_init_file(utf8Path.c_str(), nullptr, &decoder);
#endif
    if (result != MA_SUCCESS) {
        std::string message = "Invalid file path: \"" + utf8Path + "\"";

        Napi::Error::New(env, message).ThrowAsJavaScriptException();
        return env.Null();
    }
    decoderInitialized = true;

    ma_device_config deviceConfig;
    deviceConfig = ma_device_config_init(ma_device_type_playback);
    deviceConfig.playback.format = decoder.outputFormat;
    deviceConfig.playback.channels = decoder.outputChannels;
    deviceConfig.sampleRate = decoder.outputSampleRate;
    deviceConfig.dataCallback = DataCallback;
    deviceConfig.pUserData = this;

    result = ma_device_init(nullptr, &deviceConfig, &device);
    if (result != MA_SUCCESS) {
        ma_decoder_uninit(&decoder);
        decoderInitialized = false;

        Napi::Error::New(env, "Failed to open playback device!")
        .ThrowAsJavaScriptException();
        return env.Null();
    }
    deviceInitialized = true;

    ma_device_set_master_volume(&device, volume);

    result = ma_device_start(&device);
    if (result != MA_SUCCESS) {
        ma_device_uninit(&device);
        deviceInitialized = false;

        ma_decoder_uninit(&decoder);
        decoderInitialized = false;

        playing = false;

        Napi::Error::New(env, "Failed to start playback device!")
        .ThrowAsJavaScriptException();
        return env.Null();
    }

    playing = true;
    songFramesPlayed.store(0, std::memory_order_relaxed);
    ended.store(false, std::memory_order_relaxed);

    return env.Undefined();
}

Napi::Value AudioPlayer::GetState(const Napi::CallbackInfo& info) {
    const auto env = info.Env();
    const ma_uint64 frames = songFramesPlayed.load(std::memory_order_relaxed);

    double seconds = 0.0;
    if (decoderInitialized && decoder.outputSampleRate > 0) {
        seconds = static_cast<double>(frames) / static_cast<double>(decoder.outputSampleRate);
    }

    Napi::Object state = Napi::Object::New(env);
    state.Set("songFramesPlayed", Napi::Number::New(env, static_cast<double>(frames)));
    state.Set("songSecondsPlayed", Napi::Number::New(env, seconds));
    state.Set("ended", Napi::Boolean::New(env, ended.load(std::memory_order_relaxed)));

    return state;
}
