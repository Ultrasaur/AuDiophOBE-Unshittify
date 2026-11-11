#include "better_declipper.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace BetterDeclipper
{
    namespace
    {
        struct PlateauResult
        {
            std::optional<double> level;
            std::size_t count = 0;
            double robustPeak = 0.0;
        };

        double median(std::vector<double> values)
        {
            if (values.empty())
                return 0.0;

            const std::size_t middle = values.size() / 2;
            std::nth_element(values.begin(), values.begin() + middle, values.end());
            const double upper = values[middle];

            if (values.size() % 2 != 0)
                return upper;

            std::nth_element(values.begin(), values.begin() + middle - 1, values.end());
            return 0.5 * (values[middle - 1] + upper);
        }

        double quantile(std::vector<double>& values, double q)
        {
            if (values.empty())
                return 0.0;

            const std::size_t index = static_cast<std::size_t>(
                std::floor((values.size() - 1) * q));
            std::nth_element(values.begin(), values.begin() + index, values.end());
            return values[index];
        }

        PlateauResult findPlateau(
            const std::vector<double>& amplitudes,
            double lsb,
            std::size_t minRun = 4,
            double densityRatio = 8.0,
            double searchFraction = 0.15,
            double peakDiscard = 1e-4)
        {
            std::vector<double> positive;
            positive.reserve(amplitudes.size());

            for (double value : amplitudes)
                if (value > 0.0 && std::isfinite(value))
                    positive.push_back(value);

            PlateauResult result;
            if (positive.size() < 100)
                return result;

            const std::size_t discard = static_cast<std::size_t>(
                std::floor(positive.size() * peakDiscard));
            const std::size_t peakIndex = positive.size() - 1 - discard;
            const double top = quantile(positive,
                static_cast<double>(peakIndex) / (positive.size() - 1));
            const double peak = *std::max_element(positive.begin(), positive.end());
            result.robustPeak = top;

            const double binWidth = std::max(lsb, top * 1e-5);
            const double lower = top * (1.0 - searchFraction);
            const std::size_t binCount = static_cast<std::size_t>(
                std::ceil((peak - lower) / binWidth)) + 1;
            if (binCount < 2 || binCount > 10000000)
                return result;

            std::vector<double> histogram(binCount, 0.0);
            for (double value : positive)
            {
                if (value < lower)
                    continue;

                std::size_t index = static_cast<std::size_t>(
                    (value - lower) / binWidth);
                index = std::min(index, binCount - 1);
                histogram[index] += 1.0;
            }

            std::vector<double> backgroundBins(
                histogram.begin(),
                histogram.begin() + binCount / 2);
            const double background = std::max(
                median(backgroundBins), 1.0);

            std::vector<unsigned char> dense(binCount, 0);
            for (std::size_t i = 0; i < binCount; ++i)
                dense[i] = histogram[i] > densityRatio * background;

            long long i = std::min(
                static_cast<long long>((top - lower) / binWidth),
                static_cast<long long>(binCount - 1));

            while (i >= 0 && !dense[static_cast<std::size_t>(i)])
            {
                --i;
                if ((static_cast<long long>(binCount - 1) - i) * binWidth >
                    0.02 * top)
                    return result;
            }

            if (i < 0)
                return result;

            long long j = i;
            int gap = 0;
            while (j - 1 >= 0 &&
                (dense[static_cast<std::size_t>(j - 1)] ||
                    (gap < 1 && j - 2 >= 0 &&
                        dense[static_cast<std::size_t>(j - 2)])))
            {
                if (!dense[static_cast<std::size_t>(j - 1)])
                    ++gap;
                --j;
            }

            const double level = lower + j * binWidth;
            const std::size_t count = static_cast<std::size_t>(
                std::count_if(positive.begin(), positive.end(),
                    [level](double value) { return value >= level; }));

            if (count < minRun)
                return result;

            result.level = level;
            result.count = count;
            return result;
        }

        std::optional<double> findSoftKnee(
            const std::vector<double>& amplitudes,
            double peakDiscard = 1e-4)
        {
            std::vector<double> positive;
            positive.reserve(amplitudes.size());
            for (double value : amplitudes)
                if (value > 0.0 && std::isfinite(value))
                    positive.push_back(value);

            constexpr std::size_t bins = 200;
            constexpr double fitLow = 0.25;
            constexpr double fitHigh = 0.5;
            constexpr double excess = 1.6;
            constexpr std::size_t requiredRun = 6;

            if (positive.size() < 1000)
                return std::nullopt;

            const std::size_t discard = static_cast<std::size_t>(
                std::floor(positive.size() * peakDiscard));
            const std::size_t peakIndex = positive.size() - 1 - discard;
            const double peak = quantile(
                positive,
                static_cast<double>(peakIndex) / (positive.size() - 1));

            if (!(peak > 0.0))
                return std::nullopt;

            std::vector<double> histogram(bins, 0.0);
            for (double value : positive)
            {
                const double normalized = std::min(value / peak, 1.0);
                if (normalized < 0.0)
                    continue;
                const std::size_t index = std::min(
                    static_cast<std::size_t>(normalized * bins), bins - 1);
                histogram[index] += 1.0;
            }

            double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
            std::size_t selected = 0;
            for (std::size_t i = 0; i < bins; ++i)
            {
                const double center = (i + 0.5) / bins;
                if (center < fitLow || center > fitHigh || histogram[i] <= 0.0)
                    continue;

                const double x = center;
                const double y = std::log(histogram[i]);
                sx += x; sy += y; sxx += x * x; sxy += x * y;
                ++selected;
            }

            if (selected < 5)
                return std::nullopt;

            const double denominator = selected * sxx - sx * sx;
            if (std::abs(denominator) < 1e-12)
                return std::nullopt;

            double slope = (selected * sxy - sx * sy) / denominator;
            const double intercept = (sy - slope * sx) / selected;
            slope = std::min(slope, 0.0);

            std::vector<double> smoothed(bins, 0.0);
            for (std::size_t i = 0; i < bins; ++i)
            {
                for (int offset = -1; offset <= 1; ++offset)
                {
                    const long long j = static_cast<long long>(i) + offset;
                    if (j >= 0 && j < static_cast<long long>(bins))
                        smoothed[i] += histogram[static_cast<std::size_t>(j)] / 3.0;
                }
            }

            std::vector<unsigned char> above(bins, 0);
            for (std::size_t i = 0; i < bins; ++i)
            {
                const double center = (i + 0.5) / bins;
                const double predicted = std::exp(intercept + slope * center);
                above[i] = smoothed[i] > excess * predicted;
            }

            const std::size_t start = static_cast<std::size_t>(
                std::ceil(fitHigh * bins - 0.5));
            for (std::size_t i = start; i + requiredRun <= bins; ++i)
            {
                bool consecutive = true;
                for (std::size_t j = 0; j < requiredRun; ++j)
                    consecutive = consecutive && above[i + j];

                if (consecutive)
                    return (static_cast<double>(i) / bins) * peak;
            }

            return std::nullopt;
        }

        std::pair<double, double> runStats(
            const std::vector<double>& signal,
            double level,
            double lsb,
            std::size_t minRun = 4)
        {
            std::vector<unsigned char> at(signal.size(), 0);
            std::size_t count = 0;
            for (std::size_t i = 0; i < signal.size(); ++i)
            {
                at[i] = signal[i] >= level;
                count += at[i] != 0;
            }

            if (count == 0)
                return { 0.0, 0.0 };

            std::size_t longSamples = 0;
            std::size_t runLength = 0;
            for (std::size_t i = 0; i <= at.size(); ++i)
            {
                if (i < at.size() && at[i])
                {
                    ++runLength;
                }
                else
                {
                    if (runLength >= minRun)
                        longSamples += runLength;
                    runLength = 0;
                }
            }

            std::size_t flat = 0;
            std::size_t steps = 0;
            const double flatThreshold = std::max(3.0 * std::max(lsb, 1e-7),
                1e-4 * level);

            for (std::size_t i = 1; i < signal.size(); ++i)
            {
                if (at[i] && at[i - 1])
                {
                    ++steps;
                    if (std::abs(signal[i] - signal[i - 1]) <= flatThreshold)
                        ++flat;
                }
            }

            return {
                static_cast<double>(longSamples) / count,
                steps ? static_cast<double>(flat) / steps : 0.0
            };
        }

        std::uint64_t countEvents(
            const std::vector<double>& signal,
            double level)
        {
            std::uint64_t events = 0;
            bool previous = false;
            for (double value : signal)
            {
                const bool current = value >= level;
                if (current && !previous)
                    ++events;
                previous = current;
            }
            return events;
        }

        PolarityAnalysis analyzePolarity(
            const std::vector<double>& signal,
            int polarity,
            double lsb,
            unsigned int sampleRate)
        {
            PolarityAnalysis result;
            result.polarity = polarity;

            const PlateauResult plateau = findPlateau(signal, lsb);
            result.plateauLevel = plateau.level;
            result.samplesAtPlateau = plateau.count;
            result.robustPeak = plateau.robustPeak;
            result.softKnee = findSoftKnee(signal);

            if (!plateau.level)
                return result;

            // This is the sharp-ceiling branch of BetterDeclipper's auto
            // classifier (the fitted-sigma refinement is implemented next).
            const double threshold = *plateau.level -
                1.5 * std::max(lsb, 1e-7);
            const auto stats = runStats(signal, threshold, lsb);
            result.longRunFraction = stats.first;
            result.flatStepFraction = stats.second;
            result.touchEvents = countEvents(signal, threshold);

            const double duration = sampleRate > 0
                ? static_cast<double>(signal.size()) / sampleRate
                : 0.0;
            const std::uint64_t requiredEvents = static_cast<std::uint64_t>(
                std::max(20.0, 8.0 * duration));
            const bool fewEvents = result.touchEvents < requiredEvents;

            if (result.longRunFraction < 0.3)
                result.kind = fewEvents ? CeilingKind::None : CeilingKind::Limiter;
            else if (result.flatStepFraction >= 0.5)
                result.kind = CeilingKind::HardClip;
            else if (!fewEvents)
                result.kind = CeilingKind::Smeared;

            return result;
        }
    }

    double estimateLsb(const std::vector<float>& samples)
    {
        const std::size_t count = std::min<std::size_t>(samples.size(), 200000);
        std::vector<double> values;
        values.reserve(count);

        for (std::size_t i = 0; i < count; ++i)
            values.push_back(std::abs(static_cast<double>(samples[i])));

        for (int bits : { 8, 16, 24 })
        {
            const double scale = std::pow(2.0, bits - 1);
            bool quantized = true;

            for (double value : values)
            {
                if (std::abs(value * scale - std::round(value * scale)) >= 1e-6)
                {
                    quantized = false;
                    break;
                }
            }

            if (quantized)
                return 1.0 / scale;
        }

        return 1e-7;
    }

    DetectionResult analyze(
        const std::vector<float>& samples,
        std::size_t frameCount,
        std::size_t channelCount,
        unsigned int sampleRate)
    {
        DetectionResult result;
        result.estimatedLsb = estimateLsb(samples);

        if (channelCount == 0 ||
            frameCount == 0 ||
            samples.size() != frameCount * channelCount)
            return result;

        result.channels.resize(channelCount);

        for (std::size_t channel = 0; channel < channelCount; ++channel)
        {
            std::vector<double> positive;
            std::vector<double> negative;
            positive.reserve(frameCount);
            negative.reserve(frameCount);

            for (std::size_t frame = 0; frame < frameCount; ++frame)
            {
                const double sample = samples[frame * channelCount + channel];
                positive.push_back(sample);
                negative.push_back(-sample);
            }

            result.channels[channel].positive = analyzePolarity(
                positive, 1, result.estimatedLsb, sampleRate);
            result.channels[channel].negative = analyzePolarity(
                negative, -1, result.estimatedLsb, sampleRate);
        }

        return result;
    }
}
