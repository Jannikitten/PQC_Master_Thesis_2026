#include "../include/NetworkExecutor.h"

#include <exception>

#include <spdlog/spdlog.h>

namespace Safira {

NetworkExecutor::~NetworkExecutor() {
    Stop();
}

bool NetworkExecutor::Start(std::string_view name) {
    std::lock_guard lock(m_Mutex);
    if (m_Worker.joinable())
        return false;

    m_Name = std::string(name);
    m_AcceptingTasks = true;
    m_StopRequested  = false;
    m_DrainOnStop    = false;
    m_Tasks.clear();

    m_Worker = std::thread(&NetworkExecutor::ThreadMain, this);
    return true;
}

bool NetworkExecutor::Post(Task task) {
    if (!task)
        return false;

    {
        std::lock_guard lock(m_Mutex);
        if (!m_AcceptingTasks || !m_Worker.joinable())
            return false;
        m_Tasks.push_back(std::move(task));
    }

    m_Cv.notify_one();
    return true;
}

void NetworkExecutor::Stop(bool drain) {
    {
        std::lock_guard lock(m_Mutex);
        if (!m_Worker.joinable())
            return;

        m_AcceptingTasks = false;
        m_StopRequested  = true;
        m_DrainOnStop    = drain;

        if (!m_DrainOnStop)
            m_Tasks.clear();
    }

    m_Cv.notify_all();

    if (RunsTasksOnCurrentThread())
        return;

    if (m_Worker.joinable())
        m_Worker.join();
}

bool NetworkExecutor::RunsTasksOnCurrentThread() const noexcept {
    std::lock_guard lock(m_Mutex);
    return m_Worker.joinable() && m_WorkerId == std::this_thread::get_id();
}

void NetworkExecutor::ThreadMain() {
    {
        std::lock_guard lock(m_Mutex);
        m_WorkerId = std::this_thread::get_id();
        m_Running.store(true, std::memory_order_release);
    }

    while (true) {
        Task next;
        {
            std::unique_lock lock(m_Mutex);
            m_Cv.wait(lock, [this] {
                return m_StopRequested || !m_Tasks.empty();
            });

            if (!m_Tasks.empty()) {
                next = std::move(m_Tasks.front());
                m_Tasks.pop_front();
            } else if (m_StopRequested) {
                break;
            }
        }

        try {
            if (next)
                next();
        } catch (const std::exception& ex) {
            if (m_Name.empty())
                spdlog::error("[network-executor] task threw exception: {}", ex.what());
            else
                spdlog::error("[network-executor:{}] task threw exception: {}", m_Name, ex.what());
        } catch (...) {
            if (m_Name.empty())
                spdlog::error("[network-executor] task threw unknown exception");
            else
                spdlog::error("[network-executor:{}] task threw unknown exception", m_Name);
        }

        bool shouldExit = false;
        {
            std::lock_guard lock(m_Mutex);
            if (m_StopRequested) {
                if (!m_DrainOnStop || m_Tasks.empty())
                    shouldExit = true;
            }
        }
        if (shouldExit)
            break;
    }

    {
        std::lock_guard lock(m_Mutex);
        m_AcceptingTasks = false;
        m_StopRequested  = false;
        m_DrainOnStop    = false;
        m_WorkerId       = {};
        m_Tasks.clear();
        m_Running.store(false, std::memory_order_release);
    }
}

} // namespace Safira
