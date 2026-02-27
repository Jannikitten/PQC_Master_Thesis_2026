#ifndef PQC_MASTER_THESIS_2026_TIMER_H
#define PQC_MASTER_THESIS_2026_TIMER_H

#include <iostream>
#include <string>
#include <chrono>

namespace Safira {
    class Timer {
    public:
        Timer() {
            Reset();
        }

        void Reset() {
            m_Start = std::chrono::high_resolution_clock::now();
        }

        [[nodiscard]] float Elapsed() const {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now() - m_Start).count()
                * 0.001f * 0.001f * 0.001f;
        }

        [[nodiscard]] float ElapsedMillis() const {
            return Elapsed() * 1000.0f;
        }

    private:
        std::chrono::time_point<std::chrono::high_resolution_clock> m_Start;
    };

    class ScopedTimer {
    public:
        explicit ScopedTimer(const std::string& name)
            : m_Name(name) {}

        ~ScopedTimer() {
            const float time = m_Timer.ElapsedMillis();
            std::cout << "[TIMER] " << m_Name << " - " << time << "ms\n";
        }
    private:
        std::string m_Name;
        Timer m_Timer;
    };
}

#endif //PQC_MASTER_THESIS_2026_TIMER_H