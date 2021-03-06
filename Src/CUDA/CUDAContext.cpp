#include "CUDAContext.h"

#include <malloc.h>

#include <GL/glew.h>
#include <cudaGL.h>

#include "Util/Util.h"

static CUdevice  device;
static CUcontext context;

static int device_get_attribute(CUdevice_attribute attribute, CUdevice cuda_device = device) {
	int result;
	CUDACALL(cuDeviceGetAttribute(&result, attribute, cuda_device));

	return result;
}

void CUDAContext::init() {
	CUDACALL(cuInit(0));

	int device_count;
	CUDACALL(cuDeviceGetCount(&device_count));

	if (device_count == 0) {
		puts("ERROR: No suitable Device found!");

		abort();
	}

	CUdevice * devices = MALLOCA(CUdevice, device_count);
		
	unsigned gl_device_count;
	CUDACALL(cuGLGetDevices(&gl_device_count, devices, device_count, CU_GL_DEVICE_LIST_ALL));

	if (gl_device_count == 0) {
		puts("ERROR: No suitable GL Device found!");

		abort();
	}

	CUdevice best_device;
	int      best_compute_capability = 0;

	for (int i = 0; i < gl_device_count; i++) {
		int major = device_get_attribute(CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, devices[i]);
		int minor = device_get_attribute(CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, devices[i]);

		int	device_compute_capability = major * 10 + minor;
		if (device_compute_capability > best_compute_capability) {
			best_device             = devices[i];
			best_compute_capability = device_compute_capability;
		}
	}

	FREEA(devices);

	compute_capability = best_compute_capability;

	device = best_device;

	CUDACALL(cuCtxCreate(&context, 0, device));
		
	CUfunc_cache   config_cache;
	CUsharedconfig config_shared;
	CUDACALL(cuCtxGetCacheConfig    (&config_cache));
	CUDACALL(cuCtxGetSharedMemConfig(&config_shared));

	unsigned long long bytes_free;
	CUDACALL(cuMemGetInfo(&bytes_free, &total_memory));

	puts("CUDA Info:");
	printf("Memory available: %llu MB\n", total_memory >> 20);
	printf("Compute Capability: %i\n", compute_capability);

	switch (config_cache) {
		case CU_FUNC_CACHE_PREFER_NONE:   puts("Cache Config: Prefer None");   break;
		case CU_FUNC_CACHE_PREFER_SHARED: puts("Cache Config: Prefer Shared"); break;
		case CU_FUNC_CACHE_PREFER_L1:     puts("Cache Config: Prefer L1");     break;
		case CU_FUNC_CACHE_PREFER_EQUAL:  puts("Cache Config: Prefer Equal");  break;
	}

	switch (config_shared) {
		case CU_SHARED_MEM_CONFIG_DEFAULT_BANK_SIZE:    puts("Shared Memory Config: Default"); break;
		case CU_SHARED_MEM_CONFIG_FOUR_BYTE_BANK_SIZE:  puts("Shared Memory Config: 4 Bytes"); break;
		case CU_SHARED_MEM_CONFIG_EIGHT_BYTE_BANK_SIZE: puts("Shared Memory Config: 8 Bytes"); break;
	}

	puts("");
}

void CUDAContext::destroy() {
	CUDACALL(cuCtxDestroy(context));
	CUDACALL(cuDevicePrimaryCtxReset(device));
}

unsigned long long CUDAContext::get_available_memory() {
	unsigned long long bytes_available;
	unsigned long long bytes_total;
	CUDACALL(cuMemGetInfo(&bytes_available, &bytes_total));

	return bytes_available;
}

unsigned CUDAContext::get_shared_memory() { return device_get_attribute(CU_DEVICE_ATTRIBUTE_SHARED_MEMORY_PER_BLOCK); }
unsigned CUDAContext::get_sm_count()      { return device_get_attribute(CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT); }
