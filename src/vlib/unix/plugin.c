/*
 * plugin.c: plugin handling
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

#include <vlib/unix/plugin.h>
#include <vppinfra/elf.h>
#include <dlfcn.h>
#include <dirent.h>

plugin_main_t vlib_plugin_main;

char *vlib_plugin_path __attribute__ ((weak));
char *vlib_plugin_path = "";
char *vlib_plugin_app_version __attribute__ ((weak));
char *vlib_plugin_app_version = "";

//通过插件名称查找插件，然后在插件中查找符号symbol_name
void *
vlib_get_plugin_symbol (char *plugin_name, char *symbol_name)
{
  plugin_main_t *pm = &vlib_plugin_main;
  uword *p;
  plugin_info_t *pi;

  if ((p = hash_get_mem (pm->plugin_by_name_hash, plugin_name)) == 0)
    return 0;

  pi = vec_elt_at_index (pm->plugin_info, p[0]);
  return dlsym (pi->handle, symbol_name);
}

static char *
str_array_to_vec (char *array, int len)
{
  char c, *r = 0;
  int n = 0;

  do
    {
      //将array中的len个元素添加进r中
      c = array[n];
      vec_add1 (r, c);
    }
  while (c && ++n < len);

  //加入'\0'标记字符串结束
  if (c)
    vec_add1 (r, 0);

  return r;
}

//加载插件
static int
load_one_plugin (plugin_main_t * pm, plugin_info_t * pi, int from_early_init)
{
  void *handle;
  clib_error_t *error;
  elf_main_t em = { 0 };
  elf_section_t *section;
  u8 *data;
  char *version_required;
  vlib_plugin_registration_t *reg;
  plugin_config_t *pc = 0;
  uword *p;

  //解析elf文件
  if (elf_read_file (&em, (char *) pi->filename))
    return -1;

  //取elf文件指定section
  error = elf_get_section_by_name (&em, ".vlib_plugin_registration",
				   &section);
  if (error)
    {
      //如果无此section，则认为非规范的插件
      clib_warning ("Not a plugin: %s\n", (char *) pi->name);
      return -1;
    }

  //取section内容
  data = elf_get_section_contents (&em, section->index, 1);
  reg = (vlib_plugin_registration_t *) data;

  //解析体长度检查
  if (vec_len (data) != sizeof (*reg))
    {
      clib_warning ("vlib_plugin_registration size mismatch in plugin %s\n",
		    (char *) pi->name);
      goto error;
    }

  if (pm->plugins_default_disable)
    reg->default_disabled = 1;

  //通过插件名称查找配置索引
  p = hash_get_mem (pm->config_index_by_name, pi->name);
  if (p)
    {
	  //通过配置索引，查找插件配置
      pc = vec_elt_at_index (pm->configs, p[0]);
      if (pc->is_disabled)
	{
	  clib_warning ("Plugin disabled: %s", pi->name);
	  goto error;
	}
      //如果没有明确开启，且默认禁用，则禁用
      if (reg->default_disabled && pc->is_enabled == 0)
	{
	  clib_warning ("Plugin disabled (default): %s", pi->name);
	  goto error;
	}
    }
  else if (reg->default_disabled)
    {
      clib_warning ("Plugin disabled (default): %s", pi->name);
      goto error;
    }

  version_required = str_array_to_vec ((char *) &reg->version_required,
				       sizeof (reg->version_required));

  //检查插件要求的vpp版本(上面花大力气就搞了一个版本检查，但思路是可以用于支持其它自定义module)
  if ((strlen (version_required) > 0) &&
      (strncmp (vlib_plugin_app_version, version_required,
		strlen (version_required))))
    {
      clib_warning ("Plugin %s version mismatch: %s != %s",
		    pi->name, vlib_plugin_app_version, reg->version_required);
      if (!(pc && pc->skip_version_check == 1))
	{
    	  //如果不可跳过版本检查，则报错
	  vec_free (version_required);
	  goto error;
	}
    }

  vec_free (version_required);
  vec_free (data);
  elf_main_free (&em);

  //再通过dl库打开so
  handle = dlopen ((char *) pi->filename, RTLD_LAZY);

  if (handle == 0)
    {
      clib_warning ("%s", dlerror ());
      clib_warning ("Failed to load plugin '%s'", pi->name);
      goto error;
    }

  pi->handle = handle;

  reg = dlsym (pi->handle, "vlib_plugin_registration");

  if (reg == 0)
    {
	  //无vlib_plugin_registration,报错
      /* This should never happen unless somebody chagnes registration macro */
      clib_warning ("Missing plugin registration in plugin '%s'", pi->name);
      dlclose (pi->handle);
      goto error;
    }

  pi->reg = reg;
  pi->version = str_array_to_vec ((char *) &reg->version,
				  sizeof (reg->version));

  //用户指明了init函数名称，则查找并调用
  if (reg->early_init)
    {
      clib_error_t *(*ei) (vlib_main_t *);
      void *h;

      //查找$early_init函数
      h = dlsym (pi->handle, reg->early_init);
      if (h)
	{
	  ei = h;
	  error = (*ei) (pm->vlib_main);//调用初始化函数
	  if (error)
	    {
	      //初始化失败
	      clib_error_report (error);
	      dlclose (pi->handle);
	      goto error;
	    }
	}
      else
	clib_warning ("Plugin %s: early init function %s set but not found",
		      (char *) pi->name, reg->early_init);
    }

  //显示插件描述信息
  if (reg->description)
    clib_warning ("Loaded plugin: %s (%s)", pi->name, reg->description);
  else
    clib_warning ("Loaded plugin: %s", pi->name);

  return 0;
