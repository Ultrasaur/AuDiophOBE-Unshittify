
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
#include <iostream>
#include <chrono>

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
        double& bestScore,
        bool& coarsePassed,
        bool& fullPassed)
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

            coarsePassed = true;

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

            fullPassed = true;
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
        size_t hopFrames,
        size_t& eligibleFrames)
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

        eligibleFrames = frameData.size();

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
    const auto totalStart = std::chrono::steady_clock::now();

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


    // Временная диагностика поиска.
    size_t totalEligibleFrames = 0;
    size_t totalCandidates = 0;
    size_t totalCoarsePasses = 0;
    size_t totalFullPasses = 0;
    std::chrono::duration<double> candidateGenerationTime{ 0.0 };
    std::chrono::duration<double> comparisonTime{ 0.0 };
    std::chrono::duration<double> groupingTime{ 0.0 };
    std::chrono::duration<double> residualTime{ 0.0 };

    // Этап 1. Собираем все подтверждённые совпадения.
    // Пока ничего не помечаем занятым.
    std::map<size_t, std::vector<Match>> matchesByLength;

    for (size_t windowFrames : windowSizes)
    {
        if (windowFrames < 16 ||
            windowFrames >= input.frameCount)
            continue;

        size_t eligibleFrames = 0;
        const auto candidateStart = std::chrono::steady_clock::now();
        std::vector<CandidatePair> candidates =
            findCandidates(input, windowFrames, hopFrames, eligibleFrames);
        const auto candidateEnd = std::chrono::steady_clock::now();
        candidateGenerationTime += candidateEnd - candidateStart;

        totalEligibleFrames += eligibleFrames;
        totalCandidates += candidates.size();

        const size_t maxShift =
            std::max<size_t>(1, hopFrames / 2);

        const auto comparisonStart = std::chrono::steady_clock::now();

        for (const CandidatePair& candidate : candidates)
        {
            int offset = 0;
            double gainLeft = 1.0;
            double gainRight = 1.0;
            double score = 0.0;
            bool coarsePassed = false;
            bool fullPassed = false;

            if (!compareFragments(
                input,
                candidate.first,
                candidate.second,
                windowFrames,
                maxShift,
                settings,
                offset,
                gainLeft,
                gainRight,
                score,
                coarsePassed,
                fullPassed))
            {
                totalCoarsePasses += coarsePassed ? 1 : 0;
                totalFullPasses += fullPassed ? 1 : 0;
                continue;
            }

            totalCoarsePasses += coarsePassed ? 1 : 0;
            totalFullPasses += fullPassed ? 1 : 0;

            long long aligned =
                static_cast<long long>(candidate.second) + offset;

            if (aligned < 0 ||
                static_cast<uint64_t>(aligned) + windowFrames >
                input.frameCount)
                continue;

            Match match = {};
            match.first = candidate.first;
            match.second = static_cast<size_t>(aligned);
            match.length = windowFrames;
            match.offset = offset;
            match.gainLeft = gainLeft;
            match.gainRight = gainRight;
            match.score = score;

            matchesByLength[windowFrames].push_back(match);
        }

        const auto comparisonEnd = std::chrono::steady_clock::now();
        comparisonTime += comparisonEnd - comparisonStart;
    }

    // Этап 2. Объединяем пары в группы связанных вхождений.
    struct Link
    {
        size_t other;
        double gainLeft;
        double gainRight;
    };

    std::map<size_t, size_t> occupiedRanges;
    const auto groupingStart = std::chrono::steady_clock::now();

    for (const auto& lengthEntry : matchesByLength)
    {
        size_t windowFrames = lengthEntry.first;
        const std::vector<Match>& matches = lengthEntry.second;

        // Каждой уникальной позиции соответствует один узел.
        std::map<size_t, size_t> nodeIndex;
        std::vector<size_t> positions;

        for (const Match& match : matches)
        {
            nodeIndex.emplace(match.first, 0);
            nodeIndex.emplace(match.second, 0);
        }

        for (auto& entry : nodeIndex)
        {
            entry.second = positions.size();
            positions.push_back(entry.first);
        }

        std::vector<std::vector<Link>> graph(positions.size());

        // Ребро хранит отношение амплитуд второго вхождения
        // к первому. Для обратного направления используется
        // обратное отношение.
        for (const Match& match : matches)
        {
            size_t a = nodeIndex.at(match.first);
            size_t b = nodeIndex.at(match.second);

            if (std::abs(match.gainLeft) < 1e-12 ||
                std::abs(match.gainRight) < 1e-12)
                continue;

            graph[a].push_back({
                b,
                match.gainLeft,
                match.gainRight
                });

            graph[b].push_back({
                a,
                1.0 / match.gainLeft,
                1.0 / match.gainRight
                });
        }

        std::vector<bool> visited(positions.size(), false);

        // Буферы коэффициентов переиспользуются между компонентами графа.
        std::vector<double> relativeGainLeft(positions.size(), 1.0);
        std::vector<double> relativeGainRight(positions.size(), 1.0);

        // Этап 3. Обрабатываем каждую связанную группу.
        for (size_t root = 0; root < positions.size(); ++root)
        {
            if (visited[root])
                continue;

            std::vector<size_t> queue;
            std::vector<size_t> group;

            relativeGainLeft[root] = 1.0;
            relativeGainRight[root] = 1.0;

            visited[root] = true;
            queue.push_back(root);

            for (size_t q = 0; q < queue.size(); ++q)
            {
                size_t current = queue[q];
                group.push_back(current);

                for (const Link& link : graph[current])
                {
                    size_t next = link.other;

                    if (visited[next])
                        continue;

                    relativeGainLeft[next] =
                        relativeGainLeft[current] * link.gainLeft;

                    relativeGainRight[next] =
                        relativeGainRight[current] * link.gainRight;

                    if (!std::isfinite(relativeGainLeft[next]) ||
                        !std::isfinite(relativeGainRight[next]) ||
                        std::abs(relativeGainLeft[next]) < 1e-12 ||
                        std::abs(relativeGainRight[next]) < 1e-12)
                    {
                        continue;
                    }

                    visited[next] = true;
                    queue.push_back(next);
                }
            }

            // Оставляем только группы с нужным количеством вхождений.
            if (group.size() < settings.minOccurrences)
                continue;

            // BFS не гарантирует временной порядок обхода узлов.
            // Сортируем позиции до проверки перекрытий.
            std::sort(group.begin(), group.end(),
                [&](size_t a, size_t b)
                {
                    return positions[a] < positions[b];
                });

            // Проверяем, что вхождения внутри группы не перекрываются.
            bool validGroup = true;

            for (size_t i = 1; i < group.size(); ++i)
            {
                size_t previous = positions[group[i - 1]];
                size_t current = positions[group[i]];

                if (current - previous < windowFrames)
                {
                    validGroup = false;
                    break;
                }
            }

            if (!validGroup)
                continue;

            // Не допускаем перекрытия с уже обработанными группами.
            for (size_t node : group)
            {
                if (rangeOverlaps(
                    occupiedRanges,
                    positions[node],
                    windowFrames))
                {
                    validGroup = false;
                    break;
                }
            }

            if (!validGroup)
                continue;

            // Вычисляем общий шаблон повторяющегося звука.
            // Каждый экземпляр предварительно приводится
            // к амплитуде корневого вхождения.
            for (size_t i = 0; i < windowFrames; ++i)
            {
                for (unsigned int ch = 0;
                    ch < input.channels;
                    ++ch)
                {
                    double sum = 0.0;

                    for (size_t node : group)
                    {
                        size_t index =
                            (positions[node] + i) * input.channels + ch;

                        double gain = ch == 0
                            ? relativeGainLeft[node]
                            : relativeGainRight[node];

                        sum += input.samples[index] / gain;
                    }

                    double common =
                        sum / static_cast<double>(group.size());

                    // Восстанавливаем каждый экземпляр
                    // с его исходным относительным усилением.
                    for (size_t node : group)
                    {
                        size_t index =
                            (positions[node] + i) * input.channels + ch;

                        double gain = ch == 0
                            ? relativeGainLeft[node]
                            : relativeGainRight[node];

                        repeated.samples[index] =
                            static_cast<float>(common * gain);
                    }
                }
            }

            // Помечаем всю группу после её обработки.
            for (size_t node : group)
            {
                size_t start = positions[node];

                occupiedRanges.emplace(
                    start,
                    start + windowFrames
                );
            }

            ++result.groups;
            result.occurrences += group.size();
        }
    }

    const auto groupingEnd = std::chrono::steady_clock::now();
    groupingTime = groupingEnd - groupingStart;

    std::cerr
        << "\n[Repetition diagnostics]\n"
        << "Eligible frames: " << totalEligibleFrames << "\n"
        << "Candidate pairs: " << totalCandidates << "\n"
        << "Candidate pairs passing coarse check: " << totalCoarsePasses << "\n"
        << "Candidate pairs passing full check: " << totalFullPasses << "\n"
        << "Accepted groups: " << result.groups << "\n"
        << "Accepted occurrences: " << result.occurrences << "\n";

    // Поэтому сумма выходов восстанавливает исходный сигнал.
    const auto residualStart = std::chrono::steady_clock::now();
    for (size_t i = 0; i < input.samples.size(); ++i)
    {
        residual.samples[i] =
            input.samples[i] - repeated.samples[i];
    }
    const auto residualEnd = std::chrono::steady_clock::now();
    residualTime = residualEnd - residualStart;

    const auto totalEnd = std::chrono::steady_clock::now();
    const std::chrono::duration<double> totalTime = totalEnd - totalStart;

    std::cerr
        << "\\n[Repetition timing]\\n"
        << "Candidate generation: " << candidateGenerationTime.count() << " s\\n"
        << "Fragment comparison: " << comparisonTime.count() << " s\\n"
        << "Grouping: " << groupingTime.count() << " s\\n"
        << "Residual creation: " << residualTime.count() << " s\\n"
        << "Total: " << totalTime.count() << " s\\n";

    error.clear();
    return true;
}
