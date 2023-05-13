#include <types.h>
#include <kern/errno.h>
#include <kern/unistd.h>
#include <kern/wait.h>
#include <lib.h>
#include <syscall.h>
#include <current.h>
#include <proc.h>
#include <synch.h>
#include <thread.h>
#include <addrspace.h>
#include <copyinout.h>
#include "opt-A2.h"


#if OPT_A2
  #include <mips/trapframe.h>
  #include <vfs.h>
  #include <kern/fcntl.h>
#endif //OPT_A2

//this entire file contains new changes

#if OPT_A2
void copyArgs(vaddr_t *stackptr, char **kernelArgs, int count) {
  //copy arguments from user space to new address space
    vaddr_t storeArg[count + 1];
    int alignLen = 0;
    int retVal = 0;

    storeArg[count] = (vaddr_t) NULL;
    for (int i = count - 1; i >= 0; i--) {
      alignLen = strlen(kernelArgs[i]) + 1; //length of the string
      *stackptr -= ROUNDUP(alignLen, 4); //ensure each argument/pointer is 4 block aligned on the stack
      retVal = copyout(kernelArgs[i], (userptr_t) *stackptr, alignLen);
      if (retVal) { panic("Error occurred during first copyout in execv."); }
      storeArg[i] = *stackptr;
    }

    for (int i = count; i >= 0; i--) {
      *stackptr -= sizeof(vaddr_t);
      if (i == count) {
        retVal = copyout(NULL, (userptr_t) *stackptr, sizeof(vaddr_t));
        continue;
      } else {
        retVal = copyout(&storeArg[i], (userptr_t) *stackptr, sizeof(vaddr_t));
      }
      if (retVal) { panic("Error occurred during second copyout in execv."); }
    }
}
#endif //OPT_A2

#if OPT_A2
  int sys_execv(userptr_t program, userptr_t args) {
    //code from runprogram.c
    struct addrspace *as;
    struct vnode *v;
    vaddr_t entrypoint, stackptr;
    int result;

    //cast userptr_t types
    char *param1 = (char *) program;
    char **param2 = (char **) args;

    int count = 0;
    int index = 0;
    size_t gotlen; //dummy variable

    //count number of args
    while (param2[index] != NULL) {
      index++;
      count++;
    }    
    
    //copy arguments into the kernel
    char *kernelArgs[count + 1]; //holds the arguments for kernel
    for (int i = 0; i < count; i++) {
      int len = strlen(param2[i]) + 1;
      kernelArgs[i] = kmalloc(len);
      int retVal = copyinstr((userptr_t)param2[i], kernelArgs[i], len, &gotlen);
      if (retVal) {
        panic("Error occurred during copyinstr in execv.");
        return retVal;
      }
    }
    kernelArgs[count] = NULL;

    //copy program path from user space into the kernel
    int programLen = strlen(param1) + 1; //program name length
    char kernelProgram[programLen]; //kernel program path
    int retVal = copyinstr((userptr_t) param1, kernelProgram, programLen, &gotlen);
    if (retVal) {
      panic("Error occurred during copyinstr in execv.");
      return retVal;
    }

    // kprintf("Program Name: %s\n", kernelProgram);
    // kprintf("Arguments: \n");
    // for (int j = 0; j < count; j++) {
    //   kprintf("%s\n", kernelArgs[j]);
    // }

    //runprogram code:
    /* Open the file. */
    result = vfs_open(kernelProgram, O_RDONLY, 0, &v);
    if (result) {
      return result;
    }

    /* We should be a new process. */
    //KASSERT(curproc_getas() == NULL);

    /* Create a new address space. */
    as = as_create();
    if (as ==NULL) {
      vfs_close(v);
      return ENOMEM;
    }

    /* Switch to it and activate it. */
    struct addrspace *old = curproc_setas(as);
    as_activate();

    /* Load the executable. */
    result = load_elf(v, &entrypoint);
    if (result) {
      /* p_addrspace will go away when curproc is destroyed */
      vfs_close(v);
      return result;
    }

    /* Done with the file now. */
    vfs_close(v);

    /* Define the user stack in the address space */
    result = as_define_stack(as, &stackptr);
    if (result) {
      /* p_addrspace will go away when curproc is destroyed */
      return result;
    }

    copyArgs(&stackptr, kernelArgs, count);

    for (int k = 0; k < count; k++) {
      kfree(kernelArgs[k]);
    }

    if (old != NULL)  as_destroy(old);

    /* Warp to user mode. */
    enter_new_process(count /*argc*/, (userptr_t)stackptr /*userspace addr of argv*/,
          stackptr, entrypoint);
    
    /* enter_new_process does not return. */
    panic("enter_new_process returned\n");
    return EINVAL;
  }
