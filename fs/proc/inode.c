/*
 *  linux/fs/proc/inode.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include <linux/time.h>
#include <linux/proc_fs.h>
#include <linux/kernel.h>
#include <linux/pid_namespace.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/completion.h>
#include <linux/poll.h>
#include <linux/printk.h>
#include <linux/file.h>
#include <linux/limits.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/sysctl.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/mount.h>

#include <asm/uaccess.h>

#include "internal.h"

static void proc_evict_inode(struct inode *inode)
{
	struct proc_dir_entry *de;
	struct ctl_table_header *head;
	const struct proc_ns_operations *ns_ops;
	void *ns;

	truncate_inode_pages(&inode->i_data, 0);
	clear_inode(inode);

	/* Stop tracking associated processes */
	put_pid(PROC_I(inode)->pid);

	/* Let go of any associated proc directory entry */
	de = PROC_I(inode)->pde;
	if (de)
		pde_put(de);
	head = PROC_I(inode)->sysctl;
	if (head) {
		rcu_assign_pointer(PROC_I(inode)->sysctl, NULL);
		sysctl_head_put(head);
	}
	/* Release any associated namespace */
	ns_ops = PROC_I(inode)->ns_ops;
	ns = PROC_I(inode)->ns;
	if (ns_ops && ns)
		ns_ops->put(ns);
}

static struct kmem_cache * proc_inode_cachep;

static struct inode *proc_alloc_inode(struct super_block *sb)
{
	struct proc_inode *ei;
	struct inode *inode;

	ei = (struct proc_inode *)kmem_cache_alloc(proc_inode_cachep, GFP_KERNEL);
	if (!ei)
		return NULL;
	ei->pid = NULL;
	ei->fd = 0;
	ei->op.proc_get_link = NULL;
	ei->pde = NULL;
	ei->sysctl = NULL;
	ei->sysctl_entry = NULL;
	ei->ns = NULL;
	ei->ns_ops = NULL;
	inode = &ei->vfs_inode;
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	return inode;
}

static void proc_i_callback(struct rcu_head *head)
{
	struct inode *inode = container_of(head, struct inode, i_rcu);
	kmem_cache_free(proc_inode_cachep, PROC_I(inode));
}

static void proc_destroy_inode(struct inode *inode)
{
	call_rcu(&inode->i_rcu, proc_i_callback);
}

static void init_once(void *foo)
{
	struct proc_inode *ei = (struct proc_inode *) foo;

	inode_init_once(&ei->vfs_inode);
}

void __init proc_init_inodecache(void)
{
	proc_inode_cachep = kmem_cache_create("proc_inode_cache",
					     sizeof(struct proc_inode),
					     0, (SLAB_RECLAIM_ACCOUNT|
						SLAB_MEM_SPREAD|SLAB_PANIC),
					     init_once);
}

static int proc_show_options(struct seq_file *seq, struct dentry *root)
{
	struct super_block *sb = root->d_sb;
	struct pid_namespace *pid = sb->s_fs_info;

	if (!gid_eq(pid->pid_gid, GLOBAL_ROOT_GID))
		seq_printf(seq, ",gid=%u", from_kgid_munged(&init_user_ns, pid->pid_gid));
	if (pid->hide_pid != 0)
		seq_printf(seq, ",hidepid=%u", pid->hide_pid);

	return 0;
}

static const struct super_operations proc_sops = {
	.alloc_inode	= proc_alloc_inode,
	.destroy_inode	= proc_destroy_inode,
	.drop_inode	= generic_delete_inode,
	.evict_inode	= proc_evict_inode,
	.statfs		= simple_statfs,
	.remount_fs	= proc_remount,
	.show_options	= proc_show_options,
};

void pde_users_dec(struct proc_dir_entry *pde)
{
	if (atomic_dec_and_test(&pde->pde_users) && pde->pde_unload_completion)
		complete(pde->pde_unload_completion);
}

