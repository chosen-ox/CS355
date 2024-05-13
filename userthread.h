#ifndef __USERTHREAD_H_
#define __USERTHREAD_H_

enum {FIFO, SJF, PRIORITY};
#define STACKSIZE (256*1024)

int thread_libinit(int policy);

int thread_libterminate(void);

int thread_create(void (*func)(void *), void *arg, int priority);

int thread_yield(void);

int thread_join(int tid);

#endif
