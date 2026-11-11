#define NOMINMAX

#include <windows.h>
#include <shobjidl.h>

#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <iomanip>
#include <chrono>
#include <cwctype>
#include <sstream>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstdlib>
#include <cmath>
#include <limits>
#include <utility>

#include "audio.h"
#include "fft.h"
#include "profiles.h"
#include "equalizer.h"
#include "repetition.h"

#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Uuid.lib")

using namespace std;
namespace fs = std::filesystem;

// Выбор папки через стандартное окно Windows
bool selectFolder(fs::path& selectedFolder)
{
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    if (FAILED(result))
    {
        return false;
    }

    IFileOpenDialog* dialog = nullptr;

    result = CoCreateInstance(
        CLSID_FileOpenDialog,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog)
    );

    if (SUCCEEDED(result))
    {
        DWORD options = 0;

        result = dialog->GetOptions(&options);

        if (SUCCEEDED(result))
        {
            result = dialog->SetOptions(
                options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM
            );
        }

        if (SUCCEEDED(result))
        {
            result = dialog->SetTitle(L"Select a folder with audio files");
        }

        if (SUCCEEDED(result))
        {
            result = dialog->Show(nullptr);
        }

        if (SUCCEEDED(result))
        {
            IShellItem* item = nullptr;

            result = dialog->GetResult(&item);

            if (SUCCEEDED(result))
            {
                PWSTR folderPath = nullptr;

                result = item->GetDisplayName(
                    SIGDN_FILESYSPATH,
                    &folderPath
                );

                if (SUCCEEDED(result))
                {
                    selectedFolder = fs::path(folderPath);
                    CoTaskMemFree(folderPath);
                }

                item->Release();
            }
        }

        dialog->Release();
    }

    CoUninitialize();

    return SUCCEEDED(result);
}

// Проверка поддерживаемого формата
bool isSupportedAudioFile(const fs::path& filename)
{
    wstring extension = filename.extension().wstring();

    transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        towlower
    );

    return extension == L".wav" ||
        extension == L".mp3" ||
        extension == L".flac";
}

// Суффикс папок для выбранного профиля шума
wstring getProfileFolderTag(NoiseProfile profile)
{
    switch (profile)
    {
    case NoiseProfile::WHITE:
        return L"white";

    case NoiseProfile::PINK:
        return L"pink";

    case NoiseProfile::BROWN:
        return L"brown";
    }

    return L"custom";
}

// Выбор профиля шума в консоли
bool selectNoiseProfile(NoiseProfile& profile)
{
    wcout << L"Select target noise profile:" << endl;
    wcout << L"  1 - White noise (flat spectrum)" << endl;
    wcout << L"  2 - Pink noise (-3 dB/octave) [default]" << endl;
    wcout << L"  3 - Brown noise (-6 dB/octave)" << endl;

    while (true)
    {
        wcout << L"Your choice (1-3, Enter = 2): " << flush;

        wstring line;

        if (!getline(wcin, line))
        {
            return false;
        }

        line.erase(
            remove_if(line.begin(), line.end(), iswspace),
            line.end()
        );

        if (line.empty() || line == L"2")
        {
            profile = NoiseProfile::PINK;
            return true;
        }

        if (line == L"1")
        {
            profile = NoiseProfile::WHITE;
            return true;
        }

        if (line == L"3")
        {
            profile = NoiseProfile::BROWN;
            return true;
        }

        wcout << L"Invalid choice, enter 1, 2 or 3." << endl;
    }
}

// Ввод числа с проверкой диапазона (Enter = значение по умолчанию)
bool readNumber(
    const wstring& prompt,
    double defaultValue,
    double minValue,
    double maxValue,
    double& result)
{
    while (true)
    {
        wcout << prompt << flush;

        wstring line;

        if (!getline(wcin, line))
        {
            return false;
        }

        line.erase(
            remove_if(line.begin(), line.end(), iswspace),
            line.end()
        );

        if (line.empty())
        {
            result = defaultValue;
            return true;
        }

        // Допускаем запятую как десятичный разделитель
        replace(line.begin(), line.end(), L',', L'.');

        wchar_t* end = nullptr;
        double value = wcstod(line.c_str(), &end);

        if (end == line.c_str() ||
            *end != L'\0' ||
            !isfinite(value) ||
            value < minValue ||
            value > maxValue)
        {
            wcout << L"Enter a number from " << minValue
                << L" to " << maxValue << L"." << endl;
            continue;
        }

        result = value;
        return true;
    }
}

// Выбор коэффициента эквализации
bool selectIntensity(double& intensity)
{
    // Enter must always select full equalization, even if the caller's
    // settings object was initialized with a different intensity.
    intensity = 1.0;

    wcout << L"Select equalization strength:" << endl;
    wcout << L"  0 - 0%" << endl;
    wcout << L"  1 - 25%" << endl;
    wcout << L"  2 - 50%" << endl;
    wcout << L"  3 - 75%" << endl;
    wcout << L"  4 - 100% [default]" << endl;

    while (true)
    {
        wcout << L"Your choice (1-4, Enter = 4): " << flush;

        wstring line;

        if (!getline(wcin, line))
        {
            return false;
        }

        line.erase(
            remove_if(line.begin(), line.end(), iswspace),
            line.end()
        );

        if (line.empty() || line == L"4")
        {
            return true;
        }

        if (line == L"0") { intensity = 0.0; return true; }
        if (line == L"1") { intensity = 0.25; return true; }
        if (line == L"2") { intensity = 0.5;  return true; }
        if (line == L"3") { intensity = 0.75; return true; }

        wcout << L"Invalid choice, enter 1, 2, 3 or 4." << endl;
    }
}

// Ввод мягкой и жёсткой границ коррекции
bool selectLimits(EqualizerSettings& settings)
{
    wcout << L"Correction limits (dB):" << endl;

    if (!readNumber(
        L"  Soft limit, correction is linear up to it (Enter = 6): ",
        6.0, 0.0, 6.0,
        settings.softLimitDb))
    {
        return false;
    }

    const double defaultHard = max(12.0, settings.softLimitDb);

    wcout << L"  Hard limit must be >= soft limit." << endl;

    return readNumber(
        L"  Hard limit, correction never exceeds it (Enter = "
        + to_wstring(static_cast<int>(defaultHard))
        + (defaultHard == floor(defaultHard) ? L"" : L"+")
        + L"): ",
        defaultHard, settings.softLimitDb, 12.0,
        settings.hardLimitDb
    );
}

