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
 * buffer_node.h: VLIB buffer handling node helper macros/inlines
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

#ifndef included_vlib_buffer_node_h
#define included_vlib_buffer_node_h

/** \file
    vlib buffer/node functions
*/

/** \brief Finish enqueueing two buffers forward in the graph.
 Standard dual loop boilerplate element. This is a MACRO,
 with MULTIPLE SIDE EFFECTS. In the ideal case,
 <code>next_index == next0 == next1</code>,
 which means that the speculative enqueue at the top of the dual loop
 has correctly dealt with both packets. In that case, the macro does
 nothing at all.

 @param vm vlib_main_t pointer, varies by thread
 @param node current node vlib_node_runtime_t pointer
 @param next_index speculated next index used for both packets
 @param to_next speculated vector pointer used for both packets
 @param n_left_to_next number of slots left in speculated vector
 @param bi0 first buffer index
 @param bi1 second buffer index
 @param next0 actual next index to be used for the first packet
 @param next1 actual next index to be used for the second packet

 @return @c next_index -- speculative next index to be used for future packets
 @return @c to_next -- speculative frame to be used for future packets
 @return @c n_left_to_next -- number of slots left in speculative frame
*/

#define vlib_validate_buffer_enqueue_x2(vm,node/*本次处理的node*/,next_index/*指出to_next是送给next_index的报文*/,\
to_next/*可填充frame index的数组*/,n_left_to_next/*frame index数组长度*/,\
bi0/*报文index*/,bi1/*报文index*/,next0/*报文bi0的next node索引*/,next1/*报文bi1的next node索引*/) \
do {									\
  /*通过enqueue_code计算两个报文next node索引与next_index的匹配情况*/\
  int enqueue_code = (next0 != next_index) + 2*(next1 != next_index);	\
									\
  if (PREDICT_FALSE (enqueue_code != 0))				\
    {									\
      /*当前情况，存在一个或两个报文的next node索引与next_index相同*/\
      switch (enqueue_code)						\
	{								\
	case 1:								\
	/*next0与next_index不相等*/\
	  /* A B A */							\
      /*当前to_next已预填充，且已跳过，故-2使其回到'B'位置，将B位置更正为A,接下来就需要完成bi0的填充*/\
	  to_next[-2] = bi1;						\
	  to_next -= 1;/*由于bi0不相等，故仅能跳转１格，这里退格*/	\
	  n_left_to_next += 1;/*同样的，我们没有to_next中添充，故n_left_to_next还原１位*/		\
	  /*由于next_index不是bi0适用的vector,故需要重新为next0申请vector并填充，触发添加至pending_frame队列上*/\
	  vlib_set_next_frame_buffer (vm, node, next0, bi0);		\
	  break;							\
									\
	case 2:								\
      /*next0与next_index相等，与0情况处理逻辑大致相同*/\
	  /* A A B */							\
	  to_next -= 1;							\
	  n_left_to_next += 1;						\
	  vlib_set_next_frame_buffer (vm, node, next1, bi1);		\
	  break;							\
									\
	case 3:								\
	  /* A B B or A B C */						\
      /*next0,next1均与next_index不相等，需要考虑next0与next1是否相等的情况*/\
	  to_next -= 2;							\
	  n_left_to_next += 2;						\
	  vlib_set_next_frame_buffer (vm, node, next0, bi0);		\
	  vlib_set_next_frame_buffer (vm, node, next1, bi1);		\
	  if (next0 == next1)						\
	    {								\
	      /*将next_index对应的next_frame推至pending_frame队列上*/\
	      vlib_put_next_frame (vm, node, next_index,		\
				   n_left_to_next);			\
	      next_index = next1;					\
	      vlib_get_next_frame (vm, node, next_index, to_next, n_left_to_next); \
	    }								\
	}								\
    }									\
} while (0)


/** \brief Finish enqueueing four buffers forward in the graph.
 Standard quad loop boilerplate element. This is a MACRO,
 with MULTIPLE SIDE EFFECTS. In the ideal case,
 <code>next_index == next0 == next1 == next2 == next3</code>,
 which means that the speculative enqueue at the top of the quad loop
 has correctly dealt with all four packets. In that case, the macro does
 nothing at all.

 @param vm vlib_main_t pointer, varies by thread
 @param node current node vlib_node_runtime_t pointer
 @param next_index speculated next index used for both packets
 @param to_next speculated vector pointer used for both packets
 @param n_left_to_next number of slots left in speculated vector
 @param bi0 first buffer index
 @param bi1 second buffer index
 @param bi2 third buffer index
 @param bi3 fourth buffer index
 @param next0 actual next index to be used for the first packet
 @param next1 actual next index to be used for the second packet
 @param next2 actual next index to be used for the third packet
 @param next3 actual next index to be used for the fourth packet

 @return @c next_index -- speculative next index to be used for future packets
 @return @c to_next -- speculative frame to be used for future packets
 @return @c n_left_to_next -- number of slots left in speculative frame
*/