static loff_t proc_reg_llseek(struct file *file, loff_t offset, int whence)
{
	const struct file_operations *fops;
	struct proc_dir_entry *pde = PDE(file_inode(file));
	loff_t rv = -EINVAL;
	loff_t (*llseek)(struct file *, loff_t, int);

	rcu_read_lock();
	fops = rcu_dereference(pde->proc_fops);
	/*
	 * remove_proc_entry() is going to delete PDE (as part of module
	 * cleanup sequence). No new callers into module allowed.
	 */
	if (!fops) {
		rcu_read_unlock();
		return rv;
	}
	/*
	 * Bump refcount so that remove_proc_entry will wail for ->llseek to
	 * complete.
	 */
	atomic_inc(&pde->pde_users);

	/*
	 * Save function pointer under rcu lock, to protect against
	 * ->proc_fops NULL'ifying by remove_proc_entry.
	 */
	llseek = fops->llseek;
	rcu_read_unlock();

	if (!llseek)
		llseek = default_llseek;
	rv = llseek(file, offset, whence);

	pde_users_dec(pde);
	return rv;
}

static ssize_t proc_reg_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	struct proc_dir_entry *pde = PDE(file_inode(file));
	ssize_t rv = -EIO;
	ssize_t (*read)(struct file *, char __user *, size_t, loff_t *);
	const struct file_operations *fops;

	rcu_read_lock();
	fops = rcu_dereference(pde->proc_fops);
	if (!fops) {
		rcu_read_unlock();
		return rv;
	}
	atomic_inc(&pde->pde_users);
	read = fops->read;
	rcu_read_unlock();

	if (read)
		rv = read(file, buf, count, ppos);

	pde_users_dec(pde);
	return rv;
}

static ssize_t proc_reg_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	struct proc_dir_entry *pde = PDE(file_inode(file));
	ssize_t rv = -EIO;
	ssize_t (*write)(struct file *, const char __user *, size_t, loff_t *);
	const struct file_operations *fops;

	rcu_read_lock();
	fops = rcu_dereference(pde->proc_fops);
	if (!fops) {
		rcu_read_unlock();
		return rv;
	}
	atomic_inc(&pde->pde_users);
	write = fops->write;
	rcu_read_unlock();

	if (write)
		rv = write(file, buf, count, ppos);

	pde_users_dec(pde);
	return rv;
}

static unsigned int proc_reg_poll(struct file *file, struct poll_table_struct *pts)
{
	struct proc_dir_entry *pde = PDE(file_inode(file));
	unsigned int rv = DEFAULT_POLLMASK;
	unsigned int (*poll)(struct file *, struct poll_table_struct *);
	const struct file_operations *fops;

	rcu_read_lock();
	fops = rcu_dereference(pde->proc_fops);
	if (!fops) {
		rcu_read_unlock();
		return rv;
	}
	atomic_inc(&pde->pde_users);
	poll = fops->poll;
	rcu_read_unlock();

	if (poll)
		rv = poll(file, pts);

	pde_users_dec(pde);
	return rv;
}

static long proc_reg_unlocked_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct proc_dir_entry *pde = PDE(file_inode(file));
	long rv = -ENOTTY;
	long (*ioctl)(struct file *, unsigned int, unsigned long);
	const struct file_operations *fops;

	rcu_read_lock();
	fops = rcu_dereference(pde->proc_fops);
	if (!fops) {
		rcu_read_unlock();
		return rv;
	}
	atomic_inc(&pde->pde_users);
	ioctl = fops->unlocked_ioctl;
	rcu_read_unlock();

	if (ioctl)
		rv = ioctl(file, cmd, arg);

	pde_users_dec(pde);
	return rv;
}

