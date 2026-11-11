#ifndef EQUALIZER_H
#define EQUALIZER_H

#include <vector>
#include <string>
#include <filesystem>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <array>
#include <cmath>

using ProgressCallback = std::function<void(int)>;

#include "fft.h"
#include "profiles.h"

// Параметры эквализации
struct EqualizerSettings
{
    double intensity = 1.0;     // по умолчанию применяется 100% коррекции
    double softLimitDb = 6.0;   // мягкая граница: до неё коррекция линейна
    double hardLimitDb = 12.0;   // жёсткая граница: коррекция не превышает её

    bool declip = false;            // восстановление срезанных пиков
    double declipCeilingDb = -1.0;  // целевой конечный предел пика, dBFS
    double declipOvershootDb = 2.0;  // допустимое превышение при локальной обработке, dB
    double repeatedPreservationPriority = 50.0; // 0 = residual, 100 = repeated
    double declipFadeMs = 40.0;     // длительность плавного спада общей коррекции, мс
    double declipAttackMs = 5.0;    // Attack многополосного снижения, мс
    double declipReferenceReleaseMs = 100.0; // Release на опорной частоте 1000 Гц, мс
};

bool calculateEqualizerCurve(
    const Spectrum& spectrum,
    NoiseProfile profile,
    const EqualizerSettings& settings,
    std::vector<double>& correctionDb,
    std::vector<double>& requestedCorrectionDb,
    std::string& error
);

bool saveEqualizerCurve(
    const Spectrum& spectrum,
    const std::vector<double>& correctionDb,
    const std::vector<double>& requestedCorrectionDb,
    const std::string& filename,
    std::string& error
);

bool saveEqualizerGraph(
    const Spectrum& spectrum,
    const std::vector<double>& correctionDb,
    const std::vector<double>& requestedCorrectionDb,
    double hardLimitDb,
    const std::filesystem::path& filename,
    const std::string& title,
    std::string& error
);

bool applyEqualizer(
    const AudioData& inputAudio,
    const Spectrum& spectrum,
    const std::vector<double>& correctionDb,
    AudioData& outputAudio,
    std::string& error
);

bool saveSpectrumComparisonGraph(
    const Spectrum& before,
    const Spectrum& after,
    NoiseProfile profile,
    const std::filesystem::path& filename,
    const std::string& title,
    std::string& error
);

// Нормализация пиков: при превышении порога уровень снижается целиком
double limitPeaks(
    AudioData& audio,
    double ceilingDb = -1.0
);

struct AudioQuality
{
    double peakDb = -200.0;           // пик, dBFS
    double rmsDb = -200.0;            // средний уровень, dBFS
    double crestDb = 0.0;             // пик минус RMS
    double dcOffset = 0.0;            // постоянная составляющая
    std::uint64_t clippedRuns = 0;    // число серий срезанных отсчётов
    std::uint64_t clippedSamples = 0; // число срезанных отсчётов
};

AudioQuality analyzeQuality(
    const AudioData& audio,
    std::size_t minRunLength = 3
);

// Одна восстановленная серия срезанных отсчётов
struct DeclipSegment
{
    std::size_t channel = 0;
    std::uint64_t first = 0;    // индекс первого отсчёта (кадра)
    std::uint64_t length = 0;   // длина серии в отсчётах
};

struct DeclipResult
{
    std::uint64_t runs = 0;
    std::uint64_t samples = 0;
    std::uint64_t skipped = 0;
    std::uint64_t repeatedGroupsWithCleanReference = 0;
    std::uint64_t repeatedGroupsWithoutCleanReference = 0;
    std::uint64_t repeatedOccurrencesProtected = 0;
    std::uint64_t repeatedOccurrencesReconstructed = 0;
    std::uint64_t repeatedRuns = 0;
    std::uint64_t repeatedSamples = 0;
    std::uint64_t legacyDetectedRuns = 0;
    std::uint64_t legacyRestoredRuns = 0;
    std::uint64_t legacyOvershootRejectedRuns = 0;
    std::uint64_t legacySkippedRuns = 0;
    std::vector<DeclipSegment> segments;
};

// Восстановление срезанных пиков во временной области.
// Если переданы результаты разделения повторяющихся компонентов,
// чистые вхождения защищаются, а клипующие восстанавливаются по ним.
struct RepetitionResult;

DeclipResult declipAudio(
    AudioData& audio,
    size_t minRunLength,
    size_t maxRunLength,
    const ProgressCallback& progress = {},
    const AudioData* repeatedComponents = nullptr,
    const AudioData* residualAudio = nullptr,
    const RepetitionResult* repetitions = nullptr);

// План локального снижения громкости: каждый кадр выше потолка (после восстановления
// пиков) требует усиления меньше единицы. Кривая усиления строится плавной,
// поэтому тише становится событие целиком (нарастание, пик и спад),
// а остальной трек не меняется.
struct PeakReductionPlan
{
    struct Region
    {
        std::uint64_t start = 0;        // первый кадр участка
        std::vector<float> gain;        // линейное усиление для каждого кадра участка
    };

