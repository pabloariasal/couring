# couring
C++ program that uses C++20 Corotuines and io_uring.
Implementation explained in the [blog series](https://pabloariasal.github.io/2022/11/12/couring-1/)

# Requirements

You will need a Linux kernel version 5.1 or higher and have liburing installed.
Your C++ compiler must support C++20 and coroutines.

# Build
```sh
cmake -Bbuild -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

# Usage

## Preparing the data

Download any OBJ file you want from the internet and put them in folder.

To make things interesting you'll need hundreds of them, you can duplicate a OBJ file switch with a bash one-liner

```sh
for i in {1..5000} do cp my_file.obj my_file_$1.obj done
```
## Running
Once you have all obj files on disk you can invoke the tool by passing the directory containing them.

```sh
./build/couring /folder_containing_obj_files <implementation>
```
There are five different implementations of the algorithm:

**--trivial**:
    Single-threaded synchronous implementation

**--thread-pool**
    Multi-threaded synchronous implementation

**--iouring**
    Single-threaded asynchronous implementation using iouring

**--coroutines**
    asynchronous implementation with coroutines with iouring, parsing single-threaded

**--coro-pool**
    asynchronous implementation with coroutines with iouring, parsing multi-threaded

There is a convenience script that runs all implementations and times them:

```
> ./run_all build /folder_containing_obj_files
#########
Running trivial implementation
Processed 512 files.

real	0m0.419s
user	0m0.371s
sys	0m0.046s
#########
Running multi-threading implementation
Processed 512 files.

real	0m0.177s
user	0m0.599s
sys	0m0.026s
#########
Running iouring implementation
Processed 512 files.

real	0m0.368s
user	0m0.317s
sys	0m0.050s
#########
Running couroutines implementation
Processed 512 files.

real	0m0.339s
user	0m0.305s
sys	0m0.033s
#########
Running coroutines thread pool implementation
Processed 512 files.

real	0m0.230s
user	0m0.732s
sys	0m0.047s
```
