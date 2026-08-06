#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef int nvmlReturn_t;
typedef struct nvmlDevice_st *nvmlDevice_t;

typedef struct {
    char busIdLegacy[16];
    unsigned int domain;
    unsigned int bus;
    unsigned int device;
    unsigned int pciDeviceId;
    unsigned int pciSubSystemId;
    char busId[32];
} nvmlPciInfo_t;

typedef struct {
    unsigned int gpu;
    unsigned int memory;
} nvmlUtilization_t;

typedef struct {
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
} nvmlMemory_t;

typedef struct {
    unsigned int pid;
    unsigned long long usedGpuMemory;
    unsigned int gpuInstanceId;
    unsigned int computeInstanceId;
} nvmlProcessInfo_t;

extern nvmlReturn_t nvmlInit_v2(void);
extern nvmlReturn_t nvmlShutdown(void);
extern const char *nvmlErrorString(nvmlReturn_t);
extern nvmlReturn_t nvmlDeviceGetCount_v2(unsigned int *);
extern nvmlReturn_t nvmlDeviceGetHandleByIndex_v2(unsigned int, nvmlDevice_t *);
extern nvmlReturn_t nvmlDeviceGetUUID(nvmlDevice_t, char *, unsigned int);
extern nvmlReturn_t nvmlDeviceGetName(nvmlDevice_t, char *, unsigned int);
extern nvmlReturn_t nvmlDeviceGetPciInfo_v3(nvmlDevice_t, nvmlPciInfo_t *);
extern nvmlReturn_t nvmlDeviceGetMemoryInfo(nvmlDevice_t, nvmlMemory_t *);
extern nvmlReturn_t nvmlDeviceGetUtilizationRates(nvmlDevice_t, nvmlUtilization_t *);
extern nvmlReturn_t nvmlDeviceGetComputeRunningProcesses_v3(nvmlDevice_t, unsigned int *, nvmlProcessInfo_t *);
extern nvmlReturn_t nvmlDeviceGetGraphicsRunningProcesses_v3(nvmlDevice_t, unsigned int *, nvmlProcessInfo_t *);
extern nvmlReturn_t nvmlDeviceSetMemoryLockedClocks(nvmlDevice_t, unsigned int, unsigned int);
extern nvmlReturn_t nvmlDeviceResetMemoryLockedClocks(nvmlDevice_t);
extern nvmlReturn_t nvmlDeviceGetClockInfo(nvmlDevice_t, unsigned int, unsigned int *);

#define NVML_SUCCESS 0
#define NVML_ERROR_INSUFFICIENT_SIZE 7
#define NVML_CLOCK_MEM 2

#define TARGET_NAME "NVIDIA CMP 90HX"
#define TARGET_DEVICE_ID 0x220D10DEu
#define TARGET_MEMORY_BYTES (10240ULL * 1024ULL * 1024ULL)
#define MAX_TARGETS 32u
#define IDLE_CLOCK_MHZ 405u
#define IDLE_GPU_UTIL_MAX 1u
#define IDLE_MEMORY_UTIL_MAX 1u
#define IDLE_SECONDS 60u
#define SAMPLE_SECONDS 1u

static volatile sig_atomic_t keep_running = 1;
static bool debug_logging;

typedef struct {
    unsigned int gpu_util;
    unsigned int memory_util;
    unsigned long long memory_used;
    uint64_t process_hash;
    unsigned int process_count;
} sample_t;

typedef struct {
    nvmlDevice_t device;
    char uuid[96];
    char bus_id[32];
    unsigned int device_id;
    unsigned int subsystem_id;
    bool available;
    bool locked;
    unsigned int idle_samples;
    unsigned int external_clock_mismatches;
    unsigned int busy_samples;
    bool have_previous;
    sample_t previous;
} target_t;

static void handle_signal(int signal_number) {
    (void)signal_number;
    keep_running = 0;
}

