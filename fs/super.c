/*
 *  linux/fs/super.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * super.c contains code to handle the super-block tables.
 */
#include <linux/config.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/system.h>

#include <errno.h>
#include <sys/stat.h>

int sync_dev(int dev);
void wait_for_keypress(void);

/* set_bit uses setb, as gas doesn't recognize setc */
#define set_bit(bitnr,addr) ({ \
register int __res __asm__("ax"); \
__asm__("bt %2,%3;setb %%al":"=a" (__res):"a" (0),"r" (bitnr),"m" (*(addr))); \
__res; }) //test_bit 测试指定地址指定偏移的bit值

struct super_block super_block[NR_SUPER]; //超级块数组，最多只有8个
/* this is initialized in init/main.c */
int ROOT_DEV = 0; //根文件系统设备号

static void lock_super(struct super_block * sb) //加锁
{
	cli();
	while (sb->s_lock)
		sleep_on(&(sb->s_wait));
	sb->s_lock = 1;
	sti();
}

static void free_super(struct super_block * sb) //解锁，可以改为ulock_super
{
	cli();
	sb->s_lock = 0;
	wake_up(&(sb->s_wait));
	sti();
}

static void wait_on_super(struct super_block * sb)
{
	cli();
	while (sb->s_lock)
		sleep_on(&(sb->s_wait));
	sti();
}

//get_super与put_super是对超级块数组进行操作
struct super_block * get_super(int dev) //获取超级块信息，根据设备号，遍历超级块数组
{
	struct super_block * s;

	if (!dev)
		return NULL;
	s = 0+super_block;
	while (s < NR_SUPER+super_block)  //从已经挂载的文件系统中（超级块数组）找到相应的超级块
		if (s->s_dev == dev) {
			wait_on_super(s);
			if (s->s_dev == dev)
				return s;
			s = 0+super_block;
		} else
			s++;
	return NULL;
}

void put_super(int dev) //释放指定dev的超级块
{
	struct super_block * sb;
	struct m_inode * inode;
	int i;

	if (dev == ROOT_DEV) { //判断其是否为根设备
		printk("root diskette changed: prepare for armageddon(末日)\n\r");
		return;
	}
	if (!(sb = get_super(dev))) //如果没有找到
		return;
	if (sb->s_imount) { //若该文件系统被安装的i节点还没有被处理过，也会报错
		printk("Mounted disk changed - tssk, tssk\n\r");
		return;
	}
	lock_super(sb);//锁定此超级块
	sb->s_dev = 0; //设备号清空
	for(i=0;i<I_MAP_SLOTS;i++) //释放此超级块 i节点位图对应的高速缓冲区
		brelse(sb->s_imap[i]);
	for(i=0;i<Z_MAP_SLOTS;i++)//释放此超级块 逻辑块位图对应的高速缓冲区
		brelse(sb->s_zmap[i]);
	free_super(sb); //解锁此超级块
	return;
}

static struct super_block * read_super(int dev) //将磁盘上的超级块信息读入到超级块数组中
{
	struct super_block * s;
	struct buffer_head * bh;
	int i,block;

	if (!dev) //判断是否为根文件设备号
		return NULL;
	check_disk_change(dev); // 判断1是否更新过
	if (s = get_super(dev)) //判断是否已经在超级块数组中
		return s;
	for (s = 0+super_block ;; s++) {
		if (s >= NR_SUPER+super_block)
			return NULL;
		if (!s->s_dev) //在超级快数组中找到一个空闲的超级块数组位（即超级块数组中还有没有被占用的）
			break;
	}
	//对此超级块一些内存参数进行一些设置
	s->s_dev = dev;
	s->s_isup = NULL;
	s->s_imount = NULL;
	s->s_time = 0;
	s->s_rd_only = 0;
	s->s_dirt = 0;
	lock_super(s); //锁定超级块数组
	if (!(bh = bread(dev,1))) { //将磁盘中的超级块信息读入到高速缓存区中
	//没有找到，则清空超级块数组
		s->s_dev=0;
		free_super(s);
		return NULL;
	}
	*((struct d_super_block *) s) =
		*((struct d_super_block *) bh->b_data); //将读取到的高速缓冲区的数据对超级块数组赋值
	brelse(bh); //释放高速缓冲区
	//检测文件系统的id号(幻数)，是否匹配，
	if (s->s_magic != SUPER_MAGIC) {   
		s->s_dev = 0; //不匹配，则清空超级块数组，返回
		free_super(s);
		return NULL;
	}
	//初始化i节点位图与逻辑块位图
	for (i=0;i<I_MAP_SLOTS;i++)
		s->s_imap[i] = NULL;
	for (i=0;i<Z_MAP_SLOTS;i++)
		s->s_zmap[i] = NULL;
	block=2;
	for (i=0 ; i < s->s_imap_blocks ; i++)
		if (s->s_imap[i]=bread(dev,block)) //分配该设备文件系统i节点位图，将其i节点位图与高速缓冲区绑定
			block++;
		else
			break;
	for (i=0 ; i < s->s_zmap_blocks ; i++)//分配该设备文件系统逻辑块节点位图，将其逻辑块位图与高速缓冲区绑定
		if (s->s_zmap[i]=bread(dev,block))
			block++;
		else
			break;
	//如果block不等于引导块（1）+超级块（1）+i节点位图数+逻辑块位图数，出错，恢复
	if (block != 2+s->s_imap_blocks+s->s_zmap_blocks) {
		for(i=0;i<I_MAP_SLOTS;i++)
			brelse(s->s_imap[i]);
		for(i=0;i<Z_MAP_SLOTS;i++)
			brelse(s->s_zmap[i]);
		s->s_dev=0;
		free_super(s);
		return NULL;
	}
	s->s_imap[0]->b_data[0] |= 1; //将i节点位图bit0位置1
	s->s_zmap[0]->b_data[0] |= 1;//将逻辑块位图bit0位置1
	free_super(s); //读取完毕，解锁超级块
	return s;
}

