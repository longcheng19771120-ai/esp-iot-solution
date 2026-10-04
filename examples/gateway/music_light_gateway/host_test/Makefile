# Host build of the platform independent analysis code
SRC_DIR := ../main
CFLAGS  := -O2 -Wall -Wextra -I$(SRC_DIR)
LIB_SRC := $(SRC_DIR)/music_analyzer.c $(SRC_DIR)/mood_estimator.c
LIB_HDR := $(wildcard $(SRC_DIR)/*.h)

all: test_synthetic analyze_wav

test_synthetic: test_synthetic.c $(LIB_SRC) $(LIB_HDR)
	$(CC) $(CFLAGS) -o $@ test_synthetic.c $(LIB_SRC) -lm

analyze_wav: analyze_wav.c $(LIB_SRC) $(LIB_HDR)
	$(CC) $(CFLAGS) -o $@ analyze_wav.c $(LIB_SRC) -lm

test: test_synthetic
	./test_synthetic

clean:
	rm -f test_synthetic analyze_wav

.PHONY: all test clean
