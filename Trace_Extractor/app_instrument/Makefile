all:
	clang++ -shared -fPIC LoopAnnotationPass.cpp -o LoopAnnotationPass.so `llvm-config --cxxflags --ldflags --libs`
	clang++ -shared -fPIC BBAnnotationPass.cpp -o BBAnnotationPass.so `llvm-config --cxxflags --ldflags --libs`

clean:
	rm *.so