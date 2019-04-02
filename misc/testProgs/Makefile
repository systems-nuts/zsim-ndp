# Common deps
DEPS=Makefile

default: test_cc_exts

test_cc_exts: test-cc-exts.cpp $(DEPS)
	g++ -O3 -g -o $@ $< --std=c++11 -pthread

run_tests: default
	./test_cc_exts

clean:
	rm -f *.o *.so *.a test_*

.PHONY: clean default

