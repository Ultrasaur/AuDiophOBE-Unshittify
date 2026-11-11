
#include "repetition.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <map>
#include <iterator>
#include <array>

namespace
{
    struct Match
    {
        size_t first;
        size_t second;
        size_t length;
        int offset;
        double gainLeft;
        double gainRight;
        double score;
    };

    struct CandidatePair
    {
        size_t first;
        size_t second;

        bool operator<(const CandidatePair& other) const
        {
            if (first != other.first)
                return first < other.first;

            return second < other.second;
        }

        bool operator==(const CandidatePair& other) const
        {
            return first == other.first &&
                second == other.second;
        }
    };

    // Формирует 16-битную частичную сигнатуру.
    // Одна сигнатура описывает только часть фрагмента.
    uint16_t makePartialSignature(
        const AudioData& audio,
        size_t firstFrame,
        size_t windowFrames,
        size_t part)
    {
        uint16_t key = 0;
        size_t bit = 0;

        size_t partStart = part * windowFrames / 4;
        size_t partEnd = (part + 1) * windowFrames / 4;
        size_t partLength = partEnd - partStart;

        if (audio.channels == 1)
        {
            for (size_t i = 0; i < 16; ++i)
            {
                size_t offset =
                    partStart + i * (partLength - 1) / 15;

                size_t index = firstFrame + offset;

                if (audio.samples[index] >= 0.0f)
                    key |= static_cast<uint16_t>(1u << bit);

                ++bit;
            }
        }
        else
        {
            for (unsigned int channel = 0; channel < 2; ++channel)
            {
                for (size_t i = 0; i < 8; ++i)
                {
                    size_t offset =
                        partStart + i * (partLength - 1) / 7;

                    size_t index =
                        (firstFrame + offset) * audio.channels + channel;

                    if (audio.samples[index] >= 0.0f)
                        key |= static_cast<uint16_t>(1u << bit);

                    ++bit;
                }
            }
        }

        uint16_t inverted = static_cast<uint16_t>(~key);

        return std::min(key, inverted);
    }

    bool coarseCompareFragments(
        const AudioData& audio,
        size_t first,
        size_t second,
        size_t windowFrames,
        double minCorrelation)
    {
        double dot[2] = {};
        double energyA[2] = {};
        double energyB[2] = {};

        constexpr size_t sampleCount = 32;

        for (size_t i = 0; i < sampleCount; ++i)
        {
            size_t offset =
                i * (windowFrames - 1) / (sampleCount - 1);

            for (unsigned int ch = 0; ch < audio.channels; ++ch)
            {
                double a = audio.samples[
                    (first + offset) * audio.channels + ch];

                double b = audio.samples[
                    (second + offset) * audio.channels + ch];

                dot[ch] += a * b;
                energyA[ch] += a * a;
                energyB[ch] += b * b;
            }
        }

        // Предварительная проверка намеренно мягче
        // окончательного критерия корреляции.
        double coarseThreshold = minCorrelation * 0.75;

        for (unsigned int ch = 0; ch < audio.channels; ++ch)
        {
            if (energyA[ch] < 1e-12 ||
                energyB[ch] < 1e-12)
            {
                continue;
            }

            double correlation =
                std::abs(dot[ch]) /
                std::sqrt(energyA[ch] * energyB[ch]);

            if (correlation < coarseThreshold)
                return false;
        }

        return true;
    }

