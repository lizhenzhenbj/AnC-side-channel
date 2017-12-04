#include<errno.h>
#include<fcntl.h>
#include<stdint.h>
#include<stdio.h>
#include<stdlib.h>
#include<sys/mman.h>
#include<sys/stat.h>
#include<sys/types.h>
#include<unistd.h>
#include<string.h>
#include<limits.h>

#define KB (1 << 10)
#define MB (1 << 20)
#define GB (1 << 30)
#define TB (1L << 40)
#define PAGE_SIZE_PTL1 (4 * KB)
#define PAGE_SIZE_PTL2 (2 * MB)
#define PAGE_SIZE_PTL3 (1L * GB)
#define PAGE_SIZE_PTL4 (512L * GB)
#define UNIFIED_TLB_SIZE (128 * MB)
#define I_TLB_SIZE (2 * MB)
#define NUM_CACHE_ENTRIES_PTL1 1088
#define NUM_CACHE_ENTRIES_PTL2 32
#define NUM_CACHE_ENTRIES_PTL3 4
#define NUM_CACHE_ENTRIES_PTL4 4
#define CACHE_LINE_SIZE 64
#define NUMBER_OF_CACHE_OFFSETS 64
#define FLUSH_ALL_PTL 0
#define FLUSH_PTL4 4
#define FLUSH_PTL3 3
#define FLUSH_PTL2 2
#define FLUSH_PTL1 1

typedef void (*fp)(void);

int evict_instr(volatile unsigned char *buffer, uint64_t i, uint64_t maxi, uint64_t step)
{
	fp ptr;
	
	for(; i < maxi; i += step)
	{
		ptr = (fp)(&(buffer[i]));
		
		if(ptr == NULL)
		{
			printf("Failed to execute instruction stored in buffer at index %llu\n", i);
			return -1;
		}
		
		ptr();
	}
	
	return 0;
}

void evict_data(volatile unsigned char *buffer, uint64_t i, uint64_t maxi, uint64_t step)
{
	for(; i < maxi; i += step)
	{
		buffer[i] = 0xc3;
	}
}

int evict_cacheline(volatile unsigned char *buffer, unsigned short int cache_line_offset, unsigned short int flush_lvl)
{
	if(flush_lvl > 4)
	{
		printf("flush_lvl is more than 4 (level number). Use 0 to flush all levels of translation caches.\n");
		return -1;
	}
	
	if(buffer == NULL)
	{
		return -1;
	}
	
	if(cache_line_offset >= NUMBER_OF_CACHE_OFFSETS)
	{
		printf("Cache line offset must be between 0 and 63 (incl.)\n");
		return -1;
	}
	
	cache_line_offset *= CACHE_LINE_SIZE; // convert offset to number of  bytes
	
	if(flush_lvl == FLUSH_ALL_PTL || flush_lvl == FLUSH_PTL1)
	{
		// flush L3 cache, unified TLB and translation cache for PTL1
		evict_data(buffer, cache_line_offset, UNIFIED_TLB_SIZE, PAGE_SIZE_PTL1);
	}
	
	if(flush_lvl == FLUSH_ALL_PTL || flush_lvl == FLUSH_PTL2)
	{
		// flush translation cache for PTL2
		evict_data(buffer, cache_line_offset, NUM_CACHE_ENTRIES_PTL2 * PAGE_SIZE_PTL2, PAGE_SIZE_PTL2);
	}
	
	if(flush_lvl == FLUSH_ALL_PTL || flush_lvl == FLUSH_PTL3)
	{
		// flush translation cache for PTL3
		evict_data(buffer, cache_line_offset, NUM_CACHE_ENTRIES_PTL3 * PAGE_SIZE_PTL3, PAGE_SIZE_PTL3);
	}
	
	if(flush_lvl == FLUSH_ALL_PTL || flush_lvl == FLUSH_PTL4)
	{
		// flush translation cache for PTL4
		evict_data(buffer, cache_line_offset, NUM_CACHE_ENTRIES_PTL4 * PAGE_SIZE_PTL4, PAGE_SIZE_PTL4);
	}
	
	// flush iTLB
	if(evict_instr(buffer, cache_line_offset, I_TLB_SIZE, PAGE_SIZE_PTL1) < 0)
	{
		printf("Failed to evict iTLB\n");
		return -1;
	}
	
	return 0;
}

