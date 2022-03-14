/*
 *  linux/fs/inode.c
 *
 *  (C) 1991  Linus Torvalds
 */
//处理inode节点的函数
#include <string.h>
#include <sys/stat.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <asm/system.h>

struct m_inode inode_table[NR_INODE]={{0,},}; //inode数组 应该对应磁盘中的inode节点

static void read_inode(struct m_inode * inode);
static void write_inode(struct m_inode * inode);

static inline void wait_on_inode(struct m_inode * inode) //等待inode解锁
{
	cli();
	while (inode->i_lock)
		sleep_on(&inode->i_wait);
	sti();
}

static inline void lock_inode(struct m_inode * inode) //给inode上锁
{
	cli();
	while (inode->i_lock)
		sleep_on(&inode->i_wait);
	inode->i_lock=1;
	sti();
}

static inline void unlock_inode(struct m_inode * inode) //解锁inode
{
	inode->i_lock=0;
	wake_up(&inode->i_wait);
}

void invalidate_inodes(int dev) //释放所有inode节点
{
	int i;
	struct m_inode * inode;

	inode = 0+inode_table;
	for(i=0 ; i<NR_INODE ; i++,inode++) {
		wait_on_inode(inode);
		if (inode->i_dev == dev) {
			if (inode->i_count)
				printk("inode in use on removed disk\n\r");
			inode->i_dev = inode->i_dirt = 0;
		}
	}
}

void sync_inodes(void) //inode节点同步
{
	int i;
	struct m_inode * inode;

	inode = 0+inode_table;
	for(i=0 ; i<NR_INODE ; i++,inode++) {
		wait_on_inode(inode);
		if (inode->i_dirt && !inode->i_pipe) //inode节点是脏的且不是管道文件
			write_inode(inode); //写盘,将其写入高速缓冲区中
	}
}

//一个文件的块号是0~7+512+512*512-1，此函数是找到对应的文件块号对应的磁盘的逻辑块号，同时若此文件块号没有创建，则创建
static int _bmap(struct m_inode * inode,int block,int create) //进行inode节点的磁盘块映射（就是对结构体的磁盘映射变量赋值）
//如果有磁盘信息，则进行赋值，若无，贼分配逻辑块然后赋值
{
	struct buffer_head * bh;
	int i;

	if (block<0)                    //判断逻辑块数目是否正确
		panic("_bmap: block<0");
	if (block >= 7+512+512*512)
		panic("_bmap: block>big");
		//文件占用的块号小于等于7
	if (block<7) { //creat:是否创建逻辑块标志 1 创建    0 不创建
		if (create && !inode->i_zone[block])
			if (inode->i_zone[block]=new_block(inode->i_dev)) {
				inode->i_ctime=CURRENT_TIME;
				inode->i_dirt=1;
			}
		return inode->i_zone[block];
	}
	block -= 7;
	//创建一次间接块
	if (block<512) {
		//
		if (create && !inode->i_zone[7])
			if (inode->i_zone[7]=new_block(inode->i_dev)) {
				inode->i_dirt=1;
				inode->i_ctime=CURRENT_TIME;
			}
		if (!inode->i_zone[7])
			return 0;
		if (!(bh = bread(inode->i_dev,inode->i_zone[7])))
			return 0;
		i = ((unsigned short *) (bh->b_data))[block];
		if (create && !i)
			if (i=new_block(inode->i_dev)) {
				((unsigned short *) (bh->b_data))[block]=i;
				bh->b_dirt=1;
			}
		brelse(bh);
		return i;
	}
	block -= 512;
	//c创建二次间接块
	if (create && !inode->i_zone[8])
		if (inode->i_zone[8]=new_block(inode->i_dev)) {
			inode->i_dirt=1;
			inode->i_ctime=CURRENT_TIME;
		}
	if (!inode->i_zone[8])
		return 0;
	if (!(bh=bread(inode->i_dev,inode->i_zone[8])))
		return 0;
	i = ((unsigned short *)bh->b_data)[block>>9];
	if (create && !i)
		if (i=new_block(inode->i_dev)) {
			((unsigned short *) (bh->b_data))[block>>9]=i;
			bh->b_dirt=1;
		}
	brelse(bh);
	if (!i)
		return 0;
	if (!(bh=bread(inode->i_dev,i)))
		return 0;
	i = ((unsigned short *)bh->b_data)[block&511];
	if (create && !i)
		if (i=new_block(inode->i_dev)) {
			((unsigned short *) (bh->b_data))[block&511]=i;
			bh->b_dirt=1;
		}
	brelse(bh);
	return i;
}

