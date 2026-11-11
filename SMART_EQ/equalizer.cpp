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
    if (!(settings.intensity >= 0.0 && settings.intensity <= 1.0) ||
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

    // При нулевой интенсивности эквалайзер полностью прозрачен:
    // не выполняем FFT и сохраняем аудиоданные без каких-либо изменений.
    const bool noCorrection = all_of(
        correctionDb.begin(),
        correctionDb.end(),
        [](double value) { return abs(value) < 1e-12; }
    );

    if (noCorrection)
    {
        outputAudio = inputAudio;
        return true;
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
    const size_t firstSeparator = left.find(" | ");
    const size_t secondSeparator = firstSeparator == string::npos
        ? string::npos
        : left.find(" | ", firstSeparator + 3);
    if (secondSeparator != string::npos)
    {
        leftFirst = left.substr(0, secondSeparator);
        leftSecond = left.substr(secondSeparator + 3);
    }

    string rightFirst = right;
    string rightSecond;
    const size_t rightSplit = right.find(", attack ");
    if (rightSplit != string::npos)
    {
        rightFirst = right.substr(0, rightSplit);
        rightSecond = right.substr(rightSplit + 2);
    }

    file << "<text x=\"24\" y=\"18\" text-anchor=\"start\" font-size=\"12\" font-weight=\"bold\" fill=\"#222222\">"
        << escapeXml(leftFirst) << "</text>\n";
    if (!leftSecond.empty())
        file << "<text x=\"24\" y=\"34\" text-anchor=\"start\" font-size=\"12\" font-weight=\"bold\" fill=\"#222222\">"
            << escapeXml(leftSecond) << "</text>\n";

    if (!rightFirst.empty())
        file << "<text x=\"" << width - 24.0 << "\" y=\"18\" text-anchor=\"end\" font-size=\"12\" font-weight=\"bold\" fill=\"#222222\">"
            << escapeXml(rightFirst) << "</text>\n";
    if (!rightSecond.empty())
        file << "<text x=\"" << width - 24.0 << "\" y=\"34\" text-anchor=\"end\" font-size=\"12\" font-weight=\"bold\" fill=\"#222222\">"
            << escapeXml(rightSecond) << "</text>\n";
}

// Сохранение графика кривой коррекции в SVG
bool saveEqualizerGraph(
    const Spectrum& spectrum,
    const vector<double>& correctionDb,
    const vector<double>& requestedCorrectionDb,
    double hardLimitDb,
    const filesystem::path& filename,
    const string& title,
    string& error)
{
    if (spectrum.frequencies.size() < 2 ||
        spectrum.frequencies.size() != correctionDb.size() ||
        correctionDb.size() != requestedCorrectionDb.size())
    {
        error = "Invalid equalizer curve data.";
        return false;
    }

    const double WIDTH = 800.0;
    const double HEIGHT = 430.0;
    const double MARGIN_LEFT = 64.0;
    const double MARGIN_RIGHT = 24.0;
    const double MARGIN_TOP = 60.0;
    const double MARGIN_BOTTOM = 52.0;

    const double plotWidth = WIDTH - MARGIN_LEFT - MARGIN_RIGHT;
    const double plotHeight = HEIGHT - MARGIN_TOP - MARGIN_BOTTOM;

    const double MIN_FREQUENCY = 20.0;
    const double MAX_FREQUENCY = 20000.0;

    // Подбор вертикального диапазона по данным
    double maxAbs = hardLimitDb;

    for (size_t i = 0; i < correctionDb.size(); i++)
    {
        maxAbs = max(maxAbs, abs(correctionDb[i]));
        maxAbs = max(maxAbs, abs(requestedCorrectionDb[i]));
    }

    double limit = max(6.0, ceil(maxAbs) + 1.0);

    double step = 2.0;

    if (limit > 30.0)
    {
        step = 10.0;
    }
    else if (limit > 12.0)
    {
        step = 5.0;
    }

    limit = ceil(limit / step) * step;

    auto xPosition = [&](double frequency)
        {
            frequency = min(max(frequency, MIN_FREQUENCY), MAX_FREQUENCY);

            return MARGIN_LEFT + plotWidth *
                (log10(frequency) - log10(MIN_FREQUENCY)) /
                (log10(MAX_FREQUENCY) - log10(MIN_FREQUENCY));
        };

    auto yPosition = [&](double db)
        {
            return MARGIN_TOP + plotHeight * (limit - db) / (2.0 * limit);
        };

    ofstream file(filename);

    if (!file.is_open())
    {
        error = "Unable to create equalizer graph file.";
        return false;
    }

    file << fixed << setprecision(2);

    file << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    file << "<svg xmlns=\"http://www.w3.org/2000/svg\" "
        << "viewBox=\"0 0 " << WIDTH << " " << HEIGHT << "\" "
        << "width=\"" << WIDTH << "\" height=\"" << HEIGHT << "\" "
        << "font-family=\"Arial, Helvetica, sans-serif\" font-size=\"12\">\n";

    // Фон
    file << "<rect width=\"100%\" height=\"100%\" fill=\"#ffffff\"/>\n";

    // Заголовок
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

        file << "<text x=\"" << MARGIN_LEFT - 8.0 << "\" y=\"" << y + 4.0
            << "\" text-anchor=\"end\" fill=\"#444444\">"
            << setprecision(0) << (db > 0.0 ? "+" : "") << db
            << setprecision(2) << "</text>\n";
    }

    // Вертикальная сетка и подписи по оси частот
    const double gridFrequencies[] = {
        20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000
    };

    const char* gridLabels[] = {
        "20", "50", "100", "200", "500", "1k", "2k", "5k", "10k", "20k"
    };

    for (int i = 0; i < 10; i++)
    {
        double x = xPosition(gridFrequencies[i]);

        file << "<line x1=\"" << x << "\" y1=\"" << MARGIN_TOP
            << "\" x2=\"" << x << "\" y2=\"" << MARGIN_TOP + plotHeight
            << "\" stroke=\"#dddddd\"/>\n";

        file << "<text x=\"" << x << "\" y=\""
            << MARGIN_TOP + plotHeight + 18.0
            << "\" text-anchor=\"middle\" fill=\"#444444\">"
            << gridLabels[i] << "</text>\n";
    }

    // Рамка графика
    file << "<rect x=\"" << MARGIN_LEFT << "\" y=\"" << MARGIN_TOP
        << "\" width=\"" << plotWidth << "\" height=\"" << plotHeight
        << "\" fill=\"none\" stroke=\"#888888\"/>\n";

    // Предел сжатия коррекции
    for (int sign = -1; sign <= 1; sign += 2)
    {
        double y = yPosition(sign * hardLimitDb);

        file << "<line x1=\"" << MARGIN_LEFT << "\" y1=\"" << y
            << "\" x2=\"" << MARGIN_LEFT + plotWidth << "\" y2=\"" << y
            << "\" stroke=\"#cc4444\" stroke-width=\"1\" "
            << "stroke-dasharray=\"2,4\"/>\n";
    }

    // Построение ломаной линии
    auto writePolyline = [&](const vector<double>& values,
        const char* color,
        double strokeWidth,
        const char* dashArray)
        {
            file << "<polyline fill=\"none\" stroke=\"" << color
                << "\" stroke-width=\"" << strokeWidth
                << "\" stroke-linejoin=\"round\"";

            if (dashArray != nullptr)
            {
                file << " stroke-dasharray=\"" << dashArray << "\"";
            }

            file << " points=\"";

            for (size_t i = 0; i < values.size(); i++)
            {
                file << xPosition(spectrum.frequencies[i]) << ","
                    << yPosition(values[i]) << " ";
            }

            file << "\"/>\n";
        };

    writePolyline(requestedCorrectionDb, "#999999", 1.5, "6,4");
    writePolyline(correctionDb, "#1f6feb", 2.5, nullptr);

    // Легенда
    const double legendRight = MARGIN_LEFT + plotWidth;
    const double legendY = 46.0;

    file << "<line x1=\"" << legendRight - 300.0 << "\" y1=\"" << legendY
        << "\" x2=\"" << legendRight - 274.0 << "\" y2=\"" << legendY
        << "\" stroke=\"#999999\" stroke-width=\"1.5\" "
        << "stroke-dasharray=\"6,4\"/>\n";

    file << "<text x=\"" << legendRight - 268.0 << "\" y=\""
        << legendY + 4.0 << "\" fill=\"#444444\">Requested</text>\n";

    file << "<line x1=\"" << legendRight - 190.0 << "\" y1=\"" << legendY
        << "\" x2=\"" << legendRight - 164.0 << "\" y2=\"" << legendY
        << "\" stroke=\"#1f6feb\" stroke-width=\"2.5\"/>\n";

    file << "<text x=\"" << legendRight - 158.0 << "\" y=\""
        << legendY + 4.0 << "\" fill=\"#444444\">Applied</text>\n";

    file << "<line x1=\"" << legendRight - 90.0 << "\" y1=\"" << legendY
        << "\" x2=\"" << legendRight - 64.0 << "\" y2=\"" << legendY
        << "\" stroke=\"#cc4444\" stroke-dasharray=\"2,4\"/>\n";

    file << "<text x=\"" << legendRight - 58.0 << "\" y=\""
        << legendY + 4.0 << "\" fill=\"#444444\">Limit</text>\n";

    // Подписи осей
    file << "<text x=\"" << MARGIN_LEFT + plotWidth / 2.0 << "\" y=\""
        << HEIGHT - 10.0 << "\" text-anchor=\"middle\" fill=\"#222222\">"
        << "Frequency, Hz</text>\n";

    file << "<text transform=\"translate(18 "
        << MARGIN_TOP + plotHeight / 2.0
        << ") rotate(-90)\" text-anchor=\"middle\" fill=\"#222222\">"
        << "Correction, dB</text>\n";

    file << "</svg>\n";

    file.close();

    if (file.fail())
    {
        error = "Unable to write equalizer graph file.";
        return false;
    }

    return true;
}

// Сохранение графика спектра до/после эквализации в сравнении с целевым шумом
bool saveSpectrumComparisonGraph(
    const Spectrum& before,
    const Spectrum& after,
    NoiseProfile profile,
    const filesystem::path& filename,
    const string& title,
    string& error)
{
    if (before.frequencies.size() < 2 ||
        before.frequencies.size() != before.powerDb.size() ||
        after.frequencies.size() != before.frequencies.size() ||
        after.powerDb.size() != after.frequencies.size())
    {
        error = "Invalid spectrum data.";
        return false;
    }

    if (before.frequencies.front() > REFERENCE_FREQUENCY ||
        before.frequencies.back() < REFERENCE_FREQUENCY)
    {
        error = "Spectrum does not contain the reference frequency.";
        return false;
    }

    // Нормализация относительно 1000 Гц с ограничением выбросов
    auto normalize = [&](const Spectrum& s)
        {
            double reference = interpolateValue(
                s.frequencies,
                s.powerDb,
                REFERENCE_FREQUENCY
            );

            vector<double> result(s.powerDb.size());

            for (size_t i = 0; i < result.size(); i++)
            {
                result[i] = min(
                    max(s.powerDb[i] - reference, -120.0),
                    60.0
                );
            }

            return result;
        };

    vector<double> beforeDb = normalize(before);
    vector<double> afterDb = normalize(after);

    // Целевая кривая выбранного шума
    vector<double> targetDb(before.frequencies.size());

    for (size_t i = 0; i < targetDb.size(); i++)
    {
        targetDb[i] = getTargetPowerDb(before.frequencies[i], profile);
    }

    // Вертикальный диапазон по данным
    double minValue = targetDb[0];
    double maxValue = targetDb[0];

    for (size_t i = 0; i < targetDb.size(); i++)
    {
        minValue = min({ minValue, beforeDb[i], afterDb[i], targetDb[i] });
        maxValue = max({ maxValue, beforeDb[i], afterDb[i], targetDb[i] });
    }

    double range = maxValue - minValue;
    double step = 5.0;

    if (range > 100.0)
    {
        step = 20.0;
    }
    else if (range > 40.0)
    {
        step = 10.0;
    }

    const double yMin = floor((minValue - 2.0) / step) * step;
    const double yMax = ceil((maxValue + 2.0) / step) * step;

    const double WIDTH = 800.0;
    const double HEIGHT = 430.0;
    const double MARGIN_LEFT = 64.0;
    const double MARGIN_RIGHT = 24.0;
    const double MARGIN_TOP = 60.0;
    const double MARGIN_BOTTOM = 52.0;

    const double plotWidth = WIDTH - MARGIN_LEFT - MARGIN_RIGHT;
    const double plotHeight = HEIGHT - MARGIN_TOP - MARGIN_BOTTOM;

    const double MIN_FREQUENCY = 20.0;
    const double MAX_FREQUENCY = 20000.0;

    auto xPosition = [&](double frequency)
        {
            frequency = min(max(frequency, MIN_FREQUENCY), MAX_FREQUENCY);

            return MARGIN_LEFT + plotWidth *
                (log10(frequency) - log10(MIN_FREQUENCY)) /
                (log10(MAX_FREQUENCY) - log10(MIN_FREQUENCY));
        };

    auto yPosition = [&](double db)
        {
            return MARGIN_TOP + plotHeight * (yMax - db) / (yMax - yMin);
        };

    ofstream file(filename);

    if (!file.is_open())
    {
        error = "Unable to create spectrum graph file.";
        return false;
    }

    file << fixed << setprecision(2);

    file << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    file << "<svg xmlns=\"http://www.w3.org/2000/svg\" "
        << "viewBox=\"0 0 " << WIDTH << " " << HEIGHT << "\" "
        << "width=\"" << WIDTH << "\" height=\"" << HEIGHT << "\" "
        << "font-family=\"Arial, Helvetica, sans-serif\" font-size=\"12\">\n";

    file << "<rect width=\"100%\" height=\"100%\" fill=\"#ffffff\"/>\n";

    // Заголовок
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

        file << "<text x=\"" << MARGIN_LEFT - 8.0 << "\" y=\"" << y + 4.0
            << "\" text-anchor=\"end\" fill=\"#444444\">"
            << setprecision(0) << (db > 0.0 ? "+" : "") << db
            << setprecision(2) << "</text>\n";
    }

    // Вертикальная сетка и подписи по оси частот
    const double gridFrequencies[] = {
        20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000
    };

    const char* gridLabels[] = {
        "20", "50", "100", "200", "500", "1k", "2k", "5k", "10k", "20k"
    };

    for (int i = 0; i < 10; i++)
    {
        double x = xPosition(gridFrequencies[i]);

        file << "<line x1=\"" << x << "\" y1=\"" << MARGIN_TOP
            << "\" x2=\"" << x << "\" y2=\"" << MARGIN_TOP + plotHeight
            << "\" stroke=\"#dddddd\"/>\n";

        file << "<text x=\"" << x << "\" y=\""
            << MARGIN_TOP + plotHeight + 18.0
            << "\" text-anchor=\"middle\" fill=\"#444444\">"
            << gridLabels[i] << "</text>\n";
    }

    // Рамка графика
    file << "<rect x=\"" << MARGIN_LEFT << "\" y=\"" << MARGIN_TOP
        << "\" width=\"" << plotWidth << "\" height=\"" << plotHeight
        << "\" fill=\"none\" stroke=\"#888888\"/>\n";

    // Построение ломаной линии
    auto writePolyline = [&](const vector<double>& frequencies,
        const vector<double>& values,
        const char* color,
        double strokeWidth,
        const char* dashArray)
        {
            file << "<polyline fill=\"none\" stroke=\"" << color
                << "\" stroke-width=\"" << strokeWidth
                << "\" stroke-linejoin=\"round\"";

            if (dashArray != nullptr)
            {
                file << " stroke-dasharray=\"" << dashArray << "\"";
            }

            file << " points=\"";

            for (size_t i = 0; i < values.size(); i++)
            {
                file << xPosition(frequencies[i]) << ","
                    << yPosition(values[i]) << " ";
            }

            file << "\"/>\n";
        };

    writePolyline(before.frequencies, beforeDb, "#999999", 2.0, nullptr);
    writePolyline(after.frequencies, afterDb, "#1f6feb", 2.5, nullptr);
    writePolyline(before.frequencies, targetDb, "#e07b00", 2.5, "6,4");

    // Легенда
    const double legendY = 46.0;
    double legendX = MARGIN_LEFT;

    auto writeLegendItem = [&](const char* color,
        double strokeWidth,
        const char* dashArray,
        const string& label,
        double itemWidth)
        {
            file << "<line x1=\"" << legendX << "\" y1=\"" << legendY
                << "\" x2=\"" << legendX + 26.0 << "\" y2=\"" << legendY
                << "\" stroke=\"" << color
                << "\" stroke-width=\"" << strokeWidth << "\"";

            if (dashArray != nullptr)
            {
                file << " stroke-dasharray=\"" << dashArray << "\"";
            }

            file << "/>\n";

            file << "<text x=\"" << legendX + 32.0 << "\" y=\""
                << legendY + 4.0 << "\" fill=\"#444444\">"
                << escapeXml(label) << "</text>\n";

            legendX += itemWidth;
        };

    writeLegendItem("#999999", 2.0, nullptr, "Before EQ", 120.0);
    writeLegendItem("#1f6feb", 2.5, nullptr, "After EQ", 115.0);
    writeLegendItem(
        "#e07b00", 2.5, "6,4",
        "Target: " + getProfileName(profile),
        180.0
    );

    // Подписи осей
    file << "<text x=\"" << MARGIN_LEFT + plotWidth / 2.0 << "\" y=\""
        << HEIGHT - 10.0 << "\" text-anchor=\"middle\" fill=\"#222222\">"
        << "Frequency, Hz</text>\n";

    file << "<text transform=\"translate(18 "
        << MARGIN_TOP + plotHeight / 2.0
        << ") rotate(-90)\" text-anchor=\"middle\" fill=\"#222222\">"
        << "Level relative to 1 kHz, dB</text>\n";

    file << "</svg>\n";

    file.close();

    if (file.fail())
    {
        error = "Unable to write spectrum graph file.";
        return false;
    }

    return true;
}

