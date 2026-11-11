
#include "fft.h"

#include <cmath>
#include <algorithm>
#include <limits>
#include <stdexcept>

using namespace std;

const double PI = 3.14159265358979323846;

// Быстрое преобразование Фурье
void fft(vector<double>& real,
    vector<double>& imag)
{
    size_t n = real.size();

    if (n != imag.size() ||
        n == 0 ||
        (n & (n - 1)) != 0)
    {
        throw invalid_argument("Invalid FFT size.");
    }

    size_t j = 0;

    for (size_t i = 1; i < n; i++)
    {
        size_t bit = n >> 1;

        while (j & bit)
        {
            j ^= bit;
            bit >>= 1;
        }

        j ^= bit;

        if (i < j)
        {
            swap(real[i], real[j]);
            swap(imag[i], imag[j]);
        }
    }

    for (size_t len = 2; len <= n; len <<= 1)
    {
        double angle = -2.0 * PI / len;

        double wLenReal = cos(angle);
        double wLenImag = sin(angle);

        for (size_t i = 0; i < n; i += len)
        {
            double wReal = 1.0;
            double wImag = 0.0;

            for (size_t j = 0; j < len / 2; j++)
            {
                size_t u = i + j;
                size_t v = i + j + len / 2;

                double evenReal = real[u];
                double evenImag = imag[u];

                double oddReal =
                    real[v] * wReal -
                    imag[v] * wImag;

                double oddImag =
                    real[v] * wImag +
                    imag[v] * wReal;

                real[u] = evenReal + oddReal;
                imag[u] = evenImag + oddImag;

                real[v] = evenReal - oddReal;
                imag[v] = evenImag - oddImag;

                double nextWReal =
                    wReal * wLenReal -
                    wImag * wLenImag;

                double nextWImag =
                    wReal * wLenImag +
                    wImag * wLenReal;

                wReal = nextWReal;
                wImag = nextWImag;
            }
        }
    }
}

// Обратное быстрое преобразование Фурье
void ifft(
    vector<double>& real,
    vector<double>& imag)
{
    size_t n = real.size();

    for (size_t i = 0; i < n; i++)
    {
        imag[i] = -imag[i];
    }

    fft(real, imag);

    for (size_t i = 0; i < n; i++)
    {
        real[i] /= n;
        imag[i] = -imag[i] / n;
    }
}

// Медианный фильтр
vector<double> medianFilter(
    const vector<double>& values,
    size_t windowSize)
{
    if (windowSize < 3 || values.size() < 3)
    {
        return values;
    }

    if (windowSize % 2 == 0)
    {
        windowSize++;
    }

    const long long half = static_cast<long long>(windowSize / 2);
    const long long count = static_cast<long long>(values.size());

    vector<double> result(values.size());
    vector<double> buffer;
    buffer.reserve(windowSize);

    for (long long i = 0; i < count; i++)
    {
        long long from = max(0LL, i - half);
        long long to = min(count - 1, i + half);

        buffer.assign(
            values.begin() + from,
            values.begin() + to + 1
        );

        size_t middle = buffer.size() / 2;

        nth_element(
            buffer.begin(),
            buffer.begin() + middle,
            buffer.end()
        );

        result[i] = buffer[middle];
    }

    return result;
}