static void log_nvml_error(const char *operation, nvmlReturn_t result) {
    fprintf(stderr, "%s failed: %s (%d)\n", operation, nvmlErrorString(result), result);
}

static bool read_identity(nvmlDevice_t device, char *uuid, size_t uuid_size,
                          char *bus_id, size_t bus_id_size,
                          unsigned int *device_id, unsigned int *subsystem_id,
                          unsigned long long *memory_total) {
    char name[96] = {0};
    nvmlPciInfo_t pci = {0};
    nvmlMemory_t memory = {0};

    if (nvmlDeviceGetUUID(device, uuid, (unsigned int)uuid_size) != NVML_SUCCESS ||
        nvmlDeviceGetName(device, name, sizeof(name)) != NVML_SUCCESS ||
        nvmlDeviceGetPciInfo_v3(device, &pci) != NVML_SUCCESS ||
        nvmlDeviceGetMemoryInfo(device, &memory) != NVML_SUCCESS) {
        return false;
    }
    if (strcmp(name, TARGET_NAME) != 0 || pci.pciDeviceId != TARGET_DEVICE_ID ||
        memory.total != TARGET_MEMORY_BYTES) {
        return false;
    }

    snprintf(bus_id, bus_id_size, "%s", pci.busId);
    *device_id = pci.pciDeviceId;
    *subsystem_id = pci.pciSubSystemId;
    *memory_total = memory.total;
    return true;
}

static unsigned int discover_targets(target_t *targets, unsigned int capacity) {
    unsigned int count = 0;
    unsigned int found = 0;
    if (nvmlDeviceGetCount_v2(&count) != NVML_SUCCESS) return 0;

    for (unsigned int index = 0; index < count; ++index) {
        nvmlDevice_t device = NULL;
        char uuid[96] = {0};
        char bus_id[32] = {0};
        unsigned int device_id = 0;
        unsigned int subsystem_id = 0;
        unsigned long long memory_total = 0;

        if (nvmlDeviceGetHandleByIndex_v2(index, &device) != NVML_SUCCESS ||
            !read_identity(device, uuid, sizeof(uuid), bus_id, sizeof(bus_id),
                           &device_id, &subsystem_id, &memory_total)) {
            continue;
        }
        if (found >= capacity) {
            fprintf(stderr, "more than %u CMP 90HX devices found; ignoring extras\n", capacity);
            break;
        }
        targets[found].device = device;
        snprintf(targets[found].uuid, sizeof(targets[found].uuid), "%s", uuid);
        snprintf(targets[found].bus_id, sizeof(targets[found].bus_id), "%s", bus_id);
        targets[found].device_id = device_id;
        targets[found].subsystem_id = subsystem_id;
        targets[found].available = true;
        found++;
    }
    return found;
}

static bool verify_identity(const target_t *target) {
    char uuid[96] = {0};
    char bus_id[32] = {0};
    unsigned int device_id = 0;
    unsigned int subsystem_id = 0;
    unsigned long long memory_total = 0;

    if (!read_identity(target->device, uuid, sizeof(uuid), bus_id, sizeof(bus_id),
                       &device_id, &subsystem_id, &memory_total)) {
        return false;
    }
    return strcmp(uuid, target->uuid) == 0 &&
           strcmp(bus_id, target->bus_id) == 0 &&
           device_id == target->device_id &&
           subsystem_id == target->subsystem_id &&
           memory_total == TARGET_MEMORY_BYTES;
}

static uint64_t mix_pid(uint64_t hash, unsigned int pid, unsigned int kind) {
    uint64_t value = ((uint64_t)pid << 1) | kind;
    hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
    return hash;
}

