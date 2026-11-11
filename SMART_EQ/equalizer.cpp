#include "equalizer.h"
#include "repetition.h"

#include <cmath>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <deque>

using namespace std;

const double SMOOTHING_SIGMA = 1.0 / 6.0;
const double SMOOTHING_RADIUS = 0.5;
const double REFERENCE_FREQUENCY = 1000.0;

// Сглаживание спектра в логарифмической шкале частот
vector<double> smoothSpectrum(
    const vector<double>& frequencies,
    const vector<double>& values)
{
    vector<double> smoothed(values.size(), 0.0);

    for (size_t i = 0; i < values.size(); i++)
    {
        double weightedSum = 0.0;
        double weightSum = 0.0;

        double center = log2(frequencies[i]);

        for (size_t j = 0; j < values.size(); j++)
        {
            double distance =
                log2(frequencies[j]) - center;

            if (abs(distance) > SMOOTHING_RADIUS)
            {
                continue;
            }

            double weight = exp(
                -0.5 * distance * distance /
                (SMOOTHING_SIGMA * SMOOTHING_SIGMA)
            );

            weightedSum += values[j] * weight;
            weightSum += weight;
        }

        if (weightSum > 0.0)
        {
            smoothed[i] = weightedSum / weightSum;
        }
        else
        {
            smoothed[i] = values[i];
        }
    }

    return smoothed;
}

// Интерполяция значения на заданной частоте
double interpolateValue(
    const vector<double>& frequencies,
    const vector<double>& values,
    double targetFrequency)
{
    auto it = lower_bound(
        frequencies.begin(),
        frequencies.end(),
        targetFrequency
    );

    if (it == frequencies.begin())
    {
        return values.front();
    }

    if (it == frequencies.end())
    {
        return values.back();
    }

    size_t rightIndex = it - frequencies.begin();
    size_t leftIndex = rightIndex - 1;

    double leftFrequency = frequencies[leftIndex];
    double rightFrequency = frequencies[rightIndex];

    double position =
        log(targetFrequency / leftFrequency) /
        log(rightFrequency / leftFrequency);

    return values[leftIndex] +
        position * (values[rightIndex] - values[leftIndex]);
}

// Плавное нелинейное ограничение коррекции
double compressCorrection(
    double correction,
    double softLimit,
    double hardLimit)
{
    double absoluteCorrection = abs(correction);

    if (absoluteCorrection <= softLimit)
    {
        return correction;
    }

    // Границы совпадают: обычное жёсткое ограничение
    if (hardLimit <= softLimit)
    {
        return copysign(hardLimit, correction);
    }

    double range = hardLimit - softLimit;

    double compressed =
        softLimit +
        range * tanh((absoluteCorrection - softLimit) / range);

    return copysign(compressed, correction);
}

// Расчёт кривой коррекции эквалайзера
bool calculateEqualizerCurve(
    const Spectrum& spectrum,
    NoiseProfile profile,
    const EqualizerSettings& settings,
    vector<double>& correctionDb,
    vector<double>& requestedCorrectionDb,
    string& error)
{
    if (!(settings.intensity > 0.0 && settings.intensity <= 1.0) ||
        !(settings.softLimitDb >= 0.0) ||
        !(settings.hardLimitDb >= settings.softLimitDb))
    {
        error = "Invalid equalizer settings.";
        return false;
    }

    if (spectrum.frequencies.front() > REFERENCE_FREQUENCY ||
        spectrum.frequencies.back() < REFERENCE_FREQUENCY)
    {
        error = "Spectrum does not contain the reference frequency.";
        return false;
    }

    // Сглаживание исходного спектра
    vector<double> sourceSpectrum = smoothSpectrum(
        spectrum.frequencies,
        spectrum.powerDb
    );

    // Нормализация относительно 1000 Гц
    double referenceLevel = interpolateValue(
        spectrum.frequencies,
        sourceSpectrum,
        REFERENCE_FREQUENCY
    );

    vector<double> rawCorrection(
        spectrum.frequencies.size()
    );

    // Расчёт разницы между целевым и исходным спектром
    for (size_t i = 0;
        i < spectrum.frequencies.size();
        i++)
    {
        double sourceLevel =
            sourceSpectrum[i] - referenceLevel;

        double targetLevel = getTargetPowerDb(
            spectrum.frequencies[i],
            profile
        );

        rawCorrection[i] =
            settings.intensity *
            (targetLevel - sourceLevel);
    }

    // Сглаживание рассчитанной коррекции
    requestedCorrectionDb = smoothSpectrum(
        spectrum.frequencies,
        rawCorrection
    );

    // Нелинейное сжатие больших значений коррекции
    correctionDb = requestedCorrectionDb;

    for (size_t i = 0; i < correctionDb.size(); i++)
    {
        correctionDb[i] = compressCorrection(
            correctionDb[i],
            settings.softLimitDb,
            settings.hardLimitDb
        );
    }

    return true;
}

