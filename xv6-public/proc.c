#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

volatile int num_stride;
volatile int total_share;

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

struct {
  struct proc *proc;
  int pass;
  int stride;
} stride_list[NPROC];

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);
extern void threadret(void);

static void wakeup1(void *chan);

void
ret(void)
{
  asm volatile("ret");
}

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Getting maximum level of RUNNABLEs in mlfq
int
maxlev(void) {
  struct proc *p;
  int max = 0;
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if(p->state == RUNNABLE) {
      if(p->mlfqlev > max) max = p->mlfqlev;
    }
  }
  return max;
}

// Priority boost
void
boost(void) {
  struct proc *p;
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if(p->state == RUNNABLE ||
	p->state == RUNNING ||
	p->state == SLEEPING) {
      if(p->mlfqlev != -1) p->mlfqlev = 2;
      p->hitzone = 0;
      p->strike = 0;
      //p->allotment = 50000000; FOR MLFQ + STRIDE
      p->allotment = 200000000;
      p->stampin = 0;
    }
  }
}

// Greatest Common Divisor Function
int
gcd(int a, int b) {
  if(a == 0 || b == 0)
    panic("Zero can't be GCDed");
  if(a == b) return a;
  if(a > b) return gcd(a-b, b);
  return gcd(a, b-a);
}