int bmap(struct m_inode * inode,int block)
{
	return _bmap(inode,block,0); //只映射
}

int create_block(struct m_inode * inode, int block)
{
	return _bmap(inode,block,1); //创建
}
		
void iput(struct m_inode * inode) //释放inode节点 就是将iNode节点中的i_count置0，
{
	if (!inode) //判断是否为空
		return;
	wait_on_inode(inode); //不为空，等待inode节点解锁
	if (!inode->i_count) //如果此inode节点未被使用，已经为空
		panic("iput: trying to free free inode");
	if (inode->i_pipe) { //如果此inode为pipe
		wake_up(&inode->i_wait); //唤醒等待此inode节点的进程
		if (--inode->i_count) //先减在判断i_count是否为0
			return;
		free_page(inode->i_size); //
		inode->i_count=0;
		inode->i_dirt=0;
		inode->i_pipe=0;
		return;
	}
	if (!inode->i_dev) {
		inode->i_count--;
		return;
	}
	if (S_ISBLK(inode->i_mode)) { //如果此文件是一个块设备的情况下
		sync_dev(inode->i_zone[0]);  //进行设备的同步
		wait_on_inode(inode);
	}
repeat:
	if (inode->i_count>1) {
		inode->i_count--;
		return;
	}
	if (!inode->i_nlinks) {
		truncate(inode);
		free_inode(inode);
		return;
	}
	if (inode->i_dirt) {
		write_inode(inode);	/* we can sleep - so do again */
		wait_on_inode(inode);
		goto repeat;
	}
	inode->i_count--;
	return;
}

struct m_inode * get_empty_inode(void) 
{
	struct m_inode * inode;
	static struct m_inode * last_inode = inode_table;
	int i;
  //在inode数组中找到一个没有被应用、锁定、修改的位置
	do {
		inode = NULL;
		for (i = NR_INODE; i ; i--) {
			if (++last_inode >= inode_table + NR_INODE)
				last_inode = inode_table;
			if (!last_inode->i_count) {
				inode = last_inode;
				if (!inode->i_dirt && !inode->i_lock)
					break;
			}
		}
		if (!inode) { //如果没有找到，即inode数组中每个节点i_count都不为0；
			for (i=0 ; i<NR_INODE ; i++)
				printk("%04x: %6d\t",inode_table[i].i_dev,
					inode_table[i].i_num); //打印数组中所以的inode
			panic("No free inodes in mem");
		}
		//如果找到了
		wait_on_inode(inode); //等待inode解锁（这种情况是count=0,但是lock=1）
		while (inode->i_dirt) { //同步此inode节点与高速缓冲区的内容
			write_inode(inode);
			wait_on_inode(inode);
		}
	} while (inode->i_count);
	//此时找到的inode节点count=0,且是干净的
	memset(inode,0,sizeof(*inode)); //清空此inode节点
	inode->i_count = 1; //count置1，为新的inode使用
	return inode;
}

struct m_inode * get_pipe_inode(void)
{
	struct m_inode * inode;
  //获得inode数组中的可使用的inode节点
	if (!(inode = get_empty_inode()))
		return NULL; 
		//由于是作为pipe，分配内存页
	if (!(inode->i_size=get_free_page())) {
		inode->i_count = 0;
		return NULL;
	}
	//pipe，两个进程使用，又读又写
	inode->i_count = 2;	/* sum of readers/writers */
	//初始化pipe的头尾
	PIPE_HEAD(*inode) = PIPE_TAIL(*inode) = 0;
	//置pipe标志为1
	inode->i_pipe = 1;
	return inode;
}