#define vlib_validate_buffer_enqueue_x4(vm,node,next_index,to_next,n_left_to_next,bi0,bi1,bi2,bi3,next0,next1,next2,next3) \
do {                                                                    \
  /* After the fact: check the [speculative] enqueue to "next" */       \
  u32 fix_speculation = (next_index ^ next0) | (next_index ^ next1)     \
    | (next_index ^ next2) | (next_index ^ next3);                      \
  if (PREDICT_FALSE(fix_speculation))                                   \
    {                                                                   \
      /* rewind... */                                                   \
      to_next -= 4;                                                     \
      n_left_to_next += 4;                                              \
                                                                        \
      /* If bi0 belongs to "next", send it there */                     \
      if (next_index == next0)                                          \
        {                                                               \
          to_next[0] = bi0;                                             \
          to_next++;                                                    \
          n_left_to_next --;                                            \
        }                                                               \
      else              /* send it where it needs to go */              \
        vlib_set_next_frame_buffer (vm, node, next0, bi0);              \
                                                                        \
      if (next_index == next1)                                          \
        {                                                               \
          to_next[0] = bi1;                                             \
          to_next++;                                                    \
          n_left_to_next --;                                            \
        }                                                               \
      else                                                              \
        vlib_set_next_frame_buffer (vm, node, next1, bi1);              \
                                                                        \
      if (next_index == next2)                                          \
        {                                                               \
          to_next[0] = bi2;                                             \
          to_next++;                                                    \
          n_left_to_next --;                                            \
        }                                                               \
      else                                                              \
        vlib_set_next_frame_buffer (vm, node, next2, bi2);              \
                                                                        \
      if (next_index == next3)                                          \
        {                                                               \
          to_next[0] = bi3;                                             \
          to_next++;                                                    \
          n_left_to_next --;                                            \
        }                                                               \
      else                                                              \
        {                                                               \
          vlib_set_next_frame_buffer (vm, node, next3, bi3);            \
                                                                        \
          /* Change speculation: last 2 packets went to the same node*/ \
          if (next2 == next3)                                           \
            {                                                           \
              vlib_put_next_frame (vm, node, next_index, n_left_to_next); \
              next_index = next3;                                       \
              vlib_get_next_frame (vm, node, next_index, to_next, n_left_to_next); \
            }                                                           \
	}                                                               \
    }                                                                   \
 } while(0);

/** \brief Finish enqueueing one buffer forward in the graph.
 Standard single loop boilerplate element. This is a MACRO,
 with MULTIPLE SIDE EFFECTS. In the ideal case,
 <code>next_index == next0</code>,
 which means that the speculative enqueue at the top of the single loop
 has correctly dealt with the packet in hand. In that case, the macro does
 nothing at all.

 @param vm vlib_main_t pointer, varies by thread
 @param node current node vlib_node_runtime_t pointer
 @param next_index speculated next index used for both packets
 @param to_next speculated vector pointer used for both packets
 @param n_left_to_next number of slots left in speculated vector
 @param bi0 first buffer index
 @param next0 actual next index to be used for the first packet

 @return @c next_index -- speculative next index to be used for future packets
 @return @c to_next -- speculative frame to be used for future packets
 @return @c n_left_to_next -- number of slots left in speculative frame
*/
#define vlib_validate_buffer_enqueue_x1(vm,node,next_index/*预测的next node索引，可能不准确*/,to_next,n_left_to_next/*to_next的剩余空间*/,bi0,next0/*指出next node*/) \
do {									\
  if (PREDICT_FALSE (next0 != next_index))				\
    {									\
      /*如果next_index已有报文，则触发其入队到pending_frame队列*/\
      vlib_put_next_frame (vm, node, next_index, n_left_to_next + 1);	\
      /*更新为实际的next_index，这里为什么没有对to_next执行减一操作？（上式已设置n_vectors，下次还调用vlib_get_next_frame获取，故不影响）*/\
      next_index = next0;						\
      /*重新考虑next_index对应的to_next,n_left_to_next,并执行填充*/\
      vlib_get_next_frame (vm, node, next_index, to_next, n_left_to_next); \
									\
	  /*添加frame index*/\
      to_next[0] = bi0;							\
      to_next += 1;							\
      n_left_to_next -= 1;						\
    }									\
} while (0)

