// gcc  -std=c99 -O3 -o simpletest simpletest.c -Wall -Wextra -lm

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <assert.h>
#include <x86intrin.h>

typedef uint16_t value_t;

struct pcg_state_setseq_64 {    // Internals are *Private*.
    uint64_t state;             // RNG state.  All values are possible.
    uint64_t inc;               // Controls which RNG sequence (stream) is
    // selected. Must *always* be odd.
};
typedef struct pcg_state_setseq_64 pcg32_random_t;


static pcg32_random_t pcg32_global = { 0x853c49e6748fea9bULL, 0xda3e39cb94b95bdbULL };


static inline uint32_t pcg32_random_r(pcg32_random_t* rng) {
    uint64_t oldstate = rng->state;
    rng->state = oldstate * 6364136223846793005ULL + rng->inc;
    uint32_t xorshifted = ((oldstate >> 18u) ^ oldstate) >> 27u;
    uint32_t rot = oldstate >> 59u;
    return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
}

static inline uint32_t pcg32_random() {
    return pcg32_random_r(&pcg32_global);
}

int qsort_compare_value_t(const void *a, const void *b) {
    return ( *(value_t *)a - *(value_t *)b );
}
value_t *create_sorted_array(size_t length) {
    value_t *array = malloc(length * sizeof(value_t));
    for (size_t i = 0; i < length; i++) array[i] = (value_t) pcg32_random();
    qsort(array, length, sizeof(*array), qsort_compare_value_t);
    return array;
}

value_t *create_random_array(size_t count) {
    value_t *targets = malloc(count * sizeof(value_t));
    for (size_t i = 0; i < count; i++) targets[i] = (value_t) pcg32_random();
    return targets;
}

// flushes the array from cache
void array_cache_flush(value_t* B, int32_t length) {
    const int32_t CACHELINESIZE = 64;// 64 bytes per cache line
    for(int32_t  k = 0; k < length; k += CACHELINESIZE/sizeof(value_t)) {
        __builtin_ia32_clflush(B + k);
    }
}

// tries to put the array in cache
void array_cache_prefetch(value_t* B, int32_t length) {
    const int32_t CACHELINESIZE = 64;// 64 bytes per cache line
    for(int32_t  k = 0; k < length; k += CACHELINESIZE/sizeof(value_t)) {
        __builtin_prefetch(B + k);
    }
}

// good old bin. search
int32_t __attribute__ ((noinline)) binary_search(uint16_t * array, int32_t lenarray, uint16_t ikey )  {
    int32_t low = 0;
    int32_t high = lenarray - 1;
    while( low <= high) {
        int32_t middleIndex = (low+high) >> 1;
        int32_t middleValue = array[middleIndex];
        if (middleValue < ikey) {
            low = middleIndex + 1;
        } else if (middleValue > ikey) {
            high = middleIndex - 1;
        } else {
            return middleIndex;
        }
    }
    return -(low + 1);
}

int32_t __attribute__ ((noinline)) simple_linear_search(uint16_t* source, int32_t n, uint16_t target) {
    int32_t length = n;
    for(int32_t k = 0; k  < length; k ++) {
        uint16_t val = source[k];
        if(val >= target) {
            if(val == target) return k;
            else return - k - 1;
        }
    }
    return - length - 1;
}

// breaks the contract
int32_t __attribute__ ((noinline)) justequal_linear_search(uint16_t* source, int32_t n, uint16_t target) {
    int32_t length = n;
    for(int32_t k = 0; k  < length; k ++) {
        uint16_t val = source[k];
        if(val == target) return k;
    }
    return - 1;
}

//  Paul-Virak Khuong and Pat Morin, http://arxiv.org/pdf/1509.05053.pdf
int32_t __attribute__ ((noinline)) branchless_binary_search(uint16_t* source, int32_t n, uint16_t target) {
    uint16_t * base = source;
    if(n == 0) return -1;
    if(target > source[n-1]) return - n - 1;// without this we have a buffer overrun
    while(n>1) {
        int32_t half = n >> 1;
        base = (base[half] < target) ? &base[half] : base;
        n -= half;
    }
    base += *base < target;
    return *base == target ? base - source : source - base -1;
}



#define RDTSC_START(cycles)                                                   \
    do {                                                                      \
        register unsigned cyc_high, cyc_low;                                  \
        __asm volatile(                                                       \
            "cpuid\n\t"                                                       \
            "rdtsc\n\t"                                                       \
            "mov %%edx, %0\n\t"                                               \
            "mov %%eax, %1\n\t"                                               \
            : "=r"(cyc_high), "=r"(cyc_low)::"%rax", "%rbx", "%rcx", "%rdx"); \
        (cycles) = ((uint64_t)cyc_high << 32) | cyc_low;                      \
    } while (0)

