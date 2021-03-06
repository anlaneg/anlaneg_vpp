/*
 * Copyright (c) 2015 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef included_vlib_threads_h
#define included_vlib_threads_h

#include <vlib/main.h>
#include <linux/sched.h>

extern vlib_main_t **vlib_mains;

void vlib_set_thread_name (char *name);

/* arg is actually a vlib__thread_t * */
typedef void (vlib_thread_function_t) (void *arg);

typedef struct vlib_thread_registration_
{
  /* constructor generated list of thread registrations */
  struct vlib_thread_registration_ *next;

  /* config parameters */
  char *name;
  char *short_name;
  vlib_thread_function_t *function;//注册的线程函数
  uword mheap_size;
  int fixed_count;
  u32 count;
  int no_data_structure_clone;
  u32 frame_queue_nelts;

  /* All threads of this type run on pthreads */
  int use_pthreads;//是否使用pthreads
  u32 first_index;
  uword *coremask;//使用的coremask
} vlib_thread_registration_t;

/*
 * Frames have their cpu / vlib_main_t index in the low-order N bits
 * Make VLIB_MAX_CPUS a power-of-two, please...
 */

#ifndef VLIB_MAX_CPUS
#define VLIB_MAX_CPUS 256
#endif

#if VLIB_MAX_CPUS > CLIB_MAX_MHEAPS
#error Please increase number of per-cpu mheaps
#endif

#define VLIB_CPU_MASK (VLIB_MAX_CPUS - 1)	/* 0x3f, max */
#define VLIB_OFFSET_MASK (~VLIB_CPU_MASK)

#define VLIB_LOG2_THREAD_STACK_SIZE (21)
#define VLIB_THREAD_STACK_SIZE (1<<VLIB_LOG2_THREAD_STACK_SIZE)

typedef enum
{
  VLIB_FRAME_QUEUE_ELT_DISPATCH_FRAME,
} vlib_frame_queue_msg_type_t;

typedef struct
{
  CLIB_CACHE_LINE_ALIGN_MARK (cacheline0);
  volatile u32 valid;//元素是否有效
  u32 msg_type;//消息类型（常等于VLIB_FRAME_QUEUE_ELT_DISPATCH_FRAME）
  u32 n_vectors;//指明存放的buffer_index数目
  u32 last_n_vectors;

  /* 256 * 4 = 1024 bytes, even mult of cache line size */
  u32 buffer_index[VLIB_FRAME_SIZE];
}
vlib_frame_queue_elt_t;

typedef struct
{
  /* First cache line */
  CLIB_CACHE_LINE_ALIGN_MARK (cacheline0);
  volatile u32 *wait_at_barrier;
  volatile u32 *workers_at_barrier;

  /* Second Cache Line */
    CLIB_CACHE_LINE_ALIGN_MARK (cacheline1);
  void *thread_mheap;
  u8 *thread_stack;//线程对应的栈空间
  void (*thread_function) (void *);
  void *thread_function_arg;
  i64 recursion_level;
  elog_track_t elog_track;
  u32 instance_id;
  vlib_thread_registration_t *registration;
  u8 *name;
  u64 barrier_sync_count;
  u8 barrier_elog_enabled;
  const char *barrier_caller;
  const char *barrier_context;
  volatile u32 *node_reforks_required;

  long lwp;//线程的标识符，通过gettid系统调用获得
  int cpu_id;//线程使用的cpu
  int core_id;//cpu_id对应的core_id
  int socket_id;//cpu_id对应的socket_id
  pthread_t thread_id;//当前工作线程线程号
} vlib_worker_thread_t;

extern vlib_worker_thread_t *vlib_worker_threads;

typedef struct
{
  /* enqueue side */
  CLIB_CACHE_LINE_ALIGN_MARK (cacheline0);
  volatile u64 tail;//写者头指针（这个值是一直增加的）
  u64 enqueues;
  u64 enqueue_ticks;
  u64 enqueue_vectors;
  u32 enqueue_full_events;//记录队列满事件发生的次数

  /* dequeue side */
    CLIB_CACHE_LINE_ALIGN_MARK (cacheline1);
  volatile u64 head;//读者头指针(这个值是一直增加的）
  u64 dequeues;
  u64 dequeue_ticks;
  u64 dequeue_vectors;
  u64 trace;
  u64 vector_threshold;

  /* dequeue hint to enqueue side */
    CLIB_CACHE_LINE_ALIGN_MARK (cacheline2);
  volatile u64 head_hint;//上次退出出队函数时记录的head

  /* read-only, constant, shared */
    CLIB_CACHE_LINE_ALIGN_MARK (cacheline3);
  vlib_frame_queue_elt_t *elts;//队列指针，指向缓冲区（vector)
  u32 nelts;//队列大小
}
vlib_frame_queue_t;

