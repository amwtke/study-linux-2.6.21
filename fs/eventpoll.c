/*
 *  fs/eventpoll.c ( Efficent event polling implementation )
 *  Copyright (C) 2001,...,2006	 Davide Libenzi
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Davide Libenzi <davidel@xmailserver.org>
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/signal.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/poll.h>
#include <linux/smp_lock.h>
#include <linux/string.h>
#include <linux/list.h>
#include <linux/hash.h>
#include <linux/spinlock.h>
#include <linux/syscalls.h>
#include <linux/rwsem.h>
#include <linux/rbtree.h>
#include <linux/wait.h>
#include <linux/eventpoll.h>
#include <linux/mount.h>
#include <linux/bitops.h>
#include <linux/mutex.h>
#include <asm/uaccess.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/mman.h>
#include <asm/atomic.h>
#include <asm/semaphore.h>


/*
 * LOCKING:
 * There are three level of locking required by epoll :
 *
 * 1) epmutex (mutex)
 * 2) ep->sem (rw_semaphore)
 * 3) ep->lock (rw_lock)
 *
 * The acquire order is the one listed above, from 1 to 3.
 * We need a spinlock (ep->lock) because we manipulate objects
 * from inside the poll callback, that might be triggered from
 * a wake_up() that in turn might be called from IRQ context.
 * So we can't sleep inside the poll callback and hence we need
 * a spinlock. During the event transfer loop (from kernel to
 * user space) we could end up sleeping due a copy_to_user(), so
 * we need a lock that will allow us to sleep. This lock is a
 * read-write semaphore (ep->sem). It is acquired on read during
 * the event transfer loop and in write during epoll_ctl(EPOLL_CTL_DEL)
 * and during eventpoll_release_file(). Then we also need a global
 * semaphore to serialize eventpoll_release_file() and ep_free().
 * This semaphore is acquired by ep_free() during the epoll file
 * cleanup path and it is also acquired by eventpoll_release_file()
 * if a file has been pushed inside an epoll set and it is then
 * close()d without a previous call toepoll_ctl(EPOLL_CTL_DEL).
 * It is possible to drop the "ep->sem" and to use the global
 * semaphore "epmutex" (together with "ep->lock") to have it working,
 * but having "ep->sem" will make the interface more scalable.
 * Events that require holding "epmutex" are very rare, while for
 * normal operations the epoll private "ep->sem" will guarantee
 * a greater scalability.
 */


#define EVENTPOLLFS_MAGIC 0x03111965 /* My birthday should work for this :) */

#define DEBUG_EPOLL 0

#if DEBUG_EPOLL > 0
#define DPRINTK(x) printk x
#define DNPRINTK(n, x) do { if ((n) <= DEBUG_EPOLL) printk x; } while (0)
#else /* #if DEBUG_EPOLL > 0 */
#define DPRINTK(x) (void) 0
#define DNPRINTK(n, x) (void) 0
#endif /* #if DEBUG_EPOLL > 0 */

#define DEBUG_EPI 0

#if DEBUG_EPI != 0
#define EPI_SLAB_DEBUG (SLAB_DEBUG_FREE | SLAB_RED_ZONE /* | SLAB_POISON */)
#else /* #if DEBUG_EPI != 0 */
#define EPI_SLAB_DEBUG 0
#endif /* #if DEBUG_EPI != 0 */

/* Epoll private bits inside the event mask */
#define EP_PRIVATE_BITS (EPOLLONESHOT | EPOLLET)

/* Maximum number of poll wake up nests we are allowing */
#define EP_MAX_POLLWAKE_NESTS 4

/* Maximum msec timeout value storeable in a long int */
#define EP_MAX_MSTIMEO min(1000ULL * MAX_SCHEDULE_TIMEOUT / HZ, (LONG_MAX - 999ULL) / HZ)

#define EP_MAX_EVENTS (INT_MAX / sizeof(struct epoll_event))


struct epoll_filefd {
	struct file *file;
	int fd;
};

/*
 * Node that is linked into the "wake_task_list" member of the "struct poll_safewake".
 * It is used to keep track on all tasks that are currently inside the wake_up() code
 * to 1) short-circuit the one coming from the same task and same wait queue head
 * ( loop ) 2) allow a maximum number of epoll descriptors inclusion nesting
 * 3) let go the ones coming from other tasks.
 */
struct wake_task_node {
	struct list_head llink;
	struct task_struct *task;
	wait_queue_head_t *wq;
};

/*
 * This is used to implement the safe poll wake up avoiding to reenter
 * the poll callback from inside wake_up().
 */
struct poll_safewake {
	struct list_head wake_task_list;
	spinlock_t lock;
};

/*
 * This structure is stored inside the "private_data" member of the file
 * structure and rapresent the main data sructure for the eventpoll
 * interface.
 */
struct eventpoll {
	/* Protect the this structure access */
	rwlock_t lock;

	/*
	 * This semaphore is used to ensure that files are not removed
	 * while epoll is using them. This is read-held during the event
	 * collection loop and it is write-held during the file cleanup
	 * path, the epoll file exit code and the ctl operations.
	 */
	struct rw_semaphore sem;

	/* Wait queue used by sys_epoll_wait()
	???head??????????????????????????????
	 */
	wait_queue_head_t wq;

	/* Wait queue used by file->poll() */
	wait_queue_head_t poll_wait;

	/* List of ready file descriptors */
	struct list_head rdllist;

	/* RB-Tree root used to store monitored fd structs */
	struct rb_root rbr; //?????????????????????????????????tfile???????????????
};

/* Wait structure used by the poll hooks */
struct eppoll_entry {
	/* List header used to link this structure to the "struct epitem" */
	struct list_head llink;

	/* The "base" pointer is set to the container "struct epitem" */
	void *base;

	/*
	 * Wait queue item that will be linked to the target file wait
	 * queue head.
	 * 
	 * ?????????????????????????????????fd????????????????????????
	 */
	wait_queue_t wait;

	/* The wait queue head that linked the "wait" wait queue item */
	wait_queue_head_t *whead;
};

/*
 * Each file descriptor added to the eventpoll interface will
 * have an entry of this type linked to the hash.
 */
struct epitem {
	/* RB-Tree node used to link this structure to the eventpoll rb-tree */
	struct rb_node rbn;

	/* List header used to link this structure to the eventpoll ready list */
	struct list_head rdllink;

	/* The file descriptor information this item refers to */
	struct epoll_filefd ffd;

	/* Number of active wait queue attached to poll operations */
	int nwait;

	/* List containing poll wait queues */
	struct list_head pwqlist;

	/* The "container" of this item */
	struct eventpoll *ep;

	/* The structure that describe the interested events and the source fd */
	struct epoll_event event;

	/*
	 * Used to keep track of the usage count of the structure. This avoids
	 * that the structure will desappear from underneath our processing.
	 */
	atomic_t usecnt;

	/* List header used to link this item to the "struct file" items list */
	struct list_head fllink;

	/* List header used to link the item to the transfer list */
	struct list_head txlink;

	/*
	 * This is used during the collection/transfer of events to userspace
	 * to pin items empty events set.
	 */
	unsigned int revents;
};

/* Wrapper struct used by poll queueing */
struct ep_pqueue {
	poll_table pt;
	struct epitem *epi;
};



static void ep_poll_safewake_init(struct poll_safewake *psw);
static void ep_poll_safewake(struct poll_safewake *psw, wait_queue_head_t *wq);
static int ep_getfd(int *efd, struct inode **einode, struct file **efile,
		    struct eventpoll *ep);
