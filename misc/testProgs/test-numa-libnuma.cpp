#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <numa.h>

#define NNODES 1024
#define MASK_SIZE ((NNODES)/sizeof(unsigned long)/8+1)
#define MASK_NBYTES ((MASK_SIZE)*sizeof(unsigned long))

#define ADDR ((void*)0x20000000)
#define SIZE (1024*1024)

static inline void print_bitmask(struct bitmask* bmp, int num, const char* header) {
    printf("%s (%d): ", header, num);
    for (int i = 0; i < num; i++) {
        printf("%c", numa_bitmask_isbitset(bmp, i) ? 'Y' : 'N');
    }
    printf("\n");
}

static inline int touch_and_get_node(void* addr) {
    assert(addr);
    volatile int* data = (volatile int*)addr;
    *data = 32;

    int node = -1;
    numa_move_pages(0, 1, &addr, NULL, &node, 0);
    return node;
}

void test1() {
    printf("zsim numa libnuma test 1\n");

    // numa_available()
    if (numa_available() < 0) {
        printf("NUMA API not supported\n");
        return;
    }

    // numa_max_possible_node(), numa_num_possible_nodes()
    printf("Possible NUMA nodes %d, max %d\n", numa_num_possible_nodes(), numa_max_possible_node());
    // numa_max_node(), numa_num_configured_nodes()
    int n = numa_max_node();
    printf("Configured NUMA nodes %d, max %d\n", numa_num_configured_nodes(), n);

    // numa_num_possible_cpus()
    printf("Possible cores %d\n", numa_num_possible_cpus());
    // numa_num_configured_cpus()
    int ncpus = numa_num_configured_cpus();
    printf("Configured cores %d\n", ncpus);

    // numa_all_nodes_ptr, numa_no_nodes_ptr
    print_bitmask(numa_all_nodes_ptr, n+1, "All nodes");
    print_bitmask(numa_no_nodes_ptr, n+1, "No nodes");
    // numa_num_configured_cpus()
    // numa_all_cpus_ptr
    print_bitmask(numa_all_cpus_ptr, ncpus, "All CPUs");

    // numa_get_mems_allowed()
    struct bitmask* allowed_node_bmp = numa_get_mems_allowed();
    print_bitmask(allowed_node_bmp, n+1, "Nodes allowed");
    // numa_num_task_cpus(), numa_num_task_nodes()
    printf("Task allowed on %d CPUs, %d nodes\n", numa_num_task_cpus(), numa_num_task_nodes());
    while (n && 0 == numa_bitmask_isbitset(allowed_node_bmp, n)) n--;
    printf("Task targets on node %d\n", n);
    numa_bitmask_free(allowed_node_bmp); allowed_node_bmp = NULL;

    // Ignored:
    // numa_parse_bitmap(), numa_parse_nodestring(), numa_parse_cpustring()

    // numa_node_size(), numa_node_size64()
    long long free_size = 0;
    long long node_size = numa_node_size64(n, &free_size);
    printf("Node %d: total %lld MB, free %lld MB\n", n, node_size/1024/1024, free_size/1024/1024);

    // numa_preferred(), numa_set_preferred()
    numa_set_preferred(n);
    int preferred_node = numa_preferred();
    if (preferred_node == n) {
        printf("Successfully set preferred node to %d\n", n);
    } else {
        printf("Failed to set preferred node, current preferred node is %d\n", preferred_node);
    }

    // numa_alloc(), numa_free(), numa_realloc()
    void* addr = NULL;
    addr = numa_alloc(SIZE);
    assert(touch_and_get_node(addr) == preferred_node);
    addr = numa_realloc(addr, SIZE, SIZE*2);
    assert(touch_and_get_node(addr) == preferred_node);
    numa_free(addr, SIZE*2);

    // numa_get_interleave_mask(), numa_set_interleave_mask()
    struct bitmask* itlv_mask = numa_get_interleave_mask();
    assert(numa_bitmask_equal(itlv_mask, numa_no_nodes_ptr));
    for (int i = 0; i <= n / 2; i++) {
        numa_bitmask_setbit(itlv_mask, i);
    }
    numa_set_interleave_mask(itlv_mask);
    numa_free_nodemask(itlv_mask);
    itlv_mask = numa_get_interleave_mask();
    for (int i = 0; i <= numa_max_node(); i++) {
        assert(numa_bitmask_isbitset(itlv_mask, i) == (i <= n / 2));
    }
    numa_free_nodemask(itlv_mask);
    addr = numa_alloc(SIZE);
    int node = -1;
    for (int i = 0; i < SIZE / numa_pagesize(); i++) {
        void* p = (void*)((unsigned long)addr + i * numa_pagesize());
        if (node >= 0) {
            assert((node + 1) % (n / 2 + 1) == touch_and_get_node(p));
        }
        node = touch_and_get_node(p);
    }

    // numa_get_interleave_node()
    for (int i = 0; i < 5; i++) {
        int next = numa_get_interleave_node();
        void* addr = numa_alloc(numa_pagesize());
        assert(numa_get_interleave_node() == (next + 1) % (n / 2 + 1));
    }

    // numa_interleave_memory()
    itlv_mask = numa_allocate_nodemask();
    numa_bitmask_setall(itlv_mask);
    numa_bitmask_clearbit(itlv_mask, n);
    addr = mmap(0, SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
    numa_interleave_memory(addr, SIZE, itlv_mask);
    node = -1;
    for (int i = 0; i < SIZE / numa_pagesize(); i++) {
        void* p = (void*)((unsigned long)addr + i * numa_pagesize());
        if (node >= 0) {
            assert((node + 1) % (n - 1 + 1) == touch_and_get_node(p));
        }
        node = touch_and_get_node(p);
    }
    munmap(addr, SIZE);

    // numa_alloc_interleaved(), numa_alloc_interleaved_subset()
    addr = numa_alloc_interleaved_subset(SIZE, itlv_mask);
    node = -1;
    for (int i = 0; i < SIZE / numa_pagesize(); i++) {
        void* p = (void*)((unsigned long)addr + i * numa_pagesize());
        if (node >= 0) {
            assert((node + 1) % (n - 1 + 1) == touch_and_get_node(p));
        }
        node = touch_and_get_node(p);
    }
    numa_free(addr, SIZE);
    numa_free_nodemask(itlv_mask);
    addr = numa_alloc_interleaved(SIZE);
    node = -1;
    for (int i = 0; i < SIZE / numa_pagesize(); i++) {
        void* p = (void*)((unsigned long)addr + i * numa_pagesize());
        if (node >= 0) {
            assert((node + 1) % numa_num_configured_nodes() == touch_and_get_node(p));
        }
        node = touch_and_get_node(p);
    }
    numa_free(addr, SIZE);

    // numa_sched_getaffinity(), numa_sched_setaffinity()
    struct bitmask* aff_bmp = numa_bitmask_alloc(ncpus);
    numa_bitmask_setbit(aff_bmp, 0);
    numa_sched_setaffinity(0, aff_bmp);
    numa_bitmask_clearall(aff_bmp);
    numa_sched_getaffinity(0, aff_bmp);
    for (int i = 0; i < ncpus; i++) {
        assert(numa_bitmask_isbitset(aff_bmp, i) == (i == 0));
    }
    numa_bitmask_free(aff_bmp);

    // numa_alloc_local()
    addr = numa_alloc_local(SIZE);
    assert(touch_and_get_node(addr) == 0);
    numa_free(addr, SIZE);

    // numa_set_localalloc()
    numa_set_localalloc();
    addr = numa_alloc(SIZE);
    assert(touch_and_get_node(addr) == 0);
    numa_free(addr, SIZE);

    // numa_alloc_onnode()
    addr = numa_alloc_onnode(SIZE, n/2);
    assert(touch_and_get_node(addr) == n/2);
    numa_free(addr, SIZE);

    // numa_run_on_node(), numa_get_run_node_mask()
    numa_run_on_node(n/2);
    struct bitmask* run_node_bmp = numa_get_run_node_mask();
    for (int i = 0; i <= n; i++) {
        assert(numa_bitmask_isbitset(run_node_bmp, i) == (i == n/2));
    }

    // numa_bind(), including numa_run_on_node_mask(), numa_set_membind()
    // numa_get_membind()
    struct bitmask* nodemask = numa_allocate_nodemask();
    numa_bitmask_setbit(nodemask, n);
    numa_bind(nodemask);
    numa_free_nodemask(nodemask);
    nodemask = numa_get_membind();
    assert(numa_bitmask_isbitset(nodemask, n));
    numa_free_nodemask(nodemask);
    addr = numa_alloc(SIZE);
    assert(touch_and_get_node(addr) == n);
    numa_free(addr, SIZE);

    // numa_tonode_memory()
    addr = mmap(0, SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
    numa_tonode_memory(addr, SIZE, n/2);
    assert(touch_and_get_node(addr) == n/2);
    munmap(addr, SIZE);
    // numa_tonodemask_memory()
    addr = mmap(0, SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
    struct bitmask* tonode_bmp = numa_allocate_nodemask();
    numa_bitmask_setbit(tonode_bmp, n/2);
    numa_tonodemask_memory(addr, SIZE, tonode_bmp);
    numa_free_nodemask(tonode_bmp);
    assert(touch_and_get_node(addr) == n/2);
    munmap(addr, SIZE);
    // numa_setlocal_memory()
    addr = mmap(0, SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
    numa_setlocal_memory(addr, SIZE);
    assert(touch_and_get_node(addr) == n);
    munmap(addr, SIZE);
    // numa_police_memory()
    addr = mmap(0, SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
    numa_police_memory(addr, SIZE);
    assert(touch_and_get_node(addr) == n);
    munmap(addr, SIZE);

    // Ignored:
    // numa_set_bind_policy()

    // numa_set_strict()
    addr = numa_alloc_onnode(SIZE, n);
    assert(touch_and_get_node(addr) == n);
    itlv_mask = numa_allocate_nodemask();
    numa_bitmask_setall(itlv_mask);
    numa_bitmask_clearbit(itlv_mask, n);
    numa_set_strict(0);
    numa_interleave_memory(addr, SIZE, itlv_mask);
    assert(errno == 0);
    numa_set_strict(1);
    numa_interleave_memory(addr, SIZE, itlv_mask);
    assert(errno == EIO);
    errno = 0;

    // numa_distance()
    printf("NUMA distance table:\n");
    for (int i = 0; i <= n; i++) {
        for (int j = 0; j <= n; j++) {
            printf("%d%c", numa_distance(i, j), (j == n ? '\n' : ' '));
        }
    }

    // numa_node_to_cpus()
    printf("NUMA node to CPUs:\n");
    struct bitmask** nodemaps = new bitmask*[n+1];
    for (int i = 0; i <= n; i++) {
        nodemaps[i] = numa_allocate_cpumask();
        numa_bitmask_clearall(nodemaps[i]);
        numa_node_to_cpus(i, nodemaps[i]);
        char buf[40];
        snprintf(buf, 40, "%d", i);
        print_bitmask(nodemaps[i], ncpus, buf);
    }
    // numa_node_of_cpu()
    printf("NUMA CPU to node:\n");
    for (int i = 0; i < ncpus; i++) {
        int node = numa_node_of_cpu(i);
        assert(node <= n);
        assert(numa_bitmask_isbitset(nodemaps[node], i));
    }
    for (int i = 0; i <= n; i++) {
        numa_free_cpumask(nodemaps[i]);
    }
    delete nodemaps;

    // Ignored:
    // bitmask manipulation

    // numa_migrate_pages()
    addr = numa_alloc_onnode(SIZE, n);
    assert(touch_and_get_node(addr) == n);
    struct bitmask* fromnodes = numa_allocate_nodemask();
    struct bitmask* tonodes = numa_allocate_nodemask();
    numa_bitmask_setbit(fromnodes, n);
    numa_bitmask_setbit(tonodes, n-1);
    if (numa_migrate_pages(0, fromnodes, tonodes) == 0) {
        assert(touch_and_get_node(addr) == n-1);
    } else {
        assert(touch_and_get_node(addr) == n);
    }
    numa_free(addr, SIZE);
    numa_free_nodemask(fromnodes);
    numa_free_nodemask(tonodes);

    // numa_move_pages()
    unsigned long count = SIZE / numa_pagesize();
    void** pages = new void*[count];
    int* nodes = new int[count];
    int* status = new int[count];
    addr = numa_alloc_onnode(SIZE, n);
    for (unsigned long i = 0; i < count; i++) {
        pages[i] = (void*)((unsigned long)addr + numa_pagesize() * i);
        nodes[i] = n-1;
    }
    for (unsigned long i = 0; i < count; i++) {
        assert(touch_and_get_node((void*)((unsigned long)addr + numa_pagesize() * i)) == n);
    }
    assert(0 == numa_move_pages(0, count, pages, nodes, status, 0));
    for (unsigned long i = 0; i < count; i++) {
        assert(touch_and_get_node((void*)((unsigned long)addr + numa_pagesize() * i)) == n-1);
    }
    numa_free(addr, SIZE);

    // numa_move_pages(), get nodes, in touch_and_get_node()

    printf("zsim numa libnuma test 1 done\n");
}

void test2() {
    printf("zsim numa libnuma test 2\n");

    if (numa_available() < 0) {
        printf("NUMA API not supported\n");
        return;
    }

    std::atomic<int> token(0);
    std::mutex mtx;
    std::condition_variable cv;

    const int c0 = 0;
    const int c1 = numa_num_configured_cpus() - 1;
    const int n0 = numa_node_of_cpu(c0);
    const int n1 = numa_node_of_cpu(c1);

    auto func0 = [&]() {
        auto aff = numa_allocate_cpumask();
        numa_bitmask_setbit(aff, c0);
        numa_sched_setaffinity(0, aff);
        numa_free_cpumask(aff);

        // Allocate with default.
        void* addr = numa_alloc(SIZE);
        assert(touch_and_get_node(addr) == n0);
        numa_free(addr, SIZE); addr = nullptr;

        {
            std::unique_lock<std::mutex> lk(mtx);
            assert(token == 0);

            // Set policy to allocate on this node.
            numa_set_preferred(n0);

            token = 1;
            lk.unlock();
            cv.notify_one();
        }
    };

    auto func1 = [&]() {
        auto aff = numa_allocate_cpumask();
        numa_bitmask_setbit(aff, c1);
        numa_sched_setaffinity(0, aff);
        numa_free_cpumask(aff);

        // Allocate with default.
        void* addr = numa_alloc(SIZE);
        assert(touch_and_get_node(addr) == n1);
        numa_free(addr, SIZE); addr = nullptr;

        {
            std::unique_lock<std::mutex> lk(mtx);
            cv.wait(lk, [&]{ return token == 1; });

            // Allocate after the sibling thread sets policy.
            // Not affected.
            addr = numa_alloc(SIZE);
            assert(touch_and_get_node(addr) == n1);
            numa_free(addr, SIZE); addr = nullptr;

            // Locally set policy.
            numa_set_preferred(n0);
            addr = numa_alloc(SIZE);
            assert(touch_and_get_node(addr) == n0);
            numa_free(addr, SIZE); addr = nullptr;

            token = 2;
            lk.unlock();
            cv.notify_one();
        }
    };

    std::thread th0(func0);
    std::thread th1(func1);

    th0.join();
    th1.join();

    printf("zsim numa libnuma test 2 done\n");
}

void test3() {
    printf("zsim numa libnuma test 3\n");

    if (numa_available() < 0) {
        printf("NUMA API not supported\n");
        return;
    }

    std::atomic<int> token(0);
    std::mutex mtx;
    std::condition_variable cv;

    const int c0 = 0;
    const int c1 = numa_num_configured_cpus() - 1;
    const int n0 = numa_node_of_cpu(c0);
    const int n1 = numa_node_of_cpu(c1);

    void* gaddr = nullptr;

    auto func0 = [&]() {
        // Set bind policy.
        auto memb = numa_allocate_nodemask();
        numa_bitmask_setbit(memb, n0);
        numa_set_membind(memb);
        numa_free_nodemask(memb);

        // Allocate.
        void* addr = numa_alloc(SIZE);
        assert(touch_and_get_node(addr) == n0);
        numa_free(addr, SIZE); addr = nullptr;

        {
            std::unique_lock<std::mutex> lk(mtx);
            assert(token == 0);

            // Allocate but not touch.
            gaddr = mmap(0, SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);

            token = 1;
            lk.unlock();
            cv.notify_one();
        }
    };

    auto func1 = [&]() {
        // Set bind policy.
        auto memb = numa_allocate_nodemask();
        numa_bitmask_setbit(memb, n1);
        numa_set_membind(memb);
        numa_free_nodemask(memb);

        // Allocate.
        void* addr = numa_alloc(SIZE);
        assert(touch_and_get_node(addr) == n1);
        numa_free(addr, SIZE); addr = nullptr;

        {
            std::unique_lock<std::mutex> lk(mtx);
            cv.wait(lk, [&]{ return token == 1; });

            // Touch pages allocated by the other thread.
            // Will use this thread policy.
            assert(gaddr != nullptr);
            assert(touch_and_get_node(gaddr) == n1);
            munmap(gaddr, SIZE); gaddr = nullptr;

            token = 2;
            lk.unlock();
            cv.notify_one();
        }
    };

    std::thread th0(func0);
    std::thread th1(func1);

    th0.join();
    th1.join();

    printf("zsim numa libnuma test 3 done\n");
}

void test4() {
    printf("zsim numa libnuma test 4\n");

    if (numa_available() < 0) {
        printf("NUMA API not supported\n");
        return;
    }

    const uint32_t page_size = numa_pagesize();
    const int node = 1;

    // Allocate contiguous range.

    void* expected1 = ADDR;
    void* addr1 = mmap(expected1, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    numa_tonode_memory(addr1, page_size, node);
    assert(addr1 == expected1);
    assert(touch_and_get_node(addr1) == node);

    void* expected2 = (void*)((size_t)ADDR + page_size);
    void* addr2 = mmap(expected2, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    numa_tonode_memory(addr2, page_size, node);
    assert(addr2 == expected2);
    assert(touch_and_get_node(addr2) == node);

    // Free partial range.

    munmap(addr2, page_size);
    assert(touch_and_get_node(addr1) == node);

    printf("zsim numa libnuma test 4 done\n");
}

int main(int argc, char* argv[]) {
    if (argc > 1) {
        if (std::string(argv[1]) == "2") {
            test2();
            return 0;
        } else if (std::string(argv[1]) == "3") {
            test3();
            return 0;
        } else if (std::string(argv[1]) == "4") {
            test4();
            return 0;
        }
    }

    test1();
    return 0;
}

