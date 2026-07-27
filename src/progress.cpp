#include "cpm/progress.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace cpm {

namespace ansi {
static constexpr const char *reset = "\033[0m";
static constexpr const char *green = "\033[32m";
static constexpr const char *yellow = "\033[33m";
static constexpr const char *red = "\033[31m";
static constexpr const char *bold = "\033[1m";
static constexpr const char *dim = "\033[2m";
static constexpr const char *cyan = "\033[36m";
static constexpr const char *cr_erase = "\r\033[K";
} // namespace ansi

static std::string elapsed_str(std::chrono::steady_clock::time_point start) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    std::ostringstream ss;
    if (ms < 1000) {
        ss << ms << "ms";
    } else {
        ss << std::fixed << std::setprecision(1) << (ms / 1000.0) << "s";
    }
    return ss.str();
}


ProgressDisplay::ProgressDisplay() = default;
ProgressDisplay::~ProgressDisplay() { stop(); }

int ProgressDisplay::add_task(const std::string &name) {
    std::scoped_lock lock(mutex_);
    tasks_.push_back({.name = name, .status = TaskStatus::Pending, .detail = ""});
    return static_cast<int>(tasks_.size()) - 1;
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
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
        }
    });
}

void ProgressDisplay::stop() {
    running_ = false;
    if (display_thread_.joinable()) display_thread_.join();

    std::scoped_lock lock(mutex_);
    std::cout << ansi::cr_erase;
    for (const auto &t : tasks_) {
        switch (t.status) {
        case TaskStatus::Done:
            std::cout << ansi::green << "  ✓ " << ansi::reset << t.name << "\n";
            break;
        case TaskStatus::Cached:
            std::cout << ansi::yellow << "  ● " << ansi::reset << t.name << ansi::dim << " (cached)" << ansi::reset << "\n";
            break;
        case TaskStatus::Failed:
            std::cout << ansi::red << "  ✗ " << ansi::reset << t.name << "\n";
            break;
        default:
            break;
        }
    }
}

void ProgressDisplay::print(const std::string &msg) {
    std::scoped_lock lock(mutex_);
    std::cout << ansi::cr_erase << msg << "\n" << std::flush;
}

std::string ProgressDisplay::status_icon(TaskStatus status) {
    static const char *spinner[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
    static int frame = 0;
    switch (status) {
    case TaskStatus::Pending:
        return std::string(ansi::dim) + "○" + ansi::reset;
    case TaskStatus::Downloading:
        return std::string(ansi::cyan) + spinner[(frame++) % 10] + ansi::reset;
    case TaskStatus::Building:
        return std::string(ansi::yellow) + spinner[(frame++) % 10] + ansi::reset;
    case TaskStatus::Done:
        return std::string(ansi::green) + "✓" + ansi::reset;
    case TaskStatus::Failed:
        return std::string(ansi::red) + "✗" + ansi::reset;
    case TaskStatus::Cached:
        return std::string(ansi::yellow) + "●" + ansi::reset;
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

    int active = 0, done = 0;
    int total = static_cast<int>(tasks_.size());

    for (const auto &t : tasks_) {
        if (t.status == TaskStatus::Done || t.status == TaskStatus::Cached) ++done;
        if (t.status == TaskStatus::Downloading || t.status == TaskStatus::Building) ++active;
    }

    std::string line = ansi::cr_erase;

    bool first = true;
    for (const auto &t : tasks_) {
        if (t.status != TaskStatus::Downloading && t.status != TaskStatus::Building) continue;
        if (!first) line += "  ";
        line += status_icon(t.status) + " " + t.name;
        if (!t.detail.empty()) line += std::string(ansi::dim) + " (" + t.detail + ")" + ansi::reset;
        first = false;
    }

    if (active == 0 && done < total) line += std::string(ansi::dim) + "○ waiting..." + ansi::reset;

    line += "  " + std::string(ansi::dim) + "[" + std::to_string(done) + "/" + std::to_string(total) + "]" + ansi::reset;
    std::cout << line << std::flush;
}


// Braille spinner with a wider set for a smoother animation
static const char *k_build_frames[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
static constexpr int k_num_frames = 10;

BuildSpinner::BuildSpinner(std::string label, std::string detail) : label_(std::move(label)), detail_(std::move(detail)), start_(std::chrono::steady_clock::now()) {
    thread_ = std::thread([this]() { loop(); });
}

BuildSpinner::~BuildSpinner() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void BuildSpinner::finish(bool success) {
    success_ = success;
    finished_ = true;
    running_ = false;
    if (thread_.joinable()) thread_.join();

    // Print final status line
    std::string elapsed = elapsed_str(start_);
    std::cout << ansi::cr_erase;
    if (success) {
        std::cout << ansi::bold << ansi::green << "  ✓ " << ansi::reset << ansi::bold << "Built " << label_ << ansi::reset << ansi::dim << "  " << elapsed << ansi::reset << "\n";
    } else {
        std::cout << ansi::bold << ansi::red << "  ✗ " << ansi::reset << ansi::bold << "Build failed" << ansi::reset << ansi::dim << "  " << elapsed << ansi::reset << "\n";
    }
    std::cout.flush();
}

void BuildSpinner::loop() {
    int frame = 0;
    while (running_) {
        std::string elapsed = elapsed_str(start_);

        std::cout << ansi::cr_erase << ansi::cyan << "  " << k_build_frames[frame % k_num_frames] << " " << ansi::reset << ansi::bold << "Building " << label_ << ansi::reset << ansi::dim << "  "
                  << detail_ << "  " << elapsed << ansi::reset << std::flush;

        ++frame;
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }
}


void parallel_execute(const std::vector<std::function<void()>> &tasks, int max_parallel) {
    if (tasks.empty()) return;

    std::atomic<int> next{0};
    int total = static_cast<int>(tasks.size());
    int num_threads = std::min(max_parallel, total);

    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&]() {
            while (true) {
                int idx = next.fetch_add(1);
                if (idx >= total) break;
                tasks[idx]();
            }
        });
    }

    for (auto &t : threads) t.join();
}

} // namespace cpm
