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
            intensity = 1.0;
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
        L"  Peak ceiling after restoration, dBFS (Enter = -1): ",
        -1.0, -12.0, 0.0,
        settings.declipCeilingDb))
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
        tag << L"_declip" << settings.declipCeilingDb << L"dB_"
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

        wcout << L"\r" << output;

        if (previousLength > output.length())
        {
            wcout << wstring(previousLength - output.length(), L' ');
        }

        previousLength = output.length();

        wcout << L"\r" << output << flush;
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

    // Информационное сообщение на отдельной строке
    void note(const wstring& text)
    {
        lock_guard<mutex> guard(lock);

        wcout << L"\r" << wstring(previousLength, L' ') << L"\r";
        wcout << text << endl;

        previousLength = 0;
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
            << L" dBFS, residual fade " << settings.declipFadeMs
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
        job.graphCorrection += L".03_correction.svg";

        job.graphSpectrum = graphsDirectory / relativePath;
        job.graphSpectrum += L".04_spectrum.svg";

        job.graphDeclip = graphsDirectory / relativePath;
        job.graphDeclip += L".02_declip.svg";

        job.graphBands = graphsDirectory / relativePath;
        job.graphBands += L".01_bands.svg";

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
                titleStream << " | declip " << settings.declipCeilingDb << " dB, residual fade "
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

            AudioData repeatedComponents;
            AudioData residualAudio;
            AudioData repeatedProcessed;

            // Восстановление срезанных пиков (опционально, до анализа спектра)
            if (settings.declip)
            {
                progress.update(index, 5, L"Finding repeated components", job.input);

                // Копия исходного аудио нужна только для графика сравнения
                AudioData originalAudio = audio;

                RepetitionResult repetitions;
                RepetitionSettings repetitionSettings;
                string repetitionError;

                progress.clearLine();

                const bool repetitionReady = separateRepeatedComponents(
                    originalAudio,
                    repeatedComponents,
                    residualAudio,
                    repetitionSettings,
                    repetitions,
                    repetitionError
                );

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
                    repetitionReady ? &repetitions : nullptr
                );

                const bool hasRepeatedGroups =
                    repetitionReady && repetitions.groups > 0 &&
                    !repetitions.groupDetails.empty();

                vector<uint8_t> repeatedFrames(
                    static_cast<size_t>(audio.frameCount), 0);

                if (hasRepeatedGroups)
                {
                    repeatedProcessed = std::move(repeatedComponents);

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

                const double peakCeiling =
                    pow(10.0, settings.declipCeilingDb / 20.0);

                bool mixExceedsCeiling = false;
                for (float sample : audio.samples)
                {
                    if (abs(static_cast<double>(sample)) > peakCeiling)
                    {
                        mixExceedsCeiling = true;
                        break;
                    }
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

                    // First attenuate each repeated sound as a whole. The
                    // same scalar gain is used for every occurrence and channel.
                    // All gains are calculated before any repeated samples are changed.
                    if (hasRepeatedGroups)
                    {
                        const double ceiling = pow(
                            10.0, settings.declipCeilingDb / 20.0);

                        vector<double> groupGains(
                            repetitions.groupDetails.size(), 1.0);

                        for (size_t groupIndex = 0;
                            groupIndex < repetitions.groupDetails.size();
                            ++groupIndex)
                        {
                            const RepetitionGroup& group =
                                repetitions.groupDetails[groupIndex];

                            double groupGain = 1.0;

                            for (const RepetitionOccurrence& occurrence : group.occurrences)
                            {
                                if (occurrence.start >= audio.frameCount)
                                    continue;

                                const size_t endFrame = min(
                                    static_cast<size_t>(audio.frameCount),
                                    occurrence.start + group.length);

                                for (size_t frame = occurrence.start;
                                    frame < endFrame; ++frame)
                                {
                                    for (size_t channel = 0;
                                        channel < audio.channels; ++channel)
                                    {
                                        const size_t sampleIndex =
                                            frame * audio.channels + channel;

                                        const double repeatedValue =
                                            repeatedProcessed.samples[sampleIndex];
                                        const double residualValue =
                                            residualAudio.samples[sampleIndex];
                                        const double mixed =
                                            repeatedValue + residualValue;

                                        // Reduce the repeated component only
                                        // when it pushes an otherwise safe
                                        // residual farther beyond the ceiling.
                                        if (mixed > ceiling &&
                                            repeatedValue > 1e-12)
                                        {
                                            const double allowed =
                                                (ceiling - residualValue) /
                                                repeatedValue;

                                            if (allowed >= 0.0)
                                            {
                                                groupGain = min(
                                                    groupGain, allowed);
                                            }
                                        }
                                        else if (mixed < -ceiling &&
                                            repeatedValue < -1e-12)
                                        {
                                            const double allowed =
                                                (-ceiling - residualValue) /
                                                repeatedValue;

                                            if (allowed >= 0.0)
                                            {
                                                groupGain = min(
                                                    groupGain, allowed);
                                            }
                                        }
                                    }
                                }
                            }

                            groupGains[groupIndex] =
                                clamp(groupGain, 0.0, 1.0);
                        }

                        // Apply each group's gain uniformly to all of its
                        // occurrences. The detector rejects overlapping groups;
                        // this pass also avoids multiplying gains during analysis.
                        for (size_t groupIndex = 0;
                            groupIndex < repetitions.groupDetails.size();
                            ++groupIndex)
                        {
                            const double groupGain = groupGains[groupIndex];

                            if (groupGain >= 0.999999)
                                continue;

                            const RepetitionGroup& group =
                                repetitions.groupDetails[groupIndex];

                            for (const RepetitionOccurrence& occurrence : group.occurrences)
                            {
                                if (occurrence.start >= audio.frameCount)
                                    continue;

                                const size_t endFrame = min(
                                    static_cast<size_t>(audio.frameCount),
                                    occurrence.start + group.length);

                                for (size_t frame = occurrence.start;
                                    frame < endFrame; ++frame)
                                {
                                    for (size_t channel = 0;
                                        channel < audio.channels; ++channel)
                                    {
                                        repeatedProcessed.samples[
                                            frame * audio.channels + channel] *=
                                            static_cast<float>(groupGain);
                                    }
                                }
                            }
                        }

                        // Refresh the mix before residual-only multiband analysis.
                        for (size_t i = 0; i < audio.samples.size(); ++i)
                        {
                            audio.samples[i] =
                                repeatedProcessed.samples[i] +
                                residualAudio.samples[i];
                        }
                    }

                    // Peak detection sees the full mix; the reducers modify
                    // only the non-repeating residual.
                    AudioData& bandTarget = hasRepeatedGroups
                        ? residualAudio
                        : audio;

                    const AudioData* fixedComponents = hasRepeatedGroups
                        ? &repeatedProcessed
                        : nullptr;

                    BandReductionPlan bands = planBandReduction(
                        bandTarget,
                        settings.declipCeilingDb,
                        settings.declipAttackMs,
                        settings.declipReferenceReleaseMs,
                        DECLIP_GRAPH_COLUMNS,
                        262144,
                        makeStageProgress(
                            10,
                            25,
                            L"Calculating multiband reduction"
                        ),
                        fixedComponents
                    );

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
                        settings.declipCeilingDb,
                        settings.declipFadeMs,
                        DECLIP_GRAPH_COLUMNS,
                        makeStageProgress(
                            45,
                            10,
                            L"Calculating peak reduction"
                        ),
                        fixedComponents
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

                    if (bands.affectedFrames > 0 || !residual.regions.empty())
                    {
                        saveBandReductionGraph(
                            bands,
                            residual,
                            settings.declipCeilingDb,
                            job.graphBands,
                            graphTitle,
                            graphError
                        );

                        wostringstream info;

                        info << fixed << setprecision(1)
                            << L"Peaks lowered by band: " << job.relative.wstring()
                            << L" |";

                        for (size_t b = 0; b < BAND_COUNT; b++)
                        {
                            if (bands.maxReductionDb[b] < -0.05)
                            {
                                const string label = getBandLabel(b);

                                info << L" " << wstring(label.begin(), label.end())
                                    << L" Hz " << bands.maxReductionDb[b] << L" dB;";
                            }
                        }

                        if (residual.maxReductionDb < -0.05)
                        {
                            info << L" common " << residual.maxReductionDb << L" dB";
                        }

                        progress.note(info.str());
                    }
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

            const double finalGainReductionDb =
                limitPeaks(outputAudio, -1.0);

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


