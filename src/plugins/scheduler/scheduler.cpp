// scheduler 插件：类 crontab 的定时任务（实现 cppdaemon::Plugin）。
// 每条任务支持两种调度，二选一：
//   1) cron：标准 5 段表达式（分 时 日 月 周），支持 *、*/N、a-b、a,b,c
//   2) interval_minutes：每 N 分钟执行一次
// 命令按任务顺序串行执行（同一时刻只有一个任务在跑），避免互相干扰；
// 代价是长时间运行的命令会阻塞后续任务，请自行保证命令能在合理时间内结束。
#include "core/Plugin.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <windows.h>
#define RUN_POPEN _popen
#define RUN_PCLOSE _pclose
#else
#define RUN_POPEN popen
#define RUN_PCLOSE pclose
#endif

namespace {

// ---------- 持久化日志（同时写文件与 stderr） ----------
std::string logFilePath() {
#ifdef _WIN32
  char buf[MAX_PATH] = {0};
  DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
  std::string s(buf, n);
  size_t p = s.find_last_of("\\/");
  s = (p == std::string::npos) ? "." : s.substr(0, p);
  return s + "\\cpp-daemon.log";
#else
  return "cpp-daemon.log";
#endif
}
FILE* g_logf = nullptr;
void slog(const char* fmt, ...) {
  char ts[32];
  time_t t = time(nullptr);
  struct tm tm_buf;
#ifdef _WIN32
  localtime_s(&tm_buf, &t);
#else
  localtime_r(&t, &tm_buf);
#endif
  strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);
  va_list ap;
  if (!g_logf) g_logf = fopen(logFilePath().c_str(), "a");
  if (g_logf) {
    fprintf(g_logf, "[%s] ", ts);
    va_start(ap, fmt);
    vfprintf(g_logf, fmt, ap);
    va_end(ap);
    fflush(g_logf);
  }
  fprintf(stderr, "[%s] ", ts);
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
}

