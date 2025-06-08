//
// Created by QZR on 2023/5/27.
//

#ifndef C_CODE_OURS_FLAG_H
#define C_CODE_OURS_FLAG_H


#include "params/tailored.h"
#include <cstdint>
#include <string.h>
#include <iostream>
#include "MurmurHash.h"
#include <random>
#include <mmintrin.h>
#include "time.h"
#include <stdio.h>
#include <stdlib.h>


#define Min(a,b) 	((a) < (b) ? (a) : (b))
#define THR		(0x0F)
#define  N  (1<<20)

using namespace std;

/*

random_device rd;
//default_random_engine eng(rd());
std::mt19937 eng(rd());
uniform_real_distribution<float> distr(0, 1);
*/



class Tailored {
public:
    int w, d;
    unsigned  char* counter [MAX_HASH_NUM];
    uint64_t COUNTER_SIZE_MAX_CNT = (1ULL << (COUNTER_SIZE - 1)) - 1;
    int hash_seed;
    uint64_t index[MAX_HASH_NUM];    //index of each d
    int w0,w1,w2;
    int count;
    int h1,h2;
    float p0,p1,p2;


public:
    Tailored(int _w, int _d, int _hash_seed = 1001){
        w0 = _w * 1/3 ;
        w1 = _w * 1/3;
        w2 = _w * 1/3;
        d = _d;
        count = 0;
        srand(time(0));
        hash_seed = rand() ;
        counter[0] = new unsigned char[w0]();
        counter[1] = new unsigned char[w1]();
        counter[2] = new unsigned char[w2]();
        h1 = 5;
        h2 = 10;

//
//        int mem_index = 0.2 * (1 << 20);
//        int mem_offset = 0.05 *(1<<20);

        p0 = 1/float(4);
        p1 = 1/float(16);
        p2 = 1/float(32);


    }


    void Insert(const char* str, const uint32_t str_len)
    {
        count++;
        float p_0 = MurmurHash32(str, str_len, hash_seed+count) /  float(0xffffffff);
        if (p_0 <= p0){
            uint64_t hash_v_0 = MurmurHash32(str, str_len, hash_seed+0);
            uint64_t location_0 = hash_v_0 % w0;
            if ( counter[0][location_0] < 0xff){
                counter[0][location_0]++;
            }
        }

        if (p_0 <= p1){
            uint64_t hash_v_1 = MurmurHash32(str, str_len, hash_seed+1);
            uint64_t location_1 = hash_v_1 % w1;
            char t1 = counter[1][location_1];
            if ((t1 & (0x1f)) < 0x1f){
                counter[1][location_1]++ ;
            }
            else{
                bool flag_1 = false;
                uint64_t loc_1 = location_1 | 1;
                int cnt_1 = 1;
                while (cnt_1 <= h1){
                    t1 = counter[1][loc_1];
//                counter[1][loc_1] += 0x40;
                    if ((t1 & 0x40) < 0x40){
                        counter[1][loc_1] += 0x40;
                        flag_1 = true;
                        break;
                    }
                    else{
                        uint32_t tmp_1 = loc_1 & (-loc_1);
                        loc_1 = (loc_1 | (tmp_1 << 1)) ^ tmp_1;
                        cnt_1++;
                    }
                }
                if (flag_1 == true){
                    counter[1][location_1] -= 0x1f;                          //basic-counter 清0
                    counter[1][location_1] =   counter[1][location_1] | 0x20;
                    int i_1 = 1;
                    loc_1 = location_1 | 1;
                    while (i_1 < cnt_1 ){
                        counter[1][loc_1] += 0x40;
                        counter[1][loc_1] =  counter[1][loc_1]  | 0x80;
                        uint32_t tmp_1 = loc_1 & (-loc_1);
                        loc_1 = (loc_1 | (tmp_1 << 1)) ^ tmp_1;
                        i_1++;
                    }
                }
            }
        }

        if (p_0 <= p2){
            uint64_t hash_v_2 = MurmurHash32(str, str_len, hash_seed+2);
            uint64_t location_2 = hash_v_2 % w2;
            char t2 = counter[2][location_2];
            if ((t2 & 0x0f) < 0x0f){
                counter[2][location_2] ++;
            }
            else{
                bool flag_2 = false;
                uint64_t loc_2 = location_2 | 1;
                int cnt2 = 1;
                while (cnt2 <=  h2 ){
                    char t2 =  counter[2][loc_2];
                    if ( (t2 & 0x60)  < 0x60 ){
                        counter[2][loc_2] += 0x20;
                        flag_2 = true;
                        break;
                    }
                    else{
                        uint32_t tmp_2 = loc_2 & (-loc_2);
                        loc_2 = (loc_2 | (tmp_2 << 1)) ^ tmp_2;
                        cnt2++;
                    }
                }

                if (flag_2 == true){
                    counter[2][location_2] -= 0x0f;
                    counter[2][location_2] =  counter[2][location_2] | 0x10;
                    int i_2 = 1;
                    loc_2 = location_2 | 1;
                    while (i_2 < cnt2){
                        counter[2][loc_2]  += 0x20;
                        counter[2][loc_2] = counter[2][loc_2] | 0x80;
                        uint32_t tmp_2 = loc_2 & (-loc_2);
                        loc_2 = (loc_2 | (tmp_2 << 1)) ^ tmp_2;
                        i_2++;
                    }
                }
            }
        }
    }