// Нормализация пиков. Возвращает применённое усиление в dB (0, если не требовалось)
double limitPeaks(AudioData& audio, double ceilingDb)
{
    double peak = 0.0;

    for (float sample : audio.samples)
    {
        peak = max(peak, static_cast<double>(abs(sample)));
    }

    const double ceiling = pow(10.0, ceilingDb / 20.0);

    if (peak <= ceiling || peak <= 0.0)
    {
        return 0.0;
    }

    const double gain = ceiling / peak;

    for (float& sample : audio.samples)
    {
        sample = static_cast<float>(sample * gain);
    }

    return 20.0 * log10(gain);
}

// Анализ качества исходного трека: пик, уровень, клиппинг
AudioQuality analyzeQuality(const AudioData& audio, size_t minRunLength)
{
    AudioQuality quality;

    if (audio.samples.empty() || audio.channels == 0)
    {
        return quality;
    }

    double peak = 0.0;
    double sum = 0.0;
    double sumSquares = 0.0;

    for (float sample : audio.samples)
    {
        double value = sample;

        peak = max(peak, abs(value));
        sum += value;
        sumSquares += value * value;
    }

    const double count = static_cast<double>(audio.samples.size());
    const double rms = sqrt(sumSquares / count);

    quality.dcOffset = sum / count;
    quality.peakDb = 20.0 * log10(max(peak, 1e-12));
    quality.rmsDb = 20.0 * log10(max(rms, 1e-12));
    quality.crestDb = quality.peakDb - quality.rmsDb;

    // Для очень тихих треков поиск срезов не имеет смысла
    if (peak < 0.1)
    {
        return quality;
    }

    const double threshold = peak * 0.9995;

    for (unsigned int channel = 0; channel < audio.channels; channel++)
    {
        size_t run = 0;

        auto flush = [&]()
            {
                if (run >= minRunLength)
                {
                    quality.clippedRuns++;
                    quality.clippedSamples += run;
                }

                run = 0;
            };

        for (uint64_t frame = 0; frame < audio.frameCount; frame++)
        {
            double value = abs(static_cast<double>(
                audio.samples[frame * audio.channels + channel]));

            if (value >= threshold)
            {
                run++;
            }
            else
            {
                flush();
            }
        }

        flush();
    }

    return quality;
}

// ---------- Восстановление срезанных пиков ----------

// Решение системы A x = b методом Холецкого
// (A симметричная положительно определённая, размер n x n, хранится по строкам)
static bool solveSymmetricSystem(
    vector<double>& matrix,
    vector<double>& rhs,
    size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        for (size_t j = 0; j <= i; j++)
        {
            double sum = matrix[i * n + j];

            for (size_t k = 0; k < j; k++)
            {
                sum -= matrix[i * n + k] * matrix[j * n + k];
            }

            if (i == j)
            {
                if (sum <= 1e-12)
                {
                    return false;
                }

                matrix[i * n + i] = sqrt(sum);
            }
            else
            {
                matrix[i * n + j] = sum / matrix[j * n + j];
            }
        }
    }

    for (size_t i = 0; i < n; i++)
    {
        double sum = rhs[i];

        for (size_t k = 0; k < i; k++)
        {
            sum -= matrix[i * n + k] * rhs[k];
        }

        rhs[i] = sum / matrix[i * n + i];
    }

    for (size_t i = n; i-- > 0;)
    {
        double sum = rhs[i];

        for (size_t k = i + 1; k < n; k++)
        {
            sum -= matrix[k * n + i] * rhs[k];
        }

        rhs[i] = sum / matrix[i * n + i];
    }

    return true;
}

// Коэффициенты линейного предсказания (a[0] = 1):
// метод автокорреляции с окном Ханна и рекурсия Левинсона-Дурбина
static bool estimateAutoregression(
    const vector<double>& y,
    size_t order,
    vector<double>& a)
{
    const size_t n = y.size();

    if (n < order * 4)
    {
        return false;
    }

    vector<double> windowed(n);

    for (size_t i = 0; i < n; i++)
    {
        const double w = 0.5 * (1.0 - cos(
            2.0 * 3.14159265358979323846 * (i + 0.5) / n));

        windowed[i] = y[i] * w;
    }

    vector<double> r(order + 1, 0.0);

    for (size_t k = 0; k <= order; k++)
    {
        double sum = 0.0;

        for (size_t i = 0; i + k < n; i++)
        {
            sum += windowed[i] * windowed[i + k];
        }

        r[k] = sum;
    }

    if (r[0] < 1e-12)
    {
        return false;
    }

    r[0] *= 1.000001;

    a.assign(order + 1, 0.0);
    a[0] = 1.0;

    vector<double> next(order + 1);
    double error = r[0];

    for (size_t i = 1; i <= order; i++)
    {
        double acc = r[i];

        for (size_t j = 1; j < i; j++)
        {
            acc += a[j] * r[i - j];
        }

        const double k = -acc / error;

        next = a;

        for (size_t j = 1; j < i; j++)
        {
            next[j] = a[j] + k * a[i - j];
        }

        next[i] = k;
        a = next;

        error *= 1.0 - k * k;

        if (error <= r[0] * 1e-12)
        {
            break;
        }
    }

    return true;
}

// Уточнение значений серии авторегрессионной интерполяцией (метод Янссена):
// неизвестные отсчёты подбираются так, чтобы минимизировать ошибку предсказания,
// после каждой итерации значения ограничиваются снизу уровнем среза
static bool refineWithAutoregression(
    vector<double>& y,
    size_t first,
    size_t last,
    size_t order,
    const vector<double>& lowerBound,
    double sign,
    double maxFactor,
    int iterations)
{
    const size_t gap = last - first + 1;

    if (first < order || last + order >= y.size())
    {
        return false;
    }

    vector<double> a;
    vector<double> autocorrelation(order + 1);
    vector<double> known(gap + order);
    vector<double> matrix(gap * gap);
    vector<double> rhs(gap);

    for (int iteration = 0; iteration < iterations; iteration++)
    {
        if (!estimateAutoregression(y, order, a))
        {
            return false;
        }

        for (size_t d = 0; d <= order; d++)
        {
            double sum = 0.0;

            for (size_t i = 0; i + d <= order; i++)
            {
                sum += a[i] * a[i + d];
            }

            autocorrelation[d] = sum;
        }

        // Вклад известных отсчётов в ошибку предсказания
        for (size_t n = first; n <= last + order; n++)
        {
            double sum = 0.0;

            for (size_t k = 0; k <= order; k++)
            {
                const size_t m = n - k;

                if (m >= first && m <= last)
                {
                    continue;
                }

                sum += a[k] * y[m];
            }

            known[n - first] = sum;
        }

        for (size_t j = 0; j < gap; j++)
        {
            double sum = 0.0;

            for (size_t i = 0; i <= order; i++)
            {
                sum += a[i] * known[j + i];
            }

            rhs[j] = -sum;

            for (size_t k = 0; k < gap; k++)
            {
                const size_t d = j > k ? j - k : k - j;

                matrix[j * gap + k] =
                    d <= order ? autocorrelation[d] : 0.0;
            }

            matrix[j * gap + j] += 1e-9;
        }

        if (!solveSymmetricSystem(matrix, rhs, gap))
        {
            return false;
        }

        for (size_t j = 0; j < gap; j++)
        {
            double magnitude = sign * rhs[j];

            magnitude = min(
                max(magnitude, lowerBound[j]),
                lowerBound[j] * maxFactor
            );

            y[first + j] = sign * magnitude;
        }
    }

    return true;
}

// Начальное приближение: кубическая интерполяция Эрмита по несрезанным соседям
static void hermiteFill(
    vector<float>& signal,
    size_t first,
    size_t last,
    double sign,
    const vector<double>& lowerBound,
    double maxFactor)
{
    const double span = static_cast<double>(last - first + 2);

    const double y0 = signal[first - 1];
    const double y1 = signal[last + 1];

    const double m0 =
        (static_cast<double>(signal[first - 1]) -
            static_cast<double>(signal[first - 2])) * span;

    const double m1 =
        (static_cast<double>(signal[last + 2]) -
            static_cast<double>(signal[last + 1])) * span;

    for (size_t i = first; i <= last; i++)
    {
        const double t = static_cast<double>(i - (first - 1)) / span;
        const double t2 = t * t;
        const double t3 = t2 * t;

        const double restored =
            (2.0 * t3 - 3.0 * t2 + 1.0) * y0 +
            (t3 - 2.0 * t2 + t) * m0 +
            (-2.0 * t3 + 3.0 * t2) * y1 +
            (t3 - t2) * m1;

        const double lower = lowerBound[i - first];

        const double magnitude = min(
            max(sign * restored, lower),
            lower * maxFactor
        );

        signal[i] = static_cast<float>(sign * magnitude);
    }
}

