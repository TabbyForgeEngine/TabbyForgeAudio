#include "Audio_API.h"
#include "alc.h"
#include "Filesystem.h"

#include <miniaudio.h>

#include <AL/al.h>
#include <AL/alc.h>

#if defined(__has_include)
    #if __has_include(<AL/alext.h>)
        #include <AL/alext.h>
    #endif
#endif

#ifndef AL_FORMAT_MONO_FLOAT32
    #define AL_FORMAT_MONO_FLOAT32 0x10010
#endif

#ifndef AL_FORMAT_STEREO_FLOAT32
    #define AL_FORMAT_STEREO_FLOAT32 0x10011
#endif

#include <vector>
#include <cstddef>
#include <limits>

// ------------------------------------------------------------
// Audio structures
// ------------------------------------------------------------

struct AudioSource
{
    ALuint buffer = 0;
    ALuint source = 0;

    uint32_t generation = 1;
    bool active = false;
};

struct AudioAPIData
{
    ALCdevice* device = nullptr;
    ALCcontext* context = nullptr;

    std::vector<AudioSource> audioSources;
};

static AudioAPIData* s_Data = nullptr;


// ------------------------------------------------------------
// Handle management
// ------------------------------------------------------------

static AudioHandle CreateHandle(size_t index, uint32_t generation)
{
    return (static_cast<uint64_t>(generation) << 32)
         | (static_cast<uint64_t>(index) + 1);
}

static bool DecodeHandle(
    AudioHandle handle,
    size_t& index,
    uint32_t& generation)
{
    if (handle == INVALID_AUDIO_HANDLE)
        return false;

    uint32_t encodedIndex = static_cast<uint32_t>(handle);
    generation = static_cast<uint32_t>(handle >> 32);

    if (encodedIndex == 0 || generation == 0)
        return false;

    index = static_cast<size_t>(encodedIndex - 1);

    if (index >= s_Data->audioSources.size())
        return false;

    AudioSource& audio = s_Data->audioSources[index];

    return audio.active && audio.generation == generation;
}


// ------------------------------------------------------------
// Format conversion
// ------------------------------------------------------------

static ALenum MaFormatToAL(ma_format fmt, ma_uint32 channels)
{
    if (channels == 1)
    {
        switch (fmt)
        {
            case ma_format_u8:
                return AL_FORMAT_MONO8;

            case ma_format_s16:
                return AL_FORMAT_MONO16;

            case ma_format_f32:
                return AL_FORMAT_MONO_FLOAT32;

            default:
                break;
        }
    }
    else if (channels == 2)
    {
        switch (fmt)
        {
            case ma_format_u8:
                return AL_FORMAT_STEREO8;

            case ma_format_s16:
                return AL_FORMAT_STEREO16;

            case ma_format_f32:
                return AL_FORMAT_STEREO_FLOAT32;

            default:
                break;
        }
    }

    return 0;
}


// ------------------------------------------------------------
// Initialization
// ------------------------------------------------------------

void AudioAPI_INIT()
{
    if (s_Data)
        return;

    s_Data = new AudioAPIData();

    s_Data->device = alcOpenDevice(nullptr);

    if (!s_Data->device)
        return;

    s_Data->context = alcCreateContext(s_Data->device, nullptr);

    if (!s_Data->context)
    {
        alcCloseDevice(s_Data->device);
        s_Data->device = nullptr;
        return;
    }

    if (!alcMakeContextCurrent(s_Data->context))
    {
        alcDestroyContext(s_Data->context);
        alcCloseDevice(s_Data->device);

        s_Data->context = nullptr;
        s_Data->device = nullptr;
    }
}


// ------------------------------------------------------------
// Load audio file
// ------------------------------------------------------------