// Выбор восстановления срезанных пиков
bool selectDeclip(EqualizerSettings& settings)
{
    wcout << L"Restore clipped peaks (declipping)?" << endl;
    wcout << L"  1 - No" << endl;
    wcout << L"  2 - Yes [default]" << endl;

    while (true)
    {
        wcout << L"Your choice (1-2, Enter = 2): " << flush;

        wstring line;

        if (!getline(wcin, line))
        {
            return false;
        }

        line.erase(
            remove_if(line.begin(), line.end(), iswspace),
            line.end()
        );

        if (line == L"1")
        {
            settings.declip = false;
            return true;
        }

        if (line.empty() || line == L"2")
        {
            settings.declip = true;
            break;
        }

        wcout << L"Invalid choice, enter 1 or 2." << endl;
    }

    if (!readNumber(
        L"  Reference peak ceiling for declipping, dBFS (Enter = -1): ",
        -1.0, -12.0, 0.0,
        settings.declipCeilingDb))
    {
        return false;
    }

    if (!readNumber(
        L"  Maximum declipping overshoot, dB (Enter = 2): ",
        2.0, 0.0, 10.0,
        settings.declipOvershootDb))
    {
        return false;
    }

    wcout << L"    0 = preserve residual, 100 = preserve repeated components." << endl;
    if (!readNumber(
        L"  Repeated-component preservation priority, % (Enter = 50): ",
        50.0, 0.0, 100.0,
        settings.repeatedPreservationPriority))
    {
        return false;
    }

    if (!readNumber(
        L"  Fade time for residual peak reduction, ms (Enter = 40): ",
        40.0, 5.0, 300.0,
        settings.declipFadeMs))
    {
        return false;
    }

    if (!readNumber(
        L"  Multiband Attack time, ms (Enter = 5): ",
        5.0, 0.1, 500.0,
        settings.declipAttackMs))
    {
        return false;
    }

    return readNumber(
        L"  Reference Release time at 1000 Hz, ms (Enter = 100): ",
        100.0, 1.0, 2000.0,
        settings.declipReferenceReleaseMs
    );
}

// Короткое описание настроек для имён папок: 50pct_3-5dB
wstring getSettingsTag(const EqualizerSettings& settings)
{
    wostringstream tag;

    tag << static_cast<int>(round(settings.intensity * 100.0))
        << L"pct_"
        << settings.softLimitDb << L"-"
        << settings.hardLimitDb << L"dB";

    if (settings.declip)
    {
        tag << L"_declip" << settings.declipCeilingDb << L"dB_O"
            << settings.declipOvershootDb << L"dB_P"
            << settings.repeatedPreservationPriority << L"pct_"
            << settings.declipFadeMs << L"ms_A"
            << settings.declipAttackMs << L"ms_R"
            << settings.declipReferenceReleaseMs << L"ms";
    }

    return tag.str();
}

