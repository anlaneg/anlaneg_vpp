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
/*
 * main.c: main vector processing loop
 *
 * Copyright (c) 2008 Eliot Dresselhaus
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 *  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 *  NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 *  LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 *  OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 *  WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <math.h>
#include <vppinfra/format.h>
#include <vlib/vlib.h>
#include <vlib/threads.h>
#include <vppinfra/tw_timer_1t_3w_1024sl_ov.h>

#include <vlib/unix/unix.h>
#include <vlib/unix/cj.h>

CJ_GLOBAL_LOG_PROTOTYPE;

/* Actually allocate a few extra slots of vector data to support
   speculative vector enqueues which overflow vector data in next frame. */
#define VLIB_FRAME_SIZE_ALLOC (VLIB_FRAME_SIZE + 4)

u32 wraps;

//获取frame需要的总字段
always_inline u32
vlib_frame_bytes (u32 n_scalar_bytes, u32 n_vector_bytes)
{
  u32 n_bytes;

  /* Make room for vlib_frame_t plus scalar arguments. */
  //对齐n_scalar_bytes所需要的字节
  n_bytes = vlib_frame_vector_byte_offset (n_scalar_bytes);

  /* Make room for vector arguments.
     Allocate a few extra slots of vector data to support
     speculative vector enqueues which overflow vector data in next frame. */
#define VLIB_FRAME_SIZE_EXTRA 4
  //存放vector参数
  n_bytes += (VLIB_FRAME_SIZE + VLIB_FRAME_SIZE_EXTRA) * n_vector_bytes;

  /* Magic number is first 32bit number after vector data.
     Used to make sure that vector data is never overrun. */
#define VLIB_FRAME_MAGIC (0xabadc0ed)
  //存放magic
  n_bytes += sizeof (u32);

  /* Pad to cache line. */
  //使cacheline对齐
  n_bytes = round_pow2 (n_bytes, CLIB_CACHE_LINE_BYTES);

  return n_bytes;
}

//获取frame中magic的字段
always_inline u32 *
vlib_frame_find_magic (vlib_frame_t * f, vlib_node_t * node)
{
  void *p = f;

  //加上vlib_frame_t结构体大小及node->scalar_size并对齐后大小
  p += vlib_frame_vector_byte_offset (node->scalar_size);

  //加上vector_size个(vlib_frame_size+vlib_frame_size_extra)
  p += (VLIB_FRAME_SIZE + VLIB_FRAME_SIZE_EXTRA) * node->vector_size;

  return p;
}

static inline vlib_frame_size_t *
get_frame_size_info (vlib_node_main_t * nm,
		     u32 n_scalar_bytes, u32 n_vector_bytes)
{
  //将n_scalar_bytes,n_vector_bytes合成key,在nm->frame_size_hash中查找
#ifdef VLIB_SUPPORTS_ARBITRARY_SCALAR_SIZES
  uword key = (n_scalar_bytes << 16) | n_vector_bytes;
  uword *p, i;

  p = hash_get (nm->frame_size_hash, key);
  if (p)
    i = p[0];
  else
    {
      //未在nm->frame_size_hash中查找到key,创建相应的key
      //取最后一个frame_size
      i = vec_len (nm->frame_sizes);
      vec_validate (nm->frame_sizes, i);
      hash_set (nm->frame_size_hash, key, i);
    }

  return vec_elt_at_index (nm->frame_sizes, i);
#else
  ASSERT (vlib_frame_bytes (n_scalar_bytes, n_vector_bytes)
	  == (vlib_frame_bytes (0, 4)));
  return vec_elt_at_index (nm->frame_sizes, 0);
#endif
}

//申请to_node_index节点适用的frame
static u32
vlib_frame_alloc_to_node (vlib_main_t * vm, u32 to_node_index,
			  u32 frame_flags)
{
  vlib_node_main_t *nm = &vm->node_main;
  vlib_frame_size_t *fs;
  vlib_node_t *to_node;
  vlib_frame_t *f;
  u32 fi, l, n, scalar_size, vector_size;

  //通过索引取vpp node
  to_node = vlib_get_node (vm, to_node_index);

  scalar_size = to_node->scalar_size;
  vector_size = to_node->vector_size;

  //获得frame_size信息，如果没有则创建（准备利用其来申请空闲frame)
  fs = get_frame_size_info (nm, scalar_size, vector_size);

  //获取这种frame实际需要的长度
  n = vlib_frame_bytes (scalar_size, vector_size);

  //检查fs是否有空闲的free可用于分配
  if ((l = vec_len (fs->free_frame_indices)) > 0)
    {
      /* Allocate from end of free list. */
      //有空闲，取这种frame size上空闲的index
      fi = fs->free_frame_indices[l - 1];
      f = vlib_get_frame_no_check (vm, fi);
      //使空闲减1
      _vec_len (fs->free_frame_indices) = l - 1;
    }
  else
    {
      //没有free的frame可以分配，申请n个字节，做为frame
      f = clib_mem_alloc_aligned_no_fail (n, VLIB_FRAME_ALIGN);
      //将f转换为frame index
      fi = vlib_frame_index_no_check (vm, f);
    }

  /* Poison frame when debugging. */
  if (CLIB_DEBUG > 0)
    clib_memset (f, 0xfe, n);

  /* Insert magic number. */
  {
    u32 *magic;

    //设置magic
    magic = vlib_frame_find_magic (f, to_node);
    *magic = VLIB_FRAME_MAGIC;
  }

  f->frame_flags = VLIB_FRAME_IS_ALLOCATED | frame_flags;
  f->n_vectors = 0;
  f->scalar_size = scalar_size;
  f->vector_size = vector_size;
  f->flags = 0;

  fs->n_alloc_frames += 1;

  return fi;
}

/* Allocate a frame for from FROM_NODE to TO_NODE via TO_NEXT_INDEX.
   Returns frame index. */
//申请frame，以便从from_node_runtime可以送报文到to_next_index对应的node
static u32
vlib_frame_alloc (vlib_main_t * vm, vlib_node_runtime_t * from_node_runtime,
		  u32 to_next_index)
{
  vlib_node_t *from_node;

  //源结点
  from_node = vlib_get_node (vm, from_node_runtime->node_index);
  ASSERT (to_next_index < vec_len (from_node->next_nodes));

  //申请一个frame_node->next_nodes[to_next_index]节点适用的frame
  return vlib_frame_alloc_to_node (vm, from_node->next_nodes[to_next_index]/*目标结点*/,
				   /* frame_flags */ 0);
}

//申请适用于to_node_index的frame
vlib_frame_t *
vlib_get_frame_to_node (vlib_main_t * vm, u32 to_node_index)
{
  u32 fi = vlib_frame_alloc_to_node (vm, to_node_index,
				     /* frame_flags */
				     VLIB_FRAME_FREE_AFTER_DISPATCH);
  return vlib_get_frame (vm, fi);
}

//将frame加入pending队列，指定to_node_index对应的node处理此报文
void
vlib_put_frame_to_node (vlib_main_t * vm, u32 to_node_index, vlib_frame_t * f)
{
  vlib_pending_frame_t *p;
  vlib_node_t *to_node;

  if (f->n_vectors == 0)
    return;

  //获取报文要传递给的node
  to_node = vlib_get_node (vm, to_node_index);

  //将报文加入到pending集合中
  vec_add2 (vm->node_main.pending_frames, p, 1);

  f->frame_flags |= VLIB_FRAME_PENDING;
  p->frame_index = vlib_frame_index (vm, f);
  //指出报文属于哪个node
  p->node_runtime_index = to_node->runtime_index;
  //指明无下一个frame索引
  p->next_frame_index = VLIB_PENDING_FRAME_NO_NEXT_FRAME;
}

/* Free given frame. */
void
vlib_frame_free (vlib_main_t * vm, vlib_node_runtime_t * r, vlib_frame_t * f)
{
  vlib_node_main_t *nm = &vm->node_main;
  vlib_node_t *node;
  vlib_frame_size_t *fs;
  u32 frame_index;

  ASSERT (f->frame_flags & VLIB_FRAME_IS_ALLOCATED);

  node = vlib_get_node (vm, r->node_index);
  fs = get_frame_size_info (nm, node->scalar_size, node->vector_size);

  frame_index = vlib_frame_index (vm, f);

  ASSERT (f->frame_flags & VLIB_FRAME_IS_ALLOCATED);

  /* No next frames may point to freed frame. */
  if (CLIB_DEBUG > 0)
    {
      vlib_next_frame_t *nf;
      vec_foreach (nf, vm->node_main.next_frames)
	ASSERT (nf->frame_index != frame_index);
    }

  f->frame_flags &= ~(VLIB_FRAME_IS_ALLOCATED | VLIB_FRAME_NO_APPEND);

  vec_add1 (fs->free_frame_indices, frame_index);
  ASSERT (fs->n_alloc_frames > 0);
  fs->n_alloc_frames -= 1;
}

static clib_error_t *
show_frame_stats (vlib_main_t * vm,
		  unformat_input_t * input, vlib_cli_command_t * cmd)
{
  vlib_node_main_t *nm = &vm->node_main;
  vlib_frame_size_t *fs;

  vlib_cli_output (vm, "%=6s%=12s%=12s", "Size", "# Alloc", "# Free");
  vec_foreach (fs, nm->frame_sizes)
  {
    u32 n_alloc = fs->n_alloc_frames;
    u32 n_free = vec_len (fs->free_frame_indices);

    if (n_alloc + n_free > 0)
      vlib_cli_output (vm, "%=6d%=12d%=12d",
		       fs - nm->frame_sizes, n_alloc, n_free);
  }

  return 0;
}

/* *INDENT-OFF* */
VLIB_CLI_COMMAND (show_frame_stats_cli, static) = {
  .path = "show vlib frame-allocation",
  .short_help = "Show node dispatch frame statistics",
  .function = show_frame_stats,
};
/* *INDENT-ON* */

/* Change ownership of enqueue rights to given next node. */
static void
vlib_next_frame_change_ownership (vlib_main_t * vm,
				  vlib_node_runtime_t * node_runtime,
				  u32 next_index)
{
  vlib_node_main_t *nm = &vm->node_main;
  vlib_next_frame_t *next_frame;
  vlib_node_t *node, *next_node;

  node = vec_elt (nm->nodes, node_runtime->node_index);

  /* Only internal & input nodes are allowed to call other nodes. */
  ASSERT (node->type == VLIB_NODE_TYPE_INTERNAL
	  || node->type == VLIB_NODE_TYPE_INPUT
	  || node->type == VLIB_NODE_TYPE_PROCESS);

  ASSERT (vec_len (node->next_nodes) == node_runtime->n_next_nodes);

  next_frame =
    vlib_node_runtime_get_next_frame (vm, node_runtime, next_index);
  //取下一个node
  next_node = vec_elt (nm->nodes, node->next_nodes[next_index]);

  if (next_node->owner_node_index != VLIB_INVALID_NODE_INDEX)
    {
      /* Get frame from previous owner. */
      vlib_next_frame_t *owner_next_frame;
      vlib_next_frame_t tmp;

      owner_next_frame =
	vlib_node_get_next_frame (vm,
				  next_node->owner_node_index,
				  next_node->owner_next_index);

      /* Swap target next frame with owner's. */
      tmp = owner_next_frame[0];
      owner_next_frame[0] = next_frame[0];
      next_frame[0] = tmp;

      /*
       * If next_frame is already pending, we have to track down
       * all pending frames and fix their next_frame_index fields.
       */
      if (next_frame->flags & VLIB_FRAME_PENDING)
	{
	  vlib_pending_frame_t *p;
	  if (next_frame->frame_index != ~0)
	    {
	      vec_foreach (p, nm->pending_frames)
	      {
		if (p->frame_index == next_frame->frame_index)
		  {
		    p->next_frame_index =
		      next_frame - vm->node_main.next_frames;
		  }
	      }
	    }
	}
    }
  else
    {
      /* No previous owner. Take ownership. */
      next_frame->flags |= VLIB_FRAME_OWNER;
    }

  /* Record new owner. */
  next_node->owner_node_index = node->index;
  next_node->owner_next_index = next_index;

  /* Now we should be owner. */
  ASSERT (next_frame->flags & VLIB_FRAME_OWNER);
}