// Восстановление срезанных пиков.
// Срез ищется как серия подряд идущих отсчётов на уровне локального потолка
// (максимум канала в окне 1 с, отдельно для положительных и отрицательных пиков),
// значения серии восстанавливаются авторегрессионной интерполяцией.
DeclipResult declipAudio(
    AudioData& audio,
    size_t minRunLength,
    size_t maxRunLength,
    const ProgressCallback& progress,
    const AudioData* repeatedComponents,
    const AudioData* residualAudio,
    const RepetitionResult* repetitions)
{
    DeclipResult result;

    if (audio.channels == 0 ||
        audio.sampleRate == 0 ||
        audio.frameCount < 8 ||
        audio.samples.size() != audio.frameCount * audio.channels)
    {
        return result;
    }

    // Восстановленный пик не может превышать уровень среза более чем в 2 раза (+6 дБ)
    const double MAX_RESTORE_FACTOR = 2.0;

    // Допуск при сравнении отсчёта с потолком (0.1 %)
    const double CLIP_TOLERANCE = 0.001;

    // Каналы с пиками ниже этого значения на срезы не проверяются
    const double MIN_CEILING = 0.1;

    // Серии, где восстановленный пик выше уровня среза менее чем на 1 %,
    // не отличаются от обычной вершины и не изменяются
    const double MIN_OVERSHOOT = 1.01;

    const size_t AR_ORDER = 32;
    const size_t AR_CONTEXT = 256;
    const int AR_ITERATIONS = 3;

    const size_t frames = static_cast<size_t>(audio.frameCount);
    const size_t channels = audio.channels;
    const size_t blockSize = audio.sampleRate;
    const size_t blockCount = (frames + blockSize - 1) / blockSize;

    // Frames belonging to a repeated component with a clean reference are
    // excluded from the legacy declipper. The component itself is either kept
    // byte-for-byte or reconstructed from a clean occurrence.
    vector<uint8_t> protectedFrames(frames, 0);

    const bool hasRepetitionData =
        repeatedComponents != nullptr &&
        residualAudio != nullptr &&
        repetitions != nullptr &&
        repeatedComponents->channels == audio.channels &&
        residualAudio->channels == audio.channels &&
        repeatedComponents->sampleRate == audio.sampleRate &&
        residualAudio->sampleRate == audio.sampleRate &&
        repeatedComponents->frameCount == audio.frameCount &&
        residualAudio->frameCount == audio.frameCount &&
        repeatedComponents->samples.size() == audio.samples.size() &&
        residualAudio->samples.size() == audio.samples.size();

    if (hasRepetitionData && !repetitions->groupDetails.empty())
    {
        vector<vector<double>> positiveLimits(
            channels, vector<double>(blockCount, 0.0));
        vector<vector<double>> negativeLimits(
            channels, vector<double>(blockCount, 0.0));
        // Use the same one-second local ceilings as the existing declipper.
        // Clipping is assessed on the isolated repeated component, not on the
        // complete mix: another sound may be responsible for a clipped mix peak.
        for (size_t ch = 0; ch < channels; ++ch)
        {
            for (size_t f = 0; f < frames; ++f)
            {
                const double value = audio.samples[f * channels + ch];
                const size_t block = f / blockSize;
                positiveLimits[ch][block] =
                    max(positiveLimits[ch][block], value);
                negativeLimits[ch][block] =
                    min(negativeLimits[ch][block], value);
            }
        }

        for (const RepetitionGroup& group : repetitions->groupDetails)
        {
            if (group.length == 0 || group.occurrences.size() < 2)
                continue;

            // A reference must be free of detected clipping in the mix and
            // must not exceed the local ceiling as an isolated component.
            size_t referenceIndex = group.occurrences.size();
            for (size_t oi = 0; oi < group.occurrences.size(); ++oi)
            {
                const RepetitionOccurrence& occurrence = group.occurrences[oi];
                if (occurrence.start + group.length > frames)
                    continue;

                bool clean = true;
                for (size_t i = 0; i < group.length && clean; ++i)
                {
                    const size_t frame = occurrence.start + i;
                    for (size_t ch = 0; ch < channels; ++ch)
                    {
                        const double component = repeatedComponents->samples[
                            frame * channels + ch];
                        const size_t block = frame / blockSize;
                        const double limit = component >= 0.0
                            ? positiveLimits[ch][block]
                            : -negativeLimits[ch][block];

                        if (limit >= MIN_CEILING &&
                            abs(component) > limit * (1.0 + CLIP_TOLERANCE))
                        {
                            clean = false;
                            break;
                        }
                    }
                }

                if (clean)
                {
                    referenceIndex = oi;
                    break;
                }
            }

            // Without a clean occurrence, leave the group to the original
            // declipping algorithm unchanged.
            if (referenceIndex == group.occurrences.size())
            {
                ++result.repeatedGroupsWithoutCleanReference;
                continue;
            }

            ++result.repeatedGroupsWithCleanReference;

            const RepetitionOccurrence& reference =
                group.occurrences[referenceIndex];

            if (abs(reference.gainLeft) < 1e-12 ||
                (channels > 1 && abs(reference.gainRight) < 1e-12))
                continue;

            for (size_t oi = 0; oi < group.occurrences.size(); ++oi)
            {
                const RepetitionOccurrence& target = group.occurrences[oi];
                if (target.start + group.length > frames)
                    continue;

                const double targetGains[2] = {
                    target.gainLeft, target.gainRight
                };
                const double referenceGains[2] = {
                    reference.gainLeft, reference.gainRight
                };

                bool componentClips = false;

                for (size_t i = 0; i < group.length; ++i)
                {
                    const size_t frame = target.start + i;
                    const size_t block = frame / blockSize;

                    for (size_t ch = 0; ch < channels; ++ch)
                    {
                        const double baseGain = referenceGains[ch];
                        const double targetGain = targetGains[ch];

                        if (abs(baseGain) < 1e-12)
                            continue;

                        const double base = repeatedComponents->samples[
                            (reference.start + i) * channels + ch] / baseGain;
                        const double predicted = base * targetGain;
                        const double limit = predicted >= 0.0
                            ? positiveLimits[ch][block]
                            : -negativeLimits[ch][block];

                        if (limit >= MIN_CEILING &&
                            abs(predicted) > limit * (1.0 + CLIP_TOLERANCE))
                        {
                            componentClips = true;
                        }
                    }
                }

                if (componentClips)
                {
                    ++result.repeatedOccurrencesReconstructed;
                    // Rebuild only the repeated component. The residual
                    // preserves every other sound at this time position.
                    for (size_t i = 0; i < group.length; ++i)
                    {
                        const size_t frame = target.start + i;
                        for (size_t ch = 0; ch < channels; ++ch)
                        {
                            const double base = repeatedComponents->samples[
                                (reference.start + i) * channels + ch] /
                                referenceGains[ch];
                            const double reconstructed =
                                base * targetGains[ch];
                            const size_t index = frame * channels + ch;

                            audio.samples[index] =
                                static_cast<float>(
                                    residualAudio->samples[index] + reconstructed);
                        }
                    }

                    // Record the clipped portions for the existing report/graph.
                    for (size_t ch = 0; ch < channels; ++ch)
                    {
                        size_t i = 0;
                        while (i < group.length)
                        {
                            const size_t frame = target.start + i;
                            const double base = repeatedComponents->samples[
                                (reference.start + i) * channels + ch] /
                                referenceGains[ch];
                            const double predicted = base * targetGains[ch];
                            const size_t block = frame / blockSize;
                            const double limit = predicted >= 0.0
                                ? positiveLimits[ch][block]
                                : -negativeLimits[ch][block];
                            const bool clipped = limit >= MIN_CEILING &&
                                abs(predicted) > limit * (1.0 + CLIP_TOLERANCE);

                            if (!clipped)
                            {
                                ++i;
                                continue;
                            }

                            const size_t first = i;
                            while (i < group.length)
                            {
                                const size_t f = target.start + i;
                                const double b = repeatedComponents->samples[
                                    (reference.start + i) * channels + ch] /
                                    referenceGains[ch];
                                const double p = b * targetGains[ch];
                                const size_t bl = f / blockSize;
                                const double lim = p >= 0.0
                                    ? positiveLimits[ch][bl]
                                    : -negativeLimits[ch][bl];

                                if (!(lim >= MIN_CEILING &&
                                    abs(p) > lim * (1.0 + CLIP_TOLERANCE)))
                                    break;
                                ++i;
                            }

                            DeclipSegment segment;
                            segment.channel = ch;
                            segment.first = target.start + first;
                            segment.length = i - first;
                            result.segments.push_back(segment);
                            result.runs++;
                            result.samples += segment.length;
                            result.repeatedRuns++;
                            result.repeatedSamples += segment.length;
                        }
                    }
                }

                // Protect the entire occurrence when a clean reference exists:
                // a non-clipping repeat must remain untouched.
                ++result.repeatedOccurrencesProtected;
                fill(
                    protectedFrames.begin() + target.start,
                    protectedFrames.begin() + target.start + group.length,
                    1
                );
            }
        }
    }

    auto reportChannelProgress = [&](size_t channel, int localProgress)
        {
            if (progress)
            {
                const int value = static_cast<int>(
                    100.0 * (channel + localProgress / 100.0) / channels);

                progress(value);
            }
        };

    if (progress)
    {
        progress(0);
    }

    struct Run
    {
        size_t first;
        size_t length;
        double sign;
    };

    vector<float> signal(frames);
    vector<Run> runs;

    vector<float> positiveCeiling;
    vector<float> negativeCeiling;

    vector<double> lowerBound;
    vector<float> saved;
    vector<double> local;

    for (size_t channel = 0; channel < channels; channel++)
    {
        for (size_t frame = 0; frame < frames; frame++)
        {
            signal[frame] = audio.samples[frame * channels + channel];

            if ((frame & 16383) == 0)
            {
                reportChannelProgress(
                    channel,
                    static_cast<int>(5.0 * frame / frames));
            }
        }

        // Локальные потолки: максимум и минимум канала в каждом блоке
        positiveCeiling.assign(blockCount, 0.0f);
        negativeCeiling.assign(blockCount, 0.0f);

        for (size_t frame = 0; frame < frames; frame++)
        {
            const size_t block = frame / blockSize;

            positiveCeiling[block] = max(positiveCeiling[block], signal[frame]);
            negativeCeiling[block] = min(negativeCeiling[block], signal[frame]);

            if ((frame & 16383) == 0)
            {
                reportChannelProgress(
                    channel,
                    5 + static_cast<int>(20.0 * frame / frames));
            }
        }

        // Поиск серий отсчётов на потолке
        runs.clear();

        size_t frame = 0;

        while (frame < frames)
        {
            if (protectedFrames[frame])
            {
                ++frame;
                continue;
            }

            const size_t block = frame / blockSize;
            const double value = signal[frame];

            double sign = 0.0;

            if (value > 0.0 &&
                positiveCeiling[block] >= MIN_CEILING &&
                value >= positiveCeiling[block] * (1.0 - CLIP_TOLERANCE))
            {
                sign = 1.0;
            }
            else if (value < 0.0 &&
                negativeCeiling[block] <= -MIN_CEILING &&
                value <= negativeCeiling[block] * (1.0 - CLIP_TOLERANCE))
            {
                sign = -1.0;
            }

            if (sign == 0.0)
            {
                frame++;
                continue;
            }

            const size_t first = frame;

            size_t nextProgressFrame = 0;

            while (frame < frames)
            {
                if (protectedFrames[frame])
                    break;

                if (frame >= nextProgressFrame)
                {
                    reportChannelProgress(
                        channel,
                        25 + static_cast<int>(30.0 * frame / frames));

                    nextProgressFrame = frame + 16384;
                }

                const size_t b = frame / blockSize;
                const double v = signal[frame];

                const bool onCeiling = sign > 0.0
                    ? (v > 0.0 &&
                        positiveCeiling[b] >= MIN_CEILING &&
                        v >= positiveCeiling[b] * (1.0 - CLIP_TOLERANCE))
                    : (v < 0.0 &&
                        negativeCeiling[b] <= -MIN_CEILING &&
                        v <= negativeCeiling[b] * (1.0 - CLIP_TOLERANCE));

                if (!onCeiling)
                {
                    break;
                }

                frame++;
            }

            const size_t length = frame - first;

            if (length >= minRunLength)
            {
                Run run;
                run.first = first;
                run.length = length;
                run.sign = sign;

                runs.push_back(run);
            }
        }

        // Восстановление найденных серий
        result.legacyDetectedRuns += runs.size();

        for (size_t runIndex = 0; runIndex < runs.size(); runIndex++)
        {
            const Run& run = runs[runIndex];

            reportChannelProgress(
                channel,
                55 + static_cast<int>(
                    35.0 * runIndex / max<size_t>(runs.size(), 1)));
            const size_t first = run.first;
            const size_t last = run.first + run.length - 1;

            if (run.length > maxRunLength ||
                first < 2 ||
                last + 2 >= frames)
            {
                result.skipped++;
                result.legacySkippedRuns++;
                continue;
            }

            lowerBound.resize(run.length);
            saved.resize(run.length);

            for (size_t k = 0; k < run.length; k++)
            {
                saved[k] = signal[first + k];
                lowerBound[k] = abs(static_cast<double>(saved[k]));
            }

            // Начальное приближение
            hermiteFill(
                signal, first, last,
                run.sign, lowerBound, MAX_RESTORE_FACTOR
            );

            // Уточнение авторегрессионной интерполяцией
            const size_t from = first > AR_CONTEXT ? first - AR_CONTEXT : 0;
            const size_t to = min(frames, last + 1 + AR_CONTEXT);

            const size_t localFirst = first - from;
            const size_t localLast = last - from;

            if (localFirst >= AR_ORDER &&
                (to - from - 1) - localLast >= AR_ORDER)
            {
                local.assign(signal.begin() + from, signal.begin() + to);

                if (refineWithAutoregression(
                    local, localFirst, localLast, AR_ORDER,
                    lowerBound, run.sign, MAX_RESTORE_FACTOR,
                    AR_ITERATIONS))
                {
                    bool valid = true;

                    for (size_t k = 0; k < run.length; k++)
                    {
                        if (!isfinite(local[localFirst + k]))
                        {
                            valid = false;
                            break;
                        }
                    }

                    if (valid)
                    {
                        for (size_t k = 0; k < run.length; k++)
                        {
                            signal[first + k] =
                                static_cast<float>(local[localFirst + k]);
                        }
                    }
                }
            }

            // Серия без заметного превышения уровня среза не изменяется
            double overshoot = 0.0;

            for (size_t k = 0; k < run.length; k++)
            {
                overshoot = max(
                    overshoot,
                    abs(static_cast<double>(signal[first + k])) /
                    max(lowerBound[k], 1e-9)
                );
            }

            if (overshoot < MIN_OVERSHOOT)
            {
                for (size_t k = 0; k < run.length; k++)
                {
                    signal[first + k] = saved[k];
                }

                result.legacyOvershootRejectedRuns++;
                continue;
            }

            DeclipSegment segment;
            segment.channel = channel;
            segment.first = first;
            segment.length = run.length;

            result.segments.push_back(segment);

            result.runs++;
            result.samples += run.length;
            result.legacyRestoredRuns++;
        }

        for (size_t f = 0; f < frames; f++)
        {
            audio.samples[f * channels + channel] = signal[f];

            if ((f & 16383) == 0)
            {
                reportChannelProgress(
                    channel,
                    90 + static_cast<int>(10.0 * f / frames));
            }
        }
    }

    if (progress)
    {
        progress(100);
    }

    return result;
}

