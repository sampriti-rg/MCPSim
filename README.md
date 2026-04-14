MCPSim : Memory-Centric Processing Simulator
----------------------------------------------------

A hybrid Memory-Centric Processing (MCP) simulator based on the basic Ramulator simulator. The simulation support co-simulation between Host processor and MCP processing units and hybrid MCP architecture where the MCP processing units reside in memory (as Near-Memory Processing, i.e., NMP) as well as cache side (as Near-LLC Processing, i.e., NLP). Additionally, It also promotes simulating the application as a single deployment by utilizing compiler-extracted information instead of depending on dynamic profiling tools.

```
+--------+         +--------+         +-----------+
|   CPU  | <<--->> |   LLC  | <<--->> | 3D Memory |
+--------+         +--------+         +-----------+
                   |   NLP  |         |    NMP    |
               |   +--------+         +-----------+   |
               +--------------------------------------+
                     Memory-Centric Processing
```

##### Citation:

Please cite the following papers if you find this simulator useful:

- S. Maity and M. Ghose, “MCPSim: A compiler-integrated co-simulation platform for hybrid memory-centric processing paradigm,” *Journal of Systems Architecture*, vol. 176, p. 103784, Jul. 2026, doi: https://doi.org/10.1016/j.sysarc.2026.103784.

or use BibTex:

```@article{MAITY2026103784,
title = {MCPSim: A compiler-integrated co-simulation platform for hybrid memory-centric processing paradigm},
journal = {Journal of Systems Architecture},
volume = {176},
pages = {103784},
year = {2026},
issn = {1383-7621},
doi = {https://doi.org/10.1016/j.sysarc.2026.103784},
url = {https://www.sciencedirect.com/science/article/pii/S1383762126001025},
author = {Satanu Maity and Manojit Ghose},
keywords = {Simulator, Hybrid memory-centric processing, Near-memory processing, Co-simulation, Compiler-integrated},
abstract = {Memory-Centric Processing (MCP) is an emerging paradigm designed to address the memory wall bottleneck, in which some portion of an application is executed on the host CPU, while other portions are offloaded to the memory side for execution. Although most MCP research relies heavily on simulation tools, current simulators have significant limitations. They often do not accurately model hybrid MCP architectures that enable simultaneous memory-centric computation across the main memory to the cache hierarchy. Additionally, these simulators typically lack robust co-simulation capabilities, in which both the host and MCP-side processors run concurrently. They also rely on offloading methods that necessitate time-consuming pre-execution profiling. To tackle these issues, we present MCPSim, an open-source, trace-driven simulator designed for hybrid MCP systems. MCPSim offers a relevant model for near cache and near main memory processing, a runtime environment that enables concurrent, cycle-accurate co-simulation, and an LLVM-based framework that facilitates compiler-assisted dynamic offloading, enabling end-to-end application execution and performance measurements. Additionally, the proposed simulation design focuses on a single execution session, enabling runtime decisions that eliminate the need for separate, offline profiling tools. An intensive experiment demonstrates that MCPSim is 5.7x faster than the existing simulator, with only a minor 2.9% discrepancy in result accuracy. Furthermore, the compiler-assisted dynamic offloading approach within the MCPSim framework achieves a 29% speedup and 30% energy savings compared to traditional computing systems. The experimental results demonstrate MCPSim’s potential to significantly improve simulation efficiency, inspiring confidence in its application for upcoming research and development in this area.}
}
```

**Prerequisite:**

1. Install using `sudo apt-get install g++ cmake libxerces-c-dev libboost-all-dev nlohmann-json3-dev gdb llvm clang` 

   or just execute

   `sh prerequisite.sh`.

**Installation (normal):**

Just execute the `make` command to compile and install the simulator.

**Installation using VS Code:**

Open the project in <u>VSCode</u> application. `.vscode` contain `tasks.json` to build the project, and `launch.json` to run the project. Under the `Configs` directory `co-sim.cfg` contain the configuration setups.

**Simulation:**

A sample trace of an application and compiler extracted information is available [here](https://drive.google.com/drive/folders/1uOksrm7Lasor4UPxIn1Y6DSBq-pxY1aY?usp=sharing).

Keep the instruction trace files in the `traces/` directory and make changes in configuration file (e.g., `sample.cfg`) as requirements. Then debug/run using `launch.json`.

Or run this command:  `./mcpsim --config Configs/sample.cfg --stats outputs/test.stats --trace traces/app`

A detailed documentation will be uploaded in the `documentation/` directory soon. 


-------------------------------------------------------

A special thanks to [Ramulator-PIM](https://github.com/CMU-SAFARI/ramulator-pim/).
