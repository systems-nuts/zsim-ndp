#include <cassert>
#include <cstdlib>
#include <stdio.h>
#include <string.h>

#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <numa.h>

#define NNODES 1024
#define MASK_SIZE ((NNODES)/sizeof(unsigned long)/8+1)
#define MASK_NBYTES ((MASK_SIZE)*sizeof(unsigned long))

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

int main() {
    printf("zsim numa libnuma test\n");

    // numa_available()
    if (numa_available() < 0) {
        printf("NUMA API not supported\n");
        return 0;
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

    // Not supported:
    // numa_get_interleave_node()
    // numa_get_interleave_mask(), numa_set_interleave_mask()
    // numa_interleave_memory()
    // numa_alloc_interleaved(), numa_alloc_interleaved_subset()

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
    numa_bitmask_free(nodemask);
    nodemask = numa_get_membind();
    assert(numa_bitmask_isbitset(nodemask, n));
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
    numa_bitmask_free(tonode_bmp);
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

    // Not supported:
    // numa_set_bind_policy(), numa_set_strict()

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
        numa_bitmask_free(nodemaps[i]);
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

    // numa_move_pages()
    unsigned long count = SIZE / getpagesize();
    void** pages = new void*[count];
    int* nodes = new int[count];
    int* status = new int[count];
    addr = numa_alloc_onnode(SIZE, n);
    for (unsigned long i = 0; i < count; i++) {
        pages[i] = (void*)((unsigned long)addr + getpagesize() * i);
        nodes[i] = n-1;
    }
    for (unsigned long i = 0; i < count; i++) {
        assert(touch_and_get_node((void*)((unsigned long)addr + getpagesize() * i)) == n);
    }
    assert(0 == numa_move_pages(0, count, pages, nodes, status, 0));
    for (unsigned long i = 0; i < count; i++) {
        assert(touch_and_get_node((void*)((unsigned long)addr + getpagesize() * i)) == n-1);
    }
    numa_free(addr, SIZE);

    // numa_move_pages(), get nodes, in touch_and_get_node()

    printf("zsim numa libnuma test done\n");

    return 0;
}