// ---------- Локальное снижение громкости вокруг пиков ----------

// Для каждого кадра выше потолка требуется усиление ceiling / peak (< 1).
// Кривая усиления строится как у лимитера с упреждением: скользящий минимум
// требуемого усиления по окну +-half, затем сглаживание тремя проходами
// скользящего среднего, общая полуширина которого не больше half.
// Поэтому усиление в каждой точке не выше требуемого (пик не выходит за потолок),
// а его изменения плавные. Каналы обрабатываются вместе (по максимуму),
// чтобы не нарушать стереобаланс.
PeakReductionPlan planPeakReduction(
    const AudioData& audio,
    double ceilingDb,
    double fadeMs,
    size_t graphColumns,
    const ProgressCallback& progress,
    const AudioData* fixedComponents)
{
    PeakReductionPlan plan;

    if (graphColumns > 0)
    {
        plan.columnGainDb.assign(graphColumns, 0.0f);
    }

    if (audio.channels == 0 ||
        audio.sampleRate == 0 ||
        audio.frameCount == 0 ||
        audio.samples.size() != audio.frameCount * audio.channels)
    {
        return plan;
    }

    const size_t channels = audio.channels;
    const size_t frames = static_cast<size_t>(audio.frameCount);

    if (progress)
    {
        progress(0);
    }

    const double ceiling = pow(10.0, ceilingDb / 20.0);

    size_t half = static_cast<size_t>(
        llround(max(fadeMs, 0.0) * 0.001 * audio.sampleRate));

    half = max<size_t>(half, 3);

    const size_t smoothRadius = half / 3;

    // Наибольшее расстояние, на которое пик влияет на усиление
    const size_t reach = half + 3 * smoothRadius;

    const bool hasFixedComponents = fixedComponents != nullptr &&
        fixedComponents->channels == audio.channels &&
        fixedComponents->sampleRate == audio.sampleRate &&
        fixedComponents->frameCount == audio.frameCount &&
        fixedComponents->samples.size() == audio.samples.size();

    auto peakAt = [&](size_t frame)
        {
            float peak = 0.0f;

            for (size_t channel = 0; channel < channels; channel++)
            {
                const size_t index = frame * channels + channel;
                double value = audio.samples[index];

                if (hasFixedComponents)
                    value += fixedComponents->samples[index];

                peak = max(peak, static_cast<float>(abs(value)));
            }

            return static_cast<double>(peak);
        };

    auto needAt = [&](size_t frame)
        {
            float required = 1.0f;

            for (size_t channel = 0; channel < channels; channel++)
            {
                const size_t index = frame * channels + channel;
                const double residual = audio.samples[index];
                const double fixed = hasFixedComponents
                    ? fixedComponents->samples[index]
                    : 0.0;
                const double mixed = residual + fixed;

                if (abs(mixed) <= ceiling ||
                    residual * mixed <= 0.0 ||
                    abs(residual) < 1e-12)
                    continue;

                const double direction = mixed > 0.0 ? 1.0 : -1.0;
                const double residualContribution = direction * residual;
                if (residualContribution <= 0.0)
                    continue;

                // If the fixed repeated component alone reaches the ceiling,
                // attenuating the residual cannot solve the peak; group gain
                // control handles that case later.
                if (direction * fixed >= ceiling)
                    continue;

                const double allowedResidual = ceiling - direction * fixed;
                const double gain = min(1.0,
                    max(0.0, allowedResidual / residualContribution));
                required = min(required, static_cast<float>(gain));
            }

            return required;
        };

    vector<size_t> over;

    for (size_t frame = 0; frame < frames; frame++)
    {
        if (peakAt(frame) > ceiling)
        {
            over.push_back(frame);
        }

        if ((frame & 16383) == 0 && progress)
        {
            progress(static_cast<int>(30.0 * frame / frames));
        }
    }

    if (progress)
    {
        progress(30);
    }

    size_t i = 0;

    while (i < over.size())
    {
        // Близкие пики объединяются в один участок
        size_t j = i;

        while (j + 1 < over.size() &&
            over[j + 1] - over[j] <= 2 * reach + 1)
        {
            j++;
        }

        const size_t start = over[i] > reach ? over[i] - reach : 0;
        const size_t end = min(frames - 1, over[j] + reach);
        const size_t length = end - start + 1;

        i = j + 1;

        vector<float> hold(length);
        vector<float> temp(length);

        // Скользящий минимум требуемого усиления по окну +-half
        deque<pair<size_t, float>> window;
        size_t next = 0;

        for (size_t k = 0; k < length; k++)
        {
            const size_t high = min(length - 1, k + half);
            const size_t low = k > half ? k - half : 0;

            while (next <= high)
            {
                const float value = needAt(start + next);

                while (!window.empty() && window.back().second >= value)
                {
                    window.pop_back();
                }

                window.push_back({ next, value });
                next++;
            }

            while (window.front().first < low)
            {
                window.pop_front();
            }

            hold[k] = window.front().second;
        }

        // Скользящее среднее (на границах значение крайнего отсчёта повторяется)
        auto boxPass = [&](const vector<float>& in, vector<float>& out)
            {
                const long long n = static_cast<long long>(length);
                const long long r = static_cast<long long>(smoothRadius);

                auto at = [&](long long index)
                    {
                        return static_cast<double>(
                            in[static_cast<size_t>(min(max(index, 0LL), n - 1))]);
                    };

                double sum = 0.0;

                for (long long t = -r; t <= r; t++)
                {
                    sum += at(t);
                }

                const double width = static_cast<double>(2 * r + 1);

                for (long long k = 0; k < n; k++)
                {
                    out[static_cast<size_t>(k)] = static_cast<float>(sum / width);
                    sum += at(k + 1 + r) - at(k - r);
                }
            };

        boxPass(hold, temp);
        boxPass(temp, hold);
        boxPass(hold, temp);

        // Итоговое усиление; страховка от ошибок округления
        for (size_t k = 0; k < length; k++)
        {
            float gain = min(min(temp[k], needAt(start + k)), 1.0f);

            if (!(gain > 0.0f))
            {
                gain = 1e-4f;
            }

            temp[k] = gain;

            if (gain < 1.0f)
            {
                const double db = 20.0 * log10(static_cast<double>(gain));

                plan.affectedFrames++;
                plan.maxReductionDb = min(plan.maxReductionDb, db);

                if (graphColumns > 0)
                {
                    const size_t column = min(
                        graphColumns - 1,
                        (start + k) * graphColumns / frames);

                    plan.columnGainDb[column] = min(
                        plan.columnGainDb[column],
                        static_cast<float>(db));
                }
            }
        }

        PeakReductionPlan::Region region;
        region.start = start;
        region.gain = move(temp);

        plan.regions.push_back(move(region));

        if (progress)
        {
            progress(30 + static_cast<int>(
                70.0 * i / max<size_t>(over.size(), 1)));
        }
    }

    progress(100);

    return plan;
}

// Применение плана: усиление умножается на все каналы кадра одинаково
void applyPeakReduction(
    AudioData& audio,
    const PeakReductionPlan& plan,
    const ProgressCallback& progress)
{
    const size_t channels = audio.channels;

    size_t processedFrames = 0;
    size_t totalFrames = 0;

    for (const PeakReductionPlan::Region& region : plan.regions)
    {
        totalFrames += region.gain.size();
    }

    totalFrames = max<size_t>(totalFrames, 1);

    if (progress)
    {
        progress(0);
    }

    for (const PeakReductionPlan::Region& region : plan.regions)
    {
        for (size_t k = 0; k < region.gain.size(); k++)
        {
            const float gain = region.gain[k];

            if (gain >= 1.0f)
            {
                continue;
            }

            const size_t frame = static_cast<size_t>(region.start) + k;

            if (frame >= audio.frameCount)
            {
                break;
            }

            processedFrames++;

            if ((processedFrames & 16383) == 0 && progress)
            {
                progress(static_cast<int>(
                    100.0 * processedFrames / totalFrames));
            }

            for (size_t channel = 0; channel < channels; channel++)
            {
                audio.samples[frame * channels + channel] *= gain;
            }
        }
    }

    if (progress)
    {
        progress(100);
    }
}

// ---------- Многополосное снижение громкости ----------

// Подпись полосы для графиков и сообщений
string getBandLabel(size_t band)
{
    auto format = [](double hz)
        {
            ostringstream text;

            text << defaultfloat << setprecision(3);

            if (hz >= 1000.0)
            {
                text << hz / 1000.0 << "k";
            }
            else
            {
                text << hz;
            }

            return text.str();
        };

    if (band == 0)
    {
        return "<" + format(BAND_EDGES_HZ[0]);
    }

    if (band >= BAND_COUNT - 1)
    {
        return ">" + format(BAND_EDGES_HZ[BAND_COUNT - 2]);
    }

    return format(BAND_EDGES_HZ[band - 1]) + "-" + format(BAND_EDGES_HZ[band]);
}

// Фильтр нижних частот с нулевой фазой: фильтр Баттерворта 2-го порядка,
// проходящий сигнал вперёд и назад. АЧХ равна квадрату АЧХ фильтра
// (-6 дБ на частоте среза), фазового сдвига нет.

struct LowpassCoefficients
{
    double b0;
    double b1;
    double b2;
    double a1;
    double a2;
};

static LowpassCoefficients makeLowpassCoefficients(
    double cutoffHz,
    double sampleRate)
{
    const double w0 =
        2.0 * 3.14159265358979323846 * cutoffHz / sampleRate;

    const double cosW = cos(w0);
    const double alpha =
        sin(w0) / (2.0 * 0.70710678118654752440);

    const double a0 = 1.0 + alpha;

    LowpassCoefficients c;

    c.b0 = (1.0 - cosW) * 0.5 / a0;
    c.b1 = (1.0 - cosW) / a0;
    c.b2 = c.b0;
    c.a1 = -2.0 * cosW / a0;
    c.a2 = (1.0 - alpha) / a0;

    return c;
}

static void applyZeroPhaseLowpass(
    vector<double>& data,
    const LowpassCoefficients& c)
{
    const size_t n = data.size();

    for (int direction = 0; direction < 2; direction++)
    {
        double z1 = 0.0;
        double z2 = 0.0;

        for (size_t i = 0; i < n; i++)
        {
            const size_t k =
                direction == 0 ? i : n - 1 - i;

            const double x = data[k];
            const double y = c.b0 * x + z1;

            z1 = c.b1 * x - c.a1 * y + z2;
            z2 = c.b2 * x - c.a2 * y;

            data[k] = y;
        }
    }
}


// Плавная кривая усиления с look-ahead Attack и экспоненциальным Release.
// Состояние огибающей передаётся между блоками, чтобы не требовалось
// повторно обрабатывать длинный Release-хвост в каждом блоке.
static vector<float> smoothGainCurve(
    const vector<float>& need,
    size_t attackSamples,
    size_t releaseSamples,
    size_t outputLength,
    size_t committedFrames,
    double& committedEnvelope)
{
    const size_t length = need.size();
    outputLength = min(outputLength, length);
    committedFrames = min(committedFrames, outputLength);

    vector<float> result(outputLength, 1.0f);

    if (length == 0 || outputLength == 0)
    {
        return result;
    }

    attackSamples = max<size_t>(attackSamples, 1);
    releaseSamples = max<size_t>(releaseSamples, 1);

    // Look-ahead рассчитывается только для тех отсчётов, которые будут
    // использованы текущим блоком или следующим проходом алгоритма.
    vector<float> lookAhead(outputLength);
    deque<pair<size_t, float>> window;
    size_t next = 0;

    for (size_t k = 0; k < outputLength; k++)
    {
        const size_t high = min(length - 1, k + attackSamples);

        while (next <= high)
        {
            const float value = need[next];

            while (!window.empty() && window.back().second >= value)
            {
                window.pop_back();
            }

            window.push_back({ next, value });
            next++;
        }

        while (!window.empty() && window.front().first < k)
        {
            window.pop_front();
        }

        lookAhead[k] = window.front().second;
    }

    // Состояние огибающей передаётся между блоками. Поэтому длинный Release
    // больше не требует многосекундного перекрытия соседних блоков.
    const double attackAlpha = 1.0 - exp(-3.0 / attackSamples);
    const double releaseAlpha = 1.0 - exp(-3.0 / releaseSamples);

    double envelope = committedEnvelope;

    for (size_t k = 0; k < outputLength; k++)
    {
        const double target = lookAhead[k];
        const double alpha = target < envelope
            ? attackAlpha
            : releaseAlpha;

        envelope += alpha * (target - envelope);
        envelope = min(envelope, static_cast<double>(need[k]));
        envelope = max(envelope, 0.0);

        result[k] = static_cast<float>(envelope);

        // Сохраняем состояние именно на границе полезной части блока.
        // Дальнейшие отсчёты нужны только как look-ahead для следующих проходов.
        if (k + 1 == committedFrames)
        {
            committedEnvelope = envelope;
        }
    }

    return result;
}

// Геометрический центр частотной полосы. Крайние полосы ограничены нулём
// и частотой Найквиста соответственно.
static double getBandCenterFrequency(size_t band, double sampleRate)
{
    if (band == 0)
    {
        return BAND_EDGES_HZ[0] / sqrt(2.0);
    }

    const double low = BAND_EDGES_HZ[band - 1];
    const double high = band + 1 < BAND_COUNT
        ? BAND_EDGES_HZ[band]
        : max(low, sampleRate * 0.5);

    return sqrt(low * high);
}