typedef struct
{
  //按thread线程存储申请的frame queue element(一次性填不满）
  vlib_frame_queue_elt_t **handoff_queue_elt_by_thread_index;
  //按thread线程记录拥挤的队列
  vlib_frame_queue_t **congested_handoff_queue_by_thread_index;
} vlib_frame_queue_per_thread_data_t;

//从属于node的frame队列
typedef struct
{
  u32 node_index;//所属的vpp node
  u32 frame_queue_nelts;//队列数目
  u32 queue_hi_thresh;//指出队列拥挤标准（高于此值则认为拥挤）

  //保存多个frame_queue的指针数组,每个其它vlib main一个指针数组（共n-1)
  //每个指针数组长度为vlib main个（共N个)
  vlib_frame_queue_t **vlib_frame_queues;
  //保存多个thread_data，每个线程一个thread_data
  vlib_frame_queue_per_thread_data_t *per_thread_data;

  /* for frame queue tracing */
  frame_queue_trace_t *frame_queue_traces;
  frame_queue_nelt_counter_t *frame_queue_histogram;
} vlib_frame_queue_main_t;

typedef struct
{
  uword node_index;
  uword type_opaque;
  uword data;
} vlib_process_signal_event_mt_args_t;

/* Called early, in thread 0's context */
clib_error_t *vlib_thread_init (vlib_main_t * vm);

int vlib_frame_queue_enqueue (vlib_main_t * vm, u32 node_runtime_index,
			      u32 frame_queue_index, vlib_frame_t * frame,
			      vlib_frame_queue_msg_type_t type);

int
vlib_frame_queue_dequeue (vlib_main_t * vm, vlib_frame_queue_main_t * fqm);

void vlib_worker_thread_node_runtime_update (void);

void vlib_create_worker_threads (vlib_main_t * vm, int n,
				 void (*thread_function) (void *));

void vlib_worker_thread_init (vlib_worker_thread_t * w);
u32 vlib_frame_queue_main_init (u32 node_index, u32 frame_queue_nelts);

/* Check for a barrier sync request every 30ms */
#define BARRIER_SYNC_DELAY (0.030000)

#if CLIB_DEBUG > 0
/* long barrier timeout, for gdb... */
#define BARRIER_SYNC_TIMEOUT (600.1)
#else
#define BARRIER_SYNC_TIMEOUT (1.0)
#endif

#define vlib_worker_thread_barrier_sync(X) {vlib_worker_thread_barrier_sync_int(X, __FUNCTION__);}

void vlib_worker_thread_barrier_sync_int (vlib_main_t * vm,
					  const char *func_name);
void vlib_worker_thread_barrier_release (vlib_main_t * vm);
void vlib_worker_thread_node_refork (void);

//取当前线程的线程索引
static_always_inline uword
vlib_get_thread_index (void)
{
  return __os_thread_index;
}

always_inline void
vlib_smp_unsafe_warning (void)
{
  if (CLIB_DEBUG > 0)
    {
      if (vlib_get_thread_index ())
	fformat (stderr, "%s: SMP unsafe warning...\n", __FUNCTION__);
    }
}

typedef enum
{
  VLIB_WORKER_THREAD_FORK_FIXUP_ILLEGAL = 0,
  VLIB_WORKER_THREAD_FORK_FIXUP_NEW_SW_IF_INDEX,
} vlib_fork_fixup_t;

void vlib_worker_thread_fork_fixup (vlib_fork_fixup_t which);

//采用this_vlib_main遍历每个vlib_mains数组，针对每个元素，调用body
#define foreach_vlib_main(body)                         \
do {                                                    \
  vlib_main_t ** __vlib_mains = 0, *this_vlib_main;     \
  int ii;                                               \
                                                        \
  /*遍历vlib_mains,将每一个元素添加至__vlib_mains中*/\
  for (ii = 0; ii < vec_len (vlib_mains); ii++)         \
    {                                                   \
      this_vlib_main = vlib_mains[ii];                  \
      ASSERT (ii == 0 ||                                \
	      this_vlib_main->parked_at_barrier == 1);  \
      if (this_vlib_main)                               \
        vec_add1 (__vlib_mains, this_vlib_main);        \
    }                                                   \
                                                        \
  /*遍历__vlib_main中的每一个元素，通过他调用body*/\
  for (ii = 0; ii < vec_len (__vlib_mains); ii++)       \
    {                                                   \
      this_vlib_main = __vlib_mains[ii];                \
      /* body uses this_vlib_main... */                 \
      (body);                                           \
    }                                                   \
  vec_free (__vlib_mains);                              \
} while (0);

