
#include "Audio_API.h"

#include "Filesystem.h"

#include <miniaudio.h>

#include <AL/al.h>
#include <AL/alc.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

// ============================================================
// OpenAL formats
// ============================================================

#ifndef AL_FORMAT_MONO_FLOAT32
    #define AL_FORMAT_MONO_FLOAT32 0x10010
#endif

#ifndef AL_FORMAT_STEREO_FLOAT32
    #define AL_FORMAT_STEREO_FLOAT32 0x10011
#endif


// ============================================================
// Audio structures
// ============================================================

struct AudioBuffer
{
    ALuint buffer = 0;

    uint32_t generation = 1;

    // Includes the public handle and all referencing sources.
    uint32_t references = 0;

    bool active = false;
    bool ownerReleased = false;
};

struct AudioSource
{
    ALuint source = 0;

    // Internal reference to the buffer slot.
    size_t bufferIndex = 0;
    uint32_t bufferGeneration = 0;

    uint32_t generation = 1;

    float volume = 1.0f;
    bool looping = true;

    bool active = false;
};

struct AudioAPIData
{
    ALCdevice* device = nullptr;
    ALCcontext* context = nullptr;

    std::vector<AudioBuffer> buffers;
    std::vector<AudioSource> sources;
};


// ============================================================
// Global state
// ============================================================

static AudioAPIData* s_Data = nullptr;

static std::mutex s_AudioMutex;


// ============================================================
// OpenAL error handling
// ============================================================

static void ClearALErrors()
{
    while (alGetError() != AL_NO_ERROR)
    {
    }
}

static bool CheckALError()
{
    return alGetError() == AL_NO_ERROR;
}


// ============================================================
// Context management
// ============================================================

static bool MakeContextCurrent()
{
    if (!s_Data || !s_Data->context)
        return false;

    return alcMakeContextCurrent(s_Data->context) == ALC_TRUE;
}


// ============================================================
// Generic handle management
// ============================================================

static uint64_t CreateHandle(
    size_t index,
    uint32_t generation)
{
    return
        (static_cast<uint64_t>(generation) << 32) |
        (static_cast<uint64_t>(index) + 1);
}

static bool DecodeHandle(
    uint64_t handle,
    size_t& index,
    uint32_t& generation)
{
    if (handle == 0)
        return false;

    const uint32_t encodedIndex =
        static_cast<uint32_t>(handle);

    generation = static_cast<uint32_t>(handle >> 32);

    if (encodedIndex == 0 || generation == 0)
        return false;

    index = static_cast<size_t>(encodedIndex - 1);

    return true;
}


// ============================================================
// Buffer handle management
// ============================================================

static bool GetBuffer(
    AudioBufferHandle handle,
    size_t& index,
    AudioBuffer*& buffer)
{
    uint32_t generation = 0;

    if (!DecodeHandle(handle, index, generation))
        return false;

    if (index >= s_Data->buffers.size())
        return false;

    AudioBuffer& candidate = s_Data->buffers[index];

    if (!candidate.active ||
        candidate.ownerReleased ||
        candidate.generation != generation)
    {
        return false;
    }

    buffer = &candidate;

    return true;
}


// ============================================================
// Source handle management
// ============================================================

static bool GetSource(
    AudioSourceHandle handle,
    size_t& index,
    AudioSource*& source)
{
    uint32_t generation = 0;

    if (!DecodeHandle(handle, index, generation))
        return false;

    if (index >= s_Data->sources.size())
        return false;

    AudioSource& candidate = s_Data->sources[index];

    if (!candidate.active ||
        candidate.generation != generation)
    {
        return false;
    }

    source = &candidate;

    return true;
}


// ============================================================
// Slot allocation
// ============================================================

static size_t AllocateBufferSlot()
{
    for (size_t i = 0; i < s_Data->buffers.size(); ++i)
    {
        const AudioBuffer& buffer = s_Data->buffers[i];

        if (!buffer.active && buffer.generation != 0)
            return i;
    }

    s_Data->buffers.emplace_back();

    return s_Data->buffers.size() - 1;
}

