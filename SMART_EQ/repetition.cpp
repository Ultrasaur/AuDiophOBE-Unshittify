
#include "repetition.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

namespace
{
    struct Match
    {
        size_t first;
        size_t second;
        double gainLeft;
        double gainRight;
    };

    // Создаёт грубую сигнатуру фрагмента.
    // Инверсия полярности не меняет итоговый ключ.
    uint32_t makeSignature(
        const AudioData& audio,
        size_t firstFrame,
        size_t windowFrames)
    {
        uint32_t key = 0;
        size_t bit = 0;

        for (unsigned int channel = 0;
            channel < audio.channels && channel < 2;
            ++channel)
        {
            for (size_t i = 0; i < 16; ++i)
            {
                size_t offset = i * (windowFrames - 1) / 15;
                size_t index =
                    (firstFrame + offset) * audio.channels + channel;

                if (audio.samples[index] >= 0.0f)
                    key |= (1u << bit);

                ++bit;
            }
        }

        uint32_t inverted = ~key;
        return std::min(key, inverted);
    }

    // Проверяет совпадение двух фрагментов.
    // Коэффициенты усиления рассчитываются отдельно для каналов.
    bool compareFragments(
        const AudioData& audio,
        size_t first,
        size_t second,
        size_t windowFrames,
        const RepetitionSettings& settings,
        double& gainLeft,
        double& gainRight)
    {
        double dot[2] = {};
        double energyA[2] = {};
        double energyB[2] = {};

        for (size_t i = 0; i < windowFrames; ++i)
        {
            for (unsigned int ch = 0; ch < audio.channels; ++ch)
            {
                size_t indexA =
                    (first + i) * audio.channels + ch;
                size_t indexB =
                    (second + i) * audio.channels + ch;

                double a = audio.samples[indexA];
                double b = audio.samples[indexB];

                dot[ch] += a * b;
                energyA[ch] += a * a;
                energyB[ch] += b * b;
            }
        }

        double gains[2] = {};
        unsigned int validChannels = 0;

        for (unsigned int ch = 0; ch < audio.channels; ++ch)
        {
            if (energyA[ch] < 1e-12 ||
                energyB[ch] < 1e-12)
                return false;

            double correlation =
                std::abs(dot[ch]) /
                std::sqrt(energyA[ch] * energyB[ch]);

            if (correlation < settings.minCorrelation)
                return false;

            gains[ch] = dot[ch] / energyA[ch];

            double absoluteGain = std::abs(gains[ch]);

            if (absoluteGain < settings.minGain ||
                absoluteGain > settings.maxGain)
                return false;

            ++validChannels;
        }

        if (audio.channels == 1)
        {
            gainLeft = gains[0];
            gainRight = gains[0];
        }
        else
        {
            gainLeft = gains[0];
            gainRight = gains[1];
        }

        return validChannels > 0;
    }

    bool rangesOverlap(
        size_t a,
        size_t b,
        size_t length)
    {
        return a < b + length && b < a + length;
    }
}

bool separateRepeatedComponents(
    const AudioData& input,
    AudioData& repeated,
    AudioData& residual,
    const RepetitionSettings& settings,
    RepetitionResult& result,
    std::string& error)
{
    result = {};

    if (input.channels == 0 ||
        input.channels > 2 ||
        input.sampleRate == 0 ||
        input.frameCount == 0)
    {
        error = "Invalid audio format.";
        return false;
    }

    if (input.samples.size() !=
        input.frameCount * input.channels)
    {
        error = "Audio sample count does not match its format.";
        return false;
    }

    if (settings.minFragmentMs == 0 ||
        settings.searchHopMs == 0 ||
        settings.minCorrelation <= 0.0 ||
        settings.minCorrelation > 1.0 ||
        settings.minGain <= 0.0 ||
        settings.maxGain < settings.minGain ||
        settings.minOccurrences < 2)
    {
        error = "Invalid repetition settings.";
        return false;
    }

    size_t windowFrames =
        static_cast<size_t>(
            static_cast<uint64_t>(input.sampleRate) *
            settings.minFragmentMs / 1000
            );

    size_t hopFrames =
        static_cast<size_t>(
            static_cast<uint64_t>(input.sampleRate) *
            settings.searchHopMs / 1000
            );

    if (windowFrames < 16 || hopFrames == 0 ||
        windowFrames >= input.frameCount)
    {
        error = "Audio is too short for the selected fragment length.";
        return false;
    }

    repeated = input;
    residual = input;

    std::fill(
        repeated.samples.begin(),
        repeated.samples.end(),
        0.0f
    );

    std::unordered_map<uint32_t, std::vector<size_t>> buckets;

    // Этап 1. Быстрый поиск потенциально совпадающих фрагментов.
    for (size_t frame = 0;
        frame + windowFrames <= input.frameCount;
        frame += hopFrames)
    {
        uint32_t signature =
            makeSignature(input, frame, windowFrames);

        buckets[signature].push_back(frame);
    }

    std::vector<bool> occupied(
        static_cast<size_t>(input.frameCount),
        false
    );

    // Этап 2. Проверка совпадений и разделение сигналов.
    for (const auto& entry : buckets)
    {
        const std::vector<size_t>& positions = entry.second;

        for (size_t i = 0; i < positions.size(); ++i)
        {
            for (size_t j = i + 1; j < positions.size(); ++j)
            {
                size_t first = positions[i];
                size_t second = positions[j];

                if (second - first < windowFrames)
                    continue;

                bool overlap = false;

                for (size_t k = 0; k < windowFrames; ++k)
                {
                    if (occupied[first + k] ||
                        occupied[second + k])
                    {
                        overlap = true;
                        break;
                    }
                }

                if (overlap)
                    continue;

                double gainLeft = 1.0;
                double gainRight = 1.0;

                if (!compareFragments(
                    input,
                    first,
                    second,
                    windowFrames,
                    settings,
                    gainLeft,
                    gainRight))
                {
                    continue;
                }

                // Формируем общую оценку повторяющегося сигнала.
                // Первый фрагмент служит исходной опорой,
                // второй учитывается с компенсацией усиления.
                for (size_t k = 0; k < windowFrames; ++k)
                {
                    for (unsigned int ch = 0;
                        ch < input.channels;
                        ++ch)
                    {
                        size_t indexA =
                            (first + k) * input.channels + ch;
                        size_t indexB =
                            (second + k) * input.channels + ch;

                        double gain =
                            (ch == 0) ? gainLeft : gainRight;

                        double a = input.samples[indexA];
                        double b = input.samples[indexB];

                        double common =
                            0.5 * (a + b / gain);

                        double repeatedA = common;
                        double repeatedB = gain * common;

                        repeated.samples[indexA] =
                            static_cast<float>(repeatedA);

                        repeated.samples[indexB] =
                            static_cast<float>(repeatedB);

                        residual.samples[indexA] =
                            input.samples[indexA] -
                            repeated.samples[indexA];

                        residual.samples[indexB] =
                            input.samples[indexB] -
                            repeated.samples[indexB];
                    }

                    occupied[first + k] = true;
                    occupied[second + k] = true;
                }

                ++result.groups;
                result.occurrences += 2;
            }
        }
    }

    error.clear();
    return true;
}