/* Make sure that magic number is still there.
   Otherwise, it is likely that caller has overrun frame arguments. */
always_inline void
validate_frame_magic (vlib_main_t * vm,
		      vlib_frame_t * f, vlib_node_t * n, uword next_index)
{
  vlib_node_t *next_node = vlib_get_node (vm, n->next_nodes[next_index]);
  u32 *magic = vlib_frame_find_magic (f, next_node);
  ASSERT (VLIB_FRAME_MAGIC == magic[0]);
}

//申请或者取出node->next_nodes[next_index]可使用的frame（如果需要就申请空的nf)
vlib_frame_t *
vlib_get_next_frame_internal (vlib_main_t * vm,
			      vlib_node_runtime_t * node,
			      u32 next_index, u32 allocate_new_next_frame)
{
  vlib_frame_t *f;
  vlib_next_frame_t *nf;
  u32 n_used;

  //取出node下层next_index对应的next_frame结构
  nf = vlib_node_runtime_get_next_frame (vm, node, next_index);

  /* Make sure this next frame owns right to enqueue to destination frame. */
  if (PREDICT_FALSE (!(nf->flags & VLIB_FRAME_OWNER)))
    vlib_next_frame_change_ownership (vm, node, next_index);

  /* ??? Don't need valid flag: can use frame_index == ~0 */
  if (PREDICT_FALSE (!(nf->flags & VLIB_FRAME_IS_ALLOCATED)))
    {
      //没有申请空间，则申请空间，并打上已申请空间标记
      nf->frame_index = vlib_frame_alloc (vm, node, next_index);
      nf->flags |= VLIB_FRAME_IS_ALLOCATED;
    }

  //利用frame index获得对应的frame
  f = vlib_get_frame (vm, nf->frame_index);

  /* Has frame been removed from pending vector (e.g. finished dispatching)?
     If so we can reuse frame. */
  if ((nf->flags & VLIB_FRAME_PENDING)
      && !(f->frame_flags & VLIB_FRAME_PENDING))
    {
      nf->flags &= ~VLIB_FRAME_PENDING;
      f->n_vectors = 0;
      f->flags = 0;
    }

  /* Allocate new frame if current one is marked as no-append or
     it is already full. */
  n_used = f->n_vectors;
  if (n_used >= VLIB_FRAME_SIZE || (allocate_new_next_frame && n_used > 0) ||
      (f->frame_flags & VLIB_FRAME_NO_APPEND))
    {
      //可以申请新的next_frame或者对应的frame已被使用尽，
      //或者frame不容许append新frame，则申请新的frame
      /* Old frame may need to be freed after dispatch, since we'll have
         two redundant frames from node -> next node. */
      if (!(nf->flags & VLIB_FRAME_NO_FREE_AFTER_DISPATCH))
	{
      //nf没有被标记为dispatch后不需要释放，故需要将frame标记为dispatch后需要释放
	  vlib_frame_t *f_old = vlib_get_frame (vm, nf->frame_index);
	  f_old->frame_flags |= VLIB_FRAME_FREE_AFTER_DISPATCH;
	}

      /* Allocate new frame to replace full one. */
      //申请可使用的frame(这种情况下旧的frame没有引用是否可以释放？）
      nf->frame_index = vlib_frame_alloc (vm, node, next_index);
      f = vlib_get_frame (vm, nf->frame_index);
      n_used = f->n_vectors;
    }

  /* Should have free vectors in frame now. */
  ASSERT (n_used < VLIB_FRAME_SIZE);

  if (CLIB_DEBUG > 0)
    {
      validate_frame_magic (vm, f,
			    vlib_get_node (vm, node->node_index), next_index);
    }

  return f;
}

static void
vlib_put_next_frame_validate (vlib_main_t * vm,
			      vlib_node_runtime_t * rt,
			      u32 next_index, u32 n_vectors_left)
{
  vlib_node_main_t *nm = &vm->node_main;
  vlib_next_frame_t *nf;
  vlib_frame_t *f;
  vlib_node_runtime_t *next_rt;
  vlib_node_t *next_node;
  u32 n_before, n_after;

  nf = vlib_node_runtime_get_next_frame (vm, rt, next_index);
  f = vlib_get_frame (vm, nf->frame_index);

  ASSERT (n_vectors_left <= VLIB_FRAME_SIZE);
  n_after = VLIB_FRAME_SIZE - n_vectors_left;
  n_before = f->n_vectors;

  ASSERT (n_after >= n_before);

  next_rt = vec_elt_at_index (nm->nodes_by_type[VLIB_NODE_TYPE_INTERNAL],
			      nf->node_runtime_index);
  next_node = vlib_get_node (vm, next_rt->node_index);
  if (n_after > 0 && next_node->validate_frame)
    {
      u8 *msg = next_node->validate_frame (vm, rt, f);
      if (msg)
	{
	  clib_warning ("%v", msg);
	  ASSERT (0);
	}
      vec_free (msg);
    }
}

//如果freame中已有元素，则将frame中的报文添加至pending_freams中
void
vlib_put_next_frame (vlib_main_t * vm,
		     vlib_node_runtime_t * r,
		     u32 next_index, u32 n_vectors_left)
{
  vlib_node_main_t *nm = &vm->node_main;
  vlib_next_frame_t *nf;
  vlib_frame_t *f;
  u32 n_vectors_in_frame;

  if (CLIB_DEBUG > 0)
    vlib_put_next_frame_validate (vm, r, next_index, n_vectors_left);

  //取next node对应的frame
  nf = vlib_node_runtime_get_next_frame (vm, r, next_index);
  f = vlib_get_frame (vm, nf->frame_index);

  /* Make sure that magic number is still there.  Otherwise, caller
     has overrun frame meta data. */
  if (CLIB_DEBUG > 0)
    {
      vlib_node_t *node = vlib_get_node (vm, r->node_index);
      validate_frame_magic (vm, f, node, next_index);
    }

  /* Convert # of vectors left -> number of vectors there. */
  //n_vectors_left表示f中剩余空间数，n_vectors_in_frame则表示f中当前已用空间数
  ASSERT (n_vectors_left <= VLIB_FRAME_SIZE);
  n_vectors_in_frame = VLIB_FRAME_SIZE - n_vectors_left;

  //设置frame中已用元素数
  f->n_vectors = n_vectors_in_frame;

  /* If vectors were added to frame, add to pending vector. */
  //如果frame中已有元素，则构造pending_frame，并将其添加到pending_queue中
  if (PREDICT_TRUE (n_vectors_in_frame > 0))
    {
      vlib_pending_frame_t *p;
      u32 v0, v1;

      //记录被pending的index
      r->cached_next_index = next_index;

      //frame中的报文还未加入到frame penging中，
      //则将报文加入到pending_queue中
      if (!(f->frame_flags & VLIB_FRAME_PENDING))
	{
	  __attribute__ ((unused)) vlib_node_t *node;
	  vlib_node_t *next_node;
	  vlib_node_runtime_t *next_runtime;

	  //通过node_index获取对应的node
	  node = vlib_get_node (vm, r->node_index);

	  //在此node基础上查找对应的next_node,及next_node对应的runtime
	  next_node = vlib_get_next_node (vm, r->node_index, next_index);
	  next_runtime = vlib_node_get_runtime (vm, next_node->index);

	  //获取pending_frame节点，并填充它，将报文添加到pending_frames中
	  vec_add2 (nm->pending_frames, p, 1);

	  //指明pending_frame的索引号
	  p->frame_index = nf->frame_index;
	  p->node_runtime_index = nf->node_runtime_index;
	  //此报文已被pending到队列，指出pending其的nf索引
	  p->next_frame_index = nf - nm->next_frames;
	  nf->flags |= VLIB_FRAME_PENDING;
	  f->frame_flags |= VLIB_FRAME_PENDING;

	  /*
	   * If we're going to dispatch this frame on another thread,
	   * force allocation of a new frame. Otherwise, we create
	   * a dangling frame reference. Each thread has its own copy of
	   * the next_frames vector.
	   */
	  if (0 && r->thread_index != next_runtime->thread_index)
	    {
	      //当前运行node的线程与下一个node存在跨线程问题时需要进入(当前恒为假）
	      nf->frame_index = ~0;
	      nf->flags &= ~(VLIB_FRAME_PENDING | VLIB_FRAME_IS_ALLOCATED);
	    }
	}

      /* Copy trace flag from next_frame and from runtime. */
      nf->flags |=
	(nf->flags & VLIB_NODE_FLAG_TRACE) | (r->
					      flags & VLIB_NODE_FLAG_TRACE);

      //增加overflow
      v0 = nf->vectors_since_last_overflow;
      v1 = v0 + n_vectors_in_frame;
      nf->vectors_since_last_overflow = v1;
      if (PREDICT_FALSE (v1 < v0))
	{
      //出现绕圈事件
	  vlib_node_t *node = vlib_get_node (vm, r->node_index);
	  vec_elt (node->n_vectors_by_next_node, next_index) += v0;
	}
    }
}

/* Sync up runtime (32 bit counters) and main node stats (64 bit counters). */
never_inline void
vlib_node_runtime_sync_stats (vlib_main_t * vm,
			      vlib_node_runtime_t * r,
			      uword n_calls, uword n_vectors, uword n_clocks,
			      uword n_ticks0, uword n_ticks1)
{
  vlib_node_t *n = vlib_get_node (vm, r->node_index);

  n->stats_total.calls += n_calls + r->calls_since_last_overflow;
  n->stats_total.vectors += n_vectors + r->vectors_since_last_overflow;
  n->stats_total.clocks += n_clocks + r->clocks_since_last_overflow;
  n->stats_total.perf_counter0_ticks += n_ticks0 +
    r->perf_counter0_ticks_since_last_overflow;
  n->stats_total.perf_counter1_ticks += n_ticks1 +
    r->perf_counter1_ticks_since_last_overflow;
  n->stats_total.perf_counter_vectors += n_vectors +
    r->perf_counter_vectors_since_last_overflow;
  n->stats_total.max_clock = r->max_clock;
  n->stats_total.max_clock_n = r->max_clock_n;

  r->calls_since_last_overflow = 0;
  r->vectors_since_last_overflow = 0;
  r->clocks_since_last_overflow = 0;
  r->perf_counter0_ticks_since_last_overflow = 0ULL;
  r->perf_counter1_ticks_since_last_overflow = 0ULL;
  r->perf_counter_vectors_since_last_overflow = 0ULL;
}

always_inline void __attribute__ ((unused))
vlib_process_sync_stats (vlib_main_t * vm,
			 vlib_process_t * p,
			 uword n_calls, uword n_vectors, uword n_clocks,
			 uword n_ticks0, uword n_ticks1)
{
  vlib_node_runtime_t *rt = &p->node_runtime;
  vlib_node_t *n = vlib_get_node (vm, rt->node_index);
  vlib_node_runtime_sync_stats (vm, rt, n_calls, n_vectors, n_clocks,
				n_ticks0, n_ticks1);
  n->stats_total.suspends += p->n_suspends;
  p->n_suspends = 0;
}