static size_t AllocateSourceSlot()
{
    for (size_t i = 0; i < s_Data->sources.size(); ++i)
    {
        const AudioSource& source = s_Data->sources[i];

        if (!source.active && source.generation != 0)
            return i;
    }

    s_Data->sources.emplace_back();

    return s_Data->sources.size() - 1;
}


// ============================================================
// Generation invalidation
// ============================================================

static void InvalidateBuffer(AudioBuffer& buffer)
{
    buffer.buffer = 0;
    buffer.references = 0;

    buffer.active = false;
    buffer.ownerReleased = false;

    if (buffer.generation == UINT32_MAX)
        buffer.generation = 0;
    else
        ++buffer.generation;
}

static void InvalidateSource(AudioSource& source)
{
    source.source = 0;

    source.bufferIndex = 0;
    source.bufferGeneration = 0;

    source.volume = 1.0f;
    source.looping = true;

    source.active = false;

    if (source.generation == UINT32_MAX)
        source.generation = 0;
    else
        ++source.generation;
}


// ============================================================
// Audio buffer destruction
// ============================================================

static void DestroyBuffer(size_t index)
{
    AudioBuffer& buffer = s_Data->buffers[index];

    if (!buffer.active || buffer.references != 0)
        return;

    ClearALErrors();

    if (buffer.buffer != 0)
        alDeleteBuffers(1, &buffer.buffer);

    CheckALError();

    InvalidateBuffer(buffer);
}


// ============================================================
// Buffer reference management
// ============================================================

static void ReleaseBufferReference(
    size_t index,
    uint32_t generation)
{
    if (index >= s_Data->buffers.size())
        return;

    AudioBuffer& buffer = s_Data->buffers[index];

    if (!buffer.active ||
        buffer.generation != generation ||
        buffer.references == 0)
    {
        return;
    }

    --buffer.references;

    if (buffer.references == 0)
        DestroyBuffer(index);
}


// ============================================================
// Audio format conversion
// ============================================================

static ALenum MaFormatToAL(
    ma_format format,
    ma_uint32 channels)
{
    if (channels == 1)
    {
        switch (format)
        {
            case ma_format_u8:
                return AL_FORMAT_MONO8;

            case ma_format_s16:
                return AL_FORMAT_MONO16;

            case ma_format_f32:
                return AL_FORMAT_MONO_FLOAT32;

            default:
                return 0;
        }
    }

    if (channels == 2)
    {
        switch (format)
        {
            case ma_format_u8:
                return AL_FORMAT_STEREO8;

            case ma_format_s16:
                return AL_FORMAT_STEREO16;

            case ma_format_f32:
                return AL_FORMAT_STEREO_FLOAT32;

            default:
                return 0;
        }
    }

    return 0;
}


// ============================================================
// Audio decoding
// ============================================================

static bool DecodeAudioFile(
    const std::filesystem::path& filepath,
    std::vector<std::byte>& pcm,
    ALenum& alFormat,
    ma_uint32& sampleRate)
{
    auto file = Filesystem::LoadFile(filepath);

    if (!file)
        return false;

    ma_decoder_config config =
        ma_decoder_config_init(
            ma_format_unknown,
            0,
            0);

    ma_decoder decoder{};

    if (ma_decoder_init_memory(
        file->Data(),
        file->Size(),
        &config,
        &decoder) != MA_SUCCESS)
    {
        return false;
    }

    ma_uint64 totalFrames = 0;

    if (ma_decoder_get_length_in_pcm_frames(
        &decoder,
        &totalFrames) != MA_SUCCESS ||
        totalFrames == 0)
    {
        ma_decoder_uninit(&decoder);
        return false;
    }

    const ma_uint32 channels = decoder.outputChannels;

    sampleRate = decoder.outputSampleRate;

    const ma_format format = decoder.outputFormat;

    alFormat = MaFormatToAL(format, channels);

    if (alFormat == 0 || sampleRate == 0)
    {
        ma_decoder_uninit(&decoder);
        return false;
    }

    const ma_uint64 bytesPerFrame =
        ma_get_bytes_per_frame(format, channels);

    if (bytesPerFrame == 0 ||
        totalFrames >
            std::numeric_limits<size_t>::max() / bytesPerFrame)
    {
        ma_decoder_uninit(&decoder);
        return false;
    }

    pcm.resize(
        static_cast<size_t>(
            totalFrames * bytesPerFrame));

    ma_uint64 framesRead = 0;

    const ma_result result =
        ma_decoder_read_pcm_frames(
            &decoder,
            pcm.data(),
            totalFrames,
            &framesRead);

    ma_decoder_uninit(&decoder);

    if ((result != MA_SUCCESS && result != MA_AT_END) ||
        framesRead == 0)
    {
        pcm.clear();
        return false;
    }

    const ma_uint64 dataSize =
        framesRead * bytesPerFrame;

    if (dataSize >
        static_cast<ma_uint64>(
            std::numeric_limits<ALsizei>::max()))
    {
        pcm.clear();
        return false;
    }

    if (sampleRate >
        static_cast<ma_uint32>(
            std::numeric_limits<ALsizei>::max()))
    {
        pcm.clear();
        return false;
    }

    pcm.resize(static_cast<size_t>(dataSize));

    return true;
}


