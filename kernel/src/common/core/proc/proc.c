#include <common/core/fd/cwd.h>
#include <common/core/fd/fdtable.h>
#include <common/core/memory/heap.h>
#include <common/core/memory/virt.h>
#include <common/core/proc/proc.h>
#include <common/core/proc/proclayout.h>
#include <common/lib/kmsg.h>
#include <hal/memory/phys.h>
#include <hal/memory/virt.h>
#include <hal/proc/extended.h>
#include <hal/proc/intlevel.h>
#include <hal/proc/isrhandler.h>
#include <hal/proc/stack.h>
#include <hal/proc/state.h>
#include <hal/proc/timer.h>

#define PROC_MOD_NAME "Process Manager & Scheduler"

static uint64_t m_instanceCountsByID[PROC_MAX_PROCESS_COUNT];
static struct Proc_Process *m_processesByID[PROC_MAX_PROCESS_COUNT];
static struct Proc_Process *m_CurrentProcess;
static struct Proc_Process *m_deallocQueueHead;
static struct Proc_Process *m_deallocQueueTail;
static bool m_procInitialized = false;

#define PROC_SCHEDULER_STACK_SIZE 65536
static char Proc_SchedulerStack[PROC_SCHEDULER_STACK_SIZE];

bool Proc_IsInitialized() {
	return m_procInitialized;
}

struct Proc_Process *Proc_GetProcessData(struct Proc_ProcessID id) {
	size_t array_index = id.id;
	if (array_index >= PROC_MAX_PROCESS_COUNT) {
		return NULL;
	}
	int level = HAL_InterruptLevel_Elevate();
	struct Proc_Process *data = m_processesByID[array_index];
	if (data == NULL) {
		HAL_InterruptLevel_Recover(level);
		return NULL;
	}
	if (m_instanceCountsByID[array_index] != id.instanceNumber) {
		HAL_InterruptLevel_Recover(level);
		return NULL;
	}
	HAL_InterruptLevel_Recover(level);
	return data;
}

static struct Proc_ProcessID Proc_AllocateProcessID(struct Proc_Process *process) {
	struct Proc_ProcessID result;
	for (size_t i = 0; i < PROC_MAX_PROCESS_COUNT; ++i) {
		int level = HAL_InterruptLevel_Elevate();
		if (m_processesByID[i] == NULL) {
			m_processesByID[i] = process;
			result.id = i;
			result.instanceNumber = m_instanceCountsByID[i];
			HAL_InterruptLevel_Recover(level);
			return result;
		}
		HAL_InterruptLevel_Recover(level);
	}
	result.id = PROC_MAX_PROCESS_COUNT;
	result.instanceNumber = 0;
	return result;
}

struct Proc_ProcessID Proc_MakeNewProcess(struct Proc_ProcessID parent) {
	struct Proc_Process *process = ALLOC_OBJ(struct Proc_Process);
	if (process == NULL) {
		goto fail;
	}
	uintptr_t stack = (uintptr_t)Heap_AllocateMemory(PROC_KERNEL_STACK_SIZE);
	if (stack == 0) {
		goto free_process_obj;
	}
	char *processState = (char *)(Heap_AllocateMemory(HAL_ProcessStateSize));
	if (processState == NULL) {
		goto free_stack;
	}
	char *extendedState = (char *)(Heap_AllocateMemory(HAL_ExtendedStateSize));
	if (extendedState == NULL) {
		goto free_process_state;
	}
	struct Proc_ProcessID new_id = Proc_AllocateProcessID(process);
	if (!Proc_IsValidProcessID(new_id)) {
		goto free_extended_state;
	}
	memset(processState, 0, HAL_ProcessStateSize);
	process->next = process->prev = process->waitQueueHead = process->waitQueueTail = process->nextInQueue = NULL;
	process->ppid = parent;
	process->pid = new_id;
	process->processState = processState;
	process->extendedState = extendedState;
	if ((uintptr_t)(process->extendedState) % 16 != 0) {
		KernelLog_ErrorMsg(PROC_MOD_NAME, "Incorrect align for the extended state");
	}
	process->kernelStack = stack;
	process->returnCode = 0;
	process->state = SLEEPING;
	process->addressSpace = NULL;
	process->childCount = 0;
	process->cwd = NULL;
	struct Proc_Process *parentProcess = Proc_GetProcessData(parent);
	if (parentProcess != NULL) {
		parentProcess->childCount++;
	}
	return new_id;
free_extended_state:
	Heap_FreeMemory(extendedState, HAL_ExtendedStateSize);
free_process_state:
	Heap_FreeMemory(processState, HAL_ProcessStateSize);
free_stack:
	Heap_FreeMemory((void *)stack, PROC_KERNEL_STACK_SIZE);
free_process_obj:
	FREE_OBJ(process);
fail:;
	struct Proc_ProcessID failed_id;
	failed_id.instanceNumber = 0;
	failed_id.id = PROC_MAX_PROCESS_COUNT;
	return failed_id;
}

