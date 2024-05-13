#include "userthread.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>
#include <sys/types.h>
#include <signal.h>
#include <unistd.h>
#include <sys/time.h>
#include <poll.h>
#include "/usr/include/valgrind/valgrind.h"

#define BUFFERSIZE 256
#define MAXTHREAD 300
#define HISTORY 3
#define QUANTA 100
#define HIGH -1
#define MIDDLE 0
#define LOW 1
#define HIGHSIZE 9
#define MIDDLESIZE 6
#define LOWSIZE 4
#define interval_msec 100000

int flag;
const char *log_file = "userthread_log.txt";
int log_fd;
ucontext_t main_context;

typedef enum {
    READY,
    SCHEDULED,
    STOPPED,
    FINISHED,
    WAITING
} OperationType;

struct Thread {
    ucontext_t* context;
    int tid;
    int priority;
    struct Thread *prev;
    struct Thread *next;
    int vid;
};

struct TCB {
    int tid;
    long est_time;
    long hst_time[HISTORY];
    int status;
    int waitingfor;
};

struct Thread *current_running = NULL;
struct TCB tcb[MAXTHREAD];

// SJF and FIFO use
struct Thread *thread_queue = NULL;
int queue_size = 0;

// SJF use
long cur_avg = 0;
int cur_run_num = 0;
struct timeval begin, end;

//PRIORITY use
struct Thread* head_high = NULL;
struct Thread* head_middle = NULL;
struct Thread* head_low = NULL;
int high_queue_size = 0;
int middle_queue_size = 0;
int low_queue_size = 0;
int table_size = 0;
int* table = NULL;
int mode = 0;

int tick_count = 0;

void print_queue() {
    struct Thread* tmp = thread_queue;
    while (tmp != NULL) {
        printf("%d ", tmp->tid);
        tmp = tmp->next;
    }
    printf("\n");
}
/**
 * Sets a real-time interval timer to fire a SIGALRM signal at regular intervals.
 *
 * interval_msec The interval in seconds
 */
void setrtimer() {
    struct itimerval it_val;
    it_val.it_value.tv_sec = 0;  // Start after 0 seconds
    it_val.it_value.tv_usec = interval_msec; // Start after 100000 microseconds = 100 milliseconds
    it_val.it_interval.tv_sec = 0; // Interval for periodic signal is 0 seconds
    it_val.it_interval.tv_usec = interval_msec; // Interval for periodic signal is 100 milliseconds
    if (setitimer(ITIMER_REAL, &it_val, NULL) == -1) {
        perror("error calling setitimer()");
        exit(1);
    }
}

// keep record of the status of each thread
void log_operation(OperationType op, int tid, int priority) {
    char buffer[BUFFERSIZE];
    sprintf(buffer, "[%d] ", tick_count*100); // Print the seconds part of the current time

    switch (op) {
        case READY:
            strcat(buffer, "READY");
            break;
        case SCHEDULED:
            strcat(buffer, "SCHEDULED");
            break;
        case STOPPED:
            strcat(buffer, "STOPPED");
            break;
        case FINISHED:
            strcat(buffer, "FINISHED");
            break;
        case WAITING:
            strcat(buffer, "WAITING");
            break;
    }
    sprintf(buffer + strlen(buffer), " %d %d\n", tid, priority);
    write(log_fd, buffer, strlen(buffer));
}

// a handler to stop the current running thread and switch to the next
void alarm_handler(int signal) {
    if (signal == SIGALRM && current_running != NULL) {
        //print_queue();
        tick_count ++;   
        log_operation(STOPPED, current_running->tid, current_running->priority);
        tcb[current_running->tid].status = STOPPED;   
       swapcontext(current_running->context, &main_context);
    }
}

// a handler to temporarily block sigalrm
void block_handler (int signal) {
    if (signal == SIGALRM) {
        tick_count ++;
        // do nothing
    }
}

