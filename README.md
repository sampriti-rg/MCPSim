MCPSim : Memory-Centric Processing Simulator
----------------------------------------------------

A hybrid Memory-Centric Processing (MCP) simulator based on the basic Ramulator simulator. The simulation support co-simulation between Host processor and MCP processing units and hybrid MCP architecture where the MCP processing units reside in memory (as Near-Memory Processing, i.e., NMP) as well as cache side (as Near-LLC Processing, i.e., NLP). Additionally, It also promotes simulating the application as a single deployment by utilizing compiler-extracted information instead of depending on dynamic profiling tools.

```
+--------+         +--------+         +--------+
|   CPU  | <<--->> |   LLC  | <<--->> | Memory |
+--------+         +--------+         +--------+
                   |   NLP  |         |   NMP  |
               |   +--------+         +--------+   |
               +-----------------------------------+
                     Memory-Centric Processing
```

**prerequisite:**

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