void Proc_Resume(struct Proc_ProcessID id) {
	struct Proc_Process *process = Proc_GetProcessData(id);
	if (process == NULL) {
		return;
	}
	int level = HAL_InterruptLevel_Elevate();
	process->state = RUNNING;
	struct Proc_Process *next, *prev;
	next = m_CurrentProcess;
	prev = m_CurrentProcess->prev;
	next->prev = process;
	prev->next = process;
	process->next = next;
	process->prev = prev;
	HAL_InterruptLevel_Recover(level);
}

void Proc_InsertChildBack(struct Proc_Process *process) {
	struct Proc_ProcessID id = Proc_GetProcessID();
	struct Proc_Process *currentProcess = Proc_GetProcessData(id);
	int level = HAL_InterruptLevel_Elevate();
	process->nextInQueue = currentProcess->waitQueueHead;
	currentProcess->waitQueueHead = process;
	if (currentProcess->waitQueueTail == NULL) {
		currentProcess->waitQueueHead = process;
	}
	HAL_InterruptLevel_Recover(level);
}

static void Proc_CutFromActiveList(struct Proc_Process *process) {
	struct Proc_Process *prev = process->prev;
	struct Proc_Process *next = process->next;
	prev->next = next;
	next->prev = prev;
}

void Proc_Suspend(struct Proc_ProcessID id, bool overrideState) {
	struct Proc_Process *process = Proc_GetProcessData(id);
	if (process == NULL) {
		return;
	}
	int level = HAL_InterruptLevel_Elevate();
	if (overrideState) {
		process->state = SLEEPING;
	}
	Proc_CutFromActiveList(process);
	if (process == m_CurrentProcess) {
		Proc_Yield();
		// back on track
		HAL_InterruptLevel_Recover(level);
	} else {
		HAL_InterruptLevel_Recover(level);
	}
}

struct Proc_ProcessID Proc_GetProcessID() {
	struct Proc_Process *current = m_CurrentProcess;
	struct Proc_ProcessID id = current->pid;
	return id;
}

void Proc_SuspendSelf(bool overrideState) {
	Proc_Suspend(Proc_GetProcessID(), overrideState);
}

void Proc_Dispose(struct Proc_Process *process) {
	int level = HAL_InterruptLevel_Elevate();
	if (m_deallocQueueHead == NULL) {
		m_deallocQueueHead = m_deallocQueueTail = process;
	} else {
		m_deallocQueueTail->nextInQueue = process;
		m_deallocQueueTail = process;
	}
	process->nextInQueue = NULL;
	HAL_InterruptLevel_Recover(level);
}

static void Proc_FreeProcessesFromQueueOnExit(struct Proc_Process *process) {
	int level = HAL_InterruptLevel_Elevate();
	struct Proc_Process *waitQueueHead = process->waitQueueHead;
	struct Proc_Process *waitQueueTail = process->waitQueueTail;
	if (waitQueueHead == NULL) {
		HAL_InterruptLevel_Recover(level);
		return;
	}
	if (waitQueueHead == waitQueueTail) {
		HAL_InterruptLevel_Recover(level);
		Proc_Dispose(waitQueueHead);
		return;
	}
	m_deallocQueueTail->nextInQueue = waitQueueHead;
	waitQueueTail->nextInQueue = NULL;
	m_deallocQueueTail = waitQueueTail;
	HAL_InterruptLevel_Recover(level);
}

void Proc_Exit(int exitCode) {
	HAL_InterruptLevel_Elevate();
	struct Proc_Process *process = m_CurrentProcess;
	Proc_FreeProcessesFromQueueOnExit(process);
	process->returnCode = exitCode;
	process->terminatedNormally = true;
	m_instanceCountsByID[process->pid.id]++;
	m_processesByID[process->pid.id] = NULL;
	process->state = ZOMBIE;
	struct Proc_ProcessID parentID = process->ppid;
	struct Proc_Process *parentProcess = Proc_GetProcessData(parentID);
	if (parentProcess == NULL) {
		Proc_Dispose(process);
	} else {
		if (parentProcess->waitQueueHead == NULL) {
			parentProcess->waitQueueHead = parentProcess->waitQueueTail = process;
		} else {
			parentProcess->waitQueueTail->nextInQueue = process;
		}
		process->nextInQueue = NULL;
		if (parentProcess->state == WAITING_FOR_CHILD_TERM) {
			Proc_Resume(parentID);
		}
	}
	Proc_CutFromActiveList(process);
	Proc_Yield();
}

struct Proc_Process *Proc_GetWaitingQueueHead(struct Proc_Process *process) {
	struct Proc_Process *result = process->waitQueueHead;
	if (process->waitQueueTail == result) {
		process->waitQueueHead = process->waitQueueTail = NULL;
	} else {
		process->waitQueueHead = process->waitQueueHead->nextInQueue;
	}
	return result;
}