void
vlib_node_sync_stats (vlib_main_t * vm, vlib_node_t * n)
{
  vlib_node_runtime_t *rt;

  if (n->type == VLIB_NODE_TYPE_PROCESS)
    {
      /* Nothing to do for PROCESS nodes except in main thread */
      if (vm != &vlib_global_main)
	return;

      vlib_process_t *p = vlib_get_process_from_node (vm, n);
      n->stats_total.suspends += p->n_suspends;
      p->n_suspends = 0;
      rt = &p->node_runtime;
    }
  else
    //将节点按类型加入到相应vector中
    rt =
      vec_elt_at_index (vm->node_main.nodes_by_type[n->type],
			n->runtime_index);

  vlib_node_runtime_sync_stats (vm, rt, 0, 0, 0, 0, 0);

  /* Sync up runtime next frame vector counters with main node structure. */
  {
    vlib_next_frame_t *nf;
    uword i;
    for (i = 0; i < rt->n_next_nodes; i++)
      {
	nf = vlib_node_runtime_get_next_frame (vm, rt, i);
	vec_elt (n->n_vectors_by_next_node, i) +=
	  nf->vectors_since_last_overflow;
	nf->vectors_since_last_overflow = 0;
      }
  }
}

always_inline u32
vlib_node_runtime_update_stats (vlib_main_t * vm,
				vlib_node_runtime_t * node,
				uword n_calls,
				uword n_vectors, uword n_clocks,
				uword n_ticks0, uword n_ticks1)
{
  u32 ca0, ca1, v0, v1, cl0, cl1, r;
  u32 ptick00, ptick01, ptick10, ptick11, pvec0, pvec1;

  cl0 = cl1 = node->clocks_since_last_overflow;
  ca0 = ca1 = node->calls_since_last_overflow;
  v0 = v1 = node->vectors_since_last_overflow;
  ptick00 = ptick01 = node->perf_counter0_ticks_since_last_overflow;
  ptick10 = ptick11 = node->perf_counter1_ticks_since_last_overflow;
  pvec0 = pvec1 = node->perf_counter_vectors_since_last_overflow;

  ca1 = ca0 + n_calls;
  v1 = v0 + n_vectors;
  cl1 = cl0 + n_clocks;
  ptick01 = ptick00 + n_ticks0;
  ptick11 = ptick10 + n_ticks1;
  pvec1 = pvec0 + n_vectors;

  node->calls_since_last_overflow = ca1;
  node->clocks_since_last_overflow = cl1;
  node->vectors_since_last_overflow = v1;
  node->perf_counter0_ticks_since_last_overflow = ptick01;
  node->perf_counter1_ticks_since_last_overflow = ptick11;
  node->perf_counter_vectors_since_last_overflow = pvec1;

  node->max_clock_n = node->max_clock > n_clocks ?
    node->max_clock_n : n_vectors;
  node->max_clock = node->max_clock > n_clocks ? node->max_clock : n_clocks;

  r = vlib_node_runtime_update_main_loop_vector_stats (vm, node, n_vectors);

  if (PREDICT_FALSE (ca1 < ca0 || v1 < v0 || cl1 < cl0) || (ptick01 < ptick00)
      || (ptick11 < ptick10) || (pvec1 < pvec0))
    {
      node->calls_since_last_overflow = ca0;
      node->clocks_since_last_overflow = cl0;
      node->vectors_since_last_overflow = v0;
      node->perf_counter0_ticks_since_last_overflow = ptick00;
      node->perf_counter1_ticks_since_last_overflow = ptick10;
      node->perf_counter_vectors_since_last_overflow = pvec0;

      vlib_node_runtime_sync_stats (vm, node, n_calls, n_vectors, n_clocks,
				    n_ticks0, n_ticks1);
    }

  return r;
}

//调用回调vlib_node_runtime_perf_counter_cb
static inline void
vlib_node_runtime_perf_counter (vlib_main_t * vm, u64 * pmc0, u64 * pmc1)
{
  *pmc0 = 0;
  *pmc1 = 0;
  if (PREDICT_FALSE (vm->vlib_node_runtime_perf_counter_cb != 0))
    (*vm->vlib_node_runtime_perf_counter_cb) (vm, pmc0, pmc1);
}

always_inline void
vlib_process_update_stats (vlib_main_t * vm,
			   vlib_process_t * p,
			   uword n_calls, uword n_vectors, uword n_clocks)
{
  vlib_node_runtime_update_stats (vm, &p->node_runtime,
				  n_calls, n_vectors, n_clocks, 0ULL, 0ULL);
}

static clib_error_t *
vlib_cli_elog_clear (vlib_main_t * vm,
		     unformat_input_t * input, vlib_cli_command_t * cmd)
{
  elog_reset_buffer (&vm->elog_main);
  return 0;
}

/* *INDENT-OFF* */
VLIB_CLI_COMMAND (elog_clear_cli, static) = {
  .path = "event-logger clear",
  .short_help = "Clear the event log",
  .function = vlib_cli_elog_clear,
};
/* *INDENT-ON* */

#ifdef CLIB_UNIX
static clib_error_t *
elog_save_buffer (vlib_main_t * vm,
		  unformat_input_t * input, vlib_cli_command_t * cmd)
{
  elog_main_t *em = &vm->elog_main;
  char *file, *chroot_file;
  clib_error_t *error = 0;

  if (!unformat (input, "%s", &file))
    {
      vlib_cli_output (vm, "expected file name, got `%U'",
		       format_unformat_error, input);
      return 0;
    }

  /* It's fairly hard to get "../oopsie" through unformat; just in case */
  if (strstr (file, "..") || index (file, '/'))
    {
      vlib_cli_output (vm, "illegal characters in filename '%s'", file);
      return 0;
    }

  chroot_file = (char *) format (0, "/tmp/%s%c", file, 0);

  vec_free (file);

  vlib_cli_output (vm, "Saving %wd of %wd events to %s",
		   elog_n_events_in_buffer (em),
		   elog_buffer_capacity (em), chroot_file);

  vlib_worker_thread_barrier_sync (vm);
  error = elog_write_file (em, chroot_file, 1 /* flush ring */ );
  vlib_worker_thread_barrier_release (vm);
  vec_free (chroot_file);
  return error;
}

void
elog_post_mortem_dump (void)
{
  vlib_main_t *vm = &vlib_global_main;
  elog_main_t *em = &vm->elog_main;
  u8 *filename;
  clib_error_t *error;

  if (!vm->elog_post_mortem_dump)
    return;

  filename = format (0, "/tmp/elog_post_mortem.%d%c", getpid (), 0);
  error = elog_write_file (em, (char *) filename, 1 /* flush ring */ );
  if (error)
    clib_error_report (error);
  vec_free (filename);
}

/* *INDENT-OFF* */
VLIB_CLI_COMMAND (elog_save_cli, static) = {
  .path = "event-logger save",
  .short_help = "event-logger save <filename> (saves log in /tmp/<filename>)",
  .function = elog_save_buffer,
};
/* *INDENT-ON* */

static clib_error_t *
elog_stop (vlib_main_t * vm,
	   unformat_input_t * input, vlib_cli_command_t * cmd)
{
  elog_main_t *em = &vm->elog_main;

  em->n_total_events_disable_limit = em->n_total_events;

  vlib_cli_output (vm, "Stopped the event logger...");
  return 0;
}

/* *INDENT-OFF* */
VLIB_CLI_COMMAND (elog_stop_cli, static) = {
  .path = "event-logger stop",
  .short_help = "Stop the event-logger",
  .function = elog_stop,
};
/* *INDENT-ON* */

static clib_error_t *
elog_restart (vlib_main_t * vm,
	      unformat_input_t * input, vlib_cli_command_t * cmd)
{
  elog_main_t *em = &vm->elog_main;

  em->n_total_events_disable_limit = ~0;

  vlib_cli_output (vm, "Restarted the event logger...");
  return 0;
}

/* *INDENT-OFF* */
VLIB_CLI_COMMAND (elog_restart_cli, static) = {
  .path = "event-logger restart",
  .short_help = "Restart the event-logger",
  .function = elog_restart,
};
/* *INDENT-ON* */

static clib_error_t *
elog_resize (vlib_main_t * vm,
	     unformat_input_t * input, vlib_cli_command_t * cmd)
{
  elog_main_t *em = &vm->elog_main;
  u32 tmp;

  /* Stop the parade */
  elog_reset_buffer (&vm->elog_main);

  if (unformat (input, "%d", &tmp))
    {
      elog_alloc (em, tmp);
      em->n_total_events_disable_limit = ~0;
    }
  else
    return clib_error_return (0, "Must specify how many events in the ring");

  vlib_cli_output (vm, "Resized ring and restarted the event logger...");
  return 0;
}

/* *INDENT-OFF* */
VLIB_CLI_COMMAND (elog_resize_cli, static) = {
  .path = "event-logger resize",
  .short_help = "event-logger resize <nnn>",
  .function = elog_resize,
};
/* *INDENT-ON* */

#endif /* CLIB_UNIX */

static void
elog_show_buffer_internal (vlib_main_t * vm, u32 n_events_to_show)
{
  elog_main_t *em = &vm->elog_main;
  elog_event_t *e, *es;
  f64 dt;

  /* Show events in VLIB time since log clock starts after VLIB clock. */
  dt = (em->init_time.cpu - vm->clib_time.init_cpu_time)
    * vm->clib_time.seconds_per_clock;

  //自em中取出所有存在buffer中的event,并显示它
  es = elog_peek_events (em);
  vlib_cli_output (vm, "%d of %d events in buffer, logger %s", vec_len (es),
		   em->event_ring_size,
		   em->n_total_events < em->n_total_events_disable_limit ?
		   "running" : "stopped");
  vec_foreach (e, es)
  {
    vlib_cli_output (vm, "%18.9f: %U",
		     e->time + dt, format_elog_event, em, e);
    n_events_to_show--;
    if (n_events_to_show == 0)
      break;
  }
  vec_free (es);

}

static clib_error_t *
elog_show_buffer (vlib_main_t * vm,
		  unformat_input_t * input, vlib_cli_command_t * cmd)
{
  u32 n_events_to_show;
  clib_error_t *error = 0;

  n_events_to_show = 250;
  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (input, "%d", &n_events_to_show))
	;
      else if (unformat (input, "all"))
	n_events_to_show = ~0;
      else
	return unformat_parse_error (input);
    }
  elog_show_buffer_internal (vm, n_events_to_show);
  return error;
}

/* *INDENT-OFF* */
VLIB_CLI_COMMAND (elog_show_cli, static) = {
  .path = "show event-logger",
  .short_help = "Show event logger info",
  .function = elog_show_buffer,
};
/* *INDENT-ON* */

void
vlib_gdb_show_event_log (void)
{
  elog_show_buffer_internal (vlib_get_main (), (u32) ~ 0);
}

