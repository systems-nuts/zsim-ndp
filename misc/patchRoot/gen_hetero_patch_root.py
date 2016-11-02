#! /usr/bin/python
"""
Heterogeneous patch root generator.
"""

import argparse
import os
import string
import sys

CPU_ACT = [int(x) for x in
           '665084 119979939 9019834 399242499 472611 20 159543 0 0 0'.split(' ')]

class XTemplate(string.Template):
    ''' Template class. '''
    delimiter = '$'
    escaped = '$$'


def get_template(fname, suffix='.template'):
    ''' Open the template. '''
    temp_dir = os.path.dirname(os.path.abspath(__file__))
    return XTemplate(open(os.path.join(temp_dir, fname + suffix), 'r').read())


def make_dir(*args):
    ''' Make a directory. '''
    cur = os.path.join(*args)
    os.makedirs(cur)
    if not os.path.exists(cur):
        raise OSError('Fails to create directory {}'.format(cur))
    return cur


def create_file(*args):
    ''' Open a new file to write. '''
    return open(os.path.join(*args), 'w')


def get_mask(vals, size):
    ''' Generate a bitmask in the format as 00110010,11001101, i.e., group of 32 bit. '''
    cur = 0
    l = []
    for i in range(size):
        if i in vals:
            cur |= 1 << (i % 32)
        if (i + 1) % 32 == 0 or i == size - 1:
            l.append(cur)
            cur = 0
    l.reverse()
    return ','.join(['{:08x}'.format(n) for n in l])


def get_list(vals):
    ''' Generate a list of numbers in the format as 0-2,5. '''
    rnglst = []
    for v in sorted(vals):
        if rnglst and v == rnglst[-1][-1] + 1:
            rnglst[-1][1:] = [v]
        else:
            rnglst.append([v])
    return ','.join(['-'.join([str(v) for v in r]) for r in rnglst])


