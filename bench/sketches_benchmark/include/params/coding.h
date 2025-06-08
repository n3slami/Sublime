#include <cmath>
#include <string>
#pragma once

#define COUNTER_SIZE 32    //counter_size

#define WORD_SIZE 64

#define MAX_HASH_NUM 16     //hash_number  = d

#define FILTER_SIZE 32

#define KEY_LEN 13

#define testcycles 10

const char cov0[8]={(char)0xff^(1<<0),(char)0xff^(1<<1),(char)0xff^(1<<2),(char)0xff^(1<<3),(char)0xff^(1<<4),(char)0xff^(1<<5),(char)0xff^(1<<6),(char)(0xff^(1<<7))};
int mxtimes = 2;
//memory
const int mem[6]= {0, 100000, 10000, 10000, 1500, 500};
std::string sketch_name = "";