// do cleanup before terminate the program
int thread_libterminate() {
    if (close(log_fd) == -1) {
        perror("Error closing log file");
        return -1;
    }
    // clean up the linked list
    while (thread_queue != NULL) {
        struct Thread* tmp = thread_queue;
        thread_queue = thread_queue->next;
        VALGRIND_STACK_DEREGISTER(tmp->vid);
        free(tmp->context->uc_stack.ss_sp);
        free(tmp->context);
        free(tmp);
    }
    if (flag == PRIORITY) {
        if (table != NULL) {
            free(table);
        }
        while (head_high != NULL) {
            struct Thread* tmp = head_high;
            head_high = head_high->next;
            VALGRIND_STACK_DEREGISTER(tmp->vid);
            free(tmp->context->uc_stack.ss_sp);
            free(tmp->context);
            free(tmp);
        }
        while (head_low != NULL) {
            struct Thread* tmp = head_low;
            head_low = head_low->next;
            VALGRIND_STACK_DEREGISTER(tmp->vid);
            free(tmp->context->uc_stack.ss_sp);
            free(tmp->context);
            free(tmp);
        }
        while (head_middle != NULL) {
            struct Thread* tmp = head_middle;
            head_middle = head_middle->next;
            VALGRIND_STACK_DEREGISTER(tmp->vid);
            free(tmp->context->uc_stack.ss_sp);
            free(tmp->context);
            free(tmp);
        }
    }

    return 0;
}

#define HIGHMODE 1    // Bit 0 set if high queue is not empty
#define MIDDLEMODE 2  // Bit 1 set if middle queue is not empty
#define LOWMODE 4     // Bit 2 set if low queue is not empty

// generate a table based on the current thread queue situation
int generate_table() {
    // Initialize mode to 0 to indicate the starting state
    int mode = 0;

    // Check each queue and set the corresponding mode bit if not empty
    if (head_high != NULL) mode |= HIGHMODE;
    if (head_middle != NULL) mode |= MIDDLEMODE;
    if (head_low != NULL) mode |= LOWMODE;

    // Calculate table size based on which queues are present
    table_size = 0;
    if (mode & HIGHMODE) table_size += HIGHSIZE;
    if (mode & MIDDLEMODE) table_size += MIDDLESIZE;
    if (mode & LOWMODE) table_size += LOWSIZE;

    // Allocate or reallocate memory for the table
    if (table != NULL) {
        free(table); // Free existing table if it exists
        table = NULL;
    }
    if (table_size > 0) {
        table = (int*)malloc(table_size * sizeof(int));
        if (table == NULL) {
            perror("Failed to allocate memory for scheduling table");
            return -1; // Return -1 to indicate an error
        }

        // Fill the table with priorities according to the modes present
        int index = 0;
        if (mode & HIGHMODE) {
            for (int i = 0; i < HIGHSIZE; i++) {
                table[index++] = HIGH;
            }
        }
        if (mode & MIDDLEMODE) {
            for (int i = 0; i < MIDDLESIZE; i++) {
                table[index++] = MIDDLE;
            }
        }
        if (mode & LOWMODE) {
            for (int i = 0; i < LOWSIZE; i++) {
                table[index++] = LOW;
            }
        }
    } else {
        // If all queues are empty
        mode = 0;
    }
    // Return the mode to indicate the state of the priority queues
    return mode;
}

// exit a thread and do the clean up
int thread_exit(struct Thread* current_runing) {
    log_operation(FINISHED, current_runing->tid, current_runing->priority);
    tcb[current_runing->tid].status = FINISHED;
    if (flag == PRIORITY) {
        //print_queue();
        if (current_runing == NULL) {
            return -1;
        }
        if (current_runing->priority == HIGH) {
            high_queue_size --;
        } else if (current_runing->priority == MIDDLE) {
            middle_queue_size --;
        } else low_queue_size --;
        struct Thread* tmp = current_runing;
        if (tmp->next != tmp && tmp->prev != tmp) {
            // at least two
            if (current_runing->priority == HIGH) {
                head_high = head_high->next;
            } else if (current_runing->priority == MIDDLE) {
                head_middle = head_middle->next;
            } else if (current_runing->priority == LOW) {
                head_low = head_low->next;
            }
            (tmp->prev)->next = tmp->next;
            (tmp->next)->prev = tmp->prev;
        } else {
            // the only in the queue
            if (current_runing->priority == HIGH) {
                head_high = NULL;
            } else if (current_runing->priority == MIDDLE) {
                head_middle = NULL;
            } else if (current_runing->priority == LOW) {
                head_low = NULL;
            }
        }
        tmp->prev = NULL;
        tmp->next = NULL;
        VALGRIND_STACK_DEREGISTER(tmp->vid);
        free(tmp->context->uc_stack.ss_sp);
        free(tmp->context);
        free(tmp);
        current_runing = NULL;

        if (table != NULL) {
            free(table);
            table = NULL;
        }
        return 0;
    }
    print_queue();
    // delete current from the queue
    struct Thread* current = thread_queue;
    struct Thread* previous = NULL;
    while (current != NULL) {
        if (current->tid == current_runing->tid) {
            if (previous == NULL) {
                thread_queue = thread_queue->next;
            } else {
                previous->next = current->next;
            }
            break;
        }
        previous = current;
        current = current->next;
    }
    queue_size--;
    print_queue();

    // struct Thread* tmp = thread_queue;
    // while (tmp != NULL) {
    //     if (tcb[tmp->tid].status == WAITING) {
    //         int waitingfor = tcb[tmp->tid].waitingfor;
    //         if (waitingfor == current_runing->tid) {
    //             current_running = tmp;
    //         }
    //     }
    //     tmp = tmp->next;
    // }
    // printf("The current is %d\n", current_running->tid);

    struct Thread* tmpp = current_runing;

    VALGRIND_STACK_DEREGISTER(tmpp->vid);
    if (tmpp->context->uc_stack.ss_sp != NULL){
        free(tmpp->context->uc_stack.ss_sp);
        free(tmpp->context);
    }
    printf("get to here successfully, %d\n", current_runing->tid);
    free(tmpp);

    return 0;
}

