#include "profiles.h"
#include <cmath>

const double REFERENCE_FREQUENCY = 1000.0;

// Расчёт целевого уровня мощности
double getTargetPowerDb(
    double frequency,
    NoiseProfile profile)
{
    if (frequency <= 0.0)
    {
        return 0.0;
    }

    double beta = 0.0;

    switch (profile)
    {
    case NoiseProfile::WHITE:
        beta = 0.0;
        break;

    case NoiseProfile::PINK:
        beta = 1.0;
        break;

    case NoiseProfile::BROWN:
        beta = 2.0;
        break;
    }

    return -10.0 * beta *
        log10(frequency / REFERENCE_FREQUENCY);
}

// Получение названия профиля
std::string getProfileName(NoiseProfile profile)
{
    switch (profile)
    {
    case NoiseProfile::WHITE:
        return "White Noise";

    case NoiseProfile::PINK:
        return "Pink Noise";

    case NoiseProfile::BROWN:
        return "Brown Noise";
    }

    return "Unknown";
}