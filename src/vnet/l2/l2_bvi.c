/*
 * l2_bvi.c : layer 2 Bridged Virtual Interface
 *
 * Copyright (c) 2013 Cisco and/or its affiliates.
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

#include <vlib/vlib.h>
#include <vnet/vnet.h>
#include <vnet/l2/l2_fwd.h>
#include <vnet/l2/l2_flood.h>
#include <vnet/l2/l2_bvi.h>


/* Call the L2 nodes that need the ethertype mapping */
void
l2bvi_register_input_type (vlib_main_t * vm,
			   ethernet_type_t type, u32 node_index)
{
  //为l2fwd注册可处理的next node
  l2fwd_register_input_type (vm, type, node_index);
  //为l2flood注册可处理的next node
  l2flood_register_input_type (vm, type, node_index);
}

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
