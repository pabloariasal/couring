#include <algorithm>
#include <coroutine>
#include <filesystem>
#include <iostream>
#include <sys/ioctl.h>
#include <utility>
#include <vector>

#include <liburing.h>

#include <BS_thread_pool.hpp>
#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>

off_t get_file_size(int fd) {
  struct stat st;

  if (fstat(fd, &st) < 0) {
    perror("fstat");
    return -1;
  }

  if (S_ISBLK(st.st_mode)) {
    unsigned long long bytes;
    if (ioctl(fd, BLKGETSIZE64, &bytes) != 0) {
      perror("ioctl");
      return -1;
    }
    return bytes;
  } else if (S_ISREG(st.st_mode))
    return st.st_size;

  return -1;
}

constexpr auto MAX_FILES = 512ul;

class ReadOnlyFile {
public:
  ReadOnlyFile(const std::string &file_path) : path_{file_path} {
    fd_ = open(file_path.c_str(), O_RDONLY);
    if (fd_ < 0) {
      throw std::runtime_error("Could not open file: " + file_path);
    }
    size_ = get_file_size(fd_);
    if (size_ < 0) {
      throw std::runtime_error("Could not estimate size of file");
    }
  }
  ReadOnlyFile(ReadOnlyFile &&other)
      : path_{std::exchange(other.path_, {})},
        fd_{std::exchange(other.fd_, -1)}, size_{other.size()} {};

  ~ReadOnlyFile() {
    if (fd_) {
      close(fd_);
    }
  }
  int fd() const { return fd_; }
  off_t size() const { return get_file_size(fd_); }
  const std::string &path() const { return path_; }

private:
  std::string path_;
  int fd_;
  off_t size_;
};

struct Result {
  tinyobj::ObjReader result; // stores the actual parsed obj
  int status_code{0};        // the status code of the read operation
  std::string file;
};

class IOUring {
public:
  explicit IOUring(size_t queue_size) {
    if (auto s = io_uring_queue_init(queue_size, &ring_, 0); s < 0) {
      throw std::runtime_error("error: " + std::to_string(s));
    }
  }

  IOUring(const IOUring &) = delete;
  IOUring &operator=(const IOUring &) = delete;
  IOUring(IOUring &&) = delete;
  IOUring &operator=(IOUring &&) = delete;

  ~IOUring() { io_uring_queue_exit(&ring_); }

  struct io_uring *get() {
    return &ring_;
  }

private:
  struct io_uring ring_;
};

std::vector<ReadOnlyFile> openFiles(const std::string &directory) {
  std::vector<ReadOnlyFile> files;
  files.reserve(MAX_FILES);
  for (auto const &dir_entry :
       std::filesystem::recursive_directory_iterator{directory}) {
    if (dir_entry.is_regular_file() && dir_entry.path().extension() == ".obj") {
      files.emplace_back(dir_entry.path().string());
    }
    if (files.size() >= MAX_FILES) {
      break;
    }
  }
  return files;
}

std::vector<std::vector<char>>
initializeBuffers(const std::vector<ReadOnlyFile> &files) {
  std::vector<std::vector<char>> buffs;
  buffs.reserve(files.size());
  for (const auto &file : files) {
    buffs.emplace_back(file.size());
  }
  return buffs;
}

void readObjFromBuffer(const std::vector<char> &buff,
                       tinyobj::ObjReader &reader) {
  auto s = std::string(buff.data(), buff.size());
  reader.ParseFromString(s, std::string{});
}

void pushEntriesToSubmissionQueue(const std::vector<ReadOnlyFile> &files,
                                  std::vector<std::vector<char>> &buffs,
                                  IOUring &uring) {
  for (size_t i = 0; i < files.size(); ++i) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(uring.get());
    io_uring_prep_read(sqe, files[i].fd(), buffs[i].data(), buffs[i].size(), 0);
    io_uring_sqe_set_data64(sqe, i);
  }
}

