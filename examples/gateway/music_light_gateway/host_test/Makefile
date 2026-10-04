# Host build of the platform independent analysis code
SRC_DIR := ../main
CFLAGS  := -O2 -Wall -Wextra -I$(SRC_DIR)
LIB_SRC := $(SRC_DIR)/music_analyzer.c $(SRC_DIR)/genre_classifier.c

all: test_synthetic analyze_wav

test_synthetic: test_synthetic.c $(LIB_SRC)
	$(CC) $(CFLAGS) -o $@ $^ -lm

analyze_wav: analyze_wav.c $(LIB_SRC)
	$(CC) $(CFLAGS) -o $@ $^ -lm

test: test_synthetic
	./test_synthetic

clean:
	rm -f test_synthetic analyze_wav

.PHONY: all test clean