// Set stride - stride is multiple of all others except self
// RETURNS Least Common Multiple
void
set_stride(void) {
  struct proc *p;
  int i;

  int gcd_temp;
  int lcm_temp;
  int LCM; // The LCM
  total_share = 0;
  num_stride = 0;

  cli();

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    //stride_list[num_stride].pass = 0;
    if(p->is_stride) { // == 1?
      p->mlfqlev = -1;
      total_share += p->share;
      stride_list[num_stride].proc = p;
      num_stride++;
    }
  }

  gcd_temp = 100 - total_share;
  lcm_temp = 1;
  LCM = 100 - total_share;

  //cprintf("num: %d total_share: %d\n", num_stride, total_share);

  // Actual stride setting
  if(num_stride > 0) {
    // calculate gcd
    for(i = 0; i < num_stride; i++) {
      gcd_temp = gcd(gcd_temp, stride_list[i].proc->share);
    }
    for(i = 0; i < num_stride; i++) {
      stride_list[i].pass = stride_list[i].proc->share/gcd_temp;
      lcm_temp *= stride_list[i].pass;
    }

    LCM = lcm_temp * (100 - total_share);

    // SET STRIDE
    for(i = 0; i < num_stride; i++) {
      stride_list[i].stride = LCM / stride_list[i].proc->share;
      //cprintf("[%d]: %d ", stride_list[i].proc->share, stride_list[i].stride);
    }
    //cprintf("\n");
  }

  // Put all MLFQ in one Stride process;
  stride_list[num_stride].proc = 0;
  stride_list[num_stride].pass = 0;
  stride_list[num_stride].stride = LCM / (100 - total_share); // Sharesize of "one Stride process"


} 

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  p->num_thread  = 0; // 0 threads exist
  p->proc_true = 1;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;
  p->mlfqlev = 2;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  // proc data -> deal with this
  np->state = RUNNABLE;
  np->mlfqlev     = 2; // Highest priority if new
  //np->allotment = 50000000; FOR MLFQ + STRIDE
  np->allotment   = 200000000; // Also allotment for highest priority
  np->stampin     = 0;
  np->stampout    = 0;
  np->hitzone     = 0;
  np->strike      = 0;
  np->is_stride   = 0; // Cannot be stride process if newly forked
  np->share       = 0;

  // basic variables for thread
  np->t_history = 0; // no threads made yet
  //np->num_thread  = 0; // 0 threads exist
  np->active_thread = 0; // this might be confusing but this should be done
  np->t_chan = -1; // sleeping on nothing
  np->proc_true = 1; // mother proc is running! not thread

  //np->pass        = 0;
  //np->stride      = 0;

  release(&ptable.lock);
  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
	if(p->is_stride) {
	  p->is_stride = 0;
	  set_stride();
	}

        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  int procrun;
  int passthru;
  int stride_index;
  uint empty_mlfq;
  local_ticks = 0;

  c->proc = 0;

  // Scheduler booting
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == RUNNING || p->state == RUNNABLE) {
      p->mlfqlev = 2;
      //p->allotment = 50000000; FOR MLFQ + STRIDe
      p->allotment = 200000000;
    }
    else {
      p->mlfqlev = -2; // -1 for stride
      p->allotment = 0; // 0 time for nonexisting process
    }

    p->hitzone = 0;
    p->strike = 0;
    p->stampin = 0;
    p->stampout = 0;

    p->is_stride = 0;
    p->share = 0;
  }

  // No one except GOD can set Stride process
  // before even the scheduler has run
  num_stride = 0;
  set_stride();
  
  for(;;){
    // Enable interrupts on this processor.
    // IF pass through gets heavy, move this
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);

    // passes through number of stride lists
    passthru = stride_list[0].pass;
    stride_index = 0;
    if(num_stride > 0) {
      for(int i = 0; i <= num_stride; i++) {
        if(stride_list[i].pass <= passthru) {
	  passthru = stride_list[i].pass;
	  stride_index = i;
        }
      }

      if(passthru > 0) {
	if(passthru == stride_list[0].pass) {
	  for(int i = 0; i <= num_stride; i++)
	    stride_list[i].pass = 0;
	}
      }
    }



    if(stride_index == num_stride) { // Last one is MLFQ SET
      // MLFQ PART
      if(num_stride > 0) 
	empty_mlfq = ticks;
      for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
        if(p->state != RUNNABLE) {
	  if(p == &ptable.proc[NPROC]){
	    if(num_stride > 0)
	      while(ticks == empty_mlfq);
	  }
          continue;
	}

        // Includes trashing stride processes too
        if(p->mlfqlev != maxlev())
	  continue;

	/*
	if(local_ticks == 0) {
        switch(maxlev()){
	  case 2:
	    // lapic[0x0380/4] = 10000000; FOR MLFQ + STRIDE
	    // lapic[0x0380/4] = 50000000; // 1 tick set manually
	    local_ticks = 5;
	    break;
	  case 1:
	    // if(num_stride == 0) lapic[0x0380/4] = 20000000; FOR MLFQ + STRIDE
	    // if(num_stride == 0) lapic[0x0380/4] = 100000000; // 2 tick set manually
	    local_ticks = 10;
	    break;
	  case 0:
	    // if(num_stride == 0) lapic[0x0380/4] = 40000000; FOR MLFQ + STRIDe
	    // if(num_stride == 0) lapic[0x0380/4] = 200000000; // 4 tick set manually
	    local_ticks = 20;
	    break;
	  default:
	    // lapic[0x0380/4] = 10000000; FOR MLFQ + STRIDE
	    // lapic[0x0380/4] = 50000000; // This is when this mlfq is in stride
	    local_ticks = 5;
	    break;
        }
	}*/
        // if(ticks % 100 == 0) cprintf("MLFQ\n");

        p->stampin = stamp();

        // Switch to chosen process.  It is the process's job
        // to release ptable.lock and then reacquire it
        // before jumping back to us.
        c->proc = p;
        switchuvm(p);
	p->state = RUNNING; // Where process becomes RUNNING

	swtch(&(c->scheduler), p->context);

	if(p->num_thread) {
	  cli();
	  cprintf("to proc 0\n");
	  swtch(&(c->scheduler), p->t_context[0]);
	  panic("for debug");
	}

	switchkvm();

        p->stampout = stamp();

        procrun = (p->stampout - p->stampin)/2;
        
	if(p->mlfqlev != 0) p->allotment -= procrun; // No need to deal with allotment in level 0
	
	if(p->allotment < 0 && p->mlfqlev == 2) {
	  p->mlfqlev = 1;
	  //p->allotment = 100000000; FOR MLFQ + STRIDE
	  p->allotment = 400000000;
        }
        if(p->allotment < 0 && p->mlfqlev == 1) {
	  p->mlfqlev = 0;
	  p->allotment = 0; // An Infinity
        }

        // ULTIMATE DEBUGGER
        // cprintf("[IN =%d OUT =%d, ELAPSED = %d, LEFT = %d, LEVEL = %d]\n", p->stampin, p->stampout, procrun, p->allotment, p->mlfqlev);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;

        // NEW STRIDE PROCESS
        // NOT THAT BEAUTIFUL
        // COULD POSSIBLY RESULT ERROR
        if(p->is_stride) break;

      } // for-moon inside mlfq
    } else { // Stride process begins
      // procrun = 50000000; // Stride is for 1 tick by default
      // procrun = 10000000; FOR MLFQ + STRIDE
      // lapic[0x0380/4] = procrun;
      if(local_ticks == 0) local_ticks = 5;
      p = stride_list[stride_index].proc;

      // don't use continue here
      if(p->state == RUNNABLE) {
        c->proc = p;
	switchuvm(p);
        p->state = RUNNING;
	swtch(&(c->scheduler), p->context);
	switchkvm();
	c->proc = 0;
      }
    }

    __sync_synchronize();

    if(num_stride > 0) stride_list[stride_index].pass += stride_list[stride_index].stride;
    release(&ptable.lock);
  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk) {
  struct proc *p = myproc();

  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;
  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;
  //struct proc *pruning;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      //if(p->is_stride) {
      //  p->is_stride = 0;
      //  set_stride();
      //}
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

int
thread_create(thread_t *thread, void *(*start_routine)(void *), void *arg)
{
  cli();
  //cprintf("create %d with %d\n", myproc()->num_thread, (int)arg);
  struct proc *p = myproc();
  uint sz, sp;
  //uint stack[2];
  uint ustack[2];
  char *spk;
  pde_t *pgdir = p->pgdir; // update curproc->pgdir

  /*
  // keep track
  if(p->t_history == 0) {
    sz = PGROUNDUP(p->sz);

    // also init thread-related proc variables HERE
    for(int i = 0; i < NTHREAD; i++){
      p->t_state[i] = UNUSED;
    }
  } else {
    sz = PGROUNDUP(p->t_sz_accumulative);
  }
  */

  sz = PGROUNDUP(p->sz);

  *thread = p->t_history;
  //cprintf("create %d with %d\n", (uint)*thread, (uint)arg); // arg well-fed

  ////////////////
  //
  // THREAD KERNEL
  //
  ////////////////

  if((p->t_kstack[p->t_history] = kalloc()) == 0){
    panic("thread kalloc fail");
    return -1;
  }

  spk = p->t_kstack[p->t_history] + KSTACKSIZE;

  spk -= sizeof *p->t_tf[p->t_history];
  p->t_tf[p->t_history] = (struct trapframe*)spk;
  *p->t_tf[p->t_history] = *p->tf;
  //memset(p->t_tf[p->t_history], 0, sizeof *p->t_tf[p->t_history]);

  spk -= 4;
  *(uint*)spk = (uint)trapret;

  spk -= sizeof *p->t_context[p->t_history];
  p->t_context[p->t_history] = (struct context*)spk;
  memset(p->t_context[p->t_history], 0, sizeof *p->t_context[p->t_history]);
  p->t_context[p->t_history]->eip = (uint)forkret;
  
  ////////////////
  //
  // THREAD STACK
  //
  ////////////////  

  // two pages at the page boundary
  if((sz = allocuvm(pgdir, sz, sz + 2*PGSIZE)) == 0) {
    panic("thread stack alloc fail");
    return -1; // failed to allocate
  }
  //cprintf("sz : %d\n", sz);
  clearpteu(pgdir, (char*)(sz - 2*PGSIZE));
  
  /*
  sp = sz;
  sp -= 4;
  if(copyout(pgdir, sp, arg, sizeof(uint)) < 0)
    return -1;

  ustack[3] = sp;
  ustack[4] = 0;
  ustack[0] = 0xffffffff;
  ustack[1] = 0;
  ustack[2] = sp - 4;
  sp -= 4 * sizeof(uint);
  if(copyout(pgdir, sp, ustack, 4 * sizeof(uint)))
    return -1;
  */

  sp = sz;
  sp -= 2*sizeof(uint);
  ustack[0] = 0xffffffff;
  ustack[1] = (uint)arg;
  if(copyout(pgdir, sp, ustack, 2*sizeof(uint)))
    return -1;

  /*
  spk = sp;
  sp -= sizeof *p->t_tf[p->t_history];
  p->t_tf[p->t_history] = (struct trapframe*)sp;
  *p->t_tf[p->t_history] = *p->tf;

  sp -= 4;
  *(uint*)sp = (uint)trapret;
  sp -= sizeof *p->t_context[p->t_history];
  p->t_context[p->t_history] = (struct context*)sp;
  memset(p->t_context[p->t_history], 0, sizeof *p->t_context[p->t_history]);
  p->t_context[p->t_history]->eip = (uint)forkret;
  */

  /*
  if((p->t_ustack[p->t_history] = kalloc()) == 0){
    panic("thread ualloc fail");
    return -1;
  }

  spk = p->t_ustack[p->t_history] + KSTACKSIZE;

  spk -= 4;
  *(uint*)spk = (uint)arg;
  spk -= 4;
  *(uint*)spk = 0xffffffff;
  cprintf("spk : %d\n", (uint)spk);
  */

  //void *stack = malloc(PGSIZE*2);

  //sz = (uint)stack + PGSIZE;

  //sp = sz;
  //sp = (sp - (strlen(arg) + 1)) & ~3;
  //if(copyout(pgdir, sp, arg, strlen(arg) + 1) < 0)
  //  return -1;

  //ustack[0] = 0xffffffff;
  //ustack[1] = 0;
  //ustack[2] = sp - 4;
  //ustack[3] = sp;
  //ustack[4] = 0;

  //sp -= 16;
  //if(copyout(pgdir, sp, ustack, 16) < 0)
  //  return -1;

  //__sync_synchronize();

  /*
  sp = sz;
  sp = (sp - (strlen(arg) + 1)) & ~3;
  if(copyout(pgdir, sp, arg, strlen(arg) + 1) < 0) {
    panic("failed to copy values");
    return -1; // failed to push string to the ustack
  }
  stack[3 + 0] = sp;
  stack[3 + 1] = 0;

  stack[0] = 0xffffffff; // fake return
  stack[1] = 0; // only one argument
  stack[2] = sp - (0 + 1) * 4; // arg pointer

  sp -= (3 + 0 + 1) * 4;
  if(copyout(pgdir, sp, stack, (3 + 0 + 1) * 4) < 0) {
    panic("failed to copy whole");
    return -1; // failed to copy ustack to the location
  }
  */

  ////////////////
  //
  // COMMIT IMAGE
  //
  ////////////////
  
  //curproc->thread_kstack[curproc->thread_history] = sz;
  //__sync_synchronize();
  //p->t_tf[p->t_history]->eax = 0;
  
  p->t_tf[p->t_history]->eip = (uint)start_routine;
  //p->t_tf[p->t_history]->ebp = sp;
  p->t_tf[p->t_history]->esp = sp;
  //cprintf("SZ : %d, sz : %d , sp : %d\n", p->sz, sz, sp);
  //p->t_first[p->t_history] = 1;
 
  //switchuvm(p);
  //curproc->thread_awesome[curproc->thread_history] = awesome;

  // total control
  p->t_state[p->t_history] = RUNNABLE;
  p->sz = sz; // to count in thread kstack
  //p->active_thread = p->t_history;
  p->t_history++;
  p->num_thread++;

  //cprintf("done\n");
  sti();
  return 0; // success
}

void
thread_exit(void *retval)
{
  cli();
  struct proc *curproc = myproc();
  //cprintf("                                              exit %d\n", curproc->active_thread);

  // in thread
  curproc->dyingmessage[curproc->active_thread] = retval;
  
  cprintf("exit with retval = %d\n", (int)retval);
  
  curproc->t_state[curproc->active_thread] = ZOMBIE;
  if(curproc->t_chan == curproc->active_thread)
    curproc->t_chan = -1;
  //curproc->active_thread++;
  curproc->num_thread--;
  //acquire(&ptable.lock);
  sti();
  yield();
}

int
thread_join(thread_t thread, void **retval)
{
  //cprintf("join called\n");
  struct proc *curproc = myproc();

  // in proc itself, always
  if(thread < 0 || thread > NTHREAD) {
    cprintf("\n\nOUTOFRANGE\n\n");
    return -1; // out of range
  }

  // wait if the thread is not finished
  while(curproc->t_state[thread] != ZOMBIE)
  {
    cprintf("join wait %d\n", thread);
    curproc->t_chan = thread;
    yield();
  }

  // clean up the mess
  curproc->t_state[thread] = UNUSED;
  deallocuvm(curproc->pgdir, PGROUNDUP(curproc->sz) + (thread + 1) * 3 * PGSIZE, PGROUNDUP(curproc->sz) + thread * 3 * PGSIZE);
  kfree(curproc->t_kstack[thread]);
  //kfree(curproc->t_ustack[thread]);

  // for thread stress test
  if(curproc->num_thread == 0)
    curproc->t_history = 0;

  // set the value
  cprintf("myproc()->dyingmessage[%d] = %d\n", thread, (uint)myproc()->dyingmessage[thread]);
  *retval = myproc()->dyingmessage[thread];
  return 0;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

int
getppid(void)
{
    return myproc()->parent->pid;
}
