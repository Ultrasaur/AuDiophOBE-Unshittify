#ifndef FFT_H
#define FFT_H

#include <vector>
#include <string>
#include <cstddef>

#include "audio.h"

struct Spectrum
{
    std::vector<double> frequencies;
    std::vector<double> powerDb;
};

void fft(std::vector<double>& real,
    std::vector<double>& imag);

void ifft(
    std::vector<double>& real,
    std::vector<double>& imag
);

// Медианный фильтр (окно приводится к нечётному, на краях сужается)
std::vector<double> medianFilter(
    const std::vector<double>& values,
    std::size_t windowSize
);

// Усреднённый спектр на логарифмической сетке частот
bool calculateAverageSpectrum(
    const AudioData& audio,
    Spectrum& spectrum,
    std::string& error,
    std::size_t fftSize = 8192,
    std::size_t hopSize = 2048,
    std::size_t pointsPerOctave = 24,
    std::size_t medianWindow = 5
);

#endif