std::vector<Result>
readEntriesFromCompletionQueue(const std::vector<ReadOnlyFile> &files,
                               const std::vector<std::vector<char>> &buffs,
                               IOUring &uring) {
  std::vector<Result> results;
  results.reserve(files.size());
  while (results.size() < files.size()) {
    io_uring_submit_and_wait(uring.get(), 1);

    io_uring_cqe *cqe;
    unsigned head;
    int processed{0};
    io_uring_for_each_cqe(uring.get(), head, cqe) {
      auto id = io_uring_cqe_get_data64(cqe);
      results.push_back({.status_code = cqe->res, .file = files[id].path()});
      if (results.back().status_code) {
        readObjFromBuffer(buffs[id], results.back().result);
      }
      ++processed;
    }

    io_uring_cq_advance(uring.get(), processed);
  }

  return results;
}

std::vector<Result> iouring_only(const std::vector<ReadOnlyFile> &files) {
  auto buffs = initializeBuffers(files);
  IOUring uring{files.size()};
  pushEntriesToSubmissionQueue(files, buffs, uring);
  return readEntriesFromCompletionQueue(files, buffs, uring);
}

Result readSynchronous(const ReadOnlyFile &file) {
  Result result{.file = file.path()};
  std::vector<char> buff(file.size());
  read(file.fd(), buff.data(), buff.size());
  readObjFromBuffer(buff, result.result);
  return result;
}

std::vector<Result> thread_pool(const std::vector<ReadOnlyFile> &files) {
  std::vector<Result> result(files.size());

  BS::thread_pool pool;
  pool.parallelize_loop(files.size(),
                        [&files, &result](int a, int b) {
                          for (int i = a; i < b; ++i) {
                            result[i] = readSynchronous(files[i]);
                          }
                        })
      .wait();
  return result;
}

std::vector<Result> trivial(const std::vector<ReadOnlyFile> &files) {
  std::vector<Result> results;
  results.reserve(files.size());
  for (const auto &file : files) {
    results.push_back(readSynchronous(file));
  }
  return results;
}

bool isGood(const Result &result) {
  if (result.status_code < 0) {
    std::cout << "Error reading file: " << result.file
              << ". Error: " << result.status_code << std::endl;
    return false;
  }
  if (!result.result.Error().empty()) {
    std::cerr << "Error parsing file: " << result.file
              << ". Reason: " << result.result.Error();
    return false;
  }
  return true;
}

class Task {
public:
  struct promise_type {
    Result result;

    Task get_return_object() { return Task(this); }

    void unhandled_exception() noexcept {}

    void return_value(Result result) noexcept { result = std::move(result); }
    std::suspend_never initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }
  };

  explicit Task(promise_type *promise)
      : handle_{HandleT::from_promise(*promise)} {}
  Task(Task &&other) : handle_{std::exchange(other.handle_, nullptr)} {}

  ~Task() {
    if (handle_) {
      handle_.destroy();
    }
  }

  Result getResult() const & {
    assert(handle_.done());
    return handle_.promise().result;
  }

  Result &&getResult() && {
    assert(handle_.done());
    return std::move(handle_.promise().result);
  }

  bool done() const { return handle_.done(); }

  using HandleT = std::coroutine_handle<promise_type>;
  HandleT handle_;
};

struct RequestData {
  std::coroutine_handle<> handle;
  int statusCode{-1};
};

class ReadFileAwaitable {
public:
  ReadFileAwaitable(IOUring &uring, const ReadOnlyFile &file,
                    std::vector<char> &buf) {
    sqe_ = io_uring_get_sqe(uring.get());
    io_uring_prep_read(sqe_, file.fd(), buf.data(), buf.size(), 0);
  }

  auto operator co_await() {
    struct Awaiter {
      io_uring_sqe *entry;
      RequestData requestData;
      Awaiter(io_uring_sqe *sqe) : entry{sqe} {}
      bool await_ready() { return false; }
      void await_suspend(std::coroutine_handle<> handle) noexcept {
        requestData.handle = handle;
        io_uring_sqe_set_data(entry, &requestData);
      }
      int await_resume() { return requestData.statusCode; }
    };
    return Awaiter{sqe_};
  }

private:
  io_uring_sqe *sqe_;
};

class ThreadPool {
public:
  auto schedule() {
    struct Awaiter : std::suspend_always {
      BS::thread_pool &tpool;
      Awaiter(BS::thread_pool &pool) : tpool{pool} {}
      void await_suspend(std::coroutine_handle<> handle) {
        tpool.push_task([handle, this]() { handle.resume(); });
      }
    };
    return Awaiter{pool_};
  }

  size_t numUnfinishedTasks() const { return pool_.get_tasks_total(); }

private:
  BS::thread_pool pool_;
};

