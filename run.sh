#!/bin/bash

mpic++ main.cpp -lpthread -o main 
# main args:
# numberOfPonies, numberOfBoats, maxBoatCapacity, maxVisitorCapacity
mpirun -np 3 main 2 2 11 6