error:
  vec_free (data);
  elf_main_free (&em);
  return -1;
}

//split 插件路径，获得split后的字符串数组
static u8 **
split_plugin_path (plugin_main_t * pm)
{
  int i;
  u8 **rv = 0;
  u8 *path = pm->plugin_path;
  u8 *this = 0;

  for (i = 0; i < vec_len (pm->plugin_path); i++)
    {
      //遇到的非分隔符，则直接将其加入到this中
      if (path[i] != ':')
	{
	  vec_add1 (this, path[i]);
	  continue;
	}
      //遇到':',将this截短
      vec_add1 (this, 0);
      //将字符串this,添加进rv中
      vec_add1 (rv, this);
      //this赋值为null,继续split
      this = 0;
    }
  //达到字符串结尾，将this加入到rv中
  if (this)
    {
      vec_add1 (this, 0);
      vec_add1 (rv, this);
    }
  return rv;
}

//字符串名称比对
static int
plugin_name_sort_cmp (void *a1, void *a2)
{
  plugin_info_t *p1 = a1;
  plugin_info_t *p2 = a2;

  return strcmp ((char *) p1->name, (char *) p2->name);
}

//插件加载
int
vlib_load_new_plugins (plugin_main_t * pm, int from_early_init)
{
  DIR *dp;
  struct dirent *entry;
  struct stat statb;
  uword *p;
  plugin_info_t *pi;
  u8 **plugin_path;
  u32 *load_fail_indices = 0;
  int i;

  //split多个vector插件地址
  plugin_path = split_plugin_path (pm);

  //遍历插件路径，完成插件识别
  for (i = 0; i < vec_len (plugin_path); i++)
    {
      dp = opendir ((char *) plugin_path[i]);

      //打开目录失败，继续
      if (dp == 0)
          continue;

      while ((entry = readdir (dp)))
	{
	  u8 *plugin_name;
	  u8 *filename;

	  //如果plugin_name_filter有值，则只加载plugin_name_filter指定的plugin
	  if (pm->plugin_name_filter)
	    {
	      int j;
	      for (j = 0; j < vec_len (pm->plugin_name_filter); j++)
		if (entry->d_name[j] != pm->plugin_name_filter[j])
		  goto next;
	    }

	  //构造插件名称
	  filename = format (0, "%s/%s%c", plugin_path[i], entry->d_name, 0);

	  /* Only accept .so */
	  //必须为.so文件
	  char *ext = strrchr ((const char *) filename, '.');
	  /* unreadable */
	  if (!ext || (strcmp (ext, ".so") != 0) ||
	      stat ((char *) filename, &statb) < 0)
	    {
	    ignore:
	      vec_free (filename);
	      continue;
	    }

	  //必须为文件
	  /* a dir or other things which aren't plugins */
	  if (!S_ISREG (statb.st_mode))
	    goto ignore;

	  //取插件名称
	  plugin_name = format (0, "%s%c", entry->d_name, 0);
	  /* Have we seen this plugin already? */
	  //查此名称对应的plguin是否存在
	  p = hash_get_mem (pm->plugin_by_name_hash, plugin_name);
	  if (p == 0)
	    {
		  //此插件不存在，注册它
	      /* No, add it to the plugin vector */
	      vec_add2 (pm->plugin_info, pi, 1);
	      pi->name = plugin_name;
	      pi->filename = filename;
	      pi->file_info = statb;
	      hash_set_mem (pm->plugin_by_name_hash, plugin_name,
			    pi - pm->plugin_info);
	    }
	next:
	  ;
	}
      closedir (dp);
      vec_free (plugin_path[i]);
    }
  vec_free (plugin_path);


  /*
   * Sort the plugins by name. This is important.
   * API traces contain absolute message numbers.
   * Loading plugins in directory (vs. alphabetical) order
   * makes trace replay incredibly fragile.
   */
  //按插件名称进行排序
  vec_sort_with_function (pm->plugin_info, plugin_name_sort_cmp);

  /*
   * Attempt to load the plugins
   */
  //装载识别的所有插件
  for (i = 0; i < vec_len (pm->plugin_info); i++)
    {
	  //取$i对应的插件配置
      pi = vec_elt_at_index (pm->plugin_info, i);

      //加载插件
      if (load_one_plugin (pm, pi, from_early_init))
	{
	  /* Make a note of any which fail to load */
          //记录装载失败的
	  vec_add1 (load_fail_indices, i);
	  hash_unset_mem (pm->plugin_by_name_hash, pi->name);
	  vec_free (pi->name);
	  vec_free (pi->filename);
	}
    }

  /* Remove plugin info vector elements corresponding to load failures */
  //删除装载失败的记录
  if (vec_len (load_fail_indices) > 0)
    {
      for (i = vec_len (load_fail_indices) - 1; i >= 0; i--)
	vec_delete (pm->plugin_info, 1, load_fail_indices[i]);
      vec_free (load_fail_indices);
    }

  /* Recreate the plugin name hash */
  for (i = 0; i < vec_len (pm->plugin_info); i++)
    {
      pi = vec_elt_at_index (pm->plugin_info, i);
      hash_unset_mem (pm->plugin_by_name_hash, pi->name);
      //添加插件名称到插件索引的映射
      hash_set_mem (pm->plugin_by_name_hash, pi->name, pi - pm->plugin_info);
    }

  return 0;
}

