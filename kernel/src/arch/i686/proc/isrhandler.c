#include <common/core/memory/heap.h>
#include <hal/proc/isrhandler.h>

extern void i686_isr_with_ctx_template_begin();
extern void i686_isr_with_ctx_template_end();

struct i686_isr_with_ctx_template {
	char rsvd1[0x16];
	void *arg;
	char rsvd2;
	void *func;
} packed;

static size_t i686_isr_template_size() {
	size_t template_size = (uint32_t)i686_isr_with_ctx_template_end -
						   (uint32_t)i686_isr_with_ctx_template_begin;
	return template_size;
}

hal_isr_handler_t i686_isr_handler_new(void *func, void *arg) {
	struct i686_isr_with_ctx_template *new_function =
		(struct i686_isr_with_ctx_template *)heap_alloc(
			i686_isr_template_size());
	if (new_function == NULL) {
		return NULL;
	}
	memcpy(new_function, (void *)i686_isr_with_ctx_template_begin,
		   i686_isr_template_size());
	new_function->func = func;
	new_function->arg = arg;
	return (hal_isr_handler_t)new_function;
}