    // Проверяет фрагменты с небольшим временным смещением.
    // Усиление левого и правого каналов определяется отдельно.
    bool compareFragments(
        const AudioData& audio,
        size_t first,
        size_t second,
        size_t windowFrames,
        size_t maxShift,
        const RepetitionSettings& settings,
        int& bestOffset,
        double& gainLeft,
        double& gainRight,
        double& bestScore)
    {
        const size_t shiftStep =
            std::max<size_t>(1, audio.sampleRate / 8000);

        bool found = false;
        bestScore = -1.0;

        for (int offset = -static_cast<int>(maxShift);
            offset <= static_cast<int>(maxShift);
            offset += static_cast<int>(shiftStep))
        {
            const long long alignedSecond =
                static_cast<long long>(second) + offset;

            if (alignedSecond < 0)
                continue;

            if (static_cast<uint64_t>(alignedSecond) +
                windowFrames > audio.frameCount)
                continue;

            if (!coarseCompareFragments(
                audio,
                first,
                static_cast<size_t>(alignedSecond),
                windowFrames,
                settings.minCorrelation))
            {
                continue;
            }

            double dot[2] = {};
            double energyA[2] = {};
            double energyB[2] = {};

            for (size_t i = 0; i < windowFrames; ++i)
            {
                for (unsigned int ch = 0;
                    ch < audio.channels;
                    ++ch)
                {
                    size_t indexA =
                        (first + i) * audio.channels + ch;

                    size_t indexB =
                        (static_cast<size_t>(alignedSecond) + i)
                        * audio.channels + ch;

                    double a = audio.samples[indexA];
                    double b = audio.samples[indexB];

                    dot[ch] += a * b;
                    energyA[ch] += a * a;
                    energyB[ch] += b * b;
                }
            }

            double gains[2] = {};
            double score = 0.0;
            bool valid = true;

            for (unsigned int ch = 0;
                ch < audio.channels;
                ++ch)
            {
                if (energyA[ch] < 1e-12 ||
                    energyB[ch] < 1e-12)
                {
                    valid = false;
                    break;
                }

                double correlation =
                    std::abs(dot[ch]) /
                    std::sqrt(energyA[ch] * energyB[ch]);

                if (correlation < settings.minCorrelation)
                {
                    valid = false;
                    break;
                }

                gains[ch] = dot[ch] / energyA[ch];

                double absoluteGain = std::abs(gains[ch]);

                if (absoluteGain < settings.minGain ||
                    absoluteGain > settings.maxGain)
                {
                    valid = false;
                    break;
                }

                score += correlation;
            }

            if (!valid)
                continue;

            score /= audio.channels;

            if (score > bestScore)
            {
                found = true;
                bestScore = score;
                bestOffset = offset;

                gainLeft = gains[0];
                gainRight =
                    audio.channels == 1 ? gains[0] : gains[1];
            }
        }

        return found;
    }

    bool rangeOverlaps(
        const std::map<size_t, size_t>& ranges,
        size_t start,
        size_t length)
    {
        size_t end = start + length;

        auto next = ranges.lower_bound(start);

        if (next != ranges.end() && next->first < end)
            return true;

        if (next != ranges.begin())
        {
            auto previous = std::prev(next);

            if (previous->second > start)
                return true;
        }

        return false;
    }

    // Создаёт набор длительностей для многоуровневого поиска.
    std::vector<size_t> makeWindowSizes(
        unsigned int sampleRate,
        const RepetitionSettings& settings)
    {
        std::vector<size_t> result;

        size_t minimum =
            static_cast<size_t>(
                static_cast<uint64_t>(sampleRate) *
                settings.minFragmentMs / 1000
                );

        size_t maximum =
            static_cast<size_t>(
                static_cast<uint64_t>(sampleRate) *
                settings.maxFragmentMs / 1000
                );

        result.push_back(minimum);

        size_t middle = minimum + (maximum - minimum) / 2;

        if (middle != minimum && middle != maximum)
            result.push_back(middle);

        if (maximum != minimum)
            result.push_back(maximum);

        std::sort(result.begin(), result.end(), std::greater<size_t>());

        result.erase(
            std::unique(result.begin(), result.end()),
            result.end()
        );

        return result;
    }

    // Ищет пары потенциально совпадающих фрагментов.

