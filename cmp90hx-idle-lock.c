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
extern nvmlReturn_t nvmlDeviceGetHandleByUUID(const char *, nvmlDevice_t *);
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
#define IDLE_CLOCK_MHZ 405u
#define IDLE_SECONDS 60u
#define SAMPLE_SECONDS 1u

static volatile sig_atomic_t keep_running = 1;
static const char *target_uuid;
static const char *target_bus_id;
static unsigned int target_subsystem_id;

typedef struct {
    unsigned int gpu_util;
    unsigned int memory_util;
    unsigned long long memory_used;
    uint64_t process_hash;
    unsigned int process_count;
} sample_t;

static void handle_signal(int signal_number) {
    (void)signal_number;
    keep_running = 0;
}

static void log_nvml_error(const char *operation, nvmlReturn_t result) {
    fprintf(stderr, "%s failed: %s (%d)\n", operation, nvmlErrorString(result), result);
}

static bool load_target(void) {
    const char *subsystem = getenv("CMP90HX_SUBSYSTEM_ID");
    char *end = NULL;

    target_uuid = getenv("CMP90HX_UUID");
    target_bus_id = getenv("CMP90HX_PCI_BUS_ID");
    if (target_uuid == NULL || target_uuid[0] == '\0' ||
        target_bus_id == NULL || target_bus_id[0] == '\0' ||
        subsystem == NULL || subsystem[0] == '\0') {
        fprintf(stderr, "CMP90HX_UUID, CMP90HX_PCI_BUS_ID and CMP90HX_SUBSYSTEM_ID are required\n");
        return false;
    }

    errno = 0;
    unsigned long value = strtoul(subsystem, &end, 0);
    if (errno != 0 || end == subsystem || *end != '\0' || value > UINT32_MAX) {
        fprintf(stderr, "invalid CMP90HX_SUBSYSTEM_ID\n");
        return false;
    }
    target_subsystem_id = (unsigned int)value;
    return true;
}