// the scheduler for priority mode, not return until the queues are empty
int priority_scheduler() {
    while (head_high != NULL || head_low != NULL || head_middle != NULL) {

        if (table == NULL) {
            mode = generate_table();
        } 

        struct timeval seed;
        gettimeofday(&seed, NULL);
        int random = seed.tv_usec % table_size;
        int target_level = table[random];

            if (target_level == HIGH) {
                current_running = head_high;
            } else if (target_level == MIDDLE) {
                current_running = head_middle;
            } else if (target_level == LOW) {
                current_running = head_low;
            } else return -1;

        tcb[current_running->tid].status = SCHEDULED;
        log_operation(SCHEDULED, current_running->tid, current_running->priority);
        swapcontext(&main_context, current_running->context);
        if(tcb[current_running->tid].status != STOPPED){
            // block sigalrm to prevent from interrupting the cleanup status
            signal(SIGALRM,block_handler);
            pause();
            thread_exit(current_running);
            signal(SIGALRM,alarm_handler);
        } else {
            int priority = current_running->priority;
            if (priority == HIGH) {
                head_high = head_high ->next;
            } else if (priority == MIDDLE) {
                head_middle = head_middle -> next;
            } else head_low = head_low -> next;
        }

    }
    return 0; // Return success
}

// a scheduler for SJF and FIFO mode
void scheduler(int tid) {
    while ((queue_size >= 0) && tcb[tid].status != FINISHED) {
        if (current_running == NULL) {
            current_running = thread_queue;
        } 
        if (tcb[current_running->tid].status == WAITING) {
            int waitingfor = tcb[current_running->tid].waitingfor;
            if (tcb[waitingfor].status == FINISHED) {
                tcb[current_running->tid].status = READY;
            } else{
                if (current_running->next == NULL) {
                    current_running = thread_queue;
                } else {
                    current_running = current_running->next;
                }
            }
        }
        if (tcb[current_running->tid].status == WAITING) continue;
        printf("Get to the scheduler, the next runnning is %d\n", current_running->tid);
        log_operation(SCHEDULED, current_running->tid, current_running->priority);
        tcb[current_running->tid].status = SCHEDULED;

        if (flag == SJF) {
            gettimeofday(&begin, NULL);
        }
        
        swapcontext(&main_context, current_running->context);
        if (current_running == NULL)printf("current_running is NULL\n");
        if (flag == SJF) {
            gettimeofday(&end, NULL);
            long seconds  = end.tv_sec  - begin.tv_sec;
            long useconds = end.tv_usec - begin.tv_usec;
            long runningtime = ((seconds) * 1000 + useconds/1000.0) + 0.5;
            // update the avg
            cur_avg = (long)(cur_run_num * cur_avg + runningtime)/(cur_run_num + 1);
            cur_run_num ++;
        }

        if (current_running == NULL) {
            struct Thread* curtmp = thread_queue;
            while(curtmp != NULL) {
                if (tcb[curtmp->tid].status == WAITING) {
                    int waitingfor = tcb[curtmp->tid].waitingfor;
                    if (tcb[waitingfor].status == FINISHED) {
                        current_running = curtmp;
                    }
                }
                curtmp = curtmp->next;
            }
        }
        printf("the current running is %d\n", current_running->tid);
        thread_exit(current_running);
        printf("exit finished, comming back to the scheduler\n");
        if (current_running == NULL)printf("current_running is NULL\n");
        //printf("The current running is %d, \n", current_running->tid);
        print_queue();
    }
    printf("Get out of the scheduler\n");
}