#define RDTSC_FINAL(cycles)                                                   \
    do {                                                                      \
        register unsigned cyc_high, cyc_low;                                  \
        __asm volatile(                                                       \
            "rdtscp\n\t"                                                      \
            "mov %%edx, %0\n\t"                                               \
            "mov %%eax, %1\n\t"                                               \
            "cpuid\n\t"                                                       \
            : "=r"(cyc_high), "=r"(cyc_low)::"%rax", "%rbx", "%rcx", "%rdx"); \
        (cycles) = ((uint64_t)cyc_high << 32) | cyc_low;                      \
    } while (0)



/*
 * This is like BEST_TIME except that ... it runs functions "test" using the
 * first parameter "base" and various parameters from "testvalues" (there
 * are nbrtestvalues), calling pre on base between tests
 */
#define BEST_TIME_PRE_ARRAY(base, length, test, pre,   testvalues, nbrtestvalues, bogus)        \
    do {                                                                                \
        fflush(NULL);                                                                   \
        uint64_t cycles_start, cycles_final, cycles_diff;                               \
        uint64_t min_diff = (uint64_t)-1;                                               \
        int sum = 0;                                                                    \
        printf("[%s %s] ", #test, #pre);\
        for (size_t j = 0; j < nbrtestvalues; j++) {                                    \
            pre(base,length);                                                           \
            pre(base,length);                                                           \
            __asm volatile("" ::: /* pretend to clobber */ "memory");                   \
            RDTSC_START(cycles_start);                                                  \
            bogus += test(base,length,testvalues[j]);                                            \
            RDTSC_FINAL(cycles_final);                                                  \
            cycles_diff = (cycles_final - cycles_start);                                \
            if (cycles_diff < min_diff) min_diff = cycles_diff;                         \
            sum += cycles_diff;                                                           \
        }                                                                               \
        uint64_t S = nbrtestvalues;                                                     \
        double cpo = sum / (double)S;                                                 \
        printf("%f \n",cpo);\
    } while (0)

#define ASSERT_PRE_ARRAY(base, length, test,   testvalues, nbrtestvalues)               \
    do {                                                                                \
        int error = 0;                                                                  \
        for (size_t j = 0; j < nbrtestvalues; j++) {                                    \
            int32_t re = test(base,length,testvalues[j]);                               \
            if(re >= 0) {                                                               \
              if(base[re] != testvalues[j]) {                                           \
                error=1; break;                                                         \
              }                                                                         \
            } else {                                                                    \
               int32_t ip = -re - 1;                                                    \
               if(ip < (int32_t) length) {                                              \
                 if(base[ip] <= testvalues[j]) {                                        \
                   printf(" Index returned is %d which points at %d for target %d. ",ip,base[ip],testvalues[j]);\
                   error = 3;                                                           \
                   break;                                                               \
                 }                                                                      \
               }                                                                        \
               if( ip > 0 ){                                                            \
                 if(base[ip - 1] >= testvalues[j]) {                                    \
                   printf(" Index returned is %d which points at %d for target %d. ",ip,base[ip],testvalues[j]);\
                  printf(" Previous index is %d which points at %d for target %d. ",ip-1,base[ip-1],testvalues[j]);\
                   error = 4;                                                           \
                   break;                                                               \
                 }                                                                      \
               }                                                                        \
            }                                                                           \
        }                                                                               \
        if(error != 0) printf("[%s error: %d]\n",#test,error);                     \
     } while (0)






void demo() {
    size_t nbrtestvalues = 100;
    value_t * testvalues = create_random_array(nbrtestvalues);
    int32_t bogus = 0;
    printf("# Objective: fast search in 16-bit arrays of up to 4096 integers.\n");
    printf("# output is formatted for processing with gnuplot.\n");
    for(double Nd = 1; Nd <= 64; Nd++) {
        size_t N = round(Nd);
        printf("%zu \n",N);
        value_t * source = create_sorted_array(N);
        BEST_TIME_PRE_ARRAY(source, N, simple_linear_search,               array_cache_flush,   testvalues, nbrtestvalues,  bogus);
        BEST_TIME_PRE_ARRAY(source, N, justequal_linear_search,               array_cache_flush,   testvalues, nbrtestvalues,  bogus);
        BEST_TIME_PRE_ARRAY(source, N, branchless_binary_search,               array_cache_flush,   testvalues, nbrtestvalues,  bogus);
        BEST_TIME_PRE_ARRAY(source, N, binary_search,               array_cache_flush,   testvalues, nbrtestvalues,  bogus);
        free(source);
        printf("\n");
    }
    printf("bogus = %d \n",bogus);
    free(testvalues);
}
int main() {
    demo();
    return 0;
}
