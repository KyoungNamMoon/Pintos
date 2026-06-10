/* Definitions for CPU-related data structures.
 * See cpu.h for their purpose.
 */
#include "cpu.h"

int cpu_can_acquire_spinlock;
struct cpu *bcpu;
struct cpu cpus[NCPU_MAX];
unsigned int ncpu;
int cpu_started_others;

