/*
 * plugin.h: plugin handling
 *
 * Copyright (c) 2011 Cisco and/or its affiliates.
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

#ifndef __included_plugin_h__
#define __included_plugin_h__

#include <vlib/vlib.h>
#include <vlib/unix/unix.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * vlib plugin scheme
 *
 * Almost anything which can be made to work in a vlib unix
 * application will also work in a vlib plugin.
 *
 * The elf-section magic which registers static objects
 * works so long as plugins are preset when the vlib unix process
 * starts. But wait: there's more...
 *
 * If an application calls vlib_load_new_plugins() -- possibly after
 * changing vlib_plugin_main.plugin_path / vlib_plugin_main.plugin_name_filter,
 * -- new plugins will be loaded. That, in turn, allows considerable
 * flexibility in terms of adding feature code or fixing bugs without
 * requiring the data-plane process to restart.
 *
 * When the plugin mechanism loads a plugin, it uses dlsym to locate
 * and call the plugin's function vlib_plugin_register() if it exists.
 * A plugin which expects to be loaded after the vlib application
 * starts uses this callback to modify the application. If vlib_plugin_register
 * returns non-zero, the plugin mechanism dlclose()'s the plugin.
 *
 * Applications control the plugin search path and name filter by
 * declaring the variables vlib_plugin_path and vlib_plugin_name_filter.
 * libvlib.la supplies weak references for these symbols which
 * effectively disable the scheme. In order for the elf-section magic to
 * work, static plugins must be loaded at the earliest possible moment.
 *
 * An application can change these parameters at any time and call
 * vlib_load_new_plugins().
 */

/* *INDENT-OFF* */
typedef CLIB_PACKED(struct {
  u8 default_disabled;//是否默认禁用
  const char version[32];//自身版本
  const char version_required[32];//要求的vpp版本
  const char *early_init;//初始化函数名称
  const char *description;//插件描述信息
}) vlib_plugin_registration_t;
/* *INDENT-ON* */

typedef struct
{
  u8 *name;//插件名称
  u8 *filename;//插件路径地址
  struct stat file_info;//插件路径stat
  void *handle;//插件so handle

  /* plugin registration */
  vlib_plugin_registration_t *reg;//插件提供的注册参数
  char *version;
} plugin_info_t;

typedef struct
{
  char *name;//插件名称
  u8 is_disabled;//是否禁用
  u8 is_enabled;//是否开启
  u8 skip_version_check;//是否跳过版本检查
} plugin_config_t;

typedef struct
{
  /* loaded plugin info */
  plugin_info_t *plugin_info;//数字,可通过index获得plugin_info
  uword *plugin_by_name_hash;//通过插件名称查找插件索引的hashtable

  /* paths and name filters */
  u8 *plugin_path;//插件路径
  u8 *plugin_name_filter;//如果此值非０，则仅加载此值对应的plugin
  u8 *vat_plugin_path;
  u8 *vat_plugin_name_filter;
  u8 plugins_default_disable;//是否默认disable插件

  /* plugin configs and hash by name */
  plugin_config_t *configs;//按插件名称查找插件配置
  uword *config_index_by_name;//按插件名称查找插件配置索引

  /* usual */
  vlib_main_t *vlib_main;
} plugin_main_t;

extern plugin_main_t vlib_plugin_main;

clib_error_t *vlib_plugin_config (vlib_main_t * vm, unformat_input_t * input);
int vlib_plugin_early_init (vlib_main_t * vm);
int vlib_load_new_plugins (plugin_main_t * pm, int from_early_init);
void *vlib_get_plugin_symbol (char *plugin_name, char *symbol_name);
u8 *vlib_get_vat_plugin_path (void);

//定义plugin注册变量，并使编译器将其置入相应section，并初始化
#define VLIB_PLUGIN_REGISTER() \
  vlib_plugin_registration_t vlib_plugin_registration \
  __attribute__((__section__(".vlib_plugin_registration")))

/* Call a plugin init function: used for init function dependencies. */
#define vlib_call_plugin_init_function(vm,p,x)                  \
({                                                              \
  clib_error_t *(*_f)(vlib_main_t *);                           \
  uword *_fptr = 0;                                             \
  clib_error_t * _error = 0;                                    \
  _fptr= vlib_get_plugin_symbol                                 \
    (p, CLIB_STRING_MACRO(_vlib_init_function_##x));            \
  if (_fptr == 0)                                               \
    {                                                           \
      _error = clib_error_return                                \
        (0, "Plugin %s and/or symbol %s not found.",            \
         p, CLIB_STRING_MACRO(_vlib_init_function_##x));        \
    }                                                           \
  else                                                          \
    {                                                           \
      _f = (void *)(_fptr[0]);                                  \
    }                                                           \
  if (_fptr && ! hash_get (vm->init_functions_called, _f))      \
    {                                                           \
      hash_set1 (vm->init_functions_called, _f);                \
      _error = _f (vm);                                         \
    }                                                           \
  _error;                                                       \
 })

#endif /* __included_plugin_h__ */

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
