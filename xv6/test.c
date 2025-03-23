#include "types.h"
#include "stat.h"
#include "user.h"
#define NUM_PROCS 3 // Number of processes to create
int main() {
for (int i = 0; i < NUM_PROCS; i++) {
int pid = custom_fork(1, 50); // Start later, execution time 50
if (pid < 0) {
printf(1, "Failed to fork process %d\n", i);
exit();
} else if (pid == 0) {
// Child process
printf(1, "Child %d (PID: %d) started but should not run yet.\n", i, getpid());
for (volatile int j = 0; j < 100000000; j++); // Simulated work
exit();
}
}
printf(1, "All child processes created with start_later flag set.\n");
sleep(400);
printf(1, "Calling sys_scheduler_start() to allow execution.\n");
scheduler_start();
for (int i = 0; i < NUM_PROCS; i++) {
wait();
}
printf(1, "All child processes completed.\n");
exit();
}