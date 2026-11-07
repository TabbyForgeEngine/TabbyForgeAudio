
#pragma once

#include "Common/PlatformDetection.hpp"

#include <cstdint>
#include <filesystem>

using AudioBufferHandle = uint64_t;
using AudioSourceHandle = uint64_t;

inline constexpr AudioBufferHandle INVALID_AUDIO_BUFFER = 0;
inline constexpr AudioSourceHandle INVALID_AUDIO_SOURCE = 0;

enum class AudioState : uint8_t
{
    Stopped,
    Playing,
    Paused,
    Invalid
};

extern "C"
{
    // --------------------------------------------------------
    // Lifecycle
    // --------------------------------------------------------

    void AudioAPI_INIT();

    void Audio_API AudioAPI_Shutdown();


    // --------------------------------------------------------
    // Buffer management
    // --------------------------------------------------------

    AudioBufferHandle Audio_API AudioAPI_LoadFile(
        const std::filesystem::path& filepath);

    void Audio_API AudioAPI_UnloadBuffer(
        AudioBufferHandle handle);


    // --------------------------------------------------------
    // Source management
    // --------------------------------------------------------

    AudioSourceHandle Audio_API AudioAPI_CreateSource(
        AudioBufferHandle buffer);

    void Audio_API AudioAPI_DestroySource(
        AudioSourceHandle source);


    // --------------------------------------------------------
    // Playback
    // --------------------------------------------------------

    void Audio_API AudioAPI_Play(
        AudioSourceHandle source);

    void Audio_API AudioAPI_Stop(
        AudioSourceHandle source);

    void Audio_API AudioAPI_Pause(
        AudioSourceHandle source);

    void Audio_API AudioAPI_Resume(
        AudioSourceHandle source);


    // --------------------------------------------------------
    // Configuration
    // --------------------------------------------------------

    void Audio_API AudioAPI_SetVolume(
        AudioSourceHandle source,
        float volume);

    void Audio_API AudioAPI_SetLooping(
        AudioSourceHandle source,
        bool looping);


    // --------------------------------------------------------
    // State
    // --------------------------------------------------------

    AudioState Audio_API AudioAPI_GetState(
        AudioSourceHandle source);
}