#define foreach_sched_policy \
  _(SCHED_OTHER, OTHER, "other") \
  _(SCHED_BATCH, BATCH, "batch") \
  _(SCHED_IDLE, IDLE, "idle")   \
  _(SCHED_FIFO, FIFO, "fifo")   \
  _(SCHED_RR, RR, "rr")

typedef enum
{
#define _(v,f,s) SCHED_POLICY_##f = v,
  foreach_sched_policy
#undef _
    SCHED_POLICY_N,
} sched_policy_t;

typedef struct
{
    //通过此回调，在指定线程上运行w工作（线程创建后通过此回调来执行工作）
  clib_error_t *(*vlib_launch_thread_cb) (void *fp, vlib_worker_thread_t * w,
					  unsigned cpu_id);
  //通过此回调，将指定线程绑定在指定cpu上
  clib_error_t *(*vlib_thread_set_lcore_cb) (u32 thread, u16 cpu);
} vlib_thread_callbacks_t;

typedef struct
{
  /* Link list of registrations, built by constructors */
  //注册thread对应的初始化，例如创建新的线程
  vlib_thread_registration_t *next;

  /* Vector of registrations, w/ non-data-structure clones at the top */
  vlib_thread_registration_t **registrations;//收集所有的thread_registration

  uword *thread_registrations_by_name;//名称到vlib_thread_registration_t的索引hashtable

  vlib_worker_thread_t *worker_threads;

  /*
   * Launch all threads as pthreads,
   * not eal_rte_launch (strict affinity) threads
   */
  int use_pthreads;//指明是否使用pthread库

  /* Number of vlib_main / vnet_main clones */
  u32 n_vlib_mains;//vm数量

  /* Number of thread stacks to create */
  u32 n_thread_stacks;

  /* Number of pthreads */
  u32 n_pthreads;//pthread线程数

  /* Number of threads */
  u32 n_threads;//非pthread线程数

  /* Number of cores to skip, must match the core mask */
  u32 skip_cores;//需要跳过的core数目，将自第一个有效cpu开始跳过

  /* Thread prefix name */
  u8 *thread_prefix;//线程名称前缀

  /* main thread lcore */
  u32 main_lcore;//主线程占用的core

  /* Bitmap of available CPU cores */
  uword *cpu_core_bitmap;

  /* Bitmap of available CPU sockets (NUMA nodes) */
  uword *cpu_socket_bitmap;//在线node的bitmap

  /* Worker handoff queues */
  //工作线程之间handoff型队列（N个）
  vlib_frame_queue_main_t *frame_queue_mains;

  /* worker thread initialization barrier */
  volatile u32 worker_thread_release;

  /* scheduling policy */
  u32 sched_policy;//线程调度策略

  /* scheduling policy priority */
  u32 sched_priority;

  /* callbacks */
  vlib_thread_callbacks_t cb;
  int extern_thread_mgmt;
} vlib_thread_main_t;

extern vlib_thread_main_t vlib_thread_main;

#include <vlib/global_funcs.h>

//初始化x,并将x注册到vlib_thread_main对应的链表上
#define VLIB_REGISTER_THREAD(x,...)                     \
  __VA_ARGS__ vlib_thread_registration_t x;             \
  /*声明定义线程注册函数*/\
static void __vlib_add_thread_registration_##x (void)   \
  __attribute__((__constructor__)) ;                    \
static void __vlib_add_thread_registration_##x (void)   \
{                                                       \
  vlib_thread_main_t * tm = &vlib_thread_main;          \
  /*将函数注册在next链上*/\
  x.next = tm->next;                                    \
  tm->next = &x;                                        \
}                                                       \
    /*声明定义线程解注册函数*/\
static void __vlib_rm_thread_registration_##x (void)    \
  __attribute__((__destructor__)) ;                     \
static void __vlib_rm_thread_registration_##x (void)    \
{                                                       \
  vlib_thread_main_t * tm = &vlib_thread_main;          \
  VLIB_REMOVE_FROM_LINKED_LIST (tm->next, &x, next);    \
}                                                       \
/*初始化注册线程*/\
__VA_ARGS__ vlib_thread_registration_t x

always_inline u32
vlib_num_workers ()
{
  return vlib_thread_main.n_vlib_mains - 1;
}