    std::vector<CandidatePair> findCandidates(
        const AudioData& audio,
        size_t windowFrames,
        size_t hopFrames)
    {
        constexpr size_t maxBucketSize = 256;

        // Сохраняем сигнатуры всех фрагментов,
        // прошедших проверку энергии.
        struct FrameData
        {
            size_t frame;
            std::array<uint16_t, 4> signatures;
        };

        std::vector<FrameData> frameData;

        std::unordered_map<uint16_t, std::vector<size_t>> buckets;

        // Этап 1. Формирование частичных сигнатур.
        for (size_t frame = 0;
            frame + windowFrames <= audio.frameCount;
            frame += hopFrames)
        {
            double energy = 0.0;

            const size_t sampleCount =
                std::min<size_t>(windowFrames, 128);

            for (size_t i = 0; i < sampleCount; ++i)
            {
                size_t offset =
                    i * (windowFrames - 1) /
                    std::max<size_t>(1, sampleCount - 1);

                for (unsigned int ch = 0;
                    ch < audio.channels;
                    ++ch)
                {
                    double value = audio.samples[
                        (frame + offset) * audio.channels + ch];

                    energy += value * value;
                }
            }

            if (energy < 1e-10)
                continue;

            FrameData data = {};
            data.frame = frame;

            for (size_t part = 0; part < 4; ++part)
            {
                data.signatures[part] =
                    makePartialSignature(
                        audio,
                        frame,
                        windowFrames,
                        part
                    );

                buckets[data.signatures[part]].push_back(frame);
            }

            frameData.push_back(data);
        }

        std::vector<CandidatePair> candidates;

        // Этап 2. Обычный поиск в небольших группах.
        // Здесь сохраняется прежняя логика.
        std::unordered_set<size_t> largeBucketPositions;

        for (const auto& entry : buckets)
        {
            const std::vector<size_t>& positions = entry.second;

            if (positions.size() > maxBucketSize)
            {
                for (size_t position : positions)
                    largeBucketPositions.insert(position);

                continue;
            }

            for (size_t i = 0; i < positions.size(); ++i)
            {
                for (size_t j = i + 1; j < positions.size(); ++j)
                {
                    size_t first = positions[i];
                    size_t second = positions[j];

                    if (second - first < windowFrames)
                        continue;

                    candidates.push_back({ first, second });
                }
            }
        }

        // Этап 3. Обработка больших групп.
        // Вместо одной частичной сигнатуры используем
        // комбинации двух из четырёх сигнатур.

        if (!largeBucketPositions.empty())
        {
            const size_t partPairs[6][2] =
            {
                { 0, 1 },
                { 0, 2 },
                { 0, 3 },
                { 1, 2 },
                { 1, 3 },
                { 2, 3 }
            };

            for (size_t pairIndex = 0;
                pairIndex < 6;
                ++pairIndex)
            {
                size_t partA = partPairs[pairIndex][0];
                size_t partB = partPairs[pairIndex][1];

                std::unordered_map<
                    uint32_t,
                    std::vector<size_t>
                > compositeBuckets;

                // Строим составные сигнатуры для всех фрагментов.
                // Это позволяет находить пары даже тогда,
                // когда только один из фрагментов попал
                // в большую группу исходных сигнатур.
                for (const FrameData& data : frameData)
                {
                    uint32_t key =
                        (static_cast<uint32_t>(
                            data.signatures[partA]) << 16) |
                        data.signatures[partB];

                    compositeBuckets[key].push_back(data.frame);
                }

                // Ищем совпадения составных сигнатур.
                for (const auto& entry : compositeBuckets)
                {
                    const std::vector<size_t>& positions =
                        entry.second;

                    if (positions.size() < 2)
                        continue;

                    for (size_t i = 0; i < positions.size(); ++i)
                    {
                        for (size_t j = i + 1;
                            j < positions.size();
                            ++j)
                        {
                            size_t first = positions[i];
                            size_t second = positions[j];

                            if (second - first < windowFrames)
                                continue;

                            // Хотя бы одна позиция должна
                            // относиться к большой исходной группе.
                            if (largeBucketPositions.count(first) == 0 &&
                                largeBucketPositions.count(second) == 0)
                            {
                                continue;
                            }

                            candidates.push_back({ first, second });
                        }
                    }
                }
            }
        }

        // Этап 4. Удаляем дубликаты пар.
        std::sort(candidates.begin(), candidates.end());

        candidates.erase(
            std::unique(candidates.begin(), candidates.end()),
            candidates.end()
        );

        return candidates;
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
        settings.maxFragmentMs < settings.minFragmentMs ||
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

    size_t hopFrames =
        static_cast<size_t>(
            static_cast<uint64_t>(input.sampleRate) *
            settings.searchHopMs / 1000
            );

    if (hopFrames == 0)
    {
        error = "Search hop is too small.";
        return false;
    }

    std::vector<size_t> windowSizes =
        makeWindowSizes(input.sampleRate, settings);

    if (windowSizes.empty() ||
        windowSizes.front() >= input.frameCount)
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

    std::map<size_t, size_t> occupiedRanges;

    std::vector<Match> matches;

    // Этап 1. Поиск кандидатов на нескольких длительностях.
    for (size_t windowFrames : windowSizes)
    {
        if (windowFrames < 16 ||
            windowFrames >= input.frameCount)
            continue;

        std::vector<CandidatePair> candidates =
            findCandidates(input, windowFrames, hopFrames);

        // Максимальное смещение ограничено половиной шага поиска.
        const size_t maxShift =
            std::max<size_t>(1, hopFrames / 2);

        // Этап 2. Подробное сравнение с временным выравниванием.
        for (const CandidatePair& candidate : candidates)
        {
            size_t first = candidate.first;
            size_t second = candidate.second;

            int offset = 0;
            double gainLeft = 1.0;
            double gainRight = 1.0;
            double score = 0.0;

            if (!compareFragments(
                input,
                first,
                second,
                windowFrames,
                maxShift,
                settings,
                offset,
                gainLeft,
                gainRight,
                score))
            {
                continue;
            }

            size_t alignedSecond =
                static_cast<size_t>(
                    static_cast<long long>(second) + offset
                    );

            if (rangeOverlaps(occupiedRanges, first, windowFrames) ||
                rangeOverlaps(occupiedRanges, alignedSecond, windowFrames))
            {
                continue;
            }

            Match match = {};
            match.first = first;
            match.second = alignedSecond;
            match.length = windowFrames;
            match.offset = offset;
            match.gainLeft = gainLeft;
            match.gainRight = gainRight;
            match.score = score;

            matches.push_back(match);

            occupiedRanges.emplace(first, first + windowFrames);
            occupiedRanges.emplace(alignedSecond, alignedSecond + windowFrames);
        }
    }

    // Этап 3. Формирование повторяющейся составляющей.
    for (const Match& match : matches)
    {
        for (size_t i = 0; i < match.length; ++i)
        {
            for (unsigned int ch = 0;
                ch < input.channels;
                ++ch)
            {
                size_t indexA =
                    (match.first + i) * input.channels + ch;

                size_t indexB =
                    (match.second + i) * input.channels + ch;

                double gain =
                    ch == 0 ? match.gainLeft : match.gainRight;

                double a = input.samples[indexA];
                double b = input.samples[indexB];

                double common =
                    0.5 * (a + b / gain);

                repeated.samples[indexA] =
                    static_cast<float>(common);

                repeated.samples[indexB] =
                    static_cast<float>(gain * common);
            }
        }

        ++result.groups;
        result.occurrences += 2;
    }

    // Этап 4. Остаток рассчитывается вычитанием.
    // Поэтому сумма выходов восстанавливает исходный сигнал.
    for (size_t i = 0; i < input.samples.size(); ++i)
    {
        residual.samples[i] =
            input.samples[i] - repeated.samples[i];
    }

    error.clear();
    return true;
}
