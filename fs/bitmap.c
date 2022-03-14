/*
 *  linux/fs/bitmap.c
 *
 *  (C) 1991  Linus Torvalds
 */

/* bitmap.c contains the code that handles the inode and block bitmaps */
//操作逻辑块位图与i节点位图的一系列函数（查找，销毁，创建）
#include <string.h>

#include <linux/sched.h>
#include <linux/kernel.h>

//清除相应地址的块
#define clear_block(addr) \
__asm__("cld\n\t" \
	"rep\n\t" \
	"stosl" \
	::"a" (0),"c" (BLOCK_SIZE/4),"D" ((long) (addr)):"cx","di")

//置位对应地址nr位为1
#define set_bit(nr,addr) ({\
register int res __asm__("ax"); \
__asm__ __volatile__("btsl %2,%3\n\tsetb %%al": \
"=a" (res):"0" (0),"r" (nr),"m" (*(addr))); \
res;})

//置位对应地址nr位为0
#define clear_bit(nr,addr) ({\
register int res __asm__("ax"); \
__asm__ __volatile__("btrl %2,%3\n\tsetnb %%al": \
"=a" (res):"0" (0),"r" (nr),"m" (*(addr))); \
res;})

//从addr地址处寻找bit为0的位，并返回其偏移数值
#define find_first_zero(addr) ({ \
int __res; \
__asm__("cld\n" \
	"1:\tlodsl\n\t" \
	"notl %%eax\n\t" \
	"bsfl %%eax,%%edx\n\t" \
	"je 2f\n\t" \
	"addl %%edx,%%ecx\n\t" \
	"jmp 3f\n" \
	"2:\taddl $32,%%ecx\n\t" \
	"cmpl $8192,%%ecx\n\t" \
	"jl 1b\n" \
	"3:" \
	:"=c" (__res):"c" (0),"S" (addr):"ax","dx","si"); \
__res;})

void free_block(int dev, int block)  //释放数据逻辑块                                                                                                                                                                              
{
	struct super_block * sb;
	struct buffer_head * bh;

	if (!(sb = get_super(dev))) //获得超级块信息
		panic("trying to free block on nonexistent device");
	if (block < sb->s_firstdatazone || block >= sb->s_nzones) //判断块号是否大于第一个数据逻辑块，小于总的逻辑块数
		panic("trying to free block not in datazone");       //对于一个磁盘分成多个文件系统的情况，暂不明确 
	bh = get_hash_table(dev,block); //找到当前数据块是否在高速缓冲去
	if (bh) {  //是
		if (bh->b_count != 1) { //判断是否被使用
			printk("trying to free block (%04x:%d), count=%d\n",
				dev,block,bh->b_count);
			return;
		}
		//没有被使用，释放相应的高速缓冲区                                                                                                                                                                                       
		bh->b_dirt=0;
		bh->b_uptodate=0;
		brelse(bh);
	}
	block -= sb->s_firstdatazone - 1 ;//相当于block-sb->s_firstdatazone+1
	//清空对应（数据）逻辑块位图，注意，其是从第二个bit为置于1（即第0个数据逻辑块）                                                                 
	if (clear_bit(block&8191,sb->s_zmap[block/8192]->b_data)) {
		printk("block (%04x:%d) ",dev,block+sb->s_firstdatazone-1);
		panic("free_block: bit already cleared");
	}
	sb->s_zmap[block/8192]->b_dirt = 1; //由于修改了高速缓冲区对应的逻辑块位图的信息，对应高速缓存区为dirt
}

