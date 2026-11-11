
#ifndef REPETITION_H
#define REPETITION_H

#include "audio.h"

#include <cstddef>
#include <string>

struct RepetitionSettings
{
    // Допустимая длительность повторяющегося фрагмента
    size_t minFragmentMs = 50;
    size_t maxFragmentMs = 200;

    // Шаг первоначального поиска
    size_t searchHopMs = 10;

    // Минимальная степень сходства
    double minCorrelation = 0.98;

    // Допустимое изменение амплитуды
    double minGain = 0.25;
    double maxGain = 4.0;

    // Минимальное количество совпадающих вхождений
    size_t minOccurrences = 2;
};

struct RepetitionResult
{
    size_t groups = 0;
    size_t occurrences = 0;
};

// Разделяет исходную запись на повторяющиеся компоненты
// и остаточный сигнал. Сам исходный AudioData не изменяется.
bool separateRepeatedComponents(
    const AudioData& input,
    AudioData& repeated,
    AudioData& residual,
    const RepetitionSettings& settings,
    RepetitionResult& result,
    std::string& error
);

#endif
