# Application Instrumentation and Compilation

NMPSim provides two LLVM passes to compile an application and extract static information (e.g., `proc_{id}_bb_info.json`) from different offloadable regions.

*   **BBAnnotationPass.cpp**: Extracts information (such as the number of memory and non-memory instructions) from each basic block of the source code.
*   **LoopAnnotationPass.cpp**: Only instrumented all loop regions of the application.

To make these passes accessible using LLVM `opt`, a `Makefile` is supplied which compiles these passes into an LLVM library.

Additionally, two application directories (`bfs_app` and `kmeans_app`) provide a sample of application compilation using the LLVM passes via their own dedicated `Makefiles`.