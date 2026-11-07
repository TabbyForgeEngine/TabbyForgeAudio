#pragma once

#include "Common/PlatformDetection.hpp"
#include <cstdint>
#include <filesystem>

using AudioHandle = uint64_t;

constexpr AudioHandle INVALID_AUDIO_HANDLE = 0;

extern "C"
{
    void AudioAPI_INIT();

    AudioHandle Audio_API AudioAPI_LoadFile(
        const std::filesystem::path& filepath);

    void Audio_API AudioAPI_UnloadFile(AudioHandle handle);

    void Audio_API AudioAPI_Play(AudioHandle handle);

    void Audio_API AudioAPI_Shutdown();
}