#endif //OPT_A2B

#if OPT_A2
  int sys_fork(struct trapframe *tf, pid_t *retval) {
    KASSERT(curproc != NULL); //ensure if current process is not null before continuing

    //create child process
    struct proc *childProc = proc_create_runprogram(curproc->p_name);
    //if there's no memory then the process will not be created
    if (childProc == NULL) {
      DEBUG(DB_SYSCALL,"Unsuccessful creation of child process");
      return ENOMEM;
    }
    //there will be max 64 pid's so check if 0 < pid <= 64
    if (childProc->pid < 0 || childProc->pid > 64) {
      DEBUG(DB_SYSCALL,"Error code occurred due to invalid pid");
      proc_destroy(childProc);
      return EMPROC; //too many process on the system
    }

    //copy and set address for child process. Look at curproc_setas() for hints
    spinlock_acquire(&childProc->p_lock);
    int safeCopy = as_copy(curproc_getas(), &(childProc->p_addrspace));
    spinlock_release(&childProc->p_lock);
    //check if as_copy returned an error
    if (safeCopy != 0) {
      DEBUG(DB_SYSCALL,"Error code occurred while copying the address");
      proc_destroy(childProc);
      return ENOMEM; //error occurred mos likely due to not having enough memory.
    }

    //set the parent of the child process and also add the child to the children array for this parent
    //unique pid is set in proc_create() function and is initialized in proc_bootstrap() in proc.c
    lock_acquire(curproc->conditionLock);
    childProc->parent = curproc;
    array_add(curproc->children, childProc, NULL);
    lock_release(curproc->conditionLock);

    //make a trapframe copy in the kernel heap
    struct trapframe *childTF = kmalloc(sizeof(struct trapframe));
    if (childTF == NULL) {
      DEBUG(DB_SYSCALL,"Error creating the trapframe");
      proc_destroy(childProc);
      return ENOMEM; //most likely due to insufficient VM available for child process
    }
    //make a copy of the parent's trap frame in the kernel heap and pass it in to thread_fork to avoid synchronization issues
    //don't use memcpy as mentioned in cs350
    *childTF = *tf;

    //create a thread for the child process
    int retVal = thread_fork(childProc->p_name, childProc, enter_forked_process, childTF, 0); //figure out the function signature for the argument?
    if (retVal) {
      DEBUG(DB_SYSCALL,"Error in thread_fork in sys_fork");
      proc_destroy(childProc);
      kfree(childTF);
      return ENOMEM;
    }

    *retval = childProc->pid;
    return 0;
  }
#endif //OPT_A2

  /* this implementation of sys__exit does not do anything with the exit code */
  /* this needs to be fixed to get exit() and waitpid() working properly */