//如果需要trace,则向em中添加event并记录待处理的frame,及处理完成的frame数目
static inline void
vlib_elog_main_loop_event (vlib_main_t * vm,
			   u32 node_index/*node索引*/,
			   u64 time, u32 n_vectors, u32 is_return/*是否after调用*/)
{
  vlib_main_t *evm = &vlib_global_main;
  elog_main_t *em = &evm->elog_main;
  int enabled = evm->elog_trace_graph_dispatch |
    evm->elog_trace_graph_circuit;

  if (PREDICT_FALSE (enabled && n_vectors))
  {
      if (PREDICT_FALSE (!elog_is_enabled (em)))
	  {
          evm->elog_trace_graph_dispatch = 0;
          evm->elog_trace_graph_circuit = 0;
          return;
	  }
      if (PREDICT_TRUE
	  (evm->elog_trace_graph_dispatch ||
	   (evm->elog_trace_graph_circuit &&
	    node_index == evm->elog_trace_graph_circuit_node_index)))
	  {
          //添加event,并设置其event数据为n_vectors
          elog_track (em,
		      /* event type */
	          /*return event type及call event type均按node索引存入event type*/
		      vec_elt_at_index (is_return
					? evm->node_return_elog_event_types //return时event type
					: evm->node_call_elog_event_types,
					node_index),
		      /* track */
			  /*使用哪个track*/
		      (vm->thread_index ?
		       &vlib_worker_threads[vm->thread_index].elog_track
		       : &em->default_track),
		      /* data to log 采用n_vectors记录event 数组*/ n_vectors);
	  }
   }
}

#if VLIB_BUFFER_TRACE_TRAJECTORY > 0
void (*vlib_buffer_trace_trajectory_cb) (vlib_buffer_t * b, u32 node_index);
void (*vlib_buffer_trace_trajectory_init_cb) (vlib_buffer_t * b);

void
vlib_buffer_trace_trajectory_init (vlib_buffer_t * b)
{
  if (PREDICT_TRUE (vlib_buffer_trace_trajectory_init_cb != 0))
    {
      (*vlib_buffer_trace_trajectory_init_cb) (b);
    }
}

#endif

static inline void
add_trajectory_trace (vlib_buffer_t * b, u32 node_index)
{
#if VLIB_BUFFER_TRACE_TRAJECTORY > 0
  if (PREDICT_TRUE (vlib_buffer_trace_trajectory_cb != 0))
    {
      (*vlib_buffer_trace_trajectory_cb) (b, node_index);
    }
#endif
}

u8 *format_vnet_buffer_flags (u8 * s, va_list * args) __attribute__ ((weak));
u8 *
format_vnet_buffer_flags (u8 * s, va_list * args)
{
  s = format (s, "BUG STUB %s", __FUNCTION__);
  return s;
}

u8 *format_vnet_buffer_opaque (u8 * s, va_list * args) __attribute__ ((weak));
u8 *
format_vnet_buffer_opaque (u8 * s, va_list * args)
{
  s = format (s, "BUG STUB %s", __FUNCTION__);
  return s;
}

u8 *format_vnet_buffer_opaque2 (u8 * s, va_list * args)
  __attribute__ ((weak));
u8 *
format_vnet_buffer_opaque2 (u8 * s, va_list * args)
{
  s = format (s, "BUG STUB %s", __FUNCTION__);
  return s;
}

static u8 *
format_buffer_metadata (u8 * s, va_list * args)
{
  vlib_buffer_t *b = va_arg (*args, vlib_buffer_t *);

  s = format (s, "flags: %U\n", format_vnet_buffer_flags, b);
  s = format (s, "current_data: %d, current_length: %d\n",
	      (i32) (b->current_data), (i32) (b->current_length));
  s = format (s, "current_config_index: %d, flow_id: %x, next_buffer: %x\n",
	      b->current_config_index, b->flow_id, b->next_buffer);
  s = format (s, "error: %d, ref_count: %d, buffer_pool_index: %d\n",
	      (u32) (b->error), (u32) (b->ref_count),
	      (u32) (b->buffer_pool_index));
  s = format (s,
	      "trace_index: %d, len_not_first_buf: %d\n",
	      b->trace_index, b->total_length_not_including_first_buffer);
  return s;
}

//向pcap_buffer中加入数据X
#define A(x) vec_add1(vm->pcap_buffer, (x))

//走pcap trace，将报文加入到vm->pcap_buffer中
static void
dispatch_pcap_trace (vlib_main_t * vm,
		     vlib_node_runtime_t * node, vlib_frame_t * frame)
{
  int i;
  vlib_buffer_t *bufs[VLIB_FRAME_SIZE], **bufp, *b;
  pcap_main_t *pm = &vm->dispatch_pcap_main;
  vlib_trace_main_t *tm = &vm->trace_main;
  u32 capture_size;
  vlib_node_t *n;
  i32 n_left;
  f64 time_now = vlib_time_now (vm);
  u32 *from;
  u8 *d;
  u8 string_count;

  /* Input nodes don't have frames yet */
  if (frame == 0 || frame->n_vectors == 0)
    return;

  //取报文的buffer index数组，并将其转换为buffer指针
  from = vlib_frame_vector_args (frame);
  vlib_get_buffers (vm, from, bufs, frame->n_vectors);
  bufp = bufs;

  //取对应的node
  n = vlib_get_node (vm, node->node_index);

  //遍历每个报文
  for (i = 0; i < frame->n_vectors; i++)
    {
      //需要capture的报文少于需要capture的报文时进入
      if (PREDICT_TRUE (pm->n_packets_captured < pm->n_packets_to_capture))
	{
	  b = bufp[i];

	  vec_reset_length (vm->pcap_buffer);
	  string_count = 0;

	  //将packet加入到vm->pcap_buffer中
	  /* Version, flags */
	  A ((u8) VLIB_PCAP_MAJOR_VERSION);
	  A ((u8) VLIB_PCAP_MINOR_VERSION);
	  A (0 /* string_count */ );
	  A (n->protocol_hint);

	  /* Buffer index (big endian) */
	  A ((from[i] >> 24) & 0xff);
	  A ((from[i] >> 16) & 0xff);
	  A ((from[i] >> 8) & 0xff);
	  A ((from[i] >> 0) & 0xff);

	  /* Node name, NULL-terminated ASCII */
	  vm->pcap_buffer = format (vm->pcap_buffer, "%v%c", n->name, 0);
	  string_count++;

	  vm->pcap_buffer = format (vm->pcap_buffer, "%U%c",
				    format_buffer_metadata, b, 0);
	  string_count++;
	  vm->pcap_buffer = format (vm->pcap_buffer, "%U%c",
				    format_vnet_buffer_opaque, b, 0);
	  string_count++;
	  vm->pcap_buffer = format (vm->pcap_buffer, "%U%c",
				    format_vnet_buffer_opaque2, b, 0);
	  string_count++;

	  /* Is this packet traced? */
	  if (PREDICT_FALSE (b->flags & VLIB_BUFFER_IS_TRACED))
	    {
	      vlib_trace_header_t **h
		= pool_elt_at_index (tm->trace_buffer_pool, b->trace_index);

	      vm->pcap_buffer = format (vm->pcap_buffer, "%U%c",
					format_vlib_trace, vm, h[0], 0);
	      string_count++;
	    }

	  /* Save the string count */
	  vm->pcap_buffer[2] = string_count;

	  /* Figure out how many bytes in the pcap trace */
	  capture_size = vec_len (vm->pcap_buffer) +
	    +vlib_buffer_length_in_chain (vm, b);

	  clib_spinlock_lock_if_init (&pm->lock);
	  n_left = clib_min (capture_size, 16384);
	  d = pcap_add_packet (pm, time_now, n_left, capture_size);

	  /* Copy the header */
	  clib_memcpy_fast (d, vm->pcap_buffer, vec_len (vm->pcap_buffer));
	  d += vec_len (vm->pcap_buffer);

	  n_left = clib_min
	    (vlib_buffer_length_in_chain (vm, b),
	     (16384 - vec_len (vm->pcap_buffer)));
	  /* Copy the packet data */
	  while (1)
	    {
	      u32 copy_length = clib_min ((u32) n_left, b->current_length);
	      clib_memcpy_fast (d, b->data + b->current_data, copy_length);
	      n_left -= b->current_length;
	      if (n_left <= 0)
		break;
	      d += b->current_length;
	      ASSERT (b->flags & VLIB_BUFFER_NEXT_PRESENT);
	      b = vlib_get_buffer (vm, b->next_buffer);
	    }
	  clib_spinlock_unlock_if_init (&pm->lock);
	}
    }
}