static int ep_alloc(struct eventpoll **pep);
static void ep_free(struct eventpoll *ep);
static struct epitem *ep_find(struct eventpoll *ep, struct file *file, int fd);
static void ep_use_epitem(struct epitem *epi);
static void ep_release_epitem(struct epitem *epi);
static void ep_ptable_queue_proc(struct file *file, wait_queue_head_t *whead,
				 poll_table *pt);
static void ep_rbtree_insert(struct eventpoll *ep, struct epitem *epi);
static int ep_insert(struct eventpoll *ep, struct epoll_event *event,
		     struct file *tfile, int fd);
static int ep_modify(struct eventpoll *ep, struct epitem *epi,
		     struct epoll_event *event);
static void ep_unregister_pollwait(struct eventpoll *ep, struct epitem *epi);
static int ep_unlink(struct eventpoll *ep, struct epitem *epi);
static int ep_remove(struct eventpoll *ep, struct epitem *epi);
static int ep_poll_callback(wait_queue_t *wait, unsigned mode, int sync, void *key);
static int ep_eventpoll_close(struct inode *inode, struct file *file);
static unsigned int ep_eventpoll_poll(struct file *file, poll_table *wait);
static int ep_collect_ready_items(struct eventpoll *ep,
				  struct list_head *txlist, int maxevents);
static int ep_send_events(struct eventpoll *ep, struct list_head *txlist,
			  struct epoll_event __user *events);
static void ep_reinject_items(struct eventpoll *ep, struct list_head *txlist);
static int ep_events_transfer(struct eventpoll *ep,
			      struct epoll_event __user *events,
			      int maxevents);
static int ep_poll(struct eventpoll *ep, struct epoll_event __user *events,
		   int maxevents, long timeout);
static int eventpollfs_delete_dentry(struct dentry *dentry);
static struct inode *ep_eventpoll_inode(void);
static int eventpollfs_get_sb(struct file_system_type *fs_type,
			      int flags, const char *dev_name,
			      void *data, struct vfsmount *mnt);

/*
 * This semaphore is used to serialize ep_free() and eventpoll_release_file().
 */
static struct mutex epmutex;

/* Safe wake up implementation */
static struct poll_safewake psw;

/* Slab cache used to allocate "struct epitem" */
static struct kmem_cache *epi_cache __read_mostly;

/* Slab cache used to allocate "struct eppoll_entry" */
static struct kmem_cache *pwq_cache __read_mostly;

/* Virtual fs used to allocate inodes for eventpoll files */
static struct vfsmount *eventpoll_mnt __read_mostly;

/* File callbacks that implement the eventpoll file behaviour */
static const struct file_operations eventpoll_fops = {
	.release	= ep_eventpoll_close,
	.poll		= ep_eventpoll_poll
};

/*
 * This is used to register the virtual file system from where
 * eventpoll inodes are allocated.
 */
static struct file_system_type eventpoll_fs_type = {
	.name		= "eventpollfs",
	.get_sb		= eventpollfs_get_sb,
	.kill_sb	= kill_anon_super,
};

/* Very basic directory entry operations for the eventpoll virtual file system */
static struct dentry_operations eventpollfs_dentry_operations = {
	.d_delete	= eventpollfs_delete_dentry,
};



/* Fast test to see if the file is an evenpoll file */
static inline int is_file_epoll(struct file *f)
{
	return f->f_op == &eventpoll_fops;
}

/* Setup the structure that is used as key for the rb-tree */
static inline void ep_set_ffd(struct epoll_filefd *ffd,
			      struct file *file, int fd)
{
	ffd->file = file;
	ffd->fd = fd;
}

/* Compare rb-tree keys */
static inline int ep_cmp_ffd(struct epoll_filefd *p1,
			     struct epoll_filefd *p2)
{
	return (p1->file > p2->file ? +1:
	        (p1->file < p2->file ? -1 : p1->fd - p2->fd));
}

/* Special initialization for the rb-tree node to detect linkage */
static inline void ep_rb_initnode(struct rb_node *n)
{
	rb_set_parent(n, n);
}

/* Removes a node from the rb-tree and marks it for a fast is-linked check */
static inline void ep_rb_erase(struct rb_node *n, struct rb_root *r)
{
	rb_erase(n, r);
	rb_set_parent(n, n);
}

/* Fast check to verify that the item is linked to the main rb-tree */
static inline int ep_rb_linked(struct rb_node *n)
{
	return rb_parent(n) != n;
}

/*
 * Remove the item from the list and perform its initialization.
 * This is useful for us because we can test if the item is linked
 * using "ep_is_linked(p)".
 */
static inline void ep_list_del(struct list_head *p)
{
	list_del(p);
	INIT_LIST_HEAD(p);
}

/* Tells us if the item is currently linked */
static inline int ep_is_linked(struct list_head *p)
{
	return !list_empty(p);
}

/* Get the "struct epitem" from a wait queue pointer */
static inline struct epitem * ep_item_from_wait(wait_queue_t *p)
{
	return container_of(p, struct eppoll_entry, wait)->base;
}

/* Get the "struct epitem" from an epoll queue wrapper */
static inline struct epitem * ep_item_from_epqueue(poll_table *p)
{
	return container_of(p, struct ep_pqueue, pt)->epi;
}

/* Tells if the epoll_ctl(2) operation needs an event copy from userspace */
static inline int ep_op_hash_event(int op)
{
	return op != EPOLL_CTL_DEL;
}

/* Initialize the poll safe wake up structure */
static void ep_poll_safewake_init(struct poll_safewake *psw)
{

	INIT_LIST_HEAD(&psw->wake_task_list);
	spin_lock_init(&psw->lock);
}


/*
 * Perform a safe wake up of the poll wait list. The problem is that
 * with the new callback'd wake up system, it is possible that the
 * poll callback is reentered from inside the call to wake_up() done
 * on the poll wait queue head. The rule is that we cannot reenter the
 * wake up code from the same task more than EP_MAX_POLLWAKE_NESTS times,
 * and we cannot reenter the same wait queue head at all. This will
 * enable to have a hierarchy of epoll file descriptor of no more than
 * EP_MAX_POLLWAKE_NESTS deep. We need the irq version of the spin lock
 * because this one gets called by the poll callback, that in turn is called
 * from inside a wake_up(), that might be called from irq context.
 * 
 * epoll??????epoll????????????????????????????????????ep_poll_callback ?????????
 */
static void ep_poll_safewake(struct poll_safewake *psw, wait_queue_head_t *wq)
{
	int wake_nests = 0;
	unsigned long flags;
	struct task_struct *this_task = current;
	struct list_head *lsthead = &psw->wake_task_list, *lnk;
	struct wake_task_node *tncur;
	struct wake_task_node tnode;

	spin_lock_irqsave(&psw->lock, flags);

	/* Try to see if the current task is already inside this wakeup call */
	list_for_each(lnk, lsthead) {
		tncur = list_entry(lnk, struct wake_task_node, llink);

		if (tncur->wq == wq ||
		    (tncur->task == this_task && ++wake_nests > EP_MAX_POLLWAKE_NESTS)) {
			/*
			 * Ops ... loop detected or maximum nest level reached.
			 * We abort this wake by breaking the cycle itself.
			 */
			spin_unlock_irqrestore(&psw->lock, flags);
			return;
		}
	}

	/* Add the current task to the list */
	tnode.task = this_task;
	tnode.wq = wq;
	list_add(&tnode.llink, lsthead);

	spin_unlock_irqrestore(&psw->lock, flags);

	/* Do really wake up now */
	wake_up(wq);

	/* Remove the current task from the list */
	spin_lock_irqsave(&psw->lock, flags);
	list_del(&tnode.llink);
	spin_unlock_irqrestore(&psw->lock, flags);
}