int consumeCQEntries(IOUring &uring) {
  int processed{0};
  io_uring_cqe *cqe;
  unsigned head;
  io_uring_for_each_cqe(uring.get(), head, cqe) {
    auto *request_data = static_cast<RequestData *>(io_uring_cqe_get_data(cqe));
    request_data->statusCode = cqe->res;
    request_data->handle.resume();
    ++processed;
  }
  io_uring_cq_advance(uring.get(), processed);
  return processed;
}

int consumeCQEntriesBlocking(IOUring &uring) {
  io_uring_submit_and_wait(uring.get(), 1);
  return consumeCQEntries(uring);
}

int consumeCQEntriesNonBlocking(IOUring &uring) {
  io_uring_cqe *temp;
  if (io_uring_peek_cqe(uring.get(), &temp) == 0) {
    return consumeCQEntries(uring);
  }
  return 0;
}

Task parseOBJFile(IOUring &uring, const ReadOnlyFile &file) {
  std::vector<char> buff(file.size());
  int status = co_await ReadFileAwaitable{uring, file, buff};
  Result result{.status_code = 0, .file = file.path()};
  readObjFromBuffer(buff, result.result);
  co_return result;
}

bool allDone(const std::vector<Task> &tasks) {
  return std::all_of(tasks.cbegin(), tasks.cend(),
                     [](const auto &t) { return t.done(); });
}

std::vector<Result> gatherResults(const std::vector<Task> &tasks) {
  std::vector<Result> results;
  results.reserve(tasks.size());
  for (auto &&t : tasks) {
    results.push_back(std::move(t).getResult());
  }
  return results;
}

std::vector<Result> coroutines(const std::vector<ReadOnlyFile> &files) {
  IOUring uring{files.size()};
  std::vector<Task> tasks;
  tasks.reserve(files.size());
  for (const auto &file : files) {
    tasks.push_back(parseOBJFile(uring, file));
  }
  while (!allDone(tasks)) {
    // consume all entries in the submission queue
    // if the queue is empty block until the next completion arrives
    consumeCQEntriesBlocking(uring);
  }
  return gatherResults(tasks);
}

Task parseOBJFile(IOUring &uring, const ReadOnlyFile &file, ThreadPool &pool) {
  std::vector<char> buff(file.size());
  int status = co_await ReadFileAwaitable(uring, file, buff);
  co_await pool.schedule();
  Result result{.status_code = 0, .file = file.path()};
  readObjFromBuffer(buff, result.result);
  co_return result;
}

std::vector<Result> coroutines_pool(const std::vector<ReadOnlyFile> &files) {
  IOUring uring{files.size()};
  ThreadPool pool;
  std::vector<Task> tasks;
  tasks.reserve(files.size());
  for (const auto &file : files) {
    tasks.push_back(parseOBJFile(uring, file, pool));
  }
  io_uring_submit(uring.get());
  while (pool.numUnfinishedTasks() > 0 || !allDone(tasks)) {
    // consume entries in the completion queue
    // return immediately if the queue is empty
    consumeCQEntriesNonBlocking(uring);
  }

  return gatherResults(tasks);
}

int main(int argc, char *argv[]) {
  auto files = openFiles(argv[1]);
  auto results = [&files](std::string cmd) {
    if (cmd == "--coroutines") {
      std::cout << "Running couroutines implementation" << std::endl;
      return coroutines(files);
    } else if (cmd == "--iouring") {
      std::cout << "Running iouring implementation" << std::endl;
      return iouring_only(files);
    } else if (cmd == "--thread-pool") {
      std::cout << "Running multi-threading implementation" << std::endl;
      return thread_pool(files);
    } else if (cmd == "--coro-pool") {
      std::cout << "Running coroutines thread pool implementation" << std::endl;
      return coroutines_pool(files);
    } else if (cmd == "--trivial") {
      std::cout << "Running trivial implementation" << std::endl;
      return trivial(files);
    } else {
      exit(1);
    }
  }(argv[2]);

  int processed{0};
  for (const auto &result : results) {
    if (isGood(result) && !result.result.GetShapes().empty()) {
      assert(result.result.GetShapes().front().mesh.num_face_vertices.size() ==
             1024);
    }
    ++processed;
  }
  std::cout << "Processed " << processed << " files." << std::endl;
  return 0;
}
