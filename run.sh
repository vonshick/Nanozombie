#!/bin/bash

mpic++ main.cpp -o main
mpirun main 2 2 60 40