/*
 * This is called from eventpoll_release() to unlink files from the eventpoll
 * interface. We need to have this facility to cleanup correctly files that are
 * closed without being removed from the eventpoll interface.
 */
void eventpoll_release_file(struct file *file)
{
	struct list_head *lsthead = &file->f_ep_links;
	struct eventpoll *ep;
	struct epitem *epi;

	/*
	 * We don't want to get "file->f_ep_lock" because it is not
	 * necessary. It is not necessary because we're in the "struct file"
	 * cleanup path, and this means that noone is using this file anymore.
	 * The only hit might come from ep_free() but by holding the semaphore
	 * will correctly serialize the operation. We do need to acquire
	 * "ep->sem" after "epmutex" because ep_remove() requires it when called
	 * from anywhere but ep_free().
	 */
	mutex_lock(&epmutex);

	while (!list_empty(lsthead)) {
		epi = list_entry(lsthead->next, struct epitem, fllink);

		ep = epi->ep;
		ep_list_del(&epi->fllink);
		down_write(&ep->sem);
		ep_remove(ep, epi);
		up_write(&ep->sem);
	}

	mutex_unlock(&epmutex);
}


/*
 * It opens an eventpoll file descriptor by suggesting a storage of "size"
 * file descriptors. The size parameter is just an hint about how to size
 * data structures. It won't prevent the user to store more than "size"
 * file descriptors inside the epoll interface. It is the kernel part of
 * the userspace epoll_create(2).
 */
asmlinkage long sys_epoll_create(int size)
{
	int error, fd = -1;
	struct eventpoll *ep;
	struct inode *inode;
	struct file *file;

	DNPRINTK(3, (KERN_INFO "[%p] eventpoll: sys_epoll_create(%d)\n",
		     current, size));

	/*
	 * Sanity check on the size parameter, and create the internal data
	 * structure ( "struct eventpoll" ).
	 */
	error = -EINVAL;
	if (size <= 0 || (error = ep_alloc(&ep)) != 0)
		goto eexit_1;

	/*
	 * Creates all the items needed to setup an eventpoll file. That is,
	 * a file structure, and inode and a free file descriptor.
	 */
	error = ep_getfd(&fd, &inode, &file, ep);
	if (error)
		goto eexit_2;

	DNPRINTK(3, (KERN_INFO "[%p] eventpoll: sys_epoll_create(%d) = %d\n",
		     current, size, fd));

	return fd;

eexit_2:
	ep_free(ep);
	kfree(ep);
eexit_1:
	DNPRINTK(3, (KERN_INFO "[%p] eventpoll: sys_epoll_create(%d) = %d\n",
		     current, size, error));
	return error;
}


/*
 * The following function implements the controller interface for
 * the eventpoll file that enables the insertion/removal/change of
 * file descriptors inside the interest set.  It represents
 * the kernel part of the user space epoll_ctl(2).
 */
asmlinkage long
sys_epoll_ctl(int epfd, int op, int fd, struct epoll_event __user *event)
{
	int error;
	struct file *file, *tfile;
	struct eventpoll *ep;
	struct epitem *epi;
	struct epoll_event epds;

	DNPRINTK(3, (KERN_INFO "[%p] eventpoll: sys_epoll_ctl(%d, %d, %d, %p)\n",
		     current, epfd, op, fd, event));

	error = -EFAULT;
	if (ep_op_hash_event(op) &&
	    copy_from_user(&epds, event, sizeof(struct epoll_event)))
		goto eexit_1;

	/* Get the "struct file *" for the eventpoll file */
	error = -EBADF;
	file = fget(epfd);
	if (!file)
		goto eexit_1;

	/* Get the "struct file *" for the target file */
	tfile = fget(fd);
	if (!tfile)
		goto eexit_2;

	/* The target file descriptor must support poll 
	???????????????poll?????????fd????????????epoll??????
	*/
	error = -EPERM;
	if (!tfile->f_op || !tfile->f_op->poll)
		goto eexit_3;

	/*
	 * We have to check that the file structure underneath the file descriptor
	 * the user passed to us _is_ an eventpoll file. And also we do not permit
	 * adding an epoll file descriptor inside itself.
	 */
	error = -EINVAL;
	if (file == tfile || !is_file_epoll(file))
		goto eexit_3;

	/*
	 * At this point it is safe to assume that the "private_data" contains
	 * our own data structure.
	 */
	ep = file->private_data;

	down_write(&ep->sem);

	/* Try to lookup the file inside our hash table */
	epi = ep_find(ep, tfile, fd);

	error = -EINVAL;
	switch (op) {
	case EPOLL_CTL_ADD:
		if (!epi) {
			epds.events |= POLLERR | POLLHUP;

			error = ep_insert(ep, &epds, tfile, fd);
		} else
			error = -EEXIST;
		break;
	case EPOLL_CTL_DEL:
		if (epi)
			error = ep_remove(ep, epi);
		else
			error = -ENOENT;
		break;
	case EPOLL_CTL_MOD:
		if (epi) {
			epds.events |= POLLERR | POLLHUP;
			error = ep_modify(ep, epi, &epds);
		} else
			error = -ENOENT;
		break;
	}

	/*
	 * The function ep_find() increments the usage count of the structure
	 * so, if this is not NULL, we need to release it.
	 */
	if (epi)
		ep_release_epitem(epi);

	up_write(&ep->sem);

eexit_3:
	fput(tfile);
eexit_2:
	fput(file);
eexit_1:
	DNPRINTK(3, (KERN_INFO "[%p] eventpoll: sys_epoll_ctl(%d, %d, %d, %p) = %d\n",
		     current, epfd, op, fd, event, error));

	return error;
}


/*
 * Implement the event wait interface for the eventpoll file. It is the kernel
 * part of the user space epoll_wait(2).
 * 
 * events
 */
asmlinkage long sys_epoll_wait(int epfd, struct epoll_event __user *events,
			       int maxevents, int timeout)
{
	int error;
	struct file *file;
	struct eventpoll *ep;

	DNPRINTK(3, (KERN_INFO "[%p] eventpoll: sys_epoll_wait(%d, %p, %d, %d)\n",
		     current, epfd, events, maxevents, timeout));

	/* The maximum number of event must be greater than zero */
	if (maxevents <= 0 || maxevents > EP_MAX_EVENTS)
		return -EINVAL;

	/* Verify that the area passed by the user is writeable */
	if (!access_ok(VERIFY_WRITE, events, maxevents * sizeof(struct epoll_event))) {
		error = -EFAULT;
		goto eexit_1;
	}

	/* Get the "struct file *" for the eventpoll file */
	error = -EBADF;
	file = fget(epfd);
	if (!file)
		goto eexit_1;

	/*
	 * We have to check that the file structure underneath the fd
	 * the user passed to us _is_ an eventpoll file.
	 */
	error = -EINVAL;
	if (!is_file_epoll(file))
		goto eexit_2;

	/*
	 * At this point it is safe to assume that the "private_data" contains
	 * our own data structure.
	 */
	ep = file->private_data;

	/* Time to fish for events ... */
	error = ep_poll(ep, events, maxevents, timeout);

eexit_2:
	fput(file);
eexit_1:
	DNPRINTK(3, (KERN_INFO "[%p] eventpoll: sys_epoll_wait(%d, %p, %d, %d) = %d\n",
		     current, epfd, events, maxevents, timeout, error));

	return error;
}


#ifdef TIF_RESTORE_SIGMASK

/*
 * Implement the event wait interface for the eventpoll file. It is the kernel
 * part of the user space epoll_pwait(2).
 */
