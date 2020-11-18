#ifndef __IOWAIT_H_INCLUDED__
#define __IOWAIT_H_INCLUDED__

#include <utils.h>

typedef void (*iowait_handler_t)(void *ctx, void *frame);
typedef bool (*iowait_wakeup_handler_t)(void *ctx);

void iowait_init();
struct iowait_list_entry *
iowait_add_handler(uint8_t irq, iowait_handler_t int_handler,
				   iowait_wakeup_handler_t check_hander, void *ctx);
void iowait_wait(struct iowait_list_entry *entry);
void iowait_enable_used_irq();

#endif