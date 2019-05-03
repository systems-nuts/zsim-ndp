#!/usr/bin/python
# Produces a list of syscalls in the current system
import os, re
filePattern = "/usr/include/{}/unistd.h"
fileName = filePattern.format("asm")
if not os.path.exists(fileName):
    fileName = filePattern.format("asm-generic")
    assert os.path.exists(fileName)
syscallCmd = "gcc -E -dD {} | grep __NR".format(fileName)
syscallDefs = os.popen(syscallCmd).read()
sysList = [(int(numStr), name) for (name, numStr) in re.findall("#define __NR_(.*?) (\d+)", syscallDefs)]
denseList = ["INVALID"]*(max([num for (num, name) in sysList]) + 1)
for (num, name) in sysList: denseList[num] = name
print '"' + '",\n"'.join(denseList) + '"'
