#ifndef PROFILES_H
#define PROFILES_H

#include <string>

enum class NoiseProfile
{
    WHITE,
    PINK,
    BROWN
};

double getTargetPowerDb(
    double frequency,
    NoiseProfile profile
);

std::string getProfileName(NoiseProfile profile);

#endif