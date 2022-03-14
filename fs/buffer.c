/*
 *  linux/fs/buffer.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *  'buffer.c' implements the buffer-cache functions. Race-conditions have
 * been avoided by NEVER letting a interrupt change a buffer (except for the
 * data, of course), but instead letting the caller do it. NOTE! As interrupts
 * can wake up a caller, some cli-sti sequences are needed to check for
 * sleep-on-calls. These should be extremely quick, though (I hope).
 */

/*
 * NOTE! There is one discordant note here: checking floppies for
 * disk change. This is where it fits best, I think, as it should
 * invalidate changed floppy-disk-caches.
 */

#include <stdarg.h>
 
#include <linux/config.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/system.h>
#include <asm/io.h>

extern int end;
struct buffer_head * start_buffer = (struct buffer_head *) &end; //设置高速缓冲区首地址为end处
struct buffer_head * hash_table[NR_HASH]; //哈希表
static struct buffer_head * free_list; //设置空闲链表头
static struct task_struct * buffer_wait = NULL; //等待空闲缓冲块而睡眠的队列
int NR_BUFFERS = 0;

static inline void wait_on_buffer(struct buffer_head * bh) //等待当前缓冲区完成读写操作，此时lock为1 //多线程中同步操作
{
	cli(); //禁止中断发生
	while (bh->b_lock) //若此时缓冲区被锁住，将当前进程置于深度睡眠状态 //必须等待其他进程完成当前缓冲区的操作才可进行下一步
		sleep_on(&bh->b_wait);
	sti(); //允许中断发生
}

int sys_sync(void) //同步高速缓存区与磁盘块的内容
{
	int i;
	struct buffer_head * bh;

	sync_inodes();		/* write out inodes into buffers */
	bh = start_buffer;
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		wait_on_buffer(bh);
		if (bh->b_dirt)
			ll_rw_block(WRITE,bh); 
	}
	return 0;
}

int sync_dev(int dev)
{
	int i;
	struct buffer_head * bh;

	bh = start_buffer;
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		if (bh->b_dev != dev)
			continue;
		wait_on_buffer(bh);
		if (bh->b_dev == dev && bh->b_dirt)
			ll_rw_block(WRITE,bh);
	}
	sync_inodes();
	bh = start_buffer;
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		if (bh->b_dev != dev)
			continue;
		wait_on_buffer(bh);
		if (bh->b_dev == dev && bh->b_dirt)
			ll_rw_block(WRITE,bh);
	}
	return 0;
}

void inline invalidate_buffers(int dev)
{
	int i;
	struct buffer_head * bh;

	bh = start_buffer;
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		if (bh->b_dev != dev)
			continue;
		wait_on_buffer(bh);
		if (bh->b_dev == dev)
			bh->b_uptodate = bh->b_dirt = 0;
	}
}

/*
 * This routine checks whether a floppy has been changed, and
 * invalidates all buffer-cache-entries in that case. This
 * is a relatively slow routine, so we have to try to minimize using
 * it. Thus it is called only upon a 'mount' or 'open'. This
 * is the best way of combining speed and utility, I think.
 * People changing diskettes in the middle of an operation deserve
 * to loose :-)
 *
 * NOTE! Although currently this is only for floppies, the idea is
 * that any additional removable block-device will use this routine,
 * and that mount/open needn't know that floppies/whatever are
 * special.
 */
void check_disk_change(int dev)
{
	int i;

	if (MAJOR(dev) != 2)
		return;
	if (!floppy_change(dev & 0x03))
		return;
	for (i=0 ; i<NR_SUPER ; i++)
		if (super_block[i].s_dev == dev)
			put_super(super_block[i].s_dev);
	invalidate_inodes(dev);
	invalidate_buffers(dev);
}

#define _hashfn(dev,block) (((unsigned)(dev^block))%NR_HASH) /* 根据设备号与块号，获取哈希表散列值，即哈希表的位置  */
#define hash(dev,block) hash_table[_hashfn(dev,block)]       /* 哈希表中具体的位置*/

static inline void remove_from_queues(struct buffer_head * bh)
{
/* remove from hash-queue */ //从哈希表中移出 
	if (bh->b_next)
		bh->b_next->b_prev = bh->b_prev;
	if (bh->b_prev)
		bh->b_prev->b_next = bh->b_next;
	if (hash(bh->b_dev,bh->b_blocknr) == bh)
		hash(bh->b_dev,bh->b_blocknr) = bh->b_next;
/* remove from free list */ //从空闲链表中移出
	if (!(bh->b_prev_free) || !(bh->b_next_free))
		panic("Free block list corrupted");
	bh->b_prev_free->b_next_free = bh->b_next_free;
	bh->b_next_free->b_prev_free = bh->b_prev_free;
	if (free_list == bh)
		free_list = bh->b_next_free;
}