always_inline u32
vlib_get_worker_thread_index (u32 worker_index)
{
  return worker_index + 1;
}

always_inline u32
vlib_get_worker_index (u32 thread_index)
{
  return thread_index - 1;
}

always_inline u32
vlib_get_current_worker_index ()
{
  return vlib_get_thread_index () - 1;
}

static inline void
vlib_worker_thread_barrier_check (void)
{
  if (PREDICT_FALSE (*vlib_worker_threads->wait_at_barrier))
    {
      //需要等待一个barrier

      vlib_main_t *vm = vlib_get_main ();

      //取当前线程index
      u32 thread_index = vm->thread_index;
      f64 t = vlib_time_now (vm);

      //如果相应的elog被打开，则构造当前线程对应的elog
      if (PREDICT_FALSE (vlib_worker_threads->barrier_elog_enabled))
	{
	  vlib_worker_thread_t *w = vlib_worker_threads + thread_index;
	  /* *INDENT-OFF* */
	  ELOG_TYPE_DECLARE (e) = {
	    .format = "barrier-wait-thread-%d",
	    .format_args = "i4",
	  };
	  /* *INDENT-ON* */

	  struct
	  {
	    u32 thread_index;
	  } __clib_packed *ed;

	  //填充elog数据
	  ed = ELOG_TRACK_DATA (&vlib_global_main.elog_main, e,
				w->elog_track);
	  ed->thread_index = thread_index;
	}

      //使workers_at_barrier+=1
      clib_atomic_fetch_add (vlib_worker_threads->workers_at_barrier, 1);
      if (CLIB_DEBUG > 0)
	{
	  vm = vlib_get_main ();
	  vm->parked_at_barrier = 1;
	}

      //等待wait_at_barrier为0后开始运行
      while (*vlib_worker_threads->wait_at_barrier)
	;

      /*
       * Recompute the offset from thread-0 time.
       * Note that vlib_time_now adds vm->time_offset, so
       * clear it first. Save the resulting idea of "now", to
       * see how well we're doing. See show_clock_command_fn(...)
       */
      {
	f64 now;
	vm->time_offset = 0.0;
	now = vlib_time_now (vm);
	vm->time_offset = vlib_global_main.time_last_barrier_release - now;
	vm->time_last_barrier_release = vlib_time_now (vm);
      }

      if (CLIB_DEBUG > 0)
	vm->parked_at_barrier = 0;
      clib_atomic_fetch_add (vlib_worker_threads->workers_at_barrier, -1);

      if (PREDICT_FALSE (*vlib_worker_threads->node_reforks_required))
	{
      //构造log
	  if (PREDICT_FALSE (vlib_worker_threads->barrier_elog_enabled))
	    {
	      t = vlib_time_now (vm) - t;
	      vlib_worker_thread_t *w = vlib_worker_threads + thread_index;
              /* *INDENT-OFF* */
              ELOG_TYPE_DECLARE (e) = {
                .format = "barrier-refork-thread-%d",
                .format_args = "i4",
              };
              /* *INDENT-ON* */

	      struct
	      {
		u32 thread_index;
	      } __clib_packed *ed;

	      ed = ELOG_TRACK_DATA (&vlib_global_main.elog_main, e,
				    w->elog_track);
	      ed->thread_index = thread_index;
	    }

	  vlib_worker_thread_node_refork ();
	  clib_atomic_fetch_add (vlib_worker_threads->node_reforks_required,
				 -1);
	  while (*vlib_worker_threads->node_reforks_required)
	    ;
	}
      if (PREDICT_FALSE (vlib_worker_threads->barrier_elog_enabled))
	{
	  t = vlib_time_now (vm) - t;
	  vlib_worker_thread_t *w = vlib_worker_threads + thread_index;
	  /* *INDENT-OFF* */
	  ELOG_TYPE_DECLARE (e) = {
	    .format = "barrier-released-thread-%d: %dus",
	    .format_args = "i4i4",
	  };
	  /* *INDENT-ON* */

	  struct
	  {
	    u32 thread_index;
	    u32 duration;
	  } __clib_packed *ed;

	  ed = ELOG_TRACK_DATA (&vlib_global_main.elog_main, e,
				w->elog_track);
	  ed->thread_index = thread_index;
	  ed->duration = (int) (1000000.0 * t);
	}
    }
}

always_inline vlib_main_t *
vlib_get_worker_vlib_main (u32 worker_index)
{
  vlib_main_t *vm;
  vlib_thread_main_t *tm = &vlib_thread_main;
  ASSERT (worker_index < tm->n_vlib_mains - 1);
  vm = vlib_mains[worker_index + 1];
  ASSERT (vm);
  return vm;
}

