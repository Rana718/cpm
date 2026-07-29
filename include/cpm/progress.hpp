#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cpm {

// ─── Task-level progress (used by install / parallel downloads) ───────────────

enum class TaskStatus { Pending, Downloading, Building, Done, Failed, Cached };

struct TaskProgress {
    std::string name;
    TaskStatus status;
    std::string detail;
};

// Displays live progress for parallel package operations.
// Shows: ◐ downloading json...   ◑ building yaml-cpp...   [2/4]
class ProgressDisplay {
  public:
    ProgressDisplay();
    ~ProgressDisplay();

    int add_task(const std::string &name);
    void set_status(int task_id, TaskStatus status, const std::string &detail = "");

    void start();
    void stop();

    // Thread-safe line print (clears the spinner line first)
    void print(const std::string &msg);

  private:
    std::vector<TaskProgress> tasks_;
    std::mutex mutex_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopped_{true};
    std::thread display_thread_;

    void render();
    std::string status_icon(TaskStatus status); // advances spinner frame
    [[nodiscard]] std::string status_text(TaskStatus status) const;
};

// ─── Build spinner (used during cpm build / cpm run) ─────────────────────────
//
// Shows a single animated line while the compiler is running:
//
//   ⠸ Building myapp  (c++20 · g++)  0.1s
//   ✓ Built myapp  1.4s
//   ✗ Build failed  2.1s
//
class BuildSpinner {
  public:
    // Start spinner with the label shown while building.
    // label  = e.g. "myapp"
    // detail = e.g. "c++20 · g++"
    BuildSpinner(std::string label, std::string detail);
    ~BuildSpinner();

    // Call when the build finishes. success controls ✓ vs ✗.
    void finish(bool success);

  private:
    std::string label_;
    std::string detail_;
    std::atomic<bool> running_{true};
    std::atomic<bool> finished_{false};
    bool success_{false};
    std::thread thread_;
    std::chrono::steady_clock::time_point start_;

    void loop();
};

// ─── Parallel execution ───────────────────────────────────────────────────────

void parallel_execute(const std::vector<std::function<void()>> &tasks, int max_parallel = 4);

} // namespace cpm