//装载插件
int
vlib_plugin_early_init (vlib_main_t * vm)
{
  plugin_main_t *pm = &vlib_plugin_main;

  //设置插件路径
  if (pm->plugin_path == 0)
    pm->plugin_path = format (0, "%s%c", vlib_plugin_path, 0);

  clib_warning ("plugin path %s", pm->plugin_path);

  pm->plugin_by_name_hash = hash_create_string (0, sizeof (uword));
  pm->vlib_main = vm;

  return vlib_load_new_plugins (pm, 1 /* from_early_init */ );
}

u8 *
vlib_get_vat_plugin_path (void)
{
  plugin_main_t *pm = &vlib_plugin_main;
  return (pm->vat_plugin_path);
}

u8 *
vlib_get_vat_plugin_name_filter (void)
{
  plugin_main_t *pm = &vlib_plugin_main;
  return (pm->vat_plugin_name_filter);
}

static clib_error_t *
vlib_plugins_show_cmd_fn (vlib_main_t * vm,
			  unformat_input_t * input, vlib_cli_command_t * cmd)
{
  plugin_main_t *pm = &vlib_plugin_main;
  u8 *s = 0;
  u8 *key = 0;
  uword value = 0;
  int index = 1;
  plugin_info_t *pi;

  //显示加载的插件
  s = format (s, " Plugin path is: %s\n\n", pm->plugin_path);
  s = format (s, "     %-41s%-33s%s\n", "Plugin", "Version", "Description");

  /* *INDENT-OFF* */
  //显示插件名称及插件版本，描述信息
  hash_foreach_mem (key, value, pm->plugin_by_name_hash,
    {
      if (key != 0)
        {
          pi = vec_elt_at_index (pm->plugin_info, value);
          s = format (s, "%3d. %-40s %-32s %s\n", index, key, pi->version,
		      pi->reg->description ? pi->reg->description : "");
	  index++;
        }
    });
  /* *INDENT-ON* */

  vlib_cli_output (vm, "%v", s);
  vec_free (s);
  return 0;
}

/* *INDENT-OFF* */
//显示插件信息
VLIB_CLI_COMMAND (plugins_show_cmd, static) =
{
  .path = "show plugins",
  .short_help = "show loaded plugins",
  .function = vlib_plugins_show_cmd_fn,
};
/* *INDENT-ON* */

