#include <signal.h>          // 信号处理头文件（用于进程终止信号）
#include <linux/config.h>    // 内核配置头文件（定义硬件相关常量）
#include <linux/head.h>      // 内核头文件（定义页表结构）
#include <linux/kernel.h>    // 内核基础头文件（包含printk等函数）
#include <asm/system.h>      // 系统头文件（定义内联汇编相关）

int do_exit(long code);      // 声明进程退出函数

// 刷新TLB（通过写CR3寄存器强制刷新页表缓存）
#define invalidate() \
__asm__("movl %%eax,%%cr3"::"a" (0))

// 定义低端内存起始地址（1MB或缓冲区末端）
// 若缓冲区结束地址小于1MB，则低端内存从1MB开始
// 否则从缓冲区结束地址开始
#if (BUFFER_END < 0x100000)
#define LOW_MEM 0x100000
#else
#define LOW_MEM BUFFER_END
#endif


/* 以下宏基于上述定义计算 */
#define PAGING_MEMORY (HIGH_MEMORY - LOW_MEM) // 可分页内存总量
#define PAGING_PAGES (PAGING_MEMORY/4096)      // 总页数（每页4KB）
#define MAP_NR(addr) (((addr)-LOW_MEM)>>12)    // 物理地址转mem_map数组下标

// 若可分页数小于10，编译时报错（内存过小无法运行）
#if (PAGING_PAGES < 10)
#error "Won't work"
#endif

// 快速复制4KB页（使用rep movsl指令，一次复制4字节，循环1024次）
#define copy_page(from,to) \
__asm__("cld ; rep ; movsl"::"S" (from),"D" (to),"c" (1024):"cx","di","si")

// 全局物理页帧状态数组（0=空闲，>0=引用计数）
static unsigned short mem_map [ PAGING_PAGES ] = {0,};

/*
 * Get physical address of first (actually last :-) free page, and mark it
 * used. If no free pages left, return 0.
 */
// ### 分配空闲物理页 ###
unsigned long get_free_page(void) {
    register unsigned long __res asm("ax"); // 将结果存储在ax寄存器

    // 内联汇编：从mem_map尾部逆向扫描寻找空闲页
    __asm__(
        "std ; repne ; scasw\n\t"  // 逆向扫描mem_map（方向标志DF=1）
        "jne 1f\n\t"               // 未找到跳转标签1（返回0）
        "movw $1,2(%%edi)\n\t"      // 找到空闲页，标记为已使用（设置mem_map[i]=1）
        "sall $12,%%ecx\n\t"        // 左移12位（乘以4096）计算页基址偏移
        "movl %%ecx,%%edx\n\t"      // 将偏移存入edx
        "addl %2,%%edx\n\t"         // 加上LOW_MEM得到物理地址
        "movl $1024,%%ecx\n\t"      // 准备清零页（1024次写操作）
        "leal 4092(%%edx),%%edi\n\t"// edx+4092作为目标地址（页末尾）
        "rep ; stosl\n\t"           // 用stosl将页内容清零（eax=0）
        "movl %%edx,%%eax\n"        // 结果存入eax（返回物理地址）
        "1:"                        // 标签1（未找到页时直接返回0）
        :"=a" (__res)               // 输出：结果通过ax寄存器返回
        :"0" (0),"i" (LOW_MEM),"c" (PAGING_PAGES), "D" (mem_map+PAGING_PAGES-1)
        :"di","cx","dx"             // 破坏的寄存器列表
    );
    return __res; // 返回分配的物理地址（0表示失败）
}

/*
 * Free a page of memory at physical address 'addr'. Used by
 * 'free_page_tables()'
 */
// ### 释放物理页 ###
void free_page(unsigned long addr) {
    if (addr<LOW_MEM) return; // 低端内存（内核区）不允许释放
    if (addr>HIGH_MEMORY)      // 地址超过物理内存上限
        panic("trying to free nonexistent page"); // 触发内核错误
    
    addr -= LOW_MEM;          // 计算相对于LOW_MEM的偏移
    addr >>= 12;              // 右移12位得到页号
    if (mem_map[addr]--) return; // 减少引用计数，若仍>0则返回
    mem_map[addr]=0;           // 引用计数归零，标记为空闲
    panic("trying to free free page"); // 重复释放触发错误
}

/*
 * This function frees a continuos block of page tables, as needed
 * by 'exit()'. As does copy_page_tables(), this handles only 4Mb blocks.
 */
// ### 释放连续页表块 ###
int free_page_tables(unsigned long from, unsigned long size) {
    unsigned long *pg_table;
    unsigned long *dir, nr;

    // 检查地址对齐（必须4MB对齐）
    if (from & 0x3fffff) // 低22位必须为0（4MB边界）
        panic("free_page_tables called with wrong alignment");
    if (!from) // 不能释放内核空间（from=0）
        panic("Trying to free up swapper memory space");
    
    size = (size + 0x3fffff) >> 22; // 计算需要释放的页目录项数（每项4MB）

    dir = (unsigned long *) ((from>>20) & 0xffc); // 计算起始页目录项指针
    for ( ; size-->0 ; dir++) {     // 遍历每个页目录项
        if (!(1 & *dir)) continue;   // 页目录项未使用则跳过
        pg_table = (unsigned long *) (0xfffff000 & *dir); // 获取页表地址
        for (nr=0 ; nr<1024 ; nr++) { // 遍历页表项
            if (1 & *pg_table)       // 页表项存在（P=1）
                free_page(0xfffff000 & *pg_table); // 释放对应物理页
            *pg_table = 0;           // 清空页表项
            pg_table++;
        }
        free_page(0xfffff000 & *dir); // 释放页表本身占用的页
        *dir = 0;                    // 清空页目录项
    }
    invalidate(); // 刷新TLB
    return 0;
}

