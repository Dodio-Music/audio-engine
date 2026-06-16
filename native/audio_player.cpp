#include <napi.h>
#include <cstring>
#include <atomic>
#include <mutex>
#include <memory>

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
    Napi::Value OnEnded(const Napi::CallbackInfo& info);
    Napi::Value PrepareNext(const Napi::CallbackInfo& info);
    Napi::Value OnAdvanced(const Napi::CallbackInfo& info);
    ma_result InitDecoderFromPath(const Napi::CallbackInfo& info, const std::string& utf8Path, ma_decoder* outDecoder);

    void InitDevice(Napi::Env env);
    void Cleanup(Napi::Env env);
    void ClearDecodersLocked();

    static void DataCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount);

    std::unique_ptr<ma_decoder> decoder;
    std::unique_ptr<ma_decoder> nextDecoder;
    std::mutex decoderMutex;
    std::atomic<ma_uint64> prepareGeneration = 0;

    ma_device device{};
    bool deviceInitialized = false;
    ma_format deviceFormat = ma_format_unknown;
    ma_uint32 deviceChannels = 0;
    ma_uint32 deviceSampleRate = 0;

    float volume = 1.0f;

    std::atomic<bool> playing = false;

    std::atomic<bool> seekRequested = false;
    std::atomic<ma_uint64> seekTargetFrame = 0;

    std::atomic<ma_uint64> songFramesPlayed = 0;

    std::atomic<bool> ended = false;
    Napi::ThreadSafeFunction endedCallback;
    std::atomic<bool> endedCallbackSet = false;

    Napi::ThreadSafeFunction advancedCallback;
    std::atomic<bool> advancedCallbackSet = false;
    std::atomic<ma_uint64> trackIndex = 0;

    std::mutex callbackMutex;
};

Napi::FunctionReference AudioPlayer::constructor;

AudioPlayer::AudioPlayer(const Napi::CallbackInfo& info) : Napi::ObjectWrap<AudioPlayer>(info) {}