// Release обратно пропорционален центральной частоте полосы.
// Ограничивается только минимальное время восстановления (10 мс); верхнего
// ограничения нет, поэтому низкие частоты могут восстанавливаться дольше.
static size_t getBandReleaseSamples(
    size_t band,
    double sampleRate,
    double referenceReleaseMs)
{
    constexpr double REFERENCE_FREQUENCY_HZ = 1000.0;
    constexpr double MIN_RELEASE_TIME_MS = 10.0;
    constexpr double MAX_RELEASE_TIME_MS = 1000.0;

    const double centerFrequency = max(
        getBandCenterFrequency(band, sampleRate), 1.0);

    const double calculatedReleaseMs =
        max(0.1, referenceReleaseMs) *
        (REFERENCE_FREQUENCY_HZ / centerFrequency);

    // Ограничиваем длительность восстановления, чтобы низкие частоты
    // не оставались ослабленными на протяжении нескольких секунд.
    const double releaseMs = min(
        max(calculatedReleaseMs, MIN_RELEASE_TIME_MS),
        MAX_RELEASE_TIME_MS);

    return max<size_t>(
        1,
        static_cast<size_t>(llround(releaseMs * 0.001 * sampleRate)));
}

// Сглаживание коэффициентов между соседними частотными полосами.
// Усиление переводится в dB, после чего применяется симметричное ядро 1-2-1.
// Ограничение min(result, исходное значение) гарантирует, что сглаживание
// не ослабит уже рассчитанное снижение и не вернёт опасный пик.
static void smoothGainsAcrossBands(vector<float>& gains, size_t length)
{
    if (BAND_COUNT < 2 || length == 0)
        return;

    constexpr int RADIUS = 12;
    constexpr double SIGMA = 4.0;

    static const array<double, RADIUS * 2 + 1> weights = []()
        {
            array<double, RADIUS * 2 + 1> result{};
            double sum = 0.0;

            for (int i = -RADIUS; i <= RADIUS; i++)
            {
                const double weight = exp(
                    -0.5 * i * i / (SIGMA * SIGMA));

                result[i + RADIUS] = weight;
                sum += weight;
            }

            for (double& weight : result)
                weight /= sum;

            return result;
        }();

    vector<float> originalDb(BAND_COUNT);
    vector<float> currentDb(BAND_COUNT);
    vector<float> nextDb(BAND_COUNT);

    for (size_t i = 0; i < length; i++)
    {
        for (size_t b = 0; b < BAND_COUNT; b++)
        {
            const float value = max(gains[b * length + i], 1e-6f);

            originalDb[b] = 20.0f * log10(value);
            currentDb[b] = originalDb[b];
        }

        for (size_t b = 0; b < BAND_COUNT; b++)
        {
            double average = 0.0;

            for (int offset = -RADIUS; offset <= RADIUS; offset++)
            {
                const int neighbor = clamp(
                    static_cast<int>(b) + offset,
                    0,
                    static_cast<int>(BAND_COUNT) - 1);

                average += weights[offset + RADIUS] *
                    currentDb[neighbor];
            }

            // Не допускаем ослабления требуемой коррекции.
            nextDb[b] = min(
                originalDb[b],
                static_cast<float>(average));
        }

        for (size_t b = 0; b < BAND_COUNT; b++)
        {
            gains[b * length + i] =
                pow(10.0f, nextDb[b] / 20.0f);
        }
    }
}

// Расчёт многополосного снижения громкости.
//
// Обработка идёт блоками. Для IIR-фильтров используется короткий контекст,
// а состояние Attack/Release переносится между блоками. Три прохода алгоритма
// используют look-ahead до трёх Attack-интервалов, но не создают перекрытие
// размером в несколько Release-интервалов. Для каждого блока:
// 1. Сигнал делится на полосы: каждая полоса получается вычитанием из остатка
//    сигнала, отфильтрованного фильтром с нулевой фазой, поэтому сумма полос
//    в точности равна исходному сигналу.
// 2. В каждом отсчёте выше потолка определяется, какие полосы создали пик
//    (вклад с тем же знаком, что и пик). Требуемое снижение распределяется между
//    ними пропорционально кубу вклада: полоса, создавшая пик, снижается заметно
//    сильнее случайно совпавшей с ним по фазе.
// 3. Требуемое усиление каждой полосы сглаживается во времени (как у лимитера
//    с упреждением), поэтому полоса стихает и возвращается плавно.
// 4. Шаги 2-3 повторяются до трёх раз по уже сниженному сигналу.
// Снижение каждой полосы ограничено 18 дБ. Остаток превышения (если он есть)
// убирается отдельно небольшим общим усилением (planPeakReduction).
BandReductionPlan planBandReduction(
    const AudioData& audio,
    double ceilingDb,
    double attackMs,
    double referenceReleaseMs,
    size_t graphColumns,
    size_t chunkFrames,
    const ProgressCallback& progress,
    const AudioData* fixedComponents)
{
    BandReductionPlan plan;

    plan.maxReductionDb.assign(BAND_COUNT, 0.0);
    plan.affectedFramesByBand.assign(BAND_COUNT, 0);
    plan.framesReducedAtLeast3Db.assign(BAND_COUNT, 0);
    plan.framesReducedAtLeast6Db.assign(BAND_COUNT, 0);
    plan.framesReducedAtLeast12Db.assign(BAND_COUNT, 0);
    plan.framesAtReductionLimit.assign(BAND_COUNT, 0);
    plan.sumReductionDb.assign(BAND_COUNT, 0.0);
    plan.episodesAtLeast6Db.assign(BAND_COUNT, 0);
    plan.totalEpisodeFramesAtLeast6Db.assign(BAND_COUNT, 0);
    plan.maxEpisodeFramesAtLeast6Db.assign(BAND_COUNT, 0);
    plan.episodesAtLeast12Db.assign(BAND_COUNT, 0);
    plan.totalEpisodeFramesAtLeast12Db.assign(BAND_COUNT, 0);
    plan.maxEpisodeFramesAtLeast12Db.assign(BAND_COUNT, 0);
    plan.columnGainDb.assign(BAND_COUNT, vector<float>(graphColumns, 0.0f));

    if (audio.channels == 0 ||
        audio.sampleRate < 40000 ||
        audio.frameCount == 0 ||
        audio.samples.size() != audio.frameCount * audio.channels ||
        chunkFrames == 0)
    {
        return plan;
    }

    const size_t channels = audio.channels;
    const size_t frames = static_cast<size_t>(audio.frameCount);

    const bool hasFixedComponents = fixedComponents != nullptr &&
        fixedComponents->channels == audio.channels &&
        fixedComponents->sampleRate == audio.sampleRate &&
        fixedComponents->frameCount == audio.frameCount &&
        fixedComponents->samples.size() == audio.samples.size();

    const size_t totalChunks =
        (frames + chunkFrames - 1) / chunkFrames;

    size_t processedChunks = 0;

    if (progress)
    {
        progress(0);
    }

    const double sampleRate = audio.sampleRate;
    const double ceiling = pow(10.0, ceilingDb / 20.0);

    plan.sampleRate = sampleRate;
    plan.durationSeconds = static_cast<double>(frames) / sampleRate;

    const double safeAttackMs = clamp(attackMs, 0.1, 500.0);
    const double safeReferenceReleaseMs = clamp(referenceReleaseMs, 1.0, 2000.0);

    const size_t attackSamples = max<size_t>(
        1,
        static_cast<size_t>(llround(safeAttackMs * 0.001 * sampleRate)));

    size_t maxReleaseSamples = 1;
    size_t releaseSamples[BAND_COUNT];

    for (size_t b = 0; b < BAND_COUNT; b++)
    {
        releaseSamples[b] = getBandReleaseSamples(
            b, sampleRate, safeReferenceReleaseMs);
        maxReleaseSamples = max(maxReleaseSamples, releaseSamples[b]);
    }

    constexpr int ITERATIONS = 3;
    const double MIN_BAND_GAIN = pow(10.0, -18.0 / 20.0);

    // Короткий контекст нужен только для затухания переходных процессов IIR-фильтров.
    // Attack-look-ahead добавляется отдельно; Release переносится между блоками
    // состоянием огибающей и поэтому не требует огромного перекрытия.
    const size_t filterPad = max<size_t>(1, static_cast<size_t>(sampleRate * 0.1));
    const size_t previewLimit = attackSamples * ITERATIONS;

    // Для каждого прохода и каждой полосы сохраняется состояние Release.
    // Это позволяет обрабатывать длинный трек последовательно, не пересчитывая
    // многосекундный хвост в каждом соседнем блоке.
    double envelopeState[ITERATIONS][BAND_COUNT];

    for (int iteration = 0; iteration < ITERATIONS; iteration++)
    {
        for (size_t b = 0; b < BAND_COUNT; b++)
        {
            envelopeState[iteration][b] = 1.0;
        }
    }

    auto peakAt = [&](size_t frame)
        {
            float peak = 0.0f;

            for (size_t channel = 0; channel < channels; channel++)
            {
                const size_t index = frame * channels + channel;
                double value = audio.samples[index];

                if (hasFixedComponents)
                    value += fixedComponents->samples[index];

                peak = max(peak, static_cast<float>(abs(value)));
            }

            return static_cast<double>(peak);
        };

    double strongestPeak = 0.0;

    // Track contiguous frames above the 6 dB and 12 dB attenuation thresholds.
    // State is kept outside the chunk loop so episodes can cross chunk boundaries.
    array<uint64_t, BAND_COUNT> currentEpisode6{};
    array<uint64_t, BAND_COUNT> currentEpisode12{};

    auto finishEpisode = [](size_t band, uint64_t& length,
        vector<uint64_t>& counts,
        vector<uint64_t>& totalFrames,
        vector<uint64_t>& maximumFrames)
        {
            if (length == 0)
                return;

            counts[band]++;
            totalFrames[band] += length;
            maximumFrames[band] = max(maximumFrames[band], length);
            length = 0;
        };

    auto finishAllEpisodes = [&]()
        {
            for (size_t band = 0; band < BAND_COUNT; band++)
            {
                finishEpisode(band, currentEpisode6[band],
                    plan.episodesAtLeast6Db,
                    plan.totalEpisodeFramesAtLeast6Db,
                    plan.maxEpisodeFramesAtLeast6Db);

                finishEpisode(band, currentEpisode12[band],
                    plan.episodesAtLeast12Db,
                    plan.totalEpisodeFramesAtLeast12Db,
                    plan.maxEpisodeFramesAtLeast12Db);
            }
        };

    LowpassCoefficients lowpassCoefficients[BAND_COUNT - 1];

    for (size_t b = 0; b + 1 < BAND_COUNT; b++)
    {
        lowpassCoefficients[b] = makeLowpassCoefficients(
            BAND_EDGES_HZ[b],
            sampleRate
        );
    }

    for (size_t c0 = 0; c0 < frames; c0 += chunkFrames)
    {
        const size_t c1 = min(frames, c0 + chunkFrames);
        const size_t coreFrames = c1 - c0;

        // Три последовательных прохода могут передать влияние вперёд не более
        // чем на три Attack-интервала.
        const size_t previewFrames = min(frames - c1, previewLimit);
        const size_t workLength = coreFrames + previewFrames;
        const size_t peakScanEnd = c1 + previewFrames;

        bool stateActive = false;

        for (int iteration = 0; iteration < ITERATIONS && !stateActive; iteration++)
        {
            for (size_t b = 0; b < BAND_COUNT; b++)
            {
                if (envelopeState[iteration][b] < 0.99999)
                {
                    stateActive = true;
                    break;
                }
            }
        }

        // Если в ближайшем look-ahead нет превышений и все огибающие уже
        // восстановились, блок можно пропустить без фильтрации.
        bool any = stateActive;

        if (!any)
        {
            for (size_t f = c0; f < peakScanEnd; f++)
            {
                if (peakAt(f) > ceiling)
                {
                    any = true;
                    break;
                }
            }
        }

        if (!any)
        {
            // Незначительный остаток меньше порога фактически не влияет на выход:
            // сбрасываем его, чтобы не продолжать обработку пустых блоков.
            for (int iteration = 0; iteration < ITERATIONS; iteration++)
            {
                for (size_t b = 0; b < BAND_COUNT; b++)
                {
                    envelopeState[iteration][b] = 1.0;
                }
            }

            // This chunk contains no attenuation, so any active episode ends here.
            finishAllEpisodes();

            processedChunks++;

            if (progress)
            {
                progress(static_cast<int>(
                    100.0 * processedChunks / totalChunks));
            }

            continue;
        }

        const size_t filterStart = c0 > filterPad ? c0 - filterPad : 0;
        const size_t filterRight = min(frames - c1, previewFrames + filterPad);
        const size_t filterEnd = c1 + filterRight;
        const size_t filterLength = filterEnd - filterStart;
        const size_t workOffset = c0 - filterStart;

        // Частотные полосы хранятся только для текущего блока и небольшого
        // фильтрового контекста. Release больше не раздувает этот массив до десятков секунд.
        vector<double> bandData(channels * BAND_COUNT * filterLength);
        vector<double> rest(filterLength);
        vector<double> low(filterLength);

        auto band = [&](size_t channel, size_t b)
            {
                return bandData.data() + (channel * BAND_COUNT + b) * filterLength;
            };

        for (size_t channel = 0; channel < channels; channel++)
        {
            for (size_t i = 0; i < filterLength; i++)
            {
                rest[i] = audio.samples[(filterStart + i) * channels + channel];
            }

            for (size_t b = 0; b + 1 < BAND_COUNT; b++)
            {
                low = rest;

                applyZeroPhaseLowpass(
                    low,
                    lowpassCoefficients[b]
                );

                double* target = band(channel, b);

                for (size_t i = 0; i < filterLength; i++)
                {
                    target[i] = low[i];
                    rest[i] -= low[i];
                }
            }

            double* last = band(channel, BAND_COUNT - 1);

            for (size_t i = 0; i < filterLength; i++)
            {
                last[i] = rest[i];
            }
        }

        // gain имеет общий временной шаг workLength. На каждом проходе
        // активная область сокращается на Attack-look-ahead, пока не останется core.
        vector<float> gain(BAND_COUNT * workLength, 1.0f);
        size_t activeLength = workLength;

        for (int iteration = 0; iteration < ITERATIONS; iteration++)
        {
            vector<float> need(BAND_COUNT * activeLength, 1.0f);
            bool found = false;

            for (size_t i = 0; i < activeLength; i++)
            {
                const size_t bandIndex = workOffset + i;

                for (size_t channel = 0; channel < channels; channel++)
                {
                    double z[BAND_COUNT];
                    double y = hasFixedComponents
                        ? fixedComponents->samples[
                            (c0 + i) * channels + channel]
                        : 0.0;

                    for (size_t b = 0; b < BAND_COUNT; b++)
                    {
                        z[b] = static_cast<double>(gain[b * workLength + i]) *
                            band(channel, b)[bandIndex];

                        y += z[b];
                    }

                    const double excess = abs(y) - ceiling;

                    if (excess <= ceiling * 1e-4)
                    {
                        continue;
                    }

                    const double direction = y > 0.0 ? 1.0 : -1.0;
                    const double fixedValue = hasFixedComponents
                        ? fixedComponents->samples[
                            (c0 + i) * channels + channel]
                        : 0.0;

                    // Do not attenuate the residual to compensate for a
                    // repeated component that already exceeds the ceiling.
                    if (direction * fixedValue >= ceiling)
                        continue;

                    double contribution[BAND_COUNT];
                    double cubes = 0.0;

                    for (size_t b = 0; b < BAND_COUNT; b++)
                    {
                        contribution[b] = max(0.0, direction * z[b]);
                        cubes += contribution[b] * contribution[b] * contribution[b];
                    }

                    if (cubes < 1e-18)
                    {
                        continue;
                    }

                    for (size_t b = 0; b < BAND_COUNT; b++)
                    {
                        if (contribution[b] <= 0.0)
                        {
                            continue;
                        }

                        const double cut =
                            excess * contribution[b] * contribution[b] / cubes;

                        const double floorGain = MIN_BAND_GAIN /
                            max(static_cast<double>(gain[b * workLength + i]), 1e-6);

                        const double wanted =
                            min(1.0, max(1.0 - cut, floorGain));

                        float& required = need[b * activeLength + i];

                        if (wanted < required)
                        {
                            required = static_cast<float>(wanted);
                            found = true;
                        }
                    }
                }
            }

            if (found)
            {
                smoothGainsAcrossBands(need, activeLength);
            }

            // В следующем проходе понадобится результат только до activeLength-A.
            // В конце файла look-ahead естественно сокращается до доступных отсчётов.
            const size_t outputLength = max(
                coreFrames,
                activeLength > attackSamples
                ? activeLength - attackSamples
                : coreFrames);

            vector<float> needBand(activeLength);

            for (size_t b = 0; b < BAND_COUNT; b++)
            {
                copy(
                    need.begin() + b * activeLength,
                    need.begin() + (b + 1) * activeLength,
                    needBand.begin());

                const vector<float> curve = smoothGainCurve(
                    needBand,
                    attackSamples,
                    releaseSamples[b],
                    outputLength,
                    coreFrames,
                    envelopeState[iteration][b]);

                for (size_t i = 0; i < outputLength; i++)
                {
                    gain[b * workLength + i] = max(
                        static_cast<float>(MIN_BAND_GAIN),
                        gain[b * workLength + i] * curve[i]);
                }
            }

            activeLength = outputLength;
        }

        // После трёх проходов activeLength равен coreFrames.
        const size_t outputFrames = coreFrames;
        vector<float> out(outputFrames * channels);
        bool affected = false;

        double chunkPeak = 0.0;
        size_t chunkPeakFrame = c0;

        for (size_t f = 0; f < outputFrames; f++)
        {
            bool frameAffected = false;
            float g[BAND_COUNT];

            for (size_t b = 0; b < BAND_COUNT; b++)
            {
                g[b] = gain[b * workLength + f] < 0.99999f
                    ? gain[b * workLength + f]
                    : 1.0f;

                const double db = g[b] < 1.0f
                    ? 20.0 * log10(static_cast<double>(g[b]))
                    : 0.0;

                if (g[b] < 1.0f)
                {
                    frameAffected = true;

                    plan.maxReductionDb[b] = min(plan.maxReductionDb[b], db);
                    plan.affectedFramesByBand[b]++;
                    plan.sumReductionDb[b] += db;

                    if (db <= -3.0)
                        plan.framesReducedAtLeast3Db[b]++;

                    if (db <= -6.0)
                        plan.framesReducedAtLeast6Db[b]++;

                    if (db <= -12.0)
                        plan.framesReducedAtLeast12Db[b]++;

                    // The gain is clamped to -18 dB; allow a small
                    // floating-point tolerance when counting limit hits.
                    if (db <= -17.99)
                        plan.framesAtReductionLimit[b]++;

                    if (graphColumns > 0)
                    {
                        const size_t column = min(
                            graphColumns - 1,
                            (c0 + f) * graphColumns / frames);

                        plan.columnGainDb[b][column] = min(
                            plan.columnGainDb[b][column],
                            static_cast<float>(db));
                    }
                }

                if (db <= -6.0)
                    currentEpisode6[b]++;
                else
                    finishEpisode(b, currentEpisode6[b],
                        plan.episodesAtLeast6Db,
                        plan.totalEpisodeFramesAtLeast6Db,
                        plan.maxEpisodeFramesAtLeast6Db);

                if (db <= -12.0)
                    currentEpisode12[b]++;
                else
                    finishEpisode(b, currentEpisode12[b],
                        plan.episodesAtLeast12Db,
                        plan.totalEpisodeFramesAtLeast12Db,
                        plan.maxEpisodeFramesAtLeast12Db);
            }

            for (size_t channel = 0; channel < channels; channel++)
            {
                const float original = audio.samples[(c0 + f) * channels + channel];
                double delta = 0.0;

                for (size_t b = 0; b < BAND_COUNT; b++)
                {
                    delta += (static_cast<double>(g[b]) - 1.0) *
                        band(channel, b)[workOffset + f];
                }

                out[f * channels + channel] = delta == 0.0
                    ? original
                    : static_cast<float>(original + delta);

                const size_t mixIndex = (c0 + f) * channels + channel;
                const double fixed = hasFixedComponents
                    ? fixedComponents->samples[mixIndex]
                    : 0.0;
                const double mixedOriginal = original + fixed;

                if (abs(mixedOriginal) > chunkPeak)
                {
                    chunkPeak = abs(mixedOriginal);
                    chunkPeakFrame = c0 + f;
                }
            }

            if (frameAffected)
            {
                plan.affectedFrames++;
                affected = true;
            }
        }

        if (!affected)
        {
            processedChunks++;

            if (progress)
            {
                progress(static_cast<int>(
                    100.0 * processedChunks / totalChunks));
            }

            continue;

            continue;
        }

        if (chunkPeak > strongestPeak)
        {
            strongestPeak = chunkPeak;

            const size_t eventHalf = max<size_t>(
                attackSamples + maxReleaseSamples, 200);

            const size_t from = max(c0,
                chunkPeakFrame > eventHalf ? chunkPeakFrame - eventHalf : 0);

            const size_t to = min(c1, chunkPeakFrame + eventHalf + 1);

            plan.eventStart = from;
            plan.eventBefore.assign(to - from, 0.0f);
            plan.eventAfter.assign(to - from, 0.0f);

            for (size_t f = from; f < to; f++)
            {
                const size_t index = f * channels;
                const double fixed = hasFixedComponents
                    ? fixedComponents->samples[index]
                    : 0.0;
                plan.eventBefore[f - from] = static_cast<float>(
                    audio.samples[index] + fixed);
                plan.eventAfter[f - from] = static_cast<float>(
                    out[(f - c0) * channels] + fixed);
            }
        }

        BandReductionPlan::Chunk chunk;
        chunk.start = c0;
        chunk.samples = move(out);
        plan.chunks.push_back(move(chunk));

        processedChunks++;

        if (progress)
        {
            progress(static_cast<int>(
                100.0 * processedChunks / totalChunks));
        }
    }

    // Close episodes that remain active at the end of the final chunk.
    finishAllEpisodes();

    if (progress)
    {
        progress(100);
    }

    return plan;
}

