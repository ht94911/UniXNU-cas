typedef int wait_result_t;
typedef void *thread_t;
typedef int kern_return_t;
typedef int integer_t;
typedef void (*thread_continue_t)(void *,wait_result_t);
#define THREAD_NULL ((void *)0)
#define KERN_SUCCESS 0
thread_t current_thread(void);
void thread_set_thread_name(thread_t,const char *);
kern_return_t kernel_thread_start(void (*)(void *,wait_result_t),void *,thread_t *);
void thread_deallocate(thread_t);
void thread_terminate_self(void);