#ifdef CONFIG_COMPAT
static long proc_reg_compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct proc_dir_entry *pde = PDE(file_inode(file));
	long rv = -ENOTTY;
	long (*compat_ioctl)(struct file *, unsigned int, unsigned long);
	const struct file_operations *fops;

	rcu_read_lock();
	fops = rcu_dereference(pde->proc_fops);
	if (!fops) {
		rcu_read_unlock();
		return rv;
	}
	atomic_inc(&pde->pde_users);
	compat_ioctl = fops->compat_ioctl;
	rcu_read_unlock();

	if (compat_ioctl)
		rv = compat_ioctl(file, cmd, arg);

	pde_users_dec(pde);
	return rv;
}
#endif

static int proc_reg_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct proc_dir_entry *pde = PDE(file_inode(file));
	int rv = -EIO;
	int (*mmap)(struct file *, struct vm_area_struct *);
	const struct file_operations *fops;

	rcu_read_lock();
	fops = rcu_dereference(pde->proc_fops);
	if (!fops) {
		rcu_read_unlock();
		return rv;
	}
	atomic_inc(&pde->pde_users);
	mmap = fops->mmap;
	rcu_read_unlock();

	if (mmap)
		rv = mmap(file, vma);

	pde_users_dec(pde);
	return rv;
}

static int proc_reg_open(struct inode *inode, struct file *file)
{
	struct proc_dir_entry *pde = PDE(inode);
	int rv = 0;
	int (*open)(struct inode *, struct file *);
	int (*release)(struct inode *, struct file *);
	struct pde_opener *pdeo;
	const struct file_operations *fops;

	/*
	 * What for, you ask? Well, we can have open, rmmod, remove_proc_entry
	 * sequence. ->release won't be called because ->proc_fops will be
	 * cleared. Depending on complexity of ->release, consequences vary.
	 *
	 * We can't wait for mercy when close will be done for real, it's
	 * deadlockable: rmmod foo </proc/foo . So, we're going to do ->release
	 * by hand in remove_proc_entry(). For this, save opener's credentials
	 * for later.
	 */
	pdeo = kmalloc(sizeof(struct pde_opener), GFP_KERNEL);
	if (!pdeo)
		return -ENOMEM;

	rcu_read_lock();
	fops = rcu_dereference(pde->proc_fops);
	if (!fops) {
		rcu_read_unlock();
		kfree(pdeo);
		return -ENOENT;
	}
	atomic_inc(&pde->pde_users);
	open = fops->open;
	release = fops->release;
	rcu_read_unlock();

	if (open)
		rv = open(inode, file);

	if (rv == 0 && release) {
		/* To know what to release. */
		pdeo->inode = inode;
		pdeo->file = file;
		/* Strictly for "too late" ->release in proc_reg_release(). */
		pdeo->release = release;
		spin_lock(&pde->pde_openers_lock);
		list_add(&pdeo->lh, &pde->pde_openers);
		spin_unlock(&pde->pde_openers_lock);
	} else
		kfree(pdeo);
	pde_users_dec(pde);
	return rv;
}

static struct pde_opener *find_pde_opener(struct proc_dir_entry *pde,
					struct inode *inode, struct file *file)
{
	struct pde_opener *pdeo;

	spin_lock(&pde->pde_openers_lock);
	list_for_each_entry(pdeo, &pde->pde_openers, lh) {
		if (pdeo->inode == inode && pdeo->file == file) {
			spin_unlock(&pde->pde_openers_lock);
			return pdeo;
		}
	}
	spin_unlock(&pde->pde_openers_lock);
	return NULL;
}