//节点调度
static_always_inline u64
dispatch_node (vlib_main_t * vm,
	       vlib_node_runtime_t * node/*要调度的node*/,
	       vlib_node_type_t type/*要调度的node类型*/,
	       vlib_node_state_t dispatch_state/*待调度节点的状态*/,
	       vlib_frame_t * frame/*需要本node处理的报文*/, u64 last_time_stamp)
{
  uword n, v;
  u64 t;
  vlib_node_main_t *nm = &vm->node_main;
  vlib_next_frame_t *nf;
  u64 pmc_before[2], pmc_after[2], pmc_delta[2];

  //断言node->node_index号node的类型为$type
  if (CLIB_DEBUG > 0)
  {
      vlib_node_t *n = vlib_get_node (vm, node->node_index);
      ASSERT (n->type == type);
  }

  /* Only non-internal nodes may be disabled. */
  //如果非VLIB_NODE_TYPE_INTERNAL类型的node,则不调度与期待状态不符的node
  if (type != VLIB_NODE_TYPE_INTERNAL && node->state != dispatch_state)
  {
      ASSERT (type != VLIB_NODE_TYPE_INTERNAL);
      return last_time_stamp;
  }

  //pre-input,input类型的node,非interrupt时，仅在node->input_main_loops_per_call为0时调度
  if ((type == VLIB_NODE_TYPE_PRE_INPUT || type == VLIB_NODE_TYPE_INPUT)
      && dispatch_state != VLIB_NODE_STATE_INTERRUPT)
  {
      u32 c = node->input_main_loops_per_call;
      /* Only call node when count reaches zero. */
      if (c)
      {
          //未达到0，不进入下面处理，返回
          node->input_main_loops_per_call = c - 1;
          return last_time_stamp;
      }
  }

  /* Speculatively prefetch next frames. */
  //如果node有后续节点，预取next frames
  if (node->n_next_nodes > 0)
  {
      nf = vec_elt_at_index (nm->next_frames, node->next_frame_index);
      CLIB_PREFETCH (nf, 4 * sizeof (nf[0]), WRITE);
  }

  vm->cpu_time_last_node_dispatch = last_time_stamp;

  //记录node->node_index待处理的frame数目
  vlib_elog_main_loop_event (vm, node->node_index,
			     last_time_stamp, frame ? frame->n_vectors : 0/*frame数目*/,
			     /* is_after */ 0);

  //执行vlib_node_runtime_perf_counter_cb回调
  vlib_node_runtime_perf_counter (vm, &pmc_before[0], &pmc_before[1]);

  /*
   * Turn this on if you run into
   * "bad monkey" contexts, and you want to know exactly
   * which nodes they've visited... See ixge.c...
   */
  if (VLIB_BUFFER_TRACE_TRAJECTORY && frame)
  {
      int i;
      u32 *from;

      //取buffer index数组，执行trajectory_trace回调
      from = vlib_frame_vector_args (frame);
      for (i = 0; i < frame->n_vectors; i++)
      {
          vlib_buffer_t *b = vlib_get_buffer (vm, from[i]);
          add_trajectory_trace (b, node->node_index);
      }

      //如果开启了pcap,则执行pcap流程
      if (PREDICT_FALSE (vm->dispatch_pcap_enable))
          dispatch_pcap_trace (vm, node, frame);

      //执行node的function调用
      n = node->function (vm, node, frame);
  }
  else
  {
      //如果开启了pcap,则执行pcap流程
      if (PREDICT_FALSE (vm->dispatch_pcap_enable))
          dispatch_pcap_trace (vm, node, frame);

      //执行node的function调用
      n = node->function (vm, node, frame);
  }

  t = clib_cpu_time_now ();

  /*
   * To validate accounting: pmc_delta = t - pmc_before;
   * perf ticks should equal clocks/pkt...
   */
  //执行vlib_node_runtime_perf_counter_cb回调
  vlib_node_runtime_perf_counter (vm, &pmc_after[0], &pmc_after[1]);

  //计算两者之间的差值
  pmc_delta[0] = pmc_after[0] - pmc_before[0];
  pmc_delta[1] = pmc_after[1] - pmc_before[1];

  vlib_elog_main_loop_event (vm, node->node_index, t, n, 1 /* is_after */ );

  vm->main_loop_vectors_processed += n;
  vm->main_loop_nodes_processed += n > 0;

  v = vlib_node_runtime_update_stats (vm, node,
				      /* n_calls */ 1,
				      /* n_vectors */ n,
				      /* n_clocks */ t - last_time_stamp,
				      pmc_delta[0] /* PMC0 */ ,
				      pmc_delta[1] /* PMC1 */ );

  /* When in interrupt mode and vector rate crosses threshold switch to
     polling mode. */
  //node转态转换
  if (PREDICT_FALSE ((dispatch_state == VLIB_NODE_STATE_INTERRUPT)
		     || (dispatch_state == VLIB_NODE_STATE_POLLING
			 && (node->flags
			     &
			     VLIB_NODE_FLAG_SWITCH_FROM_INTERRUPT_TO_POLLING_MODE))))
  {
      /* *INDENT-OFF* */
      //申明elog对应的event type
      ELOG_TYPE_DECLARE (e) =
      {
          .function = (char *) __FUNCTION__,
          .format = "%s vector length %d, switching to %s",
          .format_args = "T4i4t4",
          .n_enum_strings = 2,
          .enum_strings = {
            "interrupt", "polling",
          },
      };
      /* *INDENT-ON* */
      struct
      {
          u32 node_name, vector_length, is_polling;
      } *ed;

      if ((dispatch_state == VLIB_NODE_STATE_INTERRUPT
	   && v >= nm->polling_threshold_vector_length) &&
	  !(node->flags &
	    VLIB_NODE_FLAG_SWITCH_FROM_INTERRUPT_TO_POLLING_MODE))
      {
          //取相应的node，将其更新为polling状态
          vlib_node_t *n = vlib_get_node (vm, node->node_index);
          n->state = VLIB_NODE_STATE_POLLING;
          node->state = VLIB_NODE_STATE_POLLING;
          node->flags &=
                  ~VLIB_NODE_FLAG_SWITCH_FROM_POLLING_TO_INTERRUPT_MODE;
          node->flags |= VLIB_NODE_FLAG_SWITCH_FROM_INTERRUPT_TO_POLLING_MODE;
          nm->input_node_counts_by_state[VLIB_NODE_STATE_INTERRUPT] -= 1;
          nm->input_node_counts_by_state[VLIB_NODE_STATE_POLLING] += 1;

          if (PREDICT_FALSE (vlib_global_main.elog_trace_graph_dispatch))
          {
              vlib_worker_thread_t *w = vlib_worker_threads
                      + vm->thread_index;

              //注册对应的event,并对此event数据进行设置
              ed = ELOG_TRACK_DATA (&vlib_global_main.elog_main, e,
				    w->elog_track);
              ed->node_name = n->name_elog_string;
              ed->vector_length = v;
              ed->is_polling = 1;
          }
      }
      else if (dispatch_state == VLIB_NODE_STATE_POLLING
	       && v <= nm->interrupt_threshold_vector_length)
      {
          vlib_node_t *n = vlib_get_node (vm, node->node_index);
          if (node->flags &
                  VLIB_NODE_FLAG_SWITCH_FROM_POLLING_TO_INTERRUPT_MODE)
          {
              /* Switch to interrupt mode after dispatch in polling one more time.
	             This allows driver to re-enable interrupts. */
              n->state = VLIB_NODE_STATE_INTERRUPT;
              node->state = VLIB_NODE_STATE_INTERRUPT;
              node->flags &=
                      ~VLIB_NODE_FLAG_SWITCH_FROM_INTERRUPT_TO_POLLING_MODE;
              nm->input_node_counts_by_state[VLIB_NODE_STATE_POLLING] -= 1;
              nm->input_node_counts_by_state[VLIB_NODE_STATE_INTERRUPT] += 1;

          }
          else
          {
              vlib_worker_thread_t *w = vlib_worker_threads
                      + vm->thread_index;
              node->flags |=
                      VLIB_NODE_FLAG_SWITCH_FROM_POLLING_TO_INTERRUPT_MODE;
              if (PREDICT_FALSE (vlib_global_main.elog_trace_graph_dispatch))
              {
                  //注册对应的event,并对此event数据进行设置
                  ed = ELOG_TRACK_DATA (&vlib_global_main.elog_main, e,
					w->elog_track);
                  ed->node_name = n->name_elog_string;
                  ed->vector_length = v;
                  ed->is_polling = 0;
              }
          }
      }
  }

  return t;
}

//处理pending_frame链上指定索引的元素，使对应的node处理它
static u64
dispatch_pending_node (vlib_main_t * vm, uword pending_frame_index/*pending_frame索引*/,
		       u64 last_time_stamp)
{
  vlib_node_main_t *nm = &vm->node_main;
  vlib_frame_t *f;
  vlib_next_frame_t *nf, nf_dummy;
  vlib_node_runtime_t *n;
  u32 restore_frame_index;
  vlib_pending_frame_t *p;

  /* See comment below about dangling references to nm->pending_frames */
  //按索引取出待处理的帧
  p = nm->pending_frames + pending_frame_index;

  //取得此帧对应的node,这些节点均为internal类型的（可被挂起的pending的节点总为internal类型的node)
  n = vec_elt_at_index (nm->nodes_by_type[VLIB_NODE_TYPE_INTERNAL],
			p->node_runtime_index);

  //由frame index换算为frame
  f = vlib_get_frame (vm, p->frame_index);
  if (p->next_frame_index == VLIB_PENDING_FRAME_NO_NEXT_FRAME)
  {
      /* No next frame: so use dummy on stack. */
      nf = &nf_dummy;
      nf->flags = f->frame_flags & VLIB_NODE_FLAG_TRACE;
      nf->frame_index = ~p->frame_index;
  }
  else
    nf = vec_elt_at_index (nm->next_frames, p->next_frame_index);

  ASSERT (f->frame_flags & VLIB_FRAME_IS_ALLOCATED);

  /* Force allocation of new frame while current frame is being
     dispatched. */
  restore_frame_index = ~0;
  if (nf->frame_index == p->frame_index)
  {
      nf->frame_index = ~0;
      nf->flags &= ~VLIB_FRAME_IS_ALLOCATED;
      if (!(n->flags & VLIB_NODE_FLAG_FRAME_NO_FREE_AFTER_DISPATCH))
	restore_frame_index = p->frame_index;
  }

  /* Frame must be pending. */
  //这个frame一定处于pending状态，且包含报文
  ASSERT (f->frame_flags & VLIB_FRAME_PENDING);
  ASSERT (f->n_vectors > 0);

  /* Copy trace flag from next frame to node.
     Trace flag indicates that at least one vector in the dispatched
     frame is traced. */
  n->flags &= ~VLIB_NODE_FLAG_TRACE;
  n->flags |= (nf->flags & VLIB_FRAME_TRACE) ? VLIB_NODE_FLAG_TRACE : 0;
  nf->flags &= ~VLIB_FRAME_TRACE;

  //调度内部节点n,使其处理f
  last_time_stamp = dispatch_node (vm, n,
				   VLIB_NODE_TYPE_INTERNAL,
				   VLIB_NODE_STATE_POLLING,
				   f, last_time_stamp);

  //清除此frame的已pending标记，并容许其append
  f->frame_flags &= ~(VLIB_FRAME_PENDING | VLIB_FRAME_NO_APPEND);

  /* Frame is ready to be used again, so restore it. */
  if (restore_frame_index != ~0)
  {
      /*
       * We musn't restore a frame that is flagged to be freed. This
       * shouldn't happen since frames to be freed post dispatch are
       * those used when the to-node frame becomes full i.e. they form a
       * sort of queue of frames to a single node. If we get here then
       * the to-node frame and the pending frame *were* the same, and so
       * we removed the to-node frame.  Therefore this frame is no
       * longer part of the queue for that node and hence it cannot be
       * it's overspill.
       */
      ASSERT (!(f->frame_flags & VLIB_FRAME_FREE_AFTER_DISPATCH));

      /*
       * NB: dispatching node n can result in the creation and scheduling
       * of new frames, and hence in the reallocation of nm->pending_frames.
       * Recompute p, or no supper. This was broken for more than 10 years.
       */
      p = nm->pending_frames + pending_frame_index;

      /*
       * p->next_frame_index can change during node dispatch if node
       * function decides to change graph hook up.
       */
      nf = vec_elt_at_index (nm->next_frames, p->next_frame_index);
      nf->flags |= VLIB_FRAME_IS_ALLOCATED;

      if (~0 == nf->frame_index)
      {
          /* no new frame has been assigned to this node, use the saved one */
          nf->frame_index = restore_frame_index;
          f->n_vectors = 0;
      }
      else
      {
          /* The node has gained a frame, implying packets from the current frame
	     were re-queued to this same node. we don't need the saved one
	     anymore */
         vlib_frame_free (vm, n, f);
      }
  }
  else
  {
      if (f->frame_flags & VLIB_FRAME_FREE_AFTER_DISPATCH)
      {
          ASSERT (!(n->flags & VLIB_NODE_FLAG_FRAME_NO_FREE_AFTER_DISPATCH));
          vlib_frame_free (vm, n, f);
      }
  }

  return last_time_stamp;
}

//检查进程的栈空间是否有效
always_inline uword
vlib_process_stack_is_valid (vlib_process_t * p)
{
  return p->stack[0] == VLIB_PROCESS_STACK_MAGIC;
}

//进程bootstrap对应的参数
typedef struct
{
  vlib_main_t *vm;
  vlib_process_t *process;
  vlib_frame_t *frame;
} vlib_process_bootstrap_args_t;

/* Called in process stack. */
//完成指定process类型node的function调用
static uword
vlib_process_bootstrap (uword _a)
{
  vlib_process_bootstrap_args_t *a;
  vlib_main_t *vm;
  vlib_node_runtime_t *node;
  vlib_frame_t *f;
  vlib_process_t *p;
  uword n;

  a = uword_to_pointer (_a, vlib_process_bootstrap_args_t *);

  vm = a->vm;
  p = a->process;
  f = a->frame;
  node = &p->node_runtime;

  //执行node的function
  n = node->function (vm, node, f);

  ASSERT (vlib_process_stack_is_valid (p));

  //函数执行完成，以返回值n的形式，跳回return_longjmp
  clib_longjmp (&p->return_longjmp, n);

  return n;
}

/* Called in main stack. */
//vpp process启动
static_always_inline uword
vlib_process_startup (vlib_main_t * vm, vlib_process_t * p, vlib_frame_t * f)
{
  vlib_process_bootstrap_args_t a;
  uword r;

  //准备参数，完成调用
  a.vm = vm;
  a.process = p;
  a.frame = f;

  //记录跳转点（如果以return方式跳转到此点，则进程将被重启）
  r = clib_setjmp (&p->return_longjmp, VLIB_PROCESS_RETURN_LONGJMP_RETURN);
  if (r == VLIB_PROCESS_RETURN_LONGJMP_RETURN)
    //调用bootstrap函数，完成node的function调用
    //注意，在调用中使用的栈为进程专有的栈空间
    r = clib_calljmp (vlib_process_bootstrap, pointer_to_uword (&a),
		      (void *) p->stack + (1 << p->log2_n_stack_bytes));

  return r;
}