void profile_mem_access(volatile unsigned char* c, volatile unsigned char* ev_set, uint64_t offset, unsigned short int flush_lvl, char* filename)
{
	int i, j, k;
	int NUM_MEASUREMENTS = 5; // make 5 measurements and take mean
	unsigned long long hi1, lo1;
	unsigned long long hi, lo;
	uint64_t old, new, base;
	uint64_t t[NUM_MEASUREMENTS];
	uint64_t ret; // mean value for measurements
	fp ptr; // pointer to function stored in the target buffer
	FILE *f = fopen(filename, "ab+");

	if(f == NULL)
	{
		perror("Failed to open file for printing memory access profiles");
		return;
	}

	//we chose the target instruction at offset 0 within a page
	c[offset] = 0xc3;
	ptr = (fp)&(c[offset]);

	for(i = -1; i < NUMBER_OF_CACHE_OFFSETS; i++){
		if(i >= 0){
			//checking target addresss at a different offset than the i-th
			c[(((i + 1) % NUMBER_OF_CACHE_OFFSETS) * CACHE_LINE_SIZE) + offset] = 0xc3;
			ptr = (fp)&(c[(((i + 1) % NUMBER_OF_CACHE_OFFSETS) * CACHE_LINE_SIZE) + offset]);
		}
		for(j = 0; j < NUM_MEASUREMENTS; j++){
		
			//evict the i-th cacheline for each page in the eviction set
			if(i >= 0 && evict_cacheline(ev_set, i, flush_lvl) < 0)
			{
				printf("Failed to evict TLB\n");
				fclose(f);
				return;
			} else if (i < 0) {
				ptr();
			}

			asm volatile ("mfence\n\t"
								"CPUID\n\t"
								"RDTSC\n\t"
								"mov %%rdx, %0\n\t"
								"mov %%rax, %1\n\t" : "=r"(hi1), "=r"(lo1) : : "%rax", "%rbx", "%rcx", "%rdx");
			ptr();

			asm volatile ("RDTSCP\n\t"
								"mov %%rdx, %0\n\t"
								"mov %%rax, %1\n\t"
								"CPUID\n\t"
								"mfence" : "=r"(hi), "=r"(lo) : : "%rax", "%rbx", "%rcx", "%rdx");

			old = (uint64_t) (hi1 << 32) | lo1;
			new = (uint64_t) (hi << 32) | lo;
			t[j] = new - old;
		}

		ret = 0;
		
		for(j = 0; j < NUM_MEASUREMENTS; j++){
			ret += t[j];		
		}
		
		ret /= NUM_MEASUREMENTS;
		
		if(i >= 0 && fprintf(f, "%d\n", abs(ret - base)) < 0)
		{
			perror("Failed to print memory access");
			fclose(f);
			return;
		} else if (i < 0) {
			base = ret;
		}
	}

	fclose(f);
}


void scan_target(volatile unsigned char* c, volatile unsigned char* ev_set)
{
	int i;
	
	remove("scan.txt");
	remove("scan_1.txt");
	remove("scan_2.txt");
	remove("scan_3.txt");
	remove("scan_4.txt");
	
	//move 1 page at a time, for now 24 pages should be enough
	for(i = 0; i < 24; i++)
	{
		// cross 1 cacheline at a time on PTL4, 2 cachelines at PTL3, and 3 cachelines at PTL2 and 4 cachelines at PTL1
		profile_mem_access(c, ev_set, 8 * i * (PAGE_SIZE_PTL4 + 2 * PAGE_SIZE_PTL3 + 3 * PAGE_SIZE_PTL2 + 4 * PAGE_SIZE_PTL1), FLUSH_ALL_PTL, "scan.txt");
	}
	
	for(i = 0; i < 8; i++)
	{
		profile_mem_access(c, ev_set, PAGE_SIZE_PTL1, FLUSH_PTL1, "scan_1.txt");
	}
	
	for(i = 0; i < 8; i++)
	{
		profile_mem_access(c, ev_set, PAGE_SIZE_PTL2, FLUSH_PTL2, "scan_2.txt");
	}
	
	for(i = 0; i < 8; i++)
	{
		profile_mem_access(c, ev_set, PAGE_SIZE_PTL3, FLUSH_PTL3, "scan_3.txt");
	}
	
	for(i = 0; i < 8; i++)
	{
		profile_mem_access(c, ev_set, PAGE_SIZE_PTL4, FLUSH_PTL4, "scan_4.txt");
	}
}


int main(int argc, char* argv[])
{
	size_t ev_set_size = 5L * TB;
	uint64_t target_size = 96 * TB;
	volatile unsigned char *ev_set;
	uint64_t target_addr = 4L * TB;
	volatile unsigned char *target = (unsigned char*)mmap((void*)target_addr, target_size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if(target == MAP_FAILED)
	{
		perror("Failed to map memory.");
		return -1;
	}

	ev_set = (unsigned char*)mmap(NULL, ev_set_size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if(ev_set == NULL)
	{
		perror("Failed to initialize TLB eviction set");
		munmap((void*) target, ev_set_size);
		return -1;
	}

	scan_target(target, ev_set);

	// munmap((void*) target, target_size);
	// munmap((void*) ev_set, ev_set_size);
	// free(cache_flush_set);
	// free(pages);

	return 0;
}