asmlinkage long sys_epoll_pwait(int epfd, struct epoll_event __user *events,
		int maxevents, int timeout, const sigset_t __user *sigmask,
		size_t sigsetsize)
{
	int error;
	sigset_t ksigmask, sigsaved;

	/*
	 * If the caller wants a certain signal mask to be set during the wait,
	 * we apply it here.
	 */
	if (sigmask) {
		if (sigsetsize != sizeof(sigset_t))
			return -EINVAL;
		if (copy_from_user(&ksigmask, sigmask, sizeof(ksigmask)))
			return -EFAULT;
		sigdelsetmask(&ksigmask, sigmask(SIGKILL) | sigmask(SIGSTOP));
		sigprocmask(SIG_SETMASK, &ksigmask, &sigsaved);
	}

	error = sys_epoll_wait(epfd, events, maxevents, timeout);

	/*
	 * If we changed the signal mask, we need to restore the original one.
	 * In case we've got a signal while waiting, we do not restore the
	 * signal mask yet, and we allow do_signal() to deliver the signal on
	 * the way back to userspace, before the signal mask is restored.
	 */
	if (sigmask) {
		if (error == -EINTR) {
			memcpy(&current->saved_sigmask, &sigsaved,
				sizeof(sigsaved));
			set_thread_flag(TIF_RESTORE_SIGMASK);
		} else
			sigprocmask(SIG_SETMASK, &sigsaved, NULL);
	}

	return error;
}

#endif /* #ifdef TIF_RESTORE_SIGMASK */


/*
 * Creates the file descriptor to be used by the epoll interface.
 */
static int ep_getfd(int *efd, struct inode **einode, struct file **efile,
		    struct eventpoll *ep)
{
	struct qstr this;
	char name[32];
	struct dentry *dentry;
	struct inode *inode;
	struct file *file;
	int error, fd;

	/* Get an ready to use file */
	error = -ENFILE;
	file = get_empty_filp();
	if (!file)
		goto eexit_1;

	/* Allocates an inode from the eventpoll file system */
	inode = ep_eventpoll_inode();
	if (IS_ERR(inode)) {
		error = PTR_ERR(inode);
		goto eexit_2;
	}

	/* Allocates a free descriptor to plug the file onto */
	error = get_unused_fd();
	if (error < 0)
		goto eexit_3;
	fd = error;

	/*
	 * Link the inode to a directory entry by creating a unique name
	 * using the inode number.
	 */
	error = -ENOMEM;
	sprintf(name, "[%lu]", inode->i_ino);
	this.name = name;
	this.len = strlen(name);
	this.hash = inode->i_ino;
	dentry = d_alloc(eventpoll_mnt->mnt_sb->s_root, &this);
	if (!dentry)
		goto eexit_4;
	dentry->d_op = &eventpollfs_dentry_operations;
	d_add(dentry, inode);
	file->f_path.mnt = mntget(eventpoll_mnt);
	file->f_path.dentry = dentry;
	file->f_mapping = inode->i_mapping;

	file->f_pos = 0;
	file->f_flags = O_RDONLY;
	file->f_op = &eventpoll_fops;
	file->f_mode = FMODE_READ;
	file->f_version = 0;
	file->private_data = ep;//eventpoll??????file???private_data??????

	/* Install the new setup file into the allocated fd. */
	fd_install(fd, file); //???fd???file???????????????

	*efd = fd;
	*einode = inode;
	*efile = file;
	return 0;

eexit_4:
	put_unused_fd(fd);
eexit_3:
	iput(inode);
eexit_2:
	put_filp(file);
eexit_1:
	return error;
}


static int ep_alloc(struct eventpoll **pep)
{
	struct eventpoll *ep = kzalloc(sizeof(*ep), GFP_KERNEL);

	if (!ep)
		return -ENOMEM;

	rwlock_init(&ep->lock);
	init_rwsem(&ep->sem);
	init_waitqueue_head(&ep->wq);
	init_waitqueue_head(&ep->poll_wait);
	INIT_LIST_HEAD(&ep->rdllist);
	ep->rbr = RB_ROOT;

/*
	struct eventpoll **pep
	???????????? (struct eventpoll *) *pep 
	 - pep???????????????,????????????
	 - pep????????????????????? - ??????*pep???????????????
	 - pep??????????????????struct eventpoll * ?????????????????????
 */
	*pep = ep;

	DNPRINTK(3, (KERN_INFO "[%p] eventpoll: ep_alloc() ep=%p\n",
		     current, ep));
	return 0;
}


static void ep_free(struct eventpoll *ep)
{
	struct rb_node *rbp;
	struct epitem *epi;

	/* We need to release all tasks waiting for these file */
	if (waitqueue_active(&ep->poll_wait))
		ep_poll_safewake(&psw, &ep->poll_wait);

	/*
	 * We need to lock this because we could be hit by
	 * eventpoll_release_file() while we're freeing the "struct eventpoll".
	 * We do not need to hold "ep->sem" here because the epoll file
	 * is on the way to be removed and no one has references to it
	 * anymore. The only hit might come from eventpoll_release_file() but
	 * holding "epmutex" is sufficent here.
	 */
	mutex_lock(&epmutex);

	/*
	 * Walks through the whole tree by unregistering poll callbacks.
	 */
	for (rbp = rb_first(&ep->rbr); rbp; rbp = rb_next(rbp)) {
		epi = rb_entry(rbp, struct epitem, rbn);

		ep_unregister_pollwait(ep, epi);
	}

	/*
	 * Walks through the whole hash by freeing each "struct epitem". At this
	 * point we are sure no poll callbacks will be lingering around, and also by
	 * write-holding "sem" we can be sure that no file cleanup code will hit
	 * us during this operation. So we can avoid the lock on "ep->lock".
	 */
	while ((rbp = rb_first(&ep->rbr)) != 0) {
		epi = rb_entry(rbp, struct epitem, rbn);
		ep_remove(ep, epi);
	}

	mutex_unlock(&epmutex);
}


/*
 * Search the file inside the eventpoll hash. It add usage count to
 * the returned item, so the caller must call ep_release_epitem()
 * after finished using the "struct epitem".
 * 
 * ??????????????????????????????file
 * ?????????????????????file?????????epitem
 */
static struct epitem *ep_find(struct eventpoll *ep, struct file *file, int fd)
{
	int kcmp;
	unsigned long flags;
	struct rb_node *rbp;
	struct epitem *epi, *epir = NULL;
	struct epoll_filefd ffd;

	ep_set_ffd(&ffd, file, fd);
	read_lock_irqsave(&ep->lock, flags);
	for (rbp = ep->rbr.rb_node; rbp; ) {
		epi = rb_entry(rbp, struct epitem, rbn);
		kcmp = ep_cmp_ffd(&ffd, &epi->ffd);
		if (kcmp > 0)
			rbp = rbp->rb_right;
		else if (kcmp < 0)
			rbp = rbp->rb_left;
		else {
			ep_use_epitem(epi);
			epir = epi;
			break;
		}
	}
	read_unlock_irqrestore(&ep->lock, flags);

	DNPRINTK(3, (KERN_INFO "[%p] eventpoll: ep_find(%p) -> %p\n",
		     current, file, epir));

	return epir;
}


/*
 * Increment the usage count of the "struct epitem" making it sure
 * that the user will have a valid pointer to reference.
 */
static void ep_use_epitem(struct epitem *epi)
{

	atomic_inc(&epi->usecnt);
}


/*
 * Decrement ( release ) the usage count by signaling that the user
 * has finished using the structure. It might lead to freeing the
 * structure itself if the count goes to zero.
 */
static void ep_release_epitem(struct epitem *epi)
{

	if (atomic_dec_and_test(&epi->usecnt))
		kmem_cache_free(epi_cache, epi);
}


