#!/bin/bash

mpic++ main.cpp -lpthread -o main 
mpirun main 4 4 21 20