// Сохранение кривой коррекции в CSV
bool saveEqualizerCurve(
    const Spectrum& spectrum,
    const vector<double>& correctionDb,
    const vector<double>& requestedCorrectionDb,
    const string& filename,
    string& error)
{
    if (spectrum.frequencies.size() != correctionDb.size() ||
        correctionDb.size() != requestedCorrectionDb.size())
    {
        error = "Invalid equalizer curve data.";
        return false;
    }

    ofstream file(filename);

    if (!file.is_open())
    {
        error = "Unable to create equalizer curve file.";
        return false;
    }

    file << "Frequency,RequestedCorrectionDb,AppliedCorrectionDb\n";
    file << fixed << setprecision(6);

    for (size_t i = 0; i < correctionDb.size(); i++)
    {
        file << spectrum.frequencies[i] << ","
            << requestedCorrectionDb[i] << ","
            << correctionDb[i] << "\n";
    }

    file.close();

    return true;
}

// Применение статической эквалайзерной кривой
bool applyEqualizer(
    const AudioData& inputAudio,
    const Spectrum& spectrum,
    const vector<double>& correctionDb,
    AudioData& outputAudio,
    string& error)
{
    const size_t FFT_SIZE = 8192;
    const size_t HOP_SIZE = 2048;

    if (inputAudio.channels == 0 ||
        inputAudio.sampleRate == 0 ||
        inputAudio.frameCount == 0)
    {
        error = "Invalid input audio data.";
        return false;
    }

    if (inputAudio.sampleRate < 40000)
    {
        error = "Sample rate is too low.";
        return false;
    }

    if (inputAudio.samples.size() !=
        inputAudio.frameCount * inputAudio.channels)
    {
        error = "Invalid input audio size.";
        return false;
    }

    if (spectrum.frequencies.size() < 2 ||
        spectrum.frequencies.size() != correctionDb.size())
    {
        error = "Invalid equalizer curve.";
        return false;
    }

    // Создание выходного аудиофайла
    outputAudio = inputAudio;

    outputAudio.samples.assign(
        inputAudio.samples.size(),
        0.0f
    );

    // Создание окна Ханна
    vector<double> window(FFT_SIZE);

    for (size_t i = 0; i < FFT_SIZE; i++)
    {
        window[i] = 0.5 * (
            1.0 - cos(
                2.0 * 3.14159265358979323846 *
                i / (FFT_SIZE - 1)
            )
            );
    }

    // Расчёт коэффициентов усиления для частотных бинов
    vector<double> gains(FFT_SIZE / 2 + 1);

    for (size_t k = 0; k <= FFT_SIZE / 2; k++)
    {
        double frequency =
            static_cast<double>(k) *
            inputAudio.sampleRate / FFT_SIZE;

        double gainDb = interpolateValue(
            spectrum.frequencies,
            correctionDb,
            frequency
        );

        gains[k] = pow(10.0, gainDb / 20.0);
    }

    // Буфер для накопления результата
    vector<double> output(
        inputAudio.frameCount * inputAudio.channels,
        0.0
    );

    // Нормализация перекрывающихся окон
    vector<double> normalization(
        inputAudio.frameCount,
        0.0
    );

    vector<double> real(FFT_SIZE);
    vector<double> imag(FFT_SIZE);

    // Обработка всех временных окон
    long long start = -static_cast<long long>(
        FFT_SIZE - HOP_SIZE
        );

    while (start < static_cast<long long>(
        inputAudio.frameCount))
    {
        // Накопление коэффициентов нормализации
        for (size_t i = 0; i < FFT_SIZE; i++)
        {
            long long frame = start + i;

            if (frame >= 0 &&
                frame < static_cast<long long>(
                    inputAudio.frameCount))
            {
                normalization[frame] +=
                    window[i] * window[i];
            }
        }

        // Обработка каждого канала
        for (unsigned int channel = 0;
            channel < inputAudio.channels;
            channel++)
        {
            fill(real.begin(), real.end(), 0.0);
            fill(imag.begin(), imag.end(), 0.0);

            // Подготовка входного сигнала
            for (size_t i = 0; i < FFT_SIZE; i++)
            {
                long long frame = start + i;

                double sample = 0.0;

                if (frame >= 0 &&
                    frame < static_cast<long long>(
                        inputAudio.frameCount))
                {
                    size_t index =
                        static_cast<size_t>(frame) *
                        inputAudio.channels + channel;

                    sample = inputAudio.samples[index];
                }

                real[i] = sample * window[i];
            }

            // Прямое преобразование Фурье
            fft(real, imag);

            // Применение частотной коррекции
            for (size_t k = 0; k <= FFT_SIZE / 2; k++)
            {
                real[k] *= gains[k];
                imag[k] *= gains[k];

                // Симметричное изменение отрицательных частот
                if (k > 0 && k < FFT_SIZE / 2)
                {
                    size_t mirror = FFT_SIZE - k;

                    real[mirror] *= gains[k];
                    imag[mirror] *= gains[k];
                }
            }

            // Обратное преобразование Фурье
            ifft(real, imag);

            // Добавление результата в выходной буфер
            for (size_t i = 0; i < FFT_SIZE; i++)
            {
                long long frame = start + i;

                if (frame >= 0 &&
                    frame < static_cast<long long>(
                        inputAudio.frameCount))
                {
                    size_t index =
                        static_cast<size_t>(frame) *
                        inputAudio.channels + channel;

                    output[index] +=
                        real[i] * window[i];
                }
            }
        }

        start += HOP_SIZE;
    }

    // Нормализация результата после перекрытия окон
    for (size_t frame = 0;
        frame < inputAudio.frameCount;
        frame++)
    {
        if (normalization[frame] <= 0.0)
        {
            error = "Invalid overlap-add normalization.";
            return false;
        }

        for (unsigned int channel = 0;
            channel < inputAudio.channels;
            channel++)
        {
            size_t index =
                frame * inputAudio.channels + channel;

            outputAudio.samples[index] =
                static_cast<float>(
                    output[index] / normalization[frame]
                    );
        }
    }

    return true;
}