/*
 * This is the callback that is used to add our wait queue to the
 * target file wakeup lists.
 * 
 * ????????????hang???????????????????????????????????????
 * 
 * ??????pt?????????????????????pt???????????????????????????????????????
 */
static void ep_ptable_queue_proc(struct file *file, wait_queue_head_t *whead,
				 poll_table *pt)
{
	struct epitem *epi = ep_item_from_epqueue(pt);
	struct eppoll_entry *pwq;

	if (epi->nwait >= 0 && (pwq = kmem_cache_alloc(pwq_cache, GFP_KERNEL))) {

		/*?????????pwq??????wait?????????wait_queue_t wait??????func??????????????? ep_poll_callback

		????????????????????????????????? ep_poll_callback ???
		*/
		init_waitqueue_func_entry(&pwq->wait, ep_poll_callback);
		pwq->whead = whead;
		pwq->base = epi;
		/*
		???&pwq->wait?????????whead??????????????????
		???whead?????????file??????????????????????????????????????????  ep_poll_callback ??????

		?????????socket????????????????????????ep_poll_callback??????????????????socket???fd???wait queue?????????
		???????????????????????????call?????????

		????????????eppoll_entry?????????????????????fd?????????????????????
		*/
		add_wait_queue(whead, &pwq->wait);

		/*
		???&pwq->llink?????????&pwq->llink??????
		*/
		list_add_tail(&pwq->llink, &epi->pwqlist);
		epi->nwait++;
	} else {
		/* We have to signal that an error occurred */
		epi->nwait = -1;
	}
}


static void ep_rbtree_insert(struct eventpoll *ep, struct epitem *epi)
{
	int kcmp;
	struct rb_node **p = &ep->rbr.rb_node, *parent = NULL;
	struct epitem *epic;

	while (*p) {
		parent = *p;
		epic = rb_entry(parent, struct epitem, rbn);
		kcmp = ep_cmp_ffd(&epi->ffd, &epic->ffd);
		if (kcmp > 0)
			p = &parent->rb_right;
		else
			p = &parent->rb_left;
	}
	rb_link_node(&epi->rbn, parent, p);
	rb_insert_color(&epi->rbn, &ep->rbr);
}

/*
tfile fd?????????????????????????????????
??????????????????epitem??????
1?????????????????????????????????????????????????????????file?????????????????????????????????
2??????????????????epoll?????????fd?????????????????????epitem???????????????????????????
3???????????????????????? eventpoll ????????????????????????????????????????????????????????????epoll???????????????private???????????????????????????
4???epoll????????????
- ????????????efd?????????
- ????????????????????????eventpoll??????????????????????????????efd????????????????????????efd???private?????????
- eventpoll????????????wq????????????????????????????????????????????????sys_epoll_wait??????????????????????????????????????????????????????????????????????????????
- evenpoll???????????????????????????????????????????????????sys_epoll_ctl??????????????????fd???????????????????????????sys_epoll_ctl?????????????????????????????????????????????item???????????????????????????????????????????????????

- epoll???????????????????????????fd?????????????????????????????????????????????????????????fd
- ????????????wait??????????????????????????????????????????????????????????????????????????????fds?????????????????? struct epoll_event *event,?????????????????????????????????????????????????????????????????????
- ????????????wait??????????????????????????????????????????????????????wake????????????try_wake_up()????????????????????????????????????

- epoll???ctl??????????????? ?????????????????????????????????????????????????????????ep-wq??????????????????item??????????????????
- ?????????????????????epoll????????????????????????????????????fds??????????????????fds???????????????????????????????????????ep_insert???????????????
1??????tfd??????epitem????????????rb tree???
2???revents = tfile->f_op->poll(tfile, &epq.pt); ???????????????????????????TCP???????????????ep_pqueue???????????????????????????tcp????????????????????????????????????????????????epoll????????????????????????????????????
??????????????????ep_poll_callback????????????
3???ep_poll_callback ???????????????????????????epg????????????epitme???????????????ep???????????????????????????wq???????????????????????????wait??????????????????ep??????????????????????????????????????????????????????
4????????????????????????ep->wq??????????????????????????????????????????sys_epoll_wait?????????????????????????????????????????????????????? init_waitqueue_entry(&wait, current); ???????????????wait_t?????????linux????????????????????????,????????????????????????????????????ep_poll_callback??????????????????task_struct?????????????????????????????????

*/ 
static int ep_insert(struct eventpoll *ep, struct epoll_event *event,
		     struct file *tfile, int fd)
{
	int error, revents, pwake = 0;
	unsigned long flags;
	struct epitem *epi;
	struct ep_pqueue epq;

	error = -ENOMEM;
	if (!(epi = kmem_cache_alloc(epi_cache, GFP_KERNEL)))
		goto eexit_1;

	/* Item initialization follow here ... */
	ep_rb_initnode(&epi->rbn);
	INIT_LIST_HEAD(&epi->rdllink);
	INIT_LIST_HEAD(&epi->fllink);
	INIT_LIST_HEAD(&epi->txlink);
	INIT_LIST_HEAD(&epi->pwqlist);
	epi->ep = ep;
	ep_set_ffd(&epi->ffd, tfile, fd);//???????????????
	epi->event = *event;
	atomic_set(&epi->usecnt, 1);
	epi->nwait = 0;

	/* Initialize the poll table using the queue callback */
	epq.epi = epi;
	init_poll_funcptr(&epq.pt, ep_ptable_queue_proc);

	/*
	 * Attach the item to the poll hooks and get current event bits.
	 * We can safely use the file* here because its usage count has
	 * been increased by the caller of this function.
	 * 
	 * &epq.pt???????????????????????????????????????epitem????????????
	 * ?????????tcp?????????poll????????????sock.poll -> tcp_poll
	 * ???tcp_poll????????????poll_wait??????
	 * 
	 * poll wait????????????????????????????????????????????????????????????????????????
	 * ??????????????????????????????????????????????????????????????????
	 */

	/* 
		poll???????????????????????????????????????????????????????????????
		?????????linux??????????????????????????????
		?????????CPU?????????
		???????????????????????????????????????CPU-???????????????????????????????????????
	 */

	/* 
	poll???????????????????????????
	????????????????????????
	 */
	revents = tfile->f_op->poll(tfile, &epq.pt);

	/*
	 * We have to check if something went wrong during the poll wait queue
	 * install process. Namely an allocation for a wait queue failed due
	 * high memory pressure.
	 */
	if (epi->nwait < 0)
		goto eexit_2;

	/* Add the current item to the list of active epoll hook for this file 
	???epi?????????????????????????????????ep link?????????
	*/
	spin_lock(&tfile->f_ep_lock);

	//???epitem?????????socket fd???f_ep_links?????????
	list_add_tail(&epi->fllink, &tfile->f_ep_links);
	spin_unlock(&tfile->f_ep_lock);

	/* We have to drop the new item inside our item list to keep track of it */
	write_lock_irqsave(&ep->lock, flags);

	/* Add the current item to the rb-tree 
	???epi?????????eventpoll??????????????????eventpoll??????efd???????????????
	*/
	ep_rbtree_insert(ep, epi);

