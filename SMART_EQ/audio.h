#ifndef AUDIO_H
#define AUDIO_H

#include <string>
#include <vector>
#include <cstdint>
#include <filesystem>

struct AudioData
{
    unsigned int channels;
    unsigned int sampleRate;
    std::uint64_t frameCount;
    std::vector<float> samples;
};

bool loadAudio(
    const std::filesystem::path& filename,
    AudioData& audio,
    std::string& error
);

bool saveWav(
    const std::string& filename,
    const AudioData& audio,
    std::string& error
);

bool saveMp3(
    const std::filesystem::path& filename,
    const AudioData& audio,
    std::string& error
);

bool saveFlac(
    const std::filesystem::path& filename,
    const AudioData& audio,
    std::string& error
);

bool saveAudio(
    const std::filesystem::path& filename,
    const AudioData& audio,
    std::string& error
);

#endif