static bool add_processes(nvmlDevice_t device, bool graphics, sample_t *sample) {
    nvmlReturn_t (*query)(nvmlDevice_t, unsigned int *, nvmlProcessInfo_t *) =
        graphics ? nvmlDeviceGetGraphicsRunningProcesses_v3 : nvmlDeviceGetComputeRunningProcesses_v3;
    unsigned int count = 0;
    nvmlReturn_t result = query(device, &count, NULL);

    if (result == NVML_SUCCESS) return true;
    if (result != NVML_ERROR_INSUFFICIENT_SIZE) return false;

    count += 8;
    nvmlProcessInfo_t *items = calloc(count, sizeof(*items));
    if (items == NULL) return false;
    result = query(device, &count, items);
    if (result != NVML_SUCCESS) {
        free(items);
        return false;
    }

    for (unsigned int i = 0; i < count; ++i) {
        sample->process_hash = mix_pid(sample->process_hash, items[i].pid, graphics ? 1u : 0u);
        sample->process_count++;
    }
    free(items);
    return true;
}

static bool get_sample(const target_t *target, sample_t *sample) {
    nvmlUtilization_t utilization = {0};
    nvmlMemory_t memory = {0};
    memset(sample, 0, sizeof(*sample));

    if (nvmlDeviceGetUtilizationRates(target->device, &utilization) != NVML_SUCCESS ||
        nvmlDeviceGetMemoryInfo(target->device, &memory) != NVML_SUCCESS) {
        return false;
    }
    sample->gpu_util = utilization.gpu;
    sample->memory_util = utilization.memory;
    sample->memory_used = memory.used;
    sample->process_hash = 1469598103934665603ULL;
    return add_processes(target->device, false, sample) &&
           add_processes(target->device, true, sample);
}

static bool same_idle_state(const sample_t *left, const sample_t *right) {
    const unsigned long long tolerance = 512ULL * 1024ULL * 1024ULL;
    unsigned long long delta = left->memory_used > right->memory_used
        ? left->memory_used - right->memory_used
        : right->memory_used - left->memory_used;

    return left->gpu_util <= IDLE_GPU_UTIL_MAX &&
           left->memory_util <= IDLE_MEMORY_UTIL_MAX &&
           right->gpu_util <= IDLE_GPU_UTIL_MAX &&
           right->memory_util <= IDLE_MEMORY_UTIL_MAX &&
           left->process_count == right->process_count && delta <= tolerance;
}

static bool reset_clocks(target_t *target, bool report) {
    nvmlReturn_t result = nvmlDeviceResetMemoryLockedClocks(target->device);
    if (result != NVML_SUCCESS && report) log_nvml_error("reset memory clocks", result);
    return result == NVML_SUCCESS;
}

static bool lock_idle_clock(target_t *target) {
    nvmlReturn_t result = nvmlDeviceSetMemoryLockedClocks(target->device, IDLE_CLOCK_MHZ, IDLE_CLOCK_MHZ);
    if (result != NVML_SUCCESS) {
        log_nvml_error("lock memory clocks", result);
        return false;
    }
    printf("%s: idle memory clock lock requested at %u MHz\n", target->uuid, IDLE_CLOCK_MHZ);
    fflush(stdout);
    return true;
}

static bool idle_clock_is_applied(target_t *target) {
    unsigned int clock = 0;
    nvmlReturn_t result = nvmlDeviceGetClockInfo(target->device, NVML_CLOCK_MEM, &clock);
    if (result != NVML_SUCCESS) {
        log_nvml_error("read memory clock", result);
        return false;
    }
    return clock == IDLE_CLOCK_MHZ;
}

static void sleep_one_sample(void) {
    struct timespec remaining = {.tv_sec = SAMPLE_SECONDS, .tv_nsec = 0};
    while (keep_running && nanosleep(&remaining, &remaining) != 0 && errno == EINTR) {}
}