/*
process can be deleted on 3 cases
1) parent has already exited -> taken care of in exit
2) you exit and all your children are dead -> taken care of in destroy
3) parent has already called waitpid() -> we take care of this in waitpid by making the parent wait until the child exits
*/
void sys__exit(int exitcode) {
  struct addrspace *as;
  struct proc *p = curproc;

  DEBUG(DB_SYSCALL,"Syscall: _exit(%d)\n",exitcode);

  KASSERT(curproc->p_addrspace != NULL);
  as_deactivate();
  /*
   * clear p_addrspace before calling as_destroy. Otherwise if
   * as_destroy sleeps (which is quite possible) when we
   * come back we'll be calling as_activate on a
   * half-destroyed address space. This tends to be
   * messily fatal.
   */
  as = curproc_setas(NULL);
  as_destroy(as);

  /* detach this thread from its process */
  /* note: curproc cannot be used after this call */
  proc_remthread(curthread);

  //new changes
  //p is a pointer to curporc so we can use p instead of curproc since we cannot use curproc
  //after line 333
  #if OPT_A2
    //if parent is no longer living then we can delete the child (i.e. parent has already exited)
    if (p->parent == NULL) {
      proc_destroy(p);
    }
    //otherwise set the child as a zombie since the parent can still call waitpit in the future
    else {
      p->isDead = true;
      p->exitcode = exitcode;

      //wake up parent
      lock_acquire(p->conditionLock);
      cv_signal(p->waitCondition, p->conditionLock);
      lock_release(p->conditionLock);

      //set p as a zombie child of p's parent
      struct proc *parentProc = p->parent;
      lock_acquire(parentProc->conditionLock);
      array_add(parentProc->zombie, p, NULL);
      lock_release(parentProc->conditionLock); 
    }    
  #else //pre-A2 code
    /* for now, just include this to keep the compiler from complaining about
     an unused variable */
    (void)exitcode;

    /* if this is the last user process in the system, proc_destroy()
     will wake up the kernel menu thread */
    proc_destroy(p);
  #endif
  
  thread_exit();
  /* thread_exit() does not return, so we should never get here */
  panic("return from thread_exit in sys_exit\n");
}

/* stub handler for waitpid() system call                */
int sys_waitpid(pid_t pid, userptr_t status, int options, pid_t *retval) {
  int exitstatus;
  int result;

  /* this is just a stub implementation that always reports an
     exit status of 0, regardless of the actual exit status of
     the specified process.   
     In fact, this will return 0 even if the specified process
     is still running, and even if it never existed in the first place.

     Fix this!
  */

  if (options != 0) {
    return(EINVAL);
  }

  #if OPT_A2
    KASSERT(curproc != NULL);
    if (pid < 0 || pid > 64) {
      return ESRCH;
    }

    //obtain the child process which has the given pid in waitpid function
    struct proc *childProc = NULL;
    int len = array_num(curproc->children);
    for (int i = 0; i < len; i++) {
      struct proc *correctProc = array_get(curproc->children, i);
      if (correctProc->pid == pid) {
        childProc = correctProc;
        break;
      }
    }
    if (childProc == NULL) {
      return ECHILD; //if no such child process was found for the pid
    }

    //if waitpid is called before child process exits then the parent must wait/block
    lock_acquire(childProc->conditionLock);
    while(!childProc->isDead) {
      cv_wait(childProc->waitCondition, childProc->conditionLock);
    }
    //after the parent is done waiting, set the exitstatus using the macro
    exitstatus = _MKWAIT_EXIT(childProc->exitcode);
    lock_release(childProc->conditionLock);
  #else
    /* for now, just pretend the exitstatus is 0 */
    exitstatus = 0;
  #endif
    result = copyout((void *)&exitstatus,status,sizeof(int));
    if (result) {
      return(result);
    }
    *retval = pid;
    return(0);
}

/* stub handler for getpid() system call                */
int sys_getpid(pid_t *retval) {
  /* for now, this is just a stub that always returns a PID of 1 */
  /* you need to fix this to make it work properly */
  #if OPT_A2
    KASSERT(curproc != NULL);
    *retval = curproc->pid;
  #else
    *retval = 1;
  #endif //OPT_A2
  return(0);
}
