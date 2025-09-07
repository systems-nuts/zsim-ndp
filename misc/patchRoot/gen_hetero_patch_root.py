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


def parse_distance_file(distance_file, expected_nodes):
    """
    Parse a distance matrix file and return distance matrix.
    Similar to what a `numactl --hardware` would give.
    Expected format:
    node   0   1   2   3
      0:  10  16  16  16
      1:  16  10  16  16
      2:  16  16  10  16
      3:  16  16  16  10
    """
    if not os.path.exists(distance_file):
        raise ValueError('Distance file {} does not exist'.format(distance_file))

    with open(distance_file, 'r') as f:
        lines = [line.strip() for line in f.readlines() if line.strip()]

    if not lines:
        raise ValueError('Distance file {} is empty'.format(distance_file))

    # Parse header line
    header = lines[0].split()
    if header[0] != 'node':
        raise ValueError('Distance file must start with "node" header')

    # Extract node numbers from header
    try:
        header_nodes = [int(x) for x in header[1:]]
    except ValueError:
        raise ValueError('Invalid node numbers in header: {}'.format(' '.join(header[1:])))

    # Check if header nodes match expected topology
    expected_node_list = list(range(expected_nodes))
    if sorted(header_nodes) != expected_node_list:
        raise ValueError('Distance file nodes {} do not match expected nodes {}'.format(
            sorted(header_nodes), expected_node_list))

    # Parse distance matrix
    distance_matrix = {}
    for line in lines[1:]:
        parts = line.split()
        if len(parts) < 2:
            continue

        # Extract node ID (remove trailing colon if present)
        node_str = parts[0].rstrip(':')
        try:
            node_id = int(node_str)
        except ValueError:
            raise ValueError('Invalid node ID: {}'.format(node_str))

        # Extract distances
        try:
            distances = [int(x) for x in parts[1:]]
        except ValueError:
            raise ValueError('Invalid distance values in line: {}'.format(line))

        if len(distances) != len(header_nodes):
            raise ValueError('Node {} has {} distances but header has {} nodes'.format(
                node_id, len(distances), len(header_nodes)))

        # Map distances according to header order
        distance_matrix[node_id] = {}
        for i, target_node in enumerate(header_nodes):
            distance_matrix[node_id][target_node] = distances[i]

    # Verify all expected nodes are present
    for node in expected_node_list:
        if node not in distance_matrix:
            raise ValueError('Missing distance data for node {}'.format(node))

    # Verify matrix is symmetric and the diagonal makes sense
    for i in expected_node_list:
        for j in expected_node_list:
            if j not in distance_matrix[i]:
                raise ValueError('Missing distance from node {} to node {}'.format(i, j))

            # Check symmetry
            if distance_matrix[i][j] != distance_matrix[j][i]:
                raise ValueError('Distance matrix is not symmetric: distance({},{}) = {} != distance({},{}) = {}'.format(
                    i, j, distance_matrix[i][j], j, i, distance_matrix[j][i]))

            # Check diagonal (local distance should make sense)
            if i == j and distance_matrix[i][j] != 10:
                sys.stderr.write('WARN: Local distance for node {} is {} (expected 10)\n'.format(
                    i, distance_matrix[i][j]))

    return distance_matrix


def main(args):
    ''' Main. '''
    # pylint: disable=too-many-branches

    ncpus_b = args.bc
    ncpus_l = args.lc
    nmems_b = args.bn
    nmems_l = args.ln
    root = os.path.abspath(args.dir)
    share_nodes = args.share_nodes
    distance_file = args.distance_file

    ## Value check.

    # Root directory.
    if os.path.exists(root):
        raise ValueError('Output directory {} already exists, aborting'.format(root))
    make_dir(root)

    # Number of CPUs.
    ncpus = ncpus_b + ncpus_l
    cpu1st_b, cpu1st_l = 0, ncpus_b

    mask_size = 64
    if ncpus < 1:
        raise ValueError('Need >= 1 cores')
    if ncpus > mask_size:
        # Increase mask size to a power-of-two that just covers all cores
        mask_size = 1 << ((ncpus - 1).bit_length())
        assert mask_size >= ncpus
        sys.stderr.write('WARN: These many CPUs have not been tested, '
                         'x2APIC systems may be different...\n'
                         'WARN: Switch to long mask format with {} cores\n'
                         .format(mask_size))

    # Number of NUMA memory nodes.
    if share_nodes:
        if nmems_b != nmems_l:
            raise ValueError('When sharing, the numbers of NUMA nodes must be equal')
        nmems = nmems_b
        mem1st_b, mem1st_l = 0, 0
    else:
        nmems = nmems_b + nmems_l
        mem1st_b, mem1st_l = 0, nmems_b

    # Parse distance matrix if provided
    distance_matrix = None
    if distance_file:
        distance_matrix = parse_distance_file(distance_file, nmems)

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

    print('Will produce a tree for {}/{} big/little cores '
          'with {} {}/{} NUMA memory nodes in {}'
          .format(ncpus_b, ncpus_l, 'shared' if share_nodes else 'separate',
                  nmems_b, nmems_l, root))
    if distance_matrix:
        print('Using custom distance matrix from {}'.format(distance_file))

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
                if distance_matrix:
                    distance = distance_matrix[mem][mem2]
                else:
                    distance = 10 if mem2 == mem else 20
                fh.write('{}  '.format(distance))
            fh.write('\n')

    # misc
    make_dir(root, 'sys', 'bus', 'pci', 'devices')


    ## make read-only

    for (p, ds, fs) in os.walk(root):
        for d in ds:
            os.chmod(os.path.join(p, d), 0o555)
        for f in fs:
            os.chmod(os.path.join(p, f), 0o444)

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

    parser.add_argument('--distance-file', type=str,
                        help='File containing custom NUMA distance matrix')

    return parser


if __name__ == '__main__':
    sys.exit(main(get_parser().parse_args()))