// initialize the library and set the alarm
int thread_libinit(int policy) {
    flag = policy;
    log_fd = open(log_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (log_fd < 0) {
        perror("Error opening log file");
        return -1;
    }
    if (policy == PRIORITY) {
        signal(SIGALRM, alarm_handler);
    }else  signal(SIGALRM, block_handler);
    // Initialize timer to fire every interval_sec seconds.
    setrtimer();
    return 0;
}

// insert the head of the queue to the appropriate place in the queue based on estimated time
int insert_queue() {
    struct Thread* ptr = thread_queue->next;
    struct Thread* insert = thread_queue;
    struct Thread* previous = NULL;
    int insert_time = tcb[insert->tid].est_time;
   
    while (ptr != NULL) {
        int tid = ptr->tid;
        if (tcb[tid].est_time > insert_time) {
            // insert at front
            if (previous == NULL) {
                // insert at head
                //printf("%d insert at head\n", insert->tid);
                return 0;
            }
            thread_queue = thread_queue->next;
            previous->next = insert;
            insert->next = ptr;
            //printf("%d insert in the middle\n", insert->tid);
            return 0;
        }
        previous = ptr;
        ptr = ptr->next;
    }
    // insert at tail
    thread_queue = thread_queue->next;
    previous->next = insert;
    insert->next = NULL;
    //printf("%d insert at tail\n",insert->tid);
    return 0;
}

int thread_create(void (*func)(void *), void *arg, int priority) {
    if (flag == PRIORITY && priority != LOW && priority != MIDDLE && priority != HIGH) {
        return -1;
    }
    if (queue_size >= MAXTHREAD - 1) {
        return -1;
    }
    struct Thread *new_thread = malloc(sizeof(struct Thread));
    if (new_thread == NULL) {
        return -1;
    }

    new_thread->tid = queue_size;
    new_thread->priority = priority;
    ucontext_t* context = (ucontext_t*)malloc(sizeof(ucontext_t));
    new_thread->context = context;
    new_thread->context->uc_stack.ss_sp = malloc(STACKSIZE);

    if (new_thread->context->uc_stack.ss_sp == NULL) {
        free(new_thread);
        return -1;
    }
    getcontext(new_thread->context);
    new_thread->context->uc_stack.ss_size = STACKSIZE;
    new_thread->vid = VALGRIND_STACK_REGISTER(new_thread->context->uc_stack.ss_sp, new_thread->context->uc_stack.ss_sp + STACKSIZE);
    new_thread->context->uc_stack.ss_flags = 0;
    sigemptyset(&(new_thread->context->uc_sigmask));
    new_thread->context->uc_link = &main_context;
    new_thread->next = NULL;
    makecontext(new_thread->context, (void (*)(void))func, 1, arg);
    if (flag == PRIORITY) {
        tcb[queue_size].tid = queue_size;
        queue_size ++;
        if (priority == LOW) {
            low_queue_size ++;
            if (head_low == NULL) {
                new_thread->prev = new_thread;
                new_thread->next = new_thread;
                head_low = new_thread;
            } else {
                (head_low->prev)->next = new_thread;
                new_thread->prev = head_low->prev;
                head_low->prev = new_thread;
                new_thread->next = head_low;
            }
        } else if (priority == MIDDLE) {
            middle_queue_size ++;
            if (head_middle == NULL) {
                new_thread->prev = new_thread;
                new_thread->next = new_thread;
                head_middle = new_thread;
            } else{
                (head_middle->prev)->next = new_thread;
                new_thread->prev = head_middle->prev;
                head_middle->prev = new_thread;
                new_thread->next = head_middle;
            }
        } else if (priority == HIGH) {
            high_queue_size ++;
            if (head_high == NULL) {
                new_thread->prev = new_thread;
                new_thread->next = new_thread;
                head_high = new_thread;
            } else{
                (head_high->prev)->next = new_thread;
                new_thread->prev = head_high->prev;
                head_high->prev = new_thread;
                new_thread->next = head_high;
            }
        }
        log_operation(READY, new_thread->tid, new_thread->priority);
        return new_thread->tid;
    }

    tcb[queue_size].tid = queue_size;
    if (cur_avg == 0) {
        tcb[queue_size].est_time = QUANTA/2;
    }
    tcb[queue_size].hst_time[0] = 0;
    tcb[queue_size].hst_time[1] = 0;
    tcb[queue_size].hst_time[2] = 0;

    // Add the new thread to the end of the queue based on the scheduling policy
    if (flag == FIFO) {
        if (thread_queue == NULL) {
            thread_queue = new_thread;
        } else {
            struct Thread *current = thread_queue;
            while (current->next != NULL) {
                current = current->next;
            }
            current->next = new_thread;
            new_thread->next = NULL;
        }
    } else if (flag == SJF) {
        if (thread_queue == NULL) {
            thread_queue = new_thread;
        } else {
            // insert at the front
            new_thread->next = thread_queue;
            thread_queue = new_thread;
            insert_queue();
        }
    }

    queue_size++;
    return new_thread->tid;
}

int thread_yield(void) {

    // do nothing if only one in queue or no current_running
    if ((queue_size <= 1 && current_running->tid == 0)|| current_running == NULL) {
        return 0;
    }
    if (queue_size <= 1 && current_running->tid != 0) {
        return 0;
    }
    tcb[current_running->tid].status = STOPPED;
    log_operation(STOPPED, current_running->tid, current_running->priority);

    if (flag == SJF) {
        gettimeofday(&end, 0);
        long seconds  = end.tv_sec  - begin.tv_sec;
        long useconds = end.tv_usec - begin.tv_usec;
        long runningtime = ((seconds) * 1000 + useconds/1000.0) + 0.5;
        //printf("runningtime is %ld\n", runningtime);
        // update the avg
        cur_avg = (long)(cur_run_num * cur_avg + runningtime)/(cur_run_num + 1);
        cur_run_num ++;
        // update the history
        int tid = current_running->tid;
        long recent_3 = tcb[tid].hst_time[2] = tcb[tid].hst_time[1];
        long recent_2 = tcb[tid].hst_time[1] = tcb[tid].hst_time[0];
        long recent_1 = tcb[tid].hst_time[0] = runningtime;

        tcb[tid].est_time = (recent_1 + recent_2 + recent_3)/(long)3;
        //printf("r1: %ld r2: %ld r3: %ld\n", recent_1, recent_2, recent_3 );
        //printf("update the history of thread %d with %ld\n",tid, tcb[tid].est_time);
        insert_queue();
        gettimeofday(&begin, 0);
        // Select the next thread to run
        struct Thread* insert = current_running;
        current_running = thread_queue;
        swapcontext(insert->context, current_running->context);
        return 0;
    } else if (flag == FIFO) {
        struct Thread *current = thread_queue; // Initialize current to thread_queue
        while (current->next != NULL) {
            current = current->next;
        }
        // current is now tail
        current->next = current_running;
        thread_queue = thread_queue->next;
        current_running->next = NULL;

        // Select the next thread to run
        struct Thread* insert = current_running;
        current_running = thread_queue;
        swapcontext(insert->context, current_running->context);
        return 0;
    } else {
        swapcontext(current_running->context, &main_context);
        return 0;
    }
}

int thread_join(int tid) {
    printf("Get into the thread_join, %d\n", tid);

    int tmp_queue_size = queue_size;
    if (queue_size == 0) {
        return 0;
    }
    if (flag == PRIORITY){
        // if no thread is running currently
        if (current_running == NULL) {
            //printf("Get into the here1\n");
            priority_scheduler();
            //printf("Get into the here2\n");
        }
        if (tcb[tid].status == FINISHED) {
            return 0;
        }else return -1;
    }
    // if no thread is running currently
    if (current_running == NULL) {
        scheduler(tid);
    } else {
        // if there is a thread running
         printf("Switching from %d to %d\n", current_running->tid, current_running->next->tid);
        // Select the next thread to run
        tcb[current_running->tid].status = WAITING;
        log_operation(WAITING, current_running->tid, current_running->priority);
        tcb[current_running->tid].waitingfor = tid;
        scheduler(tid);
        //printf("Getting back from the scheduler, the current running is %d\n", current_running->tid);
    }

    printf("Get out of the thread_join, %d\n", tid);

    if(tid < 0 || tid >= tmp_queue_size) {
        return -1;
    }
    if (tcb[tid].status == FINISHED) {
        return 0;
    }else return -1;
}