// ---------- cron 表达式 ----------
std::vector<std::string> splitTokens(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char ch : s) {
    if (ch == ' ' || ch == '\t') {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
    } else {
      cur += ch;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

// 单个字段匹配：支持 *、*/N、a、a-b、以及逗号分隔的列表；'?' 视为 '*'
bool cronFieldMatch(const std::string& field, int val, int lo, int hi) {
  std::string s = field;
  size_t pos = 0;
  while (pos <= s.size()) {
    size_t comma = s.find(',', pos);
    std::string part = (comma == std::string::npos) ? s.substr(pos) : s.substr(pos, comma - pos);
    int step = 1;
    size_t slash = part.find('/');
    if (slash != std::string::npos) {
      try {
        step = std::stoi(part.substr(slash + 1));
      } catch (...) {
        step = 1;
      }
      part = part.substr(0, slash);
    }
    int from, to;
    if (part == "*" || part == "?") {
      from = lo;
      to = hi;
    } else {
      size_t dash = part.find('-');
      try {
        if (dash != std::string::npos) {
          from = std::stoi(part.substr(0, dash));
          to = std::stoi(part.substr(dash + 1));
        } else {
          from = to = std::stoi(part);
        }
      } catch (...) {
        return false;
      }
    }
    if (step < 1) step = 1;
    for (int v = from; v <= to; v += step)
      if (v == val) return true;
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
  return false;
}

// cron 表达式匹配。支持：
//   5 段（标准）：      分 时 日 月 周
//   6 段（Quartz）：     秒 分 时 日 月 周
//   7 段（末尾年）：     忽略“年”字段
// 字段中的 '?' 视为 '*'（Quartz 中 ? 表示“不指定”，与 * 等价）。
bool cronMatch(const std::string& expr, const std::tm& t) {
  std::vector<std::string> tok = splitTokens(expr);
  if (tok.empty()) return false;
  if (tok.size() >= 7) tok.resize(6);  // 忽略年字段
  if (tok.size() == 6) {              // 秒 分 时 日 月 周
    return cronFieldMatch(tok[1], t.tm_min, 0, 59) &&
           cronFieldMatch(tok[2], t.tm_hour, 0, 23) &&
           cronFieldMatch(tok[3], t.tm_mday, 1, 31) &&
           cronFieldMatch(tok[4], t.tm_mon + 1, 1, 12) &&
           cronFieldMatch(tok[5], t.tm_wday, 0, 6);
  }
  if (tok.size() == 5) {              // 分 时 日 月 周
    return cronFieldMatch(tok[0], t.tm_min, 0, 59) &&
           cronFieldMatch(tok[1], t.tm_hour, 0, 23) &&
           cronFieldMatch(tok[2], t.tm_mday, 1, 31) &&
           cronFieldMatch(tok[3], t.tm_mon + 1, 1, 12) &&
           cronFieldMatch(tok[4], t.tm_wday, 0, 6);
  }
  return false;
}

}  // namespace

namespace scheduler {

struct Task {
  std::string name;
  std::string cron;
  int intervalMinutes = 0;
  std::string command;
  bool enabled = true;
};

struct TaskState {
  int64_t lastRun = 0;  // epoch 秒
  bool lastOk = false;
  std::string lastError;
  std::string lastCronKey;  // 用于同一分钟内不重复触发
  int runs = 0;
};

class Scheduler : public cppdaemon::Plugin {
public:
  const char* name() const override { return "scheduler"; }
  int protocolVersion() const override { return cppdaemon::kProtocolVersion; }

  bool configure(const cppdaemon::AppConfig& cfg) override {
    identity_ = cfg.identity;
    {
      std::lock_guard<std::mutex> lk(mu_);
      tasks_ = parseTasks(cfg.pluginConfig);
    }
    startEpoch_ = (int64_t)std::time(nullptr);
    quit_ = false;
    thread_ = std::thread([this]() { loop(); });
    return true;
  }

  bool onConfigChanged(const cppdaemon::AppConfig& cfg) override {
    {
      std::lock_guard<std::mutex> lk(mu_);
      tasks_ = parseTasks(cfg.pluginConfig);
    }
    slog("[scheduler] 配置已更新，共 %zu 条任务\n", tasks_.size());
    return true;
  }

  void shutdown() override {
    quit_ = true;
    if (thread_.joinable()) thread_.join();
  }

  cppdaemon::DesiredUI desiredUI() const override {
    cppdaemon::DesiredUI ui;
    ui.routes.push_back({"GET", "/api/tasks"});
    ui.routes.push_back({"POST", "/api/tasks/run"});
    return ui;
  }

  cppdaemon::ConfigSchema configSchema() const override {
    cppdaemon::ConfigSchema s;
    s.name = "scheduler";
    s.title = "定时任务";
    cppdaemon::Field tasks;
    tasks.key = "tasks";
    tasks.label = "任务列表";
    tasks.type = "array";
    tasks.hint =
        "cron 与间隔分钟二选一：填了 cron 就按 cron 执行（留空间隔），"
        "否则按间隔分钟周期执行。任务串行运行，长时间命令会阻塞后续任务。";
    tasks.fields = {
        {"name", "名称", "text"},
        {"cron", "Cron 表达式", "text", "分 时 日 月 周，如 */5 * * * *"},
        {"interval_minutes", "间隔分钟", "number", "cron 为空时生效"},
        {"command", "命令", "text", "Windows 下以 cmd /C 执行，可直接填 .bat 路径"},
        {"enabled", "启用", "bool"},
    };
    s.fields = {tasks};
    return s;
  }

  cppdaemon::Response handle(const std::string& /*route*/,
                             const cppdaemon::Request& req) override {
    cppdaemon::Response resp;
    if (req.path == "/api/tasks") {
      resp.body = tasksJson().dump();
    } else if (req.path == "/api/tasks/run") {
      std::string want;
      nlohmann::json in = nlohmann::json::parse(req.body, nullptr, false);
      if (!in.is_discarded()) want = in.value("name", "");
      nlohmann::json out;
      if (want.empty()) {
        out = {{"ok", false}, {"error", "请在 body 中指定 name"}};
      } else {
        bool found = false;
        {
          std::lock_guard<std::mutex> lk(mu_);
          for (auto& t : tasks_)
            if (t.name == want) {
              found = true;
              break;
            }
        }
        if (!found)
          out = {{"ok", false}, {"error", "未找到任务: " + want}};
        else
          out = runByName(want);
      }
      resp.body = out.dump();
    } else {
      resp.statusCode = 404;
      resp.body = "not found";
      return resp;
    }
    resp.statusCode = 200;
    resp.header["Content-Type"] = "application/json; charset=utf-8";
    return resp;
  }

  void onTrayClick(const std::string& /*itemId*/) override {}

private:
  cppdaemon::Identity identity_;
  std::mutex mu_;
  std::vector<Task> tasks_;

  std::mutex stMu_;
  std::map<size_t, TaskState> states_;  // 按任务下标索引

  std::thread thread_;
  std::atomic<bool> quit_{false};
  int64_t startEpoch_ = 0;

  static std::vector<Task> parseTasks(const nlohmann::json& bucket) {
    std::vector<Task> out;
    if (!bucket.is_object() || !bucket.contains("tasks")) return out;
    auto& arr = bucket["tasks"];
    if (!arr.is_array()) return out;
    for (auto& it : arr) {
      if (!it.is_object()) continue;
      Task t;
      t.name = it.value("name", "");
      t.cron = it.value("cron", "");
      t.intervalMinutes = it.value("interval_minutes", 0);
      t.command = it.value("command", "");
      t.enabled = it.value("enabled", true);
      out.push_back(t);
    }
    return out;
  }

  void loop() {
    while (!quit_) {
      // 每 15 秒检查一次是否需要触发（cron 精确到分钟，足够）
      for (int i = 0; i < 15 && !quit_; ++i)
        std::this_thread::sleep_for(std::chrono::seconds(1));
      if (quit_) break;
      tick();
    }
  }

  void tick() {
    std::vector<Task> tasks;
    {
      std::lock_guard<std::mutex> lk(mu_);
      tasks = tasks_;
    }
    if (tasks.empty()) return;
    time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char key[32] = {0};
    std::strftime(key, sizeof(key), "%Y-%m-%d %H:%M", &tm);

    for (size_t i = 0; i < tasks.size(); ++i) {
      if (quit_) break;
      const Task& t = tasks[i];
      if (!t.enabled || t.command.empty()) continue;
      bool fire = false;
      if (!t.cron.empty()) {
        std::lock_guard<std::mutex> lk(stMu_);
        if (states_[i].lastCronKey != key && cronMatch(t.cron, tm)) fire = true;
      } else if (t.intervalMinutes > 0) {
        std::lock_guard<std::mutex> lk(stMu_);
        TaskState& st = states_[i];
        if (st.lastRun == 0) st.lastRun = startEpoch_;  // 首次触发需等满一个间隔
        if (now - st.lastRun >= (int64_t)t.intervalMinutes * 60) fire = true;
      }
      if (!fire) continue;
      if (!t.cron.empty()) {
        std::lock_guard<std::mutex> lk(stMu_);
        states_[i].lastCronKey = key;
      }
      runTask(i, t);
    }
  }

  void runTask(size_t idx, const Task& t) {
    slog("[scheduler] 执行任务 [%s]: %s\n", t.name.c_str(), t.command.c_str());
    std::string out;
    bool ok = false;
    std::string err;
#ifdef _WIN32
    std::string shell = "cmd /C " + t.command;
#else
    std::string shell = "sh -c " + t.command;
#endif
    FILE* pipe = RUN_POPEN(shell.c_str(), "r");
    if (pipe) {
      char buf[512];
      while (fgets(buf, sizeof(buf), pipe)) out += buf;
      int code = RUN_PCLOSE(pipe);
      ok = (code == 0);
      if (!ok) err = "exit code " + std::to_string(code);
    } else {
      err = "无法启动命令";
    }
    {
      std::lock_guard<std::mutex> lk(stMu_);
      TaskState& st = states_[idx];
      st.lastRun = (int64_t)std::time(nullptr);
      st.lastOk = ok;
      st.lastError = err;
      st.runs += 1;
    }
    if (!out.empty()) slog("[scheduler] 任务 [%s] 输出: %s\n", t.name.c_str(), out.c_str());
    slog("[scheduler] 任务 [%s] 结束 ok=%d %s\n", t.name.c_str(), ok ? 1 : 0, err.c_str());
  }

  nlohmann::json runByName(const std::string& want) {
    Task t;
    bool found = false;
    {
      std::lock_guard<std::mutex> lk(mu_);
      for (auto& x : tasks_)
        if (x.name == want) {
          t = x;
          found = true;
          break;
        }
    }
    if (!found) return {{"ok", false}, {"error", "未找到任务: " + want}};
    std::string out;
    bool ok = false;
    std::string err;
#ifdef _WIN32
    std::string shell = "cmd /C " + t.command;
#else
    std::string shell = "sh -c " + t.command;
#endif
    FILE* pipe = RUN_POPEN(shell.c_str(), "r");
    if (pipe) {
      char buf[512];
      while (fgets(buf, sizeof(buf), pipe)) out += buf;
      int code = RUN_PCLOSE(pipe);
      ok = (code == 0);
      if (!ok) err = "exit code " + std::to_string(code);
    } else {
      err = "无法启动命令";
    }
    return {{"ok", ok}, {"output", out}, {"error", err}};
  }

  nlohmann::json tasksJson() {
    std::vector<Task> tasks;
    {
      std::lock_guard<std::mutex> lk(mu_);
      tasks = tasks_;
    }
    nlohmann::json arr = nlohmann::json::array();
    for (size_t i = 0; i < tasks.size(); ++i) {
      TaskState st;
      {
        std::lock_guard<std::mutex> lk(stMu_);
        auto it = states_.find(i);
        if (it != states_.end()) st = it->second;
      }
      nlohmann::json j = {{"name", tasks[i].name},
                          {"cron", tasks[i].cron},
                          {"interval_minutes", tasks[i].intervalMinutes},
                          {"command", tasks[i].command},
                          {"enabled", tasks[i].enabled},
                          {"last_run", st.lastRun},
                          {"last_ok", st.lastOk},
                          {"last_error", st.lastError},
                          {"runs", st.runs}};
      arr.push_back(j);
    }
    return {{"tasks", arr}};
  }
};

}  // namespace scheduler

// ---------- C-ABI 工厂 ----------
extern "C" CPPD_PLUGIN_API cppdaemon::Plugin* create_plugin() {
  return new scheduler::Scheduler();
}
extern "C" CPPD_PLUGIN_API void destroy_plugin(cppdaemon::Plugin* p) { delete p; }