// Применение плана многополосного снижения
void applyBandReduction(
    AudioData& audio,
    const BandReductionPlan& plan,
    const ProgressCallback& progress)
{
    const size_t channels = audio.channels;

    size_t processedChunks = 0;
    const size_t totalChunks = max<size_t>(plan.chunks.size(), 1);

    if (progress)
    {
        progress(0);
    }

    for (const BandReductionPlan::Chunk& chunk : plan.chunks)
    {
        const size_t offset = static_cast<size_t>(chunk.start) * channels;

        if (offset + chunk.samples.size() > audio.samples.size())
        {
            continue;
        }

        copy(chunk.samples.begin(), chunk.samples.end(),
            audio.samples.begin() + offset);

        processedChunks++;

        if (progress)
        {
            progress(static_cast<int>(
                100.0 * processedChunks / totalChunks));
        }
    }

    if (progress)
    {
        progress(100);
    }
}

// Форматирование времени: секунды или м:сс
static string formatTimeLabel(double seconds, bool minutes, int decimals)
{
    ostringstream text;

    if (minutes)
    {
        long long total = static_cast<long long>(llround(seconds));
        text << total / 60 << ":" << setw(2) << setfill('0') << total % 60;
    }
    else
    {
        text << fixed << setprecision(decimals) << seconds;
    }

    return text.str();
}

