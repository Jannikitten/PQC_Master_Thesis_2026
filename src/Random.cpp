//
// Created by Jannik Verdoner on 26/02/2026.
//

#include "Random.h"

namespace Safira {
    std::mt19937 Random::s_RandomEngine;
    std::uniform_int_distribution<std::mt19937::result_type> Random::s_Distribution;
}