int main(int argc, char **argv) {
    bool check_only = argc == 2 && strcmp(argv[1], "--check") == 0;
    if (argc > 2 || (argc == 2 && !check_only)) {
        fprintf(stderr, "usage: %s [--check]\n", argv[0]);
        return 2;
    }
    debug_logging = getenv("CMP90HX_DEBUG") != NULL;

    nvmlReturn_t result = nvmlInit_v2();
    if (result != NVML_SUCCESS) {
        log_nvml_error("NVML initialization", result);
        return 1;
    }

    target_t targets[MAX_TARGETS] = {0};
    unsigned int target_count = discover_targets(targets, MAX_TARGETS);
    if (target_count == 0) {
        printf("no compatible CMP 90HX found; exiting without changes\n");
        nvmlShutdown();
        return check_only ? 1 : 0;
    }
    for (unsigned int i = 0; i < target_count; ++i) {
        printf("found CMP 90HX[%u]: %s at %s\n", i, targets[i].uuid, targets[i].bus_id);
    }
    fflush(stdout);
    if (check_only) {
        nvmlShutdown();
        return 0;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGHUP, handle_signal);
    for (unsigned int i = 0; i < target_count; ++i) reset_clocks(&targets[i], true);

    while (keep_running) {
        for (unsigned int i = 0; i < target_count; ++i) {
            target_t *target = &targets[i];
            if (!target->available) continue;
            if (!verify_identity(target)) {
                fprintf(stderr, "%s: identity check failed; disabling this target\n", target->uuid);
                if (target->locked) reset_clocks(target, true);
                target->locked = false;
                target->available = false;
                continue;
            }

            sample_t current;
            if (!get_sample(target, &current)) {
                fprintf(stderr, "%s: GPU telemetry failed; restoring clocks\n", target->uuid);
                if (target->locked) reset_clocks(target, true);
                target->locked = false;
                target->idle_samples = 0;
                target->busy_samples = 0;
                target->have_previous = false;
                continue;
            }

            bool idle = target->have_previous && same_idle_state(&target->previous, &current);
            if (debug_logging && target->idle_samples % 10u == 0u) {
                fprintf(stderr, "%s: debug util=%u/%u used=%llu processes=%u idle=%s locked=%s\n",
                        target->uuid, current.gpu_util, current.memory_util,
                        current.memory_used, current.process_count,
                        idle ? "yes" : "no", target->locked ? "yes" : "no");
            }

            if (target->locked && !idle) {
                if (++target->busy_samples >= 3u) {
                    if (reset_clocks(target, true)) {
                        printf("%s: activity detected; memory clock lock removed\n", target->uuid);
                        fflush(stdout);
                    }
                    target->locked = false;
                    target->idle_samples = 0;
                    target->busy_samples = 0;
                }
            } else if (target->locked && !idle_clock_is_applied(target)) {
                target->busy_samples = 0;
                if (++target->external_clock_mismatches >= 3u) {
                    fprintf(stderr, "%s: idle memory clock changed externally; reapplying lock\n", target->uuid);
                    if (!lock_idle_clock(target)) {
                        reset_clocks(target, true);
                        target->locked = false;
                    }
                    target->external_clock_mismatches = 0;
                }
            } else if (target->locked) {
                target->busy_samples = 0;
                target->external_clock_mismatches = 0;
            } else if (idle) {
                target->busy_samples = 0;
                target->idle_samples++;
                if (target->idle_samples >= IDLE_SECONDS / SAMPLE_SECONDS) {
                    sample_t confirmation = current;
                    if (verify_identity(target) && get_sample(target, &confirmation) &&
                        confirmation.gpu_util <= IDLE_GPU_UTIL_MAX &&
                        confirmation.memory_util <= IDLE_MEMORY_UTIL_MAX) {
                        target->locked = lock_idle_clock(target);
                    }
                    target->idle_samples = 0;
                    current = confirmation;
                }
            } else {
                target->busy_samples = 0;
                target->idle_samples = 0;
            }

            target->previous = current;
            target->have_previous = true;
        }
        sleep_one_sample();
    }

    for (unsigned int i = 0; i < target_count; ++i) {
        if (targets[i].locked) reset_clocks(&targets[i], true);
    }
    nvmlShutdown();
    printf("stopped; memory clock locks removed\n");
    return 0;
}