/*
 *  Well, here is one of the most complicated functions in mm. It
 * copies a range of linerar addresses by copying only the pages.
 * Let's hope this is bug-free, 'cause this one I don't want to debug :-)
 *
 * Note! We don't copy just any chunks of memory - addresses have to
 * be divisible by 4Mb (one page-directory entry), as this makes the
 * function easier. It's used only by fork anyway.
 *
 * NOTE 2!! When from==0 we are copying kernel space for the first
 * fork(). Then we DONT want to copy a full page-directory entry, as
 * that would lead to some serious memory waste - we just copy the
 * first 160 pages - 640kB. Even that is more than we need, but it
 * doesn't take any more memory - we don't copy-on-write in the low
 * 1 Mb-range, so the pages can be shared with the kernel. Thus the
 * special case for nr=xxxx.
 */
int copy_page_tables(unsigned long from,unsigned long to,long size)
{
	unsigned long * from_page_table;
	unsigned long * to_page_table;
	unsigned long this_page;
	unsigned long * from_dir, * to_dir;
	unsigned long nr;

	if ((from&0x3fffff) || (to&0x3fffff))
		panic("copy_page_tables called with wrong alignment");
	from_dir = (unsigned long *) ((from>>20) & 0xffc); /* _pg_dir = 0 */
	to_dir = (unsigned long *) ((to>>20) & 0xffc);
	size = ((unsigned) (size+0x3fffff)) >> 22;
	for( ; size-->0 ; from_dir++,to_dir++) {
		if (1 & *to_dir)
			panic("copy_page_tables: already exist");
		if (!(1 & *from_dir))
			continue;
		from_page_table = (unsigned long *) (0xfffff000 & *from_dir);
		if (!(to_page_table = (unsigned long *) get_free_page()))
			return -1;	/* Out of memory, see freeing */
		*to_dir = ((unsigned long) to_page_table) | 7;
		nr = (from==0)?0xA0:1024;
		for ( ; nr-- > 0 ; from_page_table++,to_page_table++) {
			this_page = *from_page_table;
			if (!(1 & this_page))
				continue;
			this_page &= ~2;
			*to_page_table = this_page;
			if (this_page > LOW_MEM) {
				*from_page_table = this_page;
				this_page -= LOW_MEM;
				this_page >>= 12;
				mem_map[this_page]++;
			}
		}
	}
	invalidate();
	return 0;
}

/*
 * This function puts a page in memory at the wanted address.
 * It returns the physical address of the page gotten, 0 if
 * out of memory (either when trying to access page-table or
 * page.)
 */
unsigned long put_page(unsigned long page,unsigned long address)
{
	unsigned long tmp, *page_table;

/* NOTE !!! This uses the fact that _pg_dir=0 */

	if (page < LOW_MEM || page > HIGH_MEMORY)
		printk("Trying to put page %p at %p\n",page,address);
	if (mem_map[(page-LOW_MEM)>>12] != 1)
		printk("mem_map disagrees with %p at %p\n",page,address);
	page_table = (unsigned long *) ((address>>20) & 0xffc);
	if ((*page_table)&1)
		page_table = (unsigned long *) (0xfffff000 & *page_table);
	else {
		if (!(tmp=get_free_page()))
			return 0;
		*page_table = tmp|7;
		page_table = (unsigned long *) tmp;
	}
	page_table[(address>>12) & 0x3ff] = page | 7;
	return page;
}

void un_wp_page(unsigned long * table_entry)
{
	unsigned long old_page,new_page;

	old_page = 0xfffff000 & *table_entry;
	if (old_page >= LOW_MEM && mem_map[MAP_NR(old_page)]==1) {
		*table_entry |= 2;
		return;
	}
	if (!(new_page=get_free_page()))
		do_exit(SIGSEGV);
	if (old_page >= LOW_MEM)
		mem_map[MAP_NR(old_page)]--;
	*table_entry = new_page | 7;
	copy_page(old_page,new_page);
}	

/*
 * This routine handles present pages, when users try to write
 * to a shared page. It is done by copying the page to a new address
 * and decrementing the shared-page counter for the old page.
 */
void do_wp_page(unsigned long error_code,unsigned long address)
{
	un_wp_page((unsigned long *)
		(((address>>10) & 0xffc) + (0xfffff000 &
		*((unsigned long *) ((address>>20) &0xffc)))));

}

void write_verify(unsigned long address)
{
	unsigned long page;

	if (!( (page = *((unsigned long *) ((address>>20) & 0xffc)) )&1))
		return;
	page &= 0xfffff000;
	page += ((address>>10) & 0xffc);
	if ((3 & *(unsigned long *) page) == 1)  /* non-writeable, present */
		un_wp_page((unsigned long *) page);
	return;
}

void do_no_page(unsigned long error_code,unsigned long address)
{
	unsigned long tmp;

	if (tmp=get_free_page())
		if (put_page(tmp,address))
			return;
	do_exit(SIGSEGV);
}

void calc_mem(void)
{
	int i,j,k,free=0;
	long * pg_tbl;

	for(i=0 ; i<PAGING_PAGES ; i++)
		if (!mem_map[i]) free++;
	printk("%d pages free (of %d)\n\r",free,PAGING_PAGES);
	for(i=2 ; i<1024 ; i++) {
		if (1&pg_dir[i]) {
			pg_tbl=(long *) (0xfffff000 & pg_dir[i]);
			for(j=k=0 ; j<1024 ; j++)
				if (pg_tbl[j]&1)
					k++;
			printk("Pg-dir[%d] uses %d pages\n",i,k);
		}
	}
}