	/* If the file is already "ready" we drop it inside the ready list 
	????????????????????????????????????????????????????????????????????????????????????
	?????????????????????????????????
	*/
	if ((revents & event->events) && !ep_is_linked(&epi->rdllink)) {
		list_add_tail(&epi->rdllink, &ep->rdllist);

		/* Notify waiting tasks that events are available 
		???????????????hang??????sys_epoll_wait????????????ep???wq??????head???????????????
		????????????????????????
		*/
		if (waitqueue_active(&ep->wq))
		/*
		??????????????????????????????__wake_up_common
		??????????????????typedef struct __wait_queue wait_queue_t;???func????????????????????????????????????????????????????????????????????????
		??????????????????????????????func?????????????????????

	wait_queue_func_t func;
	typedef int (*wait_queue_func_t)(wait_queue_t *wait, unsigned mode, int sync, void *key);

	????????????sys_epoll_wait??????????????????????????????epoll????????????
		*/
			__wake_up_locked(&ep->wq, TASK_UNINTERRUPTIBLE | TASK_INTERRUPTIBLE);
		if (waitqueue_active(&ep->poll_wait))
			pwake++;
	}

	write_unlock_irqrestore(&ep->lock, flags);

	/* We have to call this outside the lock */
	if (pwake)
		ep_poll_safewake(&psw, &ep->poll_wait);

	DNPRINTK(3, (KERN_INFO "[%p] eventpoll: ep_insert(%p, %p, %d)\n",
		     current, ep, tfile, fd));

	return 0;

eexit_2:
	ep_unregister_pollwait(ep, epi);

	/*
	 * We need to do this because an event could have been arrived on some
	 * allocated wait queue.
	 */
	write_lock_irqsave(&ep->lock, flags);
	if (ep_is_linked(&epi->rdllink))
		ep_list_del(&epi->rdllink);
	write_unlock_irqrestore(&ep->lock, flags);

	kmem_cache_free(epi_cache, epi);
eexit_1:
	return error;
}


/*
 * Modify the interest event mask by dropping an event if the new mask
 * has a match in the current file status.
 */
static int ep_modify(struct eventpoll *ep, struct epitem *epi, struct epoll_event *event)
{
	int pwake = 0;
	unsigned int revents;
	unsigned long flags;

	/*
	 * Set the new event interest mask before calling f_op->poll(), otherwise
	 * a potential race might occur. In fact if we do this operation inside
	 * the lock, an event might happen between the f_op->poll() call and the
	 * new event set registering.
	 */
	epi->event.events = event->events;

	/*
	 * Get current event bits. We can safely use the file* here because
	 * its usage count has been increased by the caller of this function.
	 * ????????????fd??????????????????
	 */
	revents = epi->ffd.file->f_op->poll(epi->ffd.file, NULL);

	write_lock_irqsave(&ep->lock, flags);

	/* Copy the data member from inside the lock */
	epi->event.data = event->data;

	/*
	 * If the item is not linked to the hash it means that it's on its
	 * way toward the removal. Do nothing in this case.
	 */
	if (ep_rb_linked(&epi->rbn)) {
		/*
		 * If the item is "hot" and it is not registered inside the ready
		 * list, push it inside. If the item is not "hot" and it is currently
		 * registered inside the ready list, unlink it.
		 */
		//?????????????????????poll?????????????????????????????????????????????????????????????????????????????????
		if (revents & event->events) {
			if (!ep_is_linked(&epi->rdllink)) {
				list_add_tail(&epi->rdllink, &ep->rdllist);

				/* Notify waiting tasks that events are available */
				if (waitqueue_active(&ep->wq))
					__wake_up_locked(&ep->wq, TASK_UNINTERRUPTIBLE |
							 TASK_INTERRUPTIBLE);
				if (waitqueue_active(&ep->poll_wait))
					pwake++;
			}
		}
	}

	write_unlock_irqrestore(&ep->lock, flags);

	/* We have to call this outside the lock */
	if (pwake)
		ep_poll_safewake(&psw, &ep->poll_wait);

	return 0;
}


/*
 * This function unregister poll callbacks from the associated file descriptor.
 * Since this must be called without holding "ep->lock" the atomic exchange trick
 * will protect us from multiple unregister.
 */
static void ep_unregister_pollwait(struct eventpoll *ep, struct epitem *epi)
{
	int nwait;
	struct list_head *lsthead = &epi->pwqlist;
	struct eppoll_entry *pwq;

	/* This is called without locks, so we need the atomic exchange */
	nwait = xchg(&epi->nwait, 0);

	if (nwait) {
		while (!list_empty(lsthead)) {
			pwq = list_entry(lsthead->next, struct eppoll_entry, llink);

			ep_list_del(&pwq->llink);
			remove_wait_queue(pwq->whead, &pwq->wait);
			kmem_cache_free(pwq_cache, pwq);
		}
	}
}


/*
 * Unlink the "struct epitem" from all places it might have been hooked up.
 * This function must be called with write IRQ lock on "ep->lock".
 */
static int ep_unlink(struct eventpoll *ep, struct epitem *epi)
{
	int error;

	/*
	 * It can happen that this one is called for an item already unlinked.
	 * The check protect us from doing a double unlink ( crash ).
	 */
	error = -ENOENT;
	if (!ep_rb_linked(&epi->rbn))
		goto eexit_1;

	/*
	 * Clear the event mask for the unlinked item. This will avoid item
	 * notifications to be sent after the unlink operation from inside
	 * the kernel->userspace event transfer loop.
	 */
	epi->event.events = 0;

	/*
	 * At this point is safe to do the job, unlink the item from our rb-tree.
	 * This operation togheter with the above check closes the door to
	 * double unlinks.
	 */
	ep_rb_erase(&epi->rbn, &ep->rbr);

	/*
	 * If the item we are going to remove is inside the ready file descriptors
	 * we want to remove it from this list to avoid stale events.
	 */
	if (ep_is_linked(&epi->rdllink))
		ep_list_del(&epi->rdllink);

	error = 0;
eexit_1:

	DNPRINTK(3, (KERN_INFO "[%p] eventpoll: ep_unlink(%p, %p) = %d\n",
		     current, ep, epi->ffd.file, error));

	return error;
}


/*
 * Removes a "struct epitem" from the eventpoll hash and deallocates
 * all the associated resources.
 */
static int ep_remove(struct eventpoll *ep, struct epitem *epi)
{
	int error;
	unsigned long flags;
	struct file *file = epi->ffd.file;

	/*
	 * Removes poll wait queue hooks. We _have_ to do this without holding
	 * the "ep->lock" otherwise a deadlock might occur. This because of the
	 * sequence of the lock acquisition. Here we do "ep->lock" then the wait
	 * queue head lock when unregistering the wait queue. The wakeup callback
	 * will run by holding the wait queue head lock and will call our callback
	 * that will try to get "ep->lock".
	 */
	ep_unregister_pollwait(ep, epi);

	/* Remove the current item from the list of epoll hooks */
	spin_lock(&file->f_ep_lock);
	if (ep_is_linked(&epi->fllink))
		ep_list_del(&epi->fllink);
	spin_unlock(&file->f_ep_lock);

	/* We need to acquire the write IRQ lock before calling ep_unlink() */
	write_lock_irqsave(&ep->lock, flags);

	/* Really unlink the item from the hash */
	error = ep_unlink(ep, epi);

	write_unlock_irqrestore(&ep->lock, flags);

	if (error)
		goto eexit_1;

	/* At this point it is safe to free the eventpoll item */
	ep_release_epitem(epi);

	error = 0;
eexit_1:
	DNPRINTK(3, (KERN_INFO "[%p] eventpoll: ep_remove(%p, %p) = %d\n",
		     current, ep, file, error));

	return error;
}


/*
 * This is the callback that is passed to the wait queue wakeup
 * machanism. It is called by the stored file descriptors when they
 * have events to report.
 * 
 * ????????????fd??????????????????????????????????????????fd???????????????????????????
 * ?????????wait_queue_t????????????ep??????????????????wait?????????
 */
