#include "cpm/progress.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace cpm {

ProgressDisplay::ProgressDisplay() = default;

ProgressDisplay::~ProgressDisplay() { stop(); }

int ProgressDisplay::add_task(const std::string &name) {
    std::scoped_lock lock(mutex_);
    tasks_.push_back({.name = name, .status = TaskStatus::Pending, .detail = ""});
    return tasks_.size() - 1;
}

void ProgressDisplay::set_status(int task_id, TaskStatus status, const std::string &detail) {
    std::scoped_lock lock(mutex_);
    if (task_id >= 0 && std::cmp_less(task_id, tasks_.size())) {
        tasks_[task_id].status = status;
        tasks_[task_id].detail = detail;
    }
}

void ProgressDisplay::start() {
    running_ = true;
    display_thread_ = std::thread([this]() {
        while (running_) {
            render();
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
    });
}

void ProgressDisplay::stop() {
    running_ = false;
    if (display_thread_.joinable()) {
        display_thread_.join();
    }
    // Final render to show completed state
    render();
    std::cout << '\n';
}

void ProgressDisplay::print(const std::string &msg) {
    std::scoped_lock lock(mutex_);
    // Clear current line and print
    std::cout << "\r\033[K" << msg << "\n" << std::flush;
}

std::string ProgressDisplay::status_icon(TaskStatus status) const {
    static int frame = 0;
    static const char *spinner[] = {"◐", "◓", "◑", "◒"};

    switch (status) {
    case TaskStatus::Pending:
        return "○";
    case TaskStatus::Downloading:
        return spinner[(frame++) % 4];
    case TaskStatus::Building:
        return spinner[(frame++) % 4];
    case TaskStatus::Done:
        return "\033[32m✓\033[0m";
    case TaskStatus::Failed:
        return "\033[31m✗\033[0m";
    case TaskStatus::Cached:
        return "\033[33m●\033[0m";
    }
    return "?";
}

std::string ProgressDisplay::status_text(TaskStatus status) const {
    switch (status) {
    case TaskStatus::Pending:
        return "waiting";
    case TaskStatus::Downloading:
        return "downloading";
    case TaskStatus::Building:
        return "building";
    case TaskStatus::Done:
        return "done";
    case TaskStatus::Failed:
        return "failed";
    case TaskStatus::Cached:
        return "cached";
    }
    return "";
}

void ProgressDisplay::render() {
    std::scoped_lock lock(mutex_);
    if (tasks_.empty()) return;

    // Count active tasks
    int active = 0;
    int done = 0;
    int total = tasks_.size();

    for (const auto &t : tasks_) {
        if (t.status == TaskStatus::Done || t.status == TaskStatus::Cached) done++;
        if (t.status == TaskStatus::Downloading || t.status == TaskStatus::Building) active++;
    }

    // Build status line showing active tasks
    std::string line = "\r\033[K";

    // Show currently active tasks inline
    bool first = true;
    for (const auto &t : tasks_) {
        if (t.status == TaskStatus::Downloading || t.status == TaskStatus::Building) {
            if (!first) line += "  ";
            line += status_icon(t.status) + " " + t.name;
            if (!t.detail.empty()) line += " (" + t.detail + ")";
            first = false;
        }
    }

    if (active == 0 && done < total) {
        line += "○ waiting...";
    }

    // Show progress count
    line += "  [" + std::to_string(done) + "/" + std::to_string(total) + "]";

    std::cout << line << std::flush;
}

// ─── Parallel execution ───

void parallel_execute(const std::vector<std::function<void()>> &tasks, int max_parallel) {
    std::vector<std::thread> threads;
    std::atomic<int> next_task{0};
    int total = tasks.size();

    int num_threads = std::min(max_parallel, total);

    threads.reserve(num_threads);
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&tasks, &next_task, total]() {
            while (true) {
                int idx = next_task.fetch_add(1);
                if (idx >= total) break;
                tasks[idx]();
            }
        });
    }

    for (auto &t : threads) {
        t.join();
    }
}

} // namespace cpm