AudioPlayer::~AudioPlayer() {
    if (deviceInitialized) {
        ma_device_uninit(&device);
        deviceInitialized = false;
    }

    {
        std::lock_guard lock(decoderMutex);
        ClearDecodersLocked();
    }

    {
        std::lock_guard lock(callbackMutex);

        if (endedCallbackSet.load(std::memory_order_acquire)) {
            endedCallback.Release();
            endedCallbackSet.store(false, std::memory_order_release);
        }

        if (advancedCallbackSet.load(std::memory_order_acquire)) {
            advancedCallback.Release();
            advancedCallbackSet.store(false, std::memory_order_release);
        }
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
            InstanceMethod("getState", &AudioPlayer::GetState),
            InstanceMethod("onEnded", &AudioPlayer::OnEnded),
            InstanceMethod("prepareNext", &AudioPlayer::PrepareNext),
            InstanceMethod("onAdvanced", &AudioPlayer::OnAdvanced)
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

static void DebugLog(const Napi::Env env, const std::string& message) {
    const Napi::Object global = env.Global();
    const Napi::Value consoleValue = global.Get("console");

    if (!consoleValue.IsObject()) return;

    const auto console = consoleValue.As<Napi::Object>();
    const Napi::Value debugValue = console.Get("debug");

    if (!debugValue.IsFunction()) return;

    debugValue.As<Napi::Function>().Call(console, { Napi::String::New(env, message) });
}

void AudioPlayer::DataCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    (void)pInput;

    auto* player = static_cast<AudioPlayer*>(pDevice->pUserData);
    if (player == nullptr) {
        return;
    }

    bool shouldCallAdvanced = false;
    ma_uint64 advancedTrackIndex = 0;

    bool shouldCallEnded = false;

    {
        std::lock_guard lock(player->decoderMutex);

        if (!player->decoder) {
            return;
        }

        if (player->seekRequested.exchange(false, std::memory_order_acquire)) {
            const ma_uint64 targetFrame = player->seekTargetFrame.load(std::memory_order_relaxed);

            if (ma_decoder_seek_to_pcm_frame(player->decoder.get(), targetFrame) == MA_SUCCESS) {
                player->songFramesPlayed.store(targetFrame, std::memory_order_relaxed);
                player->ended.store(false, std::memory_order_relaxed);
            } else {
                player->ended.store(true, std::memory_order_relaxed);
                return;
            }
        }

        if (player->ended.load(std::memory_order_relaxed)) {
            return;
        }

        ma_uint64 framesReadFromCurrent = 0;
        const ma_result result = ma_decoder_read_pcm_frames(
            player->decoder.get(),
            pOutput,
            frameCount,
            &framesReadFromCurrent
        );

        const bool currentEnded = result != MA_SUCCESS || framesReadFromCurrent < frameCount;

        if (!currentEnded) {
            player->songFramesPlayed.fetch_add(
                framesReadFromCurrent,
                std::memory_order_relaxed
            );
            return;
        }

        const ma_uint64 framesRemaining = frameCount - framesReadFromCurrent;

        if (framesRemaining > 0 && player->nextDecoder) {
            ma_decoder_uninit(player->decoder.get());

            player->decoder = std::move(player->nextDecoder);

            player->songFramesPlayed.store(0, std::memory_order_relaxed);
            player->ended.store(false, std::memory_order_relaxed);
            player->seekRequested.store(false, std::memory_order_relaxed);

            const ma_uint32 bytesPerFrame = ma_get_bytes_per_frame(player->deviceFormat, player->deviceChannels);

            void* pRemainderOutput = static_cast<unsigned char*>(pOutput) + framesReadFromCurrent * bytesPerFrame;

            ma_uint64 framesReadFromNext = 0;
            ma_result nextResult = ma_decoder_read_pcm_frames(player->decoder.get(), pRemainderOutput, framesRemaining, &framesReadFromNext);

            player->songFramesPlayed.fetch_add(framesReadFromNext, std::memory_order_relaxed);

            if (nextResult == MA_SUCCESS && framesReadFromNext > 0) {
                advancedTrackIndex = player->trackIndex.fetch_add(1, std::memory_order_acq_rel) + 1;
                shouldCallAdvanced = true;
            }

            if (nextResult != MA_SUCCESS || framesReadFromNext < framesRemaining) {
                if (!player->ended.exchange(true, std::memory_order_acq_rel)) {
                    player->playing.store(false, std::memory_order_release);
                    shouldCallEnded = true;
                }
            }
        } else {
            player->songFramesPlayed.fetch_add(framesReadFromCurrent,std::memory_order_relaxed);

            if (!player->ended.exchange(true, std::memory_order_acq_rel)) {
                player->playing.store(false, std::memory_order_release);
                shouldCallEnded = true;
            }
        }
    }

    if (shouldCallAdvanced && player->advancedCallbackSet.load(std::memory_order_acquire)) {
        std::lock_guard lock(player->callbackMutex);

        const auto status = player->advancedCallback.NonBlockingCall(
            [advancedTrackIndex](Napi::Env env, Napi::Function jsCallback) {
                jsCallback.Call({
                    Napi::Number::New(
                        env,
                        static_cast<double>(advancedTrackIndex)
                    )
                });
            }
        );

        (void)status;
    }

    if (shouldCallEnded && player->endedCallbackSet.load(std::memory_order_acquire)) {
        std::lock_guard lock(player->callbackMutex);

        const auto status = player->endedCallback.NonBlockingCall(
        [](Napi::Env env, Napi::Function jsCallback) {
                jsCallback.Call({});
            }
        );
        (void)status;
    }
}

Napi::Value AudioPlayer::Seek(const Napi::CallbackInfo &info) {
    const auto env = info.Env();
    double seconds = info[0].As<Napi::Number>().DoubleValue();
    if (seconds < 0) seconds = 0;

    ma_uint32 sampleRate = 0;

    {
        std::lock_guard lock(decoderMutex);

        if (!decoder) {
            Napi::Error::New(env, "Seek failed! Decoder is not initialized.")
                .ThrowAsJavaScriptException();
            return env.Null();
        }

        sampleRate = decoder->outputSampleRate;
    }

    if (!decoder) {
        Napi::Error::New(env, "Seek failed! Decoder is not initialized.")
                .ThrowAsJavaScriptException();
        return env.Null();
    }

    const auto frame = static_cast<ma_uint64>(seconds * sampleRate);

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

void AudioPlayer::Cleanup(const Napi::Env env) {
    if (deviceInitialized) {
        ma_device_stop(&device);
        ma_device_uninit(&device);
        DebugLog(env, "Uninitialized playback device.");
        deviceInitialized = false;
    }

    {
        std::lock_guard lock(decoderMutex);
        ClearDecodersLocked();
    }

    playing.store(false, std::memory_order_release);
    ended.store(false, std::memory_order_release);
    seekRequested.store(false, std::memory_order_release);
}

void AudioPlayer::ClearDecodersLocked() {
    if (decoder) {
        ma_decoder_uninit(decoder.get());
        decoder.reset();
    }

    if (nextDecoder) {
        ma_decoder_uninit(nextDecoder.get());
        nextDecoder.reset();
    }
}

Napi::Value AudioPlayer::Pause(const Napi::CallbackInfo& info) {
    const Napi::Env env = info.Env();
    if (deviceInitialized) {
        ma_device_stop(&device);
        playing.store(false, std::memory_order_release);
    }

    return env.Undefined();
}

Napi::Value AudioPlayer::Resume(const Napi::CallbackInfo& info) {
    const Napi::Env env = info.Env();
    if (deviceInitialized) {
        // case: song has finished playing, user wants to replay
        if (ended.load(std::memory_order_acquire)) {
            seekTargetFrame.store(0, std::memory_order_relaxed);
            songFramesPlayed.store(0, std::memory_order_relaxed);
            ended.store(false, std::memory_order_release);
            seekRequested.store(true, std::memory_order_release);
        }

        ma_result result = ma_device_start(&device);
        if (result != MA_SUCCESS) {
            Napi::Error::New(env, "Failed to resume playback!")
                .ThrowAsJavaScriptException();
            return env.Null();
        }

        playing.store(true, std::memory_order_release);
    }

    return env.Undefined();
}

Napi::Value AudioPlayer::TogglePlayPause(const Napi::CallbackInfo &info) {
    const Napi::Env env = info.Env();
    if (playing.load(std::memory_order_relaxed)) {
        Pause(info);
    } else {
        Resume(info);
    }

    return Napi::Boolean::New(env, playing.load(std::memory_order_acquire));
}

Napi::Value AudioPlayer::Load(const Napi::CallbackInfo& info) {
    const Napi::Env env = info.Env();

    if (info.Length() < 1 || !info[0].IsString()) {
        Napi::TypeError::New(env, "Expected a path (string).")
        .ThrowAsJavaScriptException();
        return env.Null();
    }
    const std::string utf8Path = info[0].As<Napi::String>().Utf8Value();

    InitDevice(env);
    if (!deviceInitialized) {
        return env.Null();
    }

    ma_device_stop(&device);

    {
        std::lock_guard lock(decoderMutex);

        if (decoder) {
            ma_decoder_uninit(decoder.get());
            decoder.reset();
        }

        if (nextDecoder) {
            ma_decoder_uninit(nextDecoder.get());
            nextDecoder.reset();
        }

        auto newDecoder = std::make_unique<ma_decoder>();
        *newDecoder = {};

        const auto result = InitDecoderFromPath(info, utf8Path, newDecoder.get());
        if (result != MA_SUCCESS) {
            std::string message = "Invalid file path: \"" + utf8Path + "\"";
            Napi::Error::New(env, message).ThrowAsJavaScriptException();
            return env.Null();
        }

        decoder = std::move(newDecoder);

        songFramesPlayed.store(0, std::memory_order_relaxed);
        ended.store(false, std::memory_order_relaxed);
        seekRequested.store(false, std::memory_order_relaxed);
        playing.store(true, std::memory_order_release);
        trackIndex.store(0, std::memory_order_relaxed);
        prepareGeneration.fetch_add(1, std::memory_order_acq_rel);
    }

    ma_result result = ma_device_start(&device);
    if (result != MA_SUCCESS) {
        std::lock_guard lock(decoderMutex);

        if (decoder) {
            ma_decoder_uninit(decoder.get());
            decoder.reset();
        }

        if (nextDecoder) {
            ma_decoder_uninit(nextDecoder.get());
            nextDecoder.reset();
        }

        playing.store(false, std::memory_order_release);

        Napi::Error::New(env, "Failed to start playback device!")
        .ThrowAsJavaScriptException();
        return env.Null();
    }

    DebugLog(
        env,
        std::string("Decoder initialized to device format [format=")
        + ma_get_format_name(decoder.get()->outputFormat)
        + ", channels=" + std::to_string(decoder.get()->outputChannels)
        + ", sampleRate=" + std::to_string(decoder.get()->outputSampleRate)
        + "]"
    );

    return env.Undefined();
}

ma_result AudioPlayer::InitDecoderFromPath(const Napi::CallbackInfo &info, const std::string& utf8Path, ma_decoder* outDecoder) {
    const ma_decoder_config decoderConfig = ma_decoder_config_init(deviceFormat, deviceChannels, deviceSampleRate);

#ifdef _WIN32
    std::u16string path16 = info[0].As<Napi::String>().Utf16Value();
    std::wstring path(path16.begin(), path16.end());

    return ma_decoder_init_file_w(path.c_str(), &decoderConfig, outDecoder);
#else
    return ma_decoder_init_file(utf8Path.c_str(), &decoderConfig, outDecoder);
#endif
}

void AudioPlayer::InitDevice(const Napi::Env env) {
    if (deviceInitialized) {
        return;
    }
    ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);

    // device defaults
    deviceConfig.playback.format = ma_format_unknown;
    deviceConfig.playback.channels = 0;
    deviceConfig.sampleRate = 0;
    deviceConfig.dataCallback = DataCallback;
    deviceConfig.pUserData = this;
    deviceConfig.noPreSilencedOutputBuffer = false;

    if (ma_device_init(nullptr, &deviceConfig, &device) != MA_SUCCESS) {
        Napi::Error::New(env, "Failed to open playback device!")
            .ThrowAsJavaScriptException();
        return;
    }

    deviceInitialized = true;

    deviceFormat = device.playback.format;
    deviceChannels = device.playback.channels;
    deviceSampleRate = device.sampleRate;

    ma_device_set_master_volume(&device, volume);

    DebugLog(
        env,
        std::string("Playback device initialized [format=")
        + ma_get_format_name(device.playback.format)
        + ", channels=" + std::to_string(device.playback.channels)
        + ", sampleRate=" + std::to_string( device.sampleRate)
        + "]"
    );
}

Napi::Value AudioPlayer::PrepareNext(const Napi::CallbackInfo &info) {
    const Napi::Env env = info.Env();

    if (info.Length() < 1 || !info[0].IsString()) {
        Napi::TypeError::New(env, "Expected a path (string).")
        .ThrowAsJavaScriptException();
        return env.Null();
    }
    const std::string utf8Path = info[0].As<Napi::String>().Utf8Value();

    InitDevice(env);
    if (!deviceInitialized) {
        return env.Null();
    }

    const ma_uint64 myGeneration = prepareGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;

    auto tempDecoder = std::make_unique<ma_decoder>();
    *tempDecoder = {};

    const auto result = InitDecoderFromPath(info, utf8Path, tempDecoder.get());

    if (result != MA_SUCCESS) {
        std::string message = "Invalid file path: \"" + utf8Path + "\"";
        Napi::Error::New(env, message).ThrowAsJavaScriptException();
        return env.Null();
    }

    {
        std::lock_guard lock(decoderMutex);

        if (prepareGeneration.load(std::memory_order_acquire) != myGeneration) {
            ma_decoder_uninit(tempDecoder.get());
            return env.Undefined();
        }

        if (nextDecoder) {
            ma_decoder_uninit(nextDecoder.get());
        }

        nextDecoder = std::move(tempDecoder);
    }

    DebugLog(env, "Prepared next decoder");
    return env.Undefined();
}

Napi::Value AudioPlayer::GetState(const Napi::CallbackInfo& info) {
    const auto env = info.Env();
    const ma_uint64 frames = songFramesPlayed.load(std::memory_order_relaxed);

    ma_uint32 sampleRate = 0;

    {
        std::lock_guard lock(decoderMutex);

        if (decoder) {
            sampleRate = decoder->outputSampleRate;
        }
    }

    double seconds = 0.0;
    if (sampleRate > 0) {
        seconds = static_cast<double>(frames) / static_cast<double>(sampleRate);
    }

    Napi::Object state = Napi::Object::New(env);
    state.Set("songFramesPlayed", Napi::Number::New(env, static_cast<double>(frames)));
    state.Set("songSecondsPlayed", Napi::Number::New(env, seconds));
    state.Set("ended", Napi::Boolean::New(env, ended.load(std::memory_order_relaxed)));
    state.Set("playing", Napi::Boolean::New(env,playing.load(std::memory_order_acquire)));

    return state;
}

Napi::Value AudioPlayer::OnEnded(const Napi::CallbackInfo &info) {
    auto env = info.Env();

    if (info.Length() < 1 || !info[0].IsFunction()) {
        Napi::TypeError::New(env, "Expected callback function.").ThrowAsJavaScriptException();
        return env.Null();
    }

    std::lock_guard lock(callbackMutex);

    if (endedCallbackSet.load(std::memory_order_relaxed)) {
        endedCallback.Release();
        endedCallbackSet.store(false, std::memory_order_relaxed);
    }

    endedCallback = Napi::ThreadSafeFunction::New(env, info[0].As<Napi::Function>(),
        "AudioPlayerEndedCallback", 0, 1);

    endedCallbackSet.store(true, std::memory_order_relaxed);
    return env.Undefined();
}

Napi::Value AudioPlayer::OnAdvanced(const Napi::CallbackInfo &info) {
    const auto env = info.Env();

    if (info.Length() < 1 || !info[0].IsFunction()) {
        Napi::TypeError::New(env, "Expected callback function.").ThrowAsJavaScriptException();
        return env.Null();
    }

    std::lock_guard lock(callbackMutex);

    if (advancedCallbackSet.load(std::memory_order_relaxed)) {
        advancedCallback.Release();
        advancedCallbackSet.store(false, std::memory_order_relaxed);
    }

    advancedCallback = Napi::ThreadSafeFunction::New(env,info[0].As<Napi::Function>(),
        "AudioPlayerAdvancedCallback",0,1);

    advancedCallbackSet.store(true, std::memory_order_release);
    return env.Undefined();
}