//解析插件的配置
static clib_error_t *
config_one_plugin (vlib_main_t * vm, char *name, unformat_input_t * input)
{
  plugin_main_t *pm = &vlib_plugin_main;
  plugin_config_t *pc;
  clib_error_t *error = 0;
  uword *p;
  int is_enable = 0;
  int is_disable = 0;
  int skip_version_check = 0;

  if (pm->config_index_by_name == 0)
    pm->config_index_by_name = hash_create_string (0, sizeof (uword));

  //通过名称找插件
  p = hash_get_mem (pm->config_index_by_name, name);

  if (p)
    {
	  //插件已注册，报错
      error = clib_error_return (0, "plugin '%s' already configured", name);
      goto done;
    }

  //自配置中提取，enable,disable,skip-version-check
  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (input, "enable"))
	is_enable = 1;
      else if (unformat (input, "disable"))
	is_disable = 1;
      else if (unformat (input, "skip-version-check"))
	skip_version_check = 1;
      else
	{
	  error = clib_error_return (0, "unknown input '%U'",
				     format_unformat_error, input);
	  goto done;
	}
    }

  if (is_enable && is_disable)
    {
	  //语意错误，两者只能有一种
      error = clib_error_return (0, "please specify either enable or disable"
				 " for plugin '%s'", name);
      goto done;
    }

  //注册$name插件的配置信息
  vec_add2 (pm->configs, pc, 1);
  hash_set_mem (pm->config_index_by_name, name, pc - pm->configs);
  pc->is_enabled = is_enable;
  pc->is_disabled = is_disable;
  pc->skip_version_check = skip_version_check;
  pc->name = name;

done:
  return error;
}

//解析插件总体配置及插件更具体的配置，初始化相应数据
clib_error_t *
vlib_plugin_config (vlib_main_t * vm, unformat_input_t * input)
{
  plugin_main_t *pm = &vlib_plugin_main;
  clib_error_t *error = 0;
  unformat_input_t in;

  unformat_init (&in, 0, 0);

  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      //按"%s %v"格式解析，在v中保存'{','}'之间的内部，在s中保存之前的字符串
      u8 *s, *v;
      if (unformat (input, "%s %v", &s, &v))
	{
          //如果s为plugins,则为in.buffer添加空格分隔的v
	  if (strncmp ((const char *) s, "plugins", 8) == 0)
	    {
	      //将plugins的配置添加到in.buffer中
	      if (vec_len (in.buffer) > 0)
		vec_add1 (in.buffer, ' ');
	      vec_add (in.buffer, v, vec_len (v));
	    }
	}
      else
	{
          //格式有误，报错
	  error = clib_error_return (0, "unknown input '%U'",
				     format_unformat_error, input);
	  goto done;
	}

      vec_free (v);
      vec_free (s);
    }
done:
  //将input指向in,准备解析plugins的配置
  input = &in;
  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      unformat_input_t sub_input;
      u8 *s = 0;
      if (unformat (input, "path %s", &s))
	pm->plugin_path = s;//设置插件地址
      else if (unformat (input, "name-filter %s", &s))
	pm->plugin_name_filter = s;
      else if (unformat (input, "vat-path %s", &s))
	pm->vat_plugin_path = s;
      else if (unformat (input, "vat-name-filter %s", &s))
	pm->vat_plugin_name_filter = s;
      else if (unformat (input, "plugin default %U",
			 unformat_vlib_cli_sub_input, &sub_input))
	{
    	  //插件默认是否disable
	  pm->plugins_default_disable =
	    unformat (&sub_input, "disable") ? 1 : 0;
	  unformat_free (&sub_input);
	}
      else if (unformat (input, "plugin %s %U", &s,
			 unformat_vlib_cli_sub_input, &sub_input))
	{
    	  //解析插件的配置
	  error = config_one_plugin (vm, (char *) s, &sub_input);
	  unformat_free (&sub_input);
	  if (error)
	    goto done2;
	}
      else
	{
	  error = clib_error_return (0, "unknown input '%U'",
				     format_unformat_error, input);
	  {
	    vec_free (s);
	    goto done2;
	  }
	}
    }

done2:
  unformat_free (&in);
  return error;
}

/* discard whole 'plugins' section, as it is already consumed prior to
   plugin load */
static clib_error_t *
plugins_config (vlib_main_t * vm, unformat_input_t * input)
{
  u8 *junk;

  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (input, "%s", &junk))
	{
	  vec_free (junk);
	  return 0;
	}
      else
	return clib_error_return (0, "unknown input '%U'",
				  format_unformat_error, input);
    }
  return 0;
}

VLIB_CONFIG_FUNCTION (plugins_config, "plugins");

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
