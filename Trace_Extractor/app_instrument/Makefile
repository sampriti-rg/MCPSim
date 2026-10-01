all:
	clang++ -shared -fPIC LoopAnnotationPass.cpp -o LoopAnnotationPass.so `llvm-config --cxxflags`
	clang++ -shared -fPIC BBAnnotationPass.cpp -o BBAnnotationPass.so `llvm-config --cxxflags`
	clang++ -shared -fPIC BBInfoPass.cpp -o BBInfoPass.so `llvm-config --cxxflags`

clean:
	rm *.so