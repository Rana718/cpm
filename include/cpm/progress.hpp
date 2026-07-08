#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>
#include <chrono>

namespace cpm {

// Task status for progress display
enum class TaskStatus {
    Pending,
    Downloading,
    Building,
    Done,
    Failed,
    Cached
};

struct TaskProgress {
    std::string name;
    TaskStatus status;
    std::string detail;
};

// Displays live progress like uv/bun
// Shows: ◐ downloading json...
//        ✓ hiredis (cached)
//        ◑ building yaml-cpp...
class ProgressDisplay {
public:
    ProgressDisplay();
    ~ProgressDisplay();

    // Add a task to track
    int add_task(const std::string& name);

    // Update task status
    void set_status(int task_id, TaskStatus status, const std::string& detail = "");

    // Start/stop the display thread
    void start();
    void stop();

    // Print a message (thread-safe, clears progress line first)
    void print(const std::string& msg);

private:
    std::vector<TaskProgress> tasks_;
    std::mutex mutex_;
    std::atomic<bool> running_{false};
    std::thread display_thread_;

    void render();
    std::string status_icon(TaskStatus status) const;
    std::string status_text(TaskStatus status) const;
};

// Run multiple tasks in parallel (up to max_parallel)
void parallel_execute(
    const std::vector<std::function<void()>>& tasks,
    int max_parallel = 4
);

} // namespace cpm