// ============================================================
// Initialization
// ============================================================

void AudioAPI_INIT()
{
    std::lock_guard<std::mutex> lock(s_AudioMutex);

    if (s_Data)
        return;

    auto data = std::make_unique<AudioAPIData>();

    data->device = alcOpenDevice(nullptr);

    if (!data->device)
        return;

    data->context =
        alcCreateContext(data->device, nullptr);

    if (!data->context)
    {
        alcCloseDevice(data->device);
        return;
    }

    if (!alcMakeContextCurrent(data->context))
    {
        alcDestroyContext(data->context);
        alcCloseDevice(data->device);
        return;
    }

    s_Data = data.release();
}


// ============================================================
// Load audio file
// ============================================================

AudioBufferHandle Audio_API AudioAPI_LoadFile(
    const std::filesystem::path& filepath)
{
    std::lock_guard<std::mutex> lock(s_AudioMutex);

    if (!s_Data || !MakeContextCurrent())
        return INVALID_AUDIO_BUFFER;

    // Decode file.

    std::vector<std::byte> pcm;

    ALenum format = 0;
    ma_uint32 sampleRate = 0;

    if (!DecodeAudioFile(
        filepath,
        pcm,
        format,
        sampleRate))
    {
        return INVALID_AUDIO_BUFFER;
    }

    // Generate OpenAL buffer.

    ALuint alBuffer = 0;

    ClearALErrors();

    alGenBuffers(1, &alBuffer);

    if (!CheckALError() || alBuffer == 0)
    {
        if (alBuffer != 0)
            alDeleteBuffers(1, &alBuffer);

        return INVALID_AUDIO_BUFFER;
    }

    // Upload PCM.

    ClearALErrors();

    alBufferData(
        alBuffer,
        format,
        pcm.data(),
        static_cast<ALsizei>(pcm.size()),
        static_cast<ALsizei>(sampleRate));

    if (!CheckALError())
    {
        alDeleteBuffers(1, &alBuffer);
        return INVALID_AUDIO_BUFFER;
    }

    // Allocate buffer slot.

    size_t index = 0;

    try
    {
        index = AllocateBufferSlot();
    }
    catch (...)
    {
        alDeleteBuffers(1, &alBuffer);
        return INVALID_AUDIO_BUFFER;
    }

    AudioBuffer& buffer = s_Data->buffers[index];

    buffer.buffer = alBuffer;
    buffer.references = 1;

    buffer.active = true;
    buffer.ownerReleased = false;

    return CreateHandle(index, buffer.generation);
}


// ============================================================
// Unload audio buffer
// ============================================================

void Audio_API AudioAPI_UnloadBuffer(
    AudioBufferHandle handle)
{
    std::lock_guard<std::mutex> lock(s_AudioMutex);

    if (!s_Data || !MakeContextCurrent())
        return;

    size_t index;
    AudioBuffer* buffer = nullptr;

    if (!GetBuffer(handle, index, buffer))
        return;

    // Release the public ownership reference.

    buffer->ownerReleased = true;

    ReleaseBufferReference(
        index,
        buffer->generation);
}


