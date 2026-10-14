// M40: full dynamic C++ runtime, plus a no-EH/no-RTTI consumer variant.
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
#ifndef CXXTEST_NO_EH
#include <exception>
#include <stdexcept>
#include <typeinfo>
#endif
static std::atomic<int> constructed{0}, destroyed{0};
struct Local { int value = 0; ~Local() { ++destroyed; } };
static thread_local Local local;
static int guarded() { static int v = [] { ++constructed; return 42; }(); return v; }
#ifndef CXXTEST_NO_EH
static int unwound;
struct Guard { ~Guard() { ++unwound; } };
__attribute__((noinline)) static void deep_throw(int depth) {
    Guard guard;
    if (depth) deep_throw(depth-1);
    else throw std::runtime_error("9os unwind");
}
struct Base { virtual ~Base() = default; };
struct Left : virtual Base { int a=7; };
struct Right : virtual Base { int b=9; };
struct Derived : Left, Right {};
#endif
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
#ifndef CXXTEST_NO_EH
    bool caught=false;
    try { deep_throw(12); }
    catch (const std::runtime_error &e) { caught=std::string(e.what())=="9os unwind"; }
    check(caught && unwound==13, "recursive exception unwinding/RAII");
    caught=false;
    try { (void)v.at(1000); }
    catch (const std::out_of_range &e) { caught=std::string(e.what()).size()>0; }
    check(caught, "exception thrown across libc++ DSO boundary");
    std::exception_ptr saved;
    std::thread worker([&] { try { throw std::logic_error("thread error"); } catch (...) { saved=std::current_exception(); } });
    worker.join(); caught=false;
    try { if (saved) std::rethrow_exception(saved); }
    catch (const std::logic_error &e) { caught=std::string(e.what())=="thread error"; }
    check(caught, "exception_ptr across thread lifetime");
    Derived derived; Base *base=&derived; Left *left=&derived;
    check(dynamic_cast<Derived *>(base)==&derived && dynamic_cast<Right *>(left)==static_cast<Right *>(&derived), "RTTI virtual-base downcast/crosscast");
    check(typeid(*base)==typeid(Derived), "dynamic typeid");
    Base plain; caught=false;
    check(dynamic_cast<Derived *>(&plain)==nullptr, "failed pointer dynamic_cast");
    try { (void)dynamic_cast<Derived &>(plain); }
    catch (const std::bad_cast &) { caught=true; }
    check(caught, "failed reference dynamic_cast throws bad_cast");
#endif
    std::cout << "cxxtest: " << (fail ? "FAILED" : "all passed") << std::endl;
    return fail != 0;
}