static int proc_reg_release(struct inode *inode, struct file *file)
{
	struct proc_dir_entry *pde = PDE(inode);
	int rv = 0;
	int (*release)(struct inode *, struct file *);
	struct pde_opener *pdeo;
	const struct file_operations *fops;

	pdeo = find_pde_opener(pde, inode, file);
	rcu_read_lock();
	fops = rcu_dereference(pde->proc_fops);
	if (!fops) {
		rcu_read_unlock();
		/*
		 * Can't simply exit, __fput() will think that everything is OK,
		 * and move on to freeing struct file. remove_proc_entry() will
		 * find slacker in opener's list and will try to do non-trivial
		 * things with struct file. Therefore, remove opener from list.
		 *
		 * But if opener is removed from list, who will ->release it?
		 */
		if (pdeo) {
			spin_lock(&pde->pde_openers_lock);
			list_del(&pdeo->lh);
			spin_unlock(&pde->pde_openers_lock);
			rv = pdeo->release(inode, file);
			kfree(pdeo);
		}
		return rv;
	}
	atomic_inc(&pde->pde_users);
	release = fops->release;
	rcu_read_unlock();
	if (pdeo) {
		spin_lock(&pde->pde_openers_lock);
		list_del(&pdeo->lh);
		spin_unlock(&pde->pde_openers_lock);
	}
	kfree(pdeo);

	if (release)
		rv = release(inode, file);

	pde_users_dec(pde);
	return rv;
}

static const struct file_operations proc_reg_file_ops = {
	.llseek		= proc_reg_llseek,
	.read		= proc_reg_read,
	.write		= proc_reg_write,
	.poll		= proc_reg_poll,
	.unlocked_ioctl	= proc_reg_unlocked_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= proc_reg_compat_ioctl,
#endif
	.mmap		= proc_reg_mmap,
	.open		= proc_reg_open,
	.release	= proc_reg_release,
};

#ifdef CONFIG_COMPAT
static const struct file_operations proc_reg_file_ops_no_compat = {
	.llseek		= proc_reg_llseek,
	.read		= proc_reg_read,
	.write		= proc_reg_write,
	.poll		= proc_reg_poll,
	.unlocked_ioctl	= proc_reg_unlocked_ioctl,
	.mmap		= proc_reg_mmap,
	.open		= proc_reg_open,
	.release	= proc_reg_release,
};
#endif

struct inode *proc_get_inode(struct super_block *sb, struct proc_dir_entry *de)
{
	struct inode *inode = iget_locked(sb, de->low_ino);
	const struct file_operations *fops;

	if (inode && (inode->i_state & I_NEW)) {
		inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
		PROC_I(inode)->pde = de;

		if (de->mode) {
			inode->i_mode = de->mode;
			inode->i_uid = de->uid;
			inode->i_gid = de->gid;
		}
		if (de->size)
			inode->i_size = de->size;
		if (de->nlink)
			set_nlink(inode, de->nlink);
		if (de->proc_iops)
			inode->i_op = de->proc_iops;
		rcu_read_lock();
		fops = rcu_dereference(de->proc_fops);
		if (fops) {
			if (S_ISREG(inode->i_mode)) {
#ifdef CONFIG_COMPAT
				if (!fops->compat_ioctl)
					inode->i_fop =
						&proc_reg_file_ops_no_compat;
				else
#endif
					inode->i_fop = &proc_reg_file_ops;
			} else {
				inode->i_fop = fops;
			}
		}
		rcu_read_unlock();
		unlock_new_inode(inode);
	} else
	       pde_put(de);
	return inode;
}

int proc_fill_super(struct super_block *s)
{
	struct inode *root_inode;

	s->s_flags |= MS_NODIRATIME | MS_NOSUID | MS_NOEXEC;
	s->s_blocksize = 1024;
	s->s_blocksize_bits = 10;
	s->s_magic = PROC_SUPER_MAGIC;
	s->s_op = &proc_sops;
	s->s_time_gran = 1;
	
	pde_get(&proc_root);
	root_inode = proc_get_inode(s, &proc_root);
	if (!root_inode) {
		pr_err("proc_fill_super: get root inode failed\n");
		return -ENOMEM;
	}

	s->s_root = d_make_root(root_inode);
	if (!s->s_root) {
		pr_err("proc_fill_super: allocate dentry failed\n");
		return -ENOMEM;
	}

	return 0;
}