// Перевод wide-строки в UTF-8
string toUtf8(const wstring& text)
{
    if (text.empty())
    {
        return string();
    }

    int size = WideCharToMultiByte(
        CP_UTF8, 0,
        text.c_str(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr
    );

    string result(size, '\0');

    WideCharToMultiByte(
        CP_UTF8, 0,
        text.c_str(), static_cast<int>(text.size()),
        result.data(), size, nullptr, nullptr
    );

    return result;
}

// Вывод общего прогресса обработки
// Общий потокобезопасный вывод прогресса
struct ProgressReporter
{
    mutex lock;
    size_t previousLength = 0;

    vector<int> jobProgress;
    size_t total = 0;
    size_t done = 0;
    size_t failed = 0;

    void init(size_t count)
    {
        total = count;
        jobProgress.assign(count, 0);
    }

    // Вызывается только при захваченном мьютексе
    void drawLine(const wstring& stage, const fs::path& file)
    {
        long long sum = 0;

        for (int value : jobProgress)
        {
            sum += value;
        }

        int percent = total == 0
            ? 100
            : static_cast<int>(sum / static_cast<long long>(total));

        wstringstream message;

        message << L"[" << setw(3) << percent << L"%] "
            << L"Done " << done << L"/" << total;

        if (failed > 0)
        {
            message << L" (failed: " << failed << L")";
        }

        message << L" | " << stage
            << L" | " << file.filename().wstring();

        wstring output = message.str();

        // Rewrite the progress line once and clear any leftover characters.
        wcout << L"\r" << output;

        if (previousLength > output.length())
        {
            wcout << wstring(previousLength - output.length(), L' ');
        }

        previousLength = output.length();
        wcout << flush;
    }

    // Новый этап обработки файла
    void update(
        size_t index,
        int percent,
        const wstring& stage,
        const fs::path& file)
    {
        lock_guard<mutex> guard(lock);

        jobProgress[index] = percent;
        drawLine(stage, file);
    }

    // Очистка текущей строки перед выводом внешней диагностики
    void clearLine()
    {
        lock_guard<mutex> guard(lock);

        wcout << L"\r" << wstring(previousLength, L' ') << L"\r" << flush;
        previousLength = 0;
    }

    // Вывод диагностических сообщений с очисткой строки прогресса.
    void note(const wstring& message)
    {
        lock_guard<mutex> guard(lock);

        if (previousLength > 0)
        {
            wcout << L"\r" << wstring(previousLength, L' ') << L"\r";
            previousLength = 0;
        }

        wcout << message << endl;
    }

    // Файл обработан успешно
    void finished(size_t index, const fs::path& file)
    {
        lock_guard<mutex> guard(lock);

        jobProgress[index] = 100;
        done++;

        drawLine(L"Completed", file);
    }

    // Ошибка: сообщение выводится на отдельной строке
    void failedJob(
        size_t index,
        const fs::path& file,
        const string& reason)
    {
        lock_guard<mutex> guard(lock);

        jobProgress[index] = 100;
        done++;
        failed++;

        wcout << L"\r" << wstring(previousLength, L' ') << L"\r";
        wcout << L"Error processing: " << file.wstring() << endl;
        wcout << L"  Reason: "
            << wstring(reason.begin(), reason.end()) << endl;

        previousLength = 0;
        drawLine(L"Failed", file);
    }
};


// Jointly optimize one repeated group and its residual.
struct RepeatedGroupOptimizationStats
{
    double repeatedGainDb = 0.0;
    double maximumResidualReductionDb = 0.0;
    size_t residualReducedFrames = 0;
    size_t smoothingAdjustedFrames = 0;
};

static double optimizeRepeatedGroup(
    AudioData& repeated, AudioData& residual,
    const RepetitionGroup& group, double ceiling,
    double repeatedPriorityPercent, double smoothingMs,
    RepeatedGroupOptimizationStats& stats)
{
    stats = RepeatedGroupOptimizationStats{};

    if (repeated.channels != residual.channels ||
        repeated.frameCount != residual.frameCount ||
        repeated.samples.size() != residual.samples.size() ||
        repeated.channels == 0) return 1.0;

    const size_t channels = repeated.channels;
    const size_t frames = static_cast<size_t>(repeated.frameCount);
    vector<size_t> groupFrames;

    for (const RepetitionOccurrence& occurrence : group.occurrences)
    {
        if (occurrence.start >= frames) continue;
        const size_t end = min(frames, occurrence.start + group.length);
        for (size_t frame = occurrence.start; frame < end; ++frame)
            groupFrames.push_back(frame);
    }

    if (groupFrames.empty()) return 1.0;

    sort(groupFrames.begin(), groupFrames.end());
    groupFrames.erase(unique(groupFrames.begin(), groupFrames.end()),
        groupFrames.end());

    const double wr = clamp(repeatedPriorityPercent / 100.0, 0.0, 1.0);
    const double ws = 1.0 - wr;
    const double minimumGain = 1e-4;
    // Leave a tiny numerical margin for float multiplication and summation.
    const double safeCeiling = ceiling * (1.0 - 1e-6);
    const size_t radius = static_cast<size_t>(max(
        0.0, round(smoothingMs * 0.0005 * repeated.sampleRate)));

    vector<float> bestResidualGains(groupFrames.size(), 1.0f);
    double bestGain = 1.0;
    double bestCost = numeric_limits<double>::infinity();

    // The objective is evaluated after smoothing. Each smoothed residual gain
    // is projected into its frame's feasible interval, preserving the ceiling.
    auto evaluate = [&](double gr, vector<float>* saved,
        RepeatedGroupOptimizationStats* savedStats)
    {
        vector<double> lowerBounds(groupFrames.size(), 0.0);
        vector<double> upperBounds(groupFrames.size(), 1.0);
        vector<float> rawGains(groupFrames.size(), 1.0f);

        for (size_t f = 0; f < groupFrames.size(); ++f)
        {
            const size_t frame = groupFrames[f];
            double lower = 0.0;
            double upper = 1.0;

            for (size_t ch = 0; ch < channels; ++ch)
            {
                const size_t index = frame * channels + ch;
                const double a = repeated.samples[index] * gr;
                const double b = residual.samples[index];

                if (abs(b) < 1e-15)
                {
                    if (abs(a) > safeCeiling)
                        return numeric_limits<double>::infinity();
                    continue;
                }

                const double x1 = (-safeCeiling - a) / b;
                const double x2 = ( safeCeiling - a) / b;
                lower = max(lower, min(x1, x2));
                upper = min(upper, max(x1, x2));
            }

            lower = max(lower, 0.0);
            upper = min(upper, 1.0);

            if (lower > upper)
                return numeric_limits<double>::infinity();

            lowerBounds[f] = lower;
            upperBounds[f] = upper;
            rawGains[f] = static_cast<float>(upper);
        }

        vector<float> gains = rawGains;

        if (radius > 0 && gains.size() > 1)
        {
            vector<float> smoothed(gains.size(), 1.0f);

            for (size_t i = 0; i < gains.size(); ++i)
            {
                size_t first = i;
                while (first > 0 &&
                    groupFrames[i] - groupFrames[first - 1] <= radius)
                    --first;

                size_t last = i;
                while (last + 1 < groupFrames.size() &&
                    groupFrames[last + 1] - groupFrames[i] <= radius)
                    ++last;

                float value = 1.0f;
                for (size_t j = first; j <= last; ++j)
                    value = min(value, rawGains[j]);

                // Do not let smoothing violate either side of the feasible range.
                smoothed[i] = static_cast<float>(clamp(
                    static_cast<double>(value),
                    lowerBounds[i], upperBounds[i]));
            }

            gains.swap(smoothed);
        }

        double residualLoss = 0.0;
        RepeatedGroupOptimizationStats localStats;

        for (size_t f = 0; f < gains.size(); ++f)
        {
            if (abs(static_cast<double>(gains[f]) -
                static_cast<double>(rawGains[f])) > 1e-6)
            {
                ++localStats.smoothingAdjustedFrames;
            }

            if (gains[f] < 1.0f)
            {
                ++localStats.residualReducedFrames;
                const double db = 20.0 * log10(max(
                    static_cast<double>(gains[f]), minimumGain));
                residualLoss += db * db;
                localStats.maximumResidualReductionDb = min(
                    localStats.maximumResidualReductionDb, db);
            }
        }

        const double grDb = 20.0 * log10(max(gr, minimumGain));
        const double meanResidualLoss =
            residualLoss / static_cast<double>(groupFrames.size());
        const double cost = wr * grDb * grDb + ws * meanResidualLoss;

        if (saved)
            *saved = move(gains);

        if (savedStats)
        {
            localStats.repeatedGainDb = grDb;
            *savedStats = localStats;
        }

        return cost;
    };

    constexpr size_t GRID = 32;
    size_t bestIndex = GRID;

    for (size_t i = 0; i <= GRID; ++i)
    {
        const double gr = minimumGain + (1.0 - minimumGain) * i / GRID;
        const double cost = evaluate(gr, nullptr, nullptr);

        if (cost < bestCost - 1e-12 ||
            (abs(cost - bestCost) <= 1e-12 && gr > bestGain))
        {
            bestCost = cost;
            bestGain = gr;
            bestIndex = i;
        }
    }

    if (!isfinite(bestCost)) return 1.0;

    double left = minimumGain + (1.0 - minimumGain) *
        (bestIndex == 0 ? 0 : bestIndex - 1) / GRID;
    double right = minimumGain + (1.0 - minimumGain) *
        (bestIndex == GRID ? GRID : bestIndex + 1) / GRID;

    const double golden = 0.6180339887498948482;
    double x1 = right - golden * (right - left);
    double x2 = left + golden * (right - left);
    double f1 = evaluate(x1, nullptr, nullptr);
    double f2 = evaluate(x2, nullptr, nullptr);

    for (int i = 0; i < 24; ++i)
    {
        if (f1 <= f2)
        {
            right = x2;
            x2 = x1;
            f2 = f1;
            x1 = right - golden * (right - left);
            f1 = evaluate(x1, nullptr, nullptr);
        }
        else
        {
            left = x1;
            x1 = x2;
            f1 = f2;
            x2 = left + golden * (right - left);
            f2 = evaluate(x2, nullptr, nullptr);
        }
    }

    if (f1 < bestCost) { bestCost = f1; bestGain = x1; }
    if (f2 < bestCost) { bestCost = f2; bestGain = x2; }

    evaluate(bestGain, &bestResidualGains, &stats);

    for (size_t f = 0; f < groupFrames.size(); ++f)
    {
        const size_t frame = groupFrames[f];

        for (size_t ch = 0; ch < channels; ++ch)
        {
            const size_t index = frame * channels + ch;
            repeated.samples[index] *= static_cast<float>(bestGain);
            residual.samples[index] *= bestResidualGains[f];
        }
    }

    return bestGain;
}

int main()
{
    // Выбор профиля шума
    NoiseProfile selectedProfile = NoiseProfile::PINK;

    if (!selectNoiseProfile(selectedProfile))
    {
        wcout << L"Input cancelled." << endl;
        return 1;
    }

    wcout << endl;

    // Настройки эквализации
    EqualizerSettings settings;

    if (!selectIntensity(settings.intensity))
    {
        wcout << L"Input cancelled." << endl;
        return 1;
    }

    wcout << endl;

    if (!selectLimits(settings))
    {
        wcout << L"Input cancelled." << endl;
        return 1;
    }

    wcout << endl;

    if (!selectDeclip(settings))
    {
        wcout << L"Input cancelled." << endl;
        return 1;
    }

    // Суффикс папок: профиль + настройки
    const wstring profileTag =
        getProfileFolderTag(selectedProfile) + L"_" +
        getSettingsTag(settings);

    wcout << endl;

    // Выбор исходной папки
    fs::path inputDirectory;

    wcout << L"Select a folder containing audio files..." << endl;

    if (!selectFolder(inputDirectory))
    {
        wcout << L"Folder selection cancelled or failed." << endl;
        return 1;
    }

    // Проверка выбранной папки
    error_code filesystemError;

    if (!fs::exists(inputDirectory, filesystemError) ||
        !fs::is_directory(inputDirectory, filesystemError))
    {
        wcerr << L"Invalid input directory." << endl;
        return 1;
    }

    // Создание выходной папки рядом с исходной
    const wstring baseName = inputDirectory.filename().wstring();

    fs::path outputDirectory =
        inputDirectory.parent_path() / (baseName + L"_EQ_" + profileTag);

    fs::path graphsDirectory =
        inputDirectory.parent_path() / (baseName + L"_graphs_" + profileTag);

    if (!fs::create_directories(graphsDirectory, filesystemError) &&
        filesystemError)
    {
        wcerr << L"Unable to create graphs directory." << endl;
        return 1;
    }

    if (!fs::create_directories(outputDirectory, filesystemError) &&
        filesystemError)
    {
        wcerr << L"Unable to create output directory." << endl;
        return 1;
    }

    // Поиск аудиофайлов, включая вложенные папки
    vector<fs::path> inputFiles;

    fs::recursive_directory_iterator iterator(
        inputDirectory,
        fs::directory_options::skip_permission_denied,
        filesystemError
    );

    fs::recursive_directory_iterator endIterator;

    while (iterator != endIterator)
    {
        if (iterator->is_regular_file(filesystemError) &&
            !filesystemError &&
            isSupportedAudioFile(iterator->path()))
        {
            inputFiles.push_back(iterator->path());
        }

        filesystemError.clear();
        iterator.increment(filesystemError);

        if (filesystemError)
        {
            filesystemError.clear();
        }
    }

    if (inputFiles.empty())
    {
        wcout << L"No supported audio files were found." << endl;
        return 0;
    }

    sort(inputFiles.begin(), inputFiles.end());

    wcout << L"\nInput folder:  "
        << inputDirectory.wstring() << endl;

    wcout << L"Output folder: "
        << outputDirectory.wstring() << endl;

    const string profileName = getProfileName(selectedProfile);

    wcout << L"Strength: "
        << static_cast<int>(round(settings.intensity * 100.0))
        << L"%, soft limit: " << settings.softLimitDb
        << L" dB, hard limit: " << settings.hardLimitDb
        << L" dB" << endl;

    wcout << L"Declipping: ";

    if (settings.declip)
    {
        wcout << L"on, ceiling " << settings.declipCeilingDb
            << L" dBFS, max overshoot " << settings.declipOvershootDb
            << L" dB, repeated priority "
            << settings.repeatedPreservationPriority
            << L"%, residual fade " << settings.declipFadeMs
            << L" ms, multiband attack " << settings.declipAttackMs
            << L" ms, reference release " << settings.declipReferenceReleaseMs
            << L" ms at 1 kHz" << endl;
    }
    else
    {
        wcout << L"off" << endl;
    }

    wcout << L"Graphs folder: "
        << graphsDirectory.wstring() << endl;

    wcout << L"Audio files found: "
        << inputFiles.size() << endl << endl;

    // Подготовка путей и папок (до запуска потоков, чтобы избежать гонок)
    struct Job
    {
        fs::path input;
        fs::path output;
        fs::path graphCorrection;
        fs::path graphSpectrum;
        fs::path graphDeclip;
        fs::path graphBands;
        fs::path graphRepeated;
        fs::path relative;
    };

    vector<Job> jobs;
    size_t skippedFiles = 0;

    for (const fs::path& inputFilename : inputFiles)
    {
        filesystemError.clear();

        fs::path relativePath =
            fs::relative(inputFilename, inputDirectory, filesystemError);

        if (filesystemError)
        {
            skippedFiles++;
            continue;
        }

        Job job;
        job.input = inputFilename;
        job.relative = relativePath;
        job.output = outputDirectory / relativePath;
        job.graphCorrection = graphsDirectory / relativePath;
        job.graphCorrection += L".04_correction.svg";

        job.graphSpectrum = graphsDirectory / relativePath;
        job.graphSpectrum += L".05_spectrum.svg";

        job.graphDeclip = graphsDirectory / relativePath;
        job.graphDeclip += L".03_declip.svg";

        job.graphBands = graphsDirectory / relativePath;
        job.graphBands += L".01_bands.svg";

        job.graphRepeated = graphsDirectory / relativePath;
        job.graphRepeated += L".02_repeated_components.svg";

        fs::create_directories(job.output.parent_path(), filesystemError);

        if (!filesystemError)
        {
            fs::create_directories(
                job.graphCorrection.parent_path(),
                filesystemError
            );
        }

        if (filesystemError)
        {
            skippedFiles++;
            continue;
        }

        jobs.push_back(move(job));
    }

    const size_t totalFiles = jobs.size();

    atomic<size_t> nextIndex(0);
    atomic<size_t> successfulFiles(0);
    atomic<size_t> failedFiles(0);

    ProgressReporter progress;
    progress.init(totalFiles);

    // Обработка одного файла; возвращает true при успехе
    auto processJob = [&](size_t index, const Job& job, string& error) -> bool
        {
            AudioData audio = {};
            AudioData outputAudio = {};
            Spectrum spectrum;
            Spectrum spectrumAfter;

            vector<double> correctionDb;
            vector<double> requestedCorrectionDb;

            ostringstream titleStream;

            titleStream << toUtf8(job.relative.wstring()) << " | "
                << getProfileName(selectedProfile) << " | "
                << static_cast<int>(round(settings.intensity * 100.0))
                << "% | " << settings.softLimitDb << "/"
                << settings.hardLimitDb << " dB";

            if (settings.declip)
            {
                titleStream << " | declip " << settings.declipCeilingDb
                    << " dB, overshoot " << settings.declipOvershootDb
                    << " dB, repeated priority "
                    << settings.repeatedPreservationPriority
                    << "%, residual fade "
                    << settings.declipFadeMs << " ms, attack "
                    << settings.declipAttackMs << " ms, reference release "
                    << settings.declipReferenceReleaseMs << " ms @ 1 kHz";
            }

            const string graphTitle = titleStream.str();

            // Ошибки графиков не прерывают обработку трека
            string graphError;

            // Создание callback для отдельного этапа обработки.
            // Локальный прогресс 0–100 преобразуется в общий прогресс файла.
            auto makeStageProgress = [&](int stageStart,
                int stageLength,
                const wstring& stageName) -> ProgressCallback
                {
                    return [&, stageStart, stageLength, stageName, lastPercent = -1]
                    (int localPercent) mutable
                        {
                            localPercent = clamp(localPercent, 0, 100);

                            int totalPercent =
                                stageStart + stageLength * localPercent / 100;

                            // Не обновляем консоль, если процент не изменился.
                            if (totalPercent != lastPercent)
                            {
                                lastPercent = totalPercent;

                                progress.update(
                                    index,
                                    totalPercent,
                                    stageName,
                                    job.input
                                );
                            }
                        };
                };

            progress.update(index, 0, L"Loading", job.input);

            if (!loadAudio(job.input, audio, error))
            {
                return false;
            }

            const AudioQuality inputQuality = analyzeQuality(audio);
            {
                wostringstream info;
                info << fixed << setprecision(3)
                    << L"\n[Diagnostics] Input: " << job.input.filename().wstring()
                    << L" | " << audio.sampleRate << L" Hz"
                    << L" | " << audio.channels << L" channels"
                    << L" | " << static_cast<double>(audio.frameCount) / audio.sampleRate
                    << L" s"
                    << L" | peak " << inputQuality.peakDb << L" dBFS"
                    << L" | RMS " << inputQuality.rmsDb << L" dBFS"
                    << L" | crest " << inputQuality.crestDb << L" dB"
                    << L" | clipped runs " << inputQuality.clippedRuns
                    << L" | clipped samples " << inputQuality.clippedSamples;
                progress.note(info.str());
            }

            AudioData repeatedComponents;
            AudioData residualAudio;
            AudioData repeatedProcessed;
            RepetitionResult repetitions;
            vector<double> repeatedGroupReductionDb;

            // Восстановление срезанных пиков (опционально, до анализа спектра)
            if (settings.declip)
            {
                progress.update(index, 5, L"Finding repeated components", job.input);

                // Копия исходного аудио нужна только для графика сравнения
                AudioData originalAudio = audio;

                RepetitionSettings repetitionSettings;
                string repetitionError;
                string repetitionDiagnostics;

                const bool repetitionReady = separateRepeatedComponents(
                    originalAudio,
                    repeatedComponents,
                    residualAudio,
                    repetitionSettings,
                    repetitions,
                    repetitionError,
                    repetitionDiagnostics
                );

                if (!repetitionDiagnostics.empty())
                {
                    progress.note(wstring(
                        repetitionDiagnostics.begin(),
                        repetitionDiagnostics.end()
                    ));
                }

                if (repetitionReady && repetitions.groups > 0)
                {
                    wostringstream repetitionInfo;
                    repetitionInfo << L"Repeated components: "
                        << repetitions.groups << L" groups, "
                        << repetitions.occurrences << L" occurrences";
                    progress.note(repetitionInfo.str());
                }

                progress.update(index, 5, L"Restoring peaks", job.input);

                DeclipResult declip = declipAudio(
                    audio,
                    2,
                    64,
                    makeStageProgress(
                        5,
                        5,
                        L"Restoring clipped peaks"
                    ),
                    repetitionReady ? &repeatedComponents : nullptr,
                    repetitionReady ? &residualAudio : nullptr,
                    repetitionReady ? &repetitions : nullptr,
                    &originalAudio
                );

                {
                    const AudioQuality restoredQuality = analyzeQuality(audio);
                    wostringstream info;
                    info << fixed << setprecision(3)
                        << L"[Diagnostics] After declipping: peak "
                        << restoredQuality.peakDb << L" dBFS"
                        << L" | RMS " << restoredQuality.rmsDb << L" dBFS"
                        << L" | clipped runs " << restoredQuality.clippedRuns
                        << L" | clipped samples " << restoredQuality.clippedSamples
                        << L" | restored runs " << declip.runs
                        << L" | skipped runs " << declip.skipped
                        << L" | repeated groups with/without clean reference "
                        << declip.repeatedGroupsWithCleanReference << L"/"
                        << declip.repeatedGroupsWithoutCleanReference
                        << L" | repeated occurrences protected/reconstructed "
                        << declip.repeatedOccurrencesProtected << L"/"
                        << declip.repeatedOccurrencesReconstructed
                        << L" | repeated runs/samples "
                        << declip.repeatedRuns << L"/" << declip.repeatedSamples
                        << L" | legacy detected/restored "
                        << declip.legacyDetectedRuns << L"/"
                        << declip.legacyRestoredRuns
                        << L" | legacy restored/rejected "
                        << declip.legacyRestoredRuns << L"/"
                        << declip.legacyOvershootRejectedRuns
                        << L" | too long/edge/short "
                        << declip.legacyTooLongRuns << L"/"
                        << declip.legacyEdgeRuns << L"/"
                        << declip.legacyShortRuns
                        << L" | candidate lengths 1/2/3-4/5-8/9+ "
                        << declip.legacyCandidateLen1 << L"/"
                        << declip.legacyCandidateLen2 << L"/"
                        << declip.legacyCandidateLen3To4 << L"/"
                        << declip.legacyCandidateLen5To8 << L"/"
                        << declip.legacyCandidateLen9Plus
                        << L" | shape exact-flat/tight/moderate/wide "
                        << declip.legacyShapeExactFlatRuns << L"/"
                        << declip.legacyShapeRangeTightRuns << L"/"
                        << declip.legacyShapeRangeModerateRuns << L"/"
                        << declip.legacyShapeRangeWideRuns
                        << L" | neighbors available/both lower "
                        << declip.legacyShapeNeighborAvailableRuns << L"/"
                        << declip.legacyShapeBothNeighborsLowerRuns
                        << L" | neighbor symmetry <=1/5/20%/>20% "
                        << declip.legacyShapeNeighborSymmetry01 << L"/"
                        << declip.legacyShapeNeighborSymmetry05 << L"/"
                        << declip.legacyShapeNeighborSymmetry20 << L"/"
                        << declip.legacyShapeNeighborAsymmetricRuns
                        << L" | candidate overlap none/partial/full "
                        << declip.legacyCandidateNoOverlapRuns << L"/"
                        << declip.legacyCandidatePartialOverlapRuns << L"/"
                        << declip.legacyCandidateFullOverlapRuns
                        << L" | overlap samples " << declip.legacyCandidateOverlapSamples
                        << L" | restored/rejected/skipped overlap runs "
                        << declip.legacyRestoredOverlapRuns << L"/"
                        << declip.legacyRejectedOverlapRuns << L"/"
                        << declip.legacySkippedOverlapRuns
                        << L" | overlap samples by outcome "
                        << declip.legacyRestoredOverlapSamples << L"/"
                        << declip.legacyRejectedOverlapSamples << L"/"
                        << declip.legacySkippedOverlapSamples
                        << L" | short peak <.90/.90-.98/.98-.999/>=.999 "
                        << declip.legacyShortPeakBelow90 << L"/"
                        << declip.legacyShortPeak90To98 << L"/"
                        << declip.legacyShortPeak98To999 << L"/"
                        << declip.legacyShortPeak999Plus;
                    progress.note(L"[Diagnostics] " +
                        job.input.filename().wstring() + L" | " + info.str());

                    for (const SmearedCeilingDiagnostic& fit :
                        declip.smearedCeilings)
                    {
                        const double thetaDb =
                            20.0 * log10(max(fit.theta, 1e-12));
                        const double kneeDb =
                            20.0 * log10(max(fit.knee, 1e-12));
                        const double sigmaPercent =
                            100.0 * fit.sigma / max(fit.theta, 1e-12);

                        wostringstream fitInfo;
                        fitInfo << fixed << setprecision(3)
                            << L"[Smeared ceiling fit] Channel "
                            << (fit.channel + 1)
                            << (fit.polarity > 0 ? L" positive" : L" negative")
                            << L" | theta " << thetaDb << L" dBFS"
                            << L" | sigma/theta " << sigmaPercent << L"%"
                            << L" | knee(theta-6*sigma) " << kneeDb << L" dBFS"
                            << L" | bump ratio " << fit.bumpRatio
                            << L" | touch events " << fit.touchEvents
                            << L" | smeared evidence "
                            << (fit.smearedEvidence ? L"yes" : L"no");

                        progress.note(fitInfo.str());
                    }
                }

                const bool hasRepeatedGroups =
                    repetitionReady && repetitions.groups > 0 &&
                    !repetitions.groupDetails.empty();

                vector<uint8_t> repeatedFrames(
                    static_cast<size_t>(audio.frameCount), 0);

                if (hasRepeatedGroups)
                {
                    repeatedProcessed = std::move(repeatedComponents);
                    repeatedGroupReductionDb.assign(
                        repetitions.groupDetails.size(), 0.0);

                    for (const RepetitionGroup& group : repetitions.groupDetails)
                    {
                        for (const RepetitionOccurrence& occurrence : group.occurrences)
                        {
                            if (occurrence.start >= audio.frameCount)
                                continue;

                            const size_t endFrame = min(
                                static_cast<size_t>(audio.frameCount),
                                occurrence.start + group.length);

                            fill(
                                repeatedFrames.begin() + occurrence.start,
                                repeatedFrames.begin() + endFrame,
                                1
                            );
                        }
                    }

                    // Keep the repeated component separate after declipping.
                    // Changes inside a detected occurrence are assigned to it;
                    // the remaining signal stays in residualAudio.
                    for (size_t frame = 0; frame < audio.frameCount; ++frame)
                    {
                        for (size_t channel = 0; channel < audio.channels; ++channel)
                        {
                            const size_t sampleIndex =
                                frame * audio.channels + channel;

                            if (repeatedFrames[frame])
                            {
                                repeatedProcessed.samples[sampleIndex] =
                                    audio.samples[sampleIndex] -
                                    residualAudio.samples[sampleIndex];
                            }
                            else
                            {
                                repeatedProcessed.samples[sampleIndex] = 0.0f;
                            }

                            residualAudio.samples[sampleIndex] =
                                audio.samples[sampleIndex] -
                                repeatedProcessed.samples[sampleIndex];
                        }
                    }
                }

                if (declip.runs > 0 || declip.skipped > 0)
                {
                    wostringstream info;

                    info << fixed << setprecision(1)
                        << L"Restored: " << job.relative.wstring()
                        << L" | " << declip.runs << L" runs ("
                        << declip.samples << L" samples)";

                    if (declip.skipped > 0)
                    {
                        info << L", not restored: " << declip.skipped
                            << L" (too long or at file edge)";
                    }

                    progress.note(info.str());
                }

                const double workingCeilingDb =
                    settings.declipCeilingDb + settings.declipOvershootDb;
                const double peakCeiling =
                    pow(10.0, workingCeilingDb / 20.0);

                bool mixExceedsCeiling = false;
                double mixPeak = 0.0;

                for (float sample : audio.samples)
                {
                    const double magnitude = abs(static_cast<double>(sample));
                    mixPeak = max(mixPeak, magnitude);

                    if (magnitude > peakCeiling)
                    {
                        mixExceedsCeiling = true;
                    }
                }

                {
                    const double mixPeakDb = mixPeak > 0.0
                        ? 20.0 * log10(mixPeak)
                        : -200.0;

                    wostringstream info;
                    info << fixed << setprecision(3)
                        << L"[Diagnostics] Multiband gate: working ceiling "
                        << workingCeilingDb << L" dBFS"
                        << L" (" << peakCeiling << L")"
                        << L" | post-declip mix peak " << mixPeakDb << L" dBFS"
                        << L" | declip runs " << declip.runs
                        << L" | gate " << ((declip.runs > 0 || mixExceedsCeiling)
                            ? L"OPEN" : L"CLOSED");

                    if (declip.runs == 0 && !mixExceedsCeiling)
                    {
                        info << L" | reason: no restored runs and no sample exceeds ceiling";
                    }

                    progress.note(info.str());
                }

                if (declip.runs > 0 || mixExceedsCeiling)
                {
                    if (declip.runs > 0)
                    {
                        saveDeclipGraph(
                            originalAudio,
                            audio,
                            declip,
                            job.graphDeclip,
                            graphTitle,
                            graphError
                        );
                    }

                    originalAudio.samples.clear();
                    originalAudio.samples.shrink_to_fit();

                    // First reduce peaks that exceed the working ceiling in the residual
                    // itself. Combined repeated/residual peaks are handled jointly below.
                    AudioData& bandTarget = hasRepeatedGroups
                        ? residualAudio
                        : audio;

                    const AudioQuality bandTargetQuality = analyzeQuality(bandTarget);
                    {
                        wostringstream info;
                        info << fixed << setprecision(3)
                            << L"[Diagnostics] Multiband target: "
                            << (hasRepeatedGroups ? L"residual only" : L"full signal")
                            << L" | peak " << bandTargetQuality.peakDb << L" dBFS"
                            << L" | RMS " << bandTargetQuality.rmsDb << L" dBFS"
                            << L" | sample rate " << bandTarget.sampleRate << L" Hz"
                            << L" | frames " << bandTarget.frameCount
                            << L" | working ceiling " << workingCeilingDb << L" dBFS";

                        if (bandTarget.channels == 0 ||
                            bandTarget.sampleRate < 40000 ||
                            bandTarget.frameCount == 0 ||
                            bandTarget.samples.size() !=
                                bandTarget.frameCount * bandTarget.channels)
                        {
                            info << L" | WARNING: invalid input for planBandReduction";
                            info << L" (channels=" << bandTarget.channels
                                << L", sampleRate=" << bandTarget.sampleRate
                                << L", frames=" << bandTarget.frameCount
                                << L", samples=" << bandTarget.samples.size() << L")";
                        }

                        progress.note(info.str());
                    }

                    BandReductionPlan bands = planBandReduction(
                        bandTarget,
                        workingCeilingDb,
                        settings.declipAttackMs,
                        settings.declipReferenceReleaseMs,
                        DECLIP_GRAPH_COLUMNS,
                        262144,
                        makeStageProgress(
                            10,
                            25,
                            L"Calculating multiband reduction"
                        ),
                        nullptr
                    );

                    {
                        size_t activeBands = 0;
                        for (size_t band = 0; band < BAND_COUNT; ++band)
                        {
                            if (bands.affectedFramesByBand[band] > 0)
                                ++activeBands;
                        }

                        double maximumBandReductionDb = 0.0;
                        for (double reductionDb : bands.maxReductionDb)
                            maximumBandReductionDb = min(maximumBandReductionDb, reductionDb);

                        wostringstream info;
                        info << fixed << setprecision(3)
                            << L"[Diagnostics] Multiband plan: affected frames "
                            << bands.affectedFrames
                            << L" | active bands " << activeBands << L"/" << BAND_COUNT
                            << L" | strongest reduction " << maximumBandReductionDb << L" dB";

                        if (bands.affectedFrames == 0)
                        {
                            info << L" | no band gain was reduced";

                            if (bandTargetQuality.peakDb <= workingCeilingDb + 0.001)
                                info << L" | target peak is at or below working ceiling";
                            else
                                info << L" | target peak exceeds ceiling: investigate band-plan analysis";
                        }

                        progress.note(info.str());
                    }

                    applyBandReduction(
                        bandTarget,
                        bands,
                        makeStageProgress(
                            35,
                            10,
                            L"Applying multiband reduction"
                        )
                    );

                    bands.chunks.clear();
                    bands.chunks.shrink_to_fit();

                    // Local peak reduction also changes only the residual.
                    PeakReductionPlan residual = planPeakReduction(
                        bandTarget,
                        workingCeilingDb,
                        settings.declipFadeMs,
                        DECLIP_GRAPH_COLUMNS,
                        makeStageProgress(
                            45,
                            10,
                            L"Calculating peak reduction"
                        ),
                        nullptr
                    );

                    applyPeakReduction(
                        bandTarget,
                        residual,
                        makeStageProgress(
                            55,
                            5,
                            L"Applying peak reduction"
                        )
                    );

                    // Report the two preliminary residual-reduction stages separately.
                    // These statistics are distinct from the later joint repeated/residual
                    // optimizer and must not be interpreted as its additional reduction.
                    double maximumBandReductionDb = 0.0;

                    for (double reductionDb : bands.maxReductionDb)
                    {
                        maximumBandReductionDb = min(
                            maximumBandReductionDb, reductionDb);
                    }

                    wostringstream preliminaryReductionInfo;
                    preliminaryReductionInfo << fixed << setprecision(2)
                        << (hasRepeatedGroups
                            ? L"Residual preliminary reduction: "
                            : L"Signal preliminary reduction: ")
                        << L"multiband " << bands.affectedFrames
                        << L" frames (max " << maximumBandReductionDb << L" dB)"
                        << L" | local peak " << residual.affectedFrames
                        << L" frames (max " << residual.maxReductionDb << L" dB)";
                    progress.note(preliminaryReductionInfo.str());

                    // Detailed distribution of multiband attenuation by frequency band.
                    // Counts are per audio frame, not per channel or per sample.
                    wostringstream bandReductionDetails;
                    bandReductionDetails
                        << L"Per-band reduction statistics:"
                        << L" frames | >=3 dB | >=6 dB | >=12 dB | at -18 dB"
                        << L" | mean | max";

                    for (size_t band = 0; band < BAND_COUNT; band++)
                    {
                        const std::uint64_t affected =
                            bands.affectedFramesByBand[band];

                        if (affected == 0)
                            continue;

                        const double meanReductionDb =
                            bands.sumReductionDb[band] /
                            static_cast<double>(affected);

                        const string bandLabel = getBandLabel(band);
                        const wstring wideBandLabel(
                            bandLabel.begin(),
                            bandLabel.end());

                        bandReductionDetails
                            << L"\n" << wideBandLabel
                            << L" | " << affected
                            << L" | " << bands.framesReducedAtLeast3Db[band]
                            << L" | " << bands.framesReducedAtLeast6Db[band]
                            << L" | " << bands.framesReducedAtLeast12Db[band]
                            << L" | " << bands.framesAtReductionLimit[band]
                            << L" | " << fixed << setprecision(2)
                            << meanReductionDb << L" dB"
                            << L" | " << bands.maxReductionDb[band] << L" dB";

                        const std::uint64_t episodes6 =
                            bands.episodesAtLeast6Db[band];
                        const std::uint64_t episodes12 =
                            bands.episodesAtLeast12Db[band];

                        if (episodes6 > 0 || episodes12 > 0)
                        {
                            const double total6Seconds =
                                bands.totalEpisodeFramesAtLeast6Db[band] /
                                bands.sampleRate;
                            const double average6Ms = episodes6 > 0
                                ? 1000.0 * bands.totalEpisodeFramesAtLeast6Db[band] /
                                    (episodes6 * bands.sampleRate)
                                : 0.0;
                            const double maximum6Ms =
                                1000.0 * bands.maxEpisodeFramesAtLeast6Db[band] /
                                bands.sampleRate;

                            const double total12Seconds =
                                bands.totalEpisodeFramesAtLeast12Db[band] /
                                bands.sampleRate;
                            const double average12Ms = episodes12 > 0
                                ? 1000.0 * bands.totalEpisodeFramesAtLeast12Db[band] /
                                    (episodes12 * bands.sampleRate)
                                : 0.0;
                            const double maximum12Ms =
                                1000.0 * bands.maxEpisodeFramesAtLeast12Db[band] /
                                bands.sampleRate;

                            bandReductionDetails
                                << L"\n    >=6 dB episodes: " << episodes6
                                << L" | total " << fixed << setprecision(2)
                                << total6Seconds << L" s"
                                << L" | average " << setprecision(1)
                                << average6Ms << L" ms"
                                << L" | max " << maximum6Ms << L" ms"
                                << L"\n    >=12 dB episodes: " << episodes12
                                << L" | total " << fixed << setprecision(2)
                                << total12Seconds << L" s"
                                << L" | average " << setprecision(1)
                                << average12Ms << L" ms"
                                << L" | max " << maximum12Ms << L" ms";
                        }
                    }

                    // Per-band detail is intentionally omitted from the
                    // console log; the compact plan summary above contains
                    // the aggregate information needed for format comparison.

                    // If no multiband event was recorded, use a short neutral
                    // excerpt around the strongest sample so the diagnostic graph
                    // can still be generated with zero band correction.
                    if (bands.eventBefore.size() < 2 &&
                        bandTarget.channels > 0 &&
                        bandTarget.frameCount > 1 &&
                        bandTarget.samples.size() ==
                            bandTarget.frameCount * bandTarget.channels)
                    {
                        const size_t frameCount =
                            static_cast<size_t>(bandTarget.frameCount);
                        size_t peakFrame = 0;
                        double peakMagnitude = 0.0;

                        for (size_t frame = 0; frame < frameCount; frame++)
                        {
                            const double magnitude = abs(static_cast<double>(
                                bandTarget.samples[frame * bandTarget.channels]));

                            if (magnitude > peakMagnitude)
                            {
                                peakMagnitude = magnitude;
                                peakFrame = frame;
                            }
                        }

                        const size_t eventHalf = max<size_t>(
                            200,
                            static_cast<size_t>(bands.sampleRate * 0.01));
                        const size_t eventStart =
                            peakFrame > eventHalf ? peakFrame - eventHalf : 0;
                        const size_t eventEnd = min(
                            frameCount,
                            peakFrame + eventHalf + 1);

                        bands.eventStart = eventStart;
                        bands.eventBefore.resize(eventEnd - eventStart);
                        bands.eventAfter.resize(eventEnd - eventStart);

                        for (size_t frame = eventStart; frame < eventEnd; frame++)
                        {
                            const float sample = bandTarget.samples[
                                frame * bandTarget.channels];

                            bands.eventBefore[frame - eventStart] = sample;
                            bands.eventAfter[frame - eventStart] = sample;
                        }
                    }

                    // Always save the bands diagnostic whenever this processing
                    // stage runs, even when no reduction was necessary.
                    saveBandReductionGraph(
                        bands,
                        residual,
                        workingCeilingDb,
                        job.graphBands,
                        graphTitle,
                        graphError
                    );

                    // Aggregate band reductions are already reported in the
                    // compact multiband plan summary above. Do not print all
                    // 128 frequency-band values to the console.

                    // Jointly optimize the shared repeated-group gain
                    // and local residual gain under the working peak ceiling.
                    if (hasRepeatedGroups)
                    {
                        const double ceiling = pow(10.0, workingCeilingDb / 20.0);
                        repeatedGroupReductionDb.assign(
                            repetitions.groupDetails.size(), 0.0);

                        size_t repeatedAttenuatedGroups = 0;
                        size_t residualOnlyGroups = 0;
                        size_t unchangedGroups = 0;
                        size_t smoothingAdjustedFrames = 0;
                        double maximumRepeatedReductionDb = 0.0;
                        double maximumResidualReductionDb = 0.0;

                        for (size_t groupIndex = 0;
                            groupIndex < repetitions.groupDetails.size();
                            ++groupIndex)
                        {
                            const RepetitionGroup& group =
                                repetitions.groupDetails[groupIndex];
                            RepeatedGroupOptimizationStats groupStats;
                            const double groupGain = optimizeRepeatedGroup(
                                repeatedProcessed, residualAudio, group,
                                ceiling, settings.repeatedPreservationPriority,
                                settings.declipFadeMs, groupStats);

                            repeatedGroupReductionDb[groupIndex] =
                                20.0 * log10(max(groupGain, 1e-12));

                            maximumRepeatedReductionDb = min(
                                maximumRepeatedReductionDb,
                                groupStats.repeatedGainDb);
                            maximumResidualReductionDb = min(
                                maximumResidualReductionDb,
                                groupStats.maximumResidualReductionDb);
                            smoothingAdjustedFrames +=
                                groupStats.smoothingAdjustedFrames;

                            const bool repeatedWasReduced =
                                groupStats.repeatedGainDb < -0.001;
                            const bool residualWasReduced =
                                groupStats.residualReducedFrames > 0;

                            if (repeatedWasReduced)
                                ++repeatedAttenuatedGroups;
                            else if (residualWasReduced)
                                ++residualOnlyGroups;
                            else
                                ++unchangedGroups;
                        }

                        wostringstream optimizationInfo;
                        optimizationInfo << fixed << setprecision(2)
                            << L"Repeated/residual optimization: "
                            << repetitions.groupDetails.size() << L" groups | "
                            << L"repeated attenuated: " << repeatedAttenuatedGroups
                            << L" (max " << maximumRepeatedReductionDb << L" dB) | "
                            << L"residual only: " << residualOnlyGroups
                            << L" (max " << maximumResidualReductionDb << L" dB) | "
                            << L"unchanged: " << unchangedGroups
                            << L" | smoothing constrained: "
                            << smoothingAdjustedFrames << L" frames";
                        progress.note(optimizationInfo.str());

                        for (size_t i = 0; i < audio.samples.size(); ++i)
                            audio.samples[i] =
                                repeatedProcessed.samples[i] +
                                residualAudio.samples[i];
                    }
                }
                else if (hasRepeatedGroups)
                {
                    progress.note(
                        L"Repeated/residual optimization skipped: "
                        L"no declipping runs and no samples above working ceiling"
                    );
                }

                if (repetitionReady && !repetitions.groupDetails.empty())
                {
                    saveRepeatedComponentsGraph(
                        repeatedProcessed,
                        repetitions,
                        repeatedGroupReductionDb,
                        job.graphRepeated,
                        graphTitle,
                        graphError
                    );
                }
            }

            repeatedComponents.samples.clear();
            repeatedComponents.samples.shrink_to_fit();
            residualAudio.samples.clear();
            residualAudio.samples.shrink_to_fit();
            repeatedProcessed.samples.clear();
            repeatedProcessed.samples.shrink_to_fit();

            // Проверка клиппинга (после восстановления, если оно включено)
            AudioQuality quality = analyzeQuality(audio);

            if (quality.clippedRuns > 0 || quality.peakDb > 0.0)
            {
                wostringstream warning;

                warning << fixed << setprecision(1)
                    << L"Before EQ: " << job.relative.wstring()
                    << L" | peak after declipping " << quality.peakDb << L" dBFS";

                if (quality.clippedRuns > 0)
                {
                    warning << L", clipping: " << quality.clippedRuns
                        << L" runs (" << quality.clippedSamples
                        << L" samples)";
                }

                progress.note(warning.str());
            }

            progress.update(index, 65, L"Analyzing spectrum", job.input);

            if (!calculateAverageSpectrum(audio, spectrum, error))
            {
                return false;
            }

            progress.update(index, 75, L"Calculating curve", job.input);

            if (!calculateEqualizerCurve(
                spectrum,
                selectedProfile,
                settings,
                correctionDb,
                requestedCorrectionDb,
                error))
            {
                return false;
            }

            saveEqualizerGraph(
                spectrum,
                correctionDb,
                requestedCorrectionDb,
                settings.hardLimitDb,
                job.graphCorrection,
                graphTitle,
                graphError
            );

            progress.update(index, 80, L"Applying equalizer", job.input);

            if (!applyEqualizer(
                audio,
                spectrum,
                correctionDb,
                outputAudio,
                error))
            {
                return false;
            }

            const AudioQuality beforeFinalLimiterQuality = analyzeQuality(outputAudio);

            {
                wostringstream info;
                info << fixed << setprecision(3)
                    << L"[Diagnostics] After EQ, before final limiter: peak "
                    << beforeFinalLimiterQuality.peakDb << L" dBFS"
                    << L" | RMS " << beforeFinalLimiterQuality.rmsDb << L" dBFS";
                progress.note(info.str());
            }

            const double finalPeakCeilingDb =
                settings.declip ? settings.declipCeilingDb : -1.0;

            const double finalGainReductionDb =
                limitPeaks(outputAudio, finalPeakCeilingDb);

            {
                const AudioQuality finalQuality = analyzeQuality(outputAudio);
                wostringstream info;
                info << fixed << setprecision(3)
                    << L"[Diagnostics] Final output: peak "
                    << finalQuality.peakDb << L" dBFS"
                    << L" | RMS " << finalQuality.rmsDb << L" dBFS"
                    << L" | final whole-track gain " << finalGainReductionDb << L" dB";
                progress.note(info.str());
            }

            if (finalGainReductionDb < -0.001)
            {
                wostringstream info;
                info << fixed << setprecision(2)
                    << L"Whole-track level reduction after EQ: "
                    << finalGainReductionDb << L" dB";
                progress.note(info.str());
            }

            // Освобождаем память исходного аудио
            audio = AudioData{};

            progress.update(index, 95, L"Analyzing result", job.input);

            if (calculateAverageSpectrum(
                outputAudio,
                spectrumAfter,
                graphError))
            {
                saveSpectrumComparisonGraph(
                    spectrum,
                    spectrumAfter,
                    selectedProfile,
                    job.graphSpectrum,
                    graphTitle,
                    graphError
                );
            }

            progress.update(index, 98, L"Saving", job.output);

            return saveAudio(job.output, outputAudio, error);
        };

    // Рабочий поток: берёт следующий файл, пока они есть
    auto worker = [&]()
        {
            while (true)
            {
                size_t index = nextIndex.fetch_add(1);

                if (index >= totalFiles)
                {
                    break;
                }

                const Job& job = jobs[index];

                string error;
                bool success = false;

                try
                {
                    success = processJob(index, job, error);
                }
                catch (const exception& e)
                {
                    error = e.what();
                }
                catch (...)
                {
                    error = "Unknown error.";
                }

                if (success)
                {
                    successfulFiles++;
                    progress.finished(index, job.output);
                }
                else
                {
                    failedFiles++;
                    progress.failedJob(index, job.input, error);
                }
            }
        };

    // Запуск пула потоков
    size_t threadCount = thread::hardware_concurrency() - 1;

    if (threadCount == 0)
    {
        threadCount = 2;
    }

    threadCount = min(threadCount, totalFiles);

    wcout << L"Threads: " << threadCount << endl << endl;

    vector<thread> threads;

    const auto processingStart = chrono::steady_clock::now();

    for (size_t i = 0; i < threadCount; i++)
    {
        threads.emplace_back(worker);
    }

    for (thread& t : threads)
    {
        t.join();
    }

    failedFiles += skippedFiles;

    const auto processingEnd = chrono::steady_clock::now();
    const auto elapsedMilliseconds =
        chrono::duration_cast<chrono::milliseconds>(
            processingEnd - processingStart
        ).count();

    const long long hours = elapsedMilliseconds / 3600000;
    const long long minutes = (elapsedMilliseconds / 60000) % 60;
    const long long seconds = (elapsedMilliseconds / 1000) % 60;
    const long long milliseconds = elapsedMilliseconds % 1000;

    wcout << endl << endl;
    wcout << L"Processing finished." << endl;
    wcout << L"Total elapsed time: "
        << setfill(L'0') << setw(2) << hours << L":"
        << setw(2) << minutes << L":"
        << setw(2) << seconds << L"."
        << setw(3) << milliseconds
        << setfill(L' ') << endl;
    wcout << L"Successfully processed: "
        << successfulFiles << endl;
    wcout << L"Failed: "
        << failedFiles << endl;
    wcout << L"Output folder: "
        << outputDirectory.wstring() << endl;

    return failedFiles == 0 ? 0 : 1;
}


