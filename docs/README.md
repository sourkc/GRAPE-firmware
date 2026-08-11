# GRAPE documentation

GRAPE (stands for **G**raphics **R**endering & **A**cceleration **P**latform for **E**mbedded systems) is an
Open Source graphics co-processor unit based on the ESP32-P4 chip by Espressif.

GRAPE consists of both a reusable hardware design and the supporting
graphics software stack. The hardware can be integrated into embedded
projects either by reproducing the published schematics/PCB design or
by using a GRAPE module through its board-to-board interface.

This is the documentation for the code and architecture of GRAPE. 

## List of links
### [Architecture](ARCHITECTURE.md)
Describes the architecture of GRAPE and the overall structure of the project. 

## Important notes for contributors!
### [PERFORMANCE.md](PERFORMANCE.md)
This file contains important optimizations and performance bottlenecks when writing code, found out
experimentally. PLEASE read through this before writing code and use it as your guide to write code
that works fast