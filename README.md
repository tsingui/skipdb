# skipdbv2

rewrite and fix for https://github.com/stevedekorte/skipdb

嵌入式 k/v 数据库(Clone to Xiaobao)，可代替nvram，并有nvram更强大的功能。[详细说明](http://koolshare.cn/thread-4850-1-1.html)
为了在嵌入式下更少的占用jffs的空间。建议最多使用配置条目为1w。merlin正常使用nvram的配置条目为2000条，当jffs占用大于8M时，会进行一次refresh，重新计算空间。

# 介绍
* skipd 为小型的k/v数据库服务端
* dbus 为k/v数据库的客户端

# 大小
[root@build]$ du -sh ./bin/skipd ./bin/dbus
* 300K        ./bin/skipd
* 12K        ./bin/dbus
* 因为它非常小，所以引入merlin可认为是很廉价的。除了体积小，它占用内存也不多。

# 速度
* 写15000条数据需要6s
* time ./test.sh #测试脚本在tests目录
* real	0m6.697s

# 实现功能有：

* dbus set key=value #保存
* dbus ram key=value #保存到内存
* dbus replace key=value #替换
* dbus get key #读取
* dbus list key #读取所有
* dbus delay key tick path_of_shell.sh
* dbus time key H:M:S path_of_shell.sh
* dbus export key #将配置导入到脚本
* dbus update key #将脚本配置保存到数据库
* dbus inc key=value #增加数值
* dbus desc key=value #减去数值
* dbus event name path_of_shell.sh #注册一个事件脚本
* dbus fire name #触发一个事件脚本

# 例子
* dbus delay name 60 /jffs/scripts/update_ddns.sh
可以保存一项配置到jffs，同时会每隔60秒自动运行一次/jffs/scipts/update_ddns.sh。
因信息保存到数据库，系统重启也不会丢失。

* dbus time name2 20:20:00 /jffs/scripts/reboot.sh
能自动在晚上8:20运行一个reboot.sh脚本。如果在脚本里对设备进行重启，系统就自动重启。
同样，信息保存到数据库，重启之后也会在第二天的8点20分钟重启。

* 写入dbus set ddns_xx
* dbus set ddns_ip=8.8.8.8
* dbus set ddns_port=88
* dbus set ddns_username=user
* dbus set ddns_password=pass

通过dbus list ddns读取所有以"ddns"开头信息：
* ddns_ip=8.8.8.8
* ddns_port=88
* ddns_username=user
* ddns_password=pass

# 编译方法
* 依赖库 libev，需要自行编译或安装
* cd skipdb
* mkdir build
* cd build
* cmake ..
* make

# 执行方法
* skipd -d /path/of/data
* dbus command params
