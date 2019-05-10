/** $lic$
 * Copyright (C) 2012-2015 by Massachusetts Institute of Technology
 * Copyright (C) 2010-2013 by The Board of Trustees of Stanford University
 *
 * This file is part of zsim.
 *
 * zsim is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, version 2.
 *
 * If you use this software in your research, we request that you reference
 * the zsim paper ("ZSim: Fast and Accurate Microarchitectural Simulation of
 * Thousand-Core Systems", Sanchez and Kozyrakis, ISCA-40, June 2013) as the
 * source of the simulator in any publications that use this software, and that
 * you send us a citation of your work.
 *
 * zsim is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "debug_zsim.h"
#include <fcntl.h>
#include <link.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "log.h"

/* This file is pretty much self-contained, and has minimal external dependencies.
 * Please keep it this way, and ESPECIALLY don't include Pin headers since there
 * seem to be conflicts between those and some system headers.
 */

// From gelf.h in libelf, it is assumed that 64-bit binaries are the largest
// class and therefore all other classes can be represented without loss.
typedef Elf64_Ehdr Elf_Ehdr;
typedef Elf64_Shdr Elf_Shdr;

// References in Linux code base.
// kernel/module-internal.h: struct load_info
// kernel/module.c: SYSCALL_DEFINE3(init_module, ...)
// arch/x86/kernel/module.c: module_finalize()

static int pp_callback(dl_phdr_info* info, size_t size, void* data) {
    if (strstr(info->dlpi_name, "libzsim.so")) {
        int fd;
        if ((fd = open (info->dlpi_name, O_RDONLY , 0)) < 0)
            panic("Opening %s failed", info->dlpi_name);
        struct stat st;
        if (fstat(fd, &st))
            panic("Getting %s stat failed", info->dlpi_name);
        auto size = st.st_size;
        void* addr = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
        if (unlikely(addr == MAP_FAILED))
            panic("Loading %s failed", info->dlpi_name);
        auto hdr = static_cast<Elf_Ehdr*>(addr);

        // Get section header table.
        size_t shoff = hdr->e_shoff;
        size_t shnum = hdr->e_shnum;
        if (unlikely(!shoff))
            panic("Section header table does not exist.")
        auto shdr = reinterpret_cast<Elf_Shdr*>(reinterpret_cast<uintptr_t>(addr) + shoff);

        // Get entry for the section name string table.
        size_t shstrndx = hdr->e_shstrndx;
        if (unlikely(shstrndx == SHN_XINDEX))
            panic("Large index for section name string table (SHN_XINDEX) is not handled");
        auto shstr = reinterpret_cast<char*>(reinterpret_cast<uintptr_t>(addr) + shdr[shstrndx].sh_offset);

        LibInfo* offsets = static_cast<LibInfo*>(data);
        offsets->textAddr = nullptr;
        offsets->dataAddr = nullptr;
        offsets->bssAddr = nullptr;

        // Scan each section.
        for (Elf_Shdr* s = shdr; s < shdr + shnum; s++) {
            char* name = shstr + s->sh_name;
            void* sectionAddr = reinterpret_cast<void*>(info->dlpi_addr + s->sh_addr);
            if (strcmp(".text", name) == 0) {
                offsets->textAddr = sectionAddr;
            } else if (strcmp(".data", name) == 0) {
                offsets->dataAddr = sectionAddr;
            } else if (strcmp(".bss", name) == 0) {
                offsets->bssAddr = sectionAddr;
            }
        }
        munmap(addr, size);
        close(fd);

        //Check that we got all the section addresses; it'd be extremely weird if we didn't
        assert(offsets->textAddr && offsets->dataAddr && offsets->bssAddr);

        return 1; //stops iterating
    }
    return 0; //continues iterating
}

void getLibzsimAddrs(LibInfo* libzsimAddrs) {
    int ret = dl_iterate_phdr(pp_callback, libzsimAddrs);
    if (ret != 1) panic("libzsim.so not found");
}


void notifyHarnessForDebugger(int harnessPid) {
    kill(harnessPid, SIGUSR1);
    sleep(1); //this is a bit of a hack, but ensures the debugger catches us
}