    std::vector<Region> regions;
    std::uint64_t affectedFrames = 0;   // кадры с усилением меньше единицы
    double maxReductionDb = 0.0;        // наибольшее снижение, dB (<= 0)
    std::vector<float> columnGainDb;    // минимальное усиление по столбцам графика, dB
};

// Число столбцов графика во временной области (используется для панели усиления)
constexpr std::size_t DECLIP_GRAPH_COLUMNS = 356;

PeakReductionPlan planPeakReduction(
    const AudioData& audio,
    double ceilingDb,
    double fadeMs,
    size_t graphColumns,
    const ProgressCallback& progress = {},
    const AudioData* fixedComponents = nullptr);

void applyPeakReduction(
    AudioData& audio,
    const PeakReductionPlan& plan,
    const ProgressCallback& progress = {});

// ---------- Многополосное снижение громкости ----------
// Громкость снижается только в тех полосах частот, которые создали выброс.
// Сигнал делится на полосы фильтрами с нулевой фазой (сумма полос в точности равна
// исходному сигналу), для каждой полосы строится собственная плавная кривая усиления.

constexpr std::size_t BAND_COUNT = 128;

// 127 логарифмически распределённых границ от 20 Гц до 20 кГц.
inline const std::array<double, BAND_COUNT - 1> BAND_EDGES_HZ = []()
    {
        std::array<double, BAND_COUNT - 1> edges{};

        const double minFrequency = 20.0;
        const double maxFrequency = 20000.0;

        const double logMin = std::log(minFrequency);
        const double logMax = std::log(maxFrequency);

        for (std::size_t i = 0; i < edges.size(); i++)
        {
            const double t = static_cast<double>(i) /
                static_cast<double>(edges.size() - 1);

            edges[i] = std::exp(logMin + t * (logMax - logMin));
        }

        return edges;
    }();

// Подпись полосы формируется по её фактическим границам.
std::string getBandLabel(std::size_t band);

struct BandReductionPlan
{
    // Обработанный участок: значения отсчётов после снижения (с чередованием каналов)
    struct Chunk
    {
        std::uint64_t start = 0;
        std::vector<float> samples;
    };

    std::vector<Chunk> chunks;
    std::uint64_t affectedFrames = 0;           // кадры, в которых снижена хотя бы одна полоса
    std::vector<double> maxReductionDb;         // наибольшее снижение по полосам, dB (<= 0)
    std::vector<std::uint64_t> affectedFramesByBand;
    std::vector<std::uint64_t> framesReducedAtLeast3Db;
    std::vector<std::uint64_t> framesReducedAtLeast6Db;
    std::vector<std::uint64_t> framesReducedAtLeast12Db;
    std::vector<std::uint64_t> framesAtReductionLimit;
    std::vector<double> sumReductionDb;

    // Continuous attenuation episodes, measured in audio frames.
    std::vector<std::uint64_t> episodesAtLeast6Db;
    std::vector<std::uint64_t> totalEpisodeFramesAtLeast6Db;
    std::vector<std::uint64_t> maxEpisodeFramesAtLeast6Db;
    std::vector<std::uint64_t> episodesAtLeast12Db;
    std::vector<std::uint64_t> totalEpisodeFramesAtLeast12Db;
    std::vector<std::uint64_t> maxEpisodeFramesAtLeast12Db;
    std::vector<std::vector<float>> columnGainDb;  // [полоса][столбец графика], dB

    // Самое сильное событие (первый канал): фрагмент до и после снижения
    std::uint64_t eventStart = 0;
    std::vector<float> eventBefore;
    std::vector<float> eventAfter;

    double sampleRate = 0.0;
    double durationSeconds = 0.0;
};

BandReductionPlan planBandReduction(
    const AudioData& audio,
    double ceilingDb,
    double attackMs,
    double referenceReleaseMs,
    size_t graphColumns,
    size_t chunkFrames,
    const ProgressCallback& progress = {},
    const AudioData* fixedComponents = nullptr);

void applyBandReduction(
    AudioData& audio,
    const BandReductionPlan& plan,
    const ProgressCallback& progress = {});

// График многополосного снижения: усиление по полосам во времени
// и сильнейшее событие до и после обработки
bool saveBandReductionGraph(
    const BandReductionPlan& bands,
    const PeakReductionPlan& residual,
    double ceilingDb,
    const std::filesystem::path& filename,
    const std::string& title,
    std::string& error
);

bool saveRepeatedComponentsGraph(
    const AudioData& repeatedAudio,
    const RepetitionResult& repetitions,
    const std::vector<double>& groupReductionDb,
    const std::filesystem::path& filename,
    const std::string& title,
    std::string& error
);

// График во временной области: обзор трека и увеличенный фрагмент
// с самым заметным восстановленным пиком (before - исходный, after - восстановленный)
bool saveDeclipGraph(
    const AudioData& before,
    const AudioData& after,
    const DeclipResult& result,
    const std::filesystem::path& filename,
    const std::string& title,
    std::string& error
);

#endif