// График во временной области: обзор трека и увеличенный фрагмент
bool saveDeclipGraph(
    const AudioData& before,
    const AudioData& after,
    const DeclipResult& result,
    const filesystem::path& filename,
    const string& title,
    string& error)
{
    if (before.channels == 0 ||
        before.sampleRate == 0 ||
        before.frameCount == 0 ||
        before.channels != after.channels ||
        before.frameCount != after.frameCount ||
        before.samples.size() != after.samples.size() ||
        before.samples.size() != before.frameCount * before.channels ||
        result.segments.empty())
    {
        error = "Invalid declipping graph data.";
        return false;
    }

    const size_t channels = before.channels;
    const uint64_t frames = before.frameCount;
    const double sampleRate = before.sampleRate;
    const double duration = static_cast<double>(frames) / sampleRate;
    const double gain = 1.0;

    const double WIDTH = 800.0;
    const double HEIGHT = 600.0;
    const double MARGIN_LEFT = 64.0;
    const double MARGIN_RIGHT = 24.0;
    const double plotWidth = WIDTH - MARGIN_LEFT - MARGIN_RIGHT;

    // Прозрачность заливки огибающих на обзоре: чем меньше значение, тем прозрачнее
    // (восстановленный сигнал рисуется поверх исходного и не должен его закрывать)
    const double ORIGINAL_FILL_OPACITY = 0.55;
    const double RESTORED_FILL_OPACITY = 0.30;

    const double overviewTop = 72.0;
    const double overviewHeight = 190.0;
    const double stripTop = overviewTop + overviewHeight + 6.0;
    const double stripHeight = 8.0;
    const double zoomTop = 352.0;
    const double zoomHeight = 190.0;

    // ---------- Обзор трека: огибающая min/max по столбцам ----------
    const double COLUMN_WIDTH = 2.0;
    const size_t columns = static_cast<size_t>(plotWidth / COLUMN_WIDTH);

    vector<float> beforeMin(columns), beforeMax(columns);
    vector<float> afterMin(columns), afterMax(columns);
    vector<uint32_t> restoredCount(columns, 0);

    double peak = 0.0;

    for (size_t c = 0; c < columns; c++)
    {
        uint64_t from = frames * c / columns;
        uint64_t to = frames * (c + 1) / columns;

        if (from >= frames)
        {
            from = frames - 1;
        }

        if (to <= from)
        {
            to = from + 1;
        }

        if (to > frames)
        {
            to = frames;
        }

        float bMin = 0.0f, bMax = 0.0f, aMin = 0.0f, aMax = 0.0f;

        for (uint64_t f = from; f < to; f++)
        {
            for (size_t ch = 0; ch < channels; ch++)
            {
                const size_t index = static_cast<size_t>(f) * channels + ch;

                bMin = min(bMin, before.samples[index]);
                bMax = max(bMax, before.samples[index]);
                aMin = min(aMin, after.samples[index]);
                aMax = max(aMax, after.samples[index]);
            }
        }

        beforeMin[c] = bMin;
        beforeMax[c] = bMax;
        afterMin[c] = aMin;
        afterMax[c] = aMax;

        peak = max({ peak,
            static_cast<double>(abs(bMin)), static_cast<double>(abs(bMax)),
            static_cast<double>(abs(aMin)), static_cast<double>(abs(aMax)) });
    }

    for (const DeclipSegment& segment : result.segments)
    {
        size_t c = static_cast<size_t>(segment.first * columns / frames);

        restoredCount[min(c, columns - 1)]++;
    }

    const double limit = max(0.2, ceil(peak * 1.05 * 20.0) / 20.0);

    double amplitudeStep = 0.5;

    for (double candidate : { 0.05, 0.1, 0.2, 0.25, 0.5 })
    {
        if (limit / candidate <= 4.5)
        {
            amplitudeStep = candidate;
            break;
        }
    }

    auto xOverview = [&](double column)
        {
            return MARGIN_LEFT + column * COLUMN_WIDTH + COLUMN_WIDTH / 2.0;
        };

    auto yOverview = [&](double value)
        {
            return overviewTop +
                overviewHeight * (limit - value) / (2.0 * limit);
        };

    // ---------- Увеличенный фрагмент: самое заметное восстановление ----------
    size_t bestIndex = 0;
    double bestScore = -1.0;

    for (size_t i = 0; i < result.segments.size(); i++)
    {
        const DeclipSegment& segment = result.segments[i];

        double ratio = 0.0;

        for (uint64_t k = 0; k < segment.length; k++)
        {
            uint64_t frame = segment.first + k;

            if (frame >= frames || segment.channel >= channels)
            {
                break;
            }

            const size_t index =
                static_cast<size_t>(frame) * channels + segment.channel;

            double b = abs(static_cast<double>(before.samples[index]));
            double a = abs(static_cast<double>(after.samples[index])) / gain;

            if (b > 1e-9)
            {
                ratio = max(ratio, a / b);
            }
        }

        if (ratio > bestScore)
        {
            bestScore = ratio;
            bestIndex = i;
        }
    }

    const DeclipSegment best = result.segments[bestIndex];

    const uint64_t half = max<uint64_t>(24, best.length * 2 + 8);
    const uint64_t zoomStart = best.first > half ? best.first - half : 0;
    const uint64_t zoomEnd =
        min<uint64_t>(frames - 1, best.first + best.length - 1 + half);

    if (zoomEnd <= zoomStart || best.channel >= channels)
    {
        error = "Invalid declipping graph data.";
        return false;
    }

    const size_t zoomCount = static_cast<size_t>(zoomEnd - zoomStart + 1);

    auto sampleAt = [&](const AudioData& audio, uint64_t frame)
        {
            return static_cast<double>(audio.samples[
                static_cast<size_t>(frame) * channels + best.channel]);
        };

    // Восстановленный сигнал в увеличенном фрагменте показывается в исходном
    // масштабе (без общего ослабления), чтобы его можно было сравнить с оригиналом
    auto afterValue = [&](uint64_t frame)
        {
            return sampleAt(after, frame) / gain;
        };

    double zoomMin = 0.0;
    double zoomMax = 0.0;
    bool zoomInitialized = false;

    for (uint64_t f = zoomStart; f <= zoomEnd; f++)
    {
        for (double value : { sampleAt(before, f), afterValue(f) })
        {
            if (!zoomInitialized)
            {
                zoomMin = zoomMax = value;
                zoomInitialized = true;
            }

            zoomMin = min(zoomMin, value);
            zoomMax = max(zoomMax, value);
        }
    }

    double zoomPad = (zoomMax - zoomMin) * 0.1;

    if (zoomPad <= 0.0)
    {
        zoomPad = 0.05;
    }

    zoomMin -= zoomPad;
    zoomMax += zoomPad;

    auto xZoom = [&](uint64_t frame)
        {
            return MARGIN_LEFT + plotWidth *
                static_cast<double>(frame - zoomStart) /
                static_cast<double>(zoomEnd - zoomStart);
        };

    auto yZoom = [&](double value)
        {
            return zoomTop +
                zoomHeight * (zoomMax - value) / (zoomMax - zoomMin);
        };

    // Уровень среза: наибольшее по модулю значение серии до восстановления
    double clipLevel = 0.0;

    for (uint64_t k = 0; k < best.length; k++)
    {
        double value = sampleAt(before, best.first + k);

        if (abs(value) > abs(clipLevel))
        {
            clipLevel = value;
        }
    }

    // ---------- Запись SVG ----------
    ofstream file(filename);

    if (!file.is_open())
    {
        error = "Unable to create declipping graph file.";
        return false;
    }

    file << fixed << setprecision(1);

    file << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    file << "<svg xmlns=\"http://www.w3.org/2000/svg\" "
        << "viewBox=\"0 0 " << WIDTH << " " << HEIGHT << "\" "
        << "width=\"" << WIDTH << "\" height=\"" << HEIGHT << "\" "
        << "font-family=\"Arial, Helvetica, sans-serif\" font-size=\"12\">\n";

    file << "<rect width=\"100%\" height=\"100%\" fill=\"#ffffff\"/>\n";

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

                file << "<text x=\"" << legendX + 28.0 << "\" y=\""
                    << legendY + 4.0 << "\" fill=\"#444444\">"
                    << label << "</text>\n";

                legendX += width;
            };

        legendItem("#888888", "Original", 100.0, ORIGINAL_FILL_OPACITY);
        legendItem("#1f6feb", "Restored", 100.0, RESTORED_FILL_OPACITY);
        legendItem("#cc3333", "Restored regions", 160.0, 0.6);
    }

    // Подпись обзора
    {
        ostringstream caption;

        caption << fixed << setprecision(1) << "Track overview, all channels";

        caption << " | " << result.runs << " restored runs";

        file << "<text x=\"" << MARGIN_LEFT << "\" y=\"" << overviewTop - 8.0
            << "\" fill=\"#222222\" font-weight=\"bold\">"
            << escapeXml(caption.str()) << "</text>\n";
    }

    // Сетка обзора по оси амплитуды
    for (int i = -static_cast<int>(floor(limit / amplitudeStep));
        i <= static_cast<int>(floor(limit / amplitudeStep));
        i++)
    {
        double value = i * amplitudeStep;
        double y = yOverview(value);

        file << "<line x1=\"" << MARGIN_LEFT << "\" y1=\"" << y
            << "\" x2=\"" << MARGIN_LEFT + plotWidth << "\" y2=\"" << y
            << "\" stroke=\"" << (i == 0 ? "#666666" : "#dddddd") << "\"/>\n";

        file << "<text x=\"" << MARGIN_LEFT - 8.0 << "\" y=\"" << y + 4.0
            << "\" text-anchor=\"end\" fill=\"#444444\">"
            << setprecision(2) << value << setprecision(1) << "</text>\n";
    }

    // Сетка обзора по оси времени
    {
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
            double x = MARGIN_LEFT + plotWidth * time / duration;

            file << "<line x1=\"" << x << "\" y1=\"" << overviewTop
                << "\" x2=\"" << x << "\" y2=\"" << overviewTop + overviewHeight
                << "\" stroke=\"#eeeeee\"/>\n";

            file << "<text x=\"" << x << "\" y=\""
                << stripTop + stripHeight + 14.0
                << "\" text-anchor=\"middle\" fill=\"#444444\">"
                << formatTimeLabel(time, minutes, decimals) << "</text>\n";
        }

        file << "<text x=\"" << MARGIN_LEFT + plotWidth / 2.0 << "\" y=\""
            << stripTop + stripHeight + 30.0
            << "\" text-anchor=\"middle\" fill=\"#222222\">"
            << (minutes ? "Time, min:s" : "Time, s") << "</text>\n";
    }

    // Огибающая: исходная и восстановленная
    auto writeEnvelope = [&](const vector<float>& low,
        const vector<float>& high,
        const char* color,
        double opacity)
        {
            file << "<polygon fill=\"" << color
                << "\" fill-opacity=\"" << opacity << "\" stroke=\"" << color
                << "\" stroke-width=\"0.5\" points=\"";

            for (size_t c = 0; c < columns; c++)
            {
                file << xOverview(static_cast<double>(c)) << ","
                    << yOverview(high[c]) << " ";
            }

            for (size_t c = columns; c-- > 0;)
            {
                file << xOverview(static_cast<double>(c)) << ","
                    << yOverview(low[c]) << " ";
            }

            file << "\"/>\n";
        };

    writeEnvelope(beforeMin, beforeMax, "#888888", ORIGINAL_FILL_OPACITY);
    writeEnvelope(afterMin, afterMax, "#1f6feb", RESTORED_FILL_OPACITY);

    file << "<rect x=\"" << MARGIN_LEFT << "\" y=\"" << overviewTop
        << "\" width=\"" << plotWidth << "\" height=\"" << overviewHeight
        << "\" fill=\"none\" stroke=\"#888888\"/>\n";

    // Полоса: где выполнено восстановление
    file << "<path stroke=\"#cc3333\" stroke-width=\"" << COLUMN_WIDTH
        << "\" fill=\"none\" d=\"";

    for (size_t c = 0; c < columns; c++)
    {
        if (restoredCount[c] > 0)
        {
            file << "M" << xOverview(static_cast<double>(c)) << " "
                << stripTop << "v" << stripHeight;
        }
    }

    file << "\"/>\n";

    // ---------- Увеличенный фрагмент ----------
    {
        ostringstream caption;

        caption << fixed << setprecision(4)
            << "Largest restored peak: "
            << (channels == 2
                ? (best.channel == 0 ? "left" : "right")
                : "channel " + to_string(best.channel + 1))
            << " channel, at "
            << static_cast<double>(best.first) / sampleRate << " s, "
            << best.length << " clipped samples";

        file << "<text x=\"" << MARGIN_LEFT << "\" y=\"" << zoomTop - 12.0
            << "\" fill=\"#222222\" font-weight=\"bold\">"
            << escapeXml(caption.str()) << "</text>\n";
    }

    // Сетка по оси амплитуды (5 линий)
    for (int i = 0; i <= 4; i++)
    {
        double value = zoomMin + (zoomMax - zoomMin) * i / 4.0;
        double y = yZoom(value);

        file << "<line x1=\"" << MARGIN_LEFT << "\" y1=\"" << y
            << "\" x2=\"" << MARGIN_LEFT + plotWidth << "\" y2=\"" << y
            << "\" stroke=\"#dddddd\"/>\n";

        file << "<text x=\"" << MARGIN_LEFT - 8.0 << "\" y=\"" << y + 4.0
            << "\" text-anchor=\"end\" fill=\"#444444\">"
            << setprecision(3) << value << setprecision(1) << "</text>\n";
    }

    // Сетка по оси времени (5 линий)
    for (int i = 0; i <= 4; i++)
    {
        uint64_t frame = zoomStart + (zoomEnd - zoomStart) * i / 4;
        double x = xZoom(frame);

        file << "<line x1=\"" << x << "\" y1=\"" << zoomTop
            << "\" x2=\"" << x << "\" y2=\"" << zoomTop + zoomHeight
            << "\" stroke=\"#eeeeee\"/>\n";

        file << "<text x=\"" << x << "\" y=\"" << zoomTop + zoomHeight + 16.0
            << "\" text-anchor=\"middle\" fill=\"#444444\">"
            << setprecision(4) << static_cast<double>(frame) / sampleRate
            << setprecision(1) << "</text>\n";
    }

    file << "<text x=\"" << MARGIN_LEFT + plotWidth / 2.0 << "\" y=\""
        << zoomTop + zoomHeight + 34.0
        << "\" text-anchor=\"middle\" fill=\"#222222\">"
        << "Time, s (window " << setprecision(2)
        << static_cast<double>(zoomCount) / sampleRate * 1000.0
        << setprecision(1) << " ms), red dots: clipped samples</text>\n";

    // Линия уровня среза
    {
        double y = yZoom(clipLevel);

        file << "<line x1=\"" << MARGIN_LEFT << "\" y1=\"" << y
            << "\" x2=\"" << MARGIN_LEFT + plotWidth << "\" y2=\"" << y
            << "\" stroke=\"#cc3333\" stroke-width=\"1\" "
            << "stroke-dasharray=\"4,4\"/>\n";
    }

    // Кривые: исходная и восстановленная
    auto writeZoomLine = [&](auto valueAt,
        const char* color,
        double strokeWidth)
        {
            file << "<polyline fill=\"none\" stroke=\"" << color
                << "\" stroke-width=\"" << strokeWidth
                << "\" stroke-linejoin=\"round\" points=\"";

            for (uint64_t f = zoomStart; f <= zoomEnd; f++)
            {
                file << xZoom(f) << "," << yZoom(valueAt(f)) << " ";
            }

            file << "\"/>\n";
        };

    writeZoomLine(
        [&](uint64_t f) { return sampleAt(before, f); },
        "#888888", 2.0);

    writeZoomLine(afterValue, "#1f6feb", 2.5);

    // Срезанные отсчёты (до) и восстановленные значения (после)
    for (uint64_t k = 0; k < best.length; k++)
    {
        uint64_t frame = best.first + k;

        file << "<circle cx=\"" << xZoom(frame) << "\" cy=\""
            << yZoom(sampleAt(before, frame))
            << "\" r=\"3.5\" fill=\"#cc3333\"/>\n";

        file << "<circle cx=\"" << xZoom(frame) << "\" cy=\""
            << yZoom(afterValue(frame))
            << "\" r=\"3\" fill=\"#1f6feb\"/>\n";
    }

    file << "<rect x=\"" << MARGIN_LEFT << "\" y=\"" << zoomTop
        << "\" width=\"" << plotWidth << "\" height=\"" << zoomHeight
        << "\" fill=\"none\" stroke=\"#888888\"/>\n";

    file << "</svg>\n";

    file.close();

    if (file.fail())
    {
        error = "Unable to write declipping graph file.";
        return false;
    }

    return true;
}

// График многополосного снижения: усиление по полосам и сильнейшее событие

