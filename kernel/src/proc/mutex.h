#ifndef __MUTEX_H_INCLUDED__
#define __MUTEX_H_INCLUDED__

#include <utils.h>

struct mutex {
	struct proc_process *queue_head;
	struct proc_process *queue_tail;
	bool locked;
};

void mutex_init(struct mutex *mutex);
void mutex_lock(struct mutex *mutex);
void mutex_unlock(struct mutex *mutex);

#endif