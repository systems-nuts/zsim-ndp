#!/bin/bash
if [ "$#" -ne 1 ]; then
    echo "Illegal parameter. Usage: $0 listening_port"
    exit 1
fi
echo "Listening port $1... Please start zsim with attachDebugger on and the same sim.debugPortId" 
OUTPUT=$(nc -l $1)
# echo "${OUTPUT}"
eval "${OUTPUT}"