    int Query(const char* str, const uint32_t str_len) {
        uint32_t query = UINT32_MAX;
        uint64_t hash_v_0 = MurmurHash32(str, str_len, hash_seed+0);
        uint64_t location_0 = hash_v_0 % w0;
        char v_0 = counter[0][location_0];
        if( v_0 < 0xff){
            query = Min(v_0 / p0 ,query);
        }
        uint64_t hash_v_1 = MurmurHash32(str, str_len, hash_seed+1);
        uint64_t location_1 = hash_v_1 % w1;
        if ((counter[1][location_1] & 0x20) != 0x20){
            int temp_value= counter[1][location_1] & 0x1f;
            query = Min(temp_value / p1 ,query);
        }
        else{
            int temp_value= counter[1][location_1] & 0x1f;
            uint64_t loc = location_1 | 1;
            int cnt_1 = 1;
            while (cnt_1 <= h1){
                char t1 = counter[1][loc];
                temp_value += ((t1 & 0x40) >> 6) * 32 * pow(2,cnt_1 -1);
                uint32_t tmp = loc & (-loc);
                loc = (loc | (tmp << 1)) ^ tmp;
                cnt_1++;
                if ((t1 & 0x80) != 0x80)
                {
                    break;
                }
            }
            if (temp_value < 255){
                query = Min(temp_value / p1 ,query);
            }
        }
//        uint64_t loc = location_1 | 1;
//        int cnt = 1;
////    int coefficient = 64;
//        while (cnt <= 2)
//        {
//            char t1= counter[1][loc] ;
//            temp_value += ((t1 & 0xc0)>> 6) * 64 * pow(4,cnt-1);
//            uint32_t tmp = loc & (-loc);
//            loc = (loc | (tmp << 1)) ^ tmp;
//            cnt++;
//        }
//        if (temp_value < 1023)
//        {
//            query = Min(temp_value / p1 ,query);
//        }


        uint64_t hash_v_2 = MurmurHash32(str, str_len, hash_seed+2);
        uint64_t location_2 = hash_v_2 % w2;

        if ((counter[2][location_2] & 0x10) != 0x10){
            int v_2 = counter[2][location_2] & 0x0f;
            query = Min(v_2/ p2 ,query);
        }
        else{
            int v_2 = counter[2][location_2] & 0x0f;
            uint64_t loc_2 = location_2 | 1;
            int cnt_2 = 1;
            while (cnt_2 <= h2){
                char t2 = counter[2][loc_2];
                v_2 += ((t2  & 0x60) >> 5)* 16 * pow(4,cnt_2-1);
                uint32_t tmp_2 = loc_2 & (-loc_2);
                loc_2 = (loc_2 | (tmp_2 << 1)) ^ tmp_2;
                cnt_2 ++;
                if ((t2 & 0x80) != 0x80){
                    break;
                }
            }
            if (v_2 < pow(2,24)-1){
                query = Min(v_2 /p2 ,query);
            }
        }

        if (query == 0){
            return 1;
        }
        else{
            return query;
        }

    }

    size_t Size() {
        return w0 + w1 + w2;
    }


//void Delete(const char* str, const uint32_t str_len){
//    for (int i = 0; i < d; i++){
//        index[i] =  MurmurHash32(str, str_len, hash_seed) % w;
//        counter[i][index[i]] --;
//    }
//}



    ~Tailored(){
        for (int i = 0; i < d; i++){
            delete[]counter[i];
        }
    }
};














#endif //C_CODE_OURS_FLAG_H
