include(FetchContent)
FetchContent_Declare(
  tinyobjloader
  GIT_REPOSITORY https://github.com/tinyobjloader/tinyobjloader
  GIT_TAG        v2.0.0rc10
)
FetchContent_MakeAvailable(tinyobjloader)