static inline u8
vlib_thread_is_main_w_barrier (void)
{
  return (!vlib_num_workers ()
	  || ((vlib_get_thread_index () == 0
	       && vlib_worker_threads->wait_at_barrier[0])));
}

static inline void
vlib_put_frame_queue_elt (vlib_frame_queue_elt_t * hf)
{
  CLIB_MEMORY_BARRIER ();
  hf->valid = 1;
}

//获取一个空闲的队列节点，准备写入
static inline vlib_frame_queue_elt_t *
vlib_get_frame_queue_elt (u32 frame_queue_index, u32 index)
{
  vlib_frame_queue_t *fq;
  vlib_frame_queue_elt_t *elt;
  vlib_thread_main_t *tm = &vlib_thread_main;
  vlib_frame_queue_main_t *fqm =
    vec_elt_at_index (tm->frame_queue_mains, frame_queue_index);
  u64 new_tail;

  //取出index指明的frame queues
  fq = fqm->vlib_frame_queues[index];
  ASSERT (fq);

  //向后移动tail变量
  new_tail = clib_atomic_add_fetch (&fq->tail, 1);

  /* Wait until a ring slot is available */
  //如果当前队列为满，不能写入，等待
  while (new_tail >= fq->head_hint + fq->nelts)
    vlib_worker_thread_barrier_check ();

  //取出此元素
  elt = fq->elts + (new_tail & (fq->nelts - 1));

  /* this would be very bad... */
  //出队的还未来得及拿走数据，等待
  while (elt->valid)
    ;

  //将元素初始化，准备写入
  elt->msg_type = VLIB_FRAME_QUEUE_ELT_DISPATCH_FRAME;
  elt->last_n_vectors = elt->n_vectors = 0;

  return elt;
}

//frame queue是否拥挤，如果拥挤，则将其加入到handoff_queue_by_worker_index中
static inline vlib_frame_queue_t *
is_vlib_frame_queue_congested (u32 frame_queue_index/*队列管理索引*/,
			       u32 index/*线程索引*/,
			       u32 queue_hi_thresh,
			       vlib_frame_queue_t **
			       handoff_queue_by_worker_index)
{
  vlib_frame_queue_t *fq;
  vlib_thread_main_t *tm = &vlib_thread_main;

  //按索引取相应队列管理
  vlib_frame_queue_main_t *fqm =
    vec_elt_at_index (tm->frame_queue_mains, frame_queue_index);

  fq = handoff_queue_by_worker_index[index];
  if (fq != (vlib_frame_queue_t *) (~0))
    return fq;

  //取线程对应的frame队列
  fq = fqm->vlib_frame_queues[index];
  ASSERT (fq);

  //队列的写者头与上次退出出队时的fp->head_hint间差值已超过queue_hi_thresh，说明队列将满或已满
  if (PREDICT_FALSE (fq->tail >= (fq->head_hint + queue_hi_thresh)))
    {
      /* a valid entry in the array will indicate the queue has reached
       * the specified threshold and is congested
       */
      //记录已满队列
      handoff_queue_by_worker_index[index] = fq;
      fq->enqueue_full_events++;
      return fq;
    }

  return NULL;
}

//如果handoff....中有元素可使用，则使用，否则获取一个并设置给handoff...并使用
static inline vlib_frame_queue_elt_t *
vlib_get_worker_handoff_queue_elt (u32 frame_queue_index,
				   u32 vlib_worker_index,
				   vlib_frame_queue_elt_t **
				   handoff_queue_elt_by_worker_index)
{
  vlib_frame_queue_elt_t *elt;

  if (handoff_queue_elt_by_worker_index[vlib_worker_index])
    return handoff_queue_elt_by_worker_index[vlib_worker_index];

  //取一个节点
  elt = vlib_get_frame_queue_elt (frame_queue_index, vlib_worker_index);

  //设置取出的节点
  handoff_queue_elt_by_worker_index[vlib_worker_index] = elt;

  return elt;
}

u8 *vlib_thread_stack_init (uword thread_index);
int vlib_thread_cb_register (struct vlib_main_t *vm,
			     vlib_thread_callbacks_t * cb);
extern void *rpc_call_main_thread_cb_fn;

void
vlib_process_signal_event_mt_helper (vlib_process_signal_event_mt_args_t *
				     args);
void vlib_rpc_call_main_thread (void *function, u8 * args, u32 size);

u32 elog_global_id_for_msg_name (const char *msg_name);
#endif /* included_vlib_threads_h */

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