always_inline uword
generic_buffer_node_inline (vlib_main_t * vm,
			    vlib_node_runtime_t * node,
			    vlib_frame_t * frame,
			    uword sizeof_trace,
			    void *opaque1,
			    uword opaque2,
			    void (*two_buffers) (vlib_main_t * vm,
						 void *opaque1,
						 uword opaque2,
						 vlib_buffer_t * b0,
						 vlib_buffer_t * b1,
						 u32 * next0, u32 * next1),
			    void (*one_buffer) (vlib_main_t * vm,
						void *opaque1, uword opaque2,
						vlib_buffer_t * b0,
						u32 * next0))
{
  u32 n_left_from, *from, *to_next;
  u32 next_index;

  from = vlib_frame_vector_args (frame);
  n_left_from = frame->n_vectors;
  next_index = node->cached_next_index;

  if (node->flags & VLIB_NODE_FLAG_TRACE)
    vlib_trace_frame_buffers_only (vm, node, from, frame->n_vectors,
				   /* stride */ 1, sizeof_trace);

  while (n_left_from > 0)
    {
      u32 n_left_to_next;

      vlib_get_next_frame (vm, node, next_index, to_next, n_left_to_next);

      while (n_left_from >= 4 && n_left_to_next >= 2)
	{
	  vlib_buffer_t *p0, *p1;
	  u32 pi0, next0;
	  u32 pi1, next1;

	  /* Prefetch next iteration. */
	  {
	    vlib_buffer_t *p2, *p3;

	    p2 = vlib_get_buffer (vm, from[2]);
	    p3 = vlib_get_buffer (vm, from[3]);

	    vlib_prefetch_buffer_header (p2, LOAD);
	    vlib_prefetch_buffer_header (p3, LOAD);

	    CLIB_PREFETCH (p2->data, 64, LOAD);
	    CLIB_PREFETCH (p3->data, 64, LOAD);
	  }

	  pi0 = to_next[0] = from[0];
	  pi1 = to_next[1] = from[1];
	  from += 2;
	  to_next += 2;
	  n_left_from -= 2;
	  n_left_to_next -= 2;

	  p0 = vlib_get_buffer (vm, pi0);
	  p1 = vlib_get_buffer (vm, pi1);

	  two_buffers (vm, opaque1, opaque2, p0, p1, &next0, &next1);

	  vlib_validate_buffer_enqueue_x2 (vm, node, next_index,
					   to_next, n_left_to_next,
					   pi0, pi1, next0, next1);
	}

      while (n_left_from > 0 && n_left_to_next > 0)
	{
	  vlib_buffer_t *p0;
	  u32 pi0, next0;

	  pi0 = from[0];
	  to_next[0] = pi0;
	  from += 1;
	  to_next += 1;
	  n_left_from -= 1;
	  n_left_to_next -= 1;

	  p0 = vlib_get_buffer (vm, pi0);

	  one_buffer (vm, opaque1, opaque2, p0, &next0);

	  vlib_validate_buffer_enqueue_x1 (vm, node, next_index,
					   to_next, n_left_to_next,
					   pi0, next0);
	}

      vlib_put_next_frame (vm, node, next_index, n_left_to_next);
    }

  return frame->n_vectors;
}