struct m_inode * iget(int dev,int nr) //获得一个inode节点，（此inode节点在磁盘上）
{
	struct m_inode * inode, * empty;

	if (!dev)
		panic("iget with dev==0");
	empty = get_empty_inode(); //在inode数组中找到一个空inode
	inode = inode_table;
	while (inode < NR_INODE+inode_table) { //判断此inode节点是否已经在inode数组中
		if (inode->i_dev != dev || inode->i_num != nr) {
			inode++;
			continue; 
		}
		wait_on_inode(inode);
		if (inode->i_dev != dev || inode->i_num != nr) {
			inode = inode_table;
			continue;
		}
		inode->i_count++; //如果在inode数组中找到，说明此inode已经被其他进程使用，此进程再使用需要count+1
		if (inode->i_mount) { //判断此inode节点有没有被挂接
			int i;

			for (i = 0 ; i<NR_SUPER ; i++)
				if (super_block[i].s_imount==inode)
					break;
			if (i >= NR_SUPER) {
				printk("Mounted inode hasn't got sb\n");
				if (empty)
					iput(empty);
				return inode;
			}
			iput(inode);
			dev = super_block[i].s_dev;
			nr = ROOT_INO;
			inode = inode_table;
			continue;
		}
		if (empty)
			iput(empty);
		return inode;
	}
	if (!empty) 
		return (NULL);
	inode=empty; //如果没有在inode数组中找到已存在的inode,则将此inode节点设置为对应磁盘上的inode节点
	inode->i_dev = dev;
	inode->i_num = nr;
	read_inode(inode);//从磁盘中读出此inode数据
	return inode;
}

static void read_inode(struct m_inode * inode) //这里面的inode节点应该是根据文件目录信息获得设备号与inode节点号
{
	struct super_block * sb;
	struct buffer_head * bh;
	int block;

	lock_inode(inode);
	if (!(sb=get_super(inode->i_dev))) //获取超级块信息
		panic("trying to read inode without dev");
	block = 2 + sb->s_imap_blocks + sb->s_zmap_blocks +
		(inode->i_num-1)/INODES_PER_BLOCK; //获取inode节点对应的逻辑块号
	if (!(bh=bread(inode->i_dev,block))) //将对应的块读入到高速缓冲区
		panic("unable to read i-node block");
	*(struct d_inode *)inode =
		((struct d_inode *)bh->b_data) //读取高速缓冲区的inode节点信息
			[(inode->i_num-1)%INODES_PER_BLOCK];
	brelse(bh);
	unlock_inode(inode);//解锁
}

static void write_inode(struct m_inode * inode) //写inode 传入的参数为inode地址，将相应inode写入对应盘块的高速缓冲区
{
	struct super_block * sb;
	struct buffer_head * bh;
	int block;

	lock_inode(inode);
	if (!inode->i_dirt || !inode->i_dev) { //如果此inode被清空了，解锁返回
		unlock_inode(inode);
		return;
	}
	if (!(sb=get_super(inode->i_dev))) //获取超级块信息
		panic("trying to write inode without device");
	block = 2 + sb->s_imap_blocks + sb->s_zmap_blocks +
		(inode->i_num-1)/INODES_PER_BLOCK; //找到此i节点对应的盘块（注意，一个块内部不知一个i节点）
	if (!(bh=bread(inode->i_dev,block))) //将对应的盘块找到对应的高速缓冲区对应，并将其读入到高速缓冲区
		panic("unable to read i-node block");
	((struct d_inode *)bh->b_data)
		[(inode->i_num-1)%INODES_PER_BLOCK] =   //这个减一我推测为虽然bit0不用，但是存储的inode从1号inode节点开始
			*(struct d_inode *)inode; //将inode节点信息写入到高速缓冲区，等待系统或者人工同步（sys_snyc系统调用）
	bh->b_dirt=1;
	inode->i_dirt=0;
	brelse(bh);
	unlock_inode(inode);
}