static inline void insert_into_queues(struct buffer_head * bh)
{
/* put at end of free list */ //将其移入空闲链表中，插入空闲链表的末尾
	bh->b_next_free = free_list;
	bh->b_prev_free = free_list->b_prev_free;
	free_list->b_prev_free->b_next_free = bh;
	free_list->b_prev_free = bh;
/* put the buffer in new hash-queue if it has a device */
	bh->b_prev = NULL;
	bh->b_next = NULL;
	if (!bh->b_dev)
		return;
	bh->b_next = hash(bh->b_dev,bh->b_blocknr);//将其插入哈希表对应散列项头部
	hash(bh->b_dev,bh->b_blocknr) = bh;
	bh->b_next->b_prev = bh;
}

static struct buffer_head * find_buffer(int dev, int block) //find_buffer函数
{		
	struct buffer_head * tmp;

	for (tmp = hash(dev,block) ; tmp != NULL ; tmp = tmp->b_next) //查找相应散列值下的双向链表，是否找到与其相同的设备与块
		if (tmp->b_dev==dev && tmp->b_blocknr==block)
			return tmp;
	return NULL;
}

/*
 * Why like this, I hear you say... The reason is race-conditions.
 * As we don't lock buffers (unless we are readint them, that is),
 * something might happen to it while we sleep (ie a read-error
 * will force it bad). This shouldn't really happen currently, but
 * the code is ready.
 */
struct buffer_head * get_hash_table(int dev, int block)
{
	struct buffer_head * bh;

	for (;;) {
		if (!(bh=find_buffer(dev,block)))  //如果没有找到
			return NULL;
		bh->b_count++; //如果块在缓冲区中，将其count+1;
		wait_on_buffer(bh); //判断此块是否在进行读写
		if (bh->b_dev == dev && bh->b_blocknr == block) //再次判断设备号与块号是否一致
			return bh; //是，返回bh
		bh->b_count--;
	}
}

/*
 * Ok, this is getblk, and it isn't very clear, again to hinder
 * race-conditions. Most of the code is seldom used, (ie repeating),
 * so it should be much more efficient than it looks.
 *
 * The algoritm is changed: hopefully better, and an elusive bug removed.
 */
//getblk函数
#define BADNESS(bh) (((bh)->b_dirt<<1)+(bh)->b_lock) //设置权限，dirt大于lock
struct buffer_head * getblk(int dev,int block)
{
	struct buffer_head * tmp, * bh;

repeat:
	if (bh = get_hash_table(dev,block))  //如果在hash表中找到此缓存块，则返回其buffer_head
		return bh;
	//如果不存在，则在空闲链表中找到一个空闲块
	tmp = free_list; //循环链表头为frist_list
	do {
		if (tmp->b_count) //判断此缓冲区是否被使用
			continue; //被使用，则跳出
		if (!bh || BADNESS(tmp)<BADNESS(bh)) {
			bh = tmp;
			if (!BADNESS(tmp)) //如果此temp的权重为0,即找到的buffer_head中的b_dirt与b_lock为0，高速缓冲区为干净的缓冲区
				break;
		}
/* and repeat until we find something good */
	} while ((tmp = tmp->b_next_free) != free_list);
	if (!bh) {   //如果没找到，
		sleep_on(&buffer_wait); //该进程进入深度睡眠，进行等待 等待队列实现方式
		goto repeat; //进程激活后重新检索，因为可能睡眠的过程中，此缓冲块被其他进程使用
	}
	wait_on_buffer(bh);  //如果上锁，等待解锁
	if (bh->b_count) //再次确保等待过程中高速缓冲区未被使用
		goto repeat;
	while (bh->b_dirt) {
		sync_dev(bh->b_dev); //如果空间是脏的话，同步（数据回写操作）
		wait_on_buffer(bh);
		if (bh->b_count)  //再次判断此缓冲块是否被其他进程占用
			goto repeat;
	}
/* NOTE!! While we slept waiting for this block, somebody else might */
/* already have added "this" block to the cache. check it */
	if (find_buffer(dev,block)) //再次去哈希表中查找此块是否已经存在（因为此进程执行过程中，高速缓冲区被其他进程占用）
		goto repeat;
/* OK, FINALLY we know that this buffer is the only one of it's kind, */
/* and that it's unused (b_count=0), unlocked (b_lock=0), and clean */
//确定此高速缓冲区可用，进行下列操作
	bh->b_count=1;
	bh->b_dirt=0;
	bh->b_uptodate=0;
	remove_from_queues(bh); //将此缓冲块从原来的哈希和空闲链表中移出
	bh->b_dev=dev;
	bh->b_blocknr=block;
	insert_into_queues(bh);//将此缓冲块添加到新的哈希表与循环链表中
	return bh;
}