//process 恢复函数
static_always_inline uword
vlib_process_resume (vlib_process_t * p)
{
  uword r;
  //清除suspend标记
  p->flags &= ~(VLIB_PROCESS_IS_SUSPENDED_WAITING_FOR_CLOCK
		| VLIB_PROCESS_IS_SUSPENDED_WAITING_FOR_EVENT
		| VLIB_PROCESS_RESUME_PENDING);

  //保存恢复位置（如果以return方式跳转到此点，则process回到上次的保存点，继续运行;如果以
  //suspend方式跳回，则此函数退出后检查p->flags，检查是否需要挂起
  r = clib_setjmp (&p->return_longjmp, VLIB_PROCESS_RETURN_LONGJMP_RETURN);
  if (r == VLIB_PROCESS_RETURN_LONGJMP_RETURN)
    clib_longjmp (&p->resume_longjmp, VLIB_PROCESS_RESUME_LONGJMP_RESUME);
  return r;
}

//使进程p开始运行
static u64
dispatch_process (vlib_main_t * vm,
		  vlib_process_t * p, vlib_frame_t * f, u64 last_time_stamp)
{
  vlib_node_main_t *nm = &vm->node_main;
  vlib_node_runtime_t *node_runtime = &p->node_runtime;

  //获得process对应的node
  vlib_node_t *node = vlib_get_node (vm, node_runtime->node_index);
  u32 old_process_index;
  u64 t;
  uword n_vectors, is_suspend;

  //不处于polling状态的node不处理
  //等待clock,等待event的process不处理
  if (node->state != VLIB_NODE_STATE_POLLING
      || (p->flags & (VLIB_PROCESS_IS_SUSPENDED_WAITING_FOR_CLOCK
		      | VLIB_PROCESS_IS_SUSPENDED_WAITING_FOR_EVENT)))
    return last_time_stamp;

  //指明node处于running状态
  p->flags |= VLIB_PROCESS_IS_RUNNING;

  //调度前elog生成
  t = last_time_stamp;
  vlib_elog_main_loop_event (vm, node_runtime->node_index, t,
			     f ? f->n_vectors : 0, /* is_after */ 0);

  /* Save away current process for suspend. */
  //保存旧的正在运行的进程index,指明新的正在运行的进程index
  old_process_index = nm->current_process_index;
  nm->current_process_index = node->runtime_index;

  //启动各process,使其运行一遍
  n_vectors = vlib_process_startup (vm, p, f);

  //还原旧的process index
  nm->current_process_index = old_process_index;

  //断言，返回值为longjmp_return的会循环调用function,故不应走到此处
  ASSERT (n_vectors != VLIB_PROCESS_RETURN_LONGJMP_RETURN);

  //node挂起处理
  is_suspend = n_vectors == VLIB_PROCESS_RETURN_LONGJMP_SUSPEND;
  if (is_suspend)
  {
      //进入本分支，表示process经过function处理（或者正在function处理时）认为需要等待
      //event或者clock，而返回suspend，本分支需要将function加入suspend_process中依
      //靠定时器及报文触发processn恢复
      vlib_pending_frame_t *pf;

      n_vectors = 0;

      //自pool内分配一个pf,用于填充待suspend的进程
      pool_get (nm->suspended_process_frames, pf);
      pf->node_runtime_index = node->runtime_index;
      pf->frame_index = f ? vlib_frame_index (vm, f) : ~0;
      pf->next_frame_index = ~0;

      p->n_suspends += 1;
      p->suspended_process_frame_index = pf - nm->suspended_process_frames;

      //如果process被suspend是为了等待clock,则新建并启动定时器
      if (p->flags & VLIB_PROCESS_IS_SUSPENDED_WAITING_FOR_CLOCK)
	  {
          //启动定时器，等待p->resume_clock_interval
          TWT (tw_timer_wheel) * tw =
                  (TWT (tw_timer_wheel) *) nm->timing_wheel;
          p->stop_timer_handle =
                  TW (tw_timer_start) (tw,
				 vlib_timing_wheel_data_set_suspended_process
				 (node->runtime_index) /* [sic] pool idex */ ,
				 0 /* timer_id */ ,
				 p->resume_clock_interval);
	  }
  }
  else
  {
    //非suspend process,则取除掉其对应的running标记
    p->flags &= ~VLIB_PROCESS_IS_RUNNING;
  }

  t = clib_cpu_time_now ();

  //调度后elog生成
  vlib_elog_main_loop_event (vm, node_runtime->node_index, t, is_suspend,
			     /* is_after */ 1);

  vlib_process_update_stats (vm, p,
			     /* n_calls */ !is_suspend,
			     /* n_vectors */ n_vectors,
			     /* n_clocks */ t - last_time_stamp);

  return t;
}

//启动process
void
vlib_start_process (vlib_main_t * vm, uword process_index)
{
  vlib_node_main_t *nm = &vm->node_main;
  vlib_process_t *p = vec_elt (nm->processes, process_index);
  dispatch_process (vm, p, /* frame */ 0, /* cpu_time_now */ 0);
}

//使process（$process_index）恢复执行
static u64
dispatch_suspended_process (vlib_main_t * vm,
			    uword process_index/*进程索引*/, u64 last_time_stamp)
{
  vlib_node_main_t *nm = &vm->node_main;
  vlib_node_runtime_t *node_runtime;
  vlib_node_t *node;
  vlib_frame_t *f;
  vlib_process_t *p;
  vlib_pending_frame_t *pf;
  u64 t, n_vectors, is_suspend;

  t = last_time_stamp;

  //获取对应的process，如果其没有running标记,则返回时间
  p = vec_elt (nm->processes, process_index);
  if (PREDICT_FALSE (!(p->flags & VLIB_PROCESS_IS_RUNNING)))
    return last_time_stamp;

  //处于suspend状态的process一定在等待clock或者等待event
  ASSERT (p->flags & (VLIB_PROCESS_IS_SUSPENDED_WAITING_FOR_CLOCK
		      | VLIB_PROCESS_IS_SUSPENDED_WAITING_FOR_EVENT));

  //取suspend状态process对应的frame信息
  pf = pool_elt_at_index (nm->suspended_process_frames,
			  p->suspended_process_frame_index);

  //获取process对应的node
  node_runtime = &p->node_runtime;
  node = vlib_get_node (vm, node_runtime->node_index);

  //取待处理的frame
  f = pf->frame_index != ~0 ? vlib_get_frame (vm, pf->frame_index) : 0;

  //process trace事件
  vlib_elog_main_loop_event (vm, node_runtime->node_index, t,
			     f ? f->n_vectors : 0, /* is_after */ 0);

  /* Save away current process for suspend. */
  //指明当前运行的process index
  nm->current_process_index = node->runtime_index;

  //更新p的恢复点，并使之自此处恢复
  n_vectors = vlib_process_resume (p);
  t = clib_cpu_time_now ();

  nm->current_process_index = ~0;

  //检查process是否需要再次被suspend
  is_suspend = n_vectors == VLIB_PROCESS_RETURN_LONGJMP_SUSPEND;
  if (is_suspend)
  {
      /* Suspend it again. */
      n_vectors = 0;
      p->n_suspends += 1;
      if (p->flags & VLIB_PROCESS_IS_SUSPENDED_WAITING_FOR_CLOCK)
	  {
          //启动timer，用于执行process挂起（通过sleep 一段时间完成）
          p->stop_timer_handle =
                  TW (tw_timer_start) ((TWT (tw_timer_wheel) *) nm->timing_wheel,
				 vlib_timing_wheel_data_set_suspended_process
				 (node->runtime_index) /* [sic] pool idex */ ,
				 0 /* timer_id */ ,
				 p->resume_clock_interval);
	  }
  }
  else
    {
      //process被运行完成，清除running标记，归还占用的suspend_process_frames元素
      p->flags &= ~VLIB_PROCESS_IS_RUNNING;
      pool_put_index (nm->suspended_process_frames,
		      p->suspended_process_frame_index);
      p->suspended_process_frame_index = ~0;
    }

  t = clib_cpu_time_now ();

  //process trace事件
  vlib_elog_main_loop_event (vm, node_runtime->node_index, t, !is_suspend,
			     /* is_after */ 1);

  vlib_process_update_stats (vm, p,
			     /* n_calls */ !is_suspend,
			     /* n_vectors */ n_vectors,
			     /* n_clocks */ t - last_time_stamp);

  return t;
}

//提供弱符号的实现
void vl_api_send_pending_rpc_requests (vlib_main_t *) __attribute__ ((weak));
void
vl_api_send_pending_rpc_requests (vlib_main_t * vm)
{
}


