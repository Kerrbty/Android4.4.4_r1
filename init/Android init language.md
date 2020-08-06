AIL是Android初始化语言，以下是根据官方文档进行的翻译有助于研究Android启动过程：

Android初始化语言包含了四种类型的声明：

> - Actions（行动）
> - Commands（命令）
> - Services（服务）
> - Options（选项）



该语言的语法包括下列约定：

- 所有类型的语句都是基于行（line-oriented）的， 一个语句包含若干个tokens，token之间通过空格字符分隔. 如果一个token中需要包含空格字符，则需要通过C语言风格的反斜线（'\'）来转义，或者使用双引号把整个token引起来。反斜线还可以出现在一行的末尾，表示下一行的内容仍然属于当前语句。
- 以'#'开始的行是注释行。（前面允许有空格）;
- 动作（Actions）和服务（Services）语句隐含表示一个新的段落（section）的开始。 所有的指令（commands）和选项（options）归属于上方最近的一个段落。在第一个段落之前的指令（commands）和选项（options）是无效的。
- 动作（Actions）和服务（Services）拥有唯一性的名字。如果出现重名，那么后出现的定义将被作为错误忽略掉。

## Actions（行动）

​        Actions其实就是一序列的Commands（命令）。Actions都有一个trigger（触发器），它被用于决定action的执行时间。当一个符合action触发条件的事件发生时，action会被加入到执行队列的末尾，除非它已经在队列里了。
​        队列中的每一个action都被依次提取出，而这个action中的每个command（命令）都将被依次执行。Init在这些命令的执行期间还控制着其他的活动（设备节点的创建和注销、属性的设置、进程的重启）。

Actions的形式如下：

```AIL
on <trigger>
  <command>
  <command>
  <command>
```

## Services（服务）

​        Services（服务）是一个程序，他在初始化时启动，并在退出时重启（可选）。Services（服务）的形式如下：

```AIL
  service <name> <pathname> [ <argument> ]*
  <option>
  <option>
  ...
```

## Options（选项）

​        Options（选项）是一个Services（服务）的修正者。他们影响Services（服务）在何时，并以何种方式运行。

critical（关键）
         说明这是一个对于设备关键的服务。如果他四分钟内退出大于四次，系统将会重启并进入recovery（恢复）模式。

disabled（失效）
 说明这个服务不会同与他同trigger（触发器）下的服务自动启动。他必须被明确的按名启动。

setenv （设置环境变量）
 在进程启动时将环境变量设置为。

socket [ [ ] ]
 创建一个Uinx域的名为/dev/socket/ 的套接字，并传递它的文件描述符给已启动的进程。 必须是 "dgram"或"stream"。User 和 group默认为0。

user
 在启动这个服务前改变该服务的用户名。此时默认为root。（？？？有可能的话应该默认为nobody）。当前，如果你的进程要求Linux capabilities（能力），你无法使用这个命令。即使你是root，你也必须在程序中请求capabilities（能力）。然后降到你想要的uid。

group [ ]*
 在启动这个服务前改变该服务的组名。除了（必需的）第一个组名，附加的组名通常被用于设置进程的补充组（通过setgroups()）。此时默认为root。（？？？有可能的话应该默认为nobody）。

oneshot
 服务退出时不重启。

class
 指定一个服务类。所有同一类的服务可以同时启动和停止。如果不通过class选项指定一个类，则默认为"default"类服务。

onrestart
 当服务重启，执行一个命令（下详）。



## Triggers（触发器）

Triggers（触发器）是一个用于匹配特定事件类型的字符串，用于使Actions（行动）发生。

boot
 这是init执行后的第一个被触发的Triggers（触发器）。（在 /init.conf （启动配置文件）被装载之后）

=
 这种形式的Triggers（触发器）会在属性被设置为指定的时被触发。

device-added-
 device-removed-
 这种形式的Triggers（触发器）会在一个设备节点文件被增删时触发。

service-exited-
 这种形式的Triggers（触发器）会在一个特定的服务退出时触发。

## Commands（命令）

