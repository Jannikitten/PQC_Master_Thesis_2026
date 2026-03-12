#ifndef PQC_MASTER_THESIS_2026_NETWORKEXECUTOR_H
#define PQC_MASTER_THESIS_2026_NETWORKEXECUTOR_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace Safira {

    // Dedicated single-thread executor for network work.
    // Tasks are executed serially on one thread to keep TLS/DTLS I/O confined.
    class NetworkExecutor {
    public:
        using Task = std::function<void()>;

        NetworkExecutor() = default;
        ~NetworkExecutor();

        NetworkExecutor(const NetworkExecutor&)            = delete;
        NetworkExecutor& operator=(const NetworkExecutor&) = delete;
        NetworkExecutor(NetworkExecutor&&)                 = delete;
        NetworkExecutor& operator=(NetworkExecutor&&)      = delete;

        [[nodiscard]] bool Start(std::string_view name = {});
        [[nodiscard]] bool Post(Task task);
        void Stop(bool drain = false);

        [[nodiscard]] bool IsRunning() const noexcept {
            return m_Running.load(std::memory_order_acquire);
        }

        [[nodiscard]] bool RunsTasksOnCurrentThread() const noexcept;

    private:
        void ThreadMain();

        mutable std::mutex      m_Mutex;
        std::condition_variable m_Cv;
        std::deque<Task>        m_Tasks;
        std::thread             m_Worker;
        std::thread::id         m_WorkerId {};

        std::string m_Name;
        bool        m_AcceptingTasks = false;
        bool        m_StopRequested  = false;
        bool        m_DrainOnStop    = false;

        std::atomic<bool> m_Running { false };
    };

} // namespace Safira

#endif // PQC_MASTER_THESIS_2026_NETWORKEXECUTOR_H