static_always_inline void
vlib_buffer_enqueue_to_next (vlib_main_t * vm, vlib_node_runtime_t * node,
			     u32 * buffers/*报文索引*/, u16 * nexts/*报文去向哪个node*/, uword count/*报文数量*/)
{
  u32 *to_next, n_left_to_next, max;
  u16 next_index;

  //取出下一级index
  next_index = nexts[0];
  //取可供下一级index使用的to_next buffer及可使用的buffer长度
  vlib_get_next_frame (vm, node, next_index, to_next, n_left_to_next);
  //获得可存入的报文数
  max = clib_min (n_left_to_next, count);

  while (count)
    {
      u32 n_enqueued;
      if ((nexts[0] != next_index) || n_left_to_next == 0)
	{
      //与之前的不相等或者不足以存放frame了
      //申请新的frame
	  vlib_put_next_frame (vm, node, next_index, n_left_to_next);
	  next_index = nexts[0];
	  vlib_get_next_frame (vm, node, next_index, to_next, n_left_to_next);
	  max = clib_min (n_left_to_next, count);
	}
#if defined(CLIB_HAVE_VEC512)
      u16x32 next32 = u16x32_load_unaligned (nexts);
      next32 = (next32 == u16x32_splat (next32[0]));
      u64 bitmap = u16x32_msb_mask (next32);
      n_enqueued = count_trailing_zeros (~bitmap);
#elif defined(CLIB_HAVE_VEC256)
      u16x16 next16 = u16x16_load_unaligned (nexts);
      next16 = (next16 == u16x16_splat (next16[0]));
      u64 bitmap = u8x32_msb_mask ((u8x32) next16);
      n_enqueued = count_trailing_zeros (~bitmap) / 2;
#elif defined(CLIB_HAVE_VEC128) && defined(CLIB_HAVE_VEC128_MSB_MASK)
      u16x8 next8 = u16x8_load_unaligned (nexts);
      next8 = (next8 == u16x8_splat (next8[0]));
      u64 bitmap = u8x16_msb_mask ((u8x16) next8);
      n_enqueued = count_trailing_zeros (~bitmap) / 2;
#else
      u16 x = 0;
      if (count + 3 < max)
	{
	  x |= next_index ^ nexts[1];
	  x |= next_index ^ nexts[2];
	  x |= next_index ^ nexts[3];
	  n_enqueued = (x == 0) ? 4 : 1;
	}
      else
	n_enqueued = 1;
#endif

      if (PREDICT_FALSE (n_enqueued > max))
	n_enqueued = max;

#ifdef CLIB_HAVE_VEC512
      if (n_enqueued >= 32)
	{
	  vlib_buffer_copy_indices (to_next, buffers, 32);
	  nexts += 32;
	  to_next += 32;
	  buffers += 32;
	  n_left_to_next -= 32;
	  count -= 32;
	  max -= 32;
	  continue;
	}
#endif

#ifdef CLIB_HAVE_VEC256
      if (n_enqueued >= 16)
	{
	  vlib_buffer_copy_indices (to_next, buffers, 16);
	  nexts += 16;
	  to_next += 16;
	  buffers += 16;
	  n_left_to_next -= 16;
	  count -= 16;
	  max -= 16;
	  continue;
	}
#endif

#ifdef CLIB_HAVE_VEC128
      if (n_enqueued >= 8)
	{
	  vlib_buffer_copy_indices (to_next, buffers, 8);
	  nexts += 8;
	  to_next += 8;
	  buffers += 8;
	  n_left_to_next -= 8;
	  count -= 8;
	  max -= 8;
	  continue;
	}
#endif

      if (n_enqueued >= 4)
	{
      //优化，最好情况下直接将此4个给to_next
	  vlib_buffer_copy_indices (to_next, buffers, 4);
	  nexts += 4;
	  to_next += 4;
	  buffers += 4;
	  n_left_to_next -= 4;
	  count -= 4;
	  max -= 4;
	  continue;
	}

      /* copy */
      //将报文交给next node
      to_next[0] = buffers[0];

      /* next */
      nexts += 1;
      to_next += 1;
      buffers += 1;
      n_left_to_next -= 1;
      count -= 1;
      max -= 1;
    }
  vlib_put_next_frame (vm, node, next_index, n_left_to_next);
}

static_always_inline void
vlib_buffer_enqueue_to_single_next (vlib_main_t * vm,
				    vlib_node_runtime_t * node, u32 * buffers,
				    u16 next_index, u32 count)
{
  u32 *to_next, n_left_to_next, n_enq;

  vlib_get_next_frame (vm, node, next_index, to_next, n_left_to_next);

  if (PREDICT_TRUE (n_left_to_next >= count))
    {
      vlib_buffer_copy_indices (to_next, buffers, count);
      n_left_to_next -= count;
      vlib_put_next_frame (vm, node, next_index, n_left_to_next);
      return;
    }

  n_enq = n_left_to_next;
next:
  vlib_buffer_copy_indices (to_next, buffers, n_enq);
  n_left_to_next -= n_enq;

  if (PREDICT_FALSE (count > n_enq))
    {
      count -= n_enq;
      buffers += n_enq;

      vlib_put_next_frame (vm, node, next_index, n_left_to_next);
      vlib_get_next_frame (vm, node, next_index, to_next, n_left_to_next);
      n_enq = clib_min (n_left_to_next, count);
      goto next;
    }
  vlib_put_next_frame (vm, node, next_index, n_left_to_next);
}