int new_block(int dev) //使用一个新的数据逻辑块
{
	struct buffer_head * bh;
	struct super_block * sb;
	int i,j;

	if (!(sb = get_super(dev))) //获得对应超级块信息，必须获得
		panic("trying to get new block from nonexistant device");
	j = 8192;
	for (i=0 ; i<8 ; i++)
		if (bh=sb->s_zmap[i])
			if ((j=find_first_zero(bh->b_data))<8192) //获得对应数据逻辑块位图的buffer_head指针，并找到其中bit为0的偏移地址
				break;
	if (i>=8 || !bh || j>=8192)
		return 0; //没有找到，说明每个数据逻辑块都被使用，返回
	if (set_bit(j,bh->b_data)) //如果找到，置位对应的bit位，返回之前的状态，即0；
		panic("new_block: bit already set");
	bh->b_dirt = 1;//由于逻辑块位图对应的高速缓冲区被修改，对应buffer_head置为dirt
	j += i*8192 + sb->s_firstdatazone-1; //找到对应的块号
	if (j >= sb->s_nzones) //判断块号是否超出逻辑块号范围
		return 0;
	if (!(bh=getblk(dev,j))) //找到一个可使用的高速缓冲区
		panic("new_block: cannot get block");
	if (bh->b_count != 1) //判断现在对应的缓冲区count是否为1，（为什么不可以是2？）
		panic("new block: count is != 1"); //若是2，则说明此块已经被使用，就不可能bitmap为0
	clear_block(bh->b_data);//清空高速缓冲区内容
	bh->b_uptodate = 1; //更新标志
	bh->b_dirt = 1; //已修改标志
	brelse(bh);
	return j;
}

void free_inode(struct m_inode * inode) //释放inode节点
{
	struct super_block * sb;
	struct buffer_head * bh;

	if (!inode) //p判断是否为空                          
		return;
	if (!inode->i_dev) {  //设备号如果为0时，清除inode对应的内存
		memset(inode,0,sizeof(*inode));
		return;
	}
	if (inode->i_count>1) { //如果此inode正在被使用
		printk("trying to free inode with count=%d\n",inode->i_count);
		panic("free_inode");
	}
	if (inode->i_nlinks) //inode存在其他链接
		panic("trying to free inode with links");
	if (!(sb = get_super(inode->i_dev))) //获取超级块信息
		panic("trying to free inode on nonexistent device");
	if (inode->i_num < 1 || inode->i_num > sb->s_ninodes) //如果inode节点号小于1或者大于最大节点号
		panic("trying to free inode 0 or nonexistant inode");
	if (!(bh=sb->s_imap[inode->i_num>>13])) //相当于除以8192，找到对应高速缓冲区i节点位图
		panic("nonexistent imap in superblock");
	if (clear_bit(inode->i_num&8191,bh->b_data)) //清除对应高速缓存区bit位
		printk("free_inode: bit already cleared.\n\r");
	bh->b_dirt = 1; //由于信息改变，dirt置1
	memset(inode,0,sizeof(*inode)); //清除此inode节点对应的信息 
}

struct m_inode * new_inode(int dev) //新建一个inode节点
{
	struct m_inode * inode;
	struct super_block * sb;
	struct buffer_head * bh;
	int i,j;

	if (!(inode=get_empty_inode())) //找到一个空闲的inode来使用
		return NULL;
	if (!(sb = get_super(dev))) //获取超级块信息
		panic("new_inode with unknown device");
	j = 8192;
	for (i=0 ; i<8 ; i++) //找到对应i节点位图bit位
		if (bh=sb->s_imap[i])
			if ((j=find_first_zero(bh->b_data))<8192)
				break;
	if (!bh || j >= 8192 || j+i*8192 > sb->s_ninodes) { //如果不符合要求，进行相应操作
		iput(inode);
		return NULL;
	}
	if (set_bit(j,bh->b_data)) //置位对应高速缓冲区位图
		panic("new_inode: bit already set");
	bh->b_dirt = 1;//高速缓冲区修改，dirt置1
	//修改inode相应信息
	inode->i_count=1;
	inode->i_nlinks=1;
	inode->i_dev=dev;
	inode->i_uid=current->euid;
	inode->i_gid=current->egid;
	inode->i_dirt=1;
	inode->i_num = j + i*8192;
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	return inode;
}