static bool verify_identity(nvmlDevice_t device) {
    char uuid[96] = {0};
    char name[96] = {0};
    nvmlPciInfo_t pci = {0};
    nvmlMemory_t memory = {0};
    nvmlReturn_t result;

    result = nvmlDeviceGetUUID(device, uuid, sizeof(uuid));
    if (result != NVML_SUCCESS) return false;
    result = nvmlDeviceGetName(device, name, sizeof(name));
    if (result != NVML_SUCCESS) return false;
    result = nvmlDeviceGetPciInfo_v3(device, &pci);
    if (result != NVML_SUCCESS) return false;
    result = nvmlDeviceGetMemoryInfo(device, &memory);
    if (result != NVML_SUCCESS) return false;

    return strcmp(uuid, target_uuid) == 0 &&
           strcmp(name, TARGET_NAME) == 0 &&
           strcmp(pci.busId, target_bus_id) == 0 &&
           pci.pciDeviceId == TARGET_DEVICE_ID &&
           pci.pciSubSystemId == target_subsystem_id &&
           memory.total == TARGET_MEMORY_BYTES;
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

static bool get_sample(nvmlDevice_t device, sample_t *sample) {
    nvmlUtilization_t utilization = {0};
    nvmlMemory_t memory = {0};
    memset(sample, 0, sizeof(*sample));

    if (nvmlDeviceGetUtilizationRates(device, &utilization) != NVML_SUCCESS) return false;
    if (nvmlDeviceGetMemoryInfo(device, &memory) != NVML_SUCCESS) return false;

    sample->gpu_util = utilization.gpu;
    sample->memory_util = utilization.memory;
    sample->memory_used = memory.used;
    sample->process_hash = 1469598103934665603ULL;

    return add_processes(device, false, sample) && add_processes(device, true, sample);
}

static bool same_idle_state(const sample_t *left, const sample_t *right) {
    const unsigned long long tolerance = 16ULL * 1024ULL * 1024ULL;
    unsigned long long delta = left->memory_used > right->memory_used
        ? left->memory_used - right->memory_used
        : right->memory_used - left->memory_used;

    return left->gpu_util == 0 && left->memory_util == 0 &&
           right->gpu_util == 0 && right->memory_util == 0 &&
           left->process_count == right->process_count &&
           left->process_hash == right->process_hash &&
           delta <= tolerance;
}

static bool reset_clocks(nvmlDevice_t device, bool report) {
    nvmlReturn_t result = nvmlDeviceResetMemoryLockedClocks(device);
    if (result != NVML_SUCCESS && report) log_nvml_error("reset memory clocks", result);
    return result == NVML_SUCCESS;
}

static bool lock_idle_clock(nvmlDevice_t device) {
    nvmlReturn_t result = nvmlDeviceSetMemoryLockedClocks(device, IDLE_CLOCK_MHZ, IDLE_CLOCK_MHZ);
    if (result != NVML_SUCCESS) {
        log_nvml_error("lock memory clocks", result);
        return false;
    }

    unsigned int clock = 0;
    result = nvmlDeviceGetClockInfo(device, NVML_CLOCK_MEM, &clock);
    if (result != NVML_SUCCESS) {
        log_nvml_error("read memory clock", result);
        reset_clocks(device, true);
        return false;
    }
    printf("idle lock applied; current memory clock=%u MHz\n", clock);
    fflush(stdout);
    return true;
}

static bool idle_clock_is_applied(nvmlDevice_t device) {
    unsigned int clock = 0;
    nvmlReturn_t result = nvmlDeviceGetClockInfo(device, NVML_CLOCK_MEM, &clock);
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
    if (!load_target()) return check_only ? 1 : 2;

    nvmlReturn_t result = nvmlInit_v2();
    if (result != NVML_SUCCESS) {
        log_nvml_error("NVML initialization", result);
        return 1;
    }

    nvmlDevice_t device = NULL;
    result = nvmlDeviceGetHandleByUUID(target_uuid, &device);
    if (result != NVML_SUCCESS) {
        printf("CMP 90HX target is absent; exiting without changes\n");
        nvmlShutdown();
        return check_only ? 1 : 0;
    }
    if (!verify_identity(device)) {
        fprintf(stderr, "target UUID exists but hardware identity does not match; refusing to run\n");
        nvmlShutdown();
        return 3;
    }

    printf("verified CMP 90HX: %s at %s\n", target_uuid, target_bus_id);
    fflush(stdout);
    if (check_only) {
        nvmlShutdown();
        return 0;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGHUP, handle_signal);

    if (!reset_clocks(device, true)) {
        nvmlShutdown();
        return 4;
    }

    bool locked = false;
    unsigned int idle_samples = 0;
    sample_t previous = {0};
    bool have_previous = false;

    while (keep_running) {
        if (!verify_identity(device)) {
            fprintf(stderr, "GPU identity check failed; restoring clocks and exiting\n");
            if (locked) reset_clocks(device, true);
            break;
        }

        sample_t current;
        if (!get_sample(device, &current)) {
            fprintf(stderr, "GPU telemetry failed; restoring clocks\n");
            if (locked) reset_clocks(device, true);
            locked = false;
            idle_samples = 0;
            have_previous = false;
            sleep_one_sample();
            continue;
        }

        bool idle = have_previous && same_idle_state(&previous, &current);
        if (locked && !idle) {
            if (reset_clocks(device, true)) {
                printf("activity detected; memory clock lock removed\n");
                fflush(stdout);
            }
            locked = false;
            idle_samples = 0;
        } else if (locked && !idle_clock_is_applied(device)) {
            fprintf(stderr, "idle memory clock was changed externally; reapplying lock\n");
            if (!lock_idle_clock(device)) {
                reset_clocks(device, true);
                locked = false;
                have_previous = false;
            }
        } else if (!locked && idle) {
            idle_samples++;
            if (idle_samples >= IDLE_SECONDS / SAMPLE_SECONDS) {
                sample_t confirmation = current;
                if (verify_identity(device) && get_sample(device, &confirmation) &&
                    same_idle_state(&current, &confirmation)) {
                    locked = lock_idle_clock(device);
                }
                idle_samples = 0;
                current = confirmation;
            }
        } else if (!idle) {
            idle_samples = 0;
        }

        previous = current;
        have_previous = true;
        sleep_one_sample();
    }

    if (locked) reset_clocks(device, true);
    nvmlShutdown();
    printf("stopped; memory clock lock removed\n");
    return 0;
}
