#
# Makefile for linux.
# If you don't have '-mstring-insns' in your gcc (and nobody but me has :-)
# remove them from the CFLAGS defines.
#
# 1. 分层构建：通过递归 make 管理子模块。
# 2. ​混合工具链：16/32 位工具链协作生成可启动镜像。
# 3. ​手工依赖管理：通过 dep 规则动态生成头文件依赖。


### 工具定义 ###
AS86	= as -0 -a      # 16位汇编器（用于实模式引导程序）
CC86	= cc -0        # 16位C编译器
LD86	= ld -0        # 16位链接器
AS	= gas             # GNU汇编器（用于保护模式）
LD	= gld             # GNU链接器
CC	= gcc             # GNU C编译器
CPP	= gcc -E -nostdinc -Iinclude  # 预处理工具（忽略标准头文件路径）

#################
# 历史背景：早期 Linux 使用 gas（GNU Assembler）和 gld（GNU Linker）
# 而非现代 as/ld，部分选项（如 -0）针对 16 位代码优化。
#################

### 链接器选项 ###
LDFLAGS	=-s -x -M  
# -s：生成紧凑的可执行文件。
# -x：生成可执行文件。
# -M：生成包含符号表的可执行文件。
# 注：-M 选项通常用于调试目的，生成的文件包含了符号表信息，便于调试器定位和分析代码。

### 编译选项 ###
CFLAGS	= -Wall -O -fstrength-reduce -fomit-frame-pointer -fcombine-regs
# -Wall：启用所有警告（代码规范性检查）。
# -O：基础优化（现代版本中已废弃，现用 -O1）。
# -fstrength-reduce：强度折减优化（循环和表达式简化）。
# -fomit-frame-pointer：省略帧指针（提升性能，牺牲调试信息）。
# -fcombine-regs：寄存器组合优化（减少寄存器使用）。


ARCHIVES=kernel/kernel.o mm/mm.o fs/fs.o # 子模块目标文件
LIBS	=lib/lib.a # 静态库文件

### 通用规则 ###
# $<：第一个依赖文件（如 main.c）。
# $*：去除后缀的文件名（如 main.c → main）。
# -nostdinc：禁止包含标准头文件路径，仅使用 -Iinclude 指定目录。
# 从 .c 生成 .s 汇编文件
.c.s:
	$(CC) $(CFLAGS) -nostdinc -Iinclude -S -o $*.s $<
# 从 .s 生成 .o 目标文件
.s.o:
	$(AS) -c -o $*.o $<
# 从 .c 直接生成 .o 目标文件
.c.o:
	$(CC) $(CFLAGS) -nostdinc -Iinclude -c -o $*.o $<

### 核心目标链 ###
all: Image                # 默认目标：生成最终内核镜像
Image: boot/boot tools/system tools/build  # 依赖引导程序、内核主体和构建工具
	tools/build boot/boot tools/system > Image  # 合并引导程序与内核
	sync                   # 强制写入磁盘（确保镜像完整性）
# tools/build：将引导程序（boot/boot）和内核主体（tools/system）拼接为可启动镜像 Image。
# ​依赖传递：tools/system 进一步依赖子模块目标文件（如 kernel/kernel.o）。
# ​工具链：使用自定义工具 tools/build 来生成最终镜像。
# ​同步：确保 Image 文件写入磁盘，以确保文件系统的一致性。
# ​注：Image 是最终生成的可启动镜像，包含引导程序和内核主体。
# ​sync 命令用于强制将文件系统缓冲区中的数据写入磁盘，确保数据的完整性。

tools/build: tools/build.c
	$(CC) $(CFLAGS) \
	-o tools/build tools/build.c
	chmem +65000 tools/build

boot/head.o: boot/head.s ## 生成引导程序头文件

tools/system:	boot/head.o init/main.o \
		$(ARCHIVES) $(LIBS)
	$(LD) $(LDFLAGS) boot/head.o init/main.o \
	$(ARCHIVES) \
	$(LIBS) \
	-o tools/system > System.map

### 子模块递归构建 ###
# ​模块化设计：各子系统（如 kernel、mm）独立编译，通过 make 递归调用自身 Makefile，符合早期 Linux 的代码组织风格
kernel/kernel.o:
	(cd kernel; make)     # 进入子目录构建内核模块
mm/mm.o:
	(cd mm; make)         # 内存管理模块
fs/fs.o:
	(cd fs; make)         # 文件系统模块
lib/lib.a:
	(cd lib; make)        # 静态库构建

### 引导程序构建 ###
# ​SYSSIZE 计算：通过 ls -l 获取 tools/system 大小，计算内核占用的磁盘扇区数（+15 用于向上取整）。
# ​多阶段编译：引导程序需用 16 位工具链（as86/ld86）编译，兼容实模式运行。
boot/boot: boot/boot.s tools/system
	# 计算内核大小并嵌入到引导程序中
	(echo -n "SYSSIZE = ("; ls -l tools/system | grep system | cut -c25-31 | tr '\012' ' '; echo "+ 15 ) / 16") > tmp.s
	cat boot/boot.s >> tmp.s
	$(AS86) -o boot/boot.o tmp.s    # 汇编生成目标文件
	$(LD86) -s -o boot/boot boot/boot.o  # 链接为16位可执行文件

### 清理规则 ###
# ​作用：删除所有生成文件（如目标文件、镜像），确保构建环境干净
clean:
	rm -f Image System.map tmp_make boot/boot core
	rm -f init/*.o boot/*.o tools/system tools/build
	(cd mm; make clean)    # 递归清理子模块
	(cd fs; make clean)
	(cd kernel; make clean)
	(cd lib; make clean)

backup: clean
	(cd .. ; tar cf - linux | compress16 - > backup.Z)
	sync

### 依赖生成 ###
# 生成 Makefile 依赖：通过预处理工具（gcc -E）生成依赖关系，用于自动构建
# 使用 gcc -M 生成 init/*.c 的头文件依赖关系，追加到 Makefile 的 ### Dependencies 部分。
# 确保头文件修改时触发重新编译。
dep:
	sed '/### Dependencies/q' < Makefile > tmp_make
	(for i in init/*.c; do echo -n "init/"; $(CPP) -M $$i; done) >> tmp_make
	cp tmp_make Makefile
	(cd fs; make dep)      # 递归生成子模块依赖
	(cd kernel; make dep)
	(cd mm; make dep)

### Dependencies:
init/main.o : init/main.c include/unistd.h include/sys/stat.h \
  include/sys/types.h include/sys/times.h include/sys/utsname.h \
  include/utime.h include/time.h include/linux/tty.h include/termios.h \
  include/linux/sched.h include/linux/head.h include/linux/fs.h \
  include/linux/mm.h include/asm/system.h include/asm/io.h include/stddef.h \
  include/stdarg.h include/fcntl.h