//主或者从的loop处理
static_always_inline void
vlib_main_or_worker_loop (vlib_main_t * vm, int is_main/*是否为主线程*/)
{
  vlib_node_main_t *nm = &vm->node_main;
  vlib_thread_main_t *tm = vlib_get_thread_main ();
  uword i;
  u64 cpu_time_now;
  vlib_frame_queue_main_t *fqm;
  u32 *last_node_runtime_indices = 0;
  u32 frame_queue_check_counter = 0;

  /* Initialize pending node vector. */
  if (is_main)
  {
      //初始化pending_frames
      vec_resize (nm->pending_frames, 32);
      _vec_len (nm->pending_frames) = 0;
  }

  /* Mark time of main loop start. */
  if (is_main)
  {
      cpu_time_now = vm->clib_time.last_cpu_time;
      vm->cpu_time_main_loop_start = cpu_time_now;
  }
  else
  {
    //取当前时间
    cpu_time_now = clib_cpu_time_now ();
  }

  /* Pre-allocate interupt runtime indices and lock. */
  vec_alloc (nm->pending_interrupt_node_runtime_indices, 32);
  vec_alloc (last_node_runtime_indices, 32);
  if (!is_main)
  {
    clib_spinlock_init (&nm->pending_interrupt_lock);
  }

  /* Pre-allocate expired nodes. */
  if (!nm->polling_threshold_vector_length)
  {
    nm->polling_threshold_vector_length = 10;
  }
  if (!nm->interrupt_threshold_vector_length)
  {
    nm->interrupt_threshold_vector_length = 5;
  }

  //获取cpu_id,numa_node
  vm->cpu_id = clib_get_current_cpu_id ();
  vm->numa_node = clib_get_current_numa_node ();

  /* Start all processes. */
  //如果为主线程，则启动所有process
  if (is_main)
  {
      uword i;
      nm->current_process_index = ~0;

      //node注册时，已完成process生成，这里顺序开始运行.
      //vpp的进程是合作式process,即process与调度器相互配合作成task的切换工作
      //当前vpp支持以下几种切换：
      //1.通过等待事件在process->function内将自已挂起
      //2.通过timer在process->function内将自已挂起
      //3.process->function执行完成后，通过返回值将自已挂起
      //3.process->function执行完成，不挂起
      for (i = 0; i < vec_len (nm->processes); i++)
      {
	      cpu_time_now = dispatch_process (vm, nm->processes[i], /* frame */ 0,
					 cpu_time_now);
      }
  }

  while (1)
  {
      vlib_node_runtime_t *n;

      //如果vm有未决的rpc请求，则处理rpc请求
      if (PREDICT_FALSE (_vec_len (vm->pending_rpc_requests) > 0))
      {
    	  	  if (!is_main)
    	  	  {
    	  		  vl_api_send_pending_rpc_requests (vm);
    	  	  }
      }

      if (!is_main)
	{
          //检查是否需要等待barrier
	  vlib_worker_thread_barrier_check ();
	  if (PREDICT_FALSE (vm->check_frame_queues +
			     frame_queue_check_counter))
	    {
	      u32 processed = 0;

	      if (vm->check_frame_queues)
		{
		  frame_queue_check_counter = 100;
		  vm->check_frame_queues = 0;
		}

    	  //遍历所有handoff队列,处理所有frame_queue发送给vm线程的报文
	      vec_foreach (fqm, tm->frame_queue_mains)
    	     //将frame_queue中的元素出队，并将其交给pending_frames
    	     //（后续pending_frame节点所属的Node会处理它）
		      processed += vlib_frame_queue_dequeue (vm, fqm);

	      /* No handoff queue work found? */
	      if (processed)
		frame_queue_check_counter = 100;
	      else
		frame_queue_check_counter--;
	    }

	  //执行thead_main_loop_callback
	  if (PREDICT_FALSE (vm->worker_thread_main_loop_callback != 0))
	    ((void (*)(vlib_main_t *)) vm->worker_thread_main_loop_callback)
	      (vm);
	}

      /* Process pre-input nodes. */
      //处理pre-input类型的节点，必须处于polling状态的
      vec_foreach (n, nm->nodes_by_type[VLIB_NODE_TYPE_PRE_INPUT])
      {
    	  	  cpu_time_now = dispatch_node (vm, n,
				      VLIB_NODE_TYPE_PRE_INPUT,
				      VLIB_NODE_STATE_POLLING,
				      /* frame */ 0,
				      cpu_time_now);
      }

      /* Next process input nodes. */
      //input的nodes处理，必须处于polling状态
      vec_foreach (n, nm->nodes_by_type[VLIB_NODE_TYPE_INPUT])
      {
    	  	  cpu_time_now = dispatch_node (vm, n,
				      VLIB_NODE_TYPE_INPUT,
				      VLIB_NODE_STATE_POLLING,
				      /* frame */ 0/*input类型node调用时frame为空*/,
				      cpu_time_now);
      }

      if (PREDICT_TRUE (is_main && vm->queue_signal_pending == 0))
      {
    	  	  vm->queue_signal_callback (vm);
      }

      /* Next handle interrupts. */
      //如果存在有interrupt的input类型node,则调度它们
      {
    	  	  /* unlocked read, for performance */
    	  	  uword l = _vec_len (nm->pending_interrupt_node_runtime_indices);
    	  	  uword i;
    	  	  if (PREDICT_FALSE (l > 0))
    	  	  {
    	  	      //存在未绝的interrupt节点
    	  		  u32 *tmp;
    	  		  if (!is_main)
    	  		  {
    	  			  clib_spinlock_lock (&nm->pending_interrupt_lock);
    	  			  /* Re-read w/ lock held, in case another thread added an item */
    	  			  l = _vec_len (nm->pending_interrupt_node_runtime_indices);
    	  		  }

    	  		  tmp = nm->pending_interrupt_node_runtime_indices;
    	  		  nm->pending_interrupt_node_runtime_indices =
    	  				  last_node_runtime_indices;
    	  		  last_node_runtime_indices = tmp;
    	  		  _vec_len (last_node_runtime_indices) = 0;
    	  		  if (!is_main)
    	  		  {
    	  			  clib_spinlock_unlock (&nm->pending_interrupt_lock);
    	  		  }

    	  		  //遍历每个处于interrupt状态的input节点
    	  		  for (i = 0; i < l; i++)
    	  		  {
    	  			  n = vec_elt_at_index (nm->nodes_by_type[VLIB_NODE_TYPE_INPUT],
				      last_node_runtime_indices[i]);
    	  			  //调度处理中断状态的input节点
    	  			  cpu_time_now =
    	  					  dispatch_node (vm, n, VLIB_NODE_TYPE_INPUT,
    	  							  VLIB_NODE_STATE_INTERRUPT,
								  /* frame */ 0,
								  cpu_time_now);
    	  		  }
    	  	  }
      }

      /* Input nodes may have added work to the pending vector.
         Process pending vector until there is nothing left.
         All pending vectors will be processed from input -> output. */
      //上面我们将frame_queue中的报文存入到了pending_frames中，中间其它internal也会将报文添加至pending_frames上
      //现在遍历这些pending_frames,使相应的node处理它们,直到所有的节点均完成报文处理
      for (i = 0; i < _vec_len (nm->pending_frames); i++)
      {
          //对internal node完成调度
    	  	  cpu_time_now = dispatch_pending_node (vm, i, cpu_time_now);
      }
      /* Reset pending vector for next iteration. */
      _vec_len (nm->pending_frames) = 0;

      //node范围定时器超时处理
      if (is_main)
      {
          /* *INDENT-OFF* */
          ELOG_TYPE_DECLARE (es) =
            {
              .format = "process tw start",
              .format_args = "",
            };
          ELOG_TYPE_DECLARE (ee) =
            {
              .format = "process tw end: %d",
              .format_args = "i4",
            };
          /* *INDENT-ON* */

          struct
		  {
        	  	  int nready_procs;
		  } *ed;

		  /* Check if process nodes have expired from timing wheel. */
		  ASSERT (nm->data_from_advancing_timing_wheel != 0);

		  if (PREDICT_FALSE (vm->elog_trace_graph_dispatch))
		  {
		      //记录定时器开始事件
			  ed = ELOG_DATA (&vlib_global_main.elog_main, es);
		  }

		  //通过给定vector nm->data_from_advancing_timing_wheel收集所有已过期定时器，执行定时器维护
		  nm->data_from_advancing_timing_wheel =
				  TW (tw_timer_expire_timers_vec)
				  ((TWT (tw_timer_wheel) *) nm->timing_wheel, vlib_time_now (vm),
						  nm->data_from_advancing_timing_wheel);

		  ASSERT (nm->data_from_advancing_timing_wheel != 0);

		  if (PREDICT_FALSE (vm->elog_trace_graph_dispatch))
		  {
		      //记录定时器触发数目事件
			  ed = ELOG_DATA (&vlib_global_main.elog_main, ee);
			  //记录过期定时器数目
			  ed->nready_procs =
					  _vec_len (nm->data_from_advancing_timing_wheel);
		  }

		  //如果存在过期了的定时器，触发这些定时器函数
		  if (PREDICT_FALSE
				  (_vec_len (nm->data_from_advancing_timing_wheel) > 0))
		  {
			  uword i;

			  //遍历过期了的定时器
			  for (i = 0; i < _vec_len (nm->data_from_advancing_timing_wheel);
					  i++)
			  {
				  u32 d = nm->data_from_advancing_timing_wheel[i];
				  u32 di = vlib_timing_wheel_data_get_index (d);

				  if (vlib_timing_wheel_data_is_timed_event (d))
				  {
				      //事件触发类timer，这类timer是process要求在interval后触发事件，这里为node生成event

				      //通过索引查找到相应的te
					  vlib_signal_timed_event_data_t *te =
							  pool_elt_at_index (nm->signal_timed_event_data_pool,
									  di);
					  vlib_node_t *n =
							  vlib_get_node (vm, te->process_node_index);
					  vlib_process_t *p =
							  vec_elt (nm->processes, n->runtime_index);
					  //生成event,获得event data指针，准备填充它
					  void *data;
					  data =
							  vlib_process_signal_event_helper (nm, n, p,
							          /*知会process发生的事件编号*/
									  te->event_type_index,
									  te->n_data_elts,
									  te->n_data_elt_bytes);
					  //写入event data
					  if (te->n_data_bytes < sizeof (te->inline_event_data))
					  {
						  clib_memcpy_fast (data, te->inline_event_data,
								  te->n_data_bytes);
					  }
					  else
					  {
						  clib_memcpy_fast (data, te->event_data_as_vector,
								  te->n_data_bytes);
						  vec_free (te->event_data_as_vector);
					  }

					  //归还te
					  pool_put (nm->signal_timed_event_data_pool, te);
				  }
				  else
				  {
				      //process被挂起时间到，恢复被挂起的process
					  cpu_time_now = clib_cpu_time_now ();
					  cpu_time_now =
							  dispatch_suspended_process (vm, di, cpu_time_now);
				  }
			  }

			  //指明所有过期timer均被处理
			  _vec_len (nm->data_from_advancing_timing_wheel) = 0;
		  }
      }

      //处理统计，响应main loop退出标记
      vlib_increment_main_loop_counter (vm);

      /* Record time stamp in case there are no enabled nodes and above
         calls do not update time stamp. */
      cpu_time_now = clib_cpu_time_now ();
    }
}

//控制线程或者worker线程loop
static void
vlib_main_loop (vlib_main_t * vm)
{
  vlib_main_or_worker_loop (vm, /* is_main */ 1);
}

//worker线程loop
void
vlib_worker_loop (vlib_main_t * vm)
{
  vlib_main_or_worker_loop (vm, /* is_main */ 0);
}

vlib_main_t vlib_global_main;

static clib_error_t *
vlib_main_configure (vlib_main_t * vm, unformat_input_t * input)
{
  int turn_on_mem_trace = 0;

  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (input, "memory-trace"))
	turn_on_mem_trace = 1;

      else if (unformat (input, "elog-events %d",
			 &vm->elog_main.event_ring_size))
	;
      else if (unformat (input, "elog-post-mortem-dump"))
	vm->elog_post_mortem_dump = 1;
      else
	return unformat_parse_error (input);
    }

  unformat_free (input);

  /* Enable memory trace as early as possible. */
  if (turn_on_mem_trace)
    clib_mem_trace (1);

  return 0;
}

VLIB_EARLY_CONFIG_FUNCTION (vlib_main_configure, "vlib");

static void
dummy_queue_signal_callback (vlib_main_t * vm)
{
}

#define foreach_weak_reference_stub             \
_(vlib_map_stat_segment_init)                   \
_(vpe_api_init)                                 \
_(vlibmemory_init)                              \
_(map_api_segment_init)

//定义空的foreach_weak_reference_stub所有函数
#define _(name)                                                 \
clib_error_t *name (vlib_main_t *vm) __attribute__((weak));     \
clib_error_t *name (vlib_main_t *vm) { return 0; }
foreach_weak_reference_stub;
#undef _

