#!/bin/bash

mpic++ main.cpp -lpthread -o main 
# main args:
# numberOfPonies, numberOfBoats, maxBoatCapacity, maxVisitorCapacity
mpirun -np 2 main 1 2 11 6
