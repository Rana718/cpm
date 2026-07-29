#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cpm {

enum class TaskStatus { Pending, Downloading, Building, Done, Failed, Cached };

struct TaskProgress {
    std::string name;
    TaskStatus status;
    std::string detail;
};

// Shows: ◐ downloading json...   ◑ building yaml-cpp...   [2/4]
class ProgressDisplay {
  public:
    ProgressDisplay();
    ~ProgressDisplay();

    int add_task(const std::string &name);
    void set_status(int task_id, TaskStatus status, const std::string &detail = "");

    void start();
    void stop();

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

class BuildSpinner {
  public:
    BuildSpinner(std::string label, std::string detail);
    ~BuildSpinner();

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

void parallel_execute(const std::vector<std::function<void()>> &tasks, int max_parallel = 4);

} // namespace cpm