//将一组报文入队到frame queue中，thread_indices分别指出了报文所属的线程，drop_on_congestion用于指出队列为满时，是否丢弃
//当前drop_on_congestion需要为1，因为无法返回不丢弃的报文
static_always_inline u32
vlib_buffer_enqueue_to_thread (vlib_main_t * vm, u32 frame_queue_index/*指出队列index*/,
			       u32 * buffer_indices/*指出N个包*/, u16 * thread_indices/*指出每个包所属的线程*/,
			       u32 n_packets, int drop_on_congestion/*队列为满时是否丢包*/)
{
  vlib_thread_main_t *tm = vlib_get_thread_main ();
  vlib_frame_queue_main_t *fqm;
  vlib_frame_queue_per_thread_data_t *ptd;
  u32 n_left = n_packets;
  u32 drop_list[VLIB_FRAME_SIZE], *dbi = drop_list, n_drop = 0;
  vlib_frame_queue_elt_t *hf = 0;
  u32 n_left_to_next_thread = 0, *to_next_thread = 0;
  u32 next_thread_index, current_thread_index = ~0;
  int i;

  //通过队列索引取得队列管理，并取得当前线程对应的私有数据
  fqm = vec_elt_at_index (tm->frame_queue_mains, frame_queue_index);
  ptd = vec_elt_at_index (fqm->per_thread_data, vm->thread_index);

  //使用循环将n_packets个报文（buffer_indices指出）写入到frame_queue_index中
  //thread_indices指出了这此报文（buffer_indices)分别对应的是哪个线程
  while (n_left)
  {
      next_thread_index = thread_indices[0];

      if (next_thread_index != current_thread_index)
	{
          //检查队列是否congrested,如果拥挤，则丢包
	  if (drop_on_congestion &&
	      is_vlib_frame_queue_congested
	      (frame_queue_index, next_thread_index, fqm->queue_hi_thresh,
	       ptd->congested_handoff_queue_by_thread_index))
	  {
	      dbi[0] = buffer_indices[0];
	      dbi++;
	      n_drop++;
	      goto next;
	    }
	  vlib_mains[next_thread_index]->check_frame_queues = 1;

	  if (hf)
	    hf->n_vectors = VLIB_FRAME_SIZE - n_left_to_next_thread;

	  //自队列中取一个element,准备写入
	  hf = vlib_get_worker_handoff_queue_elt (frame_queue_index,
						  next_thread_index,
						  ptd->handoff_queue_elt_by_thread_index);

	  //element中可写入的数目
	  n_left_to_next_thread = VLIB_FRAME_SIZE - hf->n_vectors;
	  //element中可写入的首个索引
	  to_next_thread = &hf->buffer_index[hf->n_vectors];

	  //记录thread_index，用于优化
	  current_thread_index = next_thread_index;
	}

      //写入
      to_next_thread[0] = buffer_indices[0];
      to_next_thread++;
      n_left_to_next_thread--;

      //element已经被消耗光了，将elem标记为满，准备下一次申请新的element
      if (n_left_to_next_thread == 0)
	{
	  hf->n_vectors = VLIB_FRAME_SIZE;
	  vlib_put_frame_queue_elt (hf);//标明元素写入完成
	  current_thread_index = ~0;
	  ptd->handoff_queue_elt_by_thread_index[next_thread_index] = 0;
	  hf = 0;
	}

      /* next */
      //开始下一个节点的写入
    next:
      thread_indices += 1;
      buffer_indices += 1;
      n_left -= 1;
    }

  //设置最后一个hf中入队的元素数
  if (hf)
    hf->n_vectors = VLIB_FRAME_SIZE - n_left_to_next_thread;

  /* Ship frames to the thread nodes */
  //指明元素有效，以使读者可读取
  for (i = 0; i < vec_len (ptd->handoff_queue_elt_by_thread_index); i++)
    {
      if (ptd->handoff_queue_elt_by_thread_index[i])
	{
	  hf = ptd->handoff_queue_elt_by_thread_index[i];
	  /*
	   * It works better to let the handoff node
	   * rate-adapt, always ship the handoff queue element.
	   */
	  if (1 || hf->n_vectors == hf->last_n_vectors)
	    {
	      vlib_put_frame_queue_elt (hf);
	      ptd->handoff_queue_elt_by_thread_index[i] = 0;
	    }
	  else
	    hf->last_n_vectors = hf->n_vectors;
	}
      ptd->congested_handoff_queue_by_thread_index[i] =
	(vlib_frame_queue_t *) (~0);
    }

  //按要求，对因为队列满，丢掉需要丢包的包文
  if (drop_on_congestion && n_drop)
    vlib_buffer_free (vm, drop_list, n_drop);

  //返回入队的报文数
  return n_packets - n_drop;
}

#endif /* included_vlib_buffer_node_h */

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