// Диагностика повторяющихся компонентов: огибающая каждой группы,
// фактическое общее снижение её уровня и усреднённый спектр.
bool saveRepeatedComponentsGraph(
    const AudioData& repeatedAudio,
    const RepetitionResult& repetitions,
    const vector<double>& groupReductionDb,
    const filesystem::path& filename,
    const string& title,
    string& error)
{
    if (repeatedAudio.channels == 0 ||
        repeatedAudio.sampleRate == 0 ||
        repeatedAudio.frameCount == 0 ||
        repeatedAudio.samples.size() !=
            repeatedAudio.frameCount * repeatedAudio.channels ||
        repetitions.groupDetails.empty() ||
        groupReductionDb.size() != repetitions.groupDetails.size())
    {
        error = "Invalid repeated-component graph data.";
        return false;
    }

    const double WIDTH = 1100.0;
    const double HEIGHT_PER_GROUP = 190.0;
    const double TOP = 76.0;
    const double HEIGHT = TOP +
        HEIGHT_PER_GROUP * repetitions.groupDetails.size() + 28.0;

    const double timeLeft = 66.0;
    const double timeWidth = 520.0;
    const double spectrumLeft = 680.0;
    const double spectrumWidth = 360.0;
    const double plotTopOffset = 42.0;
    const double plotHeight = 104.0;
    const size_t envelopeColumns = 120;

    auto groupLabel = [](size_t index)
        {
            string label;
            ++index;

            while (index > 0)
            {
                --index;
                label.insert(label.begin(),
                    static_cast<char>('A' + index % 26));
                index /= 26;
            }

            return label;
        };

    ofstream file(filename);

    if (!file.is_open())
    {
        error = "Unable to create repeated-component graph file.";
        return false;
    }

    file << fixed << setprecision(2);
    file << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    file << "<svg xmlns=\"http://www.w3.org/2000/svg\" "
        << "viewBox=\"0 0 " << WIDTH << " " << HEIGHT << "\" "
        << "width=\"" << WIDTH << "\" height=\"" << HEIGHT << "\" "
        << "font-family=\"Arial, Helvetica, sans-serif\" font-size=\"11\">\n";
    file << "<rect width=\"100%\" height=\"100%\" fill=\"#ffffff\"/>\n";
    writeGraphTitle(file, title + " | Repeated component diagnostics", WIDTH);

    file << "<text x=\"" << timeLeft << "\" y=\"58\" "
        << "font-weight=\"bold\" fill=\"#222222\">Group attenuation across full track</text>\n";
    file << "<text x=\"" << spectrumLeft << "\" y=\"58\" "
        << "font-weight=\"bold\" fill=\"#222222\">Average spectrum of group occurrences</text>\n";

    double maxGroupReductionDb = 1.0;
    for (double reductionDb : groupReductionDb)
        maxGroupReductionDb = max(maxGroupReductionDb, -reductionDb);
    maxGroupReductionDb = ceil(maxGroupReductionDb);

    for (size_t groupIndex = 0;
        groupIndex < repetitions.groupDetails.size(); ++groupIndex)
    {
        const RepetitionGroup& group =
            repetitions.groupDetails[groupIndex];

        if (group.length == 0 || group.occurrences.empty())
            continue;

        const double sectionTop =
            TOP + groupIndex * HEIGHT_PER_GROUP;
        const double plotTop = sectionTop + plotTopOffset;
        const double plotBottom = plotTop + plotHeight;

        const double reductionDb = groupReductionDb[groupIndex];

        ostringstream heading;
        heading << "Group " << groupLabel(groupIndex)
            << " | " << group.occurrences.size() << " occurrences"
            << " | applied group gain: "
            << (reductionDb > -0.005 ? 0.0 : reductionDb) << " dB";

        file << "<text x=\"60\" y=\"" << sectionTop + 17.0
            << "\" font-size=\"14\" font-weight=\"bold\" fill=\"#222222\">"
            << escapeXml(heading.str()) << "</text>\n";

        // Собираем вхождения для усреднённого спектра.
        // Временная дорожка показывает снижение группы на шкале всего трека.
        AudioData groupAudio = {};
        groupAudio.channels = repeatedAudio.channels;
        groupAudio.sampleRate = repeatedAudio.sampleRate;

        for (const RepetitionOccurrence& occurrence : group.occurrences)
        {
            if (occurrence.start >= repeatedAudio.frameCount)
                continue;

            const size_t available = static_cast<size_t>(
                min<uint64_t>(
                    group.length,
                    repeatedAudio.frameCount - occurrence.start));

            if (available == 0)
                continue;

            const size_t oldSize = groupAudio.samples.size();
            groupAudio.samples.resize(
                oldSize + available * repeatedAudio.channels);

            copy_n(
                repeatedAudio.samples.begin() +
                    occurrence.start * repeatedAudio.channels,
                available * repeatedAudio.channels,
                groupAudio.samples.begin() + oldSize);
        }

        groupAudio.frameCount =
            groupAudio.samples.size() / groupAudio.channels;

        const double trackDuration =
            static_cast<double>(repeatedAudio.frameCount) /
            repeatedAudio.sampleRate;

        // Шкала времени совпадает со шкалой графика multiband reduction.
        double timeStep = 3600.0;

        for (double candidate : { 0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0,
            15.0, 30.0, 60.0, 120.0, 300.0, 600.0, 1200.0, 1800.0, 3600.0 })
        {
            if (trackDuration / candidate <= 10.0)
            {
                timeStep = candidate;
                break;
            }
        }

        const bool minutes = trackDuration >= 120.0;
        const int decimals = timeStep < 1.0 ? 1 : 0;

        for (double time = 0.0; time <= trackDuration + 1e-9;
            time += timeStep)
        {
            const double x = timeLeft +
                timeWidth * time / trackDuration;

            file << "<line x1=\"" << x << "\" y1=\"" << plotTop
                << "\" x2=\"" << x << "\" y2=\"" << plotBottom
                << "\" stroke=\"#eeeeee\"/>\n";

            file << "<text x=\"" << x << "\" y=\"" << plotBottom + 18.0
                << "\" text-anchor=\"middle\" fill=\"#555555\">"
                << formatTimeLabel(time, minutes, decimals) << "</text>\n";
        }

        file << "<rect x=\"" << timeLeft << "\" y=\"" << plotTop
            << "\" width=\"" << timeWidth << "\" height=\"" << plotHeight
            << "\" fill=\"none\" stroke=\"#888888\"/>\n";

        // Высота заполнения показывает глубину снижения; сами сегменты
        // расположены строго на временных интервалах обнаруженных вхождений.
        const double reductionDepth = maxGroupReductionDb;
        const double attenuationHeight = plotHeight *
            clamp(-reductionDb / reductionDepth, 0.0, 1.0);

        if (reductionDb < -0.005)
        {
            for (const RepetitionOccurrence& occurrence : group.occurrences)
            {
                if (occurrence.start >= repeatedAudio.frameCount)
                    continue;

                const size_t endFrame = min(
                    repeatedAudio.frameCount,
                    occurrence.start + group.length);

                if (endFrame <= occurrence.start)
                    continue;

                const double x1 = timeLeft + timeWidth *
                    static_cast<double>(occurrence.start) /
                    repeatedAudio.frameCount;
                const double x2 = timeLeft + timeWidth *
                    static_cast<double>(endFrame) /
                    repeatedAudio.frameCount;

                file << "<rect x=\"" << x1 << "\" y=\"" << plotTop
                    << "\" width=\"" << max(1.0, x2 - x1)
                    << "\" height=\"" << attenuationHeight
                    << "\" fill=\"#e07b00\" fill-opacity=\"0.70\"/>\n";
            }
        }

        file << "<text x=\"" << timeLeft + timeWidth / 2.0
            << "\" y=\"" << plotBottom + 34.0
            << "\" text-anchor=\"middle\" fill=\"#444444\">"
            << (minutes ? "Track time, min:s" : "Track time, s")
            << " | reduction scale: 0 to -" << reductionDepth << " dB"
            << "</text>\n";

        // Спектр строится по конкатенированным фрагментам всех вхождений.
        Spectrum groupSpectrum;
        string spectrumError;
        bool spectrumReady = false;

        if (groupAudio.frameCount > 0 &&
            groupAudio.sampleRate >= 40000)
        {
            // Use the longest practical window for low-frequency resolution;
            // short fragments automatically fall back to a smaller FFT.
            size_t fftSize = 8192;

            while (fftSize > group.length && fftSize > 256)
                fftSize >>= 1;

            spectrumReady = calculateAverageSpectrum(
                groupAudio,
                groupSpectrum,
                spectrumError,
                fftSize,
                max<size_t>(1, fftSize / 4),
                12,
                3);
        }

        if (spectrumReady)
        {
            double minDb = 0.0;
            double maxDb = -120.0;

            for (double value : groupSpectrum.powerDb)
            {
                minDb = min(minDb, value);
                maxDb = max(maxDb, value);
            }

            if (maxDb - minDb < 1.0)
                minDb = maxDb - 1.0;

            const double spectrumTop = plotTop;
            const double spectrumBottom = plotBottom;

            auto xFrequency = [&](double frequency)
                {
                    return spectrumLeft + spectrumWidth *
                        log(frequency / 20.0) / log(20000.0 / 20.0);
                };

            auto yPower = [&](double value)
                {
                    return spectrumTop + plotHeight *
                        (maxDb - value) / (maxDb - minDb);
                };

            for (int tick = 0; tick <= 4; ++tick)
            {
                const double value =
                    minDb + (maxDb - minDb) * tick / 4.0;
                const double y = yPower(value);

                file << "<line x1=\"" << spectrumLeft << "\" y1=\"" << y
                    << "\" x2=\"" << spectrumLeft + spectrumWidth
                    << "\" y2=\"" << y
                    << "\" stroke=\"#e5e5e5\"/>\n";

                file << "<text x=\"" << spectrumLeft - 7.0
                    << "\" y=\"" << y + 4.0
                    << "\" text-anchor=\"end\" fill=\"#555555\">"
                    << setprecision(1) << value << " dB</text>\n";
            }

            file << "<polyline fill=\"none\" stroke=\"#d97706\" "
                << "stroke-width=\"2\" points=\"";

            for (size_t i = 0; i < groupSpectrum.frequencies.size(); ++i)
            {
                file << xFrequency(groupSpectrum.frequencies[i]) << ","
                    << yPower(groupSpectrum.powerDb[i]) << " ";
            }

            file << "\"/>\n";

            file << "<text x=\"" << spectrumLeft << "\" y=\""
                << plotBottom + 27.0
                << "\" fill=\"#444444\">20 Hz</text>\n";
            file << "<text x=\"" << spectrumLeft + spectrumWidth
                << "\" y=\"" << plotBottom + 27.0
                << "\" text-anchor=\"end\" fill=\"#444444\">20 kHz</text>\n";
        }
        else
        {
            file << "<rect x=\"" << spectrumLeft << "\" y=\"" << plotTop
                << "\" width=\"" << spectrumWidth << "\" height=\""
                << plotHeight << "\" fill=\"none\" stroke=\"#cccccc\"/>\n";
            file << "<text x=\"" << spectrumLeft + 8.0 << "\" y=\""
                << plotTop + 20.0
                << "\" fill=\"#777777\">Average spectrum unavailable";
            if (repeatedAudio.sampleRate < 40000)
                file << " (sample rate below 40 kHz)";
            file << "</text>\n";
        }

        file << "<line x1=\"60\" y1=\""
            << sectionTop + HEIGHT_PER_GROUP - 1.0
            << "\" x2=\"" << WIDTH - 40.0 << "\" y2=\""
            << sectionTop + HEIGHT_PER_GROUP - 1.0
            << "\" stroke=\"#dddddd\"/>\n";
    }

    file << "</svg>\n";
    file.close();

    if (file.fail())
    {
        error = "Unable to write repeated-component graph file.";
        return false;
    }

    return true;
}

bool saveBandReductionGraph(
    const BandReductionPlan& bands,
    const PeakReductionPlan& residual,
    double ceilingDb,
    const filesystem::path& filename,
    const string& title,
    string& error)
{
    if (bands.columnGainDb.size() != BAND_COUNT ||
        bands.columnGainDb[0].empty() ||
        bands.sampleRate <= 0.0 ||
        bands.durationSeconds <= 0.0 ||
        bands.eventBefore.size() < 2 ||
        bands.eventBefore.size() != bands.eventAfter.size())
    {
        error = "Invalid band reduction graph data.";
        return false;
    }

    const double WIDTH = 800.0;
    const double MARGIN_LEFT = 84.0;
    const double MARGIN_RIGHT = 24.0;
    const double plotWidth = WIDTH - MARGIN_LEFT - MARGIN_RIGHT;

    const size_t columns = bands.columnGainDb[0].size();
    const size_t laneCount = BAND_COUNT + 1;

    const double lanesTop = 66.0;
    const double laneHeight = 22.0;
    const double laneGap = 4.0;
    const double lanesBottom =
        lanesTop + laneCount * (laneHeight + laneGap) - laneGap;

    const double zoomTop = lanesBottom + 92.0;
    const double zoomHeight = 170.0;
    const double HEIGHT = zoomTop + zoomHeight + 52.0;

    const double duration = bands.durationSeconds;
    const double sampleRate = bands.sampleRate;

    // Данные дорожек: полосы и общее усиление
    vector<const vector<float>*> lanes;

    for (size_t b = 0; b < BAND_COUNT; b++)
    {
        lanes.push_back(&bands.columnGainDb[b]);
    }

    lanes.push_back(&residual.columnGainDb);

    auto laneValue = [&](size_t lane, size_t column)
        {
            const vector<float>& data = *lanes[lane];

            if (data.empty())
            {
                return 0.0;
            }

            return static_cast<double>(
                data[min(data.size() - 1, column * data.size() / columns)]);
        };

    double depthMax = 1.0;

    for (size_t lane = 0; lane < laneCount; lane++)
    {
        for (size_t c = 0; c < columns; c++)
        {
            depthMax = max(depthMax, -laneValue(lane, c));
        }
    }

    depthMax = ceil(depthMax);

    auto xColumn = [&](size_t column)
        {
            return MARGIN_LEFT + plotWidth * (column + 0.5) / columns;
        };

    // Фрагмент сильнейшего события
    const size_t count = bands.eventBefore.size();
    const double ceilingValue = pow(10.0, ceilingDb / 20.0);

    double zoomMin = -ceilingValue;
    double zoomMax = ceilingValue;

    for (size_t k = 0; k < count; k++)
    {
        zoomMin = min({ zoomMin, static_cast<double>(bands.eventBefore[k]),
            static_cast<double>(bands.eventAfter[k]) });

        zoomMax = max({ zoomMax, static_cast<double>(bands.eventBefore[k]),
            static_cast<double>(bands.eventAfter[k]) });
    }

    const double zoomPad = (zoomMax - zoomMin) * 0.08;

    zoomMin -= zoomPad;
    zoomMax += zoomPad;

    auto xZoom = [&](size_t k)
        {
            return MARGIN_LEFT + plotWidth * static_cast<double>(k) /
                static_cast<double>(count - 1);
        };

    auto yZoom = [&](double value)
        {
            return zoomTop + zoomHeight * (zoomMax - value) / (zoomMax - zoomMin);
        };

    ofstream file(filename);

    if (!file.is_open())
    {
        error = "Unable to create band reduction graph file.";
        return false;
    }

    file << fixed << setprecision(1);

    file << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    file << "<svg xmlns=\"http://www.w3.org/2000/svg\" "
        << "viewBox=\"0 0 " << WIDTH << " " << HEIGHT << "\" "
        << "width=\"" << WIDTH << "\" height=\"" << HEIGHT << "\" "
        << "font-family=\"Arial, Helvetica, sans-serif\" font-size=\"12\">\n";

    file << "<rect width=\"100%\" height=\"100%\" fill=\"#ffffff\"/>\n";

    writeGraphTitle(file, title, WIDTH);

    // Подпись дорожек
    {
        ostringstream caption;

        caption << fixed << setprecision(0)
            << "Band gain reduction (Hz; residual only, repeats excluded; 0 to -"
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