struct Proc_Process *Proc_WaitForChildTermination(bool returnImmediately) {
	int level = HAL_InterruptLevel_Elevate();
	struct Proc_Process *process = m_CurrentProcess;
	if (process->childCount == 0) {
		HAL_InterruptLevel_Recover(level);
		return NULL;
	}
	process->childCount--;
	if (process->waitQueueHead != NULL) {
		struct Proc_Process *result = Proc_GetWaitingQueueHead(process);
		HAL_InterruptLevel_Recover(level);
		return result;
	}
	if (returnImmediately) {
		HAL_InterruptLevel_Recover(level);
		return NULL;
	}
	process->state = WAITING_FOR_CHILD_TERM;
	Proc_SuspendSelf(false);
	struct Proc_Process *result = Proc_GetWaitingQueueHead(process);
	return result;
}

void Proc_Yield() {
	HAL_Timer_TriggerInterrupt();
}

void Proc_PreemptCallback(MAYBE_UNUSED void *ctx, char *state) {
	memcpy(m_CurrentProcess->processState, state, HAL_ProcessStateSize);
	HAL_ExtendedState_StoreTo(m_CurrentProcess->extendedState);
	m_CurrentProcess = m_CurrentProcess->next;
	HAL_ExtendedState_LoadFrom(m_CurrentProcess->extendedState);
	memcpy(state, m_CurrentProcess->processState, HAL_ProcessStateSize);
	VirtualMM_PreemptToAddressSpace(m_CurrentProcess->addressSpace);
	HAL_ISRStacks_SetSyscallsStack(m_CurrentProcess->kernelStack + PROC_KERNEL_STACK_SIZE);
}

void Proc_Initialize() {
	for (size_t i = 0; i < PROC_MAX_PROCESS_COUNT; ++i) {
		m_processesByID[i] = NULL;
		m_instanceCountsByID[i] = 0;
	}
	struct Proc_ProcessID kernelProcID = Proc_MakeNewProcess(PROC_INVALID_PROC_ID);
	if (!Proc_IsValidProcessID(kernelProcID)) {
		KernelLog_ErrorMsg(PROC_MOD_NAME, "Failed to allocate kernel process");
	}
	struct Proc_Process *kernelProcessData = Proc_GetProcessData(kernelProcID);
	if (kernelProcessData == NULL) {
		KernelLog_ErrorMsg(PROC_MOD_NAME, "Failed to access data of the kernel process");
	}
	kernelProcessData->state = RUNNING;
	m_CurrentProcess = kernelProcessData;
	kernelProcessData->next = kernelProcessData;
	kernelProcessData->prev = kernelProcessData;
	kernelProcessData->processState = Heap_AllocateMemory(HAL_ProcessStateSize);
	if (kernelProcessData->processState == NULL) {
		KernelLog_ErrorMsg(PROC_MOD_NAME, "Failed to allocate kernel process state");
	}
	kernelProcessData->addressSpace = VirtualMM_MakeAddressSpaceFromRoot(HAL_VirtualMM_GetCurrentAddressSpace());
	if (kernelProcessData->addressSpace == NULL) {
		KernelLog_ErrorMsg(PROC_MOD_NAME, "Failed to allocate process address space object");
	}
	m_deallocQueueHead = m_deallocQueueTail = NULL;
	HAL_ISRStacks_SetISRStack((uintptr_t)(Proc_SchedulerStack) + PROC_SCHEDULER_STACK_SIZE);
	if (!HAL_Timer_SetCallback((HAL_ISR_Handler)Proc_PreemptCallback)) {
		KernelLog_ErrorMsg(PROC_MOD_NAME, "Failed to set timer callback");
	}
	m_procInitialized = true;
}

bool Proc_PollDisposeQueue() {
	int level = HAL_InterruptLevel_Elevate();
	struct Proc_Process *process = m_deallocQueueHead;
	if (process == NULL) {
		HAL_InterruptLevel_Recover(level);
		return false;
	}
	m_deallocQueueHead = process->nextInQueue;
	HAL_InterruptLevel_Recover(level);
	if (process->addressSpace != NULL) {
		VirtualMM_DropAddressSpace(process->addressSpace);
	}
	if (process->extendedState != NULL) {
		Heap_FreeMemory((void *)(process->extendedState), HAL_ExtendedStateSize);
	}
	if (process->processState != NULL) {
		Heap_FreeMemory((void *)(process->processState), HAL_ProcessStateSize);
	}
	if (process->kernelStack != 0) {
		Heap_FreeMemory((void *)(process->kernelStack), PROC_KERNEL_STACK_SIZE);
	}
	if (process->fdTable != NULL) {
		FileTable_Drop(process->fdTable);
	}
	if (process->cwd != NULL) {
		File_Drop(process->cwd);
	}
	FREE_OBJ(process);
	return true;
}