// ============================================================
// Create audio source
// ============================================================

AudioSourceHandle Audio_API AudioAPI_CreateSource(
    AudioBufferHandle bufferHandle)
{
    std::lock_guard<std::mutex> lock(s_AudioMutex);

    if (!s_Data || !MakeContextCurrent())
        return INVALID_AUDIO_SOURCE;

    // Validate buffer.

    size_t bufferIndex;
    AudioBuffer* buffer = nullptr;

    if (!GetBuffer(bufferHandle, bufferIndex, buffer))
        return INVALID_AUDIO_SOURCE;

    // Generate OpenAL source.

    ALuint alSource = 0;

    ClearALErrors();

    alGenSources(1, &alSource);

    if (!CheckALError() || alSource == 0)
    {
        if (alSource != 0)
            alDeleteSources(1, &alSource);

        return INVALID_AUDIO_SOURCE;
    }

    // Attach shared buffer.

    ClearALErrors();

    alSourcei(
        alSource,
        AL_BUFFER,
        static_cast<ALint>(buffer->buffer));

    alSourcei(alSource, AL_SOURCE_RELATIVE, AL_TRUE);

    alSourcef(alSource, AL_GAIN, 1.0f);

    alSourcei(alSource, AL_LOOPING, AL_TRUE);

    if (!CheckALError())
    {
        alDeleteSources(1, &alSource);
        return INVALID_AUDIO_SOURCE;
    }

    // Allocate source slot.

    size_t sourceIndex = 0;

    try
    {
        sourceIndex = AllocateSourceSlot();
    }
    catch (...)
    {
        alDeleteSources(1, &alSource);
        return INVALID_AUDIO_SOURCE;
    }

    // Increment buffer reference count.

    if (buffer->references == UINT32_MAX)
    {
        alDeleteSources(1, &alSource);
        return INVALID_AUDIO_SOURCE;
    }

    ++buffer->references;

    // Initialize source.

    AudioSource& source = s_Data->sources[sourceIndex];

    source.source = alSource;

    source.bufferIndex = bufferIndex;
    source.bufferGeneration = buffer->generation;

    source.volume = 1.0f;
    source.looping = true;

    source.active = true;

    return CreateHandle(
        sourceIndex,
        source.generation);
}


// ============================================================
// Destroy audio source
// ============================================================

void Audio_API AudioAPI_DestroySource(
    AudioSourceHandle handle)
{
    std::lock_guard<std::mutex> lock(s_AudioMutex);

    if (!s_Data || !MakeContextCurrent())
        return;

    size_t index;
    AudioSource* source = nullptr;

    if (!GetSource(handle, index, source))
        return;

    const size_t bufferIndex = source->bufferIndex;

    const uint32_t bufferGeneration =
        source->bufferGeneration;

    // Stop playback.

    ClearALErrors();

    alSourceStop(source->source);

    // Detach buffer.

    alSourcei(source->source, AL_BUFFER, 0);

    // Delete source.

    alDeleteSources(1, &source->source);

    CheckALError();

    // Invalidate source handle.

    InvalidateSource(*source);

    // Release buffer reference.

    ReleaseBufferReference(
        bufferIndex,
        bufferGeneration);
}


// ============================================================
// Play
// ============================================================

void Audio_API AudioAPI_Play(AudioSourceHandle handle)
{
    std::lock_guard<std::mutex> lock(s_AudioMutex);

    if (!s_Data || !MakeContextCurrent())
        return;

    size_t index;
    AudioSource* source = nullptr;

    if (!GetSource(handle, index, source))
        return;

    ClearALErrors();

    alSourcePlay(source->source);

    CheckALError();
}


// ============================================================
// Stop
// ============================================================

void Audio_API AudioAPI_Stop(AudioSourceHandle handle)
{
    std::lock_guard<std::mutex> lock(s_AudioMutex);

    if (!s_Data || !MakeContextCurrent())
        return;

    size_t index;
    AudioSource* source = nullptr;

    if (!GetSource(handle, index, source))
        return;

    ClearALErrors();

    alSourceStop(source->source);

    CheckALError();
}