static int ep_poll_callback(wait_queue_t *wait, unsigned mode, int sync, void *key)
{
	int pwake = 0;
	unsigned long flags;
	struct epitem *epi = ep_item_from_wait(wait);
	struct eventpoll *ep = epi->ep;

	DNPRINTK(3, (KERN_INFO "[%p] eventpoll: poll_callback(%p) epi=%p ep=%p\n",
		     current, epi->ffd.file, epi, ep));

	write_lock_irqsave(&ep->lock, flags);

	/*
	 * If the event mask does not contain any poll(2) event, we consider the
	 * descriptor to be disabled. This condition is likely the effect of the
	 * EPOLLONESHOT bit that disables the descriptor when an event is received,
	 * until the next EPOLL_CTL_MOD will be issued.
	 */
	if (!(epi->event.events & ~EP_PRIVATE_BITS))
		goto is_disabled;

	/* If this file is already in the ready list we exit soon */
	if (ep_is_linked(&epi->rdllink))
		goto is_linked;

/*
??????????????????. 
 */
	list_add_tail(&epi->rdllink, &ep->rdllist);

is_linked:
	/*
	 * Wake up ( if active ) both the eventpoll wait list and the ->poll()
	 * wait list.
	 * 
	 * ???????????????epoll_wait??????????????????????????????
	 * ep->wq????????????????????????
	 * 
	 * ??????????????????????????????????????? epoll_wait ??????????????????????????????wait???????????????????????????ep->wq????????????
	 * ??????????????????
	 * TODO xiaojin ?????????????????????
	 * ??????????????????
	 *
 static void __wake_up_common(wait_queue_head_t *q, unsigned int mode,
			     int nr_exclusive, int sync, void *key)
{
	struct list_head *tmp, *next;

	list_for_each_safe(tmp, next, &q->task_list) {
		wait_queue_t *curr = list_entry(tmp, wait_queue_t, task_list);
		unsigned flags = curr->flags;

		if (curr->func(curr, mode, sync, key) &&
				(flags & WQ_FLAG_EXCLUSIVE) && !--nr_exclusive)
			break;
	}
}
??????????????????nr_exclusive????????????1?????????????????????????????????
	 */
	if (waitqueue_active(&ep->wq))
		__wake_up_locked(&ep->wq, TASK_UNINTERRUPTIBLE |
				 TASK_INTERRUPTIBLE);

/* xiaojin - ???ctl-add????????????epoll fd?????????????????????????????????
 */
	if (waitqueue_active(&ep->poll_wait))
		pwake++;

is_disabled:
	write_unlock_irqrestore(&ep->lock, flags);

	/* We have to call this outside the lock */
	if (pwake)
		ep_poll_safewake(&psw, &ep->poll_wait);

	return 1;
}


static int ep_eventpoll_close(struct inode *inode, struct file *file)
{
	struct eventpoll *ep = file->private_data;

	if (ep) {
		ep_free(ep);
		kfree(ep);
	}

	DNPRINTK(3, (KERN_INFO "[%p] eventpoll: close() ep=%p\n", current, ep));
	return 0;
}


static unsigned int ep_eventpoll_poll(struct file *file, poll_table *wait)
{
	unsigned int pollflags = 0;
	unsigned long flags;
	struct eventpoll *ep = file->private_data;

	/* Insert inside our poll wait queue */
	poll_wait(file, &ep->poll_wait, wait);

	/* Check our condition */
	read_lock_irqsave(&ep->lock, flags);
	if (!list_empty(&ep->rdllist))
		pollflags = POLLIN | POLLRDNORM;
	read_unlock_irqrestore(&ep->lock, flags);

	return pollflags;
}


/*
 * Since we have to release the lock during the __copy_to_user() operation and
 * during the f_op->poll() call, we try to collect the maximum number of items
 * by reducing the irqlock/irqunlock switching rate.
 * 
 * ep??????rdlist??????????????????????????????epitem?????????rdlink???????????????ep???rdlist??????????????????????????????epitem
 * ????????????target fd
 */
static int ep_collect_ready_items(struct eventpoll *ep, struct list_head *txlist, int maxevents)
{
	int nepi;
	unsigned long flags;
	struct list_head *lsthead = &ep->rdllist, *lnk;
	struct epitem *epi;

	write_lock_irqsave(&ep->lock, flags);

	for (nepi = 0, lnk = lsthead->next; lnk != lsthead && nepi < maxevents;) {
		epi = list_entry(lnk, struct epitem, rdllink);

		lnk = lnk->next;

		/* If this file is already in the ready list we exit soon */
		if (!ep_is_linked(&epi->txlink)) {
			/*
			 * This is initialized in this way so that the default
			 * behaviour of the reinjecting code will be to push back
			 * the item inside the ready list.
			 */
			epi->revents = epi->event.events;

			/* Link the ready item into the transfer list */
			list_add(&epi->txlink, txlist);
			nepi++;

			/*
			 * Unlink the item from the ready list.
			 */
			ep_list_del(&epi->rdllink);
		}
	}

	write_unlock_irqrestore(&ep->lock, flags);

	return nepi;
}


/*
 * This function is called without holding the "ep->lock" since the call to
 * __copy_to_user() might sleep, and also f_op->poll() might reenable the IRQ
 * because of the way poll() is traditionally implemented in Linux.
 */
static int ep_send_events(struct eventpoll *ep, struct list_head *txlist,
			  struct epoll_event __user *events)
{
	int eventcnt = 0;
	unsigned int revents;
	struct list_head *lnk;
	struct epitem *epi;

	/*
	 * We can loop without lock because this is a task private list.
	 * The test done during the collection loop will guarantee us that
	 * another task will not try to collect this file. Also, items
	 * cannot vanish during the loop because we are holding "sem".
	 */
	list_for_each(lnk, txlist) {
		epi = list_entry(lnk, struct epitem, txlink);

		/*
		 * Get the ready file event set. We can safely use the file
		 * because we are holding the "sem" in read and this will
		 * guarantee that both the file and the item will not vanish.
		 */
		revents = epi->ffd.file->f_op->poll(epi->ffd.file, NULL);

		/*
		 * Set the return event set for the current file descriptor.
		 * Note that only the task task was successfully able to link
		 * the item to its "txlist" will write this field.
		 */
		epi->revents = revents & epi->event.events;

		if (epi->revents) {
			if (__put_user(epi->revents,
				       &events[eventcnt].events) ||
			    __put_user(epi->event.data,
				       &events[eventcnt].data))
				return -EFAULT;
			if (epi->event.events & EPOLLONESHOT)
				epi->event.events &= EP_PRIVATE_BITS;
			eventcnt++;
		}
	}
	return eventcnt;
}


/*
 * Walk through the transfer list we collected with ep_collect_ready_items()
 * and, if 1) the item is still "alive" 2) its event set is not empty 3) it's
 * not already linked, links it to the ready list. Same as above, we are holding
 * "sem" so items cannot vanish underneath our nose.
 */
static void ep_reinject_items(struct eventpoll *ep, struct list_head *txlist)
{
	int ricnt = 0, pwake = 0;
	unsigned long flags;
	struct epitem *epi;

	write_lock_irqsave(&ep->lock, flags);

	while (!list_empty(txlist)) {
		epi = list_entry(txlist->next, struct epitem, txlink);

		/* Unlink the current item from the transfer list */
		ep_list_del(&epi->txlink);

		/*
		 * If the item is no more linked to the interest set, we don't
		 * have to push it inside the ready list because the following
		 * ep_release_epitem() is going to drop it. Also, if the current
		 * item is set to have an Edge Triggered behaviour, we don't have
		 * to push it back either.
		 */
		if (ep_rb_linked(&epi->rbn) && !(epi->event.events & EPOLLET) &&
		    (epi->revents & epi->event.events) && !ep_is_linked(&epi->rdllink)) {
			list_add_tail(&epi->rdllink, &ep->rdllist);
			ricnt++;
		}
	}

	if (ricnt) {
		/*
		 * Wake up ( if active ) both the eventpoll wait list and the ->poll()
		 * wait list.
		 */
		if (waitqueue_active(&ep->wq))
			__wake_up_locked(&ep->wq, TASK_UNINTERRUPTIBLE |
					 TASK_INTERRUPTIBLE);
		if (waitqueue_active(&ep->poll_wait))
			pwake++;
	}

	write_unlock_irqrestore(&ep->lock, flags);

	/* We have to call this outside the lock */
	if (pwake)
		ep_poll_safewake(&psw, &ep->poll_wait);
}


