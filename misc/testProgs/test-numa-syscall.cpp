#include <cassert>
#include <cstdlib>
#include <stdio.h>
#include <string.h>

#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <numaif.h>

#define NNODES 1024
#define MASK_SIZE ((NNODES)/sizeof(unsigned long)/8+1)
#define MASK_NBYTES ((MASK_SIZE)*sizeof(unsigned long))

#define ADDR ((void*)0x20000000)
#define SIZE (1024*1024)

inline int check_err(int ret, const char* msg) {
    if (ret < 0) { perror(msg); }
    return ret;
}

int get_node(void* addr) {
    unsigned long nodemask[MASK_SIZE];
    const unsigned long maxnode = NNODES-1;
    memset(nodemask, 0, MASK_NBYTES);
    int mode = 0;
    assert(0 == check_err(get_mempolicy(&mode, nodemask, maxnode, addr, MPOL_F_ADDR | MPOL_F_NODE), "get_node"));
    return mode;
}

int main() {
    printf("zsim numa syscall test\n");

    const int node = 1;

    int mode = 0;
    unsigned long nodemask[MASK_SIZE];
    const unsigned long maxnode = NNODES-1;
    void* addr = NULL;
    unsigned long flags = 0;

    int retval = 0;

    // get_mempolicy(), flags 0
    memset(nodemask, 0, MASK_NBYTES); mode = 0;
    assert(0 == check_err(get_mempolicy(&mode, nodemask, maxnode, NULL, 0), "get_mempolicy"));
    assert(mode == MPOL_DEFAULT);
    assert(nodemask[0] == 0);

    // set_mempolicy(), MPOL_PREFERRED
    memset(nodemask, 0, MASK_NBYTES); mode = 0;
    mode = MPOL_PREFERRED;
    nodemask[0] = 0x1uL << node;
    assert(0 == check_err(set_mempolicy(mode, nodemask, maxnode), "set_mempolicy"));
    // verify
    memset(nodemask, 0, MASK_NBYTES); mode = 0;
    assert(0 == check_err(get_mempolicy(&mode, nodemask, maxnode, NULL, 0), "get_mempolicy"));
    assert(mode == MPOL_PREFERRED);
    assert(nodemask[0] == 0x1uL << node);
    // allocate
    addr = mmap(ADDR, SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
    assert(addr != NULL && addr == ADDR);
    *((int*)addr) = 1;
    assert(get_node(addr) == node);
    munmap(addr, SIZE); addr = NULL;

    // set_mempolicy(), MPOL_BIND
    memset(nodemask, 0, MASK_NBYTES); mode = 0;
    mode = MPOL_BIND;
    nodemask[0] = 0x1uL << node;
    assert(0 == check_err(set_mempolicy(mode, nodemask, maxnode), "set_mempolicy"));
    // verify
    memset(nodemask, 0, MASK_NBYTES); mode = 0;
    assert(0 == check_err(get_mempolicy(&mode, nodemask, maxnode, NULL, 0), "get_mempolicy"));
    assert(mode == MPOL_BIND);
    assert(nodemask[0] == 0x1uL << node);
    // allocate
    addr = mmap(ADDR, SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
    assert(addr != NULL && addr == ADDR);
    *((int*)addr) = 1;
    assert(get_node(addr) == node);
    munmap(addr, SIZE); addr = NULL;

    // set_mempolicy(), MPOL_INTERLEAVE;
    memset(nodemask, 0, MASK_NBYTES); mode = 0;
    mode = MPOL_INTERLEAVE;
    for (int i = 0; i <= node; i++) nodemask[0] |= (0x1uL << i);
    assert(0 == check_err(set_mempolicy(mode, nodemask, maxnode), "set_mempolicy"));
    // verify
    memset(nodemask, 0, MASK_NBYTES); mode = 0;
    assert(0 == check_err(get_mempolicy(&mode, nodemask, maxnode, NULL, 0), "get_mempolicy"));
    assert(mode == MPOL_INTERLEAVE);
    for (int i = 0; i <= node; i++) assert(nodemask[0] & (0x1uL << i));
    // allocate
    addr = mmap(ADDR, SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
    assert(addr != NULL && addr == ADDR);
    for (int i = 0; i < SIZE / getpagesize(); i++) {
        *((int*)((unsigned long)addr + getpagesize() * i)) = i;
    }
    int j = -1;
    for (int i = 0; i < SIZE / getpagesize(); i++) {
        void* p = (void*)((unsigned long)addr + getpagesize() * i);
        if (j >= 0) {
            assert((j + 1) % (node + 1) == get_node(p));
        }
        j = get_node(p);
    }
    munmap(addr, SIZE); addr = NULL;

    // get_mempolicy(), flags MPOL_F_NODE
    flags = 0; flags |= MPOL_F_NODE;
    mode = 0;
    assert(0 == check_err(get_mempolicy(&mode, NULL, 0, NULL, flags), "get_mempolicy"));
    for (int i = 0; i < 5; i++) {
        // allocate
        addr = mmap(ADDR, SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
        assert(addr != NULL && addr == ADDR);
        *((int*)addr) = 1;
        munmap(addr, SIZE); addr = NULL;
        // next node
        int next = (mode + 1) % (node + 1);
        mode = 0;
        assert(0 == check_err(get_mempolicy(&mode, NULL, 0, NULL, flags), "get_mempolicy"));
        assert(mode == next);
    }

    // set_mempolicy(), MPOL_DEFAULT
    memset(nodemask, 0, MASK_NBYTES); mode = 0;
    mode = MPOL_DEFAULT;
    assert(0 == check_err(set_mempolicy(mode, NULL, 0), "set_mempolicy"));

    // get_mempolicy(), flags MPOL_F_MEMS_ALLOWED
    flags = 0; flags |= MPOL_F_MEMS_ALLOWED;
    memset(nodemask, 0, MASK_NBYTES); mode = 0;
    assert(0 == check_err(get_mempolicy(NULL, nodemask, maxnode, NULL, flags), "get_mempolicy"));
    printf("Mems Allowed: 0x%lx\n", nodemask[0]);

    // mbind(), MPOL_INTERLEAVE
    addr = mmap(ADDR, SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
    assert(addr != NULL && addr == ADDR);
    memset(nodemask, 0, MASK_NBYTES); mode = 0;
    mode = MPOL_INTERLEAVE;
    for (int i = 0; i <= node; i++) nodemask[0] |= (0x1uL << i);
    assert(0 == check_err(mbind(addr, SIZE, mode, nodemask, maxnode, MPOL_MF_STRICT), "mbind"));
    // touch pages
    for (int i = 0; i <= node; i++) *((int*)((unsigned long)addr + getpagesize() * i)) = i;
    // verify
    for (int i = 0; i <= node; i++) assert(get_node((void*)((unsigned long)addr + getpagesize() * i)) == i);
    munmap(addr, SIZE); addr = NULL;

    // mbind(), MPOL_BIND
    addr = mmap(ADDR, SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
    assert(addr != NULL && addr == ADDR);
    memset(nodemask, 0, MASK_NBYTES); mode = 0;
    mode = MPOL_BIND;
    nodemask[0] = 0x1uL << node;
    assert(0 == check_err(mbind(addr, SIZE, mode, nodemask, maxnode, MPOL_MF_STRICT), "mbind"));
    // touch pages
    *((int*)addr) = 1;

    // get_mempolicy(), flags MPOL_F_ADDR
    flags = 0; flags |= MPOL_F_ADDR;
    memset(nodemask, 0, MASK_NBYTES); mode = 0;
    retval = check_err(get_mempolicy(&mode, nodemask, maxnode, addr, flags), "get_mempolicy");
    if (retval == 0) {
        // current zsim will fail on the previous syscall
        assert(mode == MPOL_BIND);
        assert(nodemask[0] == 0x1uL << node);
    }

    // get_mempolicy(), flags MPOL_F_ADDR | MPOL_F_NODE
    flags = 0; flags |= MPOL_F_ADDR | MPOL_F_NODE;
    memset(nodemask, 0, MASK_NBYTES); mode = 0;
    assert(0 == check_err(get_mempolicy(&mode, nodemask, maxnode, addr, flags), "get_mempolicy"));
    assert(mode == node);
    assert(nodemask[0] == 0x1uL << node);

    // mbind(), MPOL_MF_STRICT
    memset(nodemask, 0, MASK_NBYTES); mode = 0;
    mode = MPOL_BIND;
    nodemask[0] = 0x1uL;
    assert(0 != check_err(mbind(addr, SIZE, mode, nodemask, maxnode, MPOL_MF_STRICT), "mbind"));
    assert(errno == EIO);
    errno = 0;

    // mbind(), MPOL_MF_MOVE
    memset(nodemask, 0, MASK_NBYTES); mode = 0;
    mode = MPOL_BIND;
    nodemask[0] = 0x1uL;
    assert(0 == check_err(mbind(addr, SIZE, mode, nodemask, maxnode, MPOL_MF_STRICT | MPOL_MF_MOVE), "mbind"));
    assert(get_node(addr) == 0);
    // move back
    memset(nodemask, 0, MASK_NBYTES); mode = 0;
    mode = MPOL_BIND;
    nodemask[0] = 0x1uL << node;
    assert(0 == check_err(mbind(addr, SIZE, mode, nodemask, maxnode, MPOL_MF_STRICT | MPOL_MF_MOVE), "mbind"));
    assert(get_node(addr) == node);

    // migrate_pages()
    unsigned long nodemask_old[MASK_SIZE];
    memset(nodemask_old, 0, MASK_NBYTES); mode = 0;
    nodemask_old[0] = 0x1uL << node;
    memset(nodemask, 0, MASK_NBYTES); mode = 0;
    nodemask[0] = 0x1uL << 0;
    retval = check_err(migrate_pages(0, maxnode, nodemask_old, nodemask), "migrate_pages");
    // verify
    flags = 0; flags |= MPOL_F_ADDR | MPOL_F_NODE;
    mode = 0;
    assert(0 == check_err(get_mempolicy(&mode, NULL, 0, addr, flags), "get_mempolicy"));
    if (retval == 0) {
        assert(mode == 0);
    } else {
        // current zsim will fail on the previous syscall
        assert(mode == node);
    }

    // move_pages()
    unsigned long count = SIZE / getpagesize();
    void** pages = new void*[count];
    int* nodes = new int[count];
    int* status = new int[count];
    for (unsigned long i = 0; i < count; i++) {
        pages[i] = (void*)((unsigned long)addr + getpagesize() * i);
        nodes[i] = node;
    }
    assert(0 == check_err(move_pages(0, count, pages, nodes, status, MPOL_MF_MOVE), "move_pages"));
    // touch
    *((int*)addr) = 2;
    *((int*)((unsigned long)addr + SIZE - 10)) = 2;
    // verify
    flags = 0; flags |= MPOL_F_ADDR | MPOL_F_NODE;
    mode = 0;
    assert(0 == check_err(get_mempolicy(&mode, NULL, 0, addr, flags), "get_mempolicy"));
    assert(mode == node);
    assert(0 == check_err(get_mempolicy(&mode, NULL, 0, (void*)((unsigned long)addr + SIZE - 10), flags), "get_mempolicy"));
    assert(mode == node);

    // move_pages(), get nodes
    assert(0 == check_err(move_pages(0, 1, &addr, NULL, &mode, MPOL_MF_MOVE), "move_pages"));
    assert(mode == node);

    munmap(addr, SIZE);
    delete[] pages;
    delete[] nodes;
    delete[] status;

    printf("zsim numa syscall test done\n");

    return 0;
}