// ============================================================
// Pause
// ============================================================

void Audio_API AudioAPI_Pause(AudioSourceHandle handle)
{
    std::lock_guard<std::mutex> lock(s_AudioMutex);

    if (!s_Data || !MakeContextCurrent())
        return;

    size_t index;
    AudioSource* source = nullptr;

    if (!GetSource(handle, index, source))
        return;

    ClearALErrors();

    alSourcePause(source->source);

    CheckALError();
}


// ============================================================
// Resume
// ============================================================

void Audio_API AudioAPI_Resume(AudioSourceHandle handle)
{
    std::lock_guard<std::mutex> lock(s_AudioMutex);

    if (!s_Data || !MakeContextCurrent())
        return;

    size_t index;
    AudioSource* source = nullptr;

    if (!GetSource(handle, index, source))
        return;

    ClearALErrors();

    alSourcePlay(source->source);

    CheckALError();
}


// ============================================================
// Set volume
// ============================================================

void Audio_API AudioAPI_SetVolume(
    AudioSourceHandle handle,
    float volume)
{
    std::lock_guard<std::mutex> lock(s_AudioMutex);

    if (!s_Data || !MakeContextCurrent())
        return;

    size_t index;
    AudioSource* source = nullptr;

    if (!GetSource(handle, index, source))
        return;

    volume = std::clamp(volume, 0.0f, 1.0f);

    ClearALErrors();

    alSourcef(
        source->source,
        AL_GAIN,
        volume);

    if (CheckALError())
        source->volume = volume;
}


// ============================================================
// Set looping
// ============================================================

void Audio_API AudioAPI_SetLooping(
    AudioSourceHandle handle,
    bool looping)
{
    std::lock_guard<std::mutex> lock(s_AudioMutex);

    if (!s_Data || !MakeContextCurrent())
        return;

    size_t index;
    AudioSource* source = nullptr;

    if (!GetSource(handle, index, source))
        return;

    ClearALErrors();

    alSourcei(
        source->source,
        AL_LOOPING,
        looping ? AL_TRUE : AL_FALSE);

    if (CheckALError())
        source->looping = looping;
}


// ============================================================
// Get state
// ============================================================

AudioState Audio_API AudioAPI_GetState(
    AudioSourceHandle handle)
{
    std::lock_guard<std::mutex> lock(s_AudioMutex);

    if (!s_Data || !MakeContextCurrent())
        return AudioState::Invalid;

    size_t index;
    AudioSource* source = nullptr;

    if (!GetSource(handle, index, source))
        return AudioState::Invalid;

    ALint state = AL_STOPPED;

    ClearALErrors();

    alGetSourcei(
        source->source,
        AL_SOURCE_STATE,
        &state);

    if (!CheckALError())
        return AudioState::Invalid;

    switch (state)
    {
        case AL_PLAYING:
            return AudioState::Playing;

        case AL_PAUSED:
            return AudioState::Paused;

        case AL_STOPPED:
        case AL_INITIAL:
            return AudioState::Stopped;

        default:
            return AudioState::Invalid;
    }
}


// ============================================================
// Shutdown
// ============================================================

void Audio_API AudioAPI_Shutdown()
{
    std::lock_guard<std::mutex> lock(s_AudioMutex);

    if (!s_Data)
        return;

    if (s_Data->context &&
        alcMakeContextCurrent(s_Data->context))
    {
        // Destroy all sources first.

        for (AudioSource& source : s_Data->sources)
        {
            if (!source.active)
                continue;

            ClearALErrors();

            alSourceStop(source.source);

            alSourcei(source.source, AL_BUFFER, 0);

            alDeleteSources(1, &source.source);

            source.active = false;
        }

        // Then destroy all buffers.

        for (AudioBuffer& buffer : s_Data->buffers)
        {
            if (!buffer.active)
                continue;

            ClearALErrors();

            alDeleteBuffers(1, &buffer.buffer);

            buffer.active = false;
        }

        // Release context.

        alcMakeContextCurrent(nullptr);
    }

    if (s_Data->context)
    {
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