def main(args):
    ''' Main. '''
    # pylint: disable=too-many-branches

    ncpus_b = args.bc
    ncpus_l = args.lc
    nmems_b = args.bn
    nmems_l = args.ln
    root = os.path.abspath(args.dir)
    share_nodes = args.share_nodes

    ## Value check.

    # Root directory.
    if os.path.exists(root):
        raise ValueError('Output directory {} already exists, aborting'.format(root))
    make_dir(root)

    # Number of CPUs.
    ncpus = ncpus_b + ncpus_l
    cpu1st_b, cpu1st_l = 0, ncpus_b

    mask_size = 256
    if ncpus < 1:
        raise ValueError('Need >= 1 cores')
    if ncpus > mask_size:
        if ncpus > 2048:
            raise ValueError('Too many cores, currently support up to 2048')
        sys.stderr.write('WARN: These many CPUs have not been tested, '
                         'x2APIC systems may be different...\n'
                         'WARN: Switch to long mask format, up to 2048 cores\n')
        mask_size = 2048

    # Number of NUMA memory nodes.
    if share_nodes:
        if nmems_b != nmems_l:
            raise ValueError('When sharing, the numbers of NUMA nodes must be equal')
        nmems = nmems_b
        mem1st_b, mem1st_l = 0, 0
    else:
        nmems = nmems_b + nmems_l
        mem1st_b, mem1st_l = 0, nmems_b

    c2m = [None for _ in range(ncpus)]
    m2cs = [[] for _ in range(nmems)]
    if nmems < 1:
        raise ValueError('Need >= 1 NUMA memory nodes')
    if nmems_b != 0:
        # Big cores have their nodes.
        if ncpus_b % nmems_b:
            raise ValueError('{} big cores must be evenly distributed among {} nodes'
                             .format(ncpus_b, nmems_b))
        cspm = ncpus_b // nmems_b
        for idx in range(ncpus_b):
            cpu = cpu1st_b + idx
            mem = mem1st_b + idx // cspm
            c2m[cpu] = mem
            m2cs[mem].append(cpu)
    if nmems_l != 0:
        # Little cores have their nodes.
        if ncpus_l % nmems_l:
            raise ValueError('{} little cores must be evenly distributed among {} nodes'
                             .format(ncpus_l, nmems_l))
        cspm = ncpus_l // nmems_l
        for idx in range(ncpus_l):
            cpu = cpu1st_l + idx
            mem = mem1st_l + idx // cspm
            c2m[cpu] = mem
            m2cs[mem].append(cpu)

    print 'Will produce a tree for {}/{} big/little cores with {} {}/{} NUMA memory nodes in {}' \
            .format(ncpus_b, ncpus_l, 'shared' if share_nodes else 'separate',
                    nmems_b, nmems_l, root)

    ## /proc

    procdir = make_dir(root, 'proc')

    # cpuinfo
    cpuinfo_temp_b = get_template('cpuinfo')
    try:
        cpuinfo_temp_l = get_template('cpuinfo.little')
    except IOError:
        # Use the same cpuinfo template for big and little cores.
        cpuinfo_temp_l = get_template('cpuinfo')
    with create_file(procdir, 'cpuinfo') as fh:
        for cpu in range(cpu1st_b, cpu1st_b + ncpus_b):
            fh.write(cpuinfo_temp_b.substitute({'CPU': cpu, 'NCPUS': ncpus}))
        for cpu in range(cpu1st_l, cpu1st_l + ncpus_l):
            fh.write(cpuinfo_temp_l.substitute({'CPU': cpu, 'NCPUS': ncpus}))

    # stat
    stat_temp = get_template('stat')
    cpustat = 'cpu  ' + ' '.join([str(a * ncpus) for a in CPU_ACT])
    for cpu in range(ncpus):
        cpustat += '\ncpu{} '.format(cpu) + ' '.join([str(a) for a in CPU_ACT])
    with create_file(procdir, 'stat') as fh:
        fh.write(stat_temp.substitute({'CPUSTAT': cpustat}))

    # self/status
    selfdir = make_dir(root, 'proc', 'self')
    with create_file(selfdir, 'status') as fh:
        # NOTE: currently only have CPU/memory list.
        fh.write('...\n'
                 'Cpus_allowed:\t{}\n'
                 'Cpus_allowed_list:\t{}\n'
                 'Mems_allowed:\t{}\n'
                 'Mems_allowed_list:\t{}\n'
                 '...\n'.format(get_mask(range(0, ncpus), size=mask_size),
                                get_list(range(0, ncpus)),
                                get_mask(range(0, nmems), size=mask_size),
                                get_list(range(0, nmems))))

    ## /sys

    make_dir(root, 'sys')

    # cpus
    cdir = make_dir(root, 'sys', 'devices', 'system', 'cpu')

    for f in ['online', 'possible', 'present']:
        with create_file(cdir, f) as fh:
            fh.write(get_list(range(0, ncpus)) + '\n')
    with create_file(cdir, 'offline') as fh:
        fh.write('\n')
    with create_file(cdir, 'sched_mc_power_savings') as fh:
        fh.write('0\n')
    with create_file(cdir, 'kernel_max') as fh:
        fh.write(str(mask_size - 1) + '\n')

    for cpu in range(ncpus):
        mem = c2m[cpu]
        csibs = m2cs[mem] if mem is not None else [cpu]
        tsibs = [cpu]    # NOTE: assume single-thread core

        ccdir = make_dir(cdir, 'cpu{}'.format(cpu))
        with create_file(ccdir, 'online') as fh:
            fh.write('1\n')

        ccdir = make_dir(cdir, 'cpu{}'.format(cpu), 'topology')
        with create_file(ccdir, 'core_id') as fh:
            fh.write(str(cpu) + '\n')
        with create_file(ccdir, 'physical_package_id') as fh:
            fh.write((str(mem) if mem is not None else '') + '\n')
        with create_file(ccdir, 'core_siblings') as fh:
            fh.write(get_mask(csibs, size=mask_size) + '\n')
        with create_file(ccdir, 'core_siblings_list') as fh:
            fh.write(get_list(csibs) + '\n')
        with create_file(ccdir, 'thread_siblings') as fh:
            fh.write(get_mask(tsibs, size=mask_size) + '\n')
        with create_file(ccdir, 'thread_siblings_list') as fh:
            fh.write(get_list(tsibs) + '\n')

    # nodes
    ndir = make_dir(root, 'sys', 'devices', 'system', 'node')

    for f in ['online', 'possible']:
        with create_file(ndir, f) as fh:
            fh.write(get_list(range(0, nmems)) + '\n')
    with create_file(ndir, 'has_normal_memory') as fh:
        fh.write(get_list(range(0, nmems)) + '\n')
    with create_file(ndir, 'has_cpu') as fh:
        fh.write(get_list([mem for mem in range(0, nmems) if m2cs[mem]]) + '\n')

    meminfo_temp = get_template('nodeFiles/meminfo')

    for mem in range(nmems):
        csibs = m2cs[mem]

        nndir = make_dir(ndir, 'node{}'.format(mem))
        for cpu in csibs:
            os.symlink(os.path.relpath(os.path.join(cdir, 'cpu{}'.format(cpu)), nndir),
                       os.path.join(nndir, 'cpu{}'.format(cpu)))
        for f in ['numastat', 'scan_unevictable_pages', 'vmstat']:
            with create_file(nndir, f) as fh:
                fh.write(get_template('nodeFiles/' + f, suffix='').substitute({}))
        with create_file(nndir, 'cpumap') as fh:
            fh.write(get_mask(csibs, size=mask_size) + '\n')
        with create_file(nndir, 'cpulist') as fh:
            fh.write(get_list(csibs) + '\n')
        with create_file(nndir, 'meminfo') as fh:
            fh.write(meminfo_temp.substitute({'NODE': mem}))
        with create_file(nndir, 'distance') as fh:
            for mem2 in range(nmems):
                fh.write('10  ' if mem2 == mem else '20  ')
            fh.write('\n')

    # misc
    make_dir(root, 'sys', 'bus', 'pci', 'devices')


    ## make read-only

    for (p, ds, fs) in os.walk(root):
        for d in ds:
            os.chmod(os.path.join(p, d), 0555)
        for f in fs:
            os.chmod(os.path.join(p, f), 0444)

    return 0


def get_parser():
    ''' Argument parser. '''
    parser = argparse.ArgumentParser(
        description='Generate patch root for heterogeneous system')

    parser.add_argument('dir', default='patchRoot',
                        help='Destination directory')

    parser.add_argument('--bc', type=int, default=1,
                        help='Number of big cores')
    parser.add_argument('--lc', type=int, default=0,
                        help='Number of little cores')
    parser.add_argument('--bn', type=int, default=1,
                        help='Number of NUMA memory nodes for big cores')
    parser.add_argument('--ln', type=int, default=0,
                        help='Number of NUMA memory nodes for little cores')

    parser.add_argument('--share-nodes', action='store_true',
                        help='Big and little cores share the same set of NUMA memory nodes')

    return parser


if __name__ == '__main__':
    sys.exit(main(get_parser().parse_args()))

