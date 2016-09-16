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

#define SIZE 1024*1024

inline int check_err(int ret, const char* msg) {
    if (ret < 0) { perror(msg); }
    return ret;
}

int main() {
    printf("zsim numa syscall test\n");

    const int node = 1;

    int mode = 0;
    unsigned long nodemask[MASK_SIZE];
    const unsigned long maxnode = NNODES-1;

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
    retval = check_err(set_mempolicy(mode, nodemask, maxnode), "set_mempolicy");
    // verify
    memset(nodemask, 0, MASK_NBYTES); mode = 0;
    assert(0 == check_err(get_mempolicy(&mode, nodemask, maxnode, NULL, 0), "get_mempolicy"));
    if (retval == 0) {
        // current zsim will fail on the previous set_mempolicy()
        assert(mode == MPOL_PREFERRED);
        assert(nodemask[0] == 0x1uL << node);
    }

    // set_mempolicy(), MPOL_BIND
    memset(nodemask, 0, MASK_NBYTES); mode = 0;
    mode = MPOL_BIND;
    nodemask[0] = 0x1uL << node;
    retval = check_err(set_mempolicy(mode, nodemask, maxnode), "set_mempolicy");
    // verify
    memset(nodemask, 0, MASK_NBYTES); mode = 0;
    assert(0 == check_err(get_mempolicy(&mode, nodemask, maxnode, NULL, 0), "get_mempolicy"));
    if (retval == 0) {
        // current zsim will fail on the previous set_mempolicy()
        assert(mode == MPOL_BIND);
        assert(nodemask[0] == 0x1uL << node);
    }

    // set_mempolicy(), MPOL_DEFAULT
    memset(nodemask, 0, MASK_NBYTES); mode = 0;
    mode = MPOL_DEFAULT;
    assert(0 == check_err(set_mempolicy(mode, NULL, 0), "set_mempolicy"));

    // get_mempolicy(), flags MPOL_F_MEMS_ALLOWED
    unsigned long flags = 0;
    flags = 0; flags |= MPOL_F_MEMS_ALLOWED;
    memset(nodemask, 0, MASK_NBYTES); mode = 0;
    assert(0 == check_err(get_mempolicy(NULL, nodemask, maxnode, NULL, flags), "get_mempolicy"));
    printf("Mems Allowed: 0x%lx\n", nodemask[0]);

    // mbind()
    void* addr = NULL;
    addr = mmap(0, SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
    assert(addr != NULL);
    memset(nodemask, 0, MASK_NBYTES); mode = 0;
    mode = MPOL_BIND;
    nodemask[0] = 0x1uL << node;
    assert(0 == check_err(mbind(addr, SIZE, mode, nodemask, maxnode, MPOL_MF_STRICT), "mbind"));
    // touch pages
    int* data = (int*)addr;
    *data = 1;

    // get_mempolicy(), flags MPOL_F_ADDR
    flags = 0; flags |= MPOL_F_ADDR;
    memset(nodemask, 0, MASK_NBYTES); mode = 0;
    assert(0 == check_err(get_mempolicy(&mode, nodemask, maxnode, addr, flags), "get_mempolicy"));
    assert(mode == MPOL_BIND);
    assert(nodemask[0] == 0x1uL << node);

    // MPOL_F_ADDR | MPOL_F_NODE
    flags = 0; flags |= MPOL_F_ADDR | MPOL_F_NODE;
    memset(nodemask, 0, MASK_NBYTES); mode = 0;
    assert(0 == check_err(get_mempolicy(&mode, nodemask, maxnode, addr, flags), "get_mempolicy"));
    assert(mode == node);
    assert(nodemask[0] == 0x1uL << node);

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
    data = (int*)addr;
    *data = 2;
    data = (int*)((unsigned long)addr + SIZE - 10);
    *data = 2;
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

