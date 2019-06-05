#!/bin/bash

mpic++ main.cpp -Wall -lpthread -o main 
# main args:
# numberOfPonies, numberOfBoats, maxBoatCapacity, maxVisitorCapacity
mpirun -np 4 main 2 2 11 6
