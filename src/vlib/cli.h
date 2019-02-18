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
 * cli.h: command line interface
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

#ifndef included_vlib_cli_h
#define included_vlib_cli_h

#include <vppinfra/format.h>

struct vlib_cli_command_t;

typedef struct
{
  u32 min_char;

  /* Indexed by name[position] - min_char. */
  uword **bitmaps;
} vlib_cli_parse_position_t;

typedef struct
{
  u8 *name;

  u32 index;
} vlib_cli_sub_command_t;

typedef struct
{
  u8 *name;

  u32 rule_index;

  u32 command_index;
} vlib_cli_sub_rule_t;

typedef struct
{
  char *name;
  char *short_help;
  char *long_help;

  /* Number of bytes in parsed data.  Zero for vector. */
  uword data_size;

  unformat_function_t *unformat_function;

  /* Opaque for unformat function. */
  uword unformat_function_arg[2];
} vlib_cli_parse_rule_t;

/* CLI command callback function. */
//命令行回调函数
typedef clib_error_t *(vlib_cli_command_function_t)
  (struct vlib_main_t * vm,
   unformat_input_t * input, struct vlib_cli_command_t * cmd);

typedef struct vlib_cli_command_t
{
  /* Command path (e.g. "show something").
     Spaces delimit elements of path. */
  char *path;//命令行

  /* Short/long help strings. */
  char *short_help;//短的帮助信息
  char *long_help;//长的帮助信息

  /* Callback function. */
  vlib_cli_command_function_t *function;//命令行实现

  /* Opaque. */
  uword function_arg;

  /* Known MP-safe? */
  uword is_mp_safe;

  /* Sub commands for this command. */
  vlib_cli_sub_command_t *sub_commands;

  /* Hash table mapping name (e.g. last path element) to sub command index. */
  uword *sub_command_index_by_name;

  /* bitmap[p][c][i] says whether sub-command i has character
     c in position p. */
  vlib_cli_parse_position_t *sub_command_positions;

  /* Hash table mapping name (e.g. last path element) to sub rule index. */
  uword *sub_rule_index_by_name;

  /* Vector of possible parse rules for this path. */
  vlib_cli_sub_rule_t *sub_rules;

  /* List of CLI commands, built by constructors */
  struct vlib_cli_command_t *next_cli_command;//指向下一条cli command

} vlib_cli_command_t;

typedef void (vlib_cli_output_function_t) (uword arg,
					   u8 * buffer, uword buffer_bytes);
typedef struct
{
  /* Vector of all known commands. */
  vlib_cli_command_t *commands;

  /* Hash table mapping normalized path to index into all_commands. */
  uword *command_index_by_path;

  /* Vector of all known parse rules. */
  vlib_cli_parse_rule_t *parse_rules;

  /* Hash table mapping parse rule name to index into parse_rule vector. */
  uword *parse_rule_index_by_name;

  /* Data parsed for rules. */
  void **parse_rule_data;

  /* registration list added by constructors */
  vlib_cli_command_t *cli_command_registrations;//指向注册的命令
} vlib_cli_main_t;

#ifndef CLIB_MARCH_VARIANT
/**
 * 定义一个vlib_cli_command_t类型的x，并定义一个x的注册
 * 函数，在注册函数中，将x变量挂接在cli_command_registrations上（此值来自于vm->cli_main)
 * 挂接时总是在第一个位置挂接。
 * 最后一行，我们初始化定义的x
 */
#define VLIB_CLI_COMMAND(x,...)                                         \
    /*声明cli命令变量*/\
    __VA_ARGS__ vlib_cli_command_t x;                                   \
    /*定义注册函数*/\
static void __vlib_cli_command_registration_##x (void)                  \
    __attribute__((__constructor__)) ;                                  \
static void __vlib_cli_command_registration_##x (void)                  \
{                                                                       \
    vlib_main_t * vm = vlib_get_main();                                 \
    vlib_cli_main_t *cm = &vm->cli_main;                                \
    x.next_cli_command = cm->cli_command_registrations;                 \
    cm->cli_command_registrations = &x;                                 \
}                                                                       \
static void __vlib_cli_command_unregistration_##x (void)                \
    __attribute__((__destructor__)) ;                                   \
static void __vlib_cli_command_unregistration_##x (void)                \
{                                                                       \
    vlib_main_t * vm = vlib_get_main();                                 \
    vlib_cli_main_t *cm = &vm->cli_main;                                \
    VLIB_REMOVE_FROM_LINKED_LIST (cm->cli_command_registrations, &x,    \
                                  next_cli_command);                    \
}                                                                       \
/*对定义的cli命令进行赋值*/                                                 \
__VA_ARGS__ vlib_cli_command_t x
#else
/* create unused pointer to silence compiler warnings and get whole
   function optimized out */
#define VLIB_CLI_COMMAND(x,...)                                         \
static __clib_unused vlib_cli_command_t __clib_unused_##x
#endif

#define VLIB_CLI_PARSE_RULE(x) \
  vlib_cli_parse_rule_t x
/* Output to current CLI connection. */
void vlib_cli_output (struct vlib_main_t *vm, char *fmt, ...);

/* Process CLI input. */
void vlib_cli_input (struct vlib_main_t *vm,
		     unformat_input_t * input,
		     vlib_cli_output_function_t * function,
		     uword function_arg);

clib_error_t *vlib_cli_register (struct vlib_main_t *vm,
				 vlib_cli_command_t * c);
clib_error_t *vlib_cli_register_parse_rule (struct vlib_main_t *vm,
					    vlib_cli_parse_rule_t * c);

uword unformat_vlib_cli_sub_input (unformat_input_t * i, va_list * args);

/* Return an vector of strings consisting of possible auto-completions
 * for a given input string */
u8 **vlib_cli_get_possible_completions (u8 * input_str);

#endif /* included_vlib_cli_h */

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