void brelse(struct buffer_head * buf) //释放指定的高速缓存区
{
	if (!buf)
		return;
	wait_on_buffer(buf);
	if (!(buf->b_count--))
		panic("Trying to free free buffer");
	wake_up(&buffer_wait);  //唤醒等待队列的进程
}

/*
 * bread() reads a specified block and returns the buffer that contains
 * it. It returns NULL if the block was unreadable.
 */
struct buffer_head * bread(int dev,int block) //从设备上读取指定的数据块，并返回含有数据的高速缓冲区块
{
	struct buffer_head * bh;

	if (!(bh=getblk(dev,block)))
		panic("bread: getblk returned NULL\n");
	if (bh->b_uptodate)
		return bh;
	ll_rw_block(READ,bh); //通过块设备读写函数，进行块设备与缓冲块的同步
	wait_on_buffer(bh);
	if (bh->b_uptodate)  //再次判断缓冲块是否有效（即块设备数据是否写入缓冲块）
		return bh;
	brelse(bh);
	return NULL;
}

#define COPYBLK(from,to) \
__asm__("cld\n\t" \
	"rep\n\t" \
	"movsl\n\t" \
	::"c" (BLOCK_SIZE/4),"S" (from),"D" (to) \
	:"cx","di","si")

/*
 * bread_page reads four buffers into memory at the desired address. It's
 * a function of its own, as there is some speed to be got by reading them
 * all at the same time, not waiting for one to be read, and then another
 * etc.
 */
void bread_page(unsigned long address,int dev,int b[4])
{
	struct buffer_head * bh[4];
	int i;

	for (i=0 ; i<4 ; i++)
		if (b[i]) {
			if (bh[i] = getblk(dev,b[i]))
				if (!bh[i]->b_uptodate)
					ll_rw_block(READ,bh[i]);
		} else
			bh[i] = NULL;
	for (i=0 ; i<4 ; i++,address += BLOCK_SIZE)
		if (bh[i]) {
			wait_on_buffer(bh[i]);
			if (bh[i]->b_uptodate)
				COPYBLK((unsigned long) bh[i]->b_data,address);
			brelse(bh[i]);
		}
}

/*
 * Ok, breada can be used as bread, but additionally to mark other
 * blocks for reading as well. End the argument list with a negative
 * number.
 */
struct buffer_head * breada(int dev,int first, ...) 
{
	va_list args;
	struct buffer_head * bh, *tmp;

	va_start(args,first);
	if (!(bh=getblk(dev,first)))
		panic("bread: getblk returned NULL\n");
	if (!bh->b_uptodate)
		ll_rw_block(READ,bh);
	while ((first=va_arg(args,int))>=0) {
		tmp=getblk(dev,first);
		if (tmp) {
			if (!tmp->b_uptodate)
				ll_rw_block(READA,bh);
			tmp->b_count--;
		}
	}
	va_end(args);
	wait_on_buffer(bh);
	if (bh->b_uptodate)
		return bh;
	brelse(bh);
	return (NULL);
}

void buffer_init(long buffer_end) //高速缓冲区初始化程序，实现哈希表与循环链表的创建
{
	struct buffer_head * h = start_buffer;
	void * b;
	int i;

	if (buffer_end == 1<<20)
		b = (void *) (640*1024);
	else
		b = (void *) buffer_end;
	while ( (b -= BLOCK_SIZE) >= ((void *) (h+1)) ) {
		h->b_dev = 0;
		h->b_dirt = 0;
		h->b_count = 0;
		h->b_lock = 0;
		h->b_uptodate = 0;
		h->b_wait = NULL;
		h->b_next = NULL;
		h->b_prev = NULL;
		h->b_data = (char *) b;
		h->b_prev_free = h-1;
		h->b_next_free = h+1;
		h++;
		NR_BUFFERS++;
		if (b == (void *) 0x100000)
			b = (void *) 0xA0000;
	}
	h--;
	free_list = start_buffer;
	free_list->b_prev_free = h;
	h->b_next_free = free_list;
	for (i=0;i<NR_HASH;i++)
		hash_table[i]=NULL;
}	