AudioHandle Audio_API AudioAPI_LoadFile(
    const std::filesystem::path& filepath)
{
    if (!s_Data || !s_Data->context)
        return INVALID_AUDIO_HANDLE;

    // Load file into memory.

    auto buffer = Filesystem::LoadFile(filepath);

    if (!buffer)
        return INVALID_AUDIO_HANDLE;

    // Initialize miniaudio decoder.

    ma_decoder_config cfg =
        ma_decoder_config_init(ma_format_unknown, 0, 0);

    ma_decoder decoder;

    if (ma_decoder_init_memory(
        buffer->Data(),
        buffer->Size(),
        &cfg,
        &decoder) != MA_SUCCESS)
    {
        return INVALID_AUDIO_HANDLE;
    }

    // Get audio information.

    ma_uint64 totalFrames = 0;

    if (ma_decoder_get_length_in_pcm_frames(
        &decoder, &totalFrames) != MA_SUCCESS ||
        totalFrames == 0)
    {
        ma_decoder_uninit(&decoder);
        return INVALID_AUDIO_HANDLE;
    }

    ma_uint32 channels = decoder.outputChannels;
    ma_uint32 sampleRate = decoder.outputSampleRate;

    ma_format format = decoder.outputFormat;

    ALenum alFormat = MaFormatToAL(format, channels);

    if (alFormat == 0)
    {
        ma_decoder_uninit(&decoder);
        return INVALID_AUDIO_HANDLE;
    }

    // Read decoded PCM data.

    ma_uint64 bytesPerFrame =
        ma_get_bytes_per_frame(format, channels);

    if (bytesPerFrame == 0 ||
        totalFrames > std::numeric_limits<size_t>::max() / bytesPerFrame)
    {
        ma_decoder_uninit(&decoder);
        return INVALID_AUDIO_HANDLE;
    }

    std::vector<std::byte> pcm(
        static_cast<size_t>(totalFrames * bytesPerFrame));

    ma_uint64 framesRead = 0;

    ma_result result = ma_decoder_read_pcm_frames(
        &decoder,
        pcm.data(),
        totalFrames,
        &framesRead);

    ma_decoder_uninit(&decoder);

    if (result != MA_SUCCESS || framesRead == 0)
        return INVALID_AUDIO_HANDLE;

    ma_uint64 dataSize = framesRead * bytesPerFrame;

    if (dataSize > static_cast<ma_uint64>(
        std::numeric_limits<ALsizei>::max()))
    {
        return INVALID_AUDIO_HANDLE;
    }

    // Generate OpenAL buffer.

    ALuint alBuffer = 0;

    alGenBuffers(1, &alBuffer);

    if (alGetError() != AL_NO_ERROR || alBuffer == 0)
        return INVALID_AUDIO_HANDLE;

    alBufferData(
        alBuffer,
        alFormat,
        pcm.data(),
        static_cast<ALsizei>(dataSize),
        static_cast<ALsizei>(sampleRate));

    if (alGetError() != AL_NO_ERROR)
    {
        alDeleteBuffers(1, &alBuffer);
        return INVALID_AUDIO_HANDLE;
    }

    // Generate OpenAL source.

    ALuint source = 0;

    alGenSources(1, &source);

    if (alGetError() != AL_NO_ERROR || source == 0)
    {
        alDeleteBuffers(1, &alBuffer);
        return INVALID_AUDIO_HANDLE;
    }

    alSourcei(source, AL_BUFFER, static_cast<ALint>(alBuffer));

    alSourcei(source, AL_SOURCE_RELATIVE, AL_TRUE);
    alSourcef(source, AL_GAIN, 1.0f);
    alSourcei(source, AL_LOOPING, AL_TRUE);

    if (alGetError() != AL_NO_ERROR)
    {
        alDeleteSources(1, &source);
        alDeleteBuffers(1, &alBuffer);
        return INVALID_AUDIO_HANDLE;
    }

    // Find a reusable slot.

    size_t index = 0;

    for (; index < s_Data->audioSources.size(); ++index)
    {
        if (!s_Data->audioSources[index].active)
            break;
    }

    if (index == s_Data->audioSources.size())
    {
        s_Data->audioSources.emplace_back();
    }

    AudioSource& audio = s_Data->audioSources[index];

    audio.buffer = alBuffer;
    audio.source = source;
    audio.active = true;

    return CreateHandle(index, audio.generation);
}


// ------------------------------------------------------------
// Play audio
// ------------------------------------------------------------

void Audio_API AudioAPI_Play(AudioHandle handle)
{
    if (!s_Data)
        return;

    size_t index;
    uint32_t generation;

    if (!DecodeHandle(handle, index, generation))
        return;

    AudioSource& audio = s_Data->audioSources[index];

    alSourcePlay(audio.source);
}


// ------------------------------------------------------------
// Stop audio
// ------------------------------------------------------------

void Audio_API AudioAPI_Stop(AudioHandle handle)
{
    if (!s_Data)
        return;

    size_t index;
    uint32_t generation;

    if (!DecodeHandle(handle, index, generation))
        return;

    AudioSource& audio = s_Data->audioSources[index];

    alSourceStop(audio.source);
}


// ------------------------------------------------------------
// Unload audio
// ------------------------------------------------------------

void Audio_API AudioAPI_UnloadFile(AudioHandle handle)
{
    if (!s_Data)
        return;

    size_t index;
    uint32_t generation;

    if (!DecodeHandle(handle, index, generation))
        return;

    AudioSource& audio = s_Data->audioSources[index];

    // Stop playback.

    alSourceStop(audio.source);

    // Delete OpenAL resources.

    alDeleteSources(1, &audio.source);
    alDeleteBuffers(1, &audio.buffer);

    // Reset slot.

    audio.source = 0;
    audio.buffer = 0;
    audio.active = false;

    // Increment generation to invalidate old handles.

    ++audio.generation;

    if (audio.generation == 0)
        ++audio.generation;
}


// ------------------------------------------------------------
// Shutdown
// ------------------------------------------------------------

void Audio_API AudioAPI_Shutdown()
{
    if (!s_Data)
        return;

    if (s_Data->context)
    {
        alcMakeContextCurrent(s_Data->context);

        for (AudioSource& audio : s_Data->audioSources)
        {
            if (!audio.active)
                continue;

            alSourceStop(audio.source);

            alDeleteSources(1, &audio.source);
            alDeleteBuffers(1, &audio.buffer);
        }

        alcMakeContextCurrent(nullptr);

        alcDestroyContext(s_Data->context);
        s_Data->context = nullptr;
    }

    if (s_Data->device)
    {
        alcCloseDevice(s_Data->device);
        s_Data->device = nullptr;
    }

    delete s_Data;
    s_Data = nullptr;
}