/*
 * Perform the transfer of events to user space.
 */
static int ep_events_transfer(struct eventpoll *ep,
			      struct epoll_event __user *events, int maxevents)
{
	int eventcnt = 0;
	struct list_head txlist;

	INIT_LIST_HEAD(&txlist);

	/*
	 * We need to lock this because we could be hit by
	 * eventpoll_release_file() and epoll_ctl(EPOLL_CTL_DEL).
	 */
	down_read(&ep->sem);

	/* Collect/extract ready items */
	if (ep_collect_ready_items(ep, &txlist, maxevents) > 0) {
		/* Build result set in userspace */
		eventcnt = ep_send_events(ep, &txlist, events);

		/* Reinject ready items into the ready list */
		ep_reinject_items(ep, &txlist);
	}

	up_read(&ep->sem);

	return eventcnt;
}

/*
events?????????????????????????????????????????????????????????maxevents???
*/
static int ep_poll(struct eventpoll *ep, struct epoll_event __user *events,
		   int maxevents, long timeout)
{
	int res, eavail;
	unsigned long flags;
	long jtimeout;
	wait_queue_t wait;

	/*
	 * Calculate the timeout by checking for the "infinite" value ( -1 )
	 * and the overflow condition. The passed timeout is in milliseconds,
	 * that why (t * HZ) / 1000.
	 */
	jtimeout = (timeout < 0 || timeout >= EP_MAX_MSTIMEO) ?
		MAX_SCHEDULE_TIMEOUT : (timeout * HZ + 999) / 1000;

retry:
	write_lock_irqsave(&ep->lock, flags);

	res = 0;
	/*
	ready list ???????????????????????????????????????????????????
	*/
	if (list_empty(&ep->rdllist)) {
		/*
		 * We don't have any available event to return to the caller.
		 * We need to sleep here, and we will be wake up by
		 * ep_poll_callback() when events will become available.
		 * 
		 * default_wake_function ?????????????????????????????????wait???????????????????????????
		 */
		init_waitqueue_entry(&wait, current);
		
		/*???wait?????????ep???wq????????????
		????????????????????????wait?????????ep->wq?????????wait?????????????????????????????????????????? default_wake_function ?????????
		????????????fd???????????????????????????????????????poll?????????????????????fd???poll????????????poll_wait???????????????
		ep_poll_callback?????????????????????????????????????????????

		if (waitqueue_active(&ep->wq))
		__wake_up_locked(&ep->wq, TASK_UNINTERRUPTIBLE |
				 TASK_INTERRUPTIBLE);
		??????????????????????????????
		*/
		__add_wait_queue(&ep->wq, &wait);

		for (;;) {
			/*
			 * We don't want to sleep if the ep_poll_callback() sends us
			 * a wakeup in between. That's why we set the task state
			 * to TASK_INTERRUPTIBLE before doing the checks.
			 * 
			 * ????????????????????????????????????????????????????????????????????????
			 */
			set_current_state(TASK_INTERRUPTIBLE);
			if (!list_empty(&ep->rdllist) || !jtimeout)
				break;
			if (signal_pending(current)) {
				res = -EINTR;
				break;
			}

			write_unlock_irqrestore(&ep->lock, flags);
			jtimeout = schedule_timeout(jtimeout);
			write_lock_irqsave(&ep->lock, flags);
		}
		__remove_wait_queue(&ep->wq, &wait);

		set_current_state(TASK_RUNNING);
	}

	/* Is it worth to try to dig for events ? */
	eavail = !list_empty(&ep->rdllist);

	write_unlock_irqrestore(&ep->lock, flags);

	/*
	 * Try to transfer events to user space. In case we get 0 events and
	 * there's still timeout left over, we go trying again in search of
	 * more luck.
	 * 
	 * TODO xiaojin
	 * maxevents???????????????????????????????????????????????????????????????????????????????????????????????????
	 */
	if (!res && eavail &&
	    !(res = ep_events_transfer(ep, events, maxevents)) && jtimeout)
		goto retry;

	return res;
}


static int eventpollfs_delete_dentry(struct dentry *dentry)
{

	return 1;
}


static struct inode *ep_eventpoll_inode(void)
{
	int error = -ENOMEM;
	struct inode *inode = new_inode(eventpoll_mnt->mnt_sb);

	if (!inode)
		goto eexit_1;

	inode->i_fop = &eventpoll_fops;

	/*
	 * Mark the inode dirty from the very beginning,
	 * that way it will never be moved to the dirty
	 * list because mark_inode_dirty() will think
	 * that it already _is_ on the dirty list.
	 */
	inode->i_state = I_DIRTY;
	inode->i_mode = S_IRUSR | S_IWUSR;
	inode->i_uid = current->fsuid;
	inode->i_gid = current->fsgid;
	inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME;
	return inode;

eexit_1:
	return ERR_PTR(error);
}


static int
eventpollfs_get_sb(struct file_system_type *fs_type, int flags,
		   const char *dev_name, void *data, struct vfsmount *mnt)
{
	return get_sb_pseudo(fs_type, "eventpoll:", NULL, EVENTPOLLFS_MAGIC,
			     mnt);
}


static int __init eventpoll_init(void)
{
	int error;

	mutex_init(&epmutex);

	/* Initialize the structure used to perform safe poll wait head wake ups */
	ep_poll_safewake_init(&psw);

	/* Allocates slab cache used to allocate "struct epitem" items */
	epi_cache = kmem_cache_create("eventpoll_epi", sizeof(struct epitem),
			0, SLAB_HWCACHE_ALIGN|EPI_SLAB_DEBUG|SLAB_PANIC,
			NULL, NULL);

	/* Allocates slab cache used to allocate "struct eppoll_entry" */
	pwq_cache = kmem_cache_create("eventpoll_pwq",
			sizeof(struct eppoll_entry), 0,
			EPI_SLAB_DEBUG|SLAB_PANIC, NULL, NULL);

	/*
	 * Register the virtual file system that will be the source of inodes
	 * for the eventpoll files
	 */
	error = register_filesystem(&eventpoll_fs_type);
	if (error)
		goto epanic;

	/* Mount the above commented virtual file system */
	eventpoll_mnt = kern_mount(&eventpoll_fs_type);
	error = PTR_ERR(eventpoll_mnt);
	if (IS_ERR(eventpoll_mnt))
		goto epanic;

	DNPRINTK(3, (KERN_INFO "[%p] eventpoll: successfully initialized.\n",
			current));
	return 0;

epanic:
	panic("eventpoll_init() failed\n");
}


static void __exit eventpoll_exit(void)
{
	/* Undo all operations done inside eventpoll_init() */
	unregister_filesystem(&eventpoll_fs_type);
	mntput(eventpoll_mnt);
	kmem_cache_destroy(pwq_cache);
	kmem_cache_destroy(epi_cache);
}

module_init(eventpoll_init);
module_exit(eventpoll_exit);

MODULE_LICENSE("GPL");
