#!/bin/bash
cd src/system/kernel/scheduler
g++ -std=c++11 -I../../../../headers/os/storage -I../../../../headers/os/drivers -I../../../../headers/os/support -I../../../../headers/os/kernel -I../../../../headers -I../../../../headers/private -I../../../../headers/private/kernel -I../../../../headers/private/system -I../../../../headers/os -I../../../../src/system/kernel/scheduler -c scheduler_cpu.cpp scheduler_thread.cpp
