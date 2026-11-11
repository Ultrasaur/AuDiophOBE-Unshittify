#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace BetterDeclipper
{
    // First porting layer: signal-level evidence used by BetterDeclipper's
    // detector. This module is diagnostic only; it does not modify audio.
    enum class CeilingKind
    {
        None,
        HardClip,
        Limiter,
        Smeared
    };

    struct PolarityAnalysis
    {
        int polarity = 1;
        CeilingKind kind = CeilingKind::None;
        std::optional<double> plateauLevel;
        std::optional<double> softKnee;
        double robustPeak = 0.0;
        double longRunFraction = 0.0;
        double flatStepFraction = 0.0;
        std::uint64_t touchEvents = 0;
        std::size_t samplesAtPlateau = 0;
    };

    struct ChannelAnalysis
    {
        PolarityAnalysis positive;
        PolarityAnalysis negative;
    };

    struct DetectionResult
    {
        double estimatedLsb = 1e-7;
        std::vector<ChannelAnalysis> channels;
    };

    // samples are interleaved, normalized floating-point PCM.
    DetectionResult analyze(
        const std::vector<float>& samples,
        std::size_t frameCount,
        std::size_t channelCount,
        unsigned int sampleRate);

    double estimateLsb(const std::vector<float>& interleavedSamples);
}
