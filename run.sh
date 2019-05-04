#!/bin/bash

mpic++ main.cpp -o main
mpirun main 10 20 30 40