// Экранирование текста для XML/SVG
static string escapeXml(const string& text)
{
    string result;
    result.reserve(text.size());

    for (char c : text)
    {
        switch (c)
        {
        case '&':  result += "&amp;";  break;
        case '<':  result += "&lt;";   break;
        case '>':  result += "&gt;";   break;
        case '"':  result += "&quot;"; break;
        case '\'': result += "&apos;"; break;
        default:   result += c;        break;
        }
    }

    return result;
}

// Двухколоночный заголовок SVG-графиков.
static void writeGraphTitle(ofstream& file, const string& title, double width)
{
    const size_t declipPosition = title.find(" | declip ");
    string left = declipPosition == string::npos ? title : title.substr(0, declipPosition);
    string right = declipPosition == string::npos ? string() : title.substr(declipPosition + 3);

    string leftFirst = left;
    string leftSecond;
    const size_t leftSplit = left.rfind(" | ");
    if (leftSplit != string::npos)
    {
        leftFirst = left.substr(0, leftSplit);
        leftSecond = left.substr(leftSplit + 3);
    }

    string rightFirst = right;
    string rightSecond;
    const size_t rightSplit = right.find(", attack ");
    if (rightSplit != string::npos)
    {
        rightFirst = right.substr(0, rightSplit);
        rightSecond = right.substr(rightSplit + 2);
    }

    writeGraphTitle(file, title, WIDTH);

    // Горизонтальная сетка и подписи по оси dB
    for (double db = -limit; db <= limit + 1e-9; db += step)
    {
        double y = yPosition(db);
        bool isZero = abs(db) < 1e-9;

        file << "<line x1=\"" << MARGIN_LEFT << "\" y1=\"" << y
            << "\" x2=\"" << MARGIN_LEFT + plotWidth << "\" y2=\"" << y
            << "\" stroke=\"" << (isZero ? "#666666" : "#dddddd")
            << "\" stroke-width=\"" << (isZero ? 1.5 : 1.0) << "\"/>\n";

        writeGraphTitle(file, title, WIDTH);

    // Горизонтальная сетка и подписи по оси dB
    for (double db = yMin; db <= yMax + 1e-9; db += step)
    {
        double y = yPosition(db);
        bool isZero = abs(db) < 1e-9;

        file << "<line x1=\"" << MARGIN_LEFT << "\" y1=\"" << y
            << "\" x2=\"" << MARGIN_LEFT + plotWidth << "\" y2=\"" << y
            << "\" stroke=\"" << (isZero ? "#666666" : "#dddddd")
            << "\" stroke-width=\"" << (isZero ? 1.5 : 1.0) << "\"/>\n";

        writeGraphTitle(file, title, WIDTH);

    // Легенда
    {
        double legendX = MARGIN_LEFT;
        const double legendY = 46.0;

        auto legendItem = [&](const char* color, const char* label, double width,
            double opacity)
            {
                file << "<rect x=\"" << legendX << "\" y=\"" << legendY - 5.0
                    << "\" width=\"22\" height=\"10\" fill=\"" << color
                    << "\" fill-opacity=\"" << opacity << "\" stroke=\"" << color << "\"/>\n";

                writeGraphTitle(file, title, WIDTH);

    // Подпись дорожек
    {
        ostringstream caption;

        caption << fixed << setprecision(0)
            << "Gain reduction by band, Hz (each lane: 0 to -"
            << depthMax << " dB)";

        file << "<text x=\"" << MARGIN_LEFT << "\" y=\"" << lanesTop - 10.0
            << "\" fill=\"#222222\" font-weight=\"bold\">"
            << escapeXml(caption.str()) << "</text>\n";
    }

    // Сетка по времени
    double timeStep = 3600.0;

    for (double candidate : { 0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0,
        30.0, 60.0, 120.0, 300.0, 600.0, 1200.0, 1800.0, 3600.0 })
    {
        if (duration / candidate <= 10.0)
        {
            timeStep = candidate;
            break;
        }
    }

    const bool minutes = duration >= 120.0;
    const int decimals = timeStep < 1.0 ? 1 : 0;

    for (double time = 0.0; time <= duration + 1e-9; time += timeStep)
    {
        const double x = MARGIN_LEFT + plotWidth * time / duration;

        file << "<line x1=\"" << x << "\" y1=\"" << lanesTop
            << "\" x2=\"" << x << "\" y2=\"" << lanesBottom
            << "\" stroke=\"#eeeeee\"/>\n";

        file << "<text x=\"" << x << "\" y=\"" << lanesBottom + 16.0
            << "\" text-anchor=\"middle\" fill=\"#444444\">"
            << formatTimeLabel(time, minutes, decimals) << "</text>\n";
    }

    file << "<text x=\"" << MARGIN_LEFT + plotWidth / 2.0 << "\" y=\""
        << lanesBottom + 32.0
        << "\" text-anchor=\"middle\" fill=\"#222222\">"
        << (minutes ? "Time, min:s" : "Time, s") << "</text>\n";

    // Дорожки
    for (size_t lane = 0; lane < laneCount; lane++)
    {
        const double top = lanesTop + lane * (laneHeight + laneGap);
        const bool common = lane == BAND_COUNT;
        const char* color = common ? "#5b6bb5" : "#e07b00";

        file << "<rect x=\"" << MARGIN_LEFT << "\" y=\"" << top
            << "\" width=\"" << plotWidth << "\" height=\"" << laneHeight
            << "\" fill=\"none\" stroke=\"#cccccc\"/>\n";

        double laneMax = 0.0;

        for (size_t c = 0; c < columns; c++)
        {
            laneMax = min(laneMax, laneValue(lane, c));
        }

        // Дорожка без снижения остаётся пустой
        if (laneMax < -0.05)
        {
            file << "<polygon fill=\"" << color
                << "\" fill-opacity=\"0.65\" stroke=\"" << color
                << "\" stroke-width=\"0.5\" points=\"";

            for (size_t c = 0; c < columns; c++)
            {
                file << xColumn(c) << ","
                    << top + laneHeight * (-laneValue(lane, c)) / depthMax << " ";
            }

            file << xColumn(columns - 1) << "," << top << " "
                << xColumn(0) << "," << top << "\"/>\n";
        }

        const string label = common ? string("common") : getBandLabel(lane);

        file << "<text x=\"" << MARGIN_LEFT - 8.0 << "\" y=\""
            << top + laneHeight / 2.0 + 4.0
            << "\" text-anchor=\"end\" fill=\"#444444\">"
            << escapeXml(label) << "</text>\n";

        ostringstream maxText;

        if (laneMax < -0.05)
        {
            maxText << fixed << setprecision(1) << "max " << laneMax << " dB";
        }
        else
        {
            maxText << "no reduction";
        }

        file << "<text x=\"" << MARGIN_LEFT + plotWidth - 4.0 << "\" y=\""
            << top + laneHeight / 2.0 + 4.0
            << "\" text-anchor=\"end\" fill=\"#555555\" font-size=\"10\">"
            << maxText.str() << "</text>\n";
    }

    // Сильнейшее событие
    {
        ostringstream caption;

        caption << fixed << setprecision(4)
            << "Strongest event (first channel), "
            << static_cast<double>(bands.eventStart) / sampleRate << " s: "
            << "before (gray) and after (blue) band reduction";

        file << "<text x=\"" << MARGIN_LEFT << "\" y=\"" << zoomTop - 12.0
            << "\" fill=\"#222222\" font-weight=\"bold\">"
            << escapeXml(caption.str()) << "</text>\n";
    }

    for (int i = 0; i <= 4; i++)
    {
        const double value = zoomMin + (zoomMax - zoomMin) * i / 4.0;
        const double y = yZoom(value);

        file << "<line x1=\"" << MARGIN_LEFT << "\" y1=\"" << y
            << "\" x2=\"" << MARGIN_LEFT + plotWidth << "\" y2=\"" << y
            << "\" stroke=\"#dddddd\"/>\n";

        file << "<text x=\"" << MARGIN_LEFT - 8.0 << "\" y=\"" << y + 4.0
            << "\" text-anchor=\"end\" fill=\"#444444\">"
            << setprecision(2) << value << setprecision(1) << "</text>\n";

        const size_t k = (count - 1) * i / 4;
        const double x = xZoom(k);

        file << "<line x1=\"" << x << "\" y1=\"" << zoomTop
            << "\" x2=\"" << x << "\" y2=\"" << zoomTop + zoomHeight
            << "\" stroke=\"#eeeeee\"/>\n";

        file << "<text x=\"" << x << "\" y=\"" << zoomTop + zoomHeight + 16.0
            << "\" text-anchor=\"middle\" fill=\"#444444\">"
            << setprecision(4)
            << static_cast<double>(bands.eventStart + k) / sampleRate
            << setprecision(1) << "</text>\n";
    }

    file << "<text x=\"" << MARGIN_LEFT + plotWidth / 2.0 << "\" y=\""
        << zoomTop + zoomHeight + 34.0
        << "\" text-anchor=\"middle\" fill=\"#222222\">"
        << "Time, s (red dashed lines: ceiling)</text>\n";

    // Потолок
    for (int sign = -1; sign <= 1; sign += 2)
    {
        const double y = yZoom(sign * ceilingValue);

        file << "<line x1=\"" << MARGIN_LEFT << "\" y1=\"" << y
            << "\" x2=\"" << MARGIN_LEFT + plotWidth << "\" y2=\"" << y
            << "\" stroke=\"#cc3333\" stroke-width=\"1\" "
            << "stroke-dasharray=\"4,4\"/>\n";
    }

    // Файл остаётся лёгким: если отсчётов больше, чем нужно для рисунка,
    // на каждый столбец выводятся минимум и максимум (пики не теряются)
    const size_t zoomColumns = min(count, static_cast<size_t>(plotWidth / 1.5));

    auto writeLine = [&](const vector<float>& values,
        const char* color,
        double strokeWidth)
        {
            file << "<polyline fill=\"none\" stroke=\"" << color
                << "\" stroke-width=\"" << strokeWidth
                << "\" stroke-linejoin=\"round\" points=\"";

            if (count <= zoomColumns * 2)
            {
                for (size_t k = 0; k < count; k++)
                {
                    file << xZoom(k) << "," << yZoom(values[k]) << " ";
                }
            }
            else
            {
                for (size_t c = 0; c < zoomColumns; c++)
                {
                    const size_t from = count * c / zoomColumns;
                    const size_t to = max(from + 1, count * (c + 1) / zoomColumns);

                    size_t lowIndex = from;
                    size_t highIndex = from;

                    for (size_t k = from; k < to && k < count; k++)
                    {
                        if (values[k] < values[lowIndex]) lowIndex = k;
                        if (values[k] > values[highIndex]) highIndex = k;
                    }

                    const double x = MARGIN_LEFT +
                        plotWidth * (c + 0.5) / zoomColumns;

                    const size_t first = min(lowIndex, highIndex);
                    const size_t second = max(lowIndex, highIndex);

                    file << x << "," << yZoom(values[first]) << " ";

                    if (second != first)
                    {
                        file << x << "," << yZoom(values[second]) << " ";
                    }
                }
            }

            file << "\"/>\n";
        };

    writeLine(bands.eventBefore, "#888888", 2.0);
    writeLine(bands.eventAfter, "#1f6feb", 2.2);

    file << "<rect x=\"" << MARGIN_LEFT << "\" y=\"" << zoomTop
        << "\" width=\"" << plotWidth << "\" height=\"" << zoomHeight
        << "\" fill=\"none\" stroke=\"#888888\"/>\n";

    file << "</svg>\n";

    file.close();

    if (file.fail())
    {
        error = "Unable to write band reduction graph file.";
        return false;
    }

    return true;
}