// Расчёт усреднённого спектра на логарифмической шкале частот
bool calculateAverageSpectrum(
    const AudioData& audio,
    Spectrum& spectrum,
    string& error,
    size_t fftSize,
    size_t hopSize,
    size_t pointsPerOctave,
    size_t medianWindow)
{
    if (audio.channels == 0 ||
        audio.sampleRate == 0 ||
        audio.frameCount == 0)
    {
        error = "Invalid audio data.";
        return false;
    }

    if (audio.samples.size() !=
        audio.frameCount * audio.channels)
    {
        error = "Invalid audio data size.";
        return false;
    }

    if (fftSize < 2 ||
        (fftSize & (fftSize - 1)) != 0 ||
        hopSize == 0 ||
        pointsPerOctave == 0)
    {
        error = "Invalid FFT parameters.";
        return false;
    }

    if (audio.sampleRate < 40000)
    {
        error = "Sample rate is too low.";
        return false;
    }

    const size_t binCount = fftSize / 2 + 1;

    vector<double> averagePower(binCount, 0.0);
    vector<double> window(fftSize);

    double windowEnergy = 0.0;

    // Окно Ханна
    for (size_t i = 0; i < fftSize; i++)
    {
        window[i] = 0.5 * (
            1.0 - cos(2.0 * PI * i / (fftSize - 1))
            );

        windowEnergy += window[i] * window[i];
    }

    vector<double> real(fftSize);
    vector<double> imag(fftSize);

    size_t analyzedFrames = 0;

    // Обработка одного окна
    auto processFrame = [&](size_t start)
        {
            for (unsigned int channel = 0;
                channel < audio.channels;
                channel++)
            {
                fill(real.begin(), real.end(), 0.0);
                fill(imag.begin(), imag.end(), 0.0);

                for (size_t i = 0; i < fftSize; i++)
                {
                    size_t frame = start + i;

                    double sample = 0.0;

                    if (frame < audio.frameCount)
                    {
                        sample = audio.samples[
                            frame * audio.channels + channel
                        ];
                    }

                    real[i] = sample * window[i];
                }

                fft(real, imag);

                for (size_t k = 0; k < binCount; k++)
                {
                    averagePower[k] +=
                        real[k] * real[k] +
                        imag[k] * imag[k];
                }
            }

            analyzedFrames++;
        };

    // Обработка всей записи
    if (audio.frameCount < fftSize)
    {
        processFrame(0);
    }
    else
    {
        size_t start = 0;
        size_t lastProcessed = 0;

        while (start + fftSize <= audio.frameCount)
        {
            processFrame(start);
            lastProcessed = start;
            start += hopSize;
        }

        size_t lastStart = audio.frameCount - fftSize;

        if (lastProcessed != lastStart)
        {
            processFrame(lastStart);
        }
    }

    const double normalization =
        analyzedFrames *
        audio.channels *
        windowEnergy;

    // Мощность линейных бинов
    vector<double> binPower(binCount);

    for (size_t k = 0; k < binCount; k++)
    {
        binPower[k] = averagePower[k] / normalization;
    }

    // Переход на логарифмическую сетку частот
    const double MIN_FREQUENCY = 20.0;
    const double MAX_FREQUENCY = 20000.0;

    const double nyquist = audio.sampleRate / 2.0;
    const double binWidth =
        static_cast<double>(audio.sampleRate) / fftSize;

    const double halfBandFactor =
        pow(2.0, 0.5 / pointsPerOctave);

    spectrum.frequencies.clear();
    spectrum.powerDb.clear();

    for (size_t i = 0; ; i++)
    {
        double center = MIN_FREQUENCY *
            pow(2.0, static_cast<double>(i) / pointsPerOctave);

        if (center > MAX_FREQUENCY || center > nyquist)
        {
            break;
        }

        double lowEdge = center / halfBandFactor;
        double highEdge = center * halfBandFactor;

        size_t firstBin = static_cast<size_t>(
            ceil(lowEdge / binWidth));

        size_t lastBin = static_cast<size_t>(
            floor(highEdge / binWidth));

        lastBin = min(lastBin, binCount - 1);

        double power = 0.0;

        if (firstBin <= lastBin)
        {
            // Среднее по бинам, попавшим в полосу
            for (size_t k = firstBin; k <= lastBin; k++)
            {
                power += binPower[k];
            }

            power /= static_cast<double>(lastBin - firstBin + 1);
        }
        else
        {
            // В полосу не попал ни один бин: линейная интерполяция
            double position = center / binWidth;

            size_t left = static_cast<size_t>(floor(position));
            size_t right = min(left + 1, binCount - 1);

            double fraction = position - static_cast<double>(left);

            power = binPower[left] +
                fraction * (binPower[right] - binPower[left]);
        }

        spectrum.frequencies.push_back(center);
        spectrum.powerDb.push_back(
            10.0 * log10(
                max(power, numeric_limits<double>::min())
            )
        );
    }

    if (spectrum.frequencies.size() < 2)
    {
        error = "Unable to build logarithmic spectrum.";
        return false;
    }

    // Медианный фильтр для сглаживания результата
    spectrum.powerDb = medianFilter(
        spectrum.powerDb,
        medianWindow
    );

    return true;
}