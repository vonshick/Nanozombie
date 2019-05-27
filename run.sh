#!/bin/bash

mpic++ main.cpp -lpthread -o main 
mpirun -np 4 main 2 2 21 20