exec [ ]*
 创建和执行一个程序（）。在程序完全执行前，init将会阻塞。由于它不是内置命令，应尽量避免使用exec，它可能会引起init卡死。(??? 是否需要一个超时设置?)

export
 在全局环境变量中设在环境变量 为。（这将会被所有在这命令之后运行的进程所继承）

ifup
 启动网络接口

import
 解析一个init配置文件，扩展当前配置。

hostname
 设置主机名。

chmod
 更改文件访问权限。

chown
 更改文件的所有者和组。

class_start
 启动所有指定服务类下的未运行服务。

class_stop
 停止指定服务类下的所有已运行的服务。

domainname
 设置域名。

insmod
 加载中的模块。

mkdir [mode] [owner] [group]
 创建一个目录，可以选择性地指定mode、owner以及group。如果没有指定，默认的权限为755，并属于root用户和root组。

mount

[ ]*
 试图在目录挂载指定的设备。 可以是以 [mtd@name](https://links.jianshu.com/go?to=http%3A%2F%2Fblog.163.com%2Fkissinger_1984%2Fblog%2Fstatic%2F168992520099121305590%2Fmtd%40name) 的形式指定一个mtd块设备。包括 "ro"、"rw"、"remount"、"noatime"、 ...

setkey
 待完成......（暂时不可用）

setprop
 设置系统属性 为 值.

setrlimit
 设置的rlimit（资源限制）。

start
 启动指定服务（如果此服务还未运行）。

stop
 停止指定服务（如果此服务在运行中）。

symlink
 创建一个指向的软连接。

sysclktz <mins_west_of_gmt>
 设置系统时钟基准（0代表时钟滴答以格林威治平均时（GMT）为准）

trigger
 触发一个事件。用于将一个action与另一个 action排列。（？？？？？）

write [ ]*
 打开路径为的一个文件，并写入一个或多个字符串。

## Properties（属性）

Init更新一些系统属性以提供对正在发生的事件的监控能力:

init.action
 此属性值为正在被执行的action的名字，如果没有则为""。

init.command
 此属性值为正在被执行的command的名字，如果没有则为""。

init.svc.
 名为的service的状态("stopped"（停止）, "running"（运行）, "restarting"（重启）)

## init.rc 实例



```csharp
# not complete -- just providing some examples of usage  
#  
on boot  
   export PATH /sbin:/system/sbin:/system/bin  
   export LD_LIBRARY_PATH /system/lib

   mkdir /dev  
   mkdir /proc  
   mkdir /sys

   mount tmpfs tmpfs /dev  
   mkdir /dev/pts  
   mkdir /dev/socket  
   mount devpts devpts /dev/pts  
   mount proc proc /proc  
   mount sysfs sysfs /sys

   write /proc/cpu/alignment 4

   ifup lo

   hostname localhost  
   domainname localhost

   mount yaffs2 [mtd@system](http://blog.163.com/kissinger_1984/blog/static/168992520099121305590/mtd@system) /system  
   mount yaffs2 [mtd@userdata](http://blog.163.com/kissinger_1984/blog/static/168992520099121305590/mtd@userdata) /data

   import /system/etc/init.conf

   class_start default

service adbd /sbin/adbd  
   user adb  
   group adb

service usbd /system/bin/usbd -r  
   user usbd  
   group usbd  
   socket usbd 666

service zygote /system/bin/app_process -Xzygote /system/bin --zygote  
   socket zygote 666

service runtime /system/bin/runtime  
   user system  
   group system

on device-added-/dev/compass  
   start akmd

on device-removed-/dev/compass  
   stop akmd

service akmd /sbin/akmd  
   disabled  
   user akmd  
   group akmd
```

## 调试记录

在默认情况下，程序在被init执行时会将标准输出和标准错误都重定向到/dev/null（丢弃）。若你想要获得调试信息，你可以通过Andoird系统中的logwrapper程序执行你的程序。它会将标准输出/标准错误都重定向到Android日志系统(通过logcat访问)。

例如：

service akmd /system/bin/logwrapper /sbin/akmd

