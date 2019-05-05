#!/bin/bash

mpic++ main.cpp -lpthread -o main 
mpirun main 2 2 60 40