int sys_umount(char * dev_name)
{
	struct m_inode * inode;
	struct super_block * sb;
	int dev;

	if (!(inode=namei(dev_name))) //根据路径获取(设备)文件inode节点
		return -ENOENT;
	dev = inode->i_zone[0];//设备文件的i_zone[0]为其设备号
	if (!S_ISBLK(inode->i_mode)) { //判断是否为块设备
		iput(inode); //不是，则释放
		return -ENOTBLK;
	}
	iput(inode); //释放inode节点
	if (dev==ROOT_DEV) //判断是否为根文件设备
		return -EBUSY;
	if (!(sb=get_super(dev)) || !(sb->s_imount)) //非根文件系统，读出其超级块，且挂载节点不为0（没有挂接或者读取错误）
		return -ENOENT;
	if (!sb->s_imount->i_mount) //如果该文件系统的挂接节点为空，返回错误
		printk("Mounted inode has i_mount=0\n");
	for (inode=inode_table+0 ; inode<inode_table+NR_INODE ; inode++)
		if (inode->i_dev==dev && inode->i_count) //如果此文件系统被一些进程使用，则不能卸载此文件系统·1
				return -EBUSY;
	sb->s_imount->i_mount=0; //将其挂接节点的挂接标志置0
	iput(sb->s_imount); //释放挂接inode节点
	sb->s_imount = NULL;
	iput(sb->s_isup);
	sb->s_isup = NULL;
	put_super(dev); //释放指定的超级块
	sync_dev(dev); //同步设备
	return 0;
}

int sys_mount(char * dev_name, char * dir_name, int rw_flag) //加载文件系统
{
	struct m_inode * dev_i, * dir_i;
	struct super_block * sb;
	int dev;

	if (!(dev_i=namei(dev_name))) //通过设备名找到对应的节点
		return -ENOENT;
	dev = dev_i->i_zone[0]; //通过i节点,找到其设备号
	if (!S_ISBLK(dev_i->i_mode)) {//判断是否为块设备
		iput(dev_i); //不是贼释放inode数组中的i节点
		return -EPERM;
	}
	iput(dev_i); //找到设备号后，同样可释放inode节点
	if (!(dir_i=namei(dir_name))) //找到需要挂载到的i节点
		return -ENOENT;
		//此节点没有被使用，且不是1号节点
	if (dir_i->i_count != 1 || dir_i->i_num == ROOT_INO) {
		iput(dir_i); //是，则释放此inode节点
		return -EBUSY; //返回此挂载节点忙
	}
	if (!S_ISDIR(dir_i->i_mode)) { //判断此i节点是否为目录文件
		iput(dir_i);//不是，则释放此i节点
		return -EPERM;
	}
	//将其设备的超级块从磁盘中读入到超级块数组中
	if (!(sb=read_super(dev))) { 
		iput(dir_i); //失败，释放挂载点inode节点
		return -EBUSY;
	}
	if (sb->s_imount) { //判断此设备是否已经被挂载
		iput(dir_i);
		return -EBUSY;
	}
	if (dir_i->i_mount) { 判断inode节点是否已挂载其他文件系统
		iput(dir_i);
		return -EPERM;
	}
	sb->s_imount=dir_i; //设置此文件系统挂载点
	dir_i->i_mount=1; //修改挂载点inode属性
	dir_i->i_dirt=1;	/* NOTE! we don't iput(dir_i) */
	return 0;			/* we do that in umount */
}

void mount_root(void) //根文件系统的挂接，此程序只会执行一次
{
	int i,free;
	struct super_block * p;
	struct m_inode * mi;

	if (32 != sizeof (struct d_inode))
		panic("bad i-node size");
	for(i=0;i<NR_FILE;i++) //文件表信息初始化
		file_table[i].f_count=0;
		//验证根文件系统是否在软盘上
	if (MAJOR(ROOT_DEV) == 2) {
		printk("Insert root floppy and press ENTER");
		wait_for_keypress();
	}
	//初始化超级块数组
	for(p = &super_block[0] ; p < &super_block[NR_SUPER] ; p++) {
		p->s_dev = 0;
		p->s_lock = 0;
		p->s_wait = NULL;
	}
	if (!(p=read_super(ROOT_DEV))) //将此设备的超级块（磁盘中）读入到超级块数组中
		panic("Unable to mount root");
	if (!(mi=iget(ROOT_DEV,ROOT_INO))) //获得此设备的第一个i节点信息
		panic("Unable to read root i-node");
	mi->i_count += 3 ;	/* NOTE! it is logically used 4 times, not 1 */
	p->s_isup = p->s_imount = mi; //设置根文件系统的挂载i节点，即1号i节点
	current->pwd = mi;
	current->root = mi;
	free=0;
	i=p->s_nzones;
	while (-- i >= 0)
		if (!set_bit(i&8191,p->s_zmap[i>>13]->b_data))
			free++;
	printk("%d/%d free blocks\n\r",free,p->s_nzones);
	free=0;
	i=p->s_ninodes+1;
	while (-- i >= 0)
		if (!set_bit(i&8191,p->s_imap[i>>13]->b_data))
			free++;
	printk("%d/%d free inodes\n\r",free,p->s_ninodes);
}
