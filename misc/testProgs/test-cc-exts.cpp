#include <atomic>
#include <condition_variable>
#include <cassert>
#include <mutex>
#include <thread>
#include <vector>

#define SIZE (512 * 1024 / sizeof(int))
#define ITER 64
#define NT 4

int main() {
    int array[SIZE];
    for (int i = 0; i < SIZE; i++) array[i] = 0;

    std::mutex lock;
    std::condition_variable cv;
    std::atomic<int> token(0);

    // Threads process array one after one.
    auto func = [&array, &lock, &cv, &token](int t) {
        for (int j = 0; j < ITER; j++) {
            std::unique_lock<std::mutex> lk(lock);
            cv.wait(lk, [&token, t]{ return token % NT == t; });
            for (int i = 0; i < SIZE; i++) array[i]++;
            token++;
            lk.unlock();
            cv.notify_all();
        }
    };

    std::vector<std::thread> th;
    for (int t = 0; t < NT; t++) th.emplace_back(func, t);
    for (int t = 0; t < NT; t++) th[t].join();

    for (int i = 0; i < SIZE; i++) assert(array[i] == NT * ITER);

    return 0;
}

