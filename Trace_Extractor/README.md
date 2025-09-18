# MCPSim tracer

The included PIN tool pass `MCPSimTracer.cpp` can be used to generate new traces. It has been tested using PIN 3.22.

## Download and install PIN tool

Download the source of PIN from Intel's website, then build it in `Trace_Extractor/` location (or just run the `setup.sh`, it will download and install automatically).

```bash
$ wget https://software.intel.com/sites/landingpage/pintool/downloads/pin-3.22-98547-g7a303a835-gcc-linux.tar.gz
$ tar zxf pin-3.22-98547-g7a303a835-gcc-linux.tar.gz
$ cd pin-3.22-98547-g7a303a835-gcc-linux/source/tools
$ make
```



## Building the tracer

The provided Makefile will generate `obj-intel64/MCPSimTracer.so`.

```bash
$ export PIN_ROOT=/your/path/to/pin
$ make
$ $PIN_ROOT/pin -t obj-intel64/MCPSimTracer.so -- <your program here>
```
e.g., ```$PIN_ROOT/pin -t obj-intel64/MCPSimTracer.so -e 10000 -p 4 -- app_instrument/bfs_app/bfs --dataset app_instrument/bfs_app/fb --separator , --threadnum 8```


The tracer has three options you can set:

```bash
-o <filename> : Specify the output file for your trace. The default is mcpsim.trace.
-s <number> : Specify the number of instructions to skip in the program before tracing begins. The default value is 0.
-e <number> : The number of instructions to trace, after -s instructions have been skipped. The default value is 1,000,000.
-p <number> : Specifies the process identifier (<id>) that corresponds to the compiler-generated data file, named using the pattern proc_<id>_bb_info.json.
```

For example, you could trace 200,000 instructions of the program bfs, after skipping the first 100,000 instructions, with this command:

```bash
$ $PIN_ROOT/pin -t obj-intel64/MCPSimTracer.so  -s 100000 -e 200000 -o ../traces/bfs.0 -p 4 -- app_instrument/bfs_app/bfs --dataset app_instrument/bfs_app/fb --separator , --threadnum 8
```

Details of the trace format can be found in the file `trace_format.h`.