/* Main function. */
int
vlib_main (vlib_main_t * volatile vm, unformat_input_t * input)
{
  clib_error_t *volatile error;
  vlib_node_main_t *nm = &vm->node_main;

  vm->queue_signal_callback = dummy_queue_signal_callback;

  clib_time_init (&vm->clib_time);

  /* Turn on event log. */
  //初始化event log
  if (!vm->elog_main.event_ring_size)
  {
	  vm->elog_main.event_ring_size = 128 << 10;
  }
  elog_init (&vm->elog_main, vm->elog_main.event_ring_size);
  elog_enable_disable (&vm->elog_main, 1);

  /* Default name. */
  //如果未设置，则为其设置默认名称
  if (!vm->name)
  {
	  vm->name = "VLIB";
  }

  //物理内存初始化
  if ((error = vlib_physmem_init (vm)))
  {
      clib_error_report (error);
      goto done;
  }

  if ((error = vlib_map_stat_segment_init (vm)))
  {
      clib_error_report (error);
      goto done;
  }

  if ((error = vlib_buffer_main_init (vm)))
  {
      clib_error_report (error);
      goto done;
  }

  //线程初始化（所有注册的线程将被启动）
  if ((error = vlib_thread_init (vm)))
  {
      clib_error_report (error);
      goto done;
  }

  /* Register static nodes so that init functions may use them. */
  //所有nodes注册
  vlib_register_all_static_nodes (vm);

  /* Set seed for random number generator.
     Allow user to specify seed to make random sequence deterministic. */
  if (!unformat (input, "seed %wd", &vm->random_seed))
  {
    vm->random_seed = clib_cpu_time_now ();
  }
  clib_random_buffer_init (&vm->random_buffer, vm->random_seed);

  /* Initialize node graph. */
  //初始化node间关系
  if ((error = vlib_node_main_init (vm)))
  {
      /* Arrange for graph hook up error to not be fatal when debugging. */
      if (CLIB_DEBUG > 0)
      {
    	  	  clib_error_report (error);
      }
      else
      {
    	  	  goto done;
      }
  }

  /* Direct call / weak reference, for vlib standalone use-cases */
  if ((error = vpe_api_init (vm)))
  {
      clib_error_report (error);
      goto done;
  }

  if ((error = vlibmemory_init (vm)))
  {
      clib_error_report (error);
      goto done;
  }

  if ((error = map_api_segment_init (vm)))
  {
      clib_error_report (error);
      goto done;
  }

  /* See unix/main.c; most likely already set up */
  //初始化init函数查找的hash表
  if (vm->init_functions_called == 0)
  {
	  vm->init_functions_called = hash_create (0, /* value bytes */ 0);
  }

  //调用所有注册的初始化函数
  if ((error = vlib_call_all_init_functions (vm)))
  {
	  goto done;
  }

  //定义定时器
  nm->timing_wheel = clib_mem_alloc_aligned (sizeof (TWT (tw_timer_wheel)),
					     CLIB_CACHE_LINE_BYTES);

  //设置定时器临时缓存buffer
  vec_validate (nm->data_from_advancing_timing_wheel, 10);
  _vec_len (nm->data_from_advancing_timing_wheel) = 0;

  /* Create the process timing wheel */
  //初始化定时器
  TW (tw_timer_wheel_init) ((TWT (tw_timer_wheel) *) nm->timing_wheel,
			    0 /* no callback */ ,
			    10e-6 /* timer period 10us */ ,
			    ~0 /* max expirations per call */ );

  vec_validate (vm->pending_rpc_requests, 0);
  _vec_len (vm->pending_rpc_requests) = 0;
  vec_validate (vm->processing_rpc_requests, 0);
  _vec_len (vm->processing_rpc_requests) = 0;

  //调用所有其它非早期配置函数
  if ((error = vlib_call_all_config_functions (vm, input/*所有模块配置参数*/, 0 /* is_early */ )))
    goto done;

  /* Call all main loop enter functions. */
  {
    clib_error_t *sub_error;
    //触发main loop进入前钩子函数
    sub_error = vlib_call_all_main_loop_enter_functions (vm);
    if (sub_error)
      clib_error_report (sub_error);
  }

  //记录退出点
  switch (clib_setjmp (&vm->main_loop_exit, VLIB_MAIN_LOOP_EXIT_NONE))
  {
    case VLIB_MAIN_LOOP_EXIT_NONE:
      vm->main_loop_exit_set = 1;
      break;

    case VLIB_MAIN_LOOP_EXIT_CLI:
      goto done;//通过cli退出

    default:
      error = vm->main_loop_error;
      goto done;
  }

  //执行main loop函数
  vlib_main_loop (vm);

done:
  /* Call all exit functions. */
  {
    //触发main_loop离开前钩子函数
    clib_error_t *sub_error;
    sub_error = vlib_call_all_main_loop_exit_functions (vm);
    if (sub_error)
      clib_error_report (sub_error);
  }

  if (error)
  {
    clib_error_report (error);
  }

  return 0;
}

static inline clib_error_t *
pcap_dispatch_trace_command_internal (vlib_main_t * vm,
				      unformat_input_t * input,
				      vlib_cli_command_t * cmd, int rx_tx)
{
  unformat_input_t _line_input, *line_input = &_line_input;
  pcap_main_t *pm = &vm->dispatch_pcap_main;
  u8 *filename = 0;
  u32 max = 1000;
  int enabled = 0;
  int is_error = 0;
  clib_error_t *error = 0;
  u32 node_index, add;
  vlib_trace_main_t *tm;
  vlib_trace_node_t *tn;

  /* Get a line of input. */
  if (!unformat_user (input, unformat_line_input, line_input))
    return 0;

  while (unformat_check_input (line_input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (line_input, "on"))
	{
	  if (vm->dispatch_pcap_enable == 0)
	    {
	      enabled = 1;
	    }
	  else
	    {
	      vlib_cli_output (vm, "pcap dispatch capture already on...");
	      is_error = 1;
	      break;
	    }
	}
      else if (unformat (line_input, "off"))
	{
	  if (vm->dispatch_pcap_enable)
	    {
	      vlib_cli_output
		(vm, "captured %d pkts...", pm->n_packets_captured);
	      if (pm->n_packets_captured)
		{
		  pm->n_packets_to_capture = pm->n_packets_captured;
		  error = pcap_write (pm);
		  if (error)
		    clib_error_report (error);
		  else
		    vlib_cli_output (vm, "saved to %s...", pm->file_name);
		}
	      vm->dispatch_pcap_enable = 0;
	    }
	  else
	    {
	      vlib_cli_output (vm, "pcap tx capture already off...");
	      is_error = 1;
	      break;
	    }
	}
      else if (unformat (line_input, "max %d", &max))
	{
	  if (vm->dispatch_pcap_enable)
	    {
	      vlib_cli_output
		(vm,
		 "can't change max value while pcap tx capture active...");
	      is_error = 1;
	      break;
	    }
	  pm->n_packets_to_capture = max;
	}
      else
	if (unformat
	    (line_input, "file %U", unformat_vlib_tmpfile, &filename))
	{
	  if (vm->dispatch_pcap_enable)
	    {
	      vlib_cli_output
		(vm, "can't change file while pcap tx capture active...");
	      is_error = 1;
	      break;
	    }
	}
      else if (unformat (line_input, "status"))
	{
	  if (vm->dispatch_pcap_enable)
	    {
	      vlib_cli_output
		(vm, "pcap dispatch capture is on: %d of %d pkts...",
		 pm->n_packets_captured, pm->n_packets_to_capture);
	      vlib_cli_output (vm, "Capture to file %s", pm->file_name);
	    }
	  else
	    {
	      vlib_cli_output (vm, "pcap dispatch capture is off...");
	    }
	  break;
	}
      else if (unformat (line_input, "buffer-trace %U %d",
			 unformat_vlib_node, vm, &node_index, &add))
	{
	  if (vnet_trace_dummy == 0)
	    vec_validate_aligned (vnet_trace_dummy, 2048,
				  CLIB_CACHE_LINE_BYTES);
	  vlib_cli_output (vm, "Buffer tracing of %d pkts from %U enabled...",
			   add, format_vlib_node_name, vm, node_index);

          /* *INDENT-OFF* */
          foreach_vlib_main ((
            {
              tm = &this_vlib_main->trace_main;
              tm->verbose = 0;  /* not sure this ever did anything... */
              vec_validate (tm->nodes, node_index);
              tn = tm->nodes + node_index;
              tn->limit += add;
              tm->trace_enable = 1;
            }));
          /* *INDENT-ON* */
	}

      else
	{
	  error = clib_error_return (0, "unknown input `%U'",
				     format_unformat_error, line_input);
	  is_error = 1;
	  break;
	}
    }
  unformat_free (line_input);

  if (is_error == 0)
    {
      /* Clean up from previous run */
      vec_free (pm->file_name);
      vec_free (pm->pcap_data);

      memset (pm, 0, sizeof (*pm));
      pm->n_packets_to_capture = max;

      if (enabled)
	{
	  if (filename == 0)
	    filename = format (0, "/tmp/dispatch.pcap%c", 0);

	  pm->file_name = (char *) filename;
	  pm->n_packets_captured = 0;
	  pm->packet_type = PCAP_PACKET_TYPE_vpp;
	  if (pm->lock == 0)
	    clib_spinlock_init (&(pm->lock));
	  vm->dispatch_pcap_enable = 1;
	  vlib_cli_output (vm, "pcap dispatch capture on...");
	}
    }

  return error;
}

static clib_error_t *
pcap_dispatch_trace_command_fn (vlib_main_t * vm,
				unformat_input_t * input,
				vlib_cli_command_t * cmd)
{
  return pcap_dispatch_trace_command_internal (vm, input, cmd, VLIB_RX);
}

/*?
 * This command is used to start or stop pcap dispatch trace capture, or show
 * the capture status.
 *
 * This command has the following optional parameters:
 *
 * - <b>on|off</b> - Used to start or stop capture.
 *
 * - <b>max <nn></b> - Depth of local buffer. Once '<em>nn</em>' number
 *   of packets have been received, buffer is flushed to file. Once another
 *   '<em>nn</em>' number of packets have been received, buffer is flushed
 *   to file, overwriting previous write. If not entered, value defaults
 *   to 100. Can only be updated if packet capture is off.
 *
 * - <b>file <name></b> - Used to specify the output filename. The file will
 *   be placed in the '<em>/tmp</em>' directory, so only the filename is
 *   supported. Directory should not be entered. If file already exists, file
 *   will be overwritten. If no filename is provided, '<em>/tmp/vpe.pcap</em>'
 *   will be used. Can only be updated if packet capture is off.
 *
 * - <b>status</b> - Displays the current status and configured attributes
 *   associated with a packet capture. If packet capture is in progress,
 *   '<em>status</em>' also will return the number of packets currently in
 *   the local buffer. All additional attributes entered on command line
 *   with '<em>status</em>' will be ignored and not applied.
 *
 * @cliexpar
 * Example of how to display the status of capture when off:
 * @cliexstart{pcap dispatch trace status}
 * max is 100, for any interface to file /tmp/vpe.pcap
 * pcap dispatch capture is off...
 * @cliexend
 * Example of how to start a dispatch trace capture:
 * @cliexstart{pcap dispatch trace on max 35 file dispatchTrace.pcap}
 * pcap dispatch capture on...
 * @cliexend
 * Example of how to start a dispatch trace capture with buffer tracing
 * @cliexstart{pcap dispatch trace on max 10000 file dispatchTrace.pcap buffer-trace dpdk-input 1000}
 * pcap dispatch capture on...
 * @cliexend
 * Example of how to display the status of a tx packet capture in progress:
 * @cliexstart{pcap tx trace status}
 * max is 35, dispatch trace to file /tmp/vppTest.pcap
 * pcap tx capture is on: 20 of 35 pkts...
 * @cliexend
 * Example of how to stop a tx packet capture:
 * @cliexstart{vppctl pcap dispatch trace off}
 * captured 21 pkts...
 * saved to /tmp/dispatchTrace.pcap...
 * @cliexend
?*/
/* *INDENT-OFF* */
VLIB_CLI_COMMAND (pcap_dispatch_trace_command, static) = {
    .path = "pcap dispatch trace",
    .short_help =
    "pcap dispatch trace [on|off] [max <nn>] [file <name>] [status]\n"
    "              [buffer-trace <input-node-name> <nn>]",
    .function = pcap_dispatch_trace_command_fn,
};
/* *INDENT-ON* */

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
