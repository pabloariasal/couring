include(FetchContent)
FetchContent_Declare(
  threadpool
  GIT_REPOSITORY https://github.com/bshoshany/thread-pool
  GIT_TAG        v3.3.0
)
FetchContent_MakeAvailable(threadpool)
add_library(threadpool INTERFACE)
target_include_directories(threadpool INTERFACE ${threadpool_SOURCE_DIR})
