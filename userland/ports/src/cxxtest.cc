// M38: dynamically linked libc++/libc++abi with exceptions and RTTI disabled.
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
static std::atomic<int> constructed{0}, destroyed{0};
struct Local { int value = 0; ~Local() { ++destroyed; } };
static thread_local Local local;
static int guarded() { static int v = [] { ++constructed; return 42; }(); return v; }
int main() {
    int fail = 0;
    auto check = [&](bool ok, const char *what) { std::printf("cxxtest: %s %s\n", ok ? "ok" : "FAIL", what); if (!ok) ++fail; };
    std::vector<int> v{8, 2, 7, 1}; std::sort(v.begin(), v.end());
    check(v == std::vector<int>({1, 2, 7, 8}), "allocation/vector/sort");
    std::ostringstream s; s << "mesa-" << 38 << '-' << std::string(100, 'x');
    check(s.str() == "mesa-38-" + std::string(100, 'x'), "string/iostream formatting");
    std::mutex m; std::condition_variable cv; bool ready = false; int sum = 0;
    std::thread a([&] { local.value = guarded(); { std::lock_guard<std::mutex> l(m); sum += local.value; ready = true; } cv.notify_one(); });
    std::thread b([&] { local.value = guarded(); std::lock_guard<std::mutex> l(m); sum += local.value; });
    { std::unique_lock<std::mutex> l(m); cv.wait(l, [&] { return ready; }); }
    a.join(); b.join();
    check(sum == 84 && constructed == 1, "threads/mutex/condition variable/static guard");
    check(destroyed == 2, "thread-local destructors");
    std::cout << "cxxtest: " << (fail ? "FAILED" : "all passed") << std::endl;
    return fail != 0;
}
